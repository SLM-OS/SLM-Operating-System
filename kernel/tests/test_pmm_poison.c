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

#if defined(CONFIG_PMM_UNMAP)
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
#if defined(CONFIG_PMM_UNMAP)
    POISON_TEST(test_unmap_block_round_trip);
#endif
    return UnityEnd();
}

#else   /* !CONFIG_PMM_POISON: nothing (ISO C needs a declaration). */
typedef int test_pmm_poison_unused;
#endif
