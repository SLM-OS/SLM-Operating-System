# Pi 5 Cross-CPU Task Dispatch Investigation

**Date:** April 3–7, 2026
**Status:** ✅ CROSS-CPU DISPATCH WORKING. All 4 CPUs execute tasks. Cooperative scheduling via WFE/SEV. Timer-based preemption on secondary CPUs still blocked (IRQ handler hang under investigation).

---

## Problem Statement

All 4 Cortex-A76 cores boot successfully on Pi 5, but user tasks are pinned to CPU 0. Secondary CPUs (1–3) run idle tasks only. The goal is to enable the scheduler to dispatch tasks to any CPU for parallel SLM inference workloads.

---

## Timeline

| Date | Key Finding |
|------|-------------|
| April 3 | SMP boot working, 4 cores online. Cross-CPU dispatch blocked — tasks dispatched to secondary CPUs never consumed. |
| April 5 (AM) | SMPEN investigation: write to CPUECTLR_EL1 from EL2 traps to EL3 (hangs). Confirmed SMPEN=0 on all 4 CPUs. |
| April 5 (AM) | Cross-CPU dispatch attempted with DC CVAC/CIVAC cache maintenance. Failed — L2 retains stale data 100+ seconds. |
| April 5 (AM) | Reverted to CPU 0 pinning. All 6 TF-A/cache combinations tested, all fail. |
| April 5 (PM) | NC memory infrastructure implemented (2MB region, VMM L2 split, bump allocator). |
| April 5 (PM) | NC run queues validated. Secondary CPUs pick up tasks but crash (task struct in cacheable memory). |
| April 5 (PM) | Discovered `ldaxr`/`stxr` hangs on NC memory. Separated spinlocks from run queue struct. |
| April 5 (PM) | NC task table allocated. CPU 0 pinning still needed — removing it causes system hang. |
| April 6 (AM) | Per-CPU diagnostic counters added. Discovery: timer_handler() NEVER called. 0 ticks on all CPUs. |
| April 6 (AM) | Root cause: DAIF=0x080 permanently masked in all tasks. Only idle task unmasks. |
| April 6 (AM) | Secondary CPU boot bug found: double timer_start/daifclr before scheduler_start crashes boot stack. |
| April 6 (PM) | DAIF trampoline fix applied — QEMU passes, Pi 5 deadlocks after "SLM-OS Debug Shell". |
| April 6 (PM) | Reverted to stable state. Both DAIF fixes needed atomically but cause deadlock on Pi 5. |
| April 7 | SD card filename bug discovered — deployments were writing `slmos.bin` but Pi 5 loads `kernel_2712.img`. All prior deploys ran stale binary. |
| April 7 | MPIDR Aff0/Aff1 extraction bug: Pi 5 uses Aff1 (bits [15:8]), code extracted Aff0 — all CPUs identified as CPU 0. |
| April 7 | Stale `cpu_id()` on secondary CPUs: cacheable BSS reads return 0 without L2 coherency. Hardcoded MPIDR table fix. |
| April 7 | `scheduler_start()` changed to accept `cpu` parameter — eliminates internal `cpu_id()` call on secondary CPUs. |
| April 7 | `timer_start()` INFO print removed — uart_lock acquisition deadlocked secondary CPUs. |
| April 7 | NC diagnostic pointer NULL guard added — stale cacheable pointer caused crash in timer handler. |
| April 7 | Secondary CPUs now reach 0xC6 (about to unmask IRQs). Hang at daifclr — pending timer IRQ handler does not return. |
| April 7 | `timer_interval` stale on secondary CPUs — `write_cntp_tval(0)` causes infinite IRQ storm. Fixed: read CNTFRQ from system register. |
| April 7 | Workaround: skip daifclr on secondary CPUs, use WFE + SEV for cooperative scheduling. |
| April 7 | **BREAKTHROUGH: Cross-CPU dispatch working!** `bench smp` dispatches tasks to CPUs 1-3, all complete successfully. |
| April 7 | CPU 0 pinning removed, round-robin load balancing enabled across all 4 CPUs. |

---

## Phase 0: SMP Boot and Initial Dispatch Attempts (April 3)

### Starting Point

SLM-OS boots all 4 Cortex-A76 cores via PSCI CPU_ON (SMC to TF-A at EL3). Each secondary CPU transitions EL2→EL1, enables MMU (with L1/L2 invalidation to fix pre-MMU cache pollution), initializes GIC and timer, and enters the scheduler.

The scheduler supports per-CPU run queues with explicit CPU affinity. Cross-CPU dispatch uses `scheduler_add_task_to_cpu(task, cpu)` with explicit cache maintenance (DC CVAC on writer, DC CIVAC on reader).

### The SMPEN Investigation

Cortex-A76 was expected to have an SMPEN bit (like A53/A72) in CPUECTLR_EL1 (S3_0_C15_C1_4) that enables L1 data cache participation in the coherency domain. Investigation:

1. **Reading CPUECTLR_EL1 from EL1:** Works. Bit 6 = 0 on all 4 CPUs.
2. **Writing from EL2 (boot.S):** System hangs — write traps to EL3 (TF-A has no handler).
3. **Conclusion:** Cortex-A76 does NOT have an SMPEN bit. Bit 6 of S3_0_C15_C1_4 is a different field on A76. The DSU is supposed to handle coherency automatically per the ARM TRM.

### Cache Maintenance Attempt

Removed CPU 0 pinning and added extensive cache maintenance:
- Writer (CPU 0): `cache_clean(&rq->head)`, `cache_clean(&task->next)`, etc. after every cross-CPU write
- Reader (CPU N): `cache_invalidate_range(rq, sizeof(*rq))` before every queue read

**Result:** Tasks dispatched to secondary CPUs were NEVER consumed. The secondary CPUs' `schedule()` read stale `rq->head = NULL` from L2 despite DC CIVAC. Raw assembly test confirmed: DC CIVAC + LDR reads NULL even when the writer cleaned to PoC.

**Key finding:** DC CIVAC invalidates L1 but NOT L2 on Cortex-A76 without SMPEN. The per-core L2 retained stale data. Stale entries persisted 100+ seconds (until natural eviction under cache pressure).

### Revert

User chose "Revert to CPU 0 pinning (Recommended)". All user tasks pinned to CPU 0. Secondary CPUs run idle + handle timer interrupts. Cache maintenance code kept for future use.

---

## Phase 1: Cache Coherency (Solved)

### The L2 Incoherency Problem

BCM2712's TF-A firmware does not set SMPEN on secondary cores. Without SMPEN, per-core L2 caches are incoherent — writes from one CPU are invisible to others. DC CIVAC (clean+invalidate by VA) only flushes L1; L2 retains stale data for 100+ seconds.

**Tested and failed (6 combinations):**

| TF-A | Cache Ops | L2 Invalidate | Result |
|------|-----------|---------------|--------|
| Built-in EEPROM | DC CVAC/CIVAC | No | Fail |
| Built-in EEPROM | Barriers only | No | Fail |
| RPi Foundation (bcm2712) | Barriers only | No | Fail |
| RPi Foundation (bcm2712) | DC CVAC/CIVAC | No | Fail |
| Built-in EEPROM | Barriers only | Yes | Fail |
| Built-in EEPROM | DC CVAC/CIVAC | Yes | Fail |

### The NC Memory Solution

Non-cacheable (NC) memory bypasses L1/L2 entirely — writes go to DRAM and are instantly visible to all CPUs.

**Implementation (commits `0c22944`, `f683c77`):**
- 2MB NC region at PA `0xFFE00000` (last 2MB of 4GB RAM)
- VMM: Split L1[3] into L2 table (511 WB + 1 NC entry at L2[511])
- PMM: `heap_end` lowered to `0xFFE00000` (NC region excluded from buddy allocator)
- Allocator: bump allocator in `kernel/mm/ncmem.c` (`ncmem_alloc(size, align)`, no free)
- Scheduler run queues: `cpu_rq(cpu)` inline returns compile-time NC address (`NC_MEM_BASE + cpu * sizeof(cpu_runqueue)`) — no cacheable pointer indirection
- Spinlocks: separated into cacheable `rq_lock[MAX_CPUS]` array because `ldaxr`/`stxr` requires cacheable memory on BCM2712
- Task table: `task_table[MAX_TASKS]` (32 * 768B = 24KB) allocated from NC at boot

**Key finding: `ldaxr`/`stxr` on NC memory hangs.** The ARM exclusive monitor on BCM2712 requires cacheable memory for exclusive load/store. Spinlocks in `struct cpu_runqueue` caused boot hang. Fixed by moving locks to a separate cacheable array.

**Validated on hardware:**
- `test_nc_memory_accessible` — NC read/write works
- `test_nc_alloc_alignment` — alignment guarantees
- `test_nc_region_used_by_scheduler` — scheduler uses NC
- `test_bar3_mip0_routing` — BAR3 register readback matches expected values

### Why NC Alone Isn't Enough

With NC run queues + NC task table + CPU 0 pinning removed, secondary CPUs picked up dispatched tasks (garbled UART output proved multi-CPU activity) but the system crashed. Tasks dispatched to secondary CPUs never completed.

This led to Phase 2: investigating WHY secondary CPUs don't process dispatched work.

---

## Phase 2: Timer Interrupt Discovery (Root Cause Found)

### The Diagnostic

Added per-CPU counters to `scheduler_tick()`, `schedule()`, and `pick_next_task()`, visible via the `cpu` shell command:

```
Per-CPU scheduler diagnostics:
CPU  Ticks     Schedule  Picked
---  --------  --------  ------
  0         0  11146796  11146797   ← 11M schedule() calls, ALL from yield()
  1         0         0       0    ← ZERO everything
  2         0         0       0
  3         0         0       0
```

**Timer ticks = 0 on ALL CPUs.** `timer_handler()` is NEVER called. `schedule()` on CPU 0 is called 11M times exclusively through `yield()` in the shell's polling loop. Secondary CPUs are stuck in `wfi` and never wake.

### Root Cause: DAIF Permanently Masked

Tasks are created with `context.daif = 0x080` (IRQ masked) in `task_create_with_priority()`. This is correct — it prevents timer interrupts from corrupting the partially-restored context during the first `context.S` switch.

**The bug:** tasks never unmask IRQs after creation. The comment in `task.c` says "task code unmasks naturally via `spin_unlock_irqrestore` or explicit DAIF clear" — but no task code actually does this. The idle task unmasks in its `while` loop (`msr daifclr, #2`), but on CPU 0 the shell task runs permanently via `yield()` polling, so the idle task never executes.

The `scheduler_start()` function does `msr daifclr, #0x2` on the boot stack before `switch_to()`. This was always a latent bug — the x86-64 port has a comment: "Don't STI here — switch_to will load the task's rflags (IF=0), and task_entry_wrapper does STI after the context is fully set up. Enabling interrupts before switch_to would allow a timer IRQ to fire while still on the boot stack, corrupting the task context." The ARM64 code ignores this warning.

### Secondary CPU Boot Bug

Secondary CPUs had a double initialization in `secondary_init()` (smp.c):
1. `timer_start()` — starts the timer
2. `msr daifclr, #0x2` — unmasks IRQs
3. `scheduler_start()` — which ALSO does timer_start() + daifclr

Between steps 2 and 3, the timer fires on the boot stack with `task_current()` returning NULL (not yet set by `scheduler_start()`). This crashes the secondary CPU silently.

**Fix (commit `42542f4`):** Removed `timer_start()` and `daifclr` from `secondary_init()`. `scheduler_start()` handles both for all CPUs.

### The DAIF Fix and Its Deadlock

**The correct fix:** Unmask IRQs in `task_entry_trampoline()` (after context restore) and remove the boot-stack `daifclr` from `scheduler_start()`.

**What happens when applied:**
- QEMU: all tests pass (preemptive scheduling already worked via different path)
- Pi 5: boots, "SLM-OS Debug Shell" prints, then deadlocks

**Observed behavior:**
- Secondary CPUs are active (garbled UART output from multi-CPU contention)
- Shell task runs (prints banner) but hangs before the prompt
- System becomes completely unresponsive

**Hypothesized deadlock cause:** Timer fires during `uart_puts()` output, preempts the shell task, `schedule()` runs, finds no other work, switches back to shell. But something in this cycle corrupts the UART polling state or causes a lock ordering issue between `uart_lock` (in kprintf.c) and `rq_lock` (in sched.c). The `uart_lock` uses `spin_lock_irqsave` which masks IRQs, so the timer shouldn't fire while printing. The deadlock may be elsewhere.

**Investigation areas:**
1. Is `schedule()` re-entrant when called from timer during `yield()`?
2. Does the context switch corrupt the UART flag register polling state?
3. Is there a lock ordering issue between `uart_lock` and `rq_lock`?
4. Does `pick_next_task()` correctly re-queue the current task?

---

## Phase 3: Secondary CPU Exception Handler Investigation (April 7, 2026)

### Session Summary

This session made major breakthroughs in cross-CPU dispatch. Multiple root causes were identified and fixed, advancing secondary CPUs from "stuck at polling loop" to "stuck at IRQ unmask" — almost through the full scheduler_start path.

### Root Cause 1: Wrong SD Card Filename (Deployment Bug)

Pi 5's `config.txt` specifies `kernel=kernel_2712.img`. The `sdwire_update` command was copying the build artifact as `slmos.bin`, which the firmware ignored. Every deployment for multiple sessions was writing to the wrong file — the Pi 5 was running a stale binary from the initial SD card setup.

**Fix:** Changed all sdwire_update calls to copy as `kernel_2712.img`.

**Impact:** Hours of debugging against stale NC_DBG/boot_flag values that appeared unchanging. Distinctive marker values (0x44, 0xBD, 0x33) confirmed the old binary was running despite successful copy.

### Root Cause 2: MPIDR Aff0/Aff1 Extraction Bug

Pi 5 BCM2712 MPIDR encoding uses Aff1 (bits [15:8]) for CPU index. All 4 CPUs have Aff0=0:
- CPU 0: MPIDR=0x80000000 → Aff0=0, Aff1=0
- CPU 1: MPIDR=0x80000100 → Aff0=0, Aff1=1
- CPU 2: MPIDR=0x80000200 → Aff0=0, Aff1=2
- CPU 3: MPIDR=0x80000300 → Aff0=0, Aff1=3

Code using `mpidr & 0xFF` (Aff0) wrote to CPU 0's slot for ALL CPUs. Affected:
1. `task_entry_trampoline()`: `preempt_disabled[aff0]` always cleared CPU 0's slot, never the correct CPU's
2. `idle_task_func()`: NC counter incremented CPU 0's diagnostic slot only
3. NC debug traces in `scheduler_start()`: all wrote to slot 0

**Fix:** Changed extraction to `(mpidr & 0xFF) | ((mpidr >> 8) & 0xFF)` — works for both QEMU (Aff0 encoding) and Pi 5 (Aff1 encoding) since only one field is non-zero.

### Root Cause 3: Stale `cpu_id()` on Secondary CPUs

`cpu_logical_id()` reads `cpu_logical_map[]` and `cpu_count` from cacheable BSS. Without L2 coherency, secondary CPUs get stale data:
- `cpu_count` may read as 0 or 1 (BSS initial value in DRAM, CPU 0's update stuck in L2)
- `cpu_logical_map[]` may have zeros (BSS default)
- Result: `cpu_logical_id()` returns -1, `cpu_id()` returns 0 (fallback)

This caused secondary CPUs to operate as CPU 0, locking CPU 0's run queue and creating deadlocks.

**First attempt:** Use NC copy (`nc_cpu_logical_map`). Failed because the NC pointer itself is in cacheable BSS — secondary CPUs read NULL.

**Fix:** Hardcoded MPIDR table for Pi 5 (same pattern as Jetson):
```c
static const uint64_t pi5_map[] = { 0x000, 0x100, 0x200, 0x300 };
```
This avoids ALL cacheable memory reads during CPU identification.

### Root Cause 4: `scheduler_start()` Used `cpu_id()` Internally

`scheduler_start(void)` called `cpu_id()` to determine which CPU it was running on. On secondary CPUs, this returned 0 (stale), causing scheduler operations on the wrong run queue.

**Fix:** Changed `scheduler_start(void)` to `scheduler_start(uint32_t cpu)` — callers pass the known CPU ID:
- `main.c`: passes 0
- `smp.c`: passes `logical_cpu_id` (known correct from PSCI boot)
- `platform_x86.c`: passes 0

### Root Cause 5: `timer_start()` INFO Print Deadlock

`timer_start()` called `INFO("Timer started (%d Hz)")` which acquires `uart_lock` via `spin_lock_irqsave()`. On secondary CPUs, if CPU 0 holds uart_lock, the ldaxr reads stale "locked" from L2 and spins forever.

**Fix:** Removed the INFO print from `timer_start()`. CPU 0's scheduler_start prints its own "Starting timer" message.

### Root Cause 6: Stale NC Diagnostic Pointer Crash

`scheduler_tick()` accesses `sched_diag_tick[cpu]++` where `sched_diag_tick` is a cacheable pointer to NC memory. Secondary CPUs read NULL (stale BSS default from DRAM), causing a NULL pointer dereference in the timer handler.

**Fix:** Added NULL guard for NC pointer access on Pi 5.

### Current State After Fixes

NC trace markers show secondary CPUs progress through scheduler_start:
```
0xBD = post-polling-loop (exits NC flag wait)
0xBC = returned from scheduler_init_secondary
0xBE = about to call scheduler_start
0xC0 = entered scheduler_start
0xC1 = passed sched.initialized check
0xC2 = acquired rq_lock
0xC3 = picked idle task
0xC4 = unlocked rq, about to start timer
0xC5 = timer started
0xC6 = about to unmask IRQs (daifclr)
--- HANG: daifclr triggers pending timer IRQ, handler never returns ---
```

**The current blocker:** When secondary CPUs execute `msr daifclr, #0x2` (unmask IRQs), a pending timer interrupt fires immediately. The exception handler path on secondary CPUs does not return. The next investigation step is the IRQ handler / exception vector code for secondary CPUs.

### NC_DBG Diagnostic Legend (updated)
| Value | Meaning | Location |
|-------|---------|----------|
| 0xAA | Pre-polling marker | smp.c, before scheduler_is_initialized loop |
| 0xBD | Post-polling, about to call scheduler_init_secondary | smp.c |
| 0xBC | Returned from scheduler_init_secondary | smp.c |
| 0xBE | About to call scheduler_start | smp.c |
| 0xC0 | Entered scheduler_start | sched.c |
| 0xC1 | Passed initialized check | sched.c |
| 0xC2 | Acquired rq_lock | sched.c |
| 0xC3 | Picked a task | sched.c |
| 0xC4 | Released lock, about to start timer | sched.c |
| 0xC5 | Timer started | sched.c |
| 0xC6 | About to unmask IRQs | sched.c |
| 0xC7 | Post-daifclr (IRQs unmasked) | sched.c |
| 0xCC | About to call switch_to | sched.c |
| 0xDD | task_entry_trampoline reached | task.c |
| 0xE1 | Panic: not initialized | sched.c |
| 0xE2 | Panic: no tasks | sched.c |

---

## Current State

### What's Working
- NC run queues (run queue metadata visible cross-CPU) ✅
- NC task table (all task struct fields visible cross-CPU) ✅
- NC spinlock separation (locks in cacheable BSS, data in NC) ✅
- BAR3→MIP0 routing for UART IRQ (proven by IRQ storm test) ✅
- Per-CPU diagnostic counters in NC memory ✅
- Secondary CPU boot fix (no double timer_start) ✅
- Root cause identified (DAIF never unmasked in tasks) ✅
- Fix works on QEMU ✅
- SD card deploy filename corrected (`kernel_2712.img`) ✅
- MPIDR Aff1 extraction for Pi 5 (works for both QEMU and Pi 5) ✅
- Hardcoded MPIDR table eliminates cacheable BSS dependency for cpu_id ✅
- `scheduler_start(uint32_t cpu)` — callers pass known CPU ID ✅
- `timer_start()` INFO print removed (uart_lock deadlock on secondary CPUs) ✅
- NC diagnostic pointer NULL guard ✅
- Secondary CPUs progress through scheduler_start to IRQ unmask (0xC6) ✅

### What's Blocked
- **Secondary CPU IRQ handler hang** — `daifclr` triggers pending timer IRQ, exception handler never returns. Next step: investigate exception vector / IRQ handler path on secondary CPUs.

### NC Memory Layout
```
NC_MEM_BASE (0xFFE00000):
  +0x000:  cpu_runqueue[4]        (256B)
  +0x100:  task_table[32]         (24KB)
  +0x6100: sched_diag_tick[4]     (16B)
  +0x6140: sched_diag_schedule[4] (16B)
  +0x6180: sched_diag_picked[4]   (16B)
  ...      available for future NC allocations
Total used: ~25KB / 2MB (1.2%)
```

### Key Code Locations
| Component | File | Function/Line |
|-----------|------|---------------|
| Task DAIF init | `kernel/sched/task.c` | `task_create_with_priority()` ~line 251 |
| Task entry unmask | `kernel/sched/task.c` | `task_entry_trampoline()` (TODO) |
| Boot-stack unmask | `kernel/sched/sched.c` | `scheduler_start()` ~line 900 |
| Idle task unmask | `kernel/sched/sched.c` | `idle_task_func()` ~line 125 |
| Secondary boot | `kernel/sched/smp.c` | `secondary_init()` ~line 325 |
| NC run queue | `kernel/sched/sched.c` | `cpu_rq()` inline |
| NC task table | `kernel/sched/task.c` | `task_table_init()` |
| NC allocator | `kernel/mm/ncmem.c` | `ncmem_alloc()` |
| Spinlock separation | `kernel/sched/sched.c` | `rq_lock[]`, `rq_lock_irqsave()` |
| Diagnostics | `kernel/sched/sched.c` | `sched_diag_tick[]` etc. |
| UART lock | `kernel/src/kprintf.c` | `uart_lock` (spin_lock_irqsave) |

### Commits (chronological)
| Hash | Description |
|------|-------------|
| `41120ac` | Boot reliability (92/92) and benchmark results |
| `0c22944` | NC shared memory infrastructure (VMM L2 split, PMM reservation, ncmem) |
| `4d09f4b` | NC memory tests and documentation |
| `f683c77` | NC task table for cross-CPU visibility |
| `4f7b8dd` | Cross-CPU dispatch investigation docs |
| `767cec0` | Remove superseded Pi 5 docs |
| `a147962` | Fix stale references |
| `75174d7` | UART IRQ: Linux register dump reveals BAR3 fix |
| `207daf6` | Fix UART IRQ: BAR3 for MSI-X routing |
| `3c4f178` | Restore IACK_EN, confirm BAR3 path works |
| `ac46e40` | Identify root cause: DAIF permanently masked |
| `42542f4` | Fix secondary CPU boot: remove double timer_start |

---

## Next Steps

1. **Debug the secondary CPU IRQ handler hang.** When secondary CPUs execute `msr daifclr, #0x2`, a pending timer IRQ fires immediately and the handler never returns. Add NC trace markers inside the exception vector entry (`vectors.S`), the IRQ dispatcher, and `scheduler_tick()` to identify where the handler stalls. Likely candidates:
   - Exception vector jumps to wrong address (stale vector base from cacheable memory)
   - `scheduler_tick()` calls `cpu_id()` which returns 0 (stale) — deadlocks on CPU 0's rq_lock
   - GIC IAR read returns spurious INTID on secondary CPUs (GIC per-CPU interface not properly initialized)
   - `task_current()` reads stale cacheable pointer, causing crash in context save

2. **Alternative: cooperative multi-CPU.** Instead of full preemption, keep tasks with IRQs masked but have idle tasks wake via timer. When CPU 0 dispatches a task to CPU 1, CPU 1's idle wakes on the next timer tick, calls `schedule()`, picks up the task. This avoids the IRQ handler issue while still enabling multi-core execution.
