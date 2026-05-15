/*
 * SLM GPU Dispatch Counter FFI Tests
 *
 * Locks in the contract for `slm_runtime_dispatch_stats` /
 * `slm_runtime_dispatch_stats_reset` introduced in PR #831. The
 * counter increment paths themselves (the `op_dispatch_record_*`
 * calls inside each `slm_runtime_dispatch_<op>_simt`) require real
 * GPU dispatch to fire, so they're covered only by hardware runs
 * (see `slm gpu` after a Jetson `slm prompt`). What this file pins
 * is the public FFI surface that the shell + `slm gpu` verb
 * consume: reset works, reads zero after reset, defends against
 * out-of-range op kinds and NULL out parameters.
 *
 * Built on every platform — the non-Jetson stub `slm_ffi.c` path
 * provides matching zero-return implementations so cross-platform
 * builds link, and these tests then verify those zeros behave the
 * same as the post-reset Jetson state.
 */

#include "unity.h"
#include "../include/slm_ffi.h"
#include "../include/gpu_handoff.h"  /* SLM_GPU_OP_* + SLM_GPU_OP_COUNT */
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Helpers
 * ============================================================================ */

static void assert_all_counters_zero(void)
{
    for (uint32_t op = 0; op < SLM_GPU_OP_COUNT; op++) {
        uint64_t attempts = (uint64_t)-1;
        uint64_t ok       = (uint64_t)-1;
        slm_runtime_dispatch_stats(op, &attempts, &ok);
        TEST_ASSERT_EQUAL_UINT64(0u, attempts);
        TEST_ASSERT_EQUAL_UINT64(0u, ok);
    }
}

/* ============================================================================
 * Tests
 * ============================================================================ */

static void test_reset_clears_all_counters(void)
{
    /* Reset is idempotent and safe to call before any dispatch ever
     * fires. Verifies the contract the shell relies on: a fresh
     * benchmark / smoke pass can zero the counters from a clean
     * slate. */
    slm_runtime_dispatch_stats_reset();
    assert_all_counters_zero();

    /* Second reset is still zero. */
    slm_runtime_dispatch_stats_reset();
    assert_all_counters_zero();
}

static void test_stats_tolerates_null_out_parameters(void)
{
    /* Either out-param can be NULL — `slm gpu` doesn't always need
     * both columns. Header documents this; pin it. */
    slm_runtime_dispatch_stats_reset();

    slm_runtime_dispatch_stats(SLM_GPU_OP_RMSNORM, NULL, NULL);
    /* No crash, no fault — counters still readable: */
    uint64_t attempts = 1u;
    uint64_t ok       = 1u;
    slm_runtime_dispatch_stats(SLM_GPU_OP_RMSNORM, &attempts, NULL);
    TEST_ASSERT_EQUAL_UINT64(0u, attempts);
    slm_runtime_dispatch_stats(SLM_GPU_OP_RMSNORM, NULL, &ok);
    TEST_ASSERT_EQUAL_UINT64(0u, ok);
}

static void test_stats_rejects_out_of_range_op_kind(void)
{
    /* Op kinds 8..11 are the CNN-shaped ops with no per-op SLM
     * dispatch FFI (the counter array is sized for transformer ops
     * only). 12 is Q4K_DEQUANT, same story. Reads of these op kinds
     * must return 0 cleanly — never out-of-bounds the static array.
     *
     * UINT32_MAX exercises the "absurdly large" guard the inline
     * helpers use; the contract is "all out-of-range op kinds read
     * 0," not "you may access the array unchecked." */
    slm_runtime_dispatch_stats_reset();

    const uint32_t out_of_range[] = {
        SLM_GPU_OP_COUNT,        /* one past the array */
        SLM_GPU_OP_GEMM_GENERIC, /* CNN op (= 8) */
        SLM_GPU_OP_Q4K_DEQUANT,  /* 12, well past */
        (uint32_t)-1,            /* UINT32_MAX */
    };
    for (size_t i = 0; i < sizeof(out_of_range) / sizeof(out_of_range[0]); i++) {
        uint64_t attempts = (uint64_t)-1;
        uint64_t ok       = (uint64_t)-1;
        slm_runtime_dispatch_stats(out_of_range[i], &attempts, &ok);
        TEST_ASSERT_EQUAL_UINT64(0u, attempts);
        TEST_ASSERT_EQUAL_UINT64(0u, ok);
    }
}

static void test_stats_covers_every_transformer_op(void)
{
    /* For each of the 8 transformer-op kinds, the FFI must produce
     * a defined (zero) read after reset. If a future renumbering
     * inside `enum slm_gpu_op_kind` shifts these discriminants
     * unexpectedly, this catches the drift. */
    slm_runtime_dispatch_stats_reset();

    uint32_t kinds[] = {
        SLM_GPU_OP_RMSNORM,
        SLM_GPU_OP_ROPE,
        SLM_GPU_OP_EMBEDDING,
        SLM_GPU_OP_Q4K_DOT,
        SLM_GPU_OP_Q4K_GEMM,
        SLM_GPU_OP_GQA_ATTN,
        SLM_GPU_OP_SWIGLU,
        SLM_GPU_OP_LM_HEAD,
    };
    TEST_ASSERT_EQUAL_UINT(SLM_GPU_OP_COUNT,
                            (unsigned)(sizeof(kinds) / sizeof(kinds[0])));

    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        uint64_t attempts = (uint64_t)-1;
        uint64_t ok       = (uint64_t)-1;
        slm_runtime_dispatch_stats(kinds[i], &attempts, &ok);
        TEST_ASSERT_EQUAL_UINT64(0u, attempts);
        TEST_ASSERT_EQUAL_UINT64(0u, ok);
    }
}

/* ============================================================================
 * Suite Runner
 * ============================================================================ */

int test_suite_slm_dispatch_stats(void)
{
    UnityBegin("SLM GPU Dispatch Stats FFI Tests");

    RUN_TEST(test_reset_clears_all_counters);
    RUN_TEST(test_stats_tolerates_null_out_parameters);
    RUN_TEST(test_stats_rejects_out_of_range_op_kind);
    RUN_TEST(test_stats_covers_every_transformer_op);

    return (int)UnityEnd();
}
