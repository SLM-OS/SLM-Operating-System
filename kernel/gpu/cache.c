/*
 * ARM64 Cache Maintenance Operations
 *
 * Provides cache coherency for CPU/GPU (and other DMA) data sharing.
 *
 * Key operations:
 * - Clean: Write back dirty cache lines to memory (DC CVAC)
 * - Invalidate: Discard cached data, force reload from memory (DC IVAC)
 * - Flush: Clean + Invalidate (DC CIVAC)
 *
 * Usage for GPU DMA:
 * 1. CPU writes data, GPU will read:  cache_clean_range()
 * 2. GPU writes data, CPU will read:  cache_invalidate_range()
 * 3. Bidirectional:                   cache_flush_range()
 *
 * Note: On Cortex-A53/A55, DC IVAC may perform clean before invalidate
 * if the line is dirty. This is implementation-defined behavior.
 */

#include "gpu.h"
#include <stdint.h>
#include <stddef.h>

/*
 * Cache line size for ARMv8-A.
 *
 * Most Cortex-A cores use 64-byte cache lines.
 * Could be read from CTR_EL0.DminLine, but 64 is safe for all current cores.
 */
#define CACHE_LINE_SIZE     64
#define CACHE_LINE_MASK     (CACHE_LINE_SIZE - 1)

/*
 * Round address down to cache line boundary.
 */
static inline uintptr_t cache_line_start(void *addr)
{
    return (uintptr_t)addr & ~(uintptr_t)CACHE_LINE_MASK;
}

/*
 * Round address up to next cache line boundary.
 */
static inline uintptr_t cache_line_end(void *addr, size_t size)
{
    return ((uintptr_t)addr + size + CACHE_LINE_MASK) & ~(uintptr_t)CACHE_LINE_MASK;
}

/*
 * Clean data cache by virtual address to Point of Coherency.
 *
 * DC CVAC: Data Cache Clean by VA to PoC
 * Writes back any dirty cache lines to memory without invalidating.
 * Use before GPU/DMA reads from the buffer.
 */
void cache_clean_range(void *addr, size_t size)
{
    if (!addr || size == 0) {
        return;
    }

    uintptr_t start = cache_line_start(addr);
    uintptr_t end = cache_line_end(addr, size);

    /* Clean each cache line */
    for (uintptr_t line = start; line < end; line += CACHE_LINE_SIZE) {
        __asm__ volatile("dc cvac, %0" : : "r"(line) : "memory");
    }

    /* Data Synchronization Barrier - ensure all cache ops complete */
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Invalidate data cache by virtual address to Point of Coherency.
 *
 * DC IVAC: Data Cache Invalidate by VA to PoC
 * Discards cached data, forcing reload from memory on next access.
 * Use after GPU/DMA writes to the buffer.
 *
 * WARNING: Any dirty (unwritten) data in the cache will be lost!
 * Only use on buffers that were not modified by CPU since last clean.
 */
void cache_invalidate_range(void *addr, size_t size)
{
    if (!addr || size == 0) {
        return;
    }

    uintptr_t start = cache_line_start(addr);
    uintptr_t end = cache_line_end(addr, size);

    /* Invalidate each cache line */
    for (uintptr_t line = start; line < end; line += CACHE_LINE_SIZE) {
        __asm__ volatile("dc ivac, %0" : : "r"(line) : "memory");
    }

    /* Data Synchronization Barrier - ensure all cache ops complete */
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Clean and invalidate data cache by virtual address to Point of Coherency.
 *
 * DC CIVAC: Data Cache Clean and Invalidate by VA to PoC
 * Writes back dirty lines, then invalidates. Safe for bidirectional sharing.
 * Use when both CPU and GPU may have modified the buffer.
 */
void cache_flush_range(void *addr, size_t size)
{
    if (!addr || size == 0) {
        return;
    }

    uintptr_t start = cache_line_start(addr);
    uintptr_t end = cache_line_end(addr, size);

    /* Clean and invalidate each cache line */
    for (uintptr_t line = start; line < end; line += CACHE_LINE_SIZE) {
        __asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
    }

    /* Data Synchronization Barrier - ensure all cache ops complete */
    __asm__ volatile("dsb sy" ::: "memory");
}
