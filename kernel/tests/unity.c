/*
 * Unity Test Framework - Bare Metal Edition
 *
 * Implementation of Unity test framework for freestanding environments.
 * No libc dependencies - uses provided output functions.
 */

#include "unity.h"
#include "test_filter.h"

/* Generic timer / TSC counter, for the filtered per-iteration time. */
extern uint64_t timer_get_count(void);
extern uint64_t timer_get_frequency(void);

/* ============================================================================
 * Global State
 * ============================================================================ */

UnityState Unity = {
    .current_test = NULL,
    .current_file = NULL,
    .ignore_message = NULL,
    .current_line = 0,
    .test_count = 0,
    .test_failures = 0,
    .test_ignores = 0,
    .current_test_failed = 0,
    .current_test_ignored = 0,
};

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

void UnityBegin(const char *filename)
{
    Unity.current_file = filename;
    Unity.test_count = 0;
    Unity.test_failures = 0;
    Unity.test_ignores = 0;
    Unity.current_test_failed = 0;
    Unity.current_test_ignored = 0;
    Unity.ignore_message = NULL;

    /* Filtered runs print one line per test iteration instead. */
    if (test_filter_active()) {
        return;
    }

    unity_output_string("[TEST] ");
    unity_output_string(filename);
    unity_output_char('\n');
}

int UnityEnd(void)
{
    if (test_filter_active()) {
        return Unity.test_failures;
    }

    unity_output_string("\n----------------------------------------\n");
    unity_output_string("Tests: ");
    unity_output_number(Unity.test_count);
    unity_output_string("  Passed: ");
    unity_output_number(Unity.test_count - Unity.test_failures - Unity.test_ignores);
    unity_output_string("  Failed: ");
    unity_output_number(Unity.test_failures);
    if (Unity.test_ignores > 0) {
        unity_output_string("  Ignored: ");
        unity_output_number(Unity.test_ignores);
    }
    unity_output_char('\n');

    if (Unity.test_failures == 0) {
        unity_output_string("[PASS] All tests passed\n");
    } else {
        unity_output_string("[FAIL] ");
        unity_output_number(Unity.test_failures);
        unity_output_string(" test(s) failed\n");
    }

    return Unity.test_failures;
}

/* ============================================================================
 * Running Tests
 * ============================================================================ */

static void unity_print_result(const char *name)
{
    if (Unity.current_test_failed) {
        unity_output_string("  [FAIL] ");
    } else if (Unity.current_test_ignored) {
        unity_output_string("  [IGNORE] ");
    } else {
        unity_output_string("  [PASS] ");
    }
    unity_output_string(name);
    if (Unity.ignore_message) {
        unity_output_string(" - ");
        unity_output_string(Unity.ignore_message);
    }
}

static void unity_run_body(UnityTestFunction func, const char *name)
{
    Unity.current_test = name;
    Unity.current_test_failed = 0;
    Unity.current_test_ignored = 0;
    Unity.ignore_message = NULL;
    Unity.test_count++;
    setUp();
    func();
    tearDown();
}

static void unity_run_filtered(UnityTestFunction func, const char *name,
                               int idx)
{
    uint64_t t0 = timer_get_count();
    unity_run_body(func, name);
    uint64_t dt = timer_get_count() - t0;

    test_filter_stats_t *st = &g_test_filter_stats[idx];
    st->runs++;
    if (Unity.current_test_failed) {
        st->failed++;
    } else if (Unity.current_test_ignored) {
        st->ignored++;
    } else {
        st->passed++;
    }
    st->total_ticks += dt;
    if (st->runs == 1 || dt < st->min_ticks) {
        st->min_ticks = dt;
    }
    if (dt > st->max_ticks) {
        st->max_ticks = dt;
    }

    uint64_t freq = timer_get_frequency();
    uint64_t us = freq ? (dt * 1000000u) / freq : 0;

    unity_output_string("  [ITER ");
    unity_output_number((int64_t)g_test_filter_pass);
    unity_output_char('/');
    unity_output_number((int64_t)g_test_filter.repeat);
    unity_output_char(']');
    unity_print_result(name);
    unity_output_string(" (");
    unity_output_number((int64_t)us);
    unity_output_string(" us)\n");
}

void UnityRunTest(UnityTestFunction func, const char *name)
{
    if (test_filter_active()) {
        int idx = test_filter_index_of(&g_test_filter, name);
        if (idx >= 0) {
            unity_run_filtered(func, name, idx);
        }
        return;
    }

    unity_run_body(func, name);
    unity_print_result(name);
    unity_output_char('\n');
}

/* ============================================================================
 * Default setUp/tearDown (weak symbols - can be overridden)
 * ============================================================================ */

__attribute__((weak)) void setUp(void)
{
    /* Default: do nothing */
}

__attribute__((weak)) void tearDown(void)
{
    /* Default: do nothing */
}

/* ============================================================================
 * Failure Reporting
 * ============================================================================ */

void unity_fail(const char *file, int line, const char *msg)
{
    Unity.test_failures++;
    Unity.current_test_failed = 1;

    unity_output_string("    FAILED at ");
    unity_output_string(file);
    unity_output_char(':');
    unity_output_number(line);
    unity_output_string(" - ");
    unity_output_string(msg);
    unity_output_char('\n');
}

void unity_fail_expected_actual(const char *file, int line,
                                 int64_t expected, int64_t actual,
                                 const char *msg)
{
    Unity.test_failures++;
    Unity.current_test_failed = 1;

    unity_output_string("    FAILED at ");
    unity_output_string(file);
    unity_output_char(':');
    unity_output_number(line);
    unity_output_string("\n      Expected: ");
    unity_output_number(expected);
    unity_output_string("\n      Actual:   ");
    unity_output_number(actual);
    if (msg) {
        unity_output_string(" (");
        unity_output_string(msg);
        unity_output_char(')');
    }
    unity_output_char('\n');
}

void unity_fail_expected_actual_hex(const char *file, int line,
                                     uint64_t expected, uint64_t actual,
                                     const char *msg)
{
    Unity.test_failures++;
    Unity.current_test_failed = 1;

    unity_output_string("    FAILED at ");
    unity_output_string(file);
    unity_output_char(':');
    unity_output_number(line);
    unity_output_string("\n      Expected: 0x");
    unity_output_hex(expected);
    unity_output_string("\n      Actual:   0x");
    unity_output_hex(actual);
    if (msg) {
        unity_output_string(" (");
        unity_output_string(msg);
        unity_output_char(')');
    }
    unity_output_char('\n');
}
