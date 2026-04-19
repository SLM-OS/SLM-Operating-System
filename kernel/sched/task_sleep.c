/*
 * task_sleep.c — Scheduler-blocking sleep primitive (#319).
 *
 * Before this change, sleep_ms (and therefore slm.sleep in Lua) was a
 * busy-wait that yielded in a CNTPCT polling loop. The calling task
 * stayed on its run queue the entire time, stealing scheduling slots
 * from genuinely-ready work and preventing the idle task from wfi'ing
 * to save power.
 *
 * This implementation provides a proper task_sleep_ms that:
 *
 *   1. Marks the calling task TASK_BLOCKED and adds it to a global
 *      sleep queue keyed on a CNTPCT deadline.
 *   2. Calls schedule() — the scheduler skips TASK_BLOCKED tasks so
 *      another ready task (or the idle task) runs.
 *   3. On each scheduler_tick, task_wake_sleepers() scans the queue
 *      and moves any expired task back to TASK_READY + its assigned
 *      CPU's run queue.
 *
 * Sleep queue is a global singly-linked list using task->sleep_next.
 * Concurrency: sleep_queue_lock is a plain cacheable spinlock,
 * acquired briefly on add/remove/scan. Never held across
 * schedule() or allocation.
 *
 * Field reuse: task->wake_time_ns is used to store the CNTPCT
 * deadline as a raw cycle count. The field name's "_ns" suffix
 * predates this implementation; converting cycles ↔ nanoseconds on
 * every tick would be needlessly expensive. Comparison against
 * timer_get_count() stays in native CNTPCT units.
 */

#include "task.h"
#include "sched.h"
#include "spinlock.h"
#include "timer.h"
#include <stdint.h>

/* Global sleep queue. Singly-linked via task->sleep_next. Unsorted —
 * we walk the whole list each tick (cost is bounded by MAX_TASKS and
 * sleeps are rare in bare-metal workloads). */
static struct task *sleep_queue_head = NULL;
static spinlock_t sleep_queue_lock = SPINLOCK_INIT;

void task_sleep_init(void)
{
    sleep_queue_head = NULL;
}

/*
 * Enqueue task at the head of the sleep queue. Caller holds
 * sleep_queue_lock.
 */
static void sleep_queue_add_locked(struct task *task)
{
    task->sleep_next = sleep_queue_head;
    sleep_queue_head = task;
}

void task_sleep_ms(uint32_t ms)
{
    if (ms == 0) {
        return;
    }

    uint64_t freq = timer_get_frequency();
    if (freq == 0) {
        /* Fallback: timer not initialized yet — busy-wait loop at
         * early boot. Should not happen after scheduler_init. */
        return;
    }

    struct task *self = task_current();
    if (!self) {
        /* Called from non-task context (shouldn't happen). */
        return;
    }

    uint64_t deadline = timer_get_count() + (freq / 1000) * (uint64_t)ms;

    irq_flags_t flags = spin_lock_irqsave(&sleep_queue_lock);
    self->wake_time_ns = deadline;
    sleep_queue_add_locked(self);
    self->state = TASK_BLOCKED;
    spin_unlock_irqrestore(&sleep_queue_lock, flags);

    /* schedule() sees TASK_BLOCKED and picks a different runnable task
     * (or the idle task). We resume here after task_wake_sleepers
     * moves us back to TASK_READY and the scheduler re-dispatches us.
     *
     * Same race-window note as pi_mutex_lock: between the state flip
     * above (protected by the lock, then released) and schedule()
     * below, a synthetic tick could fire via coop_preempt_maybe_tick
     * — that would just run task_wake_sleepers, see our deadline
     * isn't met yet, and leave us alone. Safe. */
    schedule();
}

void task_wake_sleepers(void)
{
    uint64_t now = timer_get_count();

    /* Collect expired sleepers under the lock, then wake them outside
     * the lock. Waking requires scheduler_add_task which takes
     * rq_lock — don't nest locks. */
    struct task *to_wake = NULL;

    irq_flags_t flags = spin_lock_irqsave(&sleep_queue_lock);
    struct task **link = &sleep_queue_head;
    while (*link) {
        struct task *t = *link;
        if (t->wake_time_ns <= now) {
            /* Detach from sleep queue */
            *link = t->sleep_next;
            /* Reuse sleep_next to build a private wake list (the task
             * is no longer on the sleep queue, so sleep_next is free
             * to repurpose briefly). */
            t->sleep_next = to_wake;
            to_wake = t;
        } else {
            link = &t->sleep_next;
        }
    }
    spin_unlock_irqrestore(&sleep_queue_lock, flags);

    /* Wake each expired task. scheduler_add_task handles the run
     * queue re-entry; set state first so the scheduler accepts the
     * task. */
    while (to_wake) {
        struct task *t = to_wake;
        to_wake = t->sleep_next;
        t->sleep_next = NULL;
        t->wake_time_ns = 0;
        t->state = TASK_READY;
        scheduler_add_task(t);
    }
}
