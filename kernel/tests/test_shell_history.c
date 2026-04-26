/*
 * test_shell_history.c — Per-session command history (#434)
 *
 * Storage layer:
 *   - Add 1 entry → prev returns it; next returns NULL.
 *   - Fill ring beyond capacity → oldest overwritten; cursor clamps at
 *     the new oldest.
 *   - Empty / whitespace-only inputs are skipped.
 *   - Exact duplicate of the most recent entry is skipped (cursor still
 *     resets to the live edit buffer).
 *   - Inputs longer than SHELL_HISTORY_LINE_MAX - 1 are truncated.
 *   - shell_history_add() resets the cursor to -1.
 *   - Two sessions have fully independent histories.
 *   - shell_history_reset_cursor() forces the cursor back to -1.
 *
 * Line-edit layer (shell_read_command):
 *   - Feed "ESC [ A" → shell_history_prev is invoked; the recalled
 *     entry replaces the visible input region.
 *   - Feed bare "ESC X" → ESC is dropped, 'X' lands in the buffer.
 *
 * Tests construct a stack-local shell_session with only the .history
 * field exercised for the storage layer, and bind a real session +
 * mock shell_io for the parser tests so shell_read_command sees the
 * same byte stream a telnet / UART client would deliver.
 */

#include "unity.h"
#include "../include/shell.h"
#include "../include/shell_session.h"
#include "../include/shell_io.h"
#include "../include/string.h"
#include "../include/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Storage-layer tests — exercise the APIs against a stack-local session.
 * ============================================================================ */

static void test_add_one_then_prev_then_next(void)
{
    struct shell_session sess;
    memset(&sess, 0, sizeof(sess));
    sess.history.cursor = -1;

    shell_history_add(&sess, "ls");

    const char *prev = shell_history_prev(&sess);
    TEST_ASSERT_NOT_NULL(prev);
    TEST_ASSERT_EQUAL_STRING("ls", prev);

    /* Already at oldest — second prev clamps. */
    TEST_ASSERT_NULL(shell_history_prev(&sess));

    /* Next walks back to the live buffer. The sentinel is non-NULL but
     * empty so the caller knows to repaint with no input. */
    const char *next = shell_history_next(&sess);
    TEST_ASSERT_NOT_NULL(next);
    TEST_ASSERT_EQUAL_INT(0, (int)strlen(next));

    /* Already on the live buffer — further next is NULL. */
    TEST_ASSERT_NULL(shell_history_next(&sess));
}

static void test_ring_overwrites_oldest_when_full(void)
{
    struct shell_session sess;
    memset(&sess, 0, sizeof(sess));
    sess.history.cursor = -1;

    char buf[16];
    /* Push DEPTH + 5 distinct entries so the first 5 fall off the back. */
    for (int i = 0; i < SHELL_HISTORY_DEPTH + 5; i++) {
        /* Two-digit decimal keeps strings short and unique. */
        buf[0] = 'c';
        buf[1] = (char)('0' + (i / 10) % 10);
        buf[2] = (char)('0' + i % 10);
        buf[3] = '\0';
        shell_history_add(&sess, buf);
    }

    /* Most recent entry should still be the very last "cNN". */
    int last = SHELL_HISTORY_DEPTH + 5 - 1;
    const char *prev = shell_history_prev(&sess);
    TEST_ASSERT_NOT_NULL(prev);
    char expect[8];
    expect[0] = 'c';
    expect[1] = (char)('0' + (last / 10) % 10);
    expect[2] = (char)('0' + last % 10);
    expect[3] = '\0';
    TEST_ASSERT_EQUAL_STRING(expect, prev);

    /* Walk to the oldest stored entry — index (last - DEPTH + 1) which
     * equals the 6th entry pushed (i == 5). */
    for (int i = 1; i < SHELL_HISTORY_DEPTH; i++) {
        const char *p = shell_history_prev(&sess);
        TEST_ASSERT_NOT_NULL(p);
    }
    /* Beyond the oldest must clamp. */
    TEST_ASSERT_NULL(shell_history_prev(&sess));

    /* The current entry (oldest valid) is i == 5 ("c05"). */
    const char *next = shell_history_next(&sess);   /* one step toward newer */
    TEST_ASSERT_NOT_NULL(next);
    /* "c06" — second-oldest valid entry. */
    expect[0] = 'c';
    expect[1] = '0';
    expect[2] = '6';
    expect[3] = '\0';
    TEST_ASSERT_EQUAL_STRING(expect, next);
}

static void test_skip_blank_and_whitespace(void)
{
    struct shell_session sess;
    memset(&sess, 0, sizeof(sess));
    sess.history.cursor = -1;

    shell_history_add(&sess, "");
    shell_history_add(&sess, "   ");
    shell_history_add(&sess, "\t\t \t");
    /* Nothing should be stored. */
    TEST_ASSERT_NULL(shell_history_prev(&sess));
    TEST_ASSERT_EQUAL_UINT8(0, sess.history.count);

    shell_history_add(&sess, "real");
    TEST_ASSERT_EQUAL_STRING("real", shell_history_prev(&sess));
}

static void test_skip_duplicate_of_most_recent(void)
{
    struct shell_session sess;
    memset(&sess, 0, sizeof(sess));
    sess.history.cursor = -1;

    shell_history_add(&sess, "ls");
    shell_history_add(&sess, "ls");
    shell_history_add(&sess, "ls");
    TEST_ASSERT_EQUAL_UINT8(1, sess.history.count);

    /* Different command in between → the next "ls" is allowed. */
    shell_history_add(&sess, "pwd");
    shell_history_add(&sess, "ls");
    TEST_ASSERT_EQUAL_UINT8(3, sess.history.count);

    TEST_ASSERT_EQUAL_STRING("ls",  shell_history_prev(&sess));
    TEST_ASSERT_EQUAL_STRING("pwd", shell_history_prev(&sess));
    TEST_ASSERT_EQUAL_STRING("ls",  shell_history_prev(&sess));
}

static void test_truncate_at_line_max(void)
{
    struct shell_session sess;
    memset(&sess, 0, sizeof(sess));
    sess.history.cursor = -1;

    /* Two times the limit — the storage layer must clip to LINE_MAX-1
     * with a trailing NUL. */
    char input[2 * SHELL_HISTORY_LINE_MAX];
    for (size_t i = 0; i < sizeof(input) - 1; i++) {
        input[i] = 'A' + (char)(i % 26);
    }
    input[sizeof(input) - 1] = '\0';

    shell_history_add(&sess, input);

    const char *stored = shell_history_prev(&sess);
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_EQUAL_INT(SHELL_HISTORY_LINE_MAX - 1, (int)strlen(stored));
    /* Prefix must match the input byte-for-byte. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(stored, input, SHELL_HISTORY_LINE_MAX - 1));
}

static void test_add_resets_cursor_to_live(void)
{
    struct shell_session sess;
    memset(&sess, 0, sizeof(sess));
    sess.history.cursor = -1;

    shell_history_add(&sess, "alpha");
    shell_history_add(&sess, "bravo");
    /* Browse back to "alpha". */
    TEST_ASSERT_EQUAL_STRING("bravo", shell_history_prev(&sess));
    TEST_ASSERT_EQUAL_STRING("alpha", shell_history_prev(&sess));
    TEST_ASSERT_EQUAL_INT8(1, sess.history.cursor);

    /* Adding a new command must drop the cursor back to the live edit
     * buffer so the next up arrow starts from the most recent entry. */
    shell_history_add(&sess, "charlie");
    TEST_ASSERT_EQUAL_INT8(-1, sess.history.cursor);
    TEST_ASSERT_EQUAL_STRING("charlie", shell_history_prev(&sess));
}

static void test_duplicate_of_recent_still_resets_cursor(void)
{
    struct shell_session sess;
    memset(&sess, 0, sizeof(sess));
    sess.history.cursor = -1;

    shell_history_add(&sess, "alpha");
    shell_history_add(&sess, "bravo");
    /* Cursor browsing into history. */
    (void)shell_history_prev(&sess);
    (void)shell_history_prev(&sess);
    TEST_ASSERT_NOT_EQUAL(-1, sess.history.cursor);

    /* Repeating the most recent entry skips storage but still moves
     * the cursor back to the live buffer (otherwise the next up arrow
     * would start mid-history). */
    shell_history_add(&sess, "bravo");
    TEST_ASSERT_EQUAL_INT8(-1, sess.history.cursor);
}

static void test_two_sessions_have_independent_history(void)
{
    struct shell_session a;
    struct shell_session b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.history.cursor = -1;
    b.history.cursor = -1;

    shell_history_add(&a, "alpha-1");
    shell_history_add(&a, "alpha-2");
    shell_history_add(&b, "bravo-only");

    TEST_ASSERT_EQUAL_STRING("alpha-2", shell_history_prev(&a));
    TEST_ASSERT_EQUAL_STRING("alpha-1", shell_history_prev(&a));
    TEST_ASSERT_NULL(shell_history_prev(&a));

    TEST_ASSERT_EQUAL_STRING("bravo-only", shell_history_prev(&b));
    TEST_ASSERT_NULL(shell_history_prev(&b));
}

static void test_reset_cursor_returns_to_live(void)
{
    struct shell_session sess;
    memset(&sess, 0, sizeof(sess));
    sess.history.cursor = -1;

    shell_history_add(&sess, "one");
    shell_history_add(&sess, "two");
    (void)shell_history_prev(&sess);
    TEST_ASSERT_EQUAL_INT8(0, sess.history.cursor);

    shell_history_reset_cursor(&sess);
    TEST_ASSERT_EQUAL_INT8(-1, sess.history.cursor);

    /* Next call to next() must report no movement now that we're back
     * on the live buffer. */
    TEST_ASSERT_NULL(shell_history_next(&sess));
}

static void test_null_session_is_safe(void)
{
    /* No crash, no return value with stale state. */
    shell_history_add(NULL, "anything");
    TEST_ASSERT_NULL(shell_history_prev(NULL));
    TEST_ASSERT_NULL(shell_history_next(NULL));
    shell_history_reset_cursor(NULL);
}

/* ============================================================================
 * Line-edit-layer tests — drive shell_read_command through a mock io.
 *
 * The mock keeps an input queue (bytes to deliver to read_char in order)
 * and a flat output capture (every byte the line-edit loop writes).
 * shell_read_command obtains the io via shell_session_current(), so each
 * test allocates a fresh session, swaps its io for the mock, and binds
 * it to the running task.
 * ============================================================================ */

#define HIST_IO_INPUT_CAP   256
#define HIST_IO_OUTPUT_CAP  1024

struct hist_io_ctx {
    uint8_t  input[HIST_IO_INPUT_CAP];
    size_t   input_len;
    size_t   input_pos;

    char     output[HIST_IO_OUTPUT_CAP];
    size_t   output_len;

    bool     open;
};

static void hist_io_queue(struct hist_io_ctx *c, const uint8_t *bytes, size_t n)
{
    for (size_t i = 0; i < n && c->input_len < HIST_IO_INPUT_CAP; i++) {
        c->input[c->input_len++] = bytes[i];
    }
}

static int hist_io_read_char(struct shell_io *io)
{
    struct hist_io_ctx *c = (struct hist_io_ctx *)io->ctx;
    if (c->input_pos >= c->input_len) {
        c->open = false;
        return -1;     /* EOF — stops shell_read_command's loop */
    }
    return (int)c->input[c->input_pos++];
}

static int hist_io_try_read_char(struct shell_io *io)
{
    struct hist_io_ctx *c = (struct hist_io_ctx *)io->ctx;
    if (c->input_pos >= c->input_len) return -1;
    return (int)c->input[c->input_pos++];
}

static void hist_io_write(struct shell_io *io, const char *buf, size_t len)
{
    struct hist_io_ctx *c = (struct hist_io_ctx *)io->ctx;
    for (size_t i = 0; i < len && c->output_len + 1 < HIST_IO_OUTPUT_CAP; i++) {
        c->output[c->output_len++] = buf[i];
    }
    c->output[c->output_len] = '\0';
}

static void hist_io_flush(struct shell_io *io)  { (void)io; }
static void hist_io_close(struct shell_io *io)  { ((struct hist_io_ctx *)io->ctx)->open = false; }
static bool hist_io_is_open(struct shell_io *io){ return ((struct hist_io_ctx *)io->ctx)->open; }

static void hist_io_init(struct shell_io *io, struct hist_io_ctx *c)
{
    c->input_len  = 0;
    c->input_pos  = 0;
    c->output_len = 0;
    c->output[0]  = '\0';
    c->open       = true;

    io->read_char     = hist_io_read_char;
    io->try_read_char = hist_io_try_read_char;
    io->write         = hist_io_write;
    io->flush         = hist_io_flush;
    io->close         = hist_io_close;
    io->is_open       = hist_io_is_open;
    io->ctx           = c;
}

/* True if `needle` appears anywhere in the captured output. The output
 * is byte-flat (CRLF normalization happens at the io layer for real
 * sessions; the mock just records bytes verbatim) so memcmp is fine. */
static bool output_contains(const struct hist_io_ctx *c, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > c->output_len) return false;
    for (size_t i = 0; i + nlen <= c->output_len; i++) {
        if (memcmp(c->output + i, needle, nlen) == 0) return true;
    }
    return false;
}

static void test_esc_bracket_a_recalls_previous(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    /* Pre-populate the recall ring directly so the test is independent
     * of the parser entirely. */
    shell_history_add(s, "ls -la");

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    /* ESC [ A then CR. shell_read_command must recall "ls -la", echo
     * the recall via the repaint sequence, then exit on Enter with
     * the recalled string in the buffer. */
    static const uint8_t seq[] = { 0x1B, '[', 'A', '\r' };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[32];
    int len = shell_read_command("> ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;
    shell_session_free(s);

    TEST_ASSERT_EQUAL_INT(6, len);
    TEST_ASSERT_EQUAL_STRING("ls -la", buf);

    /* Repaint sequence visible: \r, ESC [ K, then prompt + recall. */
    TEST_ASSERT_TRUE(output_contains(&ctx, "\r\x1b[K"));
    TEST_ASSERT_TRUE(output_contains(&ctx, "> ls -la"));
}

static void test_bare_esc_drops_then_letter_lands_in_buffer(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    /* ESC X then CR. The bare ESC must be discarded and 'X' must end
     * up in the line buffer per spec. */
    static const uint8_t seq[] = { 0x1B, 'X', '\r' };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[16];
    int len = shell_read_command("$ ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;
    shell_session_free(s);

    TEST_ASSERT_EQUAL_INT(1, len);
    TEST_ASSERT_EQUAL_STRING("X", buf);
}

static void test_unknown_csi_lets_trailing_byte_pass_through(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    /* ESC [ C — would be the right-arrow CSI on a normal terminal.
     * Spec keeps the parser tiny and lets the trailing 'C' fall through
     * to the data path so a future implementor (or paranoid manual
     * tester) can see what arrived. */
    static const uint8_t seq[] = { 0x1B, '[', 'C', '\r' };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[16];
    int len = shell_read_command("# ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;
    shell_session_free(s);

    TEST_ASSERT_EQUAL_INT(1, len);
    TEST_ASSERT_EQUAL_STRING("C", buf);
}

static void test_esc_bracket_b_on_live_buffer_is_noop(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    /* ESC [ B with no recall in flight (cursor == -1). The parser
     * consumes the bytes but no repaint should happen, and Enter
     * yields an empty line. */
    static const uint8_t seq[] = { 0x1B, '[', 'B', '\r' };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[16];
    int len = shell_read_command("? ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;
    shell_session_free(s);

    TEST_ASSERT_EQUAL_INT(0, len);
    TEST_ASSERT_EQUAL_STRING("", buf);

    /* Output should contain the prompt + CRLF only (no repaint
     * sequence, no recalled text). */
    TEST_ASSERT_FALSE(output_contains(&ctx, "\r\x1b[K"));
    TEST_ASSERT_TRUE(output_contains(&ctx, "? "));
}

/* ============================================================================
 * shell_read_command regression coverage — the line-edit primitives that
 * already worked in shell_read_line must keep working now that the same
 * loop hosts the ESC parser.
 * ============================================================================ */

static void test_backspace_erases_in_command_loop(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    /* "ab" then BS then 'c' then Enter → "ac". Both 0x08 and 0x7F
     * (DEL) should map to backspace; cover both. */
    static const uint8_t seq[] = { 'a', 'b', 0x08, 'c', 0x7F, 'd', '\r' };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[16];
    int len = shell_read_command("# ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;
    shell_session_free(s);

    /* a, b, BS (b drops), c, DEL (c drops), d → "ad". */
    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_STRING("ad", buf);
    /* The visual erase sequence "\b \b" should appear at least twice
     * (one per backspace). */
    size_t hits = 0;
    for (size_t i = 0; i + 3 <= ctx.output_len; i++) {
        if (memcmp(ctx.output + i, "\b \b", 3) == 0) hits++;
    }
    TEST_ASSERT_TRUE(hits >= 2);
}

static void test_ctrl_c_cancels_and_resets_history_cursor(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    /* Pre-populate two entries and step the cursor into the middle
     * of the ring. Ctrl+C must reset cursor to -1 so the next prompt
     * starts on the live buffer. */
    shell_history_add(s, "first");
    shell_history_add(s, "second");
    s->history.cursor = 1;   /* browsing "first" */

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    /* "abc" then Ctrl+C. */
    static const uint8_t seq[] = { 'a', 'b', 'c', 0x03 };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[16];
    int len = shell_read_command("> ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;

    TEST_ASSERT_EQUAL_INT(0, len);
    TEST_ASSERT_EQUAL_STRING("", buf);
    /* Visible cancel marker. */
    TEST_ASSERT_TRUE(output_contains(&ctx, "^C"));
    /* Cursor reset is the load-bearing assertion — without it, the
     * next up arrow on this session would resume the previous browse. */
    TEST_ASSERT_EQUAL_INT8(-1, s->history.cursor);

    shell_session_free(s);
}

static void test_lf_alone_terminates_line(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    /* Bare LF (no preceding CR) — raw `nc` clients send this. */
    static const uint8_t seq[] = { 'h', 'i', '\n' };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[16];
    int len = shell_read_command("$ ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;
    shell_session_free(s);

    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_STRING("hi", buf);
}

static void test_recall_truncates_to_max_len(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    /* Fill a 40-char entry; the read buffer is only 8 chars (7 chars
     * + NUL). Repaint must clip the recalled text to fit. */
    char big[41];
    for (size_t i = 0; i < sizeof(big) - 1; i++) big[i] = 'a' + (char)(i % 26);
    big[sizeof(big) - 1] = '\0';
    shell_history_add(s, big);

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    /* ESC [ A then \r. The recalled string is 40 chars; with max_len=8
     * the buffer can hold at most 7 chars + NUL. */
    static const uint8_t seq[] = { 0x1B, '[', 'A', '\r' };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[8];
    int len = shell_read_command("> ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;
    shell_session_free(s);

    TEST_ASSERT_EQUAL_INT(7, len);
    /* First 7 chars of `big` followed by NUL. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(buf, big, 7));
    TEST_ASSERT_EQUAL_INT8('\0', buf[7]);
}

static void test_multiple_prev_walks_through_history(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    /* Three entries, walk all the way back via three up arrows then
     * Enter on the oldest. */
    shell_history_add(s, "one");
    shell_history_add(s, "two");
    shell_history_add(s, "three");

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    static const uint8_t seq[] = {
        0x1B, '[', 'A',   /* up — "three" */
        0x1B, '[', 'A',   /* up — "two" */
        0x1B, '[', 'A',   /* up — "one" */
        0x1B, '[', 'A',   /* up — clamps; no movement */
        '\r',
    };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[32];
    int len = shell_read_command("> ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;
    shell_session_free(s);

    TEST_ASSERT_EQUAL_INT(3, len);
    TEST_ASSERT_EQUAL_STRING("one", buf);
}

static void test_recall_then_edit_then_enter_captures_edited_form(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    shell_history_add(s, "ls");

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    /* Up arrow recalls "ls", then 'x' appends → "lsx", then Enter.
     * The captured buffer must reflect the post-edit text, not the
     * stored recall. */
    static const uint8_t seq[] = { 0x1B, '[', 'A', 'x', '\r' };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[16];
    int len = shell_read_command("> ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;

    TEST_ASSERT_EQUAL_INT(3, len);
    TEST_ASSERT_EQUAL_STRING("lsx", buf);

    /* The in-place edit must not have mutated the stored entry.
     * The browse cursor is already parked at the most recent entry
     * after the recall inside the loop; reset it so prev() walks to
     * the (only) stored entry instead of clamping at the oldest. */
    shell_history_reset_cursor(s);
    const char *again = shell_history_prev(s);
    TEST_ASSERT_NOT_NULL(again);
    TEST_ASSERT_EQUAL_STRING("ls", again);

    shell_session_free(s);
}

static void test_command_emits_prompt_before_first_byte(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    static const uint8_t seq[] = { 'q', '\r' };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[8];
    (void)shell_read_command("slmos> ", buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;
    shell_session_free(s);

    /* The first 7 bytes of output must be the prompt. shell_run no
     * longer puts the prompt itself, so a regression here would leave
     * the user with no visible prompt at all. */
    TEST_ASSERT_TRUE(ctx.output_len >= 7);
    TEST_ASSERT_EQUAL_INT(0, memcmp(ctx.output, "slmos> ", 7));
}

static void test_command_with_null_prompt_does_not_crash(void)
{
    struct shell_session *s = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(s);

    struct shell_io      io;
    struct hist_io_ctx   ctx;
    hist_io_init(&io, &ctx);
    s->io = &io;

    static const uint8_t seq[] = { 'a', '\r' };
    hist_io_queue(&ctx, seq, sizeof(seq));

    shell_session_bind(task_current(), s);
    char buf[8];
    int len = shell_read_command(NULL, buf, sizeof(buf));
    shell_session_unbind(task_current());
    s->io = NULL;
    shell_session_free(s);

    TEST_ASSERT_EQUAL_INT(1, len);
    TEST_ASSERT_EQUAL_STRING("a", buf);
}

/* ============================================================================
 * Entry point
 * ============================================================================ */

int test_suite_shell_history(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_add_one_then_prev_then_next);
    RUN_TEST(test_ring_overwrites_oldest_when_full);
    RUN_TEST(test_skip_blank_and_whitespace);
    RUN_TEST(test_skip_duplicate_of_most_recent);
    RUN_TEST(test_truncate_at_line_max);
    RUN_TEST(test_add_resets_cursor_to_live);
    RUN_TEST(test_duplicate_of_recent_still_resets_cursor);
    RUN_TEST(test_two_sessions_have_independent_history);
    RUN_TEST(test_reset_cursor_returns_to_live);
    RUN_TEST(test_null_session_is_safe);

    RUN_TEST(test_esc_bracket_a_recalls_previous);
    RUN_TEST(test_bare_esc_drops_then_letter_lands_in_buffer);
    RUN_TEST(test_unknown_csi_lets_trailing_byte_pass_through);
    RUN_TEST(test_esc_bracket_b_on_live_buffer_is_noop);

    RUN_TEST(test_backspace_erases_in_command_loop);
    RUN_TEST(test_ctrl_c_cancels_and_resets_history_cursor);
    RUN_TEST(test_lf_alone_terminates_line);
    RUN_TEST(test_recall_truncates_to_max_len);
    RUN_TEST(test_multiple_prev_walks_through_history);
    RUN_TEST(test_recall_then_edit_then_enter_captures_edited_form);
    RUN_TEST(test_command_emits_prompt_before_first_byte);
    RUN_TEST(test_command_with_null_prompt_does_not_crash);

    return UNITY_END();
}
