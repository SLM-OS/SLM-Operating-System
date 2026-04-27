/*
 * Model Memory Tests
 *
 * Tests the Rust model memory allocator via FFI.
 * These tests validate actual behavior, not just that calls succeed.
 */

#include "unity.h"
#include "../include/slm_ffi.h"
#include <stdint.h>

/* ============================================================================
 * Constants (must match Rust side)
 * ============================================================================ */

#define MODEL_BLOCK_SIZE    (2 * 1024 * 1024)  /* 2 MB */
#define WEIGHT_POOL_BLOCKS  128                 /* 256 MB / 2 MB */
#define WORKSPACE_POOL_BLOCKS 64                /* 128 MB / 2 MB */

/* ============================================================================
 * FFI Declarations for Model Memory
 *
 * These are the Rust functions exposed via FFI.
 * ============================================================================ */

/* Opaque handle type - matches Rust ModelHandle */
typedef struct {
    uint16_t block_index;
    uint8_t pool_id;
    uint8_t generation;
    uint32_t _reserved;
} ModelHandle;

/* Rust FFI functions */
extern int rust_model_mem_init(uint32_t weight_mb, uint32_t workspace_mb);
extern ModelHandle rust_model_alloc_weights(size_t size);
extern ModelHandle rust_model_alloc_workspace(size_t size);
extern int rust_model_free(ModelHandle handle);
extern ModelHandle rust_model_share(ModelHandle handle);
extern void *rust_model_get_ptr(ModelHandle handle);
extern size_t rust_model_get_size(ModelHandle handle);
extern int rust_eviction_enabled(void);

/* Helper to check if handle is null */
static int handle_is_null(ModelHandle h)
{
    return h.block_index == 0xFFFF && h.pool_id == 0xFF;
}

/* ============================================================================
 * Test: Weight Pool Allocation Returns Valid Pointer
 * ============================================================================ */

static void test_weight_alloc_returns_valid_pointer(void)
{
    ModelHandle h = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));

    void *ptr = rust_model_get_ptr(h);
    TEST_ASSERT_NOT_NULL(ptr);

    /* Verify pointer is 2MB aligned */
    TEST_ASSERT_EQUAL_HEX64(0, (uintptr_t)ptr % MODEL_BLOCK_SIZE);

    rust_model_free(h);
}

/* ============================================================================
 * Test: Allocated Memory is Actually Writable
 * ============================================================================ */

static void test_allocated_memory_is_writable(void)
{
    ModelHandle h = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));

    volatile uint64_t *ptr = (volatile uint64_t *)rust_model_get_ptr(h);
    TEST_ASSERT_NOT_NULL(ptr);

    /* Write a pattern */
    const uint64_t magic = 0xDEADBEEFCAFEBABEULL;
    ptr[0] = magic;
    ptr[1] = ~magic;

    /* Read it back */
    TEST_ASSERT_EQUAL_HEX64(magic, ptr[0]);
    TEST_ASSERT_EQUAL_HEX64(~magic, ptr[1]);

    /* Write at end of block (2MB - 8 bytes) */
    volatile uint64_t *end_ptr = (volatile uint64_t *)((uint8_t *)ptr + MODEL_BLOCK_SIZE - sizeof(uint64_t));
    end_ptr[0] = 0x1234567890ABCDEFULL;
    TEST_ASSERT_EQUAL_HEX64(0x1234567890ABCDEFULL, end_ptr[0]);

    rust_model_free(h);
}

/* ============================================================================
 * Test: Different Allocations Return Different Addresses
 * ============================================================================ */

static void test_different_allocs_different_addresses(void)
{
    ModelHandle h1 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    ModelHandle h2 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);

    TEST_ASSERT_FALSE(handle_is_null(h1));
    TEST_ASSERT_FALSE(handle_is_null(h2));

    void *ptr1 = rust_model_get_ptr(h1);
    void *ptr2 = rust_model_get_ptr(h2);

    TEST_ASSERT_NOT_NULL(ptr1);
    TEST_ASSERT_NOT_NULL(ptr2);

    /* Addresses must be different */
    TEST_ASSERT_TRUE(ptr1 != ptr2);

    /* And at least 2MB apart */
    uintptr_t diff = (ptr1 > ptr2) ? (uintptr_t)ptr1 - (uintptr_t)ptr2
                                   : (uintptr_t)ptr2 - (uintptr_t)ptr1;
    TEST_ASSERT_GREATER_OR_EQUAL(MODEL_BLOCK_SIZE, diff);

    rust_model_free(h1);
    rust_model_free(h2);
}

/* ============================================================================
 * Test: Size Returns Correct Value
 * ============================================================================ */

static void test_size_returns_block_size(void)
{
    ModelHandle h = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h));

    size_t size = rust_model_get_size(h);
    TEST_ASSERT_EQUAL_UINT64(MODEL_BLOCK_SIZE, size);

    rust_model_free(h);
}

/* ============================================================================
 * Test: Weight and Workspace Pools are Separate
 * ============================================================================ */

static void test_pools_are_separate(void)
{
    ModelHandle weight = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    ModelHandle workspace = rust_model_alloc_workspace(MODEL_BLOCK_SIZE);

    TEST_ASSERT_FALSE(handle_is_null(weight));
    TEST_ASSERT_FALSE(handle_is_null(workspace));

    /* Pool IDs should differ */
    TEST_ASSERT_TRUE(weight.pool_id != workspace.pool_id);

    /* Pointers should be in different regions */
    void *w_ptr = rust_model_get_ptr(weight);
    void *ws_ptr = rust_model_get_ptr(workspace);

    TEST_ASSERT_NOT_NULL(w_ptr);
    TEST_ASSERT_NOT_NULL(ws_ptr);
    TEST_ASSERT_TRUE(w_ptr != ws_ptr);

    rust_model_free(weight);
    rust_model_free(workspace);
}

/* ============================================================================
 * Test: Reference Counting Works Correctly
 * ============================================================================ */

static void test_refcount_prevents_premature_free(void)
{
    ModelHandle h1 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(h1));

    /* Write a value */
    volatile uint64_t *ptr = (volatile uint64_t *)rust_model_get_ptr(h1);
    ptr[0] = 0xFEEDFACE;

    /* Share the block (refcount = 2) */
    ModelHandle h2 = rust_model_share(h1);
    TEST_ASSERT_FALSE(handle_is_null(h2));

    /* First free (refcount = 1) - block should still be valid */
    rust_model_free(h1);

    /* Memory should still be accessible via original pointer */
    TEST_ASSERT_EQUAL_HEX64(0xFEEDFACE, ptr[0]);

    /* Second free (refcount = 0) - now actually freed */
    rust_model_free(h2);
}

/* ============================================================================
 * Test: Statistics Reflect Actual State
 * ============================================================================ */

static void test_statistics_accuracy(void)
{
    RustPoolStats before = rust_weight_pool_stats();

    /* Allocate 3 blocks */
    ModelHandle h1 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    ModelHandle h2 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    ModelHandle h3 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);

    RustPoolStats during = rust_weight_pool_stats();

    /* Should have 3 fewer free blocks */
    TEST_ASSERT_EQUAL_UINT64(before.free_blocks - 3, during.free_blocks);
    TEST_ASSERT_EQUAL_UINT64(3, during.allocated_blocks);

    /* Free them */
    rust_model_free(h1);
    rust_model_free(h2);
    rust_model_free(h3);

    RustPoolStats after = rust_weight_pool_stats();

    /* Should be back to original */
    TEST_ASSERT_EQUAL_UINT64(before.free_blocks, after.free_blocks);
    TEST_ASSERT_EQUAL_UINT64(0, after.allocated_blocks);
}

/* ============================================================================
 * Test: Pool Exhaustion Returns Null Handle
 * ============================================================================ */

static void test_pool_exhaustion(void)
{
    ModelHandle handles[WEIGHT_POOL_BLOCKS + 2];
    int allocated = 0;

    /* Allocate until pool is exhausted */
    for (int i = 0; i < WEIGHT_POOL_BLOCKS + 2; i++) {
        handles[i] = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
        if (handle_is_null(handles[i])) {
            break;
        }
        allocated++;
    }

    if (rust_eviction_enabled()) {
        /* With AI eviction ON, `alloc_weights` evicts one of the
         * existing blocks on OOM. Every request succeeds until the
         * test array is full, and the last handle points at a freshly
         * allocated slot whose predecessor was evicted. */
        TEST_ASSERT_EQUAL_INT(WEIGHT_POOL_BLOCKS + 2, allocated);
        /* One of the original handles is now stale (its slot was the
         * eviction victim). Freeing it returns an error — we tolerate
         * that and free the rest. */
        for (int i = 0; i < allocated; i++) {
            (void)rust_model_free(handles[i]);
        }
    } else {
        /* Classic behaviour: pool fills, subsequent allocs fail. */
        TEST_ASSERT_EQUAL_INT(WEIGHT_POOL_BLOCKS, allocated);
        ModelHandle overflow = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
        TEST_ASSERT_TRUE(handle_is_null(overflow));
        for (int i = 0; i < allocated; i++) {
            rust_model_free(handles[i]);
        }
    }

    /* Either way, a fresh alloc now succeeds. */
    ModelHandle recovered = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    TEST_ASSERT_FALSE(handle_is_null(recovered));
    rust_model_free(recovered);
}

/* ============================================================================
 * Test: Freed Memory is Reused
 * ============================================================================ */

static void test_freed_memory_reused(void)
{
    /* Allocate a block and get its pointer */
    ModelHandle h1 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    void *ptr1 = rust_model_get_ptr(h1);

    /* Free it */
    rust_model_free(h1);

    /* Allocate again - should get same memory back (first free block) */
    ModelHandle h2 = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
    void *ptr2 = rust_model_get_ptr(h2);

    TEST_ASSERT_EQUAL_PTR(ptr1, ptr2);

    rust_model_free(h2);
}

/* ============================================================================
 * Test: Large Model Allocation (approach pool limits)
 *
 * Allocates 200MB (100 blocks) to verify near-limit allocations work.
 * This tests the scenario of loading a small LLM's weights.
 * ============================================================================ */

#define LARGE_MODEL_BLOCKS  100  /* 200 MB */

static void test_large_model_allocation(void)
{
    ModelHandle handles[LARGE_MODEL_BLOCKS];
    int allocated = 0;
    size_t total_bytes = 0;

    /* Get initial stats */
    RustPoolStats before = rust_weight_pool_stats();

    /* Allocate 100 blocks (200 MB) - simulates loading a small LLM */
    for (int i = 0; i < LARGE_MODEL_BLOCKS; i++) {
        handles[i] = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
        if (handle_is_null(handles[i])) {
            break;
        }
        allocated++;
        total_bytes += MODEL_BLOCK_SIZE;

        /* Verify each block is accessible by writing to it */
        volatile uint64_t *ptr = (volatile uint64_t *)rust_model_get_ptr(handles[i]);
        if (ptr) {
            ptr[0] = 0xDEADBEEF00000000ULL | (uint64_t)i;
        }
    }

    /* Should have allocated all requested blocks */
    TEST_ASSERT_EQUAL_INT(LARGE_MODEL_BLOCKS, allocated);

    /* Verify stats updated correctly */
    RustPoolStats during = rust_weight_pool_stats();
    TEST_ASSERT_EQUAL_UINT64(LARGE_MODEL_BLOCKS, during.allocated_blocks);
    TEST_ASSERT_EQUAL_UINT64(before.free_blocks - LARGE_MODEL_BLOCKS, during.free_blocks);

    /* Verify all blocks still readable (not corrupted) */
    for (int i = 0; i < allocated; i++) {
        volatile uint64_t *ptr = (volatile uint64_t *)rust_model_get_ptr(handles[i]);
        uint64_t expected = 0xDEADBEEF00000000ULL | (uint64_t)i;
        TEST_ASSERT_EQUAL_HEX64(expected, ptr[0]);
    }

    /* Free all blocks */
    for (int i = 0; i < allocated; i++) {
        rust_model_free(handles[i]);
    }

    /* Verify stats restored */
    RustPoolStats after = rust_weight_pool_stats();
    TEST_ASSERT_EQUAL_UINT64(before.free_blocks, after.free_blocks);
    TEST_ASSERT_EQUAL_UINT64(0, after.allocated_blocks);
}

/* ============================================================================
 * Test Suite Runner
 * ============================================================================ */

int test_suite_model_mem(void)
{
    UnityBegin("Model Memory Tests");

    RUN_TEST(test_weight_alloc_returns_valid_pointer);
    RUN_TEST(test_allocated_memory_is_writable);
    RUN_TEST(test_different_allocs_different_addresses);
    RUN_TEST(test_size_returns_block_size);
    RUN_TEST(test_pools_are_separate);
    RUN_TEST(test_refcount_prevents_premature_free);
    RUN_TEST(test_statistics_accuracy);
    RUN_TEST(test_pool_exhaustion);
    RUN_TEST(test_freed_memory_reused);
    RUN_TEST(test_large_model_allocation);

    return UnityEnd();
}
