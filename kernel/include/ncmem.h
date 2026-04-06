/*
 * ncmem.h - Non-cacheable shared memory allocator
 *
 * On Pi 5, TF-A doesn't set SMPEN so L1/L2 caches are incoherent across
 * CPUs. DC CIVAC only flushes L1 — per-core L2 retains stale data.
 *
 * Non-cacheable memory bypasses L1/L2 entirely, making writes instantly
 * visible to all CPUs. Used for scheduler run queue data that must be
 * shared across cores.
 *
 * The NC region is the last 2MB of physical RAM, mapped with MAIR index 2
 * (Normal Non-Cacheable, Inner Shareable) via an L2 table entry in vmm.c.
 */

#ifndef NCMEM_H
#define NCMEM_H

#include <stdint.h>
#include <stddef.h>

#if defined(PLATFORM_RASPI5)

/* Non-cacheable shared memory region (last 2MB of 4GB RAM) */
#define NC_MEM_BASE     0xFFE00000UL
#define NC_MEM_SIZE     0x00200000UL    /* 2 MB */

/* Initialize NC memory allocator (call after vmm_init) */
void ncmem_init(void);

/* Allocate from NC region (bump allocator, cacheline-aligned) */
void *ncmem_alloc(size_t size, size_t align);

/* Return bytes allocated so far */
size_t ncmem_used(void);

#else /* !PLATFORM_RASPI5 — caches are coherent, NC not needed */

static inline void ncmem_init(void) {}
static inline void *ncmem_alloc(size_t size, size_t align)
{
    (void)size; (void)align;
    return NULL;
}
static inline size_t ncmem_used(void) { return 0; }

#endif /* PLATFORM_RASPI5 */

#endif /* NCMEM_H */
