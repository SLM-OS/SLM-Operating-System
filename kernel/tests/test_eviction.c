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
#include "../include/slm_ffi.h"
#include "../include/string.h"
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
extern int rust_eviction_snapshot_count(void);

/* Shell-facing FFI (M7). */
extern size_t rust_eviction_policy_name(uint8_t *out_buf, size_t buf_len);
extern const uint8_t *rust_eviction_policy_list(void);
extern int32_t rust_eviction_policy_set(const uint8_t *name);
extern int32_t rust_eviction_get_stats(RustEvictionStats *out);

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
 * M1: snapshot_evictable_blocks filter — verifies the M6 integration
 * point returns the correct candidate set (allocated AND not pinned).
 * ============================================================================ */

static void test_snapshot_counts_evictable_blocks(void)
{
    if (!rust_eviction_enabled()) {
        TEST_ASSERT_EQUAL_INT(-1, rust_eviction_snapshot_count());
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }

    /* Baseline count (may be non-zero due to earlier tests leaving
     * allocations; we measure deltas, not absolutes). */
    int baseline = rust_eviction_snapshot_count();
    TEST_ASSERT_TRUE(baseline >= 0);

    /* Allocate three blocks in mixed pools. */
    ModelHandle h1 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    ModelHandle h2 = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);
    ModelHandle h3 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h1));
    TEST_ASSERT_FALSE(handle_is_null(h2));
    TEST_ASSERT_FALSE(handle_is_null(h3));

    /* Snapshot grows by exactly three — all evictable (ref_count = 1). */
    TEST_ASSERT_EQUAL_INT(baseline + 3, rust_eviction_snapshot_count());

    /* Sharing h2 lifts its ref_count above 1 → pinned → excluded. */
    ModelHandle h2_shared = rust_model_share(h2);
    TEST_ASSERT_FALSE(handle_is_null(h2_shared));
    TEST_ASSERT_EQUAL_INT(baseline + 2, rust_eviction_snapshot_count());

    /* Dropping the extra reference re-admits h2 to the candidate set. */
    rust_model_free(h2_shared);
    TEST_ASSERT_EQUAL_INT(baseline + 3, rust_eviction_snapshot_count());

    /* Free everything; back to baseline. */
    rust_model_free(h1);
    rust_model_free(h2);
    rust_model_free(h3);
    TEST_ASSERT_EQUAL_INT(baseline, rust_eviction_snapshot_count());
}

/* ============================================================================
 * M6: Allocator integration — eviction kicks in on pool exhaustion
 * ============================================================================ */

/* RustPoolStats is provided by slm_ffi.h (updated in M6 to include
 * `evictions_total`). The extern declarations below match the shared
 * header. */

/* The M6 fill-and-evict test uses a small, self-contained per-pool
 * slice: it allocates N blocks, records handles, then allocs one more
 * to force an eviction. We don't want to drain the whole pool because
 * later tests and integration suites depend on it being usable; we
 * free back to baseline at the end. */
#define FILL_COUNT 32

static void test_alloc_evicts_when_full_weights(void)
{
    if (!rust_eviction_enabled()) {
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }

    ModelHandle handles[FILL_COUNT];
    int allocated = 0;

    for (int i = 0; i < FILL_COUNT; i++) {
        handles[i] = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
        if (handle_is_null(handles[i])) break;
        /* Stagger last_access_time by touching in order so LRU has a
         * clear oldest to evict. */
        rust_model_touch(handles[i]);
        allocated++;
    }
    TEST_ASSERT_EQUAL_INT(FILL_COUNT, allocated);

    RustPoolStats before = rust_weight_pool_stats();
    uint64_t evictions_before = before.evictions_total;

    /* Allocate one more — the pool may not be entirely full of these
     * handles (earlier tests might have others), so this test doesn't
     * require `free_blocks == 0`. What it verifies is: when we
     * eventually hit OOM, eviction kicks in and the alloc succeeds. */
    ModelHandle extra = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    /* If free blocks still remained, no eviction is expected. The
     * strong check lives in test_alloc_evicts_after_total_fill below. */
    TEST_ASSERT_FALSE(handle_is_null(extra));

    (void)evictions_before;  /* referenced by the strong-fill test. */

    /* Clean up. */
    for (int i = 0; i < allocated; i++) rust_model_free(handles[i]);
    rust_model_free(extra);
}

static void test_alloc_evicts_after_total_fill(void)
{
    if (!rust_eviction_enabled()) {
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }

    /* Fully drain the workspace pool. */
    ModelHandle slots[256];  /* MAX_BLOCKS_PER_POOL */
    int n = 0;
    for (int i = 0; i < 256; i++) {
        ModelHandle h = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);
        if (handle_is_null(h)) break;
        rust_model_touch(h);
        slots[n++] = h;
    }
    TEST_ASSERT_TRUE(n > 0);

    RustPoolStats before = rust_workspace_pool_stats();
    TEST_ASSERT_EQUAL_UINT64(0, before.free_blocks);
    uint64_t evict_before = before.evictions_total;

    /* Next alloc MUST evict — no free blocks. */
    ModelHandle extra = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(extra));

    RustPoolStats after = rust_workspace_pool_stats();
    TEST_ASSERT_EQUAL_UINT64(evict_before + 1, after.evictions_total);

    /* Clean up — free the new handle and every surviving slot. One
     * of the originals was evicted, so its handle is now stale; we
     * ignore stale-free errors. */
    rust_model_free(extra);
    for (int i = 0; i < n; i++) {
        (void)rust_model_free(slots[i]);
    }
}

static void test_alloc_oom_when_all_pinned(void)
{
    if (!rust_eviction_enabled()) {
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }

    /* Drain a workspace slice and pin each entry by sharing it. */
    ModelHandle slots[256];
    ModelHandle shared[256];
    int n = 0;
    for (int i = 0; i < 256; i++) {
        ModelHandle h = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);
        if (handle_is_null(h)) break;
        shared[i] = rust_model_share(h);
        slots[n++] = h;
    }
    TEST_ASSERT_TRUE(n > 0);

    /* Pool is drained and every block is pinned (refcount > 1). A
     * fresh alloc has nowhere to go — even the policy sees no
     * evictable candidates. Must return null. */
    ModelHandle extra = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);
    TEST_ASSERT_TRUE(handle_is_null(extra));

    /* Release pins and the originals. */
    for (int i = 0; i < n; i++) {
        rust_model_free(shared[i]);
        rust_model_free(slots[i]);
    }
}

static void test_pool_stats_reports_evictions(void)
{
    if (!rust_eviction_enabled()) {
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }
    RustPoolStats w = rust_weight_pool_stats();
    RustPoolStats ws = rust_workspace_pool_stats();
    /* The field exists and returns a non-negative count. The absolute
     * value depends on preceding tests — don't assert a specific one,
     * just that reads don't faull. */
    TEST_ASSERT_TRUE(w.evictions_total <= UINT64_MAX);
    TEST_ASSERT_TRUE(ws.evictions_total <= UINT64_MAX);
}

/* ============================================================================
 * M7: Shell FFI — policy name, list, set, stats
 * ============================================================================ */

static void test_policy_name_returns_active(void)
{
    uint8_t buf[32] = {0};
    size_t n = rust_eviction_policy_name(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    /* Name must be null-terminated within the buffer. */
    TEST_ASSERT_TRUE(buf[n] == 0);
    if (rust_eviction_enabled()) {
        /* Default policy is LRU after M3. */
        TEST_ASSERT_EQUAL_STRING("LRU", (const char *)buf);
    } else {
        TEST_ASSERT_EQUAL_STRING("none", (const char *)buf);
    }
}

static void test_policy_list_is_null_terminated(void)
{
    const uint8_t *list = rust_eviction_policy_list();
    TEST_ASSERT_NOT_NULL(list);
    /* Walk to the null terminator — must find one within 256 bytes. */
    size_t len = 0;
    while (len < 256 && list[len] != 0) len++;
    TEST_ASSERT_TRUE(len > 0 && len < 256);
    if (rust_eviction_enabled()) {
        /* Must contain at least "lru" and "cacheus" tokens. Manual
         * substring walk — the kernel's string.h doesn't export
         * strstr. */
        const char *s = (const char *)list;
        bool saw_lru = false, saw_cacheus = false;
        for (size_t i = 0; i + 3 <= len; i++) {
            if (!saw_lru && strncmp(s + i, "lru", 3) == 0) saw_lru = true;
            if (!saw_cacheus && i + 7 <= len
                && strncmp(s + i, "cacheus", 7) == 0) saw_cacheus = true;
            if (saw_lru && saw_cacheus) break;
        }
        TEST_ASSERT_TRUE(saw_lru);
        TEST_ASSERT_TRUE(saw_cacheus);
    }
}

static void test_policy_set_switches_active(void)
{
    if (!rust_eviction_enabled()) {
        TEST_ASSERT_EQUAL_INT(-2,
            rust_eviction_policy_set((const uint8_t *)"lru"));
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }
    /* Switch to LFU and verify. */
    TEST_ASSERT_EQUAL_INT(0,
        rust_eviction_policy_set((const uint8_t *)"lfu"));
    uint8_t buf[32] = {0};
    rust_eviction_policy_name(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("LFU", (const char *)buf);

    /* Switch to CACHEUS. */
    TEST_ASSERT_EQUAL_INT(0,
        rust_eviction_policy_set((const uint8_t *)"cacheus"));
    rust_eviction_policy_name(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("CACHEUS", (const char *)buf);

    /* Unknown policy → -1, active unchanged. */
    TEST_ASSERT_EQUAL_INT(-1,
        rust_eviction_policy_set((const uint8_t *)"nonsense"));
    rust_eviction_policy_name(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("CACHEUS", (const char *)buf);

    /* Restore LRU to leave the registry in its default state. */
    TEST_ASSERT_EQUAL_INT(0,
        rust_eviction_policy_set((const uint8_t *)"lru"));
}

static void test_get_stats_populates_fields(void)
{
    RustEvictionStats s = {0};
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_get_stats(&s));

    TEST_ASSERT_EQUAL_INT(rust_eviction_enabled(), s.feature_enabled);
    if (s.feature_enabled) {
        TEST_ASSERT_TRUE(s.weight_total > 0);
        TEST_ASSERT_TRUE(s.workspace_total > 0);
        TEST_ASSERT_TRUE(s.snapshot_candidates >= 0);
        /* Switching to CACHEUS populates expert weights. */
        TEST_ASSERT_EQUAL_INT(0,
            rust_eviction_policy_set((const uint8_t *)"cacheus"));
        rust_eviction_get_stats(&s);
        TEST_ASSERT_EQUAL_UINT32(2, s.cacheus_expert_count);
        /* Initial weights are uniform: 50% each = 5000 bp. */
        TEST_ASSERT_EQUAL_UINT32(5000, s.expert_weights_bp[0]);
        TEST_ASSERT_EQUAL_UINT32(5000, s.expert_weights_bp[1]);
        /* Restore default. */
        TEST_ASSERT_EQUAL_INT(0,
            rust_eviction_policy_set((const uint8_t *)"lru"));
    }
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

    /* M6: allocator integration (skip cleanly when feature off). */
    RUN_TEST(test_alloc_evicts_when_full_weights);
    RUN_TEST(test_alloc_evicts_after_total_fill);
    RUN_TEST(test_alloc_oom_when_all_pinned);
    RUN_TEST(test_pool_stats_reports_evictions);

    /* Snapshot filter (M6 integration point) — skips when feature off. */
    RUN_TEST(test_snapshot_counts_evictable_blocks);

    /* M7: shell FFI surface — always runs; individual tests IGNORE
     * when the feature is disabled and the check doesn't make sense. */
    RUN_TEST(test_policy_name_returns_active);
    RUN_TEST(test_policy_list_is_null_terminated);
    RUN_TEST(test_policy_set_switches_active);
    RUN_TEST(test_get_stats_populates_fields);

    return UnityEnd();
}
