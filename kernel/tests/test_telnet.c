/*
 * test_telnet.c - Unit tests for the telnet IAC state machine
 *
 * The parser is pure (no lwIP / shell_session), so these tests wire
 * a stub telnet_ops vtable that records every callback invocation.
 * Each test drives bytes through telnet_rx and asserts against the
 * recorded events.
 */

#include "unity.h"
#include "../include/telnet.h"
#include "../include/string.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Stub callbacks
 * ============================================================================ */

#define RX_CAP   256
#define SEND_CAP 256

struct stub {
    uint8_t  rx[RX_CAP];
    size_t   rx_len;

    uint8_t  sent[SEND_CAP];
    size_t   sent_len;

    uint16_t naws_cols;
    uint16_t naws_rows;
    uint32_t naws_count;

    char     term[64];
    uint32_t term_count;

    uint32_t interrupt_count;
};

static void stub_inject_rx(void *c, uint8_t b)
{
    struct stub *s = c;
    if (s->rx_len < RX_CAP) s->rx[s->rx_len++] = b;
}

static void stub_send(void *c, const uint8_t *data, size_t len)
{
    struct stub *s = c;
    for (size_t i = 0; i < len && s->sent_len < SEND_CAP; i++) {
        s->sent[s->sent_len++] = data[i];
    }
}

static void stub_naws(void *c, uint16_t cols, uint16_t rows)
{
    struct stub *s = c;
    s->naws_cols = cols;
    s->naws_rows = rows;
    s->naws_count++;
}

static void stub_term(void *c, const char *term)
{
    struct stub *s = c;
    size_t n = strlen(term);
    if (n >= sizeof(s->term)) n = sizeof(s->term) - 1;
    for (size_t i = 0; i < n; i++) s->term[i] = term[i];
    s->term[n] = '\0';
    s->term_count++;
}

static void stub_interrupt(void *c)
{
    struct stub *s = c;
    s->interrupt_count++;
}

static void stub_init(struct stub *s, struct telnet_parser *tp)
{
    s->rx_len        = 0;
    s->sent_len      = 0;
    s->naws_cols     = 0;
    s->naws_rows     = 0;
    s->naws_count    = 0;
    s->term[0]       = '\0';
    s->term_count    = 0;
    s->interrupt_count = 0;

    struct telnet_ops ops = {
        .inject_rx    = stub_inject_rx,
        .send_to_peer = stub_send,
        .on_naws      = stub_naws,
        .on_term_type = stub_term,
        .on_interrupt = stub_interrupt,
        .ctx          = s,
    };
    telnet_init(tp, &ops);
}

/* ============================================================================
 * Data pass-through
 * ============================================================================ */

static void test_plain_data_passes_through(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = "hello";
    telnet_rx(&tp, in, 5);

    TEST_ASSERT_EQUAL_UINT32(5, s.rx_len);
    TEST_ASSERT_EQUAL_MEMORY("hello", s.rx, 5);
    TEST_ASSERT_EQUAL_UINT32(0, s.sent_len);
}

static void test_iac_iac_emits_literal_0xFF(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    /* "A" IAC IAC "B" → "A\xFFB" */
    const uint8_t in[] = { 'A', 0xFF, 0xFF, 'B' };
    telnet_rx(&tp, in, sizeof(in));

    TEST_ASSERT_EQUAL_UINT32(3, s.rx_len);
    TEST_ASSERT_EQUAL_UINT8('A',  s.rx[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s.rx[1]);
    TEST_ASSERT_EQUAL_UINT8('B',  s.rx[2]);
    TEST_ASSERT_EQUAL_UINT32(0, s.sent_len);
}

/* ============================================================================
 * CR / LF normalization
 * ============================================================================ */

static void test_cr_lf_submits_once(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = { 'x', '\r', '\n' };
    telnet_rx(&tp, in, sizeof(in));

    /* Expect "x\r" — LF swallowed. */
    TEST_ASSERT_EQUAL_UINT32(2, s.rx_len);
    TEST_ASSERT_EQUAL_UINT8('x',  s.rx[0]);
    TEST_ASSERT_EQUAL_UINT8('\r', s.rx[1]);
}

static void test_cr_nul_submits_once(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = { 'x', '\r', 0x00 };
    telnet_rx(&tp, in, sizeof(in));

    TEST_ASSERT_EQUAL_UINT32(2, s.rx_len);
    TEST_ASSERT_EQUAL_UINT8('x',  s.rx[0]);
    TEST_ASSERT_EQUAL_UINT8('\r', s.rx[1]);
}

static void test_bare_lf_passes_through(void)
{
    /* Raw-nc style input — bare LF is a valid submit. */
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = { 'x', '\n' };
    telnet_rx(&tp, in, sizeof(in));

    TEST_ASSERT_EQUAL_UINT32(2, s.rx_len);
    TEST_ASSERT_EQUAL_UINT8('x',  s.rx[0]);
    TEST_ASSERT_EQUAL_UINT8('\n', s.rx[1]);
}

static void test_cr_other_byte_emits_both(void)
{
    /* CR followed by a non-LF, non-NUL byte: both reach the shell. */
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = { '\r', 'y' };
    telnet_rx(&tp, in, sizeof(in));

    TEST_ASSERT_EQUAL_UINT32(2, s.rx_len);
    TEST_ASSERT_EQUAL_UINT8('\r', s.rx[0]);
    TEST_ASSERT_EQUAL_UINT8('y',  s.rx[1]);
}

/* ============================================================================
 * IAC IP (interrupt)
 * ============================================================================ */

static void test_iac_ip_fires_interrupt(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = { 'a', 0xFF, TELNET_IP, 'b' };
    telnet_rx(&tp, in, sizeof(in));

    TEST_ASSERT_EQUAL_UINT32(1, s.interrupt_count);
    /* 'a' and 'b' still flow through. */
    TEST_ASSERT_EQUAL_UINT32(2, s.rx_len);
    TEST_ASSERT_EQUAL_UINT8('a', s.rx[0]);
    TEST_ASSERT_EQUAL_UINT8('b', s.rx[1]);
}

/* ============================================================================
 * Option negotiation replies
 * ============================================================================ */

static void test_initial_negotiation_sends_four_options(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    telnet_send_initial_negotiation(&tp);

    /* 4 × 3 = 12 bytes: IAC WILL ECHO, IAC WILL SGA, IAC DO NAWS, IAC DO TTYPE. */
    const uint8_t expected[] = {
        0xFF, TELNET_WILL, TELNET_OPT_ECHO,
        0xFF, TELNET_WILL, TELNET_OPT_SGA,
        0xFF, TELNET_DO,   TELNET_OPT_NAWS,
        0xFF, TELNET_DO,   TELNET_OPT_TTYPE,
    };
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), s.sent_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, s.sent, sizeof(expected));
}

static void test_do_echo_replies_will_echo(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = { 0xFF, TELNET_DO, TELNET_OPT_ECHO };
    telnet_rx(&tp, in, sizeof(in));

    const uint8_t expected[] = { 0xFF, TELNET_WILL, TELNET_OPT_ECHO };
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), s.sent_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, s.sent, sizeof(expected));
}

static void test_do_unknown_option_refused(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    /* DO BINARY (option 0) — we refuse. */
    const uint8_t in[] = { 0xFF, TELNET_DO, TELNET_OPT_BINARY };
    telnet_rx(&tp, in, sizeof(in));

    const uint8_t expected[] = { 0xFF, TELNET_WONT, TELNET_OPT_BINARY };
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), s.sent_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, s.sent, sizeof(expected));
}

static void test_will_naws_triggers_do_naws(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = { 0xFF, TELNET_WILL, TELNET_OPT_NAWS };
    telnet_rx(&tp, in, sizeof(in));

    const uint8_t expected[] = { 0xFF, TELNET_DO, TELNET_OPT_NAWS };
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), s.sent_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, s.sent, sizeof(expected));
}

static void test_will_ttype_triggers_do_plus_send_request(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = { 0xFF, TELNET_WILL, TELNET_OPT_TTYPE };
    telnet_rx(&tp, in, sizeof(in));

    /* Expect DO TTYPE, then a SEND subneg request. */
    const uint8_t expected[] = {
        0xFF, TELNET_DO,   TELNET_OPT_TTYPE,
        0xFF, TELNET_SB,   TELNET_OPT_TTYPE, TELNET_TTYPE_SEND, 0xFF, TELNET_SE,
    };
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), s.sent_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, s.sent, sizeof(expected));
}

static void test_duplicate_do_echo_no_repeat(void)
{
    /* Two identical DO ECHO arrivals: first one replies, second is a no-op. */
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = { 0xFF, TELNET_DO, TELNET_OPT_ECHO,
                           0xFF, TELNET_DO, TELNET_OPT_ECHO };
    telnet_rx(&tp, in, sizeof(in));

    const uint8_t expected[] = { 0xFF, TELNET_WILL, TELNET_OPT_ECHO };
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), s.sent_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, s.sent, sizeof(expected));
}

/* ============================================================================
 * Subnegotiation
 * ============================================================================ */

static void test_naws_subneg_parses_cols_rows(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    /* Pretend we already did DO NAWS so the flag is set (irrelevant for
     * the subneg path, but realistic). Send:
     *   IAC SB NAWS 0 120 0 40 IAC SE  → 120x40 */
    const uint8_t in[] = {
        0xFF, TELNET_SB, TELNET_OPT_NAWS,
        0x00, 0x78,    /* cols = 120 */
        0x00, 0x28,    /* rows = 40 */
        0xFF, TELNET_SE,
    };
    telnet_rx(&tp, in, sizeof(in));

    TEST_ASSERT_EQUAL_UINT32(1, s.naws_count);
    TEST_ASSERT_EQUAL_UINT16(120, s.naws_cols);
    TEST_ASSERT_EQUAL_UINT16(40,  s.naws_rows);
}

static void test_ttype_subneg_parses_string(void)
{
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    /* IAC SB TTYPE IS "xterm-256color" IAC SE */
    const uint8_t in[] = {
        0xFF, TELNET_SB, TELNET_OPT_TTYPE, TELNET_TTYPE_IS,
        'x','t','e','r','m','-','2','5','6','c','o','l','o','r',
        0xFF, TELNET_SE,
    };
    telnet_rx(&tp, in, sizeof(in));

    TEST_ASSERT_EQUAL_UINT32(1, s.term_count);
    TEST_ASSERT_EQUAL_STRING("xterm-256color", s.term);
}

static void test_subneg_iac_iac_is_literal(void)
{
    /* TTYPE string containing a literal 0xFF byte (escaped as IAC IAC). */
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    const uint8_t in[] = {
        0xFF, TELNET_SB, TELNET_OPT_TTYPE, TELNET_TTYPE_IS,
        'a', 0xFF, 0xFF, 'b',      /* "a\xFFb" */
        0xFF, TELNET_SE,
    };
    telnet_rx(&tp, in, sizeof(in));

    TEST_ASSERT_EQUAL_UINT32(1, s.term_count);
    TEST_ASSERT_EQUAL_UINT32(3, strlen(s.term));
    TEST_ASSERT_EQUAL_UINT8('a',  (uint8_t)s.term[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, (uint8_t)s.term[1]);
    TEST_ASSERT_EQUAL_UINT8('b',  (uint8_t)s.term[2]);
}

static void test_subneg_oversize_bounded(void)
{
    /* Feed a subneg payload longer than TELNET_SB_BUF_SIZE.
     * Must not crash; truncation is acceptable. */
    struct telnet_parser tp;
    struct stub s;
    stub_init(&s, &tp);

    /* Start of TTYPE IS payload, then 200 'x' bytes. */
    uint8_t buf[256];
    size_t n = 0;
    buf[n++] = 0xFF;
    buf[n++] = TELNET_SB;
    buf[n++] = TELNET_OPT_TTYPE;
    buf[n++] = TELNET_TTYPE_IS;
    for (int i = 0; i < 200; i++) buf[n++] = 'x';
    buf[n++] = 0xFF;
    buf[n++] = TELNET_SE;

    telnet_rx(&tp, buf, n);
    /* Parser shouldn't crash — whether term_count fires or not is
     * implementation detail; assert only bounded string length. */
    TEST_ASSERT_TRUE(strlen(s.term) < sizeof(s.term));
}

/* ============================================================================
 * Suite entry
 * ============================================================================ */

int test_suite_telnet(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_plain_data_passes_through);
    RUN_TEST(test_iac_iac_emits_literal_0xFF);

    RUN_TEST(test_cr_lf_submits_once);
    RUN_TEST(test_cr_nul_submits_once);
    RUN_TEST(test_bare_lf_passes_through);
    RUN_TEST(test_cr_other_byte_emits_both);

    RUN_TEST(test_iac_ip_fires_interrupt);

    RUN_TEST(test_initial_negotiation_sends_four_options);
    RUN_TEST(test_do_echo_replies_will_echo);
    RUN_TEST(test_do_unknown_option_refused);
    RUN_TEST(test_will_naws_triggers_do_naws);
    RUN_TEST(test_will_ttype_triggers_do_plus_send_request);
    RUN_TEST(test_duplicate_do_echo_no_repeat);

    RUN_TEST(test_naws_subneg_parses_cols_rows);
    RUN_TEST(test_ttype_subneg_parses_string);
    RUN_TEST(test_subneg_iac_iac_is_literal);
    RUN_TEST(test_subneg_oversize_bounded);

    return UNITY_END();
}
