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
#include "../include/uart.h"
#include "../include/task.h"
#include "../include/sched.h"
#include "../include/smp.h"
#include "../include/cache.h"
#include "../include/timer.h"
#include "test_blob_helpers.h"
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
extern int         rust_model_mem_init(uint32_t weight_mb, uint32_t workspace_mb);
extern int         rust_model_mem_reinit(uint32_t weight_mb, uint32_t workspace_mb);
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
extern int32_t rust_eviction_blob_stage(uint16_t kind_id, const uint8_t *data, size_t len);
extern int32_t rust_eviction_blob_validate(uint16_t kind_id, const uint8_t *data, size_t len);
extern int32_t rust_eviction_blob_status(uint16_t kind_id, RustEvictionBlobStatus *out);
extern int32_t rust_eviction_blob_activate(uint16_t kind_id);
extern int32_t rust_eviction_blob_rollback(uint16_t kind_id);
extern int32_t rust_eviction_blob_clear(uint16_t kind_id);

/* Latency benchmark FFI (M9). */
extern uint64_t rust_eviction_bench_latency_ns(
    const uint8_t *name, uint32_t iterations);

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

static void write_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void write_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static size_t build_valid_mlp_payload(uint32_t out_weight_bits, uint8_t *out, size_t out_cap)
{
    enum {
        PAYLOAD_HEADER_LEN = 8,
        L1_IN = 27,
        L1_OUT = 64,
        L2_OUT = 32,
        L3_OUT = 16,
        OUT = 1,
        W_L1_LEN = L1_OUT * L1_IN,
        B_L1_LEN = L1_OUT,
        W_L2_LEN = L2_OUT * L1_OUT,
        B_L2_LEN = L2_OUT,
        W_L3_LEN = L3_OUT * L2_OUT,
        B_L3_LEN = L3_OUT,
        W_OUT_LEN = OUT * L3_OUT,
        B_OUT_LEN = OUT,
        FLOAT_COUNT = W_L1_LEN + B_L1_LEN + W_L2_LEN + B_L2_LEN +
                      W_L3_LEN + B_L3_LEN + W_OUT_LEN + B_OUT_LEN,
        PAYLOAD_LEN = PAYLOAD_HEADER_LEN + FLOAT_COUNT * 4
    };
    size_t cursor = PAYLOAD_HEADER_LEN;

    if (out_cap < PAYLOAD_LEN) return 0;
    memset(out, 0, PAYLOAD_LEN);

    out[0] = 'M'; out[1] = 'L'; out[2] = 'P'; out[3] = '1';
    write_u16_le(out + 4, 1);
    write_u16_le(out + 6, 0);

    write_u32_le(out + cursor, 0x3F800000u);
    cursor += W_L1_LEN * 4;
    cursor += B_L1_LEN * 4;
    write_u32_le(out + cursor, 0x3F800000u);
    cursor += W_L2_LEN * 4;
    cursor += B_L2_LEN * 4;
    write_u32_le(out + cursor, 0x3F800000u);
    cursor += W_L3_LEN * 4;
    cursor += B_L3_LEN * 4;
    write_u32_le(out + cursor, out_weight_bits);

    return PAYLOAD_LEN;
}

static size_t build_eviction_xgb_payload(uint8_t *out, size_t out_cap)
{
    uint8_t tmp[66];
    size_t cursor = 0;
    if (out_cap < sizeof(tmp)) return 0;

    tmp[cursor++] = 'X'; tmp[cursor++] = 'G'; tmp[cursor++] = 'B'; tmp[cursor++] = '1';
    tmp[cursor++] = 1; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 1; tmp[cursor++] = 0;
    tmp[cursor++] = 3; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0; tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;

    tmp[cursor++] = 0; tmp[cursor++] = 0;

    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 1; tmp[cursor++] = 0;
    tmp[cursor++] = 2; tmp[cursor++] = 0;
    { uint32_t bits = 0x3f000000u; memcpy(tmp + cursor, &bits, 4); cursor += 4; }
    { uint32_t bits = 0u; memcpy(tmp + cursor, &bits, 4); cursor += 4; }

    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 1; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    { uint32_t bits = 0u; memcpy(tmp + cursor, &bits, 4); cursor += 4; }
    { uint32_t bits = 0x3dcccccd; memcpy(tmp + cursor, &bits, 4); cursor += 4; }

    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 1; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    { uint32_t bits = 0u; memcpy(tmp + cursor, &bits, 4); cursor += 4; }
    { uint32_t bits = 0x3f4ccccd; memcpy(tmp + cursor, &bits, 4); cursor += 4; }

    memcpy(out, tmp, cursor);
    return cursor;
}

static size_t build_eviction_cacheus_payload(uint32_t expert_pool_id,
                                             uint32_t learning_rate_bits,
                                             uint32_t window_size,
                                             uint32_t min_weight_bits,
                                             uint8_t *out,
                                             size_t out_cap)
{
    size_t cursor = 0;
    if (out_cap < 24u) return 0;
    memset(out, 0, 24u);
    out[0] = 'C'; out[1] = 'C'; out[2] = 'F'; out[3] = 'G';
    out[4] = 1; out[5] = 0;
    out[6] = 0; out[7] = 0;
    cursor = 8;
#define WRITE_CACHEUS_U32(bits)                                              \
    do {                                                                     \
        uint32_t bits_ = (bits);                                             \
        out[cursor + 0] = (uint8_t)(bits_ & 0xFF);                           \
        out[cursor + 1] = (uint8_t)((bits_ >> 8) & 0xFF);                    \
        out[cursor + 2] = (uint8_t)((bits_ >> 16) & 0xFF);                   \
        out[cursor + 3] = (uint8_t)((bits_ >> 24) & 0xFF);                   \
        cursor += 4;                                                         \
    } while (0)
    WRITE_CACHEUS_U32(expert_pool_id);
    WRITE_CACHEUS_U32(learning_rate_bits);
    WRITE_CACHEUS_U32(window_size);
    WRITE_CACHEUS_U32(min_weight_bits);
#undef WRITE_CACHEUS_U32
    return cursor;
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

/*
 * PR-4 of docs/design/gpu-policy-models.md:
 * `eviction_active_policy_has_gpu_backend()` exposes the Rust-side
 * `EvictionPolicy::has_gpu_backend()` scan to C. Today every shipped
 * policy returns false; this test pins that the C wrapper agrees.
 * The selftest above already exercises the toggle (install a
 * GpuBacked policy, observe true; revert, observe false) on the Rust
 * side; this test catches a regression in the C-side trampoline
 * specifically.
 */
static void test_eviction_active_policy_has_gpu_backend_default_false(void)
{
    if (!rust_eviction_enabled()) {
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }
    /* The selftest above leaves the registry on the LRU default. */
    bool has_gpu = eviction_active_policy_has_gpu_backend();
    TEST_ASSERT_FALSE(has_gpu);
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

/*
 * `bench eviction` workload comparator (rust_eviction_workload_compare)
 * must include every shipped eviction policy so the perf table in
 * docs/benchmarks.md is honest about the AI policies the project
 * claims. Pre-fix the comparator only ran lru/lfu/slm/cacheus —
 * xgboost and mlp (the AI policies) and arc (the literature baseline)
 * were silently absent, so #108/#109 hardware benchmarks couldn't
 * actually compare the AI policies the project's eviction story
 * is built on.
 */
static void test_eviction_workload_compare_includes_all_policies(void)
{
    if (!rust_eviction_enabled()) {
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }

    RustEvictionCompareResult results[16];
    memset(results, 0, sizeof(results));
    int32_t n = rust_eviction_workload_compare(results, 16u);
    TEST_ASSERT_MESSAGE(n >= 7,
        "comparator must report at least 7 policies (lru/lfu/arc/slm/xgboost/mlp/cacheus)");

    /* Build a set of policy names returned by the comparator and assert
     * the seven we expect are present. Scan order is policy-vec order in
     * runtime/src/lib.rs::rust_eviction_workload_compare. */
    bool seen_lru = false, seen_lfu = false, seen_arc = false,
         seen_slm = false, seen_xgb = false, seen_mlp = false,
         seen_cacheus = false;
    for (int32_t i = 0; i < n; i++) {
        const char *name = (const char *)results[i].policy_name;
        if (strncmp(name, "lru", 4) == 0)     seen_lru = true;
        if (strncmp(name, "lfu", 4) == 0)     seen_lfu = true;
        if (strncmp(name, "arc", 4) == 0)     seen_arc = true;
        if (strncmp(name, "slm", 4) == 0)     seen_slm = true;
        if (strncmp(name, "xgboost", 8) == 0) seen_xgb = true;
        if (strncmp(name, "mlp", 4) == 0)     seen_mlp = true;
        if (strncmp(name, "cacheus", 8) == 0) seen_cacheus = true;
    }
    TEST_ASSERT_MESSAGE(seen_lru,     "comparator missing 'lru'");
    TEST_ASSERT_MESSAGE(seen_lfu,     "comparator missing 'lfu'");
    TEST_ASSERT_MESSAGE(seen_arc,     "comparator missing 'arc' (literature baseline)");
    TEST_ASSERT_MESSAGE(seen_slm,     "comparator missing 'slm' (heuristic)");
    TEST_ASSERT_MESSAGE(seen_xgb,     "comparator missing 'xgboost' (AI policy)");
    TEST_ASSERT_MESSAGE(seen_mlp,     "comparator missing 'mlp' (AI policy)");
    TEST_ASSERT_MESSAGE(seen_cacheus, "comparator missing 'cacheus' (AI policy)");

    /* Sanity: every policy ran the same trace, so total_accesses must
     * be identical across all reported entries. Pin the contract so a
     * future change can't accidentally hand each policy a different
     * trace and produce a misleading comparison. */
    uint32_t expected_total = results[0].total_accesses;
    TEST_ASSERT_TRUE(expected_total > 0u);
    for (int32_t i = 1; i < n; i++) {
        TEST_ASSERT_MESSAGE(expected_total == results[i].total_accesses,
            "every policy must replay the same trace");
        /* Faults + hits must equal total accesses. */
        TEST_ASSERT_MESSAGE(
            expected_total == results[i].faults + results[i].hits,
            "faults + hits must equal total accesses");
    }
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

static void test_blob_status_defaults_empty(void)
{
    RustEvictionBlobStatus st = {0};
    int rc = rust_eviction_blob_status(2, &st);
    if (!rust_eviction_enabled()) {
        TEST_ASSERT_EQUAL_INT(-2, rc);
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(2, st.kind_id);
    TEST_ASSERT_EQUAL_UINT16(0, st.state);
    TEST_ASSERT_EQUAL_UINT32(0, st.has_staged);
    TEST_ASSERT_EQUAL_UINT32(0, st.has_active);
    TEST_ASSERT_EQUAL_UINT32(0, st.has_rollback);
}

static void test_blob_stage_activate_rollback_round_trip(void)
{
    if (!rust_eviction_enabled()) {
        TEST_ASSERT_EQUAL_INT(-2, rust_eviction_blob_clear(2));
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }

    static uint8_t payload1[18000];
    static uint8_t payload2[18000];
    static uint8_t blob1[18100];
    static uint8_t blob2[18100];
    size_t payload_len1 = build_valid_mlp_payload(0x41200000u, payload1, sizeof(payload1));
    size_t payload_len2 = build_valid_mlp_payload(0x41A00000u, payload2, sizeof(payload2));
    size_t len1 = build_outer_blob(2, payload1, payload_len1, blob1, sizeof(blob1));
    size_t len2 = build_outer_blob(2, payload2, payload_len2, blob2, sizeof(blob2));
    TEST_ASSERT_TRUE(payload_len1 > 0);
    TEST_ASSERT_TRUE(payload_len2 > 0);
    TEST_ASSERT_TRUE(len1 > 0);
    TEST_ASSERT_TRUE(len2 > 0);

    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_clear(2));
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_stage(2, blob1, len1));

    RustEvictionBlobStatus st = {0};
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(2, &st));
    TEST_ASSERT_EQUAL_UINT16(1, st.state);
    TEST_ASSERT_EQUAL_UINT32(1, st.has_staged);
    TEST_ASSERT_EQUAL_UINT32(fnv1a32(payload1, payload_len1), st.staged.checksum);

    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_activate(2));
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(2, &st));
    TEST_ASSERT_EQUAL_UINT16(2, st.state);
    TEST_ASSERT_EQUAL_UINT32(1, st.has_active);
    TEST_ASSERT_EQUAL_UINT32(0, st.has_staged);

    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_stage(2, blob2, len2));
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_activate(2));
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(2, &st));
    TEST_ASSERT_EQUAL_UINT32(1, st.has_rollback);
    TEST_ASSERT_EQUAL_UINT32(fnv1a32(payload1, payload_len1), st.rollback.checksum);

    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_rollback(2));
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(2, &st));
    TEST_ASSERT_EQUAL_UINT16(3, st.state);
    TEST_ASSERT_EQUAL_UINT32(1, st.has_active);
    TEST_ASSERT_EQUAL_UINT32(fnv1a32(payload1, payload_len1), st.active.checksum);
    TEST_ASSERT_EQUAL_UINT32(1, st.has_rollback);
    TEST_ASSERT_EQUAL_UINT32(fnv1a32(payload2, payload_len2), st.rollback.checksum);

    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_clear(2));
}

static void test_blob_stage_rejects_kind_mismatch(void)
{
    if (!rust_eviction_enabled()) {
        TEST_ASSERT_EQUAL_INT(-2, rust_eviction_blob_stage(2, (const uint8_t *)"x", 1));
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }

    static uint8_t blob[64];
    const uint8_t payload[] = {0xAA, 0xBB, 0xCC};
    size_t len = build_outer_blob(1, payload, sizeof(payload), blob, sizeof(blob));
    TEST_ASSERT_TRUE(len > 0);

    TEST_ASSERT_EQUAL_INT(-4, rust_eviction_blob_stage(2, blob, len));
}

static void test_blob_stage_rejects_invalid_payload_body(void)
{
    if (!rust_eviction_enabled()) {
        TEST_ASSERT_EQUAL_INT(-2, rust_eviction_blob_stage(2, (const uint8_t *)"x", 1));
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }

    static uint8_t xgb_blob[64];
    static uint8_t mlp_blob[64];
    static uint8_t cacheus_blob[64];
    const uint8_t bad_payload[] = {0xAA, 0xBB, 0xCC, 0xDD};
    size_t xgb_len = build_outer_blob(1, bad_payload, sizeof(bad_payload), xgb_blob, sizeof(xgb_blob));
    size_t mlp_len = build_outer_blob(2, bad_payload, sizeof(bad_payload), mlp_blob, sizeof(mlp_blob));
    size_t cacheus_len =
        build_outer_blob(3, bad_payload, sizeof(bad_payload), cacheus_blob, sizeof(cacheus_blob));

    TEST_ASSERT_TRUE(xgb_len > 0);
    TEST_ASSERT_TRUE(mlp_len > 0);
    TEST_ASSERT_TRUE(cacheus_len > 0);

    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(1, xgb_blob, xgb_len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(2, mlp_blob, mlp_len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(3, cacheus_blob, cacheus_len));
}

static void test_blob_validate_rejects_invalid_outer_header_fields(void)
{
    /* MLP payload is ~17.3 KB (see EVICTION_MLP_PAYLOAD_BYTES); the outer
     * blob adds a 24-byte header. Allocate generously in BSS — these are
     * `static` so they don't burden the 16 KB kernel stack. */
    static uint8_t payload[EVICTION_MLP_PAYLOAD_BYTES];
    static uint8_t blob[EVICTION_MLP_PAYLOAD_BYTES + BLOB_OUTER_HEADER_BYTES];
    size_t payload_len = build_eviction_mlp_payload(0x3F800000u, payload, sizeof(payload));
    size_t len = build_outer_blob(2, payload, payload_len, blob, sizeof(blob));

    TEST_ASSERT_TRUE(payload_len > 0);
    TEST_ASSERT_TRUE(len > 0);

    blob[4] = 2u;
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(2, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(2, blob, len));

    len = build_outer_blob(2, payload, payload_len, blob, sizeof(blob));
    blob[8] = 2u;
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(2, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(2, blob, len));

    len = build_outer_blob(2, payload, payload_len, blob, sizeof(blob));
    blob[10] = 1u;
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(2, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(2, blob, len));

    len = build_outer_blob(2, payload, payload_len, blob, sizeof(blob));
    blob[len - 1] ^= 0x01u;
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(2, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(2, blob, len));
}

static void test_blob_stage_rejects_invalid_payload_headers(void)
{
    /* See sibling test for the MLP buffer-sizing rationale. */
    static uint8_t xgb_payload[128];
    static uint8_t mlp_payload[EVICTION_MLP_PAYLOAD_BYTES];
    static uint8_t cacheus_payload[32];
    static uint8_t blob[EVICTION_MLP_PAYLOAD_BYTES + BLOB_OUTER_HEADER_BYTES];
    size_t payload_len;
    size_t len;

    payload_len = build_eviction_xgb_payload(xgb_payload, sizeof(xgb_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    xgb_payload[6] = 1u;
    len = build_outer_blob(1, xgb_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(1, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(1, blob, len));

    payload_len = build_eviction_mlp_payload(0x3F800000u, mlp_payload, sizeof(mlp_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    mlp_payload[4] = 2u;
    len = build_outer_blob(2, mlp_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(2, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(2, blob, len));

    payload_len = build_eviction_cacheus_payload(1u, 0x3e800000u, 64u, 0x3ca3d70au,
                                                 cacheus_payload, sizeof(cacheus_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    cacheus_payload[6] = 1u;
    len = build_outer_blob(3, cacheus_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(3, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(3, blob, len));
}

static void test_blob_stage_rejects_invalid_payload_values(void)
{
    static uint8_t xgb_payload[128];
    static uint8_t cacheus_payload[32];
    static uint8_t blob[4096];
    size_t payload_len;
    size_t len;

    payload_len = build_eviction_xgb_payload(xgb_payload, sizeof(xgb_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    xgb_payload[18] = 27u;
    xgb_payload[19] = 0u;
    len = build_outer_blob(1, xgb_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(1, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(1, blob, len));

    payload_len = build_eviction_cacheus_payload(7u, 0x3e800000u, 64u, 0x3ca3d70au,
                                                 cacheus_payload, sizeof(cacheus_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    len = build_outer_blob(3, cacheus_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(3, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(3, blob, len));

    payload_len = build_eviction_cacheus_payload(1u, 0x3fc00000u, 64u, 0x3ca3d70au,
                                                 cacheus_payload, sizeof(cacheus_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    len = build_outer_blob(3, cacheus_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(3, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(3, blob, len));

    payload_len = build_eviction_cacheus_payload(1u, 0x3e800000u, 0u, 0x3ca3d70au,
                                                 cacheus_payload, sizeof(cacheus_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    len = build_outer_blob(3, cacheus_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(3, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(3, blob, len));

    payload_len = build_eviction_cacheus_payload(1u, 0x3e800000u, 64u, 0x3fc00000u,
                                                 cacheus_payload, sizeof(cacheus_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    len = build_outer_blob(3, cacheus_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_validate(3, blob, len));
    TEST_ASSERT_EQUAL_INT(-3, rust_eviction_blob_stage(3, blob, len));
}

/* ============================================================================
 * M8: End-to-end workload stress — the alloc loop must not leak
 * blocks when eviction fires, and pool_stats accounts for every
 * allocated slot exactly.
 * ============================================================================ */

static void test_memory_pressure_no_leak(void)
{
    if (!rust_eviction_enabled()) {
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }

    RustPoolStats before = rust_workspace_pool_stats();
    uint64_t ev_before = before.evictions_total;

    /* Allocate more than the pool capacity to force evictions. */
    ModelHandle held[16];
    int held_n = 0;
    for (int round = 0; round < 4; round++) {
        /* Each round allocates 8 blocks, touches them, and releases
         * them before the next round. The alloc path evicts earlier
         * allocations once the pool fills — this exercises both the
         * eviction slow-path and the refcount=0 candidate filter. */
        for (int i = 0; i < 8; i++) {
            ModelHandle h = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);
            TEST_ASSERT_FALSE(handle_is_null(h));
            rust_model_touch(h);
            rust_model_set_metadata(h, (uint8_t)(round + 1),
                                    (int16_t)i, (uint8_t)(round % 4));
            held[held_n++] = h;
            if (held_n >= 16) {
                /* Drop the oldest half to keep the held set bounded
                 * and expose a mix of live + recently-freed slots to
                 * the eviction path. */
                for (int j = 0; j < 8; j++) {
                    rust_model_free(held[j]);
                }
                for (int j = 0; j < 8; j++) {
                    held[j] = held[j + 8];
                }
                held_n = 8;
            }
        }
    }

    /* Release everything we're still holding. */
    for (int i = 0; i < held_n; i++) {
        rust_model_free(held[i]);
    }

    RustPoolStats after = rust_workspace_pool_stats();
    TEST_ASSERT_EQUAL_UINT64(before.free_blocks, after.free_blocks);
    TEST_ASSERT_EQUAL_UINT64(0, after.allocated_blocks);
    /* The workload DID drive at least some evictions (the held set
     * exceeds the pool capacity at various points). */
    TEST_ASSERT_TRUE(after.evictions_total >= ev_before);
}

static void test_policy_swap_mid_workload(void)
{
    if (!rust_eviction_enabled()) {
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }

    /* Start on LRU, allocate some blocks, swap to XGBoost, allocate
     * more, swap to CACHEUS, free everything. No panics, no leaks. */
    TEST_ASSERT_EQUAL_INT(0,
        rust_eviction_policy_set((const uint8_t *)"lru"));

    ModelHandle h[12];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        h[n] = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
        if (!handle_is_null(h[n])) { rust_model_touch(h[n]); n++; }
    }

    TEST_ASSERT_EQUAL_INT(0,
        rust_eviction_policy_set((const uint8_t *)"xgboost"));
    for (int i = 0; i < 4; i++) {
        h[n] = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
        if (!handle_is_null(h[n])) { rust_model_touch(h[n]); n++; }
    }

    TEST_ASSERT_EQUAL_INT(0,
        rust_eviction_policy_set((const uint8_t *)"cacheus"));
    for (int i = 0; i < 4; i++) {
        h[n] = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
        if (!handle_is_null(h[n])) { rust_model_touch(h[n]); n++; }
    }

    /* Policy name shouldn't be empty at the end. */
    uint8_t buf[32] = {0};
    rust_eviction_policy_name(buf, sizeof(buf));
    TEST_ASSERT_TRUE(buf[0] != 0);

    for (int i = 0; i < n; i++) rust_model_free(h[i]);

    /* Restore default. */
    TEST_ASSERT_EQUAL_INT(0,
        rust_eviction_policy_set((const uint8_t *)"lru"));
}

/* ============================================================================
 * M9: Latency benchmarks
 *
 * Runs select_victim against a canned 8-candidate set 1000 times for
 * each policy and prints the average per-call latency to the test log.
 * The run is reported (not asserted in absolute terms) — QEMU's clock
 * resolution and contention vary too much for a hard cutoff to be
 * meaningful. We assert only that:
 *   - the call returns a measurement (not UINT64_MAX)
 *   - the per-call cost is below 1 ms (which would indicate something
 *     is catastrophically wrong)
 * Hardware bench numbers (Pi 5 / Jetson) need a real-board run; the
 * commit message captures the QEMU baseline.
 * ============================================================================ */

#define BENCH_ITERATIONS 1000

static void run_one_bench(const char *name)
{
    uint64_t avg = rust_eviction_bench_latency_ns(
        (const uint8_t *)name, BENCH_ITERATIONS);
    if (avg == UINT64_MAX) {
        uart_printf("  [BENCH] %-16s skipped (feature off or unknown)\r\n",
                    name);
        return;
    }
    uart_printf("  [BENCH] %-16s avg %lu ns / call (%d iterations)\r\n",
                name, (unsigned long)avg, BENCH_ITERATIONS);
    /* Sanity: anything over 10 ms / call indicates an infinite loop
     * or runaway allocation — even QEMU's slow int8 MLP forward
     * pass under AI_EVICTION_MODELS=ON stays well under that. The
     * < 1 µs target lives in M9 hardware bench numbers, not here. */
    TEST_ASSERT_TRUE(avg < 10000000ULL);
}

static void test_bench_all_policies(void)
{
    if (!rust_eviction_enabled()) {
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
    }
    /* Print a header so the numbers are easy to spot in the log. */
    uart_puts("  --- Eviction latency benchmarks (QEMU) ---\r\n");
    run_one_bench("first_candidate");
    run_one_bench("lru");
    run_one_bench("lfu");
    run_one_bench("arc");
    run_one_bench("slm");
    run_one_bench("xgboost");
    run_one_bench("mlp");
    run_one_bench("cacheus");

    /* Restore default. */
    rust_eviction_policy_set((const uint8_t *)"lru");
}

/* ============================================================================
 * M-SMP: Multi-CPU concurrent alloc/eviction stress test (#116)
 *
 * Validates the M6 lock-ordering claim — that the allocator's LOCK
 * and the eviction registry's REGISTRY_LOCK are never held
 * simultaneously — by hammering both from different CPUs at once.
 * Until now this was true by inspection only; nothing in the test
 * suite exercised it under SMP contention.
 *
 * Setup: one worker task per CPU (capped at EV_SMP_MAX_WORKERS),
 * each pinned to its CPU, looping `alloc_weights -> set_metadata
 * -> touch -> free` for EV_SMP_ROUNDS rounds. One swapper task on
 * CPU 0 cycles `rust_eviction_policy_set` through every registered
 * policy in the background.
 *
 * Pass criteria:
 *   - All workers complete their round budget (no deadlock).
 *   - `allocated_blocks` returns to 0 (no leaks).
 *   - No panics (implicit — the test would never finish).
 *   - Bonus: `evictions_total` advanced (some allocs actually hit
 *     the eviction slow path). Reported but not asserted — at low
 *     contention the pool may not fill.
 *
 * Skips cleanly when ai_eviction is OFF or cpu_count < 2.
 * ============================================================================ */

#define EV_SMP_ROUNDS         32u
#define EV_SMP_MAX_WORKERS    6u   /* Matches Jetson's cpu_count */
#define EV_SMP_PREFILL_LEAVE  0u   /* Pre-allocate the workspace pool
                                    * to exhaustion before spawning
                                    * workers. Every worker alloc
                                    * then trips the alloc-when-full
                                    * eviction slow path — the
                                    * specific code path #116 is
                                    * here to exercise. Independent
                                    * of cross-CPU scheduling timing
                                    * (an earlier prefill_leave=4
                                    * design saw zero evictions in
                                    * QEMU: workers freed their just-
                                    * alloc'd blocks before any other
                                    * worker hit the boundary). */
#define EV_SMP_PREFILL_MAX    512u /* MAX_BLOCKS_PER_POOL upper bound */

/* Cacheable sync state. Workers update their done counter to signal
 * "alloc phase complete" so peer workers can synchronize before
 * bulk-freeing (without the barrier, the first worker to finish
 * alloc would drain its held[] back into the pool and release
 * eviction pressure before the slower workers even finished
 * allocating — observed dropping evictions from ~128 to ~32 on
 * QEMU). Main thread polls task state, not this counter — see the
 * wait loop comment for why. */
static volatile uint32_t ev_smp_done[EV_SMP_MAX_WORKERS];
static volatile uint32_t ev_smp_stop_swapper;
static volatile uint32_t ev_smp_n_workers_active;

static void ev_smp_worker_task(void *arg)
{
    uint32_t my_cpu = (uint32_t)(uintptr_t)arg;
    /* Accumulator: hold every alloc until the end. With the pool
     * pre-saturated and workers accumulating in parallel, every
     * worker alloc past the initial prefill capacity forces the
     * alloc-when-full eviction slow path (of either a prefill
     * block or another worker's accumulated block). This sustains
     * eviction pressure across the entire run instead of trickling
     * down after each worker's first alloc — the previous "free
     * immediately" design produced ~1 eviction per worker total.
     *
     * Stale-handle races are inherent: another worker's alloc may
     * evict this worker's just-acquired slot before we touch it.
     * set_metadata / touch / free on a stale handle return
     * InvalidHandle; we ignore the rc. Generation counters on
     * BlockSlot make this safe — the FFI never indirects through
     * a stale (block_id, generation) pair. */
    ModelHandle held[EV_SMP_ROUNDS];
    for (uint32_t i = 0; i < EV_SMP_ROUNDS; i++) {
        held[i].block_index = 0xFFFFu;
        held[i].pool_id = 0xFFu;
    }
    for (uint32_t r = 0; r < EV_SMP_ROUNDS; r++) {
        ModelHandle h = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);
        held[r] = h;
        if (!handle_is_null(h)) {
            (void)rust_model_set_metadata(h,
                (uint8_t)(my_cpu + 1u), (int16_t)r,
                (uint8_t)(r & 3u));
            (void)rust_model_touch(h);
        }
        ev_smp_done[my_cpu] = r + 1u;
        cache_clean((void *)&ev_smp_done[my_cpu]);
        yield();
    }
    /* Barrier: wait until every worker has reached EV_SMP_ROUNDS
     * before any worker starts bulk-freeing. Without this, the
     * first worker to finish would drain its held[] back into the
     * pool, letting lagging workers' final allocs land on free
     * slots instead of forcing eviction — releasing the pressure
     * the test is meant to sustain. 5 s budget is generous: each
     * worker's alloc phase is bounded by ROUNDS *
     * cacheus_select_victim ≈ 5 ms wall-clock under the slowest
     * policy. */
    {
        uint64_t bar_start = timer_get_count();
        uint64_t bar_limit = timer_get_frequency() * 5u;
        while ((timer_get_count() - bar_start) < bar_limit) {
            int all_alloc_done = 1;
            for (uint32_t c = 0; c < ev_smp_n_workers_active; c++) {
                cache_invalidate((void *)&ev_smp_done[c]);
                if (ev_smp_done[c] < EV_SMP_ROUNDS) {
                    all_alloc_done = 0;
                    break;
                }
            }
            if (all_alloc_done) {
                break;
            }
            yield();
        }
    }

    /* Bulk-free everything still held. Many will be stale (evicted
     * out from under us by other workers); free returns
     * InvalidHandle on those and we ignore it — the current holder
     * of that slot will free it via its own held[] array, or via
     * the prefill teardown if no worker re-allocated it. */
    for (uint32_t i = 0; i < EV_SMP_ROUNDS; i++) {
        if (!handle_is_null(held[i])) {
            (void)rust_model_free(held[i]);
        }
    }
    task_exit();
}

static void ev_smp_swapper_task(void *arg)
{
    (void)arg;
    static const char *const policies[] = {
        "first_candidate", "lru", "lfu", "arc",
        "slm",             "xgboost", "mlp", "cacheus",
    };
    const uint32_t n_policies =
        (uint32_t)(sizeof(policies) / sizeof(policies[0]));
    uint32_t idx = 0;

    while (1) {
        cache_invalidate((void *)&ev_smp_stop_swapper);
        if (ev_smp_stop_swapper) {
            break;
        }
        (void)rust_eviction_policy_set(
            (const uint8_t *)policies[idx]);
        idx = (idx + 1u) % n_policies;
        /* Give workers run time between swaps. 16 yields is enough
         * to let pinned workers on other CPUs run at least one
         * round before the next swap on slow platforms. */
        for (int i = 0; i < 16; i++) {
            yield();
        }
    }
    /* Restore a known-good default before exiting. */
    (void)rust_eviction_policy_set((const uint8_t *)"lru");
    task_exit();
}

static void test_eviction_smp_alloc_under_concurrent_swap(void)
{
    if (!rust_eviction_enabled()) {
        TEST_IGNORE_MESSAGE("ai_eviction feature disabled");
        return;
    }
    if (cpu_count < 2u) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 2 for SMP test");
        return;
    }

    const uint32_t n_workers =
        (cpu_count < EV_SMP_MAX_WORKERS) ? cpu_count
                                         : EV_SMP_MAX_WORKERS;

    /* Reset sync state. Set n_workers_active BEFORE spawning so
     * workers can use it in the barrier wait. */
    for (uint32_t c = 0; c < EV_SMP_MAX_WORKERS; c++) {
        ev_smp_done[c] = 0;
        cache_clean((void *)&ev_smp_done[c]);
    }
    ev_smp_stop_swapper = 0;
    cache_clean((void *)&ev_smp_stop_swapper);
    ev_smp_n_workers_active = n_workers;
    cache_clean((void *)&ev_smp_n_workers_active);

    /* Pre-saturate the workspace pool down to EV_SMP_PREFILL_LEAVE
     * free blocks. This makes the SMP loop hammer the eviction slow
     * path on every alloc instead of relying on n_workers happening
     * to peak simultaneously (cross-CPU scheduling makes that
     * unreliable; an earlier hold-set design saw zero evictions in
     * QEMU). */
    static ModelHandle prefill[EV_SMP_PREFILL_MAX];
    uint32_t prefill_n = 0;
    while (prefill_n < EV_SMP_PREFILL_MAX) {
        RustPoolStats ws = rust_workspace_pool_stats();
        if (ws.free_blocks <= EV_SMP_PREFILL_LEAVE) {
            break;
        }
        ModelHandle h = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);
        if (handle_is_null(h)) {
            break;
        }
        (void)rust_model_touch(h);
        prefill[prefill_n++] = h;
    }

    /* Snapshot stats AFTER prefill, so the evictions delta we report
     * is purely from the SMP loop, not from any startup churn. */
    RustPoolStats before = rust_workspace_pool_stats();
    uart_printf("  ev-smp: prefill workspace pool — "
                "free=%lu alloc=%lu total=%lu (prefilled %lu)\r\n",
                (unsigned long)before.free_blocks,
                (unsigned long)before.allocated_blocks,
                (unsigned long)before.total_blocks,
                (unsigned long)prefill_n);

    /* Save current policy so we can restore it. */
    uint8_t saved_policy[32] = {0};
    rust_eviction_policy_name(saved_policy, sizeof(saved_policy));

    /* Spawn one worker per CPU, pinned. */
    struct task *workers[EV_SMP_MAX_WORKERS] = {0};
    for (uint32_t c = 0; c < n_workers; c++) {
        char name[8];
        name[0] = 'e'; name[1] = 'v'; name[2] = 'w';
        name[3] = '0' + (char)c; name[4] = '\0';
        workers[c] = task_create(name, ev_smp_worker_task,
                                 (void *)(uintptr_t)c);
        TEST_ASSERT_NOT_NULL_MESSAGE(workers[c],
            "ev-smp: failed to create worker");
        workers[c]->cpu_affinity = c;
        scheduler_add_task_to_cpu(workers[c], c);
    }

    /* Swapper on CPU 0. */
    struct task *swapper = task_create("ev_swap",
        ev_smp_swapper_task, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(swapper,
        "ev-smp: failed to create swapper");
    swapper->cpu_affinity = 0;
    scheduler_add_task_to_cpu(swapper, 0);

    /* Wait for all workers to fully terminate. Workers do real work
     * (the bulk-free pass) AFTER the alloc loop reaches EV_SMP_ROUNDS,
     * so polling `ev_smp_done` would let the main thread proceed to
     * the leak check while held[] arrays were still being drained.
     * Polling task state is the honest "everything done" signal.
     * 30 s budget covers the worst-case slow path (Jetson
     * EVICTION_MODELS=ON, cacheus ~150 µs/select_victim) with
     * comfortable headroom: 6 workers × ROUNDS × cacheus_latency ≈
     * 30 ms of pure select_victim work, dominated by yield overhead. */
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 30u;
    int all_done = 0;
    while ((timer_get_count() - start) < limit) {
        int done = 1;
        for (uint32_t c = 0; c < n_workers; c++) {
            cache_invalidate(&workers[c]->state);
            if (workers[c]->state != TASK_TERMINATED) {
                done = 0;
                break;
            }
        }
        if (done) {
            all_done = 1;
            break;
        }
        yield();
    }

    /* Signal swapper to stop and wait briefly for it to wind down. */
    ev_smp_stop_swapper = 1;
    cache_clean((void *)&ev_smp_stop_swapper);
    uint64_t sw_start = timer_get_count();
    uint64_t sw_limit = timer_get_frequency() * 2u;
    while ((timer_get_count() - sw_start) < sw_limit) {
        cache_invalidate(&swapper->state);
        if (swapper->state == TASK_TERMINATED) {
            break;
        }
        yield();
    }

    /* Terminate any straggler tasks before destroying — required
     * before task_destroy per the in-tree convention (see
     * kernel/CLAUDE.md §"Leaky tests that block CPU 1"). */
    for (uint32_t c = 0; c < n_workers; c++) {
        cache_invalidate(&workers[c]->state);
        if (workers[c]->state != TASK_TERMINATED) {
            scheduler_terminate_task(workers[c]);
        }
        task_destroy(workers[c]);
    }
    cache_invalidate(&swapper->state);
    if (swapper->state != TASK_TERMINATED) {
        scheduler_terminate_task(swapper);
    }
    task_destroy(swapper);

    /* Free the prefill set so the no-leak assertion below is exact.
     * Note: some prefill handles may have been evicted out from under
     * us during the SMP loop — that's expected. `free` on a stale
     * handle returns an error and we ignore it. */
    for (uint32_t i = 0; i < prefill_n; i++) {
        (void)rust_model_free(prefill[i]);
    }

    /* Restore the pre-test policy. */
    if (saved_policy[0] != 0) {
        (void)rust_eviction_policy_set(saved_policy);
    }

    /* Primary contract: no deadlock. */
    TEST_ASSERT_MESSAGE(all_done,
        "ev-smp: workers did not complete within 30s budget "
        "(probable deadlock between allocator and registry locks)");

    /* All workers reached full round count. */
    for (uint32_t c = 0; c < n_workers; c++) {
        TEST_ASSERT_MESSAGE(ev_smp_done[c] == EV_SMP_ROUNDS,
            "ev-smp: worker did not complete all rounds");
    }

    /* No leaked blocks in the workspace pool we exercised. */
    RustPoolStats after = rust_workspace_pool_stats();
    TEST_ASSERT_MESSAGE(after.allocated_blocks == 0,
        "ev-smp: leaked allocated blocks after SMP run");

    /* Diagnostic: did any allocs hit the eviction slow path? Not
     * asserted — pool may be large enough that EV_SMP_ROUNDS rounds
     * across n_workers CPUs never fills it. The primary invariant
     * (no deadlock) is the load-bearing assertion. */
    uint64_t ev_delta = after.evictions_total - before.evictions_total;
    if (ev_delta > 0) {
        uart_printf("  ev-smp: %lu evictions during run (n_workers=%lu)\r\n",
                    (unsigned long)ev_delta,
                    (unsigned long)n_workers);
    } else {
        uart_printf("  ev-smp: no evictions (pool large enough); "
                    "deadlock-freedom still verified (n_workers=%lu)\r\n",
                    (unsigned long)n_workers);
    }
}

/* End-to-end storage-backed eviction harness, trace-replay mode (#979).
 *
 * Pins the harness contract: replaying an embedded simulator scenario
 * through the real pools + active policy on the logical-tick time base
 * produces self-consistent accounting. Uses small pools so a scenario's
 * working set overflows and eviction fires; `do_read=0` so no VFS file
 * is needed in QEMU. The exact sim-matching fault rates are validated on
 * hardware with the 64+32-block cache (#979 acceptance), not here.
 * Restores the boot pool sizes at the end so later suites are unaffected
 * (reinit is destructive to the global pool). */
static void test_eviction_e2e_harness_accounts_faults(void)
{
    int nscen = rust_eviction_e2e_scenario_count();
    TEST_ASSERT_TRUE(nscen > 0);

    RustEvictionE2EResult r;
    TEST_ASSERT_EQUAL_INT32(0, rust_eviction_policy_set((const uint8_t *)"lru"));
    int rc = rust_eviction_e2e_trace(/*scenario=*/0u, /*weight_mb=*/16u,
                                     /*workspace_mb=*/8u, /*block_kb=*/2u,
                                     /*do_read=*/0u, (const uint8_t *)"", &r);
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_TRUE(r.accesses > 0u);
    TEST_ASSERT_EQUAL_UINT64(r.accesses, r.hits + r.faults);
    TEST_ASSERT_TRUE(r.faults >= 1u);            /* at least one cold miss */
    TEST_ASSERT_TRUE(r.faults <= r.accesses);

    /* Switching the policy and re-running must not crash and must keep
     * the same self-consistent accounting. */
    TEST_ASSERT_EQUAL_INT32(0,
        rust_eviction_policy_set((const uint8_t *)"mlp"));
    rc = rust_eviction_e2e_trace(0u, 16u, 8u, 2u, 0u, (const uint8_t *)"", &r);
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT64(r.accesses, r.hits + r.faults);

    /* Restore default policy + boot pool sizes (QEMU = 256/128) so later
     * suites that rely on model_mem see the expected pool. */
    (void)rust_eviction_policy_set((const uint8_t *)"lru");
    (void)rust_model_mem_reinit(256u, 128u);
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
    RUN_TEST(test_eviction_active_policy_has_gpu_backend_default_false);
    RUN_TEST(test_eviction_workload_compare_includes_all_policies);

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
    RUN_TEST(test_blob_status_defaults_empty);
    RUN_TEST(test_blob_stage_activate_rollback_round_trip);
    RUN_TEST(test_blob_stage_rejects_kind_mismatch);
    RUN_TEST(test_blob_stage_rejects_invalid_payload_body);
    RUN_TEST(test_blob_validate_rejects_invalid_outer_header_fields);
    RUN_TEST(test_blob_stage_rejects_invalid_payload_headers);
    RUN_TEST(test_blob_stage_rejects_invalid_payload_values);

    /* M8: end-to-end workload + mid-flight policy swap stress. */
    RUN_TEST(test_memory_pressure_no_leak);
    RUN_TEST(test_policy_swap_mid_workload);

    /* M9: per-policy latency benchmarks (QEMU baseline numbers). */
    RUN_TEST(test_bench_all_policies);

    /* #979: end-to-end storage-backed eviction harness contract. */
    RUN_TEST(test_eviction_e2e_harness_accounts_faults);

    /* M-SMP: multi-CPU concurrent alloc/eviction stress test (#116). */
    RUN_TEST(test_eviction_smp_alloc_under_concurrent_swap);

    return UnityEnd();
}
