/*
 * test_telnetd_config.c - Unit tests for /etc/telnetd.conf parser
 *
 * The parser is pure (no I/O, no VFS), so these tests feed string
 * literals directly and assert against the parsed config struct.
 */

#include "unity.h"
#include "../include/telnetd_config.h"
#include "../include/shell_session.h"   /* MAX_TCP_SHELL_SESSIONS */
#include "../include/string.h"
#include "../include/uart.h"             /* uart_snprintf */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Make a writable copy for the parser (which rewrites the buffer). */
#define PARSE(config_literal) do {                                       \
    char _buf[512];                                                      \
    size_t _n = sizeof(_buf) - 1;                                        \
    strncpy(_buf, (config_literal), _n);                                 \
    _buf[_n] = '\0';                                                     \
    size_t _len = strlen(_buf) + 1;                                      \
    telnetd_config_parse(&cfg, _buf, _len);                              \
} while (0)

/* ============================================================================
 * Defaults
 * ============================================================================ */

static void test_defaults_sane(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);

    TEST_ASSERT_FALSE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(2323, cfg.port);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.bind_ip);     /* 0.0.0.0 */
    TEST_ASSERT_EQUAL_UINT8(MAX_TCP_SHELL_SESSIONS, cfg.max_sessions);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.unknown_keys);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.malformed_lines);
}

/* ============================================================================
 * Happy path
 * ============================================================================ */

static void test_full_config_parses(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("enabled=true\n"
          "port=2300\n"
          "bind=127.0.0.1\n"
          "max_sessions=16\n");

    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(2300, cfg.port);
    /* 127.0.0.1 = 0x7F.00.00.01 in network byte order (big-endian high
     * bytes first on the wire). Our parser stores it little-endian
     * (127 in the lowest byte) because parse_ipv4 shifts by octet*8
     * starting at offset 0. 127 | 0 | 0 | (1<<24) = 0x0100007F. */
    TEST_ASSERT_EQUAL_UINT32(0x0100007F, cfg.bind_ip);
    TEST_ASSERT_EQUAL_UINT8(16, cfg.max_sessions);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.unknown_keys);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.malformed_lines);
}

static void test_boolean_variants_accepted(void)
{
    const char *truthy[] = { "true", "TRUE", "True", "1", "yes", "YES",
                             "on", "On", NULL };
    const char *falsy[] = { "false", "FALSE", "0", "no", "off", NULL };

    for (int i = 0; truthy[i]; i++) {
        struct telnetd_config cfg;
        telnetd_config_defaults(&cfg);
        char buf[64];
        uart_snprintf(buf, sizeof(buf), "enabled=%s\n", truthy[i]);
        size_t len = strlen(buf) + 1;
        telnetd_config_parse(&cfg, buf, len);
        TEST_ASSERT_TRUE(cfg.enabled);
    }

    for (int i = 0; falsy[i]; i++) {
        struct telnetd_config cfg;
        telnetd_config_defaults(&cfg);
        /* Start from enabled=true so false is an observable change. */
        cfg.enabled = true;
        char buf[64];
        uart_snprintf(buf, sizeof(buf), "enabled=%s\n", falsy[i]);
        size_t len = strlen(buf) + 1;
        telnetd_config_parse(&cfg, buf, len);
        TEST_ASSERT_FALSE(cfg.enabled);
    }
}

static void test_comments_and_blanks_ignored(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("# This is a comment\n"
          "\n"
          "   \n"
          "# enabled=true (this is commented out)\n"
          "port=1234\n"
          "\n");

    TEST_ASSERT_FALSE(cfg.enabled);                /* never set */
    TEST_ASSERT_EQUAL_UINT16(1234, cfg.port);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.unknown_keys);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.malformed_lines);
}

static void test_whitespace_around_equals(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("  port   =   5000  \n"
          "enabled\t=\ttrue\n");

    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(5000, cfg.port);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.malformed_lines);
}

static void test_case_insensitive_keys(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("Enabled=yes\n"
          "PORT=4242\n");

    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(4242, cfg.port);
}

/* ============================================================================
 * Error handling — unknown keys, malformed lines
 * ============================================================================ */

static void test_unknown_key_counted_not_fatal(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("enabled=true\n"
          "this_key_does_not_exist=hello\n"
          "port=8080\n");

    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(8080, cfg.port);
    TEST_ASSERT_EQUAL_UINT16(1, cfg.unknown_keys);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.malformed_lines);
}

static void test_malformed_line_counted(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("enabled=true\n"
          "this has no equals sign\n"
          "port=bogus\n"
          "bind=300.400.500.600\n"
          "port=9090\n");

    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(9090, cfg.port);        /* last good wins */
    TEST_ASSERT_EQUAL_UINT16(3, cfg.malformed_lines); /* 3 bad lines */
}

static void test_port_out_of_range_malformed(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("port=99999\n");

    TEST_ASSERT_EQUAL_UINT16(2323, cfg.port);   /* unchanged */
    TEST_ASSERT_EQUAL_UINT16(1, cfg.malformed_lines);
}

static void test_max_sessions_clamped_to_pool_cap(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("max_sessions=250\n");   /* >= MAX_TCP_SHELL_SESSIONS */

    /* Clamped to the pool cap, not flagged as malformed. */
    TEST_ASSERT_EQUAL_UINT8(MAX_TCP_SHELL_SESSIONS, cfg.max_sessions);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.malformed_lines);
}

static void test_max_sessions_zero_rejected(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("max_sessions=0\n");

    /* Zero is meaningless for a session pool; reject as malformed. */
    TEST_ASSERT_EQUAL_UINT8(MAX_TCP_SHELL_SESSIONS, cfg.max_sessions);
    TEST_ASSERT_EQUAL_UINT16(1, cfg.malformed_lines);
}

/* ============================================================================
 * Edge cases
 * ============================================================================ */

static void test_empty_input_is_defaults(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    char buf[4] = "";
    telnetd_config_parse(&cfg, buf, 1);   /* just the NUL */

    TEST_ASSERT_FALSE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(2323, cfg.port);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.unknown_keys);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.malformed_lines);
}

static void test_null_cfg_rejected(void)
{
    char buf[] = "enabled=true\n";
    int rc = telnetd_config_parse(NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

static void test_missing_newline_on_last_line(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("port=7777");    /* no trailing \n */

    TEST_ASSERT_EQUAL_UINT16(7777, cfg.port);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.malformed_lines);
}

static void test_crlf_line_endings_handled(void)
{
    struct telnetd_config cfg;
    telnetd_config_defaults(&cfg);
    PARSE("enabled=true\r\nport=8888\r\n");

    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_UINT16(8888, cfg.port);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.malformed_lines);
}

/* ============================================================================
 * Entry
 * ============================================================================ */

int test_suite_telnetd_config(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_defaults_sane);

    RUN_TEST(test_full_config_parses);
    RUN_TEST(test_boolean_variants_accepted);
    RUN_TEST(test_comments_and_blanks_ignored);
    RUN_TEST(test_whitespace_around_equals);
    RUN_TEST(test_case_insensitive_keys);

    RUN_TEST(test_unknown_key_counted_not_fatal);
    RUN_TEST(test_malformed_line_counted);
    RUN_TEST(test_port_out_of_range_malformed);
    RUN_TEST(test_max_sessions_clamped_to_pool_cap);
    RUN_TEST(test_max_sessions_zero_rejected);

    RUN_TEST(test_empty_input_is_defaults);
    RUN_TEST(test_null_cfg_rejected);
    RUN_TEST(test_missing_newline_on_last_line);
    RUN_TEST(test_crlf_line_endings_handled);

    return UNITY_END();
}
