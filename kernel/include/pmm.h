/*
 * pmm.h - Physical Memory Manager for SLM-OS
 *
 * Buddy allocator for efficient O(log n) allocation of power-of-two page counts.
 * Automatically coalesces adjacent free blocks to reduce fragmentation.
 *
 * Note: Allocations are rounded up to the next power of 2.
 * For example, requesting 3 pages returns 4 pages (order 2).
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

/* Page sizes */
#define PAGE_SIZE           4096            /* 4 KB standard page */
#define PAGE_SHIFT          12              /* log2(PAGE_SIZE) */
#define PAGE_MASK           (~(PAGE_SIZE - 1))

/* Huge pages (for model weights, future use) */
#define HUGE_PAGE_SIZE      (2 * 1024 * 1024)   /* 2 MB */
#define HUGE_PAGE_SHIFT     21

/* Page alignment macros */
#define PAGE_ALIGN_UP(addr)     (((addr) + PAGE_SIZE - 1) & PAGE_MASK)
#define PAGE_ALIGN_DOWN(addr)   ((addr) & PAGE_MASK)

/* Convert between addresses and page frame numbers */
#define ADDR_TO_PFN(addr)   ((addr) >> PAGE_SHIFT)
#define PFN_TO_ADDR(pfn)    ((pfn) << PAGE_SHIFT)

/* PMM statistics */
struct pmm_stats {
    size_t total_pages;         /* Total physical pages */
    size_t free_pages;          /* Currently free pages */
    size_t used_pages;          /* Currently allocated pages */
    size_t reserved_pages;      /* Pages reserved (kernel, etc.) */
    uintptr_t heap_start;       /* First allocatable address */
    uintptr_t heap_end;         /* Last allocatable address + 1 */
};

/* Buddy allocator specific statistics (for testing/debugging).
 *
 * Bumped 2026-05-02 from 18 (1 GiB) to 19 (2 GiB) so a Q4_K_M GGUF
 * for Qwen2.5-1.5B (1.04 GB) fits in a single contiguous allocation.
 * The cost is negligible — `struct buddy_state` adds one extra
 * free-list pointer + one count (~16 bytes). All call sites that
 * loop `0..=MAX_ORDER` get one extra iteration, irrelevant in
 * boot/teardown context. Free-list / split / merge logic is
 * order-agnostic; #578 follow-up tracks model_mem multi-block
 * support which would obviate further bumps. */
#define PMM_MAX_ORDER   19      /* Maximum order: 2^19 = 524288 pages (2 GB) */

struct pmm_buddy_stats {
    size_t free_counts[PMM_MAX_ORDER + 1];  /* Free blocks at each order */
    size_t alloc_count;         /* Total allocation operations */
    size_t free_count;          /* Total free operations */
    size_t split_count;         /* Number of block splits */
    size_t merge_count;         /* Number of block merges (coalesces) */
};

/*
 * Initialize the physical memory manager.
 *
 * Must be called early in boot, after UART is available for debug output.
 * Uses kernel_end symbol from linker script to determine heap start.
 */
void pmm_init(void);

/*
 * Maximum runtime memreserves the PMM tracks. Sized for the
 * GA10B GMMU use case: one entry covering Linux's inst block +
 * PDB chain region (typically a single 1-2 MB cluster). Bump if
 * a future consumer needs to reserve more disjoint ranges.
 */
#define PMM_MAX_RUNTIME_RESERVES   8

/*
 * Append a runtime-discovered phys range to the PMM reserve list.
 * MUST be called BEFORE pmm_init — reserves take effect inside
 * pmm_add_region_split's carve pass.
 *
 * Used to keep specific phys pages out of the buddy allocator
 * when some other consumer needs them at fixed addresses. The
 * canonical use case is the GA10B GPU inst block + PDB chain
 * after kexec: the GPU's MMU walker reads from those phys, so
 * SLM-OS PMM must not reuse them. See `kernel/src/main.c` for
 * the call site (Jetson-only, between vmm_init and pmm_init).
 *
 * Returns 0 on success, -1 if the reserve table is full
 * (PMM_MAX_RUNTIME_RESERVES) or `size` is 0.
 */
int pmm_runtime_reserve_add(uint64_t addr, uint64_t size);

/*
 * Allocate a single 4KB physical page.
 *
 * Returns: Physical address of allocated page, or 0 on failure.
 */
void *pmm_alloc_page(void);

/*
 * Allocate contiguous physical pages.
 *
 * @count: Number of contiguous pages to allocate.
 *         Rounded up to next power of 2 (e.g., 3 -> 4 pages).
 * Returns: Physical address of first page, or 0 on failure.
 *
 * Complexity: O(log n) where n is the number of pages.
 */
void *pmm_alloc_pages(size_t count);

/*
 * Allocate contiguous physical pages, preferring the LOWEST-address
 * free block large enough. Same shape as pmm_alloc_pages but walks
 * every free list to pick the lowest-address candidate, splitting
 * down as needed.
 *
 * Use case: PCIe inbound translation windows on some platforms only
 * cover the bottom of physical RAM, so DMA buffers MUST live there.
 * The buddy allocator's normal LIFO pop returns high-end blocks,
 * which puts DMA targets out of reach.
 *
 * Performance: linear scan over free lists at every order >= the
 * requested order, holding pmm_lock + IRQs-off for the full scan.
 * Bounded by total-free-block count (the allocator coalesces
 * aggressively on free, so typically a few dozen blocks across all
 * orders). NOT safe for per-submit hot paths — intended for per-load
 * or per-DMA-buffer setup only.
 *
 * @count: Number of contiguous pages. Rounded up to next power of 2.
 * Returns: Physical address of first page, or NULL on failure.
 */
void *pmm_alloc_pages_low(size_t count);

/*
 * Free a single physical page.
 *
 * @page: Physical address of page to free (must be page-aligned).
 */
void pmm_free_page(void *page);

/*
 * Free contiguous physical pages.
 *
 * @page: Physical address of first page to free.
 * @count: Number of pages to free.
 */
void pmm_free_pages(void *page, size_t count);

/*
 * Get current PMM statistics.
 *
 * @stats: Pointer to stats structure to fill.
 */
void pmm_get_stats(struct pmm_stats *stats);

/*
 * Print PMM statistics to UART.
 */
void pmm_dump_stats(void);

/*
 * Get number of free pages.
 */
size_t pmm_get_free_pages(void);

/*
 * Get total number of managed pages.
 */
size_t pmm_get_total_pages(void);

/*
 * Get buddy allocator specific statistics.
 * Useful for testing and debugging coalescing behavior.
 *
 * @stats: Pointer to buddy stats structure to fill.
 */
void pmm_get_buddy_stats(struct pmm_buddy_stats *stats);

/*
 * Walk the free list at the given order and print each block's
 * address + next/prev pointer values. Bounded at `max_blocks` to
 * keep a corrupted cycle from looping forever.
 *
 * Diagnostic only — output goes to UART (uart_printf), not the
 * active shell's I/O. Capture via serial console. Used by
 * `mem buddy <order>` to debug free-list corruption (e.g., #608's
 * order-19 fault).
 */
void pmm_dump_free_list(unsigned int order, size_t max_blocks);

#endif /* PMM_H */
