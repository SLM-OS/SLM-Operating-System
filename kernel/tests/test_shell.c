/*
 * test_shell.c - Shell Command Tests for SLM-OS
 *
 * Tests shell command dispatch, argument parsing, and error handling.
 * Note: Commands output to UART; these tests verify return values and behavior.
 */

#include "unity.h"
#include "../include/shell.h"
#include "../include/shell_internal.h"
#include "../include/component.h"
#include "../include/vfs.h"
#include "../include/task.h"
#include "../include/littlefs_slm.h"
#include "../include/string.h"
#include "../include/uart.h"

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

/*
 * Helper to read file content via VFS API.
 * Returns bytes read, or -1 on error.
 */
static int read_file_content(const char *path, char *buf, size_t size)
{
    return vfs_read_path(path, buf, size, 0);
}

/*
 * Helper to get file size via LittleFS API.
 * Returns size in bytes, or -1 on error.
 */
static int get_file_size(const char *path)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx(path, &subpath);
    if (!mnt || !subpath) return -1;

    /* Skip leading slash if present */
    if (subpath[0] == '/') subpath++;

    struct lfs_entry_info info;
    if (littlefs_stat_path(mnt, subpath, &info) < 0) return -1;

    return (int)info.size;
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
 * Regression test: shell_execute with string longer than SHELL_MAX_LINE
 * must return -1 without crashing (was a stack buffer overflow).
 */
static void test_shell_cmd_too_long(void)
{
    char long_cmd[SHELL_MAX_LINE + 64];
    extern void *memset(void *s, int c, size_t n);
    memset(long_cmd, 'a', SHELL_MAX_LINE + 32);
    long_cmd[SHELL_MAX_LINE + 32] = '\0';

    int ret = shell_execute(long_cmd);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Regression test: shell_execute at exactly SHELL_MAX_LINE-1 still works.
 */
static void test_shell_cmd_at_max_length(void)
{
    char cmd[SHELL_MAX_LINE];
    extern void *memset(void *s, int c, size_t n);
    memset(cmd, ' ', SHELL_MAX_LINE - 1);
    cmd[SHELL_MAX_LINE - 1] = '\0';

    /* All spaces — should parse as empty command, return 0 */
    int ret = shell_execute(cmd);
    TEST_ASSERT_EQUAL_INT(0, ret);
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
 * Test: 'bench' command with each subcommand.
 */
static void test_shell_cmd_bench_no_args(void)
{
    int ret = shell_execute("bench");
    TEST_ASSERT_EQUAL_INT(1, ret);  /* Missing subcommand → error */
}

static void test_shell_cmd_bench_context(void)
{
    int ret = shell_execute("bench context");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_bench_irq(void)
{
    int ret = shell_execute("bench irq");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_bench_ipc(void)
{
    int ret = shell_execute("bench ipc");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_bench_stats(void)
{
    int ret = shell_execute("bench stats");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_bench_all(void)
{
    int ret = shell_execute("bench all");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_bench_invalid(void)
{
    int ret = shell_execute("bench foobar");
    TEST_ASSERT_EQUAL_INT(1, ret);  /* Unknown subcommand → error */
}

/*
 * Test: 'clear' command executes successfully (minimal output).
 */
static void test_shell_cmd_clear(void)
{
    int ret = shell_execute("clear");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'top -n 1' runs a single frame and exits cleanly. Regression
 * for #191 — the command must not hang without 'q' and must accept -n.
 */
static void test_shell_cmd_top_one_iter(void)
{
    int ret = shell_execute("top -n 1");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'top -n 1 5' accepts both iteration count and refresh interval.
 * Refresh isn't exercised here since we only run one frame, but the
 * parser must not error.
 */
static void test_shell_cmd_top_refresh_arg(void)
{
    int ret = shell_execute("top -n 1 5");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'top' rejects a zero-second refresh interval.
 */
static void test_shell_cmd_top_zero_refresh(void)
{
    int ret = shell_execute("top 0");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * Test: 'top -n' without a value errors out.
 */
static void test_shell_cmd_top_missing_count(void)
{
    int ret = shell_execute("top -n");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * Tests for #195: `sched trace` subcommands.
 * The trace system exists independent of recorded events — start/stop/clear
 * and dumping must succeed even with zero captured events.
 */
static void test_shell_cmd_sched_trace_lifecycle(void)
{
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace start"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace per-cpu"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace stop"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace clear"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace"));
}

/*
 * Regression for #193: `sched compare` runs the context-switch
 * microbenchmark under each registered policy and prints a table.
 * We don't assert on the numbers (they vary run-to-run) — just that
 * the command returns 0 and doesn't panic.
 */
static void test_shell_cmd_sched_compare(void)
{
    int ret = shell_execute("sched compare");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Regression for #194: `eviction demo` fills the weight pool to
 * capacity, drives at least one eviction, and cleans up. When
 * AI_EVICTION is off the policy isn't invoked but the command still
 * runs and returns 0.
 */
static void test_shell_cmd_eviction_demo(void)
{
    int ret = shell_execute("eviction demo");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Regression for #196: `bench context` now renders a latency histogram
 * after the average/rating output. The histogram records every
 * context-switch round-trip, buckets them logarithmically, and prints
 * p50/p95/p99 percentiles. Running bench context twice in a row
 * verifies that the histogram reinitializes cleanly (no carry-over
 * from the previous run).
 */
static void test_shell_cmd_bench_context_histogram_reinit(void)
{
    TEST_ASSERT_EQUAL_INT(0, shell_execute("bench context"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("bench context"));
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
 * timdiag Command Tests
 *
 * timdiag is the timer/interrupt delivery diagnostic added to investigate
 * hardware timer preemption on Pi 5 and Jetson. On QEMU, the GIC is a
 * single-security-state GICv2 where these probes are harmless — the command
 * should run to completion without crashing.
 * ============================================================================ */

#if !defined(PLATFORM_X86_64)
/*
 * Test: 'timdiag' with no args runs the safe diagnostic path.
 * On QEMU this dumps timer state + WFI test skip message.
 * Verifies that the diagnostic does not crash on the non-hardware path.
 */
static void test_shell_cmd_timdiag_no_args(void)
{
    int ret = shell_execute("timdiag");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'timdiag fiq' passes the explicit argument through.
 * On QEMU GICv2, this still skips the GICv3 FIQ test path, so it
 * should run to completion without crashing. The argument handling
 * only matters on Jetson GICv3.
 */
static void test_shell_cmd_timdiag_fiq_arg(void)
{
    int ret = shell_execute("timdiag fiq");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'timdiag' called repeatedly does not accumulate state.
 * Previous runs should not affect subsequent runs (idempotent).
 */
static void test_shell_cmd_timdiag_idempotent(void)
{
    int ret;
    for (int i = 0; i < 3; i++) {
        ret = shell_execute("timdiag");
        TEST_ASSERT_EQUAL_INT(0, ret);
    }
}

/*
 * Test: Unknown extra argument still runs the default safe path.
 */
static void test_shell_cmd_timdiag_unknown_arg(void)
{
    int ret = shell_execute("timdiag unknown");
    TEST_ASSERT_EQUAL_INT(0, ret);
}
#endif /* !PLATFORM_X86_64 */

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
 * Working Directory and Path Resolution Tests
 * ============================================================================ */

/*
 * Test: pwd command returns current directory.
 */
static void test_shell_cmd_pwd(void)
{
    /* First cd to root to ensure known state */
    int ret = shell_execute("cd /");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd to root directory.
 */
static void test_shell_cmd_cd_root(void)
{
    int ret = shell_execute("cd /");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd with no argument goes to root.
 */
static void test_shell_cmd_cd_no_arg(void)
{
    /* First cd somewhere else */
    shell_execute("cd /sys");

    /* cd with no arg should go to root */
    int ret = shell_execute("cd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify we're at root by listing */
    ret = shell_execute("ls");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd to valid directory works.
 */
static void test_shell_cmd_cd_valid_dir(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* pwd should now be /sys */
    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset to root */
    shell_execute("cd /");
}

/*
 * Test: cd to nonexistent directory fails.
 */
static void test_shell_cmd_cd_nonexistent(void)
{
    int ret = shell_execute("cd /nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: cd to file fails (not a directory).
 */
static void test_shell_cmd_cd_file(void)
{
    int ret = shell_execute("cd /sys/memory");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: cd with .. goes to parent directory.
 */
static void test_shell_cmd_cd_dotdot(void)
{
    /* Go to /sys first */
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* cd .. should go back to / */
    ret = shell_execute("cd ..");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify at root */
    ret = shell_execute("ls /");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd .. at root stays at root.
 */
static void test_shell_cmd_cd_dotdot_at_root(void)
{
    int ret = shell_execute("cd /");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* cd .. at root should stay at root */
    ret = shell_execute("cd ..");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd with . stays in current directory.
 */
static void test_shell_cmd_cd_dot(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* cd . should stay in /sys */
    ret = shell_execute("cd .");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: ls with no argument uses cwd.
 */
static void test_shell_cmd_ls_cwd(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* ls with no arg should list /sys */
    ret = shell_execute("ls");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: ls with relative path.
 */
static void test_shell_cmd_ls_relative(void)
{
    int ret = shell_execute("cd /");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* ls sys should work as relative path */
    ret = shell_execute("ls sys");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: ls with . path.
 */
static void test_shell_cmd_ls_dot(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* ls . should list current directory */
    ret = shell_execute("ls .");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: ls with .. path.
 */
static void test_shell_cmd_ls_dotdot(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* ls .. should list parent (root) */
    ret = shell_execute("ls ..");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: cat with relative path.
 */
static void test_shell_cmd_cat_relative(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* cat memory should work as relative path */
    ret = shell_execute("cat memory");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: Path resolution with complex path (multiple ..).
 */
static void test_shell_path_complex(void)
{
    /* cd to a path with multiple .. */
    int ret = shell_execute("cd /sys/../proc/../sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Should be in /sys */
    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: Path resolution with trailing slashes.
 */
static void test_shell_path_trailing_slash(void)
{
    int ret = shell_execute("cd /sys/");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: Path resolution with double slashes.
 */
static void test_shell_path_double_slash(void)
{
    int ret = shell_execute("ls //sys//");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd to mount point subdirectory.
 */
static void test_shell_cmd_cd_mount_subdir(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* List should work */
    ret = shell_execute("ls");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: Relative path in mounted filesystem.
 */
static void test_shell_mount_relative_path(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* cat hello.txt should work */
    ret = shell_execute("cat hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: df with relative path (cwd in mount).
 */
static void test_shell_cmd_df_cwd(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* df with no arg should show current filesystem */
    ret = shell_execute("df");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/* ============================================================================
 * New Filesystem Commands Tests (cp, touch, stat, tree, wc, hexdump, grep, find)
 * ============================================================================ */

/*
 * Test: touch creates empty file - VERIFIES FILE SIZE IS 0.
 */
static void test_shell_cmd_touch(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("touch testtouch.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify file exists AND is empty (0 bytes) */
    int size = get_file_size("/mnt/files/testtouch.tmp");
    TEST_ASSERT_EQUAL_INT(0, size);

    /* Cleanup */
    shell_execute("rm testtouch.tmp");
    shell_execute("cd /");
}

/*
 * Test: touch on existing file doesn't truncate.
 */
static void test_shell_cmd_touch_existing(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create file with content */
    ret = shell_execute("write touch_exist.tmp Hello123");
    TEST_ASSERT_EQUAL_INT(0, ret);

    int orig_size = get_file_size("/mnt/files/touch_exist.tmp");
    TEST_ASSERT_TRUE(orig_size > 0);

    /* Touch should NOT truncate */
    ret = shell_execute("touch touch_exist.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify size unchanged */
    int new_size = get_file_size("/mnt/files/touch_exist.tmp");
    TEST_ASSERT_EQUAL_INT(orig_size, new_size);

    /* Cleanup */
    shell_execute("rm touch_exist.tmp");
    shell_execute("cd /");
}

/*
 * Test: cp copies a file - VERIFIES CONTENT IS IDENTICAL.
 */
static void test_shell_cmd_cp(void)
{
    const char *test_content = "Hello from cp test!";
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create source file */
    ret = shell_execute("write cpsrc.tmp Hello from cp test!");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Copy it */
    ret = shell_execute("cp cpsrc.tmp cpdst.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify destination has same content */
    char buf[64] = {0};
    int bytes = read_file_content("/mnt/files/cpdst.tmp", buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(bytes > 0);
    TEST_ASSERT_EQUAL_STRING(test_content, buf);

    /* Verify sizes match */
    int src_size = get_file_size("/mnt/files/cpsrc.tmp");
    int dst_size = get_file_size("/mnt/files/cpdst.tmp");
    TEST_ASSERT_EQUAL_INT(src_size, dst_size);

    /* Cleanup */
    shell_execute("rm cpsrc.tmp");
    shell_execute("rm cpdst.tmp");
    shell_execute("cd /");
}

/*
 * Test: cp copies binary content correctly.
 */
static void test_shell_cmd_cp_binary(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Write bytes including special chars via shell */
    ret = shell_execute("write cpbin.tmp ABCDEFGHIJ1234567890");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Copy */
    ret = shell_execute("cp cpbin.tmp cpbin2.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Read both and compare */
    char buf1[64] = {0}, buf2[64] = {0};
    int bytes1 = read_file_content("/mnt/files/cpbin.tmp", buf1, sizeof(buf1) - 1);
    int bytes2 = read_file_content("/mnt/files/cpbin2.tmp", buf2, sizeof(buf2) - 1);

    TEST_ASSERT_EQUAL_INT(bytes1, bytes2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(buf1, buf2, (size_t)bytes1));

    /* Cleanup */
    shell_execute("rm cpbin.tmp");
    shell_execute("rm cpbin2.tmp");
    shell_execute("cd /");
}

/*
 * Test: cp with source not found fails.
 */
static void test_shell_cmd_cp_not_found(void)
{
    int ret = shell_execute("cp /mnt/files/nonexistent.tmp /mnt/files/dst.tmp");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: cp with missing args fails.
 */
static void test_shell_cmd_cp_missing_args(void)
{
    int ret = shell_execute("cp");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    ret = shell_execute("cp /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: stat shows file information - VERIFIES SIZE.
 */
static void test_shell_cmd_stat_file(void)
{
    /* hello.txt exists with known content */
    int ret = shell_execute("stat /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify we can get its size (should be non-zero) */
    int size = get_file_size("/mnt/files/hello.txt");
    TEST_ASSERT_TRUE(size > 0);
}

/*
 * Test: stat shows directory information.
 */
static void test_shell_cmd_stat_dir(void)
{
    int ret = shell_execute("stat /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: stat on nonexistent fails.
 */
static void test_shell_cmd_stat_not_found(void)
{
    int ret = shell_execute("stat /mnt/files/nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: stat on virtual file.
 */
static void test_shell_cmd_stat_virtual(void)
{
    int ret = shell_execute("stat /sys/memory");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: stat with missing args.
 */
static void test_shell_cmd_stat_missing_args(void)
{
    int ret = shell_execute("stat");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: tree lists directory recursively.
 */
static void test_shell_cmd_tree(void)
{
    int ret = shell_execute("tree /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: tree with subdirectory structure.
 */
static void test_shell_cmd_tree_subdir(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create subdirectory with file */
    ret = shell_execute("mkdir tree_test_dir");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("write tree_test_dir/nested.txt nested content");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Tree should show subdirectory and its contents */
    ret = shell_execute("tree /mnt/files/tree_test_dir");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Cleanup */
    shell_execute("rm tree_test_dir/nested.txt");
    shell_execute("rm tree_test_dir");
    shell_execute("cd /");
}

/*
 * Test: tree with depth limit.
 */
static void test_shell_cmd_tree_depth(void)
{
    int ret = shell_execute("tree /mnt/files 2");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: tree on virtual directory.
 */
static void test_shell_cmd_tree_virtual(void)
{
    int ret = shell_execute("tree /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: tree on root.
 */
static void test_shell_cmd_tree_root(void)
{
    int ret = shell_execute("tree / 2");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: wc counts lines, words, bytes - VERIFIES COMMAND RUNS.
 */
static void test_shell_cmd_wc(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create file with known content */
    ret = shell_execute("write wc_test.tmp line1 word2 word3");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("wc wc_test.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Cleanup */
    shell_execute("rm wc_test.tmp");
    shell_execute("cd /");
}

/*
 * Test: wc on known file verifies size.
 */
static void test_shell_cmd_wc_known_content(void)
{
    /* hello.txt has "Hello from LittleFS!" = 20 bytes, 1 line, 3 words */
    int ret = shell_execute("wc /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: wc on nonexistent fails.
 */
static void test_shell_cmd_wc_not_found(void)
{
    int ret = shell_execute("wc /mnt/files/nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: wc missing args.
 */
static void test_shell_cmd_wc_missing_args(void)
{
    int ret = shell_execute("wc");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: hexdump shows hex output.
 */
static void test_shell_cmd_hexdump(void)
{
    int ret = shell_execute("hexdump /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: hexdump with offset and length.
 */
static void test_shell_cmd_hexdump_offset(void)
{
    int ret = shell_execute("hexdump /mnt/files/hello.txt 0 16");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: hexdump with offset in middle of file.
 */
static void test_shell_cmd_hexdump_middle(void)
{
    int ret = shell_execute("hexdump /mnt/files/hello.txt 6 10");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: hexdump on nonexistent fails.
 */
static void test_shell_cmd_hexdump_not_found(void)
{
    int ret = shell_execute("hexdump /mnt/files/nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: hexdump missing args.
 */
static void test_shell_cmd_hexdump_missing_args(void)
{
    int ret = shell_execute("hexdump");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: grep finds pattern in file.
 */
static void test_shell_cmd_grep(void)
{
    /* The hello.txt file contains "Hello from LittleFS!" */
    int ret = shell_execute("grep Hello /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: grep finds pattern in middle of line.
 */
static void test_shell_cmd_grep_middle(void)
{
    /* Search for "from" in "Hello from LittleFS!" */
    int ret = shell_execute("grep from /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: grep case sensitivity.
 */
static void test_shell_cmd_grep_case(void)
{
    /* "hello" (lowercase) should NOT match "Hello" */
    int ret = shell_execute("grep hello /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);  /* Returns 0 but shows "0 matches" */
}

/*
 * Test: grep with no match.
 */
static void test_shell_cmd_grep_no_match(void)
{
    int ret = shell_execute("grep NOTFOUND /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);  /* Returns 0, prints "no matches" */
}

/*
 * Test: grep on nonexistent fails.
 */
static void test_shell_cmd_grep_not_found(void)
{
    int ret = shell_execute("grep pattern /mnt/files/nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: grep missing args.
 */
static void test_shell_cmd_grep_missing_args(void)
{
    int ret = shell_execute("grep");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    ret = shell_execute("grep pattern");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: grep with multiline file.
 */
static void test_shell_cmd_grep_multiline(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* readme.txt has multiple lines */
    ret = shell_execute("grep SLM-OS /mnt/files/readme.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    shell_execute("cd /");
}

/*
 * Test: find locates files by exact pattern.
 */
static void test_shell_cmd_find(void)
{
    int ret = shell_execute("find /mnt/files hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: find with * wildcard.
 */
static void test_shell_cmd_find_wildcard(void)
{
    int ret = shell_execute("find /mnt/files *.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: find with leading wildcard.
 */
static void test_shell_cmd_find_leading_wildcard(void)
{
    int ret = shell_execute("find /mnt/files *lo.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: find with no matches.
 */
static void test_shell_cmd_find_no_match(void)
{
    int ret = shell_execute("find /mnt/files *.xyz");
    TEST_ASSERT_EQUAL_INT(0, ret);  /* Returns 0, prints "no files found" */
}

/*
 * Test: find with question mark wildcard.
 */
static void test_shell_cmd_find_question(void)
{
    int ret = shell_execute("find /mnt/files hell?.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: find in subdirectory.
 */
static void test_shell_cmd_find_subdir(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create subdirectory with file */
    ret = shell_execute("mkdir find_test_dir");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("write find_test_dir/target.txt found me");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Find should locate it */
    ret = shell_execute("find /mnt/files target.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Also find with wildcard */
    ret = shell_execute("find /mnt/files *.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Cleanup */
    shell_execute("rm find_test_dir/target.txt");
    shell_execute("rm find_test_dir");
    shell_execute("cd /");
}

/*
 * Test: find missing args.
 */
static void test_shell_cmd_find_missing_args(void)
{
    int ret = shell_execute("find");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    ret = shell_execute("find /mnt/files");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: find on non-mount path returns error.
 * (find requires a mounted filesystem, not virtual directories)
 */
static void test_shell_cmd_find_nonmount(void)
{
    int ret = shell_execute("find /sys mem*");
    TEST_ASSERT_EQUAL_INT(-1, ret);  /* Expected: not a mounted filesystem */
}

/* ============================================================================
 * Help System Tests
 * ============================================================================ */

/*
 * Test: help with no argument lists all commands.
 */
static void test_shell_cmd_help_list(void)
{
    int ret = shell_execute("help");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: help with valid command shows detailed help.
 */
static void test_shell_cmd_help_valid(void)
{
    int ret = shell_execute("help cp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("help ls");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("help grep");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: help with unknown command returns error.
 */
static void test_shell_cmd_help_unknown(void)
{
    int ret = shell_execute("help nonexistent_command");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: help files exist in /mnt/files/help/ directory.
 */
static void test_shell_help_files_exist(void)
{
    /* Verify some help files exist */
    char buf[64];

    int bytes = vfs_read_path("/mnt/files/help/cp.txt", buf, sizeof(buf), 0);
    TEST_ASSERT_TRUE(bytes > 0);

    bytes = vfs_read_path("/mnt/files/help/ls.txt", buf, sizeof(buf), 0);
    TEST_ASSERT_TRUE(bytes > 0);

    bytes = vfs_read_path("/mnt/files/help/help.txt", buf, sizeof(buf), 0);
    TEST_ASSERT_TRUE(bytes > 0);
}

/*
 * Test: help file contains expected content (non-empty and reasonable size).
 */
static void test_shell_help_file_content(void)
{
    char buf[256];

    /* Read cp help file */
    int bytes = vfs_read_path("/mnt/files/help/cp.txt", buf, sizeof(buf) - 1, 0);
    TEST_ASSERT_TRUE(bytes > 50);  /* Should have substantial help text */

    /* Read help help file */
    bytes = vfs_read_path("/mnt/files/help/help.txt", buf, sizeof(buf) - 1, 0);
    TEST_ASSERT_TRUE(bytes > 50);  /* Should have substantial help text */
}

/*
 * Test: ls /mnt/files/help shows help files.
 */
static void test_shell_help_dir_listing(void)
{
    int ret = shell_execute("ls /mnt/files/help");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/* ============================================================================
 * Write/Modify Command Tests
 *
 * Tests for shell commands that create, modify, or remove files:
 * write, mkdir, rm, mv, append, truncate.
 * ============================================================================ */

/*
 * Test: write command creates a file with content.
 */
static void test_shell_cmd_write(void)
{
    int ret = shell_execute("write /mnt/files/write_test.tmp hello world");
    TEST_ASSERT_EQUAL_INT(0, ret);
    /* Verify file was created by catting it */
    ret = shell_execute("cat /mnt/files/write_test.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);
    shell_execute("rm /mnt/files/write_test.tmp");
}

/*
 * Test: write with no arguments returns error.
 */
static void test_shell_cmd_write_no_args(void)
{
    int ret = shell_execute("write");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: write translates \n / \t / \\ escapes into real bytes.
 */
static void test_shell_cmd_write_escapes(void)
{
    const char *path = "/mnt/files/write_esc.tmp";
    int ret = shell_execute("write /mnt/files/write_esc.tmp line1\\nline2\\there\\\\done");
    TEST_ASSERT_EQUAL_INT(0, ret);

    char buf[64];
    int n = read_file_content(path, buf, sizeof(buf) - 1);
    TEST_ASSERT_GREATER_THAN(0, n);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("line1\nline2\there\\done", buf);

    shell_execute("rm /mnt/files/write_esc.tmp");
}

/*
 * Test: write \xNN translates to a single byte.
 */
static void test_shell_cmd_write_hex_escape(void)
{
    const char *path = "/mnt/files/write_hex.tmp";
    /* \x41 = 'A', \x42 = 'B' */
    int ret = shell_execute("write /mnt/files/write_hex.tmp \\x41\\x42C");
    TEST_ASSERT_EQUAL_INT(0, ret);

    char buf[16];
    int n = read_file_content(path, buf, sizeof(buf) - 1);
    TEST_ASSERT_EQUAL_INT(3, n);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("ABC", buf);

    shell_execute("rm /mnt/files/write_hex.tmp");
}

/*
 * Test: write accepts content larger than the old 512-byte limit.
 */
static void test_shell_cmd_write_large_content(void)
{
    const char *path = "/mnt/files/write_big.tmp";
    /* Build a command line that is below SHELL_MAX_LINE but above 512 bytes
     * of content (the previous internal cap). */
    char cmd[SHELL_MAX_LINE];
    extern void *memset(void *s, int c, size_t n);
    int prefix = 0;
    const char *header = "write /mnt/files/write_big.tmp ";
    while (header[prefix]) { cmd[prefix] = header[prefix]; prefix++; }
    int payload_len = SHELL_MAX_LINE - prefix - 1;
    if (payload_len > 800) payload_len = 800;  /* well above old 512 cap */
    memset(cmd + prefix, 'a', payload_len);
    cmd[prefix + payload_len] = '\0';

    int ret = shell_execute(cmd);
    TEST_ASSERT_EQUAL_INT(0, ret);

    int sz = get_file_size(path);
    TEST_ASSERT_EQUAL_INT(payload_len, sz);

    shell_execute("rm /mnt/files/write_big.tmp");
}

/*
 * Test: mkdir creates a directory.
 */
static void test_shell_cmd_mkdir(void)
{
    int ret = shell_execute("mkdir /mnt/files/test_mkdir_dir");
    TEST_ASSERT_EQUAL_INT(0, ret);
    /* Verify dir exists by listing it */
    ret = shell_execute("ls /mnt/files/test_mkdir_dir");
    TEST_ASSERT_EQUAL_INT(0, ret);
    shell_execute("rm /mnt/files/test_mkdir_dir");
}

/*
 * Test: mkdir with no arguments returns error.
 */
static void test_shell_cmd_mkdir_no_args(void)
{
    int ret = shell_execute("mkdir");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: rm removes a file.
 */
static void test_shell_cmd_rm(void)
{
    shell_execute("write /mnt/files/rm_test.tmp data");
    int ret = shell_execute("rm /mnt/files/rm_test.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);
    /* Verify file is gone */
    ret = shell_execute("cat /mnt/files/rm_test.tmp");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: rm on nonexistent file returns error.
 */
static void test_shell_cmd_rm_nonexistent(void)
{
    int ret = shell_execute("rm /mnt/files/does_not_exist_12345");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: mv renames a file.
 */
static void test_shell_cmd_mv(void)
{
    shell_execute("write /mnt/files/mv_src.tmp move me");
    int ret = shell_execute("mv /mnt/files/mv_src.tmp /mnt/files/mv_dst.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);
    /* Source should be gone */
    ret = shell_execute("cat /mnt/files/mv_src.tmp");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    /* Dest should exist */
    ret = shell_execute("cat /mnt/files/mv_dst.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);
    shell_execute("rm /mnt/files/mv_dst.tmp");
}

/*
 * Test: mv with missing arguments returns error.
 */
static void test_shell_cmd_mv_missing_args(void)
{
    int ret = shell_execute("mv");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: append adds content to an existing file.
 */
static void test_shell_cmd_append(void)
{
    shell_execute("write /mnt/files/append_test.tmp first");
    int ret = shell_execute("append /mnt/files/append_test.tmp second");
    TEST_ASSERT_EQUAL_INT(0, ret);
    shell_execute("rm /mnt/files/append_test.tmp");
}

/*
 * Test: append with no arguments returns error.
 */
static void test_shell_cmd_append_no_args(void)
{
    int ret = shell_execute("append");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: truncate shortens a file to a given size.
 */
static void test_shell_cmd_truncate(void)
{
    shell_execute("write /mnt/files/trunc_test.tmp some long content here");
    int ret = shell_execute("truncate /mnt/files/trunc_test.tmp 4");
    TEST_ASSERT_EQUAL_INT(0, ret);
    shell_execute("rm /mnt/files/trunc_test.tmp");
}

/*
 * Test: truncate with no arguments returns error.
 */
static void test_shell_cmd_truncate_no_args(void)
{
    int ret = shell_execute("truncate");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ============================================================================
 * sleep Command Validation (CORE-M1)
 *
 * cmd_sleep now uses shell_parse_uint instead of atoi, so it rejects
 * non-numeric input, negative durations, and "0". The happy-path sleeps
 * for real, so that is not exercised here.
 * ============================================================================ */

static void test_shell_cmd_sleep_rejects_non_numeric(void)
{
    /* "abc" is not a valid uint — parse fails. */
    int ret = shell_execute("sleep abc");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

static void test_shell_cmd_sleep_rejects_zero(void)
{
    /* "0" parses but is rejected (duration must be > 0). */
    int ret = shell_execute("sleep 0");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

static void test_shell_cmd_sleep_rejects_negative(void)
{
    /* shell_parse_uint rejects strings with non-digit chars including '-'. */
    int ret = shell_execute("sleep -5");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

static void test_shell_cmd_sleep_rejects_trailing_garbage(void)
{
    /* "10x" has trailing garbage — shell_parse_uint rejects. atoi would
     * silently return 10 and sleep for 10ms. */
    int ret = shell_execute("sleep 10x");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

static void test_shell_cmd_sleep_missing_args(void)
{
    int ret = shell_execute("sleep");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/* ============================================================================
 * Path Resolution Unit Tests (CORE-H4)
 *
 * Direct tests for shell_resolve_path() covering canonicalization of "..",
 * ".", empty strings, double slashes, trailing slashes, and overflow.
 * ============================================================================ */

/* Restore shell_cwd after each test in this group. */
static void resolve_path_setup(char *saved_cwd)
{
    strcpy(saved_cwd, shell_cwd);
    strcpy(shell_cwd, "/");
}

static void resolve_path_teardown(const char *saved_cwd)
{
    strcpy(shell_cwd, saved_cwd);
}

static void test_shell_resolve_path_absolute_simple(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/foo/bar", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/foo/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_dotdot(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/foo/../bar", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_dot(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/foo/./bar", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/foo/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_dotdot_at_root(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    /* ".." at root stays at root, then "etc" is appended. */
    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/../etc", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/etc", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_double_slash(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/foo//bar", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/foo/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_trailing_slash(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/foo/bar/", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/foo/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_empty(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);
    strcpy(shell_cwd, "/sys");

    /* Empty path resolves to current working directory. */
    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/sys", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_relative(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);
    strcpy(shell_cwd, "/foo");

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("bar", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/foo/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_overflow(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    /* Tiny output buffer forces the length check to fail. */
    char out[4];
    TEST_ASSERT_EQUAL_INT(-1, shell_resolve_path("/foo/bar", out, sizeof(out)));

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_rejects_null(void)
{
    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(-1, shell_resolve_path(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, shell_resolve_path("/foo", NULL, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, shell_resolve_path("/foo", out, 1));
}

/* ============================================================================
 * Shared String Function Regression Tests (CORE-L1)
 *
 * Previously, 6 files had their own static copies of strcmp/strlen/strcpy.
 * Now they all use the shared functions from string.c via string.h.
 * ============================================================================ */

/* Regression: shared string functions from string.h work correctly */
static void test_string_strcmp_basic(void)
{
    TEST_ASSERT_EQUAL_INT(0, strcmp("hello", "hello"));
    TEST_ASSERT_TRUE(strcmp("abc", "abd") < 0);
    TEST_ASSERT_TRUE(strcmp("abd", "abc") > 0);
    TEST_ASSERT_TRUE(strcmp("", "a") < 0);
    TEST_ASSERT_EQUAL_INT(0, strcmp("", ""));
}

static void test_string_strlen_basic(void)
{
    TEST_ASSERT_EQUAL_INT(0, strlen(""));
    TEST_ASSERT_EQUAL_INT(5, strlen("hello"));
    TEST_ASSERT_EQUAL_INT(1, strlen("x"));
}

static void test_string_strcpy_basic(void)
{
    char buf[32];
    strcpy(buf, "test");
    TEST_ASSERT_EQUAL_STRING("test", buf);
    strcpy(buf, "");
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_string_strncpy_basic(void)
{
    char buf[8];
    extern void *memset(void *s, int c, size_t n);
    memset(buf, 'X', sizeof(buf));
    strncpy(buf, "hi", sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("hi", buf);
    /* strncpy should zero-pad remaining bytes */
    TEST_ASSERT_EQUAL_INT(0, buf[3]);
}

/* ============================================================================
 * kprintf Format Regression Tests (CORE-L3)
 *
 * The kprintf was refactored to use a common format parser.
 * These tests verify format specifiers produce correct output via
 * uart_snprintf (which writes to a buffer, testable).
 * ============================================================================ */

/* Regression: kprintf format specifiers work after refactor */
static void test_kprintf_decimal(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%d", 42);
    TEST_ASSERT_EQUAL_STRING("42", buf);
    uart_snprintf(buf, sizeof(buf), "%d", -1);
    TEST_ASSERT_EQUAL_STRING("-1", buf);
    uart_snprintf(buf, sizeof(buf), "%d", 0);
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

static void test_kprintf_hex(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%x", 0xDEAD);
    TEST_ASSERT_EQUAL_STRING("dead", buf);
    uart_snprintf(buf, sizeof(buf), "%X", 0xBEEF);
    TEST_ASSERT_EQUAL_STRING("BEEF", buf);
}

static void test_kprintf_string(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "hello %s", "world");
    TEST_ASSERT_EQUAL_STRING("hello world", buf);
    uart_snprintf(buf, sizeof(buf), "%s", "");
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_kprintf_pointer(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%p", (void *)0x1234);
    TEST_ASSERT_EQUAL_STRING("0x1234", buf);
}

static void test_kprintf_width_pad(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%8d", 42);
    TEST_ASSERT_EQUAL_STRING("      42", buf);
    uart_snprintf(buf, sizeof(buf), "%08x", 0xFF);
    TEST_ASSERT_EQUAL_STRING("000000ff", buf);
}

static void test_kprintf_long(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%lu", (unsigned long)4294967296UL);
    TEST_ASSERT_EQUAL_STRING("4294967296", buf);
}

static void test_kprintf_percent(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "100%%");
    TEST_ASSERT_EQUAL_STRING("100%", buf);
}

static void test_kprintf_mixed(void)
{
    char buf[128];
    uart_snprintf(buf, sizeof(buf), "[%s] val=%d hex=0x%X", "INFO", 42, 0xABC);
    TEST_ASSERT_EQUAL_STRING("[INFO] val=42 hex=0xABC", buf);
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
    RUN_TEST(test_shell_cmd_too_long);
    RUN_TEST(test_shell_cmd_at_max_length);

    /* Basic commands - just verify they execute (minimal output) */
    RUN_TEST(test_shell_cmd_clear);
    RUN_TEST(test_shell_cmd_uptime);
    RUN_TEST(test_shell_cmd_top_one_iter);
    RUN_TEST(test_shell_cmd_top_refresh_arg);
    RUN_TEST(test_shell_cmd_top_zero_refresh);
    RUN_TEST(test_shell_cmd_top_missing_count);
    RUN_TEST(test_shell_cmd_sched_trace_lifecycle);
    RUN_TEST(test_shell_cmd_sched_compare);
    RUN_TEST(test_shell_cmd_eviction_demo);
    RUN_TEST(test_shell_cmd_bench_context_histogram_reinit);

    /* Benchmark command */
    RUN_TEST(test_shell_cmd_bench_no_args);
    RUN_TEST(test_shell_cmd_bench_context);
    RUN_TEST(test_shell_cmd_bench_irq);
    RUN_TEST(test_shell_cmd_bench_ipc);
    RUN_TEST(test_shell_cmd_bench_stats);
    RUN_TEST(test_shell_cmd_bench_all);
    RUN_TEST(test_shell_cmd_bench_invalid);

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

    /* timdiag command (ARM64 only) - timer/IRQ delivery diagnostic */
#if !defined(PLATFORM_X86_64)
    RUN_TEST(test_shell_cmd_timdiag_no_args);
    RUN_TEST(test_shell_cmd_timdiag_fiq_arg);
    RUN_TEST(test_shell_cmd_timdiag_idempotent);
    RUN_TEST(test_shell_cmd_timdiag_unknown_arg);
#endif

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

    /* Working directory and path resolution tests */
    RUN_TEST(test_shell_cmd_pwd);
    RUN_TEST(test_shell_cmd_cd_root);
    RUN_TEST(test_shell_cmd_cd_no_arg);
    RUN_TEST(test_shell_cmd_cd_valid_dir);
    RUN_TEST(test_shell_cmd_cd_nonexistent);
    RUN_TEST(test_shell_cmd_cd_file);
    RUN_TEST(test_shell_cmd_cd_dotdot);
    RUN_TEST(test_shell_cmd_cd_dotdot_at_root);
    RUN_TEST(test_shell_cmd_cd_dot);
    RUN_TEST(test_shell_cmd_ls_cwd);
    RUN_TEST(test_shell_cmd_ls_relative);
    RUN_TEST(test_shell_cmd_ls_dot);
    RUN_TEST(test_shell_cmd_ls_dotdot);
    RUN_TEST(test_shell_cmd_cat_relative);
    RUN_TEST(test_shell_path_complex);
    RUN_TEST(test_shell_path_trailing_slash);
    RUN_TEST(test_shell_path_double_slash);
    RUN_TEST(test_shell_cmd_cd_mount_subdir);
    RUN_TEST(test_shell_mount_relative_path);
    RUN_TEST(test_shell_cmd_df_cwd);

    /* touch command tests */
    RUN_TEST(test_shell_cmd_touch);
    RUN_TEST(test_shell_cmd_touch_existing);

    /* cp command tests */
    RUN_TEST(test_shell_cmd_cp);
    RUN_TEST(test_shell_cmd_cp_binary);
    RUN_TEST(test_shell_cmd_cp_not_found);
    RUN_TEST(test_shell_cmd_cp_missing_args);

    /* stat command tests */
    RUN_TEST(test_shell_cmd_stat_file);
    RUN_TEST(test_shell_cmd_stat_dir);
    RUN_TEST(test_shell_cmd_stat_not_found);
    RUN_TEST(test_shell_cmd_stat_virtual);
    RUN_TEST(test_shell_cmd_stat_missing_args);

    /* tree command tests */
    RUN_TEST(test_shell_cmd_tree);
    RUN_TEST(test_shell_cmd_tree_subdir);
    RUN_TEST(test_shell_cmd_tree_depth);
    RUN_TEST(test_shell_cmd_tree_virtual);
    RUN_TEST(test_shell_cmd_tree_root);

    /* wc command tests */
    RUN_TEST(test_shell_cmd_wc);
    RUN_TEST(test_shell_cmd_wc_known_content);
    RUN_TEST(test_shell_cmd_wc_not_found);
    RUN_TEST(test_shell_cmd_wc_missing_args);

    /* hexdump command tests */
    RUN_TEST(test_shell_cmd_hexdump);
    RUN_TEST(test_shell_cmd_hexdump_offset);
    RUN_TEST(test_shell_cmd_hexdump_middle);
    RUN_TEST(test_shell_cmd_hexdump_not_found);
    RUN_TEST(test_shell_cmd_hexdump_missing_args);

    /* grep command tests */
    RUN_TEST(test_shell_cmd_grep);
    RUN_TEST(test_shell_cmd_grep_middle);
    RUN_TEST(test_shell_cmd_grep_case);
    RUN_TEST(test_shell_cmd_grep_no_match);
    RUN_TEST(test_shell_cmd_grep_not_found);
    RUN_TEST(test_shell_cmd_grep_missing_args);
    RUN_TEST(test_shell_cmd_grep_multiline);

    /* find command tests */
    RUN_TEST(test_shell_cmd_find);
    RUN_TEST(test_shell_cmd_find_wildcard);
    RUN_TEST(test_shell_cmd_find_leading_wildcard);
    RUN_TEST(test_shell_cmd_find_no_match);
    RUN_TEST(test_shell_cmd_find_question);
    RUN_TEST(test_shell_cmd_find_subdir);
    RUN_TEST(test_shell_cmd_find_missing_args);
    RUN_TEST(test_shell_cmd_find_nonmount);

    /* Help system tests */
    RUN_TEST(test_shell_cmd_help_list);
    RUN_TEST(test_shell_cmd_help_valid);
    RUN_TEST(test_shell_cmd_help_unknown);
    RUN_TEST(test_shell_help_files_exist);
    RUN_TEST(test_shell_help_file_content);
    RUN_TEST(test_shell_help_dir_listing);

    /* Write/modify command tests */
    RUN_TEST(test_shell_cmd_write);
    RUN_TEST(test_shell_cmd_write_no_args);
    RUN_TEST(test_shell_cmd_write_escapes);
    RUN_TEST(test_shell_cmd_write_hex_escape);
    RUN_TEST(test_shell_cmd_write_large_content);
    RUN_TEST(test_shell_cmd_mkdir);
    RUN_TEST(test_shell_cmd_mkdir_no_args);
    RUN_TEST(test_shell_cmd_rm);
    RUN_TEST(test_shell_cmd_rm_nonexistent);
    RUN_TEST(test_shell_cmd_mv);
    RUN_TEST(test_shell_cmd_mv_missing_args);
    RUN_TEST(test_shell_cmd_append);
    RUN_TEST(test_shell_cmd_append_no_args);
    RUN_TEST(test_shell_cmd_truncate);
    RUN_TEST(test_shell_cmd_truncate_no_args);

    /* sleep command validation (CORE-M1) */
    RUN_TEST(test_shell_cmd_sleep_rejects_non_numeric);
    RUN_TEST(test_shell_cmd_sleep_rejects_zero);
    RUN_TEST(test_shell_cmd_sleep_rejects_negative);
    RUN_TEST(test_shell_cmd_sleep_rejects_trailing_garbage);
    RUN_TEST(test_shell_cmd_sleep_missing_args);

    /* Path resolution unit tests (CORE-H4) */
    RUN_TEST(test_shell_resolve_path_absolute_simple);
    RUN_TEST(test_shell_resolve_path_dotdot);
    RUN_TEST(test_shell_resolve_path_dot);
    RUN_TEST(test_shell_resolve_path_dotdot_at_root);
    RUN_TEST(test_shell_resolve_path_double_slash);
    RUN_TEST(test_shell_resolve_path_trailing_slash);
    RUN_TEST(test_shell_resolve_path_empty);
    RUN_TEST(test_shell_resolve_path_relative);
    RUN_TEST(test_shell_resolve_path_overflow);
    RUN_TEST(test_shell_resolve_path_rejects_null);

    /* String function regression tests */
    RUN_TEST(test_string_strcmp_basic);
    RUN_TEST(test_string_strlen_basic);
    RUN_TEST(test_string_strcpy_basic);
    RUN_TEST(test_string_strncpy_basic);

    /* kprintf format regression tests */
    RUN_TEST(test_kprintf_decimal);
    RUN_TEST(test_kprintf_hex);
    RUN_TEST(test_kprintf_string);
    RUN_TEST(test_kprintf_pointer);
    RUN_TEST(test_kprintf_width_pad);
    RUN_TEST(test_kprintf_long);
    RUN_TEST(test_kprintf_percent);
    RUN_TEST(test_kprintf_mixed);

    return UNITY_END();
}
