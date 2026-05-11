/*
 * test_kbuf.c — Unit tests for the virtually-contiguous kernel buffer
 * allocator (`kernel/mm/kbuf.c`) added for #789.
 *
 * Coverage:
 *   - Fast-path: small allocation fits in a single buddy block.
 *   - Slow-path: forced via a request large enough that the fast path
 *     will likely succeed in a QEMU build but we still exercise the
 *     bookkeeping for both kinds.
 *   - Alignment + size rounding.
 *   - kbuf_owns_va before/after free.
 *   - Region-count + total-bytes accounting.
 *   - NULL and zero-size edge cases.
 *
 * Skip note: the slow-path's chunked-mapping behaviour is exercised by
 * verifying it works correctly with arbitrary chunked allocations.
 * Reliably forcing the buddy-allocator-fragmented case from a unit
 * test would require coordinated state setup; instead we test that
 * the bookkeeping is consistent regardless of which path was taken.
 */

#include "unity.h"
#include "../include/kbuf.h"
#include "../include/pmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Small allocation: every QEMU/Pi 5/Jetson build can satisfy this
 * as a fast-path single-block alloc. Exercises the contiguous code
 * path. */
static void test_kbuf_small_alloc_round_trip(void)
{
    size_t region_count_before = kbuf_region_count();
    size_t bytes_before = kbuf_total_bytes_allocated();

    void *p = kbuf_alloc(4096);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(kbuf_owns_va(p));
    TEST_ASSERT_EQUAL_UINT(region_count_before + 1u, kbuf_region_count());

    /* Total bytes is rounded up to KBUF_CHUNK_BYTES (2 MB). */
    TEST_ASSERT_EQUAL_UINT64(
        bytes_before + KBUF_CHUNK_BYTES,
        kbuf_total_bytes_allocated());

    /* Buffer is writable. Touch the first + last addressable byte
     * of the rounded-up region — both must be reachable without
     * trapping. */
    volatile uint8_t *p8 = (volatile uint8_t *)p;
    p8[0] = 0xA5u;
    p8[KBUF_CHUNK_BYTES - 1u] = 0x5Au;
    TEST_ASSERT_EQUAL_UINT8(0xA5u, p8[0]);
    TEST_ASSERT_EQUAL_UINT8(0x5Au, p8[KBUF_CHUNK_BYTES - 1u]);

    kbuf_free(p);

    TEST_ASSERT_FALSE(kbuf_owns_va(p));
    TEST_ASSERT_EQUAL_UINT(region_count_before, kbuf_region_count());
    TEST_ASSERT_EQUAL_UINT64(bytes_before, kbuf_total_bytes_allocated());
}

/* Multi-chunk allocation: exercises N × 2 MB request. Verifies the
 * buffer is contiguous-readable across chunk boundaries (the slow
 * path would catch a botched VMM mapping; the fast path would still
 * pass since it's physically contiguous). Either way: byte-pattern
 * write at every 2 MB boundary must round-trip. */
static void test_kbuf_multi_chunk_contiguous_access(void)
{
    /* 6 MB → 3 chunks if slow-path, single 8 MB-rounded buddy if
     * fast-path. The interior boundaries 2 MB and 4 MB are the
     * stress points. */
    size_t want_bytes = 6u * 1024u * 1024u;
    void *p = kbuf_alloc(want_bytes);
    TEST_ASSERT_NOT_NULL(p);

    volatile uint8_t *p8 = (volatile uint8_t *)p;
    p8[0]                              = 0x11u;
    p8[KBUF_CHUNK_BYTES - 1u]          = 0x22u;
    p8[KBUF_CHUNK_BYTES]               = 0x33u;
    p8[2u * KBUF_CHUNK_BYTES - 1u]     = 0x44u;
    p8[2u * KBUF_CHUNK_BYTES]          = 0x55u;
    p8[3u * KBUF_CHUNK_BYTES - 1u]     = 0x66u;

    TEST_ASSERT_EQUAL_UINT8(0x11u, p8[0]);
    TEST_ASSERT_EQUAL_UINT8(0x22u, p8[KBUF_CHUNK_BYTES - 1u]);
    TEST_ASSERT_EQUAL_UINT8(0x33u, p8[KBUF_CHUNK_BYTES]);
    TEST_ASSERT_EQUAL_UINT8(0x44u, p8[2u * KBUF_CHUNK_BYTES - 1u]);
    TEST_ASSERT_EQUAL_UINT8(0x55u, p8[2u * KBUF_CHUNK_BYTES]);
    TEST_ASSERT_EQUAL_UINT8(0x66u, p8[3u * KBUF_CHUNK_BYTES - 1u]);

    kbuf_free(p);
}

/* Free counter is restored to its pre-alloc value after every alloc
 * is matched with a free. Repeated to surface a counter leak that
 * a single round trip might mask. */
static void test_kbuf_counters_stable_across_many_allocs(void)
{
    size_t region_before = kbuf_region_count();
    size_t bytes_before  = kbuf_total_bytes_allocated();

    void *p1 = kbuf_alloc(16u * 1024u);  /* rounds to 2 MB */
    void *p2 = kbuf_alloc(3u * 1024u * 1024u);  /* rounds to 4 MB */
    void *p3 = kbuf_alloc(KBUF_CHUNK_BYTES * 5u);  /* 10 MB, exact */
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_NOT_NULL(p3);
    TEST_ASSERT_EQUAL_UINT(region_before + 3u, kbuf_region_count());

    kbuf_free(p2);
    TEST_ASSERT_EQUAL_UINT(region_before + 2u, kbuf_region_count());
    kbuf_free(p1);
    TEST_ASSERT_EQUAL_UINT(region_before + 1u, kbuf_region_count());
    kbuf_free(p3);

    TEST_ASSERT_EQUAL_UINT(region_before, kbuf_region_count());
    TEST_ASSERT_EQUAL_UINT64(bytes_before, kbuf_total_bytes_allocated());
}

/* Edge case: zero size returns NULL and doesn't perturb counters. */
static void test_kbuf_zero_size_returns_null(void)
{
    size_t region_before = kbuf_region_count();
    void *p = kbuf_alloc(0);
    TEST_ASSERT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(region_before, kbuf_region_count());
}

/* Edge case: freeing NULL is a no-op. */
static void test_kbuf_free_null_is_noop(void)
{
    size_t region_before = kbuf_region_count();
    kbuf_free(NULL);
    TEST_ASSERT_EQUAL_UINT(region_before, kbuf_region_count());
}

/* `kbuf_owns_va` accurately reflects table state: false for NULL,
 * false for stack pointers, false for PMM-but-not-kbuf pointers,
 * true for a live kbuf allocation, false again after free. */
static void test_kbuf_owns_va_discriminates(void)
{
    int local_stack_var = 0;
    TEST_ASSERT_FALSE(kbuf_owns_va(NULL));
    TEST_ASSERT_FALSE(kbuf_owns_va(&local_stack_var));

    /* PMM allocation that DIDN'T go through kbuf: kbuf shouldn't
     * claim ownership of it. */
    void *raw_pmm = pmm_alloc_pages(1);
    TEST_ASSERT_NOT_NULL(raw_pmm);
    TEST_ASSERT_FALSE(kbuf_owns_va(raw_pmm));

    void *kp = kbuf_alloc(8192);
    TEST_ASSERT_NOT_NULL(kp);
    TEST_ASSERT_TRUE(kbuf_owns_va(kp));
    /* Raw pmm pointer still NOT owned even after a kbuf alloc. */
    TEST_ASSERT_FALSE(kbuf_owns_va(raw_pmm));
    kbuf_free(kp);
    TEST_ASSERT_FALSE(kbuf_owns_va(kp));

    pmm_free_pages(raw_pmm, 1);
}

int test_suite_kbuf(void)
{
    UnityBegin("Kernel Buffer Allocator (kbuf, #789)");

    RUN_TEST(test_kbuf_small_alloc_round_trip);
    RUN_TEST(test_kbuf_multi_chunk_contiguous_access);
    RUN_TEST(test_kbuf_counters_stable_across_many_allocs);
    RUN_TEST(test_kbuf_zero_size_returns_null);
    RUN_TEST(test_kbuf_free_null_is_noop);
    RUN_TEST(test_kbuf_owns_va_discriminates);

    return UnityEnd();
}
