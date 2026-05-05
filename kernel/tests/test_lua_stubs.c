/*
 * test_lua_stubs.c — strtol / strtod regression tests.
 *
 * The PR that braced single-statement bodies in the kernel C tree
 * touched two `if/else if/else if/else` chains and one `if/else`
 * pair in `kernel/src/lua_stubs.c`:
 *
 *   - strtol: digit class chain (0-9 / a-z / A-Z / break)
 *   - strtod: integer / fraction / exponent parse chain
 *   - strtod: exponent-apply branch (multiply vs divide)
 *
 * Adding braces around a single statement is semantically a no-op
 * in C, but these are libc-replacement stubs the compiler is free
 * to optimise differently across the rewrite. This file pins their
 * observable behaviour so a future "tweak the chain" edit caught
 * by clang-tidy doesn't silently change parsing.
 *
 * Hosted in the `lua` static library (CMakeLists ~1196) which is
 * compiled WITHOUT `-mgeneral-regs-only`. That's needed so the
 * test functions can hold a `double` returned from strtod — the
 * AArch64 ABI returns floating-point values in `d0`, which a
 * `-mgeneral-regs-only` caller cannot legally read.
 *
 * Unity in this tree has no `TEST_ASSERT_EQUAL_DOUBLE`, so the
 * strtod tests use an inline tolerance helper that takes two
 * doubles, computes |a-b|, and routes the boolean result through
 * `TEST_ASSERT_TRUE_MESSAGE`. Tolerance is tight enough (1e-12)
 * to catch any real algorithmic regression while absorbing the
 * 1-2 ULP slop between strtod's iterative `*= 10.0 / *= 0.1`
 * accumulation and the compiler's one-shot literal evaluation.
 */

#include "unity.h"
#include "../include/slm_lua_stubs.h"

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Absolute value for double — pulling in math.h for fabs would force
 * the freestanding kernel build to either link libm or stub it; this
 * is one line. */
static double abs_d(double x) { return x < 0.0 ? -x : x; }

/* Tolerance comparison. Caller supplies a meaningful message so a
 * failure tells the reader which case mismatched.
 *
 * Note: declared `static inline` so the macro-expanded TEST_ASSERT
 * file/line locations in failures point at the test body, not at
 * a single helper line. */
static inline void assert_double_close(double expected, double actual,
                                       const char *msg)
{
    /* 1e-12 absolute tolerance is well below any single-ULP error
     * for the magnitudes we test (≤ 1e10) and well above any
     * accumulated rounding from strtod's iterative loops. */
    TEST_ASSERT_MESSAGE(abs_d(expected - actual) < 1e-12, msg);
}

/* ============================================================================
 * strtol — exercises the three-way digit chain at lua_stubs.c:548-562
 * ============================================================================ */

/* '0'-'9' branch — base-10 happy path. */
static void test_strtol_decimal_basic(void)
{
    TEST_ASSERT_EQUAL_INT64(0,    strtol("0",    NULL, 10));
    TEST_ASSERT_EQUAL_INT64(1,    strtol("1",    NULL, 10));
    TEST_ASSERT_EQUAL_INT64(123,  strtol("123",  NULL, 10));
    TEST_ASSERT_EQUAL_INT64(-456, strtol("-456", NULL, 10));
    TEST_ASSERT_EQUAL_INT64(789,  strtol("+789", NULL, 10));
}

/* 'a'-'z' branch — lowercase hex + base-36. */
static void test_strtol_lowercase_letters(void)
{
    TEST_ASSERT_EQUAL_INT64(255, strtol("ff", NULL, 16));   /* a-f path */
    TEST_ASSERT_EQUAL_INT64(10,  strtol("a",  NULL, 16));
    TEST_ASSERT_EQUAL_INT64(15,  strtol("f",  NULL, 16));
    TEST_ASSERT_EQUAL_INT64(35,  strtol("z",  NULL, 36));   /* full a-z path */
}

/* 'A'-'Z' branch — uppercase hex + base-36. */
static void test_strtol_uppercase_letters(void)
{
    TEST_ASSERT_EQUAL_INT64(255, strtol("FF", NULL, 16));   /* A-F path */
    TEST_ASSERT_EQUAL_INT64(10,  strtol("A",  NULL, 16));
    TEST_ASSERT_EQUAL_INT64(15,  strtol("F",  NULL, 16));
    TEST_ASSERT_EQUAL_INT64(35,  strtol("Z",  NULL, 36));   /* full A-Z path */
}

/* `else break` branch — non-digit terminates the chain.
 * Verifies endptr lands on the first un-consumed char. */
static void test_strtol_endptr_stops_at_non_digit(void)
{
    char *end = NULL;
    long  v   = strtol("42abc", &end, 10);
    TEST_ASSERT_EQUAL_INT64(42, v);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_INT8('a', *end);

    /* "9" with base 8: digit (9) >= base (8) → second-stage break. */
    v = strtol("9", &end, 8);
    TEST_ASSERT_EQUAL_INT64(0, v);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_INT8('9', *end);
}

/* `0x` prefix detection (separate from the digit chain but on the
 * same hot path). */
static void test_strtol_hex_prefix(void)
{
    TEST_ASSERT_EQUAL_INT64(255,  strtol("0xFF", NULL, 16));
    TEST_ASSERT_EQUAL_INT64(255,  strtol("0XfF", NULL, 16));
    TEST_ASSERT_EQUAL_INT64(255,  strtol("0xFF", NULL, 0));   /* base auto-detect */
    TEST_ASSERT_EQUAL_INT64(63,   strtol("077",  NULL, 0));   /* octal auto-detect */
    TEST_ASSERT_EQUAL_INT64(123,  strtol("123",  NULL, 0));   /* decimal auto-detect */
}

/* Leading whitespace skip. */
static void test_strtol_skips_whitespace(void)
{
    TEST_ASSERT_EQUAL_INT64(42, strtol("   42",   NULL, 10));
    TEST_ASSERT_EQUAL_INT64(42, strtol("\t42",    NULL, 10));
    TEST_ASSERT_EQUAL_INT64(42, strtol("\n42",    NULL, 10));
    TEST_ASSERT_EQUAL_INT64(42, strtol(" \t\n42", NULL, 10));
}

/* ============================================================================
 * strtod — exercises the integer/fraction/exponent chain at
 * lua_stubs.c:600-621 and the exponent-apply if/else at 626-632.
 * ============================================================================ */

/* Integer-only path: '0'-'9' branch in the parse chain. */
static void test_strtod_integer(void)
{
    assert_double_close(0.0,    strtod("0",     NULL), "strtod(\"0\")");
    assert_double_close(1.0,    strtod("1",     NULL), "strtod(\"1\")");
    assert_double_close(42.0,   strtod("42",    NULL), "strtod(\"42\")");
    assert_double_close(-42.0,  strtod("-42",   NULL), "strtod(\"-42\")");
    assert_double_close(1234.0, strtod("+1234", NULL), "strtod(\"+1234\")");
}

/* Fraction path: '.' transitions in_fraction, then digits accumulate
 * via the fraction branch instead of the integer branch. */
static void test_strtod_fraction(void)
{
    assert_double_close(0.5,   strtod("0.5",   NULL), "strtod(\"0.5\")");
    assert_double_close(1.5,   strtod("1.5",   NULL), "strtod(\"1.5\")");
    assert_double_close(-1.25, strtod("-1.25", NULL), "strtod(\"-1.25\")");
    assert_double_close(0.5,   strtod(".5",    NULL), "strtod(\".5\")");
    assert_double_close(3.0,   strtod("3.",    NULL), "strtod(\"3.\")");
}

/* Exponent path: 'e'/'E' branch + the apply-exponent if/else.
 *  - Positive exponent → result *= 10.0 branch.
 *  - Negative exponent → result /= 10.0 branch. */
static void test_strtod_exponent_positive(void)
{
    assert_double_close(150.0,    strtod("1.5e2",  NULL), "1.5e2");
    assert_double_close(150.0,    strtod("1.5E2",  NULL), "1.5E2");
    assert_double_close(150.0,    strtod("1.5e+2", NULL), "1.5e+2");
    assert_double_close(1.0e10,   strtod("1e10",   NULL), "1e10");
    assert_double_close(123000.0, strtod("123e3",  NULL), "123e3");
}

static void test_strtod_exponent_negative(void)
{
    assert_double_close(0.015,  strtod("1.5e-2", NULL), "1.5e-2");
    assert_double_close(0.015,  strtod("1.5E-2", NULL), "1.5E-2");
    assert_double_close(0.001,  strtod("1e-3",   NULL), "1e-3");
    assert_double_close(0.0001, strtod("1.0e-4", NULL), "1.0e-4");
}

/* `else break` path: a non-digit, non-`.`, non-`e/E` char terminates
 * parsing. Verifies endptr lands on the first un-consumed char. */
static void test_strtod_endptr_stops_at_non_digit(void)
{
    char *end = NULL;
    double v  = strtod("1.5xyz", &end);
    assert_double_close(1.5, v, "strtod(\"1.5xyz\") value");
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_INT8('x', *end);

    v = strtod("3.14e2trailing", &end);
    assert_double_close(314.0, v, "strtod(\"3.14e2trailing\") value");
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_INT8('t', *end);
}

/* ============================================================================
 * Suite registration
 * ============================================================================ */

int test_suite_lua_stubs(void)
{
    UnityBegin("strtol / strtod regression (chain rewrite)");

    RUN_TEST(test_strtol_decimal_basic);
    RUN_TEST(test_strtol_lowercase_letters);
    RUN_TEST(test_strtol_uppercase_letters);
    RUN_TEST(test_strtol_endptr_stops_at_non_digit);
    RUN_TEST(test_strtol_hex_prefix);
    RUN_TEST(test_strtol_skips_whitespace);

    RUN_TEST(test_strtod_integer);
    RUN_TEST(test_strtod_fraction);
    RUN_TEST(test_strtod_exponent_positive);
    RUN_TEST(test_strtod_exponent_negative);
    RUN_TEST(test_strtod_endptr_stops_at_non_digit);

    return UnityEnd();
}
