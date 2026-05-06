/*
 * SLM Shell Verb Tests (Phase SLM, M7.1).
 *
 * Exercises the entry-point dispatch in cmd_slm — covering the
 * three main behaviors that don't require live model bytes:
 *
 *   1. `slm load <bad-path>` returns non-zero (file not found).
 *   2. `slm status` with an empty registry prints "models=0
 *      sessions=0".
 *   3. `slm <unknown-verb>` returns non-zero with help text.
 *
 * Hardware-side acceptance (real GGUF load, prompt streaming,
 * stats) lives in the integration / hardware-deploy path documented
 * in docs/specs/slm-integration.md.
 */

#include "unity.h"
#include "../include/shell.h"
#include "../include/shell_internal.h"
#include "../include/slm_shell.h"
#include "../include/slm_ffi.h"
#include "../include/string.h"

/* ============================================================================
 * Tests
 * ========================================================================= */

/*
 * `slm load /<missing>` → non-zero. The file doesn't exist in the
 * test VFS, so vfs_stat_path returns -1 and the verb fails cleanly.
 * Confirms the verb dispatch and error path don't crash.
 */
static void test_slm_shell_load_invalid_path(void)
{
    /* Reset the SLM registry so the test starts from a known state. */
    rust_slm_test_reset();

    /* shell_execute parses the line into argv. The path is unlikely
     * to exist in the test VFS image. */
    int ret = shell_execute("slm load /no/such/slm/file.gguf");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * `slm status` with no models loaded returns 0 and prints a one-line
 * summary. We can't easily snoop the UART output here, but we assert
 * the dispatch returns 0 (success) which proves both the verb table
 * lookup and the status handler ran to completion.
 */
static void test_slm_shell_status_with_no_models(void)
{
    rust_slm_test_reset();
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());

    int ret = shell_execute("slm status");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * `slm <garbage-verb>` returns -1 and prints help. Confirms the
 * dispatcher's fall-through path fires for unknown verbs without
 * crashing the shell.
 */
static void test_slm_shell_dispatch_unknown_verb(void)
{
    rust_slm_test_reset();

    int ret = shell_execute("slm garbage");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * `slm` with no verb prints usage and returns non-zero.
 */
static void test_slm_shell_no_verb_prints_usage(void)
{
    rust_slm_test_reset();

    int ret = shell_execute("slm");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * `slm info <bad-handle>` returns non-zero (slot empty).
 */
static void test_slm_shell_info_empty_slot(void)
{
    rust_slm_test_reset();

    int ret = shell_execute("slm info 0");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * `slm list` with empty registry returns 0 ("No SLMs loaded.").
 */
static void test_slm_shell_list_empty_registry(void)
{
    rust_slm_test_reset();

    int ret = shell_execute("slm list");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/* ============================================================================
 * `slm xload` argument-validation coverage
 * ----------------------------------------------------------------------------
 * The streaming body of `slm xload` requires a live telnet session in
 * binary mode and can't be exercised under Unity, but the verb's
 * argument-parse + cap-check guards run synchronously before any IO
 * begins. Cover each negative branch that returns before the read
 * loop so a future regression in dispatch / parse shows up in CI
 * instead of on hardware. Each case must NOT alter the registry; we
 * assert that with a `rust_slm_count()` check after.
 * ========================================================================= */

/* `slm xload` with no name/total → -1 (usage). */
static void test_slm_shell_xload_missing_args(void)
{
    rust_slm_test_reset();
    int ret = shell_execute("slm xload");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

/* `slm xload qwen` with no total → -1 (still missing args). */
static void test_slm_shell_xload_missing_total(void)
{
    rust_slm_test_reset();
    int ret = shell_execute("slm xload qwen");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

/* `slm xload qwen abc` → -1 (parse_uint rejects non-digits). */
static void test_slm_shell_xload_invalid_total(void)
{
    rust_slm_test_reset();
    int ret = shell_execute("slm xload qwen abc");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

/* `slm xload qwen 0` → -1 (total must be > 0; otherwise we'd
 * pmm_alloc_pages(0) and stream into a zero-size buffer). */
static void test_slm_shell_xload_zero_total(void)
{
    rust_slm_test_reset();
    int ret = shell_execute("slm xload qwen 0");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

/* `slm xload qwen <too-large>` → -1 (cap check via
 * rust_slm_max_gguf_bytes; declines BEFORE pmm_alloc_pages, so we
 * don't actually try to grab a 4 GB buddy block in the test.) */
static void test_slm_shell_xload_oversize_total(void)
{
    rust_slm_test_reset();
    /* uint32 max is 4294967295; the registry cap is 2 GiB, so this
     * is comfortably above and exercises the cap branch. */
    int ret = shell_execute("slm xload qwen 4000000000");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

/* ============================================================================
 * Suite Runner
 * ========================================================================= */

int test_suite_slm_shell(void)
{
    UnityBegin("SLM Shell Verb Tests");

    RUN_TEST(test_slm_shell_load_invalid_path);
    RUN_TEST(test_slm_shell_status_with_no_models);
    RUN_TEST(test_slm_shell_dispatch_unknown_verb);
    RUN_TEST(test_slm_shell_no_verb_prints_usage);
    RUN_TEST(test_slm_shell_info_empty_slot);
    RUN_TEST(test_slm_shell_list_empty_registry);
    RUN_TEST(test_slm_shell_xload_missing_args);
    RUN_TEST(test_slm_shell_xload_missing_total);
    RUN_TEST(test_slm_shell_xload_invalid_total);
    RUN_TEST(test_slm_shell_xload_zero_total);
    RUN_TEST(test_slm_shell_xload_oversize_total);

    return (int)UnityEnd();
}
