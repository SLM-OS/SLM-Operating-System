/*
 * test_model_loader.c - Tests for the ONNX model loader
 *
 * Calls into Rust model loader test suite and verifies C-side FFI.
 * Also includes Unity-style C tests for FFI boundary validation.
 */

#include "test_harness.h"
#include "unity.h"
#include "component.h"
#include "slm_ffi.h"
#include "uart.h"
#include "string.h"
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

/* ============================================================================
 * Inference engine tests
 * ============================================================================ */

/*
 * Note: These tests use uint32_t arrays cast to float* to avoid
 * floating-point operations in -mgeneral-regs-only kernel code.
 * The Rust FFI functions only check for NULL pointers at this level.
 */

static void test_infer_null_input(void)
{
    uint32_t output[10];
    int result = rust_infer(0, NULL, 784, (void *)output, 10);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

static void test_infer_null_output(void)
{
    uint32_t input[4];
    memset(input, 0, sizeof(input));
    int result = rust_infer(0, (const void *)input, 4, NULL, 10);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

static void test_infer_invalid_model(void)
{
    uint32_t input[4];
    uint32_t output[4];
    memset(input, 0, sizeof(input));
    memset(output, 0, sizeof(output));
    int result = rust_infer(99, (const void *)input, 4, (void *)output, 4);
    TEST_ASSERT_TRUE(result < 0);
}

/*
 * rust_infer_and_print error/success-path coverage.
 *
 * The function was reworked alongside rust_infer_buf_and_print to
 * (a) lift its OUTPUT array off `static mut` so two concurrent shell
 * sessions can't race it, (b) reject `result == 0` from the engine
 * with -3 instead of a misleading "predicted class 0", and (c) cap
 * the argmax loop at output.len() so a future engine drift can't
 * panic-abort the kernel via an OOB index. Tests below pin the
 * invalid-index and success-path contracts; the result==0 / OOB
 * branches are dormant today (engine never produces those shapes
 * for MNIST) but the cap + early-return are belt-and-braces.
 */
static void test_infer_and_print_invalid_model(void)
{
    int result = rust_infer_and_print(99);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

static void test_infer_and_print_success_path(void)
{
    /* Load built-in MNIST so model index returned by load is valid.
     * rust_infer_and_print uses an internal zero-input buffer so we
     * don't need to prepare features here. */
    int idx = rust_model_load_builtin_mnist();
    TEST_ASSERT_MESSAGE(idx >= 0, "MNIST load must succeed");

    int result = rust_infer_and_print((uint32_t)idx);
    TEST_ASSERT_EQUAL_INT(0, result);

    /* Clean up so subsequent tests start with a fresh registry. */
    (void)rust_model_unload((uint32_t)idx);
}

/*
 * rust_infer_buf_and_print contract checks. Same -1/-2/-3 mapping the
 * shell `model infer-file` command uses; pin each input-validation
 * branch.
 */
static void test_infer_buf_and_print_null_input(void)
{
    /* SAFETY: even with a non-existent model index, the NULL/0
     * checks fire before the registry lookup, so -2 is expected
     * before any model state is touched. */
    int result = rust_infer_buf_and_print(0, NULL, 784);
    TEST_ASSERT_EQUAL_INT(-2, result);
}

static void test_infer_buf_and_print_zero_floats(void)
{
    uint32_t buf[1] = { 0 };
    int result = rust_infer_buf_and_print(0, (const float *)buf, 0);
    TEST_ASSERT_EQUAL_INT(-2, result);
}

static void test_infer_buf_and_print_invalid_model(void)
{
    uint32_t buf[1] = { 0 };
    int result = rust_infer_buf_and_print(99, (const float *)buf, 1);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

int test_suite_inference(void)
{
    /* Part 1: Rust-side inference tests */
    int failures = rust_inference_test();

    /* Part 2: C-side FFI boundary tests */
    UnityBegin("Inference FFI Tests");

    RUN_TEST(test_infer_null_input);
    RUN_TEST(test_infer_null_output);
    RUN_TEST(test_infer_invalid_model);
    RUN_TEST(test_infer_and_print_invalid_model);
    RUN_TEST(test_infer_and_print_success_path);
    RUN_TEST(test_infer_buf_and_print_null_input);
    RUN_TEST(test_infer_buf_and_print_zero_floats);
    RUN_TEST(test_infer_buf_and_print_invalid_model);

    failures += UnityEnd();

    return failures;
}

/* ============================================================================
 * GPU compute integration tests
 * ============================================================================ */

static void test_slm_gpu_available_returns(void)
{
    int result = slm_gpu_available();
    TEST_ASSERT_TRUE(result == 0 || result == 1);
}

static void test_slm_gpu_get_info_fills_struct(void)
{
    RustGpuInfo info;
    memset(&info, 0xFF, sizeof(info));
    int result = slm_gpu_get_info(&info);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_TRUE(info.name[0] != 0xFF);
}

static void test_slm_gpu_get_info_null_returns_error(void)
{
    int result = slm_gpu_get_info(NULL);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

int test_suite_gpu_compute(void)
{
    /* Part 1: Rust-side GPU compute tests */
    int failures = rust_gpu_compute_test();

    /* Part 2: C-side FFI tests */
    UnityBegin("GPU Compute FFI Tests");

    RUN_TEST(test_slm_gpu_available_returns);
    RUN_TEST(test_slm_gpu_get_info_fills_struct);
    RUN_TEST(test_slm_gpu_get_info_null_returns_error);

    failures += UnityEnd();

    return failures;
}

/* ============================================================================
 * Component integration tests (M5)
 * ============================================================================ */

static void test_infer_classify_invalid_model(void)
{
    int result = rust_infer_classify(99);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

/*
 * Test hot-swap of components with subscription preservation.
 *
 * Starts sensor_monitor, swaps it with a new sensor_monitor instance,
 * verifies the swap returns a valid component index (subscriptions
 * are preserved internally by component_hot_swap).
 */
extern int component_run(const char *name);
extern int component_hot_swap(const char *old_name, const char *new_name);
extern void yield(void);

static void cleanup_all_components(void)
{
    for (uint32_t i = 0; i < COMPONENT_MAX_COUNT; i++) {
        component_unregister(i);
    }
}

static void test_component_hot_swap(void)
{
    cleanup_all_components();

    /* Start sensor_monitor */
    int idx = component_run("sensor_monitor");
    TEST_ASSERT_TRUE(idx >= 0);

    /* Give it a tick to start and subscribe */
    yield();

    /* Hot-swap sensor_monitor with itself (new instance) */
    int new_idx = component_hot_swap("sensor_monitor", "sensor_monitor");
    TEST_ASSERT_TRUE(new_idx >= 0);
    /* New index may differ from old — both are valid */

    /* Give new instance a tick to run */
    yield();

    cleanup_all_components();
}

int test_suite_components_m5(void)
{
    /* Part 1: Rust-side component tests */
    int failures = rust_component_test();

    /* Part 2: C-side FFI tests */
    UnityBegin("Component FFI Tests");

    RUN_TEST(test_infer_classify_invalid_model);
    RUN_TEST(test_component_hot_swap);

    failures += UnityEnd();

    return failures;
}
