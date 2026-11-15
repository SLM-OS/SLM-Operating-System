/*
 * test_pmm.c - Physical Memory Manager (Buddy Allocator) Tests
 *
 * Comprehensive tests for the buddy allocator implementation including:
 * - Buddy address calculation verification
 * - Actual coalescing verification (not just page counts)
 * - Split/merge operation counting
 * - Free list integrity
 * - Alignment requirements per order
 * - Exhaustion and recovery
 */

#include "unity.h"
#include "../include/pmm.h"
#include "../include/pmm_internal.h"
#include "../include/uart.h"
#include "../include/ncmem.h"
#include "../include/dtb.h"
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Calculate expected buddy address using the same algorithm as PMM.
 * Buddy = addr XOR (2^order * PAGE_SIZE)
 */
static uintptr_t calculate_buddy_addr(uintptr_t addr, unsigned int order)
{
    size_t block_size = (size_t)PAGE_SIZE << order;
    return addr ^ block_size;
}

/* ============================================================================
 * Basic Allocation Tests
 * ============================================================================ */

/*
 * Test: Single page allocation returns valid, aligned pointer
 */
static void test_single_page_alloc(void)
{
    size_t free_before = pmm_get_free_pages();

    void *page = pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(page);

    /* Verify page-aligned */
    TEST_ASSERT_EQUAL_UINT64(0, (uintptr_t)page & (PAGE_SIZE - 1));

    /* Free pages should decrease by 1 */
    TEST_ASSERT_EQUAL_UINT64(free_before - 1, pmm_get_free_pages());

    pmm_free_page(page);
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
}

/*
 * Test: Power-of-two allocation (exact)
 */
static void test_power_of_two_alloc(void)
{
    size_t free_before = pmm_get_free_pages();

    void *pages = pmm_alloc_pages(4);
    TEST_ASSERT_NOT_NULL(pages);
    TEST_ASSERT_EQUAL_UINT64(free_before - 4, pmm_get_free_pages());

    pmm_free_pages(pages, 4);
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
}

/*
 * Test: pmm_alloc_pages_low returns a valid non-NULL page
 * of correct alignment. Same contract as pmm_alloc_pages for
 * basic behavior — the "low bias" is best-effort and can't be
 * asserted deterministically in a pmm_init'd test (the allocator's
 * starting state doesn't guarantee any particular block at any
 * particular address). Verify the allocation basics work.
 */
static void test_pmm_alloc_pages_low_basic(void)
{
    size_t free_before = pmm_get_free_pages();

    void *pages = pmm_alloc_pages_low(4);
    TEST_ASSERT_NOT_NULL(pages);

    /* Order-2 (4 pages) alignment. */
    TEST_ASSERT_EQUAL_UINT64(0, (uintptr_t)pages & ((4 * PAGE_SIZE) - 1));

    /* Free-page accounting matches. */
    TEST_ASSERT_EQUAL_UINT64(free_before - 4, pmm_get_free_pages());

    pmm_free_pages(pages, 4);
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
}

/*
 * Test: pmm_alloc_pages_low biases toward lower addresses
 * relative to pmm_alloc_pages when both are available.
 *
 * Not a strict requirement — the bias is best-effort and depends
 * on free-list state. This test documents the expected behavior
 * (same as pmm_alloc_pages for 0/MAX_ORDER+1 requests) and
 * validates the "at-least-one-block-of-correct-size" contract.
 */
static void test_pmm_alloc_pages_low_edge_cases(void)
{
    /* count=0 → NULL */
    TEST_ASSERT_NULL(pmm_alloc_pages_low(0));

    /* Non-power-of-two rounds up — request 3, get order-2 (4 pages). */
    size_t free_before = pmm_get_free_pages();
    void *pages = pmm_alloc_pages_low(3);
    TEST_ASSERT_NOT_NULL(pages);
    TEST_ASSERT_EQUAL_UINT64(free_before - 4, pmm_get_free_pages());
    pmm_free_pages(pages, 3);
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
}

/*
 * Test: pmm_alloc_pages_low under back-to-back allocation keeps
 * the free-page count consistent. Regression guard against the
 * split-down path mis-accounting block state.
 */
static void test_pmm_alloc_pages_low_accounting(void)
{
    size_t free_before = pmm_get_free_pages();
    void *a = pmm_alloc_pages_low(2);
    void *b = pmm_alloc_pages_low(4);
    void *c = pmm_alloc_pages_low(1);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT64(free_before - 2 - 4 - 1, pmm_get_free_pages());

    /* All three distinct allocations. */
    TEST_ASSERT_NOT_EQUAL(a, b);
    TEST_ASSERT_NOT_EQUAL(a, c);
    TEST_ASSERT_NOT_EQUAL(b, c);

    pmm_free_pages(a, 2);
    pmm_free_pages(b, 4);
    pmm_free_pages(c, 1);
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
}

/*
 * Test: Non-power-of-two allocation rounds up
 */
static void test_non_power_of_two_rounds_up(void)
{
    size_t free_before = pmm_get_free_pages();

    /* Request 3 pages - should get 4 (rounded up to order 2) */
    void *pages = pmm_alloc_pages(3);
    TEST_ASSERT_NOT_NULL(pages);

    /* Should consume 4 pages (next power of 2) */
    TEST_ASSERT_EQUAL_UINT64(free_before - 4, pmm_get_free_pages());

    pmm_free_pages(pages, 3);
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
}

/* ============================================================================
 * Alignment Verification Tests
 * ============================================================================ */

/*
 * Test: All orders return properly aligned addresses
 * Order N allocation must be aligned to 2^N * PAGE_SIZE
 */
static void test_alignment_all_orders(void)
{
    /* Test orders 0 through 8 (1 page to 256 pages) */
    for (unsigned int order = 0; order <= 8; order++) {
        size_t count = (size_t)1 << order;
        size_t required_alignment = count * PAGE_SIZE;

        void *block = pmm_alloc_pages(count);
        TEST_ASSERT_NOT_NULL_MESSAGE(block, "Allocation failed");

        /* Verify alignment matches order requirement */
        uintptr_t addr = (uintptr_t)block;
        TEST_ASSERT_MESSAGE(
            (addr & (required_alignment - 1)) == 0,
            "Block not aligned to order size");

        pmm_free_pages(block, count);
    }
}

/* ============================================================================
 * Buddy Address Calculation Tests
 * ============================================================================ */

/*
 * Test: Verify buddy address calculation is correct
 * Two sequential allocations from the same parent should be buddies.
 */
static void test_buddy_address_calculation(void)
{
    /* Allocate a 2-page block, then free it to create two order-0 buddies */
    void *block2 = pmm_alloc_pages(2);
    TEST_ASSERT_NOT_NULL(block2);
    pmm_free_pages(block2, 2);

    /* Now allocate two single pages - they might come from this split */
    void *p1 = pmm_alloc_page();
    void *p2 = pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);

    /* If these came from adjacent blocks, verify buddy relationship */
    uintptr_t addr1 = (uintptr_t)p1;
    uintptr_t addr2 = (uintptr_t)p2;

    /* Calculate what the buddy of p1 should be */
    uintptr_t expected_buddy = calculate_buddy_addr(addr1, 0);

    /* The pages should be different */
    TEST_ASSERT_TRUE(addr1 != addr2);

    /* Verify buddy math: buddy of buddy should be original */
    uintptr_t buddy_of_buddy = calculate_buddy_addr(expected_buddy, 0);
    TEST_ASSERT_EQUAL_UINT64(addr1, buddy_of_buddy);

    pmm_free_page(p1);
    pmm_free_page(p2);
}

/* ============================================================================
 * Coalescing Verification Tests (The Critical Ones)
 * ============================================================================ */

/*
 * The coalescing proofs below count splits and merges while single pages
 * are allocated and freed, which only says something when those pages come
 * out of one larger block. With free single pages already on the order-0
 * list the allocator hands those out first: no split, and nothing to
 * merge with when they are freed (their buddies are in use). A Jetson
 * kexec boot has about a hundred such pages, left by the carve-outs for
 * the GPU state it inherits, so both proofs failed on every Jetson kexec
 * suite boot (#1109, #1207). They passed in QEMU (none), on Pi 5 (one)
 * and after a Jetson UEFI-direct boot.
 *
 * order0_drain() takes every block off the order-0 list and holds it,
 * chained through the first word of each page; order0_release() gives
 * them back. Between the two, a single-page allocation has to split a
 * larger block, and the page after it is that block's other half.
 */
struct held_page {
    struct held_page *next;
};

static struct held_page *order0_drain(void)
{
    struct held_page *held = NULL;

    for (unsigned int n = 0; n < 1000000u; n++) {
        struct pmm_buddy_stats stats;
        pmm_get_buddy_stats(&stats);
        if (stats.free_counts[0] == 0) {
            break;
        }
        struct held_page *p = pmm_alloc_page();
        if (!p) {
            break;
        }
        p->next = held;
        held = p;
    }
    return held;
}

static void order0_release(struct held_page *held)
{
    while (held) {
        struct held_page *next = held->next;
        pmm_free_page(held);
        held = next;
    }
}

/*
 * Test: PROVE coalescing works by allocating a larger block after freeing buddies
 * This is the definitive test - if coalescing didn't happen, the large
 * allocation would fail due to fragmentation.
 */
static void coalesce_enables_larger_allocation_inner(void)
{
    struct pmm_buddy_stats stats_before, stats_after;
    pmm_get_buddy_stats(&stats_before);

    /* First, allocate a 4-page block and free it - creates order-2 in free list */
    void *block4 = pmm_alloc_pages(4);
    TEST_ASSERT_NOT_NULL(block4);
    pmm_free_pages(block4, 4);

    /* Now allocate 4 single pages - this will split that order-2 block */
    void *pages[4];
    for (int i = 0; i < 4; i++) {
        pages[i] = pmm_alloc_page();
        TEST_ASSERT_NOT_NULL(pages[i]);
    }

    pmm_get_buddy_stats(&stats_after);
    size_t splits_during_alloc = stats_after.split_count - stats_before.split_count;
    /* Splitting order-2 -> order-1 -> order-0 should cause splits */
    TEST_ASSERT_MESSAGE(splits_during_alloc > 0, "Expected splits during allocation");

    /* Free all 4 pages - they should coalesce back to order-2 */
    size_t merges_before = stats_after.merge_count;
    for (int i = 0; i < 4; i++) {
        pmm_free_page(pages[i]);
    }

    pmm_get_buddy_stats(&stats_after);
    size_t merges_during_free = stats_after.merge_count - merges_before;

    /* THE KEY TEST: If coalescing worked, we should be able to allocate 4 pages again */
    void *block4_again = pmm_alloc_pages(4);
    TEST_ASSERT_NOT_NULL_MESSAGE(block4_again,
        "4-page allocation failed - coalescing did not work!");

    /* Should have had merges during the free operations */
    TEST_ASSERT_MESSAGE(merges_during_free > 0,
        "No merges occurred - coalescing is broken");

    pmm_free_pages(block4_again, 4);
}

/*
 * Test: Recursive coalescing - freeing should merge multiple levels
 * Free 4 adjacent order-0 blocks -> should become 1 order-2 block
 */
static void recursive_coalescing_inner(void)
{
    struct pmm_buddy_stats stats_before, stats_after;

    /* Allocate an 8-page block, then free it */
    void *block8 = pmm_alloc_pages(8);
    TEST_ASSERT_NOT_NULL(block8);
    pmm_free_pages(block8, 8);

    /* Allocate 8 single pages (will split order-3 down to order-0) */
    void *pages[8];
    for (int i = 0; i < 8; i++) {
        pages[i] = pmm_alloc_page();
        TEST_ASSERT_NOT_NULL(pages[i]);
    }

    pmm_get_buddy_stats(&stats_before);

    /* Free all 8 pages - should trigger recursive coalescing */
    for (int i = 0; i < 8; i++) {
        pmm_free_page(pages[i]);
    }

    pmm_get_buddy_stats(&stats_after);

    /* Should have multiple merges: order-0->1, order-1->2, order-2->3 */
    size_t total_merges = stats_after.merge_count - stats_before.merge_count;
    TEST_ASSERT_MESSAGE(total_merges >= 4,
        "Expected at least 4 merges for recursive coalescing");

    /* Verify we can allocate 8 pages again */
    void *block8_again = pmm_alloc_pages(8);
    TEST_ASSERT_NOT_NULL_MESSAGE(block8_again,
        "8-page allocation failed after freeing 8 single pages");
    pmm_free_pages(block8_again, 8);
}

/* The proofs run with the order-0 list empty (see order0_drain). An
 * assertion that fails inside returns to here, so the held pages are
 * given back either way. */
static void coalesce_enables_larger_allocation_body(void)
{
    struct held_page *held = order0_drain();
    coalesce_enables_larger_allocation_inner();
    order0_release(held);
}

static void recursive_coalescing_body(void)
{
    struct held_page *held = order0_drain();
    recursive_coalescing_inner();
    order0_release(held);
}

static void test_coalesce_enables_larger_allocation(void)
{
    coalesce_enables_larger_allocation_body();
}

static void test_recursive_coalescing(void)
{
    recursive_coalescing_body();
}

/*
 * The state the two tests above meet after a Jetson kexec boot (#1109,
 * #1207): free single pages on the order-0 list whose buddies are in
 * use. The reserve carve-outs leave about a hundred of them there; QEMU
 * has none, so the tests never saw them. Make eight: take a
 * 16-page block and free its even pages one by one (each odd buddy stays
 * allocated, so none of them merges).
 */
#define LONE_PAGES_BLOCK 16
static void *lone_free_pages_make(void)
{
    void *block = pmm_alloc_pages(LONE_PAGES_BLOCK);
    if (block) {
        for (int i = 0; i < LONE_PAGES_BLOCK; i += 2) {
            pmm_free_page((uint8_t *)block + (size_t)i * PAGE_SIZE);
        }
    }
    return block;
}

/* Free the odd pages: each merges with its even buddy, and the block is
 * whole again. */
static void lone_free_pages_release(void *block)
{
    for (int i = 1; i < LONE_PAGES_BLOCK; i += 2) {
        pmm_free_page((uint8_t *)block + (size_t)i * PAGE_SIZE);
    }
}

/*
 * Test: the coalescing proof holds with free single pages on the order-0
 * list (the Jetson kexec state; regression test for the #1109 failures
 * of test_coalesce_enables_larger_allocation).
 */
static void test_coalesce_with_lone_free_pages(void)
{
    size_t free_before = pmm_get_free_pages();
    void *block = lone_free_pages_make();
    TEST_ASSERT_NOT_NULL(block);

    coalesce_enables_larger_allocation_body();

    lone_free_pages_release(block);
    if (!Unity.current_test_failed) {
        TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
    }
}

/*
 * Test: recursive coalescing holds with free single pages on the order-0
 * list (regression test for the #1109 failures of
 * test_recursive_coalescing).
 */
static void test_recursive_coalescing_with_lone_free_pages(void)
{
    size_t free_before = pmm_get_free_pages();
    void *block = lone_free_pages_make();
    TEST_ASSERT_NOT_NULL(block);

    recursive_coalescing_body();

    lone_free_pages_release(block);
    if (!Unity.current_test_failed) {
        TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
    }
}

/*
 * Test: Free order matters - verify coalescing works regardless of free order
 */
static void test_coalesce_any_free_order(void)
{
    /* Allocate 4 pages from same parent */
    void *block4 = pmm_alloc_pages(4);
    TEST_ASSERT_NOT_NULL(block4);
    pmm_free_pages(block4, 4);

    void *pages[4];
    for (int i = 0; i < 4; i++) {
        pages[i] = pmm_alloc_page();
        TEST_ASSERT_NOT_NULL(pages[i]);
    }

    /* Free in reverse order */
    pmm_free_page(pages[3]);
    pmm_free_page(pages[2]);
    pmm_free_page(pages[1]);
    pmm_free_page(pages[0]);

    /* Should still coalesce - verify by allocating 4 pages */
    void *block4_again = pmm_alloc_pages(4);
    TEST_ASSERT_NOT_NULL_MESSAGE(block4_again,
        "Reverse-order free did not coalesce properly");
    pmm_free_pages(block4_again, 4);

    /* Repeat with interleaved order */
    for (int i = 0; i < 4; i++) {
        pages[i] = pmm_alloc_page();
        TEST_ASSERT_NOT_NULL(pages[i]);
    }

    pmm_free_page(pages[1]);  /* Free second */
    pmm_free_page(pages[3]);  /* Free fourth */
    pmm_free_page(pages[0]);  /* Free first */
    pmm_free_page(pages[2]);  /* Free third */

    void *block4_interleaved = pmm_alloc_pages(4);
    TEST_ASSERT_NOT_NULL_MESSAGE(block4_interleaved,
        "Interleaved-order free did not coalesce properly");
    pmm_free_pages(block4_interleaved, 4);
}

/* ============================================================================
 * Split Verification Tests
 * ============================================================================ */

/*
 * Test: Verify splits are counted and free list updates correctly
 */
static void test_split_tracking(void)
{
    struct pmm_buddy_stats stats_before, stats_after;
    pmm_get_buddy_stats(&stats_before);

    /* Allocate a single page - may need to split a larger block */
    void *page = pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(page);

    pmm_get_buddy_stats(&stats_after);

    /* If we had to split, split_count should increase */
    /* (Note: might be 0 if order-0 blocks were already available) */
    TEST_ASSERT_TRUE(stats_after.split_count >= stats_before.split_count);

    pmm_free_page(page);
}

/*
 * Test: Splitting a large block creates correct number of smaller blocks
 *
 * This test verifies that when we need to split a block, the split count
 * increases appropriately. Since the allocator uses the smallest available
 * block, we need to ensure no smaller blocks exist before testing.
 */
static void test_split_creates_buddies(void)
{
    struct pmm_buddy_stats stats_before;
    pmm_get_buddy_stats(&stats_before);

    /* If there are already small blocks (order 0 or 1), no split will occur */
    if (stats_before.free_counts[0] > 0 || stats_before.free_counts[1] > 0) {
        TEST_IGNORE_MESSAGE("Skipped: small blocks already available, no split needed");
        return;
    }

    /* Find the smallest order >= 2 with free blocks */
    int found_order = -1;
    for (int o = 2; o <= PMM_MAX_ORDER; o++) {
        if (stats_before.free_counts[o] > 0) {
            found_order = o;
            break;
        }
    }

    if (found_order < 2) {
        TEST_IGNORE_MESSAGE("Skipped: no large blocks available for split test");
        return;
    }

    size_t splits_before = stats_before.split_count;

    /* Allocate 1 page - will split blocks down from found_order to order 0 */
    void *page = pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(page);

    struct pmm_buddy_stats stats_after;
    pmm_get_buddy_stats(&stats_after);

    /* The source block should be consumed (split or removed) */
    TEST_ASSERT_TRUE(stats_after.free_counts[found_order] <
                     stats_before.free_counts[found_order]);

    /* Splits should have occurred (found_order splits into smaller blocks) */
    TEST_ASSERT_TRUE(stats_after.split_count > splits_before);

    /* The number of splits should equal the order we split from */
    /* (splitting order N to order 0 requires N splits) */
    size_t expected_splits = (size_t)found_order;
    size_t actual_splits = stats_after.split_count - splits_before;
    TEST_ASSERT_EQUAL_UINT64(expected_splits, actual_splits);

    pmm_free_page(page);
}

/* ============================================================================
 * Merge Statistics Tests
 * ============================================================================ */

/*
 * Test: Verify merge count increases when buddies are freed
 */
static void test_merge_counting(void)
{
    struct pmm_buddy_stats stats_before, stats_mid, stats_after;

    /* Allocate 2 pages as block, free, then allocate as singles */
    void *block2 = pmm_alloc_pages(2);
    TEST_ASSERT_NOT_NULL(block2);
    pmm_free_pages(block2, 2);

    void *p1 = pmm_alloc_page();
    void *p2 = pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);

    pmm_get_buddy_stats(&stats_before);

    /* Free first page */
    pmm_free_page(p1);
    pmm_get_buddy_stats(&stats_mid);

    /* Free second page - should trigger merge if they're buddies */
    pmm_free_page(p2);
    pmm_get_buddy_stats(&stats_after);

    /* Verify merge count didn't decrease (would indicate a bug) */
    /* Note: They might not be buddies if allocator gave non-adjacent pages */
    TEST_ASSERT_TRUE(stats_after.merge_count >= stats_before.merge_count);
}

/* ============================================================================
 * Free List Integrity Tests
 * ============================================================================ */

/*
 * Test: After alloc/free cycle, free list should be consistent
 */
static void test_free_list_integrity(void)
{
    struct pmm_buddy_stats stats_before, stats_after;
    pmm_get_buddy_stats(&stats_before);
    size_t free_pages_before = pmm_get_free_pages();

    /* Perform many allocations and frees */
    void *blocks[20];
    for (int i = 0; i < 20; i++) {
        blocks[i] = pmm_alloc_pages((i % 4) + 1);  /* 1, 2, 3, 4 pages */
        TEST_ASSERT_NOT_NULL(blocks[i]);
    }

    /* Free in random-ish order */
    for (int i = 19; i >= 0; i -= 2) {
        pmm_free_pages(blocks[i], (i % 4) + 1);
    }
    for (int i = 0; i < 20; i += 2) {
        pmm_free_pages(blocks[i], (i % 4) + 1);
    }

    pmm_get_buddy_stats(&stats_after);
    size_t free_pages_after = pmm_get_free_pages();

    /* Free page count should be restored */
    TEST_ASSERT_EQUAL_UINT64(free_pages_before, free_pages_after);

    /* Free counts should be non-negative and sum correctly */
    size_t total_free_in_lists = 0;
    for (unsigned int o = 0; o <= PMM_MAX_ORDER; o++) {
        total_free_in_lists += stats_after.free_counts[o] * ((size_t)1 << o);
    }
    TEST_ASSERT_EQUAL_UINT64(free_pages_after, total_free_in_lists);
}

/* ============================================================================
 * Fragmentation and Recovery Tests
 * ============================================================================ */

/*
 * Test: Heavy fragmentation followed by complete free allows large allocation
 */
static void test_fragmentation_recovery_large(void)
{
    size_t free_before = pmm_get_free_pages();

    /* Allocate many small blocks to fragment memory */
    void *small_blocks[100];
    for (int i = 0; i < 100; i++) {
        small_blocks[i] = pmm_alloc_page();
        TEST_ASSERT_NOT_NULL(small_blocks[i]);
    }

    /* Free all small blocks */
    for (int i = 0; i < 100; i++) {
        pmm_free_page(small_blocks[i]);
    }

    /* Memory should be restored */
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());

    /* THE KEY TEST: Can we allocate a large contiguous block? */
    /* If coalescing worked, this should succeed */
    void *large_block = pmm_alloc_pages(64);  /* 64 pages = 256KB */
    TEST_ASSERT_NOT_NULL_MESSAGE(large_block,
        "Large allocation failed after freeing fragmented memory - coalescing broken");

    pmm_free_pages(large_block, 64);
}

/*
 * Test: Checkerboard pattern fragmentation and recovery
 */
static void test_checkerboard_fragmentation(void)
{
    void *pages[64];

    /* Allocate 64 pages */
    for (int i = 0; i < 64; i++) {
        pages[i] = pmm_alloc_page();
        TEST_ASSERT_NOT_NULL(pages[i]);
    }

    /* Free even-indexed pages (checkerboard) */
    for (int i = 0; i < 64; i += 2) {
        pmm_free_page(pages[i]);
    }

    /* At this point, memory is fragmented - 32 single free pages */

    /* Free odd-indexed pages */
    for (int i = 1; i < 64; i += 2) {
        pmm_free_page(pages[i]);
    }

    /* Now all 64 should be free and coalesced */
    /* Verify by allocating 64 contiguous pages */
    void *block64 = pmm_alloc_pages(64);
    TEST_ASSERT_NOT_NULL_MESSAGE(block64,
        "64-page allocation failed after checkerboard free - coalescing broken");
    pmm_free_pages(block64, 64);
}

/* ============================================================================
 * Exhaustion and Recovery Tests
 * ============================================================================ */

/*
 * Test: Allocate until OOM, then recover
 */
static void test_exhaustion_recovery(void)
{
    size_t free_before = pmm_get_free_pages();

    /* Allocate blocks until we run out */
    void *blocks[1000];
    int count = 0;

    for (int i = 0; i < 1000; i++) {
        void *block = pmm_alloc_pages(16);  /* 16 pages at a time */
        if (block == NULL) {
            break;  /* Out of memory */
        }
        blocks[count++] = block;
    }

    TEST_ASSERT_MESSAGE(count > 0, "Should have allocated at least one block");

    /* Verify we're low on memory */
    size_t free_during = pmm_get_free_pages();
    TEST_ASSERT_TRUE(free_during < free_before);

    /* Free everything */
    for (int i = 0; i < count; i++) {
        pmm_free_pages(blocks[i], 16);
    }

    /* Memory should be fully restored */
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
}

/* ============================================================================
 * Edge Cases
 * ============================================================================ */

/*
 * Test: Zero allocation returns NULL
 */
static void test_zero_alloc(void)
{
    void *page = pmm_alloc_pages(0);
    TEST_ASSERT_NULL(page);
}

/*
 * Test: Double free is detected and rejected
 *
 * After the first free, the page returns to the free pool.  A second free
 * of the same address should be silently rejected (the PMM prints a WARN
 * but does not add the block again).  Verify by checking that the free
 * page count does not increase on the second call.
 */
static void test_double_free_detected(void)
{
    void *page = pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(page);

    pmm_free_page(page);

    /* Snapshot free page count after the legitimate free */
    struct pmm_stats stats_after_first;
    pmm_get_stats(&stats_after_first);

    /* Second free should be rejected (warn but not crash) */
    pmm_free_page(page);

    /* Free page count must not increase — the double free was rejected */
    struct pmm_stats stats_after_second;
    pmm_get_stats(&stats_after_second);
    TEST_ASSERT_EQUAL_INT(stats_after_first.free_pages,
                          stats_after_second.free_pages);
}

/*
 * Test: Memory is actually writable
 */
static void test_memory_is_writable(void)
{
    void *page = pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(page);

    /* Write pattern to entire page */
    uint64_t *ptr = (uint64_t *)page;
    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) {
        ptr[i] = 0xDEADBEEF12345678UL;
    }

    /* Verify pattern */
    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) {
        TEST_ASSERT_EQUAL_HEX64(0xDEADBEEF12345678UL, ptr[i]);
    }

    pmm_free_page(page);
}

/*
 * Test: Large multi-page allocation is fully writable
 */
static void test_large_allocation_writable(void)
{
    void *block = pmm_alloc_pages(16);  /* 64KB */
    TEST_ASSERT_NOT_NULL(block);

    /* Write to every page in the block */
    uint8_t *ptr = (uint8_t *)block;
    for (size_t page = 0; page < 16; page++) {
        size_t offset = page * PAGE_SIZE;
        ptr[offset] = (uint8_t)page;
        ptr[offset + PAGE_SIZE - 1] = (uint8_t)(page + 0x80);
    }

    /* Verify writes */
    for (size_t page = 0; page < 16; page++) {
        size_t offset = page * PAGE_SIZE;
        TEST_ASSERT_EQUAL_UINT8((uint8_t)page, ptr[offset]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(page + 0x80), ptr[offset + PAGE_SIZE - 1]);
    }

    pmm_free_pages(block, 16);
}

/* ============================================================================
 * Statistics Tests
 * ============================================================================ */

/*
 * Test: Basic statistics are sane
 */
static void test_statistics_sane(void)
{
    struct pmm_stats stats;
    pmm_get_stats(&stats);

    TEST_ASSERT_TRUE(stats.total_pages > 0);
    TEST_ASSERT_TRUE(stats.free_pages <= stats.total_pages);
    TEST_ASSERT_EQUAL_UINT64(stats.total_pages - stats.free_pages, stats.used_pages);
    TEST_ASSERT_TRUE(stats.heap_start < stats.heap_end);
}

/*
 * Test: Buddy statistics track operations correctly
 */
static void test_buddy_stats_operations(void)
{
    struct pmm_buddy_stats stats_before, stats_after;
    pmm_get_buddy_stats(&stats_before);

    /* Do an allocation */
    void *page = pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(page);

    pmm_get_buddy_stats(&stats_after);
    TEST_ASSERT_EQUAL_UINT64(stats_before.alloc_count + 1, stats_after.alloc_count);

    /* Do a free */
    pmm_free_page(page);

    pmm_get_buddy_stats(&stats_after);
    TEST_ASSERT_EQUAL_UINT64(stats_before.free_count + 1, stats_after.free_count);
}

/*
 * Test: Free counts per order sum to total free pages
 */
static void test_free_counts_sum_correctly(void)
{
    struct pmm_buddy_stats buddy_stats;
    pmm_get_buddy_stats(&buddy_stats);

    size_t calculated_free = 0;
    for (unsigned int o = 0; o <= PMM_MAX_ORDER; o++) {
        calculated_free += buddy_stats.free_counts[o] * ((size_t)1 << o);
    }

    size_t actual_free = pmm_get_free_pages();
    TEST_ASSERT_EQUAL_UINT64(actual_free, calculated_free);
}

/* ============================================================================
 * Stress Tests
 * ============================================================================ */

/*
 * Test: Many allocations/frees don't leak memory
 */
static void test_no_memory_leak(void)
{
    size_t free_before = pmm_get_free_pages();

    /* Do 500 allocation/free cycles */
    for (int cycle = 0; cycle < 500; cycle++) {
        void *p = pmm_alloc_pages((cycle % 8) + 1);
        TEST_ASSERT_NOT_NULL(p);
        pmm_free_pages(p, (cycle % 8) + 1);
    }

    size_t free_after = pmm_get_free_pages();
    TEST_ASSERT_EQUAL_UINT64(free_before, free_after);
}

/*
 * Test: Concurrent-style stress (sequential but simulating mixed workload)
 * Uses fixed allocation sizes to ensure proper tracking and freeing.
 */
static void test_mixed_workload_stress(void)
{
    size_t free_before = pmm_get_free_pages();

    /* Track both pointers and their sizes */
    struct {
        void *ptr;
        size_t size;
    } held[50] = {{0}};

    for (int round = 0; round < 100; round++) {
        /* Allocate some */
        for (int i = 0; i < 10; i++) {
            int slot = (round * 7 + i * 3) % 50;
            if (held[slot].ptr == NULL) {
                size_t size = (size_t)((i % 4) + 1);
                held[slot].ptr = pmm_alloc_pages(size);
                held[slot].size = size;
            }
        }

        /* Free some (using tracked size) */
        for (int i = 0; i < 8; i++) {
            int slot = (round * 11 + i * 5) % 50;
            if (held[slot].ptr != NULL) {
                pmm_free_pages(held[slot].ptr, held[slot].size);
                held[slot].ptr = NULL;
                held[slot].size = 0;
            }
        }
    }

    /* Free remaining (using tracked sizes) */
    for (int i = 0; i < 50; i++) {
        if (held[i].ptr != NULL) {
            pmm_free_pages(held[i].ptr, held[i].size);
            held[i].ptr = NULL;
        }
    }

    /* Should have no leaks - memory should be fully restored */
    size_t free_after = pmm_get_free_pages();
    TEST_ASSERT_EQUAL_UINT64(free_before, free_after);
}

/* ============================================================================
 * Free-list consistency under states the allocator must tolerate (#1207)
 * ============================================================================ */

/*
 * Take the free page at `want` back off the order-0 list. The list is
 * LIFO, so it is normally the first page handed out; pages that are not
 * `want` are held and returned. False if `want` did not come back.
 */
static bool retake_free_page(uintptr_t want)
{
    void *others[32];
    int n = 0;
    bool got = false;

    while (n < 32) {
        void *p = pmm_alloc_page();
        if (!p) {
            break;
        }
        if ((uintptr_t)p == want) {
            got = true;
            break;
        }
        others[n++] = p;
    }
    while (n > 0) {
        pmm_free_page(others[--n]);
    }
    return got;
}

/*
 * Test: a region whose end is not page-aligned does not list the page
 * that holds the end. pmm_init carves reserved ranges out of each region
 * and adds what is left; a reservation that starts inside a page ends the
 * subrange before it inside that page, and the page belongs to the
 * reservation. Listing it also leaves two free order-0 buddies that were
 * never merged.
 */
static void test_add_region_leaves_out_partial_last_page(void)
{
    size_t free_before = pmm_get_free_pages();
    void *x = pmm_alloc_pages(4);
    TEST_ASSERT_NOT_NULL(x);
    uintptr_t base = (uintptr_t)x;

    /* [page 2, 0x100 bytes into page 3): one whole page. */
    pmm_test_add_region(base + 2 * PAGE_SIZE, base + 3 * PAGE_SIZE + 0x100);

    int order2 = pmm_free_block_order(base + 2 * PAGE_SIZE);
    size_t over = pmm_free_overlap_count_locked(base + 3 * PAGE_SIZE,
                                                base + 4 * PAGE_SIZE);

    /* Put the block back together before asserting: take back whatever
     * was listed, then free the block whole. */
    bool back = true;
    if (over) {
        back = retake_free_page(base + 3 * PAGE_SIZE) && back;
    }
    if (order2 == 0) {
        back = retake_free_page(base + 2 * PAGE_SIZE) && back;
    }
    if (back) {
        pmm_free_pages(x, 4);
    }

    TEST_ASSERT_MESSAGE(order2 == 0,
        "the whole page of the region was not listed at order 0");
    TEST_ASSERT_MESSAGE(over == 0,
        "the page holding the region's unaligned end was listed");
    TEST_ASSERT_TRUE_MESSAGE(back, "could not take the test pages back");
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
}

/*
 * Test: a free does not merge with a buddy range that is free page by
 * page but is not one free block of that order (#1207).
 *
 * Pages 2 and 3 of a held 4-page block are added as two separate regions,
 * so they sit on the order-0 list as two blocks, unmerged. Freeing pages
 * 0-1 as an order-1 block then finds its order-1 buddy range (pages 2-3)
 * free. Merging there would unlink page 2 from "the order-1 list", which
 * it is not on: free_counts[1] is decremented for a block the list never
 * held (an underflow when it is 0), page 3 stays on the order-0 list
 * inside the merged block, and a later allocation finds a non-zero count
 * over an empty list and splits from PMM_ALLOC_FAIL: the #1207 halt.
 */
static void test_free_does_not_merge_with_split_buddy(void)
{
    size_t free_before = pmm_get_free_pages();
    void *x = pmm_alloc_pages(4);
    TEST_ASSERT_NOT_NULL(x);
    uintptr_t base = (uintptr_t)x;

    pmm_test_add_region(base + 2 * PAGE_SIZE, base + 3 * PAGE_SIZE);
    pmm_test_add_region(base + 3 * PAGE_SIZE, base + 4 * PAGE_SIZE);
    TEST_ASSERT_EQUAL_INT(0, pmm_free_block_order(base + 2 * PAGE_SIZE));
    TEST_ASSERT_EQUAL_INT(0, pmm_free_block_order(base + 3 * PAGE_SIZE));

    pmm_free_pages(x, 2);

    int order0 = pmm_free_block_order(base);
    int order2 = pmm_free_block_order(base + 2 * PAGE_SIZE);
    int order3 = pmm_free_block_order(base + 3 * PAGE_SIZE);

    /* Clean up only from the expected state: pages 0-1 one order-1 block,
     * pages 2 and 3 still two order-0 blocks. Take 3 and 2 back and free
     * them one by one, as they were allocated: the second free merges the
     * pair, and the pair merges with pages 0-1. */
    bool back = false;
    if (order0 == 1 && order2 == 0 && order3 == 0) {
        back = retake_free_page(base + 3 * PAGE_SIZE) &&
               retake_free_page(base + 2 * PAGE_SIZE);
        if (back) {
            pmm_free_page((void *)(base + 2 * PAGE_SIZE));
            pmm_free_page((void *)(base + 3 * PAGE_SIZE));
        }
    }

    TEST_ASSERT_MESSAGE(order0 == 1,
        "pages 0-1 are not one order-1 free block: merged with a buddy "
        "range that is not one block");
    TEST_ASSERT_MESSAGE(order2 == 0, "page 2 left the order-0 list");
    TEST_ASSERT_MESSAGE(order3 == 0, "page 3 left the order-0 list");
    TEST_ASSERT_TRUE_MESSAGE(back, "could not take the test pages back");
    TEST_ASSERT_EQUAL_UINT64(free_before, pmm_get_free_pages());
}

/*
 * Test: a free whose range holds a page that is already free is rejected
 * (an over-sized free, or a free after part of the block was freed). The
 * double-free check used to look at the first page only, so the range
 * went onto a free list over the block already listed inside it.
 */
static void test_free_rejects_range_holding_a_free_page(void)
{
    size_t free_start = pmm_get_free_pages();
    void *x = pmm_alloc_pages(4);
    TEST_ASSERT_NOT_NULL(x);
    uintptr_t base = (uintptr_t)x;

    pmm_free_page((void *)(base + 2 * PAGE_SIZE));   /* page 2: order 0 */
    size_t free_before = pmm_get_free_pages();

    pmm_free_pages(x, 4);                            /* must be rejected */

    size_t free_after = pmm_get_free_pages();
    int order0 = pmm_free_block_order(base);
    int order2 = pmm_free_block_order(base + 2 * PAGE_SIZE);

    if (free_after == free_before && order0 < 0 && order2 == 0) {
        pmm_free_pages(x, 2);                          /* pages 0-1 */
        pmm_free_page((void *)(base + 3 * PAGE_SIZE)); /* merges all four */
    }

    TEST_ASSERT_MESSAGE(free_after == free_before,
        "a free over an already-free page changed the free page count");
    TEST_ASSERT_MESSAGE(order0 == -1,
        "the rejected range was put on a free list");
    TEST_ASSERT_MESSAGE(order2 == 0, "page 2 left the order-0 list");
    TEST_ASSERT_EQUAL_UINT64(free_start, pmm_get_free_pages());
}

/*
 * Test: the free-list verifier reports a cleared next link, once, and
 * reports nothing on consistent lists (#1207). The link of a list head
 * that has a successor is set to NULL, as found on jetson-nano-2, then put
 * back. The FINDING block this prints is the test's own.
 */
static void test_verify_reports_a_cleared_link(void)
{
    TEST_ASSERT_EQUAL_UINT(0, pmm_verify_free_lists("verifier self-test, before"));

    /* Two single pages whose buddies stay allocated: the order-0 list
     * then holds at least two blocks. */
    void *pages = pmm_alloc_pages(4);
    TEST_ASSERT_NOT_NULL(pages);
    pmm_free_page(pages);
    pmm_free_page((uint8_t *)pages + 2 * PAGE_SIZE);

    unsigned findings_before = pmm_verify_finding_count();
    unsigned first = 0, second = 0, restored = 1;
    int order = pmm_test_verify_cleared_link(&first, &second, &restored);
    unsigned printed = pmm_verify_finding_count() - findings_before;

    pmm_free_page((uint8_t *)pages + PAGE_SIZE);
    pmm_free_page((uint8_t *)pages + 3 * PAGE_SIZE);

    TEST_ASSERT_MESSAGE(order >= 0, "no order with two free blocks to test on");
    TEST_ASSERT_EQUAL_UINT(1, first);
    TEST_ASSERT_EQUAL_UINT(1, second);
    TEST_ASSERT_MESSAGE(printed == 1, "the finding was not printed exactly once");
    TEST_ASSERT_EQUAL_UINT(0, restored);
    TEST_ASSERT_EQUAL_UINT(0, pmm_verify_free_lists("verifier self-test, after"));
}

/*
 * Test: the verifier's dump reads only free pages inside the heap. The
 * heap range spans holes that are not mapped on Jetson (the OP-TEE
 * carve-out, everything above the last RAM region); they are marked
 * allocated, like pages in use, and a dump that read "the 64 bytes before"
 * a block at the start of a region, or the target of a bad link, would
 * fault there.
 */
static void test_verify_dump_reads_only_free_heap_pages(void)
{
    struct pmm_stats stats;
    pmm_get_stats(&stats);

    void *pages = pmm_alloc_pages(2);
    TEST_ASSERT_NOT_NULL(pages);
    uintptr_t base = (uintptr_t)pages;

    /* Allocated pages are not read (an unmapped hole looks the same). */
    bool alloc_first = pmm_test_dump_readable(base);
    bool alloc_before_second = pmm_test_dump_readable(base + PAGE_SIZE - 64);

    /* Free the second page only: it is read; the 64 bytes before it are in
     * the first page, still allocated, and are not. */
    pmm_free_page((void *)(base + PAGE_SIZE));
    bool free_second = pmm_test_dump_readable(base + PAGE_SIZE);
    bool before_free_second = pmm_test_dump_readable(base + PAGE_SIZE - 64);
    /* A range that crosses a page boundary is never read. */
    bool straddle = pmm_test_dump_readable(base + PAGE_SIZE - 32);

    pmm_free_page(pages);

    TEST_ASSERT_FALSE(alloc_first);
    TEST_ASSERT_FALSE(alloc_before_second);
    TEST_ASSERT_TRUE(free_second);
    TEST_ASSERT_FALSE(before_free_second);
    TEST_ASSERT_FALSE(straddle);
    /* Outside the heap range, at either end, and a wrapping address. */
    TEST_ASSERT_FALSE(pmm_test_dump_readable(stats.heap_start - 64));
    TEST_ASSERT_FALSE(pmm_test_dump_readable(stats.heap_end));
    TEST_ASSERT_FALSE(pmm_test_dump_readable(stats.heap_end - 32));
    TEST_ASSERT_FALSE(pmm_test_dump_readable((uintptr_t)-32));
    TEST_ASSERT_FALSE(pmm_test_dump_readable(0));
}

/* ============================================================================
 * NC Memory Allocator Tests (Pi 5 / Jetson only)
 * ============================================================================ */

#if defined(PLATFORM_HAS_NC_REGION)

/*
 * Test: zero size / zero alignment are rejected
 */
static void test_ncmem_rejects_invalid_args(void)
{
    TEST_ASSERT_NULL(ncmem_alloc(0, 8));
    TEST_ASSERT_NULL(ncmem_alloc(64, 0));
}

/*
 * Test: allocations return addresses with the requested alignment
 */
static void test_ncmem_alignment(void)
{
    const size_t alignments[] = {1, 8, 16, 64, 256, 4096};
    for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
        size_t align = alignments[i];
        void *p = ncmem_alloc(32, align);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL_UINT64(0, (uintptr_t)p & (align - 1));
    }
}

/*
 * Test: ncmem_used grows monotonically and matches cumulative footprint
 */
static void test_ncmem_used_monotonic(void)
{
    size_t used_before = ncmem_used();

    void *p1 = ncmem_alloc(128, 16);
    TEST_ASSERT_NOT_NULL(p1);
    size_t used_after_first = ncmem_used();
    TEST_ASSERT_TRUE(used_after_first > used_before);

    void *p2 = ncmem_alloc(256, 64);
    TEST_ASSERT_NOT_NULL(p2);
    size_t used_after_second = ncmem_used();
    TEST_ASSERT_TRUE(used_after_second > used_after_first);

    /* Second allocation's address must sit above first allocation's end */
    TEST_ASSERT_TRUE((uintptr_t)p2 >= (uintptr_t)p1 + 128);
}

/*
 * Test: requests larger than the NC region fail gracefully
 */
static void test_ncmem_oversize_rejected(void)
{
    /* Request > NC_MEM_SIZE — must return NULL and not corrupt state */
    size_t used_before = ncmem_used();
    void *p = ncmem_alloc(NC_MEM_SIZE + 4096, 16);
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_UINT64(used_before, ncmem_used());
}

/*
 * Test: overflow in alignment rounding or size addition is rejected
 */
static void test_ncmem_overflow_rejected(void)
{
    size_t used_before = ncmem_used();

    /* Pathological: SIZE_MAX size — aligned + size must overflow */
    void *p = ncmem_alloc((size_t)-1, 16);
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_UINT64(used_before, ncmem_used());
}

#endif /* PLATFORM_HAS_NC_REGION */

/* ============================================================================
 * DTB blob reservation (#1244)
 *
 * The PMM carves the DTB blob's own range out of every region and checks at
 * init that no free block overlaps it. On Jetson kexec the loader puts the
 * blob in the first free pages after the kernel image, where the heap
 * starts; on Pi 5 the firmware puts it in low RAM, also inside the heap.
 * ============================================================================ */

static void test_dtb_reserve_range_page_aligns(void)
{
    dtb_memreserve_t r = { 0, 0 };
    /* Pi 5 style: unaligned start, unaligned size. */
    TEST_ASSERT_TRUE(pmm_dtb_reserve_range(0x2eff2e00ULL, 0x13a5c, &r));
    TEST_ASSERT_EQUAL_HEX64(0x2eff2000ULL, r.addr);
    TEST_ASSERT_EQUAL_HEX64(0x2f007000ULL - 0x2eff2000ULL, r.size);
    /* Already aligned: unchanged. */
    TEST_ASSERT_TRUE(pmm_dtb_reserve_range(0x80a9b000ULL, 0x2000, &r));
    TEST_ASSERT_EQUAL_HEX64(0x80a9b000ULL, r.addr);
    TEST_ASSERT_EQUAL_HEX64(0x2000ULL, r.size);
    /* One byte takes one page. */
    TEST_ASSERT_TRUE(pmm_dtb_reserve_range(0x1fffULL, 1, &r));
    TEST_ASSERT_EQUAL_HEX64(0x1000ULL, r.addr);
    TEST_ASSERT_EQUAL_HEX64(0x1000ULL, r.size);
    /* Rejected: empty, wrapping, no output. */
    TEST_ASSERT_FALSE(pmm_dtb_reserve_range(0x1000, 0, &r));
    TEST_ASSERT_FALSE(pmm_dtb_reserve_range(~0ULL - 4, 16, &r));
    TEST_ASSERT_FALSE(pmm_dtb_reserve_range(0x1000, 16, NULL));
}

/* The kexec layout of #1244: blob starts below the (64 KB aligned) end of
 * the image and runs about 950 KB into the heap. */
static void test_carve_dtb_straddling_heap_start(void)
{
    dtb_memreserve_t r;
    TEST_ASSERT_TRUE(pmm_dtb_reserve_range(0x80a9a000ULL, 0xe8000, &r));
    uintptr_t s[4], e[4];
    int n = pmm_carve_reserves(0x80aa0000UL, 0xa1000000UL, &r, 1, s, e, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_HEX64(0x80b82000UL, s[0]);     /* first page after the blob */
    TEST_ASSERT_EQUAL_HEX64(0xa1000000UL, e[0]);
}

/* The list pmm_init carves holds the live blob's range. */
static void test_collect_reserves_includes_live_dtb(void)
{
    uint64_t addr, size;
    if (!dtb_get_blob_range(&addr, &size)) {
        TEST_IGNORE_MESSAGE("no DTB blob on this platform or boot path");
    }
    dtb_memreserve_t want;
    TEST_ASSERT_TRUE(pmm_dtb_reserve_range(addr, size, &want));

    static dtb_memreserve_t list[DTB_MAX_MEMRESERVES + 2048 + 1];
    int n = pmm_collect_reserves(list, (int)(sizeof(list) / sizeof(list[0])));
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (list[i].addr == want.addr && list[i].size == want.size) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "DTB blob range not in the PMM reserve list");
    TEST_ASSERT_TRUE(want.addr <= addr && want.addr + want.size >= addr + size);
}

/* No free block overlaps the live blob (fails on a kernel without the
 * reservation wherever the loader put the blob inside the heap: Jetson
 * kexec, Pi 5). */
static void test_no_free_block_overlaps_live_dtb(void)
{
    uint64_t addr, size;
    if (!dtb_get_blob_range(&addr, &size)) {
        TEST_IGNORE_MESSAGE("no DTB blob on this platform or boot path");
    }
    dtb_memreserve_t r;
    TEST_ASSERT_TRUE(pmm_dtb_reserve_range(addr, size, &r));
    TEST_ASSERT_EQUAL_UINT64(0, pmm_free_overlap_count_locked(
                                    (uintptr_t)r.addr,
                                    (uintptr_t)(r.addr + r.size)));
}

/* The overlap walk sees a free block and stops seeing it once allocated. */
static void test_free_overlap_count_sees_a_free_page(void)
{
    void *p = pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(p);
    uintptr_t a = (uintptr_t)p;
    TEST_ASSERT_EQUAL_UINT64(0, pmm_free_overlap_count_locked(a, a + PAGE_SIZE));
    pmm_free_page(p);
    TEST_ASSERT_TRUE(pmm_free_overlap_count_locked(a, a + PAGE_SIZE) >= 1);
}

/* ============================================================================
 * pmm_carve_reserves — pure carve helper (kernel/include/pmm_internal.h)
 *
 * Used by pmm_add_region_split to subtract firmware-supplied
 * /memreserve/ ranges (typically the BCM2712 VPU shared-memory carveout)
 * from the buddy allocator's free range. Tested in isolation; no PMM
 * state mutation needed.
 * ============================================================================ */

static void test_carve_no_reserves_returns_full_region(void)
{
    uintptr_t s[4], e[4];
    int n = pmm_carve_reserves(0x1000, 0x10000, NULL, 0, s, e, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT64(0x1000,  s[0]);
    TEST_ASSERT_EQUAL_UINT64(0x10000, e[0]);
}

static void test_carve_empty_region_returns_zero(void)
{
    uintptr_t s[4], e[4];
    int n = pmm_carve_reserves(0x10000, 0x10000, NULL, 0, s, e, 4);
    TEST_ASSERT_EQUAL_INT(0, n);
    n = pmm_carve_reserves(0x10000, 0x1000, NULL, 0, s, e, 4);
    TEST_ASSERT_EQUAL_INT(0, n);
}

static void test_carve_reserve_in_middle_splits(void)
{
    /* Pi 5 case: 4 GB heap minus 4 MB VPU carveout at 0x3fc00000. */
    dtb_memreserve_t rsv = { .addr = 0x3fc00000, .size = 0x400000 };
    uintptr_t s[4], e[4];
    int n = pmm_carve_reserves(0xbf3000, 0xffe00000, &rsv, 1, s, e, 4);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_UINT64(0xbf3000,    s[0]);
    TEST_ASSERT_EQUAL_UINT64(0x3fc00000,  e[0]);
    TEST_ASSERT_EQUAL_UINT64(0x40000000,  s[1]);
    TEST_ASSERT_EQUAL_UINT64(0xffe00000,  e[1]);
}

static void test_carve_reserve_at_start_truncates(void)
{
    dtb_memreserve_t rsv = { .addr = 0x1000, .size = 0x4000 };
    uintptr_t s[4], e[4];
    int n = pmm_carve_reserves(0x2000, 0x10000, &rsv, 1, s, e, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT64(0x5000,  s[0]);
    TEST_ASSERT_EQUAL_UINT64(0x10000, e[0]);
}

static void test_carve_reserve_at_end_truncates(void)
{
    dtb_memreserve_t rsv = { .addr = 0xC000, .size = 0x8000 };
    uintptr_t s[4], e[4];
    int n = pmm_carve_reserves(0x2000, 0x10000, &rsv, 1, s, e, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT64(0x2000, s[0]);
    TEST_ASSERT_EQUAL_UINT64(0xC000, e[0]);
}

static void test_carve_reserve_outside_region_no_effect(void)
{
    dtb_memreserve_t rsv[2] = {
        { .addr = 0x100,    .size = 0x100  },
        { .addr = 0x100000, .size = 0x1000 },
    };
    uintptr_t s[4], e[4];
    int n = pmm_carve_reserves(0x10000, 0x20000, rsv, 2, s, e, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT64(0x10000, s[0]);
    TEST_ASSERT_EQUAL_UINT64(0x20000, e[0]);
}

static void test_carve_reserve_swallows_region(void)
{
    dtb_memreserve_t rsv = { .addr = 0x0, .size = 0x100000 };
    uintptr_t s[4], e[4];
    int n = pmm_carve_reserves(0x10000, 0x20000, &rsv, 1, s, e, 4);
    TEST_ASSERT_EQUAL_INT(0, n);
}

static void test_carve_multiple_unsorted_reserves(void)
{
    dtb_memreserve_t rsv[2] = {
        { .addr = 0x8000, .size = 0x1000 },
        { .addr = 0x4000, .size = 0x1000 },
    };
    uintptr_t s[8], e[8];
    int n = pmm_carve_reserves(0x1000, 0x10000, rsv, 2, s, e, 8);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_UINT64(0x1000, s[0]); TEST_ASSERT_EQUAL_UINT64(0x4000, e[0]);
    TEST_ASSERT_EQUAL_UINT64(0x5000, s[1]); TEST_ASSERT_EQUAL_UINT64(0x8000, e[1]);
    TEST_ASSERT_EQUAL_UINT64(0x9000, s[2]); TEST_ASSERT_EQUAL_UINT64(0x10000, e[2]);
}

static void test_carve_truncates_at_max_out(void)
{
    dtb_memreserve_t rsv[3] = {
        { .addr = 0x2000, .size = 0x1000 },
        { .addr = 0x4000, .size = 0x1000 },
        { .addr = 0x6000, .size = 0x1000 },
    };
    uintptr_t s[2], e[2];
    int n = pmm_carve_reserves(0x1000, 0x10000, rsv, 3, s, e, 2);
    TEST_ASSERT_TRUE(n <= 2);
    TEST_ASSERT_EQUAL_UINT64(0x1000, s[0]);
    TEST_ASSERT_EQUAL_UINT64(0x2000, e[0]);
}

static void test_carve_zero_size_reserve_skipped(void)
{
    /* Zero-size reservation must be a no-op (re <= rs guard). */
    dtb_memreserve_t rsv = { .addr = 0x4000, .size = 0 };
    uintptr_t s[4], e[4];
    int n = pmm_carve_reserves(0x1000, 0x10000, &rsv, 1, s, e, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT64(0x1000,  s[0]);
    TEST_ASSERT_EQUAL_UINT64(0x10000, e[0]);
}

static void test_carve_null_output_returns_zero(void)
{
    int n = pmm_carve_reserves(0x1000, 0x10000, NULL, 0, NULL, NULL, 4);
    TEST_ASSERT_EQUAL_INT(0, n);
}

/* ============================================================================
 * User-reserve API (#788)
 * ============================================================================
 *
 * `pmm_user_reserve_add` registers a (phys, size) range that PMM init
 * carves out of every region added to the buddy allocator. The
 * production caller is `ga10b_kexec_handoff_register_reserves` in
 * the Jetson early-boot path; these unit tests cover the API shape
 * (count tracking, reset, full-table behaviour) so a regression
 * shows up in QEMU `make test` before it can break the Jetson
 * boot path.
 *
 * Every test resets the table at entry so it doesn't matter which
 * other test ran before. */

static void test_user_reserve_count_starts_zero(void)
{
    pmm_user_reserve_reset();
    TEST_ASSERT_EQUAL_UINT64(0, pmm_user_reserve_count());
}

static void test_user_reserve_add_increments_count(void)
{
    pmm_user_reserve_reset();

    TEST_ASSERT_EQUAL_INT(0, pmm_user_reserve_add(0x100000, 0x1000));
    TEST_ASSERT_EQUAL_UINT64(1, pmm_user_reserve_count());

    TEST_ASSERT_EQUAL_INT(0, pmm_user_reserve_add(0x200000, 0x2000));
    TEST_ASSERT_EQUAL_UINT64(2, pmm_user_reserve_count());

    TEST_ASSERT_EQUAL_INT(0, pmm_user_reserve_add(0x300000, 0x1000));
    TEST_ASSERT_EQUAL_UINT64(3, pmm_user_reserve_count());
}

static void test_user_reserve_zero_size_rejected(void)
{
    pmm_user_reserve_reset();

    /* Zero-size reservations are nonsense — `pmm_carve_reserves`
     * silently skips them, so accepting them here would just bloat
     * the table without effect. Reject up front. */
    TEST_ASSERT_EQUAL_INT(-1, pmm_user_reserve_add(0x100000, 0));
    TEST_ASSERT_EQUAL_UINT64(0, pmm_user_reserve_count());
}

static void test_user_reserve_reset_zeros_count(void)
{
    pmm_user_reserve_reset();
    (void)pmm_user_reserve_add(0x100000, 0x1000);
    (void)pmm_user_reserve_add(0x200000, 0x1000);
    TEST_ASSERT_EQUAL_UINT64(2, pmm_user_reserve_count());

    pmm_user_reserve_reset();
    TEST_ASSERT_EQUAL_UINT64(0, pmm_user_reserve_count());
}

static void test_user_reserve_table_full_rejects(void)
{
    pmm_user_reserve_reset();

    /* Fill the table — capacity = PMM_MAX_USER_RESERVES (file-static
     * in pmm.c). The current value is 8192 (#788 Stage 4 bumped from
     * 16 to fit the weights-pool extents list); the upper-bound here
     * is 10000 so the loop has headroom over the cap and the
     * test still works if the cap is bumped further. */
    int last_ok_count = 0;
    for (int i = 0; i < 10000; i++) {
        int rc = pmm_user_reserve_add(
            (uint64_t)(0x100000 + (uint64_t)i * 0x1000), 0x1000);
        if (rc == 0) {
            last_ok_count = i + 1;
        } else {
            break;
        }
    }
    TEST_ASSERT_TRUE(last_ok_count >= 1);

    /* Once the table is full, further adds reject. */
    TEST_ASSERT_EQUAL_INT(-1, pmm_user_reserve_add(0x900000, 0x1000));

    /* Count is stable at the cap, not silently incremented. */
    TEST_ASSERT_EQUAL_UINT64((size_t)last_ok_count,
                             pmm_user_reserve_count());

    /* Clean up so a later test doesn't inherit a full table — defensive
     * even though every test starts with a reset (one consistent
     * idiom). */
    pmm_user_reserve_reset();
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_pmm(void)
{
    UnityBegin("PMM Buddy Allocator Tests");

    /* Basic allocation */
    RUN_TEST(test_single_page_alloc);
    RUN_TEST(test_power_of_two_alloc);
    RUN_TEST(test_non_power_of_two_rounds_up);

    /* Low-biased allocator (Phase 8 #253 DMA-window support) */
    RUN_TEST(test_pmm_alloc_pages_low_basic);
    RUN_TEST(test_pmm_alloc_pages_low_edge_cases);
    RUN_TEST(test_pmm_alloc_pages_low_accounting);

    /* Alignment verification */
    RUN_TEST(test_alignment_all_orders);

    /* Buddy address calculation */
    RUN_TEST(test_buddy_address_calculation);

    /* CRITICAL: Coalescing verification */
    RUN_TEST(test_coalesce_enables_larger_allocation);
    RUN_TEST(test_recursive_coalescing);
    RUN_TEST(test_coalesce_with_lone_free_pages);
    RUN_TEST(test_recursive_coalescing_with_lone_free_pages);
    RUN_TEST(test_coalesce_any_free_order);

    /* Split verification */
    RUN_TEST(test_split_tracking);
    RUN_TEST(test_split_creates_buddies);

    /* Merge verification */
    RUN_TEST(test_merge_counting);

    /* Free list integrity */
    RUN_TEST(test_free_list_integrity);

    /* Fragmentation and recovery */
    RUN_TEST(test_fragmentation_recovery_large);
    RUN_TEST(test_checkerboard_fragmentation);

    /* Exhaustion and recovery */
    RUN_TEST(test_exhaustion_recovery);

    /* Edge cases */
    RUN_TEST(test_zero_alloc);
    RUN_TEST(test_double_free_detected);
    RUN_TEST(test_memory_is_writable);
    RUN_TEST(test_large_allocation_writable);

    /* Statistics */
    RUN_TEST(test_statistics_sane);
    RUN_TEST(test_buddy_stats_operations);
    RUN_TEST(test_free_counts_sum_correctly);

    /* Stress tests */
    RUN_TEST(test_no_memory_leak);
    RUN_TEST(test_mixed_workload_stress);

    /* Free-list consistency (#1207) */
    RUN_TEST(test_add_region_leaves_out_partial_last_page);
    RUN_TEST(test_free_does_not_merge_with_split_buddy);
    RUN_TEST(test_free_rejects_range_holding_a_free_page);
    RUN_TEST(test_verify_reports_a_cleared_link);
    RUN_TEST(test_verify_dump_reads_only_free_heap_pages);

    /* /memreserve/ carve helper */
    RUN_TEST(test_carve_no_reserves_returns_full_region);
    RUN_TEST(test_carve_empty_region_returns_zero);
    RUN_TEST(test_carve_reserve_in_middle_splits);
    RUN_TEST(test_carve_reserve_at_start_truncates);
    RUN_TEST(test_carve_reserve_at_end_truncates);
    RUN_TEST(test_carve_reserve_outside_region_no_effect);
    RUN_TEST(test_carve_reserve_swallows_region);
    RUN_TEST(test_carve_multiple_unsorted_reserves);
    RUN_TEST(test_carve_truncates_at_max_out);
    RUN_TEST(test_carve_zero_size_reserve_skipped);
    RUN_TEST(test_carve_null_output_returns_zero);

    /* DTB blob reservation (#1244) */
    RUN_TEST(test_dtb_reserve_range_page_aligns);
    RUN_TEST(test_carve_dtb_straddling_heap_start);
    RUN_TEST(test_collect_reserves_includes_live_dtb);
    RUN_TEST(test_no_free_block_overlaps_live_dtb);
    RUN_TEST(test_free_overlap_count_sees_a_free_page);

#if defined(PLATFORM_HAS_NC_REGION)
    /* NC allocator (Pi 5 / Jetson only) */
    RUN_TEST(test_ncmem_rejects_invalid_args);
    RUN_TEST(test_ncmem_alignment);
    RUN_TEST(test_ncmem_used_monotonic);
    RUN_TEST(test_ncmem_oversize_rejected);
    RUN_TEST(test_ncmem_overflow_rejected);
#endif

    /* User-reserve API (#788 root-cause fix prep). The integration
     * test that the reserves are actually honored by `pmm_init` is
     * impractical here — `pmm_init` has already run by the time the
     * test harness starts, so a fresh registration has no effect on
     * the live buddy. The unit tests below cover the API shape
     * (count, add, reset, full-table behavior); the integration is
     * exercised on hardware via the call site in `kernel_main`. */
    RUN_TEST(test_user_reserve_count_starts_zero);
    RUN_TEST(test_user_reserve_add_increments_count);
    RUN_TEST(test_user_reserve_zero_size_rejected);
    RUN_TEST(test_user_reserve_reset_zeros_count);
    RUN_TEST(test_user_reserve_table_full_rejects);

    return UnityEnd();
}
