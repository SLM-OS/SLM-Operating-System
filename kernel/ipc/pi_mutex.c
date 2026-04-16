/*
 * pi_mutex.c - Priority-Inheriting Mutex Implementation
 *
 * Implements priority inheritance to prevent unbounded priority inversion.
 */

#include "pi_mutex.h"
#include "task.h"
#include "sched.h"
#include "spinlock.h"
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
    mutex->wait_head = NULL;
    mutex->wait_tail = NULL;
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
 *
 * If the mutex is held, the caller is appended to a FIFO wait queue
 * and marked TASK_BLOCKED. The scheduler removes it from the run
 * queue; pi_mutex_unlock wakes the highest-priority waiter. This
 * replaces the previous spin-wait loop (#93).
 */
void pi_mutex_lock(pi_mutex_t *mutex)
{
    struct task *self = task_current();

    last_inversion_flag = 0;

    irq_flags_t flags = spin_lock_irqsave(&mutex->guard);

    if (!mutex->locked) {
        /* Fast path: mutex is free. */
        mutex->locked = 1;
        mutex->owner = self;
        mutex->owner_original_pri = self->effective_priority;
        spin_unlock_irqrestore(&mutex->guard, flags);
        return;
    }

    /* Mutex is held — boost owner and block. */
    try_boost_owner(mutex, self);

    /* Append to wait queue (FIFO). task->next is free because the
     * task is about to leave the run queue (TASK_BLOCKED removes it
     * from the scheduler's linked list). */
    self->next = NULL;
    if (mutex->wait_tail) {
        mutex->wait_tail->next = self;
    } else {
        mutex->wait_head = self;
    }
    mutex->wait_tail = self;

    self->state = TASK_BLOCKED;

    spin_unlock_irqrestore(&mutex->guard, flags);

    /* Deschedule — schedule() sees TASK_BLOCKED and won't pick us.
     * We resume here after pi_mutex_unlock wakes us and the scheduler
     * re-dispatches us. At that point the mutex is ours. */
    schedule();
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
 *
 * If there are blocked waiters, the highest-priority one is woken
 * and directly given ownership of the mutex (hand-off) so it doesn't
 * need to re-acquire. This avoids a thundering-herd wake.
 */
void pi_mutex_unlock(pi_mutex_t *mutex)
{
    irq_flags_t flags = spin_lock_irqsave(&mutex->guard);

    struct task *owner = mutex->owner;

    if (owner) {
        if (owner->effective_priority != mutex->owner_original_pri) {
            INFO("PI: Restoring task '%s' priority %u->%u",
                 owner->name, owner->effective_priority, mutex->owner_original_pri);
        }
        owner->effective_priority = mutex->owner_original_pri;
    }

    /* Pick the highest-priority waiter from the wait queue. Walk the
     * FIFO and select the one with the largest effective_priority;
     * unlink it. O(n) in waiter count, acceptable for the small queues
     * expected in SLM-OS. */
    struct task *best = NULL;
    struct task **best_prev_next = NULL;
    {
        struct task **pp = &mutex->wait_head;
        struct task *cur = mutex->wait_head;
        while (cur) {
            if (!best || cur->effective_priority > best->effective_priority) {
                best = cur;
                best_prev_next = pp;
            }
            pp = &cur->next;
            cur = cur->next;
        }
    }

    if (best) {
        /* Unlink `best` from the wait queue. */
        *best_prev_next = best->next;
        if (mutex->wait_tail == best) {
            /* Recalculate tail: walk from head (short list). */
            mutex->wait_tail = NULL;
            struct task *t = mutex->wait_head;
            while (t) {
                mutex->wait_tail = t;
                t = t->next;
            }
        }
        best->next = NULL;

        /* Hand off ownership directly — the woken task resumes inside
         * pi_mutex_lock after its schedule() call and finds the mutex
         * already acquired on its behalf. */
        mutex->owner = best;
        mutex->owner_original_pri = best->effective_priority;
        /* mutex->locked stays 1. */

        best->state = TASK_READY;

        spin_unlock_irqrestore(&mutex->guard, flags);

        /* Re-add the woken task to the run queue so the scheduler can
         * dispatch it. scheduler_add_task_to_cpu handles NC memory
         * visibility and cross-CPU notification. */
        scheduler_add_task_to_cpu(best, best->assigned_cpu);
    } else {
        /* No waiters — just release. */
        mutex->owner = NULL;
        mutex->owner_original_pri = 0;
        mutex->locked = 0;

        spin_unlock_irqrestore(&mutex->guard, flags);
    }
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
