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

/* Block states for tracking */
#define BLOCK_FREE      0
#define BLOCK_ALLOCATED 1
#define BLOCK_SPLIT     2           /* Parent block that was split */

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
 * NOTE: On Jetson after kexec, spinlock operations are no-ops (defined in
 * spinlock.h) since LDAXR/STXR hangs due to corrupted exclusive monitor state.
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
 * Free List Operations
 * ========================================================================== */

/*
 * Add a block to the free list for its order
 */
static void free_list_add(uintptr_t addr, unsigned int order)
{
    struct free_block *block = (struct free_block *)addr;
    struct free_block *head = buddy_state.free_lists[order];

    block->prev = NULL;
    block->next = head;

    if (head) {
        head->prev = block;
    }

    buddy_state.free_lists[order] = block;
    buddy_state.free_counts[order]++;
}

/*
 * Remove a specific block from the free list
 */
static void free_list_remove(uintptr_t addr, unsigned int order)
{
    struct free_block *block = (struct free_block *)addr;

    if (block->prev) {
        block->prev->next = block->next;
    } else {
        buddy_state.free_lists[order] = block->next;
    }

    if (block->next) {
        block->next->prev = block->prev;
    }

    buddy_state.free_counts[order]--;
}

/*
 * Pop the first block from a free list
 */
static uintptr_t free_list_pop(unsigned int order)
{
    struct free_block *block = buddy_state.free_lists[order];

    if (!block) {
        return PMM_ALLOC_FAIL;
    }

    buddy_state.free_lists[order] = block->next;
    if (block->next) {
        block->next->prev = NULL;
    }

    /* Clear the popped block's link pointers. Callers (buddy_alloc /
     * pmm_alloc_pages_low) treat the returned region as fresh memory and
     * do not zero the header. If a caller ever frees this region back
     * with the wrong order, the stale prev/next bytes would re-enter the
     * free list as garbage and corrupt the doubly-linked list. */
    block->next = NULL;
    block->prev = NULL;

    buddy_state.free_counts[order]--;
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
 * Check if a buddy block is free and at the same order
 * (all pages in the buddy must be marked FREE)
 */
static bool is_buddy_free(uintptr_t buddy_addr, unsigned int order)
{
    /* Check buddy is within heap bounds */
    if (buddy_addr < buddy_state.heap_start || buddy_addr >= buddy_state.heap_end) {
        return false;
    }

    /* Check all pages in the buddy block are free */
    size_t base_index = addr_to_block_index(buddy_addr);
    size_t count = order_to_pages(order);

    for (size_t i = 0; i < count; i++) {
        if (block_state[base_index + i] != BLOCK_FREE) {
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
static uintptr_t        pmm_split_starts[DTB_MAX_MEMRESERVES + 1];
static uintptr_t        pmm_split_ends  [DTB_MAX_MEMRESERVES + 1];
static dtb_memreserve_t pmm_split_rsv   [DTB_MAX_MEMRESERVES];

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

    int n_rsv = dtb_get_memreserves(pmm_split_rsv, DTB_MAX_MEMRESERVES);
    int n_out = pmm_carve_reserves(start, end, pmm_split_rsv, n_rsv,
                                   pmm_split_starts, pmm_split_ends,
                                   DTB_MAX_MEMRESERVES + 1);
    for (int i = 0; i < n_out; i++) {
        pmm_add_region(pmm_split_starts[i], pmm_split_ends[i]);
    }

    in_split = false;
}

/*
 * Add a contiguous memory region to the buddy allocator.
 * Breaks it into the largest power-of-two aligned blocks possible.
 */
static void pmm_add_region(uintptr_t start, uintptr_t end)
{
    uintptr_t current = PAGE_ALIGN_UP(start);
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
        set_block_state(current, order, BLOCK_FREE);
        free_list_add(current, order);
        buddy_state.free_pages += order_to_pages(order);

        current += block_size;
    }
}

void pmm_init(void)
{
    uintptr_t kernel_end = (uintptr_t)&__kernel_end;
    buddy_state.heap_start = PAGE_ALIGN_UP(kernel_end);
#if defined(PLATFORM_RASPI5)
    /* Reserve last 2MB for non-cacheable shared memory (see ncmem.h) */
    buddy_state.heap_end = (RAM_BASE + RAM_SIZE) - 0x200000UL;
#else
    buddy_state.heap_end = RAM_BASE + RAM_SIZE;
#endif

#if defined(PLATFORM_X86_64)
    /* On x86-64, RAM_SIZE in platform.h is the MAX array size, not actual RAM.
     * Use the detected RAM end from vmm_init() (Multiboot2 memory map). */
    {
        extern uintptr_t x86_detected_ram_end;
        if (x86_detected_ram_end > 0)
            buddy_state.heap_end = x86_detected_ram_end;
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

    buddy_state.initialized = true;

    INFO("PMM initialized (buddy allocator)");
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
    uintptr_t addr = buddy_alloc(order);
    spin_unlock_irqrestore(&pmm_lock, flags);

    if (addr == PMM_ALLOC_FAIL) {
        WARN("PMM: Cannot allocate %u pages (order %u)", (unsigned)count, order);
        return NULL;
    }

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
             blk = blk->next) {
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
    buddy_state.free_pages -= order_to_pages(order);
    buddy_state.alloc_count++;

    spin_unlock_irqrestore(&pmm_lock, flags);
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

    /* Check for double-free */
    if (get_block_state(addr) == BLOCK_FREE) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        WARN("PMM: Double-free detected at 0x%lx", addr);
        return;
    }

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
