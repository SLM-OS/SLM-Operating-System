/*
 * task.c - Task management for SLM-OS
 */

#include "task.h"
#include "sched.h"
#include "pmm.h"
#include "uart.h"
#include "debug.h"
#include "smp.h"
#include "spinlock.h"
#include <stddef.h>

/* Task table - static allocation for simplicity */
static struct task task_table[MAX_TASKS];
static uint32_t next_task_id = 1;       /* ID 0 reserved for idle task */

/* Lock protecting task_table and next_task_id */
static spinlock_t task_lock = SPINLOCK_INIT;

/* Per-CPU current running task (set by scheduler) */
static struct task *current_task[MAX_CPUS];

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
 * and x20 contains the argument. We use inline assembly to read these
 * registers before the compiler can clobber them.
 */
static void task_entry_wrapper(void)
{
    uint64_t entry_reg, arg_reg;

    /*
     * Read entry point and arg from callee-saved registers.
     * These were set by task_create and restored by switch_to.
     * We must use volatile asm to ensure the compiler actually reads the registers.
     */
    __asm__ volatile("mov %0, x19" : "=r"(entry_reg));
    __asm__ volatile("mov %0, x20" : "=r"(arg_reg));

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
    irq_flags_t flags;
    struct task *task;
    uint32_t task_id;

    /* Allocate stack first (outside lock - pmm has its own locking) */
    size_t stack_pages = TASK_STACK_SIZE / 4096;
    void *stack = pmm_alloc_pages(stack_pages);
    if (!stack) {
        ERROR("task_create: failed to allocate stack");
        return NULL;
    }

    /* Acquire lock to access task_table and next_task_id */
    flags = spin_lock_irqsave(&task_lock);

    /* Find free task slot */
    task = alloc_task_slot();
    if (!task) {
        spin_unlock_irqrestore(&task_lock, flags);
        pmm_free_pages(stack, stack_pages);
        ERROR("task_create: no free task slots");
        return NULL;
    }

    /* Reserve task ID atomically */
    task_id = next_task_id++;

    /* Mark slot as used immediately (id != 0 means in use) */
    task->id = task_id;

    spin_unlock_irqrestore(&task_lock, flags);

    /* Initialize rest of task structure (slot is ours now) */
    str_copy(task->name, name ? name : "unnamed", TASK_NAME_LEN);
    task->state = TASK_READY;
    task->next = NULL;
    task->cpu_affinity = CPU_AFFINITY_ANY;  /* Can run on any CPU */
    task->assigned_cpu = 0;                  /* Default to CPU 0 */
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
 * Get current running task (for this CPU).
 */
struct task *task_current(void)
{
    return current_task[cpu_id()];
}

/*
 * Set current running task (called by scheduler).
 */
void task_set_current(struct task *task)
{
    current_task[cpu_id()] = task;
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

/*
 * Set task CPU affinity.
 */
void task_set_affinity(struct task *task, uint32_t cpu)
{
    if (!task) return;

    task->cpu_affinity = cpu;

    /* If pinning to a specific CPU, update assigned_cpu */
    if (cpu != CPU_AFFINITY_ANY && cpu < cpu_count) {
        task->assigned_cpu = cpu;
    }
}

/*
 * Get task CPU affinity.
 */
uint32_t task_get_affinity(struct task *task)
{
    if (!task) return CPU_AFFINITY_ANY;
    return task->cpu_affinity;
}
