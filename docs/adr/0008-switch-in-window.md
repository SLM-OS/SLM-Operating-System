# ADR 0008: a task is switched in only after its previous CPU has saved it

## Status

Proposed, round 2 (switch gauntlet run 5, C2). Design checkpoint before any
fix code. Round 1 (d73604f7) failed its blind design critic
(`critic-5-C2-design.md` on `gauntlet/switch-5`, section 7): the decision
depended on a CPU id `schedule()` reads with interrupts open (#1242), the
deferred task's bound was misstated, and the no-deadlock argument rested on
a lock list. This revision answers items 1 to 7; "Changes from round 1" at
the end maps each item to the text.

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

A third sequence, found by the round-1 critic and filed as #1242, is not a
window on another CPU's context but the same class of error inside one
`schedule()` call, and the decision below depends on closing it:

3. **Stale CPU id (#1242).** `schedule()` reads `this_cpu = cpu_id()` and
   `rq` at entry and takes `rq_lock[this_cpu]` later, with the caller's
   interrupt state in between (COOP tick, steal prelude); it also drops the
   lock around the zombie destroy and retakes it by the same id. In task
   context interrupts are open and `preempt_disabled[cpu]` is 0, so a tick
   there preempts the task (the tick ISR on QEMU and x86, the trampoline on
   Pi 5 and Jetson); the nested `schedule()` requeues it READY, a mover puts
   it on another CPU, it resumes there inside the nested call, returns to
   the outer prelude and continues with the old id. The outer call then
   works on the wrong CPU's run queue: it requeues `current` there, picks
   from there, sets `preempt_disabled[]` for the other CPU (leaving its own
   window unguarded against its own tick), and in the round-1 design it
   would write the other CPU's deferral flag and switch to the other CPU's
   idle task, which the check exempts. #1229 fixed the same staleness after
   `switch_to`; nothing covers the part before the lock.

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
#1234 and gets a Jetson trace. (A Jetson R2 SLOT_TRACE capture is scheduled;
this section is pending until it lands.)

**#1242 evidence.** Reproducer: `SWITCH_CHECK` now checks, under
`rq_lock[this_cpu]` at the first lock and after the zombie relock, that
`cpu_id() == this_cpu`; both R3 tests print the count and fail on any
(`c2/switch-window` 7a95c012). Base rate on "base plus the round-1 deferral"
(`c2/proto-1242` 1f58b249 = 7a95c012 plus the critic's prototype
`critic-proto-adr0008-v2.patch` without its probe hunk), R3 migrate,
`SWITCH_CHECK=ON`, x40, QEMU: 5 of 5 runs show it; 26 of 128 reported
iterations have a stale lock; first at iterations 1, 1, 4, 1, 5 (mean 2.4);
3 of 5 runs end in a fault. In run 04 the instrument also caught the
derived step: `[switch_check] FIRST: task id=3 'idle_2' switched in on cpu
2: previous save not complete, owned by another CPU (owner=3)`, in an
iteration with `stale_cpu=1`, i.e. a CPU holding the stale id 2 switched in
CPU 2's idle task while CPU 2 owned it (the "one idle task on two CPUs"
signature of #1238). Files: `gauntlet-5-hw-pi5` cdb60557,
`docs/archive/switch-gauntlet/run5-hw/c2/qemu-1242-base/r3m-proto-1f58b249-run01..05.txt`.
On the plain base the window kills R3 before the stale id shows (critic
probe-run01). Filed as its own issue (#1242, P1); the fix closes both.

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

Two parts. Part 1 makes the CPU id `schedule()` uses valid for its whole
locked section (#1242); part 2 defers a switch-in whose previous CPU has
not released the task (#1234), relying on part 1.

### Part 1: pin the CPU id

`schedule()` masks the interrupts that can reach `scheduler_tick` before it
reads `cpu_id()`, and keeps them masked until the unlock before the switch
window:

```c
void schedule(void)
{
    irq_flags_t flags = sched_irq_save();   /* I; also F under PI5_FIQ_TIMER */
    uint32_t this_cpu = cpu_id();
    struct cpu_runqueue *rq = cpu_rq(this_cpu);
#if defined(COOP_PREEMPT)
    coop_preempt_maybe_tick(this_cpu);      /* scheduler_tick masked, as in the ISR */
#endif
#if CONFIG_WORK_STEALING
    if (!rq->head) {
        struct task *stolen = sched_try_steal(this_cpu);
        if (stolen) { rq_lock(this_cpu); add_to_cpu_queue_locked(stolen, this_cpu); rq_unlock(this_cpu); }
    }
#endif
relock:
    rq_lock(this_cpu);                      /* spin_lock: interrupts already masked */
    if (rq->zombie) {
        ... take zombie, zombie_gen ...
        rq_unlock(this_cpu);
        sched_irq_restore(flags);           /* destroy with the caller's state */
        (void)task_destroy_gen(zombie, zombie_gen);
        flags = sched_irq_save();
        this_cpu = cpu_id();                /* re-read: we may have moved */
        rq = cpu_rq(this_cpu);
        goto relock;
    }
    ... requeue, pick, part 2, task_set_current, preempt_disabled[this_cpu] = 1 ...
    rq_unlock_irqrestore(this_cpu, flags);  /* opens the window, as today */
```

Why it pins. `current` leaves this CPU only by switching out, and it
switches out only in a `schedule()` call. With the tick's interrupt class
masked, no nested `schedule()` can run on this CPU (the tick ISR, the
trampoline arming in `maybe_arm_resched_trampoline` and the x86 resched IPI
are all interrupt paths), and the code between the read and the unlock
calls `schedule()` nowhere (`coop_preempt_maybe_tick` runs `scheduler_tick`
with `preempt_disabled[cpu]` set, which suppresses its `schedule()`; the
steal prelude and the zombie path take spinlocks only). A mover cannot take
a running task off this CPU: it moves queued READY tasks or rewrites fields
of BLOCKED ones, and a woken-before-switch-out `current` that sits on
another CPU's queue is deferred there by part 2 until this CPU has saved
it. So from the id read to the unlock the executing CPU is `this_cpu`.

Coverage of every use. `rq`, `rq->idle_task`, `rq->zombie`/`zombie_gen`,
`rq_lock[this_cpu]`, `preempt_disabled[this_cpu]`, the deferral state of
part 2, `sched_diag_*[this_cpu]`, the coop tick's per-CPU timestamp and the
steal trace's per-CPU writers all take the id inside the pinned region. The
zombie path unpins for the destroy (it can take PMM locks and wait up to
2 s) and re-reads the id with interrupts masked before it touches anything
per-CPU again. After `switch_to` the task re-reads the id as today (#1229).
`scheduler_start()` runs on its own CPU with interrupts masked from entry.

Cost. The steal prelude now runs with interrupts masked: at most
`(cpu_count - 1) * STEAL_DEQUE_CAPACITY` pop-and-check pairs, each a short
irqsave section that was already interrupt-masked inside. COOP builds now
run the cooperative tick with interrupts masked, as the hardware tick ISR
runs. Both lengthen interrupt latency on the scheduling CPU by that
bounded amount only.

`PI5_FIQ_TIMER` (experimental, default OFF, not in the bars) routes the
tick through FIQ; `irq_save` masks only I, so there the existing
`rq_lock_irqsave` sections are open to the tick already (a pre-existing
hazard: a tick under `rq_lock[c]` that wakes a sleeper onto c spins on it).
`sched_irq_save` masks I and F in that build; the rest of the
`rq_lock_irqsave` users are not changed by this ADR.

### Part 2: defer the switch-in

In `schedule()` and `scheduler_start()`, under `rq_lock[this_cpu]`, after
`next` is chosen and before anything else is done with it:

```c
static inline bool switch_in_ready(struct task *t, struct task *current)
{
    return t == current || !__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE);
}
...
switch_in_waiting[this_cpu] = NULL;
if (next != rq->idle_task && !switch_in_ready(next, current)) {
    struct task *alt = NULL;
    for (struct task *t = next->next; t; t = t->next) {    /* queue order */
        if (switch_in_ready(t, current)) { alt = t; break; }
    }
    if (alt) {
        next = alt;                       /* removed from the middle below */
    } else {
        switch_in_waiting[this_cpu] = next; /* idle polls this task's on_cpu */
        next = rq->idle_task;
    }
}
/* remove next from the queue wherever it is (not only at the head) */
```

- The deferred task stays queued at its place. The CPU runs the
  highest-priority queued task that is ready to switch in (possibly the
  requeued `current`, then nothing switches), and only if there is none it
  runs its idle task. Round 1 always ran idle; round 2 runs the next
  ready task so that a deferral never stops this CPU's other work
  (item 4, below).
- `rq->idle_task` is exempt from the check. With part 1, `rq` is the
  executing CPU's run queue, and a CPU's idle task is switched in and out
  only by that CPU: whenever this CPU executes `schedule()` its idle task
  is either `current` or was switched out by this CPU, which ran
  `sched_finish_switch` for it before executing anything else. Its
  `on_cpu` is therefore 0 or it is `current` (item 7). Without part 1 the
  exemption is unsafe (#1242 run 04 above).
- `switch_in_waiting[c]` is written only under `rq_lock[c]` by CPU c (part
  1) and read only by c's idle loop.

**Idle loop.** Only the wait instruction is skipped, never the interrupt
unmask; and the check is atomic with the wait (item 3, item 5):

```c
/* ARM64 branches that unmask and WFI (QEMU CPU 0, Pi 5 / Jetson with
 * SECONDARY_PREEMPT or JETSON_HW_TICK, PI5_FIQ_TIMER): */
msr daifset, #2 (#3 for FIQ);  isb;
struct task *w = switch_in_waiting[cpu];        /* read with the tick masked */
if (!w) {
    wfi;                     /* wakes on a pending interrupt even while masked */
}
msr daifclr, #2 (#3);  isb;  /* the pending interrupt is taken here */
if (w) {
    /* bounded lock-free poll of the one word, then re-schedule */
    uint64_t t0 = timer_get_count();
    while (__atomic_load_n(&w->on_cpu, __ATOMIC_ACQUIRE) &&
           timer_get_count() - t0 < POLL_CYCLES)       /* about 50 us */
        cpu_relax();
}
yield();
```

The flag can be set only by a `schedule()` on this CPU. On these branches
such a `schedule()` can come asynchronously (tick ISR on QEMU CPU 0 and
x86; the trampoline, which runs at the interrupted PC's `eret`, on Pi 5 and
Jetson), but not between the masked read and the `wfi`, because the tick is
masked there; WFI's wake-up events include a pending physical IRQ or FIQ
whatever PSTATE.I and PSTATE.F hold (Arm ARM DDI 0487, "Wait for Interrupt",
wake-up events; the same property Linux's `cpu_do_idle` relies on with
interrupts disabled), and the interrupt is taken at the unmask. So the idle CPU never sleeps with a task waiting on its queue.
x86 uses `cli; read; if (!w) { sti; hlt } else sti` (the `sti` interrupt
shadow covers the `hlt`). Branches that WFE and never unmask in idle
(cooperative secondaries: QEMU CPUs 1 to 3, Jetson `JETSON_HW_TICK=OFF`,
Pi 5 without `SECONDARY_PREEMPT`) take no interrupt into `schedule()` in
idle at all, so the only writer is idle's own `yield()`, which runs before
the read: check-then-`wfe` is race-free there, and the IRQ state of those
CPUs is unchanged (masked, as today; round 1 wrongly said "IRQs
unmasked").

Bounds (item 3, item 4).

- Released task, deferring CPU idle: picked at the next `schedule()` after
  the poll sees `on_cpu` 0, so within one poll iteration of the release
  (a few microseconds), never a tick.
- Released task, deferring CPU running another ready task R: picked at that
  CPU's next `schedule()`: its next tick on CPUs with periodic preemption
  (10 ms), or R's next yield or block on cooperative CPUs. That is the
  latency any task queued behind a running R has on that CPU; deferral adds
  only its window. The round-1 sentence "loses at most one `schedule()` of a
  lower-priority current" was wrong and is withdrawn.
- Never released (the previous CPU is powered off or wedged: the Pi 5
  page-fault path powers a faulting CPU off with `psci_cpu_off`, see the
  `cpu_supervisor.c` header; a wedged CPU never switches): the task stays
  queued and is never switched in. It cannot be: its saved context
  predates its last run, so running it is the #1234 fault, which is what
  the base does. The deferring CPU keeps running every other ready task, so
  only the dead CPU's last task is lost, and the deferring CPU's
  `sched_diag_idle_loops` and its tasks' progress keep the Pi 5 supervisor's
  view correct (that CPU is healthy). Choosing idle (round 1) would instead
  stall every task at or below the dead task's priority on a healthy CPU
  while it looked live to the supervisor; that is why round 2 runs the next
  ready task. No deferral bound or timeout is added: a timeout could only
  end by running a stale context.

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
| new (part 2): deferral | load-acquires under `rq_lock[B]` (the candidate walk); no wait. Idle poll: no lock; IRQ state as the idle branch sets it (unmasked on ticking branches, masked on cooperative secondaries). |
| tick (`scheduler_tick`, any CPU, also inside that CPU's own window) | `sleep_queue_lock` (released) then per woken task `rq_lock[t]` (released) then `steal_deque_lock[t]`; AI: `rq_lock[c]` one at a time in `ai_update_top_tasks`; heuristic: `rq_lock[busy]` (released), `rq_lock[idle]` (released), `steal_deque_lock[idle]`. Never nested. |
| `sched_migrate_task` | `rq_lock[min]` -> `rq_lock[max]` (ascending CPU id). |
| `sched_try_steal` | `steal_deque_lock[v]` released before `rq_lock[v]`. |
| `scheduler_add_task_to_cpu` | `rq_lock[c]` released before `steal_deque_lock[c]`. |
| `pi_mutex_lock` / unlock | `guard` -> `rq_lock[c]` (reposition); unlock drops `guard` before `scheduler_add_task_to_cpu`. |
| `ipc wake_one` | ipc queue lock -> `rq_lock[c]` (via `scheduler_add_task`). |
| `task_destroy` | `on_cpu` wait (load-acquire, 2 s bound) with no lock held, then `task_lock` alone; PMM after. (`ai_update_top_tasks`' comment says the generation bump holds the rq_lock too; the code takes only `task_lock`.) |
| `pi_mutex` priority floor | `guard` -> `pi_floor.lock`. |
| ipc | `ipc_state.lock` -> `buffer->lock`; queue lock -> `rq_lock[c]` -> released, then `steal_deque_lock[c]`, and queue lock -> policy locks (`sched_runtime_balance_config_acquire`, AI builds), all through `wake_one` -> `scheduler_add_task`. |
| printing | `rq_lock[c]` -> UART lock (DEBUG/WARN/panic under the run-queue lock); every interrupt handler's own locks (UART, xHCI, network, GPU, Rust locks of #1093, `sched_model_lock` family of the AI policy tick). None of these holders waits on an `on_cpu`. |
| new (part 1) | interrupts masked from the id read to the unlock; no new lock. |
| x86 resched IPI | handler returns while `preempt_disabled[cpu]` (#1236); otherwise `schedule()` as above. |

The only nesting among run-queue locks is migrate's ascending pair; the
other nestings are outer non-scheduler locks over a single `rq_lock`, never
the reverse (nothing under `rq_lock` takes `guard`, an ipc lock or
`sleep_queue_lock`). The graph is acyclic and the decision adds no edge.

### Why no deadlock with a tick or IPI anywhere in the window

Stated without the lock list (item 6). A deadlock needs a cycle of waits.
The base has no cycle among locks (the graph above, unchanged). Part 1 adds
no lock and no wait: it only masks interrupts earlier on the scheduling CPU
over sections that take spinlocks already taken with interrupts masked.
Part 2 adds no lock and no wait inside `schedule()`. The only waits on any
task's `on_cpu` in the kernel after the change are:

1. the idle poll of part 2: no lock held, bounded (about 50 us) between
   `schedule()` calls; and
2. `task_destroy` / `task_destroy_gen` (`task_wait_off_cpu`, 2 s bound, then
   it warns and leaks the slot). Its callers, checked for spinlocks held at
   the call (the 25 lines before each call and the function's own locking;
   call chains above the function were not traced): the zombie reap in
   `schedule()` (after `rq_unlock` and, with part 1, with the caller's
   interrupt state restored), `cmd_kill` (shell), `l_task_kill` (Lua binding; what
   its Lua callers hold was not traced), `coh_remote_abandon`, `component_spawn_builtin`'s error path,
   `bench_workload_run`, and test code. None holds a spinlock there. Even a
   caller that did would wait at most 2 s, so this wait cannot be part of an
   unending cycle.

No holder of any lock waits on an `on_cpu` (wait 1 holds nothing; wait 2's
callers hold no spinlock and are bounded). The CPU A whose `on_cpu` is
awaited completes its window, `switch_to` and `sched_finish_switch` taking
no lock outside interrupt handlers, and whatever a handler on A waits for
(any lock, of any driver, the UART lock, Rust locks) is held by holders that
never wait on an `on_cpu`. So the release always comes unless A stops, the
case stated under part 2. A tick or IPI anywhere in A's window is one of
those handlers; on B (the deferring CPU) a tick preempts idle or a task as
usual. The run-4 tick cycle, the x86 IPI cycle and the crosswise wake cycle
all need a CPU that waits on an `on_cpu` while holding a lock; none does.

Progress of A's handlers against B's poll (item 6). The round-1 poll was
`yield()` in a tight loop, retaking `rq_lock[B]` (an unfair test-and-set
lock) as fast as it could, which competes with a handler on A that needs
`rq_lock[B]` (a sleeper woken onto B). Round 2's poll reads only the
waiting task's `on_cpu` word, lock-free, for up to about 50 us between
`schedule()` calls, so B takes `rq_lock[B]` at most once per poll period
while it waits; A's handler finds the lock free except during that one
short section. B's other `schedule()` calls (if it runs another ready task)
are the ordinary rate of that CPU.

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

1. From its `cpu_id()` read to the unlock before the switch window,
   `schedule()` runs with the tick's interrupt class masked, so the task
   cannot move and every per-CPU field it touches is the executing CPU's
   (#1242). After `switch_to` the id is re-read (#1229).
2. A CPU restores a task's context only after the CPU that last saved it
   has published the save (`on_cpu` store-release), observed by the
   restoring CPU with a load-acquire under its own `rq_lock`. A CPU's own
   idle task is the one exception, safe by 1. No CPU waits for a release
   while holding a lock.

## Consequences

- One load and a compare per context switch. bench smp context-switch time
  is measured against the base (bar: within 10%).
- A deferred task waits for its previous CPU's window plus, if the
  deferring CPU is running another task, that CPU's next `schedule()`
  (bounds under part 2). Tasks at or below its priority on the deferring
  CPU keep running meanwhile.
- Interrupt latency on a CPU in `schedule()` grows by the steal prelude
  (WORK_STEALING) and, on COOP builds, the cooperative tick (part 1).
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
fix head; a `SWITCH_CHECK` stale-CPU count at the fix head; R1, R2 or R3
failing at the fix head; a hang with a CPU spinning in the idle poll while
the task's previous CPU is not in its window; an idle CPU asleep in
WFI/HLT with `switch_in_waiting` set.

## Changes from round 1 (critic-5-C2-design.md section 7)

1. Stale CPU: sequence 3 in "The sequence being closed", part 1 of the
   Decision, invariant 1.
2. Reproducer and base rate: "#1242 evidence" (SWITCH_CHECK stale-CPU
   probe, 5 of 5 runs, mean 2.4 iterations, own issue #1242).
3. Idle check atomic with the wait: "Idle loop" (masked read, WFI wakes on
   a masked pending interrupt, x86 `sti; hlt`), bounds under part 2.
4. Never-released case, idle versus next ready task, the withdrawn "one
   `schedule()`" sentence: "Bounds" under part 2; the decision now runs
   the next ready task.
5. Only the wait instruction is skipped; cooperative secondaries stay
   masked: "Idle loop".
6. No-deadlock argument without the lock list, `task_destroy`'s callers,
   progress against the unfair lock: "Why no deadlock".
7. Lock list completed (table rows `pi_mutex` priority floor, ipc,
   printing) and the idle exemption justified by part 1 (part 2, second
   bullet).

The forced-tick check of the no-deadlock bar is `test_switch_window_tick_forced`
(`c2/switch-window`): on `c2/reverted-wait` 20 of 20 runs hang
(`gauntlet-5-hw-pi5` 09e691ec), and two register-sampled hangs show CPU 3
in the `ldapr` loop on `cw_T->on_cpu` holding `rq_lock[3]` and CPU 0 in
the tick, in `sched_rebalance_tick`, spinning on `rq_lock[3]`
(`forced-reverted-ef11ee3c-sampled-SYMBOLS.txt`, 91f0f36c).
