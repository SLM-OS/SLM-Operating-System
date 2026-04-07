# Cross-CPU Dispatch Debug Progress Log

**Started:** April 6, 2026
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

## Current State

- NC init flag works (secondary CPUs pass scheduler polling loop)
- Shell boots and is responsive (CPU 0 cooperative scheduling)
- Secondary CPUs run scheduler_start() but idle tasks never enter their function body
- Blocker: `switch_to(NULL, idle)` on secondary CPUs fails silently — context.S restore from NC task struct doesn't reach the trampoline
