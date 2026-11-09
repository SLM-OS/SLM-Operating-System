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
 * Spinlocks are NOT exempt: the exclusive monitor does not provide
 * coherency on Pi 5 — without SMPEN, a waiter's ldaxr can spin
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
 * Read bit 6 of CPUECTLR_EL1 (S3_0_C15_C1_4).
 *
 * The name is historical: SMPEN is a Cortex-A53/A57/A72 bit at a
 * different encoding (S3_1_C15_C2_1). On Cortex-A76 bit 6 is RES0 and
 * on Cortex-A78AE it is PF_MODE[0] (prefetch aggressiveness); neither
 * core has a coherency-enable bit, and both keep L1/L2 coherent in
 * hardware (A76 TRM §13.32, §5.9; A78AE TRM §3.2.47, §2.4.9; #1193 C0
 * facts Q1). The value says nothing about coherency. Reads are
 * permitted at NS-EL2; never write this register (it traps to EL3).
 */
#if defined(PLATFORM_RASPI5) || defined(PLATFORM_JETSON_ORIN_NANO)
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
