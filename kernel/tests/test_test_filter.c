/*
 * Boot-test filter parser tests (docs/testing.md "Running selected tests").
 *
 * The parser runs before any test on a filtered boot, from the kernel
 * command line or the build-time default, so a parse slip either runs
 * the wrong tests or none. These pin the accepted syntax, the rejected
 * inputs and the generated name -> suite index.
 */

#include "unity.h"
#include "test_filter.h"
#include <stdint.h>

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static bool seq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void test_filter_parses_single_name(void)
{
    test_filter_t f = { 0 };
    const char *s = "test_preempt_flag_follows_migrated_task";
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_OK, test_filter_parse_names(&f, s, slen(s)));
    TEST_ASSERT_EQUAL_UINT32(1, f.count);
    TEST_ASSERT_TRUE(seq(f.names[0], s));
}

static void test_filter_parses_comma_list_in_order(void)
{
    test_filter_t f = { 0 };
    const char *s = "test_checkerboard_fragmentation,test_exhaustion_recovery,t3";
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_OK, test_filter_parse_names(&f, s, slen(s)));
    TEST_ASSERT_EQUAL_UINT32(3, f.count);
    TEST_ASSERT_TRUE(seq(f.names[0], "test_checkerboard_fragmentation"));
    TEST_ASSERT_TRUE(seq(f.names[1], "test_exhaustion_recovery"));
    TEST_ASSERT_TRUE(seq(f.names[2], "t3"));
}

static void test_filter_rejects_empty_elements(void)
{
    test_filter_t f = { 0 };
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_EMPTY, test_filter_parse_names(&f, "", 0));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_EMPTY, test_filter_parse_names(&f, "a,", 2));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_EMPTY, test_filter_parse_names(&f, ",a", 2));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_EMPTY, test_filter_parse_names(&f, "a,,b", 4));
}

static void test_filter_rejects_bad_characters(void)
{
    test_filter_t f = { 0 };
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_CHAR, test_filter_parse_names(&f, "a b", 3));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_CHAR, test_filter_parse_names(&f, "test_*", 6));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_CHAR, test_filter_parse_names(&f, "a;b", 3));
}

static void test_filter_limits_count_and_length(void)
{
    test_filter_t f = { 0 };
    /* 16 names fit, 17 do not. */
    const char *sixteen = "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p";
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_OK, test_filter_parse_names(&f, sixteen, slen(sixteen)));
    TEST_ASSERT_EQUAL_UINT32(16, f.count);
    const char *seventeen = "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q";
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_TOO_MANY,
                          test_filter_parse_names(&f, seventeen, slen(seventeen)));

    char longname[TEST_FILTER_NAME_MAX + 1];
    for (int i = 0; i < TEST_FILTER_NAME_MAX; i++) {
        longname[i] = 'x';
    }
    longname[TEST_FILTER_NAME_MAX] = '\0';
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_TOO_LONG,
                          test_filter_parse_names(&f, longname, TEST_FILTER_NAME_MAX));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_OK,
                          test_filter_parse_names(&f, longname, TEST_FILTER_NAME_MAX - 1));
}

static void test_filter_parses_repeat_bounds(void)
{
    uint32_t r = 0;
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_OK, test_filter_parse_repeat("50", 2, &r));
    TEST_ASSERT_EQUAL_UINT32(50, r);
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_OK, test_filter_parse_repeat("1000000", 7, &r));
    TEST_ASSERT_EQUAL_UINT32(1000000, r);
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_REPEAT, test_filter_parse_repeat("0", 1, &r));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_REPEAT, test_filter_parse_repeat("1000001", 7, &r));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_REPEAT, test_filter_parse_repeat("12345678", 8, &r));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_REPEAT, test_filter_parse_repeat("5x", 2, &r));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_REPEAT, test_filter_parse_repeat("-5", 2, &r));
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_REPEAT, test_filter_parse_repeat("", 0, &r));
}

/* The Pi 5 firmware prepends its own arguments; the keys may sit anywhere. */
static void test_filter_reads_cmdline_tokens(void)
{
    test_filter_t f = { 0 };
    const char *cl = "reboot=w coherent_pool=1M slmos.test=test_a,test_b "
                     "console=ttyAMA0 slmos.test_repeat=200\n";
    TEST_ASSERT_EQUAL_INT(1, test_filter_from_cmdline(&f, cl));
    TEST_ASSERT_TRUE(f.active);
    TEST_ASSERT_EQUAL_UINT32(2, f.count);
    TEST_ASSERT_EQUAL_UINT32(200, f.repeat);
    TEST_ASSERT_TRUE(seq(f.names[1], "test_b"));
    TEST_ASSERT_TRUE(seq(f.source, "cmdline"));
}

static void test_filter_cmdline_defaults_repeat_to_one(void)
{
    test_filter_t f = { 0 };
    TEST_ASSERT_EQUAL_INT(1, test_filter_from_cmdline(&f, "slmos.test=test_a"));
    TEST_ASSERT_EQUAL_UINT32(1, f.repeat);
}

/* slmos.test_repeat= must not be read as slmos.test=, and a key that only
 * starts with "slmos.test" is not the filter. */
static void test_filter_cmdline_key_is_exact(void)
{
    test_filter_t f = { 0 };
    TEST_ASSERT_EQUAL_INT(0, test_filter_from_cmdline(&f, "slmos.test_repeat=5"));
    TEST_ASSERT_FALSE(f.active);
    TEST_ASSERT_EQUAL_INT(0, test_filter_from_cmdline(&f, "slmos.testx=a xslmos.test=a"));
    TEST_ASSERT_FALSE(f.active);
    TEST_ASSERT_EQUAL_INT(0, test_filter_from_cmdline(&f, ""));
}

static void test_filter_cmdline_errors_leave_filter_inactive(void)
{
    test_filter_t f = { 0 };
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_REPEAT,
                          test_filter_from_cmdline(&f, "slmos.test=a slmos.test_repeat=0"));
    TEST_ASSERT_FALSE(f.active);
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_ERR_EMPTY, test_filter_from_cmdline(&f, "slmos.test="));
    TEST_ASSERT_FALSE(f.active);
}

static void test_filter_match_is_exact(void)
{
    test_filter_t f = { 0 };
    const char *s = "test_task,test_task_lifecycle";
    TEST_ASSERT_EQUAL_INT(TEST_FILTER_OK, test_filter_parse_names(&f, s, slen(s)));
    TEST_ASSERT_EQUAL_INT(0, test_filter_index_of(&f, "test_task"));
    TEST_ASSERT_EQUAL_INT(1, test_filter_index_of(&f, "test_task_lifecycle"));
    TEST_ASSERT_EQUAL_INT(-1, test_filter_index_of(&f, "test_tas"));
    TEST_ASSERT_EQUAL_INT(-1, test_filter_index_of(&f, "test_task_"));
    TEST_ASSERT_EQUAL_INT(-1, test_filter_index_of(&f, "TEST_TASK"));
}

/* The generated index maps the run-5 targets to their suites. */
static void test_filter_index_maps_known_tests(void)
{
    if (test_index_size() == 0) {
        TEST_IGNORE_MESSAGE("no generated index in this image");
        return;
    }
    uint32_t c = 0;
    const char *s = test_index_next_suite("test_preempt_flag_follows_migrated_task", &c);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(seq(s, "test_suite_scheduler"));
    c = 0;
    s = test_index_next_suite("test_exhaustion_recovery", &c);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(seq(s, "test_suite_pmm"));
    c = 0;
    TEST_ASSERT_NULL(test_index_next_suite("test_no_such_test_anywhere", &c));
    /* This suite's own tests are indexed too. */
    c = 0;
    s = test_index_next_suite("test_filter_match_is_exact", &c);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(seq(s, "test_suite_test_filter"));
}

int test_suite_test_filter(void)
{
    UnityBegin("Test Filter Tests");

    RUN_TEST(test_filter_parses_single_name);
    RUN_TEST(test_filter_parses_comma_list_in_order);
    RUN_TEST(test_filter_rejects_empty_elements);
    RUN_TEST(test_filter_rejects_bad_characters);
    RUN_TEST(test_filter_limits_count_and_length);
    RUN_TEST(test_filter_parses_repeat_bounds);
    RUN_TEST(test_filter_reads_cmdline_tokens);
    RUN_TEST(test_filter_cmdline_defaults_repeat_to_one);
    RUN_TEST(test_filter_cmdline_key_is_exact);
    RUN_TEST(test_filter_cmdline_errors_leave_filter_inactive);
    RUN_TEST(test_filter_match_is_exact);
    RUN_TEST(test_filter_index_maps_known_tests);

    return UnityEnd();
}
