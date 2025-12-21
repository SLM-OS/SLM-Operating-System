/*
 * sched.h - Scheduler for SLM-OS
 *
 * Simple round-robin scheduler with preemptive timer support.
 */

#ifndef SCHED_H
#define SCHED_H

#include "task.h"

/* Scheduler configuration */
#define TIMER_HZ            100         /* 100 Hz = 10ms time slice */
#define TIME_SLICE_MS       (1000 / TIMER_HZ)

/*
 * Initialize the scheduler.
 *
 * Sets up the idle task and prepares the run queue.
 * Must be called before any other scheduler functions.
 */
void scheduler_init(void);

/*
 * Add a task to the run queue.
 *
 * @task: Task to add (must be in READY state)
 */
void scheduler_add_task(struct task *task);

/*
 * Remove a task from the run queue.
 *
 * @task: Task to remove
 */
void scheduler_remove_task(struct task *task);

/*
 * Select next task and perform context switch.
 *
 * Called by:
 *   - yield() for voluntary switch
 *   - Timer interrupt handler for preemptive switch
 */
void schedule(void);

/*
 * Voluntarily yield the CPU to another task.
 *
 * Current task goes to back of run queue.
 */
void yield(void);

/*
 * Start the scheduler.
 *
 * Enables timer interrupts and begins executing tasks.
 * Does not return.
 */
void scheduler_start(void);

/*
 * Get scheduler statistics.
 */
struct sched_stats {
    uint32_t task_count;        /* Number of tasks in system */
    uint32_t ready_count;       /* Number of ready tasks */
    uint64_t context_switches;  /* Total context switches */
    uint64_t timer_ticks;       /* Total timer interrupts */
};

void scheduler_get_stats(struct sched_stats *stats);

/*
 * Print scheduler state to UART (for debugging).
 */
void scheduler_dump(void);

/*
 * Timer tick handler.
 *
 * Called from timer interrupt handler. Decrements time slice
 * and triggers preemption when it expires.
 */
void scheduler_tick(void);

/*
 * Context switch (implemented in assembly).
 *
 * Saves context of 'old' task, restores context of 'new' task.
 * Returns to new task's saved PC.
 *
 * @old: Task to save context from (may be NULL on first switch)
 * @new: Task to restore context to
 */
extern void switch_to(struct task *old, struct task *new);

#endif /* SCHED_H */
