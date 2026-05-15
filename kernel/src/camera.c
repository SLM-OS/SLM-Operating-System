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

#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "imx219.h"   /* imx219_capture_one_frame */
#endif

/* The embed file declares these as global symbols when MOCK_CAMERA_FRAME
 * is ON. Both are weak so kernels built with MOCK_CAMERA_FRAME=OFF link
 * cleanly — camera_open("mock") then reports "backend not built". */
extern const uint8_t mock_camera_frame_start[] __attribute__((weak));
extern const uint8_t mock_camera_frame_end[]   __attribute__((weak));

/* Frame geometry — pinned to IMX219 2-lane RAW10 1640x1232 binned
 * mode, the only shape the mock and the IMX219 driver produce.
 * Generalising preprocess_mnist to other shapes is fine later but
 * not in scope today. */
#define MOCK_FRAME_W       1640u
#define MOCK_FRAME_H       1232u
#define MOCK_FRAME_RAW10_BYTES (MOCK_FRAME_W * MOCK_FRAME_H * 10u / 8u)
#define MOCK_FRAME_T_R16_BYTES (MOCK_FRAME_W * MOCK_FRAME_H * 2u)

#define MNIST_DIM          28u
#define MNIST_BLOCK_FULL   44u   /* 28 * 44 == 1232 (full frame height) */
#define MNIST_BLOCK_HALF   22u   /* 28 * 22 == 616  (centre-crop mode)  */

/* Flat-field reference. Captured once via camera_flatfield_capture
 * with the same centre-crop geometry preprocess uses, then divided
 * out per-block from subsequent inputs to flatten lens vignette.
 * Storage is small (~3 KB) because it's at MNIST-block resolution,
 * not source-pixel resolution.
 *
 * Single-task contract: every read and write of g_flatfield comes
 * from the shell task (camera_flatfield_capture/clear via Lua,
 * camera_preprocess_mnist via the inference path that the same
 * shell task drives). No locking — if anyone moves preprocess to a
 * worker task in the future they must add one, otherwise a racing
 * re-capture can hand out a torn (valid=true, anchor_*=stale) view. */
_Static_assert(MNIST_DIM == CAMERA_FLATFIELD_DIM,
    "Flat-field grid must match MNIST output grid");
static struct {
    bool     valid;
    uint32_t block[CAMERA_FLATFIELD_DIM][CAMERA_FLATFIELD_DIM];
    uint32_t anchor_center;     /* avg of central 2×2 blocks */
    uint32_t anchor_max;        /* global max block sum */
    uint32_t anchor_mid_edge;   /* avg of blocks at half-radius from centre */
} g_flatfield;

/* Anchor-block coordinates, derived from CAMERA_FLATFIELD_DIM so a
 * future grid-size change doesn't silently mean something different.
 * CENTER_LO/HI are the two indices straddling the grid's geometric
 * centre (DIM-1)/2 = 13.5 for DIM=28). MID_NEAR / MID_FAR are the
 * quarter-radius cardinal points from the centre row/column. */
#define FF_CENTER_LO  ((CAMERA_FLATFIELD_DIM / 2u) - 1u)   /* 13 */
#define FF_CENTER_HI  ( CAMERA_FLATFIELD_DIM / 2u)         /* 14 */
#define FF_MID_NEAR   ( CAMERA_FLATFIELD_DIM / 4u)         /*  7 */
#define FF_MID_FAR    ((CAMERA_FLATFIELD_DIM * 3u) / 4u)   /* 21 */

/* Below this raw block sum the per-block reference is dominated by
 * sensor noise (heavy-vignette corners can read near-zero), so the
 * divide path passes the live sample through uncorrected rather
 * than amplifying noise into a wild "corrected" value. Threshold
 * picked empirically against blank-paper captures on the wide-angle
 * IMX219 module — corners read ~30-150 there, so 64 stays clear of
 * the legitimate corner-block range while still rejecting genuine
 * dead-pixel / lens-cap captures. */
#define FF_REF_NOISE_FLOOR  64u

/* Geometry invariants. Both block sizes must be even (preserves Bayer
 * row phase + identical green-sample count per row), and the centred
 * crop offsets must be even (preserves RGGB column phase). Numbers
 * baked at compile time so a future tweak to MOCK_FRAME_W / either
 * block size can't silently break the algorithm. */
_Static_assert((MNIST_BLOCK_FULL & 1u) == 0u,
    "Full-frame box-average block must be even");
_Static_assert((MNIST_BLOCK_HALF & 1u) == 0u,
    "Centre-crop box-average block must be even");
_Static_assert(((MOCK_FRAME_W - MNIST_DIM * MNIST_BLOCK_FULL) / 2u & 1u) == 0u,
    "Full-frame X-offset must be even (Bayer phase)");
_Static_assert(((MOCK_FRAME_W - MNIST_DIM * MNIST_BLOCK_HALF) / 2u & 1u) == 0u,
    "Centre-crop X-offset must be even (Bayer phase)");
_Static_assert(((MOCK_FRAME_H - MNIST_DIM * MNIST_BLOCK_HALF) / 2u & 1u) == 0u,
    "Centre-crop Y-offset must be even (Bayer phase)");
_Static_assert(MNIST_DIM * MNIST_BLOCK_FULL == MOCK_FRAME_H,
    "Full-frame mode covers the whole frame height with no Y-offset");

/*
 * Read the high 8 bits of pixel `pixel_index` from a RAW10 packed
 * buffer. In RAW10, 4 pixels live in 5 bytes: bytes 0..3 hold the
 * high 8 bits of each pixel and byte 4 packs the four 2-bit low
 * remainders. We discard the low bits — see the file header for why.
 */
static inline uint8_t raw10_packed_hi8(const uint8_t *raw10,
                                       uint32_t pixel_index)
{
    uint32_t group   = pixel_index >> 2;
    uint32_t in_grp  = pixel_index & 3u;
    return raw10[group * 5u + in_grp];
}

/*
 * Read the high 8 bits of pixel `pixel_index` from a T_R16 buffer.
 * T_R16 is little-endian u16 with the RAW10 sample in bits [15:6],
 * so the high 8 bits live in the high byte of each u16 — i.e. byte
 * `pixel_index * 2 + 1`. ARM64 little-endian is the build's
 * universal endianness; no cross-platform endianness wrapper needed.
 */
static inline uint8_t t_r16_hi8(const uint8_t *t_r16, uint32_t pixel_index)
{
    return t_r16[pixel_index * 2u + 1u];
}

static inline uint8_t pixel_hi8(const uint8_t *buf,
                                uint32_t pixel_index,
                                uint32_t format)
{
    if (format == CAMERA_FORMAT_T_R16) {
        return t_r16_hi8(buf, pixel_index);
    }
    return raw10_packed_hi8(buf, pixel_index);
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
    /* Guard against truncation-to-zero (num << 32 < den, e.g. callers
     * passing tiny num with very large den). The normalising shift loop
     * below would spin forever on mantissa_raw == 0. The current
     * preprocess pipeline never hits this — its (num, den) is bounded
     * by (246840, 246840) — but the helper is otherwise inviting an
     * infinite loop on misuse. Round to zero, the IEEE 754 behaviour
     * any new caller would expect. */
    if (mantissa_raw == 0u) return 0u;
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
        out->format = CAMERA_FORMAT_RAW10_PACKED;
        /* Mock frame is hand-crafted MNIST-shape data (bright digit
         * on dark background, full-frame). No vignette, no polarity
         * flip needed. */
        out->recommended_invert      = false;
        out->recommended_center_crop = false;
        return 0;
    }

    if (strcmp(name, "imx219-0") == 0) {
#if defined(PLATFORM_JETSON_ORIN_NANO)
        int rc = imx219_capture_one_frame(out);
        if (rc != 0) return -3;
        return 0;
#else
        return -2;
#endif
    }

    return -1;
}

/* Sum the green-channel (RGGB) high-8-bit samples in one MNIST
 * block of the source frame at logical (i, j). Pure block-level
 * helper — caller owns the geometry / crop math. Inlined into both
 * preprocess and flat-field-capture so the two paths produce
 * byte-identical sums and the divide is mathematically cancellable
 * under uniform illumination. */
static inline uint32_t green_block_sum(const uint8_t *data,
                                       uint32_t       format,
                                       uint32_t       i,
                                       uint32_t       j,
                                       uint32_t       mnist_block,
                                       uint32_t       crop_x_off,
                                       uint32_t       crop_y_off)
{
    uint32_t r0  = i * mnist_block + crop_y_off;
    uint32_t c0  = j * mnist_block + crop_x_off;
    uint32_t sum = 0;
    for (uint32_t dy = 0; dy < mnist_block; dy++) {
        uint32_t r = r0 + dy;
        /* RGGB green pixels: (r%2==0, c%2==1) ∪ (r%2==1, c%2==0).
         * Pick starting column phase from r's parity, stride 2. */
        uint32_t c_start = c0 + ((r & 1u) ? 0u : 1u);
        for (uint32_t c = c_start; c < c0 + mnist_block; c += 2u) {
            sum += pixel_hi8(data, r * MOCK_FRAME_W + c, format);
        }
    }
    return sum;
}

/* Pick the anchor reference value for a given normalize mode.
 * Returns UINT32_MAX if no reference is stored or the mode isn't
 * a FLATFIELD_* mode (matches camera_flatfield_get's sentinel
 * convention; lets callers tell "no data" from a legitimate zero
 * anchor on a fully-dark reference). */
static uint32_t flatfield_anchor_for(uint32_t mode)
{
    if (!g_flatfield.valid) return UINT32_MAX;
    switch (mode) {
    case CAMERA_NORMALIZE_FLATFIELD_CENTER:   return g_flatfield.anchor_center;
    case CAMERA_NORMALIZE_FLATFIELD_MAX:      return g_flatfield.anchor_max;
    case CAMERA_NORMALIZE_FLATFIELD_MID_EDGE: return g_flatfield.anchor_mid_edge;
    default: return UINT32_MAX;
    }
}

int camera_preprocess_mnist(const uint8_t *data,
                            size_t         data_len,
                            uint32_t       width,
                            uint32_t       height,
                            uint32_t       bayer,
                            uint32_t       format,
                            bool           invert,
                            bool           center_crop,
                            uint32_t       normalize_mode,
                            uint8_t       *out_bytes,
                            size_t         out_capacity)
{
    if (!data || !out_bytes) return -1;
    if (out_capacity < CAMERA_MNIST_OUT_BYTES) return -1;

    /* Hard-coded geometry — see file header. */
    if (width != MOCK_FRAME_W || height != MOCK_FRAME_H) return -2;
    if (bayer != CAMERA_BAYER_RGGB) return -2;

    size_t min_bytes;
    switch (format) {
    case CAMERA_FORMAT_RAW10_PACKED:
        min_bytes = MOCK_FRAME_RAW10_BYTES;
        break;
    case CAMERA_FORMAT_T_R16:
        min_bytes = MOCK_FRAME_T_R16_BYTES;
        break;
    default:
        return -2;
    }
    if (data_len < min_bytes) return -1;

    if (normalize_mode >= CAMERA_NORMALIZE_COUNT) return -2;
    bool use_flatfield = (normalize_mode != CAMERA_NORMALIZE_NONE);
    /* Flat-field normalization only makes sense for the center-crop
     * geometry — that's the only one the reference is captured at. */
    if (use_flatfield && !center_crop) return -2;
    uint32_t anchor = 0u;
    if (use_flatfield) {
        if (!g_flatfield.valid) return -3;
        anchor = flatfield_anchor_for(normalize_mode);
        /* The two upstream guards (mode < COUNT and g_flatfield.valid)
         * make UINT32_MAX unreachable today. Keep the check as
         * defense-in-depth so a future refactor that drops one of
         * them surfaces here instead of later as a wild divide. A
         * legitimate anchor of 0 (fully-dark reference) is fine —
         * the per-block divide skips via FF_REF_NOISE_FLOOR. */
        if (anchor == UINT32_MAX) return -2;
    }

    /* Crop geometry is selected by center_crop. Full-frame mode
     * matches the legacy contract: cover the whole 1232-tall frame
     * (Y-offset 0) with 28 × 44-pixel blocks. Centre-crop mode uses
     * 28 × 22-pixel blocks over a 616×616 region centred on the
     * sensor — the half-area is needed because IMX219's wide-angle
     * lens has heavy vignette in the corners (see camera_heatmap
     * blank-paper trace, this PR), and the L4T `nvarguscamerasrc`
     * ISP path that would normally apply lens-shading correction
     * isn't reachable from SLM-OS. */
    const uint32_t mnist_block      = center_crop ? MNIST_BLOCK_HALF
                                                  : MNIST_BLOCK_FULL;
    const uint32_t mnist_crop       = MNIST_DIM * mnist_block;
    const uint32_t crop_x_off       = (MOCK_FRAME_W - mnist_crop) / 2u;
    const uint32_t crop_y_off       = (MOCK_FRAME_H - mnist_crop) / 2u;
    const uint32_t green_per_block  = (mnist_block * mnist_block) / 2u;
    const uint32_t denom            = green_per_block * 255u;

    for (uint32_t i = 0; i < MNIST_DIM; i++) {
        for (uint32_t j = 0; j < MNIST_DIM; j++) {
            uint32_t sum = green_block_sum(data, format, i, j,
                                           mnist_block,
                                           crop_x_off, crop_y_off);

            uint32_t num;
            if (use_flatfield) {
                /* Flat-field divide: corrected = sum × anchor / ref.
                 * Numerator fits u64 comfortably (sum, anchor, ref
                 * each < 62000; product < 4×10⁹). Renormalize to
                 * the same denom = green_per_block × 255 the NONE
                 * path uses so an inverted-output value of 0.5 means
                 * the same thing in both modes. Skip the divide
                 * when ref is suspiciously low — corner blocks under
                 * extreme vignette can read near-zero, in which case
                 * the corrected value is dominated by sensor noise
                 * and any answer we synthesise is misleading. Pass
                 * the raw sum through instead. */
                uint32_t ref = g_flatfield.block[i][j];
                if (ref < FF_REF_NOISE_FLOOR) {
                    num = sum;
                } else {
                    uint64_t corrected = (uint64_t)sum * anchor / ref;
                    if (corrected > denom) corrected = denom;
                    num = (uint32_t)corrected;
                }
            } else {
                num = sum;
            }

            /* Polarity: MNIST trains on bright-stroke-on-dark-
             * background. Sensor produces the inverse. The invert
             * path flips at the integer numerator step so we stay
             * out of fp arithmetic. */
            if (invert) num = denom - num;
            uint32_t bits = fp32_div_bits(num, denom);
            uint8_t *dst = out_bytes + (i * MNIST_DIM + j) * 4u;
            __builtin_memcpy(dst, &bits, sizeof(bits));
        }
    }

    return 0;
}

int camera_flatfield_capture(const uint8_t *data,
                             size_t         data_len,
                             uint32_t       width,
                             uint32_t       height,
                             uint32_t       bayer,
                             uint32_t       format)
{
    if (!data) return -1;
    if (width != MOCK_FRAME_W || height != MOCK_FRAME_H) return -2;
    if (bayer != CAMERA_BAYER_RGGB) return -2;

    size_t min_bytes;
    switch (format) {
    case CAMERA_FORMAT_RAW10_PACKED: min_bytes = MOCK_FRAME_RAW10_BYTES; break;
    case CAMERA_FORMAT_T_R16:        min_bytes = MOCK_FRAME_T_R16_BYTES; break;
    default: return -2;
    }
    if (data_len < min_bytes) return -1;

    /* Mirror the centre-crop preprocess geometry exactly. */
    const uint32_t mnist_block = MNIST_BLOCK_HALF;
    const uint32_t mnist_crop  = MNIST_DIM * mnist_block;
    const uint32_t crop_x_off  = (MOCK_FRAME_W - mnist_crop) / 2u;
    const uint32_t crop_y_off  = (MOCK_FRAME_H - mnist_crop) / 2u;

    uint32_t max_sum = 0;
    for (uint32_t i = 0; i < MNIST_DIM; i++) {
        for (uint32_t j = 0; j < MNIST_DIM; j++) {
            uint32_t s = green_block_sum(data, format, i, j,
                                         mnist_block,
                                         crop_x_off, crop_y_off);
            g_flatfield.block[i][j] = s;
            if (s > max_sum) max_sum = s;
        }
    }

    /* Anchors. Center: avg of the 4 blocks straddling the grid's
     * geometric centre. Mid-edge: avg of the 4 quarter-radius
     * cardinal points (north/south/east/west of centre). Index
     * constants live near the storage decl so a future grid-size
     * change pulls them along. */
    uint32_t cs = g_flatfield.block[FF_CENTER_LO][FF_CENTER_LO]
                + g_flatfield.block[FF_CENTER_LO][FF_CENTER_HI]
                + g_flatfield.block[FF_CENTER_HI][FF_CENTER_LO]
                + g_flatfield.block[FF_CENTER_HI][FF_CENTER_HI];
    g_flatfield.anchor_center = cs / 4u;

    uint32_t es = g_flatfield.block[FF_CENTER_HI][FF_MID_NEAR]
                + g_flatfield.block[FF_CENTER_HI][FF_MID_FAR]
                + g_flatfield.block[FF_MID_NEAR][FF_CENTER_HI]
                + g_flatfield.block[FF_MID_FAR ][FF_CENTER_HI];
    g_flatfield.anchor_mid_edge = es / 4u;

    g_flatfield.anchor_max = max_sum;
    g_flatfield.valid = true;
    return 0;
}

void camera_flatfield_clear(void)
{
    g_flatfield.valid = false;
    g_flatfield.anchor_center   = 0;
    g_flatfield.anchor_max      = 0;
    g_flatfield.anchor_mid_edge = 0;
}

bool camera_flatfield_is_valid(void)
{
    return g_flatfield.valid;
}

uint32_t camera_flatfield_get(uint32_t i, uint32_t j)
{
    if (!g_flatfield.valid) return UINT32_MAX;
    if (i >= CAMERA_FLATFIELD_DIM || j >= CAMERA_FLATFIELD_DIM) {
        return UINT32_MAX;
    }
    return g_flatfield.block[i][j];
}

uint32_t camera_flatfield_anchor(uint32_t mode)
{
    return flatfield_anchor_for(mode);
}
