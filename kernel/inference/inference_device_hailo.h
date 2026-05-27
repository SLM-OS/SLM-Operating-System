/*
 * inference_device_hailo.h — public helpers exported by the Hailo
 * inference backend beyond the generic `struct inference_device_ops`
 * surface.
 *
 * Created PR #355 review (2026-04-24) to consolidate the extern
 * declarations that were duplicated across hailo_shell.c, lua_slm.c,
 * and test_hailo.c. Symbols are implemented in
 * kernel/inference/inference_device_hailo.c.
 */

#ifndef INFERENCE_DEVICE_HAILO_H
#define INFERENCE_DEVICE_HAILO_H

#include <stdint.h>
#include "inference_device.h"

/*
 * Report transport byte counts for a loaded model. Used by callers
 * (Lua slm.hailo binding, scheduler shell load path) that need to
 * size policy buffers against backend-reported tensor bytes.
 *
 * Returns 0 (HAILO_OK) on success, non-zero on out-of-range or
 * unloaded handle. *in_bytes / *out_bytes left untouched on error.
 * Either pointer may be NULL to skip that output.
 */
int hailo_backend_model_sizes(inference_model_handle_t h,
                              uint32_t *in_bytes,
                              uint32_t *out_bytes);

/* Number of slots currently holding a loaded model. */
uint32_t hailo_backend_in_use_slots(void);

/* Maximum number of concurrent models the backend can hold. */
uint32_t hailo_backend_slots_max(void);

/*
 * #1001 follow-up: dump every host-RAM region fw might DMA-read from
 * during inference — CCWS data buffers (cfg ch0 + ch1), all four
 * descriptor lists (cfg ch0, cfg ch1, boundary IN, boundary OUT),
 * and the boundary IN/OUT tensor buffers — in a hex format suitable
 * for byte-diff against an equivalent capture from HailoRT/Linux.
 * Output format per region:
 *
 *     [dma-dump:<region>] iova=0x<hex> size=<dec>
 *     0x<offset_8hex>: bb bb bb bb bb bb bb bb bb bb bb bb bb bb bb bb
 *     ...
 *
 * Hex bytes lowercase, space-separated, 16 bytes per line, no ASCII
 * column. Cache-invalidates each region before dumping so the host
 * CPU observes fw's most recent DMA writes (relevant for the OUT
 * buffer post-inference; irrelevant pre-runmodel but harmless).
 *
 * Returns 0 on success, non-zero if `h` doesn't refer to a loaded
 * slot. Throughput is UART-bound; the CCWS dump alone takes ~40 s
 * at 115200 baud. Intended for once-per-investigation captures, not
 * production paths.
 */
int hailo_backend_dma_dump(inference_model_handle_t h);

/*
 * Test-only helpers (reset_slots_for_tests, get_boundary_iovas_for_tests,
 * test_set_inflight) live in inference_device_hailo_test_helpers.h.
 * Production code must NOT include that header.
 */

#endif /* INFERENCE_DEVICE_HAILO_H */
