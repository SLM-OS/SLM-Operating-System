/*
 * sched.c - Per-core priority scheduler for SLM-OS
 *
 * Each CPU has its own run queue ordered by effective priority (highest first).
 * Within the same priority, tasks are scheduled FIFO.
 *
 * Each CPU has its own run queue lock, reducing contention for multi-core
 * scheduling. Global statistics use atomic operations or accept benign races.
 */

#include "sched.h"
#include "task.h"
#include "uart.h"
#include "debug.h"
#include "platform.h"
#include "smp.h"
#include "spinlock.h"
#include "slm_ffi.h"
#include "gic.h"
#include "timer.h"
#include "cache.h"
#include <stdint.h>

/* Deadline boost thresholds (in nanoseconds) */
#define DEADLINE_CRITICAL_NS    (10 * 1000000ULL)   /* 10ms - boost to CRITICAL */
#define DEADLINE_HIGH_NS        (50 * 1000000ULL)   /* 50ms - boost to HIGH */
#define DEADLINE_BOOST_NS       (100 * 1000000ULL)  /* 100ms - boost +1 */

/* External functions from task.c */
extern void task_set_current(struct task *task);
extern void task_destroy(struct task *task);

/* Per-CPU run queue */
struct cpu_runqueue {
    spinlock_t lock;
    struct task *head;
    struct task *tail;
    struct task *idle_task;
    struct task *zombie;
    uint32_t ready_count;
} __attribute__((aligned(CACHE_LINE_SIZE)));

/*
 * Global scheduler state.
 *
 * Lock strategy: Each CPU run queue has its own lock for local operations.
 * Cross-queue operations (migration) lock both queues in CPU ID order
 * to prevent deadlock. Global statistics are racy but acceptable.
 */
static struct {
    struct cpu_runqueue cpu[MAX_CPUS];  /* Per-CPU run queues (each has own lock) */
    uint32_t task_count;                 /* Total tasks (racy but OK for stats) */
    uint64_t context_switches;           /* Total switches (racy but OK) */
    uint64_t timer_ticks;                /* Timer interrupts (racy but OK) */
    uint32_t isolated_cores;             /* Bitmask of isolated cores */
    int initialized;                     /* Scheduler initialized flag */
} sched;

/*
 * Idle task - runs when no other tasks are ready.
 * Each CPU has its own idle task.
 */
static void idle_task_func(void *arg)
{
    (void)arg;

    while (1) {
        /* Unmask IRQ so timer interrupts can fire.
         * This must be inside the loop because context switch saves/restores
         * DAIF. When idle is preempted by the timer ISR, the saved DAIF has
         * IRQ masked (hardware masks IRQ on exception entry). On resume,
         * the restored DAIF keeps IRQ masked — so we must re-clear it here. */
        __asm__ volatile("msr daifclr, #2" ::: "memory");

        /* Wait for interrupt (timer will wake us) */
        __asm__ volatile("wfi");

        /* Yield to check if other tasks are ready */
        yield();
    }
}

/*
 * Update a task's effective priority based on deadline proximity.
 *
 * Called when adding a task to the run queue and during scheduler tick.
 * The effective_priority determines scheduling order.
 */
static void update_deadline_boost(struct task *task)
{
    if (!task) {
        return;
    }

    /* Deadline may have been set by another CPU */
    cache_invalidate(&task->deadline_ns);

    if (task->deadline_ns == 0) {
        /* No deadline - effective priority equals base priority */
        task->effective_priority = task->priority;
        cache_clean(&task->effective_priority);
        return;
    }

    uint64_t now = slm_get_time_ns();
    uint8_t base = task->priority;
    uint8_t boosted = base;

    if (now >= task->deadline_ns) {
        /* Deadline missed! Boost to critical to finish ASAP */
        boosted = TASK_PRIORITY_CRITICAL;
    } else {
        uint64_t remaining = task->deadline_ns - now;

        if (remaining < DEADLINE_CRITICAL_NS) {
            /* < 10ms: boost to CRITICAL (7) */
            boosted = TASK_PRIORITY_CRITICAL;
        } else if (remaining < DEADLINE_HIGH_NS) {
            /* < 50ms: boost to HIGH (6) */
            boosted = TASK_PRIORITY_HIGH;
        } else if (remaining < DEADLINE_BOOST_NS) {
            /* < 100ms: boost +1 (capped at max) */
            boosted = (base < TASK_PRIORITY_MAX) ? base + 1 : TASK_PRIORITY_MAX;
        }
        /* else: > 100ms remaining, no boost */
    }

    task->effective_priority = boosted;
    cache_clean(&task->effective_priority);
}

/*
 * Initialize the scheduler (called on boot CPU).
 */
void scheduler_init(void)
{
    /* Initialize global state */
    sched.task_count = 0;
    sched.context_switches = 0;
    sched.timer_ticks = 0;
    sched.isolated_cores = 0;

    /* Initialize per-CPU run queues with per-queue locks */
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        spin_init(&sched.cpu[i].lock);
        sched.cpu[i].head = NULL;
        sched.cpu[i].tail = NULL;
        sched.cpu[i].idle_task = NULL;
        sched.cpu[i].zombie = NULL;
        sched.cpu[i].ready_count = 0;
    }

    /* Create idle task for boot CPU (CPU 0) */
    sched.cpu[0].idle_task = task_create("idle", idle_task_func, NULL);
    if (!sched.cpu[0].idle_task) {
        panic("scheduler_init: failed to create idle task");
    }
    sched.cpu[0].idle_task->state = TASK_READY;
    sched.cpu[0].idle_task->cpu_affinity = 0;  /* Pinned to CPU 0 */
    sched.cpu[0].idle_task->assigned_cpu = 0;

    sched.initialized = 1;
    cache_clean(&sched.initialized);

    /* Wake any secondary CPUs waiting for scheduler init */
    __asm__ volatile("sev" ::: "memory");

    INFO("Scheduler initialized");
}

/*
 * Check if the scheduler has been initialized.
 */
int scheduler_is_initialized(void)
{
    cache_invalidate(&sched.initialized);
    return sched.initialized;
}

/*
 * Initialize scheduler for a secondary CPU.
 * Creates the idle task for this CPU.
 */
void scheduler_init_secondary(uint32_t cpu)
{
    if (cpu == 0 || cpu >= MAX_CPUS) {
        return;  /* CPU 0 uses scheduler_init(), invalid CPUs ignored */
    }

    /* Create idle task for this CPU */
    char idle_name[TASK_NAME_LEN];
    /* Simple integer to string for idle task name */
    idle_name[0] = 'i';
    idle_name[1] = 'd';
    idle_name[2] = 'l';
    idle_name[3] = 'e';
    idle_name[4] = '_';
    idle_name[5] = '0' + cpu;
    idle_name[6] = '\0';

    /* Create task outside of lock (task_create may allocate memory) */
    struct task *idle = task_create(idle_name, idle_task_func, NULL);
    if (!idle) {
        panic("scheduler_init_secondary: failed to create idle task for CPU %u", cpu);
    }

    /* Lock this CPU's queue to update idle task */
    irq_flags_t flags = spin_lock_irqsave(&sched.cpu[cpu].lock);

    idle->state = TASK_READY;
    idle->cpu_affinity = cpu;  /* Pinned to this CPU */
    idle->assigned_cpu = cpu;
    sched.cpu[cpu].idle_task = idle;

    spin_unlock_irqrestore(&sched.cpu[cpu].lock, flags);

    INFO("CPU %u: scheduler initialized", cpu);
}

/*
 * Add a task to a specific CPU's run queue (internal, requires lock held).
 *
 * Tasks are inserted in priority order (highest effective_priority first).
 * Within the same priority, new tasks go after existing ones (FIFO).
 */
static void add_to_cpu_queue_locked(struct task *task, uint32_t cpu)
{
    struct cpu_runqueue *rq = &sched.cpu[cpu];

    task->assigned_cpu = cpu;

    /* Update effective priority based on deadline before insertion */
    update_deadline_boost(task);

    /* Empty queue - just add */
    if (!rq->head) {
        task->next = NULL;
        rq->head = task;
        rq->tail = task;
        rq->ready_count++;
        return;
    }

    /* Find insertion point: insert before first task with LOWER priority */
    struct task *prev = NULL;
    struct task *curr = rq->head;

    while (curr && curr->effective_priority >= task->effective_priority) {
        prev = curr;
        curr = curr->next;
    }

    /* Insert task between prev and curr */
    task->next = curr;

    if (prev) {
        prev->next = task;
    } else {
        /* New head (highest priority) */
        rq->head = task;
    }

    /* Update tail if inserting at end */
    if (!curr) {
        rq->tail = task;
    }

    rq->ready_count++;
}

/*
 * Remove a task from its CPU's run queue (internal, requires lock held).
 * Returns 1 if task was found and removed, 0 if not found.
 */
static int remove_from_cpu_queue_locked(struct task *task, uint32_t cpu)
{
    struct cpu_runqueue *rq = &sched.cpu[cpu];
    struct task *prev = NULL;
    struct task *curr = rq->head;

    while (curr) {
        if (curr == task) {
            if (prev) {
                prev->next = curr->next;
            } else {
                rq->head = curr->next;
            }

            if (curr == rq->tail) {
                rq->tail = prev;
            }

            task->next = NULL;
            rq->ready_count--;
            return 1;  /* Found and removed */
        }
        prev = curr;
        curr = curr->next;
    }
    return 0;  /* Not found */
}

/*
 * Add a task to a specific CPU's run queue.
 */
void scheduler_add_task_to_cpu(struct task *task, uint32_t cpu)
{
    if (!task || task->state != TASK_READY || cpu >= cpu_count) {
        return;
    }

    struct cpu_runqueue *rq = &sched.cpu[cpu];
    irq_flags_t flags = spin_lock_irqsave(&rq->lock);

    add_to_cpu_queue_locked(task, cpu);
    sched.task_count++;  /* Racy but acceptable for stats */

    DEBUG_PRINT("Added task '%s' to CPU %u run queue (ready=%u)",
                task->name, cpu, rq->ready_count);

    /* Clean specific fields written inside the lock to PoC.
     * Without SMPEN, these stay in our L1 cache. The target CPU's
     * schedule() invalidates before reading. Only clean the fields
     * that were modified — NOT the entire task struct (which includes
     * the task's context/stack that may be in active use). */
    cache_clean(&rq->head);
    cache_clean(&rq->tail);
    cache_clean(&rq->ready_count);
    cache_clean(&task->next);
    cache_clean(&task->assigned_cpu);
    cache_clean(&task->state);
    cache_clean(&task->effective_priority);

    spin_unlock_irqrestore(&rq->lock, flags);
}

/*
 * Calculate deadline pressure for a CPU's run queue.
 *
 * Returns a score based on the sum of urgency of deadline-constrained tasks.
 * Higher score = more deadline pressure = avoid placing more work here.
 *
 * Urgency scoring:
 *   - No deadline: 0 points
 *   - Deadline > 100ms: 1 point
 *   - Deadline 50-100ms: 2 points
 *   - Deadline 10-50ms: 4 points
 *   - Deadline < 10ms or missed: 8 points
 */
static uint32_t calculate_deadline_pressure(uint32_t cpu)
{
    struct cpu_runqueue *rq = &sched.cpu[cpu];
    uint32_t pressure = 0;
    uint64_t now = slm_get_time_ns();

    struct task *t = rq->head;
    while (t) {
        if (t->deadline_ns > 0) {
            if (now >= t->deadline_ns) {
                /* Deadline missed - very high pressure */
                pressure += 8;
            } else {
                uint64_t remaining = t->deadline_ns - now;
                if (remaining < DEADLINE_CRITICAL_NS) {
                    pressure += 8;  /* < 10ms */
                } else if (remaining < DEADLINE_HIGH_NS) {
                    pressure += 4;  /* < 50ms */
                } else if (remaining < DEADLINE_BOOST_NS) {
                    pressure += 2;  /* < 100ms */
                } else {
                    pressure += 1;  /* distant deadline */
                }
            }
        }
        t = t->next;
    }

    return pressure;
}

/*
 * Find a non-isolated CPU for a task with CPU_AFFINITY_ANY.
 *
 * Uses a combined metric: ready_count + deadline_pressure.
 * This spreads deadline-constrained tasks across cores to reduce
 * the chance of missing deadlines due to queue contention.
 *
 * Returns CPU 0 as fallback (CPU 0 cannot be isolated).
 */
static uint32_t find_target_cpu(void)
{
    uint32_t best_cpu = 0;
    uint32_t best_score = sched.cpu[0].ready_count + calculate_deadline_pressure(0);

    for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
        /* Skip isolated cores */
        if (sched.isolated_cores & (1U << cpu)) {
            continue;
        }

        /* Combined score: ready count + deadline pressure */
        uint32_t score = sched.cpu[cpu].ready_count + calculate_deadline_pressure(cpu);

        /* Pick CPU with lowest combined score */
        if (score < best_score) {
            best_cpu = cpu;
            best_score = score;
        }
    }

    return best_cpu;
}

/*
 * Find a "performance" core for deadline-critical tasks.
 *
 * In a big.LITTLE system, this would return a big core.
 * In QEMU virt (homogeneous), we use CPU 1+ as "performance" cores
 * to keep CPU 0 available for system tasks.
 *
 * Returns the least-loaded non-isolated CPU > 0, or falls back to find_target_cpu().
 */
__attribute__((unused))
static uint32_t find_performance_cpu(void)
{
    if (cpu_count <= 1) {
        return 0;  /* Only one CPU available */
    }

    uint32_t best_cpu = 0;
    uint32_t best_score = UINT32_MAX;
    int found_perf_core = 0;

    /* Prefer CPUs > 0 for performance tasks */
    for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
        /* Skip isolated cores - they're manually managed */
        if (sched.isolated_cores & (1U << cpu)) {
            continue;
        }

        uint32_t score = sched.cpu[cpu].ready_count + calculate_deadline_pressure(cpu);
        if (score < best_score) {
            best_cpu = cpu;
            best_score = score;
            found_perf_core = 1;
        }
    }

    /* Fall back to any CPU if no performance core available */
    if (!found_perf_core) {
        return find_target_cpu();
    }

    return best_cpu;
}

/*
 * Add a task to the run queue (assigns to a CPU based on affinity).
 *
 * Policy for deadline-constrained tasks:
 *   - Tasks with deadline_ns > 0 are placed on "performance" cores (CPU > 0)
 *   - This keeps CPU 0 available for system tasks and reduces interference
 *   - On big.LITTLE hardware, this would route to big cores
 */
void scheduler_add_task(struct task *task)
{
    if (!task || task->state != TASK_READY) {
        return;
    }

    uint32_t target_cpu;

    /* Affinity may have been set by another CPU */
    cache_invalidate(&task->cpu_affinity);

    if (task->cpu_affinity != CPU_AFFINITY_ANY) {
        target_cpu = task->cpu_affinity;
#if defined(PLATFORM_RASPI5)
    } else {
        /* Pi 5: cross-CPU task dispatch requires SMPEN for L2 cache
         * coherency, which TF-A doesn't set. DC CIVAC does not
         * propagate through per-core L2 caches without SMPEN.
         * All tasks without explicit affinity run on CPU 0. */
        target_cpu = 0;
#else
    } else if (task->deadline_ns > 0) {
        target_cpu = find_performance_cpu();
        DEBUG_PRINT("Deadline task '%s' -> CPU %u (performance core)",
                    task->name, target_cpu);
    } else {
        target_cpu = find_target_cpu();
#endif
    }

    scheduler_add_task_to_cpu(task, target_cpu);
}

/*
 * Remove a task from the run queue.
 */
void scheduler_remove_task(struct task *task)
{
    if (!task) {
        return;
    }

    uint32_t cpu = task->assigned_cpu;
    struct cpu_runqueue *rq = &sched.cpu[cpu];
    irq_flags_t flags = spin_lock_irqsave(&rq->lock);

    /*
     * Task might already have been removed from the queue when it
     * started running (schedule() removes from queue before switching).
     * Only decrement task_count if task was actually in the queue.
     */
    if (remove_from_cpu_queue_locked(task, cpu)) {
        sched.task_count--;  /* Racy but acceptable for stats */
        DEBUG_PRINT("Removed task '%s' from CPU %u run queue (ready=%u)",
                    task->name, cpu, rq->ready_count);
    }

    spin_unlock_irqrestore(&rq->lock, flags);
}

/*
 * Migrate a task to a different CPU.
 *
 * Locks both source and target queues in CPU ID order to prevent deadlock.
 */
int sched_migrate_task(struct task *task, uint32_t target_cpu)
{
    if (!task || target_cpu >= cpu_count) {
        return -1;
    }

    /* Cannot migrate running tasks */
    if (task->state == TASK_RUNNING) {
        WARN("Cannot migrate running task '%s'", task->name);
        return -1;
    }

    /* Check affinity constraint */
    if (task->cpu_affinity != CPU_AFFINITY_ANY &&
        task->cpu_affinity != target_cpu) {
        WARN("Cannot migrate task '%s' - affinity constraint", task->name);
        return -1;
    }

    uint32_t old_cpu = task->assigned_cpu;

    if (old_cpu == target_cpu) {
        return 0;  /* Already on target CPU */
    }

    /*
     * Lock both queues in CPU ID order to prevent deadlock.
     * If old_cpu < target_cpu, lock old first; otherwise lock target first.
     */
    struct cpu_runqueue *rq_old = &sched.cpu[old_cpu];
    struct cpu_runqueue *rq_new = &sched.cpu[target_cpu];
    irq_flags_t flags;

    if (old_cpu < target_cpu) {
        flags = spin_lock_irqsave(&rq_old->lock);
        spin_lock(&rq_new->lock);
    } else {
        flags = spin_lock_irqsave(&rq_new->lock);
        spin_lock(&rq_old->lock);
    }

    /* Remove from old CPU queue if task is ready */
    if (task->state == TASK_READY) {
        remove_from_cpu_queue_locked(task, old_cpu);
        add_to_cpu_queue_locked(task, target_cpu);
    } else {
        /* Task is blocked - just update assigned_cpu */
        task->assigned_cpu = target_cpu;
    }

    /* Clean modified fields to PoC for cross-CPU visibility.
     * Without SMPEN, writes stay in this CPU's L1 cache. The target
     * CPU's schedule() invalidates before reading. */
    cache_clean(&rq_old->head);
    cache_clean(&rq_old->tail);
    cache_clean(&rq_old->ready_count);
    cache_clean(&rq_new->head);
    cache_clean(&rq_new->tail);
    cache_clean(&rq_new->ready_count);
    cache_clean(&task->next);
    cache_clean(&task->assigned_cpu);
    cache_clean(&task->state);
    cache_clean(&task->effective_priority);

    DEBUG_PRINT("Migrated task '%s' from CPU %u to CPU %u",
                task->name, old_cpu, target_cpu);

    /* Unlock in reverse order */
    if (old_cpu < target_cpu) {
        spin_unlock(&rq_new->lock);
        spin_unlock_irqrestore(&rq_old->lock, flags);
    } else {
        spin_unlock(&rq_old->lock);
        spin_unlock_irqrestore(&rq_new->lock, flags);
    }

    return 0;
}

/*
 * Pick the next task to run on this CPU.
 */
static struct task *pick_next_task(uint32_t cpu)
{
    struct cpu_runqueue *rq = &sched.cpu[cpu];

    if (rq->head) {
        return rq->head;
    }

    /* No ready tasks - run idle task */
    return rq->idle_task;
}

/*
 * Schedule - select next task and switch to it (per-CPU).
 */
void schedule(void)
{
    uint32_t this_cpu = cpu_id();
    struct cpu_runqueue *rq = &sched.cpu[this_cpu];

    /* Invalidate our cached copy of the run queue before reading.
     * Another CPU may have added tasks to our queue (scheduler_add_task).
     * Without SMPEN, those writes stay in the other CPU's L1 cache. */
    cache_invalidate_range(rq, sizeof(*rq));

    irq_flags_t flags = spin_lock_irqsave(&rq->lock);

    /*
     * Clean up zombie task from previous schedule cycle.
     * This is safe because we've already switched away from it.
     */
    if (rq->zombie) {
        struct task *zombie = rq->zombie;
        rq->zombie = NULL;
        cache_clean(&rq->zombie);
        spin_unlock_irqrestore(&rq->lock, flags);

        /* Destroy outside lock - task_destroy may call pmm */
        task_destroy(zombie);

        flags = spin_lock_irqsave(&rq->lock);
    }

    struct task *current = task_current();
    struct task *next = pick_next_task(this_cpu);

    /* If current task is still running and ready, re-add to queue */
    if (current) {
        cache_invalidate(&current->state);
    }
    if (current && current->state == TASK_RUNNING) {
        current->state = TASK_READY;
        cache_clean(&current->state);

        /* Re-add to run queue if it's a normal task (not idle) */
        if (current != rq->idle_task) {
            /* Priority-ordered insertion (updates deadline boost) */
            add_to_cpu_queue_locked(current, this_cpu);
        }

        /* Update deadline boost for next candidate too */
        if (rq->head) {
            update_deadline_boost(rq->head);
        }

        /* Re-pick in case queue changed */
        next = pick_next_task(this_cpu);
    }

    /* Remove next from front of queue */
    if (next != rq->idle_task && rq->head == next) {
        rq->head = next->next;
        if (!rq->head) {
            rq->tail = NULL;
        }
        next->next = NULL;
        rq->ready_count--;
    }

    /*
     * No switch needed if same task AND task is not terminated.
     * If the current task is terminated, we MUST switch to a different task.
     */
    if (next == current) {
        if (current && current->state == TASK_TERMINATED) {
            /*
             * This should never happen: terminated task picked as next.
             * The terminated task should not be in the run queue, and
             * pick_next_task should return idle_task if queue is empty.
             */
            spin_unlock_irqrestore(&rq->lock, flags);
            panic("schedule: terminated task selected as next (CPU %u, task '%s')",
                  this_cpu, current->name);
        }
        if (current) {
            current->state = TASK_RUNNING;
        }
        spin_unlock_irqrestore(&rq->lock, flags);
        return;
    }

    /* Perform context switch */
    next->state = TASK_RUNNING;
    cache_clean(&next->state);
    next->switches++;
    cache_clean(&next->switches);
    sched.context_switches++;  /* Racy but acceptable for stats */

    /*
     * If current task is terminated, mark it as zombie for cleanup.
     * It will be destroyed on the next schedule() call after we've
     * safely switched to a different stack.
     */
    if (current) {
        cache_invalidate(&current->state);
    }
    if (current && current->state == TASK_TERMINATED) {
        rq->zombie = current;
    }

    task_set_current(next);

    /* Clean run queue fields modified in this schedule() cycle.
     * Without this, the next dc civac at the start of schedule() would
     * write back our stale dirty cacheline, overwriting another CPU's
     * fresh data (e.g., a newly added task from scheduler_add_task). */
    cache_clean(&rq->head);
    cache_clean(&rq->tail);
    cache_clean(&rq->ready_count);
    cache_clean(&rq->zombie);

    spin_unlock_irqrestore(&rq->lock, flags);

    /* switch_to saves current context and restores next's context */
    switch_to(current, next);
}

/*
 * Yield - voluntarily give up the CPU.
 */
void yield(void)
{
    schedule();
}

/*
 * Start the scheduler on this CPU.
 */
void scheduler_start(void)
{
    uint32_t this_cpu = cpu_id();
    struct cpu_runqueue *rq = &sched.cpu[this_cpu];

    if (!sched.initialized) {
        panic("scheduler_start: scheduler not initialized");
    }

    INFO("CPU %u: Starting scheduler", this_cpu);

    irq_flags_t flags = spin_lock_irqsave(&rq->lock);

    /* Pick first task */
    struct task *first = pick_next_task(this_cpu);
    if (!first) {
        spin_unlock_irqrestore(&rq->lock, flags);
        panic("scheduler_start: no tasks to run");
    }

    /* Remove from queue */
    if (first != rq->idle_task && rq->head == first) {
        rq->head = first->next;
        if (!rq->head) {
            rq->tail = NULL;
        }
        rq->ready_count--;
    }

    first->state = TASK_RUNNING;
    first->switches++;
    sched.context_switches++;  /* Racy but acceptable for stats */

    task_set_current(first);

    INFO("CPU %u: Switching to first task: '%s'", this_cpu, first->name);

    spin_unlock_irqrestore(&rq->lock, flags);

    /* Start timer and enable interrupts now that a task is active.
     * Must be done AFTER task_set_current() so that timer IRQ handler
     * can safely call task_current() in schedule(). */
    INFO("Starting timer (100 Hz)...");
    timer_start();

    INFO("Enabling interrupts...");
    __asm__ volatile("msr daifclr, #0x2" ::: "memory");  /* Clear IRQ mask */
    __asm__ volatile("isb" ::: "memory");  /* Ensure unmask takes effect */

    /* Switch to first task (NULL = no previous context to save) */
    switch_to(NULL, first);

    /* Should never reach here */
    panic("scheduler_start: switch_to returned!");
}

/*
 * Timer tick handler (called from interrupt).
 */
void scheduler_tick(void)
{
    /* Note: timer_ticks is racy but acceptable for stats */
    sched.timer_ticks++;

    /* Preempt current task */
    schedule();
}

/*
 * Get scheduler statistics.
 *
 * Note: Stats are read without locking (racy but acceptable).
 * This avoids lock contention for frequent stats queries.
 */
void scheduler_get_stats(struct sched_stats *stats)
{
    if (!stats) return;

    /* Read global stats (racy but acceptable for diagnostics) */
    stats->task_count = sched.task_count;
    stats->context_switches = sched.context_switches;
    stats->timer_ticks = sched.timer_ticks;

    /* Sum ready counts from all CPUs */
    stats->ready_count = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        stats->ready_count += sched.cpu[i].ready_count;
    }
}

/*
 * Dump scheduler state for debugging.
 *
 * Locks each CPU queue individually while dumping it.
 */
void scheduler_dump(void)
{
    /* Print global stats (racy but acceptable for debug output) */
    uart_puts("\nScheduler State:\n");
    uart_printf("  Initialized:      %s\n", sched.initialized ? "yes" : "no");
    uart_printf("  Task count:       %u\n", sched.task_count);
    uart_printf("  Context switches: %lu\n", sched.context_switches);
    uart_printf("  Timer ticks:      %lu\n", sched.timer_ticks);

    struct task *current = task_current();
    if (current) {
        uart_printf("  Current task:     '%s' (id=%u, cpu=%u)\n",
                    current->name, current->id, cpu_id());
    }

    /* Dump per-CPU run queues (lock each individually) */
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu_runqueue *rq = &sched.cpu[i];
        irq_flags_t flags = spin_lock_irqsave(&rq->lock);

        uart_printf("  CPU %u queue (%u): ", i, rq->ready_count);

        if (rq->head) {
            struct task *t = rq->head;
            while (t) {
                uart_printf("%s[p%u]", t->name, t->effective_priority);
                if (t->next) uart_puts(" -> ");
                t = t->next;
            }
            uart_puts("\n");
        } else {
            uart_puts("(empty)\n");
        }

        spin_unlock_irqrestore(&rq->lock, flags);
    }
}

/*
 * Isolate a core from general scheduling.
 *
 * Isolated cores only run tasks that are explicitly pinned to them
 * via cpu_affinity. Tasks with CPU_AFFINITY_ANY will not be placed
 * on isolated cores.
 *
 * Additionally, SPIs (Shared Peripheral Interrupts) are routed away
 * from isolated cores to minimize interrupt interference. Timer IRQs
 * (PPIs) are unaffected - each CPU still gets its own timer interrupt
 * for scheduler preemption.
 *
 * Use for real-time or latency-sensitive workloads that need
 * dedicated CPU time without interference from other tasks.
 *
 * @cpu: CPU ID to isolate
 *
 * Returns: 0 on success, -1 if invalid CPU
 */
int sched_isolate_core(uint32_t cpu)
{
    if (cpu >= cpu_count) {
        return -1;
    }

    /* Cannot isolate CPU 0 (boot CPU runs system tasks) */
    if (cpu == 0) {
        WARN("Cannot isolate CPU 0 (boot CPU)");
        return -1;
    }

    sched.isolated_cores |= (1U << cpu);

    /* Route SPIs away from this CPU to minimize interrupt interference.
     * Timer IRQs (PPIs) are unaffected - still needed for preemption. */
    gic_exclude_cpu_from_spis(cpu);

    INFO("CPU %u: isolated from general scheduling (SPIs excluded)", cpu);
    return 0;
}

/*
 * Remove core isolation.
 *
 * Restores SPI routing to include this CPU and returns it to the
 * general scheduling pool.
 *
 * @cpu: CPU ID to un-isolate
 *
 * Returns: 0 on success, -1 if invalid CPU
 */
int sched_unisolate_core(uint32_t cpu)
{
    if (cpu >= cpu_count) {
        return -1;
    }

    sched.isolated_cores &= ~(1U << cpu);

    /* Restore SPI routing to this CPU */
    gic_include_cpu_in_spis(cpu);

    INFO("CPU %u: returned to general scheduling (SPIs restored)", cpu);
    return 0;
}

/*
 * Check if a core is isolated.
 *
 * @cpu: CPU ID to check
 *
 * Returns: 1 if isolated, 0 if not (or invalid CPU)
 */
int sched_is_core_isolated(uint32_t cpu)
{
    if (cpu >= cpu_count) {
        return 0;
    }
    return (sched.isolated_cores & (1U << cpu)) != 0;
}

/*
 * Get the bitmask of isolated cores.
 */
uint32_t sched_get_isolated_cores(void)
{
    return sched.isolated_cores;
}

/*
 * Set task CPU affinity.
 *
 * If the task is currently in a run queue, it will be migrated
 * to the new target CPU.
 */
int sched_set_task_affinity(struct task *task, uint32_t cpu)
{
    if (!task) {
        return -1;
    }

    /* Validate CPU (CPU_AFFINITY_ANY is a special value) */
    if (cpu != CPU_AFFINITY_ANY && cpu >= cpu_count) {
        return -1;
    }

    uint32_t old_affinity = task->cpu_affinity;
    task->cpu_affinity = cpu;

    /*
     * If affinity changed and task is ready, migrate to appropriate CPU.
     * Running tasks will be placed correctly on next schedule().
     */
    if (task->state == TASK_READY && old_affinity != cpu) {
        uint32_t target;
        if (cpu == CPU_AFFINITY_ANY) {
            target = find_target_cpu();
        } else {
            target = cpu;
        }

        if (target != task->assigned_cpu) {
            sched_migrate_task(task, target);
        }
    }

    return 0;
}
