/*
 * task.h - Task management for SLM-OS
 *
 * Defines the task control block and task-related operations.
 */

#ifndef TASK_H
#define TASK_H

#include <stdalign.h>
#include <stdbool.h>
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

/* Task flags (struct task::flags). Set once at task creation; never
 * cleared. Currently only TASK_FLAG_IDLE is defined (#200, 2026-05-03). */
#define TASK_FLAG_IDLE          (1u << 0)   /* per-CPU perpetual idle task */

/* Task states */
typedef enum {
    TASK_READY,         /* Ready to run, in run queue */
    TASK_RUNNING,       /* Currently executing on CPU */
    TASK_BLOCKED,       /* Waiting for something (I/O, sleep, etc.) */
    TASK_TERMINATED,    /* Finished execution, awaiting cleanup */
    TASK_DESTROYED      /* Reclamation in progress; must not be picked */
} task_state_t;

#if defined(PLATFORM_X86_64)
/*
 * x86-64 CPU context saved during context switch.
 *
 * System V AMD64 ABI callee-saved registers:
 *   rbx, rbp, r12, r13, r14, r15
 * Plus rsp (stack pointer) and rip (saved return address).
 *
 * New tasks have rip = task_entry_wrapper, rbx = entry, r12 = arg.
 * rflags is saved to preserve the interrupt flag (IF) state.
 */
struct cpu_context {
    uint64_t rbx;       /* 0x00 */
    uint64_t rbp;       /* 0x08 */
    uint64_t r12;       /* 0x10 */
    uint64_t r13;       /* 0x18 */
    uint64_t r14;       /* 0x20 */
    uint64_t r15;       /* 0x28 */
    uint64_t rsp;       /* 0x30 */
    uint64_t rip;       /* 0x38 */
    uint64_t rflags;    /* 0x40 */
    uint64_t _pad0;     /* 0x48 — align fxsave to 16 bytes */
    /* 512-byte FXSAVE area (D2 / P1-6). Saves x87, MMX, and XMM
     * state across context switches so multiple concurrent tasks
     * can safely use SSE — required as soon as Phase C's inference
     * SIMD kernels are invoked from more than one task. The
     * alignas(16) makes the whole cpu_context struct 16-byte aligned
     * so the fxsave/fxrstor instructions don't #GP. Offsets above
     * stay unchanged so kernel/arch/x86_64/context.S's CTX_* defines
     * don't shift. */
    alignas(16) uint8_t fxsave[512];  /* 0x50 */
};

#else /* ARM64 */
/*
 * ARM64 CPU context saved during context switch.
 *
 * Callee-saved: x19-x28, x29 (fp), x30 (lr), sp.
 * FPU/SIMD: v0-v31, fpcr, fpsr (eager save for SLM workloads).
 * Interrupt state: DAIF register.
 *
 * Task-DAIF invariant (Pi 5 / Jetson):
 *   New tasks are created with DAIF.I=1 (IRQ masked). See task.c where
 *   task->context.daif is initialized to 0x080, and context.S where DAIF
 *   is restored early in the switch sequence. This invariant is platform-
 *   required: on real hardware without SMPEN, a timer IRQ taken mid-
 *   context-restore corrupts the partially-restored task state. On QEMU
 *   the same masking applies for uniformity.
 *   Rationale: docs/smp.md and kernel/CLAUDE.md §Pi-5-Timer-IRQs.
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

/* Pin offsets that kernel/arch/arm64/context.S uses as `CTX_DAIF`
 * literals. If a future field is appended without updating the
 * matching .S defines, the assembly would read garbage on restore.
 * Crash early at compile time instead. */
_Static_assert(offsetof(struct cpu_context, daif) == 0x280,
               "CTX_DAIF in arm64/context.S expects daif at offset 0x280");
#endif /* PLATFORM_X86_64 */

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

    /* User mode support (Phase 5 M4) */
    uint8_t is_user;                    /* 1 if runs at EL0, 0 for kernel EL1 */

    /* Task-flag bits (#200 investigation, 2026-05-03). Currently
     * only TASK_FLAG_IDLE is defined; set once at idle creation in
     * `scheduler_init` / `scheduler_init_secondary`. Used by
     * `is_idle_task(t)` for O(1) guards in state-mutation paths
     * (terminate, destroy, kill) — replaces the old loop-over-cpu_rq
     * pointer comparison. Pattern ported from Linux's PF_IDLE
     * (`include/linux/sched.h:PF_IDLE`).
     *
     * Flag bits are ORed in; never cleared. Adding a new bit is
     * additive and won't affect existing callers. */
    uint8_t flags;                      /* TASK_FLAG_* bits */
    uint8_t _user_pad[6];              /* Alignment padding */
    void (*user_entry)(void *arg);      /* EL0 entry point (for user tasks) */

    /* Per-task TTBR0_EL1 L1 table (#697 PR-3 scaffolding).
     *
     * Physical address of this task's L1 table for the lower VA half.
     * Populated by task_create_user (PR-3); written to TTBR0_EL1 on
     * context-switch into a user task (PR-3); freed by task_destroy
     * (PR-3). Set to 0 for kernel-mode tasks.
     *
     * Stored as PA (not VA) because the page-table walker does PA
     * lookups and because the L1 table is allocated from PMM (which
     * returns PAs). Value 0 = "no per-task user mapping; do not write
     * TTBR0 on switch_to". Allocated unused in PR-1 so PR-3 can
     * populate without changing the struct layout / TASK_CONTEXT_OFFSET
     * invariants. */
    uint64_t user_l1_pa;

    /* Per-task EL0 stack (#697 PR-4).
     *
     * `user_stack_top` is the user VA passed to user_task_enter as
     * SP_EL0 (one past the last addressable byte of the stack page —
     * standard ARM64 SP convention). `user_stack_phys` is the PA of
     * the PMM page backing the stack, captured so task_destroy can
     * free it without re-walking the per-task L1.
     *
     * Both fields are zero for kernel-mode tasks. */
    uint64_t user_stack_top;
    uint64_t user_stack_phys;

    /* sys_mmap bump-allocator cursor. Starts at USER_MMAP_VA_START on
     * task_create_user; each sys_mmap advances by the requested size
     * (rounded up to page size). The allocator never reuses VA, so
     * a freshly-allocated range cannot alias stale TLB entries from
     * a prior munmap. Zero for kernel-mode tasks. */
    uint64_t user_va_next;

    /* Slot generation counter for work-stealing ABA avoidance (#139).
     *
     * Bumped by task_destroy each time this task_table slot is freed,
     * so the same `struct task *` pointer reused by task_create after
     * recycle has a different generation value. steal_deque_t captures
     * this at push time; sched_try_steal re-reads and compares under
     * the victim's rq_lock — mismatch means the captured entry refers
     * to a prior logical task (now gone) and must be discarded.
     *
     * Placed after context to preserve TASK_CONTEXT_OFFSET. */
    uint32_t generation;
    uint32_t _gen_pad;                  /* Alignment padding for next field */

#ifdef CONFIG_AI_SCHEDULER
    /* AI scheduler tracking (M5) */
    uint64_t arrival_time_ns;           /* When task was added to scheduler */
    uint64_t completion_time_ns;        /* When task exited (0 if still running) */

    /* Last AI-scheduler action recorded for this task (#211).
     *
     * Encoded action index: core * AI_ACTIONS_PER_CORE +
     * priority_adj * AI_SCHED_PREEMPT_OPTS + preempt. -1 means the
     * AI policy has not run on this task yet (or the last decision
     * fell back to the heuristic). ai_decode_action() in ai_types.h
     * splits the integer back into the three decision components.
     *
     * Placed at the end of the struct so enabling CONFIG_AI_SCHEDULER
     * does not shift TASK_CONTEXT_OFFSET for non-AI builds. */
    int32_t last_ai_action;             /* -1 when unset */
    uint32_t _ai_pad;                   /* alignment */
#endif
};

/* Task function prototype */
typedef void (*task_entry_t)(void *arg);

/* ===== Stack canary diagnostic (#601 Bug B) =====
 *
 * 64-byte magic pattern at the LOW address of every task's stack
 * (i.e., at stack_base, the boundary stack overflow would smash
 * first since ARM64/x86 stacks grow DOWN). A wild pointer that
 * writes to the bottom of any task's stack will leave a recognizable
 * pattern in the corruption — comparing the actual contents to the
 * expected pattern lets us detect both stack overflow AND wild
 * cross-task writes.
 *
 * Diagnostic only: this is observation infrastructure, not a fix.
 * Once Bug B is root-caused we can decide whether to keep it. */
#define TASK_STACK_CANARY_BYTES   64u
#define TASK_STACK_CANARY_PATTERN 0xDEADBEEFCAFEBABEULL

/* Initialize the canary at stack bottom. Called from
 * `task_create_with_priority` (the standard path) and from
 * `elf_create_task_with_args` (the ELF-loader path that bypasses
 * task_create). Any future task-creation path that assigns
 * stack_base directly must also call this so cmd_canary and the
 * panic-time inventory cover those tasks too. */
void task_canary_init(struct task *task);

/* Verify a single task's canary. Returns 0 if intact, 1 if broken.
 * On corruption logs the corrupted bytes with their offset within
 * the canary region. Uses the locked uart_printf path; safe from
 * normal task context but will deadlock if called from a panic
 * handler with the UART lock held. The panic-safe equivalent is
 * baked into `task_canary_check_all_unlocked`. */
int task_canary_check(struct task *task);

/* Iterate all live tasks and check each canary. Returns the number
 * of broken canaries found (0 = all intact). Logs each break.
 *
 * Uses the locked uart_printf path — safe from normal task context
 * (e.g., the `canary` shell command). Do NOT call from a panic
 * handler with locks unsafe; use `task_canary_check_all_unlocked`
 * instead. */
int task_canary_check_all(void);

/* Same as task_canary_check_all but uses uart_printf_unlocked, so
 * it's safe to call from contexts where the UART lock cannot be
 * held — specifically the panic handler, where IRQs are off and
 * locks are presumed corrupted. The output may interleave with
 * concurrent log output from other CPUs, so callers from normal
 * task context should prefer the locked variant above. */
int task_canary_check_all_unlocked(void);

/*
 * is_idle_task — return true if `t` is a per-CPU idle task.
 *
 * Used as an O(1) guard in state-mutation paths (terminate, destroy,
 * kill) to refuse operations that would corrupt a CPU's perpetual
 * idle. The flag is set once at idle creation and never cleared.
 *
 * Pattern ported from Linux's `is_idle_task` in
 * `include/linux/sched.h` (uses PF_IDLE there). NULL-safe for
 * callers that may not have validated the pointer.
 */
static inline bool is_idle_task(const struct task *t)
{
    return t && (t->flags & TASK_FLAG_IDLE);
}

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
 * Get the currently running task for a specific CPU.
 *
 * Returns NULL if the CPU index is out of range or no task is current.
 */
struct task *task_current_on_cpu(uint32_t cpu);

/*
 * Create a user-mode (EL0) task.
 *
 * The task transitions from EL1 to EL0 via ERET on first schedule.
 * Syscalls (SVC #0) trap back to EL1. Faults terminate the task.
 *
 * ARM64 only (x86-64 not yet supported).
 */
#if !defined(PLATFORM_X86_64)
struct task *task_create_user(const char *name, task_entry_t user_entry,
                              void *arg, uint8_t priority);

/*
 * Create a new user-mode (EL0) task from a static ARM64 ELF blob.
 *
 * Parses the ELF, allocates a per-task L1, maps every PT_LOAD
 * segment via elf_load_user, allocates + maps a stack page at
 * USER_ELF_STACK_PAGE_VA, and registers a kernel-side task that
 * will ERET into the ELF's entry point on first schedule.
 *
 * Sibling of task_create_user — same is_user/per-task-L1 plumbing,
 * different VA layout (the embedded smoke binary lives at
 * USER_TEXT_VA + 1 page; ELF segments span multiple pages and need
 * the high stack VA so they don't collide).
 *
 * @name:     Task name (for debugging / shell output).
 * @blob:     Pointer to the ELF image (typically a kernel-VA pointer
 *            into an .incbin'd blob).
 * @blob_len: Size of the ELF image.
 * @priority: Scheduler priority.
 *
 * Returns the task pointer on success, NULL on failure (invalid
 * ELF, PMM exhausted, task table full, etc.). Failure paths free
 * any partial state — no leak. The caller adds the task to the
 * scheduler via scheduler_add_task.
 */
struct task *task_create_user_elf(const char *name,
                                  const void *blob, size_t blob_len,
                                  uint8_t priority);
#endif

/*
 * Get task by ID.
 *
 * Returns: Pointer to task, or NULL if not found.
 */
struct task *task_get(uint32_t id);

/*
 * Get task by table slot index (0..MAX_TASKS-1).
 *
 * Unlike task_get(), which searches by the monotonically-increasing
 * task ID, task_slot() returns the i-th entry of the task table
 * directly. Use this when iterating over all live tasks — task IDs
 * are NOT bounded by MAX_TASKS (they come from next_task_id), so
 * iterating with task_get(0..MAX_TASKS-1) misses any task whose ID
 * has grown past MAX_TASKS.
 *
 * Returns the raw slot pointer. Callers MUST check t->id == 0 to skip
 * empty slots — task_destroy() zeros the id field as its free-slot
 * marker (task.c "Clear task slot (marks as free: id == 0)"). The
 * t->state field is NOT reset on destroy (it stays TASK_TERMINATED)
 * so a state check alone is insufficient; id == 0 is authoritative.
 *
 * Returns NULL only if idx is out of range.
 */
struct task *task_slot(uint32_t idx);

/*
 * Sleep the current task for the given number of milliseconds (#319).
 *
 * Unlike the earlier busy-wait sleep_ms, this blocks the calling task
 * via TASK_BLOCKED + scheduler yield — the task is removed from the
 * run queue and another ready task (or the idle task, which can wfi
 * on CPU 0) runs while it sleeps. The task is woken by the next
 * scheduler_tick whose CNTPCT ≥ the computed deadline.
 *
 * Preconditions:
 *
 *   - Must be called from task context (NOT from an ISR).
 *   - IRQs must be enabled. Do NOT invoke from inside an
 *     interrupt-masked critical section; spin_unlock_irqrestore
 *     would restore IRQs as disabled and schedule() would then run
 *     with IRQs off, preventing the coop-preempt tick that drives
 *     task_wake_sleepers on Pi 5 / Jetson and leading to a silent
 *     hang on those platforms.
 *   - The task must not be holding a spinlock — the scheduler yield
 *     inside this call invalidates that invariant.
 *
 * ms == 0 returns immediately.
 */
void task_sleep_ms(uint32_t ms);

/*
 * Wake any tasks whose sleep deadline has expired.
 *
 * Called from scheduler_tick(). Walks the sleep queue, moves every
 * task whose CNTPCT deadline is past back to TASK_READY and onto its
 * assigned CPU's run queue. Safe to call at any time; O(n) in the
 * number of sleeping tasks.
 *
 * Renamed from the no-op stub in drivers/timer.c. Backing
 * implementation lives in kernel/sched/task_sleep.c (#319).
 */
void task_wake_sleepers(void);

/*
 * Initialize sleep-queue state. Called once from scheduler_init.
 */
void task_sleep_init(void);

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
