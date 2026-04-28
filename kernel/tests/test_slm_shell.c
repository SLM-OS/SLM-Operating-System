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

    return (int)UnityEnd();
}
