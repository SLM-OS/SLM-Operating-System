/*
 * cache.h - Cache maintenance helpers for cross-CPU data sharing
 *
 * Pi 5 and Jetson treat cross-CPU data as needing explicit maintenance
 * (the NC-memory model, kernel/CLAUDE.md). SMPEN plays no part in
 * this: CPUECTLR_EL1 bit 6 is RES0 on Cortex-A76 and PF_MODE[0] on
 * Cortex-A78AE, and neither core has a coherency-enable bit (#1194).
 *
 * These helpers provide explicit cache maintenance:
 *   cache_clean(addr)      — push dirty cacheline to Point of Coherency
 *   cache_invalidate(addr) — discard local cacheline, force re-read from PoC
 *   cache_clean_range(addr, size) — clean multiple cachelines
 *
 * On QEMU and platforms with working coherency, these are no-ops.
 * Spinlocks are NOT exempt: the exclusive monitor does not provide
 * coherency on Pi 5: a waiter's ldaxr was observed to spin
 * forever on a stale "locked" cacheline. spinlock.h's
 * PLATFORM_RASPI5 path does its own DC CIVAC + DSB SY around
 * ldaxr/stlr, so lock users still never call these helpers for the
 * lock word itself (see kernel/CLAUDE.md §"Pi 5 spinlock DRAM flush").
 */

#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>
#include <stdint.h>

/* Cortex-A76 cacheline size */
#define CACHE_LINE_SIZE 64

/*
 * Pi 5 and Jetson: explicit cache maintenance by VA.
 * Other platforms: coherency works, these are just barriers.
 */
#if defined(PLATFORM_RASPI5) || defined(PLATFORM_JETSON_ORIN_NANO)

/* Clean cacheline containing addr to Point of Coherency */
static inline void cache_clean(const volatile void *addr)
{
    __asm__ volatile("dc cvac, %0" :: "r"(addr) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Clean and invalidate cacheline — force re-read from PoC */
static inline void cache_invalidate(const volatile void *addr)
{
    __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Clean a range of memory (multiple cachelines) */
static inline void cache_clean_range(const volatile void *addr, size_t size)
{
    /* Start at the line containing `addr`: stepping by line size from
     * an unaligned start can stop one line short of the end. */
    const char *end = (const char *)addr + size;
    const char *p = (const char *)((uintptr_t)addr & ~(uintptr_t)(CACHE_LINE_SIZE - 1));
    for (; p < end; p += CACHE_LINE_SIZE) {
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Invalidate a range of memory (multiple cachelines) */
static inline void cache_invalidate_range(const volatile void *addr, size_t size)
{
    /* Line-aligned start, as in cache_clean_range. */
    const char *end = (const char *)addr + size;
    const char *p = (const char *)((uintptr_t)addr & ~(uintptr_t)(CACHE_LINE_SIZE - 1));
    for (; p < end; p += CACHE_LINE_SIZE) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Discard cacheline without writeback (DC IVAC).
 * DANGEROUS: any uncleaned dirty data in the cacheline is lost.
 */
static inline void cache_discard_range(const volatile void *addr, size_t size)
{
    const char *p = (const char *)addr;
    const char *end = p + size;
    for (; p < end; p += CACHE_LINE_SIZE) {
        __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

#elif defined(PLATFORM_X86_64)

/* x86-64: fully hardware cache coherent, no explicit maintenance needed */
static inline void cache_clean(const volatile void *addr)
{
    (void)addr;
    __asm__ volatile("" ::: "memory");
}

static inline void cache_invalidate(const volatile void *addr)
{
    (void)addr;
    __asm__ volatile("" ::: "memory");
}

static inline void cache_clean_range(const volatile void *addr, size_t size)
{
    (void)addr; (void)size;
    __asm__ volatile("" ::: "memory");
}

static inline void cache_invalidate_range(const volatile void *addr, size_t size)
{
    (void)addr; (void)size;
    __asm__ volatile("" ::: "memory");
}

#else /* ARM64 — coherency works (QEMU) */

static inline void cache_clean(const volatile void *addr)
{
    (void)addr;
    __asm__ volatile("dmb ish" ::: "memory");
}

static inline void cache_invalidate(const volatile void *addr)
{
    (void)addr;
    __asm__ volatile("dmb ish" ::: "memory");
}

static inline void cache_clean_range(const volatile void *addr, size_t size)
{
    (void)addr;
    (void)size;
    __asm__ volatile("dmb ish" ::: "memory");
}

static inline void cache_invalidate_range(const volatile void *addr, size_t size)
{
    (void)addr;
    (void)size;
    __asm__ volatile("dmb ish" ::: "memory");
}

static inline void cache_discard_range(const volatile void *addr, size_t size)
{
    (void)addr;
    (void)size;
    __asm__ volatile("dmb ish" ::: "memory");
}

#endif /* cache platform selection */

#endif /* CACHE_H */
