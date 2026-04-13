/*
 * Eviction-Policy Tests (Phase AI-Eviction M1 / M2)
 *
 * Tests the Rust eviction subsystem via FFI:
 *   - M1: block-tracking fields populated by alloc / touch / set_*,
 *         accessed via the new getter FFI; eviction registry swap and
 *         selftest; comprehensive Rust-internal tests for the trait,
 *         registry, FirstCandidatePolicy, and score helper.
 *   - M2: stub vs real generated predictor (xgb_predict, mlp_predict)
 *         return finite values in [0, 1]; stub build returns exactly 0.5.
 *
 * The suite compiles regardless of the `ai_eviction` Cargo feature.
 * When the feature is OFF, the eviction-specific tests skip cleanly via
 * the `rust_eviction_enabled()` probe (reported as TEST_IGNORE).
 */

#include "unity.h"
#include <stdint.h>

/* ============================================================================
 * FFI declarations — test-local (matches test_model_mem.c convention; the
 * shared slm_ffi.h does not yet expose ModelHandle).
 * ============================================================================ */

typedef struct {
    uint16_t block_index;
    uint8_t  pool_id;
    uint8_t  generation;
    uint32_t _reserved;
} ModelHandle;

#define MODEL_BLOCK_SIZE (2u * 1024u * 1024u)

/* Model-memory FFI (existing). */
extern int         rust_model_mem_init(void);
extern ModelHandle rust_model_alloc_weights(size_t size);
extern ModelHandle rust_model_alloc_workspace(size_t size);
extern int         rust_model_free(ModelHandle handle);
extern ModelHandle rust_model_share(ModelHandle handle);

/* Eviction-tracking setters (M1). */
extern int rust_model_touch(ModelHandle handle);
extern int rust_model_set_metadata(
    ModelHandle handle, uint8_t model_id, int16_t layer_idx, uint8_t model_priority);
extern int rust_model_set_gpu_mapped(ModelHandle handle, int mapped);
extern int rust_model_set_dirty(ModelHandle handle, int dirty);

/* Eviction-tracking getters (M1). */
extern uint32_t rust_model_get_access_count(ModelHandle handle);
extern uint64_t rust_model_get_load_time(ModelHandle handle);
extern uint64_t rust_model_get_last_access_time(ModelHandle handle);
extern int32_t  rust_model_get_model_id(ModelHandle handle);
extern int32_t  rust_model_get_layer_idx(ModelHandle handle);
extern int32_t  rust_model_get_model_priority(ModelHandle handle);
extern int32_t  rust_model_is_gpu_mapped(ModelHandle handle);
extern int32_t  rust_model_is_dirty(ModelHandle handle);

/* Eviction registry + test runners (M1 / M2). */
extern int rust_eviction_enabled(void);
extern int rust_eviction_selftest(void);
extern int rust_eviction_run_tests(void);

/* ============================================================================
 * Helpers
 * ============================================================================ */

static int handle_is_null(ModelHandle h)
{
    return h.block_index == 0xFFFF && h.pool_id == 0xFF;
}

static ModelHandle null_handle(void)
{
    ModelHandle h = { .block_index = 0xFFFF, .pool_id = 0xFF,
                      .generation = 0, ._reserved = 0 };
    return h;
}

/* Busy-wait burning a measurable number of nanoseconds of the monotonic
 * clock. Without this between `alloc` and `touch`, two consecutive
 * get_time_ns() calls may return the same value on fast emulators.
 * A short arithmetic loop forces enough wall-clock delay to guarantee
 * a different nanosecond reading.
 */
static void spin_a_bit(void)
{
    volatile uint64_t sink = 0;
    for (volatile uint64_t i = 0; i < 100000ull; i++) {
        sink += i;
    }
    (void)sink;
}

/* ============================================================================
 * M1: alloc seeds the tracking fields
 * ============================================================================ */

static void test_alloc_seeds_tracking_fields(void)
{
    ModelHandle h = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));

    uint64_t load_time = rust_model_get_load_time(h);
    uint64_t last_acc  = rust_model_get_last_access_time(h);
    uint32_t count     = rust_model_get_access_count(h);

    TEST_ASSERT_TRUE(load_time > 0);
    TEST_ASSERT_TRUE(last_acc > 0);
    TEST_ASSERT_EQUAL_UINT32(1, count);

    /* Default identity is zero. */
    TEST_ASSERT_EQUAL_INT32(0, rust_model_get_model_id(h));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_get_layer_idx(h));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_get_model_priority(h));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_is_gpu_mapped(h));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_is_dirty(h));

    rust_model_free(h);
}

/* ============================================================================
 * M1: touch bumps access_count and updates last_access_time monotonically
 * ============================================================================ */

static void test_touch_bumps_access_count(void)
{
    ModelHandle h = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));
    TEST_ASSERT_EQUAL_UINT32(1, rust_model_get_access_count(h));

    TEST_ASSERT_EQUAL_INT(0, rust_model_touch(h));
    TEST_ASSERT_EQUAL_UINT32(2, rust_model_get_access_count(h));

    TEST_ASSERT_EQUAL_INT(0, rust_model_touch(h));
    TEST_ASSERT_EQUAL_UINT32(3, rust_model_get_access_count(h));

    rust_model_free(h);
}

static void test_touch_updates_last_access_time(void)
{
    ModelHandle h = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));

    uint64_t before = rust_model_get_last_access_time(h);
    spin_a_bit();
    TEST_ASSERT_EQUAL_INT(0, rust_model_touch(h));
    uint64_t after = rust_model_get_last_access_time(h);

    TEST_ASSERT_TRUE(after > before);

    rust_model_free(h);
}

/* ============================================================================
 * M1: set_metadata round-trips
 * ============================================================================ */

static void test_set_metadata_round_trip(void)
{
    ModelHandle h = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));

    TEST_ASSERT_EQUAL_INT(0, rust_model_set_metadata(h,
        /* model_id */ 5, /* layer_idx */ -3, /* priority */ 7));

    TEST_ASSERT_EQUAL_INT32(5,  rust_model_get_model_id(h));
    TEST_ASSERT_EQUAL_INT32(-3, rust_model_get_layer_idx(h));
    TEST_ASSERT_EQUAL_INT32(7,  rust_model_get_model_priority(h));

    /* Overwrite to confirm re-assignment works. */
    TEST_ASSERT_EQUAL_INT(0, rust_model_set_metadata(h, 11, 42, 2));
    TEST_ASSERT_EQUAL_INT32(11, rust_model_get_model_id(h));
    TEST_ASSERT_EQUAL_INT32(42, rust_model_get_layer_idx(h));
    TEST_ASSERT_EQUAL_INT32(2,  rust_model_get_model_priority(h));

    rust_model_free(h);
}

/* ============================================================================
 * M1: set_gpu_mapped / set_dirty round-trip
 * ============================================================================ */

static void test_set_gpu_mapped_round_trip(void)
{
    ModelHandle h = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_is_gpu_mapped(h));

    TEST_ASSERT_EQUAL_INT(0, rust_model_set_gpu_mapped(h, 1));
    TEST_ASSERT_EQUAL_INT32(1, rust_model_is_gpu_mapped(h));

    TEST_ASSERT_EQUAL_INT(0, rust_model_set_gpu_mapped(h, 0));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_is_gpu_mapped(h));

    rust_model_free(h);
}

static void test_set_dirty_round_trip(void)
{
    ModelHandle h = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_is_dirty(h));

    TEST_ASSERT_EQUAL_INT(0, rust_model_set_dirty(h, 1));
    TEST_ASSERT_EQUAL_INT32(1, rust_model_is_dirty(h));

    TEST_ASSERT_EQUAL_INT(0, rust_model_set_dirty(h, 0));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_is_dirty(h));

    rust_model_free(h);
}

/* ============================================================================
 * M1: invalid-handle paths return error sentinels
 * ============================================================================ */

static void test_invalid_handle_returns_errors(void)
{
    ModelHandle bad = null_handle();

    /* Setters return -1. */
    TEST_ASSERT_EQUAL_INT(-1, rust_model_touch(bad));
    TEST_ASSERT_EQUAL_INT(-1, rust_model_set_metadata(bad, 1, 1, 1));
    TEST_ASSERT_EQUAL_INT(-1, rust_model_set_gpu_mapped(bad, 1));
    TEST_ASSERT_EQUAL_INT(-1, rust_model_set_dirty(bad, 1));

    /* Getters return their sentinels. */
    TEST_ASSERT_EQUAL_UINT32(0, rust_model_get_access_count(bad));
    TEST_ASSERT_EQUAL_UINT64(0, rust_model_get_load_time(bad));
    TEST_ASSERT_EQUAL_UINT64(0, rust_model_get_last_access_time(bad));
    TEST_ASSERT_EQUAL_INT32(-1, rust_model_get_model_id(bad));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, rust_model_get_layer_idx(bad));
    TEST_ASSERT_EQUAL_INT32(-1, rust_model_get_model_priority(bad));
    TEST_ASSERT_EQUAL_INT32(-1, rust_model_is_gpu_mapped(bad));
    TEST_ASSERT_EQUAL_INT32(-1, rust_model_is_dirty(bad));
}

/* ============================================================================
 * M1: free clears tracking (the next allocation starts fresh)
 * ============================================================================ */

static void test_free_clears_tracking(void)
{
    /* Allocate, populate, and free. */
    ModelHandle h1 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h1));
    rust_model_set_metadata(h1, 9, 77, 3);
    rust_model_set_gpu_mapped(h1, 1);
    rust_model_set_dirty(h1, 1);
    rust_model_touch(h1);
    rust_model_touch(h1);
    rust_model_free(h1);

    /* Same slot, fresh allocation: tracking must be reset. */
    ModelHandle h2 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h2));

    TEST_ASSERT_EQUAL_UINT32(1, rust_model_get_access_count(h2));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_get_model_id(h2));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_get_layer_idx(h2));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_get_model_priority(h2));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_is_gpu_mapped(h2));
    TEST_ASSERT_EQUAL_INT32(0, rust_model_is_dirty(h2));

    rust_model_free(h2);
}

/* ============================================================================
 * M1: shared (ref_count > 1) blocks keep their tracking on partial release
 * ============================================================================ */

static void test_shared_block_preserves_tracking(void)
{
    ModelHandle h1 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h1));

    rust_model_set_metadata(h1, 4, 2, 1);
    rust_model_touch(h1);
    rust_model_touch(h1);
    /* access_count is now 3 (alloc + 2 touches). */
    TEST_ASSERT_EQUAL_UINT32(3, rust_model_get_access_count(h1));

    ModelHandle shared = rust_model_share(h1);
    TEST_ASSERT_FALSE(handle_is_null(shared));

    /* Release one ref — the tracking fields are still intact. */
    TEST_ASSERT_EQUAL_INT(0, rust_model_free(shared));
    TEST_ASSERT_EQUAL_UINT32(3, rust_model_get_access_count(h1));
    TEST_ASSERT_EQUAL_INT32(4, rust_model_get_model_id(h1));

    /* Final free clears everything. */
    rust_model_free(h1);
}

/* ============================================================================
 * M1/M2: selftest + comprehensive Rust-internal tests
 * ============================================================================ */

static void test_eviction_enabled_probe(void)
{
    /* The probe itself is always safe to call. It returns 1 or 0, not an
     * error. On this build we can't assert a specific value without
     * knowing the feature flag, but we can assert the result is a
     * valid boolean-shaped value. */
    int e = rust_eviction_enabled();
    TEST_ASSERT_TRUE(e == 0 || e == 1);
}

static void test_eviction_selftest_passes(void)
{
    if (!rust_eviction_enabled()) {
        /* ai_eviction feature off — selftest returns -2 (skipped). */
        TEST_ASSERT_EQUAL_INT(-2, rust_eviction_selftest());
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_selftest());
}

static void test_eviction_run_tests_passes(void)
{
    if (!rust_eviction_enabled()) {
        /* Returns 0 (no tests to run) when the feature is off. */
        TEST_ASSERT_EQUAL_INT(0, rust_eviction_run_tests());
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }
    int failures = rust_eviction_run_tests();
    TEST_ASSERT_EQUAL_INT(0, failures);
}

/* ============================================================================
 * Test Suite Runner
 * ============================================================================ */

int test_suite_eviction(void)
{
    UnityBegin("Eviction Policy Tests");

    /* Tracking-field FFI (always available; fields are always on). */
    RUN_TEST(test_alloc_seeds_tracking_fields);
    RUN_TEST(test_touch_bumps_access_count);
    RUN_TEST(test_touch_updates_last_access_time);
    RUN_TEST(test_set_metadata_round_trip);
    RUN_TEST(test_set_gpu_mapped_round_trip);
    RUN_TEST(test_set_dirty_round_trip);
    RUN_TEST(test_invalid_handle_returns_errors);
    RUN_TEST(test_free_clears_tracking);
    RUN_TEST(test_shared_block_preserves_tracking);

    /* Registry + comprehensive Rust-internal tests (skip cleanly when
     * the ai_eviction feature is disabled). */
    RUN_TEST(test_eviction_enabled_probe);
    RUN_TEST(test_eviction_selftest_passes);
    RUN_TEST(test_eviction_run_tests_passes);

    return UnityEnd();
}
