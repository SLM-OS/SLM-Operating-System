/*
 * spinlock.h - Spinlocks and synchronization primitives for SLM-OS
 *
 * Provides spinlocks and ticket locks for multi-core synchronization.
 * Uses ARM64 exclusive load/store instructions for atomic operations.
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include "platform.h"   /* For PLATFORM_JETSON_ORIN_NANO */

/*
 * Memory barrier macros for ARM64.
 *
 * DMB (Data Memory Barrier) - ensures memory accesses complete
 * DSB (Data Synchronization Barrier) - ensures memory + cache ops complete
 * ISB (Instruction Synchronization Barrier) - flushes pipeline
 *
 * Barrier options:
 *   ish   - Inner Shareable (all cores in the system)
 *   ishld - Inner Shareable, load operations
 *   ishst - Inner Shareable, store operations
 */
#define dmb(opt)    __asm__ volatile("dmb " #opt ::: "memory")
#define dsb(opt)    __asm__ volatile("dsb " #opt ::: "memory")
#define isb()       __asm__ volatile("isb" ::: "memory")

/* Full memory barriers */
#define smp_mb()    dmb(ish)      /* Full barrier */
#define smp_rmb()   dmb(ishld)    /* Read barrier */
#define smp_wmb()   dmb(ishst)    /* Write barrier */

/* Compiler barrier (prevents reordering, no CPU barrier) */
#define barrier()   __asm__ volatile("" ::: "memory")

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
 */
static inline void spin_lock(spinlock_t *lock)
{
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Jetson: skip locking, just barrier for memory ordering */
    (void)lock;
    dmb(ish);
#else
    uint32_t tmp;

    /* Other platforms: use WFE for low-power spinning */
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
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Jetson: always succeed, we're single-core after kexec */
    (void)lock;
    dmb(ish);
    return 1;
#else
    uint32_t tmp, result;

    __asm__ volatile(
        "   ldaxr   %w0, [%2]\n"        /* Load-acquire exclusive */
        "   cbnz    %w0, 1f\n"          /* If locked, fail */
        "   stxr    %w0, %w3, [%2]\n"   /* Try to store 1 (locked) */
        "   cbnz    %w0, 1f\n"          /* If store failed, fail */
        "   mov     %w1, #1\n"          /* Success */
        "   b       2f\n"
        "1: mov     %w1, #0\n"          /* Failure */
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
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Jetson: skip unlocking, just barrier for memory ordering */
    (void)lock;
    dmb(ish);
#else
    __asm__ volatile(
        "   stlr    wzr, [%0]\n"        /* Store-release 0 (unlocked) */
        "   sev\n"                      /* Send event to wake waiters */
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

    /* Wait until our ticket is called */
    while (lock->owner != ticket) {
        __asm__ volatile("wfe" ::: "memory");
    }

    /* Acquire barrier */
    dmb(ish);
}

/*
 * Release a ticket lock.
 */
static inline void ticket_unlock(ticket_lock_t *lock)
{
    uint16_t next_owner = lock->owner + 1;

    /* Release barrier then update owner */
    dmb(ish);
    lock->owner = next_owner;

    /* Wake waiters */
    __asm__ volatile("sev" ::: "memory");
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
    __asm__ volatile(
        "mrs    %0, daif\n"
        "msr    daifset, #2\n"      /* Set IRQ mask bit */
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

/*
 * Restore IRQ state.
 */
static inline void irq_restore(irq_flags_t flags)
{
    __asm__ volatile(
        "msr    daif, %0\n"
        :
        : "r"(flags)
        : "memory"
    );
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
#if !defined(PLATFORM_JETSON_ORIN_NANO)
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
#if !defined(PLATFORM_JETSON_ORIN_NANO)
    spin_unlock(lock);
#else
    (void)lock;  /* Suppress unused parameter warning */
#endif
    irq_restore(flags);
}

#endif /* SPINLOCK_H */
