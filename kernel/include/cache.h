/*
 * cache.h - Cache maintenance by VA to the Point of Coherency
 *
 *   cache_clean(addr)                  DC CVAC: write a dirty line back
 *   cache_invalidate(addr)             DC CIVAC: write back, then drop
 *   cache_clean_range(addr, size)      DC CVAC over a range
 *   cache_invalidate_range(addr, size) DC CIVAC over a range
 *   cache_discard_range(addr, size)    DC IVAC: drop without writeback
 *
 * Each is followed by DSB SY on Pi 5 and Jetson. Their real job there is
 * device DMA and the caches-off boot paths: a device that does not snoop
 * the CPU caches needs the CPU to clean what it wrote before the device
 * reads it, and to invalidate before the CPU reads what the device wrote.
 *
 * CPU-to-CPU sharing does not need them. Cortex-A76 and Cortex-A78AE keep
 * their L1 and L2 coherent in hardware, with no software enable, for
 * Normal Write-Back Inner Shareable memory (docs/adr/0006-cross-cpu-coherency.md,
 * #1193). The scheduler calls that remain on shared fields are redundant
 * but harmless. Spinlocks never call them for the lock word.
 *
 * On QEMU these are DMB ISH only; on x86-64, compiler barriers.
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

/* Pi 5 and Jetson: real maintenance by VA (see the header comment). */
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
