/*
 * test_pmm_poison.c - PMM_POISON / PMM_UNMAP diagnostics (#1207)
 *
 * Built only with PMM_POISON=ON (the file is empty otherwise). A write
 * through the PMM's Non-cacheable alias stands in for a device writing a
 * free page (it bypasses the linear map and the CPU caches, as DMA does).
 * The tests check that the allocation check and the scan report such a
 * write with the right block, offsets, byte count and zero flag, and that
 * a free page has no linear-map entry under PMM_UNMAP.
 *
 * Every finding caused here is passed to pmm_poison_note_injected(), so
 * the end-of-run summary line can say how many were not caused by a test
 * ("UNEXPECTED"). The tests use a local runner macro, not RUN_TEST: the
 * test-filter index (scripts/gen-test-index.py) reads RUN_TEST names
 * without the preprocessor, and these names must not change the index of
 * builds without the option.
 */

#if defined(CONFIG_PMM_POISON)

#include "unity.h"
#include "../include/pmm.h"
#include "../include/pmm_internal.h"
#include "../include/vmm.h"
#include <stdint.h>
#include <stdbool.h>

#define POISON_TAG  0xF4EE000000000000UL
#define POISON_TEST(fn) UnityRunTest(fn, #fn)

/* What the PMM's check should report for bytes [off, off + len) of the
 * page at pa after they were set to `val` (bytes, little endian). */
struct expect {
    bool any, nonzero;
    size_t first, last, nbytes;
};

static void expect_bytes(struct expect *e, uintptr_t pa, size_t off,
                         const uint8_t *val, size_t len)
{
    for (size_t k = 0; k < len; k++) {
        size_t o = off + k;
        uint64_t word = POISON_TAG | (uint64_t)(pa + (o & ~(size_t)7));
        uint8_t pat = (uint8_t)(word >> (8 * (o & 7)));
        if (pat == val[k]) {
            continue;
        }
        if (!e->any) {
            e->any = true;
            e->first = o;
        }
        e->last = o;
        e->nbytes++;
        if (val[k] != 0) {
            e->nonzero = true;
        }
    }
}

static void barrier_sy(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * A free order-0 page that the next pmm_alloc_pages(1) is very likely to
 * return: the upper page of an order-1 block, freed alone. Its buddy (the
 * lower page) stays allocated, so it is not merged and sits at the head of
 * the order-0 list. *lower is the page to free afterwards.
 */
static uintptr_t lone_free_page(uintptr_t *lower)
{
    uint8_t *q = pmm_alloc_pages(2);
    if (q == NULL) {
        return 0;
    }
    uintptr_t r = (uintptr_t)q + PAGE_SIZE;
    pmm_free_pages((void *)r, 1);
    *lower = (uintptr_t)q;
    return r;
}

static void test_poison_is_armed(void)
{
    struct pmm_poison_stats ps;
    pmm_poison_get_stats(&ps);
    TEST_ASSERT_TRUE(ps.armed);
    TEST_ASSERT_TRUE(ps.arm_free_pages > 0);
#if defined(CONFIG_PMM_UNMAP)
    TEST_ASSERT_TRUE(ps.unmap);
    TEST_ASSERT_TRUE(ps.l3_tables > 0);
#else
    TEST_ASSERT_FALSE(ps.unmap);
#endif
    TEST_ASSERT_EQUAL_HEX64(0x1000UL + VMM_PMM_ALIAS_OFFSET,
                            pmm_poison_alias_of(0x1000UL));

    /* The alias covers the whole heap, including the first pages, which
     * share a 2 MB block with the end of the image (a missing alias
     * would fault here). */
    struct pmm_stats st;
    pmm_get_stats(&st);
    const volatile uint64_t *first = (const volatile uint64_t *)
        pmm_poison_alias_of(st.heap_start);
    const volatile uint64_t *last = (const volatile uint64_t *)
        pmm_poison_alias_of(st.heap_end - PAGE_SIZE);
    TEST_ASSERT_TRUE(first[PAGE_SIZE / 8 - 1] != 0x5aUL || last[0] != 0x5aUL);
}

/* A freed page holds the pattern (past the two link words) and, under
 * PMM_UNMAP, has no linear-map entry; allocated again it is mapped. */
static void test_poison_freed_page_holds_pattern(void)
{
    uintptr_t lower;
    uintptr_t r = lone_free_page(&lower);
    TEST_ASSERT_TRUE(r != 0);
    TEST_ASSERT_EQUAL_INT(0, pmm_free_block_order(r));

    const volatile uint64_t *a = (const volatile uint64_t *)pmm_poison_alias_of(r);
    size_t bad = 0;
    for (size_t i = 2; i < PAGE_SIZE / 8; i++) {
        if (a[i] != (POISON_TAG | (uint64_t)(r + i * 8))) {
            bad++;
        }
    }
    TEST_ASSERT_EQUAL_UINT(0, bad);
#if defined(CONFIG_PMM_UNMAP)
    TEST_ASSERT_EQUAL_INT(0, pmm_poison_identity_state(r));
    TEST_ASSERT_EQUAL_HEX64(0, vmm_virt_to_phys(r));
    TEST_ASSERT_FALSE(vmm_is_mapped(r));
    /* The allocated lower half stays mapped, and translates. */
    TEST_ASSERT_EQUAL_INT(1, pmm_poison_identity_state(lower));
    TEST_ASSERT_EQUAL_HEX64(lower, vmm_virt_to_phys(lower));
#endif
    pmm_free_pages((void *)lower, 1);
}

/*
 * Proof 2 (allocation): a raw write into a free page is reported when the
 * page is allocated, with the block, the first and last differing offsets,
 * the byte count, the zero flag and the page; the page is then mapped
 * and the caller can use it.
 */
static void test_poison_alloc_reports_raw_write(void)
{
    struct pmm_poison_stats before, after;
    struct pmm_poison_report rep;
    uintptr_t lower;
    uintptr_t r = lone_free_page(&lower);
    TEST_ASSERT_TRUE(r != 0);
    TEST_ASSERT_EQUAL_INT(0, pmm_free_block_order(r));
    pmm_poison_get_stats(&before);

    /* 64 zero bytes at 0x100 and one 64-bit value at 0xF80. */
    volatile uint64_t *a = (volatile uint64_t *)pmm_poison_alias_of(r);
    static const uint8_t zeros[64];
    const uint64_t v = 0x1122334455667788UL;
    for (size_t i = 0x100 / 8; i < 0x140 / 8; i++) {
        a[i] = 0;
    }
    a[0xF80 / 8] = v;
    barrier_sy();
    pmm_poison_note_injected(1);

    struct expect e = {0};
    expect_bytes(&e, r, 0x100, zeros, sizeof(zeros));
    expect_bytes(&e, r, 0xF80, (const uint8_t *)&v, sizeof(v));

    uint8_t *got = pmm_alloc_pages(1);
    pmm_poison_get_stats(&after);
    if ((uintptr_t)got != r) {
        /* Another CPU took the page first: clear the damage with a scan
         * (it is counted as injected already) and say so. */
        pmm_free_pages(got, 1);
        (void)pmm_poison_scan("test_poison_alloc_reports_raw_write cleanup", true);
        pmm_free_pages((void *)lower, 1);
        TEST_IGNORE_MESSAGE("the injected page was not the next allocation");
        return;
    }
    TEST_ASSERT_EQUAL_UINT(before.alloc_findings + 1, after.alloc_findings);
    pmm_poison_last_report(&rep);
    TEST_ASSERT_FALSE(rep.scan);
    TEST_ASSERT_EQUAL_HEX64(r, rep.block);
    TEST_ASSERT_EQUAL_INT(0, rep.order);
    TEST_ASSERT_EQUAL_HEX64(r, rep.base);
    TEST_ASSERT_EQUAL_UINT(e.first, rep.first);
    TEST_ASSERT_EQUAL_UINT(e.last, rep.last);
    TEST_ASSERT_EQUAL_UINT(e.nbytes, rep.nbytes);
    TEST_ASSERT_FALSE(rep.all_zero);
    TEST_ASSERT_EQUAL_UINT(1, rep.pages_hit);
    TEST_ASSERT_EQUAL_HEX64(r, rep.first_page);
#if defined(CONFIG_PMM_UNMAP)
    TEST_ASSERT_EQUAL_INT(1, pmm_poison_identity_state(r));
#endif
    /* Usable through the linear map, and it still holds the write. */
    volatile uint64_t *idw = (volatile uint64_t *)got;
    TEST_ASSERT_EQUAL_HEX64(v, idw[0xF80 / 8]);
    idw[0] = 0xabcdUL;
    TEST_ASSERT_EQUAL_HEX64(0xabcdUL, idw[0]);
    pmm_free_pages(got, 1);
    pmm_free_pages((void *)lower, 1);
}

/*
 * Proof 2 (scan): a raw write of zeros into a free page is reported by the
 * next full scan as an all-zero finding on that page, the page is put back
 * to the pattern (a second scan reports nothing new), and the allocation
 * that follows finds nothing.
 */
static void test_poison_scan_reports_raw_write(void)
{
    struct pmm_poison_stats before, after;
    struct pmm_poison_report rep;
    uintptr_t lower;
    uintptr_t r = lone_free_page(&lower);
    TEST_ASSERT_TRUE(r != 0);
    pmm_poison_get_stats(&before);

    volatile uint64_t *a = (volatile uint64_t *)pmm_poison_alias_of(r);
    static const uint8_t zeros[64];
    for (size_t i = 0x40 / 8; i < 0x80 / 8; i++) {
        a[i] = 0;
    }
    barrier_sy();
    pmm_poison_note_injected(1);
    struct expect e = {0};
    expect_bytes(&e, r, 0x40, zeros, sizeof(zeros));

    unsigned n = pmm_poison_scan("test_poison_scan_reports_raw_write", true);
    pmm_poison_get_stats(&after);
    TEST_ASSERT_TRUE(n >= 1);
    TEST_ASSERT_EQUAL_UINT(before.scan_findings + 1, after.scan_findings);
    pmm_poison_last_report(&rep);
    TEST_ASSERT_TRUE(rep.scan);
    TEST_ASSERT_EQUAL_HEX64(r, rep.base);
    TEST_ASSERT_EQUAL_HEX64(r, rep.block);
    TEST_ASSERT_EQUAL_INT(0, rep.order);
    TEST_ASSERT_EQUAL_UINT(e.first, rep.first);
    TEST_ASSERT_EQUAL_UINT(e.last, rep.last);
    TEST_ASSERT_EQUAL_UINT(e.nbytes, rep.nbytes);
    TEST_ASSERT_TRUE(rep.all_zero);

    /* Re-poisoned: nothing new on the next scan or the allocation. */
    (void)pmm_poison_scan("test_poison_scan_reports_raw_write (again)", true);
    pmm_poison_get_stats(&before);
    TEST_ASSERT_EQUAL_UINT(after.scan_findings, before.scan_findings);
    uint8_t *got = pmm_alloc_pages(1);
    pmm_poison_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT(before.alloc_findings, after.alloc_findings);
    pmm_free_pages(got, 1);
    pmm_free_pages((void *)lower, 1);
}

/* The cheap scan (first and last 64 bytes of each free page) catches a
 * write inside those bytes; past the head's link words here. */
static void test_poison_edge_scan_reports_write_near_start(void)
{
    struct pmm_poison_stats before, after;
    struct pmm_poison_report rep;
    uintptr_t lower;
    uintptr_t r = lone_free_page(&lower);
    TEST_ASSERT_TRUE(r != 0);
    pmm_poison_get_stats(&before);

    volatile uint64_t *a = (volatile uint64_t *)pmm_poison_alias_of(r);
    a[4] = 0;                   /* bytes 0x20..0x27 */
    a[PAGE_SIZE / 8 - 1] = 0;   /* the page's last 8 bytes */
    barrier_sy();
    pmm_poison_note_injected(1);

    (void)pmm_poison_scan("test_poison_edge_scan_reports_write_near_start", false);
    pmm_poison_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT(before.scan_findings + 1, after.scan_findings);
    pmm_poison_last_report(&rep);
    TEST_ASSERT_EQUAL_HEX64(r, rep.base);
    TEST_ASSERT_TRUE(rep.first >= 0x20 && rep.first < 0x28);
    TEST_ASSERT_TRUE(rep.last >= PAGE_SIZE - 8 && rep.last < PAGE_SIZE);
    TEST_ASSERT_TRUE(rep.all_zero);
    pmm_free_pages((void *)lower, 1);
}

/*
 * #1207's shape: zeros across a page boundary inside a multi-page free
 * block. The cheap scan (first and last 64 bytes per page) finds it, and
 * the report covers the whole block: its extent (first and last differing
 * byte from the block start) and both pages, in one finding.
 */
static void test_poison_scan_reports_whole_block_extent(void)
{
    struct pmm_poison_stats before, after;
    struct pmm_poison_report rep;
    uint8_t *b = pmm_alloc_pages(8);                /* order 3 */
    TEST_ASSERT_NOT_NULL(b);
    uintptr_t up = (uintptr_t)b + 4 * PAGE_SIZE;    /* upper order-2 half */
    pmm_free_pages((void *)up, 4);                  /* lower half stays: no merge */
    TEST_ASSERT_EQUAL_INT(2, pmm_free_block_order(up));
    pmm_poison_get_stats(&before);

    /* 512 zero bytes: the last 256 of page 1 and the first 256 of page 2. */
    static const uint8_t zeros[512];
    volatile uint64_t *a = (volatile uint64_t *)pmm_poison_alias_of(up);
    for (size_t o = 2 * PAGE_SIZE - 256; o < 2 * PAGE_SIZE + 256; o += 8) {
        a[o / 8] = 0;
    }
    barrier_sy();
    pmm_poison_note_injected(1);
    struct expect e = {0};
    expect_bytes(&e, up, 2 * PAGE_SIZE - 256, zeros, sizeof(zeros));

    (void)pmm_poison_scan("test_poison_scan_reports_whole_block_extent", false);
    pmm_poison_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT(before.scan_findings + 1, after.scan_findings);
    pmm_poison_last_report(&rep);
    TEST_ASSERT_EQUAL_HEX64(up, rep.block);
    TEST_ASSERT_EQUAL_HEX64(up, rep.base);
    TEST_ASSERT_EQUAL_INT(2, rep.order);
    TEST_ASSERT_EQUAL_UINT(e.first, rep.first);
    TEST_ASSERT_EQUAL_UINT(e.last, rep.last);
    TEST_ASSERT_EQUAL_UINT(e.nbytes, rep.nbytes);
    TEST_ASSERT_TRUE(rep.all_zero);
    TEST_ASSERT_EQUAL_UINT(2, rep.pages_hit);
    TEST_ASSERT_EQUAL_HEX64(up + PAGE_SIZE, rep.first_page);

    /* The block holds the pattern again: the next full scan is clean. */
    (void)pmm_poison_scan("test_poison_scan_reports_whole_block_extent (again)", true);
    pmm_poison_get_stats(&before);
    TEST_ASSERT_EQUAL_UINT(after.scan_findings, before.scan_findings);
    pmm_free_pages(b, 4);
}

#if defined(CONFIG_PMM_UNMAP)
static void *split_table_page(void)
{
    return pmm_alloc_pages(1);
}

static uint64_t split_root[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t split_pool[1][ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));

/*
 * Regression for the first Jetson PMM_UNMAP run (76ec16fd): splitting a
 * 1 GB L1 block (RAM above 4 GB on Jetson; QEMU's RAM is on L2 tables, so
 * the real map never takes this path here) took its L2 table from the PMM,
 * inside the 1 GB it replaced; splitting that table's own 2 MB block then
 * unmapped the table mid break-before-make. Driven on a private root that
 * is never installed (its addresses are not accessed), at 0x1_0000_0000:
 * the L2 table must come from the static pool, and every 2 MB block must
 * get an L3 table mapping the same pages with the same attributes.
 */
static void test_unmap_split_l1_block_above_4gb(void)
{
    const uint64_t gb = 0x100000000UL;
    const uint32_t ram = VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_EXEC;
    for (size_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        split_root[i] = 0;
    }
    /* A 1 GB-aligned PA: the L2 block format is the L1 one. */
    split_root[L1_INDEX(gb)] = vmm_test_make_block_desc(gb, ram);
    uint64_t want_attr = split_root[L1_INDEX(gb)] & ~0x0000FFFFC0000000UL &
                         ~(uint64_t)PTE_TYPE_MASK;
    size_t used = 0, missed = 0;
    size_t n = vmm_pmm_split_core(split_root, gb, gb + L1_BLOCK_SIZE,
                                  split_table_page, split_pool, 1, &used, &missed);
    TEST_ASSERT_EQUAL_UINT(ENTRIES_PER_TABLE, n);
    TEST_ASSERT_EQUAL_UINT(0, missed);
    TEST_ASSERT_EQUAL_UINT(1, used);
    uint64_t d1 = split_root[L1_INDEX(gb)];
    TEST_ASSERT_EQUAL_HEX64(PTE_TYPE_TABLE, d1 & PTE_TYPE_MASK);
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(uintptr_t)split_pool[0], d1 & PTE_ADDR_MASK);
    size_t bad = 0;
    for (int e = 0; e < ENTRIES_PER_TABLE; e++) {
        uint64_t d2 = split_pool[0][e];
        if ((d2 & PTE_TYPE_MASK) != PTE_TYPE_TABLE) {
            bad++;
            continue;
        }
        uint64_t *l3 = (uint64_t *)(uintptr_t)(d2 & PTE_ADDR_MASK);
        uint64_t blk = gb + (uint64_t)e * BLOCK_SIZE;
        if (l3[0] != (want_attr | PTE_TYPE_PAGE | blk) ||
            l3[511] != (want_attr | PTE_TYPE_PAGE | (blk + 511 * PAGE_SIZE))) {
            bad++;
        }
        pmm_free_pages(l3, 1);
    }
    TEST_ASSERT_EQUAL_UINT(0, bad);

    /* Pool empty: a second 1 GB block is left alone and counted. */
    split_root[L1_INDEX(gb) + 1] = vmm_test_make_block_desc(gb + L1_BLOCK_SIZE, ram);
    n = vmm_pmm_split_core(split_root, gb + L1_BLOCK_SIZE, gb + 2 * L1_BLOCK_SIZE,
                           split_table_page, split_pool, 1, &used, &missed);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT(ENTRIES_PER_TABLE, missed);
    TEST_ASSERT_EQUAL_HEX64(PTE_TYPE_BLOCK, split_root[L1_INDEX(gb) + 1] & PTE_TYPE_MASK);
}

/* The other half of that defect: an L2 entry that lives inside the 2 MB
 * block it maps is never broken (the write after the break would fault);
 * it is skipped and counted. */
static void test_unmap_split_skips_entry_inside_its_block(void)
{
    /* A 2 MB-aligned block of our own, so not the image's last block. */
    uint64_t *l2 = pmm_alloc_pages(512);
    TEST_ASSERT_NOT_NULL(l2);
    for (size_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        split_root[i] = 0;
        l2[i] = 0;
    }
    uint64_t self = (uint64_t)(uintptr_t)l2;
    uint64_t blk = self & ~(uint64_t)(BLOCK_SIZE - 1);
    l2[L2_INDEX(self)] = vmm_test_make_block_desc(blk, VMM_FLAG_READ | VMM_FLAG_WRITE);
    split_root[L1_INDEX(self)] = PTE_TYPE_TABLE | (self & PTE_ADDR_MASK);
    uint64_t before = l2[L2_INDEX(self)];
    size_t used = 0, missed = 0;
    size_t n = vmm_pmm_split_core(split_root, blk, blk + BLOCK_SIZE,
                                  split_table_page, split_pool, 1, &used, &missed);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT(1, missed);
    TEST_ASSERT_EQUAL_HEX64(before, l2[L2_INDEX(self)]);
    pmm_free_pages(l2, 512);
}

/* A multi-page block: every page unmapped while free, mapped when
 * allocated, and a free-then-allocate of the same pages round-trips. */
static void test_unmap_block_round_trip(void)
{
    uint8_t *b = pmm_alloc_pages(8);
    TEST_ASSERT_NOT_NULL(b);
    for (size_t p = 0; p < 8; p++) {
        TEST_ASSERT_EQUAL_INT(1, pmm_poison_identity_state((uintptr_t)b + p * PAGE_SIZE));
        b[p * PAGE_SIZE + 5] = (uint8_t)p;
    }
    pmm_free_pages(b, 8);
    for (size_t p = 0; p < 8; p++) {
        TEST_ASSERT_TRUE(pmm_poison_identity_state((uintptr_t)b + p * PAGE_SIZE) != 1);
    }
}
#endif

int test_suite_pmm_poison(void)
{
    UnityBegin("PMM_POISON / PMM_UNMAP Tests (#1207)");
    POISON_TEST(test_poison_is_armed);
    POISON_TEST(test_poison_freed_page_holds_pattern);
    POISON_TEST(test_poison_alloc_reports_raw_write);
    POISON_TEST(test_poison_scan_reports_raw_write);
    POISON_TEST(test_poison_edge_scan_reports_write_near_start);
    POISON_TEST(test_poison_scan_reports_whole_block_extent);
#if defined(CONFIG_PMM_UNMAP)
    POISON_TEST(test_unmap_block_round_trip);
    POISON_TEST(test_unmap_split_l1_block_above_4gb);
    POISON_TEST(test_unmap_split_skips_entry_inside_its_block);
#endif
    return UnityEnd();
}

#else   /* !CONFIG_PMM_POISON: nothing (ISO C needs a declaration). */
typedef int test_pmm_poison_unused;
#endif
