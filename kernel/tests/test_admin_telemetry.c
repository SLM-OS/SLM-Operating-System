/*
 * test_admin_telemetry.c - Unit tests for the consumer-scoped
 * latency + rate counters wired into the eviction registry (Rust)
 * and the model_infer Lua bindings (M3).
 */

#include "unity.h"
#include "../include/admin_telemetry.h"
#include "../include/latency_hist.h"
#include "../include/rate_ewma.h"

#include <stdint.h>

/* ---------- Eviction --------------------------------------------- */

static void test_eviction_starts_zeroed_after_reset(void)
{
    admin_telemetry_reset_eviction();

    struct latency_hist h;
    uint64_t dr = 0, fr = 0, td = 0, tf = 0, tn = 0;
    int rc = admin_telemetry_get_eviction_stats(&h, &dr, &fr, &td, &tf, &tn);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT64(0, h.count);
    TEST_ASSERT_EQUAL_UINT64(0, dr);
    TEST_ASSERT_EQUAL_UINT64(0, fr);
    TEST_ASSERT_EQUAL_UINT64(0, td);
    TEST_ASSERT_EQUAL_UINT64(0, tf);
    TEST_ASSERT_EQUAL_UINT64(0, tn);
}

static void test_eviction_decision_records_latency_and_rate(void)
{
    admin_telemetry_reset_eviction();

    /* Simulate 50 victim selections, each ~5 us. */
    for (int i = 0; i < 50; i++) {
        admin_telemetry_record_eviction_decision(5000u);
    }

    struct latency_hist h;
    uint64_t dr = 0, fr = 0, td = 0, tf = 0, tn = 0;
    admin_telemetry_get_eviction_stats(&h, &dr, &fr, &td, &tf, &tn);

    TEST_ASSERT_EQUAL_UINT64(50, h.count);
    TEST_ASSERT_EQUAL_UINT64(50 * 5000ull, h.sum_ns);
    TEST_ASSERT_EQUAL_UINT64(50, td);
    TEST_ASSERT_EQUAL_UINT64(0, tf);
    TEST_ASSERT_EQUAL_UINT64(50 * 5000ull, tn);
    /* 5 us is between 2^12=4096 and 2^13=8192, bucket 12-5 = 7 (no — recompute):
     *   bucket 0 = [32,64), bucket i = [2^(i+5), 2^(i+6)).
     *   5000 falls in [4096, 8192) → bucket where 2^(i+5) = 4096 → i+5 = 12 → i = 7.
     */
    TEST_ASSERT_EQUAL_UINT64(50, h.buckets[7]);
}

static void test_eviction_fallback_separately_counted(void)
{
    admin_telemetry_reset_eviction();
    admin_telemetry_record_eviction_decision(1000u);
    admin_telemetry_record_eviction_decision(1000u);
    admin_telemetry_record_eviction_fallback();

    uint64_t td = 0, tf = 0;
    admin_telemetry_get_eviction_stats(NULL, NULL, NULL, &td, &tf, NULL);
    TEST_ASSERT_EQUAL_UINT64(2, td);
    TEST_ASSERT_EQUAL_UINT64(1, tf);
}

static void test_eviction_reset_zeros_state(void)
{
    /* Build up some state, then reset. Confirm everything is zero. */
    for (int i = 0; i < 10; i++) {
        admin_telemetry_record_eviction_decision(2000u);
    }
    admin_telemetry_record_eviction_fallback();

    admin_telemetry_reset_eviction();

    struct latency_hist h;
    uint64_t td = 0, tf = 0, tn = 0;
    admin_telemetry_get_eviction_stats(&h, NULL, NULL, &td, &tf, &tn);
    TEST_ASSERT_EQUAL_UINT64(0, h.count);
    TEST_ASSERT_EQUAL_UINT64(0, td);
    TEST_ASSERT_EQUAL_UINT64(0, tf);
    TEST_ASSERT_EQUAL_UINT64(0, tn);
}

/* ---------- Inference --------------------------------------------- */

static void test_inference_starts_zeroed_after_reset(void)
{
    admin_telemetry_reset_inference();

    struct latency_hist h;
    uint64_t cps = 0, eps = 0, tc = 0, te = 0, tn = 0;
    int rc = admin_telemetry_get_inference_stats(&h, &cps, &eps, &tc, &te, &tn);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT64(0, h.count);
    TEST_ASSERT_EQUAL_UINT64(0, tc);
    TEST_ASSERT_EQUAL_UINT64(0, te);
}

static void test_inference_records_call_and_error(void)
{
    admin_telemetry_reset_inference();

    /* Three successful calls, two errors. The latency histogram
     * counts every call regardless of ok/err — operators want to
     * see "how long does a failed call take" too. */
    admin_telemetry_record_inference(10000u, true);
    admin_telemetry_record_inference(10000u, true);
    admin_telemetry_record_inference(10000u, true);
    admin_telemetry_record_inference(10000u, false);
    admin_telemetry_record_inference(10000u, false);

    struct latency_hist h;
    uint64_t tc = 0, te = 0, tn = 0;
    admin_telemetry_get_inference_stats(&h, NULL, NULL, &tc, &te, &tn);

    TEST_ASSERT_EQUAL_UINT64(5, h.count);
    TEST_ASSERT_EQUAL_UINT64(5 * 10000ull, h.sum_ns);
    TEST_ASSERT_EQUAL_UINT64(5, tc);
    TEST_ASSERT_EQUAL_UINT64(2, te);
    TEST_ASSERT_EQUAL_UINT64(5 * 10000ull, tn);
}

static void test_inference_reset_zeros_state(void)
{
    admin_telemetry_record_inference(1000u, true);
    admin_telemetry_record_inference(1000u, false);
    admin_telemetry_reset_inference();

    struct latency_hist h;
    uint64_t tc = 0, te = 0;
    admin_telemetry_get_inference_stats(&h, NULL, NULL, &tc, &te, NULL);
    TEST_ASSERT_EQUAL_UINT64(0, h.count);
    TEST_ASSERT_EQUAL_UINT64(0, tc);
    TEST_ASSERT_EQUAL_UINT64(0, te);
}

/* ---------- NULL output args are safe ------------------------------ */

static void test_get_with_all_nulls_does_not_crash(void)
{
    /* Caller asks for nothing — every output pointer is NULL.
     * The implementation must skip every store rather than NULL-deref. */
    int rc = admin_telemetry_get_eviction_stats(NULL, NULL, NULL,
                                                NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = admin_telemetry_get_inference_stats(NULL, NULL, NULL,
                                             NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, rc);
}

/* ---------- Suite registration ------------------------------------- */

int test_suite_admin_telemetry(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_eviction_starts_zeroed_after_reset);
    RUN_TEST(test_eviction_decision_records_latency_and_rate);
    RUN_TEST(test_eviction_fallback_separately_counted);
    RUN_TEST(test_eviction_reset_zeros_state);
    RUN_TEST(test_inference_starts_zeroed_after_reset);
    RUN_TEST(test_inference_records_call_and_error);
    RUN_TEST(test_inference_reset_zeros_state);
    RUN_TEST(test_get_with_all_nulls_does_not_crash);
    return UNITY_END();
}
