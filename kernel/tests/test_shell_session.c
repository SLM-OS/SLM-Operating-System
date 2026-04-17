/*
 * test_shell_session.c - Unit tests for shell_session + pool + lookup
 *
 * Covers:
 *   - Console session singleton (always reachable, sensible defaults)
 *   - TCP pool alloc / free / exhaustion
 *   - Per-task binding and shell_session_current() resolution
 *   - Fallback to console when no binding exists
 *   - Idempotence of free (including safe no-op on the console)
 */

#include "unity.h"
#include "../include/shell.h"
#include "../include/shell_session.h"
#include "../include/shell_io.h"
#include "../include/task.h"
#include "../include/string.h"

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Console session
 * ============================================================================ */

static void test_console_session_is_singleton(void)
{
    struct shell_session *a = shell_session_console();
    struct shell_session *b = shell_session_console();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_PTR(a, b);
}

static void test_console_session_has_defaults(void)
{
    struct shell_session *c = shell_session_console();
    TEST_ASSERT_EQUAL_UINT32(0, c->id);          /* reserved console id */
    TEST_ASSERT_NOT_NULL(c->io);                  /* UART backend wired */
    TEST_ASSERT_TRUE(c->in_use);                  /* always "in use" */
    TEST_ASSERT_NOT_NULL(c->cwd);
    TEST_ASSERT_NOT_EQUAL('\0', c->cwd[0]);       /* non-empty cwd */
}

/* ============================================================================
 * Pool alloc / free
 * ============================================================================ */

static void test_alloc_returns_fresh_session(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->in_use);
    TEST_ASSERT_NOT_EQUAL(0, s->id);              /* id > 0 distinguishes from console */
    TEST_ASSERT_NULL(s->io);                      /* caller wires io */
    TEST_ASSERT_NULL(s->owner_task);
    TEST_ASSERT_EQUAL_STRING("/", s->cwd);        /* reset to root */
    shell_session_free(s);
}

static void test_alloc_returns_distinct_slots(void)
{
    struct shell_session *a = shell_session_alloc();
    struct shell_session *b = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_EQUAL(a, b);
    TEST_ASSERT_NOT_EQUAL(a->id, b->id);
    shell_session_free(a);
    shell_session_free(b);
}

static void test_pool_exhaustion_returns_null(void)
{
    struct shell_session *slots[MAX_TCP_SHELL_SESSIONS];
    for (uint32_t i = 0; i < MAX_TCP_SHELL_SESSIONS; i++) {
        slots[i] = shell_session_alloc();
        TEST_ASSERT_NOT_NULL(slots[i]);
    }
    /* One past capacity must fail. */
    TEST_ASSERT_NULL(shell_session_alloc());
    /* Free and retry — slot should be available again. */
    shell_session_free(slots[0]);
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);
    shell_session_free(s);
    for (uint32_t i = 1; i < MAX_TCP_SHELL_SESSIONS; i++) {
        shell_session_free(slots[i]);
    }
}

static void test_free_console_is_noop(void)
{
    struct shell_session *c = shell_session_console();
    bool was_in_use = c->in_use;
    shell_session_free(c);
    /* Free of the console singleton must not mark it released. */
    TEST_ASSERT_EQUAL(was_in_use, c->in_use);
    TEST_ASSERT_NOT_NULL(c->io);
}

static void test_free_null_is_noop(void)
{
    /* Must not crash. */
    shell_session_free(NULL);
}

/* ============================================================================
 * Per-task binding
 * ============================================================================ */

static void test_current_falls_back_to_console(void)
{
    /* No explicit bind — shell_session_current must resolve to the
     * console session for the (unbound) test task. */
    struct shell_session *cur = shell_session_current();
    TEST_ASSERT_EQUAL_PTR(shell_session_console(), cur);
}

static void test_bind_unbind_round_trip(void)
{
    struct task *t = task_current();
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    shell_session_bind(t, s);
    TEST_ASSERT_EQUAL_PTR(s, shell_session_current());
    TEST_ASSERT_EQUAL_PTR(t, s->owner_task);

    shell_session_unbind(t);
    TEST_ASSERT_EQUAL_PTR(shell_session_console(), shell_session_current());

    shell_session_free(s);
}

static void test_bind_overrides_prior_binding(void)
{
    struct task *t = task_current();
    struct shell_session *first  = shell_session_alloc();
    struct shell_session *second = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);

    shell_session_bind(t, first);
    TEST_ASSERT_EQUAL_PTR(first, shell_session_current());

    shell_session_bind(t, second);
    TEST_ASSERT_EQUAL_PTR(second, shell_session_current());

    shell_session_unbind(t);
    shell_session_free(first);
    shell_session_free(second);
}

static void test_unbind_null_task_is_noop(void)
{
    /* Must not crash. */
    shell_session_unbind(NULL);
}

/* ============================================================================
 * cwd isolation between sessions (regression for §1.2 migration)
 * ============================================================================ */

static void test_cwd_isolated_per_session(void)
{
    struct shell_session *a = shell_session_alloc();
    struct shell_session *b = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    strcpy(a->cwd, "/alpha");
    strcpy(b->cwd, "/beta");

    TEST_ASSERT_EQUAL_STRING("/alpha", a->cwd);
    TEST_ASSERT_EQUAL_STRING("/beta",  b->cwd);

    /* Console unchanged by the above. */
    struct shell_session *c = shell_session_console();
    TEST_ASSERT_NOT_EQUAL(0, c->cwd[0]);   /* still non-empty */

    shell_session_free(a);
    shell_session_free(b);
}

/* ============================================================================
 * Entry point
 * ============================================================================ */

int test_suite_shell_session(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_console_session_is_singleton);
    RUN_TEST(test_console_session_has_defaults);

    RUN_TEST(test_alloc_returns_fresh_session);
    RUN_TEST(test_alloc_returns_distinct_slots);
    RUN_TEST(test_pool_exhaustion_returns_null);
    RUN_TEST(test_free_console_is_noop);
    RUN_TEST(test_free_null_is_noop);

    RUN_TEST(test_current_falls_back_to_console);
    RUN_TEST(test_bind_unbind_round_trip);
    RUN_TEST(test_bind_overrides_prior_binding);
    RUN_TEST(test_unbind_null_task_is_noop);

    RUN_TEST(test_cwd_isolated_per_session);

    return UNITY_END();
}
