# Critic verdict, switch gauntlet run 5, component C2, design checkpoint (ADR 0008)

Critic: `critic-c2-design`, blind. Model that wrote this verdict: **Fable 5.2**
(model ID `claude-fable-5-2`). Verdict written 04:40 UTC 2026-11-14. This is
the first of C2's two verdicts: the design in
`docs/adr/0008-switch-in-window.md` at `origin/c2/switch-window` d73604f7,
before any fix code exists.

**Verdict: FAIL.** The proposed deferral has no lock cycle and no blocking
wait under a lock, by the critic's own derivation, and the ordering argument
is correct. It fails on four points, the first of which is blocking:

1. `schedule()` runs with a stale CPU id after a preemption in its prelude
   followed by a migration. The ADR's decision keys everything on that id
   (`rq_lock[B]`, `rq->idle_task`, `switch_in_deferred[B]`). Observed, not
   only derived: a critic prototype of the ADR's decision panicked in R3 at
   iteration 20 right after the probe reported it (section 5).
2. The deferred task has no stated bound, and the one sentence that reads as
   a bound ("picked within one `schedule()` of its release") is wrong for a
   check-then-wait race in the idle loop (section 4.2).
3. A deferral whose previous CPU never releases (a CPU powered off or wedged)
   stops the deferring CPU from running anything at or below the deferred
   task's priority, for ever, and the ADR does not say so (section 4.3).
4. Gaps and inaccuracies in the lock graph and in the no-deadlock argument
   (sections 4.4 to 4.6). None of them hides a cycle.

Required changes are in section 7.

Heads. Base main 8ec0e330; C1 `origin/c1/harness` 2196deb5; C2
`origin/c2/switch-window` d73604f7 (code read from the builder's worktree at
that commit, clean; the critic's runs in its own worktree `s5-crit-c2d` at
d73604f7).

## 1. Order of work (blindness)

1. Brief (Goal, C2, Critic protocol, Rules), `lifecycle-4.md` Entries 11 and
   15 to 17, issues #1234, #1238, #1241.
2. ADR lines 1 to 175 only: Status, "The sequence being closed" with its
   rates and traces, "Why the reverted design deadlocked (run 4)" (an argument
   about the reverted design, not the proposed one), "Decision", "Candidate
   shapes considered". Not read at this point: "Lock graph", "Why no
   deadlock", "Ordering argument", "Invariant", "Consequences", "What would
   overturn this".
3. Code at d73604f7, sections 2 and 3 below derived and noted.
4. ADR lines 176 to the end, compared (section 4).
5. QEMU runs (section 5), after the comparison, to test point 1.

Disclosures. No process was killed. One QEMU of the critic's at a time; two
QEMUs of other agents were running on the host throughout. No labctl call.
The critic tried to commit its run files to `gauntlet-5-hw-pi5` (the C1
critic's precedent); the push was refused by the session's permission layer
as outside the critic's instructions, so the files are local only (section
5 gives paths and hashes) and the orchestrator decides where they go.

## 2. The critic's lock graph (written before reading the ADR's)

All locks are test-and-set spinlocks (`spinlock.h`: `wfe` + `ldaxr`/`stxr`,
unlock `stlr` + `sev`; x86 `lock xchg`), unfair. `X -> Y` means Y is taken
while X is held. "irq" = taken with `_irqsave`.

| Path | Locks and order | IRQ state |
|---|---|---|
| `schedule()` prelude (sched.c 2350 to 2378) | none held; `coop_preempt_maybe_tick` -> `scheduler_tick` (COOP builds); `sched_try_steal`: `steal_deque_lock[v]` irq, released, then `rq_lock[v]` irq, released; then `rq_lock[this]` irq for the add | caller's (open in task context since #1047) |
| `schedule()` body | `rq_lock[this_cpu]` irq; dropped around `task_destroy_gen(zombie)` (its `on_cpu` wait, then `task_lock`, PMM, cleanup callback) and retaken; one lock at a time | masked under the lock; caller's during the zombie destroy |
| switch window: `rq_unlock_irqrestore` to `sched_finish_switch` | no lock. `preempt_disabled[this_cpu] = 1` | caller's until `switch_to`'s `msr daif`, then the incoming task's; open in both halves for task-context callers |
| `switch_to` (context.S) | none. Saves x19 to x30, SP, q0 to q31, FPCR, FPSR, DAIF; restores; `msr daif` last; returns `old` in x0 | as above |
| `sched_finish_switch` / `task_entry_trampoline` | none. `stlr` of `prev->on_cpu = 0` | incoming task's |
| tick: `scheduler_tick` | `sleep_queue_lock` irq, released; per woken task `scheduler_add_task`: policy `assign_cpu` (AI: `sched_model_lock` family), then `rq_lock[t]` irq released, then `steal_deque_lock[t]` irq released; AI: `ai_update_top_tasks` takes each `rq_lock[c]` in turn; policy tick -> `sched_rebalance_tick` (CPU 0): `rq_lock[busy]` released, then `rq_lock[idle]`, `steal_deque_lock[idle]`. Returns before scheduling when `preempt_disabled[cpu]` | in the ISR (masked), or from `schedule()`'s prelude on COOP builds |
| preemption | ARM64 `SECONDARY_PREEMPT`: `reschedule_pending`, then `resched_trampoline` calls `schedule()` in task context with IRQs open, `preempt_disabled` set first. Otherwise (QEMU, x86): `scheduler_tick` calls `schedule()` inside the ISR | see left |
| `sched_migrate_task` | `rq_lock[min(old,target)]` irq -> `rq_lock[max]` plain. `old` is an unlocked snapshot of `assigned_cpu` (#1241) | masked |
| `sched_set_task_affinity`, Lua `l_task_migrate` | call `sched_migrate_task` with no lock | caller's |
| `scheduler_add_task_to_cpu` | `rq_lock[c]` irq, released, then `steal_deque_lock[c]` irq, then `smp_notify_cpu` (ARM64 `sev`, x86 IPI) | |
| `task_sleep_ms` | `sleep_queue_lock` irq: publish BLOCKED; unlock; `schedule()` | open between unlock and `schedule()` |
| `task_sleep_wake` | `sleep_queue_lock` irq: sets READY; unlock; `scheduler_add_task` | |
| ipc `block_on_queue` / `wake_one` | block: queue lock irq, publish BLOCKED, unlock, `yield()`. wake: queue lock held across `scheduler_add_task`: queue lock -> (policy locks) -> `rq_lock[c]`, then queue lock -> `steal_deque_lock[c]` | masked in wake |
| `pi_mutex_lock` / `unlock` | `guard` irq -> `pi_floor.lock`; lock side `guard` -> `rq_lock[c]` (reposition of the owner); unlock drops `guard` before `scheduler_add_task_to_cpu` | |
| `task_destroy` | `on_cpu` wait (`ldapr`, 2 s bound) with no lock of its own, then `task_lock` | caller's |
| idle loop | none; QEMU and tick builds `msr daifclr` + `wfi`; cooperative secondaries `wfe` with IRQs masked; Jetson `JETSON_HW_TICK=OFF` CPU 0 no wait; x86 `sti` + `hlt` | |
| x86 resched IPI | handler returns while `preempt_disabled[cpu]` (#1236), else `schedule()` in the ISR | masked |
| Pi 5 `cpu_supervisor` | `sched_drain_cpu_runqueue`: `rq_lock[c]` irq | |

One row was corrected after the comparison: the critic's first pass had
`guard -> pi_floor.lock` only; the ADR's `guard -> rq_lock[c]` was then
checked and is right (`pi_mutex_lock` calls `sched_reposition_task` under
`guard`).

Edges among scheduler locks: `rq_lock[i] -> rq_lock[j]` only for i < j
(migrate). Outer locks over one run-queue lock: ipc queue lock, `guard`.
Nothing found that takes `sleep_queue_lock`, `guard`, an ipc lock or
`task_lock` under an `rq_lock`. `rq_lock -> UART lock` exists
(`scheduler_dump`, debug prints, panic). No cycle on the base.

Applying the decision. It adds one load under `rq_lock[this_cpu]` and no
lock. The only new wait is the idle task's poll, which holds nothing between
`schedule()` calls. Chains checked:

- A in its window takes a tick that needs `rq_lock[B]` (run 4's cycle): B
  holds `rq_lock[B]` only for the length of one `schedule()` critical
  section, which no longer waits. No cycle.
- Crosswise wakes (P on A woken onto B, Q on B woken onto A): each CPU
  defers the other's task and switches to its own idle task, which is a
  switch away from its `current`, so each releases the other's task. A
  CPU's release of an `on_cpu` never depends on another CPU's release:
  `schedule()` on the releasing CPU always ends in a switch away or in
  `next == current`, and `next == current` cannot be the woken task unless
  it was queued on that same CPU.
- Steal or migrate of the deferred task: it leaves B's queue under
  `rq_lock[B]`; the next CPU repeats the check under its own lock; B's next
  `schedule()` clears its flag. A stolen task is not pushed on the thief's
  deque (`add_to_cpu_queue_locked` only), so it cannot bounce between idle
  thieves.
- The idle task as `next`: only this CPU saves and restores its own idle
  task, and it completes `sched_finish_switch(idle)` before its next
  `schedule()` (the tick and the x86 IPI honour `preempt_disabled`). The
  exemption is safe if, and only if, `rq` is the executing CPU's run queue.
  That condition fails: section 5.
- x86 IPI and tick while B polls: an ordinary `schedule()` from the ISR.

Not closed by the decision (found in this pass):

- (a) Check-then-wait race in the idle loop (section 4.2).
- (b) An `on_cpu` that never clears (section 4.3).
- (c) B polls `rq_lock[B]` in a tight loop while A's handler, inside the
  window B is waiting on, spins for the same unfair lock (section 4.5).
- (d) Stale `this_cpu` (section 5).

## 3. Ordering derivation (before reading the ADR's)

Publishing store. On the outgoing CPU A, `switch_to` writes x19 to x30, SP,
q0 to q31, FPCR, FPSR and DAIF into `prev->context` with plain stores, loads
the incoming context, moves SP off `prev`'s stack, and returns `prev`.
`sched_finish_switch(prev)` then executes `__atomic_store_n(&prev->on_cpu, 0,
__ATOMIC_RELEASE)`, an `stlr`. Every one of A's earlier accesses in program
order (the context stores, every use of `prev`'s stack, including frames of
interrupts taken during the save) is ordered before a Store-Release (Arm ARM
DDI 0487, B2.3, Load-Acquire and Store-Release). Placement in the code is
correct: `sched.c` 451 to 457 and `task.c` 122, the only two successors of
`switch_to`. Nothing on A touches `prev` after the store.

Acquiring load. On B it must be a load with acquire semantics on
`next->on_cpu` that precedes, in program order, the first read of
`next->context` in `switch_to` and every later use of `next`'s stack. The
decision's `__atomic_load_n(.., __ATOMIC_ACQUIRE)` under `rq_lock[B]` is
that load (`ldapr` with `-mcpu=cortex-a76` / `cortex-a78ae`). If it returns
the 0 written by A's `stlr`, the save is ordered before B's restore.

LDAPR or LDAR. Load-AcquirePC drops only one guarantee of Load-Acquire: order
against an earlier Store-Release by the same PE to a different address. That
matters for store-buffering shapes (each side stores, then loads the other's
flag). The decision has none: A never reads anything of B's to decide
whether to wake it, B polls. The only earlier Store-Release on B that could
be reordered is an unrelated `spin_unlock`, and the `ldaxr` that takes
`rq_lock[B]` sits between it and the load. LDAPR is sufficient.

Which 0. `on_cpu = 1` is a plain store under the switching-in CPU's
`rq_lock`. B must not read a 0 older than A's 1. Requeue path: A's 1 and
A's later requeue are in critical sections of `rq_lock[A]`; the mover takes
`rq_lock[A]` after A's release and `rq_lock[B]`; B takes `rq_lock[B]` after
the mover's release. Wake path: A's 1, then the task's own program order on
A up to the release of the wait-structure lock (`sleep_queue_lock`, `guard`,
ipc queue lock) under which BLOCKED was published; the waker acquires that
lock, then releases `rq_lock[B]`; B acquires it. Every link is an
`stlr`/`ldaxr` pair, so B reads the 1 or a later value. After B's check, the
task is on no queue and B stores 1 under the same lock hold, so no third CPU
can restore or save it concurrently.

`task_destroy`: `task_wait_off_cpu` uses the same acquire load, with the
state load-acquire ahead of it for the "which 0" question. Correct, and
unchanged by the decision.

x86-64. Plain `mov` for both; TSO keeps A's stores in order and B's loads in
order, and the lock is `lock xchg`. Correct.

## 4. Comparison with the ADR, point by point

### 4.1 Agreements

- No lock cycle on the base; the decision adds no edge. Agreed.
- The run-4 tick cycle and the x86 IPI cycle cannot form, because no CPU
  waits while holding a lock. Agreed.
- The crosswise wake cycle cannot form. Agreed (section 2).
- Publishing store, acquiring load, sufficiency of `ldapr`, the "which 0"
  chain, x86 TSO. Agreed, and the ADR's "which 0" paragraph is the part most
  often left out.
- Both named sequences (requeue plus migrate, wake before switch-out) are
  closed at the pick. Agreed by derivation, and consistent with the
  prototype: proto-run01, 40 of 40 R3 migrate, 40 of 40 R3 wake, 40 of 40
  R2, every `swo` line `bad_gen=0 two_cpus=0`; the base fails R3 in
  iteration 1 (probe-run01), as in the builder's baseline.

### 4.2 The deferred task's bound: wrong as stated

ADR, Decision: "The idle loop checks `switch_in_deferred[cpu_id()]` before
WFI/WFE/HLT; if set it skips the wait and calls `yield()` at once, so the
deferred task is picked within one `schedule()` of its release."

The flag is set by a `schedule()` that can run between the idle loop's check
and its wait instruction: from the tick ISR (QEMU, x86) or from
`resched_trampoline`, both with the idle task as `current`. That
`schedule()` defers, sets the flag, finds `next == current` and returns to
the interrupted PC, which is the `wfi` or `hlt`. The CPU then sleeps with the
flag set and the released task at the head of its queue until its next
interrupt. `smp_notify_cpu` is `sev` on ARM64 and does not end a `wfi`; A's
release sends nothing. The bound is therefore the window plus one tick period
(10 ms) on every CPU that has a periodic tick, not one `schedule()`. Derived
from code, not observed. The critic found no build in which an idle CPU both
takes interrupts that call `schedule()` and has no periodic tick, so the
delay is bounded, but the ADR neither states the bound nor the argument.

### 4.3 A release that never comes

ADR, Consequences: "the deferring CPU loses at most one `schedule()` of a
lower-priority `current`."

Two errors. First, the deferring CPU runs idle for the whole deferral, and
every task queued at or below the deferred task's priority waits with it,
including an equal-priority `current` (round-robin requeues it behind the
head). Second, there is no bound when the previous CPU never reaches
`sched_finish_switch`: Pi 5's page-fault path powers a faulting CPU off
(`psci_cpu_off`, cpu_supervisor.c header), and a wedged CPU never switches.
A task that was current there and is then woken (sleep deadline, hand-off)
onto B sits at B's head with `on_cpu` 1 for ever; B polls for ever and runs
nothing at or below that priority. B's `sched_diag_idle_loops` keeps
counting, so the Pi 5 supervisor sees a healthy CPU. On the base the same
task would be restored from a stale context, so this is not a regression in
kind, but the ADR must state it and say why idle was chosen over the next
runnable task.

### 4.4 "Idle poll: no lock, IRQs unmasked"

The decision says the wait is skipped. In `idle_task_func` the IRQ unmask
(`msr daifclr`, x86 `sti`) is in the same statement group as the wait on
every branch. The ADR does not say that only the wait instruction is skipped.
The statement is also false for cooperative secondaries (Jetson
`JETSON_HW_TICK=OFF`, Pi 5 without `SECONDARY_PREEMPT`), whose idle never
unmasks. The no-deadlock argument does not depend on B's IRQ state (A needs
nothing from B's handlers), so this is a wording fix, but an implementation
that wraps the whole block would poll with ticks blocked.

### 4.5 The no-deadlock argument enumerates the wrong thing

ADR: "a handler's locks (`rq_lock[*]`, `sleep_queue_lock`,
`steal_deque_lock[*]`) are each held by their holders only for bounded
sections".

A's window is open to every interrupt, not only the tick: UART, xHCI,
network and GPU handlers and their locks (including the Rust locks of #1093),
the AI policy tick's `sched_model_lock` family, the UART lock. The argument
does not need the list. It needs: the only waiters on an `on_cpu` are the
idle poll, which holds nothing, and `task_destroy`, whose callers must hold
nothing A's handlers can want. The ADR asserts the second ("with no lock
held") without checking `task_destroy`'s callers; neither did the critic.

Missing as well: B's poll takes and drops `rq_lock[B]` in a tight loop, and
the lock is unfair. A handler on A that needs `rq_lock[B]` (a sleeper woken
onto B) competes with the CPU that is waiting for A. Each poll iteration has
a lock-free stretch (return, idle loop, prelude) comparable to the locked
one, so A gets the lock with high probability within a few `sev` wake-ups;
that is an argument about progress the ADR should make, or the poll should
pause between attempts.

### 4.6 Lock list omissions (no cycle in any of them)

`pi_floor.lock` (`guard -> pi_floor.lock`); `ipc_state.lock -> buffer->lock`;
`rq_lock -> UART lock`; ipc queue lock -> `steal_deque_lock[c]` and ipc queue
lock -> policy locks (both through `wake_one` -> `scheduler_add_task`, which
the ADR lists only as ipc queue lock -> `rq_lock[c]`).

## 5. Stale `this_cpu`: a sequence the decision does not close, and depends on

Derivation. `schedule()` reads `this_cpu = cpu_id()` and `rq` at entry
(sched.c 2350, 2351) and takes `rq_lock[this_cpu]` at 2379, with the caller's
IRQ state in between; it also drops the lock around the zombie destroy and
retakes it by the same id (2398 to 2411). In task context IRQs are open and
`preempt_disabled[cpu]` is 0, so a tick there preempts the task (the tick ISR
on QEMU and x86, the trampoline on Pi 5 and Jetson). The nested `schedule()`
requeues it READY. Any mover (migrate, steal, rebalance, wake placement after
a blocked migrate) can put it on another CPU, where it resumes inside the
nested call, returns to the outer prelude, and continues with the old id.
#1229 fixed the same staleness after `switch_to`; nothing covers the part
before the lock. The outer call then, on the wrong CPU's run queue: requeues
`current` there, picks `next` from there, calls `task_set_current(next)` for
the executing CPU, and sets `preempt_disabled[this_cpu]` for the other CPU,
leaving the executing CPU's switch window unguarded against its own tick.

What it does to the decision. `rq_lock[B]`, `rq->idle_task` and
`switch_in_deferred[B]` are all taken from the stale id. The flag is then
written for another CPU ("written and read only by its CPU" is false). A
deferral then switches the executing CPU to another CPU's idle task, which
the decision exempts from the `on_cpu` check and which that CPU is running or
has saved: the invariant "a CPU restores a task's context only after the CPU
that last saved it has published the save" is broken for idle tasks, with
the signature #1238 lists (one idle task on two CPUs). That last step is
derived, not observed (0 `DEFER-TO-FOREIGN-IDLE` lines in both prototype
runs).

Observed. Probe: after the first `rq_lock_irqsave(this_cpu)`, print when
`cpu_id() != this_cpu`. Prototype: the ADR's decision as worded (the check
before the removal in `schedule()`, the same in `scheduler_start()`, the
flag tested before `wfi` in the QEMU idle branch). QEMU virt, 4 CPUs, `make
test SWITCH_CHECK=ON TEST_FILTER=... TEST_REPEAT=...`.

| Run | Tree | Banner build | Filter x repeat | Result |
|---|---|---|---|---|
| probe-run01 | d73604f7 + probe | 20261114041416 | R2, R3 migrate, x20 | R3 fails in iteration 1 (`[switch_check] FIRST: ... previous save not complete, owned by another CPU`, page fault on cpu 1); 0 STALE lines in 1 iteration |
| probe-run02 | d73604f7 + probe | 20261114042013 | R2 x300 | 300 of 300 PASS; 0 STALE lines |
| proto-run01 | d73604f7 + probe + prototype | 20261114042258 | R3 migrate, R3 wake, R2, x40 each | 120 of 120 PASS; 5 STALE lines (printing stopped at 5), all `swo_w`, state RUNNING, for example `STALE this_cpu=1 cpu_id=2 task id=62 'swo_w' state=1 rq_head=swo_m idle_of_stale=idle_1 n=2` |
| proto-run02 | same, print limit raised | 20261114042543 | R3 migrate x150 | 19 PASS; in iteration 20, STALE n=4 (`this_cpu=2`, `cpu_id=3`, task id=235), then on cpu 3 `stack overflow: SP is outside current task's stack / current id=241 ... live_sp=0x4227fb60 but stack=[0x423c0000..0x42400000) / next id=246`, KERNEL PANIC, `System halted`; iterations 20 and 21 FAIL; ended by the 400 s timeout |

In the panic, 0x4227fb60 lies in task 235's stack ([0x42240000..0x42280000)
in the same dump's inventory): cpu 3 was still on task 235's stack with
`current` already set to task 241, inside a nested `schedule()`. That is the
derived chain: the outer call on cpu 3 with `this_cpu=2` set
`preempt_disabled[2]`, not `[3]`, and cpu 3's tick re-entered `schedule()` in
the window. The link between the STALE line and the panic is by adjacency and
by the addresses; it was not traced with SLOT_TRACE.

Limits of this evidence. The prototype is the critic's, QEMU idle branch
only. R2 never produced a STALE line (R2's tasks spend almost no time in the
prelude), so this is not shown to be #1238. The defect is in the base and is
independent of the deferral; the base does not survive R3 long enough to show
it (1 iteration in probe-run01). It is reachable wherever a task that calls
`yield()` or blocks is preempted and then moved, which the Lua binding and
`sched_set_task_affinity` allow in production.

Consequence for the bars. With the base mean of 5.55 iterations to the first
R3 migrate failure in QEMU, the Fixed bar needs 0 failures in at least 28
iterations per run. proto-run02 failed at iteration 20. The ADR's own "What
would overturn this" lists "R3 failing at the fix head". A fix that
implements only the ADR as written is likely to fail its own bar on this
sequence.

Files (local, not on an evidence branch; see section 1):
`.claude/worktrees/s5-crit-c2d/critic-evidence/` in the main checkout.
sha256 prefixes: `probe-run01.test-output.log` a502d718b28a45fc,
`probe-run02.test-output.log` e9ccc2ad74099784,
`proto-run01.test-output.log` bc58c084dbc87e9c,
`proto-run02.test-output.log` 1cd969adee4dd2e2,
`critic-proto-adr0008.patch` 3158fa67d71c2209 (probe + prototype, run 01),
`critic-proto-adr0008-v2.patch` 861ba18fa89af007 (run 02). The probe runs
used the patch's "CRITIC PROBE" hunk alone. Each `*.make.log` is the make
output of the same run.

## 6. Failure modes checked by name

- Critic reading the claim first: no (section 1).
- "Cannot deadlock" claim, tick in the window: lock graph derived first; no
  cycle found; the forced-tick run (R1 phase B on `c2/reverted-wait` and the
  fix head) belongs to the second verdict and was not run here.
- Hypothesis stated as finding: the ADR labels #1238 as a hypothesis. This
  file labels 4.2, 4.3 and the foreign-idle step of section 5 as derived.
- Number without a file: every count above names its log; the logs are
  local only until the orchestrator places them.
- Kills, board claims: none.

## 7. Required changes

Blocking, before the fix is written:

1. Add the stale-CPU sequence to "The sequence being closed" and close it in
   the Decision. Everything per-CPU that `schedule()` and the deferral use
   (`rq`, `rq->idle_task`, `rq_lock`, `switch_in_deferred[]`,
   `preempt_disabled[]`, the zombie fields) must come from a CPU id that is
   valid while the lock is held: read or re-validated with IRQs masked, at
   the first lock and again after the zombie-destroy relock, with the steal
   prelude covered. State the invariant ("from the id read to the unlock the
   task cannot move") and add it to "Invariant established".
2. Add a reproducer for it to R3 (the probe as an assertion is enough) with
   a baseline rate on the base plus the deferral, so the Fixed bar has a
   cell for it; say whether it is filed as its own issue or under #1234.
3. Replace "picked within one `schedule()` of its release" with a true
   statement: either make the idle check and the wait atomic against
   interrupts (check with IRQs masked, then wait; `wfi` wakes on a pending
   masked interrupt; `sti; hlt` adjacent on x86), or state the bound of one
   tick period and show, per build, that every CPU whose idle can be
   interrupted into `schedule()` has a periodic tick.
4. State the unbounded case (previous CPU powered off or wedged) and its
   effect on the deferring CPU and on the Pi 5 supervisor; justify idle over
   the next runnable task, or bound the deferral. Correct the "at most one
   `schedule()` of a lower-priority `current`" sentence.

Corrections to the argument (the design does not change):

5. Say that only the wait instruction is skipped and the unmask is kept;
   correct "IRQs unmasked" for cooperative secondaries.
6. Restate the no-deadlock argument without the three-lock list: no holder of
   any lock waits on an `on_cpu`. Check `task_destroy`'s callers for held
   locks or say it was not checked. Add the progress argument for A's handler
   against B's poll on the unfair `rq_lock[B]`.
7. Complete the lock list (section 4.6) and say why the idle exemption in the
   check is safe once change 1 is in.

Nothing to change in the ordering argument.
