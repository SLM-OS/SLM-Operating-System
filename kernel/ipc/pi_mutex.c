/*
 * pi_mutex.c - Priority-Inheriting Mutex Implementation
 *
 * Implements priority inheritance to prevent unbounded priority inversion.
 */

#include "pi_mutex.h"
#include "task.h"
#include "debug.h"
#include "smp.h"

/* Statistics */
static volatile uint32_t total_inversions = 0;
static volatile int last_inversion_flag = 0;

/*
 * Initialize a priority-inheriting mutex.
 */
void pi_mutex_init(pi_mutex_t *mutex)
{
    spin_init(&mutex->guard);
    mutex->owner = NULL;
    mutex->owner_original_pri = 0;
    mutex->locked = 0;
}

/*
 * Boost owner's priority if caller has higher priority.
 * Must be called with guard held.
 *
 * Returns: 1 if priority was boosted (inversion detected), 0 otherwise.
 */
static int try_boost_owner(pi_mutex_t *mutex, struct task *caller)
{
    struct task *owner = mutex->owner;

    if (!owner || !caller) {
        return 0;
    }

    /* Check if caller has higher priority than owner's effective priority */
    if (caller->effective_priority > owner->effective_priority) {
        /* Priority inversion detected! Boost owner. */
        INFO("PI: Boosting task '%s' (pri %u->%u) for waiter '%s' (pri %u)",
             owner->name, owner->effective_priority, caller->effective_priority,
             caller->name, caller->effective_priority);

        owner->effective_priority = caller->effective_priority;
        total_inversions++;
        last_inversion_flag = 1;
        return 1;
    }

    return 0;
}

/*
 * Acquire a priority-inheriting mutex.
 */
void pi_mutex_lock(pi_mutex_t *mutex)
{
    struct task *self = task_current();

    last_inversion_flag = 0;

    while (1) {
        /* Acquire guard to check/modify mutex state */
        irq_flags_t flags = spin_lock_irqsave(&mutex->guard);

        if (!mutex->locked) {
            /* Mutex is free, acquire it */
            mutex->locked = 1;
            mutex->owner = self;
            mutex->owner_original_pri = self->effective_priority;

            spin_unlock_irqrestore(&mutex->guard, flags);
            return;
        }

        /* Mutex is held by someone else */
        /* Try to boost owner's priority if we have higher priority */
        try_boost_owner(mutex, self);

        spin_unlock_irqrestore(&mutex->guard, flags);

        /*
         * Spin-wait with IRQs enabled.
         * This allows the scheduler to preempt us and run
         * the (now potentially boosted) owner.
         */
        while (mutex->locked) {
            /* Yield to let the owner run */
#if defined(PLATFORM_X86_64)
            __asm__ volatile("pause" ::: "memory");
#else
            __asm__ volatile("yield" ::: "memory");
#endif
        }
    }
}

/*
 * Try to acquire without blocking.
 */
int pi_mutex_trylock(pi_mutex_t *mutex)
{
    struct task *self = task_current();

    irq_flags_t flags = spin_lock_irqsave(&mutex->guard);

    if (!mutex->locked) {
        mutex->locked = 1;
        mutex->owner = self;
        mutex->owner_original_pri = self->effective_priority;

        spin_unlock_irqrestore(&mutex->guard, flags);
        return 1;
    }

    spin_unlock_irqrestore(&mutex->guard, flags);
    return 0;
}

/*
 * Release the mutex and restore priority.
 */
void pi_mutex_unlock(pi_mutex_t *mutex)
{
    irq_flags_t flags = spin_lock_irqsave(&mutex->guard);

    struct task *owner = mutex->owner;

    if (owner) {
        /*
         * Restore owner's priority to original value.
         *
         * Note: In a more sophisticated implementation, if the task
         * holds multiple pi_mutexes, we'd need to set effective_priority
         * to the max of original priority and all other mutex waiters.
         * For simplicity, we just restore to original.
         */
        if (owner->effective_priority != mutex->owner_original_pri) {
            INFO("PI: Restoring task '%s' priority %u->%u",
                 owner->name, owner->effective_priority, mutex->owner_original_pri);
        }
        owner->effective_priority = mutex->owner_original_pri;
    }

    mutex->owner = NULL;
    mutex->owner_original_pri = 0;
    mutex->locked = 0;

    spin_unlock_irqrestore(&mutex->guard, flags);

    /* Wake any waiters */
#if !defined(PLATFORM_X86_64)
    __asm__ volatile("sev" ::: "memory");
#endif
}

/*
 * Check if last lock operation detected priority inversion.
 */
int pi_mutex_inversion_detected(void)
{
    return last_inversion_flag;
}

/*
 * Get total priority inversion count.
 */
uint32_t pi_mutex_inversion_count(void)
{
    return total_inversions;
}
