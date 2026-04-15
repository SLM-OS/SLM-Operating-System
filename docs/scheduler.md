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
│   C Kernel Layer (kernel/sched/)                                     │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │  Pluggable Policy Interface (sched_policy.h)                │   │
│   │  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐       │   │
│   │  │  heuristic   │ │   ai_mlp     │ │   ai_ppo     │       │   │
│   │  │ (default)    │ │ (Phase AI)   │ │ (Phase AI)   │       │   │
│   │  └──────────────┘ └──────────────┘ └──────────────┘       │   │
│   ├─────────────────────────────────────────────────────────────┤   │
│   │  Scheduler Core (sched.c) — unchanged                       │   │
│   │  - Per-CPU Run Queues (priority-ordered)                    │   │
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

## Pluggable Policy Interface

The scheduler uses a **vtable-based pluggable policy** for CPU assignment decisions. The scheduler core (run queues, locking, context switch, `pick_next_task()`) is unchanged — policies only control which CPU a new task is placed on.

### Policy Vtable

```c
struct sched_policy_ops {
    const char *name;                          /* Human-readable name */
    int      (*init)(void);                    /* Called on activation (may be NULL) */
    void     (*shutdown)(void);                /* Called on deactivation (may be NULL) */
    uint32_t (*assign_cpu)(struct task *task);  /* CPU selection for new tasks */
    void     (*tick)(uint32_t cpu);            /* Per-tick callback (may be NULL) */
};
```

### Built-in Policy: Heuristic

The default "heuristic" policy (`kernel/sched/sched_heuristic.c`) implements:
- Round-robin load balancing with deadline pressure awareness
- Deadline-constrained tasks routed to "performance" cores (CPU 1+)
- Combined `ready_count + deadline_pressure` scoring per CPU
- CPU 0 kept available for system tasks

### Runtime Policy Switching

Policies are registered at boot and can be switched at runtime via the shell:

```
sched                    # Show current policy
sched policy             # List all registered policies
sched policy <name>      # Switch to named policy
```

Programmatic API:

```c
sched_register_policy(&my_policy);            /* Register a policy */
sched_set_policy(&my_policy);                 /* Activate (calls init/shutdown) */
const char *name = sched_get_policy();        /* Get active policy name */
const struct sched_policy_ops *p =
    sched_find_policy("heuristic");           /* Look up by name */
```

Policy switching is IRQ-safe: interrupts are masked during the pointer swap. If a new policy's `init()` fails, the previous policy remains active.

### Policy Bypass

Tasks with explicit CPU affinity (`cpu_affinity != CPU_AFFINITY_ANY`) bypass the policy entirely and are placed directly on the specified CPU.

### Adding a New Policy

1. Create a `struct sched_policy_ops` with at minimum `name` and `assign_cpu`
2. Call `sched_register_policy()` during kernel init
3. The policy can inspect run queue state via `sched_cpu_rq(cpu)->ready_count` etc.
4. Use `sched_get_isolated_cores()` to avoid isolated CPUs

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
    struct task *head;
    struct task *tail;
    struct task *idle_task;
    struct task *zombie;
    uint32_t ready_count;
};
/* Per-CPU spinlocks are separate (rq_lock[]) because ARM64 exclusive
 * load/store requires cacheable memory, but run queues may be in NC memory. */
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
- The active policy's `assign_cpu()` selects from non-isolated CPUs
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

**Task placement policy (heuristic):**
- Tasks with `deadline_ns > 0` prefer "performance cores" (CPU 1+)
- This keeps CPU 0 available for system tasks
- On big.LITTLE hardware, this maps to big cores
- `find_performance_cpu()` (in `sched_heuristic.c`) returns least-loaded non-isolated performance CPU

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

### Wait-Loop Implementation (SCHED-H1, April 2026)

`pi_mutex_lock`'s inner spin loop now guards each probe of `mutex->locked` with a local `irq_save` / `irq_restore` pair and calls `yield()` between probes. This closes a narrow window where a timer ISR could re-enter `pi_mutex` paths with interrupts already enabled and deadlock against another waiter's `guard` acquire. The pseudocode:

```c
while (mutex->locked) {
    irq_flags_t f = irq_save();
    if (!mutex->locked) { irq_restore(f); break; }
    irq_restore(f);
    yield();
}
```

### Known Limitation — Single-CPU Contended Priority Inheritance

The boost path in `try_boost_owner` updates `effective_priority` but does not re-sort the run queue. A holder that was enqueued at `LOW` remains in its `LOW` slot in the per-CPU queue even after being boosted to `HIGH`. In a single-CPU scenario with one holder and one HIGH-priority waiter, the scheduler picks the waiter (rq head), which spins/yields and is itself re-queued at HIGH — starving the boosted holder. On real Pi 5 hardware this is exacerbated because `DAIF.I=1` blocks timer preemption inside tasks.

The capstone-ready conservative fix (SCHED-H1) handles the IRQ-safety issue but does not resolve the single-CPU livelock. The proper fix is a sleep-queue model — the waiter blocks on a wait condition and is woken by `pi_mutex_unlock` — and is tracked as a post-capstone enhancement.

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

// kernel/sched/sched.c
#define DEADLINE_CRITICAL_NS  (10 * 1000000ULL)   // 10ms
#define DEADLINE_HIGH_NS      (50 * 1000000ULL)   // 50ms
#define DEADLINE_BOOST_NS     (100 * 1000000ULL)  // 100ms
```

### AI Scheduler (Optional)

The AI scheduler is an optional feature that adds MLP/PPO model-based CPU assignment policies. It is disabled by default and has no effect on the standard build.

```bash
# Build with AI scheduler (adds ai_mlp and ai_ppo policies)
cmake -B build/kernel \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-none-elf.cmake \
  -DPLATFORM=QEMU_VIRT \
  -DENABLE_AI_SCHEDULER=ON

# Or for tests
cmake -B build/kernel-test \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-none-elf.cmake \
  -DPLATFORM=QEMU_VIRT \
  -DENABLE_BOOT_TESTS=ON \
  -DENABLE_AI_SCHEDULER=ON
```

When enabled:
- Defines `CONFIG_AI_SCHEDULER=1` globally
- Builds `libai_sched.a` — a separate static library compiled **without** `-mgeneral-regs-only` (same pattern as Lua), enabling FP/NEON for inference math
- Registers `ai_mlp` and `ai_ppo` policies at boot via `sched_ai_init()`
- Adds ~1 MB to binary size (mostly weight arrays; stub weights are all zeros)

**Files:** `kernel/sched/ai/` — `ai_types.h`, `ai_weights.h`, `ai_weights_stub.c`, `ai_inference.{c,h}`, `ai_state.{c,h}`, `sched_ai.c`, `fp_context.S`

#### Inference Engine Architecture

The inference engine implements a 4-layer feedforward neural network:

```
Input: state[108] (per-core, per-task, and global features)
  ↓
Layer 0: Linear(108→256) + ReLU     ~27K params
Layer 1: Linear(256→256) + ReLU     ~66K params
Layer 2: Linear(256→128) + ReLU     ~33K params
Layer 3: Linear(128→42)             ~5K params
  ↓
Output: logits[42] → argmax → decode(core, priority_adj, preempt)
```

**Total:** ~131K parameters, ~262K FLOPs per inference.

**Performance:** SIMD-optimized on both architectures:
- **AArch64**: NEON intrinsics (`vfmaq_f32` fused multiply-add, `vmaxq_f32` ReLU). Estimated ~22µs on Cortex-A78 @ 1.5 GHz.
- **x86-64**: SSE intrinsics (`_mm_mul_ps`/`_mm_add_ps` dot product, `_mm_max_ps` ReLU, shuffle-based horizontal sum).
- **Fallback**: Scalar C code on unsupported architectures.

**Thread safety:** All scratch memory is stack-allocated (two alternating 256-float buffers = 2 KB). No static globals, no locks needed. Multiple CPUs can run inference concurrently.

**Action decoding:** The argmax index encodes three decisions:
- `preempt = idx % 2` (0=no, 1=yes)
- `priority_adj = (idx / 2) % 3` (0=none, 1=boost, 2=reduce)
- `core_assignment = idx / 6` (0 to num_cores-1)

Invalid actions (core out of range, isolated core) fall back to the heuristic policy.

#### State Vector (108 dimensions)

The AI policy observes kernel state through a fixed 108-float vector matching the training simulator's observation space:

**Per-core features (36 floats = 6 cores × 6 features):**

| Feature | Source | Range |
|---------|--------|-------|
| utilization | running_ticks / total_ticks | [0, 1] |
| queue_depth | ready_count / 32 | [0, ~1] |
| cache_pressure | 0.0 (future: PMU) | [0, 1] |
| core_type | 1.0 (homogeneous) | {0, 1} |
| isolated | sched_get_isolated_cores() bit | {0, 1} |
| current_task_prio | effective_priority / 7 | [0, 1] |

Cores beyond `cpu_count` are zero-filled (e.g., Pi 5 has 4 cores, slots 4-5 are zeros).

**Per-task features (64 floats = 8 tasks × 8 features):**

Top 8 tasks by effective priority, cached and updated every 100ms.

| Feature | Source | Range |
|---------|--------|-------|
| priority | effective_priority / 7 | [0, 1] |
| deadline_urgency | 1 - (deadline - now) / 1s | [0, 1] |
| wait_time | (now - arrival_time) / 1s | [0, ∞) |
| working_set, model_size, inference_dur, can_use_gpu, component_type | 0.0 (future) | — |

**Global features (8 floats):**

| Feature | Source |
|---------|--------|
| ready_count | sum(ready_count) / 64 |
| deadline_miss_rate | rolling window of 100 completions |
| avg_latency | cumulative / count / 10ms |
| load_imbalance | std_dev(utils) / mean(utils), clamped [0,1] |
| weight/workspace_pool_pressure, gpu_queue_depth, episode_time | 0.0 (future) |

#### FP Register Safety

AI inference uses FP/NEON registers, but `scheduler_add_task()` can be called from timer IRQ context (e.g., waking a sleeping task). The AI policy wraps all inference calls with `FP_CONTEXT_SAVE()` / `FP_CONTEXT_RESTORE()` (`fp_context.h`) to save and restore all 32 SIMD registers (V0-V31) + FPCR/FPSR. This adds ~520 bytes of stack usage per inference call.

The save/restore is implemented in assembly (`fp_context.S`): `stp`/`ldp` pairs for ARM64, `FXSAVE`/`FXRSTOR` for x86-64.

#### Heuristic Fallback

The AI policy falls back to the heuristic policy when:
- Inference returns an error (e.g., NULL state)
- The decoded action targets an out-of-range CPU
- The target CPU is isolated and the task has `CPU_AFFINITY_ANY`

Fallback calls `sched_policy_heuristic.assign_cpu()` directly. Each fallback is counted in per-policy statistics.

#### Policy Statistics

Each AI policy tracks:
- **decisions**: total `assign_cpu` calls
- **fallbacks**: times heuristic was used instead
- **total_latency_ns**: cumulative inference time (state extraction + forward pass)

Statistics are logged when the policy is deactivated via `shutdown()`.

#### Scheduler Counters (CONFIG_AI_SCHEDULER)

When `CONFIG_AI_SCHEDULER` is defined, the scheduler tracks additional metrics:
- **Per-CPU**: `running_ticks` / `total_ticks` in `cpu_runqueue` (incremented in `scheduler_tick()`)
- **Per-task**: `arrival_time_ns` (set in `scheduler_add_task()`), `completion_time_ns` (set in `task_exit()`)
- **Global**: deadline miss rolling window (100 entries), cumulative latency, top-8 task cache (updated every 10 ticks)

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

### Phase AI-Sched (AI Scheduler Integration)
- ✅ Pluggable policy interface (`sched_policy.h`)
- ✅ Heuristic policy extracted (`sched_heuristic.c`)
- ✅ Shell command for runtime policy switching (`sched`)
- ☐ AI inference engine (MLP/PPO forward pass in kernel)
- ☐ State vector extraction (108-dim observation)
- ☐ AI policy implementation (`sched_ai.c`)
- See `TODO_-_PHASE_AI-Sched.md` for full tracking

### Phase 5 (Inference Scheduling)
- ☐ Implement InferenceScheduler with actual request queue
- ☐ Batch inference requests for throughput
- ☐ GPU/NPU task coordination

### Milestone 4+ (big.LITTLE)
- ☐ Actual core assignment based on CoreType hints (detect big vs LITTLE cores)
- ☐ CPU frequency scaling integration
- ☐ Dynamic core migration based on thermal/power state

---

## Cross-CPU wakeup: `smp_notify_cpu()` (Pre-1)

`scheduler_add_task_to_cpu()` calls `smp_notify_cpu(logical_cpu)`
after enqueuing a task on another CPU's run queue so that the
target, if currently idle (WFE on ARM64 / HLT on x86-64), wakes
immediately and picks up the new task without waiting for its next
local timer tick. A call with `logical_cpu == cpu_id()` is a cheap
no-op.

| Platform | Backend |
|---|---|
| ARM64 (Pi 5, Jetson, QEMU_VIRT) | `sev` — in `kernel/sched/smp.c` |
| x86-64 | Reschedule IPI on LAPIC vector 49 — handler in `kernel/arch/x86_64/platform_x86.c` |

The x86-64 handler calls `schedule()` directly (NOT `scheduler_tick()`) so
quantum accounting stays owned by the LAPIC timer. The reschedule IPI is
gated through IDT entry 49 with `ist=1`, sharing IST1 with the timer —
see A1 in `docs/x86-64-capstone-gap-closure-plan.md` for the stack rationale.

## Periodic load rebalance (`sched_rebalance_tick`)

Every `REBALANCE_INTERVAL_TICKS` (default 100 = 1 s at 100 Hz) on BSP,
`sched_rebalance_tick(cpu)` — invoked via the active policy's `tick`
callback — scans `cpu_rq(i)->ready_count` across CPUs. If the
`(max − min)` delta exceeds `REBALANCE_IMBALANCE_MIN` (2), one
migratable task moves from the busiest CPU to the idlest. Migration
is gated on:

- `cpu_affinity == CPU_AFFINITY_ANY` — tasks pinned via explicit
  affinity are never moved.
- `task != busy_rq->idle_task` — the per-CPU idle task never migrates.
- `state == TASK_READY` — runnable tasks only.

Migration sequence:
1. Acquire busy CPU's `rq_lock_irqsave` → walk list → dequeue.
2. Drop busy CPU's lock.
3. `scheduler_add_task_to_cpu(task, idle_cpu)` — which takes the
   idle CPU's lock, enqueues, calls `smp_notify_cpu(idle_cpu)`.

`sched_rebalance_get_migrations()` exposes a diagnostic counter for
tests.

## Work stealing (Pre-existing + B3 activation + S4 default flip)

`kernel/sched/steal_deque.c` implements a Chase–Lev-style deque per
CPU. When a CPU's own run queue goes empty inside `schedule()`, and
`CONFIG_WORK_STEALING` is on, it calls `sched_try_steal()` to pull
work from another CPU's deque.

**Default by platform (after Jetson capstone S4, 2026-04-14):**

| Platform | Default | Notes |
|---|---|---|
| x86-64 | ON | Since Phase B; cache-coherent SMP + LAPIC IPI |
| QEMU ARM64 | ON | 1.7× speedup measured (S3) |
| Raspberry Pi 5 | ON | 3.12× speedup measured on hardware (`docs/work-stealing-bench.md`); closed #158 |
| Jetson Orin Nano | OFF | Residual page fault during bench stealing (#166); opt-in via `WORK_STEALING=ON` |

Override: `make kernel PLATFORM=<p> WORK_STEALING=ON` or `WORK_STEALING=OFF` maps to `-DENABLE_WORK_STEALING=ON/OFF`, bypassing the CMake per-platform default.

**Locking model:**

The deque's bookkeeping (`buf`, `gen_buf`, `bottom`, `top`) is
serialized by `steal_deque_lock[MAX_CPUS]` — a separate cacheable
`spinlock_t` array in `kernel/sched/sched.c`, same pattern as
`rq_lock[]`. The deque struct itself lives in NC memory on
`PLATFORM_HAS_NC_MEMORY` (Pi 5, Jetson) for instant cross-CPU
visibility without cache maintenance. The deque used to embed its
own spinlock, but on Jetson `SPINLOCK_SKIP_LOCKING` reduced that to
a barrier-only no-op, letting concurrent pushes/pops corrupt the
deque — see #158 and commit `a3b3a0e` for the move.

Every `steal_deque_push` / `_pop` / `_steal` / `_remove` call in
`sched.c` is wrapped with `spin_lock_irqsave(&steal_deque_lock[cpu])` /
`spin_unlock_irqrestore`. The deque's own functions are lockless —
they trust the caller.

**Correctness invariants:**
- `sched_try_steal()` releases `steal_deque_lock[victim]` before
  acquiring `rq_lock[victim]`. The two locks are never held
  together, so there is no cross-lock ordering constraint.
- Under `rq_lock[victim]`, the validator re-reads
  `candidate->generation` and compares it to the generation captured
  at push time (#139 ABA mitigation). A mismatch means the slot has
  been recycled into a different logical task; the steal is
  rejected.
- `preempt_disabled[victim]` is checked as a cheap early-out (not a
  correctness requirement) to avoid contending a lock the victim is
  almost certainly about to take.

---

*Last updated: April 2026*
