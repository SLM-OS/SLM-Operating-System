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
#include "cache.h"
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

/* Forward declaration — defined in assembly below */
extern void task_entry_wrapper(void);

/*
 * C trampoline called from assembly task_entry_wrapper.
 * x19 (entry) and x20 (arg) are passed as function arguments
 * by the assembly stub, avoiding any risk of compiler clobbering.
 */
void task_entry_trampoline(uint64_t entry_addr, uint64_t arg_addr)
{
    task_entry_t entry = (task_entry_t)entry_addr;
    void *arg = (void *)arg_addr;

    entry(arg);
    task_exit();
}

/*
 * Task entry wrapper (assembly).
 *
 * When switch_to restores a new task for the first time, x19 holds
 * the entry point and x20 holds the argument (set by task_create).
 * This must be in assembly to guarantee x19/x20 are read before
 * the compiler can use them for its own purposes in a C prologue.
 */
__asm__(
    ".global task_entry_wrapper\n"
    ".type task_entry_wrapper, %function\n"
    "task_entry_wrapper:\n"
    "    mov x0, x19\n"
    "    mov x1, x20\n"
    "    b task_entry_trampoline\n"
);

/*
 * Allocate a task slot without setting up a stack.
 *
 * This is used by elf.c to create tasks with custom stack setup.
 * The caller must set up stack_base, stack_top, and context.
 */
struct task *task_alloc(const char *name, uint8_t priority)
{
    irq_flags_t flags;
    struct task *task;
    uint32_t task_id;

    /* Clamp priority to valid range */
    if (priority > TASK_PRIORITY_MAX) {
        priority = TASK_PRIORITY_MAX;
    }

    /* Acquire lock to access task_table and next_task_id */
    flags = spin_lock_irqsave(&task_lock);

    /* Find free task slot */
    task = alloc_task_slot();
    if (!task) {
        spin_unlock_irqrestore(&task_lock, flags);
        ERROR("task_alloc: no free task slots");
        return NULL;
    }

    /* Reserve task ID atomically */
    task_id = next_task_id++;

    /* Mark slot as used immediately (id != 0 means in use) */
    task->id = task_id;

    spin_unlock_irqrestore(&task_lock, flags);

    /* Initialize task structure (slot is ours now) */
    str_copy(task->name, name ? name : "unnamed", TASK_NAME_LEN);
    task->state = TASK_READY;
    task->next = NULL;
    task->cpu_affinity = CPU_AFFINITY_ANY;
    task->assigned_cpu = 0;
    task->priority = priority;
    task->effective_priority = priority;
    task->deadline_ns = 0;
    task->wake_time_ns = 0;
    task->sleep_next = NULL;
    task->switches = 0;

    /* Stack pointers left uninitialized - caller must set these */
    task->stack_base = NULL;
    task->stack_top = NULL;

    /* Zero out the context */
    for (size_t i = 0; i < sizeof(task->context); i++) {
        ((uint8_t *)&task->context)[i] = 0;
    }

    /* No cleanup callback by default */
    task->cleanup = NULL;
    task->cleanup_arg = NULL;

    DEBUG_PRINT("Allocated task '%s' (id=%u, priority=%u)",
                task->name, task->id, task->priority);

    return task;
}

/*
 * Create a new task with specified priority.
 */
struct task *task_create_with_priority(const char *name, task_entry_t entry,
                                       void *arg, uint8_t priority)
{
    irq_flags_t flags;
    struct task *task;
    uint32_t task_id;

    /* Clamp priority to valid range */
    if (priority > TASK_PRIORITY_MAX) {
        priority = TASK_PRIORITY_MAX;
    }

    /* Allocate stack first (outside lock - pmm has its own locking) */
    size_t stack_pages = STACK_SIZE / 4096;
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
    task->priority = priority;
    task->effective_priority = priority;
    task->deadline_ns = 0;                   /* No deadline by default */
    task->wake_time_ns = 0;                  /* Not sleeping */
    task->sleep_next = NULL;
    task->switches = 0;

    /* Set up stack (grows downward on ARM64) */
    task->stack_base = stack;
    task->stack_top = (void *)((uintptr_t)stack + STACK_SIZE);

    /* Initialize CPU context */
    /* Zero out the context first */
    for (size_t i = 0; i < sizeof(task->context); i++) {
        ((uint8_t *)&task->context)[i] = 0;
    }

    /* Set up initial context for first switch */
    task->context.sp = (uint64_t)task->stack_top;
    task->context.x30 = (uint64_t)task_entry_wrapper;  /* Return address */
    task->context.x29 = 0;                              /* Frame pointer */
    task->context.daif = 0x080;  /* IRQ masked (DAIF I-bit set). Critical for
                                  * correctness: context.S restores DAIF early
                                  * in the switch sequence, before GP registers
                                  * and SP are fully loaded. With DAIF=0, the
                                  * timer ISR fires mid-restore and corrupts the
                                  * partially restored context. Setting 0x080
                                  * keeps IRQs masked until the task is fully
                                  * running; task code unmasks naturally via
                                  * spin_unlock_irqrestore or explicit DAIF
                                  * clear. */

    /* Store entry point and arg in callee-saved registers for wrapper */
    task->context.x19 = (uint64_t)entry;
    task->context.x20 = (uint64_t)arg;

    /* No cleanup callback by default */
    task->cleanup = NULL;
    task->cleanup_arg = NULL;

    DEBUG_PRINT("Created task '%s' (id=%u, stack=%p-%p, priority=%u)",
                task->name, task->id, task->stack_base, task->stack_top,
                task->priority);

    return task;
}

/*
 * Create a new task with default priority.
 */
struct task *task_create(const char *name, task_entry_t entry, void *arg)
{
    return task_create_with_priority(name, entry, arg, TASK_PRIORITY_DEFAULT);
}

/*
 * Terminate the current task.
 */
void task_exit(void)
{
    struct task *task = task_current();

    INFO("Task '%s' (id=%u) exiting", task->name, task->id);

    task->state = TASK_TERMINATED;
    cache_clean(&task->state);

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
    uint32_t cpu = cpu_id();
    cache_invalidate(&current_task[cpu]);
    return current_task[cpu];
}

/*
 * Set current running task (called by scheduler).
 */
void task_set_current(struct task *task)
{
    uint32_t cpu = cpu_id();
    current_task[cpu] = task;
    cache_clean(&current_task[cpu]);
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
 *
 * This function reclaims the task's stack memory and frees the task slot
 * for reuse. Must only be called after the task has been removed from
 * all run queues and is no longer running.
 */
void task_destroy(struct task *task)
{
    if (!task) {
        return;
    }

    irq_flags_t flags = spin_lock_irqsave(&task_lock);

    /* Verify task is terminated */
    if (task->state != TASK_TERMINATED) {
        WARN("task_destroy: task '%s' not terminated (state=%d)",
             task->name, task->state);
        spin_unlock_irqrestore(&task_lock, flags);
        return;
    }

    /* Capture info before clearing */
    void *stack = task->stack_base;
    uint32_t task_id = task->id;
    char task_name[TASK_NAME_LEN];
    str_copy(task_name, task->name, TASK_NAME_LEN);
    task_cleanup_t cleanup = task->cleanup;
    void *cleanup_arg = task->cleanup_arg;

    /* Clear task slot (marks as free: id == 0) */
    task->id = 0;
    task->name[0] = '\0';
    task->stack_base = NULL;
    task->stack_top = NULL;
    task->cleanup = NULL;
    task->cleanup_arg = NULL;

    spin_unlock_irqrestore(&task_lock, flags);

    /* Call cleanup callback first (e.g., to free ELF segment memory) */
    if (cleanup) {
        cleanup(cleanup_arg);
    }

    /* Free stack outside lock - pmm has its own locking */
    if (stack) {
        size_t stack_pages = STACK_SIZE / 4096;
        pmm_free_pages(stack, stack_pages);
    }

    /* Note: DEBUG_PRINT removed here to avoid output interleaving issues
     * during test runs with concurrent task destruction across multiple CPUs.
     * Task lifecycle is well-tested; enable if debugging task issues. */
    (void)task_name;
    (void)task_id;
}

/*
 * Set task CPU affinity.
 */
void task_set_affinity(struct task *task, uint32_t cpu)
{
    if (!task) return;

    task->cpu_affinity = cpu;
    cache_clean(&task->cpu_affinity);

    /* If pinning to a specific CPU, update assigned_cpu */
    if (cpu != CPU_AFFINITY_ANY && cpu < cpu_count) {
        task->assigned_cpu = cpu;
        cache_clean(&task->assigned_cpu);
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

/*
 * Set task priority.
 */
void task_set_priority(struct task *task, uint8_t priority)
{
    if (!task) return;

    /* Clamp to valid range */
    if (priority > TASK_PRIORITY_MAX) {
        priority = TASK_PRIORITY_MAX;
    }

    task->priority = priority;
    cache_clean(&task->priority);

    /* Update effective priority (may be boosted by deadline) */
    if (task->effective_priority < priority) {
        task->effective_priority = priority;
    }
    cache_clean(&task->effective_priority);
}

/*
 * Get task priority.
 */
uint8_t task_get_priority(struct task *task)
{
    if (!task) return TASK_PRIORITY_NORMAL;
    return task->priority;
}

/*
 * Get effective task priority (includes deadline boost).
 */
uint8_t task_get_effective_priority(struct task *task)
{
    if (!task) return TASK_PRIORITY_NORMAL;
    return task->effective_priority;
}

/*
 * Set task deadline.
 */
void task_set_deadline(struct task *task, uint64_t deadline_ns)
{
    if (!task) return;
    task->deadline_ns = deadline_ns;
    cache_clean(&task->deadline_ns);
}

/*
 * Get task deadline.
 */
uint64_t task_get_deadline(struct task *task)
{
    if (!task) return 0;
    return task->deadline_ns;
}

/*
 * Set task cleanup callback.
 */
void task_set_cleanup(struct task *task, task_cleanup_t cleanup, void *cleanup_arg)
{
    if (!task) return;
    task->cleanup = cleanup;
    task->cleanup_arg = cleanup_arg;
}
