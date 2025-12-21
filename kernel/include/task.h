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

/* Task configuration */
#define TASK_STACK_SIZE     (16 * 1024)     /* 16 KB per task */
#define MAX_TASKS           32              /* Maximum concurrent tasks */
#define TASK_NAME_LEN       16              /* Max task name length */

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
};

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

    /* CPU context (saved/restored on switch) */
    struct cpu_context context;

    /* Stack */
    void *stack_base;                   /* Bottom of stack (allocation address) */
    void *stack_top;                    /* Top of stack (initial SP) */

    /* Statistics (optional, for debugging) */
    uint64_t switches;                  /* Number of times scheduled */
};

/* Task function prototype */
typedef void (*task_entry_t)(void *arg);

/*
 * Create a new task.
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

#endif /* TASK_H */
