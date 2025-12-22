# SLM-OS Scheduler

This document describes the deadline-aware scheduler implementation in SLM-OS, designed for AI inference workloads on heterogeneous ARM systems.

## Overview

The scheduler uses a **hybrid priority/deadline** approach:
- Fixed priority base (8 levels)
- Deadline boost for time-critical tasks
- Core affinity hints for big.LITTLE systems

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Scheduling Architecture                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   Rust Policy Layer (runtime/src/sched/)                            │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │  schedule_slm_task()  →  SchedulingHint                     │   │
│   │  - Deadline analysis                                         │   │
│   │  - Working set heuristics                                    │   │
│   │  - Core affinity suggestions                                 │   │
│   └─────────────────────────────────────────────────────────────┘   │
│                              │                                       │
│                              ▼ FFI                                   │
│   C Kernel Layer (kernel/src/sched.c)                               │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │  Per-CPU Run Queues (priority-ordered)                      │   │
│   │  - Priority boost based on deadline proximity               │   │
│   │  - Context switch on timer tick                             │   │
│   │  - Task migration between CPUs                              │   │
│   └─────────────────────────────────────────────────────────────┘   │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

## Priority System

### Priority Levels

SLM-OS uses 8 priority levels (0-7), where higher values indicate higher priority:

| Level | Name       | Value | Use Case                              |
|-------|------------|-------|---------------------------------------|
| 0     | IDLE       | 0     | Background tasks, idle loops          |
| 1     | -          | 1     | (reserved)                            |
| 2     | LOW        | 2     | Batch processing, non-urgent work     |
| 3     | -          | 3     | (reserved)                            |
| 4     | NORMAL     | 4     | Default priority for regular tasks    |
| 5     | -          | 5     | (reserved)                            |
| 6     | HIGH       | 6     | Important tasks, approaching deadline |
| 7     | CRITICAL   | 7     | Urgent tasks, deadline imminent       |

### Priority Fields in Task Structure

Each task has two priority fields:

```c
struct task {
    uint8_t priority;            /* Base priority (set by user) */
    uint8_t effective_priority;  /* Actual priority (may be boosted) */
    uint64_t deadline_ns;        /* Absolute deadline (0 = none) */
    ...
};
```

- `priority`: The base priority assigned when the task is created
- `effective_priority`: The actual priority used for scheduling (≥ base priority)
- `deadline_ns`: Absolute deadline in nanoseconds since boot (0 = no deadline)

## Deadline Boost

Tasks with deadlines automatically receive priority boosts as their deadline approaches:

```
Time remaining        Boost applied
─────────────────────────────────────
> 100ms               No boost (base priority)
50-100ms              +1 level
10-50ms               Boost to HIGH (6)
< 10ms or missed      Boost to CRITICAL (7)
```

### Implementation (C Kernel)

The deadline boost is calculated in `update_deadline_boost()` (sched.c:245):

```c
static void update_deadline_boost(struct task *task)
{
    if (!task || task->deadline_ns == 0) {
        task->effective_priority = task->priority;
        return;
    }

    uint64_t now = slm_get_time_ns();
    uint64_t remaining = task->deadline_ns - now;

    if (now >= task->deadline_ns) {
        task->effective_priority = TASK_PRIORITY_CRITICAL;
    } else if (remaining < DEADLINE_CRITICAL_NS) {   /* 10ms */
        task->effective_priority = TASK_PRIORITY_CRITICAL;
    } else if (remaining < DEADLINE_HIGH_NS) {       /* 50ms */
        task->effective_priority = TASK_PRIORITY_HIGH;
    } else if (remaining < DEADLINE_BOOST_NS) {      /* 100ms */
        task->effective_priority = task->priority + 1;
    } else {
        task->effective_priority = task->priority;
    }
}
```

Boost is recalculated:
- When a task is added to the run queue
- During `schedule()` before selecting the next task

## Run Queue Structure

Each CPU has its own run queue, ordered by effective priority (highest first):

```
CPU 0 Run Queue:
┌─────────────────────────────────────────────────────────────────────┐
│ HEAD → [Task A, eff_pri=7] → [Task B, eff_pri=6] → [Task C, eff_pri=4] → NULL │
└─────────────────────────────────────────────────────────────────────┘

Tasks with equal priority are ordered FIFO (first-in, first-out).
```

### Per-Queue Locking

Each CPU run queue has its own spinlock to reduce contention:

```c
struct cpu_runqueue {
    spinlock_t lock;            /* Per-queue lock */
    struct task *head;
    struct task *tail;
    struct task *idle_task;
    uint32_t ready_count;
};
```

**Lock strategy:**
- Single-queue operations (add, remove, schedule) use only that CPU's lock
- Cross-queue operations (migration) lock both queues in CPU ID order to prevent deadlock
- Global statistics (task count, context switches) are racy but acceptable for diagnostics

```c
/* Migration locks in CPU ID order to prevent deadlock */
if (old_cpu < target_cpu) {
    flags = spin_lock_irqsave(&rq_old->lock);
    spin_lock(&rq_new->lock);
} else {
    flags = spin_lock_irqsave(&rq_new->lock);
    spin_lock(&rq_old->lock);
}
```

### Core Isolation

Cores can be isolated from general scheduling for real-time or latency-sensitive workloads:

```c
int sched_isolate_core(uint32_t cpu);      /* Mark core as isolated */
int sched_unisolate_core(uint32_t cpu);    /* Return to general pool */
int sched_is_core_isolated(uint32_t cpu);  /* Check isolation status */
uint32_t sched_get_isolated_cores(void);   /* Get bitmask */
```

**Behavior:**
- CPU 0 cannot be isolated (boot CPU)
- Tasks with `CPU_AFFINITY_ANY` skip isolated cores
- Pinned tasks (explicit `cpu_affinity`) still run on isolated cores
- `find_target_cpu()` selects least-loaded non-isolated CPU

### Insertion Algorithm

When adding a task to a run queue, it is inserted at the correct position to maintain priority order:

```c
/* Find insertion point: insert before first task with LOWER priority */
struct task *prev = NULL;
struct task *curr = rq->head;

while (curr && curr->effective_priority >= task->effective_priority) {
    prev = curr;
    curr = curr->next;
}

/* Insert between prev and curr */
task->next = curr;
if (prev) {
    prev->next = task;
} else {
    rq->head = task;
}
```

## FFI Task API

The Rust runtime interacts with the C scheduler through FFI functions:

### Task Creation

```c
// C API (kernel/include/slm_ffi.h)
uint32_t slm_task_create(const char *name, slm_task_entry_t entry, void *arg);
```

Returns a task ID (non-zero on success, 0 on failure). Task IDs are validated via `task_get(id)` before use.

```rust
// Rust wrapper (runtime/src/kernel_ffi.rs)
pub fn task_create(
    name: &[u8],  // Must be null-terminated
    entry: extern "C" fn(*mut c_void),
    arg: *mut c_void,
) -> KernelResult<TaskId>
```

### Priority and Deadline

```c
// C API
int slm_task_set_priority(uint32_t task_id, uint8_t priority);
int slm_task_set_deadline(uint32_t task_id, uint64_t deadline_ns);
```

```rust
// Rust wrappers
pub fn task_set_priority(task_id: TaskId, priority: u8) -> KernelResult<()>;
pub fn task_set_deadline(task_id: TaskId, deadline_ns: u64) -> KernelResult<()>;
```

## Rust Scheduler Policy Module

The `runtime/src/sched/` module provides high-level scheduling policies for SLM inference tasks.

### Types

```rust
/// Priority levels matching C kernel
pub enum Priority {
    Idle = 0,
    Low = 2,
    Normal = 4,
    High = 6,
    Critical = 7,
}

/// Core type for heterogeneous scheduling
pub enum CoreType {
    Performance,  // Big core - compute intensive
    Efficiency,   // LITTLE core - background tasks
    Any,          // No preference
}

/// Deadline specification
pub struct TaskDeadline {
    pub deadline_ns: u64,  // 0 = no deadline
}

/// Task characteristics for scheduling
pub struct SlmTaskInfo {
    pub working_set_bytes: usize,
    pub deadline: TaskDeadline,
    pub base_priority: Priority,
}

/// Scheduling recommendation
pub struct SchedulingHint {
    pub priority: Priority,
    pub core_type: CoreType,
    pub pin_to_core: bool,
}
```

### Usage Example

```rust
use slm_runtime::sched::{schedule_slm_task, SlmTaskInfo, TaskDeadline, Priority};

// Create a task with 50ms deadline for a small model
let task_id = kernel_ffi::task_create(b"inference\0", inference_entry, arg)?;

let info = SlmTaskInfo {
    working_set_bytes: 4 * 1024 * 1024,  // 4MB model
    deadline: TaskDeadline::inference_latency_ms(50),
    base_priority: Priority::Normal,
};

let hint = schedule_slm_task(task_id, &info)?;
// hint.priority = High (boosted due to 50ms deadline)
// hint.core_type = Efficiency (small model)
```

### Core Affinity Heuristics

The `suggest_core_affinity()` function recommends core type based on:

| Working Set | Deadline      | Recommendation      |
|-------------|---------------|---------------------|
| < 8 MB      | > 50ms        | Efficiency core     |
| < 8 MB      | < 50ms        | Performance core    |
| ≥ 8 MB      | any           | Performance core    |
| any         | < 10ms        | Performance + pinned|

## Task States

```
┌─────────┐    task_create()     ┌─────────┐
│         │ ──────────────────▶  │  READY  │
│  (new)  │                      │         │
└─────────┘                      └────┬────┘
                                      │
                    ┌─────────────────┼─────────────────┐
                    │                 │                 │
                    ▼ schedule()      │                 │
              ┌─────────┐             │                 │
              │ RUNNING │ ◀───────────┘                 │
              │         │                               │
              └────┬────┘                               │
                   │                                    │
      ┌────────────┼────────────┐                       │
      │            │            │                       │
      ▼ yield()    ▼ block()    ▼ task_exit()          │
┌─────────┐  ┌─────────┐  ┌────────────┐               │
│  READY  │  │ BLOCKED │  │ TERMINATED │               │
│         │  │         │  │            │               │
└─────────┘  └────┬────┘  └────────────┘               │
      ▲           │                                    │
      │           │ unblock()                          │
      └───────────┴────────────────────────────────────┘
```

## Context Switch

Context switches occur:
1. **Timer interrupt** (100 Hz) - preemptive scheduling
2. **yield()** call - voluntary context switch
3. **block()** call - task waiting for I/O or synchronization

The context switch saves/restores:
- Callee-saved registers (x19-x30)
- Stack pointer (SP)
- FPU/SIMD registers (v0-v31, fpcr, fpsr) - eager save

## Configuration

### Compile-Time Constants

```c
// kernel/include/task.h
#define TASK_STACK_SIZE     (16 * 1024)   // 16 KB per task
#define MAX_TASKS           32            // Maximum concurrent tasks
#define TASK_NAME_LEN       16            // Max task name length

// kernel/src/sched.c
#define DEADLINE_CRITICAL_NS  (10 * 1000000ULL)   // 10ms
#define DEADLINE_HIGH_NS      (50 * 1000000ULL)   // 50ms
#define DEADLINE_BOOST_NS     (100 * 1000000ULL)  // 100ms
```

### Runtime Configuration

Timer frequency is set in `timer.c`:
```c
#define TIMER_HZ 100  // 100 Hz = 10ms tick
```

## Future Work

### Milestone 2 Remaining
- [ ] Load balancing across cores based on deadline pressure
- [ ] Priority inversion prevention (priority inheritance)
- [ ] Benchmark per-queue locks vs global lock

### Milestone 4+ (big.LITTLE)
- [ ] Actual core assignment based on CoreType hints
- [ ] CPU frequency scaling integration
- [ ] GIC affinity routing for isolated cores

---

*Last updated: December 2025*
