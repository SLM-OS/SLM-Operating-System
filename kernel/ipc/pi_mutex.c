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
#include "cache.h"
#include "ncmem.h"              /* PLATFORM_HAS_NC_MEMORY */

/* Statistics */
static volatile uint32_t total_inversions = 0;
static volatile int last_inversion_flag = 0;

/* Serializes raises of task->pi_priority. A per-mutex guard isn't enough: two
 * waiters on two mutexes held by one task hold different guards, and a plain
 * check-then-set could lose the higher value. Not an atomic max, because
 * struct task is in NC memory on Pi 5 / Jetson, where exclusive operations
 * aren't safe; this lock lives in cacheable memory. The struct gives the lock a
 * whole cache line: the Pi 5 lock path cleans the line, and aligning the lock
 * alone would leave other variables in the rest of it. */
static struct {
    spinlock_t lock;
} __attribute__((aligned(64))) pi_floor = { SPINLOCK_INIT };

/*
 * Initialize a priority-inheriting mutex.
 */
void pi_mutex_init(pi_mutex_t *mutex)
{
    spin_init(&mutex->guard);
    mutex->owner = NULL;
    mutex->wait_head = NULL;
    mutex->wait_tail = NULL;
    mutex->locked = 0;
}

/* Record that `task` now holds one more PI mutex. Guard held. */
static void note_mutex_acquired(struct task *task)
{
    task->pi_mutexes_held++;
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->pi_mutexes_held);
#endif
}

/*
 * Raise `task`'s inherited priority to at least `priority`. Guard held.
 * Returns true if it changed.
 *
 * Stored apart from effective_priority, which every run-queue insertion
 * recomputes (#1103). It is set even when the task is currently at or above
 * `priority`: that level may only be temporary (an AI level, a deadline boost,
 * a priority someone lowers later), and the task must not fall below a waiter
 * when it ends.
 */
static bool raise_inherited_priority(struct task *task, uint8_t priority)
{
    bool raised = false;
    irq_flags_t flags = spin_lock_irqsave(&pi_floor.lock);

#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_invalidate(&task->pi_priority);
#endif
    if (priority > task->pi_priority) {
        task->pi_priority = priority;
#if !defined(PLATFORM_HAS_NC_MEMORY)
        cache_clean(&task->pi_priority);
#endif
        raised = true;
    }

    spin_unlock_irqrestore(&pi_floor.lock, flags);
    return raised;
}

/*
 * Make `caller`'s priority a floor for the owner, and boost the owner now if
 * caller outranks it. Must be called with guard held.
 *
 * Returns: true if the owner's floor or effective priority changed, so its
 * run-queue position needs refreshing. Also true when only the floor rose: a
 * re-queue in flight on another CPU may have read the old floor.
 */
static bool try_boost_owner(pi_mutex_t *mutex, struct task *caller)
{
    struct task *owner = mutex->owner;

    if (!owner || !caller) {
        return false;
    }

    bool changed = raise_inherited_priority(owner, caller->effective_priority);

    /* Check if caller has higher priority than owner's effective priority */
    if (caller->effective_priority > owner->effective_priority) {
        /* Priority inversion detected! Boost owner. */
        INFO("PI: Boosting task '%s' (pri %u->%u) for waiter '%s' (pri %u)",
             owner->name, owner->effective_priority, caller->effective_priority,
             caller->name, caller->effective_priority);

        owner->effective_priority = caller->effective_priority;
#if !defined(PLATFORM_HAS_NC_MEMORY)
        cache_clean(&owner->effective_priority);
#endif
        total_inversions++;
        last_inversion_flag = 1;
        changed = true;
    }

    return changed;
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
        note_mutex_acquired(self);
        spin_unlock_irqrestore(&mutex->guard, flags);
        return;
    }

    /* Mutex is held — boost owner and block. A holder waiting in a run queue
     * is still at its old position, so move it, or everything queued ahead of
     * that position runs first. This is the one place the guard nests around
     * an rq_lock (never the reverse: nothing under rq_lock takes a pi_mutex).
     * Holding the guard keeps the owner from releasing the mutex meanwhile, and
     * no hand-off can see this task until the guard is dropped below, so this
     * adds nothing to the time between it becoming visible and switching away. */
    if (try_boost_owner(mutex, self)) {
        sched_reposition_task(mutex->owner);
    }

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
     * re-dispatches us. At that point the mutex is ours.
     *
     * A timer tick can fire between the state flip above and this call (IRQs
     * are enabled again). That is safe: schedule() re-queues only a
     * TASK_RUNNING task, so it just switches away from us without queueing
     * us, and we continue from here once woken. */
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
        note_mutex_acquired(self);

        spin_unlock_irqrestore(&mutex->guard, flags);
        return 1;
    }

    spin_unlock_irqrestore(&mutex->guard, flags);
    return 0;
}

bool pi_mutex_held_by_self(pi_mutex_t *mutex)
{
    struct task *self = task_current();
    irq_flags_t flags = spin_lock_irqsave(&mutex->guard);
    bool held = (mutex->locked && mutex->owner == self);
    spin_unlock_irqrestore(&mutex->guard, flags);
    return held;
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
        if (owner->pi_mutexes_held > 0) {
            owner->pi_mutexes_held--;
        }
        /* Drop the inherited priority with the last mutex held: what an
         * earlier-released mutex contributed can't be told apart from what
         * one still held does, and over-boosting is the safe side. */
        if (owner->pi_mutexes_held == 0 && owner->pi_priority != 0) {
            uint8_t boosted = owner->effective_priority;
            owner->pi_priority = 0;
            sched_refresh_effective_priority(owner);
            if (owner->effective_priority != boosted) {
                INFO("PI: Restoring task '%s' priority %u->%u",
                     owner->name, boosted, owner->effective_priority);
            }
        }
#if !defined(PLATFORM_HAS_NC_MEMORY)
        cache_clean(&owner->pi_mutexes_held);
        cache_clean(&owner->pi_priority);
#endif
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
        note_mutex_acquired(best);
        /* mutex->locked stays 1. `best` outranks or matches the waiters left
         * in the queue, but only for now: they set its floor. */
        for (struct task *t = mutex->wait_head; t; t = t->next) {
            raise_inherited_priority(best, t->effective_priority);
        }

        best->state = TASK_READY;

        spin_unlock_irqrestore(&mutex->guard, flags);

        /* Re-add the woken task to the run queue so the scheduler can
         * dispatch it. scheduler_add_task_to_cpu handles NC memory
         * visibility and cross-CPU notification. */
        scheduler_add_task_to_cpu(best, best->assigned_cpu);
    } else {
        /* No waiters — just release. */
        mutex->owner = NULL;
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
