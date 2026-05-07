/*
 * slm_ffi_test.h — test-only seams into slm_ffi internals.
 *
 * These declarations exist so unit tests can drive internal state
 * machines (today: the GPU dispatch circuit breaker in
 * `kernel/tests/test_gpu_dispatch_breaker.c`) without going through
 * a real GPU dispatch. They are deliberately separated from the
 * public `slm_ffi.h` so production translation units that include
 * the FFI header can't reach the seams by accident — only test
 * sources that explicitly include this header pick them up.
 *
 * Production code must not include this header. There is no runtime
 * check enforcing that, but the `_test_` prefix on each symbol plus
 * the segregation here makes the intent clear during code review.
 *
 * On non-Jetson platforms the breaker is a no-op stub (count is
 * always 0, record_result is a no-op) — see the `#else` branch in
 * `kernel/src/slm_ffi.c`.
 */
#ifndef SLM_FFI_TEST_H
#define SLM_FFI_TEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drive the breaker state machine without a real GPU dispatch.
 * `rc < 0` increments the consecutive-failure counter (and emits the
 * one-shot WARN when the threshold is crossed); `rc >= 0` zeroes
 * it. */
void slm_gpu_dispatch_breaker_test_record(int rc);

/* Read the raw consecutive-failure counter. Atomically loaded so
 * tests can assert exact values mid-run. */
int  slm_gpu_dispatch_breaker_test_count(void);

/* Sched-MLP dispatch rate-limit predicate (#651).
 *
 * Returns true iff `now - last < RATE_LIMIT_NS`, i.e. the next
 * dispatch should be rejected because the previous one finished
 * inside the window. Available on every platform — the predicate
 * itself is pure arithmetic — so the test runs cross-platform even
 * though the call site (`slm_gpu_run_sched_inference`) is gated to
 * Jetson. */
bool slm_gpu_sched_dispatch_test_within_window(uint64_t now, uint64_t last);

#ifdef __cplusplus
}
#endif

#endif /* SLM_FFI_TEST_H */
