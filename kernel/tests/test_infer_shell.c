/*
 * Inference Shell Verb Tests (#55 / PR #917).
 *
 * Exercises the argument-validation surface of:
 *
 *   - `infer batch ...` (kernel/src/shell_sys.c::cmd_infer)
 *   - `bench infer-stress ...` (kernel/src/shell_sys.c::cmd_bench)
 *
 * Both verbs forward to the Rust dynamic-batching FFI
 * (`rust_infer_batch_*` and `rust_infer_stress_run`). The Rust side
 * is covered end-to-end by `rust_batch_inference_test` (Tests 1-11
 * in runtime/src/lib.rs); the cases here pin the C-side parse +
 * range-check + dispatch paths that run synchronously before the
 * FFI call so a regression shows up in CI instead of only at
 * runtime.
 *
 * Tests that would require the scheduler to actually spawn worker
 * tasks (the success path of `bench infer-stress N`) live in
 * `rust_batch_inference_test` Test 11; this file deliberately
 * stays on the early-return branches.
 */

#include "unity.h"
#include "../include/shell.h"
#include "../include/shell_internal.h"
#include "../include/slm_ffi.h"
#include "../include/string.h"

/* ============================================================================
 * `infer batch ...` dispatch + argument validation
 * ========================================================================= */

/*
 * `infer` with no subcommand → 1 (usage printed). Covers the
 * `argc < 3 || strcmp(argv[1], "batch") != 0` early-return.
 */
static void test_infer_shell_no_subcommand_prints_usage(void)
{
    int ret = shell_execute("infer");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/*
 * `infer batch` with no sub-subcommand → 1. The collapsed argc<3
 * guard in cmd_infer (S6 from PR #917 review) covers both
 * "missing batch verb" and "missing on/off/config/status" with
 * a single early-return.
 */
static void test_infer_shell_batch_no_action(void)
{
    int ret = shell_execute("infer batch");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/*
 * `infer wrong-verb on` → 1 (only `batch` is recognised).
 */
static void test_infer_shell_unknown_verb(void)
{
    int ret = shell_execute("infer wrong-verb on");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/*
 * `infer batch garbage` → 1 (unknown sub-subcommand falls through
 * to the trailing "unknown subcommand" line).
 */
static void test_infer_shell_batch_unknown_subcommand(void)
{
    int ret = shell_execute("infer batch garbage");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/*
 * `infer batch on` and `infer batch off` round-trip through
 * `rust_infer_batch_get_mode`. Restore to OFF at the end so we
 * don't leak state into other tests.
 */
static void test_infer_shell_batch_on_off_round_trip(void)
{
    /* Force a known starting state. */
    rust_infer_batch_set_mode(0);
    TEST_ASSERT_EQUAL_INT32(0, rust_infer_batch_get_mode());

    int ret = shell_execute("infer batch on");
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_INT32(1, rust_infer_batch_get_mode());

    ret = shell_execute("infer batch off");
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_INT32(0, rust_infer_batch_get_mode());
}

/*
 * `infer batch config <size> <us>` argument validation. Each
 * negative case must NOT mutate the configuration (we sample with
 * `rust_infer_batch_status` before and after to verify).
 */
static void test_infer_shell_batch_config_missing_args(void)
{
    RustBatchStatus pre;
    rust_infer_batch_status(&pre);

    /* No size, no timeout. */
    int ret = shell_execute("infer batch config");
    TEST_ASSERT_EQUAL_INT(1, ret);

    /* Size only, no timeout. */
    ret = shell_execute("infer batch config 4");
    TEST_ASSERT_EQUAL_INT(1, ret);

    RustBatchStatus post;
    rust_infer_batch_status(&post);
    TEST_ASSERT_EQUAL_UINT32(pre.batch_size, post.batch_size);
    TEST_ASSERT_EQUAL_UINT32(pre.timeout_us, post.timeout_us);
}

static void test_infer_shell_batch_config_out_of_range(void)
{
    RustBatchStatus pre;
    rust_infer_batch_status(&pre);

    /* size = 0 → rejected. */
    int ret = shell_execute("infer batch config 0 5000");
    TEST_ASSERT_EQUAL_INT(1, ret);

    /* size = 33 → above MAX_BATCH (32). */
    ret = shell_execute("infer batch config 33 5000");
    TEST_ASSERT_EQUAL_INT(1, ret);

    /* timeout < 100 us — pinned by the shell-side guard. */
    ret = shell_execute("infer batch config 4 50");
    TEST_ASSERT_EQUAL_INT(1, ret);

    /* timeout > 100_000 us. */
    ret = shell_execute("infer batch config 4 200000");
    TEST_ASSERT_EQUAL_INT(1, ret);

    RustBatchStatus post;
    rust_infer_batch_status(&post);
    TEST_ASSERT_EQUAL_UINT32(pre.batch_size, post.batch_size);
    TEST_ASSERT_EQUAL_UINT32(pre.timeout_us, post.timeout_us);
}

/*
 * `infer batch config 4 1234` is in range → applied. Restore to
 * defaults at the end.
 */
static void test_infer_shell_batch_config_in_range_applies(void)
{
    int ret = shell_execute("infer batch config 4 1234");
    TEST_ASSERT_EQUAL_INT(0, ret);

    RustBatchStatus s;
    rust_infer_batch_status(&s);
    TEST_ASSERT_EQUAL_UINT32(4u, s.batch_size);
    TEST_ASSERT_EQUAL_UINT32(1234u, s.timeout_us);

    /* Restore defaults (matches `DEFAULT_BATCH_SIZE` / default
     * `BATCH_TIMEOUT_US` in runtime/src/sched/inference.rs). */
    rust_infer_batch_set_config(8, 5000);
}

/*
 * `infer batch status` → 0. We can't capture UART output here, but
 * a return of 0 proves the verb ran the FFI path through to
 * completion without faulting.
 */
static void test_infer_shell_batch_status_returns_ok(void)
{
    int ret = shell_execute("infer batch status");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/* ============================================================================
 * `bench infer-stress ...` argument validation
 * ========================================================================= */

/*
 * `bench infer-stress` with no N → 1 (usage). The success path
 * (`bench infer-stress 4 8`) requires a running scheduler and is
 * covered by `rust_batch_inference_test` Test 11.
 */
static void test_bench_infer_stress_no_workers(void)
{
    int ret = shell_execute("bench infer-stress");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/*
 * `bench infer-stress 0` → 1 (N must be 1..32).
 */
static void test_bench_infer_stress_zero_workers(void)
{
    int ret = shell_execute("bench infer-stress 0");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/*
 * `bench infer-stress 33` → 1 (N > 32).
 */
static void test_bench_infer_stress_oversize_workers(void)
{
    int ret = shell_execute("bench infer-stress 33");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/*
 * `bench infer-stress 4 abc` → 1. Pins the S5 fix from PR #917:
 * the iters arg used to silently default to 50 when parse failed;
 * now it returns 1 with a usage line.
 */
static void test_bench_infer_stress_bogus_iters(void)
{
    int ret = shell_execute("bench infer-stress 4 abc");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/*
 * `bench infer-stress 4 0` → 1 (iters must be > 0).
 */
static void test_bench_infer_stress_zero_iters(void)
{
    int ret = shell_execute("bench infer-stress 4 0");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/*
 * `bench infer-stress 4 100001` → 1 (iters above the upper bound).
 */
static void test_bench_infer_stress_oversize_iters(void)
{
    int ret = shell_execute("bench infer-stress 4 100001");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/* ============================================================================
 * Suite Runner
 * ========================================================================= */

int test_suite_infer_shell(void)
{
    UnityBegin("Inference Shell Verb Tests");

    RUN_TEST(test_infer_shell_no_subcommand_prints_usage);
    RUN_TEST(test_infer_shell_batch_no_action);
    RUN_TEST(test_infer_shell_unknown_verb);
    RUN_TEST(test_infer_shell_batch_unknown_subcommand);
    RUN_TEST(test_infer_shell_batch_on_off_round_trip);
    RUN_TEST(test_infer_shell_batch_config_missing_args);
    RUN_TEST(test_infer_shell_batch_config_out_of_range);
    RUN_TEST(test_infer_shell_batch_config_in_range_applies);
    RUN_TEST(test_infer_shell_batch_status_returns_ok);

    RUN_TEST(test_bench_infer_stress_no_workers);
    RUN_TEST(test_bench_infer_stress_zero_workers);
    RUN_TEST(test_bench_infer_stress_oversize_workers);
    RUN_TEST(test_bench_infer_stress_bogus_iters);
    RUN_TEST(test_bench_infer_stress_zero_iters);
    RUN_TEST(test_bench_infer_stress_oversize_iters);

    return (int)UnityEnd();
}
