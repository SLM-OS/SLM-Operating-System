/*
 * sched.h - Scheduler for SLM-OS
 *
 * Per-core round-robin scheduler with preemptive timer support.
 * Uses a global lock for simplicity (sufficient for 4-6 cores).
 */

#ifndef SCHED_H
#define SCHED_H

#include "task.h"
#include "smp.h"

/* Scheduler configuration */
#define TIMER_HZ            100         /* 100 Hz = 10ms time slice */
#define TIME_SLICE_MS       (1000 / TIMER_HZ)

/*
 * Initialize the scheduler (boot CPU).
 *
 * Sets up the idle task and prepares the run queue.
 * Must be called before any other scheduler functions.
 */
void scheduler_init(void);

/*
 * Initialize scheduler for a secondary CPU.
 *
 * Creates the idle task for this CPU. Called by secondary CPUs
 * during their initialization before calling scheduler_start().
 *
 * @cpu: The logical CPU ID of the calling CPU
 */
void scheduler_init_secondary(uint32_t cpu);

/*
 * Check if the scheduler has been initialized.
 *
 * Used by secondary CPUs to wait for CPU 0 to initialize the scheduler
 * before they attempt to start their own scheduling.
 *
 * Returns: true if scheduler_init() has completed, false otherwise
 */
int scheduler_is_initialized(void);

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
    uint32_t ready_count;       /* Number of ready tasks (all CPUs) */
    uint64_t context_switches;  /* Total context switches */
    uint64_t timer_ticks;       /* Total timer interrupts */
};

void scheduler_get_stats(struct sched_stats *stats);

/*
 * Migrate a task to a different CPU.
 *
 * @task:       Task to migrate (must not be running)
 * @target_cpu: Destination CPU ID
 *
 * Returns: 0 on success, -1 on error
 */
int sched_migrate_task(struct task *task, uint32_t target_cpu);

/*
 * Add a task to a specific CPU's run queue.
 *
 * @task: Task to add (must be in READY state)
 * @cpu:  Target CPU ID
 */
void scheduler_add_task_to_cpu(struct task *task, uint32_t cpu);

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
