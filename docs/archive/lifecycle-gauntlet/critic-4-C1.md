# Critic verdict, lifecycle gauntlet run 4, C1 (#1213, #1223, #1211, #1229), QEMU part

Critic: blind critic for C1, written by the model Fable 5.2 (model ID
`claude-fable-5-2`), 2026-11-12 UTC. Bound by `run3-critic-protocol.md` and the
format of `critic-3-C1.md`. Not opened at any point: `lifecycle-4.md`, any PR
body, any builder report. Order of reading: (1) fix diff and code, then section
1 below was written; (2) the fix commit messages (`884aab2d..6d4fe041`);
(3) the raw run files, counted by the critic's own scripts; (4) only after
those counts, C1's `qemu-rates-table.txt`, `qemu-1213-summary.txt`, C2's
summaries and the issue comments. The window experiment in section 1.6 was
run after step (2); the finding it tests was written before step (2).

Evidence read from `gauntlet-4-hw-pi5` @ 43b3af1c (still the branch head at
05:10 UTC), paths below relative to `docs/archive/lifecycle-gauntlet/run4-hw/`.
Critic files: `critic-c1-runs/` (path at the end). No lab hardware and no
labctl call. No branch edited; the critic's worktree
(`.claude/worktrees/l4-c1-critic`) is removed. One rule was broken by the
critic: a name-pattern kill that hit other agents' QEMU runs (section 8).

## Summary

| Row | Verdict |
|---|---|
| Mechanism review (step 1) | **FAIL**: the fix's claim that the `schedule()` wait on `next->on_cpu` "cannot deadlock" is wrong. Derived from the code before the commit messages were read, then reproduced at 6d4fe041 with the switch window held open (2 of 2 runs, `critic-window-6d4fe041-run01.txt`, `-run02.txt`). The rest of the mechanism (i, ii, iii, iv) agrees |
| Bar (a), three first-fault traces | **PASS** (`c1/qemu-1213-trace-multi-run04/05/06.txt`) |
| Bar (b), stress test fails on the unfixed kernel at a measured rate | **PASS**: C1 20/20, critic 10/10 at 884aab2d |
| Bar (c), 5x rule, stress test | **PASS**: needs 5; 0 faults in 26 C1 runs and 10 critic runs at 6d4fe041 |
| Bar (c), 5x rule, 1 s variant | **PASS**: needs 17; 0 faults in 26 C1 runs at 6d4fe041 (6 default, 20 WS=ON) plus 10 critic runs, and 20 default runs at d8b21ec3 (same non-test kernel code). In the baseline's own configuration at the named head C1 has 6 files |
| Isolation path, isolated=10/10 in 20 of 20 at the fix head, CPU 1 never faults | **PASS** on the WS=ON set (20/20 at 6d4fe041); default configuration at that head: 6/6 (C1) and 10/10 (critic) |
| Trace ring: build option, default off, zero cost | **PASS** by the critic's own check at 6d4fe041; C1's file covers b8df6f5f only |
| #1229 regression test fails on the old code | **PASS**: C1 6/6 (local builds), critic 3/3 |
| #1229 and #1219 relation | **PASS** (C2: class A 3/20 on main, 3/20 at 5be808c5, 0/34 at 364544c1) |
| #1213, #1223, #1211 each with an end state and files | **INCOMPLETE**: end states are in `c1/qemu-1213-summary.txt`; none of the three issues has a run-4 comment; Pi 5 parts pending |
| C2 WS=ON cell at 6d4fe041 | landed later (78ab8747): 18 valid runs, class A 0, class B 0; 2 runs INVALID, at least one killed by the critic (section 8) |
| Draft PR | not opened (by design, after this pass) |
| Pi 5 (20 boots, #1223, #1211 hardware side) | pending (later phase) |

**Overall: FAIL**, on definition-of-done row "draft PR (fix, ...)" through the
mechanism review: the fix at 6d4fe041 adds a lock-order cycle that can stop
every CPU with IRQs masked. File: `critic-c1-runs/critic-window-6d4fe041-run01.txt`.
Every measured bar that the brief sets for QEMU is met.

## 1. Mechanism review

Sections 1.1 to 1.5 were written before any C1 commit message, doc, issue
comment or evidence summary was read, and are unchanged except for the two
corrections marked "added after".

Inputs: `git diff 6c1f4c9c 6d4fe041 -- kernel/` without the tests, and the
code at 6d4fe041 (`kernel/sched/sched.c`, `kernel/sched/task.c`,
`kernel/sched/task_sleep.c`, `kernel/arch/arm64/context.S`,
`kernel/include/spinlock.h`). Arm ARM (DDI 0487) B2.3 terms: a Store-Release
(STLR) is Barrier-ordered-after every read and write of the same PE that
precedes it in program order; a Load-Acquire (LDAR, LDAXR) is
Barrier-ordered-before every read and write that follows it in program order;
a Load-Acquire that follows a Store-Release in program order on one PE is
ordered after it; a read is ordered after the write it Reads-from. Plain
stores to different locations by one PE are not ordered against each other.
`spin_lock` is `dmb ish` + LDAXR/STXR, `spin_unlock` is `dmb ish` + STLR
(`spinlock.h:131-232`), so every lock hand-over is an ordered edge.

What the fix adds: `task.on_cpu` (set to 1 by a plain store under the
switching CPU's `rq_lock` in `schedule()` `sched.c:2495` and
`scheduler_start()` `:2681`; cleared by STLR in `sched_finish_switch()`
`:450`, called right after `switch_to()` returns on the incoming stack
`:2608` or first thing in `task_entry_trampoline` `task.c:122`); `switch_to`
returns its `old` argument (x0 untouched by the restore half; x86 `mov
%rdi,%rax`); `task_destroy` waits for `on_cpu == 0` with LDAR, bounded at
2 s, when the state it LDAR-reads is TERMINATED (`task.c:855`);
`rq->zombie_gen` and `task_destroy_gen` for the reaper (`sched.c:2393-2407`);
`task_alloc`/`task_create` set `state = TASK_READY` inside the `task_lock`
section that claims the slot (`task.c:229`, `:327`); an unbounded LDAR wait
on `next->on_cpu` in `schedule()` under `rq_lock[this_cpu]` with IRQs masked
(`sched.c:2487`); and `cpu_id()` re-read after `switch_to` for the
`preempt_disabled` and `reschedule_pending` clears (`sched.c:2627-2636`).

### 1.1 (i) No reuse of a slot while a CPU is still switching out of it

Accesses that must be ordered: on the CPU leaving the task (A), the stores of
x19-x30, SP, FP/SIMD into `task->context` by `switch_to` and every access to
the task's stack up to the SP move (W_ctx); on the destroyer (B), the slot
clear, `generation++`, `pmm_free_pages(stack)`, and any later `task_create`
stores to the same slot and pages (W_reuse). Required: W_ctx before W_reuse.

The code gives: W_ctx, then A's STLR `on_cpu = 0` (program order, so W_ctx is
Barrier-ordered-before the STLR); B's LDAR reads 0 from that STLR; B's LDAR
is Barrier-ordered-before B's later writes. The chain closes provided the 0
B reads is the switch-out's and not the 0 from before the task's switch-in.
For that, B's on_cpu read must be ordered after the plain store `on_cpu = 1`.
Path: `on_cpu = 1` (under rq_lock) -> that CPU's `rq_unlock` STLR -> LDAXR of
the rq_lock taken by `scheduler_terminate_task` (the exiting CPU itself, or a
third CPU that force-terminates) -> store of TERMINATED -> B's LDAR of
`state` reads it -> B's LDAR of `on_cpu`. The LDAR on `state` is therefore
load-bearing and is present. Holds for the task_exit path and the
`scheduler_terminate_task` path. For `shell_exec.c:293` (plain
`target->state = TASK_TERMINATED` by the killer itself) the chain runs
through `scheduler_remove_task`'s rq_lock instead; it holds if that lock is
the rq_lock of the CPU that switched the task in (`scheduler_remove_task`
locks `task->assigned_cpu`'s queue).

Logical (not memory-model) checks: a TERMINATED task is never switched in
again (`pick_next_task` panics on one, `scheduler_terminate_task` dequeues
it), so on_cpu cannot go 0 -> 1 between B's wait and B's `task_lock`
section. The state re-check under `task_lock` stays. `task == task_current()`
is refused. Verdict on (i): ordered.

Limits: the wait is bounded (2 s, then WARN and leak), which is the right
side to fail on. A task whose state is not TERMINATED is not waited for and
not destroyed (unchanged behaviour).

### 1.2 (ii) The reaper must not destroy a reused slot

Stores and loads: reaper R reads `rq->zombie`, `rq->zombie_gen` under its
rq_lock, later (lock dropped) reads `task->state`, `task->generation`;
owner O does `generation++` and the slot clear under `task_lock`; creator C
writes `id`, `state = READY` under `task_lock`, then the rest of the slot
unlocked. Required: R's decision to reclaim and O's/C's writes are mutually
exclusive, and R decides on a value that identifies the occupant.

The code: `zombie_gen` is taken in `schedule()` while `current->on_cpu == 1`
(the exiting task is the caller), so no destroy of that occupant can have
passed its wait, hence no `generation++` yet: the recorded generation is the
exiting occupant's. `task_destroy_gen` compares it under `task_lock`, the
same lock under which `generation++` happens, so R either reclaims the same
occupant (and O then finds state != TERMINATED and returns) or sees a
mismatch and returns 0. The `state = READY` in the claim section closes the
other half: any destroyer (with or without a generation) that takes
`task_lock` after the claim fails the TERMINATED check. Before the claim the
slot has `id == 0` and state DESTROYED (cleared by the first destroy), also
not TERMINATED. Verdict on (ii): the reaper path is closed.

Not closed, and not in C1's definition of done as far as the brief states
it: an owner holds a bare pointer. If the reaper reclaims first, a creator
reuses the slot, and that new task runs and exits before the owner's
`task_destroy(ptr)` call, the owner destroys the new occupant (ABA on a
pointer without a generation). It needs a whole task lifetime inside the
owner's window. Recorded as a residual, not a fail.

### 1.3 (iii) schedule() must not switch into a task whose context another CPU has not finished saving

Accesses: on D (leaving T), W_ctx(T) as above; on X (entering T), the loads
of `T->context` in `switch_to`'s restore half and every use of T's stack
(R_ctx). Required: W_ctx(T) before R_ctx(T).

The code: X's LDAR loop on `T->on_cpu` reads 0 from D's STLR; same chain as
(i). Freshness of the 0: X can only hold T after taking an rq_lock that D
(or a migrator/waker) released after T's `on_cpu = 1`, so the lock hand-over
orders the earlier plain store before X's LDAR. This covers all three ways T
becomes visible while D is still in its switch window: re-queued READY and
then stolen or migrated (`sched_try_steal`, `sched_migrate_task`,
rebalancer), and blocked then woken by another CPU
(`task_wake_sleepers`/wait-queue wake -> `scheduler_add_task`) before D's
`switch_to` finished. Only one CPU can dequeue T, so the plain `on_cpu = 1`
that follows the loop is not racing another setter. Verdict on (iii):
ordered.

### 1.4 (iv) The post-switch preempt_disabled clear

The flag that must be cleared is the one of the CPU the task resumed on
(set by that CPU's `schedule()` before it switched to this task), not of the
CPU the task called `schedule()` on. The fix re-reads `cpu_id()` after
`switch_to`. Between that read and the store the task cannot move:
preemption on that CPU is off until this very store (`scheduler_tick` and
`maybe_arm_resched_trampoline` both return on `preempt_disabled[cpu]`), and
there is no voluntary switch in between. Verdict on (iv): correct.

Residual of the same class, one line later: `reschedule_pending[now_cpu] = 0`
(`sched.c:2636`, SECONDARY_PREEMPT builds) runs after the flag is cleared,
so a tick can preempt the task between the two stores; if it then resumes on
another CPU it clears the old CPU's pending flag, which costs that CPU one
reschedule (recovered at its next tick). Not a deadlock; recorded.

### 1.5 Deadlock analysis of the new waits

`task_destroy` wait (`task_wait_off_cpu`): taken before `task_lock`, holds no
lock of its own, bounded at 2 s. The CPU that must clear on_cpu needs no
lock to do so. A caller that enters with IRQs masked or a lock held can
stall up to 2 s and then leaks the slot; no caller in the tree does (list
below). The reaper call is made with rq_lock dropped. A CPU cannot wait on
itself here: it clears `prev->on_cpu` before it clears its own
`preempt_disabled`, so no nested `schedule()` (and no reaper) can run on it
in between. No deadlock.

`schedule()` wait on `next->on_cpu` (`sched.c:2487`): unbounded, under
`rq_lock[X]` with IRQs masked on X. The in-code claim is "the other CPU's
release needs no lock, so spinning under our rq_lock cannot deadlock". That
holds for the straight-line code between D's `rq_unlock_irqrestore` and
`sched_finish_switch` (stack check, TTBR0 switch, `switch_to`), which takes
no lock. It does not hold for interrupts taken by D inside that window, and
the window is open to them by design: `rq_unlock_irqrestore` restores the
caller's DAIF (IRQs on when `schedule()` was called from task context, see
the comment at `sched.c:2522`), and `switch_to` restores the incoming
task's DAIF (`context.S:188-189`) before `ret`, i.e. before
`sched_finish_switch(prev)` runs. A timer IRQ on D in that window runs
`scheduler_tick()`, which calls `task_wake_sleepers()` (`sched.c:2771`,
deliberately placed before the `preempt_disabled` early return "so sleepers
still wake even while this CPU is mid-context-switch") and
`active_policy->tick(cpu)` (`:2812`, rebalancing), both of which take other
CPUs' rq_locks with an unbounded `spin_lock`
(`task_wake_sleepers` -> `scheduler_add_task` -> `rq_lock_irqsave(target)`).

Cycle: D is in its switch window out of T (T->on_cpu == 1, T already READY
on a queue or woken). X has T at the head of its queue (stolen: the
`preempt_disabled[victim]` early-out in `sched_try_steal` is read before the
victim's lock, as the fix's own comment says; or `sched_migrate_task`, which
has no such check; or a wake to X), takes `rq_lock[X]`, picks T and spins on
`T->on_cpu`. D takes its tick inside the window; a sleeper whose target CPU
is X has expired (or the policy tick moves a task to X); D's handler spins
on `rq_lock[X]`. X waits for D's STLR, D waits for X's unlock: neither
proceeds, both with IRQs masked. Any third CPU that then needs `rq_lock[X]`
or the sleep queue also stops.

Status when this section was first written: derived from the code only. It
is a new lock-order edge that did not exist before the fix (before it,
nothing waited on another CPU's switch-out while holding an rq_lock), on a
path `kernel/CLAUDE.md` and #1093 already name as the class that silences a
board. The same edge exists for any other IRQ handler on D that takes an
rq_lock. Section 1.6 reports the experiment made afterwards.

### 1.6 Comparison with the fix commit messages, and the experiment (added after 1.1 to 1.5)

Read afterwards: 912efbd0, 5be808c5, 1ead6ef6, baf8cfbd, d8b21ec3, 364544c1,
8f4aaffd, 6d4fe041.

| Point | Commit message | Critic | Agree |
|---|---|---|---|
| (i) destroy after switch-out, state load-acquire makes the on_cpu read fresh | 912efbd0 "Ordering argument" | 1.1 | yes |
| (ii) zombie_gen read while on_cpu is 1, compare and bump under task_lock, state = READY in the claim section | 912efbd0 | 1.2 | yes |
| (iii) switch in only after the previous CPU's STLR | 1ead6ef6 item 2 | 1.3 | yes |
| (iv) re-read the CPU after switch_to | 1ead6ef6 item 1 | 1.4 | yes |
| Load instruction | LDAPR (FEAT_LRCPC) | section 1 says LDAR | critic corrected: `objdump` of the critic's 6d4fe041 build shows `ldapr w0,[x0]` in the destroy wait and `ldapr w1,[x0]` at `schedule+0x130`, `stlr wzr,[x0]` in `sched_finish_switch`. LDAPR is still Barrier-ordered-before later accesses and the chains above use the STLR-then-load-acquire rule only at the spinlock's LDAXR, so 1.1 to 1.4 stand |
| `schedule()` wait cannot deadlock | 1ead6ef6: "The previous CPU's sched_finish_switch store-release needs no lock, so the spin cannot deadlock; a cycle of two CPUs each waiting for the other's outgoing task is impossible" (same text in the comment at `sched.c:2476`) | 1.5: an IRQ taken by the outgoing CPU inside its switch window can need the waiter's rq_lock | **no** |

The two-CPU argument in 1ead6ef6 covers two CPUs that are both in the wait
loop. It does not cover the outgoing CPU being interrupted between
`rq_unlock_irqrestore` and `sched_finish_switch`.

Experiment (critic-only build, not a branch): 6d4fe041 plus
`critic-c1-runs/critic-window-experiment.patch`. The patch (a) holds the
switch window of one chosen outgoing task open for 25 ms with the caller's
IRQ state, right after `rq_unlock_irqrestore` in `schedule()`, and (b) adds
a test in which a helper on CPU 1 migrates that task (READY on CPU 0's
queue, CPU 0 inside the window) to CPU 3 with `sched_migrate_task`, while six
tasks pinned to one CPU sleep 1 to 6 ms in a loop. Phase A (control): sleepers
pinned to CPU 2. Phase B: sleepers pinned to CPU 3, the CPU that receives the
task. The window is on CPU 0 because the QEMU test kernel is built
COOP_PREEMPT=ON, SECONDARY_PREEMPT=OFF (`build/kernel-test/CMakeCache.txt`),
so only CPU 0 takes hardware timer IRQs there; a first version with the
window on CPU 2 completed both phases for that reason and is not kept as
evidence. Pi 5 and Jetson default to SECONDARY_PREEMPT=ON (project
`CLAUDE.md`), where every CPU takes them.

| File | Phase A (control) | Phase B | CPU state after 8 s without output (QMP `info registers -a`, 3 samples 0.5 s apart, identical) |
|---|---|---|---|
| `critic-window-6d4fe041-run01.txt` | COMPLETED 30/30 iterations, 30 migrations | stopped at iteration 1 | CPU 3 `schedule+0x12c` (the `yield; ldapr; cbnz` loop on `next->on_cpu`), IRQs masked. CPU 0 `scheduler_add_task_to_cpu+0x68` (the `wfe; ldaxr` loop of `spin_lock`), x19 = 3, lock address 0x407eb40c = `rq_lock[3]`, x30 = `task_wake_sleepers+0xd0`, IRQs masked. CPU 1 helper waiting for the window to close, CPU 2 busy task |
| `critic-window-6d4fe041-run02.txt` | COMPLETED 30/30, 30 migrations | stopped at iteration 1 | CPU 3 `schedule+0x12c`, IRQs masked. CPUs 0, 1 and 2 all at `scheduler_add_task_to_cpu+0x68` on `rq_lock[3]` from `task_wake_sleepers`, IRQs masked |

Reading: CPU 3 holds `rq_lock[3]` and waits for CPU 0's STLR; CPU 0 is in
its timer IRQ inside the switch window, waiting for `rq_lock[3]` to wake a
sleeper pinned to CPU 3; in run02 the other two CPUs then stop on the same
lock from their own ticks. Nothing times out: the suite produced no further
output until QEMU was killed.

Limits of the experiment, stated plainly: the 25 ms hold is artificial. In
the unmodified kernel the window is the code between
`rq_unlock_irqrestore` and `sched_finish_switch` (stack check, TTBR0 switch,
`switch_to`), so the cycle needs a hardware tick on the outgoing CPU inside
that span, a move or wake of the outgoing task to another CPU that then
picks it, and a sleeper wake (or policy tick) from that tick that targets
the waiting CPU. No unmodified run in section 3 or in C1's and C2's files
shows a hang attributable to it (0 of 26 C1 runs and 10 critic runs at
6d4fe041; C2 0/34 class A at 364544c1). The natural rate is not measured. A
host-descheduled vCPU or a long handler widens the window in the same way.
What the experiment establishes is that the cycle exists and is unbounded,
not how often it occurs.

Directions that would remove the edge (not tested by the critic): keep IRQs
masked from `rq_lock_irqsave` in `schedule()` until `sched_finish_switch`
has run on the incoming side; or bound the wait and drop `rq_lock` while
waiting; or have `task_wake_sleepers` and the policy tick skip or try-lock
when `preempt_disabled[cpu]` is set.

### 1.7 task_destroy callers at 6d4fe041 (non-test)

| Caller | Context | Note |
|---|---|---|
| `sched.c:2407` reaper | `schedule()`, rq_lock dropped, caller's IRQ state | `task_destroy_gen` |
| `sched/ai/workloads.c:446` | benchmark task, after `scheduler_terminate_task` on unfinished tasks | a task still running elsewhere is waited for (2 s) |
| `src/coherency.c:633` | only when the task is READY, after `scheduler_terminate_task` | |
| `src/component_runtime.c:691` | error path right after `task_create`, before the task is queued (state READY: destroy refuses, as before) | |
| `src/lua_slm.c:2132` | after `scheduler_remove_task` (takes the rq_lock of `task->assigned_cpu`) | |
| `src/shell_exec.c:297` | sets `state = TASK_TERMINATED` itself, then `scheduler_remove_task` | kill of a task running on another CPU now waits for its switch-out instead of freeing a live stack |
| `kernel/tests/*` | 95 call lines (77 in `test_scheduler.c`) | task context |

`switch_to` has two callers (`sched.c:2607`, `:2751`); both set `on_cpu = 1`
on the incoming task first, and both incoming paths call
`sched_finish_switch`.

## 2. Bar (a): the three first-fault traces (PASS)

Files: `c1/qemu-1213-trace-multi-run04.txt`, `-run05.txt`, `-run06.txt`
(banner b8df6f5f, SLOT_TRACE=ON, elf 34f475d1, thread=multi; run01 to 03 of
the same set passed without a dump). Checked in each:

- Taken at the first fault: the first `SLOTTRACE-FAULT` line is `bad SP at
  EL1 sync entry: cpu=1 sp=0x0 esr=0x96000045 far=0xffffffffffffffd0
  elr=0x40018a00`, and the dump (`why=el1_sync_bad_sp`) follows at once. No
  `[FAIL]`, `PAGE FAULT` or panic line precedes it in any of the three.
- All CPUs present: four `SLOTTRACE-CPU n` rings, 512 entries shown each.
  CPU 0's ring is mostly `ADD` records of the yielding test task, but its
  first entry (#19237366 in run04) precedes the first record of the
  faulting generation (#19237393), so the sequence is not cut.
- Sequence, written by the critic from run04 (global sequence numbers; run05
  and run06 show the same order on the same slot 6 with generations 169 to
  171 and 153 to 155):

| # | CPU | Record | Meaning |
|---|---|---|---|
| 19237393..402 | 1 | SWITCH, FIRST_RUN, EXIT, TERMINATE slot 6 gen 159 id 188 | the latency task runs and exits on CPU 1; TERMINATED published under CPU 1's rq_lock |
| 19237403..405 | 0 | TEST 0x202, 0x302, DESTROY slot 6 gen 159, caller in the test, `rq=20` | the owner on CPU 0 sees TERMINATED and destroys the slot while CPU 1 still holds its rq_lock inside `schedule()` for that task |
| 19237406 | 1 | ZOMBIE_SET slot 6 (already reads gen 160, id 0) | CPU 1, still switching out, parks the slot as its zombie |
| 19237407 | 0 | DESTROY_DONE gen 160 | first destroy complete |
| 19237408..410 | 1 | SWITCH to slot 1 (idle), SWITCH_RET, ZOMBIE_REAP slot 6 | CPU 1 leaves the task; idle's `schedule()` starts the second destroy |
| 19237411 | 0 | CREATE_ALLOC slot 6 id 189, st=3 | CPU 0 claims the slot for the next task; state still the old TERMINATED |
| 19237412..413 | 1 | DESTROY slot 6 id 189 (stack base 0x0), DESTROY_DONE gen 161 id 0 | the reaper on CPU 1 destroys the new occupant mid-construction |
| 19237414..416 | 0 | CREATE_CTX_DONE ctx.sp=0x0, ADD to CPU 1, TEST 0x102 | `task_create` reads back a cleared `stack_top` into `context.sp` and queues the task |
| 19237425 | 1 | SWITCH slot 6 gen 161 id 0, incoming ctx.sp=0x0 | CPU 1 switches in with SP 0 and faults at the first stack store |

  Destroyer: CPU 0 (owner) first, CPU 1 (zombie reaper) second. Creator:
  CPU 0. CPU still switching out of the slot when it was first destroyed:
  CPU 1. The stores themselves (the reaper's clear of `stack_top`, the
  creator's copy into `context.sp`) are not individual trace records; they
  are read from the record order (DESTROY between CREATE_ALLOC and
  CREATE_CTX_DONE) and the read-back value 0. That is enough for the bar.
- Second sequence (one trace, `c1/qemu-1213-stresstrace-multi-run02.txt`,
  884aab2d SLOT_TRACE=ON, dump at the `task_exit: schedule returned!`
  panic): CPU 1 ZOMBIE_SET slot 6 gen 163 (#17567199), CPU 0 DESTROY and
  CREATE of id 189 with ctx.sp 0x42c40000 (#201 to #206), CPU 1 SWITCH out
  (#207), then CPU 1 SWITCH into slot 6 gen 164 with ctx.sp 0x42c3ff50 and
  x19 0x407eb180, which is a saved `schedule()` frame and not a
  `task_create` image. This is the `switch_to` save landing on a rebuilt
  slot. It rests on one dump; `c1/qemu-1213-stresstrace-multi-run01.txt`
  is the first sequence again.

C1's written mechanism (912efbd0, `c1/qemu-1213-summary.txt` steps 2 and 3)
matches both readings.

## 3. Reproduction by the critic

Builds: own worktree, one build at a time, `make kernel-test` (QEMU_VIRT,
defaults: WORK_STEALING=OFF, SLOT_TRACE=OFF). Runs: `critic-run1.sh.txt`, the
`make -n test` QEMU command plus `-accel tcg,tb-size=128,thread=multi`, under
`systemd-run --user --scope -p MemoryMax=3G -p CPUQuota=200% timeout 120`,
fresh 4G sparse SD image per run, two at a time. `make test` itself passes no
`-accel` flag; QEMU 8.2.2 then uses MTTCG for this guest (C2's
`c2/accel-threads.txt` shows one host thread per vCPU), so thread=multi is the
Makefile's mode. Other agents' QEMU processes ran on the host throughout
(5 to 7 at a time); load is on line 1 of each file.

| Set | Head, banner | elf sha256 | Runs | Result | 1-min load at start |
|---|---|---|---|---|---|
| `critic-unfixed-884aab2d-run01..10.txt` | 884aab2d, 10/10 | fab13aa9 | 10 | 10/10 fail: `[FAIL] test_task_slot_reuse_under_switch`, `slot_reuse_stress: iteration N: ... not TERMINATED after 1 s` at N = 6 to 268, then isolated=0/10, 20 iso TIMEOUT lines and the 120 s cap; `task_exit: schedule returned!` panic in 2 (run04, run07) | 5.72 to 9.92 |
| `critic-fix-6d4fe041-run01..10.txt` | 6d4fe041, 10/10 | 67c2cf90 | 10 | stress 0/10 faults (`iterations=1956..2028 ... bad=0 timeouts=0`, `[PASS]`); isolated=10/10, normal=10/10 in 10/10; 0 iso TIMEOUT, 0 panic, 0 page fault; `pdm: ... moved=6..8 ... bad=0` in 10/10; suite rc 0 in 1, rc 1 in 9, each with exactly one failure, `test_litmus_patterns_pass` (#1230) | 2.77 to 7.95 |
| `critic-pdrevert-6d4fe041-run01..03.txt` | 6d4fe041-dirty (`critic-pdrevert.patch`: `now_cpu = this_cpu`) | 926431ab | 3 | `[FAIL] test_preempt_flag_follows_migrated_task` 3/3, `moved=6,6,7 bad=6,6,7 (first on cpu 1)` | 5.79 to 6.08 |
| `critic-window-6d4fe041-run01..02.txt` | 6d4fe041-dirty (`critic-window-experiment.patch`) | fda5d8ca | 2 | section 1.6 | 2.45, 3.41 |

Agreement with C1: unfixed stress 10/10 against C1's 20/20; fix head 0/10
against C1's 0/26; isolation 10/10 in 10/10; the #1229 test fails on the old
code. The nine litmus failures are #1230 (open, filed for the QEMU litmus
under host scheduling); the rate here (9/10) is higher than in any C1 set
(1/6 to 8/20) and coincides with the highest host load of any set.

Note on line 1 of every run file, C1's and the critic's: the
`slot_reuse_stress:` field is cut after `ite` (the runner's `[^\r]` bracket
excludes the letter r). The full line is in the body of each file and the
counts here are taken from the body.

## 4. Counts from C1's and C2's files (critic's scripts, raw files)

Every file of a set carries one banner hash and one elf sha256; banner
against claimed head checked for all 348 run files counted here (C1 254, C2 94 in the four cells of 4.2). Fix-head
sets: every file ends with `All test suites passed` or a `total failure(s)`
line (0 truncated in `qemu-1213-fix`, `-step5`, `qemu-1229-fix3wsoff`,
`-fix3wson`, `-fix5wsoff`, `-fix5wson`, `qemu-1211-fix`). No `still on a
CPU` or `is the caller` warning in any fix-head file.

### 4.1 C1, default configuration unless marked

| Set (c1/) | Banner | n | CPU 1 stopped (iso TIMEOUT, stress timeout, dump or panic) | isolated=10/10 | Stress line | Other `[FAIL]` |
|---|---|---|---|---|---|---|
| `qemu-1213-main-multi` | 01931af6 | 20 | 6 (run01, 09, 11, 12, 14, 19; all rc 124) | 19 (run01 6/10) | none (test not present) | litmus 3 |
| `qemu-1213-main-single` | 01931af6 | 20 | 0 | 20 | none | all 20 end at the 120 s cap after the latency test; stress_mixed_priorities 1 |
| `qemu-1213-trace-multi` (SLOT_TRACE=ON) | b8df6f5f | 6 | 3 dumps (run04, 05, 06) | 3 | none | 0 |
| `qemu-1213-stress-unfixed-multi` | 884aab2d | 20 | 20 (stress test fails at iteration 2 to 189; panic in 2) | 0 | fail 20/20 | 0 |
| `qemu-1213-stresstrace-multi` (SLOT_TRACE=ON) | 884aab2d | 2 | 2 dumps | n/a | n/a | 0 |
| `qemu-1213-fix-multi` | 912efbd0 | 20 | 0 | 20 | clean 20/20, 1972 to 2068 iterations | litmus 8, net_burst_8_sends_async 1 |
| `qemu-1213-step5-multi` | 6aee62c1 | 20 | 0 | 20 | clean 20/20 | litmus 1 |
| `qemu-1229-fix3wsoff-multi` | d8b21ec3 | 20 | 0 | 20 | clean 20/20 | litmus 3 |
| `qemu-1229-fix3wson-multi` (WS=ON) | d8b21ec3 | 20 | 0 | 20 | clean 20/20 | #1229 test 4 (moved=0, bad=0), litmus 2 |
| `qemu-1229-fix5wsoff-multi` | 6d4fe041 | 6 | 0 | 6 | clean 6/6 | litmus 1 |
| `qemu-1229-fix5wson-multi` (WS=ON) | 6d4fe041 | 20 | 0 | 20 | clean 20/20 | litmus 4, slm_stats_live_from_second_session 1 |
| `qemu-1211-unfixed-multi` (SLOT_TRACE=ON, count waits) | 8b0065ce | 20 | 0 | 0 (0/10 or 1/10: the count expires first) | none | `[C1-1211] lifecycle PASS` 20/20; runner_parks_without_a_lock 1 |
| `qemu-1211-fix-multi` | 2f043ffa | 20 | 0 | 20 | clean 20/20 | lifecycle PASS 20/20; litmus 3 |
| `qemu-1229-wstrace-multi` (main, WS=ON, SLOT_TRACE=ON) | 7e07b384 | 18 | 3 dumps (run01, 17 panic; run16 stuck flag) | 0 (count waits) | none | litmus 5, proactive_load_balance 2, benchmark_context_switch 1 |
| `qemu-1229-wstrace-v1-multi` | 6f405a76 | 8 | 1 dump | 0 | none | 3 single failures |
| `qemu-1229-pdunfixed-multi`, `-pdunfixedws-multi` | 6d4fe041-dirty | 3 + 3 | 0 | 6 | clean | #1229 test fails 6/6 with bad > 0; one WS=ON run at the 120 s cap |
| `qemu-1229-pdunfixed-8f4aaffd-multi`, `-pdunfixedws-8f4aaffd-multi` | 8f4aaffd-dirty | 3 + 3 | 0 | 6 | clean | #1229 test fails 6/6 (bad > 0 in 3, moved=0 in 3) |
| `qemu-1229-pdm-fix-multi`, `-pdm-unfixed-multi` | baf8cfbd-dirty, header "head wip" | 1 + 1 | 0 | 2 | clean | not counted for any claim |

These agree with `c1/qemu-rates-table.txt` and `c1/qemu-1213-summary.txt`
row for row.

### 4.2 C2, `make test` command, thread=multi

| Cell (c2/) | Banner | n | A (#1219 batch stress, cap) | B (#1213-like panic, cap) | isolated=10/10 | Other |
|---|---|---|---|---|---|---|
| `ws-off-main-6c1f4c9c` (WS=OFF, main itself) | 6c1f4c9c | 20 | 0 | 0 | 0 (0/10 in 19, 1/10 in 1) | litmus 9 |
| `ws-main-6c1f4c9c` (WS=ON, main) | 6c1f4c9c | 20 | 3 | 2 | 0 | litmus 3 |
| `ws-on-5be808c5` (WS=ON, #1213 fix only) | 5be808c5 | 20 | 3 | 0 | 20 | litmus 4, benchmark_context_switch 3, proactive_load_balance 1 |
| `ws-on-364544c1` (WS=ON, #1213 and #1229 fixes) | 364544c1 | 34 | 0 | 0 | 34 | #1229 test 13 (moved=0, bad=0), policy_custom_assign_cpu_called 1 |

### 4.3 Each threshold against this run's own baseline

| Claim | Baseline (this run) | Needed | Have at the fix | Verdict |
|---|---|---|---|---|
| Baseline rates on main | main itself, default: 0/20 faults, but isolated=0/10: the isolation path does not run on main (`c2/ws-off-main-6c1f4c9c`). main + 1 s waits: multi 6/20, single 0/20 (`c1/qemu-1213-main-*`). main WS=ON: A 3/20, B 2/20 (`c2/ws-main-6c1f4c9c`) | | | recorded, both accel modes |
| Stress test, bar (b) | 20/20 at 884aab2d (C1), 10/10 (critic) | measured in 20 runs before the fix | | PASS |
| Stress test, bar (c) | 1 run per fault | 5 | 6d4fe041: 6 + 20 (WS=ON) + 10 critic, 0 faults; earlier fix heads: 20 + 20 + 20 + 20 | PASS |
| 1 s variant, bar (c) | 6/20: 3.33 runs per fault | 17 | 6d4fe041: default 6 (C1) + 10 (critic) = 16, WS=ON 20; d8b21ec3 default 20, WS=ON 20; `git diff d8b21ec3 6d4fe041` touches `docs/scheduler.md`, `kernel/CLAUDE.md` and the #1229 test only. 912efbd0 20 and 6aee62c1 20 predate 1ead6ef6 (the `schedule()` wait and the CPU re-read), so they are a different kernel | PASS (0 faults in 36 default-configuration runs on the head's kernel code) |
| WS=ON class B | 2/20 on main: 10 runs per fault | 50 | 364544c1 34 (C2) + d8b21ec3 20 + 6d4fe041 20 (C1) = 74 runs on the same scheduler code, 0 class B. C2's own cell alone is 34 (C2 says so) | PASS on the pooled count; not on one cell |
| WS=ON class A (#1219) | 3/20 on main: 6.67 runs per fault | 34 | 364544c1 34, plus 40 C1 runs, 0 class A | PASS |
| Isolation path | not exercised on main (count waits) | isolated=10/10 in 20 of 20 at the fix head | 6d4fe041: WS=ON 20/20, default 6/6 + critic 10/10 | PASS; the default configuration at that head has 6 C1 files |

Paths confirmed to have run in every run cited as passing: stress test
1956 to 2163 iterations with `same_slot = iterations - 1` and `ran =
iterations`; `Samples collected: isolated=10/10, normal=10/10`; `pdm`
`moved` 6 to 8 at 6d4fe041 (26/26 C1, 10/10 critic). At d8b21ec3 and
364544c1 the #1229 test did not move its task in 4/20 and 13/34 WS=ON runs
(moved=0, bad=0, reported as a failure by the test itself); 8f4aaffd and
6d4fe041 changed the test, and no moved=0 run exists at 6d4fe041.

## 5. Trace ring: build option, default off, zero cost (PASS by the critic's check)

- Option: `option(SLOT_TRACE ... OFF)` in `CMakeLists.txt`; the Makefile
  passes `-DSLOT_TRACE=ON` only for `SLOT_TRACE=ON`; `SLOT_TRACE:BOOL=OFF` in
  the critic's default `build/kernel/CMakeCache.txt`.
- C1's `c1/slot-trace-size-check.txt` compares 01931af6 with b8df6f5f. The
  trace code changed after that (baf8cfbd, d8b21ec3: more hooks,
  `slot_trace_pd_tick`), so that file does not cover the fix head.
- Critic's check at 6d4fe041, `make kernel` (QEMU_VIRT): build 1 unmodified
  with the option off; build 2 with every `SLOT_TRACE*` and
  `slot_trace_pd_tick` invocation and every `#include "slot_trace.h"`
  blanked in place (line numbers kept; `critic-strip_trace.py.txt`: 31 hooks
  in `sched.c`, `task.c`, `test_scheduler.c`) and `slot_trace.c`,
  `slot_trace.h` removed from the tree and from `CMakeLists.txt`.
  Result: linked `.text` 2442648 B in both; the six objects that reference
  the trace (`sched.c`, `task.c`, `test_scheduler.c`, `exceptions.c`,
  `panic.c`, `vectors.S`) have byte-identical `.text` and identical
  `objdump -dr` output (0 diff lines each); `slot_trace.c.obj` has `.text`,
  `.data` and `.bss` of 0 bytes; no `slot_trace` symbol in the option-off
  elf. The linked `.text` bytes of the two elfs differ only through
  `.rodata` addresses, because build 2's banner reads `6d4fe041-dirty`.
  elfs: 81cf1798 (option off), 2f3be569 (stripped).

## 6. Issues

Read with comments after the counts. State at 05:45 UTC 2026-11-12:

| Issue | State | Run-4 end state on the issue | In C1's files |
|---|---|---|---|
| #1213 | OPEN | none (last comment is run 3's) | mechanism, rates, fix: `c1/qemu-1213-summary.txt` |
| #1223 | OPEN, 0 comments | none | "QEMU side same defect" by signature (sequence B); the summary states that the Pi 5 occurrence is untraced |
| #1211 | OPEN | none (last comment is run 3's) | not reproduced in QEMU: lifecycle PASS 20/20 unfixed, 20/20 fixed |
| #1229 | OPEN, body only | n/a | traced (`c1/qemu-1229-wstrace-multi-run16.txt`: CPU 1 steals id 651 from CPU 3, PD_SET a=1, SWITCH, PD_CLEAR a=3 b=1, stuck-flag dump 300 ticks later; read by the critic), fixed, regression test |
| #1219 | OPEN | C2's comment names #1229 as the cause | A 3/20, 3/20, 0/34 as in 4.2 |
| #1228 | OPEN | n/a | not reproduced; the link to #1213 is labelled a hypothesis in C1's summary |

Row "each with an end state and files": INCOMPLETE until the three issues
carry the end state, and the Pi 5 phase is done.

## 7. Failure modes, by name

| Mode | Finding |
|---|---|
| Selective summary | None found: C1's table carries every run of every set, including the failing #1229-test runs and the 6-run set |
| Truncated capture | None in the fix-head sets. Runs that end at the 120 s cap are counted as such. Line 1 of every run file truncates the stress field at `ite` (cosmetic, body intact) |
| Assembled register file | None among C1's files. The critic's `critic-window-*.txt` are assembled (selected serial lines, QMP samples, symbolization) and say so; the raw QMP output is alongside |
| Redefined criterion | "CPU 1 stopped" is defined at the top of C1's table and applied uniformly. The 5x count for the 1 s variant in C1's summary (0/40) is taken at 912efbd0 and 6aee62c1, before 1ead6ef6 changed `schedule()`; section 4.3 recounts on the head's kernel code |
| Hypothesis stated as finding | 1ead6ef6 and the comment at `sched.c:2476` state "cannot deadlock" as fact; it is false (section 1.6). C1's summary labels the `schedule()` wait as "found by code reading, not observed in a trace", the #1223 and #1228 links as untraced or hypothesis: those are labelled correctly |
| Stale base | origin/main is 6c1f4c9c, the merge base of 6d4fe041: not stale. `c1/slot-trace-size-check.txt` (b8df6f5f) and `c1/maketest-arm64-fix-5be808c5.txt`, `c1/maketest-x86_64-fix-5be808c5.txt` (5be808c5) predate 1ead6ef6, which changes code on both architectures: no `make test` or x86-64 file exists for 6d4fe041 |
| Partial per-CPU table | The three dumps carry all four rings; section 2 uses records from CPUs 0 and 1, and CPUs 2 and 3 have no slot-6 record in the window |
| Number without a file | None found; every number in C1's summary resolved to a file |
| Uncommitted tree | `qemu-1229-pdunfixed*` (12 files) and `qemu-1229-pdm-*` (2 files) come from local builds with `-dirty` banners; the revert is described in words, with no patch file. Disclosed by C1. The critic's own revert (`critic-pdrevert.patch`) gives the same result 3/3. All other heads are on pushed branches (checked with `git branch -r --contains`) |
| Banner hash mismatch | None in 348 files |
| Bar computed from one cell | WS=ON class B meets 5x only when C2's 34 runs and C1's 40 are pooled (4.3); C2's summary says its cell alone does not |
| Precondition from a prior run | The zero-cost claim rested on the b8df6f5f check; redone at 6d4fe041 (section 5) |
| Path never exercised | Checked per run (end of 4.3). On main the isolation test collects 0/10 or 1/10 samples, so main's 0/20 says nothing about the fault. The new `schedule()` wait: no file shows that any run actually waited in it (no counter, and C1's summary says the window was not observed in a trace), so the fix-head runs do not exercise the path in which section 1.6's cycle lives |
| Critic read the builder's claim first | No; order stated at the top |

Also noted: while the critic's runs were in progress the host ran QEMU
processes on an elf named `elf-fix6-wson-9185d4cb.elf`. No commit 9185d4cb
was reviewed; this verdict is for 6d4fe041.

## 8. Rule break: name-pattern kill

Yes: commands of the critic sent SIGTERM to QEMU processes it did not start.
The first version of the window-experiment runner ended each run with
`pkill -f "qmp.sock,server=on"`, which matches every QEMU on the host that
was started with a QMP socket (and the `timeout` wrapping it), not only the
critic's.

| Time (UTC, 2026-11-12) | Critic run | pkill pid | Known effect |
|---|---|---|---|
| about 05:39 | first version of the experiment (window on CPU 2; not kept as evidence) | 2746705 (in the critic's own output) | unknown outside the critic's QEMU |
| about 05:42 | `critic-window-6d4fe041-run01.txt` | 2755413 | killed C2's `c2/ws-on-6d4fe041-run19.txt` (that file ends `terminating on signal 15 from pid 2755413 ()`); C2 marks it INVALID |
| about 05:43 | `critic-window-6d4fe041-run02.txt` | not recorded (the critic's QEMU shows `/usr/bin/timeout` as its killer, the pkill having hit the timeout wrapper) | unknown |

`c2/ws-on-6d4fe041-run17.txt` ends `terminating on signal 15 from pid
2748681 ()`. That pid matches none the critic recorded; it lies between the
first and second pkill pids, and the critic cannot attribute it. The third
call is the only one whose pid is unknown.

Fix: `critic-run_window.sh.txt` now kills only the QEMU it started (found by
its unique temporary QMP socket path, then `kill <pid>`). No pkill or killall
by name is used anywhere. The later stop of the critic's own 9185d4cb runs
(below) was done by PID, selected by the critic's own elf path.

Check of the files counted in this verdict for `terminating on signal`:

- C1 (254 run files at 43b3af1c): 48 files carry the line, every one from
  `/usr/bin/timeout` (the 120 s cap): `qemu-1213-main-multi` 6,
  `qemu-1213-main-single` 20, `qemu-1213-stress-unfixed-multi` 20,
  `qemu-1229-pdunfixedws-multi` 1, `qemu-1229-wstrace-v1-multi` 1. None from
  another process. No fix-head file carries it. No count in section 4 changes.
- C2 cells counted in 4.2: `ws-main-6c1f4c9c` 5 and `ws-on-5be808c5` 3, all
  from `/usr/bin/timeout`. No count changes.
- C2's `ws-on-6d4fe041` (landed after section 4 was written, evidence branch
  78ab8747): run17 and run19 are INVALID as above; the other 18 show class A
  0, class B 0, isolated=10/10 in 18, stress clean in 18, `pdm` moved 5 to 8
  and bad=0 in 18 (critic's count). This closes the summary table's row "C2
  WS=ON cell at 6d4fe041" as 18 valid runs, 2 invalid.
- Critic: the 10 unfixed runs end with the timeout's SIGTERM, which is the
  expected 120 s cap after the fault. The 10 fix-head runs at 6d4fe041 and
  the 3 revert runs have no such line and end with a suite summary. The two
  window runs were ended by the critic after sampling and are evidence of a
  hang, not passes.

## 9. Head change to 9185d4cb (orchestrator update, then stop)

The orchestrator moved the fix head to 9185d4cb and then told the critic to
stop before any run at that head. State at the stop:

- `git diff --stat 364544c1 9185d4cb` and `git diff --stat 6d4fe041
  9185d4cb`: `kernel/tests/test_scheduler.c` only. The kernel code at
  9185d4cb equals 6d4fe041's, so section 1 applies to it unchanged.
- All critic runs in section 3 are at 6d4fe041 and stay labelled so. Four
  runs at 9185d4cb were started before the stop. One finished on its own:
  `critic-fix-9185d4cb-run02.txt` (banner 9185d4cb, elf 6eaaf069, load 3.70,
  suite PASS, stress 2002 iterations bad=0 timeouts=0, isolated=10/10,
  `pdm` moved=6 bad=0, no SIGTERM line). It is kept, labelled with its head,
  and bears on no bar by itself. The other three (run01, run03, run04) were
  killed mid-suite by the critic, by PID, selected by the critic's own elf
  path; they are INVALID and not kept in `critic-c1-runs/`. Their line 1
  reads `RESULT PASS; qemu rc 0` because QEMU exits 0 on SIGTERM: the
  critic's runner has the defect the orchestrator described, and a run
  without a final suite line must not be read from line 1.
- Test changes in 9185d4cb, read but not run (redefined criterion check):
  `test_policy_custom_assign_cpu_called` and
  `test_proactive_load_balance_inert_when_light` accept `assigned_cpu != 0`
  only if the sum of `sched_diag_steal_successes` over all CPUs moved between
  just before `scheduler_add_task` and the assert. Without work stealing the
  counter never moves, so the default `make test` asserts stay exact. With
  work stealing the tests stay meaningful with one weakening: the counter
  is global, so a steal of any other task in that window (a few
  microseconds) would also excuse a wrong placement. The counter is
  incremented in `sched_try_steal` (`sched.c:1983`) before the thief
  re-queues the task and sets `assigned_cpu`, so the new check cannot fail
  falsely on ordering. `test_benchmark_context_switch` re-checks its bound
  under the lock; no criterion changes. Whether
  `test_preempt_flag_follows_migrated_task` still fails on the old code at
  9185d4cb was not run; its code is unchanged from 6d4fe041, where it failed
  3/3 for the critic.
- C1's files at 9185d4cb (evidence branch 78ab8747), critic's count:
  `c1/qemu-1229-fix6wsoff-multi` 6 runs and `c1/qemu-1229-fix6wson-multi` 20
  runs, banner 9185d4cb, 0 CPU 1 stops, isolated=10/10 in 26, stress clean in
  26, `pdm` moved 6 to 8 and bad=0 in 26; `test_litmus_patterns_pass` (#1230)
  fails in 2/6 and 20/20, `test_net_burst_8_sends_async` in 3/20.

Not done, for the re-check at the next head: mechanism review of the new
fix, the window patch against it, spot runs.

## 10. Pending (later phase)

Pi 5: isolated=10/10 in 20 of 20 boots at the fix head, #1223 and #1211 on
hardware. Not started, not judged here.

## 11. Verdict

**FAIL.** Definition-of-done row: the fix to be carried by the draft PR.
Reason: the mechanism review disagrees with the fix's stated deadlock
freedom, and the disagreement is reproduced (`critic-window-6d4fe041-run01.txt`,
`-run02.txt`): with a timer tick inside the outgoing CPU's switch window,
`schedule()`'s unbounded wait on `next->on_cpu` under `rq_lock` and
`task_wake_sleepers` -> `scheduler_add_task` on the outgoing CPU form a
cycle that stops the machine with IRQs masked. The window was held open
artificially; the natural rate is unmeasured.

Everything else asked of the QEMU part is met or pending as listed in the
summary table: bars (a), (b), (c), the isolation row, the zero-cost row and
the #1229 rows pass on the critic's own counts and reproduction.

Critic files:
`/tmp/claude-1000/-home-john-projects-CS-496-Capstone-SLM-Operating-System--claude-worktrees-bridge-cse-014Q9aryGYwccZqUzMHyre5U/eff61e97-fcbb-4f99-885f-81fd717f4885/scratchpad/critic-c1-runs/`
(25 run files with a `#` summary on line 1, two raw QMP files, two patches,
the runner and helper scripts). This verdict is for 6d4fe041; the head has
since moved (section 9) and a re-check at the next head is owed.
