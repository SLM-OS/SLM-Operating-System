/*
 * test_shell_session.c - Unit tests for shell_session + shell_io + per-task lookup
 *
 * Covers:
 *   - Console session singleton (always reachable, sensible defaults)
 *   - TCP pool alloc / free / exhaustion
 *   - Per-task binding and shell_session_current() resolution
 *   - Fallback to console when no binding exists
 *   - Idempotence of free (including safe no-op on the console)
 *   - shell_io helpers (shell_io_puts, shell_io_printf, shell_io_vprintf)
 *     via a capturing mock backend
 *   - shell_puts / shell_printf routing through the bound session
 */

#include "unity.h"
#include "../include/shell.h"
#include "../include/shell_session.h"
#include "../include/shell_io.h"
#include "../include/task.h"
#include "../include/string.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Capturing shell_io mock — records everything written so helpers can be
 * asserted byte-for-byte.
 * ============================================================================ */

#define CAPTURE_BUF_SIZE 512

struct capture_ctx {
    char     buf[CAPTURE_BUF_SIZE];
    size_t   len;
    uint32_t flush_count;
    uint32_t close_count;
    bool     open;
    int      next_rx;           /* single-char queue; -1 = empty */
};

static int capture_read_char(struct shell_io *io)
{
    struct capture_ctx *c = (struct capture_ctx *)io->ctx;
    int v = c->next_rx;
    c->next_rx = -1;
    return v;
}

static int capture_try_read_char(struct shell_io *io)
{
    return capture_read_char(io);
}

static void capture_write(struct shell_io *io, const char *buf, size_t len)
{
    struct capture_ctx *c = (struct capture_ctx *)io->ctx;
    for (size_t i = 0; i < len && c->len + 1 < CAPTURE_BUF_SIZE; i++) {
        c->buf[c->len++] = buf[i];
    }
    c->buf[c->len] = '\0';
}

static void capture_flush(struct shell_io *io)
{
    struct capture_ctx *c = (struct capture_ctx *)io->ctx;
    c->flush_count++;
}

static void capture_close(struct shell_io *io)
{
    struct capture_ctx *c = (struct capture_ctx *)io->ctx;
    c->close_count++;
    c->open = false;
}

static bool capture_is_open(struct shell_io *io)
{
    struct capture_ctx *c = (struct capture_ctx *)io->ctx;
    return c->open;
}

static void capture_init(struct shell_io *io, struct capture_ctx *c)
{
    c->len         = 0;
    c->buf[0]      = '\0';
    c->flush_count = 0;
    c->close_count = 0;
    c->open        = true;
    c->next_rx     = -1;

    io->read_char     = capture_read_char;
    io->try_read_char = capture_try_read_char;
    io->write         = capture_write;
    io->flush         = capture_flush;
    io->close         = capture_close;
    io->is_open       = capture_is_open;
    io->ctx           = c;
}

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
 * shell_io helpers (shell_io_puts / shell_io_printf / shell_io_vprintf)
 * ============================================================================ */

static void test_io_puts_writes_string(void)
{
    struct shell_io io;
    struct capture_ctx cap;
    capture_init(&io, &cap);

    shell_io_puts(&io, "hello");
    TEST_ASSERT_EQUAL_STRING("hello", cap.buf);
    TEST_ASSERT_EQUAL_UINT32(5, cap.len);
}

static void test_io_puts_null_string_is_noop(void)
{
    struct shell_io io;
    struct capture_ctx cap;
    capture_init(&io, &cap);

    shell_io_puts(&io, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, cap.len);
}

static void test_io_puts_null_io_is_noop(void)
{
    /* Must not crash. */
    shell_io_puts(NULL, "ignored");
}

static void test_io_printf_formats_correctly(void)
{
    struct shell_io io;
    struct capture_ctx cap;
    capture_init(&io, &cap);

    int n = shell_io_printf(&io, "val=%d hex=0x%X s=%s", 42, 0xCAFE, "abc");
    TEST_ASSERT_EQUAL_STRING("val=42 hex=0xCAFE s=abc", cap.buf);
    TEST_ASSERT_EQUAL_INT((int)cap.len, n);
}

/* Thin trampoline for exercising shell_io_vprintf directly. */
static int wrapped_vprintf(struct shell_io *io, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = shell_io_vprintf(io, fmt, args);
    va_end(args);
    return n;
}

static void test_io_vprintf_formats_correctly(void)
{
    struct shell_io io;
    struct capture_ctx cap;
    capture_init(&io, &cap);

    int n = wrapped_vprintf(&io, "%s:%u", "port", 2323u);
    TEST_ASSERT_EQUAL_STRING("port:2323", cap.buf);
    TEST_ASSERT_EQUAL_INT((int)cap.len, n);
}

static void test_io_printf_null_inputs_are_noop(void)
{
    struct capture_ctx cap = { 0 };
    cap.open = true;

    /* NULL io must not crash and must report zero. */
    TEST_ASSERT_EQUAL_INT(0, shell_io_printf(NULL, "%d", 1));

    struct shell_io io;
    capture_init(&io, &cap);
    TEST_ASSERT_EQUAL_INT(0, shell_io_printf(&io, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, cap.len);
}

static void test_io_printf_truncates_long_output_gracefully(void)
{
    /* shell_io_printf has an internal SHELL_IO_PRINTF_BUF-sized buffer
     * (~1 KB). A format that expands to > that size must not corrupt
     * memory — the helper is documented to clamp. We assert only that
     * *something* made it through and nothing crashed. */
    struct shell_io io;
    struct capture_ctx cap;
    capture_init(&io, &cap);

    /* %s with a 2048-char input blows past the 1024-byte buffer. */
    static char big[2048];
    for (size_t i = 0; i < sizeof(big) - 1; i++) big[i] = 'x';
    big[sizeof(big) - 1] = '\0';

    shell_io_printf(&io, "%s", big);
    TEST_ASSERT_TRUE(cap.len > 0);
    TEST_ASSERT_TRUE(cap.len < CAPTURE_BUF_SIZE);
}

/* ============================================================================
 * Session routing — shell_puts / shell_printf go through the bound session
 * ============================================================================ */

static void test_shell_puts_routes_to_bound_session(void)
{
    struct task *t = task_current();
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    struct shell_io io;
    struct capture_ctx cap;
    capture_init(&io, &cap);
    s->io = &io;

    shell_session_bind(t, s);
    shell_puts("routed\r\n");
    shell_printf("n=%d\n", 7);
    TEST_ASSERT_EQUAL_STRING("routed\r\nn=7\n", cap.buf);

    shell_session_unbind(t);
    s->io = NULL;
    shell_session_free(s);
}

static void test_shell_putc_routes_to_bound_session(void)
{
    struct task *t = task_current();
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    struct shell_io io;
    struct capture_ctx cap;
    capture_init(&io, &cap);
    s->io = &io;

    shell_session_bind(t, s);
    shell_putc('X');
    shell_putc('Y');
    TEST_ASSERT_EQUAL_STRING("XY", cap.buf);

    shell_session_unbind(t);
    s->io = NULL;
    shell_session_free(s);
}

static void test_shell_getc_reads_from_bound_session(void)
{
    struct task *t = task_current();
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    struct shell_io io;
    struct capture_ctx cap;
    capture_init(&io, &cap);
    cap.next_rx = 'Z';
    s->io = &io;

    shell_session_bind(t, s);
    int c = shell_try_getc();
    TEST_ASSERT_EQUAL_INT('Z', c);
    /* Drained — next_rx is now -1 */
    int c2 = shell_try_getc();
    TEST_ASSERT_EQUAL_INT(-1, c2);

    shell_session_unbind(t);
    s->io = NULL;
    shell_session_free(s);
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

    RUN_TEST(test_io_puts_writes_string);
    RUN_TEST(test_io_puts_null_string_is_noop);
    RUN_TEST(test_io_puts_null_io_is_noop);
    RUN_TEST(test_io_printf_formats_correctly);
    RUN_TEST(test_io_vprintf_formats_correctly);
    RUN_TEST(test_io_printf_null_inputs_are_noop);
    RUN_TEST(test_io_printf_truncates_long_output_gracefully);

    RUN_TEST(test_shell_puts_routes_to_bound_session);
    RUN_TEST(test_shell_putc_routes_to_bound_session);
    RUN_TEST(test_shell_getc_reads_from_bound_session);

    return UNITY_END();
}
