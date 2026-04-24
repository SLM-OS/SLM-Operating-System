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
 * Test-only helpers. Always present so tests linked into both test
 * and non-test builds resolve, but only intended for use from
 * kernel/tests/.
 */
void hailo_backend_reset_slots_for_tests(void);
void hailo_backend_get_boundary_iovas_for_tests(
    inference_model_handle_t h, uint64_t *in_iova, uint64_t *out_iova);
uint32_t hailo_backend_test_set_inflight(
    inference_model_handle_t h, uint32_t count);

#endif /* INFERENCE_DEVICE_HAILO_H */
