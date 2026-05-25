/*
 * test_sshd_autostart.c — config-file parser regression coverage.
 *
 * `kernel/net/ssh/sshd_autostart.c` reads /mnt/files/etc/sshd.conf
 * at boot to decide whether to bring the daemon up. The decimal /
 * bool / line parsers are pure functions — testable in isolation
 * via the test-hook header.
 *
 * What's not exercised here: the integration path that actually
 * calls net_init() + sshd_start(). That requires a live VFS mount
 * + net stack; the parsers are the part that benefits from cheap
 * unit-level coverage.
 *
 * Gated on NET_SSHD (the autostart module only compiles then).
 */

#if defined(NET_SSHD)

#include "sshd_autostart_test.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* parse_decimal_u16                                                  */
/* ---------------------------------------------------------------- */

static void test_decimal_accepts_basic(void)
{
    uint16_t out = 0;
    TEST_ASSERT_EQUAL_INT(0, sshd_autostart_test_parse_decimal_u16("22", &out));
    TEST_ASSERT_EQUAL_UINT16(22, out);

    TEST_ASSERT_EQUAL_INT(0, sshd_autostart_test_parse_decimal_u16("2222", &out));
    TEST_ASSERT_EQUAL_UINT16(2222, out);

    TEST_ASSERT_EQUAL_INT(0, sshd_autostart_test_parse_decimal_u16("65535", &out));
    TEST_ASSERT_EQUAL_UINT16(65535, out);
}

static void test_decimal_stops_at_whitespace_or_eol(void)
{
    /* The parser is used on field values that may carry trailing
     * whitespace / CR / LF — must terminate cleanly without
     * confusing the trailing byte for a digit. */
    uint16_t out = 0;
    TEST_ASSERT_EQUAL_INT(0, sshd_autostart_test_parse_decimal_u16("22\r", &out));
    TEST_ASSERT_EQUAL_UINT16(22, out);

    TEST_ASSERT_EQUAL_INT(0, sshd_autostart_test_parse_decimal_u16("22\n", &out));
    TEST_ASSERT_EQUAL_UINT16(22, out);

    TEST_ASSERT_EQUAL_INT(0, sshd_autostart_test_parse_decimal_u16("22 ", &out));
    TEST_ASSERT_EQUAL_UINT16(22, out);
}

static void test_decimal_rejects_non_digit(void)
{
    uint16_t out = 0xCCCCu;
    TEST_ASSERT_NOT_EQUAL(0, sshd_autostart_test_parse_decimal_u16("22a", &out));
    TEST_ASSERT_NOT_EQUAL(0, sshd_autostart_test_parse_decimal_u16("abc", &out));
    TEST_ASSERT_NOT_EQUAL(0, sshd_autostart_test_parse_decimal_u16("-1",  &out));
    TEST_ASSERT_NOT_EQUAL(0, sshd_autostart_test_parse_decimal_u16("",    &out));
    TEST_ASSERT_NOT_EQUAL(0, sshd_autostart_test_parse_decimal_u16(NULL,  &out));
}

static void test_decimal_rejects_overflow(void)
{
    /* Anything strictly greater than 0xFFFF must reject. */
    uint16_t out = 0;
    TEST_ASSERT_NOT_EQUAL(0, sshd_autostart_test_parse_decimal_u16("65536", &out));
    TEST_ASSERT_NOT_EQUAL(0, sshd_autostart_test_parse_decimal_u16("99999", &out));
    TEST_ASSERT_NOT_EQUAL(0, sshd_autostart_test_parse_decimal_u16("4294967295", &out));
}

/* ---------------------------------------------------------------- */
/* token_equals + parse_bool                                          */
/* ---------------------------------------------------------------- */

static void test_token_equals_strict_match(void)
{
    /* Exact match, terminator-aware. */
    TEST_ASSERT_TRUE(sshd_autostart_test_token_equals("yes",   "yes"));
    TEST_ASSERT_TRUE(sshd_autostart_test_token_equals("yes\r", "yes"));
    TEST_ASSERT_TRUE(sshd_autostart_test_token_equals("yes\n", "yes"));
    TEST_ASSERT_TRUE(sshd_autostart_test_token_equals("yes ",  "yes"));
    TEST_ASSERT_TRUE(sshd_autostart_test_token_equals("yes\t", "yes"));
}

static void test_token_equals_rejects_prefix(void)
{
    /* Regression for the strncmp-style prefix bug parse_bool used
     * to have ("1abc" matched "1", "trueish" matched "true"). */
    TEST_ASSERT_FALSE(sshd_autostart_test_token_equals("yesyes",   "yes"));
    TEST_ASSERT_FALSE(sshd_autostart_test_token_equals("trueish",  "true"));
    TEST_ASSERT_FALSE(sshd_autostart_test_token_equals("11",       "1"));
    TEST_ASSERT_FALSE(sshd_autostart_test_token_equals("oneself",  "on"));
}

static void test_parse_bool_true_set(void)
{
    TEST_ASSERT_TRUE(sshd_autostart_test_parse_bool("1"));
    TEST_ASSERT_TRUE(sshd_autostart_test_parse_bool("true"));
    TEST_ASSERT_TRUE(sshd_autostart_test_parse_bool("yes"));
    TEST_ASSERT_TRUE(sshd_autostart_test_parse_bool("on"));
    /* Trailing-CR + trailing-LF must still parse — Windows line
     * endings in /etc/sshd.conf are common. */
    TEST_ASSERT_TRUE(sshd_autostart_test_parse_bool("1\r"));
    TEST_ASSERT_TRUE(sshd_autostart_test_parse_bool("true\n"));
}

static void test_parse_bool_false_set(void)
{
    /* parse_bool's contract is "true on a recognised yes-token,
     * false otherwise". Verify the non-yes side. */
    TEST_ASSERT_FALSE(sshd_autostart_test_parse_bool("0"));
    TEST_ASSERT_FALSE(sshd_autostart_test_parse_bool("false"));
    TEST_ASSERT_FALSE(sshd_autostart_test_parse_bool("no"));
    TEST_ASSERT_FALSE(sshd_autostart_test_parse_bool("off"));
    TEST_ASSERT_FALSE(sshd_autostart_test_parse_bool(""));
    TEST_ASSERT_FALSE(sshd_autostart_test_parse_bool(NULL));
    /* The prefix-match regression — "1abc" must NOT parse as 1. */
    TEST_ASSERT_FALSE(sshd_autostart_test_parse_bool("1abc"));
    TEST_ASSERT_FALSE(sshd_autostart_test_parse_bool("trueish"));
}

/* ---------------------------------------------------------------- */
/* parse_config                                                       */
/* ---------------------------------------------------------------- */

static void run_parse_config(const char *src, bool initial_enabled,
                             uint16_t initial_port,
                             struct sshd_autostart_cfg_view *out)
{
    /* parse_config mutates the buffer in place; copy first. */
    char buf[512];
    size_t n = strlen(src);
    TEST_ASSERT_TRUE_MESSAGE(n + 1u <= sizeof(buf), "test fixture too large");
    memcpy(buf, src, n);
    buf[n] = '\0';

    out->enabled = initial_enabled;
    out->port    = initial_port;
    sshd_autostart_test_parse_config(out, buf, n + 1u);
}

static void test_parse_config_enables_and_sets_port(void)
{
    struct sshd_autostart_cfg_view cfg = { 0 };
    run_parse_config("enabled=1\nport=22\n", false, 2222, &cfg);
    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(22, cfg.port);
}

static void test_parse_config_can_disable(void)
{
    /* An `enabled=0` line must turn off a default-on caller. */
    struct sshd_autostart_cfg_view cfg = { 0 };
    run_parse_config("enabled=0\n", true, 2222, &cfg);
    TEST_ASSERT_FALSE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(2222, cfg.port);
}

static void test_parse_config_ignores_comments_and_blanks(void)
{
    struct sshd_autostart_cfg_view cfg = { 0 };
    run_parse_config("# this is a comment\n"
                     "\n"
                     "   # leading whitespace before hash\n"
                     "enabled=true\n"
                     "\n",
                     false, 2222, &cfg);
    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(2222, cfg.port);
}

static void test_parse_config_handles_crlf(void)
{
    /* Windows / cross-platform sshd.conf edits. */
    struct sshd_autostart_cfg_view cfg = { 0 };
    run_parse_config("enabled=yes\r\nport=22\r\n", false, 2222, &cfg);
    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(22, cfg.port);
}

static void test_parse_config_invalid_port_is_ignored(void)
{
    /* A garbled `port=` line must NOT overwrite a sane default. */
    struct sshd_autostart_cfg_view cfg = { 0 };
    run_parse_config("enabled=1\nport=abcdef\n", false, 2222, &cfg);
    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(2222, cfg.port);
}

static void test_parse_config_zero_port_is_rejected(void)
{
    /* port=0 is meaningless (would let sshd_start pick the default);
     * the parser only writes when the parsed value > 0. */
    struct sshd_autostart_cfg_view cfg = { 0 };
    run_parse_config("port=0\n", false, 2222, &cfg);
    TEST_ASSERT_EQUAL_UINT16(2222, cfg.port);
}

static void test_parse_config_ignores_unknown_keys(void)
{
    /* Forward-compat: keys we don't recognise are silently dropped
     * rather than rejecting the whole file. */
    struct sshd_autostart_cfg_view cfg = { 0 };
    run_parse_config("hostname=raspi5\n"
                     "enabled=1\n"
                     "color=blue\n",
                     false, 2222, &cfg);
    TEST_ASSERT_TRUE(cfg.enabled);
}

int test_suite_sshd_autostart(void)
{
    UnityBegin("sshd_autostart (config-file parser)");

    RUN_TEST(test_decimal_accepts_basic);
    RUN_TEST(test_decimal_stops_at_whitespace_or_eol);
    RUN_TEST(test_decimal_rejects_non_digit);
    RUN_TEST(test_decimal_rejects_overflow);

    RUN_TEST(test_token_equals_strict_match);
    RUN_TEST(test_token_equals_rejects_prefix);
    RUN_TEST(test_parse_bool_true_set);
    RUN_TEST(test_parse_bool_false_set);

    RUN_TEST(test_parse_config_enables_and_sets_port);
    RUN_TEST(test_parse_config_can_disable);
    RUN_TEST(test_parse_config_ignores_comments_and_blanks);
    RUN_TEST(test_parse_config_handles_crlf);
    RUN_TEST(test_parse_config_invalid_port_is_ignored);
    RUN_TEST(test_parse_config_zero_port_is_rejected);
    RUN_TEST(test_parse_config_ignores_unknown_keys);

    return UnityEnd();
}

#else  /* !NET_SSHD */

typedef int test_sshd_autostart_placeholder_t;

#endif /* NET_SSHD */
