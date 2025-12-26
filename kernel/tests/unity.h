/*
 * Unity Test Framework - Bare Metal Edition
 *
 * A slim implementation of the Unity testing API for freestanding environments.
 * API-compatible with ThrowTheSwitch Unity (https://github.com/ThrowTheSwitch/Unity)
 *
 * MIT License - Compatible with original Unity license
 */

#ifndef UNITY_H
#define UNITY_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Output function - must be provided by the test harness */
extern void unity_output_char(char c);
extern void unity_output_string(const char *s);
extern void unity_output_number(int64_t n);
extern void unity_output_hex(uint64_t n);

/* ============================================================================
 * Test State
 * ============================================================================ */

typedef struct {
    const char *current_test;
    const char *current_file;
    int current_line;
    int test_count;
    int test_failures;
    int test_ignores;
    int current_test_failed;
} UnityState;

extern UnityState Unity;

/* ============================================================================
 * Test Lifecycle
 * ============================================================================ */

/* Called before each test */
void setUp(void);

/* Called after each test */
void tearDown(void);

/* Initialize Unity - call once at start */
void UnityBegin(const char *filename);

/* Finalize Unity - returns number of failures */
int UnityEnd(void);

/* Convenience macros for test suite begin/end */
#define UNITY_BEGIN() UnityBegin(__FILE__)
#define UNITY_END() UnityEnd()

/* ============================================================================
 * Running Tests
 * ============================================================================ */

#define RUN_TEST(func) \
    do { \
        Unity.current_test = #func; \
        Unity.current_test_failed = 0; \
        Unity.test_count++; \
        setUp(); \
        func(); \
        tearDown(); \
        if (Unity.current_test_failed) { \
            unity_output_string("  [FAIL] "); \
        } else { \
            unity_output_string("  [PASS] "); \
        } \
        unity_output_string(#func); \
        unity_output_char('\n'); \
    } while (0)

/* ============================================================================
 * Core Assertion Internals
 * ============================================================================ */

void unity_fail(const char *file, int line, const char *msg);
void unity_fail_expected_actual(const char *file, int line,
                                 int64_t expected, int64_t actual,
                                 const char *msg);
void unity_fail_expected_actual_hex(const char *file, int line,
                                     uint64_t expected, uint64_t actual,
                                     const char *msg);

/* ============================================================================
 * Basic Assertions
 * ============================================================================ */

#define TEST_FAIL() \
    do { unity_fail(__FILE__, __LINE__, "TEST_FAIL"); return; } while (0)

#define TEST_FAIL_MESSAGE(msg) \
    do { unity_fail(__FILE__, __LINE__, msg); return; } while (0)

#define TEST_PASS() \
    do { return; } while (0)

#define TEST_IGNORE() \
    do { Unity.test_ignores++; Unity.current_test_failed = 0; return; } while (0)

#define TEST_IGNORE_MESSAGE(msg) \
    do { Unity.test_ignores++; Unity.current_test_failed = 0; return; } while (0)

/* ============================================================================
 * Boolean Assertions
 * ============================================================================ */

#define TEST_ASSERT(condition) \
    do { if (!(condition)) { unity_fail(__FILE__, __LINE__, #condition); return; } } while (0)

#define TEST_ASSERT_TRUE(condition) \
    TEST_ASSERT(condition)

#define TEST_ASSERT_FALSE(condition) \
    TEST_ASSERT(!(condition))

#define TEST_ASSERT_MESSAGE(condition, msg) \
    do { if (!(condition)) { unity_fail(__FILE__, __LINE__, msg); return; } } while (0)

/* ============================================================================
 * Pointer Assertions
 * ============================================================================ */

#define TEST_ASSERT_NULL(ptr) \
    do { if ((ptr) != NULL) { unity_fail(__FILE__, __LINE__, #ptr " expected NULL"); return; } } while (0)

#define TEST_ASSERT_NOT_NULL(ptr) \
    do { if ((ptr) == NULL) { unity_fail(__FILE__, __LINE__, #ptr " expected not NULL"); return; } } while (0)

/* ============================================================================
 * Integer Equality Assertions
 * ============================================================================ */

#define TEST_ASSERT_EQUAL(expected, actual) \
    TEST_ASSERT_EQUAL_INT64((int64_t)(expected), (int64_t)(actual))

#define TEST_ASSERT_EQUAL_INT(expected, actual) \
    TEST_ASSERT_EQUAL_INT64((int64_t)(expected), (int64_t)(actual))

#define TEST_ASSERT_EQUAL_INT8(expected, actual) \
    do { \
        if ((int8_t)(expected) != (int8_t)(actual)) { \
            unity_fail_expected_actual(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_INT16(expected, actual) \
    do { \
        if ((int16_t)(expected) != (int16_t)(actual)) { \
            unity_fail_expected_actual(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_INT32(expected, actual) \
    do { \
        if ((int32_t)(expected) != (int32_t)(actual)) { \
            unity_fail_expected_actual(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_INT64(expected, actual) \
    do { \
        if ((int64_t)(expected) != (int64_t)(actual)) { \
            unity_fail_expected_actual(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

/* ============================================================================
 * Unsigned Integer Equality Assertions
 * ============================================================================ */

#define TEST_ASSERT_EQUAL_UINT(expected, actual) \
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(expected), (uint64_t)(actual))

#define TEST_ASSERT_EQUAL_UINT8(expected, actual) \
    do { \
        if ((uint8_t)(expected) != (uint8_t)(actual)) { \
            unity_fail_expected_actual(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_UINT16(expected, actual) \
    do { \
        if ((uint16_t)(expected) != (uint16_t)(actual)) { \
            unity_fail_expected_actual(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_UINT32(expected, actual) \
    do { \
        if ((uint32_t)(expected) != (uint32_t)(actual)) { \
            unity_fail_expected_actual(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_UINT64(expected, actual) \
    do { \
        if ((uint64_t)(expected) != (uint64_t)(actual)) { \
            unity_fail_expected_actual(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

/* ============================================================================
 * Hex Assertions (display as hex on failure)
 * ============================================================================ */

#define TEST_ASSERT_EQUAL_HEX(expected, actual) \
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(expected), (uint64_t)(actual))

#define TEST_ASSERT_EQUAL_HEX8(expected, actual) \
    do { \
        if ((uint8_t)(expected) != (uint8_t)(actual)) { \
            unity_fail_expected_actual_hex(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_HEX16(expected, actual) \
    do { \
        if ((uint16_t)(expected) != (uint16_t)(actual)) { \
            unity_fail_expected_actual_hex(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_HEX32(expected, actual) \
    do { \
        if ((uint32_t)(expected) != (uint32_t)(actual)) { \
            unity_fail_expected_actual_hex(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_HEX64(expected, actual) \
    do { \
        if ((uint64_t)(expected) != (uint64_t)(actual)) { \
            unity_fail_expected_actual_hex(__FILE__, __LINE__, (expected), (actual), NULL); \
            return; \
        } \
    } while (0)

/* ============================================================================
 * Pointer Equality
 * ============================================================================ */

#define TEST_ASSERT_EQUAL_PTR(expected, actual) \
    do { \
        if ((void *)(expected) != (void *)(actual)) { \
            unity_fail_expected_actual_hex(__FILE__, __LINE__, \
                (uint64_t)(uintptr_t)(expected), \
                (uint64_t)(uintptr_t)(actual), "pointers"); \
            return; \
        } \
    } while (0)

/* ============================================================================
 * Not-Equal Assertions
 * ============================================================================ */

#define TEST_ASSERT_NOT_EQUAL(expected, actual) \
    do { \
        if ((expected) == (actual)) { \
            unity_fail(__FILE__, __LINE__, #expected " should not equal " #actual); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_NOT_EQUAL_INT(expected, actual) \
    TEST_ASSERT_NOT_EQUAL(expected, actual)

/* ============================================================================
 * Range Assertions
 * ============================================================================ */

#define TEST_ASSERT_GREATER_THAN(threshold, actual) \
    do { \
        if (!((actual) > (threshold))) { \
            unity_fail(__FILE__, __LINE__, #actual " not greater than " #threshold); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_LESS_THAN(threshold, actual) \
    do { \
        if (!((actual) < (threshold))) { \
            unity_fail(__FILE__, __LINE__, #actual " not less than " #threshold); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_GREATER_OR_EQUAL(threshold, actual) \
    do { \
        if (!((actual) >= (threshold))) { \
            unity_fail(__FILE__, __LINE__, #actual " not >= " #threshold); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_LESS_OR_EQUAL(threshold, actual) \
    do { \
        if (!((actual) <= (threshold))) { \
            unity_fail(__FILE__, __LINE__, #actual " not <= " #threshold); \
            return; \
        } \
    } while (0)

/* Range assertions with custom message */
#define TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(threshold, actual, msg) \
    do { \
        if (!((actual) >= (threshold))) { \
            unity_fail(__FILE__, __LINE__, msg); \
            return; \
        } \
    } while (0)

/* ============================================================================
 * String Assertions
 * ============================================================================ */

/* Helper for inline string comparison */
static inline int unity_strcmp(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return (a != b) ? 1 : 0;
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

#define TEST_ASSERT_EQUAL_STRING(expected, actual) \
    do { \
        if (unity_strcmp((expected), (actual)) != 0) { \
            unity_fail(__FILE__, __LINE__, "strings not equal"); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, actual, msg) \
    do { \
        if (unity_strcmp((expected), (actual)) != 0) { \
            unity_fail(__FILE__, __LINE__, msg); \
            return; \
        } \
    } while (0)

/* ============================================================================
 * Pointer Assertions with Message
 * ============================================================================ */

#define TEST_ASSERT_NOT_NULL_MESSAGE(ptr, msg) \
    do { if ((ptr) == NULL) { unity_fail(__FILE__, __LINE__, msg); return; } } while (0)

/* ============================================================================
 * Memory Assertions
 * ============================================================================ */

#define TEST_ASSERT_EQUAL_MEMORY(expected, actual, len) \
    do { \
        const uint8_t *_e = (const uint8_t *)(expected); \
        const uint8_t *_a = (const uint8_t *)(actual); \
        for (size_t _i = 0; _i < (len); _i++) { \
            if (_e[_i] != _a[_i]) { \
                unity_fail(__FILE__, __LINE__, "memory mismatch"); \
                return; \
            } \
        } \
    } while (0)

/* ============================================================================
 * Bit Assertions
 * ============================================================================ */

#define TEST_ASSERT_BITS(mask, expected, actual) \
    do { \
        if (((expected) & (mask)) != ((actual) & (mask))) { \
            unity_fail(__FILE__, __LINE__, "bits mismatch"); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_BIT_HIGH(bit, actual) \
    do { \
        if (!(((actual) >> (bit)) & 1)) { \
            unity_fail(__FILE__, __LINE__, "expected bit high"); \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_BIT_LOW(bit, actual) \
    do { \
        if (((actual) >> (bit)) & 1) { \
            unity_fail(__FILE__, __LINE__, "expected bit low"); \
            return; \
        } \
    } while (0)

#endif /* UNITY_H */
