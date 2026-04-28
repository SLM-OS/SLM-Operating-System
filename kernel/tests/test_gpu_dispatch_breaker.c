/*
 * test_gpu_dispatch_breaker.c — unit tests for the GPU dispatch
 * circuit breaker (PR #555 / mitigation for #552).
 *
 * The breaker is a small atomic state machine in `kernel/src/slm_ffi.c`:
 *
 *   - record_result(rc < 0) increments the counter; record_result(rc
 *     >= 0) zeroes it.
 *   - is_tripped() returns true once the counter reaches
 *     GPU_DISPATCH_BREAKER_THRESHOLD (default 3).
 *   - reset() forces the counter back to zero.
 *   - A one-shot WARN log fires exactly when the threshold is
 *     crossed, not on every subsequent failure.
 *
 * Tests drive the state machine via two test seams exposed in
 * `slm_ffi.h` (slm_gpu_dispatch_breaker_test_record / _test_count)
 * — production code paths reach the same state machine through
 * the dispatch hot-path (slm_gpu_run_mnist / _sched_inference),
 * which can't be exercised in QEMU without a real GA10B.
 *
 * On platforms without PLATFORM_JETSON_ORIN_NANO, the seams are
 * no-ops (count is always 0, is_tripped is always false). The
 * "Jetson-only" tests explicitly skip themselves there so the
 * suite stays green on QEMU and x86-64.
 */

#include "unity.h"
#include "../include/slm_ffi.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(PLATFORM_JETSON_ORIN_NANO)

/*
 * Reset the breaker to a known-clean state at the start of each
 * test. Calling _reset() also clears the counter and emits a one-
 * shot INFO line if the breaker had been tripped. Cheap to call
 * unconditionally so each test is independent of ordering.
 *
 * Only defined on Jetson — the non-Jetson stub path doesn't need
 * setup, and -Werror=unused-function would otherwise reject it.
 */
static void breaker_setup(void)
{
    slm_gpu_dispatch_breaker_reset();
}

/*
 * Default state: counter is 0, breaker is not tripped.
 */
static void test_breaker_default_state(void)
{
    breaker_setup();
    TEST_ASSERT_EQUAL_INT(0, slm_gpu_dispatch_breaker_test_count());
    TEST_ASSERT_FALSE(slm_gpu_dispatch_breaker_is_tripped());
}

/*
 * One failure increments the counter to 1; breaker not yet tripped
 * (threshold is 3 by default).
 */
static void test_breaker_single_failure_does_not_trip(void)
{
    breaker_setup();
    slm_gpu_dispatch_breaker_test_record(-1);
    TEST_ASSERT_EQUAL_INT(1, slm_gpu_dispatch_breaker_test_count());
    TEST_ASSERT_FALSE(slm_gpu_dispatch_breaker_is_tripped());
}

/*
 * Two failures: counter at 2, still not tripped.
 */
static void test_breaker_two_failures_do_not_trip(void)
{
    breaker_setup();
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-2);
    TEST_ASSERT_EQUAL_INT(2, slm_gpu_dispatch_breaker_test_count());
    TEST_ASSERT_FALSE(slm_gpu_dispatch_breaker_is_tripped());
}

/*
 * Three back-to-back failures: counter at 3, breaker is tripped.
 * (Default threshold is 3 — see GPU_DISPATCH_BREAKER_THRESHOLD in
 * slm_ffi.c. If a future build overrides the threshold via
 * -DGPU_DISPATCH_BREAKER_THRESHOLD=N this test should be updated.)
 */
static void test_breaker_three_failures_trip(void)
{
    breaker_setup();
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-2);
    slm_gpu_dispatch_breaker_test_record(-3);
    TEST_ASSERT_EQUAL_INT(3, slm_gpu_dispatch_breaker_test_count());
    TEST_ASSERT_TRUE(slm_gpu_dispatch_breaker_is_tripped());
}

/*
 * One success zeroes the counter mid-run, even after several
 * failures had accumulated below the threshold.
 */
static void test_breaker_success_resets_counter(void)
{
    breaker_setup();
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-2);
    slm_gpu_dispatch_breaker_test_record(0);   /* success */
    TEST_ASSERT_EQUAL_INT(0, slm_gpu_dispatch_breaker_test_count());
    TEST_ASSERT_FALSE(slm_gpu_dispatch_breaker_is_tripped());
}

/*
 * Manual reset clears the counter even after the breaker has tripped.
 * This is the operator-initiated recovery path (gpu use inference on
 * calls slm_gpu_dispatch_breaker_reset).
 */
static void test_breaker_manual_reset_clears_tripped_state(void)
{
    breaker_setup();
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-1);
    TEST_ASSERT_TRUE(slm_gpu_dispatch_breaker_is_tripped());
    slm_gpu_dispatch_breaker_reset();
    TEST_ASSERT_EQUAL_INT(0, slm_gpu_dispatch_breaker_test_count());
    TEST_ASSERT_FALSE(slm_gpu_dispatch_breaker_is_tripped());
}

/*
 * Once tripped, additional failures continue to increment the
 * counter. The state machine doesn't saturate on the threshold —
 * the counter just keeps growing. (The WARN is one-shot, but the
 * counter itself is monotonic until a success or manual reset.)
 */
static void test_breaker_continues_counting_past_threshold(void)
{
    breaker_setup();
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-1);
    TEST_ASSERT_EQUAL_INT(5, slm_gpu_dispatch_breaker_test_count());
    TEST_ASSERT_TRUE(slm_gpu_dispatch_breaker_is_tripped());
}

/*
 * Failure-then-success-then-failure pattern: the success resets to
 * 0, so the next failure goes to 1, NOT back to where we were.
 * This is the recovery-then-re-degrade case — should be treated as
 * a fresh failure run.
 */
static void test_breaker_resets_on_intermittent_success(void)
{
    breaker_setup();
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(0);   /* recovery */
    slm_gpu_dispatch_breaker_test_record(-1);
    TEST_ASSERT_EQUAL_INT(1, slm_gpu_dispatch_breaker_test_count());
    TEST_ASSERT_FALSE(slm_gpu_dispatch_breaker_is_tripped());
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

/*
 * On non-Jetson platforms the breaker is a no-op stub. Sanity-
 * check that record/count/reset/is_tripped all behave as
 * documented (count == 0, is_tripped == false, reset is harmless).
 */
static void test_breaker_stubs_on_non_jetson(void)
{
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-1);
    slm_gpu_dispatch_breaker_test_record(-1);
    TEST_ASSERT_EQUAL_INT(0, slm_gpu_dispatch_breaker_test_count());
    TEST_ASSERT_FALSE(slm_gpu_dispatch_breaker_is_tripped());
    slm_gpu_dispatch_breaker_reset();
    TEST_ASSERT_EQUAL_INT(0, slm_gpu_dispatch_breaker_test_count());
    TEST_ASSERT_FALSE(slm_gpu_dispatch_breaker_is_tripped());
}

#endif

int test_suite_gpu_dispatch_breaker(void)
{
    UNITY_BEGIN();
#if defined(PLATFORM_JETSON_ORIN_NANO)
    RUN_TEST(test_breaker_default_state);
    RUN_TEST(test_breaker_single_failure_does_not_trip);
    RUN_TEST(test_breaker_two_failures_do_not_trip);
    RUN_TEST(test_breaker_three_failures_trip);
    RUN_TEST(test_breaker_success_resets_counter);
    RUN_TEST(test_breaker_manual_reset_clears_tripped_state);
    RUN_TEST(test_breaker_continues_counting_past_threshold);
    RUN_TEST(test_breaker_resets_on_intermittent_success);
#else
    RUN_TEST(test_breaker_stubs_on_non_jetson);
#endif
    return UNITY_END();
}
