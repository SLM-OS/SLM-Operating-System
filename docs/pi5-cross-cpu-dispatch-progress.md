# Cross-CPU Dispatch Debug Progress Log

> **Note:** This file is a historical debug log from April 6, 2026. Cross-CPU dispatch was resolved on April 7, 2026. See `pi5-cross-cpu-dispatch-investigation.md` for the full investigation and resolution.

**Started:** April 6, 2026
**Resolved:** April 7, 2026
**Goal:** Enable multi-core task dispatch on Pi 5

---

## Iteration 1: Fix NC diagnostic readback

**Problem:** NC diagnostic counters showed NC addresses instead of values.
**Cause:** `shell_sys.c` didn't include `ncmem.h`, so `PLATFORM_HAS_NC_MEMORY` wasn't defined. The extern declared `sched_diag_tick[]` as an array (BSS) instead of `volatile uint32_t *` (pointer). The linker matched the symbol but the array access read the pointer value as data.
**Fix:** Added `#include "ncmem.h"` to shell_sys.c.
**Result:** `diag_tick ptr=0xffe06240 val[0]=0x0` — correct NC pointer, correct zero value.

## Iteration 2: Fix scheduler_is_initialized() cross-CPU polling

**Problem:** Secondary CPUs stuck in `while (!scheduler_is_initialized())` loop forever.
**Cause:** `sched.initialized` is in cacheable BSS. CPU 0 writes 1 and cleans with `cache_clean`. Secondary CPUs read with `cache_invalidate` (DC CIVAC). But DC CIVAC doesn't propagate through L2 on Pi 5 — secondary CPUs' L2 caches `initialized=0` and never see the update.
**Fix:** Added `nc_sched_initialized` at fixed NC address (`NC_MEM_BASE + NC_MEM_SIZE - 64`). CPU 0 writes 1 to NC. `scheduler_is_initialized()` reads from NC on Pi 5.
**Result:** Secondary CPUs NOW pass the polling loop (shell task id=6, meaning 3 idle tasks were created for CPUs 1-3 before the shell task).

## Iteration 3: Fix NC uart_lock causing deadlock

**Problem:** With NC init flag working, all 4 CPUs call scheduler_start() which does INFO prints. The NC-allocated uart_lock deadlocks because `ldaxr`/`stxr` hangs on NC memory (BCM2712 exclusive monitor requires cacheable memory).
**Fix:** Reverted uart_lock to cacheable BSS. Spinlocks DO work cross-CPU because the exclusive monitor is independent of L2 coherency.
**Result:** System boots to shell. But still hangs if secondary CPUs' scheduler_start does too much printing.

## Iteration 4: Reduce secondary CPU UART contention

**Problem:** Secondary CPUs' INFO prints in scheduler_start() contend with CPU 0, causing slow/garbled output or deadlock.
**Fix:** Guarded INFO prints in scheduler_start() with `if (this_cpu == 0)`.
**Result:** Shell boots cleanly. Secondary CPUs run silently.

## Iteration 5: Verify secondary CPU idle task entry

**Finding:** IdleLoops diagnostic is 0 for all CPUs including 1-3. The idle task function body is NEVER entered on secondary CPUs. `switch_to(NULL, idle)` in scheduler_start() doesn't reach idle_task_func.

**Hypothesis:** The context.S restore reads the task context from NC memory (ldp for FPU registers etc. at NC addresses). This may fault or read garbage on BCM2712, preventing the idle task from starting.

**Next test:** Try allocating the idle task's context in cacheable memory, or skip the FPU restore for the first switch_to.

## Iteration 6: NC context is NOT the issue

**Test:** Forced task_table to cacheable BSS (disabled NC alloc). Secondary CPUs still show 0 idle loops.
**Conclusion:** NC memory for task structs is NOT the problem. switch_to fails regardless of memory type.

## Iteration 7: Secondary CPUs never reach scheduler_start

**Test:** Added 0xBBBB marker in smp.c right before scheduler_start(). CPU 0 shows 0xAAAA (scheduler_start marker), CPUs 1-3 show 0 (not even 0xBBBB).
**Conclusion:** `scheduler_init_secondary()` hangs — secondary CPUs never return from it.

## Iteration 8: task_lock cross-CPU deadlock

**Hypothesis:** `task_lock` in task.c uses ldaxr/stxr which fails under cross-CPU contention.
**Fix:** Replaced with `__atomic_test_and_set` (SWPALB).
**Result:** No change — secondary CPUs still stuck.

## Iteration 9: NC init flag timing

**Problem:** NC init flag at `NC_MEM_BASE + NC_MEM_SIZE - 64` contained garbage from uninitialized DRAM. When properly zeroed, secondary CPUs correctly wait.
**Issue:** Zeroing in `ncmem_init` happens before VMM → write goes to cacheable identity map, not NC. Fixed by zeroing in `main.c` after `vmm_init` and before `smp_init`.
**Result:** Shell id=3 (secondary CPUs don't create idle tasks). Secondary CPUs stuck in init polling loop despite NC flag being set to 1.

## Iteration 10: TLB invalidation

**Hypothesis:** Secondary CPUs' TLB has stale entries from boot.S (L1[3] = 1GB cacheable block). NC reads go through cacheable path.
**Fix:** Added `tlbi vmalle1; dsb sy; isb` before MMU enable in smp_boot.S.
**Result:** No change. Shell id=3.

## Current State (as of latest iteration)

- NC init flag: zeroed correctly, set to 1 by scheduler_init
- DEBUG_PRINT confirms flag is set (readback shows value=1)
- Secondary CPUs boot successfully (SMP tests pass, "CPU N: online" printed)
- Secondary CPUs DO enter the scheduler_is_initialized() polling loop
- But they NEVER see the NC flag = 1 and NEVER exit the loop
- The write (from CPU 0) to NC_MEM_BASE + NC_MEM_SIZE - 64 does NOT reach DRAM
- TLB invalidation didn't help — the issue is likely in how CPU 0 writes to NC memory

**Root cause hypothesis:** CPU 0's write to the NC flag address goes through the cacheable L1/L2 cache (since CPU 0's TLB might also have a stale entry for this address range). CPU 0's boot.S initial page table had L1[3] as a 1GB cacheable block. vmm_init() installs new tables with L2 split, but CPU 0's TLB may retain the old cacheable mapping.

**Next step:** Add TLB invalidation on CPU 0 in vmm_init() after installing new page tables. Or verify that vmm_init already does TLBI.
