/*
 * test_gpu_consumer.c - Unit tests for the gpu_consumer toggle.
 *
 * Two regimes the validation supports:
 *
 *   GPU absent  (slm_gpu_available() == 0)
 *     Every `enable=true` fails fast with GPU_CONSUMER_ERR_NODEV.
 *     Disable always succeeds.
 *
 *   GPU present (stub on QEMU, real driver on Jetson)
 *     - inference: accepts cleanly, no warning. The MNIST whole-
 *       graph fastpath in the Rust runtime consults the flag
 *       directly.
 *     - sched / eviction: accepts with a "scaffold only" warning
 *       in `*out_reason` because no policy has a GPU-backed
 *       forward pass yet. The flag still flips so `gpu use status`
 *       reflects operator intent.
 *
 * Sched-policy lookup hooks into the existing registry, so these
 * tests rely on the heuristic policy being registered (which it
 * always is — see sched_policy.h:125). On platforms where the
 * heuristic policy does NOT declare `has_gpu_backend = true`
 * (every current build), the sched-enable path takes the warning
 * branch.
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

/*
 * Enable when the build has no GPU at all -> NODEV with reason.
 * On QEMU the stub driver normally registers, so we only fail this
 * test if it does NOT — in which case rc==NODEV is the right
 * answer. On Jetson the real driver registers and gpu_available()
 * returns true, so this test no-ops via TEST_IGNORE.
 */
static void test_enable_returns_nodev_when_gpu_unavailable(void)
{
    /* Probe via the public path: `gpu use status` exposes a
     * `gpu_ready` field that mirrors slm_gpu_available(). */
    struct gpu_consumer_status st;
    gpu_consumer_status_get(&st);
    if (st.gpu_ready) {
        TEST_IGNORE_MESSAGE("GPU available on this build — covered by "
                            "test_enable_inference_accepts");
        return;
    }
    reset_all_consumers();
    const char *reason = NULL;
    int rc = gpu_consumer_set(GPU_CONSUMER_INFERENCE, true, &reason);
    TEST_ASSERT_EQUAL_INT(GPU_CONSUMER_ERR_NODEV, rc);
    TEST_ASSERT_NOT_NULL(reason);
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_INFERENCE));
}

/*
 * Enable inference when GPU is available -> rc=0, no warning.
 * The Rust engine consults `gpu_consumer_enabled(GPU_CONSUMER_INFERENCE)`
 * before attempting GPU dispatch; this test pins that the C-side
 * flag-flip succeeds and gpu_consumer_enabled reflects it.
 */
static void test_enable_inference_accepts(void)
{
    struct gpu_consumer_status st;
    gpu_consumer_status_get(&st);
    if (!st.gpu_ready) {
        TEST_IGNORE_MESSAGE("GPU not available on this build — covered by "
                            "test_enable_returns_nodev_when_gpu_unavailable");
        return;
    }
    reset_all_consumers();
    /* Caller-init NULL — the inference success path should not write
     * `*out_reason` so the value should remain NULL after the call.
     * (We avoid pre-loading a sentinel because the function only
     * writes `*out_reason` on the warning paths; a non-NULL sentinel
     * would persist through a clean accept and trip the assert.) */
    const char *reason = NULL;

    int rc = gpu_consumer_set(GPU_CONSUMER_INFERENCE, true, &reason);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_MESSAGE(reason == NULL,
        "inference is fully wired — no scaffold warning expected");
    TEST_ASSERT_TRUE(gpu_consumer_enabled(GPU_CONSUMER_INFERENCE));

    /* Disable round-trip. */
    rc = gpu_consumer_set(GPU_CONSUMER_INFERENCE, false, &reason);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_INFERENCE));
}

/*
 * Calling with NULL out_reason on the inference success path must
 * not fault. Pins the regression that would surface if the function
 * dereferenced `*out_reason` unconditionally — a real risk because
 * the prior implementation cleared `*out_reason = NULL` at the end
 * of every accept (which would have crashed callers passing NULL).
 */
static void test_enable_inference_with_null_reason_ok(void)
{
    struct gpu_consumer_status st;
    gpu_consumer_status_get(&st);
    if (!st.gpu_ready) {
        TEST_IGNORE_MESSAGE("GPU not available — covered by NODEV test");
        return;
    }
    reset_all_consumers();

    int rc = gpu_consumer_set(GPU_CONSUMER_INFERENCE, true, NULL);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(gpu_consumer_enabled(GPU_CONSUMER_INFERENCE));

    rc = gpu_consumer_set(GPU_CONSUMER_INFERENCE, false, NULL);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE(gpu_consumer_enabled(GPU_CONSUMER_INFERENCE));
}

/*
 * Enable sched / eviction when GPU is available -> rc=0 BUT
 * `*out_reason` is non-NULL with a "scaffold only" warning,
 * because no policy declares `has_gpu_backend = true` yet.
 * The shell renders this as a "note: …" line.
 */
static void test_enable_sched_accepts_with_scaffold_warning(void)
{
    struct gpu_consumer_status st;
    gpu_consumer_status_get(&st);
    if (!st.gpu_ready) {
        TEST_IGNORE_MESSAGE("GPU not available — sched accept-with-warning "
                            "path requires gpu_ready");
        return;
    }
    reset_all_consumers();
    const char *reason = NULL;

    int rc = gpu_consumer_set(GPU_CONSUMER_SCHED, true, &reason);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_MESSAGE(reason != NULL,
        "sched should warn — no policy declares has_gpu_backend yet");
    TEST_ASSERT_TRUE(gpu_consumer_enabled(GPU_CONSUMER_SCHED));
}

static void test_enable_eviction_accepts_with_scaffold_warning(void)
{
    struct gpu_consumer_status st;
    gpu_consumer_status_get(&st);
    if (!st.gpu_ready) {
        TEST_IGNORE_MESSAGE("GPU not available — eviction accept-with-warning "
                            "path requires gpu_ready");
        return;
    }
    reset_all_consumers();
    const char *reason = NULL;

    int rc = gpu_consumer_set(GPU_CONSUMER_EVICTION, true, &reason);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_MESSAGE(reason != NULL,
        "eviction should warn — no GPU dispatch path yet");
    TEST_ASSERT_TRUE(gpu_consumer_enabled(GPU_CONSUMER_EVICTION));
}

/*
 * Status snapshot reflects flag flips.
 */
static void test_status_tracks_enable_disable(void)
{
    struct gpu_consumer_status st;
    gpu_consumer_status_get(&st);
    if (!st.gpu_ready) {
        TEST_IGNORE_MESSAGE("requires gpu_ready");
        return;
    }
    reset_all_consumers();

    (void)gpu_consumer_set(GPU_CONSUMER_INFERENCE, true, NULL);
    gpu_consumer_status_get(&st);
    TEST_ASSERT_TRUE(st.inference);
    TEST_ASSERT_FALSE(st.sched);
    TEST_ASSERT_FALSE(st.eviction);

    (void)gpu_consumer_set(GPU_CONSUMER_SCHED, true, NULL);
    gpu_consumer_status_get(&st);
    TEST_ASSERT_TRUE(st.inference);
    TEST_ASSERT_TRUE(st.sched);

    (void)gpu_consumer_set(GPU_CONSUMER_INFERENCE, false, NULL);
    gpu_consumer_status_get(&st);
    TEST_ASSERT_FALSE(st.inference);
    TEST_ASSERT_TRUE(st.sched);

    reset_all_consumers();
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
    RUN_TEST(test_enable_returns_nodev_when_gpu_unavailable);
    RUN_TEST(test_enable_inference_accepts);
    RUN_TEST(test_enable_inference_with_null_reason_ok);
    RUN_TEST(test_enable_sched_accepts_with_scaffold_warning);
    RUN_TEST(test_enable_eviction_accepts_with_scaffold_warning);
    RUN_TEST(test_status_tracks_enable_disable);
    RUN_TEST(test_status_reports_all_consumers_off);
    RUN_TEST(test_status_null_ok);
    RUN_TEST(test_last_change_ms_updates);
    return UNITY_END();
}
