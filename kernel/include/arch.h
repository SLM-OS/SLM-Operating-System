/*
 * arch.h - Architecture-agnostic interface for SLM-OS
 *
 * Provides inline functions that abstract architecture-specific
 * operations (IRQ control, CPU halt, memory barriers). Platform
 * selection is via PLATFORM_X86_64 vs ARM64 at compile time.
 *
 * This header supplements (not replaces) platform.h (hardware addresses)
 * and spinlock.h (locking primitives). It provides the minimal set of
 * operations needed by shared kernel code without pulling in the full
 * platform-specific headers.
 */

#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>
#include "platform.h"

/* ============================================================================
 * IRQ Control
 * ============================================================================ */

#if defined(PLATFORM_X86_64)

static inline void arch_irq_enable(void)
{
    __asm__ volatile("sti" ::: "memory");
}

static inline void arch_irq_disable(void)
{
    __asm__ volatile("cli" ::: "memory");
}

static inline int arch_irqs_enabled(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    return (flags & 0x200) != 0;  /* IF flag (bit 9) */
}

#else /* ARM64 */

static inline void arch_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline void arch_irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

static inline int arch_irqs_enabled(void)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return (daif & (1 << 7)) == 0;  /* IRQ mask bit (bit 7 of DAIF) */
}

#endif

/* ============================================================================
 * CPU Halt / Idle
 * ============================================================================ */

#if defined(PLATFORM_X86_64)

static inline void arch_halt(void)
{
    __asm__ volatile("hlt");
}

static inline void arch_wait_for_event(void)
{
    __asm__ volatile("pause" ::: "memory");
}

#else /* ARM64 */

static inline void arch_halt(void)
{
    __asm__ volatile("wfi");
}

static inline void arch_wait_for_event(void)
{
    __asm__ volatile("wfe" ::: "memory");
}

#endif

/* ============================================================================
 * Memory Barriers
 * ============================================================================ */

#if defined(PLATFORM_X86_64)

/* x86-64 TSO model: stores are ordered, loads are ordered.
 * Only store-load reordering needs explicit fencing. */
#define arch_mb()   __asm__ volatile("mfence" ::: "memory")
#define arch_rmb()  __asm__ volatile("lfence" ::: "memory")
#define arch_wmb()  __asm__ volatile("sfence" ::: "memory")

#else /* ARM64 */

#define arch_mb()   __asm__ volatile("dmb ish" ::: "memory")
#define arch_rmb()  __asm__ volatile("dmb ishld" ::: "memory")
#define arch_wmb()  __asm__ volatile("dmb ishst" ::: "memory")

#endif

/* Compiler barrier (no hardware fence) */
#define arch_barrier() __asm__ volatile("" ::: "memory")

#endif /* ARCH_H */
