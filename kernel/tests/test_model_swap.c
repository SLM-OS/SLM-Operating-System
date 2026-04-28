/*
 * test_model_swap.c — coverage for `rust_model_swap` and
 * `registry::swap_model`.
 *
 * The bulk of the coverage lives Rust-side (`rust_model_swap_test`)
 * because the runtime owns both the embedded MNIST ONNX bytes and
 * the WeightLease type that pins the safety invariant we need to
 * exercise. This C-side suite calls into the Rust self-test and
 * adds a few FFI boundary checks on the C side that the Rust test
 * can't easily express.
 */

#include "test_harness.h"
#include "unity.h"
#include "slm_ffi.h"
#include "uart.h"
#include "string.h"
#include <stddef.h>
#include <stdint.h>

/*
 * C-side FFI argument validation. The Rust self-test covers these
 * branches too, but pinning them from C makes a regression in the
 * extern "C" boundary (signature drift, ABI mismatch) fail the
 * suite even if the Rust-side path stops being reachable.
 */

static void test_swap_null_name(void)
{
    uint8_t dummy[] = { 0x01, 0x02 };
    int rc = rust_model_swap(0, NULL, dummy, sizeof(dummy));
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

static void test_swap_null_data(void)
{
    int rc = rust_model_swap(0, "x", NULL, 100);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

static void test_swap_zero_len(void)
{
    uint8_t dummy[] = { 0x01, 0x02 };
    int rc = rust_model_swap(0, "x", dummy, 0);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/*
 * After a fresh registry init, every slot is empty. Swapping any of
 * them must return -2 (InvalidIndex), regardless of whether the
 * payload would otherwise parse. We use `rust_model_loader_init` to
 * drain — `rust_model_loader_test` and prior tests may have left
 * models loaded.
 *
 * WARNING: this test mutates global registry state (drains every
 * slot) and never restores it. Tests that run after
 * `test_suite_model_swap` and depend on a preloaded model must load
 * it themselves. As of 2026-04-28 the only post-swap suite that
 * touches the registry is `test_suite_components_m5`, and its
 * `test_infer_classify_success_path` already calls
 * `rust_model_load_builtin_mnist` first, so it's unaffected. If
 * test ordering ever changes such that a "model is preloaded"
 * assumption arrives later, save/restore in this test instead of
 * leaving the registry empty.
 */
static void test_swap_empty_slot_after_init(void)
{
    rust_model_loader_init();
    /* Drain anything left over from earlier suites. */
    for (uint32_t i = 0; i < 8; i++) {
        (void)rust_model_unload(i);
    }

    /* Use a small dummy buffer — the empty-slot pre-flight check
     * runs BEFORE the parser, so we don't need real ONNX bytes. */
    uint8_t dummy[8] = { 0 };
    int rc = rust_model_swap(0, "x", dummy, sizeof(dummy));
    TEST_ASSERT_EQUAL_INT(-2, rc);
}

/*
 * Out-of-range slot id must also map to -2. The MAX_MODELS bound is
 * 8 (compile-time constant in the registry); any index >= that is
 * rejected before parse.
 */
static void test_swap_out_of_range_slot(void)
{
    uint8_t dummy[8] = { 0 };
    int rc = rust_model_swap(99, "x", dummy, sizeof(dummy));
    TEST_ASSERT_EQUAL_INT(-2, rc);
}

int test_suite_model_swap(void)
{
    /*
     * Part 1: Rust-side self-test. This is the load-bearing part of
     * the suite — it exercises the actual swap machinery (parse,
     * alloc, atomic replace, refcount lease).
     */
    int failures = rust_model_swap_test();

    /*
     * Part 2: C-side FFI boundary tests.
     */
    UnityBegin("Model Swap FFI Tests");

    RUN_TEST(test_swap_null_name);
    RUN_TEST(test_swap_null_data);
    RUN_TEST(test_swap_zero_len);
    RUN_TEST(test_swap_empty_slot_after_init);
    RUN_TEST(test_swap_out_of_range_slot);

    failures += UnityEnd();

    return failures;
}
