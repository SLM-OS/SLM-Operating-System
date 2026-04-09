/*
 * sched.h - Scheduler for SLM-OS
 *
 * Per-core priority scheduler with preemptive timer support.
 * Each CPU has its own run queue with a per-queue lock to reduce contention.
 * Cross-queue operations (migration) lock both queues in CPU ID order.
 */

#ifndef SCHED_H
#define SCHED_H

#include "task.h"
#include "smp.h"
#include "config.h"

/*
 * Per-CPU run queue.
 *
 * On Pi 5, the data fields live in NC memory for cross-CPU visibility,
 * but the lock stays in cacheable memory (ldaxr/stxr require cacheable).
 *
 * Exposed in the header so scheduling policies can inspect queue state
 * (e.g., ready_count, head) for CPU assignment decisions. Policies must
 * NOT modify run queue fields — use scheduler_add_task_to_cpu() etc.
 */
struct cpu_runqueue {
    struct task *head;
    struct task *tail;
    struct task *idle_task;
    struct task *zombie;
    uint32_t ready_count;
#ifdef CONFIG_AI_SCHEDULER
    uint64_t running_ticks;     /* Ticks where current task is not idle */
    uint64_t total_ticks;       /* Total ticks since boot on this CPU */
#endif
} __attribute__((aligned(64)));  /* CACHE_LINE_SIZE */

/*
 * Get the run queue for a CPU. Used by scheduling policies to inspect
 * queue state. Policies must not modify the returned struct.
 */
struct cpu_runqueue *sched_cpu_rq(uint32_t cpu);

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
 * Start the scheduler on the specified CPU.
 *
 * Caller passes the logical CPU ID to avoid stale cpu_id() reads on
 * secondary CPUs (L2 cache incoherency on Pi 5). Enables timer,
 * unmasks interrupts, and switches to the first task. Does not return.
 */
void scheduler_start(uint32_t cpu);

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

/*
 * Core isolation for real-time / latency-sensitive workloads.
 *
 * Isolated cores only run tasks that are explicitly pinned to them
 * via cpu_affinity. Tasks with CPU_AFFINITY_ANY skip isolated cores.
 */

/*
 * Isolate a core from general scheduling.
 *
 * @cpu: CPU ID to isolate (cannot be CPU 0)
 *
 * Returns: 0 on success, -1 if invalid or CPU 0
 */
int sched_isolate_core(uint32_t cpu);

/*
 * Remove core isolation, returning it to general scheduling.
 *
 * @cpu: CPU ID to un-isolate
 *
 * Returns: 0 on success, -1 if invalid CPU
 */
int sched_unisolate_core(uint32_t cpu);

/*
 * Check if a core is isolated.
 *
 * @cpu: CPU ID to check
 *
 * Returns: 1 if isolated, 0 if not (or invalid CPU)
 */
int sched_is_core_isolated(uint32_t cpu);

/*
 * Get the bitmask of isolated cores.
 *
 * Returns: Bitmask where bit N is set if CPU N is isolated
 */
uint32_t sched_get_isolated_cores(void);

/*
 * Set task CPU affinity.
 *
 * @task: Task to modify
 * @cpu:  CPU ID to pin to, or CPU_AFFINITY_ANY for any non-isolated core
 *
 * Returns: 0 on success, -1 if invalid CPU
 */
int sched_set_task_affinity(struct task *task, uint32_t cpu);

#endif /* SCHED_H */
