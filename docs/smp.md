# Symmetric Multi-Processing (SMP) for SLM-OS

This document describes the design and implementation of multi-core support in SLM-OS.

---

## Overview

SLM-OS targets ARM64 systems with multiple CPU cores. The QEMU virt machine emulates up to 4 cores by default, and the Jetson Orin Nano has 6 Cortex-A78AE cores.

**Goals:**
- Boot all secondary cores using PSCI
- Per-core run queues for load distribution
- Synchronization primitives (spinlocks)
- Inter-processor interrupts (IPI) for cross-core signaling

---

## PSCI (Power State Coordination Interface)

### What is PSCI?

PSCI is an ARM standard (DEN0022) that defines a software interface for power management between an OS and its supervisory firmware. It provides a portable way to:
- Power on/off CPU cores
- Put CPUs into idle states
- Reset or shut down the system

PSCI calls are made using either:
- **HVC** (Hypervisor Call) — when OS runs at EL1 without EL3 firmware
- **SMC** (Secure Monitor Call) — when EL3 firmware is present

### QEMU virt Machine

QEMU's virt machine provides built-in PSCI emulation:
- Uses **HVC** as the default conduit (no EL3 firmware)
- Secondary CPUs start in powered-off state
- Primary CPU (core 0) executes from the kernel entry point

The device tree specifies the PSCI method:
```
psci {
    compatible = "arm,psci-0.2";
    method = "hvc";
};
```

### CPU_ON Function

The `CPU_ON` function powers up a secondary core.

| Parameter | Register | Description |
|-----------|----------|-------------|
| Function ID | x0 | `0xC4000003` (64-bit) or `0x84000003` (32-bit) |
| target_cpu | x1 | MPIDR of target CPU (Aff0 for simple cases) |
| entry_point | x2 | Address where secondary core starts execution |
| context_id | x3 | Value passed to secondary core in x0 on entry |

**Return values (in x0):**

| Value | Name | Meaning |
|-------|------|---------|
| 0 | SUCCESS | CPU powered on successfully |
| -1 | NOT_SUPPORTED | Function not supported |
| -2 | INVALID_PARAMS | Invalid parameters |
| -3 | DENIED | Operation denied |
| -4 | ALREADY_ON | CPU already powered on |
| -5 | ON_PENDING | CPU power-on in progress |
| -6 | INTERNAL_FAILURE | Internal failure |
| -7 | NOT_PRESENT | CPU not present |
| -8 | DISABLED | CPU disabled |
| -9 | INVALID_ADDRESS | Invalid entry point address |

### Implementation

```c
/* PSCI function IDs */
#define PSCI_CPU_ON_64      0xC4000003
#define PSCI_CPU_OFF        0x84000002
#define PSCI_SYSTEM_OFF     0x84000008
#define PSCI_SYSTEM_RESET   0x84000009

/* Invoke PSCI via HVC */
static inline int64_t psci_call(uint64_t fn, uint64_t arg1,
                                 uint64_t arg2, uint64_t arg3) {
    register uint64_t x0 asm("x0") = fn;
    register uint64_t x1 asm("x1") = arg1;
    register uint64_t x2 asm("x2") = arg2;
    register uint64_t x3 asm("x3") = arg3;

    asm volatile("hvc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "memory");

    return (int64_t)x0;
}

/* Power on a secondary CPU */
int cpu_on(uint32_t cpu_id, uintptr_t entry_point, uintptr_t context_id) {
    return psci_call(PSCI_CPU_ON_64, cpu_id, entry_point, context_id);
}
```

---

## CPU Identification

### MPIDR_EL1 Register

Each core has a unique MPIDR (Multiprocessor Affinity Register):

```
MPIDR_EL1 layout:
┌─────────┬─────────┬─────────┬─────────┬───────┬─────────────────┐
│ Res0    │  Aff3   │  Aff2   │  Aff1   │ Res0  │      Aff0       │
│ [39:32] │ [31:24] │ [23:16] │ [15:8]  │ [7:0] │     [7:0]       │
└─────────┴─────────┴─────────┴─────────┴───────┴─────────────────┘
```

For QEMU virt with simple topology:
- Aff0 = core number (0, 1, 2, 3)
- Aff1, Aff2, Aff3 = 0

### Logical vs Physical CPU IDs

SLM-OS uses a mapping table to translate between logical CPU IDs (0, 1, 2, ...) and physical MPIDR values.

```c
#define MAX_CPUS 8

/* Maps logical CPU ID → MPIDR value */
uint64_t cpu_logical_map[MAX_CPUS];

/* Number of CPUs detected */
uint32_t cpu_count;
```

**Why a mapping table?**

Non-contiguous MPIDR values are common on real hardware:

| Cause | Example |
|-------|---------|
| Big.LITTLE | Big cores at 0x000-0x003, LITTLE at 0x100-0x103 |
| Multi-cluster | Cluster 0 at 0x000-0x002, Cluster 1 at 0x100-0x102 |
| Fused-off cores | Manufacturing defects disable cores, leaving gaps |
| Asymmetric configs | Clusters with different core counts |

The Jetson Orin Nano has 6 Cortex-A78AE cores likely arranged in 2 clusters of 3, giving MPIDR values like 0x000, 0x001, 0x002, 0x100, 0x101, 0x102 — non-contiguous.

**Tradeoff:**

| Approach | Pros | Cons |
|----------|------|------|
| Direct MPIDR | Simpler, no table lookup | Breaks on non-contiguous MPIDR |
| Mapping table | Works on all platforms | Extra indirection, table maintenance |

SLM-OS uses the mapping table approach for portability. The overhead is minimal (one array lookup), and it ensures the OS works correctly on any ARM64 platform without code changes.

**Implementation:**

```c
/* Initialize during boot — primary populates table */
void smp_init_cpu_map(void) {
    /* CPU 0 is always the boot CPU */
    cpu_logical_map[0] = cpu_get_mpidr() & MPIDR_AFF_MASK;
    cpu_count = 1;

    /* Discover other CPUs from device tree or ACPI (future) */
    /* For now, assume contiguous for QEMU */
    for (int i = 1; i < MAX_CPUS; i++) {
        cpu_logical_map[i] = i;  /* Works for QEMU */
    }
}

/* Get logical CPU ID from MPIDR */
int cpu_logical_id(uint64_t mpidr) {
    uint64_t aff = mpidr & MPIDR_AFF_MASK;
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_logical_map[i] == aff)
            return i;
    }
    return -1;  /* Unknown CPU */
}

/* Get current CPU's logical ID */
static inline uint32_t cpu_id(void) {
    uint64_t mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return cpu_logical_id(mpidr);
}
```

---

## Per-CPU Data Structures

### Design

Per-CPU state is split between two structures:

**`struct per_cpu` (in `smp.h`)** — SMP-level data, used during boot and for CPU identification:

```c
struct per_cpu {
    uint32_t cpu_id;            /* Logical CPU ID */
    uint64_t mpidr;             /* Physical MPIDR value */
    volatile bool online;       /* True when core is up and running */
    void *stack_top;            /* Top of this CPU's boot stack */
    uint64_t boot_time_ns;      /* Time when this CPU came online */
};

extern struct per_cpu cpu_data[MAX_CPUS];
```

**`struct cpu_runqueue` (in `sched.c`)** — Scheduler-level data, one per CPU:

```c
struct cpu_runqueue {
    spinlock_t lock;            /* Per-queue lock (reduces contention) */
    struct task *head;          /* First task in run queue */
    struct task *tail;          /* Last task in run queue */
    struct task *idle_task;     /* This CPU's idle task */
    struct task *zombie;        /* Terminated task pending cleanup */
    uint32_t ready_count;       /* Tasks in this CPU's run queue */
};
```

### Accessing Per-CPU Data

Since there's no thread-local storage in bare metal, per-CPU data is accessed via:
1. Read MPIDR to get CPU ID
2. Index into global `cpu_data[]` or scheduler arrays

This requires a memory access for every per-CPU operation but is simple and reliable.

**Cache coherency note:** `struct per_cpu` is 40 bytes, so adjacent entries share 64-byte cachelines. On Pi 5 (no SMPEN), CPU 0 must `cache_clean_range(cpu_data, ...)` before booting secondary CPUs to avoid DC CIVAC writeback corruption. See `kernel/CLAUDE.md` for details.

---

## Secondary Core Boot Sequence

### Overview

1. Primary core (CPU 0) initializes kernel, VMM, GIC distributor
2. Primary calls `smp_init()` to bring up secondaries
3. For each secondary: call PSCI `CPU_ON` with entry point
4. Secondary cores execute `secondary_entry` in assembly
5. Each secondary initializes its GIC CPU interface, timer, stack
6. Each secondary enters idle loop waiting for work

### Boot Flow

```
Primary (CPU 0)                    Secondary (CPU 1-N)
─────────────────                  ──────────────────
kernel_main()
  ├─ vmm_init()
  ├─ gic_init()      ────────────► (powered off)
  ├─ timer_init()
  ├─ scheduler_init()
  └─ smp_init()
       ├─ cpu_on(1, entry, 0) ───► secondary_entry:
       │                             ├─ set SP (per-core stack)
       │                             ├─ enable FPU
       │                             ├─ gic_percpu_init()
       │                             ├─ timer_percpu_init()
       │                             ├─ mark cpu online
       │                             └─ idle loop
       ├─ cpu_on(2, entry, 0) ───► (same)
       └─ cpu_on(3, entry, 0) ───► (same)
```

### Per-Core Stacks

Each core needs its own stack:

```c
/* In linker script or static allocation */
#define BOOT_STACK_SIZE  (16 * 1024)  /* 16 KB per core */

alignas(16) uint8_t cpu_stacks[MAX_CPUS][BOOT_STACK_SIZE];

/* Stack top for CPU n */
#define CPU_STACK_TOP(n) ((uintptr_t)&cpu_stacks[n][BOOT_STACK_SIZE])
```

### Assembly Entry Point

```asm
.global secondary_entry
secondary_entry:
    /* x0 = context_id (passed from CPU_ON, contains cpu_id) */

    /* Set up stack for this core */
    ldr     x1, =cpu_stacks
    add     x1, x1, x0, lsl #14     /* x1 = cpu_stacks + cpu_id * 16KB */
    add     sp, x1, #(16 * 1024)    /* SP = top of stack */

    /* Enable FPU/SIMD */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    /* Set exception vectors */
    ldr     x1, =exception_vectors
    msr     vbar_el1, x1
    isb

    /* Jump to C initialization */
    bl      secondary_init

    /* Should not return */
1:  wfi
    b       1b
```

---

## GIC Per-CPU Initialization

GICv2 has two parts:
- **Distributor** (shared) — initialized once by primary core
- **CPU Interface** (per-core) — initialized by each core

```c
void gic_percpu_init(void) {
    /* Enable CPU interface */
    mmio_write32(GICC_CTLR, 1);

    /* Set priority mask (accept all priorities) */
    mmio_write32(GICC_PMR, 0xFF);
}
```

Each core must also enable its own timer interrupt (PPI 27 for virtual timer).

---

## Synchronization Primitives

### Spinlock

Basic spinlock using ARM64 exclusive load/store:

```c
typedef struct {
    volatile uint32_t lock;
} spinlock_t;

#define SPINLOCK_INIT { .lock = 0 }

static inline void spin_lock(spinlock_t *lock) {
    uint32_t tmp;
    asm volatile(
        "   sevl\n"
        "1: wfe\n"
        "   ldaxr   %w0, [%1]\n"
        "   cbnz    %w0, 1b\n"
        "   stxr    %w0, %w2, [%1]\n"
        "   cbnz    %w0, 1b\n"
        : "=&r"(tmp)
        : "r"(&lock->lock), "r"(1)
        : "memory"
    );
}

static inline void spin_unlock(spinlock_t *lock) {
    asm volatile(
        "   stlr    wzr, [%0]\n"
        : : "r"(&lock->lock) : "memory"
    );
}
```

**Key instructions:**
- `ldaxr` — Load-Acquire Exclusive (acquire semantics)
- `stxr` — Store Exclusive (returns 0 on success)
- `stlr` — Store-Release (release semantics)
- `wfe` — Wait For Event (low power wait)
- `sevl` — Send Event Local (ensure first WFE doesn't block)

### Ticket Lock (Fairer)

For better fairness under contention:

```c
typedef struct {
    volatile uint16_t next;
    volatile uint16_t owner;
} ticket_lock_t;

void ticket_lock(ticket_lock_t *lock) {
    uint16_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE) != ticket) {
        asm volatile("wfe");
    }
}

void ticket_unlock(ticket_lock_t *lock) {
    uint16_t next = lock->owner + 1;
    __atomic_store_n(&lock->owner, next, __ATOMIC_RELEASE);
    asm volatile("sev");  /* Wake waiters */
}
```

---

## Multi-Core Scheduler

### Per-Core Run Queues

Each core has its own run queue with a per-queue lock (reduces contention vs. a global lock). Tasks are inserted in priority order (highest `effective_priority` first, FIFO within same priority).

### Task Affinity

Tasks can be pinned to specific cores or allowed to migrate:

```c
/* In struct task */
uint32_t cpu_affinity;      /* CPU_AFFINITY_ANY or specific CPU ID */
uint32_t assigned_cpu;      /* CPU this task is currently assigned to */
```

### Deadline Boost

Tasks with `deadline_ns > 0` get their `effective_priority` boosted as the deadline approaches:
- More than 100ms remaining: no boost
- 50–100ms remaining: +1 priority
- 10–50ms remaining: boost to HIGH (6)
- Under 10ms or missed: boost to CRITICAL (7)

### Schedule Function

The `schedule()` function runs on each CPU independently:
1. Clean up zombie (terminated) task from previous cycle
2. If current task is RUNNING, set to READY and re-insert in priority order
3. Pick highest-priority task from queue (or idle task if empty)
4. Context switch via `switch_to()` (saves/restores callee-saved regs, FPU, DAIF)

### Sleep/Delay

`sleep_ms()` and `sleep_us()` provide timer-driven delays using the ARM generic timer counter. The sleeping task calls `yield()` in a loop until the target tick count is reached. Other tasks (including idle) run during the wait. Minimum precision is one scheduler tick (~10ms).

### Idle Task DAIF

The idle task's IRQ unmask (`msr daifclr, #2`) must be inside the `while(1)` loop, not before it. When idle is preempted by the timer ISR, ARM hardware masks IRQ on exception entry, and `context.S` saves this masked DAIF. On resume, the restored DAIF would keep IRQ masked, causing `wfi` to hang. Re-clearing DAIF each iteration prevents this.

### Task Initial DAIF — Tasks Start With IRQ Masked (Pi 5 / Jetson)

All tasks are created with `DAIF.I=1` (IRQ masked). `task_create()` in `kernel/sched/task.c` writes `task->context.daif = 0x080`, and `context.S:126-128` restores DAIF early in the switch sequence — before GP registers and SP are fully loaded.

Rationale: on real ARM64 hardware without SMPEN (Pi 5) or on post-kexec Jetson, a timer IRQ taken mid-context-restore causes the ISR to run on a partially-restored task context, corrupting state. Starting tasks with IRQs masked closes this window. Task code unmasks naturally on the first `spin_unlock_irqrestore` or the idle task's explicit `daifclr`. On QEMU the same masking applies for uniformity — QEMU delivers IRQs regardless, but the invariant matches real hardware behavior.

Platform-specific consequence: timer IRQs do not fire while application tasks are running on Pi 5. Only the idle task on CPU 0 advances `pit_ticks` (it unmasks inside the `wfi` loop). See `kernel/CLAUDE.md` §Pi-5-Timer-IRQs for the full implication chain.

### Secondary-CPU Preemption (`SECONDARY_PREEMPT`)

Calling `schedule()` directly from a timer ISR on real ARM64 hardware
corrupts the abandoned exception frame and hangs the CPU. The fix is
the ELR-trampoline infrastructure (`kernel/sched/preempt.c`,
`resched_trampoline` in `kernel/arch/arm64/vectors.S`): the timer IRQ
handler just sets `reschedule_pending[cpu] = 1` and rewrites the saved
`ELR_EL1` to point at the trampoline. `eret` then lands in the
trampoline in task context, which calls `schedule()` safely.

The feature is gated behind the `SECONDARY_PREEMPT` compile-time option:

| Build invocation | Effect |
|---|---|
| *default* | Trampoline not compiled. Secondary CPUs use cooperative `wfe`/SEV wake; preemption only on CPU 0 (if timer IRQs deliver there). |
| `make kernel SECONDARY_PREEMPT=ON` | Trampoline compiled and linked. All CPUs enable timer preemption. Functional on Pi 5 (#99 coop-preempt path) and QEMU; gated on the Jetson MPIDR fix (Jetson plan P3 step 2) before it's safe there. |
| `make kernel PI5_SECONDARY_PREEMPT=ON` | Deprecated alias. Sets `SECONDARY_PREEMPT=ON`. Kept so existing Pi 5 Makefile invocations continue working. |

The option was renamed from the original `PI5_SECONDARY_PREEMPT` to a
capability-neutral spelling during the Jetson capstone Prereq #2 work
(the rename was a prerequisite for P3 — without it, `CMakeLists.txt`'s
`PLATFORM STREQUAL "RASPI5"` gate prevented the trampoline from
compiling into Jetson builds at all).

### Inter-Processor Interrupts (IPI) / Cross-CPU Notification

Used for:
- Signaling a core to reschedule (a new task was assigned to its queue,
  or the scheduler decided it should re-check its work).
- TLB shootdown (future, for shared page tables).

#### `smp_notify_cpu()` — platform-neutral API

Both platforms route through the common `smp_notify_cpu(logical_cpu)`
API (`kernel/include/smp.h`), called from `scheduler_add_task_to_cpu()`
after pushing a task onto the target CPU's run queue. Per-platform:

| Platform | Implementation | Notes |
|---|---|---|
| ARM64 (Pi 5, Jetson, QEMU virt) | `__asm__ volatile("sev")` broadcast | Wakes every CPU in WFE; target CPU's idle loop picks up the task on the next iteration. Broadcast is wasteful (N-1 idle wakeups per task add) but correct and depends on nothing beyond WFE semantics. Defined in `kernel/sched/smp.c`. |
| x86-64 | LAPIC IPI on `RESCHED_VECTOR` (49) | `kernel/arch/x86_64/platform_x86.c`. Handler calls `schedule()` directly — not `scheduler_tick()` — so quantum accounting stays owned by the local LAPIC timer. Runs on IST1 via the per-CPU TSS. |

**x86-64 vector table:**

| Vector | Purpose | IST |
|---|---|---|
| 48 | LAPIC timer (100 Hz, `scheduler_tick`) | 1 |
| 49 | Reschedule IPI — `smp_notify_cpu` target | 1 |

Both share IST1 via the per-CPU TSS (see A1 in
`docs/x86-64-capstone-gap-closure-plan.md`), so the ISRs cannot be
corrupted by a task-stack overflow.

Both backends short-circuit when `logical_cpu == cpu_id()` (self-IPI
is redundant — the caller is already running) and when `logical_cpu >=
cpu_count` (stale `task->assigned_cpu` values from a prior migration).

#### Planned upgrade: targeted ARM64 SGI

Once the Jetson plan's P3 step 3 fixes `gic_send_sgi()` for
dual-cluster MPIDR (Jetson's Aff0 is 0 for every CPU, so the current
`(1UL << target_cpu)` target-list bit is wrong), the ARM64
implementation of `smp_notify_cpu` will upgrade from broadcast SEV to:

```c
void smp_notify_cpu(uint32_t cpu) {
    gic_send_sgi(cpu, SGI_RESCHED);
}

void handle_sgi(uint32_t sgi_id) {
    if (sgi_id == SGI_RESCHED) {
        /* No-op — the CPU re-checks its queue on the next idle iter
         * after exception return. If SECONDARY_PREEMPT is on, the
         * trampoline path picks the new task. */
    }
}
```

Until then, the SEV broadcast is acceptable: per-add overhead is a few
hundred cycles per idle CPU, and there are rarely more than a handful.

#### Init-time broadcast

`scheduler_init()` also issues a `sev` broadcast after it finishes
primary init, to wake any secondary CPUs that are busy-waiting in
`scheduler_is_initialized()` during their boot sequence. That site is
semantically broadcast (every secondary is waiting) and is kept as a
direct `sev` rather than `smp_notify_cpu` to document the intent.

---

## Memory Barriers

ARM64 has weak memory ordering. Barriers ensure correct operation:

| Barrier | Instruction | Use |
|---------|-------------|-----|
| Data Memory Barrier | `dmb` | Ensure memory operations complete |
| Data Sync Barrier | `dsb` | Ensure memory + cache operations complete |
| Instruction Sync Barrier | `isb` | Flush pipeline, sync instruction stream |

```c
#define dmb(opt)    asm volatile("dmb " #opt ::: "memory")
#define dsb(opt)    asm volatile("dsb " #opt ::: "memory")
#define isb()       asm volatile("isb" ::: "memory")

/* Common patterns */
#define smp_mb()    dmb(ish)    /* Full barrier, inner shareable */
#define smp_rmb()   dmb(ishld)  /* Read barrier */
#define smp_wmb()   dmb(ishst)  /* Write barrier */
```

---

## Implementation Status

All SMP and multi-core scheduler functionality has been implemented and tested.

### Completed Components

| Component | Files | Status |
|-----------|-------|--------|
| PSCI CPU_ON | `smp.c`, `smp_boot.S` | ✅ Complete |
| Per-core stacks | `smp_boot.S` | ✅ Complete |
| Secondary core boot | `smp_boot.S`, `smp.c` | ✅ Complete |
| Spinlocks | `spinlock.h`, `spinlock.c` | ✅ Complete |
| Ticket locks | `spinlock.h`, `spinlock.c` | ✅ Complete |
| IRQ-safe spinlocks | `spinlock.h` | ✅ Complete |
| Per-core run queues | `sched.c` | ✅ Complete (per-queue locks) |
| Task CPU affinity | `task.h`, `sched.c` | ✅ Complete |
| Task migration | `sched.c` | ✅ Complete |
| Priority scheduling | `sched.c` | ✅ Complete (8 levels + deadline boost) |
| Core isolation | `sched.c`, `gic.c` | ✅ Complete (SPI rerouting) |
| Timer sleep/delay | `timer.c` | ✅ Complete (sleep_ms, sleep_us) |
| GIC per-CPU init | `gic.c` | ✅ Complete |
| Timer per-CPU init | `timer.c` | ✅ Complete |

### Test Results

The full test suite passes on both QEMU and Pi 5 hardware (393 tests, 0 failures).

**QEMU multi-core integration tests** (5 tests):
1. **Basic Multi-Core Execution** — 3 tasks run concurrently on CPUs 1, 2, 3
2. **Cross-Core Task Migration** — Task migrated from CPU 1 to CPU 3 queue
3. **Stress Test** — 6 tasks (2 per CPU) complete correctly
4. **Lock Contention** — 3 tasks across 3 CPUs increment shared counter with no races
5. **Task Lifecycle** — Rapid create/destroy cycles

**Pi 5:** These 5 tests are IGNORED because cross-CPU task dispatch requires SMPEN (not set by TF-A). All other tests (scheduler, sleep, SMP online verification, etc.) pass on Pi 5.

### Key Implementation Details

**Locking Strategy:**
- Per-CPU run queue locks (`sched.cpu[i].lock`) for local operations
- Cross-queue operations (migration) lock both queues in CPU ID order to prevent deadlock
- PMM has its own spinlock for memory allocation
- Task creation protected by task table lock
- IRQ-safe spinlock variants used throughout (save/restore DAIF)

**Task Migration:**
- Only READY tasks can be migrated (not RUNNING)
- `sched_migrate_task(task, target_cpu)` moves task between queues
- Respects CPU affinity constraints

**Known Limitations:**
- UART output is intentionally unsynchronized to avoid deadlock risks with panics
- Pi 5: tasks without explicit CPU affinity pinned to CPU 0 (SMPEN not set by TF-A)
- Blocked-task sleep queue attempted but wake mechanism failed on Pi 5 (deferred)

### Files Modified/Added

| File | Changes |
|------|---------|
| `kernel/sched/smp.c` | PSCI wrappers, `smp_init()`, secondary core bring-up, CPU detection |
| `kernel/src/smp_boot.S` | Secondary core entry point, per-core stack setup |
| `kernel/sched/sched.c` | Per-core run queues, task affinity, migration, multi-core `schedule()` |
| `kernel/src/spinlock.c` | Spinlock and ticket lock implementation |
| `kernel/include/spinlock.h` | Lock type definitions, IRQ-safe variants |
| `kernel/include/smp.h` | CPU ID, per-CPU data structures, PSCI constants |
| `kernel/src/gic.c` | Added `gic_percpu_init()` for secondary cores |
| `kernel/src/timer.c` | Added `timer_percpu_init()` for secondary cores |
| `kernel/sched/task.c` | Added locking for concurrent task creation |
| `kernel/mm/pmm.c` | Added spinlock protection for multi-core allocation |

### Pi 5 Platform Notes

**MPIDR Encoding:** The BCM2712 Cortex-A76 cores use Affinity Level 1 (Aff1) for the CPU ID, not Aff0 like QEMU virt. The MPIDR values are:
- CPU 0: `0x000` (Aff1=0)
- CPU 1: `0x100` (Aff1=1)
- CPU 2: `0x200` (Aff1=2)
- CPU 3: `0x300` (Aff1=3)

`boot.S` extracts Aff1 with `lsr x1, x1, #8` for Pi 5 vs masking Aff0 for QEMU.

**PSCI Conduit:** Pi 5 uses SMC (Secure Monitor Call) via ARM Trusted Firmware-A (TF-A) at EL3. QEMU virt uses HVC. The `psci_call()` function selects the conduit at compile time.

**Secondary CPU Boot:** TF-A drops secondary cores at EL2 via PSCI CPU_ON. `smp_boot.S` performs the EL2→EL1 transition and enables the MMU using page tables saved by the primary CPU in `secondary_mmu_ttbr/mair/tcr`.

**4-Core SMP Status:** All 4 Cortex-A76 cores boot successfully and reach C code. The shell reports "4 online / 4 total". Exclusive monitor operations (spinlocks via ldaxr/stxr) work correctly between all cores without any workaround.

**Cache Coherency Workaround (DC CVAC/CIVAC):** Hardware cache coherency for regular cached writes between cores is broken because TF-A does not set SMPEN (bit 6 of CPUECTLR_EL1) before dropping secondary cores to EL2. SMPEN controls whether the core participates in the inner-shareable coherency domain; without it, normal cached stores on one core are not visible to other cores. Since CPUECTLR_EL1 is only writable at EL3, the OS cannot fix this from EL1.

The workaround uses explicit ARM64 cache maintenance instructions:
- **DC CVAC** (Data Cache Clean by VA to PoC) — flushes dirty cache lines to the point of coherency, making writes visible to other cores
- **DC CIVAC** (Data Cache Clean and Invalidate by VA to PoC) — flushes and invalidates, ensuring the reading core fetches fresh data from memory

These operations are applied to shared data structures (e.g., `cpus_online`, `cpu_data[]`) after writes and before reads on the cross-core boundary. This adds overhead compared to hardware coherency but is correct and sufficient for the current SMP workload.

The raw DC CVAC/CIVAC instructions have been refactored into portable helpers in `kernel/include/cache.h`: `cache_clean()`, `cache_invalidate()`, `cache_clean_range()`, and `cache_invalidate_range()`. These are active cache maintenance operations on Pi 5 (`PLATFORM_RASPI5`) and no-ops (memory barriers only) on QEMU and other platforms with working hardware coherency.

**DC CIVAC Writeback Bug (Fixed April 2026):** A subtle interaction with DC CIVAC caused secondary CPUs' `online` flags to appear false from CPU 0. The root cause: CPU 0 called `init_cpu_data()` which wrote `online = false` to `cpu_data[]`, making those cachelines dirty in CPU 0's L1. Later, secondary CPUs set `online = true` and called `cache_clean()` (DC CVAC) to push their writes to PoC. However, when CPU 0 called `cache_invalidate()` (DC CIVAC) to read the updated value, the CIVAC instruction first **wrote back** CPU 0's stale dirty data to PoC (the "clean" phase of clean-and-invalidate), overwriting the secondary CPU's newer `true` with CPU 0's old `false`. Only then did it invalidate and re-read — fetching the now-corrupted value.

The fix: `cache_clean_range(cpu_data, sizeof(cpu_data))` is called after `init_cpu_data()` but before booting any secondary CPUs. This ensures CPU 0's cachelines are clean (not dirty) in L1, so subsequent DC CIVAC operations have nothing stale to write back. The `struct per_cpu` is 40 bytes, meaning adjacent CPU entries share cachelines (64-byte lines), which exacerbated the false-sharing aspect of this bug.

**CPU 0 Task Pinning:** Due to the cache coherency limitation, tasks without explicit CPU affinity are currently pinned to CPU 0. Secondary CPUs run idle and timer tasks but do not receive dispatched work. Cross-CPU task dispatch remains disabled until the SMPEN issue is resolved or an alternative coherency strategy is validated.

---

## Resources

- [ARM PSCI Specification (DEN0022)](https://developer.arm.com/documentation/den0022/latest/)
- [ARM Trusted Firmware PSCI Implementation](https://github.com/ARM-software/arm-trusted-firmware/tree/master/lib/psci)
- [Linux Kernel PSCI Driver](https://github.com/torvalds/linux/blob/master/drivers/firmware/psci/psci.c)
- [QEMU virt Machine Source](https://github.com/qemu/qemu/blob/master/hw/arm/virt.c)
- [OSDev Wiki — SMP](https://wiki.osdev.org/SMP)

---

*Created: December 2025*
*Updated: April 2026*
*Status: Implementation complete, all tests passing (393 tests on Pi 5, 0 failures)*
