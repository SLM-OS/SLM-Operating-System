/*
 * pmm.c - Physical Memory Manager for SLM-OS
 *
 * Bitmap-based physical page allocator.
 * Each bit in the bitmap represents one 4KB page:
 *   0 = free, 1 = allocated/reserved
 */

#include "pmm.h"
#include "platform.h"
#include "uart.h"
#include "debug.h"

/* External symbols from linker script */
extern char __kernel_end;

/* Bitmap storage - statically allocated for simplicity */
#define MAX_PAGES       (RAM_SIZE / PAGE_SIZE)
#define BITMAP_SIZE     ((MAX_PAGES + 7) / 8)

static uint8_t page_bitmap[BITMAP_SIZE];

/* PMM state */
static struct {
    uintptr_t heap_start;       /* First allocatable address (page-aligned) */
    uintptr_t heap_end;         /* End of RAM */
    size_t total_pages;         /* Total pages in heap */
    size_t free_pages;          /* Currently free */
    size_t reserved_pages;      /* Kernel + bitmap pages */
    int initialized;            /* Guard against use before init */
} pmm_state;

/*
 * Bitmap operations
 */
static inline void bitmap_set(size_t bit)
{
    page_bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(size_t bit)
{
    page_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline int bitmap_test(size_t bit)
{
    return (page_bitmap[bit / 8] >> (bit % 8)) & 1;
}

/*
 * Convert physical address to page index (relative to heap_start)
 */
static inline size_t addr_to_index(uintptr_t addr)
{
    return (addr - pmm_state.heap_start) >> PAGE_SHIFT;
}

/*
 * Convert page index to physical address
 */
static inline uintptr_t index_to_addr(size_t index)
{
    return pmm_state.heap_start + (index << PAGE_SHIFT);
}

/*
 * Find first free page in bitmap
 * Returns index or MAX_PAGES if none found
 */
static size_t find_free_page(void)
{
    for (size_t i = 0; i < pmm_state.total_pages; i++) {
        if (!bitmap_test(i)) {
            return i;
        }
    }
    return pmm_state.total_pages; /* None found */
}

/*
 * Find contiguous free pages in bitmap
 * Returns starting index or MAX_PAGES if none found
 */
static size_t find_free_pages(size_t count)
{
    if (count == 0 || count > pmm_state.total_pages) {
        return pmm_state.total_pages;
    }

    size_t consecutive = 0;
    size_t start = 0;

    for (size_t i = 0; i < pmm_state.total_pages; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) {
                start = i;
            }
            consecutive++;
            if (consecutive == count) {
                return start;
            }
        } else {
            consecutive = 0;
        }
    }

    return pmm_state.total_pages; /* Not enough contiguous pages */
}

/*
 * Initialize the physical memory manager.
 */
void pmm_init(void)
{
    /* Calculate heap boundaries */
    uintptr_t kernel_end = (uintptr_t)&__kernel_end;
    pmm_state.heap_start = PAGE_ALIGN_UP(kernel_end);
    pmm_state.heap_end = RAM_BASE + RAM_SIZE;

    /* Calculate page counts */
    pmm_state.total_pages = (pmm_state.heap_end - pmm_state.heap_start) / PAGE_SIZE;
    pmm_state.reserved_pages = (pmm_state.heap_start - RAM_BASE) / PAGE_SIZE;

    /* Clear bitmap - all pages start as free */
    for (size_t i = 0; i < BITMAP_SIZE; i++) {
        page_bitmap[i] = 0;
    }

    pmm_state.free_pages = pmm_state.total_pages;
    pmm_state.initialized = 1;

    INFO("PMM initialized");
    DEBUG_PRINT("  Kernel ends at:  0x%lx", kernel_end);
    DEBUG_PRINT("  Heap start:      0x%lx", pmm_state.heap_start);
    DEBUG_PRINT("  Heap end:        0x%lx", pmm_state.heap_end);
    DEBUG_PRINT("  Total pages:     %u", (unsigned)pmm_state.total_pages);
    DEBUG_PRINT("  Reserved pages:  %u (kernel)", (unsigned)pmm_state.reserved_pages);
}

/*
 * Allocate a single physical page.
 */
void *pmm_alloc_page(void)
{
    if (!pmm_state.initialized) {
        ERROR("PMM not initialized");
        return (void *)0;
    }

    size_t index = find_free_page();
    if (index >= pmm_state.total_pages) {
        WARN("PMM: Out of memory");
        return (void *)0;
    }

    bitmap_set(index);
    pmm_state.free_pages--;

    return (void *)index_to_addr(index);
}

/*
 * Allocate contiguous physical pages.
 */
void *pmm_alloc_pages(size_t count)
{
    if (!pmm_state.initialized) {
        ERROR("PMM not initialized");
        return (void *)0;
    }

    if (count == 0) {
        return (void *)0;
    }

    if (count == 1) {
        return pmm_alloc_page();
    }

    size_t start = find_free_pages(count);
    if (start >= pmm_state.total_pages) {
        WARN("PMM: Cannot allocate %u contiguous pages", (unsigned)count);
        return (void *)0;
    }

    /* Mark all pages as allocated */
    for (size_t i = 0; i < count; i++) {
        bitmap_set(start + i);
    }
    pmm_state.free_pages -= count;

    return (void *)index_to_addr(start);
}

/*
 * Free a single physical page.
 */
void pmm_free_page(void *page)
{
    if (!pmm_state.initialized) {
        ERROR("PMM not initialized");
        return;
    }

    uintptr_t addr = (uintptr_t)page;

    /* Validate address */
    if (addr < pmm_state.heap_start || addr >= pmm_state.heap_end) {
        ERROR("PMM: Invalid free address 0x%lx", addr);
        return;
    }

    if (addr & (PAGE_SIZE - 1)) {
        ERROR("PMM: Unaligned free address 0x%lx", addr);
        return;
    }

    size_t index = addr_to_index(addr);

    /* Check for double-free */
    if (!bitmap_test(index)) {
        WARN("PMM: Double-free detected at 0x%lx", addr);
        return;
    }

    bitmap_clear(index);
    pmm_state.free_pages++;
}

/*
 * Free contiguous physical pages.
 */
void pmm_free_pages(void *page, size_t count)
{
    if (!pmm_state.initialized) {
        ERROR("PMM not initialized");
        return;
    }

    uintptr_t addr = (uintptr_t)page;

    for (size_t i = 0; i < count; i++) {
        pmm_free_page((void *)(addr + i * PAGE_SIZE));
    }
}

/*
 * Get PMM statistics.
 */
void pmm_get_stats(struct pmm_stats *stats)
{
    if (!stats) {
        return;
    }

    stats->total_pages = pmm_state.total_pages;
    stats->free_pages = pmm_state.free_pages;
    stats->used_pages = pmm_state.total_pages - pmm_state.free_pages;
    stats->reserved_pages = pmm_state.reserved_pages;
    stats->heap_start = pmm_state.heap_start;
    stats->heap_end = pmm_state.heap_end;
}

/*
 * Print PMM statistics.
 */
void pmm_dump_stats(void)
{
    struct pmm_stats stats;
    pmm_get_stats(&stats);

    uart_puts("\nPMM Statistics:\n");
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
}

/*
 * Convenience functions
 */
size_t pmm_get_free_pages(void)
{
    return pmm_state.free_pages;
}

size_t pmm_get_total_pages(void)
{
    return pmm_state.total_pages;
}
