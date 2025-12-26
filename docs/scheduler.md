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
- SPIs (Shared Peripheral Interrupts) are routed away from isolated cores
- Timer IRQs (PPIs) are unaffected — each CPU keeps its timer for preemption

### Deadline-Aware Load Balancing

When placing tasks, the scheduler considers both queue depth and deadline pressure:

```c
/*
 * Deadline pressure scoring:
 *   - No deadline: 0 points
 *   - Deadline > 100ms: 1 point
 *   - Deadline 50-100ms: 2 points
 *   - Deadline 10-50ms: 4 points
 *   - Deadline < 10ms or missed: 8 points
 */
static uint32_t calculate_deadline_pressure(uint32_t cpu);
```

**Task placement policy:**
- Tasks with `deadline_ns > 0` prefer "performance cores" (CPU 1+)
- This keeps CPU 0 available for system tasks
- On big.LITTLE hardware, this maps to big cores
- `find_performance_cpu()` returns least-loaded non-isolated performance CPU

### GIC Affinity for Isolated Cores

The GIC driver provides functions to control interrupt routing:

```c
int gic_set_affinity(uint32_t irq, uint32_t cpu_mask);  /* Set SPI targets */
uint32_t gic_get_affinity(uint32_t irq);                /* Get current targets */
void gic_exclude_cpu_from_spis(uint32_t cpu);           /* Route SPIs away */
void gic_include_cpu_in_spis(uint32_t cpu);             /* Restore SPI routing */
```

When a core is isolated via `sched_isolate_core()`:
1. SPIs are automatically routed away from the core
2. Timer IRQs (PPIs) remain active for scheduler preemption
3. When un-isolated, SPI routing is restored

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

## Priority Inversion Prevention

Priority inversion occurs when a high-priority task waits on a lock held by a low-priority task, effectively running at the lower priority. SLM-OS provides a **priority-inheriting mutex** to prevent this.

### PI Mutex API

```c
#include "pi_mutex.h"

typedef struct {
    spinlock_t guard;           /* Protects mutex state */
    struct task *owner;         /* Current owner (NULL if unlocked) */
    uint8_t owner_original_pri; /* Owner's priority before inheritance */
    volatile uint8_t locked;    /* 1 if locked, 0 if unlocked */
} pi_mutex_t;

void pi_mutex_init(pi_mutex_t *mutex);
void pi_mutex_lock(pi_mutex_t *mutex);      /* Blocking acquire */
int pi_mutex_trylock(pi_mutex_t *mutex);    /* Non-blocking: 1=success, 0=fail */
void pi_mutex_unlock(pi_mutex_t *mutex);

/* Statistics */
uint32_t pi_mutex_inversion_count(void);    /* Total inversions detected */
```

### Priority Inheritance Behavior

When a high-priority task attempts to lock a mutex held by a lower-priority task:

1. The lock holder's `effective_priority` is boosted to match the waiter
2. The original priority is saved in `owner_original_pri`
3. When the lock is released, the owner's priority is restored
4. Each boost is logged: `"PI: Boosting task 'X' (pri N->M) for waiter 'Y'"`

```
Before PI:
  Task A (LOW, pri=2) holds mutex
  Task B (HIGH, pri=6) waiting on mutex
  Task C (NORMAL, pri=4) ready

  Schedule order: C, A, B  ← A blocks B despite lower priority!

With PI:
  Task A boosted to pri=6 while holding mutex
  Schedule order: A (boosted), B, C  ← A runs to release lock quickly
```

### When to Use PI Mutex

Use `pi_mutex_t` instead of `spinlock_t` when:
- The critical section may be held for non-trivial time
- High-priority tasks may contend with low-priority tasks
- Predictable latency is important

Continue using spinlocks for:
- Very short critical sections
- Interrupt handlers (IRQ context)
- Single-CPU scenarios

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

## Heterogeneous Scheduling (big.LITTLE)

The `runtime/src/sched/heterogeneous.rs` module provides CPU topology awareness for heterogeneous systems:

### CPU Topology

```rust
/// CPU topology detection
let topology = CpuTopology::detect();  // Auto-detect (defaults to 4 homogeneous cores)

/// Or explicitly configure
let topology = CpuTopology::big_little(2, 4);  // 2 big + 4 LITTLE cores

/// Query topology
println!("Cores: {}", topology.num_cores());
println!("Heterogeneous: {}", topology.is_heterogeneous());
```

### Core Selection

```rust
/// Task placement preferences
pub struct TaskPlacement {
    pub core_type: CoreType,        // Performance, Efficiency, or Any
    pub pinned_core: Option<u8>,    // Specific core (if any)
    pub preferred_cluster: Option<u8>,
    pub allow_migration: bool,
}

/// Built-in presets
TaskPlacement::DEFAULT          // Any core, allow migration
TaskPlacement::INFERENCE_HIGH   // Performance core, pinned
TaskPlacement::BACKGROUND       // Efficiency core

/// For inference tasks
let placement = TaskPlacement::for_inference(model_size, is_urgent);
```

### Load Balancer

```rust
/// Load-aware core selection
let balancer = LoadBalancer::detect();

// Update load info (from kernel)
balancer.update_load(cpu_id, load_percent, task_count);

// Select best core for task
let core = balancer.select_core(&placement);
```

## Inference Scheduler (Phase 5)

The `runtime/src/sched/inference.rs` module provides a skeleton for AI inference task management:

### API (Skeleton)

```rust
/// Create scheduler
let mut scheduler = InferenceScheduler::new();
scheduler.start()?;

/// Submit inference request
let config = InferenceConfig {
    max_tokens: 128,
    temperature: 0.7,
    deadline: TaskDeadline::inference_latency_ms(100),
    ..Default::default()
};
let request = InferenceRequest::new(model_handle, config)
    .with_priority(Priority::High);
let request_id = scheduler.submit(request)?;  // Returns NotImplemented for now

/// Get results
let result = scheduler.get_result(request_id, timeout_ms)?;
```

### Request States

```
QUEUED → RUNNING → COMPLETED
           ↓         ↓
        FAILED   CANCELLED
```

Full implementation will be added in Phase 5 with actual inference engine integration.

---

## Future Work

### Phase 5 (AI Integration)
- ☐ Implement InferenceScheduler with actual request queue
- ☐ Batch inference requests for throughput
- ☐ GPU/NPU task coordination

### Milestone 4+ (big.LITTLE)
- ☐ Actual core assignment based on CoreType hints (detect big vs LITTLE cores)
- ☐ CPU frequency scaling integration
- ☐ Dynamic core migration based on thermal/power state

---

*Last updated: December 2025*
