/*
 * camera.h — Camera capture API for SLM-OS.
 *
 * The runtime surface is shared between two backends:
 *
 *   - Mock camera: a baked-in 1640x1232 RAW10 RGGB frame embedded
 *     into the kernel via .incbin (gated on MOCK_CAMERA_FRAME=ON).
 *     Used by QEMU CI and as a fallback on hardware where the IMX219
 *     stack is not yet available. Always reachable when the embed is
 *     compiled in; does no allocation or I/O.
 *
 *   - IMX219 camera (Phase 0+ on Jetson, not yet implemented): real
 *     CSI capture. When it lands, camera_open("imx219-0") routes to
 *     it via the same struct camera_frame contract.
 *
 * preprocess_mnist takes a captured frame and produces 3,136 bytes of
 * fp32 in [0, 1] suitable for slm.model_infer_bytes (the MNIST
 * 1x1x28x28 input shape).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define CAMERA_BAYER_RGGB 0u
#define CAMERA_BAYER_GRBG 1u
#define CAMERA_BAYER_GBRG 2u
#define CAMERA_BAYER_BGGR 3u

/* MNIST input: 28*28 fp32 little-endian. Pinned by the runtime
 * inference path; see runtime/src/inference/gpu.rs. */
#define CAMERA_MNIST_OUT_BYTES (28u * 28u * 4u)

/* Bound used by callers to size temporary stack buffers when probing
 * a capture. The IMX219 2-lane RAW10 1640x1232 mode is the largest
 * single frame any current backend produces. */
#define CAMERA_FRAME_MAX_BYTES (1640u * 1232u * 10u / 8u)

struct camera_frame {
    /* Pointer into kernel-owned storage. The mock backend returns
     * a pointer into .rodata; real backends would point into a DMA
     * buffer. Lifetime is tied to the camera handle. */
    const uint8_t *data;
    size_t         size;
    uint32_t       width;
    uint32_t       height;
    uint32_t       bayer;   /* CAMERA_BAYER_* */
};

/*
 * Resolve a camera by name and write a frame descriptor into *out.
 * Currently recognised names: "mock" (when MOCK_CAMERA_FRAME is
 * compiled in). Returns 0 on success, negative on error:
 *   -1 = unknown camera name
 *   -2 = backend not built (e.g. MOCK_CAMERA_FRAME=OFF)
 */
int camera_open(const char *name, struct camera_frame *out);

/*
 * Decode a RAW10 RGGB Bayer frame into a 28x28 fp32 grayscale image
 * suitable for the MNIST classifier. Pipeline:
 *   1. Centred crop to a 1232x1232 square (drops the 204-pixel L/R
 *      margins of the 1640-wide source). Crop offsets are forced to
 *      be even so the RGGB phase is preserved.
 *   2. Extract green-channel pixels only (50% of the Bayer grid):
 *      RGGB has G at (y%2==0,x%2==1) and (y%2==1,x%2==0).
 *   3. Box-average each 44x44 source block into one output pixel.
 *      28*44 == 1232, so the grid covers the cropped square exactly.
 *   4. Normalise to fp32 in [0, 1] using the high 8 bits of each
 *      RAW10 sample (skips the 5th-byte low bits — the green-only
 *      MNIST classifier doesn't need 10-bit precision).
 *
 * Returns 0 on success. Negative on bad arguments:
 *   -1 = NULL pointer / out_bytes too small
 *   -2 = unsupported width/height/bayer (only RGGB 1640x1232 today)
 */
int camera_preprocess_mnist(const uint8_t *raw10,
                            size_t         raw10_len,
                            uint32_t       width,
                            uint32_t       height,
                            uint32_t       bayer,
                            uint8_t       *out_bytes,
                            size_t         out_capacity);
