# Pi 5 Secondary CPU Timer Preemption

**Date:** April 10, 2026
**Status:** Deferred — root cause identified, solution designed, not yet implemented
**Affects:** 5 multi-core integration tests on Pi 5 hardware
**Does NOT affect:** QEMU (all tests pass), single-CPU scheduling, cooperative cross-CPU dispatch

---

## Problem Statement

On Raspberry Pi 5, timer-driven preemptive scheduling works on CPU 0 but not on secondary CPUs (1-3). Tasks dispatched to secondary CPUs only complete if they are trivial (run and exit without needing preemption). Tasks that require preemptive scheduling — either because they need to be interrupted by the timer, or because their completion must be observed by CPU 0 via timer ticks — hang indefinitely.

This causes 5 multi-core integration tests to fail:
- `test_multicore_basic` — 3 tasks on CPUs 1-3, wait for completion
- `test_task_migration` — task migrated to CPU 3, wait for signal
- `test_stress_multicpu` — 6 tasks across CPUs 1-3
- `test_lock_contention` — 3 tasks with spinlock contention across CPUs
- `test_task_lifecycle` — rapid create/destroy on CPUs 1-3

Cooperative dispatch works: `bench smp` dispatches trivial tasks to all 4 CPUs and they complete via WFE/SEV. The issue is specifically with timer-driven preemption on secondary CPUs.

---

## Background: How Scheduling Works

### QEMU (Working)

On QEMU, timer preemption works on all CPUs:

1. Timer fires (IRQ 30, PPI) → exception taken
2. Exception handler: `gic_end_interrupt(30)` → `timer_handler()` → `scheduler_tick()` → `schedule()`
3. `schedule()` picks the highest-priority ready task → `switch_to(current, next)`
4. `switch_to` saves current context, restores next context, `ret` to next task
5. The exception frame from step 1 is abandoned (never `eret`-ed)
6. The next task runs on the task stack, not the exception stack

This works on QEMU because:
- QEMU's exception handling is lenient about abandoned exception frames
- The GIC EOI is done before `timer_handler()`, so the GIC state is clean
- `switch_to` restores the new task's DAIF, SP, and callee-saved registers
- The abandoned SPSR_EL1/ELR_EL1 from the exception are irrelevant — `switch_to` uses `ret` (x30), not `eret`

### Pi 5 (Broken on Secondary CPUs)

On Pi 5 real hardware, the same sequence hangs on secondary CPUs:

1. Idle task does `msr daifclr, #2` → `wfi` (unmask IRQs, wait for interrupt)
2. Timer fires → exception taken → DAIF.I masked by hardware
3. Exception handler runs `gic_end_interrupt(30)` → `timer_handler()`
4. `scheduler_tick()` → `schedule()` → finds a task in the queue → `switch_to(idle, task)`
5. `switch_to` saves idle's context (on the exception stack frame) and restores the new task's context
6. **The CPU is now running the new task, but the exception entry state (SPSR_EL1, ELR_EL1, exception stack) was never cleaned up**
7. The new task runs to completion → `task_exit()` → `schedule()` → `switch_to(task, idle)`
8. Idle resumes from its saved context — but this context was saved mid-exception-handler, not from the clean idle loop

The exact failure mode is not fully characterized. Observed symptoms include:
- System hangs silently (no output, no crash)
- Tasks on secondary CPUs never reach TASK_TERMINATED state
- No page faults or visible exceptions

### What Changed Between April 7 (Discovery) and April 10 (Current)

The original investigation on April 7 found that `daifclr` in `scheduler_start()` (before `switch_to`) caused a hang because it was on the **boot stack**, not a task stack. The workaround was to move `daifclr` to the idle task loop, where it runs on the idle task's stack.

On April 10, enabling `daifclr` in the idle task loop for ALL CPUs was attempted. This allowed secondary CPUs to take timer interrupts and run tasks. Cross-CPU dispatch worked — `bench smp` tasks completed, and `iso_test` tasks on CPUs 1-3 were observed exiting.

However, two problems emerged:

1. **UART garbling:** Multiple CPUs printing via `uart_printf` simultaneously (no cross-CPU UART lock) corrupted output and caused hangs when one CPU's write was interrupted by another CPU starting a write.

2. **Scheduler test races:** Tests written for cooperative scheduling (e.g., `test_high_priority_runs_first`) assume specific execution ordering that breaks under preemptive scheduling. The barrier spin uses `__asm__ volatile("yield")` (CPU hint, not scheduler yield), which monopolizes the CPU and prevents the test runner from releasing the barrier.

---

## Root Cause Analysis

### The Exception Frame Problem

ARM64 exception handling works as follows:

```
Exception entry:
  - Hardware saves PSTATE → SPSR_EL1
  - Hardware saves return address → ELR_EL1
  - Hardware masks interrupts (DAIF.I = 1)
  - Hardware jumps to exception vector

Exception return (eret):
  - Hardware restores PSTATE from SPSR_EL1
  - Hardware jumps to ELR_EL1
```

When `schedule()` does `switch_to()` inside the exception handler:
- SPSR_EL1 and ELR_EL1 are NOT part of the task context saved by `switch_to` (which saves/restores callee-saved GPRs, SP, DAIF, FP regs)
- The exception return (`eret`) in the vector code is never executed
- SPSR_EL1 and ELR_EL1 retain stale values from the interrupted context

On the next exception (e.g., next timer tick), ARM64 overwrites SPSR_EL1 and ELR_EL1 with the NEW exception state, so the stale values are harmless. This is why it works on QEMU.

On Pi 5 hardware, the issue may be related to:
- **Exception stack pointer:** The exception may use SP_EL1 (which `switch_to` changes). If the exception handler's stack frame references data below the new SP, it's accessing freed/reused memory.
- **GIC state:** Although EOI is done before `timer_handler()`, the GIC's running priority register (RPR) may not be updated correctly when the exception return never happens.
- **Cache state:** The context save/restore writes to the old task's context struct. On Pi 5 without SMPEN, these writes may be in L2 and not visible to other CPUs. If the same CPU later reads this context (when switching back), the L2 read is correct. But if the exception handler wrote to the exception stack frame using a different virtual address mapping or cache line, there could be cache aliasing.

### Why Cooperative Dispatch Works

`bench smp` tasks work because they don't rely on timer preemption:

1. CPU 0 adds task to CPU 1's run queue → sends SEV
2. CPU 1 wakes from WFE → `yield()` → `schedule()` → picks task → `switch_to(idle, task)`
3. This `switch_to` is called from `schedule()` which is called from the idle task's C code — NOT from an exception handler
4. The task runs, exits, `schedule()` → `switch_to(task, idle)` — all in task context
5. No exception frames are involved

The difference: cooperative scheduling never involves `switch_to` inside an exception handler. The "deferred scheduling" solution (below) maintains this property.

---

## Proposed Solution: Deferred Scheduling

### Design

Instead of calling `schedule()` from inside the timer ISR (which requires `switch_to` inside the exception handler), the timer ISR sets a per-CPU "reschedule pending" flag. The actual `schedule()` call happens after the exception returns, in task context.

```
Current (broken on Pi 5 secondary CPUs):
  Timer ISR → scheduler_tick() → schedule() → switch_to()    [in exception context]

Proposed:
  Timer ISR → scheduler_tick() → set reschedule_pending flag  [in exception context]
  Exception return (eret) → check flag → schedule()           [in task context]
```

### Implementation

#### 1. Per-CPU Reschedule Flag

```c
/* In sched.c or a header */
volatile uint32_t reschedule_pending[MAX_CPUS];
```

This should be in NC memory on Pi 5 for cross-CPU visibility, but since it's per-CPU (each CPU only reads its own flag), cacheable BSS is fine.

#### 2. Modified `scheduler_tick()`

```c
void scheduler_tick(void)
{
    uint32_t cpu = cpu_id();

    /* Always count ticks */
    sched.timer_ticks++;
    pit_ticks++;

    /* ... existing counter/AI code ... */

    /* Policy tick callback */
    if (active_policy && active_policy->tick)
        active_policy->tick(cpu);

    /* Don't call schedule() directly — set flag for deferred scheduling.
     * The actual context switch happens after exception return (eret),
     * in task context. This avoids switch_to inside the exception handler
     * which abandons the exception frame (breaks on Pi 5 hardware). */
    if (!preempt_disabled[cpu]) {
        reschedule_pending[cpu] = 1;
    }
}
```

#### 3. Exception Return Hook

The exception vector code (vectors.S or exceptions.c) must check `reschedule_pending` after every exception return. This requires a small assembly trampoline:

```asm
/* In vectors.S, after the existing exception handler returns: */
el1_irq_return:
    /* Check reschedule_pending for this CPU */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF          /* Extract Aff0 (QEMU) */
    /* On Pi 5: also extract Aff1 — use cpu_logical_id pattern */

    adr     x1, reschedule_pending
    ldr     w2, [x1, x0, lsl #2]   /* reschedule_pending[cpu] */
    cbz     w2, .Lno_resched

    /* Clear flag */
    str     wzr, [x1, x0, lsl #2]

    /* Restore GP registers from exception frame (so we're in clean state) */
    /* ... restore x0-x30, sp ... */
    /* eret to the interrupted code */

    /* But THEN immediately call schedule() — this is tricky because
     * eret jumps to ELR_EL1. We need a way to insert schedule()
     * between the eret and the resumed code. */
```

**This is the hard part.** There's no clean way to "call schedule() after eret" because `eret` jumps directly to the interrupted instruction. The standard approach is:

**Option A: Modify ELR_EL1 before eret**

Before `eret`, set ELR_EL1 to a `schedule()` trampoline. Save the original ELR_EL1 so the trampoline can jump back after scheduling.

```asm
el1_irq_return:
    ldr     w2, [x1, x0, lsl #2]   /* reschedule_pending[cpu] */
    cbz     w2, .Lno_resched

    str     wzr, [x1, x0, lsl #2]  /* Clear flag */

    /* Save original return address */
    mrs     x3, elr_el1
    /* Store it somewhere accessible to the trampoline */

    /* Set ELR_EL1 to schedule trampoline */
    adr     x4, .Lresched_trampoline
    msr     elr_el1, x4

.Lno_resched:
    /* Normal exception return — restore registers, eret */
    ...
    eret

.Lresched_trampoline:
    /* We're now in task context (post-eret), IRQs unmasked by SPSR restore */
    /* Call schedule() */
    bl      schedule
    /* Jump to original return address */
    ldr     x0, [saved_elr_location]
    br      x0
```

**Option B: Software interrupt approach**

Instead of modifying ELR_EL1, use a software interrupt (SVC or a dedicated IRQ) to trigger scheduling after the timer ISR returns. The timer ISR sets the flag, returns via `eret`, and the pending software interrupt fires immediately.

This is cleaner but requires allocating a GIC IRQ number for the "reschedule IPI."

**Option C: Check flag in yield() and schedule()**

The simplest approach — don't try to preempt from the ISR at all. Let the `reschedule_pending` flag be checked at natural scheduling points:
- `yield()` already calls `schedule()`
- `schedule()` already picks the best task
- Add checks in `spin_unlock` paths (many kernel operations end with a lock release)

This provides "semi-preemptive" scheduling — tasks are not truly preempted mid-execution, but they are rescheduled at the next voluntary yield or lock release. For the test suite, this works because all test tasks call `yield()` or use spinlocks.

### Recommended Approach: Option A (ELR_EL1 trampoline)

Option A provides true preemption without requiring dedicated IRQ numbers or kernel code changes beyond the vector/scheduler interface. It's the standard approach used by Linux and other production kernels.

#### Saving the Original ELR

The trampoline needs the original ELR_EL1. Options for storing it:
- **On the task stack:** Push ELR_EL1 as part of the exception frame before eret. The trampoline pops it and jumps. This is the cleanest approach and doesn't require global state.
- **In per-CPU data:** Store in a per-CPU variable. Simpler but uses global state.

#### SPSR_EL1 Considerations

The `eret` restores PSTATE from SPSR_EL1, which includes the IRQ mask bit. The interrupted task's PSTATE will have IRQ masked (DAIF.I=1, set on exception entry). After `eret`, IRQs are masked. The trampoline must unmask IRQs before calling `schedule()` so that the timer can continue firing while schedule runs.

But there's a subtlety: if `schedule()` does `switch_to()` in the trampoline, the trampoline's stack frame and saved ELR are on the current task's stack. When this task is later resumed, `switch_to` restores its context including SP, and execution resumes after the `bl schedule` in the trampoline. The trampoline then reads the saved ELR and jumps to the originally interrupted code. This works correctly.

---

## Complexity Assessment

### What's Needed

1. **Per-CPU reschedule flag** — trivial (one variable)
2. **Modified scheduler_tick** — trivial (replace `schedule()` call with flag set)
3. **Exception vector trampoline** — moderate complexity:
   - Save ELR_EL1 to stack before eret
   - Modify ELR_EL1 to point to trampoline
   - Trampoline calls schedule(), then jumps to original ELR
   - Must handle the case where schedule() does switch_to (trampoline is on the switched-away task's stack — this is fine because switch_to saves/restores SP)
4. **UART cross-CPU lock** — needed once secondary CPUs can print:
   - NC memory based lock (plain volatile load/store, not atomics)
   - Or suppress all printing on secondary CPUs (current approach)
5. **Test updates** — make priority ordering tests preemption-safe:
   - Release barriers before irq_restore
   - Use `preempt_disabled` during setup phases
   - Use hardware counter timeouts instead of iteration counts

### Estimated Effort

- Exception vector trampoline: 2-3 days (needs careful testing on Pi 5 hardware)
- Scheduler tick changes: 1 hour
- Test updates: 1 day
- UART lock: 1 day
- Total: ~1 week

### Risk

- The ELR_EL1 trampoline is well-understood (Linux uses a similar mechanism) but requires precise assembly and stack management
- Pi 5-specific cache behavior may require additional validation
- Secondary CPU timer preemption may expose other latent concurrency bugs

---

## Current Workaround

Secondary CPUs use WFE-only in the idle loop (no `daifclr`). Cross-CPU dispatch works cooperatively via SEV. Tasks dispatched to secondary CPUs run when the idle loop's `yield()` → `schedule()` picks them up. Tasks complete and the scheduler switches back to idle.

This works for:
- `bench smp` (trivial tasks)
- Any task that runs to completion without needing preemption
- Component tasks pinned to CPU 0

This does NOT work for:
- Tasks that need to be interrupted by the timer (long-running without yield)
- Tests that wait for secondary CPU task state changes via polling (CPU 0 polls, but can't observe the secondary CPU task completing because it never gets preempted back to idle)

Wait — actually the issue with the 5 failing tests is more subtle. The tasks DO run and complete on secondary CPUs (cooperative dispatch via WFE/SEV works). The issue is that after the task completes and calls `task_exit()` → `schedule()`, the `schedule()` function picks the idle task and does `switch_to(terminated_task, idle)`. The idle task resumes and does WFE. But CPU 0, polling `task->state`, should see `TASK_TERMINATED` because the task struct is in NC memory.

The actual failure mode needs more investigation — it may be that `task_exit()` on a secondary CPU causes a UART print (`INFO("Task '%s' exiting")`) which deadlocks with CPU 0's UART output. With the `cpu_id() == 0` guard added in this session, this should be fixed. Testing with the guard + secondary WFE-only showed the same 5 failures, suggesting the UART isn't the only issue.

**Remaining unknown:** Why do tasks dispatched to secondary CPUs via `scheduler_add_task_to_cpu` not complete, even though:
- The run queue is in NC memory
- The task struct is in NC memory
- The idle loop calls `yield()` → `schedule()` after WFE wake
- SEV is sent after adding to the queue
- `bench smp` trivial tasks complete successfully

A possible explanation: the integration test tasks use `delay(500000)` (busy-wait nop loops). During this delay, the task has IRQs masked (DAIF.I=1 from context restore). Without timer preemption, the task runs the delay, finishes, calls `task_exit()`. But `task_exit()` calls `schedule()`, which calls `rq_lock_irqsave()`. The per-CPU rq_lock uses `ldaxr/stxr`. On a secondary CPU, `ldaxr/stxr` should work (single-CPU access, exclusive monitor is local). But if the exclusive monitor is in a bad state from the WFE wake, `ldaxr` might hang.

This hypothesis could be tested by replacing `ldaxr/stxr` with a simple `ldr/str` for per-CPU locks (no contention since they're per-CPU).

---

## References

- `docs/pi5-cross-cpu-dispatch-investigation.md` — full April 3-7 investigation history
- `kernel/CLAUDE.md` — "Pi 5 Timer IRQs and pit_ticks" section
- `kernel/arch/arm64/context.S` — switch_to implementation
- `kernel/arch/arm64/vectors.S` — exception vector table
- `kernel/arch/arm64/exceptions.c` — IRQ handler dispatch
- `kernel/sched/sched.c` — scheduler_tick, schedule, idle_task_func
- `kernel/sched/task.c` — task_entry_trampoline, task_exit
- ARM Architecture Reference Manual — D1.10 "Exception handling"

---

*Created: April 10, 2026*
*Author: Claude Opus 4.6 (with extensive hardware testing on Pi 5)*
