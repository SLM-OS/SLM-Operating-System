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
 *   Order 18: 262144 pages (1 GB) - maximum
 */

#include "pmm.h"
#include "platform.h"
#include "uart.h"
#include "debug.h"
#include "spinlock.h"
#include <stdbool.h>

/* External symbols from linker script */
extern char __kernel_end;

/* ==========================================================================
 * Buddy Allocator Configuration
 * ========================================================================== */

#define MAX_ORDER       18          /* Maximum order: 2^18 = 262144 pages (1 GB) */
#define MIN_BLOCK_SIZE  PAGE_SIZE   /* Minimum allocation: 4 KB */

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
    return (addr - buddy_state.heap_start) >> PAGE_SHIFT;
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
        return 0;
    }

    buddy_state.free_lists[order] = block->next;
    if (block->next) {
        block->next->prev = NULL;
    }

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

    return 0; /* Out of memory */
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
     */
    pmm_add_region(buddy_state.heap_start, 0xBDE00000UL);  /* Last 2MB reserved for NC memory */
    pmm_add_region(0xC2000000UL, 0xFFFE0000UL);
    pmm_add_region(0x100000000UL, 0x240000000UL);
#else
    pmm_add_region(buddy_state.heap_start, buddy_state.heap_end);
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

    if (addr == 0) {
        WARN("PMM: Cannot allocate %u pages (order %u)", (unsigned)count, order);
    }

    return (void *)addr;
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
    return buddy_state.free_pages;
}

size_t pmm_get_total_pages(void)
{
    return buddy_state.total_pages;
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
