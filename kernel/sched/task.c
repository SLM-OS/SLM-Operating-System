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
#include "ncmem.h"
#include "arch.h"
#include <stddef.h>

/* Task table - NC on Pi 5, BSS fallback otherwise */
static struct task *task_table;
static struct task task_table_fallback[MAX_TASKS];
static uint32_t next_task_id = 1;       /* ID 0 reserved for idle task */

/* Lock protecting task_table and next_task_id.
 * On Pi 5, standard ldaxr/stxr spinlocks fail under cross-CPU contention
 * (L2 retains stale lock values). Use atomic test-and-set instead. */
#if defined(PLATFORM_HAS_NC_MEMORY)
/* Use standard spinlock but add DC CIVAC after release to push
 * "unlocked" state through L2 to DRAM. Secondary CPUs' first lock
 * attempt reads from DRAM (cold L2) and sees "unlocked". */
static spinlock_t task_lock __attribute__((aligned(64))) = SPINLOCK_INIT;
#define TASK_LOCK_IRQSAVE() \
    irq_flags_t _task_flags = spin_lock_irqsave(&task_lock)
#define TASK_UNLOCK_IRQRESTORE() \
    do { \
        spin_unlock_irqrestore(&task_lock, _task_flags); \
        __asm__ volatile("dc civac, %0" :: "r"(&task_lock) : "memory"); \
        __asm__ volatile("dsb sy" ::: "memory"); \
    } while(0)
#else
static spinlock_t task_lock = SPINLOCK_INIT;
#define TASK_LOCK_IRQSAVE() \
    irq_flags_t _task_flags = spin_lock_irqsave(&task_lock)
#define TASK_UNLOCK_IRQRESTORE() \
    spin_unlock_irqrestore(&task_lock, _task_flags)
#endif

/* Per-CPU current running task (set by scheduler) */
static struct task *current_task[MAX_CPUS];

void task_table_init(void)
{
#if defined(PLATFORM_HAS_NC_MEMORY)
    task_table = ncmem_alloc(MAX_TASKS * sizeof(struct task), CACHE_LINE_SIZE);
    if (!task_table) {
        task_table = task_table_fallback;
        return;
    }
    volatile uint8_t *p = (volatile uint8_t *)task_table;
    for (size_t i = 0; i < MAX_TASKS * sizeof(struct task); i++)
        p[i] = 0;
#else
    task_table = task_table_fallback;
#endif
}

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

/* Forward declaration — defined in assembly (context.S for x86-64, inline below for ARM64) */
extern void task_entry_wrapper(void);

/*
 * C trampoline called from assembly task_entry_wrapper.
 * Callee-saved registers hold the entry point and argument.
 */
void task_entry_trampoline(uint64_t entry_addr, uint64_t arg_addr)
{
    task_entry_t entry = (task_entry_t)entry_addr;
    void *arg = (void *)arg_addr;

    /* NC debug marker: 0xDD = trampoline reached.
     * If this appears in diagnostics, context.S restore completed. */
#if defined(PLATFORM_HAS_NC_MEMORY)
    {
        uint64_t _mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(_mpidr));
        /* Pi 5: CPU index in Aff1 (bits[15:8]), QEMU: Aff0 (bits[7:0]).
         * OR gives correct index when only one field is non-zero. */
        uint32_t _cpu = (_mpidr & 0xFF) | ((_mpidr >> 8) & 0xFF);
        *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + _cpu * 4) = 0xDD;
    }
#endif

    /* Clear preempt_disabled for this CPU.
     * scheduler_start() sets it to 1 before switch_to(NULL, first),
     * but switch_to never returns — it jumps here.
     *
     * Note: On Pi 5, tasks run with IRQs masked (DAIF.I=1). Timer
     * preemption is not enabled — scheduling is cooperative via yield().
     * pit_ticks is advanced by the idle task which unmasks IRQs in its
     * loop. On QEMU, timer IRQs fire regardless of DAIF. */
#if !defined(PLATFORM_X86_64)
    {
        extern volatile int preempt_disabled[];
        uint64_t mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        uint32_t hw_cpu = (mpidr & 0xFF) | ((mpidr >> 8) & 0xFF);
        if (hw_cpu < MAX_CPUS)
            preempt_disabled[hw_cpu] = 0;
        __asm__ volatile("dsb sy" ::: "memory");
    }
#else
    {
        extern volatile int preempt_disabled[];
        preempt_disabled[cpu_id()] = 0;
        __asm__ volatile("mfence" ::: "memory");
    }
#endif

    entry(arg);
    task_exit();
}

#if !defined(PLATFORM_X86_64)
/*
 * ARM64 task entry wrapper (inline assembly).
 *
 * When switch_to restores a new task, x19 = entry, x20 = arg.
 * Passes them to task_entry_trampoline as function arguments.
 */
__asm__(
    ".global task_entry_wrapper\n"
    ".type task_entry_wrapper, %function\n"
    "task_entry_wrapper:\n"
    "    mov x0, x19\n"
    "    mov x1, x20\n"
    "    b task_entry_trampoline\n"
);
#endif /* !PLATFORM_X86_64 — x86-64 wrapper is in context.S */

/*
 * Allocate a task slot without setting up a stack.
 *
 * This is used by elf.c to create tasks with custom stack setup.
 * The caller must set up stack_base, stack_top, and context.
 */
struct task *task_alloc(const char *name, uint8_t priority)
{
    struct task *task;
    uint32_t task_id;

    /* Clamp priority to valid range */
    if (priority > TASK_PRIORITY_MAX) {
        priority = TASK_PRIORITY_MAX;
    }

    /* Acquire lock to access task_table and next_task_id */
    TASK_LOCK_IRQSAVE();

    /* Find free task slot */
    task = alloc_task_slot();
    if (!task) {
        TASK_UNLOCK_IRQRESTORE();
        ERROR("task_alloc: no free task slots");
        return NULL;
    }

    /* Reserve task ID atomically */
    task_id = next_task_id++;

    /* Mark slot as used immediately (id != 0 means in use) */
    task->id = task_id;

    TASK_UNLOCK_IRQRESTORE();

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
    TASK_LOCK_IRQSAVE();

    /* Find free task slot */
    task = alloc_task_slot();
    if (!task) {
        TASK_UNLOCK_IRQRESTORE();
        pmm_free_pages(stack, stack_pages);
        ERROR("task_create: no free task slots");
        return NULL;
    }

    /* Reserve task ID atomically */
    task_id = next_task_id++;

    /* Mark slot as used immediately (id != 0 means in use) */
    task->id = task_id;

    TASK_UNLOCK_IRQRESTORE();

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
#if defined(PLATFORM_X86_64)
    task->context.rsp = (uint64_t)task->stack_top;
    task->context.rip = (uint64_t)task_entry_wrapper;
    task->context.rbp = 0;                              /* Frame pointer */
    task->context.rflags = 0;                           /* IF=0: interrupts disabled */
    task->context.rbx = (uint64_t)entry;                /* Entry function */
    task->context.r12 = (uint64_t)arg;                  /* Argument */
#else
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
#endif /* PLATFORM_X86_64 */

    /* No cleanup callback by default */
    task->cleanup = NULL;
    task->cleanup_arg = NULL;

    /* Clean the context struct to PoC so a secondary CPU can read it
     * during switch_to(). Without SMPEN, task_create's writes to
     * context.sp, context.x30, etc. stay in this CPU's L1 cache.
     * scheduler_add_task_to_cpu() intentionally skips cleaning the
     * context (to avoid overwriting a running task's live state),
     * so we must clean it here at creation time. */
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean_range(&task->context, sizeof(task->context));
#endif

    /* Skip DEBUG_PRINT on secondary CPUs — uart_lock contention with
     * CPU 0's boot output causes deadlock/hang on real hardware. */
#if defined(PLATFORM_HAS_NC_MEMORY)
    {
        uint64_t _mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(_mpidr));
        if (cpu_logical_id(_mpidr) == 0) {
            DEBUG_PRINT("Created task '%s' (id=%u, stack=%p-%p, priority=%u)",
                        task->name, task->id, task->stack_base, task->stack_top,
                        task->priority);
        }
    }
#else
    DEBUG_PRINT("Created task '%s' (id=%u, stack=%p-%p, priority=%u)",
                task->name, task->id, task->stack_base, task->stack_top,
                task->priority);
#endif

    return task;
}

/*
 * Create a new task with default priority.
 */
struct task *task_create(const char *name, task_entry_t entry, void *arg)
{
    return task_create_with_priority(name, entry, arg, TASK_PRIORITY_DEFAULT);
}

#if !defined(PLATFORM_X86_64)
/*
 * Assembly trampoline that transitions from EL1 to EL0 via ERET.
 * Defined in user_entry.S.
 */
extern void user_task_enter(void *entry, void *stack_top, void *arg);

/*
 * Kernel-mode wrapper for user tasks.
 *
 * When switch_to() restores this task for the first time, x19=entry, x20=arg.
 * task_entry_trampoline calls this function, which then ERETsm into EL0.
 *
 * Note: At this point we are at EL1 with interrupts masked. user_task_enter
 * sets SPSR to EL0t with interrupts enabled, so ERET unmasks them.
 */
static void user_task_wrapper(void *arg)
{
    struct task *t = task_current();
    if (!t || !t->user_entry) {
        task_exit();
        return;
    }

    /* ERET to EL0 — does not return */
    user_task_enter((void *)(uintptr_t)t->user_entry, t->stack_top, arg);

    /* Should never reach here */
    task_exit();
}

/*
 * Create a new user-mode (EL0) task.
 *
 * The task starts in kernel mode (via task_entry_wrapper) then
 * transitions to EL0 via ERET. Syscalls (SVC #0) return to EL1.
 */
struct task *task_create_user(const char *name, task_entry_t user_entry,
                              void *arg, uint8_t priority)
{
    /* Create a kernel task that runs the user_task_wrapper */
    struct task *task = task_create_with_priority(name, user_task_wrapper,
                                                   arg, priority);
    if (!task) return NULL;

    /* Mark as user-mode and store the real EL0 entry point */
    task->is_user = 1;
    task->user_entry = user_entry;

    return task;
}
#endif /* !PLATFORM_X86_64 */

/*
 * Terminate the current task.
 */
void task_exit(void)
{
    struct task *task = task_current();

    INFO("Task '%s' (id=%u) exiting", task->name, task->id);

    /* Mask IRQs to prevent a timer-driven schedule() from racing with
     * the state change below. Without this, the timer can fire between
     * setting TASK_TERMINATED and scheduler_remove_task(), causing
     * schedule() to find a terminated task still in the run queue
     * (pick_next_task returns it, next == current → panic). */
    arch_irq_disable();

#ifdef CONFIG_AI_SCHEDULER
    {
        extern void sched_ai_record_completion(struct task *task);
        sched_ai_record_completion(task);
    }
#endif

    task->state = TASK_TERMINATED;
#if !defined(PLATFORM_HAS_NC_MEMORY) && !defined(PLATFORM_X86_64)
    cache_clean(&task->state);
#endif

    /* Remove from run queue and schedule next task.
     * schedule() -> spin_lock_irqsave saves our masked DAIF state.
     * The context switch to the next task restores that task's DAIF,
     * which will have IRQs unmasked. */
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

    TASK_LOCK_IRQSAVE();

    /* Verify task is terminated */
    if (task->state != TASK_TERMINATED) {
        WARN("task_destroy: task '%s' not terminated (state=%d)",
             task->name, task->state);
        TASK_UNLOCK_IRQRESTORE();
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

    TASK_UNLOCK_IRQRESTORE();

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
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->cpu_affinity);
#endif

    /* If pinning to a specific CPU, update assigned_cpu */
    if (cpu != CPU_AFFINITY_ANY && cpu < cpu_count) {
        task->assigned_cpu = cpu;
#if !defined(PLATFORM_HAS_NC_MEMORY)
        cache_clean(&task->assigned_cpu);
#endif
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
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->priority);
#endif

    /* Update effective priority (may be boosted by deadline) */
    if (task->effective_priority < priority) {
        task->effective_priority = priority;
    }
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->effective_priority);
#endif
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
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->deadline_ns);
#endif
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
