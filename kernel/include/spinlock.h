/*
 * spinlock.h - Spinlocks and synchronization primitives for SLM-OS
 *
 * Provides spinlocks and ticket locks for multi-core synchronization.
 * Uses ARM64 exclusive load/store instructions for atomic operations.
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include "platform.h"   /* For PLATFORM_JETSON_ORIN_NANO (SPINLOCK_SKIP_LOCKING) */

/*
 * On Jetson after kexec, the exclusive monitor is broken — skip locking.
 *
 * On Pi 5, the exclusive monitor works but ONLY after the MMU is enabled.
 * Pre-MMU, memory is non-cacheable and ldaxr/stxr hang on Cortex-A76.
 * A runtime flag (spinlock_hw_enabled) gates real locking: set to true
 * by vmm_init() after MMU enable. Before that, spinlocks use barriers only.
 */
#if defined(SPINLOCK_SKIP_LOCKING)
#define SPINLOCK_SKIP_LOCKING 1
#endif

/*
 * Runtime flag: when false, spin_lock/spin_unlock use barriers only.
 * Set to true after MMU is enabled and exclusive monitor is functional.
 * Declared in spinlock.h, defined in vmm.c (set after MMU enable).
 */
#if !defined(SPINLOCK_SKIP_LOCKING)
extern volatile int spinlock_hw_enabled;
#endif

#if defined(PLATFORM_X86_64)
/*
 * Memory barrier macros for x86-64.
 *
 * x86-64 has a strong memory model: stores are not reordered with other
 * stores, loads are not reordered with other loads. Only store-load
 * reordering occurs. MFENCE provides a full barrier.
 */
#define dmb(opt)    __asm__ volatile("" ::: "memory")   /* compiler barrier suffices for most */
#define dsb(opt)    __asm__ volatile("mfence" ::: "memory")
#define isb()       __asm__ volatile("" ::: "memory")

#define smp_mb()    __asm__ volatile("mfence" ::: "memory")
#define smp_rmb()   __asm__ volatile("lfence" ::: "memory")
#define smp_wmb()   __asm__ volatile("sfence" ::: "memory")
#define barrier()   __asm__ volatile("" ::: "memory")

#else /* ARM64 */
/*
 * Memory barrier macros for ARM64.
 *
 * DMB (Data Memory Barrier) - ensures memory accesses complete
 * DSB (Data Synchronization Barrier) - ensures memory + cache ops complete
 * ISB (Instruction Synchronization Barrier) - flushes pipeline
 */
#define dmb(opt)    __asm__ volatile("dmb " #opt ::: "memory")
#define dsb(opt)    __asm__ volatile("dsb " #opt ::: "memory")
#define isb()       __asm__ volatile("isb" ::: "memory")

#define smp_mb()    dmb(ish)
#define smp_rmb()   dmb(ishld)
#define smp_wmb()   dmb(ishst)
#define barrier()   __asm__ volatile("" ::: "memory")
#endif /* PLATFORM_X86_64 */

/*
 * ============================================================================
 * Simple Spinlock
 * ============================================================================
 *
 * Basic test-and-set spinlock using ARM64 exclusive load/store.
 * Simple but unfair under contention (no FIFO ordering).
 */

typedef struct {
    volatile uint32_t lock;
} spinlock_t;

#define SPINLOCK_INIT { .lock = 0 }

/*
 * Initialize a spinlock.
 */
static inline void spin_init(spinlock_t *lock)
{
    lock->lock = 0;
}

/*
 * Acquire a spinlock.
 * Spins until the lock is acquired.
 *
 * NOTE: On Jetson after kexec, the ARM exclusive monitor state is corrupted
 * in a way that LDAXR/STXR operations hang (even without WFE). Since we're
 * running single-core after kexec, we skip locking entirely and just use
 * a memory barrier.
 *
 * On Pi 5, the exclusive monitor requires cacheable memory (MMU enabled).
 * Before MMU init, spinlock_hw_enabled is 0 and spin_lock uses barrier-only
 * fallback. After vmm_init() enables the MMU, spinlock_hw_enabled is set to 1
 * and real ldaxr/stxr operations are used.
 */
static inline void spin_lock(spinlock_t *lock)
{
#if defined(PLATFORM_X86_64)
    /* x86-64: test-and-set spinlock using LOCK XCHG */
    uint32_t val = 1;
    while (__atomic_exchange_n(&lock->lock, val, __ATOMIC_ACQUIRE) != 0) {
        /* Spin on read (avoids bus lock contention) until lock looks free */
        while (__atomic_load_n(&lock->lock, __ATOMIC_RELAXED) != 0)
            __asm__ volatile("pause" ::: "memory");
    }
#elif defined(SPINLOCK_SKIP_LOCKING)
    /* Jetson: skip locking, just barrier for memory ordering */
    (void)lock;
    dmb(ish);
#else
    /* Before MMU enable, exclusive monitor doesn't work (non-cacheable memory).
     * Use barrier-only fallback until vmm_init sets spinlock_hw_enabled. */
    if (!spinlock_hw_enabled) {
        (void)lock;
        dmb(ish);
        return;
    }

    uint32_t tmp;

    /* Use WFE for low-power spinning */
    __asm__ volatile(
        "   sevl\n"                     /* Set event locally (avoid initial WFE block) */
        "1: wfe\n"                      /* Wait for event (low power spin) */
        "   ldaxr   %w0, [%1]\n"        /* Load-acquire exclusive */
        "   cbnz    %w0, 1b\n"          /* If locked, retry */
        "   stxr    %w0, %w2, [%1]\n"   /* Try to store 1 (locked) */
        "   cbnz    %w0, 1b\n"          /* If store failed, retry */
        : "=&r"(tmp)
        : "r"(&lock->lock), "r"(1)
        : "memory"
    );
#endif
}

/*
 * Try to acquire a spinlock without blocking.
 * Returns: 1 if lock acquired, 0 if lock was already held.
 */
static inline int spin_trylock(spinlock_t *lock)
{
#if defined(PLATFORM_X86_64)
    return __atomic_exchange_n(&lock->lock, 1, __ATOMIC_ACQUIRE) == 0;
#elif defined(SPINLOCK_SKIP_LOCKING)
    (void)lock;
    dmb(ish);
    return 1;
#else
    if (!spinlock_hw_enabled) {
        (void)lock;
        dmb(ish);
        return 1;
    }

    uint32_t tmp, result;

    __asm__ volatile(
        "   ldaxr   %w0, [%2]\n"
        "   cbnz    %w0, 1f\n"
        "   stxr    %w0, %w3, [%2]\n"
        "   cbnz    %w0, 1f\n"
        "   mov     %w1, #1\n"
        "   b       2f\n"
        "1: mov     %w1, #0\n"
        "2:\n"
        : "=&r"(tmp), "=&r"(result)
        : "r"(&lock->lock), "r"(1)
        : "memory"
    );

    return result;
#endif
}

/*
 * Release a spinlock.
 */
static inline void spin_unlock(spinlock_t *lock)
{
#if defined(PLATFORM_X86_64)
    __atomic_store_n(&lock->lock, 0, __ATOMIC_RELEASE);
#elif defined(SPINLOCK_SKIP_LOCKING)
    (void)lock;
    dmb(ish);
#else
    if (!spinlock_hw_enabled) {
        (void)lock;
        dmb(ish);
        return;
    }

    __asm__ volatile(
        "   stlr    wzr, [%0]\n"
        "   sev\n"
        :
        : "r"(&lock->lock)
        : "memory"
    );
#endif
}

/*
 * Check if a spinlock is currently held.
 * Note: This is racy and should only be used for debugging.
 */
static inline int spin_is_locked(spinlock_t *lock)
{
    return lock->lock != 0;
}

/*
 * ============================================================================
 * Ticket Lock
 * ============================================================================
 *
 * Fair spinlock using a ticket system. Waiters are served in FIFO order.
 * Better than simple spinlock under contention.
 */

typedef struct {
    volatile uint16_t next;     /* Next ticket to be issued */
    volatile uint16_t owner;    /* Ticket currently being served */
} ticket_lock_t;

#define TICKET_LOCK_INIT { .next = 0, .owner = 0 }

/*
 * Initialize a ticket lock.
 */
static inline void ticket_init(ticket_lock_t *lock)
{
    lock->next = 0;
    lock->owner = 0;
}

/*
 * Acquire a ticket lock.
 * Waiters are served in FIFO order.
 */
static inline void ticket_lock(ticket_lock_t *lock)
{
#if defined(PLATFORM_X86_64)
    /* x86-64: atomic fetch-and-add using LOCK XADD */
    uint16_t ticket;
    __asm__ volatile(
        "lock xaddw %0, %1"
        : "=r"(ticket), "+m"(lock->next)
        : "0"((uint16_t)1)
        : "memory"
    );
    while (lock->owner != ticket) {
        __asm__ volatile("pause" ::: "memory");
    }
#else
    uint32_t status;
    uint16_t ticket;
    uint16_t next_ticket;

    /* Atomically fetch and increment the next ticket */
    __asm__ volatile(
        "1: ldaxrh  %w0, [%3]\n"        /* Load-acquire exclusive (next) */
        "   add     %w1, %w0, #1\n"     /* Increment */
        "   stxrh   %w2, %w1, [%3]\n"   /* Store exclusive (status separate) */
        "   cbnz    %w2, 1b\n"          /* Retry if failed */
        : "=&r"(ticket), "=&r"(next_ticket), "=&r"(status)
        : "r"(&lock->next)
        : "memory"
    );

    /* Wait until our ticket is called.
     *
     * Use load-acquire (LDARH) when re-reading owner after WFE returns:
     * this provides the acquire barrier in-line with the load, so we can
     * drop the separate dmb(ish) that used to follow the spin loop.
     * A plain volatile read followed by dmb was susceptible to a stale
     * cached value being compared before the barrier took effect.
     */
    {
        uint16_t seen;
        while (1) {
            __asm__ volatile("ldarh %w0, [%1]"
                : "=r"(seen)
                : "r"(&lock->owner)
                : "memory");
            if (seen == ticket)
                break;
            __asm__ volatile("wfe" ::: "memory");
        }
    }
#endif
}

/*
 * Release a ticket lock.
 */
static inline void ticket_unlock(ticket_lock_t *lock)
{
    uint16_t next_owner = lock->owner + 1;

#if defined(PLATFORM_X86_64)
    barrier();
    lock->owner = next_owner;
#else
    /* Release barrier then update owner */
    dmb(ish);
    lock->owner = next_owner;

    /* Wake waiters */
    __asm__ volatile("sev" ::: "memory");
#endif
}

/*
 * ============================================================================
 * IRQ-safe Spinlocks
 * ============================================================================
 *
 * Spinlocks that disable interrupts to prevent deadlock when a lock
 * is held by thread context and an interrupt handler tries to acquire it.
 */

typedef uint64_t irq_flags_t;

/*
 * Disable IRQs and save previous state.
 */
static inline irq_flags_t irq_save(void)
{
    irq_flags_t flags;
#if defined(PLATFORM_X86_64)
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
#else
    __asm__ volatile(
        "mrs    %0, daif\n"
        "msr    daifset, #2\n"      /* Set IRQ mask bit */
        : "=r"(flags)
        :
        : "memory"
    );
#endif
    return flags;
}

/*
 * Restore IRQ state.
 */
static inline void irq_restore(irq_flags_t flags)
{
#if defined(PLATFORM_X86_64)
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
#else
    __asm__ volatile(
        "msr    daif, %0\n"
        :
        : "r"(flags)
        : "memory"
    );
#endif
}

/*
 * Acquire spinlock with IRQs disabled.
 *
 * NOTE: On Jetson after kexec, the ARM exclusive monitor state is corrupted
 * in a way that LDAXR/STXR operations hang. We skip actual locking since we're
 * running single-core after kexec anyway. IRQ disabling still provides
 * protection against interrupt handlers.
 */
static inline irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    irq_flags_t flags = irq_save();
#if !defined(SPINLOCK_SKIP_LOCKING)
    spin_lock(lock);
#else
    (void)lock;  /* Suppress unused parameter warning */
#endif
    return flags;
}

/*
 * Release spinlock and restore IRQ state.
 */
static inline void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags)
{
#if !defined(SPINLOCK_SKIP_LOCKING)
    spin_unlock(lock);
#else
    (void)lock;  /* Suppress unused parameter warning */
#endif
    irq_restore(flags);
}

#endif /* SPINLOCK_H */
