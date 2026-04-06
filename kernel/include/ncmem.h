/*
 * ncmem.h - Non-cacheable shared memory allocator
 *
 * On real ARM64 hardware (Pi 5, Jetson), per-core L2 caches are incoherent
 * across CPUs despite SMPEN being set. DC CIVAC doesn't propagate through
 * per-core L2, so cross-CPU data visibility requires non-cacheable memory.
 *
 * NC memory bypasses L1/L2 entirely — writes are instantly visible to all
 * CPUs. Used for boot flags, scheduler run queues, and other cross-CPU
 * shared data structures.
 *
 * The NC region is a 2MB block mapped with MAIR index 2 (Normal
 * Non-Cacheable, Inner Shareable) via an L2 table entry in vmm.c.
 * The physical address is platform-specific.
 */

#ifndef NCMEM_H
#define NCMEM_H

#include <stdint.h>
#include <stddef.h>

#if defined(PLATFORM_RASPI5) || defined(PLATFORM_JETSON_ORIN_NANO)

#if defined(PLATFORM_RASPI5)
#define NC_MEM_BASE     0xFFE00000UL    /* Last 2MB of 4GB RAM */
#elif defined(PLATFORM_JETSON_ORIN_NANO)
#define NC_MEM_BASE     0xBDE00000UL    /* Last 2MB of region 1 (before OP-TEE carveout) */
#endif
#define NC_MEM_SIZE     0x00200000UL    /* 2 MB */

/* Initialize NC memory allocator (call after vmm_init) */
void ncmem_init(void);

/* Allocate from NC region (bump allocator, cacheline-aligned) */
void *ncmem_alloc(size_t size, size_t align);

/* Return bytes allocated so far */
size_t ncmem_used(void);

#else /* No NC memory needed (QEMU caches are coherent) */

static inline void ncmem_init(void) {}
static inline void *ncmem_alloc(size_t size, size_t align)
{
    (void)size; (void)align;
    return NULL;
}
static inline size_t ncmem_used(void) { return 0; }

#endif

#endif /* NCMEM_H */
