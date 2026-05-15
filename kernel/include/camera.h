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

/* Per-sample byte layout in `camera_frame.data`. Bayer pattern
 * (`CAMERA_BAYER_*`) is orthogonal to this — both fields must be
 * set by the producing backend.
 *
 *   RAW10_PACKED: CSI-2 RAW10 packed, 4 pixels in 5 bytes
 *     (bytes 0..3 = high 8 bits of pixels 0..3, byte 4 = packed
 *     low 2 bits). What the mock embed produces.
 *   T_R16:        VI atomp output for any RAW8/10/12 sensor on
 *     Tegra VI5: each sample stored as a little-endian u16 with
 *     the RAW value in bits [15:6] (high 10 bits) and bits [5:0]
 *     either zeroed or left undefined depending on
 *     `vi_channel_config.pixfmt.pad0_en`. What `camera_open
 *     ("imx219-0")` returns on Jetson.
 */
#define CAMERA_FORMAT_RAW10_PACKED  0u
#define CAMERA_FORMAT_T_R16         1u

/* MNIST input: 28*28 fp32 little-endian. Pinned by the runtime
 * inference path; see runtime/src/inference/gpu.rs. */
#define CAMERA_MNIST_OUT_BYTES (28u * 28u * 4u)

/* Flat-field reference grid dimensions. Matches the centre-crop
 * preprocess geometry one-to-one (28×28 MNIST blocks at 22 source
 * pixels per block over the 616×616 centred region) so the per-
 * block reference sum can divide the per-block input sum directly.
 * Anchored full-frame mode isn't supported — flat-field calibration
 * only makes sense for the center_crop=true preprocess path that
 * real captures use. */
#define CAMERA_FLATFIELD_DIM 28u

/* Normalization modes for camera_preprocess_mnist's normalize_mode
 * arg. NONE keeps the legacy contract (per-block sum / max). The
 * FLATFIELD_* modes divide each input block sum by the stored
 * flat-field reference block sum, then re-anchor to a chosen
 * scalar so the output stays in [0, 1]. The anchor controls which
 * scene location keeps its original brightness:
 *
 *   FLATFIELD_CENTER   — central 4 blocks' reference avg.
 *                        Vignetted corners brighten toward what
 *                        the centre reads.
 *   FLATFIELD_MAX      — max reference block sum (typically
 *                        somewhere near the centre, but not
 *                        guaranteed). Output ratio stays in
 *                        [0, 1] without clamping.
 *   FLATFIELD_MID_EDGE — average of blocks at half-radius from
 *                        centre. Compromise between the centre-
 *                        bright bias and the corner-noise bias.
 *
 * Modes other than NONE require a stored reference (capture one
 * via camera_flatfield_capture); without one, preprocess returns
 * -3 instead of silently falling back. */
#define CAMERA_NORMALIZE_NONE                0u
#define CAMERA_NORMALIZE_FLATFIELD_CENTER    1u
#define CAMERA_NORMALIZE_FLATFIELD_MAX       2u
#define CAMERA_NORMALIZE_FLATFIELD_MID_EDGE  3u
/* Sentinel — keep last. New modes must be inserted ABOVE this so
 * range checks (`mode < CAMERA_NORMALIZE_COUNT`) stay correct
 * automatically. */
#define CAMERA_NORMALIZE_COUNT               4u

/* Bound used by callers to size temporary stack buffers when probing
 * a capture. The IMX219 T_R16 1640x1232 buffer is the largest single
 * frame any current backend produces (~4 MB; double the packed RAW10
 * size because each sample occupies a full u16). */
#define CAMERA_FRAME_MAX_BYTES (1640u * 1232u * 2u)

struct camera_frame {
    /* Pointer into kernel-owned storage. The mock backend returns
     * a pointer into .rodata; real backends point into a DMA
     * buffer. Lifetime is tied to the camera handle. */
    const uint8_t *data;
    size_t         size;
    uint32_t       width;
    uint32_t       height;
    uint32_t       bayer;   /* CAMERA_BAYER_*  */
    uint32_t       format;  /* CAMERA_FORMAT_* */

    /* Per-backend defaults for `camera_preprocess_mnist`'s `invert`
     * and `center_crop` args. The mock backend ships MNIST-shaped
     * data (bright stroke on dark background, full-frame digit) so
     * both default false. The IMX219 backend captures raw
     * photographic data (black ink on white paper) through a wide-
     * angle lens with heavy corner vignette, so both default true.
     *
     * The Lua wrapper consults these when the caller omits the
     * corresponding optional arg — keeping the recommendation
     * data-driven from the backend rather than implicit on the
     * camera name string. */
    bool recommended_invert;
    bool recommended_center_crop;
};

/*
 * Resolve a camera by name and write a frame descriptor into *out.
 * Currently recognised names:
 *   "mock"     — embedded test frame (when MOCK_CAMERA_FRAME=ON;
 *                returns RAW10_PACKED).
 *   "imx219-0" — IMX219 on the J20 connector of the Jetson Orin
 *                Nano carrier (only when built for
 *                PLATFORM_JETSON_ORIN_NANO; returns T_R16).
 * Each call to `camera_open("imx219-0", …)` powers the sensor (if
 * needed), brings the RCE capture path online (idempotent), and
 * triggers ONE fresh capture. The returned frame buffer is kernel-
 * owned and overwritten on the next call.
 *
 * Returns 0 on success, negative on error:
 *   -1 = unknown camera name / NULL args
 *   -2 = backend not built (MOCK_CAMERA_FRAME=OFF, or imx219-0 on
 *        a non-Jetson platform)
 *   -3 = backend reached but capture failed (see WARN log for the
 *        failing stage; for imx219-0 this includes power-on,
 *        RCE IVC, sensor I²C, or VI Falcon errors).
 */
int camera_open(const char *name, struct camera_frame *out);

/*
 * Decode an RGGB Bayer frame into a 28x28 fp32 grayscale image
 * suitable for the MNIST classifier. Pipeline:
 *   1. Centred crop to a 1232x1232 square (drops the 204-pixel L/R
 *      margins of the 1640-wide source). Crop offsets are forced to
 *      be even so the RGGB phase is preserved.
 *   2. Extract green-channel pixels only (50% of the Bayer grid):
 *      RGGB has G at (y%2==0,x%2==1) and (y%2==1,x%2==0).
 *   3. Box-average each 44x44 source block into one output pixel.
 *      28*44 == 1232, so the grid covers the cropped square exactly.
 *   4. Normalise to fp32 in [0, 1] using the high 8 bits of each
 *      sample (low bits are dropped; the green-only MNIST classifier
 *      doesn't need 10-bit precision).
 *
 * `format` is `CAMERA_FORMAT_RAW10_PACKED` (mock) or
 * `CAMERA_FORMAT_T_R16` (IMX219 via VI atomp). Both formats yield
 * the same high-8-bit value per pixel, so the produced fp32 output
 * is byte-identical regardless of source format for a given scene.
 *
 * `invert` selects the output polarity. MNIST is trained on images
 * where the *digit strokes* are bright (~1.0) on a dark (~0.0)
 * background — i.e. the opposite of natural photographic data.
 *
 *   invert=false → straight normalize: bright input → output near 1.0
 *                  Correct for the embedded mock frame, which is
 *                  already MNIST-shaped (black background, bright
 *                  stroke pixels). All pre-existing pinned-output
 *                  tests use this polarity.
 *
 *   invert=true  → inverted normalize: bright input → output near 0.0
 *                  Correct for raw photographic data like the IMX219
 *                  sensor capturing a hand-drawn black digit on
 *                  white paper. Without this, the model receives a
 *                  photographic negative of what it was trained on,
 *                  and argmax collapses toward closed-shape classes
 *                  (0/6/8) regardless of the actual digit.
 *
 * `center_crop` selects how much of the source frame is sampled.
 *
 *   center_crop=false → cover the full 1232-tall frame: 28×44-pixel
 *                       blocks, no Y-offset, X-offset 204 (matches
 *                       the legacy mock-frame contract; pinned tests
 *                       use this mode).
 *
 *   center_crop=true  → use a 616×616 region centred on the sensor:
 *                       28×22-pixel blocks, X-offset 512, Y-offset
 *                       308. Required for the IMX219 backend because
 *                       the Tegra ISP isn't reachable from SLM-OS,
 *                       so the raw sensor frame carries the lens's
 *                       full vignette pattern (heavy ~50% light
 *                       fall-off at the corners on the standard
 *                       wide-angle modules). Sampling only the
 *                       central half rejects the vignetted region.
 *
 * Returns 0 on success. Negative on bad arguments:
 *   -1 = NULL pointer / out_bytes too small
 *   -2 = unsupported width/height/bayer/format (only RGGB 1640x1232
 *        in RAW10_PACKED or T_R16 today), unknown normalize_mode,
 *        or FLATFIELD_* mode requested with center_crop=false
 *   -3 = FLATFIELD_* mode requested but no flat-field reference
 *        is currently stored (capture one via
 *        camera_flatfield_capture)
 */
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
                            size_t         out_capacity);

/*
 * Capture-time flat-field calibration.
 *
 * The wide-angle IMX219 module on the Jetson Orin Nano carrier has
 * heavy lens vignette (~30-50 % corner light fall-off) which the
 * Tegra ISP's lens-shading correction would normally remove. The
 * ISP isn't reachable from SLM-OS, so the raw frame carries the
 * full vignette pattern. Capturing a uniformly-lit scene (a sheet
 * of blank paper filling the frame) and storing its per-block
 * green-channel sums gives `camera_preprocess_mnist` a reference
 * to divide subsequent inputs by, flattening the lens response.
 *
 * Same geometry as preprocess(center_crop=true): 28×28 blocks of
 * 22×22 source pixels each, walking the centred 616×616 region.
 *
 * Returns 0 on success, -1 on NULL/short buffer, -2 on shape
 * mismatch.
 */
int camera_flatfield_capture(const uint8_t *data,
                             size_t         data_len,
                             uint32_t       width,
                             uint32_t       height,
                             uint32_t       bayer,
                             uint32_t       format);

/* Drop the stored reference. Subsequent FLATFIELD_* preprocess
 * calls will return -3 until another capture lands. */
void camera_flatfield_clear(void);

/* True if a flat-field reference is currently stored. */
bool camera_flatfield_is_valid(void);

/* Read one cell of the stored reference. Returns the raw
 * green-channel sum (range [0, (CAMERA_FLATFIELD_DIM-block)²/2 *
 * 255] = [0, 61710]). Returns UINT32_MAX if (i, j) is out of
 * range or no reference is stored — useful for the operator-side
 * gradient analysis. */
uint32_t camera_flatfield_get(uint32_t i, uint32_t j);

/* The per-mode anchor value (the reference block sum the
 * normalization re-anchors corrected output to). Returns
 * UINT32_MAX if no reference is stored or `mode` isn't a
 * FLATFIELD_* mode (matches camera_flatfield_get's sentinel
 * convention so callers can disambiguate "no data" from a
 * legitimate zero anchor on a fully-dark reference). */
uint32_t camera_flatfield_anchor(uint32_t mode);
