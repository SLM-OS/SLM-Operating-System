/*
 * pmm.c - Physical Memory Manager for SLM-OS
 *
 * Buddy allocator for efficient O(log n) allocation of power-of-two page counts.
 * Automatically coalesces adjacent free blocks to reduce fragmentation.
 *
 * Orders:
 *   Order 0:  1 page     (4 KB)
 *   Order 1:  2 pages    (8 KB)
 *   Order 2:  4 pages    (16 KB)
 *   ...
 *   Order 10: 1024 pages (4 MB)
 *   Order 16: 65536 pages (256 MB)
 *   Order 18: 262144 pages (1 GB)
 *   Order 19: 524288 pages (2 GB) - maximum
 */

#include "pmm.h"
#include "pmm_internal.h"
#include "platform.h"
#include "uart.h"
#include "debug.h"
#include "spinlock.h"
#include "dtb.h"
#include "timer.h"
#if defined(CONFIG_PMM_POISON)
#include "vmm.h"     /* VMM_PMM_ALIAS_OFFSET, linear-map unmapping */
#include "cache.h"
#endif
#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "camrtc_layout.h"   /* CAMRTC_FRAME_BUFFER_PHYS / _END */
#endif
#include <stdbool.h>

/* External symbols from linker script */
extern char __kernel_end;

/* ==========================================================================
 * Buddy Allocator Configuration
 * ========================================================================== */

/* MAX_ORDER kept in sync with `PMM_MAX_ORDER` in `kernel/include/pmm.h`
 * — both must change together. The public stats struct
 * (`pmm_buddy_stats.free_counts[]`) is sized from `PMM_MAX_ORDER`. */
#define MAX_ORDER       19          /* Maximum order: 2^19 = 524288 pages (2 GB) */
_Static_assert(MAX_ORDER == PMM_MAX_ORDER,
               "MAX_ORDER must match PMM_MAX_ORDER in pmm.h");
#define MIN_BLOCK_SIZE  PAGE_SIZE   /* Minimum allocation: 4 KB */

/* Allocation-failure sentinel returned by the internal buddy helpers.
 * PA 0 is a legitimate page address (e.g. Pi 5 RAM_BASE = 0x0) once the
 * kernel image is placed above it, so 0 cannot be reused as "failure". */
#define PMM_ALLOC_FAIL  ((uintptr_t)-1)

/*
 * Per-page state (block_state[]).
 *
 * A free block is recorded twice: every page of it is "not allocated",
 * and its first page carries BLOCK_FREE_HEAD | order while the block is on
 * that order's free list. The merge test in buddy_free reads the head
 * value, so a buddy is merged only when it is one listed block of exactly
 * that order (#1207). The page-by-page state alone cannot tell one free
 * block of order n from two free blocks of order n-1 lying side by side.
 */
#define BLOCK_FREE      0           /* Free page, not the first of a listed block */
#define BLOCK_ALLOCATED 1
#define BLOCK_FREE_HEAD 0x80        /* | order: first page of a listed free block */

/* ==========================================================================
 * Data Structures
 * ========================================================================== */

/*
 * Free block header - stored at the start of each free block.
 * Since free blocks aren't being used, we can store metadata in them.
 */
struct free_block {
    struct free_block *next;
    struct free_block *prev;
};

/*
 * PMM_FB(a): the header of the free block at address a, as this file reads
 * and writes it; PMM_VA(a): the same for any byte of a free page. The free
 * lists hold linear-map (identity) addresses. With PMM_POISON armed, free
 * pages are reached through the PMM's Non-cacheable alias instead (see
 * "PMM_POISON / PMM_UNMAP" below; under PMM_UNMAP the identity mapping of
 * a free page is gone). Without the option both are plain casts.
 */
#if defined(CONFIG_PMM_POISON)
/* pmm_alias: VMM_PMM_ALIAS_OFFSET once the alias exists (pmm_init), else
 * 0. pmm_pstate: one word per heap page, 0 while the page is not armed
 * (reached through the identity map like any page), PMM_ST_PATTERN when
 * it holds the pattern, any other value its snapshot hash; an armed page
 * is reached through the alias. Arming runs page by page
 * (pmm_poison_arm_rest), so both kinds exist at once. */
static uintptr_t pmm_alias;
static uint64_t *pmm_pstate;
static uintptr_t pmm_heap_lo, pmm_heap_hi;
#define PMM_ST_PATTERN  1u

static inline uintptr_t pmm_va_off(uintptr_t a)
{
    if (pmm_pstate == NULL || a < pmm_heap_lo || a >= pmm_heap_hi) {
        return 0;
    }
    return pmm_pstate[(a - pmm_heap_lo) >> PAGE_SHIFT] ? pmm_alias : 0;
}
#define PMM_VA(a)   ((uintptr_t)(a) + pmm_va_off((uintptr_t)(a)))
#else
#define PMM_VA(a)   ((uintptr_t)(a))
#endif
#define PMM_FB(a)   ((struct free_block *)PMM_VA(a))

/*
 * Buddy allocator state
 */
static struct {
    struct free_block *free_lists[MAX_ORDER + 1];   /* One list per order */
    size_t free_counts[MAX_ORDER + 1];              /* Blocks free at each order */

    uintptr_t heap_start;           /* First allocatable address (aligned) */
    uintptr_t heap_end;             /* End of allocatable memory */
    size_t total_pages;             /* Total pages in heap */
    size_t free_pages;              /* Currently free pages */
    size_t reserved_pages;          /* Pages reserved for kernel */

    /* Statistics */
    size_t alloc_count;             /* Total allocations */
    size_t free_count;              /* Total frees */
    size_t split_count;             /* Number of block splits */
    size_t merge_count;             /* Number of block merges (coalesces) */

    bool initialized;
} buddy_state;

/* PMM lock - protects all buddy state
 * NOTE: Pre-MMU (spinlock_hw_enabled == 0) spin_lock falls through to a
 * barrier-only path on Pi 5/Jetson; once vmm_init enables the MMU, real
 * LDAXR/STXR runs and provides cross-CPU exclusion. Jetson's former
 * SPINLOCK_SKIP_LOCKING no-op let concurrent CPUs corrupt this free
 * list (issue #166).
 */
static spinlock_t pmm_lock = SPINLOCK_INIT;

/*
 * Block state bitmap - tracks whether each minimum-sized block is free/allocated.
 * We need 2 bits per block to track: FREE, ALLOCATED, or SPLIT.
 * But for simplicity, use 1 byte per minimum block (order 0).
 */
#define MAX_BLOCKS      (RAM_SIZE / PAGE_SIZE)
static uint8_t block_state[MAX_BLOCKS];

/* ==========================================================================
 * Helper Functions
 * ========================================================================== */

/*
 * Calculate ceiling of log2 for a value.
 * Used to find the order needed for a given page count.
 */
static inline unsigned int log2_ceil(size_t n)
{
    if (n <= 1) return 0;

    unsigned int order = 0;
    size_t val = 1;

    while (val < n) {
        val <<= 1;
        order++;
    }

    return order;
}

/*
 * Calculate pages for a given order: 2^order
 */
static inline size_t order_to_pages(unsigned int order)
{
    return (size_t)1 << order;
}

/*
 * Calculate block size in bytes for a given order
 */
static inline size_t order_to_size(unsigned int order)
{
    return order_to_pages(order) * PAGE_SIZE;
}

/*
 * Convert address to block index (relative to heap_start)
 */
static inline size_t addr_to_block_index(uintptr_t addr)
{
    size_t idx = (addr - buddy_state.heap_start) >> PAGE_SHIFT;
    ASSERT(idx < MAX_BLOCKS);
    return idx;
}

/*
 * Convert block index to address
 */
static inline uintptr_t block_index_to_addr(size_t index)
{
    return buddy_state.heap_start + (index << PAGE_SHIFT);
}

/*
 * Calculate the buddy address for a block of given order.
 * Buddy is found by XORing with the block size.
 */
static inline uintptr_t get_buddy_addr(uintptr_t addr, unsigned int order)
{
    return addr ^ order_to_size(order);
}

/*
 * Check if an address is aligned to a given order
 */
static inline bool is_aligned_to_order(uintptr_t addr, unsigned int order)
{
    return (addr & (order_to_size(order) - 1)) == 0;
}

/* ==========================================================================
 * PMM_CHECK: free-list and count assertions (#1207)
 *
 * Built only with `make ... PMM_CHECK=ON` (CMake -DPMM_CHECK=ON, which
 * defines CONFIG_PMM_CHECK). With the option off nothing here is compiled
 * and every PMM_CHK_* hook expands to nothing.
 *
 * With it on, each free-list operation checks its own preconditions and
 * panics, with the list state printed, at the operation that finds them
 * broken:
 *  - free_list_add: the block is already listed, or the old head's prev
 *    link is not NULL;
 *  - free_list_remove: the count is zero, the block is not listed at that
 *    order, or its neighbours do not link back to it;
 *  - free_list_pop: the list is empty while the count is not zero (the
 *    #1207 signature, reported before buddy_alloc splits from
 *    PMM_ALLOC_FAIL), the head is not listed at that order, or its links
 *    are not valid list elements;
 *  - buddy_free: the buddy range is free page by page but is not one
 *    listed block of that order;
 *  - pmm_free_pages: a page of the range is not allocated, or the order
 *    is larger than the order the block was allocated with (a smaller
 *    order is printed once per caller and allowed: a partial free).
 * pmm_check_walk() walks every list (links, order tags, page state,
 * length against free_counts, the page total); the test harness calls it
 * after every suite, so a link overwritten by something other than the
 * PMM is found at the first suite boundary after the write.
 *
 * The checks are O(1) per operation, apart from the page-state scan of a
 * freed range (the walk is O(free blocks) and runs only where it is
 * called). One tag byte per page:
 *   0             the page starts neither a listed nor an allocated block
 *   0x80 | order  the page starts a free block listed at that order
 *   0x40 | order  the page starts a block allocated at that order
 * ========================================================================== */
#if defined(CONFIG_PMM_CHECK)

#define PMM_TAG_LISTED(o)   ((uint8_t)(0x80u | (o)))
#define PMM_TAG_ALLOCED(o)  ((uint8_t)(0x40u | (o)))

static uint8_t   pmm_chk_tag[MAX_BLOCKS];
static uintptr_t pmm_chk_pc;            /* caller of the public entry point */
static unsigned  pmm_chk_reports;
static uintptr_t pmm_chk_partial_pcs[8];

static bool pmm_chk_ptr_ok(uintptr_t a, unsigned int order)
{
    return a >= buddy_state.heap_start && a < buddy_state.heap_end &&
           is_aligned_to_order(a, order);
}

static void pmm_chk_dump_list(unsigned int order)
{
    struct free_block *b = buddy_state.free_lists[order];
    uart_printf("  order %u: count=%lu head=0x%lx\n", order,
                (unsigned long)buddy_state.free_counts[order],
                (unsigned long)(uintptr_t)b);
    for (unsigned int i = 0; b && i < 8; i++) {
        uintptr_t a = (uintptr_t)b;
        if (!pmm_chk_ptr_ok(a, 0)) {
            uart_printf("    [%u] 0x%lx (outside the heap or not page-aligned: "
                        "not followed)\n", i, (unsigned long)a);
            break;
        }
        uart_printf("    [%u] 0x%lx next=0x%lx prev=0x%lx tag=0x%x state=%u\n",
                    i, (unsigned long)a, (unsigned long)(uintptr_t)PMM_FB(b)->next,
                    (unsigned long)(uintptr_t)PMM_FB(b)->prev,
                    (unsigned)pmm_chk_tag[addr_to_block_index(a)],
                    (unsigned)block_state[addr_to_block_index(a)]);
        b = PMM_FB(b)->next;
    }
}

/* Report an inconsistency found at `addr` (0: none) on the order-`order`
 * list and panic (noreturn). pmm_lock is held, so the state printed is the state at
 * the failing operation. */
static void pmm_chk_fail(const char *what, uintptr_t addr, unsigned int order)
{
    pmm_chk_reports++;
    uart_printf("\n[pmm-check] %s\n", what);
    uart_printf("  at 0x%lx order %u, caller pc=0x%lx\n",
                (unsigned long)addr, order, (unsigned long)pmm_chk_pc);
    if (addr && pmm_chk_ptr_ok(addr, 0)) {
        const volatile uint64_t *raw = (const volatile uint64_t *)PMM_VA(addr);
        uart_printf("  page tag=0x%x state=%u raw[0..3]=%016lx %016lx %016lx %016lx\n",
                    (unsigned)pmm_chk_tag[addr_to_block_index(addr)],
                    (unsigned)block_state[addr_to_block_index(addr)],
                    (unsigned long)raw[0], (unsigned long)raw[1],
                    (unsigned long)raw[2], (unsigned long)raw[3]);
    }
    uart_puts("  free lists (first 8 elements of the failing order):\n");
    pmm_chk_dump_list(order);
    for (unsigned int o = 0; o <= MAX_ORDER; o++) {
        if (o != order && (buddy_state.free_counts[o] || buddy_state.free_lists[o])) {
            uart_printf("  order %u: count=%lu head=0x%lx\n", o,
                        (unsigned long)buddy_state.free_counts[o],
                        (unsigned long)(uintptr_t)buddy_state.free_lists[o]);
        }
    }
    panic("pmm-check: %s (0x%lx, order %u)", what, (unsigned long)addr, order);
}

/* A listed element: in the heap, aligned for its order, tagged as listed
 * at that order, first page not allocated. (The tag is the option's own
 * record, kept apart from block_state[]'s head value so the two can
 * disagree.) */
static bool pmm_chk_listed(uintptr_t a, unsigned int order)
{
    return pmm_chk_ptr_ok(a, order) &&
           pmm_chk_tag[addr_to_block_index(a)] == PMM_TAG_LISTED(order) &&
           block_state[addr_to_block_index(a)] != BLOCK_ALLOCATED;
}

static void pmm_chk_add(uintptr_t addr, unsigned int order)
{
    struct free_block *head = buddy_state.free_lists[order];
    if (!pmm_chk_ptr_ok(addr, order)) {
        pmm_chk_fail("free_list_add: block outside the heap or misaligned",
                     addr, order);
        return;
    }
    if (pmm_chk_tag[addr_to_block_index(addr)] & 0x80u) {
        pmm_chk_fail("free_list_add: block is already on a free list",
                     addr, order);
    }
    if (head && (!pmm_chk_listed((uintptr_t)head, order) || PMM_FB(head)->prev)) {
        pmm_chk_fail("free_list_add: list head is not a listed block of this "
                     "order, or its prev link is not NULL",
                     (uintptr_t)head, order);
    }
    if (!head && buddy_state.free_counts[order]) {
        pmm_chk_fail("free_list_add: list empty but free_counts is not zero",
                     addr, order);
    }
    pmm_chk_tag[addr_to_block_index(addr)] = PMM_TAG_LISTED(order);
}

static void pmm_chk_remove(uintptr_t addr, unsigned int order)
{
    struct free_block *block = (struct free_block *)addr;
    struct free_block *bprev, *bnext;
    if (buddy_state.free_counts[order] == 0) {
        pmm_chk_fail("free_list_remove: free_counts is zero (would underflow)",
                     addr, order);
    }
    if (!pmm_chk_listed(addr, order)) {
        pmm_chk_fail("free_list_remove: block is not a listed block of this order",
                     addr, order);
        return;
    }
    bprev = PMM_FB(block)->prev;
    bnext = PMM_FB(block)->next;
    if (bprev ? (!pmm_chk_listed((uintptr_t)bprev, order) ||
                 PMM_FB(bprev)->next != block)
              : buddy_state.free_lists[order] != block) {
        pmm_chk_fail("free_list_remove: prev link (or the list head) does not "
                     "lead to the block", addr, order);
    }
    if (bnext && (!pmm_chk_listed((uintptr_t)bnext, order) ||
                  PMM_FB(bnext)->prev != block)) {
        pmm_chk_fail("free_list_remove: next link does not lead back to the block",
                     addr, order);
    }
    pmm_chk_tag[addr_to_block_index(addr)] = 0;
}

static void pmm_chk_pop(unsigned int order)
{
    struct free_block *block = buddy_state.free_lists[order];
    if (!block) {
        if (buddy_state.free_counts[order]) {
            pmm_chk_fail("free_list_pop: list empty but free_counts is not zero",
                         0, order);
        }
        return;
    }
    if (buddy_state.free_counts[order] == 0) {
        pmm_chk_fail("free_list_pop: list not empty but free_counts is zero",
                     (uintptr_t)block, order);
    }
    if (!pmm_chk_listed((uintptr_t)block, order)) {
        pmm_chk_fail("free_list_pop: list head is not a listed block of this order",
                     (uintptr_t)block, order);
        return;
    }
    if (PMM_FB(block)->prev) {
        pmm_chk_fail("free_list_pop: list head has a prev link",
                     (uintptr_t)block, order);
    }
    struct free_block *bnext = PMM_FB(block)->next;
    if (bnext && (!pmm_chk_listed((uintptr_t)bnext, order) ||
                  PMM_FB(bnext)->prev != block)) {
        pmm_chk_fail("free_list_pop: head's next link is not a listed block "
                     "that links back", (uintptr_t)block, order);
    }
    pmm_chk_tag[addr_to_block_index((uintptr_t)block)] = 0;
}

/* buddy_free found every page of the buddy range free. */
static void pmm_chk_buddy(uintptr_t buddy_addr, unsigned int order)
{
    if (!pmm_chk_listed(buddy_addr, order)) {
        pmm_chk_fail("buddy_free: buddy range is free but is not one listed "
                     "block of this order", buddy_addr, order);
    }
}

/* A block was allocated. Tags left inside it by earlier, smaller blocks
 * are cleared, so a later partial free is not compared with one. */
static void pmm_chk_alloced(uintptr_t addr, unsigned int order)
{
    if (addr != PMM_ALLOC_FAIL) {
        size_t base = addr_to_block_index(addr);
        size_t count = order_to_pages(order);
        for (size_t i = 1; i < count; i++) {
            pmm_chk_tag[base + i] = 0;
        }
        pmm_chk_tag[base] = PMM_TAG_ALLOCED(order);
    }
}

/* The list pmm_add_region_split carves out of a region (firmware
 * /memreserve/ entries first, then user reserves, then the DTB blob).
 * Printed once, on the first region: the totals, every firmware entry, and
 * the first 16 entries whose start or size is not page-aligned. */
static void pmm_chk_reserves(const dtb_memreserve_t *rsv, int n_rsv)
{
    static bool printed;
    static dtb_memreserve_t fw[DTB_MAX_MEMRESERVES];
    int n_fw, unaligned = 0;

    if (printed) {
        return;
    }
    printed = true;
    n_fw = dtb_get_memreserves(fw, DTB_MAX_MEMRESERVES);
    for (int i = 0; i < n_rsv; i++) {
        if ((rsv[i].addr | rsv[i].size) & (PAGE_SIZE - 1)) {
            if (unaligned++ < 16) {
                uart_printf("[pmm-check] reserve %d of %d is not page-aligned: "
                            "0x%lx + 0x%lx\n", i, n_rsv,
                            (unsigned long)rsv[i].addr,
                            (unsigned long)rsv[i].size);
            }
        }
    }
    for (int i = 0; i < n_fw; i++) {
        uart_printf("[pmm-check] firmware /memreserve/ %d: 0x%lx + 0x%lx\n", i,
                    (unsigned long)fw[i].addr, (unsigned long)fw[i].size);
    }
    uart_printf("[pmm-check] reserves: %d in all (%d firmware /memreserve/, the "
                "rest user reserves and the DTB blob), %d not page-aligned\n",
                n_rsv, n_fw, unaligned);
}

/* pmm_add_region is about to list [start, end): report a boundary that is
 * not page-aligned (pmm_init then lists, or drops, part of a page that a
 * reservation shares). Printed for the first few, counted for all. */
static unsigned pmm_chk_unaligned_regions;

static void pmm_chk_region(uintptr_t start, uintptr_t end)
{
    if ((start | end) & (PAGE_SIZE - 1)) {
        if (pmm_chk_unaligned_regions++ < 8) {
            uart_printf("[pmm-check] region 0x%lx-0x%lx is not page-aligned\n",
                        (unsigned long)start, (unsigned long)end);
        }
    }
}

/* End of pmm_init: walk the lists, then report the two states a correct
 * initial free list never has: a free block whose buddy is a free block
 * of the same order (never merged), and a free-page total in block_state
 * that differs from free_pages (blocks that overlap). Both are printed,
 * not fatal: they are what the per-operation checks then trip over. */
static unsigned pmm_chk_walk_locked(size_t *blocks_out);

static void pmm_chk_init_report(void)
{
    size_t blocks = 0, unmerged = 0, free_state = 0;

    (void)pmm_chk_walk_locked(&blocks);
    for (unsigned int o = 0; o < MAX_ORDER; o++) {
        for (struct free_block *b = buddy_state.free_lists[o]; b; b = PMM_FB(b)->next) {
            uintptr_t buddy = get_buddy_addr((uintptr_t)b, o);
            if (buddy > (uintptr_t)b && pmm_chk_listed(buddy, o)) {
                if (unmerged++ < 8) {
                    uart_printf("[pmm-check] init: free buddies 0x%lx and 0x%lx "
                                "(order %u) are not merged\n",
                                (unsigned long)(uintptr_t)b,
                                (unsigned long)buddy, o);
                }
            }
        }
    }
    for (size_t i = 0; i < buddy_state.total_pages; i++) {
        if (block_state[i] != BLOCK_ALLOCATED) {
            free_state++;
        }
    }
    uart_printf("[pmm-check] init: %lu free blocks, %lu free pages (%lu by page "
                "state%s), %lu unmerged buddy pair(s), %u region(s) not "
                "page-aligned\n",
                (unsigned long)blocks, (unsigned long)buddy_state.free_pages,
                (unsigned long)free_state,
                free_state == buddy_state.free_pages ? "" : ": MISMATCH",
                (unsigned long)unmerged, pmm_chk_unaligned_regions);
}

/* pmm_free_pages, after its own argument checks, under pmm_lock. */
static void pmm_chk_free(uintptr_t addr, unsigned int order)
{
    size_t base = addr_to_block_index(addr);
    size_t count = order_to_pages(order);
    uint8_t tag = pmm_chk_tag[base];

    if (addr + order_to_size(order) > buddy_state.heap_end) {
        pmm_chk_fail("pmm_free_pages: range runs past the heap end", addr, order);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (block_state[base + i] != BLOCK_ALLOCATED) {
            pmm_chk_fail("pmm_free_pages: a page of the range is not allocated "
                         "(over-sized or overlapping free)",
                         addr + (i << PAGE_SHIFT), order);
            return;
        }
    }
    if ((tag & 0xc0u) == 0x40u && (tag & 0x3fu) < order) {
        pmm_chk_fail("pmm_free_pages: order is larger than the block's "
                     "allocation order", addr, order);
    } else if ((tag & 0xc0u) == 0x40u && (tag & 0x3fu) > order) {
        for (unsigned int i = 0; i < 8; i++) {
            if (pmm_chk_partial_pcs[i] == pmm_chk_pc) {
                break;
            }
            if (pmm_chk_partial_pcs[i] == 0) {
                pmm_chk_partial_pcs[i] = pmm_chk_pc;
                uart_printf("[pmm-check] partial free: order %u at 0x%lx, "
                            "allocated at order %u (caller pc=0x%lx; printed "
                            "once per caller)\n", order, (unsigned long)addr,
                            (unsigned)(tag & 0x3fu), (unsigned long)pmm_chk_pc);
                break;
            }
        }
    }
    pmm_chk_tag[base] = 0;
}

/* Walk every free list. The first inconsistency panics, so a return is a
 * clean walk (0). Caller holds pmm_lock or runs before any other CPU
 * allocates. */
static unsigned pmm_chk_walk_locked(size_t *blocks_out)
{
    unsigned before = pmm_chk_reports;
    size_t pages = 0, blocks = 0;

    pmm_chk_pc = 0;
    for (unsigned int o = 0; o <= MAX_ORDER; o++) {
        size_t n = 0;
        struct free_block *prev = NULL;
        for (struct free_block *b = buddy_state.free_lists[o]; b; b = PMM_FB(b)->next) {
            uintptr_t a = (uintptr_t)b;
            if (n >= buddy_state.free_counts[o]) {
                pmm_chk_fail("walk: list longer than free_counts", a, o);
                break;
            }
            if (!pmm_chk_listed(a, o)) {
                pmm_chk_fail("walk: element is not a listed block of this order",
                             a, o);
                break;
            }
            if (PMM_FB(b)->prev != prev) {
                pmm_chk_fail("walk: prev link does not point at the previous "
                             "element", a, o);
                break;
            }
            if (block_state[addr_to_block_index(a) + order_to_pages(o) - 1] ==
                BLOCK_ALLOCATED) {
                pmm_chk_fail("walk: last page of a listed block is not free",
                             a, o);
                break;
            }
            prev = b;
            n++;
        }
        if (pmm_chk_reports == before && n != buddy_state.free_counts[o]) {
            pmm_chk_fail("walk: list shorter than free_counts (chain ends early "
                         "at this element)", (uintptr_t)prev, o);
        }
        pages += n << o;
        blocks += n;
    }
    if (pmm_chk_reports == before && pages != buddy_state.free_pages) {
        pmm_chk_fail("walk: pages on the lists differ from free_pages", 0, 0);
    }
    if (blocks_out) {
        *blocks_out = blocks;
    }
    return pmm_chk_reports - before;
}

unsigned pmm_check_walk(const char *where)
{
    size_t blocks = 0;
    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    unsigned bad = pmm_chk_walk_locked(&blocks);
    size_t free_pages = buddy_state.free_pages;
    spin_unlock_irqrestore(&pmm_lock, flags);
    uart_printf("[pmm-check] %s: %s (%lu free blocks, %lu free pages)\n",
                where, bad ? "INCONSISTENT" : "ok",
                (unsigned long)blocks, (unsigned long)free_pages);
    return bad;
}

#define PMM_CHK_ENTER()          (pmm_chk_pc = (uintptr_t)__builtin_return_address(0))
#define PMM_CHK_ADD(a, o)        pmm_chk_add((a), (o))
#define PMM_CHK_REMOVE(a, o)     pmm_chk_remove((a), (o))
#define PMM_CHK_POP(o)           pmm_chk_pop(o)
#define PMM_CHK_BUDDY(a, o)      pmm_chk_buddy((a), (o))
#define PMM_CHK_ALLOCED(a, o)    pmm_chk_alloced((a), (o))
#define PMM_CHK_FREE(a, o)       pmm_chk_free((a), (o))
#define PMM_CHK_INIT() \
    do { \
        for (size_t i_ = 0; i_ < MAX_BLOCKS; i_++) { \
            pmm_chk_tag[i_] = 0; \
        } \
    } while (0)
#define PMM_CHK_REGION(s, e)     pmm_chk_region((s), (e))
#define PMM_CHK_RESERVES(r, n)   pmm_chk_reserves((r), (n))
#define PMM_CHK_WALK_INIT()      pmm_chk_init_report()

#else  /* !CONFIG_PMM_CHECK */

#define PMM_CHK_ENTER()          ((void)0)
#define PMM_CHK_ADD(a, o)        ((void)0)
#define PMM_CHK_REMOVE(a, o)     ((void)0)
#define PMM_CHK_POP(o)           ((void)0)
#define PMM_CHK_BUDDY(a, o)      ((void)0)
#define PMM_CHK_ALLOCED(a, o)    ((void)0)
#define PMM_CHK_FREE(a, o)       ((void)0)
#define PMM_CHK_INIT()           ((void)0)
#define PMM_CHK_REGION(s, e)     ((void)0)
#define PMM_CHK_RESERVES(r, n)   ((void)0)
#define PMM_CHK_WALK_INIT()      ((void)0)

#endif /* CONFIG_PMM_CHECK */

/* ==========================================================================
 * Free-list verification and evidence dumps (#1207), in every build
 *
 * On jetson-nano-2 after kexec, 1 to 2 suite boots in 10 end with a count
 * without a list (#1207, open). In the one boot captured with this code,
 * a listed order-4 block read all zero across its start (both links, its
 * first 64 bytes and the 64 bytes before it) while block_state[] still
 * marked it as a listed head, so no list operation had taken it. What
 * makes the bytes read zero is not known. One candidate: a device writing
 * DRAM under a line this CPU holds. A header write lost to DC IVAC is
 * ruled out on A76 / A78AE if mmu.S's note holds (run as DC CIVAC). This
 * code exists to catch such a block with its contents. No table: about
 * 200 bytes (per-order "last reported" addresses, a counter, a label).
 *
 * pmm_verify_free_lists(where) walks every list: each element inside the
 * heap, aligned for its order, marked as the head of a listed block of
 * that order in block_state[], linked back to the element before it, and
 * the length equal to free_counts[]. A finding prints the time, `where`,
 * the list, the element that holds the bad link with its first 64 bytes,
 * the 64 bytes before it and the page states around it, then every
 * order's count against its walked length. It does not stop the kernel,
 * and the same finding is printed once. free_list_pop makes the same
 * report at the moment it takes a block whose `next` is NULL while more
 * blocks are counted, before it clears the block's header.
 * ========================================================================== */

struct pmm_finding {
    const char *what;
    unsigned int order;
    size_t walked;
    uintptr_t holder;       /* element that holds the bad link; 0: the list head */
    uintptr_t link;         /* the bad link's value */
};

static uintptr_t pmm_verify_reported[MAX_ORDER + 1];
static unsigned  pmm_verify_findings;
static const char *pmm_verify_where = "boot";   /* last suite boundary */

static uint64_t pmm_now_ms(void)
{
    uint64_t freq = timer_get_frequency();
    return freq >= 1000 ? timer_get_count() / (freq / 1000) : 0;
}

static bool pmm_block_ptr_ok(uintptr_t a, unsigned int order)
{
    return a >= buddy_state.heap_start && a < buddy_state.heap_end &&
           is_aligned_to_order(a, order);
}

/*
 * True when the 64 bytes at `a` may be read for a dump: they lie in one
 * page, inside the heap range, and that page is free according to
 * block_state[]. Only pages added by pmm_add_region are ever free, and
 * those are mapped RAM. The heap range itself is not enough: it spans
 * holes that are not mapped (on Jetson the OP-TEE carve-out
 * 0xBE000000-0xC2000000 and everything from 0x240000000 to heap_end), and
 * a read there faults inside the dump. An allocated page is not read
 * either: it may be one of those holes, which are marked allocated.
 */
static bool pmm_dump_readable(uintptr_t a)
{
    if (a < buddy_state.heap_start || a + 64 > buddy_state.heap_end ||
        a + 64 < a) {
        return false;
    }
    if ((a >> PAGE_SHIFT) != ((a + 63) >> PAGE_SHIFT)) {
        return false;
    }
    return block_state[addr_to_block_index(a & ~(uintptr_t)(PAGE_SIZE - 1))] !=
           BLOCK_ALLOCATED;
}

/* Eight 64-bit words starting at `a`, when they may be read. */
static void pmm_dump_words(const char *label, uintptr_t a)
{
    if (!pmm_dump_readable(a)) {
        uart_printf("  %s 0x%lx: not in a free page of the heap, not read\n",
                    label, (unsigned long)a);
        return;
    }
    const volatile uint64_t *w = (const volatile uint64_t *)PMM_VA(a);
    uart_printf("  %s 0x%lx: %016lx %016lx %016lx %016lx\n", label,
                (unsigned long)a, (unsigned long)w[0], (unsigned long)w[1],
                (unsigned long)w[2], (unsigned long)w[3]);
    uart_printf("  %s 0x%lx: %016lx %016lx %016lx %016lx\n", label,
                (unsigned long)(a + 32), (unsigned long)w[4],
                (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7]);
}

/* The block at `a`: its first 64 bytes, the 64 bytes before it, and the
 * page state of its first page and of the pages on either side. */
static void pmm_dump_block(uintptr_t a)
{
    if (!pmm_block_ptr_ok(a, 0)) {
        uart_printf("  block 0x%lx: outside the heap or not page-aligned\n",
                    (unsigned long)a);
        return;
    }
    size_t idx = addr_to_block_index(a);
    uart_printf("  block 0x%lx: page state 0x%x, page before 0x%x, page after 0x%x\n",
                (unsigned long)a, (unsigned)block_state[idx],
                idx ? (unsigned)block_state[idx - 1] : 0xffu,
                idx + 1 < MAX_BLOCKS ? (unsigned)block_state[idx + 1] : 0xffu);
    pmm_dump_words("first 64 bytes at", a);
    pmm_dump_words("64 bytes before, at", a - 64);
}

/* Walk one list. Returns its length; fills *f and returns early on the
 * first inconsistency (f->what stays NULL on a clean list). */
static size_t pmm_walk_list(unsigned int o, struct pmm_finding *f)
{
    size_t n = 0;
    uintptr_t prev = 0;
    uintptr_t a = (uintptr_t)buddy_state.free_lists[o];

    f->what = NULL;
    f->order = o;
    while (a) {
        if (!pmm_block_ptr_ok(a, o)) {
            f->what = "link points outside the heap or is misaligned for the order";
        } else if (block_state[addr_to_block_index(a)] !=
                   (uint8_t)(BLOCK_FREE_HEAD | o)) {
            f->what = "element is not marked as a listed free block of this order";
        } else if ((uintptr_t)PMM_FB(a)->prev != prev) {
            f->what = "element's prev link is not the element before it";
        } else if (n >= buddy_state.free_counts[o]) {
            f->what = "list is longer than its count";
        }
        if (f->what) {
            f->walked = n;
            f->holder = prev;
            f->link = a;
            return n;
        }
        prev = a;
        a = (uintptr_t)PMM_FB(a)->next;
        n++;
    }
    if (n != buddy_state.free_counts[o]) {
        f->what = "list ends early: next link is NULL with blocks still counted";
        f->walked = n;
        f->holder = prev;
        f->link = 0;
    }
    return n;
}

/* Count against walked length for every order that has either. */
static void pmm_dump_all_orders(void)
{
    uart_puts("  order: count / walked length / head\n");
    for (unsigned int o = 0; o <= MAX_ORDER; o++) {
        struct pmm_finding f;
        size_t n = pmm_walk_list(o, &f);
        if (buddy_state.free_counts[o] || buddy_state.free_lists[o]) {
            uart_printf("  %5u: %lu / %lu / 0x%lx%s\n", o,
                        (unsigned long)buddy_state.free_counts[o],
                        (unsigned long)n,
                        (unsigned long)(uintptr_t)buddy_state.free_lists[o],
                        f.what ? "  <-- inconsistent" : "");
        }
    }
}

/* Print one finding. Caller holds pmm_lock. */
static void pmm_report_finding(const char *where, const struct pmm_finding *f)
{
    pmm_verify_findings++;
    uart_printf("\n[pmm-verify] FINDING %u at %lu ms (%s; last suite boundary: %s)\n",
                pmm_verify_findings, (unsigned long)pmm_now_ms(), where,
                pmm_verify_where);
    uart_printf("  order %u: %s\n", f->order, f->what);
    uart_printf("  count %lu, walked %lu, head 0x%lx, bad link value 0x%lx, "
                "held by 0x%lx%s\n",
                (unsigned long)buddy_state.free_counts[f->order],
                (unsigned long)f->walked,
                (unsigned long)(uintptr_t)buddy_state.free_lists[f->order],
                (unsigned long)f->link, (unsigned long)f->holder,
                f->holder ? "" : " (the list head)");
    if (f->holder) {
        uart_puts("  the element holding the bad link:\n");
        pmm_dump_block(f->holder);
    }
    if (f->link && f->link != f->holder && pmm_block_ptr_ok(f->link, 0)) {
        uart_puts("  the element the link points at:\n");
        pmm_dump_block(f->link);
    }
    pmm_dump_all_orders();
    uart_puts("[pmm-verify] end of finding\n");
}

/* Walk every list and report each order's finding once. Caller holds
 * pmm_lock. Returns the number of orders with a finding. */
static unsigned pmm_verify_locked(const char *where)
{
    unsigned bad = 0;

    for (unsigned int o = 0; o <= MAX_ORDER; o++) {
        struct pmm_finding f;
        (void)pmm_walk_list(o, &f);
        if (!f.what) {
            continue;
        }
        bad++;
        /* The same finding again (same holder): already printed. */
        uintptr_t key = f.holder ? f.holder : (uintptr_t)1;
        if (pmm_verify_reported[o] != key) {
            pmm_verify_reported[o] = key;
            pmm_report_finding(where, &f);
        }
    }
    return bad;
}

unsigned pmm_verify_free_lists(const char *where)
{
    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    unsigned bad = pmm_verify_locked(where);
    if (where) {
        pmm_verify_where = where;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return bad;
}

/* From the timer tick: walk the lists if pmm_lock is free. Never waits,
 * prints only on a new finding. */
void pmm_verify_tick(void)
{
    if (!buddy_state.initialized) {
        return;
    }
    irq_flags_t flags = irq_save();
    if (spin_trylock(&pmm_lock)) {
        (void)pmm_verify_locked("timer tick");
        spin_unlock(&pmm_lock);
    }
    irq_restore(flags);
}

unsigned pmm_verify_finding_count(void)
{
    return pmm_verify_findings;
}

/* ==========================================================================
 * PMM_POISON / PMM_UNMAP: free-page poison and unmap (#1207, debug only)
 *
 * Built only with `make ... PMM_POISON=ON [PMM_UNMAP=ON]` (CMake
 * -DPMM_POISON=ON / -DPMM_UNMAP=ON, CONFIG_PMM_POISON / CONFIG_PMM_UNMAP).
 * ARM64 only. With the options off nothing below is compiled and every
 * PMM_POISON_* hook expands to nothing.
 *
 * What it is for. #1207: on Jetson kexec boots a listed free block was
 * found with zeroed bytes across its start. A device writing DRAM (a GPU
 * engine left running by Linux, or one the GPU tests start), or CPU code
 * writing through a stale pointer to memory it gave back, are the main
 * candidates. These options tell them apart:
 *  - PMM_UNMAP: a free page has no linear-map (identity) entry, so a CPU
 *    access to it takes a translation fault with the ELR of the access,
 *    and the fault dump says the address is in a PMM-free page.
 *  - PMM_POISON: every free byte holds a known pattern. A write that does
 *    not fault (a device, or a CPU path through another mapping) changes
 *    it, and the change is reported when the block is next allocated
 *    (always) and by pmm_poison_scan() (the test harness runs it around
 *    and inside test_suite_gpu).
 *
 * Pattern. The 64-bit word at physical address pa holds
 * PMM_POISON_TAG | pa (0xF4EE << 48 | pa). It is not zero, a page copied
 * from elsewhere differs from it (the address is in it), and as a pointer
 * it is non-canonical, so a stale pointer loaded from poison faults.
 * The only free bytes that are not poison are the two link words (next,
 * prev) at the start of each listed head; they are put back to the
 * pattern when the block leaves its list (free_list_pop,
 * free_list_remove: allocation, or absorption in a merge).
 *
 * Mapping and the cache argument. The PMM reaches free pages only through
 * a second mapping of RAM at PA + VMM_PMM_ALIAS_OFFSET (vmm.c), Normal
 * Non-cacheable: reads go to DRAM and writes do not allocate lines.
 *  - PMM_UNMAP: freeing a block (1) clears the valid bit of its identity
 *    L3 entries and invalidates the TLB (broadcast, TLBI ...IS; under VHE
 *    with HCR_EL2.{E2H,TGE} = {1,1} the EL1 TLBI forms act on the EL2&0
 *    regime the kernel runs in), so no new line can be allocated through
 *    the identity mapping; then (2) cleans and invalidates the block to
 *    the PoC by VA through the alias (DC CIVAC acts on the PA whatever
 *    the attributes of the VA used): the last owner's dirty lines reach
 *    DRAM and no cached copy remains; then (3) writes the pattern through
 *    the alias. After that no cacheable alias of the page exists anywhere
 *    (kbuf's second WB mapping of a chunk is removed before the chunk is
 *    freed), so a write by a device that does not snoop the CPU caches
 *    lands in DRAM and the next NC read sees it; no CPU line can hide it
 *    or be written back over it. Allocation checks the pattern through
 *    the alias, then sets the identity entries valid again.
 *  - PMM_POISON alone: the identity mapping stays, so a stale-pointer
 *    write can sit dirty in a cache and a speculative fetch can hold a
 *    clean, stale copy. Allocation therefore first cleans and invalidates
 *    the block (dirty CPU data reaches DRAM and is checked, stale clean
 *    copies are dropped) and then reads DRAM through the alias: the
 *    allocation check is exact in both modes. The scan does not clean
 *    first (it would double its cost), so in this mode it can miss a CPU
 *    write still held dirty in a cache, and, on a core whose NC reads look
 *    up the caches, a device write under a stale clean line; both are
 *    still found at allocation. Under PMM_UNMAP the scan is exact for
 *    every unmapped page.
 *  - Pages PMM_UNMAP cannot unmap (the part of the 2 MB block that holds
 *    the end of the kernel image, the block holding heap_start, or a
 *    split that found no table page) keep the PMM_POISON-alone rules;
 *    arming prints how many.
 *
 * Arming. Pages freed before the MMU and the vmm are up need the same
 * treatment, so nothing is done per operation until pmm_poison_arm(),
 * called at the end of pmm_init (vmm_init runs before pmm_init on ARM64,
 * secondaries are not up and IRQs are masked). It builds the alias, under
 * PMM_UNMAP gives the heap's linear map 4 KB pages (break-before-make per
 * entry, tables from the PMM), then unmaps, cleans and poisons every page
 * already free.
 *
 * Reports (allocation and scan) give the block and order, the first and
 * last differing byte offsets, how many bytes differ, whether every
 * differing byte is zero, which pages are hit, the first and last
 * differing 64-byte line, the time and the last suite or test boundary
 * (pmm_verify_where). They do not stop the kernel. The first 16 are
 * printed in full, the next 240 in one line, the rest only counted. A
 * scan reports the whole free block holding a damaged page (offsets and
 * extent across the block, and on Jetson kexec the offset from the MNIST
 * hand-off pushbuf) and puts the pattern back on it, so a later report of
 * the same block is a new write.
 *
 * Cost. Every free writes its block once and every allocation reads it
 * once, through the NC alias, with IRQs masked under pmm_lock for the
 * free. pmm_poison_scan() reads every free page (or, `full` false, the
 * first and last 64 bytes of each), 512 pages per pmm_lock hold.
 * ========================================================================== */
#if defined(CONFIG_PMM_POISON)
#if defined(PLATFORM_X86_64)
#error "PMM_POISON / PMM_UNMAP are ARM64 only (#1207)"
#endif

#define PMM_POISON_TAG          0xF4EE000000000000UL
#define PMM_POISON_DETAIL_MAX   16u     /* findings printed in full */
#define PMM_POISON_LINE_MAX     256u    /* findings printed at all */
#define PMM_SCAN_CHUNK_PAGES    512u    /* pages checked per pmm_lock hold */
/* The cheap scan checks lines 0 and 63 of every free page, and line 11
 * (page offset 0x2c0): on Jetson kexec a second writer, not cured by the
 * early L2 flush, changes one 64-byte line at offset 0x102c0 of a 1 MB
 * aligned region during test_suite_gpu (0x1223102c0, 0x1234102c0,
 * 0x11e4102c0, 0x10ef102c0), so the per-test scans name the test. */
#define PMM_EDGE_EXTRA_LINE     11u     /* 0x2c0 / 64 */

static spinlock_t pmm_poison_lock = SPINLOCK_INIT;  /* stats and reports */
static bool pmm_poison_unmap_on;                    /* identity unmapping armed */
static struct pmm_poison_stats pmm_pstats;
static struct pmm_poison_report pmm_plast;
static unsigned pmm_preports;                       /* findings, all kinds */

static inline uint64_t pmm_poison_word(uintptr_t pa)
{
    return PMM_POISON_TAG | (uint64_t)pa;
}

/* The alias address of pa (the option's own checks and writes). */
#define PMM_ALIAS(pa)   ((uintptr_t)(pa) + VMM_PMM_ALIAS_OFFSET)

static void pmm_st_set(size_t idx, size_t n, uint64_t v)
{
    for (size_t k = 0; k < n; k++) {
        pmm_pstate[idx + k] = v;
    }
}

/* What one check found. Offsets are from the checked range's base. */
struct pmm_pdiff {
    bool any, nonzero;
    size_t first, last, nbytes;
    size_t first_line, last_line;
    size_t pages_hit;
    uintptr_t first_page, last_page;
    size_t snap_lines;          /* changed 64-byte lines in snapshot pages */
    bool snap_unlocated;        /* a page hash changed, no line checksum did */
};

/* Write the pattern over [pa, pa + len) through the alias (len a multiple
 * of 8, pa 8-byte aligned). */
static void pmm_p_fill(uintptr_t pa, size_t len)
{
    uint64_t *w = (uint64_t *)PMM_ALIAS(pa);
    uint64_t v = pmm_poison_word(pa);
    for (size_t i = 0; i < len / 8; i++, v += 8) {
        w[i] = v;
    }
}

/* Compare [pa, pa + len) with the pattern through the alias; add what
 * differs to *d, as offsets from `base`. */
static void pmm_p_check(uintptr_t base, uintptr_t pa, size_t len,
                        struct pmm_pdiff *d)
{
    const uint64_t *w = (const uint64_t *)PMM_ALIAS(pa);
    uint64_t exp = pmm_poison_word(pa);
    for (size_t i = 0; i < len / 8; i++, exp += 8) {
        uint64_t v = w[i];
        if (__builtin_expect(v == exp, 1)) {
            continue;
        }
        uint64_t x = v ^ exp;
        size_t off0 = (pa - base) + i * 8;
        for (unsigned b = 0; b < 8; b++) {
            if (((x >> (8 * b)) & 0xffu) == 0) {
                continue;
            }
            size_t off = off0 + b;
            if (!d->any) {
                d->any = true;
                d->first = off;
                d->first_line = off & ~(size_t)63;
            }
            d->last = off;
            d->last_line = off & ~(size_t)63;
            d->nbytes++;
            if ((v >> (8 * b)) & 0xffu) {
                d->nonzero = true;
            }
        }
    }
}

/* The listed free block that contains page-aligned address a: its head
 * is a aligned down to the block's order, marked BLOCK_FREE_HEAD | order.
 * Reads block_state[] only (no lock: also used from the fault dump). */
static bool pmm_p_find_block(uintptr_t a, uintptr_t *blk, unsigned int *order)
{
    for (unsigned int o = 0; o <= MAX_ORDER; o++) {
        uintptr_t b = a & ~(uintptr_t)(order_to_size(o) - 1);
        if (b < buddy_state.heap_start) {
            break;
        }
        if (block_state[addr_to_block_index(b)] == (uint8_t)(BLOCK_FREE_HEAD | o)) {
            *blk = b;
            *order = o;
            return true;
        }
    }
    return false;
}

#if defined(CONFIG_PMM_POISON_SNAPSHOT)
/*
 * PMM_POISON_SNAPSHOT: pages already free when the option arms keep their
 * contents. On Jetson kexec boots some of them hold state that Linux left
 * for devices still in use (the inherited GPU channel failed to come up
 * after they were poisoned: "[GA10B-INHERIT] FECS not ready"). For each
 * such page arming records a 64-bit hash of bytes 16..4095 and an 8-bit
 * checksum of each 64-byte line (line 0 from byte 16: the first 16 bytes
 * of a free page are the PMM's link words whenever it heads a block).
 * Allocation and the scans compare a snapshot page with its record, so a
 * change is found and placed to a 64-byte line (a changed line whose
 * checksum happens to match, 1 in 256, is missed in the placement but not
 * in the page hash). Allocation drops the record; pages freed after
 * arming get the pattern. Memory: 72 bytes per heap page, from the PMM at
 * arming (Jetson: about 1.82 M pages, 16 MB + 128 MB blocks; QEMU 2 MB +
 * 16 MB); arming reads every free page once through the NC alias.
 */
static uint8_t *pmm_snap_sum;       /* per heap page, 64 line checksums */
static uintptr_t pmm_snap_sum_pa;
static size_t pmm_snap_sum_pages;

#define PMM_SNAP_K  0x9E3779B97F4A7C15UL

static uint8_t pmm_s_line(const uint64_t *w, unsigned line)
{
    uint64_t x = line;
    for (unsigned k = line == 0 ? 2 : 0; k < 8; k++) {
        x = (x ^ w[line * 8 + k]) * PMM_SNAP_K;
    }
    return (uint8_t)(x >> 56);
}

/* Hash and line checksums of the page at pa. The page is read once
 * through the alias into a cached buffer (one pass over uncached memory;
 * an earlier version read it twice). Never returns 0 or PMM_ST_PATTERN. */
static uint64_t pmm_s_compute(uintptr_t pa, uint8_t sums[64])
{
    uint64_t buf[PAGE_SIZE / 8];
    const uint64_t *w = (const uint64_t *)PMM_ALIAS(pa);
    for (unsigned k = 0; k < PAGE_SIZE / 8; k += 2) {
        buf[k] = w[k];
        buf[k + 1] = w[k + 1];
    }
    uint64_t h = 0x6A09E667F3BCC908UL;
    for (unsigned k = 2; k < PAGE_SIZE / 8; k++) {
        h = (h ^ buf[k]) * PMM_SNAP_K;
        h ^= h >> 29;
    }
    for (unsigned l = 0; l < 64; l++) {
        sums[l] = pmm_s_line(buf, l);
    }
    h |= 1;
    return h == PMM_ST_PATTERN ? 3 : h;
}

static void pmm_s_take(uintptr_t pa, size_t idx)
{
    pmm_pstate[idx] = pmm_s_compute(pa, &pmm_snap_sum[idx * 64]);
}

static bool pmm_s_has(size_t idx)
{
    return pmm_pstate != NULL && pmm_pstate[idx] > PMM_ST_PATTERN;
}

/* Mark line l of the snapshot page at pa (offsets from base) changed. */
static void pmm_s_mark_line(uintptr_t base, uintptr_t pa, unsigned l,
                            struct pmm_pdiff *d)
{
    const uint64_t *w = (const uint64_t *)PMM_ALIAS(pa);
    size_t off = (pa - base) + (size_t)l * 64;
    if (!d->any) {
        d->any = true;
        d->first = off;
        d->first_line = off;
    }
    d->last = off + 63;
    d->last_line = off;
    d->nbytes += 64;
    d->snap_lines++;
    for (unsigned k = l == 0 ? 2 : 0; k < 8; k++) {
        if (w[l * 8 + k] != 0) {
            d->nonzero = true;
        }
    }
}

/* Compare the snapshot page at pa with its record: every line (full) or
 * the first and last line. */
static void pmm_s_check(uintptr_t base, uintptr_t pa, size_t idx, bool full,
                        struct pmm_pdiff *d)
{
    const uint8_t *rec = &pmm_snap_sum[idx * 64];
    if (!full) {
        const uint64_t *w = (const uint64_t *)PMM_ALIAS(pa);
        if (pmm_s_line(w, 0) != rec[0]) {
            pmm_s_mark_line(base, pa, 0, d);
        }
        if (pmm_s_line(w, PMM_EDGE_EXTRA_LINE) != rec[PMM_EDGE_EXTRA_LINE]) {
            pmm_s_mark_line(base, pa, PMM_EDGE_EXTRA_LINE, d);
        }
        if (pmm_s_line(w, 63) != rec[63]) {
            pmm_s_mark_line(base, pa, 63, d);
        }
        return;
    }
    uint8_t now[64];
    if (pmm_s_compute(pa, now) == pmm_pstate[idx]) {
        return;
    }
    size_t before = d->snap_lines;
    for (unsigned l = 0; l < 64; l++) {
        if (now[l] != rec[l]) {
            pmm_s_mark_line(base, pa, l, d);
        }
    }
    if (d->snap_lines == before) {
        /* The hash changed but no checksum did: the whole page. */
        d->snap_unlocated = true;
        for (unsigned l = 0; l < 64; l++) {
            pmm_s_mark_line(base, pa, l, d);
        }
    }
}
#else
static bool pmm_s_has(size_t idx)
{
    (void)idx;
    return false;
}
#endif

/*
 * Check the free page pg (heap index idx) against what it should hold:
 * the pattern from byte `skip` (16 for a listed head, else 0), or, for a
 * PMM_POISON_SNAPSHOT page, its arming record. full false: the first and
 * last 64 bytes only. Adds to *d (offsets from base). True on a change.
 */
static bool pmm_p_check_page(uintptr_t base, uintptr_t pg, size_t idx,
                             size_t skip, bool full, struct pmm_pdiff *d)
{
    size_t before = d->nbytes;
    if (pmm_pstate[idx] == 0) {
        return false;       /* not armed yet: nothing to compare with */
    }
#if defined(CONFIG_PMM_POISON_SNAPSHOT)
    if (pmm_s_has(idx)) {
        pmm_s_check(base, pg, idx, full, d);
    } else
#endif
    if (full) {
        pmm_p_check(base, pg + skip, PAGE_SIZE - skip, d);
    } else {
        pmm_p_check(base, pg + skip, 64 - skip, d);
        pmm_p_check(base, pg + PMM_EDGE_EXTRA_LINE * 64, 64, d);
        pmm_p_check(base, pg + PAGE_SIZE - 64, 64, d);
    }
    if (d->nbytes == before) {
        return false;
    }
    if (d->pages_hit++ == 0) {
        d->first_page = pg;
    }
    d->last_page = pg;
    return true;
}

/* After a scan reported it: the pattern again, or a new snapshot. */
static void pmm_p_restore_page(uintptr_t pg, size_t idx, size_t skip)
{
#if defined(CONFIG_PMM_POISON_SNAPSHOT)
    if (pmm_s_has(idx)) {
        pmm_s_take(pg, idx);
        return;
    }
#endif
    if (pmm_pstate[idx] == PMM_ST_PATTERN) {
        pmm_p_fill(pg + skip, PAGE_SIZE - skip);
    }
}

/* The MNIST (kind 0) hand-off pushbuf on a Jetson kexec boot, or 0
 * (ga10b_handoff_reserve.c overrides this). #1207's damaged blocks sat
 * 1.0 to 1.2 MB above it, so reports give the offset. */
__attribute__((weak)) uint64_t pmm_poison_ref_pushbuf(void)
{
    return 0;
}

/* Jetson: GPU page-table lookup of a damaged range (ga10b_handoff_reserve.c
 * overrides this no-op). */
__attribute__((weak)) void pmm_poison_gpu_lookup(uint64_t lo, uint64_t hi)
{
    (void)lo;
    (void)hi;
}

static void pmm_p_print_ref(uintptr_t first, uintptr_t last)
{
    uint64_t ref = pmm_poison_ref_pushbuf();
    if (ref != 0) {
        uart_printf("  MNIST hand-off pushbuf 0x%lx: damaged bytes at +0x%lx .. +0x%lx "
                    "(signed: %ld .. %ld)\n", (unsigned long)ref,
                    (unsigned long)(first - ref), (unsigned long)(last - ref),
                    (long)(first - ref), (long)(last - ref));
    }
}

/* One 64-byte line through the alias, with what the pattern would be. */
static void pmm_p_dump_line(const char *label, uintptr_t line)
{
    if (pmm_s_has(addr_to_block_index(line & ~(uintptr_t)(PAGE_SIZE - 1)))) {
        uart_printf("  (%s is in a snapshot page: its content at arming was not "
                    "kept, only checksums)\n", label);
    }
    const volatile uint64_t *w = (const volatile uint64_t *)PMM_ALIAS(line);
    uart_printf("  %s 0x%lx: %016lx %016lx %016lx %016lx\n", label,
                (unsigned long)line, (unsigned long)w[0], (unsigned long)w[1],
                (unsigned long)w[2], (unsigned long)w[3]);
    uart_printf("  %s 0x%lx: %016lx %016lx %016lx %016lx\n", label,
                (unsigned long)(line + 32), (unsigned long)w[4],
                (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7]);
    uart_printf("  (pattern there: %016lx, +8 per word)\n",
                (unsigned long)pmm_poison_word(line));
}

/*
 * Record and print one finding: an allocation check (scan false: the
 * allocated block) or a scan (scan true: the whole free block holding the
 * damaged page, order -1 and one page when no listed head was found).
 * base == block; offsets in *d are from it, across the whole block.
 */
static void pmm_p_report(bool scan, const char *where, uintptr_t block,
                         int order, uintptr_t base, const struct pmm_pdiff *d)
{
    irq_flags_t flags = spin_lock_irqsave(&pmm_poison_lock);
    unsigned n = scan ? ++pmm_pstats.scan_findings : ++pmm_pstats.alloc_findings;
    unsigned k = pmm_preports++;
    uint64_t ms = pmm_now_ms();

    pmm_pstats.bytes += d->nbytes;
    pmm_plast.scan = scan;
    pmm_plast.where = where;
    pmm_plast.block = block;
    pmm_plast.order = order;
    pmm_plast.base = base;
    pmm_plast.first = d->first;
    pmm_plast.last = d->last;
    pmm_plast.nbytes = d->nbytes;
    pmm_plast.all_zero = !d->nonzero;
    pmm_plast.pages_hit = d->pages_hit;
    pmm_plast.first_page = d->first_page;

    if (k < PMM_POISON_DETAIL_MAX) {
        if (scan) {
            uart_printf("\n[pmm-poison] FINDING (scan %u) at %lu ms: scan \"%s\": "
                        "free block 0x%lx order %d (%lu KB), checked whole\n", n,
                        (unsigned long)ms, where, (unsigned long)block, order,
                        order >= 0 ? (unsigned long)(order_to_size((unsigned)order) / 1024)
                                   : 4UL);
        } else {
            uart_printf("\n[pmm-poison] FINDING (allocation %u) at %lu ms: %s "
                        "allocated block 0x%lx order %d (%lu KB)\n", n,
                        (unsigned long)ms, where, (unsigned long)block, order,
                        (unsigned long)(order_to_size((unsigned)order) / 1024));
        }
        uart_printf("  last boundary: %s\n", pmm_verify_where);
        uart_printf("  %lu byte(s) differ from the pattern, offsets 0x%lx to 0x%lx "
                    "from 0x%lx (PA 0x%lx .. 0x%lx, extent %lu bytes); the "
                    "differing bytes are %s\n",
                    (unsigned long)d->nbytes, (unsigned long)d->first,
                    (unsigned long)d->last, (unsigned long)base,
                    (unsigned long)(base + d->first), (unsigned long)(base + d->last),
                    (unsigned long)(d->last - d->first + 1),
                    d->nonzero ? "NOT all zero" : "all zero");
        pmm_p_print_ref(base + d->first, base + d->last);
        if (d->snap_lines) {
            uart_printf("  of those, %lu byte(s) are %lu whole 64-byte line(s) of "
                        "snapshot pages whose checksum changed%s (placed to the "
                        "line; the bytes they held at arming are not known)\n",
                        (unsigned long)(d->snap_lines * 64),
                        (unsigned long)d->snap_lines,
                        d->snap_unlocated ? ", and a page whose hash changed with no "
                                            "line checksum changed (whole page counted)"
                                          : "");
        }
        uart_printf("  pages hit: %lu (first 0x%lx, last 0x%lx)\n",
                    (unsigned long)d->pages_hit, (unsigned long)d->first_page,
                    (unsigned long)d->last_page);
        pmm_poison_gpu_lookup(base + d->first, base + d->last + 1);
        pmm_p_dump_line("first differing line", base + d->first_line);
        if (d->last_line != d->first_line) {
            pmm_p_dump_line("last differing line", base + d->last_line);
        }
        uart_puts("[pmm-poison] end of finding\n");
    } else if (k < PMM_POISON_LINE_MAX) {
        uart_printf("[pmm-poison] FINDING (%s %u) at %lu ms: base 0x%lx block 0x%lx "
                    "order %d, %lu bytes 0x%lx..0x%lx, %s, %lu page(s), pushbuf "
                    "%+ld; %s / %s\n",
                    scan ? "scan" : "allocation", n, (unsigned long)ms,
                    (unsigned long)base, (unsigned long)block, order,
                    (unsigned long)d->nbytes, (unsigned long)d->first,
                    (unsigned long)d->last, d->nonzero ? "nonzero" : "all zero",
                    (unsigned long)d->pages_hit,
                    pmm_poison_ref_pushbuf() ?
                        (long)(base + d->first - pmm_poison_ref_pushbuf()) : 0L,
                    where, pmm_verify_where);
    } else if (k == PMM_POISON_LINE_MAX) {
        uart_puts("[pmm-poison] further findings are counted, not printed\n");
    }
    spin_unlock_irqrestore(&pmm_poison_lock, flags);
}

/* A block leaves the allocator (after pmm_lock is dropped: the block is
 * the caller's and no other path touches it). Clean and invalidate the
 * armed pages (see the cache argument), check them, drop their state,
 * then map the block again. Pages not armed yet are not checked. */
static void pmm_p_on_alloc(uintptr_t addr, unsigned int order, const char *who)
{
    if (!pmm_alias) {
        return;
    }
    size_t pages = order_to_pages(order);
    size_t idx0 = addr_to_block_index(addr);
    struct pmm_pdiff d = {0};
    bool armed = false;

    for (size_t p = 0; p < pages && !armed; p++) {
        armed = pmm_pstate[idx0 + p] != 0;
    }
    if (armed) {
        cache_invalidate_range((const void *)PMM_ALIAS(addr), order_to_size(order));
        for (size_t p = 0; p < pages; p++) {
            (void)pmm_p_check_page(addr, addr + (p << PAGE_SHIFT), idx0 + p, 0,
                                   true, &d);
        }
        pmm_st_set(idx0, pages, 0);
    }
    if (d.any) {
        pmm_p_report(false, who, addr, (int)order, addr, &d);
    }
#if defined(CONFIG_PMM_UNMAP)
    if (pmm_poison_unmap_on) {
        (void)vmm_linear_set_present(addr, pages, true, false);
    }
#endif
}

/* A block enters the allocator (pmm_free_pages after its checks, or
 * pmm_add_region), under pmm_lock: unmap, clean and invalidate, mark it
 * pattern (from here PMM_VA reaches it through the alias), write it. */
static void pmm_p_on_free(uintptr_t addr, unsigned int order)
{
    if (!pmm_alias) {
        return;
    }
    size_t size = order_to_size(order);
#if defined(CONFIG_PMM_UNMAP)
    if (pmm_poison_unmap_on) {
        (void)vmm_linear_set_present(addr, order_to_pages(order), false, false);
    }
#endif
    cache_invalidate_range((const void *)PMM_ALIAS(addr), size);
    pmm_st_set(addr_to_block_index(addr), order_to_pages(order), PMM_ST_PATTERN);
    pmm_p_fill(addr, size);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* A block left its list (popped, removed for a merge or alloc_low): on a
 * pattern page its link words become pattern again. */
static void pmm_p_unlinked(uintptr_t addr)
{
    if (pmm_alias && pmm_pstate[addr_to_block_index(addr)] == PMM_ST_PATTERN) {
        pmm_p_fill(addr, 2 * sizeof(uint64_t));
    }
}

/* Tables the options need are taken from the lowest free addresses
 * (pmm_alloc_pages_low), just above the image, and arming prints where:
 * on a kexec boot, "free" pages can hold device state Linux left
 * (PMM_POISON_SNAPSHOT), so the overwritten range should be known. */
static void *pmm_p_table_page(void)
{
    return pmm_alloc_pages_low(1);
}

static size_t pmm_pstate_pages;
static uintptr_t pmm_pstate_pa;

/*
 * Arming, part 1, at the end of pmm_init (boot CPU only, IRQs masked: the
 * only point where break-before-make on live linear-map entries is safe):
 * the alias, the 4 KB linear map (PMM_UNMAP), and the state tables. From
 * here every free is unmapped and poisoned and every allocation checked;
 * the pages that are free now are left alone until part 2. Fast (the
 * first Jetson run did everything here and took 39 s, which made all five
 * secondaries time out waiting for the scheduler).
 */
static void pmm_poison_arm(void)
{
    uint64_t t0 = pmm_now_ms();
    size_t l3 = 0, missed = 0;
    irq_flags_t irq = irq_save();

    if (vmm_pmm_alias_init(pmm_p_table_page) != 0) {
        irq_restore(irq);
        WARN("PMM_POISON: the alias mapping could not be built; not armed");
        return;
    }
#if defined(CONFIG_PMM_UNMAP)
    l3 = vmm_pmm_split_linear(buddy_state.heap_start, buddy_state.heap_end,
                              pmm_p_table_page, &missed);
#endif
    pmm_pstate_pages = (buddy_state.total_pages * sizeof(uint64_t) +
                        PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *st = pmm_alloc_pages_low(pmm_pstate_pages);
#if defined(CONFIG_PMM_POISON_SNAPSHOT)
    pmm_snap_sum_pages = (buddy_state.total_pages * 64 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *sum = pmm_alloc_pages_low(pmm_snap_sum_pages);
    if (sum == NULL) {
        st = NULL;
    }
    pmm_snap_sum = sum;
    pmm_snap_sum_pa = (uintptr_t)sum;
#endif
    if (st == NULL) {
        irq_restore(irq);
        WARN("PMM_POISON: no memory for the state tables; not armed");
        return;
    }
    for (size_t i = 0; i < buddy_state.total_pages; i++) {
        st[i] = 0;
    }
    pmm_pstate_pa = (uintptr_t)st;
    pmm_heap_lo = buddy_state.heap_start;
    pmm_heap_hi = buddy_state.heap_end;
    pmm_pstate = st;
    __asm__ volatile("dmb ish" ::: "memory");
    pmm_alias = VMM_PMM_ALIAS_OFFSET;
#if defined(CONFIG_PMM_UNMAP)
    pmm_poison_unmap_on = true;
#endif
    irq_restore(irq);

    pmm_pstats.unmap = pmm_poison_unmap_on;
    pmm_pstats.l3_tables = l3;
    pmm_pstats.l3_missed = missed;
    uart_printf("[pmm-poison] arming part 1: alias at PA+0x%lx (Normal NC); unmap %s: "
                "%lu L3 tables, %lu 2 MB blocks not split; state table 0x%lx +%lu "
                "pages%s; %lu ms. The pages free now are armed by part 2 "
                "(pmm_poison_arm_rest).\n",
                (unsigned long)VMM_PMM_ALIAS_OFFSET,
                pmm_poison_unmap_on ? "on" : "off", (unsigned long)l3,
                (unsigned long)missed, (unsigned long)pmm_pstate_pa,
                (unsigned long)pmm_pstate_pages,
#if defined(CONFIG_PMM_POISON_SNAPSHOT)
                ", snapshot line table also allocated",
#else
                "",
#endif
                (unsigned long)(pmm_now_ms() - t0));
}

/*
 * Arming, part 2 (the test harness calls it before the first suite, with
 * every CPU up): every page still free and not armed, 512 pages per
 * pmm_lock hold so other CPUs keep allocating. Per chunk: unmap the
 * identity entries (PMM_UNMAP) and invalidate the TLB, clean and
 * invalidate through the alias (the links pmm_add_region wrote and any
 * line left from before the kexec reach DRAM), then record a snapshot
 * (PMM_POISON_SNAPSHOT: nothing is written) or write the pattern, and
 * mark the page armed. The page's links are reached through the alias
 * from then on (PMM_VA follows the state word).
 */
void pmm_poison_arm_rest(void)
{
    if (!pmm_alias || pmm_pstats.armed) {
        return;
    }
    uint64_t t0 = pmm_now_ms();
    size_t armed = 0, mapped = 0;
    for (size_t i0 = 0; i0 < buddy_state.total_pages; i0 += PMM_SCAN_CHUNK_PAGES) {
        size_t i1 = i0 + PMM_SCAN_CHUNK_PAGES;
        if (i1 > buddy_state.total_pages) {
            i1 = buddy_state.total_pages;
        }
        irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
        size_t n_here = 0;
        for (size_t i = i0; i < i1;) {
            if (block_state[i] == BLOCK_ALLOCATED || pmm_pstate[i] != 0) {
                i++;
                continue;
            }
            size_t n = 1;
            while (i + n < i1 && block_state[i + n] != BLOCK_ALLOCATED &&
                   pmm_pstate[i + n] == 0) {
                n++;
            }
#if defined(CONFIG_PMM_UNMAP)
            mapped += vmm_linear_set_present(block_index_to_addr(i), n, false, true);
#else
            mapped += n;
#endif
            n_here += n;
            i += n;
        }
        if (n_here == 0) {
            spin_unlock_irqrestore(&pmm_lock, flags);
            continue;
        }
#if defined(CONFIG_PMM_UNMAP)
        vmm_invalidate_tlb_all();
#endif
        for (size_t i = i0; i < i1; i++) {
            if (block_state[i] == BLOCK_ALLOCATED || pmm_pstate[i] != 0) {
                continue;
            }
            uintptr_t pg = block_index_to_addr(i);
            cache_invalidate_range((const void *)PMM_ALIAS(pg), PAGE_SIZE);
#if defined(CONFIG_PMM_POISON_SNAPSHOT)
            pmm_s_take(pg, i);
#else
            size_t skip = (block_state[i] & BLOCK_FREE_HEAD) ? 2 * sizeof(uint64_t) : 0;
            pmm_p_fill(pg + skip, PAGE_SIZE - skip);
            pmm_pstate[i] = PMM_ST_PATTERN;
#endif
            armed++;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        spin_unlock_irqrestore(&pmm_lock, flags);
    }
    pmm_pstats.armed = true;
    pmm_pstats.arm_free_pages = armed;
    pmm_pstats.arm_mapped_pages = mapped;
    pmm_pstats.arm_ms = pmm_now_ms() - t0;
#if defined(CONFIG_PMM_POISON_SNAPSHOT)
    pmm_pstats.snapshot = true;
    uart_printf("[pmm-poison] snapshot mode: the pages free at arming were NOT "
                "written; each has a page hash and 64 line checksums (line table "
                "0x%lx +%lu pages, from the lowest free addresses); pages freed "
                "from now on get the pattern\n", (unsigned long)pmm_snap_sum_pa,
                (unsigned long)pmm_snap_sum_pages);
#endif
    uart_printf("[pmm-poison] armed: %lu free pages %s (pattern %016lx | pa), "
                "alias at PA+0x%lx (Normal NC); unmap %s: %lu L3 tables, %lu "
                "2 MB blocks not split, %lu free pages unmapped, %lu left mapped; "
                "%lu ms (part 2, all CPUs up)\n",
                (unsigned long)armed,
                pmm_snap_mode() ? "snapshotted" : "poisoned",
                (unsigned long)PMM_POISON_TAG,
                (unsigned long)VMM_PMM_ALIAS_OFFSET,
                pmm_poison_unmap_on ? "on" : "off",
                (unsigned long)pmm_pstats.l3_tables,
                (unsigned long)pmm_pstats.l3_missed,
                (unsigned long)(pmm_poison_unmap_on ? armed - mapped : 0),
                (unsigned long)(pmm_poison_unmap_on ? mapped : armed),
                (unsigned long)pmm_pstats.arm_ms);
}

unsigned pmm_poison_scan(const char *where, bool full)
{
    unsigned bad = pmm_verify_free_lists(where);
    if (!pmm_alias) {
        return bad;
    }
    uint64_t t0 = pmm_now_ms();
    size_t checked = 0;
    unsigned found = 0;
    for (size_t i0 = 0; i0 < buddy_state.total_pages; i0 += PMM_SCAN_CHUNK_PAGES) {
        size_t i1 = i0 + PMM_SCAN_CHUNK_PAGES;
        irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
        if (i1 > buddy_state.total_pages) {
            i1 = buddy_state.total_pages;
        }
        for (size_t i = i0; i < i1; i++) {
            uint8_t st = block_state[i];
            if (st == BLOCK_ALLOCATED) {
                continue;
            }
            uintptr_t pg = block_index_to_addr(i);
            size_t skip = (st & BLOCK_FREE_HEAD) ? 2 * sizeof(uint64_t) : 0;
            struct pmm_pdiff d = {0};
            checked++;
            if (!pmm_p_check_page(pg, pg, i, skip, full, &d)) {
                continue;
            }
            /* Damage: check and report the whole free block holding the
             * page once (the extent across the block, every byte), put the
             * pattern back on all of it, and continue after it. */
            uintptr_t blk = pg;
            unsigned int bo = 0;
            bool have = pmm_p_find_block(pg, &blk, &bo);
            size_t npages = have ? order_to_pages(bo) : 1;
            struct pmm_pdiff bd = {0};
            size_t bidx = addr_to_block_index(blk);
            for (size_t p = 0; p < npages; p++) {
                size_t bskip = p == 0 && have ? 2 * sizeof(uint64_t) : skip;
                (void)pmm_p_check_page(blk, blk + (p << PAGE_SHIFT), bidx + p,
                                       bskip, true, &bd);
            }
            pmm_p_report(true, where, blk, have ? (int)bo : -1, blk, &bd);
            for (size_t p = 0; p < npages; p++) {
                size_t bskip = p == 0 && have ? 2 * sizeof(uint64_t) : skip;
                pmm_p_restore_page(blk + (p << PAGE_SHIFT), bidx + p, bskip);
            }
            found++;
            /* Skip the rest of the block in this chunk; pages past the
             * chunk are checked again (they hold the pattern now). */
            size_t next = addr_to_block_index(blk) + npages;
            if (next > i1) {
                next = i1;
            }
            if (next > i + 1) {
                checked += next - (i + 1);
                i = next - 1;
            }
        }
        spin_unlock_irqrestore(&pmm_lock, flags);
    }
    uint64_t ms = pmm_now_ms() - t0;
    irq_flags_t flags = spin_lock_irqsave(&pmm_poison_lock);
    pmm_pstats.scans++;
    pmm_pstats.last_scan_ms = ms;
    pmm_pstats.last_scan_pages = checked;
    if (ms > pmm_pstats.max_scan_ms) {
        pmm_pstats.max_scan_ms = ms;
    }
    spin_unlock_irqrestore(&pmm_poison_lock, flags);
    uart_printf("[pmm-poison] scan %s (%s): %lu free pages, %u damaged, %u list "
                "finding(s), %lu ms\n", where, full ? "full" : "lines 0, 11 (0x2c0), 63",
                (unsigned long)checked, found, bad, (unsigned long)ms);
    return bad + found;
}

bool pmm_snap_mode(void)
{
#if defined(CONFIG_PMM_POISON_SNAPSHOT)
    return true;
#else
    return false;
#endif
}

uintptr_t pmm_poison_find_snapshot_page(void)
{
    uintptr_t found = 0;
    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    for (size_t i = 0; i < buddy_state.total_pages && found == 0; i++) {
        if (block_state[i] == BLOCK_FREE && pmm_s_has(i)) {
            found = block_index_to_addr(i);
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return found;
}

void pmm_poison_get_stats(struct pmm_poison_stats *out)
{
    irq_flags_t flags = spin_lock_irqsave(&pmm_poison_lock);
    *out = pmm_pstats;
    spin_unlock_irqrestore(&pmm_poison_lock, flags);
}

void pmm_poison_last_report(struct pmm_poison_report *out)
{
    irq_flags_t flags = spin_lock_irqsave(&pmm_poison_lock);
    *out = pmm_plast;
    spin_unlock_irqrestore(&pmm_poison_lock, flags);
}

void pmm_poison_note_injected(unsigned n)
{
    irq_flags_t flags = spin_lock_irqsave(&pmm_poison_lock);
    pmm_pstats.injected += n;
    spin_unlock_irqrestore(&pmm_poison_lock, flags);
}

uintptr_t pmm_poison_alias_of(uintptr_t pa)
{
    return pmm_alias ? PMM_ALIAS(pa) : 0;
}

int pmm_poison_identity_state(uintptr_t pa)
{
#if defined(CONFIG_PMM_UNMAP)
    return vmm_linear_page_state(pa);
#else
    (void)pa;
    return -1;
#endif
}

/* From the EL1 fault dump: say whether FAR lies in a PMM page and which
 * free block holds it. No locks (the faulting CPU may hold pmm_lock). */
void pmm_poison_describe_fault(uint64_t far)
{
    if (!pmm_alias || far < buddy_state.heap_start || far >= buddy_state.heap_end) {
        return;
    }
    uintptr_t pg = (uintptr_t)far & ~(uintptr_t)(PAGE_SIZE - 1);
    uint8_t st = block_state[addr_to_block_index(pg)];
    uintptr_t blk;
    unsigned int o;
    int map = pmm_poison_identity_state(pg);
    const char *ms = map == 1 ? "mapped" : map == 0 ? "UNMAPPED (PMM_UNMAP)"
                                                    : "no L3 entry";
    if (st != BLOCK_ALLOCATED && pmm_p_find_block(pg, &blk, &o)) {
        uart_printf("PMM:     FAR is in a PMM-FREE page 0x%lx: free block 0x%lx, "
                    "order %u (%lu KB), page %lu of it; linear map: %s; last "
                    "boundary: %s\n", (unsigned long)pg, (unsigned long)blk, o,
                    (unsigned long)(order_to_size(o) / 1024),
                    (unsigned long)((pg - blk) >> PAGE_SHIFT), ms,
                    pmm_verify_where);
    } else if (st != BLOCK_ALLOCATED) {
        uart_printf("PMM:     FAR is in a PMM-free page 0x%lx (no listed head "
                    "found above it); linear map: %s\n", (unsigned long)pg, ms);
    } else {
        uart_printf("PMM:     FAR is in an allocated (or reserved) heap page 0x%lx; "
                    "linear map: %s\n", (unsigned long)pg, ms);
    }
}

#if defined(CONFIG_PMM_UNMAP_SELFTEST)
/* The store that the selftest expects to fault (its address is printed). */
__attribute__((noinline)) void pmm_unmap_selftest_store(volatile uint64_t *p,
                                                        uint64_t v)
{
    *p = v;
}

void pmm_unmap_selftest(void)
{
    uint64_t *p = pmm_alloc_page();
    if (p == NULL) {
        panic("PMM_UNMAP selftest: no page");
    }
    pmm_free_page(p);
    uart_printf("[pmm-unmap] selftest: page 0x%lx freed (linear map state %d: "
                "1 mapped, 0 unmapped); writing 0x%lx through the identity "
                "mapping from pmm_unmap_selftest_store (0x%lx). Expected: a "
                "data abort with that ELR, FAR 0x%lx, the page named PMM-free.\n",
                (unsigned long)(uintptr_t)p, pmm_poison_identity_state((uintptr_t)p),
                (unsigned long)(uintptr_t)(p + 3),
                (unsigned long)(uintptr_t)&pmm_unmap_selftest_store,
                (unsigned long)(uintptr_t)(p + 3));
    pmm_unmap_selftest_store((volatile uint64_t *)(p + 3), 0x5E1F7E5700000000UL);
    panic("PMM_UNMAP selftest: the write to freed page 0x%lx did not fault",
          (unsigned long)(uintptr_t)p);
}
#endif

#define PMM_POISON_ALLOC(a, o, who)  pmm_p_on_alloc((a), (o), (who))
#define PMM_POISON_FREE(a, o)        pmm_p_on_free((a), (o))
#define PMM_POISON_UNLINKED(a)       pmm_p_unlinked(a)
#define PMM_POISON_ARM()             pmm_poison_arm()

#else  /* !CONFIG_PMM_POISON */

#define PMM_POISON_ALLOC(a, o, who)  ((void)0)
#define PMM_POISON_FREE(a, o)        ((void)0)
#define PMM_POISON_UNLINKED(a)       ((void)0)
#define PMM_POISON_ARM()             ((void)0)

#endif /* CONFIG_PMM_POISON */

/* ==========================================================================
 * Free List Operations
 *
 * Callers mark every page of the block free (set_block_state) before
 * free_list_add, which then writes the head value into the first page;
 * free_list_remove and free_list_pop put the first page back to
 * BLOCK_FREE, and the caller marks the block allocated or lists the block
 * it became part of.
 * ========================================================================== */

/*
 * Add a block to the free list for its order
 */
static void free_list_add(uintptr_t addr, unsigned int order)
{
    struct free_block *block = PMM_FB(addr);
    struct free_block *head = buddy_state.free_lists[order];

    PMM_CHK_ADD(addr, order);
    block->prev = NULL;
    block->next = head;

    if (head) {
        PMM_FB(head)->prev = (struct free_block *)addr;
    }

    buddy_state.free_lists[order] = (struct free_block *)addr;
    buddy_state.free_counts[order]++;
    block_state[addr_to_block_index(addr)] = (uint8_t)(BLOCK_FREE_HEAD | order);
}

/*
 * Remove a specific block from the free list
 */
static void free_list_remove(uintptr_t addr, unsigned int order)
{
    struct free_block *block = PMM_FB(addr);

    PMM_CHK_REMOVE(addr, order);
    if (buddy_state.free_counts[order] == 0) {
        panic("PMM: free_list_remove(0x%lx) on order %u with a zero count "
              "(free-list corruption, #1207)", (unsigned long)addr, order);
    }
    if (block->prev) {
        PMM_FB(block->prev)->next = block->next;
    } else {
        buddy_state.free_lists[order] = block->next;
    }

    if (block->next) {
        PMM_FB(block->next)->prev = block->prev;
    }
    PMM_POISON_UNLINKED(addr);

    buddy_state.free_counts[order]--;
    block_state[addr_to_block_index(addr)] = BLOCK_FREE;
}

/*
 * Pop the first block from a free list
 */
static uintptr_t free_list_pop(unsigned int order)
{
    struct free_block *block = buddy_state.free_lists[order];

    PMM_CHK_POP(order);
    if (!block) {
        return PMM_ALLOC_FAIL;
    }

    /* The head's next link must be another listed block, or NULL on the
     * last one. Anything else is not what this file last wrote there
     * (#1207): report the block as it is now, before its header is cleared
     * below, and stop. A link that is not a block address cannot be
     * followed. A NULL link with blocks still counted could be: the pop
     * would succeed, but the rest of the list is lost, and the blocks left
     * behind still point at this one, which is about to be handed out (a
     * later merge that unlinks one of them would write a link into the
     * caller's page). */
    uintptr_t next = (uintptr_t)PMM_FB(block)->next;
    if (next ? !(pmm_block_ptr_ok(next, order) &&
                 block_state[addr_to_block_index(next)] ==
                     (uint8_t)(BLOCK_FREE_HEAD | order))
             : buddy_state.free_counts[order] > 1) {
        struct pmm_finding f = {
            .what = next ? "head's next link is not a listed block of this order"
                         : "head's next link is NULL with blocks still counted",
            .order = order, .walked = 0,
            .holder = (uintptr_t)block, .link = next,
        };
        pmm_verify_reported[order] = (uintptr_t)block;
        pmm_report_finding("free_list_pop", &f);
        panic("PMM: order %u free list head 0x%lx has next link 0x%lx with "
              "%lu blocks counted (free-list corruption, #1207)", order,
              (unsigned long)(uintptr_t)block, (unsigned long)next,
              (unsigned long)buddy_state.free_counts[order]);
    }

    buddy_state.free_lists[order] = PMM_FB(block)->next;
    if (PMM_FB(block)->next) {
        PMM_FB(PMM_FB(block)->next)->prev = NULL;
    }

    /* Clear the popped block's link pointers. Callers (buddy_alloc /
     * pmm_alloc_pages_low) treat the returned region as fresh memory and
     * do not zero the header. If a caller ever frees this region back
     * with the wrong order, the stale prev/next bytes would re-enter the
     * free list as garbage and corrupt the doubly-linked list. */
    PMM_FB(block)->next = NULL;
    PMM_FB(block)->prev = NULL;
    PMM_POISON_UNLINKED((uintptr_t)block);

    buddy_state.free_counts[order]--;
    block_state[addr_to_block_index((uintptr_t)block)] = BLOCK_FREE;
    return (uintptr_t)block;
}

/* ==========================================================================
 * Block State Tracking
 * ========================================================================== */

/*
 * Mark a range of pages as having a certain state
 */
static void set_block_state(uintptr_t addr, unsigned int order, uint8_t state)
{
    size_t base_index = addr_to_block_index(addr);
    size_t count = order_to_pages(order);

    for (size_t i = 0; i < count; i++) {
        block_state[base_index + i] = state;
    }
}

/*
 * Get the state of a block (checks first page of the block)
 */
static uint8_t get_block_state(uintptr_t addr)
{
    size_t index = addr_to_block_index(addr);
    return block_state[index];
}

/*
 * True when the buddy is one free block of exactly this order, on that
 * order's free list.
 *
 * This used to test that every page of the buddy range was free. That is
 * also true of a range made of smaller free blocks (two order-0 blocks
 * side by side pass as an order-1 buddy), and merging with such a range
 * unlinked its first page from a list it was not on: free_counts[order]
 * went down for a block that list never held (an underflow at zero), the
 * other blocks of the range stayed listed inside the merged block, and a
 * later allocation found a non-zero count over an empty list and split
 * from PMM_ALLOC_FAIL, writing block_state[] far outside the array. This
 * sequence produces #1207's signature in QEMU. It is not shown to occur on
 * jetson-nano-2, and the halt persists with it fixed (3 of 20 kexec suite
 * boots). The head value is written only by free_list_add, so this test
 * is exact, and O(1).
 */
static bool is_buddy_free(uintptr_t buddy_addr, unsigned int order)
{
    /* Check buddy is within heap bounds */
    if (buddy_addr < buddy_state.heap_start || buddy_addr >= buddy_state.heap_end) {
        return false;
    }

    return block_state[addr_to_block_index(buddy_addr)] ==
           (uint8_t)(BLOCK_FREE_HEAD | order);
}

/*
 * True when every page of [addr, addr + 2^order pages) is allocated and
 * inside the heap: the precondition of a free. `bad` gets the first page
 * that is not.
 */
static bool range_is_allocated(uintptr_t addr, unsigned int order, uintptr_t *bad)
{
    size_t count = order_to_pages(order);

    if (addr + order_to_size(order) > buddy_state.heap_end ||
        addr + order_to_size(order) < addr) {
        *bad = buddy_state.heap_end;
        return false;
    }
    size_t base_index = addr_to_block_index(addr);
    for (size_t i = 0; i < count; i++) {
        if (block_state[base_index + i] != BLOCK_ALLOCATED) {
            *bad = addr + (i << PAGE_SHIFT);
            return false;
        }
    }
    return true;
}

/* ==========================================================================
 * Core Buddy Allocator Functions
 * ========================================================================== */

/*
 * Allocate a block of the given order.
 * Returns physical address or 0 on failure.
 */
static uintptr_t buddy_alloc(unsigned int order)
{
    /* Find a free block at this order or higher */
    for (unsigned int o = order; o <= MAX_ORDER; o++) {
        if (buddy_state.free_counts[o] > 0) {
            /* Found a free block - pop it from the list */
            uintptr_t block = free_list_pop(o);
            if (block == PMM_ALLOC_FAIL) {
                /* A count without a list. Splitting from the sentinel
                 * would write block_state[] at a wild index (the #1207
                 * data abort); stop with the cause named instead. */
                uart_printf("\n[pmm-verify] order %u: count %lu without a list "
                            "at %lu ms (%u finding(s) reported before this; "
                            "last suite boundary: %s)\n", o,
                            (unsigned long)buddy_state.free_counts[o],
                            (unsigned long)pmm_now_ms(), pmm_verify_findings,
                            pmm_verify_where);
                pmm_dump_all_orders();
                panic("PMM: order %u free list is empty but its count is "
                      "%lu (free-list corruption, #1207)", o,
                      (unsigned long)buddy_state.free_counts[o]);
            }

            /* Split down to requested order if needed */
            while (o > order) {
                o--;
                buddy_state.split_count++;

                /* Calculate buddy (upper half of split) */
                uintptr_t buddy = block + order_to_size(o);

                /* Add buddy to free list at lower order */
                set_block_state(buddy, o, BLOCK_FREE);
                free_list_add(buddy, o);
            }

            /* Mark the allocated block */
            set_block_state(block, order, BLOCK_ALLOCATED);
            buddy_state.free_pages -= order_to_pages(order);
            buddy_state.alloc_count++;

            return block;
        }
    }

    return PMM_ALLOC_FAIL; /* Out of memory */
}

/*
 * Free a block and coalesce with buddy if possible.
 * @original_order: The order of the block being freed (for page counting)
 */
static void buddy_free(uintptr_t addr, unsigned int order, unsigned int original_order)
{
    /* Coalesce with buddy while possible */
    while (order < MAX_ORDER) {
        uintptr_t buddy_addr = get_buddy_addr(addr, order);

        /* Check if buddy exists, is free, and is properly aligned */
        if (!is_buddy_free(buddy_addr, order)) {
            break;
        }

        /* Also verify we're aligned for this order to be a valid merge */
        uintptr_t merged_addr = (addr < buddy_addr) ? addr : buddy_addr;
        if (!is_aligned_to_order(merged_addr, order + 1)) {
            break;
        }

        /* Remove buddy from its free list */
        PMM_CHK_BUDDY(buddy_addr, order);
        free_list_remove(buddy_addr, order);

        /* Merge: use lower address as the merged block */
        addr = merged_addr;
        order++;
        buddy_state.merge_count++;
    }

    /* Add the (possibly merged) block to the appropriate free list */
    set_block_state(addr, order, BLOCK_FREE);
    free_list_add(addr, order);

    /* Only count the pages we're actually freeing, not the merged total */
    buddy_state.free_pages += order_to_pages(original_order);
    buddy_state.free_count++;
}

/* ==========================================================================
 * PMM Public API
 * ========================================================================== */

/*
 * Initialize the physical memory manager.
 */

/* Forward declaration so pmm_add_region_split can call it. The defn
 * sits below pmm_carve_reserves to keep the carve logic above its only
 * (file-scope) consumer. */
static void pmm_add_region(uintptr_t start, uintptr_t end);

int pmm_carve_reserves(uintptr_t start, uintptr_t end,
                       const dtb_memreserve_t *rsv, int n_rsv,
                       uintptr_t *out_starts, uintptr_t *out_ends, int max_out)
{
    if (max_out <= 0 || !out_starts || !out_ends) return 0;
    if (start >= end) return 0;

    /* No reservations: one trivial subrange. */
    if (n_rsv <= 0 || !rsv) {
        out_starts[0] = start;
        out_ends[0]   = end;
        return 1;
    }

    int n_out = 0;
    uintptr_t cursor = start;
    while (cursor < end) {
        /* Find the earliest-starting reservation overlapping [cursor, end).
         * Reservations are not assumed sorted; re-scan each iteration.
         * Skip zero-size and degenerate (re <= rs) entries cleanly. */
        uintptr_t next_rsv_start = end;
        uintptr_t next_rsv_end   = end;
        bool found = false;
        for (int i = 0; i < n_rsv; i++) {
            uintptr_t rs = (uintptr_t)rsv[i].addr;
            uintptr_t re = (uintptr_t)(rsv[i].addr + rsv[i].size);
            if (re <= rs) continue;                  /* size 0 / overflow */
            if (re <= cursor || rs >= end) continue; /* outside cursor */
            if (!found || rs < next_rsv_start) {
                next_rsv_start = rs > cursor ? rs : cursor;
                next_rsv_end   = re < end ? re : end;
                found = true;
            }
        }

        if (!found) {
            if (n_out >= max_out) return n_out;
            out_starts[n_out] = cursor;
            out_ends[n_out]   = end;
            n_out++;
            return n_out;
        }

        if (next_rsv_start > cursor) {
            if (n_out >= max_out) return n_out;
            out_starts[n_out] = cursor;
            out_ends[n_out]   = next_rsv_start;
            n_out++;
        }
        cursor = next_rsv_end;
    }
    return n_out;
}

/*
 * Add a contiguous memory region to the buddy allocator, skipping any
 * sub-ranges declared in the firmware-supplied /memreserve/ list. Each
 * /memreserve/ entry is intersected against [start, end); on overlap,
 * the region is split so the reserved bytes are never freed.
 *
 * Why this matters: Pi firmware reserves the VPU shared memory carveout
 * (typically 4 MB at 0x3fc00000) via /memreserve/. The DTB advertises
 * the *full* RAM range in /memory@0/reg, so without this skip the buddy
 * allocator would happily hand out pages that the VPU writes to,
 * causing race-condition memory corruption the moment something on the
 * SLM-OS side issues a mailbox/firmwarekms call. The carveout is
 * outside the kernel image, so before this fix the bug was latent only
 * because we don't currently exercise the VPU.
 *
 * Carve scratch arrays are file-static rather than on-stack: pmm_init
 * runs single-threaded during boot (no concurrent split is ever in
 * flight), and DTB_MAX_MEMRESERVES is intentionally low — but if the
 * cap grows in the future, the worst-case ~1 KB stack footprint per
 * call would otherwise scale with it.
 */
/* User-supplied reserve slots. PMM init feeds these into the same
 * `pmm_carve_reserves` pipeline as the firmware /memreserve/ entries,
 * so runtime-discovered protected ranges (the canonical case: nvgpu
 * inst block + GMMU page tables + IOVMM-stitched weights-pool
 * extents that survived kexec but live in physical pages SLM-OS
 * would otherwise allocate from) get carved out before any page is
 * published to the buddy. See `pmm_user_reserve_add` in pmm.h.
 *
 * Capacity bumped from 16 → 2048 in #788 Stage 4 to accommodate the
 * weights-pool extents: a 1.5 GB IOVMM-stitched allocation
 * empirically decomposes into 2-1100 contiguous runs on Tegra
 * GA10B (depends on CMA fragmentation at allocation time on a
 * freshly-booted vs long-running Linux). 2048 caps the array at
 * 32 KB (2048 × 16 bytes) — generous over the highest observed
 * extent count (~1033 on a fragmented allocation), and small
 * enough that the boot-time BSS bloat is well within the
 * kernel image's existing budget. If a future helper logs
 * "extents truncated", bump this cap and the helper's
 * `GA10B_WEIGHTS_EXTENTS_MAX` together.
 *
 * The helper-side cap in `ga10b_channel_handoff.h`
 * (`GA10B_WEIGHTS_EXTENTS_MAX = 8192`) is larger than this cap on
 * purpose: the helper publishes the full list, and SLM-OS picks
 * the prefix that fits — anything past entry 2048 won't get a
 * PMM reservation but WILL still get a GMMU mapping (the rebuild
 * path walks the entire list, only the reservation hook truncates).
 * A page mapped but not reserved is at risk if PMM also allocates
 * from it; in practice the helper's IOVMM phys range and SLM-OS
 * PMM's allocations rarely overlap, so the partial protection is
 * sufficient until a future bump. The define lives in pmm_internal.h
 * so tests can size reserve lists from it. */
static dtb_memreserve_t pmm_user_reserves[PMM_MAX_USER_RESERVES];
static size_t           pmm_n_user_reserves = 0;

/* Scratch arrays are sized to fit the worst case = firmware reserves
 * + user reserves + the DTB blob (#1244), plus one extra slot for the
 * split helper's own bookkeeping (matches the prior
 * `DTB_MAX_MEMRESERVES + 1` rationale for
 * `pmm_split_starts`/`pmm_split_ends`). */
#define PMM_SPLIT_RSV_MAX  (DTB_MAX_MEMRESERVES + PMM_MAX_USER_RESERVES + 1)
static uintptr_t        pmm_split_starts[PMM_SPLIT_RSV_MAX + 1];
static uintptr_t        pmm_split_ends  [PMM_SPLIT_RSV_MAX + 1];
static dtb_memreserve_t pmm_split_rsv   [PMM_SPLIT_RSV_MAX];

int pmm_user_reserve_add(uint64_t phys, uint64_t size)
{
    if (size == 0u) {
        return -1;
    }
    if (pmm_n_user_reserves >= PMM_MAX_USER_RESERVES) {
        return -1;
    }
    pmm_user_reserves[pmm_n_user_reserves].addr = phys;
    pmm_user_reserves[pmm_n_user_reserves].size = size;
    pmm_n_user_reserves++;
    return 0;
}

int pmm_user_reserve_add_array(const dtb_memreserve_t *extents,
                                size_t n_extents)
{
    if (extents == NULL) {
        return 0;
    }
    int added = 0;
    for (size_t i = 0; i < n_extents; i++) {
        if (pmm_user_reserve_add(extents[i].addr,
                                  extents[i].size) != 0) {
            /* Surface truncation loudly — the caller will see
             * `added < n_extents` and the wedge that fires later
             * on the unprotected tail is otherwise hard to
             * attribute back to "PMM reserve table was full". */
            uart_printf("Warning: user-reserve table full at entry "
                        "%zu/%zu (cap=%u). Pages from extent %zu "
                        "onward are NOT protected — expect "
                        "downstream clobber.\n",
                        i, n_extents,
                        (unsigned)PMM_MAX_USER_RESERVES, i);
            break;
        }
        added++;
    }
    return added;
}

size_t pmm_user_reserve_count(void)
{
    return pmm_n_user_reserves;
}

void pmm_user_reserve_reset(void)
{
    pmm_n_user_reserves = 0;
}

/*
 * Page-aligned range that holds the DTB blob (#1244): down to the page of
 * its first byte, up to the page after its last. Pure.
 */
bool pmm_dtb_reserve_range(uint64_t blob_addr, uint64_t blob_size,
                           dtb_memreserve_t *out)
{
    /* The end is rounded up to a page, so reject any range whose last
     * byte is in the top page of the address space: the round-up would
     * wrap there even when addr + size does not. */
    if (!out || blob_size == 0 || blob_size > UINT64_MAX - (PAGE_SIZE - 1) ||
        blob_addr > UINT64_MAX - (PAGE_SIZE - 1) - blob_size) {
        return false;
    }
    uint64_t first = blob_addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (blob_addr + blob_size + PAGE_SIZE - 1) &
                    ~(uint64_t)(PAGE_SIZE - 1);
    out->addr = first;
    out->size = last - first;
    return true;
}

/*
 * Number of free blocks that overlap [start, end); the first one found is
 * returned through first_addr/first_order (may be NULL). Walks every free
 * list: boot-time and test use only. Caller holds pmm_lock or runs before
 * any other CPU allocates.
 */
size_t pmm_free_overlap_count(uintptr_t start, uintptr_t end,
                              uintptr_t *first_addr, unsigned int *first_order)
{
    size_t n = 0;
    for (unsigned int o = 0; o <= MAX_ORDER; o++) {
        size_t bsize = order_to_size(o);
        for (struct free_block *b = buddy_state.free_lists[o]; b; b = PMM_FB(b)->next) {
            uintptr_t a = (uintptr_t)b;
            if (a < end && a + bsize > start) {
                if (n == 0) {
                    if (first_addr) {
                        *first_addr = a;
                    }
                    if (first_order) {
                        *first_order = o;
                    }
                }
                n++;
            }
        }
    }
    return n;
}

size_t pmm_free_overlap_count_locked(uintptr_t start, uintptr_t end)
{
    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    size_t n = pmm_free_overlap_count(start, end, NULL, NULL);
    spin_unlock_irqrestore(&pmm_lock, flags);
    return n;
}

/*
 * Every range pmm_add_region_split carves out of a region: the firmware
 * /memreserve/ list, the user reserves, and the DTB blob itself (#1244).
 * Writes at most `max` entries; returns the number written.
 */
int pmm_collect_reserves(dtb_memreserve_t *out, int max)
{
    int n_rsv = dtb_get_memreserves(out, max < DTB_MAX_MEMRESERVES
                                             ? max : DTB_MAX_MEMRESERVES);
    /* User-registered reserves (e.g. GPU kexec handoff state — see
     * `pmm_user_reserve_add`). The carve helper handles unsorted,
     * overlapping, and out-of-region entries identically to firmware
     * reserves, so the lists can be concatenated without further
     * bookkeeping. */
    for (size_t i = 0; i < pmm_n_user_reserves && n_rsv < max; i++) {
        out[n_rsv].addr = pmm_user_reserves[i].addr;
        out[n_rsv].size = pmm_user_reserves[i].size;
        n_rsv++;
    }
    /* The DTB blob (#1244). Its own reserve map does not cover it, and
     * the loader decides where it sits: kexec-tools puts it in the first
     * free pages after the kernel image, which is where the heap starts;
     * Pi 5 firmware puts it in low RAM; UEFI leaves it above the heap.
     * Reserved from the blob's actual address and totalsize, so every
     * boot path is covered whatever the loader does. */
    uint64_t blob_addr, blob_size;
    if (n_rsv < max && dtb_get_blob_range(&blob_addr, &blob_size) &&
        pmm_dtb_reserve_range(blob_addr, blob_size, &out[n_rsv])) {
        n_rsv++;
    }
    return n_rsv;
}

static void pmm_add_region_split(uintptr_t start, uintptr_t end)
{
    /* The pmm_split_* scratch arrays are shared file-statics; the
     * "single-threaded during boot" precondition is documented above
     * but not enforced. Trip a panic on re-entry so a future caller
     * that violates the contract surfaces immediately rather than
     * silently corrupting whichever caller's split state is active.
     * The check is unconditional (not gated by NDEBUG via ASSERT)
     * because pmm_add_region_split is called only a handful of times
     * during boot — the runtime cost is irrelevant; the diagnostic is
     * the value.
     *
     * Regression coverage: `pmm_init` itself calls this function
     * once per platform region (1-4 times depending on platform).
     * If the post-call `in_split = false` reset is ever broken, the
     * second call panics during boot and every test in `test_pmm.c`
     * fails to even start. Implicit but loud. */
    static bool in_split = false;
    if (in_split) {
        panic("pmm_add_region_split: re-entered "
              "(scratch arrays would race)");
    }
    in_split = true;

    /* Firmware reserves, user reserves and the DTB blob. Bounded by
     * PMM_SPLIT_RSV_MAX so the writes can never overrun
     * `pmm_split_rsv[]`. */
    int n_rsv = pmm_collect_reserves(pmm_split_rsv, PMM_SPLIT_RSV_MAX);
    PMM_CHK_RESERVES(pmm_split_rsv, n_rsv);
    int n_out = pmm_carve_reserves(start, end, pmm_split_rsv, n_rsv,
                                   pmm_split_starts, pmm_split_ends,
                                   PMM_SPLIT_RSV_MAX + 1);
    for (int i = 0; i < n_out; i++) {
        pmm_add_region(pmm_split_starts[i], pmm_split_ends[i]);
    }

    in_split = false;
}

/*
 * Add a contiguous memory region to the buddy allocator.
 * Breaks it into the largest power-of-two aligned blocks possible.
 *
 * Only whole pages inside [start, end) are added: start rounds up and end
 * rounds down. A carved subrange ends where a reservation begins, and a
 * reservation that begins inside a page owns that page. The order-0 step
 * of the loop below has no fit test, so without the rounding the page
 * holding an unaligned end was listed, over the reservation, next to a
 * free buddy it was never merged with (#1207).
 */
static void pmm_add_region(uintptr_t start, uintptr_t end)
{
    uintptr_t current = PAGE_ALIGN_UP(start);
    PMM_CHK_REGION(start, end);
    end &= ~(uintptr_t)(PAGE_SIZE - 1);
    while (current < end) {
        unsigned int order = MAX_ORDER;

        while (order > 0) {
            size_t block_size = order_to_size(order);
            if ((current + block_size <= end) &&
                is_aligned_to_order(current, order)) {
                break;
            }
            order--;
        }

        size_t block_size = order_to_size(order);

        PMM_POISON_FREE(current, order);
        set_block_state(current, order, BLOCK_FREE);
        free_list_add(current, order);
        buddy_state.free_pages += order_to_pages(order);

        current += block_size;
    }
}

/* Built into every image, like the tests that call them (TEST_SOURCES). */
/*
 * Test hooks (kernel/include/pmm_internal.h); for kernel/tests only.
 *
 * pmm_test_add_region() hands [start, end) to pmm_add_region() under
 * pmm_lock, as pmm_init does for a carved subrange. The caller must own
 * the whole pages of the range (allocated from the PMM and not freed).
 * pmm_free_block_order() is the order of the listed free block that
 * starts at addr, or -1 when no listed block starts there.
 */
void pmm_test_add_region(uintptr_t start, uintptr_t end)
{
    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    pmm_add_region(start, end);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

int pmm_test_verify_cleared_link(unsigned int *first, unsigned int *second,
                                 unsigned int *restored)
{
    int order = -1;
    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    for (unsigned int o = 0; o <= MAX_ORDER && order < 0; o++) {
        if (buddy_state.free_counts[o] >= 2 && buddy_state.free_lists[o] &&
            PMM_FB(buddy_state.free_lists[o])->next) {
            order = (int)o;
        }
    }
    if (order >= 0) {
        struct free_block *head = PMM_FB(buddy_state.free_lists[order]);
        struct free_block *old = head->next;
        head->next = NULL;
        *first = pmm_verify_locked("verifier self-test: link cleared on purpose");
        *second = pmm_verify_locked("verifier self-test: second walk");
        head->next = old;
        pmm_verify_reported[order] = 0;
        *restored = pmm_verify_locked("verifier self-test: link restored");
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return order;
}

bool pmm_test_dump_readable(uintptr_t addr)
{
    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    bool ok = pmm_dump_readable(addr);
    spin_unlock_irqrestore(&pmm_lock, flags);
    return ok;
}

int pmm_free_block_order(uintptr_t addr)
{
    int found = -1;
    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    for (unsigned int o = 0; o <= MAX_ORDER && found < 0; o++) {
        for (struct free_block *b = buddy_state.free_lists[o]; b; b = PMM_FB(b)->next) {
            if ((uintptr_t)b == addr) {
                found = (int)o;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return found;
}


void pmm_init(void)
{
    uintptr_t kernel_end = (uintptr_t)&__kernel_end;
    buddy_state.heap_start = PAGE_ALIGN_UP(kernel_end);
#if defined(PLATFORM_RASPI5)
    /* Reserve last 2MB for the non-cacheable DMA block (see ncmem.h) */
    buddy_state.heap_end = (RAM_BASE + RAM_SIZE) - 0x200000UL;
#else
    buddy_state.heap_end = RAM_BASE + RAM_SIZE;
#endif

#if defined(PLATFORM_X86_64)
    /* On x86-64, RAM_SIZE in platform.h is the MAX array size, not actual RAM.
     * Use the detected RAM end from vmm_init() (Multiboot2 memory map). */
    {
        extern uintptr_t x86_detected_ram_end;
        if (x86_detected_ram_end > 0) {
            buddy_state.heap_end = x86_detected_ram_end;
        }
    }
#endif

    /* Calculate page counts (full range including gaps) */
    buddy_state.total_pages = (buddy_state.heap_end - buddy_state.heap_start) / PAGE_SIZE;
    buddy_state.reserved_pages = (buddy_state.heap_start - RAM_BASE) / PAGE_SIZE;

    /* Initialize free lists */
    for (unsigned int o = 0; o <= MAX_ORDER; o++) {
        buddy_state.free_lists[o] = NULL;
        buddy_state.free_counts[o] = 0;
    }

    /* Clear block state array */
    for (size_t i = 0; i < MAX_BLOCKS; i++) {
        block_state[i] = BLOCK_ALLOCATED;
    }
    PMM_CHK_INIT();

    /* Initialize statistics */
    buddy_state.free_pages = 0;
    buddy_state.alloc_count = 0;
    buddy_state.free_count = 0;
    buddy_state.split_count = 0;
    buddy_state.merge_count = 0;

    /*
     * Add memory regions to the buddy allocator.
     * On Jetson, the OP-TEE carveout splits RAM into multiple regions.
     */
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /*
     * Jetson memory regions (from /proc/iomem):
     *   80000000-BDFFFFFF : System RAM (~990 MB, region 1)
     *   BE000000-C1FFFFFF : OP-TEE carveout — SKIP
     *   C2000000-FFFDFFFF : System RAM (~958 MB, region 2)
     *   100000000-23FFFFFFF : System RAM (~5 GB, region 3, conservative end)
     *
     * Region 3 has internal reserved sub-regions above 0x240000000.
     * Use 0x240000000 as a conservative upper bound.
     *
     * The camera RTCPU CH_SETUP region lives inside the NC
     * carveout at 0xBDFE0000 — see
     * `kernel/drivers/camrtc/camrtc.c:CAMRTC_CTRL_REGION_PHYS`
     * for the rationale. PMM doesn't manage the NC region (already
     * excluded by `heap_end = 0xBDE00000`), so no extra carveout
     * is needed here.
     *
     * IMX219 frame buffer carveout: 4 MB at 0xA1000000-0xA1400000
     * (CAMRTC_FRAME_BUFFER_PHYS / _END from camrtc_layout.h —
     * shared with kernel/drivers/camrtc/camrtc.c so the carveout
     * geometry can't drift between PMM-side reservation and the
     * accessor that hands the IOVA to RCE). The carveout sits in
     * RCE's VM1 IOVA aperture (0xA0000000..0xC0000000); SMMU-
     * bypass post-kexec means IOVA == phys for the camera path.
     * PMM splits region 1 around the carveout so the buddy
     * allocator never hands out frame-buffer pages.
     */
    pmm_add_region_split(buddy_state.heap_start, CAMRTC_FRAME_BUFFER_PHYS);
    pmm_add_region_split(CAMRTC_FRAME_BUFFER_END, 0xBDE00000UL);  /* Last 2MB reserved for NC memory */
    pmm_add_region_split(0xC2000000UL, 0xFFFE0000UL);
    pmm_add_region_split(0x100000000UL, 0x240000000UL);
#else
    pmm_add_region_split(buddy_state.heap_start, buddy_state.heap_end);
#endif

    /*
     * Post-init sanity: every order's free list head must have
     * next/prev pointers either NULL or pointing inside the heap.
     * On Jetson kexec, if pmm_init has run BEFORE vmm_init while
     * SCTLR.C=0, Linux's stale L4 cache lines clobber the freshly-
     * written pointers and the next pop will fault (issue #608).
     * Catching it here turns a confusing later page fault during
     * `slm load` into a clear, immediate panic that names the
     * exact cause, so a future caller-order regression doesn't
     * silently re-introduce the bug.
     */
    for (unsigned int o = 0; o <= MAX_ORDER; o++) {
        struct free_block *head = buddy_state.free_lists[o];
        if (!head) continue;
        uintptr_t haddr = (uintptr_t)head;
        if (haddr < buddy_state.heap_start || haddr >= buddy_state.heap_end) {
            panic("pmm_init: free list head order=%u outside heap "
                  "(head=0x%lx, heap=0x%lx-0x%lx) — likely stale cache "
                  "from kexec; ensure vmm_init runs before pmm_init",
                  o, (unsigned long)haddr,
                  (unsigned long)buddy_state.heap_start,
                  (unsigned long)buddy_state.heap_end);
        }
        if (PMM_FB(head)->next != NULL || PMM_FB(head)->prev != NULL) {
            uintptr_t naddr = (uintptr_t)PMM_FB(head)->next;
            uintptr_t paddr = (uintptr_t)PMM_FB(head)->prev;
            bool n_ok = (naddr == 0) ||
                        (naddr >= buddy_state.heap_start &&
                         naddr < buddy_state.heap_end);
            if (PMM_FB(head)->prev != NULL || !n_ok) {
                panic("pmm_init: free list head order=%u corrupt "
                      "(head=0x%lx next=0x%lx prev=0x%lx) — likely "
                      "stale cache from kexec; ensure vmm_init runs "
                      "before pmm_init",
                      o, (unsigned long)haddr,
                      (unsigned long)naddr, (unsigned long)paddr);
            }
        }
    }

    /*
     * #1244: no free block may overlap the DTB blob. The blob is read for
     * the whole life of the kernel (FDT lookups by drivers), and a free
     * block inside it has its list pointers written over the blob and its
     * pages handed out. Warn, do not panic: the kernel has run this way,
     * and the line is the evidence that names the cause.
     */
    {
        uint64_t blob_addr, blob_size;
        dtb_memreserve_t r;
        if (dtb_get_blob_range(&blob_addr, &blob_size) &&
            pmm_dtb_reserve_range(blob_addr, blob_size, &r)) {
            uintptr_t first = 0;
            unsigned int first_order = 0;
            size_t n = pmm_free_overlap_count((uintptr_t)r.addr,
                                              (uintptr_t)(r.addr + r.size),
                                              &first, &first_order);
            if (n) {
                WARN("PMM: %u free block(s) overlap the DTB at 0x%lx-0x%lx "
                     "(%u bytes; first block 0x%lx order %u; heap starts "
                     "0x%lx) (#1244)",
                     (unsigned)n, (unsigned long)r.addr,
                     (unsigned long)(r.addr + r.size), (unsigned)blob_size,
                     (unsigned long)first, first_order,
                     (unsigned long)buddy_state.heap_start);
            } else {
                INFO("PMM: DTB at 0x%lx-0x%lx (%u bytes) is clear of every "
                     "free block (heap starts 0x%lx)",
                     (unsigned long)r.addr, (unsigned long)(r.addr + r.size),
                     (unsigned)blob_size,
                     (unsigned long)buddy_state.heap_start);
            }
        } else if (dtb_get_blob() != NULL) {
            /* A blob was parsed but its header gives no usable range
             * (bad magic, or totalsize 0 or above DTB_MAX_BLOB_SIZE):
             * nothing was reserved for it. */
            WARN("PMM: DTB at 0x%lx has no usable totalsize; not reserved "
                 "(#1244)", (unsigned long)(uintptr_t)dtb_get_blob());
        }
    }

    PMM_CHK_WALK_INIT();
    (void)pmm_verify_locked("end of pmm_init");
    buddy_state.initialized = true;

    INFO("PMM initialized (buddy allocator)");
    PMM_POISON_ARM();
    DEBUG_PRINT("  Kernel ends at:  0x%lx", kernel_end);
    DEBUG_PRINT("  Heap start:      0x%lx", buddy_state.heap_start);
    DEBUG_PRINT("  Heap end:        0x%lx", buddy_state.heap_end);
    DEBUG_PRINT("  Free pages:      %u (%u MB)",
                (unsigned)buddy_state.free_pages,
                (unsigned)(buddy_state.free_pages * PAGE_SIZE / (1024 * 1024)));
    DEBUG_PRINT("  Reserved pages:  %u (kernel)", (unsigned)buddy_state.reserved_pages);
}

/*
 * Allocate a single 4KB physical page.
 */
void *pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

/*
 * Allocate contiguous physical pages.
 * Note: count is rounded up to the next power of 2.
 */
void *pmm_alloc_pages(size_t count)
{
    if (!buddy_state.initialized) {
        ERROR("PMM not initialized");
        return (void *)0;
    }

    if (count == 0) {
        return (void *)0;
    }

    /* Calculate the order needed (round up to power of 2) */
    unsigned int order = log2_ceil(count);

    if (order > MAX_ORDER) {
        WARN("PMM: Requested %u pages exceeds max order", (unsigned)count);
        return (void *)0;
    }

    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    PMM_CHK_ENTER();
    uintptr_t addr = buddy_alloc(order);
    PMM_CHK_ALLOCED(addr, order);
    spin_unlock_irqrestore(&pmm_lock, flags);

    if (addr == PMM_ALLOC_FAIL) {
        WARN("PMM: Cannot allocate %u pages (order %u)", (unsigned)count, order);
        return NULL;
    }
    PMM_POISON_ALLOC(addr, order, "pmm_alloc_pages");

    return (void *)addr;
}

/*
 * Like pmm_alloc_pages but biased toward low physical addresses.
 * Walks all free lists at order >= requested order, picks the
 * lowest-address block, splits down. Used for DMA buffers on
 * platforms where the PCIe inbound translation only reaches the
 * bottom of physical RAM (e.g. Pi 5 boundary-submit hypothesis).
 *
 * O(n) over total free blocks per call — acceptable because callers
 * use it for per-load setup, not per-submit.
 */
void *pmm_alloc_pages_low(size_t count)
{
    if (!buddy_state.initialized) {
        ERROR("PMM not initialized");
        return NULL;
    }
    if (count == 0) return NULL;

    unsigned int order = log2_ceil(count);
    if (order > MAX_ORDER) {
        WARN("PMM: alloc_low %u pages exceeds max order", (unsigned)count);
        return NULL;
    }

    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    PMM_CHK_ENTER();

    /* Linear scan: find the lowest-address block at any order >= order.
     * Can't break early after finding a match at the requested order —
     * a block at a higher order that lives at a lower address would
     * split down to give us a lower result address (buddy-split puts
     * the left half at the parent's addr). Scan cost is bounded by
     * total-free-blocks, which the buddy allocator keeps small by
     * coalescing on free (typically a few dozen across all orders);
     * the scan completes in microseconds. pmm_lock stays held with
     * IRQs off throughout — acceptable for the per-load caller, not
     * safe for per-submit hot paths. */
    uintptr_t best_addr = (uintptr_t)~0UL;
    unsigned int best_order = 0;
    bool found = false;
    for (unsigned int o = order; o <= MAX_ORDER; o++) {
        for (struct free_block *blk = buddy_state.free_lists[o];
             blk != NULL;
             blk = PMM_FB(blk)->next) {
            uintptr_t a = (uintptr_t)blk;
            if (!found || a < best_addr) {
                best_addr = a;
                best_order = o;
                found = true;
            }
        }
    }

    if (!found) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        WARN("PMM: alloc_low: no block of order %u or higher", order);
        return NULL;
    }

    /* Remove from current free list, split down to requested order. */
    free_list_remove(best_addr, best_order);
    while (best_order > order) {
        best_order--;
        buddy_state.split_count++;
        uintptr_t buddy = best_addr + order_to_size(best_order);
        set_block_state(buddy, best_order, BLOCK_FREE);
        free_list_add(buddy, best_order);
    }
    set_block_state(best_addr, order, BLOCK_ALLOCATED);
    PMM_CHK_ALLOCED(best_addr, order);
    buddy_state.free_pages -= order_to_pages(order);
    buddy_state.alloc_count++;

    spin_unlock_irqrestore(&pmm_lock, flags);
    PMM_POISON_ALLOC(best_addr, order, "pmm_alloc_pages_low");
    return (void *)best_addr;
}

/*
 * Free a single physical page.
 */
void pmm_free_page(void *page)
{
    pmm_free_pages(page, 1);
}

/*
 * Free contiguous physical pages.
 * Note: count is rounded up to match the allocation order.
 */
void pmm_free_pages(void *page, size_t count)
{
    if (!buddy_state.initialized) {
        ERROR("PMM not initialized");
        return;
    }

    if (count == 0 || page == NULL) {
        return;
    }

    uintptr_t addr = (uintptr_t)page;

    /* Validate address */
    if (addr < buddy_state.heap_start || addr >= buddy_state.heap_end) {
        ERROR("PMM: Invalid free address 0x%lx", addr);
        return;
    }

    if (addr & (PAGE_SIZE - 1)) {
        ERROR("PMM: Unaligned free address 0x%lx", addr);
        return;
    }

    /* Calculate the order (round up to match allocation) */
    unsigned int order = log2_ceil(count);

    if (order > MAX_ORDER) {
        ERROR("PMM: Invalid free count %u", (unsigned)count);
        return;
    }

    /* Verify address is aligned for this order */
    if (!is_aligned_to_order(addr, order)) {
        ERROR("PMM: Address 0x%lx not aligned for order %u", addr, order);
        return;
    }

    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    PMM_CHK_ENTER();

    /* Check for double-free */
    if (get_block_state(addr) != BLOCK_ALLOCATED) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        WARN("PMM: Double-free detected at 0x%lx", addr);
        return;
    }

    /* Every page of the range must be allocated. A range that holds a
     * free page (an over-sized count, or a free after part of the block
     * was freed) would go onto a free list over the block already listed
     * inside it; the two then share pages and each other's list links. */
    uintptr_t bad;
    if (!range_is_allocated(addr, order, &bad)) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        ERROR("PMM: free of %u pages at 0x%lx rejected: page 0x%lx is not "
              "allocated", (unsigned)order_to_pages(order), addr, bad);
        return;
    }
    PMM_CHK_FREE(addr, order);
    PMM_POISON_FREE(addr, order);

    buddy_free(addr, order, order);

    spin_unlock_irqrestore(&pmm_lock, flags);
}

/*
 * Get PMM statistics.
 */
void pmm_get_stats(struct pmm_stats *stats)
{
    if (!stats) {
        return;
    }

    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);

    stats->total_pages = buddy_state.total_pages;
    stats->free_pages = buddy_state.free_pages;
    stats->used_pages = buddy_state.total_pages - buddy_state.free_pages;
    stats->reserved_pages = buddy_state.reserved_pages;
    stats->heap_start = buddy_state.heap_start;
    stats->heap_end = buddy_state.heap_end;

    spin_unlock_irqrestore(&pmm_lock, flags);
}

/*
 * Print PMM statistics.
 */
void pmm_dump_stats(void)
{
    struct pmm_stats stats;
    pmm_get_stats(&stats);

    uart_puts("\nPMM Statistics (Buddy Allocator):\n");
    uart_printf("  Heap range:      0x%lx - 0x%lx\n", stats.heap_start, stats.heap_end);
    uart_printf("  Total pages:     %u (%u KB)\n",
                (unsigned)stats.total_pages,
                (unsigned)(stats.total_pages * PAGE_SIZE / 1024));
    uart_printf("  Free pages:      %u (%u KB)\n",
                (unsigned)stats.free_pages,
                (unsigned)(stats.free_pages * PAGE_SIZE / 1024));
    uart_printf("  Used pages:      %u (%u KB)\n",
                (unsigned)stats.used_pages,
                (unsigned)(stats.used_pages * PAGE_SIZE / 1024));
    uart_printf("  Reserved:        %u pages (kernel)\n", (unsigned)stats.reserved_pages);

    /* Snapshot buddy-specific stats under lock, then print without lock held.
     * UART output at 115200 baud is slow; holding the PMM spinlock (with IRQs
     * disabled) during printing would block all allocations and timer interrupts. */
    struct pmm_buddy_stats buddy_stats;
    pmm_get_buddy_stats(&buddy_stats);

    uart_puts("\n  Free list distribution:\n");
    for (unsigned int o = 0; o <= MAX_ORDER; o++) {
        if (buddy_stats.free_counts[o] > 0) {
            uart_printf("    Order %2u (%5u KB): %u blocks\n",
                        o,
                        (unsigned)(order_to_size(o) / 1024),
                        (unsigned)buddy_stats.free_counts[o]);
        }
    }

    uart_printf("\n  Operations:\n");
    uart_printf("    Allocations: %u\n", (unsigned)buddy_stats.alloc_count);
    uart_printf("    Frees:       %u\n", (unsigned)buddy_stats.free_count);
    uart_printf("    Splits:      %u\n", (unsigned)buddy_stats.split_count);
    uart_printf("    Merges:      %u\n", (unsigned)buddy_stats.merge_count);
}

/*
 * Walk the free list at `order` and print each block's address + the
 * next/prev pointer values it carries. Bounded at `max_blocks` to
 * keep a cycle from looping forever; if the count is hit it prints
 * a "(truncated)" marker so the caller knows the list was longer.
 *
 * Validates each pointer against the heap range before chasing —
 * the whole point of this diagnostic is to find blocks whose
 * pointer fields have been clobbered, so we can't assume `next`
 * points at a valid block.
 *
 * Snapshots the head pointer under the PMM lock to get a coherent
 * starting point, then walks without the lock so the slow UART
 * output doesn't stall every other allocation. The list is
 * boot-time-stable for orders that nothing has touched, which is
 * the case for order MAX_ORDER on a freshly-booted system.
 */
void pmm_dump_free_list(unsigned int order, size_t max_blocks)
{
    if (order > MAX_ORDER) {
        uart_printf("pmm_dump_free_list: order %u > MAX_ORDER (%u)\n",
                    order, MAX_ORDER);
        return;
    }

    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    struct free_block *head = buddy_state.free_lists[order];
    size_t count = buddy_state.free_counts[order];
    uintptr_t heap_start = buddy_state.heap_start;
    uintptr_t heap_end = buddy_state.heap_end;
    spin_unlock_irqrestore(&pmm_lock, flags);

    uart_printf("\nFree list at order %u (%u KB blocks): count=%u, head=%p\n",
                order, (unsigned)(order_to_size(order) / 1024),
                (unsigned)count, (void *)head);
    uart_printf("  Heap range: 0x%lx - 0x%lx\n",
                (unsigned long)heap_start, (unsigned long)heap_end);

    struct free_block *prev_seen = NULL;
    struct free_block *cur = head;
    size_t i = 0;
    while (cur && i < max_blocks) {
        uintptr_t addr = (uintptr_t)cur;
        bool addr_in_heap = (addr >= heap_start && addr < heap_end);
        bool addr_aligned = is_aligned_to_order(addr, order);

        /* Print the block address + its in-memory next/prev fields.
         * If the address is out of heap or unaligned, those fields
         * may be MMIO or unmapped — skip the deref to avoid faulting. */
        uart_printf("  [%3u] block=%p heap=%s align=%s",
                    (unsigned)i, (void *)cur,
                    addr_in_heap ? "ok" : "BAD",
                    addr_aligned ? "ok" : "BAD");

        if (!addr_in_heap || !addr_aligned) {
            uart_puts("  (skipping deref)\n");
            break;
        }

        /* Safe to read next/prev now. */
        struct free_block *nxt = PMM_FB(cur)->next;
        struct free_block *prv = PMM_FB(cur)->prev;
        uart_printf("  next=%p prev=%p\n", (void *)nxt, (void *)prv);

        /* prev linkage check: head's prev must be NULL; others' prev
         * must equal the previous block we walked through. */
        if (i == 0 && prv != NULL) {
            uart_puts("    !! head->prev is non-NULL (corrupt linkage)\n");
        } else if (i > 0 && prv != prev_seen) {
            uart_printf("    !! prev (%p) != expected (%p)\n",
                        (void *)prv, (void *)prev_seen);
        }

        prev_seen = cur;
        cur = nxt;
        i++;
    }

    if (cur && i == max_blocks) {
        uart_printf("  (truncated at %u blocks; list longer or cyclic)\n",
                    (unsigned)max_blocks);
    } else if (!cur && i != count) {
        uart_printf("  !! walked %u blocks but free_count=%u (mismatch)\n",
                    (unsigned)i, (unsigned)count);
    }
}

/*
 * Convenience functions
 */
size_t pmm_get_free_pages(void)
{
    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    size_t v = buddy_state.free_pages;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return v;
}

size_t pmm_get_total_pages(void)
{
    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);
    size_t v = buddy_state.total_pages;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return v;
}

/*
 * Get buddy allocator specific statistics.
 */
void pmm_get_buddy_stats(struct pmm_buddy_stats *stats)
{
    if (!stats) {
        return;
    }

    irq_flags_t flags = spin_lock_irqsave(&pmm_lock);

    for (unsigned int o = 0; o <= MAX_ORDER; o++) {
        stats->free_counts[o] = buddy_state.free_counts[o];
    }
    stats->alloc_count = buddy_state.alloc_count;
    stats->free_count = buddy_state.free_count;
    stats->split_count = buddy_state.split_count;
    stats->merge_count = buddy_state.merge_count;

    spin_unlock_irqrestore(&pmm_lock, flags);
}
