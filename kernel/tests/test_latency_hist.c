/*
 * test_latency_hist.c - Unit tests for latency_hist + rate_ewma (M1)
 *
 * Pin the bucket math, percentile readout, and EWMA blend behavior so
 * downstream consumers (sched, eviction, inference) can rely on stable
 * semantics. No hardware deps — pure host-style tests over the kernel
 * library.
 */

#include "unity.h"
#include "../include/latency_hist.h"
#include "../include/rate_ewma.h"

#include <stdint.h>

/* ---------- latency_hist: bucket index ---------------------------- */

static void test_bucket_index_below_min_clamps_to_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, latency_hist_bucket_index(0u));
    TEST_ASSERT_EQUAL_UINT32(0u, latency_hist_bucket_index(1u));
    TEST_ASSERT_EQUAL_UINT32(0u, latency_hist_bucket_index(31u));
}

static void test_bucket_index_at_lower_bound(void)
{
    /* Bucket 0 covers [32, 64). 32 -> bucket 0. */
    TEST_ASSERT_EQUAL_UINT32(0u, latency_hist_bucket_index(32u));
    TEST_ASSERT_EQUAL_UINT32(0u, latency_hist_bucket_index(63u));
    /* 64 -> bucket 1 ([64, 128)). */
    TEST_ASSERT_EQUAL_UINT32(1u, latency_hist_bucket_index(64u));
    TEST_ASSERT_EQUAL_UINT32(1u, latency_hist_bucket_index(127u));
    /* 128 -> bucket 2. */
    TEST_ASSERT_EQUAL_UINT32(2u, latency_hist_bucket_index(128u));
}

static void test_bucket_index_at_high_powers(void)
{
    /* 2^36 (≈68.7s) -> bucket 31; 2^37 saturates to 31 too. */
    TEST_ASSERT_EQUAL_UINT32(31u, latency_hist_bucket_index((uint64_t)1u << 36));
    TEST_ASSERT_EQUAL_UINT32(31u, latency_hist_bucket_index((uint64_t)1u << 37));
    TEST_ASSERT_EQUAL_UINT32(31u, latency_hist_bucket_index(UINT64_MAX));
}

static void test_bucket_low_bound_helper(void)
{
    TEST_ASSERT_EQUAL_UINT64(32u, latency_hist_bucket_low_ns(0u));
    TEST_ASSERT_EQUAL_UINT64(64u, latency_hist_bucket_low_ns(1u));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)1u << 36, latency_hist_bucket_low_ns(31u));
}

/* ---------- latency_hist: record + reset --------------------------- */

static void test_record_increments_count_sum_minmax(void)
{
    struct latency_hist h;
    latency_hist_reset(&h);

    latency_hist_record(&h, 100u);
    TEST_ASSERT_EQUAL_UINT64(1u, h.count);
    TEST_ASSERT_EQUAL_UINT64(100u, h.sum_ns);
    TEST_ASSERT_EQUAL_UINT64(100u, h.min_ns);
    TEST_ASSERT_EQUAL_UINT64(100u, h.max_ns);

    latency_hist_record(&h, 50u);
    latency_hist_record(&h, 1000u);
    TEST_ASSERT_EQUAL_UINT64(3u, h.count);
    TEST_ASSERT_EQUAL_UINT64(1150u, h.sum_ns);
    TEST_ASSERT_EQUAL_UINT64(50u, h.min_ns);
    TEST_ASSERT_EQUAL_UINT64(1000u, h.max_ns);
}

static void test_reset_zeros_everything(void)
{
    struct latency_hist h;
    latency_hist_reset(&h);
    for (int i = 0; i < 100; i++) latency_hist_record(&h, 42u);
    TEST_ASSERT_EQUAL_UINT64(100u, h.count);
    latency_hist_reset(&h);
    TEST_ASSERT_EQUAL_UINT64(0u, h.count);
    TEST_ASSERT_EQUAL_UINT64(0u, h.sum_ns);
    TEST_ASSERT_EQUAL_UINT64(0u, h.min_ns);
    TEST_ASSERT_EQUAL_UINT64(0u, h.max_ns);
}

static void test_record_buckets_correctly(void)
{
    struct latency_hist h;
    latency_hist_reset(&h);

    latency_hist_record(&h, 32u);     /* bucket 0 */
    latency_hist_record(&h, 33u);     /* bucket 0 */
    latency_hist_record(&h, 64u);     /* bucket 1 */
    latency_hist_record(&h, 100u);    /* bucket 1 */
    latency_hist_record(&h, 1000u);   /* bucket 4: [512,1024) */

    TEST_ASSERT_EQUAL_UINT64(2u, h.buckets[0]);
    TEST_ASSERT_EQUAL_UINT64(2u, h.buckets[1]);
    TEST_ASSERT_EQUAL_UINT64(0u, h.buckets[2]);
    TEST_ASSERT_EQUAL_UINT64(0u, h.buckets[3]);
    TEST_ASSERT_EQUAL_UINT64(1u, h.buckets[4]);
}

/* ---------- latency_hist: percentile -------------------------------- */

static void test_percentile_empty_returns_zero(void)
{
    struct latency_hist h;
    latency_hist_reset(&h);
    TEST_ASSERT_EQUAL_UINT64(0u, latency_hist_percentile(&h, 50));
    TEST_ASSERT_EQUAL_UINT64(0u, latency_hist_percentile(&h, 99));
}

static void test_percentile_single_bucket(void)
{
    struct latency_hist h;
    latency_hist_reset(&h);
    /* All samples in bucket 1 ([64, 128)). p50 should land in bucket 1.
     * We return midpoint = low + low/2 = 64 + 32 = 96. */
    for (int i = 0; i < 100; i++) latency_hist_record(&h, 80u);
    TEST_ASSERT_EQUAL_UINT64(96u, latency_hist_percentile(&h, 50));
    TEST_ASSERT_EQUAL_UINT64(96u, latency_hist_percentile(&h, 99));
}

static void test_percentile_separates_low_and_high(void)
{
    struct latency_hist h;
    latency_hist_reset(&h);
    /* 90 samples at ~100 ns (bucket 1), 10 samples at ~10000 ns
     * (bucket 8: [8192, 16384)). p50 in bucket 1 → 96; p99 in bucket
     * 8 → 8192 + 4096 = 12288. */
    for (int i = 0; i < 90; i++) latency_hist_record(&h, 100u);
    for (int i = 0; i < 10; i++) latency_hist_record(&h, 10000u);
    TEST_ASSERT_EQUAL_UINT64(96u, latency_hist_percentile(&h, 50));
    /* p99: target = ceil(100 * 99 / 100) = 99 → first bucket whose
     * cumulative count crosses 99 is bucket 8. */
    TEST_ASSERT_EQUAL_UINT64(12288u, latency_hist_percentile(&h, 99));
}

static void test_percentile_clamps_out_of_range(void)
{
    struct latency_hist h;
    latency_hist_reset(&h);
    latency_hist_record(&h, 200u);   /* bucket 2 ([128, 256)) */
    /* pct=0 clamps to pct=1; pct=200 clamps to pct=99. Both should
     * land in the only bucket present (bucket 2: 128 + 64 = 192). */
    TEST_ASSERT_EQUAL_UINT64(192u, latency_hist_percentile(&h, 0));
    TEST_ASSERT_EQUAL_UINT64(192u, latency_hist_percentile(&h, 200));
}

/* ---------- latency_hist: snapshot ---------------------------------- */

static void test_snapshot_copies_state(void)
{
    struct latency_hist src, dst;
    latency_hist_reset(&src);
    for (int i = 0; i < 5; i++) latency_hist_record(&src, 100u);

    latency_hist_snapshot(&src, &dst);
    TEST_ASSERT_EQUAL_UINT64(5u, dst.count);
    TEST_ASSERT_EQUAL_UINT64(500u, dst.sum_ns);
    TEST_ASSERT_EQUAL_UINT64(5u, dst.buckets[1]);

    /* Snapshot is independent — mutating src doesn't change dst. */
    latency_hist_record(&src, 42u);
    TEST_ASSERT_EQUAL_UINT64(6u, src.count);
    TEST_ASSERT_EQUAL_UINT64(5u, dst.count);
}

/* ---------- rate_ewma ----------------------------------------------- */

static void test_rate_ewma_init_defaults(void)
{
    struct rate_ewma r;
    rate_ewma_init(&r, 0);
    TEST_ASSERT_EQUAL_UINT32(RATE_EWMA_DEFAULT_ALPHA_Q16, r.alpha_q16);
    TEST_ASSERT_EQUAL_UINT64(0u, r.last_event_ns);
    TEST_ASSERT_EQUAL_UINT64(0u, r.rate_per_s_q16);
}

static void test_rate_ewma_init_clamps_alpha(void)
{
    struct rate_ewma r;
    rate_ewma_init(&r, 100000u);  /* > 65536, should clamp */
    TEST_ASSERT_EQUAL_UINT32(RATE_EWMA_Q16_ONE, r.alpha_q16);
}

static void test_rate_ewma_first_tick_no_rate(void)
{
    struct rate_ewma r;
    rate_ewma_init(&r, 0);
    rate_ewma_tick(&r, 1000000u);
    /* No dt yet — rate stays at 0 until the second tick. */
    TEST_ASSERT_EQUAL_UINT64(0u, rate_ewma_get_q16(&r, 1000000u));
}

static void test_rate_ewma_steady_state_at_kHz(void)
{
    struct rate_ewma r;
    rate_ewma_init(&r, 0);
    /* Tick at 1 ms intervals → 1000 events/sec steady state. After
     * enough samples the EWMA should converge to ~1000. */
    uint64_t t = 1000000u;  /* 1 ms */
    for (int i = 0; i < 200; i++) {
        rate_ewma_tick(&r, t);
        t += 1000000u;
    }
    /* Allow a small smoothing band; expect within 5% of 1000/s. */
    uint64_t rate = rate_ewma_get(&r, t);
    TEST_ASSERT_MESSAGE(rate >= 950 && rate <= 1050,
        "EWMA should converge to ~1000/s for 1ms cadence");
}

static void test_rate_ewma_stale_decays_to_zero(void)
{
    struct rate_ewma r;
    rate_ewma_init(&r, 0);
    /* Build up a real rate. */
    uint64_t t = 1000000u;
    for (int i = 0; i < 50; i++) {
        rate_ewma_tick(&r, t);
        t += 1000000u;
    }
    TEST_ASSERT_TRUE(rate_ewma_get(&r, t) > 0);

    /* Jump 10 seconds forward without ticking → reader sees zero. */
    uint64_t now = t + (10ull * 1000ull * 1000ull * 1000ull);
    TEST_ASSERT_EQUAL_UINT64(0u, rate_ewma_get_q16(&r, now));
}

static void test_rate_ewma_handles_backwards_time(void)
{
    /* If two CPUs race the time source, `now` may briefly appear
     * to go backwards. Make sure that doesn't divide by zero or
     * produce a wild rate. */
    struct rate_ewma r;
    rate_ewma_init(&r, 0);
    rate_ewma_tick(&r, 1000000u);
    rate_ewma_tick(&r, 999999u);   /* backwards */
    /* No assertion on the value — just that we didn't crash and
     * the state is still well-formed. */
    TEST_ASSERT_NOT_EQUAL(0u, r.last_event_ns);
}

/* ---------- Suite registration -------------------------------------- */

int test_suite_latency_hist(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bucket_index_below_min_clamps_to_zero);
    RUN_TEST(test_bucket_index_at_lower_bound);
    RUN_TEST(test_bucket_index_at_high_powers);
    RUN_TEST(test_bucket_low_bound_helper);
    RUN_TEST(test_record_increments_count_sum_minmax);
    RUN_TEST(test_reset_zeros_everything);
    RUN_TEST(test_record_buckets_correctly);
    RUN_TEST(test_percentile_empty_returns_zero);
    RUN_TEST(test_percentile_single_bucket);
    RUN_TEST(test_percentile_separates_low_and_high);
    RUN_TEST(test_percentile_clamps_out_of_range);
    RUN_TEST(test_snapshot_copies_state);
    RUN_TEST(test_rate_ewma_init_defaults);
    RUN_TEST(test_rate_ewma_init_clamps_alpha);
    RUN_TEST(test_rate_ewma_first_tick_no_rate);
    RUN_TEST(test_rate_ewma_steady_state_at_kHz);
    RUN_TEST(test_rate_ewma_stale_decays_to_zero);
    RUN_TEST(test_rate_ewma_handles_backwards_time);
    return UNITY_END();
}
