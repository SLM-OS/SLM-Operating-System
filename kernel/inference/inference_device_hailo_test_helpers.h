/*
 * inference_device_hailo_test_helpers.h — test-only entry points
 * exported by the Hailo backend.
 *
 * Do NOT include from production code paths. These helpers exist
 * solely to let unit tests under kernel/tests/ poke backend state
 * (slot table reset, simulated inflight runs, boundary IOVA dump)
 * that is not part of the public production API. Implementations
 * are always present in inference_device_hailo.c so the symbols
 * resolve in both test and non-test linkage, but normal callers
 * should depend on inference_device_hailo.h instead.
 *
 * Created PR #355 review round 2 (2026-04-24) to keep the production
 * header free of test-only declarations.
 */

#ifndef INFERENCE_DEVICE_HAILO_TEST_HELPERS_H
#define INFERENCE_DEVICE_HAILO_TEST_HELPERS_H

#include <stdint.h>
#include "inference_device.h"

/* Wipe the slot table without releasing DMA — used by tests that
 * need a known-clean starting state. Skips free_model's unwind path
 * deliberately, so test fixtures don't need real DMA backing. */
void hailo_backend_reset_slots_for_tests(void);

/* Expose load-time boundary IOVAs so run-path tests can assert that
 * submit happens via the same IOVAs firmware was told about in
 * ACTIVATION. Out-of-range / unloaded handle returns 0 for both. */
void hailo_backend_get_boundary_iovas_for_tests(
    inference_model_handle_t h, uint64_t *in_iova, uint64_t *out_iova);

/* Set the slot's inflight_runs counter so a test can simulate "run is
 * in flight" without firing the full inference path. Returns the
 * previous count, or UINT32_MAX for an out-of-range or unloaded
 * handle. Audit F-07 regression coverage. */
uint32_t hailo_backend_test_set_inflight(
    inference_model_handle_t h, uint32_t count);

#endif /* INFERENCE_DEVICE_HAILO_TEST_HELPERS_H */
