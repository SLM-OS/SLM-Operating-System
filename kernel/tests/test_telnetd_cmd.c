/*
 * test_telnetd_cmd.c - Regression tests for the `telnetd` shell
 * command dispatcher and the shell_io_tcp enumeration / kick APIs.
 *
 * The live-smoke workflow in QEMU exercises the positive paths (start
 * → nc → sessions → kick). These tests pin down the error paths +
 * empty-state invariants that a real-world run can miss:
 *
 *   - `telnetd` with no args returns -1 (usage)
 *   - unknown subcommand returns -1
 *   - stop/status/sessions on a stopped daemon return 0
 *   - kick with no id, bad id, or no-matching-id returns -1
 *   - tcpsh alias dispatches through the same handler
 *   - shell_io_tcp_foreach visits zero entries when pool is empty
 *   - shell_io_tcp_kick returns false when pool is empty
 *   - shell_io_tcp_active_count starts at 0
 *
 * Tests run before any `telnetd start` is issued, so global state is
 * stable (listener off, pool idle). We never *start* the listener —
 * doing so would leak a listening pcb into subsequent suites.
 */

#include "unity.h"
#include "../include/shell.h"
#include "../include/shell_io_tcp.h"
#include "../include/tcp_shell_server.h"
#include "../include/net.h"          /* net_shell_init */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * cmd_telnetd subcommand dispatch
 * ============================================================================ */

static void test_telnetd_no_args_returns_error(void)
{
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("telnetd"));
}

static void test_telnetd_unknown_subcmd_returns_error(void)
{
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("telnetd nonsense"));
}

static void test_telnetd_status_when_stopped(void)
{
    /* When the listener hasn't been started, `status` prints
     * "not running" and returns 0. */
    TEST_ASSERT_FALSE(tcp_shell_server_running());
    TEST_ASSERT_EQUAL_INT(0, shell_execute("telnetd status"));
}

static void test_telnetd_stop_when_stopped_is_noop(void)
{
    TEST_ASSERT_FALSE(tcp_shell_server_running());
    /* stop on an already-stopped daemon is documented as a no-op
     * that returns success. */
    TEST_ASSERT_EQUAL_INT(0, shell_execute("telnetd stop"));
    TEST_ASSERT_FALSE(tcp_shell_server_running());
}

static void test_telnetd_sessions_when_stopped(void)
{
    TEST_ASSERT_FALSE(tcp_shell_server_running());
    TEST_ASSERT_EQUAL_INT(0, shell_execute("telnetd sessions"));
}

static void test_telnetd_kick_no_id_returns_error(void)
{
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("telnetd kick"));
}

static void test_telnetd_kick_invalid_id_returns_error(void)
{
    /* "abc" isn't a valid session id → parse error → -1. */
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("telnetd kick abc"));
}

static void test_telnetd_kick_nonexistent_id_returns_error(void)
{
    /* 999 isn't an active session → kick returns false → handler -1. */
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("telnetd kick 999"));
}

/* ============================================================================
 * tcpsh alias — same handler, different argv[0]
 * ============================================================================ */

static void test_tcpsh_alias_dispatches(void)
{
    /* The handler uses argv[0] for error/status prefix, so this
     * should produce "tcpsh: not running" but return 0 — the alias
     * path must land in cmd_telnetd. */
    TEST_ASSERT_EQUAL_INT(0, shell_execute("tcpsh status"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("tcpsh kick abc"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("tcpsh bogus"));
}

/* ============================================================================
 * shell_io_tcp_foreach / shell_io_tcp_kick on empty pool
 * ============================================================================ */

static uint32_t visit_count;

static bool count_visitor(const struct tcp_session_info *info, void *c)
{
    (void)info;
    (void)c;
    visit_count++;
    return true;
}

static void test_foreach_empty_pool_visits_zero(void)
{
    visit_count = 0;
    shell_io_tcp_foreach(count_visitor, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, visit_count);
}

static void test_foreach_null_visitor_is_noop(void)
{
    /* Must not crash. */
    shell_io_tcp_foreach(NULL, NULL);
}

static void test_kick_empty_pool_returns_false(void)
{
    TEST_ASSERT_FALSE(shell_io_tcp_kick(1));
    TEST_ASSERT_FALSE(shell_io_tcp_kick(0));
    TEST_ASSERT_FALSE(shell_io_tcp_kick(0xFFFFFFFF));
}

static void test_active_count_starts_zero(void)
{
    /* Runs before any TCP session, so the pool is idle. */
    TEST_ASSERT_EQUAL_UINT32(0, shell_io_tcp_active_count());
}

/* ============================================================================
 * Entry
 * ============================================================================ */

int test_suite_telnetd_cmd(void)
{
    /* The test-kernel harness doesn't call shell_init, so net/telnetd
     * commands aren't normally registered at this point. Register them
     * ourselves so shell_execute can reach the cmd_telnetd handler.
     * Idempotent in practice — no other test suite calls this. */
    static bool registered = false;
    if (!registered) {
        net_shell_init();
        registered = true;
    }

    UNITY_BEGIN();

    /* cmd_telnetd subcommand dispatch */
    RUN_TEST(test_telnetd_no_args_returns_error);
    RUN_TEST(test_telnetd_unknown_subcmd_returns_error);
    RUN_TEST(test_telnetd_status_when_stopped);
    RUN_TEST(test_telnetd_stop_when_stopped_is_noop);
    RUN_TEST(test_telnetd_sessions_when_stopped);
    RUN_TEST(test_telnetd_kick_no_id_returns_error);
    RUN_TEST(test_telnetd_kick_invalid_id_returns_error);
    RUN_TEST(test_telnetd_kick_nonexistent_id_returns_error);

    /* tcpsh alias */
    RUN_TEST(test_tcpsh_alias_dispatches);

    /* shell_io_tcp internals on empty pool */
    RUN_TEST(test_foreach_empty_pool_visits_zero);
    RUN_TEST(test_foreach_null_visitor_is_noop);
    RUN_TEST(test_kick_empty_pool_returns_false);
    RUN_TEST(test_active_count_starts_zero);

    return UNITY_END();
}
