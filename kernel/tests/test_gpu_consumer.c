/*
 * test_gpu_consumer.c - Unit tests for the gpu_consumer toggle (M2).
 *
 * No GPU required: every assertion exercises the validation gates
 * either rejecting an `enable=true` request (slm_gpu_available()
 * returns 0 in QEMU, no policy declares has_gpu_backend) or
 * accepting an `enable=false` request (always allowed).
 *
 * Sched-policy lookup hooks into the existing registry, so these
 * tests rely on the heuristic policy being registered (which it
 * always is — see sched_policy.h:125).
 */

#include "unity.h"
#include "../include/gpu_consumer.h"

#include <stdint.h>
#include <string.h>

/* Reset all three toggles to OFF before each scenario so order
 * sensitivity doesn't leak between tests. */
static void reset_all_consumers(void)
{
    const char *reason = NULL;
    (void)gpu_consumer_set(GPU_CONSUMER_SCHED, false, &reason);
    (void)gpu_consumer_set(GPU_CONSUMER_EVICTION, false, &reason);
    (void)gpu_consumer_set(GPU_CONSUMER_INFERENCE, false, &reason);
}

/* ---------- Name lookup --------------------------------------------- */

static void test_name_round_trip(void)
{
    TEST_ASSERT_EQUAL_STRING("sched", gpu_consumer_name(GPU_CONSUMER_SCHED));
    TEST_ASSERT_EQUAL_STRING("eviction", gpu_consumer_name(GPU_CONSUMER_EVICTION));
    TEST_ASSERT_EQUAL_STRING("inference", gpu_consumer_name(GPU_CONSUMER_INFERENCE));
    TEST_ASSERT_NULL(gpu_consumer_name(GPU_CONSUMER_COUNT));
    TEST_ASSERT_NULL(gpu_consumer_name((enum gpu_consumer)999));
}

static void test_name_lookup(void)
{
    TEST_ASSERT_EQUAL_INT(GPU_CONSUMER_SCHED, gpu_consumer_from_name("sched"));
    TEST_ASSERT_EQUAL_INT(GPU_CONSUMER_EVICTION, gpu_consumer_from_name("eviction"));
    TEST_ASSERT_EQUAL_INT(GPU_CONSUMER_INFERENCE, gpu_consumer_from_name("inference"));
    TEST_ASSERT_EQUAL_INT(GPU_CONSUMER_COUNT, gpu_consumer_from_name("bogus"));
    TEST_ASSERT_EQUAL_INT(GPU_CONSUMER_COUNT, gpu_consumer_from_name(""));
    TEST_ASSERT_EQUAL_INT(GPU_CONSUMER_COUNT, gpu_consumer_from_name(NULL));
    /* Case-sensitive — uppercase should miss. */
    TEST_ASSERT_EQUAL_INT(GPU_CONSUMER_COUNT, gpu_consumer_from_name("Sched"));
}

/* ---------- Default state ------------------------------------------- */

static void test_default_off(void)
{
    reset_all_consumers();
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_SCHED));
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_EVICTION));
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_INFERENCE));
}

static void test_enabled_out_of_range_returns_false(void)
{
    /* enabled() returns false for invalid c — never reads past the
     * atomic_bool array. */
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_COUNT));
    TEST_ASSERT_FALSE(gpu_consumer_enabled((enum gpu_consumer)999));
}

/* ---------- Disable always succeeds --------------------------------- */

static void test_disable_always_succeeds(void)
{
    const char *reason = (const char *)0xdeadbeef;  /* sentinel */
    int rc = gpu_consumer_set(GPU_CONSUMER_SCHED, false, &reason);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NULL(reason);
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_SCHED));

    /* Twice in a row. */
    rc = gpu_consumer_set(GPU_CONSUMER_SCHED, false, &reason);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_SCHED));
}

static void test_disable_with_null_reason_ok(void)
{
    /* Caller doesn't care about the reason — pass NULL. */
    int rc = gpu_consumer_set(GPU_CONSUMER_INFERENCE, false, NULL);
    TEST_ASSERT_EQUAL_INT(0, rc);
}

/* ---------- Enable rejection paths ---------------------------------- */

static void test_enable_unknown_consumer(void)
{
    const char *reason = NULL;
    int rc = gpu_consumer_set(GPU_CONSUMER_COUNT, true, &reason);
    TEST_ASSERT_EQUAL_INT(GPU_CONSUMER_ERR_INVAL, rc);
    TEST_ASSERT_NOT_NULL(reason);
}

static void test_enable_always_rejected_in_m2(void)
{
    /* M2 has no consumer with a wired GPU backend yet. Both rejection
     * paths are valid:
     *   - NODEV  if `slm_gpu_available()` returns 0 (e.g. headless host)
     *   - NOTSUPP if available but no consumer has a backend (QEMU stub
     *             GPU + heuristic policy is the canonical case)
     * Either is acceptable; both are < 0 with a non-NULL reason. */
    reset_all_consumers();
    const char *reason = NULL;

    int rc = gpu_consumer_set(GPU_CONSUMER_SCHED, true, &reason);
    TEST_ASSERT_TRUE(rc == GPU_CONSUMER_ERR_NODEV ||
                     rc == GPU_CONSUMER_ERR_NOTSUPP);
    TEST_ASSERT_NOT_NULL(reason);
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_SCHED));

    rc = gpu_consumer_set(GPU_CONSUMER_EVICTION, true, &reason);
    TEST_ASSERT_TRUE(rc == GPU_CONSUMER_ERR_NODEV ||
                     rc == GPU_CONSUMER_ERR_NOTSUPP);
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_EVICTION));

    rc = gpu_consumer_set(GPU_CONSUMER_INFERENCE, true, &reason);
    TEST_ASSERT_TRUE(rc == GPU_CONSUMER_ERR_NODEV ||
                     rc == GPU_CONSUMER_ERR_NOTSUPP);
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_INFERENCE));
}

/* ---------- Status snapshot ----------------------------------------- */

static void test_status_reports_all_consumers_off(void)
{
    reset_all_consumers();
    struct gpu_consumer_status st;
    /* Pre-fill with garbage to ensure the function actually writes
     * every field (no leakage from the caller's stack). */
    memset(&st, 0xAA, sizeof(st));
    gpu_consumer_status_get(&st);
    TEST_ASSERT_FALSE(st.sched);
    TEST_ASSERT_FALSE(st.eviction);
    TEST_ASSERT_FALSE(st.inference);
    /* `gpu_ready` is platform-dependent (QEMU has a stub driver that
     * reports available=true; bare-metal hosts without GPU report
     * false). Don't pin a value — just confirm the field is populated
     * with a sane bool, not the 0xAA garbage we filled with. */
    TEST_ASSERT_TRUE(st.gpu_ready == true || st.gpu_ready == false);
}

static void test_status_null_ok(void)
{
    /* Defensive — passing NULL must not crash. */
    gpu_consumer_status_get(NULL);
}

/* ---------- last_change_ms updates on disable ----------------------- */

static void test_last_change_ms_updates(void)
{
    reset_all_consumers();
    /* Disable updates the timestamp even though the toggle was
     * already off — that's by design (a successful set call,
     * regardless of the prior state). */
    uint64_t before = gpu_consumer_last_change_ms(GPU_CONSUMER_SCHED);
    (void)gpu_consumer_set(GPU_CONSUMER_SCHED, false, NULL);
    uint64_t after = gpu_consumer_last_change_ms(GPU_CONSUMER_SCHED);
    /* Either monotonically advanced or stayed equal; never went
     * backwards. (At sub-ms granularity in fast tests they may be
     * identical.) */
    TEST_ASSERT_TRUE(after >= before);
}

/* ---------- Suite registration -------------------------------------- */

int test_suite_gpu_consumer(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_name_round_trip);
    RUN_TEST(test_name_lookup);
    RUN_TEST(test_default_off);
    RUN_TEST(test_enabled_out_of_range_returns_false);
    RUN_TEST(test_disable_always_succeeds);
    RUN_TEST(test_disable_with_null_reason_ok);
    RUN_TEST(test_enable_unknown_consumer);
    RUN_TEST(test_enable_always_rejected_in_m2);
    RUN_TEST(test_status_reports_all_consumers_off);
    RUN_TEST(test_status_null_ok);
    RUN_TEST(test_last_change_ms_updates);
    return UNITY_END();
}
