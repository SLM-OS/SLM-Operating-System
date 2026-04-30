/*
 * test_model_mem_smoke.c — cross-platform model-memory init smoke test.
 *
 * The full `test_model_mem.c` suite is ARM64-only because some of its
 * tests (e.g. `test_large_model_allocation` which asks for 200 MB)
 * don't fit the x86-64 QEMU q35 256 MB RAM budget. This smaller suite
 * runs everywhere and catches the specific failure mode that the
 * x86-64 build was silently regressing on:
 *
 *   `rust_model_mem_init` requested 256 MB weights + 128 MB workspace
 *   regardless of platform. On x86-64 with 256 MB total RAM the PMM
 *   couldn't satisfy the request, init returned -1, and every later
 *   `rust_model_alloc_weights` returned a null handle. The kernel
 *   continued booting and tests that didn't directly check the init
 *   status (e.g. `eviction demo` which silently no-op'd) hid the
 *   regression.
 *
 * The platform-aware sizing in `runtime/src/lib.rs::rust_model_mem_init`
 * gives x86-64 a 64 MB weights / 32 MB workspace pool that fits.
 * These tests assert that the post-`kernel_main` allocator is in a
 * usable state on every platform: non-zero pool sizes, at least one
 * weights+workspace block can be allocated, freed, and re-allocated.
 */

#include "test_harness.h"
#include "unity.h"
#include "config.h"      /* RUST_HEAP_MB */
#include "slm_ffi.h"
#include <stddef.h>
#include <stdint.h>

#define MODEL_MEM_BLOCK_SIZE (2u * 1024u * 1024u)

/* Mirror the Rust ModelHandle layout (kept private in Rust; replicated
 * here so this test can call alloc/free without a public C header).
 *
 * `kernel/tests/test_model_mem.c` has the same struct under the name
 * `ModelHandle`. The two copies are kept independent on purpose —
 * since this smoke suite runs FIRST, a layout drift fails here
 * before the larger model_mem suite runs, surfacing one clean
 * regression instead of cascading symbol-shape errors. If a third
 * caller appears, factor a shared `kernel/tests/_model_handle_priv.h`. */
typedef struct {
    uint16_t block_index;
    uint8_t  pool_id;
    uint8_t  generation;
    uint32_t _reserved;
} SmokeModelHandle;

extern SmokeModelHandle rust_model_alloc_weights(size_t size);
extern SmokeModelHandle rust_model_alloc_workspace(size_t size);
extern int rust_model_free(SmokeModelHandle handle);
extern void *rust_model_get_ptr(SmokeModelHandle handle);

static int handle_is_null(SmokeModelHandle h)
{
    return h.block_index == 0xFFFF && h.pool_id == 0xFF;
}

/*
 * The main regression check. If `rust_model_mem_init` failed at boot
 * (e.g. PMM allocation oversubscribed) the pool stats report
 * `total_blocks == 0`. Verifying both pools are non-empty proves
 * init produced a usable allocator on this platform.
 */
static void test_pools_initialized_with_blocks(void)
{
    /* `total_blocks == 0` is the unique signal that
     * `rust_model_mem_init` returned an error (PMM oversubscribe,
     * alignment failure, etc.) and left the pool statics in their
     * empty default. Failing this assertion means downstream
     * allocations will all return null handles and tests that don't
     * check init explicitly will silently no-op. */
    RustPoolStats w = rust_weight_pool_stats();
    RustPoolStats ws = rust_workspace_pool_stats();

    TEST_ASSERT_TRUE(w.total_blocks > 0);
    TEST_ASSERT_TRUE(ws.total_blocks > 0);
}

/*
 * After init, the weight pool reports at least one free block and a
 * fresh allocate+free round-trip leaves it back to the same free
 * count. Catches the case where init reported success but allocation
 * is still broken for some platform-specific reason (alignment,
 * pointer math, etc.).
 */
static void test_weight_alloc_round_trip(void)
{
    RustPoolStats before = rust_weight_pool_stats();
    TEST_ASSERT_TRUE(before.free_blocks > 0);

    SmokeModelHandle h = rust_model_alloc_weights(MODEL_MEM_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));

    void *ptr = rust_model_get_ptr(h);
    TEST_ASSERT_NOT_NULL(ptr);

    /* Block must be 2 MB aligned (matches model_mem BLOCK_SIZE
     * invariant). */
    TEST_ASSERT_EQUAL_HEX64(0, (uintptr_t)ptr % MODEL_MEM_BLOCK_SIZE);

    int rc = rust_model_free(h);
    TEST_ASSERT_EQUAL_INT(0, rc);

    RustPoolStats after = rust_weight_pool_stats();
    TEST_ASSERT_EQUAL_UINT64(before.free_blocks, after.free_blocks);
}

/*
 * Same round-trip for the workspace pool. Catches a case where one
 * pool is healthy and the other isn't (asymmetric init failure).
 */
static void test_workspace_alloc_round_trip(void)
{
    RustPoolStats before = rust_workspace_pool_stats();
    TEST_ASSERT_TRUE(before.free_blocks > 0);

    SmokeModelHandle h = rust_model_alloc_workspace(MODEL_MEM_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));

    int rc = rust_model_free(h);
    TEST_ASSERT_EQUAL_INT(0, rc);

    RustPoolStats after = rust_workspace_pool_stats();
    TEST_ASSERT_EQUAL_UINT64(before.free_blocks, after.free_blocks);
}

/*
 * Confirm the Rust heap was sized to the per-platform `RUST_HEAP_MB`
 * knob from `<config.h>` and not silently regressed. The SLM forward
 * path's KV cache (~28 MB at ctx=1024 for Qwen2.5-1.5B) and
 * `ForwardScratch` live on this heap; a regression that cuts the
 * heap below the SLM working set would surface as an opaque
 * "alloc::alloc::handle_alloc_error" panic mid-decode rather than at
 * boot. This test pins the boot-time invariant.
 */
static void test_rust_heap_size_matches_config(void)
{
    /* Width-explicit MB→bytes via shift instead of `MB * 1024u * 1024u`
     * so the math is unambiguous on a hypothetical 32-bit host build
     * (size_t == uint32_t would overflow at the 4096 MB cap from
     * config.h's _Static_assert; SLM-OS is 64-bit-only today, but the
     * shift form makes the intent obvious for future readers). */
    const uint64_t expected = (uint64_t)RUST_HEAP_MB << 20;
    const size_t actual = rust_heap_size_bytes();

    /* Non-zero: rust_heap_init was actually called. */
    TEST_ASSERT_TRUE(actual > 0);

    /* Exact match: the kernel boot path passed RUST_HEAP_MB through
     * unmodified. Drift here means either main.c's pmm_alloc_pages
     * call diverged from the constant, or the Rust side is rounding
     * down on its own. Either is a regression worth catching. */
    TEST_ASSERT_EQUAL_UINT64(expected, actual);
}

int test_suite_model_mem_smoke(void)
{
    UnityBegin("Model Memory Smoke Tests");

    RUN_TEST(test_pools_initialized_with_blocks);
    RUN_TEST(test_weight_alloc_round_trip);
    RUN_TEST(test_workspace_alloc_round_trip);
    RUN_TEST(test_rust_heap_size_matches_config);

    return UnityEnd();
}
