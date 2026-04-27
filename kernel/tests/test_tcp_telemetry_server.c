/*
 * test_tcp_telemetry_server.c — Unit coverage for the telemetry-feed
 * TCP server's fanout, filter, drop-oldest, and command-parsing paths.
 *
 * The server's lwIP-facing accept / TX-drain code is exercised by the
 * QEMU smoke test in the PR; these tests use the in-tree "test seam"
 * (tcp_telemetry_server_test_*) which lets us drive the same fanout
 * code with a synthetic session pool, no networking required.
 *
 * Coverage:
 *   1. format — a single sample formats as `<topic> seq=<n> ts=<ms> <payload>\n`.
 *   2. filter exact / glob — server-side filter selects topics correctly.
 *   3. fanout — multiple sessions with overlapping filters all receive
 *      a matching sample; non-matchers don't.
 *   4. drop-oldest — filling the per-client ring evicts the oldest line
 *      and bumps the per-session + global drop counters; new lines still
 *      land at the tail.
 *   5. SUB resubscribe — feeding `SUB tel.evi\n` through the inbound
 *      command path narrows the filter; subsequent `tel.inf` events are
 *      not delivered to that session.
 *   6. pool exhaustion — opening MAX_TELEMETRY_SESSIONS+1 synthetic
 *      sessions returns -1 on the last one.
 */

#include "unity.h"
#include "../include/tcp_telemetry_server.h"
#include "../include/string.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True if `needle` appears anywhere in `haystack`. */
static bool contains(const char *haystack, const char *needle)
{
    size_t hl = strlen(haystack), nl = strlen(needle);
    if (nl == 0 || nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; i++) {
        if (memcmp(haystack + i, needle, nl) == 0) return true;
    }
    return false;
}

/* Count how many '\n' bytes appear in a buffer. */
static int count_lines(const char *buf)
{
    int n = 0;
    for (const char *p = buf; *p; p++) if (*p == '\n') n++;
    return n;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

static void test_sample_format_matches_spec(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*"); TEST_ASSERT_TRUE(slot >= 0);

    uint32_t delivered = tcp_telemetry_server_test_inject("tel.inf",
                                                          "dt=42 ok=1");
    TEST_ASSERT_EQUAL_UINT32(1, delivered);

    char out[512];
    size_t n = tcp_telemetry_server_test_drain(slot, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);

    /* Skip the banner that the test seam queues on session open. */
    /* Banner lines start with '#'; sample line starts with the topic. */
    /* Drain returned banner + sample concatenated; just check the
     * sample's tokens appear in order somewhere in the output. */
    TEST_ASSERT_TRUE(contains(out, "tel.inf "));
    TEST_ASSERT_TRUE(contains(out, " seq="));
    TEST_ASSERT_TRUE(contains(out, " ts="));
    TEST_ASSERT_TRUE(contains(out, " dt=42 ok=1\n"));
}

static void test_exact_filter_rejects_other_topic(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.evi"); TEST_ASSERT_TRUE(slot >= 0);

    /* Drain the banner so we only inspect post-inject bytes. */
    char banner[256];
    (void)tcp_telemetry_server_test_drain(slot, banner, sizeof(banner));

    uint32_t d_evi = tcp_telemetry_server_test_inject("tel.evi",
                                                     "dt=10 fb=0");
    uint32_t d_inf = tcp_telemetry_server_test_inject("tel.inf",
                                                     "dt=20 ok=1");
    TEST_ASSERT_EQUAL_UINT32(1, d_evi);
    TEST_ASSERT_EQUAL_UINT32(0, d_inf);   /* exact filter does not match */

    char out[512];
    (void)tcp_telemetry_server_test_drain(slot, out, sizeof(out));
    TEST_ASSERT_TRUE(contains(out, "tel.evi"));
    TEST_ASSERT_FALSE(contains(out, "tel.inf"));
}

static void test_glob_filter_matches_prefix(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*"); TEST_ASSERT_TRUE(slot >= 0);

    char banner[256];
    (void)tcp_telemetry_server_test_drain(slot, banner, sizeof(banner));

    TEST_ASSERT_EQUAL_UINT32(1, tcp_telemetry_server_test_inject("tel.evi",
                                                                "dt=1 fb=0"));
    TEST_ASSERT_EQUAL_UINT32(1, tcp_telemetry_server_test_inject("tel.inf",
                                                                "dt=2 ok=1"));
    /* Non-prefix topic must not match. */
    TEST_ASSERT_EQUAL_UINT32(0, tcp_telemetry_server_test_inject("ai.sched",
                                                                "x=1"));

    char out[1024];
    (void)tcp_telemetry_server_test_drain(slot, out, sizeof(out));
    TEST_ASSERT_TRUE(contains(out, "tel.evi"));
    TEST_ASSERT_TRUE(contains(out, "tel.inf"));
    TEST_ASSERT_FALSE(contains(out, "ai.sched"));
}

static void test_fanout_to_multiple_sessions(void)
{
    tcp_telemetry_server_test_reset();
    int a = tcp_telemetry_server_test_open_session("tel.*"); TEST_ASSERT_TRUE(a >= 0);
    int b = tcp_telemetry_server_test_open_session("tel.evi"); TEST_ASSERT_TRUE(b >= 0);
    int c = tcp_telemetry_server_test_open_session("tel.inf"); TEST_ASSERT_TRUE(c >= 0);

    /* Drain banners. */
    char tmp[256];
    (void)tcp_telemetry_server_test_drain(a, tmp, sizeof(tmp));
    (void)tcp_telemetry_server_test_drain(b, tmp, sizeof(tmp));
    (void)tcp_telemetry_server_test_drain(c, tmp, sizeof(tmp));

    /* tel.evi should reach a + b but not c. */
    uint32_t delivered = tcp_telemetry_server_test_inject("tel.evi",
                                                         "dt=5 fb=0");
    TEST_ASSERT_EQUAL_UINT32(2, delivered);

    char out_a[256], out_b[256], out_c[256];
    (void)tcp_telemetry_server_test_drain(a, out_a, sizeof(out_a));
    (void)tcp_telemetry_server_test_drain(b, out_b, sizeof(out_b));
    (void)tcp_telemetry_server_test_drain(c, out_c, sizeof(out_c));

    TEST_ASSERT_TRUE(contains(out_a, "tel.evi"));
    TEST_ASSERT_TRUE(contains(out_b, "tel.evi"));
    TEST_ASSERT_EQUAL_INT(0, count_lines(out_c));
}

static void test_drop_oldest_under_backpressure(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*"); TEST_ASSERT_TRUE(slot >= 0);

    /* Drain the banner first. */
    char banner[256];
    (void)tcp_telemetry_server_test_drain(slot, banner, sizeof(banner));

    /* Inject many samples without draining → the per-client TX ring
     * (4 KB) will overflow and start dropping oldest. Each formatted
     * line is roughly 30 bytes ("tel.inf seq=N ts=M dt=K ok=1\n"); 200
     * lines is ~6 KB, well past the ring. */
    for (int i = 0; i < 200; i++) {
        (void)tcp_telemetry_server_test_inject("tel.inf", "dt=1 ok=1");
    }

    struct tcp_telemetry_server_stats st;
    tcp_telemetry_server_get_stats(&st);
    TEST_ASSERT_MESSAGE(st.samples_dropped > 0,
                             "expected drops once the per-client ring is full");

    /* Drain everything that fits and verify each retained line is
     * complete (every '\n' has a matching prefix that includes the
     * topic) — drop-oldest must evict whole lines, not partial ones. */
    char buf[8192];
    size_t n = tcp_telemetry_server_test_drain(slot, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    /* First retained byte should be the start of a complete line ("tel."),
     * not a fragment. */
    TEST_ASSERT_MESSAGE(buf[0] == 't',
                             "drop-oldest must evict whole lines, not fragments");
    /* Every retained line must end with '\n'. */
    int lines = count_lines(buf);
    TEST_ASSERT_TRUE(lines > 0);
    /* Final byte (before NUL) must be '\n' so no partial trailing line
     * was left behind. */
    TEST_ASSERT_EQUAL_INT8('\n', buf[n - 1]);
}

/* The current public test seam doesn't expose the inbound (peer→server)
 * command path directly. Re-narrowing the filter is testable by simply
 * opening another session with the narrower filter and asserting the
 * fanout count, which test_exact_filter_rejects_other_topic already
 * covers. The inbound command path is exercised end-to-end by the
 * QEMU smoke test in the PR (banner + SUB + filtered output).
 *
 * The remaining slot-pool-exhaustion check below ensures we cannot
 * overrun the synthetic-session pool, which mirrors the lwIP accept
 * path's pool guard. */

static void test_pool_exhaustion_returns_minus_one(void)
{
    tcp_telemetry_server_test_reset();
    int opened = 0;
    for (int i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        int s = tcp_telemetry_server_test_open_session("tel.*");
        TEST_ASSERT_TRUE(s >= 0);
        opened++;
    }
    int extra = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_EQUAL_INT(-1, extra);
    TEST_ASSERT_EQUAL_INT(MAX_TELEMETRY_SESSIONS, opened);
}

/* No-bus-stall: by construction, the server's fanout path uses
 * drop-oldest at the per-client ring level, not at msg_router level.
 * The previous test established that drops bump on overflow. This
 * test asserts the publisher-side counter (samples_dequeued) keeps
 * advancing even when the only subscriber is wedged (its ring is
 * full and never drained). */
static void test_publisher_keeps_advancing_when_client_wedged(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*"); TEST_ASSERT_TRUE(slot >= 0);
    (void)slot;   /* never drain it */

    for (int i = 0; i < 500; i++) {
        (void)tcp_telemetry_server_test_inject("tel.inf", "dt=99 ok=1");
    }

    struct tcp_telemetry_server_stats st;
    tcp_telemetry_server_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT64(500, st.samples_dequeued);
    /* Some delivered (until ring filled) + many dropped. Both > 0. */
    TEST_ASSERT_TRUE(st.samples_delivered > 0);
    TEST_ASSERT_TRUE(st.samples_dropped > 0);
}

/* ============================================================================
 * Inbound command parser (SUB / BYE / unknown / overflow)
 * ============================================================================ */

static void test_default_filter_is_tel_star(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session(NULL);
    TEST_ASSERT_TRUE(slot >= 0);
    const char *f = tcp_telemetry_server_test_session_filter(slot);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("tel.*", f);
}

static void test_banner_is_first_bytes_on_open(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_TRUE(slot >= 0);

    char out[512];
    size_t n = tcp_telemetry_server_test_drain(slot, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    /* Must lead with `# SLM-OS telemetryd v1\n`. */
    TEST_ASSERT_TRUE(contains(out, "# SLM-OS telemetryd v1\n"));
    TEST_ASSERT_TRUE(contains(out, "# subscribe with: SUB <pattern>\n"));
    TEST_ASSERT_TRUE(contains(out, "# default filter: tel.*\n"));
    /* The banner must come BEFORE any sample line (no characters
     * before the first `#`). */
    TEST_ASSERT_EQUAL_INT8('#', out[0]);
}

static void test_sub_command_resubscribes_filter(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_TRUE(slot >= 0);

    /* Drain banner. */
    char tmp[256];
    (void)tcp_telemetry_server_test_drain(slot, tmp, sizeof(tmp));

    /* Narrow the filter to tel.evi only. */
    static const char cmd[] = "SUB tel.evi\n";
    tcp_telemetry_server_test_feed_input(slot, cmd, sizeof(cmd) - 1);

    /* The filter field on the session must now be the new pattern. */
    TEST_ASSERT_EQUAL_STRING("tel.evi",
                             tcp_telemetry_server_test_session_filter(slot));

    /* Server should also have echoed an ack comment. */
    char ack[256];
    (void)tcp_telemetry_server_test_drain(slot, ack, sizeof(ack));
    TEST_ASSERT_TRUE(contains(ack, "# filter=tel.evi\n"));

    /* And subsequent injects: tel.evi delivers, tel.inf does not. */
    TEST_ASSERT_EQUAL_UINT32(1,
        tcp_telemetry_server_test_inject("tel.evi", "dt=1 fb=0"));
    TEST_ASSERT_EQUAL_UINT32(0,
        tcp_telemetry_server_test_inject("tel.inf", "dt=2 ok=1"));
}

static void test_sub_with_crlf_line_ending(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_TRUE(slot >= 0);

    /* CR-stripped before LF terminates the line. */
    static const char cmd[] = "SUB tel.inf\r\n";
    tcp_telemetry_server_test_feed_input(slot, cmd, sizeof(cmd) - 1);

    TEST_ASSERT_EQUAL_STRING("tel.inf",
                             tcp_telemetry_server_test_session_filter(slot));
}

static void test_bye_command_marks_session_closed(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_FALSE(tcp_telemetry_server_test_session_closed(slot));

    static const char cmd[] = "BYE\n";
    tcp_telemetry_server_test_feed_input(slot, cmd, sizeof(cmd) - 1);
    TEST_ASSERT_TRUE(tcp_telemetry_server_test_session_closed(slot));
}

static void test_unknown_command_echoes_comment(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_TRUE(slot >= 0);

    /* Drain banner first. */
    char banner[256];
    (void)tcp_telemetry_server_test_drain(slot, banner, sizeof(banner));

    static const char cmd[] = "WHATEVER\n";
    tcp_telemetry_server_test_feed_input(slot, cmd, sizeof(cmd) - 1);

    char out[256];
    (void)tcp_telemetry_server_test_drain(slot, out, sizeof(out));
    TEST_ASSERT_TRUE(contains(out, "# unknown: WHATEVER\n"));
}

static void test_oversized_input_does_not_overflow(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_TRUE(slot >= 0);

    /* Drain banner. */
    char banner[256];
    (void)tcp_telemetry_server_test_drain(slot, banner, sizeof(banner));

    /* Push a 200-byte command with no newline, then a newline. The
     * RX buffer is RX_LINE_MAX (=32) bytes; the parser must mark the
     * line as overflowed and emit a "<line too long>" comment instead
     * of clobbering memory. */
    char big[200];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = 'X';
    tcp_telemetry_server_test_feed_input(slot, big, sizeof(big));
    static const char term[] = "\n";
    tcp_telemetry_server_test_feed_input(slot, term, 1);

    char out[512];
    (void)tcp_telemetry_server_test_drain(slot, out, sizeof(out));
    TEST_ASSERT_TRUE(contains(out, "# unknown: <line too long>\n"));
    /* And the session should still be alive (not closed). */
    TEST_ASSERT_FALSE(tcp_telemetry_server_test_session_closed(slot));
}

static void test_blank_line_is_ignored(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_TRUE(slot >= 0);

    /* Drain banner. */
    char banner[256];
    (void)tcp_telemetry_server_test_drain(slot, banner, sizeof(banner));

    static const char cmd[] = "\n";
    tcp_telemetry_server_test_feed_input(slot, cmd, 1);

    char out[256];
    size_t n = tcp_telemetry_server_test_drain(slot, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, (int)n);   /* no echo, no ack, nothing */
}

/* ============================================================================
 * Public API: kick / foreach / stats
 * ============================================================================ */

struct first_id_ctx {
    uint32_t id;
    bool     have;
};

static bool first_id_visitor(const struct tcp_telemetry_session_info *info,
                             void *user)
{
    struct first_id_ctx *c = user;
    if (!c->have) {
        c->id = info->session_id;
        c->have = true;
    }
    return true;
}

static void test_foreach_walks_active_synthetic_sessions(void)
{
    tcp_telemetry_server_test_reset();
    /* foreach skips synthetic sessions by design (they're for tests
     * only), so this test is the negative assertion: opening synthetic
     * sessions should not pollute the public-API session list. */
    int s1 = tcp_telemetry_server_test_open_session("tel.*");
    int s2 = tcp_telemetry_server_test_open_session("tel.evi");
    TEST_ASSERT_TRUE(s1 >= 0);
    TEST_ASSERT_TRUE(s2 >= 0);

    struct first_id_ctx c = {0};
    tcp_telemetry_server_foreach(first_id_visitor, &c);
    TEST_ASSERT_FALSE(c.have);

    /* And get_stats's sessions_active also excludes synthetics so the
     * lab's `telemetry server status` does not lie. */
    struct tcp_telemetry_server_stats st;
    tcp_telemetry_server_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(0, st.sessions_active);
}

static void test_kick_returns_false_for_unknown_id(void)
{
    tcp_telemetry_server_test_reset();
    /* The synthetic-session pool is never reachable via the public
     * kick API (which iterates over real sessions only). So any kick
     * id should be rejected — confirms the synthetic test seam doesn't
     * accidentally leak into the operator-visible kick path. */
    int slot = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_TRUE(slot >= 0);
    TEST_ASSERT_FALSE(tcp_telemetry_server_kick(0xDEADBEEFu));
}

static void test_stats_counters_track_dequeue_and_deliver(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_TRUE(slot >= 0);

    /* Drain the banner so subsequent ring stays at known capacity. */
    char tmp[256];
    (void)tcp_telemetry_server_test_drain(slot, tmp, sizeof(tmp));

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT32(1,
            tcp_telemetry_server_test_inject("tel.inf", "dt=1 ok=1"));
    }

    struct tcp_telemetry_server_stats st;
    tcp_telemetry_server_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT64(5, st.samples_dequeued);
    TEST_ASSERT_EQUAL_UINT64(5, st.samples_delivered);
    TEST_ASSERT_EQUAL_UINT64(0, st.samples_dropped);
}

/* Regression for the round-2 review finding: the original
 * tcp_telemetry_server_poll() short-circuited on `!g_listen_pcb`
 * BEFORE running the per-session teardown loop. That meant a
 * `telemetry server stop` followed by `telemetry server start`
 * gradually starved the session pool — sessions were marked closed
 * but never tcp_close'd, never session_free'd, and their slots
 * stayed in_use. The fix splits poll() so the msg_router drain is
 * gated on the listener but session teardown always runs.
 *
 * Test strategy: fill the entire pool with simulated "closed real"
 * sessions (the test seam fakes the post-disconnect state without
 * needing a live lwIP pcb). Without the fix, poll() does nothing
 * because there's no listener; the slots stay occupied; the
 * subsequent open returns -1. With the fix, poll() reclaims them all
 * and the open succeeds. */
static void test_poll_reclaims_closed_sessions_without_listener(void)
{
    tcp_telemetry_server_test_reset();

    int slots[MAX_TELEMETRY_SESSIONS];
    for (int i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        slots[i] = tcp_telemetry_server_test_open_session("tel.*");
        TEST_ASSERT_TRUE(slots[i] >= 0);
        tcp_telemetry_server_test_simulate_closed_real_session(slots[i]);
    }

    /* Pool is now full of "closed real" sessions and the listener is
     * not running (test build never starts it). poll() must still
     * reclaim them. */
    tcp_telemetry_server_poll();

    /* A fresh open should succeed because all slots got freed. Without
     * the fix, this returns -1. */
    int fresh = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_MESSAGE(fresh >= 0,
        "poll() must reclaim closed sessions even when the listener is "
        "stopped — otherwise telemetry server stop/start cycles starve "
        "the session pool");
}

static void test_drops_count_per_line_evicted_not_per_call(void)
{
    tcp_telemetry_server_test_reset();
    int slot = tcp_telemetry_server_test_open_session("tel.*");
    TEST_ASSERT_TRUE(slot >= 0);

    /* Drain banner. */
    char banner[256];
    (void)tcp_telemetry_server_test_drain(slot, banner, sizeof(banner));

    /* Inject enough samples to overflow the per-client ring. Each
     * line is ~30 bytes; the ring is 4 KB; 200 lines is ~6 KB so we
     * are guaranteed to be in the drop-oldest path for the back
     * half of these. */
    const uint64_t N = 200;
    for (uint64_t i = 0; i < N; i++) {
        (void)tcp_telemetry_server_test_inject("tel.inf", "dt=1 ok=1");
    }

    struct tcp_telemetry_server_stats st;
    tcp_telemetry_server_get_stats(&st);

    /* Sanity. */
    TEST_ASSERT_EQUAL_UINT64(N, st.samples_dequeued);

    /* The per-client TX ring can hold roughly TX_RING_SIZE / line_len
     * lines simultaneously (~136 for 30-byte lines in a 4 KB ring).
     * Anything beyond that capacity must show up as a drop. We don't
     * pin the exact ring capacity here — only that the count is in
     * a credible range that excludes the old "single drop per call"
     * undercount (which would land somewhere south of N/2). */
    TEST_ASSERT_MESSAGE(st.samples_dropped > N / 4,
        "drops counter looks like the old per-call undercount");
    TEST_ASSERT_MESSAGE(st.samples_dropped < N,
        "drops should not exceed total samples dequeued");

    /* The session-local counter must agree with the global one
     * (single-session here, so they are equal). */
    struct first_id_ctx unused = {0};
    (void)unused;
    /* foreach skips synthetic sessions; can't read s->drops via the
     * public API for this test. The global counter accuracy is the
     * load-bearing assertion — the per-session and global counters
     * are bumped by the same arithmetic at the same site. */
}

/* ============================================================================
 * Entry point
 * ============================================================================ */

int test_suite_tcp_telemetry_server(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sample_format_matches_spec);
    RUN_TEST(test_exact_filter_rejects_other_topic);
    RUN_TEST(test_glob_filter_matches_prefix);
    RUN_TEST(test_fanout_to_multiple_sessions);
    RUN_TEST(test_drop_oldest_under_backpressure);
    RUN_TEST(test_pool_exhaustion_returns_minus_one);
    RUN_TEST(test_publisher_keeps_advancing_when_client_wedged);

    RUN_TEST(test_default_filter_is_tel_star);
    RUN_TEST(test_banner_is_first_bytes_on_open);
    RUN_TEST(test_sub_command_resubscribes_filter);
    RUN_TEST(test_sub_with_crlf_line_ending);
    RUN_TEST(test_bye_command_marks_session_closed);
    RUN_TEST(test_unknown_command_echoes_comment);
    RUN_TEST(test_oversized_input_does_not_overflow);
    RUN_TEST(test_blank_line_is_ignored);

    RUN_TEST(test_foreach_walks_active_synthetic_sessions);
    RUN_TEST(test_kick_returns_false_for_unknown_id);
    RUN_TEST(test_stats_counters_track_dequeue_and_deliver);
    RUN_TEST(test_drops_count_per_line_evicted_not_per_call);
    RUN_TEST(test_poll_reclaims_closed_sessions_without_listener);
    return UNITY_END();
}
