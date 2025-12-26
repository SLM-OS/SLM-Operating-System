/*
 * test_shell.c - Shell Command Tests for SLM-OS
 *
 * Tests shell command dispatch, argument parsing, and error handling.
 * Note: Commands output to UART; these tests verify return values and behavior.
 */

#include "unity.h"
#include "../include/shell.h"
#include "../include/component.h"
#include "../include/vfs.h"
#include "../include/task.h"

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

/*
 * Clean up any components registered during tests.
 */
static void cleanup_components(void)
{
    for (uint32_t i = 0; i < COMPONENT_MAX_COUNT; i++) {
        component_unregister(i);
    }
}

/* ============================================================================
 * Command Dispatch Tests
 * ============================================================================ */

/*
 * Test: shell_execute with empty command returns 0.
 */
static void test_shell_empty_command(void)
{
    int ret = shell_execute("");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: shell_execute with whitespace-only returns 0.
 */
static void test_shell_whitespace_only(void)
{
    int ret = shell_execute("   ");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: Unknown command returns -1.
 */
static void test_shell_unknown_command(void)
{
    int ret = shell_execute("nonexistent_command");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'uptime' command executes successfully (minimal output).
 */
static void test_shell_cmd_uptime(void)
{
    int ret = shell_execute("uptime");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'clear' command executes successfully (minimal output).
 */
static void test_shell_cmd_clear(void)
{
    int ret = shell_execute("clear");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/* ============================================================================
 * VFS Command Tests (ls, cat) - Error Cases
 * ============================================================================ */

/*
 * Test: 'ls /nonexistent' returns error.
 */
static void test_shell_cmd_ls_nonexistent(void)
{
    int ret = shell_execute("ls /nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'cat' with no args shows usage (returns error).
 */
static void test_shell_cmd_cat_no_args(void)
{
    int ret = shell_execute("cat");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'cat /nonexistent' returns error.
 */
static void test_shell_cmd_cat_nonexistent(void)
{
    int ret = shell_execute("cat /nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'cat /sys' on directory returns error.
 */
static void test_shell_cmd_cat_directory(void)
{
    int ret = shell_execute("cat /sys");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ============================================================================
 * VFS Command Tests (ls, cat) - Success Cases
 * ============================================================================ */

/*
 * Test: 'ls /' lists root directory.
 */
static void test_shell_cmd_ls_root(void)
{
    int ret = shell_execute("ls /");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'ls /sys' lists sys directory.
 */
static void test_shell_cmd_ls_sys(void)
{
    int ret = shell_execute("ls /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'cat /sys/memory' reads memory info file.
 */
static void test_shell_cmd_cat_sys_memory(void)
{
    int ret = shell_execute("cat /sys/memory");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'cat /sys/cpus' reads CPU info file.
 */
static void test_shell_cmd_cat_sys_cpus(void)
{
    int ret = shell_execute("cat /sys/cpus");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/* ============================================================================
 * Verbose Status Command Tests
 * ============================================================================ */

/*
 * Test: 'help' lists available commands.
 */
static void test_shell_cmd_help(void)
{
    int ret = shell_execute("help");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'mem' shows memory statistics.
 */
static void test_shell_cmd_mem(void)
{
    int ret = shell_execute("mem");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'tasks' shows task list.
 */
static void test_shell_cmd_tasks(void)
{
    int ret = shell_execute("tasks");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'cpu' shows CPU information.
 */
static void test_shell_cmd_cpu(void)
{
    int ret = shell_execute("cpu");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'vmm' shows virtual memory mappings.
 */
static void test_shell_cmd_vmm(void)
{
    int ret = shell_execute("vmm");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'ipc' shows IPC statistics.
 */
static void test_shell_cmd_ipc(void)
{
    int ret = shell_execute("ipc");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'model' shows model memory info.
 */
static void test_shell_cmd_model(void)
{
    int ret = shell_execute("model");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'dtb' shows device tree info.
 */
static void test_shell_cmd_dtb(void)
{
    int ret = shell_execute("dtb");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/* ============================================================================
 * Component Command Tests
 * ============================================================================ */

/*
 * Test: 'component' with no args shows help (returns 0).
 */
static void test_shell_cmd_component_help(void)
{
    int ret = shell_execute("component");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'component list' shows empty list.
 */
static void test_shell_cmd_component_list_empty(void)
{
    cleanup_components();
    int ret = shell_execute("component list");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'component register' with missing args returns error.
 */
static void test_shell_cmd_component_register_missing_args(void)
{
    int ret = shell_execute("component register");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    ret = shell_execute("component register mycomp");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    ret = shell_execute("component register mycomp 1.0.0");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component register' with valid args succeeds.
 */
static void test_shell_cmd_component_register_valid(void)
{
    cleanup_components();

    int ret = shell_execute("component register test-svc 1.0.0 service");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify it was registered */
    int idx = component_find("test-svc");
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    cleanup_components();
}

/*
 * Test: 'component register' with invalid type returns error.
 */
static void test_shell_cmd_component_register_invalid_type(void)
{
    int ret = shell_execute("component register mycomp 1.0.0 invalid_type");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component unregister' with missing args returns error.
 */
static void test_shell_cmd_component_unregister_missing_args(void)
{
    int ret = shell_execute("component unregister");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component unregister' with invalid index returns error.
 */
static void test_shell_cmd_component_unregister_invalid(void)
{
    cleanup_components();
    int ret = shell_execute("component unregister 0");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component unregister' with valid index succeeds.
 */
static void test_shell_cmd_component_unregister_valid(void)
{
    cleanup_components();

    shell_execute("component register to-remove 1.0.0 service");
    TEST_ASSERT_EQUAL_UINT32(1, component_count());

    int ret = shell_execute("component unregister 0");
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(0, component_count());

    cleanup_components();
}

/*
 * Test: 'component status' with missing args returns error.
 */
static void test_shell_cmd_component_status_missing_args(void)
{
    int ret = shell_execute("component status");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component status' by index.
 */
static void test_shell_cmd_component_status_by_index(void)
{
    cleanup_components();

    shell_execute("component register status-test 1.0.0 driver");

    int ret = shell_execute("component status 0");
    TEST_ASSERT_EQUAL_INT(0, ret);

    cleanup_components();
}

/*
 * Test: 'component status' with nonexistent name returns error.
 */
static void test_shell_cmd_component_status_not_found(void)
{
    cleanup_components();
    int ret = shell_execute("component status nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component' with unknown subcommand returns error.
 */
static void test_shell_cmd_component_unknown_subcmd(void)
{
    int ret = shell_execute("component foobar");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ============================================================================
 * Run Command Tests
 * ============================================================================ */

/*
 * Test: 'run' with unknown program returns error.
 */
static void test_shell_cmd_run_unknown_program(void)
{
    int ret = shell_execute("run nonexistent_program");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ============================================================================
 * Kill Command Tests
 * ============================================================================ */

/*
 * Test: 'kill' with no args shows usage.
 */
static void test_shell_cmd_kill_no_args(void)
{
    int ret = shell_execute("kill");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'kill' with invalid PID format returns error.
 */
static void test_shell_cmd_kill_invalid_pid(void)
{
    int ret = shell_execute("kill abc");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'kill' with nonexistent PID returns error.
 */
static void test_shell_cmd_kill_nonexistent_pid(void)
{
    int ret = shell_execute("kill 9999");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ============================================================================
 * Argument Parsing Tests
 * ============================================================================ */

/*
 * Test: Commands handle extra whitespace.
 */
static void test_shell_extra_whitespace(void)
{
    int ret = shell_execute("  clear  ");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("\tclear\t");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: Commands handle arguments with spaces between.
 */
static void test_shell_args_with_spaces(void)
{
    cleanup_components();

    /* Register with multiple args separated by spaces */
    int ret = shell_execute("component   register   spaced-comp   1.0.0   service");
    TEST_ASSERT_EQUAL_INT(0, ret);

    int idx = component_find("spaced-comp");
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    cleanup_components();
}

/* ============================================================================
 * Command Registration Tests
 * ============================================================================ */

/* Custom command handler for testing */
static int test_custom_cmd_called = 0;

static int custom_cmd_handler(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    test_custom_cmd_called = 1;
    return 42;  /* Distinctive return value */
}

/*
 * Test: External command registration and execution.
 */
static void test_shell_register_external_command(void)
{
    test_custom_cmd_called = 0;

    shell_cmd_t cmd = {
        .name = "testcmd",
        .handler = custom_cmd_handler,
        .help = "Test command"
    };

    int ret = shell_register_command(&cmd);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Execute the custom command */
    ret = shell_execute("testcmd");
    TEST_ASSERT_EQUAL_INT(42, ret);
    TEST_ASSERT_EQUAL_INT(1, test_custom_cmd_called);
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_shell(void)
{
    UNITY_BEGIN();

    /* Command dispatch tests - essential */
    RUN_TEST(test_shell_empty_command);
    RUN_TEST(test_shell_whitespace_only);
    RUN_TEST(test_shell_unknown_command);

    /* Basic commands - just verify they execute (minimal output) */
    RUN_TEST(test_shell_cmd_clear);
    RUN_TEST(test_shell_cmd_uptime);

    /* VFS commands - error cases */
    RUN_TEST(test_shell_cmd_ls_nonexistent);
    RUN_TEST(test_shell_cmd_cat_no_args);
    RUN_TEST(test_shell_cmd_cat_nonexistent);
    RUN_TEST(test_shell_cmd_cat_directory);

    /* VFS commands - success cases */
    RUN_TEST(test_shell_cmd_ls_root);
    RUN_TEST(test_shell_cmd_ls_sys);
    RUN_TEST(test_shell_cmd_cat_sys_memory);
    RUN_TEST(test_shell_cmd_cat_sys_cpus);

    /* Verbose status commands */
    RUN_TEST(test_shell_cmd_help);
    RUN_TEST(test_shell_cmd_mem);
    RUN_TEST(test_shell_cmd_tasks);
    RUN_TEST(test_shell_cmd_cpu);
    RUN_TEST(test_shell_cmd_vmm);
    RUN_TEST(test_shell_cmd_ipc);
    RUN_TEST(test_shell_cmd_model);
    RUN_TEST(test_shell_cmd_dtb);

    /* Component commands - core functionality */
    RUN_TEST(test_shell_cmd_component_help);
    RUN_TEST(test_shell_cmd_component_list_empty);
    RUN_TEST(test_shell_cmd_component_register_missing_args);
    RUN_TEST(test_shell_cmd_component_register_valid);
    RUN_TEST(test_shell_cmd_component_register_invalid_type);
    RUN_TEST(test_shell_cmd_component_unregister_missing_args);
    RUN_TEST(test_shell_cmd_component_unregister_invalid);
    RUN_TEST(test_shell_cmd_component_unregister_valid);
    RUN_TEST(test_shell_cmd_component_status_missing_args);
    RUN_TEST(test_shell_cmd_component_status_by_index);
    RUN_TEST(test_shell_cmd_component_status_not_found);
    RUN_TEST(test_shell_cmd_component_unknown_subcmd);

    /* Run command - error cases only */
    RUN_TEST(test_shell_cmd_run_unknown_program);

    /* Kill command - error cases only */
    RUN_TEST(test_shell_cmd_kill_no_args);
    RUN_TEST(test_shell_cmd_kill_invalid_pid);
    RUN_TEST(test_shell_cmd_kill_nonexistent_pid);

    /* Argument parsing */
    RUN_TEST(test_shell_extra_whitespace);
    RUN_TEST(test_shell_args_with_spaces);

    /* External command registration */
    RUN_TEST(test_shell_register_external_command);

    return UNITY_END();
}
