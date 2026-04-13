# x86-64 Scheduler Reentrance Investigation

**Status:** ✅ Resolved (April 12, 2026) — #91 fixed by routing the x86-64
`task_entry_wrapper` through `task_entry_trampoline`, which clears
`preempt_disabled[cpu]` on new-task entry. See the "Resolution" section
at the end of this document.
**Platform:** x86-64 (i7-6700, 8 CPUs)
**Dates:** April 5–6, 2026 (investigation), April 12, 2026 (fix)
**Related commits:** `cf1a6f8` through `865f571` on `x86-64-port` branch

---

## Problem Statement

When two or more user tasks on the same CPU do `yield()`-based `sleep_ms()` simultaneously, the scheduler deadlocks or corrupts state. Single task + shell via timer preemption works correctly.

**Reproducer:**
```
slmos> component run echo
Component 'echo' v1.0 started (idx=0, task=11)
[echo] Started (component 0), listening for messages...

slmos> sleep 2000
(hangs — never returns)
```

Counter component (single task + shell) works reliably:
```
slmos> component run counter
[counter] Started (component 0)
[counter] Count: 3
[counter] Count: 6
[counter] Count: 9
[counter] Done (count=10)
```

---

## Root Cause Analysis

### The Race Window

In `kernel/sched/sched.c`, `schedule()` has a critical window between releasing the run queue lock and performing the context switch:

```c
// Line 827 (approximate)
rq_unlock_irqrestore(this_cpu, flags);  // Re-enables IRQs!
                                         // ← TIMER CAN FIRE HERE
switch_to(current, next);               // Context switch
```

When the timer fires in this window:

1. The timer ISR calls `scheduler_tick()` → `schedule()`
2. The inner `schedule()` tries to acquire `rq_lock[this_cpu]`
3. The lock was released by the outer `schedule()`, so it succeeds
4. BUT: `task_set_current(next)` was already called at line 814
5. The inner `schedule()` sees `task_current() = next` (the task being switched TO)
6. The inner `schedule()` re-enqueues `next` (thinking it's the running task)
7. The inner `schedule()` picks another task and calls `switch_to(next_wrong, other)`
8. This corrupts `next`'s saved context (saving from the wrong stack)

### Why It Only Affects Multi-Task Yield

With a single task + shell, the shell busy-waits on UART input (polling `inb(COM1+5)`). The timer ISR fires, calls `schedule()`, which switches shell → idle or shell → counter. The counter runs, eventually yields back. There's no nested `schedule()` because the shell isn't inside `schedule()` when the timer fires.

With two tasks both doing `sleep_ms()` → `yield()` → `schedule()`, BOTH tasks call `schedule()` voluntarily. The timer can fire while either is in the critical window.

### x86-64 Specific: No Separate ISR Stack

On x86-64, the timer ISR runs on the **current task's kernel stack** (no separate ISR stack without TSS IST entries). The ISR frame is pushed onto the same stack where `schedule()` is executing. This means the reentrant `schedule()` from the timer ISR runs on the same stack, corrupting local variables.

---

## Investigation Timeline

### Phase 1: Discovery (April 5)

**Symptom:** `component send` command hangs when echo service is running.

**Initial hypothesis:** IPC queue pointer not visible across tasks (cross-CPU cache coherency).

**Findings:**
- Echo task was being routed to a different CPU via `find_target_cpu()` (load balancing sent it to CPU 1–7)
- Shell's `yield()` only context-switches between tasks on CPU 0's queue
- Echo on CPU 2 never gets CPU time from shell's yields

**Fix applied:** Pin component tasks to CPU 0 (`task->cpu_affinity = 0`)

**Result:** Echo starts on CPU 0, but IPC still fails.

### Phase 2: Timer Tick Rate Bug (April 5)

**Discovery:** `sleep_ms(5000)` returned in 625ms instead of 5000ms.

**Root cause:** All 8 CPUs were incrementing `pit_ticks` in their LAPIC timer ISRs. With 8 CPUs, tick rate was 800 Hz instead of 100 Hz.

**Fix:** Only BSP (CPU 0) increments `pit_ticks`:
```c
if (cpu_id() == 0)
    pit_ticks++;
```
Commit `7a3beb1`.

**Verification:** `sleep 3000` → 3020ms. Correct.

### Phase 3: dmb sy → arch_mb() Fix (April 5)

A merge from the Pi 5 agent changed `mfence` to `dmb sy` (ARM64 instruction) in `component_runtime.c`. This would crash on x86-64 with #UD (invalid opcode). Fixed to use `arch_mb()` from `arch.h`.

### Phase 4: Scheduler Reentrance Analysis (April 6)

**Deep investigation** of the schedule() race window:

1. **Explored the full code flow** in schedule() (lines 696–835), context.S, timer_x86.c, and scheduler_tick()
2. **Researched standard solutions:**
   - Linux kernel: `finish_task_switch()` pattern (lock held through switch, released by new task)
   - xv6: Scheduler thread as lock intermediary
   - Bare-metal x86-64: Mask timer during switch
3. **Confirmed the exact race:** Timer ISR between `rq_unlock_irqrestore` and `switch_to` causes stale `task_current()` corruption

### Phase 5: Fix Attempt 1 — Reentrance Guard (April 6)

**Approach:** Per-CPU `schedule_in_progress` flag checked at schedule() entry.

```c
static volatile int schedule_in_progress[MAX_CPUS];

void schedule(void) {
    if (schedule_in_progress[this_cpu]) return;
    schedule_in_progress[this_cpu] = 1;
    // ... schedule logic ...
    schedule_in_progress[this_cpu] = 0;
}
```

**Result:** **Broke boot completely.** The guard prevented `scheduler_tick()` from calling `schedule()` during ANY context switch, which is how preemptive scheduling works. With the guard, no timer-initiated preemption occurred, and the system hung.

**Reverted.**

### Phase 6: Fix Attempt 2 — IF Clearing in context.S (April 6)

**Approach:** Clear the IF (Interrupt Flag) bit in RFLAGS during `popfq` in `switch_to`, preventing IRQs from being re-enabled during the context restore:

```asm
mov     CTX_RFLAGS(%rdx), %rax
and     $~0x200, %rax           /* Clear IF */
push    %rax
popfq
```

Then `sti` after switch_to in schedule().

**Result:** Tasks resumed with IRQs disabled. `sleep_ms` never returned because `pit_ticks` never incremented (timer ISR couldn't fire). The `sti` after switch_to was supposed to re-enable, but the task's RFLAGS was saved with IF=0, so on the NEXT switch_to resume, IF was still 0.

**Reverted.**

### Phase 7: Fix Attempt 3 — spin_unlock Without IRQ Restore (April 6)

**Approach:** Release the spinlock but DON'T restore IRQs before switch_to. Use `irq_restore(flags)` after switch_to.

```c
spin_unlock(&rq_lock[this_cpu]);  // Lock released, IRQs still disabled
switch_to(current, next);
irq_restore(flags);               // flags from THIS task's stack
```

**Result:** `sleep 2000` worked with echo running (2013ms)! But `component send` still hung. Investigation showed the echo task's `sleep_ms(100)` completed faster than expected (300 iterations in ~10-15 seconds instead of 30 seconds). The echo exited before the send command arrived.

**Partial success — demonstrated that preventing timer reentrance fixes the deadlock for simple cases.**

### Phase 8: Fix Attempt 4 — LAPIC Timer Masking (April 6, reverted)

**Abandoned.** The `lapic_timer_mask()` / `lapic_timer_unmask()` helpers
were removed in April 2026 after the `preempt_disabled` flag proved
sufficient. The analysis below is retained for historical context;
see the Resolution section for the final fix.

**Approach:** Mask the LAPIC timer LVT entry before releasing the lock. The timer can't fire during the switch_to window. Unmask after switch_to returns.

```c
lapic_timer_mask();                     // Prevent timer during switch
rq_unlock_irqrestore(this_cpu, flags);  // IRQs re-enabled but timer masked
switch_to(current, next);
lapic_timer_unmask();                   // Resumed: re-enable timer
```

New functions in `lapic.c`:
```c
void lapic_timer_mask(void) {
    lapic_write(LAPIC_LVT_TIMER, lapic_read(LAPIC_LVT_TIMER) | LVT_MASKED);
}
void lapic_timer_unmask(void) {
    lapic_write(LAPIC_LVT_TIMER, lapic_read(LAPIC_LVT_TIMER) & ~LVT_MASKED);
}
```

Also added `call lapic_timer_unmask` in `task_entry_wrapper` (context.S) for new tasks.

**Result:** `sleep 2000` returned correctly (2013ms) while echo was running on one test. Subsequent tests showed inconsistent results — sometimes `sleep 2000` hung. The timer mask during every context switch prevents `pit_ticks` from advancing during rapid yield cycles.

**Kept as current approach** despite inconsistency.

### Phase 9: Fix Attempt 5 — Conditional Timer Masking (April 6)

**Approach:** Only mask the timer when `schedule()` is called from the timer ISR (`scheduler_tick`), not from `yield()`.

```c
static volatile int from_timer_isr[MAX_CPUS];

void scheduler_tick(void) {
    from_timer_isr[cpu_id()] = 1;
    schedule();
    from_timer_isr[cpu_id()] = 0;
}

// In schedule():
int need_timer_mask = from_timer_isr[this_cpu];
if (need_timer_mask) lapic_timer_mask();
// ... unlock, switch_to ...
if (need_timer_mask) lapic_timer_unmask();
```

**Result:** `component send` still hung. The yield()-initiated switches DON'T mask the timer, so the original race still exists for yield-initiated schedules. A timer can fire during a yield's schedule() between unlock and switch_to, and the reentrant schedule() corrupts task_current().

**Analysis:** The reentrance from yield-initiated switches isn't a spinlock deadlock (the lock was released), but a **stale `task_current()`** issue. `task_set_current(next)` was called before switch_to, so the reentrant schedule() sees the wrong current task.

### Phase 10: Fix Attempt 6 — Move task_set_current After switch_to (April 6)

**Approach:** Call `task_set_current(next)` AFTER switch_to instead of before. This way, if the timer fires before switch_to, `task_current()` still returns the correct (old) task.

**Problem:** After switch_to returns (on the resumed task's stack), the `next` local variable contains the task that was being switched TO when THIS schedule() instance ran — NOT the task that we are now. If shell calls schedule(), `next = idle`. Shell is saved. Later, shell resumes. Shell's `next` = idle. `task_set_current(idle)` — wrong, we're shell!

**Attempted fix:** Use `self = current` (saved before switch_to) instead of `next`.

**Result:** Shell never gets prompt after boot. The `task_current()` is wrong during the time between switch_to and set_current, which breaks anything that reads task_current (scheduler_tick, uart_printf with locks, etc.).

**Reverted to original task_set_current before switch_to.**

### Phase 11: Fix Attempt 7 — Explicit STI After switch_to (April 6)

**Approach:** `spin_unlock` (without irq_restore) + `switch_to` + `sti` (explicit interrupt enable).

```c
spin_unlock(&rq_lock[this_cpu]);
switch_to(current, next);
__asm__ volatile("sti" ::: "memory");
```

**Result:** `sleep 2000` hung even with only shell (no echo). The `sti` unconditionally enables IRQs, but tasks may have been called with IRQs disabled for legitimate reasons (e.g., inside spinlock critical sections). The unconditional `sti` breaks the IRQ state invariant.

**Reverted.**

---

## Current State (April 6, 2026)

### What Works
- Unconditional LAPIC timer mask around switch_to (prevents spinlock deadlock and crash)
- Single task + shell via timer preemption (counter component, benchmarks, GPU, PCI)
- `sleep_ms()` without echo task running
- 8/8 CPUs online, shell responsive, all subsystems functional
- Boot time ~4–7 seconds

### What Doesn't Work
- `sleep 2000` while echo task is running (hangs)
- `component send` mailbox IPC (hangs in sleep_ms polling loop)
- Any scenario with 2+ tasks doing yield()-based sleep on the same CPU

### What's In The Code
- `lapic_timer_mask()` / `lapic_timer_unmask()` in `lapic.c`
- Unconditional mask/unmask around switch_to in `schedule()`
- `task_entry_wrapper` calls `lapic_timer_unmask()` for new tasks
- `from_timer_isr[]` flag (unused but present)
- `scheduler_tick()` unmasks timer after `schedule()` returns

---

## Why the LAPIC Timer Mask Doesn't Fully Work

The timer mask prevents the SPECIFIC reentrance that causes spinlock deadlock. But it creates a NEW problem:

1. Shell calls `yield()` → `schedule()` → masks timer → unlocks → switch_to(shell, echo)
2. Echo resumes at `lapic_timer_unmask()` → timer starts firing again
3. Echo runs for <10ms (one timer period), calls `sleep_ms(100)` → `yield()` → `schedule()`
4. Schedule masks timer → unlocks → switch_to(echo, shell)
5. Shell resumes at `lapic_timer_unmask()` → timer unmasked
6. But `pit_ticks` may not have advanced during steps 1–5 because the timer was masked for most of the time

Each context switch masks the timer for the duration of switch_to (~100 CPU cycles). But the timer fires every 10ms (~33 million cycles). The mask duration is tiny compared to the timer period. The timer SHOULD fire normally between yields.

**The actual issue may be:** After `lapic_timer_unmask()`, the timer's current count has expired (the timer counted to zero while masked). The timer fires IMMEDIATELY (pending interrupt). This interrupt calls `scheduler_tick()` → `schedule()`, which does another switch, which masks the timer again. The cycle repeats without `pit_ticks` having a chance to count up.

This creates a "timer thrashing" pattern where the timer fires, gets masked, fires again immediately on unmask, gets masked again — never allowing normal 10ms periods.

---

## Approaches Not Yet Tried

### 1. Linux-style finish_task_switch()
Hold the rq_lock through switch_to. The resumed task calls `finish_task_switch()` to release the lock. New tasks do this in a modified `task_entry_wrapper`. This is the most robust solution but requires:
- All schedule() callers to understand the lock handoff
- task_entry_wrapper to release a lock it never acquired
- scheduler_start() to handle the initial switch differently

### 2. Separate ISR Stack (TSS IST)
Configure x86-64 TSS with an Interrupt Stack Table entry for the timer vector. Timer ISR runs on a dedicated stack, not the current task's kernel stack. This doesn't directly fix the reentrance but prevents stack corruption.

### 3. GDB Debugging
Set up QEMU with `-s -S`, attach GDB, set breakpoints at schedule(), switch_to, and lapic_timer_irq. Trace exact register values and stack state during multi-task yield. This would definitively show whether the issue is:
- Timer thrashing (rapid mask/unmask)
- Stale task_current
- Stack corruption
- Something else entirely

### 4. Disable Preemption During yield()
Instead of masking the timer, set a per-CPU "preemption disabled" flag that `scheduler_tick()` checks:
```c
void scheduler_tick(void) {
    sched.timer_ticks++;
    if (!preempt_disabled[this_cpu])
        schedule();
}
```
This is simpler than timer masking and doesn't affect timer counting.

### 5. Non-Blocking IPC
Avoid yield()-based polling entirely. Use a callback/event-driven IPC pattern where the echo service registers a callback that's invoked directly by `msg_send()`. No cross-task scheduling needed.

---

## Key Files

| File | Role |
|------|------|
| `kernel/sched/sched.c` | schedule(), scheduler_tick(), yield() |
| `kernel/arch/x86_64/context.S` | switch_to(), task_entry_wrapper |
| `kernel/arch/x86_64/timer_x86.c` | lapic_timer_irq, sleep_ms |
| `kernel/arch/x86_64/lapic.c` | LAPIC LVT, EOI fence, timer calibration (timer-mask helpers removed after the approach was abandoned) |
| `kernel/src/component_runtime.c` | echo service, component_send_echo |
| `kernel/arch/x86_64/idt.c` | exception_handler, IRQ dispatch |

---

## Key Register/Memory Locations

| Symbol | Address | Purpose |
|--------|---------|---------|
| `pit_ticks` | BSS | Global tick counter (100 Hz on BSP) |
| `current_task[cpu]` | BSS | Per-CPU current task pointer |
| `rq_lock[cpu]` | BSS | Per-CPU run queue spinlock |
| `LAPIC_LVT_TIMER` | 0xFEE00320 | LAPIC timer vector register (bit 16 = mask) |
| `from_timer_isr[cpu]` | BSS | Flag: schedule() called from timer ISR |

---

## Resolution (April 12, 2026)

**Root cause identified.** The x86-64 `task_entry_wrapper` in
`kernel/arch/x86_64/context.S` called the task's entry function
directly, bypassing `task_entry_trampoline` (the shared C trampoline in
`kernel/sched/task.c` that ARM64 already routed through). That
trampoline is the only code path that clears `preempt_disabled[cpu]` on
new-task entry. On x86-64, `preempt_disabled` therefore stayed at `1`
throughout a new task's first timeslice — the value set by the
outgoing `schedule()` (or `scheduler_start()`) immediately before
`switch_to`. Until the outgoing task was resumed (which required a
cooperative yield from the new task) timer preemption was suppressed
on the whole CPU, so any cooperative workload that waited on
`pit_ticks` or another task's yield would stall.

**Fix.** Rewrite the x86-64 wrapper as a tail call to
`task_entry_trampoline(entry=rbx, arg=r12)`:

```asm
task_entry_wrapper:
    sti
    mov     %rbx, %rdi     /* entry → first parameter */
    mov     %r12, %rsi     /* arg   → second parameter */
    jmp     task_entry_trampoline
```

`task_entry_trampoline` already contained an `#if defined(PLATFORM_X86_64)`
branch that clears `preempt_disabled[cpu_id()]` and `mfence`s — that
branch had simply never been reached on x86-64 because nothing called
it. After this change both platforms share the trampoline; the
"switch to a new task" case no longer gets stuck non-preemptible.

**Regression tests** (in `kernel/tests/test_x86_boot.c`):
- `test_preempt_disabled_cleared_in_task` — asserts
  `preempt_disabled[cpu_id()] == 0` from inside a running task.
- `test_new_task_runs_and_yields` — spawns a worker that yields four
  times and asserts the counter advances, exercising the first-
  timeslice preemption path.

Both fail pre-fix (the worker stalls because `preempt_disabled` was
never cleared on its first timeslice) and pass post-fix.

*Last updated: April 12, 2026*
