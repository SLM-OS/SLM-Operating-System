# ADR 0008: a task is switched in only after its previous CPU has saved it

## Status

Proposed (switch gauntlet run 5, C2). Design checkpoint before any fix code;
the blind design critic reviews it against the code.

## Context

### The sequence being closed

`on_cpu` (#1213, #1237) is set to 1 by the CPU that switches a task in, under
that CPU's `rq_lock`, and cleared with a store-release by the same CPU in
`sched_finish_switch()` right after `switch_to()` has written the task's
callee-saved registers, SP, FP/SIMD state and DAIF into `task->context` and
moved SP off the task's stack. Until then the task's saved context is stale
(it holds the previous switch-out) and its stack is live.

A task becomes runnable on another CPU while `on_cpu` is still set in two
ways:

1. **Requeue path.** `schedule()` on CPU A puts its RUNNING `current` back
   on A's queue as READY, picks another task, and drops `rq_lock[A]`
   (`rq_unlock_irqrestore`, which also restores the caller's IRQ mask)
   before `switch_to()`. In that window any path that takes `rq_lock[A]`
   can move the READY task to CPU B: `sched_migrate_task` (from
   `sched_set_task_affinity`, the Lua binding `lua_slm.c`, tests),
   `sched_try_steal` (WORK_STEALING), and `sched_rebalance_tick` (heuristic
   policy, CPU 0's tick). B's `schedule()` then picks it and `switch_to()`
   loads the stale context while A is still running on that stack.
2. **Wake path.** A task sets `TASK_BLOCKED` and publishes itself on a wait
   structure (`task_sleep_ms`: sleep queue; `pi_mutex_lock`: wait queue;
   `ipc.c block_on_queue`) under that structure's lock, drops the lock, and
   only then calls `schedule()`. A waker that takes the lock in between
   (`task_sleep_wake`, `pi_mutex_unlock` hand-off, `ipc wake_one`,
   `task_wake_sleepers` from any CPU's tick) sets READY and enqueues it on
   a CPU chosen by `assigned_cpu`, affinity or the policy's `assign_cpu`.
   That CPU can switch it in before the blocking CPU has even entered
   `schedule()`, a window of the whole `schedule()` prelude plus the
   switch.

Neither path checks `on_cpu`. Evidence on the base (main 8ec0e330 +
c1/harness 6e39f425 + the tests, `c2/switch-window` 88d4d71b):

Rates (tables and every file in the run-5 log; a cell that never failed
cannot later be used to claim a fix):

| Reproducer | QEMU (20 runs) | Jetson kexec (x200, 3 boots) | Jetson UEFI-direct (x200) | pi-5-1 (x200, 3 boots) |
|---|---|---|---|---|
| R1 `test_switch_window_held` (SWITCH_WINDOW_TEST=ON) | 20 of 20 fail at iteration 1 (14 fault, 6 hang) | not run (QEMU-only reproducer) | not run | not run |
| R2 `test_preempt_flag_follows_migrated_task` | not run | 3 of 3 boots fail at iteration 1 (2 fault dumps, 1 hang) | 1 boot: 38 of 200 fail, first at 7 (2 dumps) | 3 of 3 boots 200/200: never failed |
| R3 migrate `test_migrate_ready_during_switch_out` | 20 of 20 fail, mean 5.55 iterations to the first | 1 of 3 boots (hang at 54; 454 iterations, 1 failure) | not run | 3 of 3 boots 200/200: never failed |
| R3 wake `test_wake_during_switch_out` | 20 of 20 fail, mean 1.1 | 3 of 3 boots hang at iteration 1 | 1 boot: hang at iteration 1 | 3 of 3 boots fail at iteration 1 |

Files: `gauntlet-5-hw-pi5` `docs/archive/switch-gauntlet/run5-hw/c2/qemu-base/`
(R1, R3) and `hw-base/` (Pi 5); `gauntlet-5-hw-jetson`
`docs/archive/switch-gauntlet/run5-hw/c2/hw-base/`. R3 aims its movers by
reading `on_cpu` (no kernel change) and has the wake phase; both were added
because a plain migrate loop made 60k migrations per run in QEMU with no hit.
A HANG (timeout, no dump) is attributed to the window by exclusion only.

**Traced sequence** (`SLOT_TRACE`, `c2/trace` 1b7410f1, which dumps the rings
the moment `schedule()` is about to switch in a task whose `on_cpu` is set;
`gauntlet-5-hw-pi5` `.../c2/qemu-trace/trace-r3{m,w}-1b7410f1-run01..03.txt`,
all six runs dumped in iteration 1):

- Migrate, all three dumps: CPU A requeues its running task T as READY
  (`ADD`, caller `schedule`, `on_cpu`=1), records `SWITCH` to its next task
  and has not reached `SWITCH_RET` (so not `sched_finish_switch`); CPU B's
  mover `REMOVE`s T from A's queue and `ADD`s it to B's with `on_cpu`=1
  (`MIGRATE` c=0x10001); B's `schedule()` picks T with `on_cpu` still 1.
  Run 1: T id 11, A = CPU 2 (`SWITCH` #61349, no later CPU 2 record), B =
  CPU 0 (#61369 to #61371, busy switch-in #61421).
- Wake, all three dumps: T (running on A, TASK_BLOCKED published in
  `task_sleep_ms`) is woken by `task_sleep_wake` on another CPU and queued
  there with `on_cpu`=1 (`ADD`, caller `scheduler_add_task_to_cpu`) before A
  has switched out of it; that CPU's `schedule()` picks it while `on_cpu` is
  1. Run 2: T id 7 resumes on CPU 3 (#1336), CPU 0 queues it (#1338), CPU 0
  picks it (#1353) around CPU 3's switch away (#1346).
- In four of the six dumps (migrate run 3, wake runs 1 to 3) the busy
  record comes after the outgoing CPU's `SWITCH_RET` in trace order. The
  check's load precedes its own record in program order and a QEMU vCPU can
  be descheduled between them, so trace order is not load order. What every
  dump shows is that the switch-in decision was taken on a load that had not
  yet observed the outgoing CPU's release, i.e. with nothing ordering the
  save before the restore. Whether a given restore then read stale words
  depends on timing; the R3 generation and fault failures above are the
  cases where it did.

Both are the #1234 window. **#1238 (R2 on Jetson) is not traced**: its fault
signatures (EC 0xe illegal execution state and EC 0 on idle and `pdm` tasks
with wrong frames) match R3's on the same board, and R2's test does exercise
wake placement onto other CPUs, but no Jetson trace of R2 exists yet.
Hypothesis, not finding: R2 is the wake path of this window. Test: R2 at the
fix head on kexec and UEFI-direct; if it still fails, #1238 splits from
#1234 and gets a Jetson trace.

### Why the reverted design deadlocked (run 4)

1ead6ef6 spun on `next->on_cpu` under `rq_lock[B]`. The outgoing CPU A's
window is open to IRQs (`rq_unlock_irqrestore` restores the caller's DAIF;
`switch_to` restores the incoming task's DAIF before `sched_finish_switch`).
A tick on A inside the window runs `task_wake_sleepers` ->
`scheduler_add_task_to_cpu(t, B)` -> `spin_lock(rq_lock[B])`, held by B
waiting for A: a cycle (critic-4-C1.md section 1.5; 2 of 2 at 6d4fe041).
x86 had a second cycle through the resched IPI (#1236).

There is a third cycle the spin design has even with IRQs masked through
the window (shape (c) below), through the wake path: task P runs on A and
task Q runs on B; each blocks (TASK_BLOCKED published) and each is woken
onto the other CPU before it calls `schedule()` (for example two pi_mutex
hand-offs, or `ipc wake_one` placing by policy). A's `schedule()` does not
requeue P (it is READY, not RUNNING), picks Q and spins on Q's `on_cpu`
under `rq_lock[A]`; B picks P and spins on P's `on_cpu` under `rq_lock[B]`.
Each `on_cpu` is cleared only by the CPU that is spinning on the other. No
IRQ is involved. 1ead6ef6's argument ("next must already be on that queue,
so two CPUs cannot each wait for the other's outgoing task") assumed the
requeue path only.

## Decision

**Defer the switch-in, never wait for it under a lock.** In `schedule()`
(and `scheduler_start()`), under `rq_lock[B]`, after `next` is chosen and
before anything else is done with it:

```c
if (next != current && next != rq->idle_task &&
    __atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE)) {
    /* Previous CPU has not finished switching out of next (#1234). Leave
     * it at the head of this queue and run idle, which polls without
     * sleeping until a schedule() finds it released. */
    switch_in_deferred[B] = 1;
    next = rq->idle_task;
} else {
    switch_in_deferred[B] = 0;
}
```

- `next` stays queued (it is not removed: the removal below only removes
  `rq->head` when it is `next`). A requeued `current` stays queued too; the
  CPU switches to its idle task, or keeps running idle if `current` is
  idle (then `next == current` and nothing switches).
- The idle loop checks `switch_in_deferred[cpu_id()]` before WFI/WFE/HLT; if
  set it skips the wait and calls `yield()` at once, so the deferred task
  is picked within one `schedule()` of its release. The flag is per-CPU,
  written and read only by its CPU.
- Nothing else changes: migrate, steal, rebalance, wake placement and
  `task_destroy` keep their code. Every path that can make a task runnable
  on another CPU (including ones added later, and the Lua binding) is
  closed at the single point where a context is restored.

### Candidate shapes considered

- (a) Migrate, steal and rebalance refuse a task with `on_cpu` set, and wake
  placement redirects it to the CPU it is on. Correct only if every enqueue
  path is covered (seven today: migrate, steal, rebalance,
  `scheduler_add_task`, `scheduler_add_task_to_cpu`, `sched_set_task_affinity`,
  and the requeue itself); the wake path needs the running CPU recorded
  separately from `assigned_cpu` (which `sched_migrate_task` rewrites for a
  BLOCKED task, as `test_preempt_flag_follows_migrated_task` does); a
  refused `sched_set_task_affinity` leaves a task off its affinity. Rejected:
  many sites, and one missed site reopens the window.
- (b) The waiter spins with no run-queue lock held and re-validates. Fine for
  `sched_migrate_task` from task context, but the pick in `schedule()`
  cannot spin anywhere in the outgoing-CPU sequence without the cross-CPU
  cycle above (both CPUs in their own windows, each waiting for the other's
  task). Spinning before taking `rq_lock[B]` has nothing to wait on yet.
  The decision is (b) reduced to its safe form: the waiting happens in the
  idle loop, which holds nothing.
- (c) Keep the outgoing CPU's IRQs masked from the context save through the
  `on_cpu` release, then spin under `rq_lock`. Removes the tick and IPI
  cycles but not the wake-path cycle, and needs `context.S` (ARM64 DAIF,
  and FIQ for `PI5_FIQ_TIMER`), x86 RFLAGS and `task_entry_wrapper`
  changes. Rejected.

## Lock graph

Locks (all spinlocks; `_irqsave` unless noted): `rq_lock[c]`,
`steal_deque_lock[c]`, `sleep_queue_lock`, `pi_mutex.guard`, ipc queue
locks, `task_lock` (task table), `sched_model_lock` /
`sched_model_stage_lock` (AI runtime config), the UART lock.

Edges (A -> B means B is taken while A is held), from the code at 88d4d71b:

| Path | Locks, in order |
|---|---|
| `schedule()` on B (switch-out side) | `steal_deque_lock[v]` (released), then `rq_lock[v]` (released) in `sched_try_steal`; then `rq_lock[B]`; zombie reap drops it around `task_destroy_gen` (task_lock, PMM; its `on_cpu` wait holds no run-queue lock); `rq_lock[B]` dropped before the window. Window, `switch_to`, `sched_finish_switch`: no lock. |
| new: deferral | one load-acquire under `rq_lock[B]`; no wait. Idle poll: no lock, IRQs unmasked. |
| tick (`scheduler_tick`, any CPU, also inside that CPU's own window) | `sleep_queue_lock` (released) then per woken task `rq_lock[t]` (released) then `steal_deque_lock[t]`; AI: `rq_lock[c]` one at a time in `ai_update_top_tasks`; heuristic: `rq_lock[busy]` (released), `rq_lock[idle]` (released), `steal_deque_lock[idle]`. Never nested. |
| `sched_migrate_task` | `rq_lock[min]` -> `rq_lock[max]` (ascending CPU id). |
| `sched_try_steal` | `steal_deque_lock[v]` released before `rq_lock[v]`. |
| `scheduler_add_task_to_cpu` | `rq_lock[c]` released before `steal_deque_lock[c]`. |
| `pi_mutex_lock` / unlock | `guard` -> `rq_lock[c]` (reposition); unlock drops `guard` before `scheduler_add_task_to_cpu`. |
| `ipc wake_one` | ipc queue lock -> `rq_lock[c]` (via `scheduler_add_task`). |
| `task_destroy` | `on_cpu` wait (load-acquire, 2 s bound) with no lock held, then `task_lock` alone; PMM after. (`ai_update_top_tasks`' comment says the generation bump holds the rq_lock too; the code takes only `task_lock`.) |
| x86 resched IPI | handler returns while `preempt_disabled[cpu]` (#1236); otherwise `schedule()` as above. |

The only nesting among run-queue locks is migrate's ascending pair; the
other nestings are outer non-scheduler locks over a single `rq_lock`, never
the reverse (nothing under `rq_lock` takes `guard`, an ipc lock or
`sleep_queue_lock`). The graph is acyclic and the decision adds no edge.

### Why no deadlock with a tick or IPI anywhere in the window

The decision adds exactly one wait-for relation: CPU B's idle task polls
until CPU A clears `next->on_cpu`. While polling B holds no lock and has
IRQs unmasked. A clears `on_cpu` after finishing its window, `switch_to`
and the call to `sched_finish_switch`; on that path A takes no lock except
inside interrupt handlers, and a handler's locks (`rq_lock[*]`,
`sleep_queue_lock`, `steal_deque_lock[*]`) are each held by their holders
only for bounded sections that do not wait on any CPU's `on_cpu`. B in
particular holds `rq_lock[B]` only inside `schedule()`, where the deferral
is a single load. So A's progress depends on no action of B, and no cycle
can close through B. A tick on B while it polls is ordinary (preemption of
idle). A tick on A in its window runs `scheduler_tick`, which returns
without scheduling while `preempt_disabled[A]` is set, as today. The x86
resched IPI is unchanged (#1236). The cross-CPU wake cycle cannot form
because neither CPU waits: each defers the other's task and runs idle.

Forced check: R1 phase B puts a tick into CPU 0's held window that takes
CPU 3's `rq_lock` while CPU 3 has picked the migrated task (no-deadlock
bar). The fix head must complete it; `c2/reverted-wait` (1ead6ef6's spin
on this base) must hang there.

## Ordering argument (Arm ARM DDI 0487, B2.3)

- **Publishing store:** the `stlr` of `prev->on_cpu = 0` in
  `sched_finish_switch()` on A. Every memory access before it in program
  order, which includes `switch_to`'s `stp`/`str` of x19 to x30, SP,
  q0 to q31, FPCR, FPSR and DAIF into `prev->context`, and A's last use of
  `prev`'s stack, is Barrier-ordered-before it (Store-Release).
- **Acquiring load:** the `ldapr` of `next->on_cpu` in B's `schedule()`
  under `rq_lock[B]`. It is Barrier-ordered-before every later access in
  program order on B, including `switch_to`'s loads of `next->context`
  after `rq_unlock` (Load-AcquirePC; program order survives the unlock).
- If B's load reads 0 from A's store (reads-from, external), the save is
  Ordered-before the restore loads, so they see the saved values.
- **Which 0 it can read.** The 1 written at the task's last switch-in (a
  plain store under `rq_lock[A]`) is ordered before B's load by the lock
  chain that delivered the task to B's queue: requeue path, A's `rq_unlock`
  (release) -> mover's `rq_lock[A]` (acquire) -> mover's `rq_unlock[B]` ->
  B's `rq_lock[B]`; wake path, the task's own program order on A after the
  1, then the wait-structure lock release -> waker's acquire -> waker's
  `rq_lock[B]` release -> B's acquire. So B cannot read an older 0 than
  that 1 (coherence), only the 1 or A's release of it.
- Once B has removed `next` from its queue and set `on_cpu = 1` under
  `rq_lock[B]`, no other CPU can pick it (it is on no queue), so nothing
  can re-open the window between B's check and B's `switch_to`.
- x86-64: TSO; the same `__atomic` release/acquire compile to plain moves
  with compiler barriers, which TSO orders.

## Invariant established

A CPU restores a task's context only after the CPU that last saved it has
published the save (`on_cpu` store-release), observed by the restoring CPU
with a load-acquire under its own `rq_lock`. No CPU waits for that while
holding a lock.

## Consequences

- One load and a compare per context switch. bench smp context-switch time
  is measured against the base (bar: within 10%).
- A deferred task waits for its previous CPU's window (normally tens of
  instructions; longer if that CPU takes an interrupt in it) while the
  deferring CPU runs idle: the deferring CPU loses at most one `schedule()`
  of a lower-priority `current`.
- Stealing or migrating a task mid-switch is still allowed and simply
  waits; rebalance and steal could skip such tasks as an optimisation
  (not done).
- R3 makes `sched_migrate_task` print "Cannot migrate running task" about
  14 times per iteration on Jetson (2741 lines in 200 iterations) and about
  800 times per 300 ms in QEMU: a benign race between the caller's state
  read and migrate's unlocked RUNNING check. Not a rate distortion; no
  rate limit in this decision.
- Side defect seen in the trace, not part of this decision: #1241
  (`sched_migrate_task` rewrites `assigned_cpu` of a task queued on another
  CPU).

## What would overturn this

A trace in which a context is restored while its `on_cpu` is set at the
fix head; R1, R2 or R3 failing at the fix head; a hang with a CPU spinning
in the idle poll while the task's previous CPU is not in its window.
