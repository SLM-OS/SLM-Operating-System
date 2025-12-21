/*
 * task.c - Task management for SLM-OS
 */

#include "task.h"
#include "sched.h"
#include "pmm.h"
#include "uart.h"
#include "debug.h"
#include <stddef.h>

/* Task table - static allocation for simplicity */
static struct task task_table[MAX_TASKS];
static uint32_t next_task_id = 1;       /* ID 0 reserved for idle task */

/* Current running task (set by scheduler) */
static struct task *current_task = NULL;

/*
 * String copy helper (no libc)
 */
static void str_copy(char *dst, const char *src, size_t max)
{
    size_t i;
    for (i = 0; i < max - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/*
 * Find a free slot in the task table.
 */
static struct task *alloc_task_slot(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].id == 0) {
            return &task_table[i];
        }
    }
    return NULL;
}

/*
 * Task wrapper function.
 *
 * This is the actual entry point set in the task's context.
 * It calls the user's entry function and handles task exit.
 *
 * When we context-switch to a new task, x19 contains the entry point
 * and x20 contains the argument. We read these immediately before
 * the C compiler might use these callee-saved registers for other purposes.
 */
static void task_entry_wrapper(void)
{
    /* Read entry point and arg from registers (set by task_create) */
    register uint64_t entry_reg __asm__("x19");
    register uint64_t arg_reg __asm__("x20");

    task_entry_t entry = (task_entry_t)entry_reg;
    void *arg = (void *)arg_reg;

    /* Call the actual task function */
    entry(arg);

    /* Task returned - exit cleanly */
    task_exit();
}

/*
 * Create a new task.
 */
struct task *task_create(const char *name, task_entry_t entry, void *arg)
{
    /* Find free task slot */
    struct task *task = alloc_task_slot();
    if (!task) {
        ERROR("task_create: no free task slots");
        return NULL;
    }

    /* Allocate stack (16KB = 4 pages) */
    size_t stack_pages = TASK_STACK_SIZE / 4096;
    void *stack = pmm_alloc_pages(stack_pages);
    if (!stack) {
        ERROR("task_create: failed to allocate stack");
        return NULL;
    }

    /* Initialize task structure */
    task->id = next_task_id++;
    str_copy(task->name, name ? name : "unnamed", TASK_NAME_LEN);
    task->state = TASK_READY;
    task->next = NULL;
    task->switches = 0;

    /* Set up stack (grows downward on ARM64) */
    task->stack_base = stack;
    task->stack_top = (void *)((uintptr_t)stack + TASK_STACK_SIZE);

    /* Initialize CPU context */
    /* Zero out the context first */
    for (size_t i = 0; i < sizeof(task->context); i++) {
        ((uint8_t *)&task->context)[i] = 0;
    }

    /* Set up initial context for first switch */
    task->context.sp = (uint64_t)task->stack_top;
    task->context.x30 = (uint64_t)task_entry_wrapper;  /* Return address */
    task->context.x29 = 0;                              /* Frame pointer */

    /* Store entry point and arg in callee-saved registers for wrapper */
    task->context.x19 = (uint64_t)entry;
    task->context.x20 = (uint64_t)arg;

    DEBUG_PRINT("Created task '%s' (id=%u, stack=%p-%p)",
                task->name, task->id, task->stack_base, task->stack_top);

    return task;
}

/*
 * Terminate the current task.
 */
void task_exit(void)
{
    struct task *task = task_current();

    INFO("Task '%s' (id=%u) exiting", task->name, task->id);

    task->state = TASK_TERMINATED;

    /* Remove from run queue and schedule next task */
    scheduler_remove_task(task);
    schedule();

    /* Should never reach here */
    panic("task_exit: schedule returned!");
}

/*
 * Get current running task.
 */
struct task *task_current(void)
{
    return current_task;
}

/*
 * Set current running task (called by scheduler).
 */
void task_set_current(struct task *task)
{
    current_task = task;
}

/*
 * Get task by ID.
 */
struct task *task_get(uint32_t id)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].id == id) {
            return &task_table[i];
        }
    }
    return NULL;
}

/*
 * Free a terminated task's resources.
 */
void task_destroy(struct task *task)
{
    if (!task || task->state != TASK_TERMINATED) {
        return;
    }

    DEBUG_PRINT("Destroying task '%s' (id=%u)", task->name, task->id);

    /* Free stack */
    if (task->stack_base) {
        size_t stack_pages = TASK_STACK_SIZE / 4096;
        pmm_free_pages(task->stack_base, stack_pages);
    }

    /* Clear task slot */
    task->id = 0;
    task->name[0] = '\0';
    task->state = TASK_TERMINATED;
    task->stack_base = NULL;
    task->stack_top = NULL;
}
