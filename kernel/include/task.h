/*
 * task.h - Task management for SLM-OS
 *
 * Defines the task control block and task-related operations.
 */

#ifndef TASK_H
#define TASK_H

#include <stdalign.h>
#include <stdint.h>
#include <stddef.h>
#include "config.h"

/* CPU affinity constants */
#define CPU_AFFINITY_ANY    ((uint32_t)-1)  /* Task can run on any CPU */

/* Priority levels (0-7, higher = more important) */
#define TASK_PRIORITY_IDLE      0           /* Background/idle tasks */
#define TASK_PRIORITY_LOW       2           /* Low priority */
#define TASK_PRIORITY_NORMAL    4           /* Default priority */
#define TASK_PRIORITY_HIGH      6           /* High priority */
#define TASK_PRIORITY_CRITICAL  7           /* Highest priority */
#define TASK_PRIORITY_DEFAULT   TASK_PRIORITY_NORMAL
#define TASK_PRIORITY_MAX       7
#define TASK_PRIORITY_MIN       0

/* Task states */
typedef enum {
    TASK_READY,         /* Ready to run, in run queue */
    TASK_RUNNING,       /* Currently executing on CPU */
    TASK_BLOCKED,       /* Waiting for something (I/O, sleep, etc.) */
    TASK_TERMINATED     /* Finished execution, awaiting cleanup */
} task_state_t;

/*
 * CPU context saved during context switch.
 *
 * ARM64 calling convention:
 *   - x0-x7:   Arguments/results (caller-saved)
 *   - x8:      Indirect result (caller-saved)
 *   - x9-x15:  Temporary (caller-saved)
 *   - x16-x17: Intra-procedure-call (caller-saved)
 *   - x18:     Platform register (reserved)
 *   - x19-x28: Callee-saved (we must preserve these)
 *   - x29:     Frame pointer (callee-saved)
 *   - x30:     Link register (return address)
 *   - sp:      Stack pointer
 *
 * For voluntary context switch (yield), we only need callee-saved registers.
 * For preemptive switch (interrupt), we save everything in the exception handler.
 *
 * FPU/SIMD registers (eager save for SLM workloads):
 *   - v0-v31:  128-bit SIMD registers (512 bytes total)
 *   - fpcr:    Floating-point control register
 *   - fpsr:    Floating-point status register
 */
struct cpu_context {
    /* Callee-saved general purpose registers */
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;       /* Frame pointer */
    uint64_t x30;       /* Link register (return address) */
    uint64_t sp;        /* Stack pointer */

    /* FPU/SIMD state - eager save for SLM workloads */
    alignas(16) __uint128_t v[32];  /* V0-V31 SIMD registers */
    uint64_t fpcr;      /* Floating-point control register */
    uint64_t fpsr;      /* Floating-point status register */

    /* Interrupt state — preserved across context switches */
    uint64_t daif;      /* DAIF register (interrupt mask state) */
};

/* Task cleanup callback (called when task is destroyed) */
typedef void (*task_cleanup_t)(void *cleanup_arg);

/*
 * Task Control Block (TCB)
 *
 * Contains all per-task state needed by the scheduler.
 */
struct task {
    /* Task identification */
    uint32_t id;                        /* Unique task ID */
    char name[TASK_NAME_LEN];           /* Human-readable name */

    /* Scheduling state */
    task_state_t state;                 /* Current task state */
    struct task *next;                  /* Next task in queue (run queue or wait queue) */

    /* CPU context (saved/restored on switch) - MUST be at offset 0x20 for context.S */
    struct cpu_context context;

    /* Stack */
    void *stack_base;                   /* Bottom of stack (allocation address) */
    void *stack_top;                    /* Top of stack (initial SP) */

    /* CPU affinity (placed after context to preserve context offset) */
    uint32_t cpu_affinity;              /* CPU this task must run on, or CPU_AFFINITY_ANY */
    uint32_t assigned_cpu;              /* CPU this task is currently assigned to */

    /* Priority scheduling */
    uint8_t priority;                   /* 0-7, higher = more important */
    uint8_t effective_priority;         /* Actual priority (may be boosted) */
    uint8_t _priority_pad[2];           /* Padding for alignment */

    /* Deadline scheduling */
    uint64_t deadline_ns;               /* Absolute deadline (0 = no deadline) */

    /* Sleep support */
    uint64_t wake_time_ns;              /* Absolute wake time (0 = not sleeping) */
    struct task *sleep_next;            /* Next task in sleep queue */

    /* Statistics (optional, for debugging) */
    uint64_t switches;                  /* Number of times scheduled */

    /* Cleanup callback (called when task is destroyed) */
    task_cleanup_t cleanup;             /* Optional cleanup function */
    void *cleanup_arg;                  /* Argument passed to cleanup function */
};

/* Task function prototype */
typedef void (*task_entry_t)(void *arg);

/*
 * Create a new task with specified priority.
 *
 * @name:     Human-readable task name (truncated to TASK_NAME_LEN-1)
 * @entry:    Task entry point function
 * @arg:      Argument passed to entry function
 * @priority: Priority level (0-7, use TASK_PRIORITY_* constants)
 *
 * Returns: Pointer to new task, or NULL on failure.
 */
struct task *task_create_with_priority(const char *name, task_entry_t entry,
                                       void *arg, uint8_t priority);

/*
 * Create a new task with default priority (TASK_PRIORITY_NORMAL).
 *
 * @name:  Human-readable task name (truncated to TASK_NAME_LEN-1)
 * @entry: Task entry point function
 * @arg:   Argument passed to entry function
 *
 * Returns: Pointer to new task, or NULL on failure.
 */
struct task *task_create(const char *name, task_entry_t entry, void *arg);

/*
 * Terminate the current task.
 *
 * Marks task as TERMINATED and yields to scheduler.
 * Does not return.
 */
void task_exit(void);

/*
 * Get the currently running task.
 */
struct task *task_current(void);

/*
 * Get task by ID.
 *
 * Returns: Pointer to task, or NULL if not found.
 */
struct task *task_get(uint32_t id);

/*
 * Set task CPU affinity.
 *
 * @task:     Task to modify
 * @cpu:      CPU ID to pin to, or CPU_AFFINITY_ANY for any CPU
 *
 * Note: If task is currently running on a different CPU, it will
 * be migrated on its next scheduling event.
 */
void task_set_affinity(struct task *task, uint32_t cpu);

/*
 * Get task CPU affinity.
 *
 * @task: Task to query
 *
 * Returns: CPU ID or CPU_AFFINITY_ANY
 */
uint32_t task_get_affinity(struct task *task);

/*
 * Set task priority.
 *
 * @task:     Task to modify
 * @priority: Priority level (0-7, higher = more important)
 *
 * Note: Priority is clamped to valid range.
 */
void task_set_priority(struct task *task, uint8_t priority);

/*
 * Get task priority.
 *
 * @task: Task to query
 *
 * Returns: Priority level (0-7)
 */
uint8_t task_get_priority(struct task *task);

/*
 * Get effective task priority (includes deadline boost).
 *
 * @task: Task to query
 *
 * Returns: Effective priority level (0-7)
 */
uint8_t task_get_effective_priority(struct task *task);

/*
 * Set task deadline.
 *
 * @task:        Task to modify
 * @deadline_ns: Absolute deadline in nanoseconds (0 = no deadline)
 */
void task_set_deadline(struct task *task, uint64_t deadline_ns);

/*
 * Get task deadline.
 *
 * @task: Task to query
 *
 * Returns: Deadline in nanoseconds (0 = no deadline)
 */
uint64_t task_get_deadline(struct task *task);

/*
 * Destroy a terminated task and free its resources.
 *
 * @task: Task to destroy (must be in TERMINATED state)
 *
 * Note: The task must have been removed from the scheduler first.
 *       If a cleanup callback is set, it will be called before
 *       freeing the task's stack.
 */
void task_destroy(struct task *task);

/*
 * Set task cleanup callback.
 *
 * The cleanup function will be called when the task is destroyed.
 * This is useful for freeing resources associated with the task,
 * such as ELF segment memory.
 *
 * @task:        Task to modify
 * @cleanup:     Cleanup function (or NULL to clear)
 * @cleanup_arg: Argument passed to cleanup function
 */
void task_set_cleanup(struct task *task, task_cleanup_t cleanup, void *cleanup_arg);

#endif /* TASK_H */
