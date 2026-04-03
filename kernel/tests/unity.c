/*
 * Unity Test Framework - Bare Metal Edition
 *
 * Implementation of Unity test framework for freestanding environments.
 * No libc dependencies - uses provided output functions.
 */

#include "unity.h"

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

    unity_output_string("[TEST] ");
    unity_output_string(filename);
    unity_output_char('\n');
}

int UnityEnd(void)
{
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
