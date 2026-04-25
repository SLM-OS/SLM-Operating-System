/*
 * camera.c — Camera open + preprocess implementation.
 *
 * Mock backend: returns the embedded .rodata frame produced by
 * scripts/generate-mock-camera-frame.py. Real backends will appear
 * here once the IMX219 / NVCSI / VI drivers land
 * (docs/jetson-camera-imx219-plan.md).
 *
 * preprocess_mnist intentionally takes the simplest path that produces
 * deterministic output for unit testing: green-channel-only, box-
 * average from a 44x44 source block to one 28x28 destination pixel,
 * 8-bit (high RAW10 bits) precision. Adding 10-bit precision or full
 * demosaic would change the MD5 the QEMU tests pin and gain very
 * little for digit recognition.
 */

#include "camera.h"

#include <stddef.h>
#include <stdint.h>

#include "string.h"   /* strcmp */

/* The embed file declares these as global symbols when MOCK_CAMERA_FRAME
 * is ON. Both are weak so kernels built with MOCK_CAMERA_FRAME=OFF link
 * cleanly — camera_open("mock") then reports "backend not built". */
extern const uint8_t mock_camera_frame_start[] __attribute__((weak));
extern const uint8_t mock_camera_frame_end[]   __attribute__((weak));

/* Frame geometry — pinned to IMX219 2-lane RAW10 1640x1232 binned
 * mode, the only shape the mock and the in-progress IMX219 driver
 * produce. Generalising preprocess_mnist to other shapes is fine
 * later but not in scope today. */
#define MOCK_FRAME_W       1640u
#define MOCK_FRAME_H       1232u
#define MOCK_FRAME_BYTES   (MOCK_FRAME_W * MOCK_FRAME_H * 10u / 8u)

#define MNIST_DIM          28u
#define MNIST_BLOCK        44u   /* 28 * 44 == 1232 */
#define MNIST_CROP         (MNIST_DIM * MNIST_BLOCK)
#define MNIST_CROP_X_OFF   ((MOCK_FRAME_W - MNIST_CROP) / 2u)  /* 204, even */

/* Half the RGGB grid is green: 44 * 44 / 2 = 968 samples per block. */
#define MNIST_GREEN_PER_BLOCK ((MNIST_BLOCK * MNIST_BLOCK) / 2u)

/*
 * Read the high 8 bits of pixel `pixel_index` from a RAW10 packed
 * buffer. In RAW10, 4 pixels live in 5 bytes: bytes 0..3 hold the
 * high 8 bits of each pixel and byte 4 packs the four 2-bit low
 * remainders. We discard the low bits — see the file header for why.
 */
static inline uint8_t raw10_hi8(const uint8_t *raw10, uint32_t pixel_index)
{
    uint32_t group   = pixel_index >> 2;
    uint32_t in_grp  = pixel_index & 3u;
    return raw10[group * 5u + in_grp];
}

/*
 * Build the IEEE 754 single-precision bit pattern for num/den, with
 * num/den in [0, 1]. Integer-only because the kernel is compiled
 * with -mgeneral-regs-only (no FP/NEON registers). Truncating
 * division — the test pin has to be computed with the matching
 * truncation, see scripts/generate-mock-camera-frame.py for the
 * Python mirror.
 *
 * Subnormals are not produced: the smallest non-zero value the
 * preprocess pipeline emits is 1/(968*255) ≈ 2^-18, well above the
 * subnormal threshold of 2^-126.
 */
static inline uint32_t fp32_div_bits(uint32_t num, uint32_t den)
{
    if (num == 0u) return 0u;
    if (num >= den) return 0x3f800000u;   /* exact 1.0 */

    /* mantissa_raw = (num * 2^32) / den, fits in u64 since num < den < 2^32. */
    uint64_t mantissa_raw = ((uint64_t)num << 32) / (uint64_t)den;
    /* (num << 32) / den == num/den * 2^32. The corresponding implicit-1
     * bit is at position 32 + (something). exp_unbiased = 23 - K where
     * K = 32 ⇒ -9 if mantissa_raw is already in [2^23, 2^24). */
    int exp_unbiased = -9;
    while (mantissa_raw < (1ULL << 23)) {
        mantissa_raw <<= 1;
        exp_unbiased--;
    }
    while (mantissa_raw >= (1ULL << 24)) {
        mantissa_raw >>= 1;
        exp_unbiased++;
    }
    uint32_t exp_biased = (uint32_t)(exp_unbiased + 127);
    uint32_t mantissa   = (uint32_t)(mantissa_raw & 0x7fffffULL);
    return (exp_biased << 23) | mantissa;
}

int camera_open(const char *name, struct camera_frame *out)
{
    if (!name || !out) return -1;

    if (strcmp(name, "mock") == 0) {
        if (!mock_camera_frame_start || !mock_camera_frame_end) {
            return -2;
        }
        out->data   = mock_camera_frame_start;
        out->size   = (size_t)(mock_camera_frame_end - mock_camera_frame_start);
        out->width  = MOCK_FRAME_W;
        out->height = MOCK_FRAME_H;
        out->bayer  = CAMERA_BAYER_RGGB;
        return 0;
    }

    return -1;
}

int camera_preprocess_mnist(const uint8_t *raw10,
                            size_t         raw10_len,
                            uint32_t       width,
                            uint32_t       height,
                            uint32_t       bayer,
                            uint8_t       *out_bytes,
                            size_t         out_capacity)
{
    if (!raw10 || !out_bytes) return -1;
    if (out_capacity < CAMERA_MNIST_OUT_BYTES) return -1;

    /* Hard-coded geometry — see file header. Generalising to other
     * sensor modes is a follow-up once the real IMX219 driver lands
     * and we know which mode tables it ships with. */
    if (width != MOCK_FRAME_W || height != MOCK_FRAME_H) return -2;
    if (bayer != CAMERA_BAYER_RGGB) return -2;
    if (raw10_len < MOCK_FRAME_BYTES) return -1;

    for (uint32_t i = 0; i < MNIST_DIM; i++) {
        uint32_t r0 = i * MNIST_BLOCK;
        for (uint32_t j = 0; j < MNIST_DIM; j++) {
            uint32_t c0 = j * MNIST_BLOCK + MNIST_CROP_X_OFF;
            uint32_t sum = 0;

            for (uint32_t dy = 0; dy < MNIST_BLOCK; dy++) {
                uint32_t r = r0 + dy;
                /* RGGB green pixels: (r%2 == 0, c%2 == 1) ∪
                 *                    (r%2 == 1, c%2 == 0). Pick the
                 * starting column phase based on r's parity, then
                 * stride by 2. */
                uint32_t c_start = c0 + ((r & 1u) ? 0u : 1u);
                for (uint32_t c = c_start; c < c0 + MNIST_BLOCK; c += 2u) {
                    sum += raw10_hi8(raw10, r * MOCK_FRAME_W + c);
                }
            }

            /* avg = sum / 968 ∈ [0, 255]; normalise to [0, 1] by
             * dividing by 968*255. Integer-only IEEE 754 build because
             * -mgeneral-regs-only forbids float/NEON in kernel C. */
            uint32_t bits = fp32_div_bits(sum,
                                          MNIST_GREEN_PER_BLOCK * 255u);
            uint8_t *dst = out_bytes + (i * MNIST_DIM + j) * 4u;
            __builtin_memcpy(dst, &bits, sizeof(bits));
        }
    }

    return 0;
}
