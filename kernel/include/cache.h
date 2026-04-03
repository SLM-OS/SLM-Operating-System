/*
 * cache.h - Cache maintenance helpers for cross-CPU data sharing
 *
 * On Pi 5, TF-A doesn't set CPUECTLR_EL1.SMPEN for secondary cores,
 * so L1 cache writes don't participate in the coherency protocol.
 * Regular stores from one CPU are invisible to other CPUs.
 *
 * These helpers provide explicit cache maintenance:
 *   cache_clean(addr)      — push dirty cacheline to Point of Coherency
 *   cache_invalidate(addr) — discard local cacheline, force re-read from PoC
 *   cache_clean_range(addr, size) — clean multiple cachelines
 *
 * On QEMU and platforms with working coherency, these are no-ops.
 * Spinlock-protected accesses (ldaxr/stxr) don't need these — the
 * exclusive monitor has its own coherency mechanism.
 */

#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

/* Cortex-A76 cacheline size */
#define CACHE_LINE_SIZE 64

/*
 * Pi 5: explicit cache maintenance needed (SMPEN not set by TF-A).
 * Other platforms: coherency works, these are just barriers.
 */
#if defined(PLATFORM_RASPI5)

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
    const char *p = (const char *)addr;
    const char *end = p + size;
    for (; p < end; p += CACHE_LINE_SIZE) {
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Invalidate a range of memory (multiple cachelines) */
static inline void cache_invalidate_range(const volatile void *addr, size_t size)
{
    const char *p = (const char *)addr;
    const char *end = p + size;
    for (; p < end; p += CACHE_LINE_SIZE) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

#else /* !PLATFORM_RASPI5 — coherency works */

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

#endif /* PLATFORM_RASPI5 */

#endif /* CACHE_H */
