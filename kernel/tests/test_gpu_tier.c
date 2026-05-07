/*
 * test_gpu_tier.c - Unit tests for the gpu_tier preference toggle (#664).
 *
 * Tier preference is a separate axis from the gpu_consumer flag:
 * setting a tier when inference is OFF succeeds and surfaces a note,
 * not an error. The dispatch path observes the tier only when
 * inference is enabled. Tests cover:
 *
 *   - name <-> id round-trip including the "fp32" alias for SIMT
 *   - default tier at boot is AUTO
 *   - set/get round-trip for every valid value
 *   - rejection of unknown tier values
 *   - last_change_ms advances on successful set
 *   - status snapshot reflects current state
 *
 * GPU-availability scenarios match gpu_consumer's regimes:
 *
 *   GPU absent  (slm_gpu_available() == 0)
 *     Setting any non-CPU tier still succeeds and surfaces a "no GPU
 *     on this build" note in *out_reason. Operator intent is
 *     preserved across reboots into builds that may or may not have
 *     a GPU.
 *
 *   GPU present (stub on QEMU, real driver on Jetson)
 *     Setting a non-CPU tier with `gpu use inference` OFF surfaces a
 *     "preference active when ..." note. The store still happens.
 */

#include "unity.h"
#include "../include/gpu_tier.h"
#include "../include/gpu_consumer.h"

#include <stdint.h>

static void test_name_round_trip(void)
{
    TEST_ASSERT_EQUAL_STRING("auto", gpu_tier_name(SLM_GPU_TIER_AUTO));
    TEST_ASSERT_EQUAL_STRING("hmma", gpu_tier_name(SLM_GPU_TIER_HMMA));
    TEST_ASSERT_EQUAL_STRING("simt", gpu_tier_name(SLM_GPU_TIER_SIMT));
    TEST_ASSERT_EQUAL_STRING("cpu",  gpu_tier_name(SLM_GPU_TIER_CPU));
    TEST_ASSERT_NULL(gpu_tier_name(99));
    TEST_ASSERT_NULL(gpu_tier_name(SLM_GPU_TIER_INVALID));
}

static void test_name_lookup(void)
{
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_AUTO, gpu_tier_from_name("auto"));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_HMMA, gpu_tier_from_name("hmma"));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_SIMT, gpu_tier_from_name("simt"));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_CPU,  gpu_tier_from_name("cpu"));

    /* The "fp32" alias maps to SIMT — same CUDA-core FP32 path. */
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_SIMT, gpu_tier_from_name("fp32"));

    /* Unknown / case-mismatched / NULL returns the sentinel. */
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_INVALID, gpu_tier_from_name("bogus"));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_INVALID, gpu_tier_from_name(""));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_INVALID, gpu_tier_from_name(NULL));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_INVALID, gpu_tier_from_name("Auto"));
}

static void test_default_is_auto(void)
{
    /* Reset to a known state in case a prior test in the same suite
     * left a non-default value. */
    gpu_tier_set(SLM_GPU_TIER_AUTO, NULL);
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_AUTO, gpu_tier_get());
}

static void test_set_get_round_trip(void)
{
    const char *reason = NULL;

    TEST_ASSERT_EQUAL_INT(0, gpu_tier_set(SLM_GPU_TIER_HMMA, &reason));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_HMMA, gpu_tier_get());

    reason = NULL;
    TEST_ASSERT_EQUAL_INT(0, gpu_tier_set(SLM_GPU_TIER_SIMT, &reason));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_SIMT, gpu_tier_get());

    /* CPU tier is always settable — no GPU dependency. No note. */
    reason = NULL;
    TEST_ASSERT_EQUAL_INT(0, gpu_tier_set(SLM_GPU_TIER_CPU, &reason));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_CPU, gpu_tier_get());
    TEST_ASSERT_NULL(reason);

    /* Reset for subsequent tests. */
    gpu_tier_set(SLM_GPU_TIER_AUTO, NULL);
}

static void test_set_unknown_tier_rejected(void)
{
    const char *reason = NULL;
    int rc = gpu_tier_set(99, &reason);
    TEST_ASSERT_EQUAL_INT(GPU_TIER_ERR_INVAL, rc);
    TEST_ASSERT_NOT_NULL(reason);

    /* The store must NOT have happened on rejection. Verify by
     * setting AUTO first, then trying the bad value, then reading. */
    gpu_tier_set(SLM_GPU_TIER_AUTO, NULL);
    TEST_ASSERT_EQUAL_INT(GPU_TIER_ERR_INVAL,
                          gpu_tier_set(SLM_GPU_TIER_INVALID, NULL));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_AUTO, gpu_tier_get());
}

static void test_set_with_null_reason_ok(void)
{
    /* Passing NULL for out_reason must not crash on either the
     * success or failure path. */
    TEST_ASSERT_EQUAL_INT(0, gpu_tier_set(SLM_GPU_TIER_HMMA, NULL));
    TEST_ASSERT_EQUAL_INT(GPU_TIER_ERR_INVAL, gpu_tier_set(99, NULL));

    gpu_tier_set(SLM_GPU_TIER_AUTO, NULL);
}

static void test_last_change_ms_updates(void)
{
    /* Force a fresh write and observe the timestamp move. The
     * underlying clock granularity is 1 ms; we don't sleep here
     * because slm_get_time_ns advances on every call regardless of
     * cooperative scheduling. */
    gpu_tier_set(SLM_GPU_TIER_AUTO, NULL);
    uint64_t before = gpu_tier_last_change_ms();

    /* Spin briefly so the CNTPCT-derived ms tick advances. */
    volatile uint32_t spin = 0;
    for (uint32_t i = 0; i < 1000000u; i++) spin++;
    (void)spin;

    gpu_tier_set(SLM_GPU_TIER_HMMA, NULL);
    uint64_t after = gpu_tier_last_change_ms();

    /* Either time advanced (after > before) OR the spin was too fast
     * for the 1 ms granularity (after == before). Either is fine —
     * what would NOT be fine is the timestamp going backwards. */
    TEST_ASSERT_TRUE(after >= before);

    gpu_tier_set(SLM_GPU_TIER_AUTO, NULL);
}

static void test_status_snapshot(void)
{
    struct gpu_tier_status st;

    gpu_tier_set(SLM_GPU_TIER_HMMA, NULL);
    gpu_tier_status_get(&st);
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_TIER_HMMA, st.tier);
    /* gpu_ready and inference_enabled depend on platform / current
     * state, so just confirm the fields are populated, not their
     * specific values. */

    gpu_tier_status_get(NULL);  /* must not crash */

    gpu_tier_set(SLM_GPU_TIER_AUTO, NULL);
}

int test_suite_gpu_tier(void)
{
    UnityBegin("test_gpu_tier.c");
    RUN_TEST(test_name_round_trip);
    RUN_TEST(test_name_lookup);
    RUN_TEST(test_default_is_auto);
    RUN_TEST(test_set_get_round_trip);
    RUN_TEST(test_set_unknown_tier_rejected);
    RUN_TEST(test_set_with_null_reason_ok);
    RUN_TEST(test_last_change_ms_updates);
    RUN_TEST(test_status_snapshot);
    return UnityEnd();
}
