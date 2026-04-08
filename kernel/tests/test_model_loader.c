/*
 * test_model_loader.c - Tests for the ONNX model loader
 *
 * Calls into Rust model loader test suite and verifies C-side FFI.
 * Also includes Unity-style C tests for FFI boundary validation.
 */

#include "test_harness.h"
#include "unity.h"
#include "slm_ffi.h"
#include "uart.h"
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Unity C-side FFI boundary tests
 *
 * These tests verify that the Rust FFI functions handle NULL pointers,
 * invalid indices, and other edge cases correctly from the C side.
 * ============================================================================ */

/* Test: rust_model_load with NULL name returns -1 */
static void test_model_load_null_name(void)
{
    uint8_t dummy_data[] = { 0x01, 0x02, 0x03, 0x04 };
    int result = rust_model_load(NULL, dummy_data, sizeof(dummy_data));
    TEST_ASSERT_EQUAL_INT(-1, result);
}

/* Test: rust_model_load with NULL data returns -1 */
static void test_model_load_null_data(void)
{
    int result = rust_model_load("test_model", NULL, 100);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

/* Test: rust_model_load with 0 length returns -1 */
static void test_model_load_zero_length(void)
{
    uint8_t dummy_data[] = { 0x01, 0x02 };
    int result = rust_model_load("test_model", dummy_data, 0);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

/* Test: rust_model_get_info with invalid index returns -1 */
static void test_model_get_info_invalid_index(void)
{
    RustModelInfo info;
    int result = rust_model_get_info(999, &info);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

/* Test: rust_model_find with NULL returns -1 */
static void test_model_find_null(void)
{
    int result = rust_model_find(NULL);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

/* Test: rust_model_unload with invalid index returns -1 */
static void test_model_unload_invalid_index(void)
{
    int result = rust_model_unload(999);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

/* Test: rust_model_count after init returns 0 */
static void test_model_count_after_init(void)
{
    rust_model_loader_init();
    uint32_t count = rust_model_count();
    TEST_ASSERT_EQUAL_UINT32(0, count);
}

/* ============================================================================
 * Test Suite Runner
 * ============================================================================ */

int test_suite_model_loader(void)
{
    /*
     * Part 1: Rust-side model loader tests (protobuf parsing, ONNX parsing,
     * graph construction, registry lifecycle, error/edge cases).
     */
    int failures = rust_model_loader_test();

    /*
     * Part 2: C-side FFI boundary tests using Unity framework.
     * These verify that the Rust FFI functions correctly reject invalid
     * arguments passed from C code.
     */
    UnityBegin("Model Loader FFI Tests");

    RUN_TEST(test_model_load_null_name);
    RUN_TEST(test_model_load_null_data);
    RUN_TEST(test_model_load_zero_length);
    RUN_TEST(test_model_get_info_invalid_index);
    RUN_TEST(test_model_find_null);
    RUN_TEST(test_model_unload_invalid_index);
    RUN_TEST(test_model_count_after_init);

    failures += UnityEnd();

    return failures;
}
