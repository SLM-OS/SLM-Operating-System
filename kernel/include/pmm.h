/*
 * pmm.h - Physical Memory Manager for SLM-OS
 *
 * Bitmap-based physical page allocator.
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

/*
 * Initialize the physical memory manager.
 *
 * Must be called early in boot, after UART is available for debug output.
 * Uses kernel_end symbol from linker script to determine heap start.
 */
void pmm_init(void);

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
 * Returns: Physical address of first page, or 0 on failure.
 */
void *pmm_alloc_pages(size_t count);

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

#endif /* PMM_H */
