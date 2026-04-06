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
 * Check if SMPEN (bit 6) is set in CPUECTLR_EL1.
 * On Cortex-A76, this register is encoded as S3_0_C15_C1_4.
 * Returns true if the current core has cache coherency enabled.
 */
#if defined(PLATFORM_RASPI5)
#include <stdbool.h>
static inline bool cpu_has_smpen(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, S3_0_C15_C1_4" : "=r"(val));
    return (val & (1UL << 6)) != 0;
}
#else
static inline bool cpu_has_smpen(void) { return true; }
#endif

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

#else /* ARM64 — coherency works (QEMU, Jetson) */

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
