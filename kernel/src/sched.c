/*
 * sched.c - Round-robin scheduler for SLM-OS
 */

#include "sched.h"
#include "task.h"
#include "uart.h"
#include "debug.h"
#include "platform.h"
#include <stddef.h>

/* External function to set current task (in task.c) */
extern void task_set_current(struct task *task);

/* Scheduler state */
static struct {
    struct task *run_queue_head;    /* First task in run queue */
    struct task *run_queue_tail;    /* Last task in run queue */
    struct task *idle_task;         /* Idle task (runs when nothing else ready) */
    uint32_t task_count;            /* Total tasks in system */
    uint32_t ready_count;           /* Tasks in run queue */
    uint64_t context_switches;      /* Total switches performed */
    uint64_t timer_ticks;           /* Timer interrupts received */
    int initialized;                /* Scheduler initialized flag */
} sched;

/*
 * Idle task - runs when no other tasks are ready.
 *
 * Simply waits for interrupts.
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
 * Initialize the scheduler.
 */
void scheduler_init(void)
{
    /* Clear scheduler state */
    sched.run_queue_head = NULL;
    sched.run_queue_tail = NULL;
    sched.idle_task = NULL;
    sched.task_count = 0;
    sched.ready_count = 0;
    sched.context_switches = 0;
    sched.timer_ticks = 0;
    sched.initialized = 1;

    /* Create idle task */
    sched.idle_task = task_create("idle", idle_task_func, NULL);
    if (!sched.idle_task) {
        panic("scheduler_init: failed to create idle task");
    }

    /* Idle task is special - don't add to normal run queue */
    sched.idle_task->state = TASK_READY;

    INFO("Scheduler initialized");
}

/*
 * Add a task to the run queue (tail).
 */
void scheduler_add_task(struct task *task)
{
    if (!task || task->state != TASK_READY) {
        return;
    }

    task->next = NULL;

    if (sched.run_queue_tail) {
        sched.run_queue_tail->next = task;
        sched.run_queue_tail = task;
    } else {
        sched.run_queue_head = task;
        sched.run_queue_tail = task;
    }

    sched.ready_count++;
    sched.task_count++;

    DEBUG_PRINT("Added task '%s' to run queue (ready=%u)",
                task->name, sched.ready_count);
}

/*
 * Remove a task from the run queue.
 */
void scheduler_remove_task(struct task *task)
{
    if (!task) {
        return;
    }

    struct task *prev = NULL;
    struct task *curr = sched.run_queue_head;

    while (curr) {
        if (curr == task) {
            /* Found it - unlink */
            if (prev) {
                prev->next = curr->next;
            } else {
                sched.run_queue_head = curr->next;
            }

            if (curr == sched.run_queue_tail) {
                sched.run_queue_tail = prev;
            }

            task->next = NULL;
            sched.ready_count--;

            DEBUG_PRINT("Removed task '%s' from run queue (ready=%u)",
                        task->name, sched.ready_count);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

/*
 * Pick the next task to run.
 *
 * Returns head of run queue, or idle task if queue is empty.
 */
static struct task *pick_next_task(void)
{
    if (sched.run_queue_head) {
        return sched.run_queue_head;
    }

    /* No ready tasks - run idle task */
    return sched.idle_task;
}

/*
 * Schedule - select next task and switch to it.
 */
void schedule(void)
{
    struct task *current = task_current();
    struct task *next = pick_next_task();

    /* If current task is still running and ready, move to back of queue */
    if (current && current->state == TASK_RUNNING) {
        current->state = TASK_READY;

        /* Re-add to run queue if it's a normal task (not idle) */
        if (current != sched.idle_task) {
            /* Remove from front (it was running) and add to back */
            if (sched.run_queue_head == current) {
                sched.run_queue_head = current->next;
                if (!sched.run_queue_head) {
                    sched.run_queue_tail = NULL;
                }
            }
            current->next = NULL;

            /* Add to tail */
            if (sched.run_queue_tail) {
                sched.run_queue_tail->next = current;
                sched.run_queue_tail = current;
            } else {
                sched.run_queue_head = current;
                sched.run_queue_tail = current;
            }
        }

        /* Re-pick in case queue changed */
        next = pick_next_task();
    }

    /* Remove next from front of queue */
    if (next != sched.idle_task && sched.run_queue_head == next) {
        sched.run_queue_head = next->next;
        if (!sched.run_queue_head) {
            sched.run_queue_tail = NULL;
        }
        next->next = NULL;
        sched.ready_count--;
    }

    /* No switch needed if same task */
    if (next == current) {
        if (current) {
            current->state = TASK_RUNNING;
        }
        return;
    }

    /* Perform context switch */
    next->state = TASK_RUNNING;
    next->switches++;
    sched.context_switches++;

    task_set_current(next);

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
 * Start the scheduler.
 *
 * This doesn't return - it switches to the first ready task.
 */
void scheduler_start(void)
{
    if (!sched.initialized) {
        panic("scheduler_start: scheduler not initialized");
    }

    INFO("Starting scheduler");

    /* Pick first task */
    struct task *first = pick_next_task();
    if (!first) {
        panic("scheduler_start: no tasks to run");
    }

    /* Remove from queue */
    if (first != sched.idle_task && sched.run_queue_head == first) {
        sched.run_queue_head = first->next;
        if (!sched.run_queue_head) {
            sched.run_queue_tail = NULL;
        }
        sched.ready_count--;
    }

    first->state = TASK_RUNNING;
    first->switches++;
    sched.context_switches++;

    task_set_current(first);

    INFO("Switching to first task: '%s'", first->name);

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

    stats->task_count = sched.task_count;
    stats->ready_count = sched.ready_count;
    stats->context_switches = sched.context_switches;
    stats->timer_ticks = sched.timer_ticks;
}

/*
 * Dump scheduler state for debugging.
 */
void scheduler_dump(void)
{
    uart_puts("\nScheduler State:\n");
    uart_printf("  Initialized:      %s\n", sched.initialized ? "yes" : "no");
    uart_printf("  Task count:       %u\n", sched.task_count);
    uart_printf("  Ready count:      %u\n", sched.ready_count);
    uart_printf("  Context switches: %lu\n", sched.context_switches);
    uart_printf("  Timer ticks:      %lu\n", sched.timer_ticks);

    struct task *current = task_current();
    if (current) {
        uart_printf("  Current task:     '%s' (id=%u)\n", current->name, current->id);
    }

    uart_puts("  Run queue:        ");
    if (sched.run_queue_head) {
        struct task *t = sched.run_queue_head;
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
