/*
 * ncmem.h - Non-cacheable memory allocator for device DMA
 *
 * Pi 5 and Jetson reserve one 2 MB block of DRAM that vmm.c maps with
 * MAIR index 2 (Normal Non-Cacheable, Inner Shareable) via an L2 table
 * entry and pmm.c keeps out of the page allocator. The physical address
 * is platform-specific. PLATFORM_HAS_NC_REGION marks the platforms that
 * have it, and ncmem_alloc() hands out pieces of it.
 *
 * Its users are devices that do not snoop the CPU caches: xHCI rings,
 * MACB, CDC-ECM and USB bounce buffers, camera RCE, plus the fixed-
 * address diagnostic slots (diag_pi5.h). CPU-to-CPU shared state (run
 * queues, task table, current-task pointers, steal deques, counters)
 * lives in ordinary cacheable memory: the cores keep their caches
 * coherent in hardware (docs/adr/0006-cross-cpu-coherency.md, #1193).
 * Do not allocate CPU-shared state here.
 */

#ifndef NCMEM_H
#define NCMEM_H

#include <stdint.h>
#include <stddef.h>

#if defined(PLATFORM_RASPI5) || defined(PLATFORM_JETSON_ORIN_NANO)

/* The NC block and its allocator (see the header comment). */
#define PLATFORM_HAS_NC_REGION 1

#if defined(PLATFORM_RASPI5)
#define NC_MEM_BASE     0xFFE00000UL    /* Last 2MB of 4GB RAM */
#elif defined(PLATFORM_JETSON_ORIN_NANO)
#define NC_MEM_BASE     0xBDE00000UL    /* Last 2MB of region 1 (before OP-TEE carveout) */
#endif
#define NC_MEM_SIZE     0x00200000UL    /* 2 MB */
#if defined(PLATFORM_JETSON_ORIN_NANO)
#define NC_MEM_RESERVED_BYTES 0x00005000UL
#else
#define NC_MEM_RESERVED_BYTES 0x00001000UL
#endif

/* Initialize NC memory allocator (call after vmm_init) */
void ncmem_init(void);

/* Allocate from NC region (bump allocator, cacheline-aligned) */
void *ncmem_alloc(size_t size, size_t align);

/* Return bytes allocated so far */
size_t ncmem_used(void);

#else /* No NC region (QEMU, x86-64) */

static inline void ncmem_init(void) {}
static inline void *ncmem_alloc(size_t size, size_t align)
{
    (void)size; (void)align;
    return NULL;
}
static inline size_t ncmem_used(void) { return 0; }

#endif

#endif /* NCMEM_H */
