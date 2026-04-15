# Kernel-Specific Notes

Notes for working on the C kernel code.

---

## Critical Struct Layout Rules

### struct task Field Ordering

**CRITICAL:** The `context` field in `struct task` MUST remain at offset 0x20. The assembly code in `context.S` has a hardcoded `TASK_CONTEXT_OFFSET = 0x20`.

If you need to add new fields to `struct task`:
- Add them AFTER the `context` field, not before
- Or update `TASK_CONTEXT_OFFSET` in `context.S` to match

If the offset is wrong, context switches will corrupt memory and cause crashes (typically instruction abort at address 0x0).

---

## Assembly/C Interface

### Struct Offsets in Assembly

**Issue:** Hardcoded struct offsets in assembly files (like `context.S`) must match the actual C struct layout. When C structs change — especially due to alignment attributes like `alignas(16)` — the assembly offsets become stale and cause subtle bugs (typically alignment faults jumping to exception vectors).

**Example:** Adding `alignas(16)` to a struct member inserts padding, shifting all subsequent field offsets.

**Prevention:**
- When modifying structs accessed by assembly, update the corresponding `#define` offsets in `.S` files
- Consider using `offsetof()` in C to generate offsets and pass to assembly
- Add comments linking assembly offsets to their C struct definitions

**Affected files:**
- `kernel/include/task.h` — defines `struct cpu_context` and `struct task`
- `kernel/src/context.S` — has `CTX_*` and `TASK_CONTEXT_OFFSET` defines

---

## C23 Freestanding Headers

In bare-metal code, only these standard headers are safe (no libc required):

| Header | Provides |
|--------|----------|
| `<float.h>` | Floating-point limits |
| `<iso646.h>` | Alternative operator spellings |
| `<limits.h>` | Integer limits |
| `<stdarg.h>` | `va_list`, `va_start`, `va_arg`, `va_end` |
| `<stddef.h>` | `NULL`, `nullptr`, `size_t`, `ptrdiff_t`, `offsetof` |
| `<stdint.h>` | `uint32_t`, `int64_t`, `uintptr_t`, etc. |

**C23 keywords (no header needed):**
- `bool`, `true`, `false` — boolean type and constants
- `alignas`, `alignof` — alignment specifiers
- `nullptr` — type-safe null pointer
- `static_assert` — compile-time assertions

**NOT safe:** `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>` — these require libc.

---

## C23 Strict Compliance

The kernel is compiled with `-std=c23 -Wpedantic` (no GNU extensions). This means:

**Use `__asm__` instead of `asm`:**
```c
/* Correct */
__asm__ volatile("wfi");

/* Incorrect - will not compile */
asm volatile("wfi");
```

**Use `__asm__` for register constraints:**
```c
/* Correct */
register uint64_t x0 __asm__("x0") = value;

/* Incorrect */
register uint64_t x0 asm("x0") = value;
```

**C23: `alignas` is now a keyword (no header needed):**
```c
/* C23 - alignas works directly */
alignas(16) uint8_t buffer[64];

/* _Alignas also still works */
_Alignas(16) uint8_t buffer[64];
```

**C23: Use `__VA_OPT__` for variadic macros:**
```c
/* Correct - C23 standard */
#define INFO(fmt, ...) uart_printf("[INFO] " fmt "\n" __VA_OPT__(,) __VA_ARGS__)

/* Incorrect - GNU extension, triggers -Wpedantic warning */
#define INFO(fmt, ...) uart_printf("[INFO] " fmt "\n", ##__VA_ARGS__)
```

---

## Cache Maintenance (Pi 5 / No SMPEN)

On Pi 5, TF-A does not set SMPEN for secondary cores. Use `cache.h` helpers for cross-CPU data:

**Critical rule: DC CIVAC writes back dirty data before invalidating.**

If CPU 0 has a dirty cacheline and you call `cache_invalidate()` (DC CIVAC), it first writes CPU 0's stale data to PoC, overwriting any newer value written by another CPU. Always `cache_clean()` or `cache_clean_range()` shared data on the writer side before another CPU uses `cache_invalidate()` to read it.

**Pattern for cross-CPU data init:**
```c
/* CPU 0: initialize shared data, then clean before secondary boot */
for (uint32_t i = 0; i < cpu_count; i++)
    init_data(i);
cache_clean_range(shared_data, sizeof(shared_data));

/* Secondary CPU: write, then clean */
shared_data[cpu].field = value;
cache_clean(&shared_data[cpu].field);

/* CPU 0 (reader): invalidate, then read */
cache_invalidate(&shared_data[cpu].field);
val = shared_data[cpu].field;
```

**False sharing:** `struct per_cpu` is 40 bytes. Adjacent entries share 64-byte cachelines. A dirty write to `cpu_data[0]` can be written back by CIVAC when invalidating `cpu_data[1]`.

---

## Non-Cacheable Shared Memory (Pi 5 + Jetson)

On real ARM64 hardware (Pi 5, Jetson), per-core L2 caches are incoherent despite SMPEN. DC CIVAC doesn't propagate through per-core L2. Non-cacheable (NC) memory bypasses L1/L2 entirely, making writes instantly visible to all CPUs. Enabled when `PLATFORM_HAS_NC_MEMORY` is defined (set in `ncmem.h`).

**NC region:** Platform-specific 2MB block mapped as MAIR index 2 (Normal Non-Cacheable, Inner Shareable) via L2 table entry in `vmm.c`. Reserved from PMM in `pmm.c`.
- Pi 5: `0xFFE00000` (last 2MB of 4GB RAM)
- Jetson: `0xBDE00000` (last 2MB of region 1, before OP-TEE carveout)

**Allocator:** `ncmem_alloc(size, align)` in `kernel/include/ncmem.h` / `kernel/mm/ncmem.c`. Simple bump allocator, no free. Used for permanent kernel-lifetime structures.

**Scheduler run queues:** `cpu_rq(cpu)` returns an NC address computed from `NC_MEM_BASE` (compile-time constant). No cacheable pointer indirection — secondary CPUs can compute the address without reading any cacheable data.

**Spinlock separation:** `rq_lock[MAX_CPUS]` is a separate cacheable array. ARM exclusive load/store (`ldaxr`/`stxr`) used by spinlocks requires cacheable memory on BCM2712. The lock is NOT in the `cpu_runqueue` struct — it's accessed via `rq_lock_irqsave(cpu)` / `rq_unlock_irqrestore(cpu, flags)`.

**Same pattern for work-stealing:** `steal_deque_lock[MAX_CPUS]` in `kernel/sched/sched.c` is also a separate cacheable array. The deque struct itself (`steal_deque_t`) lives in NC memory on `PLATFORM_HAS_NC_MEMORY` for cross-CPU visibility, but the lock guarding it must be cacheable — LDAXR/STXR on NC memory is never executed. Pre-S4 the lock was embedded in `steal_deque_t` in NC memory, which caused Pi 5's WORK_STEALING=ON boot to hang (#158) because concurrent pushes corrupted the deque. Acquire `steal_deque_lock[cpu]` before every `steal_deque_*` call; the deque's own API is lockless. See `docs/scheduler.md` §Work stealing for the full model.

**Spinlock hardware mode (Pi 5 + Jetson):** Both platforms now share the same `spinlock_hw_enabled` runtime-flag model (set by `vmm_init` after MMU enable). Pre-MMU the flag is 0 and every `spin_lock_irqsave` falls through to a barrier-only path. Post-MMU it is 1, so LDAXR/STXR run for real and provide proper cross-CPU mutual exclusion on cacheable memory. Jetson previously defined `SPINLOCK_SKIP_LOCKING` unconditionally in `platform.h`, which silently turned every cacheable spinlock (PMM, task-table, rq_lock, steal_deque_lock) into a no-op. That surfaced as issue #166 — a `pmm_free_pages` free-list page fault during `bench stealing` with WORK_STEALING=ON, because multiple CPUs mutated the buddy free list without serialization. If you need a cross-CPU critical section on Jetson, a plain cacheable `spinlock_t` + `spin_lock_irqsave` is the right primitive (not barrier-only, not NC-embedded).

**Task table:** `task_table` is allocated from NC memory at boot via `task_table_init()` in `task.c`. All task struct fields are NC-visible. The `task_table_fallback[MAX_TASKS]` BSS array is used on platforms without NC memory. Task STACKS remain in cacheable PMM (only accessed by owning CPU).

**Current-task pointers:** `current_task[MAX_CPUS]` is also allocated from NC memory in `task_table_init()` (SCHED-C2 fix, April 2026). Previously this lived in BSS and relied on DC CIVAC for cross-CPU visibility, which has a known failure mode on Pi 5 (writer's stale cacheline can be written back during an invalidate, clobbering a newer cross-CPU write). NC relocation makes writes from one CPU's `task_set_current` instantly visible to other CPUs' `task_current` without cache maintenance. `current_task_fallback[MAX_CPUS]` is the BSS fallback if NC allocation fails.

**When to use NC memory:** Only for data that MUST be visible across CPUs without cache maintenance. NC memory is slower than cached memory (every access goes to DRAM). Do not use for hot-path per-CPU data.

**NC memory layout** (allocator is a bump allocator — offsets shown are approximate, based on allocation order in `scheduler_init` then `task_table_init`):
| Contents | Size | Notes |
|---|---|---|
| `cpu_runqueue[MAX_CPUS]` | ~256B | First alloc; address pinned via `cpu_rq()` computing from `NC_MEM_BASE` |
| `nc_cpu_logical_map[MAX_CPUS]` | ~32B | MPIDR→logical CPU map |
| `sched_diag_*[MAX_CPUS]` | ~1KB | Diagnostic counters (tick, schedule, picked, idle_loops) |
| `task_table[MAX_TASKS]` | ~24KB | All task structs |
| `current_task[MAX_CPUS]` | ~32B | Per-CPU current running task pointer |
| (Trace slots at `NC_MEM_SIZE - 256`) | 256B | Reserved for `nc_trace.h` diagnostics (not allocated) |

**Cross-CPU dispatch status (April 10, 2026):** Working via cooperative scheduling (WFE/SEV). NC run queues and task table in use. CPU 0 pinning removed, round-robin load balancing enabled across all 4 CPUs. Tasks dispatched to secondary CPUs complete successfully (`bench smp` validates). Timer-based preemption on secondary CPUs still under investigation (IRQ handler hang). CPU 0 timer preemption works via idle task `daifclr` + `wfi`. All tasks run with `DAIF.I=1` (no in-task timer preemption). Full test suite completes on Pi 5 (~14s, 5 multi-core integration test failures expected). See `docs/pi5-cross-cpu-dispatch-investigation.md` for full history.

---

## Idle Task DAIF

The idle task's `msr daifclr, #2` (IRQ unmask) must be **inside** the `while(1)` loop, not before it. When idle is preempted by the timer ISR, ARM hardware masks IRQ on exception entry. `context.S` saves this masked DAIF into idle's context. On resume, the restored DAIF keeps IRQ masked. If the unmask is only at function entry, idle would loop forever in `wfi` with IRQ disabled.

**Pi 5/Jetson platform split:** CPU 0's idle task does `daifclr` + `wfi` (timer-driven preemption). Secondary CPUs use `wfe` only (cooperative via SEV) because timer IRQs on secondary CPUs cause an exception handler hang (under investigation). This means `pit_ticks` only advances when CPU 0 is idle.

---

## ARM64 Hardware Timer IRQs — cooperative preemption (April 2026)

**Hardware timer IRQs do not deliver to EL1/EL2 on Pi 5 or Jetson.** The GIC in both platforms runs with two security states; the Group register that routes a PPI to IRQ vs FIQ is owned by EL3 firmware and Non-secure writes are silently ignored. Pi 5 evidence: `docs/pi5-preemption-resolution.md`. Jetson evidence: PR #138 (`timer_handler_count: 0` pre-fix, `[JDIAG]` confirming `GICR_IGROUPR0` stays at `0x0` after our NS write).

**Resolution — `COOP_PREEMPT` (CMake option, default `ON` for RASPI5 and JETSON_ORIN_NANO):** `schedule()` checks `CNTPCT_EL0` on every entry and synthesizes a `scheduler_tick()` call whenever ≥10 ms has elapsed on that CPU since the last tick. See `coop_preempt_maybe_tick()` in `kernel/sched/sched.c`. This drives the AI scheduler, deadline boosts, migration, and `pit_ticks` / `timer_handler_count` observability at yield points rather than preemptively.

The `PI5_COOP_PREEMPT` spelling is retained as a deprecated Makefile/CMake alias for backward compatibility; source code uses `COOP_PREEMPT` everywhere.

**Consequences:**
- `pit_ticks` and `timer_handler_count` advance on all CPUs when those CPUs yield.
- A pure CPU-bound loop that never yields still monopolizes its CPU. The `delay()` helper in `kernel/tests/test_integration.c` yields every ~1k iterations for this reason.
- `timer_get_count()` (CNTPCT_EL0) remains the authoritative wall-clock source for timeouts; see `hw_timeout_start()` / `hw_timeout_expired()` in `component_runtime.c`. Works regardless of IRQ delivery state.

**`sched_set_policy()` must hold IRQs disabled.** The function wraps init/swap/shutdown in `irq_save`/`irq_restore`. Retained because the ELR-trampoline path (PR #98, inert under coop-preempt but kept for future hardware-IRQ restoration) still relies on it.

---

## Secondary-CPU preemption — `SECONDARY_PREEMPT` (April 2026)

The ELR-trampoline infrastructure (`kernel/sched/preempt.c`,
`resched_trampoline` in `kernel/arch/arm64/vectors.S`) is gated behind
the CMake option `SECONDARY_PREEMPT` (default OFF). When ON, timer IRQs
on secondary CPUs are deferred to task context via the trampoline
rather than calling `schedule()` from the ISR (which corrupts the
abandoned exception frame on real ARM64 hardware).

**Invocation:**

- `make kernel SECONDARY_PREEMPT=ON` — enable on any ARM64 platform.
- `make kernel PI5_SECONDARY_PREEMPT=ON` — deprecated alias; still maps
  to `SECONDARY_PREEMPT=ON`. Pi 5 Makefiles that use the old name keep
  working.

**Platform status:**

- **Pi 5:** functional in combination with `COOP_PREEMPT` (the
  trampoline path is inert today because timer IRQs don't deliver;
  infrastructure kept for when #134 restores hardware IRQ delivery).
- **Jetson:** compiles but **not safe to enable yet** — the
  `resched_trampoline` in `vectors.S:377-380` uses
  `(mpidr & 0xFF) | ((mpidr >> 8) & 0xFF)` to compute the CPU index,
  which collides on dual-cluster CPU 4/5. Jetson plan P3 step 2 owns
  the fix. **Enforced at boot (#137):** `preempt_check_cpu_mpidr`
  (kernel/sched/preempt.c) panics from `scheduler_init` /
  `secondary_init` if the fold disagrees with the caller's logical
  CPU id, so the soft documentation warning can no longer be
  bypassed silently — a Jetson build with `SECONDARY_PREEMPT=ON`
  halts loudly on the first secondary bring-up.
- **QEMU:** works today but rarely needed — QEMU's timer IRQ from an
  ISR doesn't crash the kernel; the default cooperative path is fine.

The option was originally `PI5_SECONDARY_PREEMPT`; it was renamed
during the Jetson capstone Prereq #2 (commit `b820bee`) so Jetson
builds could compile the trampoline, which had been gated behind
`PLATFORM STREQUAL "RASPI5"`. See `docs/smp.md` §"Secondary-CPU
Preemption" for the full history.

---

## Cross-CPU notification — `smp_notify_cpu()`

Scheduler code calls `smp_notify_cpu(cpu)` to wake a specific CPU that
may have just gained work on its run queue. API in
`kernel/include/smp.h`, platform implementations split:

- **ARM64** (`kernel/sched/smp.c`): `sev` broadcast. SEV wakes every
  CPU in WFE and the target picks up the task on its next idle loop
  iteration. Upgrade to `gic_send_sgi(cpu, SGI_RESCHED)` once Jetson
  plan P3 step 3 fixes `gic_send_sgi` for dual-cluster MPIDR.
- **x86-64** (`kernel/arch/x86_64/platform_x86.c`): LAPIC IPI on
  `RESCHED_VECTOR` (49). The handler calls `schedule()` directly —
  not `scheduler_tick()` — so quantum accounting stays owned by the
  local timer. Runs on IST1 so a task-stack overflow cannot corrupt
  the IPI frame.

Both backends short-circuit self-notifications and out-of-range
`cpu` ids. Replaces the previous
`#if !defined(PLATFORM_X86_64) __asm__ volatile("sev") #endif` pattern
in `scheduler_add_task_to_cpu()`. Platform-specific wake logic no
longer leaks into `kernel/sched/sched.c`. See `docs/smp.md` §"IPI /
Cross-CPU Notification".

---

## UART Lock on Pi 5 / Jetson

On platforms with `PLATFORM_HAS_NC_MEMORY`, the UART lock uses **IRQ-disable-only** (no cross-CPU lock). Standard `ldaxr`/`stxr` spinlocks deadlock under cross-CPU contention because per-core L2 caches are incoherent (no SMPEN). LSE atomics (`SWPALB`) also operate through L2 and have the same problem. NC memory atomic ops may fault (implementation-defined per ARM ARM).

This is safe because secondary CPUs do not print after boot (the `secondary_init` comment at smp.c:328 documents this). All UART output during normal operation comes from CPU 0 (shell, tests, INFO logs). If secondary CPU printing is needed in the future, an NC-memory-based lock must be implemented.

---

## Platform Abstraction

- Use compile-time `#ifdef` for driver selection (UART, timer)
- Platform-specific values go in `kernel/include/platform.h`
- See `docs/platform-abstraction.md` for full strategy

---

## Buddy Allocator (PMM)

The physical memory manager uses a buddy allocator (`kernel/mm/pmm.c`).

### Key Implementation Details

**Orders and Block Sizes:**
- Order 0: 1 page (4KB)
- Order 18: 262144 pages (1GB) - maximum
- Total: 19 orders (0-18)

**Data Structures:**
```c
struct free_block {
    struct free_block *next;
    struct free_block *prev;  // Doubly-linked for O(1) removal
};

struct buddy_state {
    struct free_block *free_lists[MAX_ORDER + 1];  // One list per order
    size_t free_counts[MAX_ORDER + 1];             // Blocks free at each order
    // ... statistics
};
```

**Buddy Address Calculation:**
```c
// Two blocks are buddies if XOR gives the parent block address
uintptr_t buddy_addr = block_addr ^ (PAGE_SIZE << order);
```

**Allocation:**
1. Round request up to power of 2 (order = log2_ceil(count))
2. Search from target order up to MAX_ORDER for a free block
3. Split larger blocks recursively, adding smaller halves to free lists

**Freeing:**
1. Round count up to power of 2 to find block order
2. Check if buddy is free at same order (look it up in free list)
3. If buddy is free, remove it and merge into parent (order + 1)
4. Repeat until buddy is not free or MAX_ORDER reached

**Statistics API:**
```c
void pmm_get_buddy_stats(struct pmm_buddy_stats *stats);
// Returns free_counts[], alloc_count, free_count, split_count, merge_count
```

### Testing

Tests in `kernel/tests/test_pmm.c` verify:
- Basic alloc/free
- Power-of-two rounding
- Block splitting creates correct buddies
- Coalescing enables larger allocations
- Exhaustion and recovery
- Mixed workload stress

---

*Last updated: December 2025*
