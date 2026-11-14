# Critic verdict, switch gauntlet run 5, component C2, the fix (#1234, #1238, #1242)

Critic: `critic-c2`, blind. Model that wrote this verdict: **Fable 5.2**
(model ID `claude-fable-5-2`). Brief received 08:14 UTC 2026-11-14; verdict
written 09:32 UTC.

**Verdict: FAIL, on the "No regression" bar and one "Fixed" cell, not on the
fix.** The code implements the design the first verdict passed, with all four
text corrections; the critic's own lock graph has no wait under a lock and no
cycle; every idle branch that unmasks before its wait does mask, read,
wait-if-NULL, unmask (checked in source for all six branches and in the
disassembly of the Jetson build); the "No deadlock" bar is met; R1, R2 and
R3 are at 0 under the 5x rule on every cell that was run, and the critic
reproduced that in QEMU and on jetson-nano-2 kexec on both heads. What fails
is the paperwork of the bars as written:

1. Regression rows owed at the fix head and absent from both evidence
   branches: `boot_test --count 10` on Jetson kexec (3 boots exist) and on
   Jetson UEFI-direct (0 boots); `bench stealing --sustain 2500
   --assert-shares` 20 of 20 on Jetson kexec (no file); the full integration
   suite, 5 boots on each Jetson path and on Pi 5 (no file on any board).
   The last one is the run-4b failure mode by name: no board has run the
   full test kernel at the PR head.
2. The Fixed cell "R3 wake, Jetson UEFI-direct" failed at baseline
   (`uefi-r3w-boot1.txt`, hang at iteration 1) and was not run at the fix
   head.
3. The context-switch row is `bench context`, not `bench smp`, exists only
   as a transcription, and on Jetson reads +10% to +13% on the mean (87.5 ns
   to 97 ns), which is outside "within 10%" as written.
4. `/ci` is not green on record at c0ea0714: plain `make test` failed in 3
   of 3 recorded runs and `make test AI_SCHED=ON` in 1 of 2, every time in
   `test_litmus_patterns_pass` (#1230). The critic's both-heads runs show
   that test failing at least as often on the base under the same host load
   (section 6), so the critic does not attribute it to the fix; a green run
   is still owed.
5. One fix-head `make test WORK_STEALING=ON` run timed out inside
   `test_regs_every_cpu_reports` with no dump. Not reproduced by the critic
   (0 in 1030 filtered iterations and 0 in 6 full runs, section 7) or by the
   builder; no path found in the code; it stays open with a number.

None of the five needs a code change to the fix as far as the critic can
tell. Items 1 and 2 are lab time; 3 needs raw captures and either a second
measurement or a sentence in the PR accepting the cost; 4 needs a quiet
host; 5 needs an issue.

Heads. Base `8ec0e330`; C1 harness `2196deb5`; base with the C2 tests and no
fix `3b369aef` (parent of the fix commit; used as "base" in the critic's
runs); fix `origin/c2/switch-window-r2`
`c0ea07148e8bbf48590c204c6e51eb37af0d5e05` (`4bfc15a5` fix, `68f0cb7b` one
macro, `c0ea0714` docs: `git diff --stat 68f0cb7b c0ea0714` touches only
`docs/scheduler.md` and `kernel/CLAUDE.md`; `git diff --stat 4bfc15a5
68f0cb7b` touches only `switch_check.h`). Reverted design
`origin/c2/reverted-wait-r2` `ef11ee3c`. Builder evidence:
`gauntlet-5-hw-pi5` e6745ebe and later, `gauntlet-5-hw-jetson` 1b017c7a,
`docs/archive/switch-gauntlet/run5-hw/c2/` and `run5-hw/c0/`. No PR exists
for the branch (`gh pr list --head c2/switch-window-r2`: empty), as the
brief requires before a critic pass.

Critic evidence (not pushed, for the orchestrator to place):
`/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/critic5c2-evidence/`
(QEMU runs, `summary.txt`, scripts) and its `jetson/` folder (ELFs, trigger
logs, the five boot captures). File names below without a directory are in
that folder; `run5-hw/...` paths are the builder's or the orchestrator's.

## 1. Order of work and process

Order: the brief; the diff `3b369aef..c0ea0714` of `kernel/sched/sched.c`
and the headers, without commit messages; `schedule()`,
`sched_finish_switch`, migrate, steal, the tick, the sleep queue,
`task_destroy`, `switch_to` at the fix head; the lock graph in section 2,
written to `lockgraph-notes.txt` at 08:21 UTC; only then
`critic-5-C2-design.md` round 2 and ADR 0008's lock graph. Evidence was
counted from the serial text of each file (line 7 onward of the builder's
QEMU files); the first six header lines, which carry the builder's
classification, were printed while listing and are disclosed as seen. No
README, summary or tally was used for a number below except where a summary
is the only record (the `/ci` step results, section 6), and that is said
there.

Leaks, disclosed. The orchestrator's task message stated that fix-ws-3 had
timed out and where. Issue #1242's body (read at 08:20, after the code)
describes the fix direction. The scheduler.md and kernel/CLAUDE.md
paragraphs were read at 08:27 while checking step 6.

Process. One critic QEMU at a time; the builder ran its own QEMU during
08:38 to 09:02 and a QEMU from an older session (pid 3381445, about 46 h
old, 100% of one core) ran throughout, so every rate tonight was taken on a
shared host. The critic killed only its own shell scripts by PID (two
copies of its `plan1.sh`, while they waited to start) and stopped one of
its own hung `grep` commands. Jetson test kernels were built in separate
worktrees. Board time came only from the orchestrator's handover (08:37,
jetson-nano-2, kexec only); labctl MCP tools only; 08:37 to 09:07, 30
minutes. One critic error: the first `boot_test` call used a pattern that
matched the boot banner's "[PASS] All 6 CPUs online", so its two captures
stop before the tests (kept as `jetson/fix-c0ea0714-boot0{1,2}-raw.txt`,
not counted); the two boots were repeated. A second: the 200 s timeout on
the base R3 migrate boot cut it at iteration 132 (section 5). Board left as
found: SD Linux (`BootCurrent: 0001`, no BootNext line), `/root/slmos.elf`
sha256 098f3163 unchanged, the five `/root/slmos-crit-*.elf` deleted and
synced, ESP not touched (no UEFI boot was made), health online, power on,
recovery relay open, claim released.

## 2. The critic's lock graph (written before the ADR's)

From `kernel/sched/sched.c`, `task_sleep.c`, `task.c` at c0ea0714.

Locks on the paths in question: `rq_lock[c]`, `steal_deque_lock[c]`,
`sleep_queue_lock`, `task_lock`. `on_cpu` is not a lock: one store-release
in `sched_finish_switch` (no lock held), load-acquires in
`pick_ready_locked` (under `rq_lock[this]`), `idle_poll_waiting` (no lock)
and `task_wait_off_cpu` (no lock, 2 s bound).

| Path | Locks, in order |
|---|---|
| `schedule()` | tick class masked, then `cpu_id()`; COOP tick (as the tick row, with `preempt_disabled` set); steal prelude: `steal_deque_lock[v]`, released, `rq_lock[v]`, released, `rq_lock[this]`, released; then `rq_lock[this]`; zombie: unlock, caller's mask restored, `task_destroy_gen` (its `on_cpu` wait and `task_lock`, no run-queue lock), mask, id re-read, relock; pick: loads of `on_cpu`, no second lock, no wait; unlock with the caller's mask. |
| window, `switch_to`, `sched_finish_switch` | no lock; `preempt_disabled[this]` = 1; caller's IRQ state |
| tick, any CPU, also inside its own window | `sleep_queue_lock`, released; per woken task `rq_lock[t]`, released, then `steal_deque_lock[t]`; policy tick; returns at `preempt_disabled` |
| `sched_migrate_task` | `rq_lock[min]` then `rq_lock[max]` |
| `sched_try_steal` | `steal_deque_lock[v]` released before `rq_lock[v]` |
| `scheduler_add_task_to_cpu` | `rq_lock[c]` released before `steal_deque_lock[c]` |
| idle | none; mask, read `switch_in_waiting[c]`, wait if NULL, unmask; bounded poll; `yield()` |

Wait-for edges: `rq_lock[a]` to `rq_lock[b]` with a < b (migrate) and
nothing else among these locks. There is no edge from a lock to "T's
`on_cpu` clears". The run-4 cycle needs exactly that edge (B holds
`rq_lock[B]` and waits for A's window; A's tick in the window wants
`rq_lock[B]`); at the fix head B reads once under its lock and defers.
No cycle. The ADR's graph agrees on these rows and adds outer locks
(pi_mutex guard, ipc, UART) that take one `rq_lock` and are never taken
under it; the critic did not re-derive those.

Windows the fix opens to a tick, each forced or exercised:

| Window | What a tick there does | Exercised by |
|---|---|---|
| Deferred T queued on B while A is still in its window, A ticks | A's handler takes `rq_lock[B]` for a wake or add, no wait on `on_cpu` | `test_switch_window_tick_forced`: 2 ticks seen after the move in 10 of 10 critic iterations (`all-fix-c0ea0714-run01.txt`) and in the builder's 400 |
| Idle on B between its unmask and `yield()`, the poll included | nested `schedule()` on the idle task decides under the lock; the poll's `w` is a hint into the static `task_table` | R1 phases and R3 (every deferral with an empty queue goes through it) |
| Zombie destroy, unmasked, id re-read at the relock | the task can be preempted and moved; probe site 2 of `SWITCH_CHECK_CPU` | `stale_cpu=0` in all 20 `switch_check` lines of `all-fix-c0ea0714-run01.txt` |
| Masked check-then-WFI | none can run; a pending tick ends the WFI and is taken at the unmask | by construction; disassembly below |

Not forced by any test: a tick in the masked steal prelude cannot happen by
construction; the `PI5_FIQ_TIMER` and x86 branches were read, not run by the
critic (x86 `make test` passed once at the fix head in the `/ci` record).

## 3. Does the code implement the design that passed

Yes. Part 1: `sched_irq_save()` before `cpu_id()`, masked through the COOP
tick, the steal prelude (`rq_lock_masked`) and the locked section; the
zombie path restores the caller's mask for the destroy, masks again,
re-reads `this_cpu` and `rq`, and jumps back to the lock. Part 2:
`pick_ready_locked` under the lock, exempting `current` and this CPU's
idle task, walking the queue for the first released task, else idle with
`switch_in_waiting[cpu]` set; non-head removal through
`remove_from_cpu_queue_locked`; same in `scheduler_start`. Publishing
store: `__atomic_store_n(&prev->on_cpu, 0, __ATOMIC_RELEASE)` after
`switch_to` returns on the next task's stack (and in
`task_entry_trampoline` for a new task). Acquiring load:
`__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE)` before `switch_to`'s
loads.

Correction 1, branch by branch in `idle_task_func`:

| Branch | Unmasks before waiting | Code at c0ea0714 |
|---|---|---|
| x86 | yes | `cli`; read; NULL: `sti; hlt` in one statement; else `sti` |
| Pi 5 `PI5_FIQ_TIMER` | yes | `daifset #3`; `isb`; read; `wfi` if NULL; `daifclr #3`; `isb` |
| Pi 5 / Jetson `SECONDARY_PREEMPT` (with `JETSON_HW_TICK`) | yes | `daifset #2`; `isb`; read; `wfi` if NULL; `daifclr #2`; `isb` |
| Jetson without `JETSON_HW_TICK` | no | read; `wfe` only if cpu != 0 and NULL |
| Pi 5 without `SECONDARY_PREEMPT`, CPU 0 | yes | `daifset #2`; `isb`; read; `wfi` if NULL; `daifclr #2`; `isb` |
| same, CPUs 1 to 3 | no | read; `wfe` if NULL |
| final `#else` (QEMU, every CPU) | yes | `daifset #2`; `isb`; read; `wfi` if NULL; `daifclr #2`; `isb` |

Disassembly of `idle_task_func` in the critic's Jetson build
(`jetson/slmos-s5cc2-r2-c0ea0714.elf`): `msr daifset,#2` 8002c3a0, `isb`,
`ldr x20,[x0]`, `cbnz x20`, `wfi` 8002c3b4, `msr daifclr,#2`, `isb`,
`bl schedule`; the non-NULL arm unmasks at 8002c3c8 and polls. The order is
as written.

Corrections 2 to 4 are in the ADR text (lines 222 to 240 and 296 to 350:
the boot-context reason for `scheduler_start`, the static `task_table`
reason for the unlocked read, the x86 kick dropped by the #1236 guard).
ADR status reads "Proposed ... stays proposed until the fix verdict", which
is right for a FAIL. `docs/scheduler.md` and `kernel/CLAUDE.md` each have
the one paragraph (step 6).

Observations on the code, none a defect the critic could turn into a
failure:

- A deferred higher-priority task waits up to one tick (10 ms) when the
  deferring CPU has another ready task, because nothing requests a
  reschedule when the release comes. The design verdict left this to the
  builder; it is stated in the ADR. It is the likeliest source of the
  Jetson mean in item 3 not being pure noise only if `bench context`
  migrates, which the critic did not check.
- `pick_ready_locked` can now switch in a task that is not the queue head,
  so `pick_next_task`'s panic on a TERMINATED or DESTROYED head no longer
  guards that candidate. Only reachable with a stale queue entry (#1241
  describes one way to make one). Worth one state check in the walk.
- `switch_to`'s comment says the caller holds interrupts masked across it;
  `schedule()` has unlocked and restored the caller's mask before it. Old,
  not changed by this fix, and the comment is wrong.

## 4. Numbers from the builder's files, counted by the critic

Baseline (88d4d71b = base + C1 + tests), `run5-hw/c2/qemu-base/` and
`hw-base/`:

| Reproducer | Cell | Files | Count | Iterations per failure | 5x |
|---|---|---|---|---|---|
| R1 | QEMU | `r1-base-88d4d71b-run01..20.txt` | 20 of 20 fail in iteration 1 (14 with a dump, 6 timeout with none) | 1 | 5 |
| R3 migrate | QEMU | `r3m-base-88d4d71b-run01..20.txt` | 20 of 20 fail; first failure after 88 iterations summed over the runs | 4.4 | 22 |
| R3 wake | QEMU | `r3w-base-88d4d71b-run01..20.txt` | 20 of 20 fail; 22 iterations summed | 1.1 | 6 |
| R2 | Jetson kexec | `kexec-r2-boot1..3.txt` | 3 of 3 boots fail at iteration 1 (boot 2 with no dump) | 1 | 5 |
| R3 migrate | Jetson kexec | `kexec-r3m-boot1..3.txt` | 200, 200, then a dump after 53 passes: 1 failure in 454 | 454 | 2270 |
| R3 wake | Jetson kexec | `kexec-r3w-boot1..3.txt` | 3 of 3 stop at iteration 1, no dump | 1 | 5 |
| R2 | Jetson UEFI | `uefi-r2-boot1.txt` (1 boot, not 3) | 38 of 198 printed iterations fail, first at 7 | about 5 | 26 |
| R3 wake | Jetson UEFI | `uefi-r3w-boot1.txt` (1 boot) | stops at iteration 1, no dump | 1 | 5 |
| R3 migrate | Jetson UEFI | none | not run | - | - |
| R2 | Pi 5 | `pi5-r2-boot1..3.txt` | 600 of 600 pass: never failed | - | - |
| R3 migrate | Pi 5 | `pi5-r3m-boot1..3.txt` | 3 summaries `runs=200 passed=200`: never failed | - | - |
| R3 wake | Pi 5 | `pi5-r3w-boot1..3.txt` | 3 of 3 stop at iteration 1 (1 dump, 2 none) | 1 | 5 |

The ADR's table says 5.55 for QEMU R3 migrate; the critic's count of the
first failing or missing iteration gives 4.4. Either way the 5x number is
under 30. Two `[ITER]` lines in the Jetson and Pi 5 R3 migrate captures are
split by interleaved output; the summaries say 200.

Fix head, `run5-hw/c2/qemu-fix/` and `hw-fix/`:

| Reproducer | Cell | Files | Count | Needed | Met |
|---|---|---|---|---|---|
| R1 | QEMU | `all-fix-4bfc15a5-run1..20`, `all-fix-c0ea0714-run1..20` | 10 of 10 in each of 40 runs: 400, 200 of them at c0ea0714 | 5 | yes |
| R3 migrate | QEMU | same 40, plus `r3-r2-default-68f0cb7b-run01..05` | 400 with `SWITCH_CHECK=ON`, 100 with default options | 22 | yes |
| R3 wake | QEMU | same | 400 and 100 | 6 | yes |
| R2 | Jetson kexec | `kexec-r2-fix-c0ea0714-boot1.txt` | 200 of 200 | 5 | yes |
| R3 migrate | Jetson kexec | `kexec-r3m800-fix-c0ea0714-boot1..3.txt` | 800, 800, 800 | 2270 | yes |
| R3 wake | Jetson kexec | `kexec-r3w-fix-c0ea0714-boot1.txt` | 200 of 200 | 5 | yes |
| R2 | Jetson UEFI | `uefi-r2-fix-c0ea0714-boot1.txt` | 200 of 200 | 26 | yes |
| R3 wake | Jetson UEFI | none | not run | 5 | **missing** |
| R2 | Pi 5 | `pi5-r2-fix-c0ea0714-boot1.txt` | 200 of 200 | run regardless | yes |
| R3 migrate | Pi 5 | `pi5-r3m-fix-c0ea0714-boot1.txt` | 200 of 200 | run regardless | yes |
| R3 wake | Pi 5 | `pi5-r3w-fix-c0ea0714-boot1.txt` | 200 of 200 | 5 | yes |

Every fix-head capture carries the banner `(c0ea0714)` and starts at the
boot ROM or firmware line. R3 at the fix head still aims at the window: the
migrate phase reports 12084 aimed moves in an iteration
(`all-fix-c0ea0714-run1.txt`), against 42584 at the base.

No deadlock, `run5-hw/c2/qemu-nodeadlock/`:
`forced-reverted-653650d5-run01..20.txt`, 20 of 20 end in the timeout with
no summary and no dump. The file names carry 653650d5, not the ef11ee3c the
orchestrator named for the reverted design; the two `-sampled` runs are at
ef11ee3c and also hang. At the fix head the forced test completes 400 of
400 (the 40 `all-fix` runs).

## 5. The critic's reproduction

QEMU, `summary.txt` and one file per run:

| Run | Head | Result |
|---|---|---|
| `all-fix-c0ea0714-run01.txt` (`SWITCH_CHECK=ON SWITCH_WINDOW_TEST=ON`, x10) | fix | R1 10 of 10, forced tick 10 of 10 with 2 ticks seen each, R3 migrate 10, R3 wake 10, R2 10; 20 `switch_check` lines, all `bad_gen=0 bad_dual=0 stale_cpu=0` |
| `r3-r2-default-fix-c0ea0714-run01.txt` (default options, x50) | fix | R3 migrate 50, R3 wake 50, R2 50 |
| `r1-base-3b369aef-run01,02.txt` | base | 2 of 2: instruction abort on cpu0 in iteration 1 |
| `forced-reverted-ef11ee3c-run01,02.txt` | reverted | 2 of 2 hang with cpu0 in the window (at phase iterations 2 and 0), no dump, 60 s timeout |
| `r3m-base-3b369aef-run01,02.txt` | base | 2 of 2 fault, after 1 and 2 passes |
| `r3w-base-3b369aef-run01,02.txt` | base | 2 of 2 stop in iteration 1, no dump within the 60 s timeout |

Jetson kexec, one boot each, full serial from power-on, banner checked,
ELF sha256 echoed by the trigger loop (`jetson/trigger-fix.log`,
`jetson/trigger-base.log`):

| File | Head | Result |
|---|---|---|
| `jetson/fix-c0ea0714-boot01.txt` | fix, build 20261114083105 | R2 200 of 200 |
| `jetson/fix-c0ea0714-boot02.txt` | fix, build 20261114083233 | R3 migrate 200 of 200, R3 wake 200 of 200 |
| `jetson/base-3b369aef-r2-boot02.txt` | base | first failure at iteration 2; 15 of 34 iterations fail; PC alignment fault on idle_1 (cpu4, ELR 0xb00000001), then a data abort on cpu5 |
| `jetson/base-3b369aef-r3w-boot03.txt` | base | data abort on cpu2 before iteration 1 completes |
| `jetson/base-3b369aef-r3m-boot01.txt` | base | 131 passes, no failure; cut by the critic's 200 s timeout, not a hang. Not a reproduction of the baseline failure (1 in 454 there) |

So R2 and R3 wake fail on the base and pass 200 of 200 at the fix head on
the critic's own boots; R3 migrate passes at the fix head and was not made
to fail at the base in one short boot.

## 6. The litmus failures at the fix head

Record. `run5-hw/c0/ci-c2-c0ea0714/summary.txt` and the `Test Results`
block of each log (the serial logs of these runs were not kept, so the
failing test name comes from the make output): `04-test` FAILED,
`05-test-aisched` FAILED, `10-ws-on-01..20` 13 PASSED and 7 FAILED, every
failure `test_litmus_patterns_pass` and nothing else. Reruns in
`run5-hw/c0/ab-ws-c0ea0714/summary.txt`: `fix-04r` FAILED (litmus),
`fix-05r` PASSED. The builder's own `fix-regress/maketest-default-c0ea0714-run01.txt`
also fails litmus (2 weak rows, P1 1>3 and P2 1>0, `observed=0`). So plain
`make test` has no passing run on record at c0ea0714 (0 of 3), and the
x20 row reads 7 of 20 against the C0 baseline's 0 of 3.

The same test fails on heads without the fix when the host is loaded:
`run5-hw/c0/ci-c1-2196deb5/summary.txt` has `04-test` and `05-test-aisched`
failing at start loads 5.57 and 4.38 and passing on rerun at 2.79 and 2.60.
The seven failing WS runs at the fix head started at loads 4.35 to 6.13;
the C0 baseline ran at about 2.1.

Interleaved A/B (orchestrator, `ab-ws-c0ea0714/summary.txt`): base 5 of 6
pass (1 litmus); fix 4 of 6 pass (1 litmus, 1 timeout, section 7).

Critic, both heads, one after the other on the same host,
`TEST_FILTER=test_regs_every_cpu_reports,test_litmus_patterns_pass
TEST_REPEAT=15`:

| File | Config | Head | litmus failed | start load |
|---|---|---|---|---|
| `lit-default-fix-c0ea0714-run01.txt` | default | fix | 2 of 15 | 3.80 |
| `lit-default-base-3b369aef-run01.txt` | default | base | 5 of 15 | 2.96 |
| `lit-ws-fix-c0ea0714-run01.txt` | WS=ON | fix | 8 of 15 | 3.94 |
| `lit-ws-base-3b369aef-run01.txt` | WS=ON | base | 11 of 15 | 4.95 |

The builder's later pair says the same
(`run5-hw/c2/qemu-regs-hang/regs-litmus-ws-c0ea0714-run01.txt` 51 of 200,
`regs-litmus-ws-base2196deb5-run01.txt` 112 of 200). QEMU CPU time for the
same filter is the same on both heads (36.1 s user at each, default
config), so idle vCPUs are not spinning more at the fix head.

Judgement. Hypothesis, supported and not proven: the failures are #1230
(a short row whose reader's vCPU is not running), driven by host load and
the 200% CPU quota, and the fix does not raise the rate. On the bar as
written, "/ci green" is not met on record and "WS no worse than this run's
baseline rate" compares 7 of 20 with 0 of 3 taken at half the load. The
critic will not turn a failed row into a pass by argument; it asks for the
row to be re-run (fix 2 below).

## 7. The open row: fix-ws-3 timed out in `test_regs_every_cpu_reports`

File: `run5-hw/c0/ab-ws-c0ea0714/fix-ws-3.test-output.log`, 7335 lines,
last test line `[PASS] test_iteration_counts`, then the timeout's signal.
The next test is `test_regs_every_cpu_reports`; its register table prints
only after `coherency_regs_collect` returns, so the stall is inside that
call: the caller waits, in 1 ms sleeps, up to 120 s for a HIGH-priority
task pinned to each other CPU. A stall there means a pinned READY task did
not run, or the caller's own sleep was not woken, for the rest of the
120 s test budget. The run was at about 81% of the suite's output, so a
slow run killed at a random point would have had to land inside one short
test; the critic treats it as a real stall of unknown length.

From the code, the question was whether the fix can leave a READY pinned
task unrun. The critic found no way:

- Such a task is deferred only while its `on_cpu` reads 1. `on_cpu` becomes
  1 only at a switch-in under the picking CPU's lock, and every switch-in
  is followed, on the same CPU, by `sched_finish_switch(prev)` for the task
  it replaced (after `switch_to` in `schedule()`, or in
  `task_entry_trampoline`), with `preempt_disabled` covering the gap. A
  pinned task's previous CPU is its own, which cannot call `schedule()`
  again before that store. `task_alloc` sets `on_cpu = 0` for a new task,
  and `task_destroy` has waited for the old occupant.
- A deferral never drops a task: the head stays queued, a non-head pick is
  removed with `remove_from_cpu_queue_locked`, and `current`, if queued, is
  always eligible, so idle is chosen only when nothing else can run.
- Idle cannot sleep through a queued task that it deferred:
  `switch_in_waiting` is read with the tick masked on every branch that
  unmasks, and QEMU's idle takes a tick every 10 ms in any case.
- The sleep and wake path is unchanged; a wake that lands in the caller's
  own window queues it on its own CPU, whose next `schedule()` comes after
  its own release.

By experiment, WORK_STEALING=ON:

| File | Head | Result |
|---|---|---|
| `lit-ws-fix-c0ea0714-run01.txt`, `lit-default-fix-c0ea0714-run01.txt` | fix | `test_regs_every_cpu_reports` 15 of 15 each |
| `regs-ws-fix-c0ea0714-run01.txt` | fix | 1000 of 1000 |
| `regs-ws-base-3b369aef-run01.txt` | base | 1000 of 1000 |
| `full-ws-fix-c0ea0714-run01..06.txt` (full `make test WORK_STEALING=ON`, 90 to 96 s of QEMU each against the 120 s timeout) | fix | 0 timeouts; `test_regs_every_cpu_reports` passed in 6 of 6; 3 runs pass, runs 03 and 04 fail `test_litmus_patterns_pass` (#1230), run 05 fails `test_task_pinned_to_cpu0` at `test_scheduler.c:6621` (`assigned_cpu` 1, the stolen-task assumption already filed as #1233) |
| builder, `run5-hw/c2/qemu-regs-hang/regs-ws-c0ea0714-x2000-run01.txt` and the base twin | both | 2000 of 2000 each |
| builder, `full-ws-c0ea0714-sampled-run01..03.txt` | fix | 3 full runs, no timeout |

Not reproduced. Rate on record: 1 timeout in 35 full WS=ON runs at the fix
head (orchestrator 20 + 6, critic 6, builder 3), against 0 in 9 on the
base (3 + 6). A full run needs about 90 s of the 120 s budget on this
host, so the stalled run lost at least some tens of seconds in that test.
Issue #1139 (Jetson, before this fix: a secondary CPU stopped running
pinned tasks mid-suite, once) describes the same symptom on another
platform; whether it is the same defect is not known. That difference is not evidence either way.
The filtered loop does not carry the state the full suite has built up by
that point (about 7000 lines of earlier tests, the R3 tests among them,
which exist only at the fix head and are the ones that exercise #1241), so
the filter not reproducing it does not clear the full suite. Open, with no
cause; it needs an issue and a capture (fix 5).

## 8. The bars, as written

**Fixed.** Met on every cell that was run (section 4), reproduced by the
critic in QEMU and on Jetson kexec. One cell that failed at baseline has no
fix-head run: R3 wake on Jetson UEFI-direct. Jetson UEFI baselines were 1
boot each where step 1 asked for 3 (R2) and R3 was not baselined there
beyond the wake boot. **Not met as written: one cell missing.**

**No deadlock.** Reverted design hangs 20 of 20 (builder, 653650d5) and 2
of 2 (critic, ef11ee3c) with the forced tick; fix head completes 400 of 400
(builder) and 10 of 10 (critic) with 2 ticks seen in the window. Lock graph
derived first. **Met.**

**No regression.**

| Row | Evidence | Judged |
|---|---|---|
| /ci green | `ci-c2-c0ea0714`: builds, x86 test, runtime test, braces, build order, Jetson layout pass; `make test` and `make test AI_SCHED=ON` fail litmus; reruns 1 fail, 1 pass | not green on record; cause not the fix on the critic's data |
| WS=ON no worse than baseline | 13 of 20 and 4 of 6 against 3 of 3 and 5 of 6; litmus rate per iteration not higher at the fix head (section 6); one unexplained timeout | not shown as written; open item |
| boot_test --count 10, Pi 5 | `hw-fix/pi5-prod-fix-c0ea0714-boottest-01..10.txt`, 10 files, each from the firmware line to `slmos>`, banner c0ea0714 | met |
| boot_test --count 10, Jetson kexec | `hw-fix/kexec-prod-fix-c0ea0714-boot1..3.txt`, 3 boots to the shell | 3 of 10: **missing** |
| boot_test --count 10, Jetson UEFI | none | **missing** |
| --assert-shares 20 of 20, Jetson kexec | none (no file contains `assert-shares` or `bench stealing`) | **missing** |
| bench smp context switch within 10%, both boards | `hw-fix/bench-context-pi5.txt`, `bench-context-jetson.txt`: `bench context`, numbers "transcribed" from tool results, no raw capture; Pi 5 106 to 110 and 112 ns (+3.8% to +5.7%); Jetson 87 and 88 to 96 and 98 ns (+10% to +13%), 32 ns counter resolution, p50 unchanged | Pi 5 within, unverifiable; Jetson **outside 10% as written**; wrong bench; number without a raw file |
| Full suite, 5 boots, each Jetson path and Pi 5 | none | **missing** |

**Run-4b row (test kernel at the PR head on every board and path).**
Filtered test kernels at c0ea0714 ran on Jetson kexec (R2, R3 migrate, R3
wake), Jetson UEFI (R2 only) and Pi 5 (R2, R3, and a COOP build with all
three). The full test suite ran at c0ea0714 only in QEMU. **Not met.**

## 9. Failure modes, by name

| Mode | Finding |
|---|---|
| Selective summary | The `/ci` and WS x20 results were given to the critic as an open question, not hidden. The missing rows of section 8 are not in any file; whether the builder's report lists them as missing the critic has not read. |
| Truncated or tail-only capture | None in the builder's hardware files (each starts at MB1 or the Pi firmware). The critic's two bad-pattern captures and one short boot are disclosed. |
| Assembled register file | `bench-context-*.txt` are transcriptions, not captures. |
| Redefined criterion | `bench context` stands in for `bench smp`; 3 kexec boots stand in for 10. |
| Hypothesis stated as finding | The ADR labels "#1238 is the wake path" as a hypothesis and names the deciding test; R2 at the fix head on kexec and UEFI now passes, which is the test it named. The Jetson R2 trace is still absent (`kexec-r2trace-base-4812e821-boot1.txt` has no dump), so the sequence on Jetson is inferred, not traced. |
| Stale base | Fix branch is on 2196deb5 on 8ec0e330 = origin/main at the brief. |
| Missing critic | This file. |
| Partial per-CPU table | Not applicable to a fix head with no dumps; baseline dumps not audited CPU by CPU. |
| Number without a file | Bench numbers (above). |
| Uncommitted tree | Critic worktrees clean at each build; builder headers say 0 uncommitted changes. |
| Critic reading the claim first | Section 1; leaks disclosed. |
| Undisclosed rule break | None found. The builder ran QEMU while the critic did; allowed (two at once). |
| Banner hash mismatch | None: every counted fix-head file has `(c0ea0714)`, `(68f0cb7b)` or `(4bfc15a5)` as named. |
| Bar computed from one cell | 5x thresholds computed per cell (section 4). |
| Precondition from a prior run | Baselines are this run's. |
| Path never exercised | Jetson UEFI R3 at the fix head; `PI5_FIQ_TIMER` and the Pi 5 non-`SECONDARY_PREEMPT` CPU 0 branch (the builder's COOP boot covers the WFE branches; the critic did not check which idle branch that build compiles). |
| Run 4, C1: "cannot deadlock" | Lock graph from code first (section 2); a tick forced into the window; no wait under a lock. |
| Run 4b: production-only rows | Section 8, last paragraph: not met. |
| Run 4b: wrong frame as fault site | The ADR cites no ELR as a fault site; it cites exception classes with "wrong frames". EC 0xe (illegal execution state) and PC alignment faults can be raised at any instruction once SPSR or ELR is corrupt, which is what the claim needs. |
| Run 4 critic: pkill | The critic killed two of its own script PIDs and nothing else. |
| Run 4 critic: claiming a board | Handover from the orchestrator at 08:37; claim as `switch-5-critic-c2`; released 09:07. |

## 10. Fixes required for a pass

1. Run the missing hardware rows at c0ea0714 (or the rebased head) with
   full captures: `boot_test --count 10` on Jetson kexec and on Jetson
   UEFI-direct; `bench stealing --sustain 2500 --assert-shares` 20 of 20 on
   Jetson kexec; the full integration suite 5 boots on Jetson kexec, Jetson
   UEFI-direct and Pi 5, each failure with an issue number; R3 wake x200 on
   Jetson UEFI-direct.
2. A green `/ci` at the fix head with the host quiet (the stray QEMU
   pid 3381445 dealt with by its owner, no second QEMU), and the WS x20 row
   interleaved with the base at the same load, serial logs kept for every
   failing run.
3. `bench smp` on both boards, base and fix, raw serial captures, at least
   5 runs a side on Jetson given the 32 ns resolution. If the Jetson mean
   stays above 10%, the PR says so and the user decides; the bar does not
   let the critic waive it.
4. The PR description lists, per board and path, which rows ran on the
   production kernel and which on the test kernel.
5. File the `test_regs_every_cpu_reports` timeout as an issue (platform
   qemu, sub sched, Backlog) with `fix-ws-3.test-output.log`, and run full
   `make test WORK_STEALING=ON` under the QMP sampler until it recurs or 50
   runs pass.

Suggested, not required: a TERMINATED/DESTROYED check on the non-head
candidate in `pick_ready_locked`; correct the `switch_to` comment about the
caller's mask.
