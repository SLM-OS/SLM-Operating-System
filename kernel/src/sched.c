/*
 * sched.c - Per-core round-robin scheduler for SLM-OS
 *
 * Each CPU has its own run queue. A global spinlock protects all
 * scheduler data structures (simpler than per-queue locks, sufficient
 * for 4-6 cores).
 */

#include "sched.h"
#include "task.h"
#include "uart.h"
#include "debug.h"
#include "platform.h"
#include "smp.h"
#include "spinlock.h"
#include <stddef.h>

/* External functions from task.c */
extern void task_set_current(struct task *task);
extern void task_destroy(struct task *task);

/* Per-CPU run queue */
struct cpu_runqueue {
    struct task *head;          /* First task in run queue */
    struct task *tail;          /* Last task in run queue */
    struct task *idle_task;     /* This CPU's idle task */
    struct task *zombie;        /* Terminated task pending cleanup */
    uint32_t ready_count;       /* Tasks in this CPU's run queue */
};

/*
 * Global scheduler state.
 *
 * Lock strategy: Single global lock protects all scheduler data.
 * This is simpler than per-queue locks and sufficient for Phase 2.
 * Phase 3 will implement per-queue locks for scalability to larger
 * core counts. The SLM-based scheduler may also supersede this with
 * smarter task placement.
 */
static struct {
    struct cpu_runqueue cpu[MAX_CPUS];  /* Per-CPU run queues */
    spinlock_t lock;                     /* Global scheduler lock */
    uint32_t task_count;                 /* Total tasks in system */
    uint64_t context_switches;           /* Total switches performed */
    uint64_t timer_ticks;                /* Timer interrupts received */
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
        /* Wait for interrupt (timer will wake us) */
        __asm__ volatile("wfi");

        /* Yield to check if other tasks are ready */
        yield();
    }
}

/*
 * Initialize the scheduler (called on boot CPU).
 */
void scheduler_init(void)
{
    /* Initialize global state */
    spin_init(&sched.lock);
    sched.task_count = 0;
    sched.context_switches = 0;
    sched.timer_ticks = 0;

    /* Initialize per-CPU run queues */
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        sched.cpu[i].head = NULL;
        sched.cpu[i].tail = NULL;
        sched.cpu[i].idle_task = NULL;
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

    /* Wake any secondary CPUs waiting for scheduler init */
    __asm__ volatile("sev" ::: "memory");

    INFO("Scheduler initialized");
}

/*
 * Check if the scheduler has been initialized.
 */
int scheduler_is_initialized(void)
{
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

    irq_flags_t flags = spin_lock_irqsave(&sched.lock);

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

    spin_unlock_irqrestore(&sched.lock, flags);

    /* Create task outside of lock (task_create may allocate memory) */
    struct task *idle = task_create(idle_name, idle_task_func, NULL);
    if (!idle) {
        panic("scheduler_init_secondary: failed to create idle task for CPU %u", cpu);
    }

    flags = spin_lock_irqsave(&sched.lock);

    idle->state = TASK_READY;
    idle->cpu_affinity = cpu;  /* Pinned to this CPU */
    idle->assigned_cpu = cpu;
    sched.cpu[cpu].idle_task = idle;

    spin_unlock_irqrestore(&sched.lock, flags);

    INFO("CPU %u: scheduler initialized", cpu);
}

/*
 * Add a task to a specific CPU's run queue (internal, requires lock held).
 */
static void add_to_cpu_queue_locked(struct task *task, uint32_t cpu)
{
    struct cpu_runqueue *rq = &sched.cpu[cpu];

    task->next = NULL;
    task->assigned_cpu = cpu;

    if (rq->tail) {
        rq->tail->next = task;
        rq->tail = task;
    } else {
        rq->head = task;
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

    irq_flags_t flags = spin_lock_irqsave(&sched.lock);

    add_to_cpu_queue_locked(task, cpu);
    sched.task_count++;

    DEBUG_PRINT("Added task '%s' to CPU %u run queue (ready=%u)",
                task->name, cpu, sched.cpu[cpu].ready_count);

    spin_unlock_irqrestore(&sched.lock, flags);
}

/*
 * Add a task to the run queue (assigns to a CPU based on affinity).
 */
void scheduler_add_task(struct task *task)
{
    if (!task || task->state != TASK_READY) {
        return;
    }

    uint32_t target_cpu;

    if (task->cpu_affinity != CPU_AFFINITY_ANY) {
        /* Task is pinned to a specific CPU */
        target_cpu = task->cpu_affinity;
    } else {
        /* Default to CPU 0 for now (load balancing can improve this later) */
        target_cpu = 0;
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

    irq_flags_t flags = spin_lock_irqsave(&sched.lock);

    /*
     * Task might already have been removed from the queue when it
     * started running (schedule() removes from queue before switching).
     * Only decrement task_count if task was actually in the queue.
     */
    if (remove_from_cpu_queue_locked(task, task->assigned_cpu)) {
        sched.task_count--;
        DEBUG_PRINT("Removed task '%s' from CPU %u run queue (ready=%u)",
                    task->name, task->assigned_cpu,
                    sched.cpu[task->assigned_cpu].ready_count);
    }

    spin_unlock_irqrestore(&sched.lock, flags);
}

/*
 * Migrate a task to a different CPU.
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

    irq_flags_t flags = spin_lock_irqsave(&sched.lock);

    uint32_t old_cpu = task->assigned_cpu;

    if (old_cpu == target_cpu) {
        spin_unlock_irqrestore(&sched.lock, flags);
        return 0;  /* Already on target CPU */
    }

    /* Remove from old CPU queue if task is ready */
    if (task->state == TASK_READY) {
        remove_from_cpu_queue_locked(task, old_cpu);
        add_to_cpu_queue_locked(task, target_cpu);
    } else {
        /* Task is blocked - just update assigned_cpu */
        task->assigned_cpu = target_cpu;
    }

    DEBUG_PRINT("Migrated task '%s' from CPU %u to CPU %u",
                task->name, old_cpu, target_cpu);

    spin_unlock_irqrestore(&sched.lock, flags);
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

    irq_flags_t flags = spin_lock_irqsave(&sched.lock);

    /*
     * Clean up zombie task from previous schedule cycle.
     * This is safe because we've already switched away from it.
     */
    if (rq->zombie) {
        struct task *zombie = rq->zombie;
        rq->zombie = NULL;
        spin_unlock_irqrestore(&sched.lock, flags);

        /* Destroy outside lock - task_destroy may call pmm */
        task_destroy(zombie);

        flags = spin_lock_irqsave(&sched.lock);
    }

    struct task *current = task_current();
    struct task *next = pick_next_task(this_cpu);

    /* If current task is still running and ready, move to back of queue */
    if (current && current->state == TASK_RUNNING) {
        current->state = TASK_READY;

        /* Re-add to run queue if it's a normal task (not idle) */
        if (current != rq->idle_task) {
            current->next = NULL;

            /* Add to tail of queue */
            if (rq->tail) {
                rq->tail->next = current;
                rq->tail = current;
            } else {
                rq->head = current;
                rq->tail = current;
            }
            rq->ready_count++;
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

    /* No switch needed if same task */
    if (next == current) {
        if (current) {
            current->state = TASK_RUNNING;
        }
        spin_unlock_irqrestore(&sched.lock, flags);
        return;
    }

    /* Perform context switch */
    next->state = TASK_RUNNING;
    next->switches++;
    sched.context_switches++;

    /*
     * If current task is terminated, mark it as zombie for cleanup.
     * It will be destroyed on the next schedule() call after we've
     * safely switched to a different stack.
     */
    if (current && current->state == TASK_TERMINATED) {
        rq->zombie = current;
    }

    task_set_current(next);

    spin_unlock_irqrestore(&sched.lock, flags);

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

    if (!sched.initialized) {
        panic("scheduler_start: scheduler not initialized");
    }

    INFO("CPU %u: Starting scheduler", this_cpu);

    irq_flags_t flags = spin_lock_irqsave(&sched.lock);

    /* Pick first task */
    struct task *first = pick_next_task(this_cpu);
    if (!first) {
        spin_unlock_irqrestore(&sched.lock, flags);
        panic("scheduler_start: no tasks to run");
    }

    /* Remove from queue */
    struct cpu_runqueue *rq = &sched.cpu[this_cpu];
    if (first != rq->idle_task && rq->head == first) {
        rq->head = first->next;
        if (!rq->head) {
            rq->tail = NULL;
        }
        rq->ready_count--;
    }

    first->state = TASK_RUNNING;
    first->switches++;
    sched.context_switches++;

    task_set_current(first);

    INFO("CPU %u: Switching to first task: '%s'", this_cpu, first->name);

    spin_unlock_irqrestore(&sched.lock, flags);

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
 */
void scheduler_get_stats(struct sched_stats *stats)
{
    if (!stats) return;

    irq_flags_t flags = spin_lock_irqsave(&sched.lock);

    stats->task_count = sched.task_count;
    stats->ready_count = 0;
    for (uint32_t i = 0; i < cpu_count; i++) {
        stats->ready_count += sched.cpu[i].ready_count;
    }
    stats->context_switches = sched.context_switches;
    stats->timer_ticks = sched.timer_ticks;

    spin_unlock_irqrestore(&sched.lock, flags);
}

/*
 * Dump scheduler state for debugging.
 */
void scheduler_dump(void)
{
    irq_flags_t flags = spin_lock_irqsave(&sched.lock);

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

    /* Dump per-CPU run queues */
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu_runqueue *rq = &sched.cpu[i];
        uart_printf("  CPU %u queue (%u): ", i, rq->ready_count);

        if (rq->head) {
            struct task *t = rq->head;
            while (t) {
                uart_printf("%s", t->name);
                if (t->next) uart_puts(" -> ");
                t = t->next;
            }
            uart_puts("\n");
        } else {
            uart_puts("(empty)\n");
        }
    }

    spin_unlock_irqrestore(&sched.lock, flags);
}
