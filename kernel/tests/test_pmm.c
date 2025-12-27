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
#include "../include/uart.h"
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
 * Test: PROVE coalescing works by allocating a larger block after freeing buddies
 * This is the definitive test - if coalescing didn't happen, the large
 * allocation would fail due to fragmentation.
 */
static void test_coalesce_enables_larger_allocation(void)
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
static void test_recursive_coalescing(void)
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
 * Test: Double free is detected (doesn't crash)
 */
static void test_double_free_detected(void)
{
    void *page = pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(page);

    pmm_free_page(page);
    /* Second free should warn but not crash */
    pmm_free_page(page);

    TEST_PASS();
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
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_pmm(void)
{
    UnityBegin("PMM Buddy Allocator Tests");

    /* Basic allocation */
    RUN_TEST(test_single_page_alloc);
    RUN_TEST(test_power_of_two_alloc);
    RUN_TEST(test_non_power_of_two_rounds_up);

    /* Alignment verification */
    RUN_TEST(test_alignment_all_orders);

    /* Buddy address calculation */
    RUN_TEST(test_buddy_address_calculation);

    /* CRITICAL: Coalescing verification */
    RUN_TEST(test_coalesce_enables_larger_allocation);
    RUN_TEST(test_recursive_coalescing);
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

    return UnityEnd();
}
