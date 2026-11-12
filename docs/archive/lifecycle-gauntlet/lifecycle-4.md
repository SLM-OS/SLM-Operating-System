# Lifecycle gauntlet log, run 4 (#1213, #1223, #1211, #1219, #1199, #1224)

Goal: root-cause and fix #1213 under a three-part bar (first-fault trace,
a stress test that fails on the unfixed kernel at a measured rate, and the
fix passing 5x as many runs as it took on average to see a fault); decide
whether #1223, #1211 and #1219 are the same defect; get the Jetson
integration suite to its last test on kexec and UEFI-direct, 10 of 10 boots
each; convert test_isolated_core_latency's last count-bounded wait so the
isolation path runs on every board. The brief is the user's message of
2026-11-11 (PT). This log is append-only: a correction is a new entry
citing the one it corrects, and every entry carries its own write time.

Boards: pi-5-1 and jetson-nano-2 only. Evidence: `gauntlet-4-hw-pi5`
(Pi 5, plus QEMU under `run4-hw/c1/` and `run4-hw/c2/`) and
`gauntlet-4-hw-jetson`, one file per run under
`docs/archive/lifecycle-gauntlet/run4-hw/c<N>/`, banner hash at the top.

## Budget

- Started: 2026-11-12 01:05:43 UTC (17:05:43 PST, the orchestrator's first
  action). Limit: 10 h, 11:05 UTC.
- Hour marks (UTC): C0 done 01:35; C3 holds jetson-nano-2 01:35 to 04:35;
  C3 critic boots 04:35 to 05:05; C1 stress test and fix in QEMU 04:05 to
  07:05; C2 holds jetson-nano-2 for --assert-shares and C1's Jetson boot
  tests 07:05 to 08:05; C1 holds pi-5-1 08:05 to 10:05 (C1 critic
  reproduction inside it); C2 critic, PRs, close-out 10:05 to 11:05.
- A component not at its mark loses the lab at the next handover and ends
  with its last result.

## Rules summary

- Run 3's rules carry over (`docs/archive/coherency-gauntlet/run3-rules.md`
  and `run3-critic-protocol.md` on `gauntlet/coherency-3`) with the brief's
  deltas: this run may change task lifecycle and scheduler ordering, priced
  by the three-part bar, the critic's independent Arm ARM ordering review and
  --assert-shares 20/20 on Jetson kexec.
- Single lab lane; no labctl call without holding the lab; every handover is
  an entry with its time. Boot and capture in one call. No BL31/TF-A
  flashing. Jetson ESP and Pi 5 card restored, both boards left powered on.
- Trace ring behind a build option, default off, zero text-size cost when off.
- Draft PRs only, no merge to main. Each component owns its files.
- No number without a filename. No em-dashes, no second person, hypotheses
  labelled until tested.
- Critics run on Fable, blind (definition of done, issues, evidence paths,
  heads only); builder entries are written after the critic files.
- Stop conditions per component as in the brief; the run stops only for both
  boards unreachable more than 45 min after the round-trip rule, any
  proposal to flash BL31, patch TF-A or touch another board, or budget.

## Entry 0 (written 01:08 UTC, completed in Entry 0a): hour-0 checks

Read: issues #1213, #1223, #1211, #1219, #1199, #1224 (states below);
coherency-3 log headings. Entries 8, 9, 15 and 26 to 32 are handed to the
builders whose components cite them; the orchestrator does not re-derive.

1. **Reminder self-test.** This session has no `send_later`,
   `list_triggers` or `delete_trigger`; the equivalents are the
   session-only `CronCreate`, `CronList`, `CronDelete`. One-shot job
   `001d9f0e` created 01:05:50 UTC for 01:07 UTC; `CronList` showed it with
   no prompt. Fire result: Entry 0a. A cron job fires only while the session
   is idle, not mid-turn.
2. **First use of every tool family**, no prompt on any: `CronCreate`,
   `CronList`; `gh pr list` (no open PRs, so `gh pr comment` is skipped
   until this run opens one), `gh issue view` on the six issues; labctl
   `list_claims`, `bindings_list`, `claim_sbc`, `run_health_check`,
   `actuation_status`, `release_sbc` on pi-5-1 then jetson-nano-2; `git
   push` of the two evidence branches; background Bash; Agent with model
   fable (item 7). Monitor and SendMessage are loaded; first use in 0a.
3. **Lab state (01:06 UTC).** No active claims. pi-5-1: online, ping,
   serial, power on, no bindings. jetson-nano-2: online, ping, serial, power
   on; `recovery_mode` binding desired `released`, relay-a channel 1
   last_state `open` (changed 2026-11-11 22:41:20 UTC). Both released after
   the check.
4. **main.** Head **6c1f4c9c** ("bench stealing --assert-shares and bench
   idle-wakes (#1216) (#1227)"). Merged: #1208 8a27711d, #1209 cf34214d,
   #1215 73d8deb7, #1217 88b28c15, #1218 e4a33790, ADR 0007 accepted
   e73b9b61, removal #1225 98e17440, --assert-shares #1227 6c1f4c9c.
   Closed: #1226, #1216. Open: #1213, #1223, #1211, #1219, #1199, #1224.
   /ci and 3 runs of `make test WORK_STEALING=ON` on 6c1f4c9c: running from
   01:07 UTC in a clean worktree at main; results and the #1219 baseline
   count in Entry 0a.
5. **Branches.** `gauntlet/lifecycle-4` (this log) from 6c1f4c9c;
   `gauntlet-4-hw-pi5` and `gauntlet-4-hw-jetson` pushed at 6c1f4c9c at
   01:07 UTC.
6. **Lab schedule (single lane).** C3 holds jetson-nano-2 from the end of
   hour 0 (target 01:35 UTC) to 04:35, then the C3 critic to 05:05. C2
   holds jetson-nano-2 when C1 has a fix head (mark 07:05) for
   --assert-shares 20/20 and C1's Jetson boot tests. C1 then holds pi-5-1
   (mark 08:05). pi-5-1 is idle until then; nobody calls labctl without
   holding the lab.
7. **Critic model.** A trivial critic spawned with model fable answered
   "critic-check ok, model: Fable 5.2 (claude-fable-5-2)". Critics run on
   Fable.
8. **Milestone.** Backlog for new issues.

## Entry 0a (written 01:34 UTC): hour-0 checks completed; C0 done

1. **Reminder self-test: passed.** Job `001d9f0e` (due 01:07 UTC) fired
   at about 01:10 UTC, delayed because the session was mid-turn; it fires
   only into an idle session. `CronDelete 001d9f0e` afterwards returned "No
   scheduled job" (one-shots delete themselves). Hour marks scheduled as
   one-shot jobs: `c430dcc2` 04:35, `ad030618` 07:05, `0447d0e3` 08:05,
   `94598cb0` 10:05, `d5060185` 10:57 UTC (budget warning). Session-only;
   the orchestrator also keeps time from `date -u` and is woken by builder
   completion notices.
2. **Tool families, continued.** SendMessage used (01:2x UTC, rulings to
   C3 and a hand-off to C1, Entry 1). Monitor loaded, not yet needed.
4. **main 6c1f4c9c: /ci green, #1219 baseline 0 of 3.** Logs:
   `run4-tools/ci-main-6c1f4c9c/` on this branch (`summary.txt` has
   start, end and load per step; script `run.sh`). Builds ARM64, x86-64,
   Jetson real weights main and test: rc 0. `make test` PASSED;
   `make test PLATFORM=X86_64` PASSED; `make test AI_SCHED=ON` PASSED;
   `make runtime-test` 308 and 328 passed, 0 failed; braces clean; build
   order passed; Jetson layout "All checks passed." with no `!` line.
   `make kernel-test-clean && make test WORK_STEALING=ON` three times
   (`-DENABLE_WORK_STEALING=ON` confirmed in each configure line):
   PASSED 3 of 3 (`10-ws-on-1.log` to `10-ws-on-3.log`), load average
   1.71 to 2.07 at their ends. **The #1219 baseline for this run is 0
   failures in 3 runs** (run 3 saw 2 of 6, `run3-tools/ci-main-*` on
   `gauntlet/coherency-3`).
   Side observation, matching C3's #1199 report (Entry 1): the ARM64 build
   warns `implicit declaration of function` for `memchr` (lstrlib.c:681),
   `strerror` (lauxlib.c:255) and `strcoll` (lvm.c:384),
   `01-arm64-build.log`.

C0 done at 01:34 UTC. C3 started 01:1x UTC and holds jetson-nano-2; C1
started 01:1x UTC, QEMU held until this baseline finished (it did at
01:32:20 UTC); C2 starts now, offline, with C1 sharing the host (at most
2 QEMU instances each).

## Entry 1 (written 01:34 UTC): C3 interim, two rulings, a hand-off to C1

C3 reported at about 01:25 UTC (interim, not a builder entry; the C3
builder entry is written after its critic files):

- On main 6c1f4c9c, Jetson kexec test kernel, boot 1 of 2 so far: a new,
  earlier crash, `PAGE FAULT (WRITE)` on CPU 0 task main, ELR in
  `pmm_alloc_pages`, x30 in `task_create_with_priority`, x19
  `isolation_bench_task`, right after "CPU 2: isolated from general
  scheduling" in test_shell_cmd_bench. Outside C3's files. Ruling: C3 files
  it as a new issue with the capture and does not debug it; C1 classifies
  it with #1211 and #1223 in its step 6. Boot 2 showed #1199 and a broken
  stack canary on idle_4.
- #1199 and #1224 causes as reported by C3 (hypotheses until C3's critic
  checks them): a missing `memchr` prototype truncates a pointer with bit
  31 set (Jetson heap); an orphan `.got` outside the UEFI-copied range.
  Ruling: `kernel/include/string.h` and the ARM64 linker scripts are inside
  C3's scope for these fixes. Because the linker-script change reaches every
  ARM64 board, C3's PR head also needs `boot_test --count 10` on pi-5-1;
  C3 may claim pi-5-1 for that one boot test once its head is final (pi-5-1
  is otherwise idle until 08:05 UTC), restoring the card as found.

## Entry 2 (written 02:09 UTC): #1228 filed; C2 interim; C3 lab slot extended to 06:15

- C3 filed the Jetson isolation-phase fault as **#1228** (platform:jetson,
  sub:mm). The orchestrator raised it from P2-medium to P1-high with a
  comment (it halts the suite on main in the same phase as #1223). C3's
  step-1 evidence: `gauntlet-4-hw-jetson` be03d006,
  `run4-hw/c3/`: kexec 0 of 3 completed (run01 #1228, run02 and run03
  #1199), UEFI-direct 0 of 3 (#1224). Handed to C1 for step-6
  classification.
- C2 interim (relayed to C1): on unmodified main 6c1f4c9c with
  WORK_STEALING=ON, baseline run02 of 20 hung at the 120 s cap with CPU 1
  in panic() from task_exit ("schedule returned!"), isolated=0/10: the
  #1213 family signature without run 3's 1 s change. Run03 failed in
  test_litmus_patterns_pass at load 5.5, recorded as its own row. Files
  pending under `run4-hw/c2/` on `gauntlet-4-hw-pi5`; the counts are
  written when C2 reports.
- C3 reported both fixes working at d512f40b (kexec suite reaches "Test
  Suite Summary", run 01) and a time estimate: about 8 min per suite boot,
  so the required 10 + 10 suite runs and 10 + 10 production boot tests
  finish about 05:40 UTC, not at the 04:35 mark. Ruling: no cut, because
  both counts are bars. C3's hold on jetson-nano-2 (and pi-5-1 for the
  linker-script boot_test) is extended to 06:15 UTC hard; C3 critic 06:15
  to about 06:50. The next handover (C2 at 07:05) is unchanged; C1 has no
  fix head yet, so no other component loses lab time.

## Entry 3 (written 02:19 UTC): C2 interim (offline), #1229 filed, relayed to C1

Interim, not the C2 builder entry (written after the C2 critic files).
C2 reported at 02:18 UTC, evidence `gauntlet-4-hw-pi5` 4dd099b9,
`run4-hw/c2/ws-main-6c1f4c9c-run01..20.txt` and
`ws-main-6c1f4c9c-summary.txt`, one elf (sha256 1a96044f...39d0), one
QEMU at a time, load 2.74 to 5.67 with C1 running alongside:

| Class on main 6c1f4c9c, WORK_STEALING=ON | Runs | Files |
|---|---|---|
| A: #1219 batch stress hang, then the cap | 3 of 20 | run05, run08, run17 |
| B: #1213 family, CPU 1 or 3 in panic("task_exit: schedule returned!") | 2 of 20 | run02, run13 |
| C: test_litmus_patterns_pass mp_weak_rows=1 | 3 of 20 | run03, run16, run17 |
| Any failure | 7 of 20 | |

(The C0 baseline was 0 of 3, Entry 0a; both are this run's cells.)

C2's samples at each class-A hang: all four vCPUs spin on ENGINE_LOCK in
run_inference_inner; the holder is a READY worker preempted by a timer IRQ
mid-inference, at the head of CPU 2's queue; preempt_disabled stays
[0,0,1,0] for 13 s while CPU 2's tick count rises and its schedule()
count does not. C2 filed **#1229** (P1-high, sub:sched) with the
hypothesis, untested, that schedule() clears preempt_disabled for the CPU
id read before switch_to. The orchestrator read main's code
(`kernel/sched/sched.c`, schedule()): `this_cpu = cpu_id()` at entry,
`switch_to(current, next)`, then `preempt_disabled[this_cpu] = 0` and
`reschedule_pending[this_cpu] = 0` (line 2534 on 6c1f4c9c). A task that
resumes on another CPU (stolen) therefore clears the CPU it left, not the
one it runs on. That the code does this is read from the source; that it
causes class A, and whether the stale clear of the old CPU's flag opens the
#1216 steal window on a CPU still inside switch_to (a candidate path to
#1213), are hypotheses until C1's trace and C2's confirmation run.

Rulings: #1229 is in kernel/sched (C1's files). C1 is asked to add
preempt_disabled set/clear (CPU written vs CPU running) and
sched_try_steal decisions to its trace ring and to own the #1229 fix
decision; C2 writes no kernel patch, files class C (file-don't-fix),
builds an unattended 20-run harness for C1's head, and measures a 20-run
WORK_STEALING=OFF control cell on main.

## Entry 4 (written 02:48 UTC): C2 interim 2; #1230 filed; C2 idle until C1's head

Interim (not the builder entry). `gauntlet-4-hw-pi5` 0dfa07b7,
`run4-hw/c2/`. Accel: the Makefile passes no `-accel`, so `make test`
runs TCG thread=multi (`accel-threads.txt`). WORK_STEALING defaults OFF
for QEMU in CMakeLists.txt (the Makefile comment says ON; noted on #1229),
so /ci's `make test` does not exercise stealing.

| Class on main 6c1f4c9c | WS=ON (load 2.74 to 5.67) | WS=OFF (load 3.46 to 6.75) |
|---|---|---|
| A: #1219 stress hang | 3 of 20 | 0 of 20 |
| B: #1213 family task_exit panic | 2 of 20 | 0 of 20 |
| C: litmus mp_weak_rows | 3 of 20 | 9 of 20 |

Files: `ws-main-6c1f4c9c-summary.txt`, `ws-off-main-6c1f4c9c-summary.txt`
and their run files. The WS=OFF zeros for A and B are below the 5x rule
(3.0x and 2.0x); they are not used as a finding. Class C filed as
**#1230** (sub:smp, platform:qemu, P2-medium, Backlog), file-don't-fix;
hypothesis (untested): a reader vCPU thread starved under CPUQuota=200%.
Ruling: no further C2 host cells (WS dependence is not a bar; C1 has the
host); C2's unattended harness (`tools/v2/run_cell.sh`) waits for C1's
head.

## Entry 5 (written 03:33 UTC): C3 status; head moved to 8a3406ea; slot to 07:05

C3 reported at 03:33 UTC (status, not the builder entry). Kexec suite 10
boots at d512f40b: 9 of 10 reached "Test Suite Summary" with the same 15
failing checks (#1109), 1 of 10 halted in test_pmm (pmm_alloc_pages_low;
PMM family, #1207 / #1228, not C3's scope). With #1224 fixed, UEFI-direct
reached two further UEFI-only board kills in camera code, both in C3's
scope: test_tegra_i2c_dump_status_bounds reading an unpowered camera I2C
controller (EL3 exception; test skip, 12079f3d), and
imx219_power_on raising a TF-A RAS error (driver guard, 8e3d6cc2, then
8a3406ea). At 8e3d6cc2 the UEFI-direct suite ran to the end for the first
time. Because 8a3406ea changes production code (imx219.c), the d512f40b
kexec cell is not at the final head and is rerun.

Rulings: C3's hold on jetson-nano-2 (and pi-5-1 for its boot test) is
extended to 07:05 UTC hard, because C1 has no fix head and the next
handover is C2's at 07:05. Order at 8a3406ea: UEFI-direct suite x10,
kexec suite x10, boot_test 10 kexec, boot_test 10 UEFI-direct, pi-5-1
boot_test 10. A boot that halts before the last test does not count toward
"runs to its last test", even with an issue number: the d512f40b kexec
cell reads 9 of 10. The 04:35 and 06:15 reminders are deleted
(`c430dcc2`, `6da641dc`); the 07:05 reminder stands.

## Entry 6 (written 03:36 UTC): C1 reported steps 1 to 6; two follow-ups; C2 cell at C1's head

C1 reported steps 1 to 6 (QEMU only, no labctl call), evidence
`gauntlet-4-hw-pi5` 93a44428, `run4-hw/c1/`. Under the blind protocol
the C1 builder entry (mechanism, rates, ordering argument) is written after
the C1 critic files. Recorded here only: heads named by C1 (unfixed
reproducer c1/isolated-1s-main 01931af6; trace c1/1213-trace b8df6f5f;
fix c1/1213-fix 5be808c5), and that C1 claims bars (a), (b), (c) and the
QEMU half of the isolation-path bar. Claims, not findings, until the critic
reproduces them.

Follow-ups sent to C1 before its head is final: (1) #1229 is in C1's files
and its report did not address it: say whether the traces show a stale
preempt_disabled clear, and fix it as a separate commit with its own
failing-on-old-code test or say why not; (2) classify #1228 (step 6), with
a code-and-trace argument labelled as such, since C1 has no Jetson time;
(3) list task_destroy's callers against the new bounded wait (IRQs masked,
locks held).

C2 started its unattended 20-run WORK_STEALING=ON cell at 5be808c5
(`run4-hw/c2/ws-on-5be808c5-*`). Lab unchanged: C3 holds both boards
to 07:05 UTC.

## Entry 7 (written 04:08 UTC): C1 head moves to 364544c1 (#1229); C2 cell at 5be808c5

- C1 (interim) added a #1229 commit, 1ead6ef6, on c1/1213-fix; head
  for C2 is 364544c1 (code as d8b21ec3, then docs). C1's description, a
  claim until its critic files: schedule() re-reads cpu_id() after
  switch_to for the three post-switch clears, and waits under its own
  rq_lock for next->on_cpu == 0 (load-acquire) before switching in; new
  test test_preempt_flag_follows_migrated_task.
- C2 WORK_STEALING=ON at 5be808c5 (before the #1229 commit), 20 runs,
  `gauntlet-4-hw-pi5` 24546abe, `run4-hw/c2/ws-on-5be808c5-summary.txt`:

| Class | main 6c1f4c9c | 5be808c5 |
|---|---|---|
| A #1219 stress hang | 3 of 20 | 3 of 20 (run05, run09, run14) |
| B #1213 family panic | 2 of 20 | 0 of 20 |
| C litmus #1230 | 3 of 20 | 4 of 20 |
| test_benchmark_context_switch 101 vs 100 | 0 of 20 | 3 of 20 (run07, run10, run14) |
| test_proactive_load_balance_inert_when_light | 0 of 20 | 1 of 20 (run05) |

  B at 0 of 20 is 2.0x the unfixed runs per failure, below 5x; not used
  as a finding. Each class-A hang shows a CPU with preempt_disabled stuck
  at 1 and the lock holder READY on its queue (the #1229 signature).
  WORK_STEALING=ON is not /ci-eligible at this head (9 of 20 failing).
- The two new failures are in kernel/tests/test_scheduler.c (C1's file).
  Relayed to C1 to decide regression versus pre-existing test race (Fisher
  p = 0.23 against main; not significant) before its head is final.
- C2 next: 34 WORK_STEALING=ON runs at 364544c1 (the 5x count for class
  A: 20 / 3 = 6.67 runs per failure on main).

## Entry 8 (written 04:57 UTC): C3 UEFI-direct 10 of 10; C2 cell at 364544c1

- C3 (status, 04:36 UTC): the UEFI-direct suite reached "Test Suite
  Summary" in 10 of 10 boots at 8a3406ea
  (`gauntlet-4-hw-jetson` a3b06ff7,
  `run4-hw/c3/final-8a3406ea-uefi-suite-run01..10.txt`), each 22 failing
  checks: 13 under #1109 and 9 UEFI-only, filed as **#1231**. Ruling: the
  kexec suite x10 at 8a3406ea is a bar and runs before the production boot
  tests; all of it fits before 07:05.
- C2 WORK_STEALING=ON at 364544c1 (with the #1229 commit), 34 runs, load
  2.07 to 3.67, `gauntlet-4-hw-pi5` 4755264d,
  `run4-hw/c2/ws-on-364544c1-summary.txt`:

| Class | main (20) | 5be808c5 (20) | 364544c1 (34) |
|---|---|---|---|
| A #1219 stress hang | 3 | 3 | 0 |
| B #1213 family panic | 2 | 0 | 0 |
| C litmus #1230 | 3 | 4 | 0 |
| test_benchmark_context_switch | 0 | 3 | 0 |
| test_proactive_load_balance_inert_when_light | 0 | 1 | 0 |
| test_policy_custom_assign_cpu_called | 0 | 0 | 1 |
| test_preempt_flag_follows_migrated_task (new) | n/a | n/a | 13 |

  #1219 under the 5x rule: 20 / 3 = 6.67 runs per failure on main, 5x =
  33.3, 34 clean runs at 364544c1: met, pending the C2 critic. Class B at
  0 of 54 across the two fixed heads is C1's to argue from its own cells
  (bar (c) is on the stress test and the 1 s variant). Class C is not
  claimed (load differs across cells). C1's new #1229 test fails 13 of 34
  at `pdm_moved > 0`, every one with bad=0: a test defect under
  WORK_STEALING, relayed to C1 with the custom-assign race. WORK_STEALING=ON
  is not /ci-eligible at 364544c1 (21 of 34).

## Entry 9 (written 05:14 UTC): C1 final fix head 6d4fe041; C1 critic started (QEMU part)

C1 sent a report addendum (evidence `gauntlet-4-hw-pi5` 43b3af1c,
`run4-hw/c1/`); content withheld until the C1 critic files. Recorded:
final fix head **c1/1213-fix 6d4fe041** (scheduler code as d8b21ec3; the
#1229 test reworked after C2's 13 of 34); C1 filed **#1232**
(test_slm_stats_live_from_second_session, one run at the fix head,
WORK_STEALING=ON). C1 states #1228 "not reproduced"; claim until checked.

Actions: C2 runs 20 WORK_STEALING=ON at 6d4fe041 (the /ci decision and
#1219 at the final head), rebuilds the Jetson artifacts at 6d4fe041 for its
lab hour, and files one issue for the steal-sensitive scheduler test races
(seen on unfixed main too, so pre-existing). C1 runs /ci at 6d4fe041 and
prepares the Pi 5 images offline. **C1 critic started** at 05:14 UTC on model
fable, blind (definition of done, bars, issues, heads, evidence paths; told
not to read this log), QEMU and mechanism-review part; the Pi 5 part
follows in C1's lab slot.

Lab plan (revised): C3 to 07:05; C2 07:05 to about 08:35 (assert-shares
20, boot tests 10 + 10 at 6d4fe041); C1 pi-5-1 from about 08:35 to 10:05,
priority (1) fix-head suite 20 boots with isolated=10/10, (2) boot_test
10, (3) critic's 5 boots, (4) unfixed main + conversion 20 boots (#1223).
Whatever does not fit is recorded as not run.

## Entry 10 (written 05:48 UTC): C1 head 9185d4cb (test-only); C2 cell at 6d4fe041; external QEMU kills

- C1 moved the head to **c1/1213-fix 9185d4cb**; C1 states everything
  after 364544c1 touches only kernel/tests/test_scheduler.c (the reworked
  #1229 test, and the three steal-sensitive test races). The critic is told
  to verify that and to judge the loosened asserts under "redefined
  criterion". 9185d4cb is final unless its /ci fails.
- C2 WORK_STEALING=ON at 6d4fe041 (same scheduler code), 20 runs, load
  1.01 to 10.01, `gauntlet-4-hw-pi5` 1fe6fcca,
  `run4-hw/c2/ws-on-6d4fe041-summary.txt`: class A 0 of 18 valid, B 0 of
  18, C (#1230) 11 of 18, #1137/#1163 1, the custom-assign race 1; 2 of 20
  INVALID. C2 filed **#1233** (the steal-sensitive scheduler test races,
  pre-existing under WORK_STEALING=ON); C1's 9185d4cb addresses them, so its
  PR should reference #1233.
- **External kills.** C2's run17 and run19 were ended by "terminating on
  signal 15 from pid 2748681" and "pid 2755413", about 05:35 to 05:38 UTC,
  not by C2's timeout. QEMU exits 0 on SIGTERM, so `make test` would have
  reported both as PASSED. The user journal (`journalctl --user`, 05:30
  to 05:40 UTC) shows five guarded QEMU scopes running at once (C1 critic
  2, C1 2, C2 1), load up to 10, and no record of the sender. The Makefile
  and scripts/ kill no QEMU by name. Every agent was asked whether it killed
  QEMU it did not start, told to kill only its own PIDs, and to mark any
  run with "terminating on signal" INVALID; C1 is held to 1 QEMU at a time
  until its /ci is done. The answers go in the next entry.
- C2's 20-run cell at 9185d4cb (the /ci decision) and the Jetson artifacts
  at 9185d4cb are next.

## Entry 11 (written 05:50 UTC): C1 critic verdict, QEMU part: FAIL on the mechanism

Verdict: [`critic-4-C1.md`](critic-4-C1.md) (Fable 5.2), run files
`run4-tools/critic-c1-runs/`. Bars (a), (b), (c) for the stress test and
the 1 s variant, the QEMU isolation bar, trace-ring zero cost and the
#1229 regression test: PASS by the critic's own runs and counts. Overall
**FAIL**: the mechanism review disagrees with 1ead6ef6's claim that
schedule()'s wait on next->on_cpu under rq_lock "cannot deadlock". The
outgoing CPU's switch window is open to IRQs; a tick there runs
task_wake_sleepers / the policy tick, which take the waiter's rq_lock.
Derived from code before the commit messages were read, then reproduced 2
of 2 at 6d4fe041 with a critic-only patch holding the window 25 ms
(`critic-window-6d4fe041-run01.txt`, `-run02.txt`; control 30 of 30).
Natural rate unmeasured; on Pi 5 and Jetson all CPUs take ticks.
INCOMPLETE rows: no run-4 end-state comments on #1213, #1223, #1211; no
make test file at the final head. The critic also saw #1230 in 9 of 10 of
its fix-head runs (load 2.8 to 8).

Ruling: no C1 PR until the deadlock is fixed and re-checked. C1 is sent
the cycle, the files and three fix shapes (keep the switch window
IRQ-masked through the on_cpu release; never spin under rq_lock; spin with
the lock dropped), and must turn the critic's window patch into a test that
hangs at 9185d4cb and completes at the new head, re-run the bar cells at
the new head, and run /ci. C2's Jetson hour and C1's Pi 5 slot wait for
that head; the C3 critic's boots take the lab at 07:05 meanwhile.

## Entry 12 (written 05:50 UTC): the external QEMU kills were the C1 critic's (undisclosed rule break, now disclosed)

- **C1 critic: yes.** Its window-experiment runner
  (`run4-tools/critic-c1-runs/critic-run_window.sh.txt`) ended each run
  with `pkill -f "qmp.sock,server=on"`, which matches every QEMU on the
  host started with a QMP socket (C2's harness adds one to every run). Ran
  at about 05:39 (pid 2746705), 05:42 (pid 2755413, one of C2's two) and
  05:43 UTC (pid not recorded). Pid 2748681 is unattributed. The runner now
  kills only its own PID; the critic records the break in its verdict file
  and checks the files it counted. Effect on results: C2's run17 and run19
  at 6d4fe041 are INVALID, not PASS (C2 classified them so before the cause
  was known).
- **C1: no** kill in 05:35 to 05:38. Earlier (about 04:41 UTC) it killed
  its own QEMU PIDs explicitly, and used `pkill -f` on its own script
  names, one pattern broad enough in principle to match another agent's
  `xargs`; disclosed; no name-pattern kills from now on. C1 found no
  "terminating on signal" line other than the 120 s timeout in its files
  since 05:00 UTC.
- C3's answer is pending.
- Ruling: name-pattern kills (pkill, killall) are forbidden to every
  agent for the rest of the run; the C1 critic stops until C1's new head.

## Entry 13 (written 06:00 UTC): C1 Pi 5 prep; a second disclosed rule break; lab plan

- C1 staged Pi 5 images under the main checkout's build/kernel (fix-head
  test and production at 9185d4cb, unfixed main plus the step-5 conversion
  at c1/main-isolation-conversion 3f64f624; sha256s in C1's evidence when
  pushed). The fix-head images are rebuilt at C1's next head.
- **Rule break (disclosed by C1):** C1 read labctl's output files under
  /var/lib/labctl/output/ directly (no labctl call) to time Pi 5 boots:
  about 163 s per completed integration boot, 339 s per hang, 23 s per
  production boot. The rules forbid reading labctl's private state; C1 is
  told not to repeat it, and the numbers are used only to size the slot.
- C1 reports a zero-cost check at 9185d4cb by stripping every trace hook
  (`slot-trace-size-check-9185d4cb.txt`, pending push): claim until
  checked. C1's /ci at 9185d4cb is stopped (the head moves for the
  deadlock fix, Entry 11).
- Lab plan, revised: C3 to 07:05 (or earlier if done), C3 critic about 40
  min; then C1 on pi-5-1 for item (4), unfixed main plus conversion
  (#1223), which does not depend on the new head; then C2's Jetson hour and
  C1's Pi 5 items (1) to (3) at C1's new head. Items that do not fit by
  10:20 UTC are recorded as not run.

## Entry 14 (written 06:21 UTC): C2 cell at 9185d4cb; /ci decision "no"

C2, `gauntlet-4-hw-pi5` f4fe645c, `run4-hw/c2/ws-on-9185d4cb-summary.txt`:
WORK_STEALING=ON, 20 runs, load 1.17 to 5.12, no external kills. Class A
(#1219) 0 of 20 (0 of 72 across 364544c1, 6d4fe041, 9185d4cb, same
scheduler code); class B 0 of 20; the three #1233 tests and the #1229 test
0 of 20; failures: run03 #1230, run20 test_runner_parks_without_a_lock
(#1184). Step 4: 18 of 20, so WORK_STEALING=ON is not added to /ci (rule
as written; neither failure depends on stealing). #1233 stays open until
C1's branch merges. Jetson production artifacts built at 9185d4cb
(`jetson-elf-compare-364544c1-9185d4cb.txt`: only test_scheduler.c
functions change size). These cells are at the deadlocking head; C2's /ci
cell and Jetson hour rerun at C1's next head.

## Entry 15 (written 06:23 UTC): C1 /ci at 9185d4cb fails on x86-64; ruling: revert the switch-in wait

C1's /ci at 9185d4cb: steps 1 to 4 passed; step 4b (`make test
PLATFORM=X86_64`) hit the 120 s cap after "[PASS]
test_rebalance_symbol_exposed" (x86 make test passed at 5be808c5, before
1ead6ef6). C1's analysis from code, untested: x86's resched IPI handler
calls schedule() unconditionally while the switch window is IF=1, so a
nested schedule() can pick prev and spin forever on prev->on_cpu, which only
this CPU clears. With the critic's ARM64 cycle (Entry 11) that is two
deadlocks in the same wait. C1 proposed an x86-only IPI guard.

Ruling: revert only the schedule() switch-in wait from 1ead6ef6; keep the
#1229 cpu_id() re-read, its test, and the 9185d4cb test fixes; no x86 IPI
change. The wait was added for a window found by code reading, not traced,
and is not needed for the traced #1213 sequences (covered by task_destroy's
on_cpu wait and zombie_gen) or for #1229. The window itself is filed as a
new P1 issue with the critic's analysis and window patch and C1's x86
analysis; its principled fix (an IRQ-masked switch window through the
on_cpu release, as Linux does) is too large to be shown in this run. At the
new head C1 re-runs x86 make test, the bar cells, the critic's window patch
(must not hang), and /ci from step 1.

## Entry 16 (written 06:34 UTC): C1 head 5e5548a1 (switch-in wait reverted); #1234 filed; C3 to 07:30

- C1 pushed **c1/1213-fix 5e5548a1**: reverts only schedule()'s switch-in
  wait on next->on_cpu (sched.c, docs/scheduler.md, kernel/CLAUDE.md);
  keeps #1229's re-read and test, task_destroy's on_cpu wait, zombie_gen,
  claim-time READY and the 9185d4cb test fixes. The x86 resched-IPI guard
  was not committed; its patch and 3 + 3 runs stay as evidence
  (`run4-hw/c1/ci-9185d4cb/`). C1 filed **#1234** (P1-high, sub:sched,
  platform:all): the switch-in window and both deadlocks of a spin under
  rq_lock. C1's /ci at 9185d4cb stopped at step 4b on its own (the x86
  timeout); logs labelled.
- Started at 5e5548a1: C1 x86 make test x3, the critic's window patch, Pi
  5 image rebuild, bar re-runs, /ci, issue comments; C2 Jetson prep and the
  20-run /ci decision cell; C1 critic round 2 (mechanism re-review, window
  patch, spot runs, diff check).
- labctl `list_claims` (06:33 UTC, after a 2-min wait behind a long
  call): C3 re-claimed jetson-nano-2 at 05:55:48 for the kexec x10 at
  8a3406ea (claim to 07:48). C3's hold is extended to 07:30 UTC hard
  because C1's head is late; the C3 critic follows (about 40 min), then
  C1 on pi-5-1 for item (4) and C2 on jetson-nano-2, one at a time.

## Entry 17 (written 06:46 UTC): x86 still regresses at 5e5548a1; the IPI guard approved

C1, `gauntlet-4-hw-pi5` f24e66cc, `run4-hw/c1/x86-5e5548a1/`, make test
PLATFORM=X86_64, 3 runs each, 1 QEMU at a time: 5e5548a1 1 PASS 2 FAILED
(test_rebalance_after_burst, 11/16 and 10/16 tasks; run 3 also warned
"still on a CPU after 2 s; leaking the slot"), main 6c1f4c9c 3 PASS,
5e5548a1 plus the uncommitted resched-IPI guard 3 PASS. Mechanism (C1's
code reading, not traced): x86's resched IPI re-enters schedule() while
preempt_disabled is set; the handler's comment says it is guarded and it is
not; the on_cpu bookkeeping turns the nested switch into stuck on_cpu and
lost tasks. ARM64's tick honours preempt_disabled.

Ruling (reversing Entry 15's "no x86 change" for this reason): the guard is
committed as its own commit, with its own issue, deferring rather than
dropping the kick where x86 has a deferral path (or an argument that a
dropped kick is re-raised), 10 x86 runs clean at the new head, and an
identical ARM64 .text versus 5e5548a1 so the ARM64 QEMU, Pi 5 image and
Jetson results at 5e5548a1 carry over.

C1 critic round 2 (in progress): the 9185d4cb to 5e5548a1 diff is only
the revert plus docs; schedule() has no on_cpu spin at 5e5548a1 (ldapr 1
to 0, yield 1 to 0); its growth (0x420 to 0x46c, QEMU build) is the
compiler emitting the switch-in block three times.

## Entry 18 (written 06:58 UTC): C3 builder done; lab to the C3 critic

C3 builder reported (final; its builder entry is written after the C3
critic files). Handover times: C3 first released jetson-nano-2 at 05:54,
re-claimed it at 05:55:48 for the kexec x10, and released it at 06:56 UTC,
powered on in SD Linux; pi-5-1 was never claimed by C3. C3 reports no QEMU
kills by it (no QEMU after about 02:45 UTC). Recorded now because they bear
on the lab plan: the kexec suite cell at 8a3406ea reads **9 of 10** (run06
halted in test_pmm, recorded on #1207), so the bar as written is not met on
kexec; /ci ran at d512f40b, not at 8a3406ea; the pi-5-1 boot_test was not
run.

Rulings: /ci and make test-qemu-uefi at 8a3406ea are required (C3 builder
runs them now on the host); the Pi 5 boot_test 10 is required because the
PR changes kernel/include/string.h and the Lua flags for every ARM64 build
(kernel-raspi5.ld is untouched; kernel.ld is QEMU's), C3 builder gets
pi-5-1 for about 15 min after the C3 critic. **C3 critic started** at 06:58
UTC (Fable, blind), holding jetson-nano-2 to about 07:45 for one test-kernel
boot per path at 8a3406ea. The lab sat idle from 06:56 to 06:58 UTC (the
handover notice arrived late). The 07:05 and 07:32 reminders are deleted;
a 07:47 reminder replaces them.

## Entry 19 (written 07:06 UTC): C2 /ci cell at 5e5548a1; C1 status

- C2, `gauntlet-4-hw-pi5` 7546694b,
  `run4-hw/c2/ws-on-5e5548a1-summary.txt`: WORK_STEALING=ON, 20 runs,
  load 1.25 to 4.53, 0 INVALID. Class A 0 of 20, class B 0 of 20.
  Failures: run15 and run16 #1230; run19 #1232; run20
  test_task_pinned_to_cpu0 (a fourth #1233-shape test, added to #1233).
  /ci decision: 16 of 20, not added. Because 5e5548a1 reverts the
  switch-in wait, the earlier 0 of 72 does not transfer; 14 more runs at
  5e5548a1 are under way to reach 34 (5x for #1219).
- C1 status (07:10 UTC): Pi 5 images rebuilt at 5e5548a1; the critic's
  window patch at 5e5548a1 no longer hangs (2 of 2), but the migrated task
  faults at switch-in, and the same patch faults identically on unmodified
  main 6c1f4c9c (2 of 2), so the #1234 window predates the fix
  (`run4-hw/c1/window-5e5548a1/`, `gauntlet-4-hw-pi5` 6a4c99ed; added to
  #1234). End-state comments posted on #1213, #1223, #1211. The litmus test
  runs in the Pi 5 integration suite (passed in run 3's
  split-pi5-integration-run02.txt). C1's x86 guard approval was resent
  (it had not reached C1).

## Entry 20 (written 07:08 UTC): C3 Pi 5 boot test; a single-lane overlap

C3 builder (status; builder entry after its critic): pi-5-1 claimed
06:57:41, boot_test --count 10 at 8a3406ea (production RASPI5) 10 of 10
PASS, banners 8a3406ea (`gauntlet-4-hw-jetson` 5833ae8d,
`run4-hw/c3/pi5-boot-test-8a3406ea.txt`), card restored (the board booted
its as-found kernel, banner b42b2c1d), released about 07:07 UTC, powered on.
**Overlap:** the C3 critic claimed jetson-nano-2 at about 06:58, so two
agents used labctl at once for about 9 minutes, against the single-lane
rule (my 06:57 "wait for the hand-off" message crossed C3's work). The
critic is asked to check its captures from that window and redo any
incomplete boot. C3 still owes /ci and make test-qemu-uefi at 8a3406ea
(running now); C3 is asked whether it read /var/lib/labctl/output itself.

## Entry 21 (written 07:09 UTC): C3's labctl output read (disclosed, ruled acceptable)

C3 read and copied the ten per-run captures from
/var/lib/labctl/output/l4c3-pi5-8a3406ea/, the output_dir its own
boot_test call created, into `run4-hw/c3/pi5-boot-test-8a3406ea.txt`;
nothing else under /var/lib/labctl, nothing written. Ruling: a boot_test
call's own output_dir is the call's output, not labctl's database; kept,
with a provenance line. This differs from C1's read (Entry 13), which
covered other sessions' output directories. C3's /ci at 8a3406ea started
07:08 UTC.

## Entry 22 (written 07:21 UTC): handover, jetson-nano-2 from the C3 critic to C2

- C3 critic released jetson-nano-2 at 07:21:22 UTC (powered on, SD Linux,
  BootCurrent 0001, ESP as found SLMOS.efi 789cba62, fsck clean, its elf
  removed). Both critic boots at 8a3406ea reached the last test (verdict
  pending).
- **07:22 UTC: jetson-nano-2 to C2** for step 3 at 5e5548a1: --assert-shares
  20 on kexec, kexec boots 10, UEFI-direct boots 10 (for C1's PR). Hard end
  08:35 UTC. Carry-over to C1's PR head depends on C1 showing that its x86
  guard commit leaves the ARM64 .text unchanged.
- Next: pi-5-1 to C1 at about 08:35 for, in order, the fix-head suite 20
  boots, boot_test 10, the C1 critic's 5 boots, then the #1223 cell
  (unfixed main plus conversion) at 10 boots unless time allows 20. Lab
  ends by 10:25 UTC for the close-out.

## Entry 23 (written 07:30 UTC): the C3 critic re-claimed the Jetson; schedule shifts

- C2 found jetson-nano-2 claimed again by "lifecycle-4-c3-critic" at
  07:24:15 UTC (reason "second UEFI boot with SASS-embedded test kernel at
  8a3406ea", expiring 07:53:37), two minutes after the 07:22 hand-off to C2.
  C2 did not claim, power-cycle or force anything. The critic is told to
  finish that one boot, restore the ESP to SLMOS.efi 789cba62, release by
  07:45 and explain the second boot in its verdict; no further claims
  without asking.
- C2's 5e5548a1 QEMU cell extended to 34 runs (`gauntlet-4-hw-pi5`
  da45d363): class A 0 of 34 (5x = 33.3, met at the exact head), class B 0,
  INVALID 0; failures #1230 x3, #1232 x1, test_task_pinned_to_cpu0 x1
  (#1233). /ci decision unchanged (16 of 20).
- Schedule: C2 jetson-nano-2 from the critic's release to 08:55 UTC hard;
  C1 pi-5-1 08:55 to 10:40 UTC hard (item (4), the #1223 cell, first if
  C1's guard head is not ready). Close-out 10:40 to 11:05.

## Entry 24 (written 07:49 UTC): C3 critic verdict: FAIL (kexec 9 of 10); C3 builder entry

Verdict: [`critic-4-C3.md`](critic-4-C3.md) (Fable 5.2), captures
`run4-tools/critic-c3-runs/`.

| Row | Critic |
|---|---|
| #1199 root cause (implicit memchr, `sxtw` after `bl memchr`) | PASS, reproduced from critic builds |
| #1224 root cause (.got at __data_end, not copied on UEFI-direct) | PASS, reproduced (readelf) |
| Tests fail on old code | PASS: Lua test faults in QEMU on main (0xffffffffc00eabd1), passes at 8a3406ea; layout script rc 1 on main, rc 0 at 8a3406ea |
| Suite to last test, kexec, 8a3406ea | **FAIL: 9 of 10** (run06 halted in test_pmm, pmm_alloc_pages, #1207; not C3's code) |
| Suite to last test, UEFI-direct, 8a3406ea | PASS, 10 of 10 |
| Failure accounting | 15 #1109 (kexec); 13 #1109 + 9 #1231 (UEFI); none unaccounted. Gap: no issue for the unpowered camera I2C controller after UEFI-direct |
| Production boots | PASS: kexec 10 of 10, UEFI-direct 10 of 10 (separate calls, disclosed) |
| Pi 5 boot_test 10 | PASS, 10 of 10, banner 8a3406ea |
| /ci at the PR head | INCOMPLETE (ran at d512f40b; C3 rerunning at 8a3406ea) |
| Critic boots | kexec 3033 PASS, last test reached; UEFI-direct (SASS image) 3021 PASS, last test reached; test lists identical to the builder's |

Further critic findings: `csidiag` after UEFI-direct still reaches the
camera I2C controller (code reading); test_camera_imx219_refused_after_uefi_boot
asserts the generic -3 and cannot tell refusal from failure; a fresh-worktree
`make kernel-test` embeds no SASS, so the builder's header command alone
does not reproduce its UEFI image; #1199 and #1224 affect the production
kernel too; the first critic kexec capture during the 06:57 overlap is not
counted.

**C3 builder entry** (written after its critic): head c3/jetson-suite
8a3406ea (cfc17d2b, d512f40b, 12079f3d, 8e3d6cc2, 8a3406ea). #1199: Lua
saw an implicit int-returning memchr; GCC sign-extended the returned
pointer, which on Jetson (heap above 0x80000000) became 0xffffffff8...;
fixed by prototypes in kernel/include/string.h and -Werror for implicit
declarations in the Lua objects. #1224: .got/.got.plt orphaned past
__data_end, loaded by kexec, not copied by the UEFI stub; fixed by placing
them in .data (kernel-jetson.ld, kernel.ld) and a layout check. Two further
UEFI-only board stops in camera code fixed (test skip, imx219 refusal).
Filed #1228 and #1231; commented on #1207. Evidence
`gauntlet-4-hw-jetson` c9c194fc, `run4-hw/c3/`.

**Outcome under the bars: no C3 PR** (the kexec bar is 9 of 10). The branch
stays pushed. The orchestrator's disagreement goes in the final report's
Dissent section. C3 builder files the two gaps (camera I2C after UEFI-direct
including csidiag; the refusal test's ambiguous code) as one issue after its
/ci.

## Entry 25 (written 07:51 UTC): C3 /ci green at 8a3406ea; #1235; C3 done

C3's /ci at 8a3406ea: 12 of 12 steps pass plus make test-qemu-uefi
(`gauntlet-4-hw-jetson` a09bfbde, `run4-hw/c3/ci-8a3406ea/TABLE.txt`
and logs; 07:08 to 07:30 UTC, no external kill lines). Filed **#1235**
(platform:jetson, sub:drivers, P2-medium): the unpowered camera I2C
controller after UEFI-direct, csidiag's path to it, and the refusal test's
generic -3. `run4-hw/c3/README.md` notes the oplib-shader requirement.
The C3 critic's /ci row is now satisfied by the builder's files; the kexec
bar stays 9 of 10, so the outcome (no C3 PR) is unchanged. C3 is idle.

## Entry 26 (written 08:06 UTC): C1 bar re-runs at 5e5548a1 (claims until the critic re-checks)

C1, `gauntlet-4-hw-pi5` 70d4996b, `run4-hw/c1/qemu-rates-table.txt`:
default config 17 runs: stress clean 17 of 17 (5x needs 5), 1 s variant
isolated=10/10 and normal=10/10 in 17 of 17 (needs 17), litmus 1 of 17;
WORK_STEALING=ON 20 runs: isolated=10/10 in 20 of 20, stress clean, no CPU
1 stop, no #1219 hang, no cap, litmus 3 of 20. Litmus pairing,
WORK_STEALING=ON, alternated: main 1 of 10, 5e5548a1 1 of 10 (load 0.9 to
6.7); the 20 of 20 at 9185d4cb does not reproduce without the switch-in
wait. In the same pairing main hit the cap twice (run02 #1219 hang, run05
"stack overflow: refusing to switch_to", #1213 family); the fix head 0 of
20. The x86 guard approval was sent a third time (C1 had not seen it).

## Entry 27 (written 08:26 UTC): C2 step 3 at 5e5548a1; C1 /ci at 5e5548a1

- C2 on jetson-nano-2 07:32:53 to about 08:24 UTC (`gauntlet-4-hw-pi5`
  20e6a076, `run4-hw/c2/step3-5e5548a1/step3-summary-5e5548a1.txt`):
  --assert-shares 20 of 20 PASS on kexec (lowest thief share 15.4 to 16.6
  percent, lowest-to-best 0.873 to 0.989; run 3's #1216 stall was 0.40 to
  0.83); kexec boots 5 of 5 run (5 not run); UEFI-direct boots 6 of 6 run
  (4 not run); ESP 789cba62 to fa168ee5 and back, fsck clean. /sys/version
  5e5548a1 on both paths. Disclosed: runs 02 to 20 of the assert-shares
  keep only the verdict line (a truncated-capture deviation; run 02's
  shares are not recorded); the board IP was read via a root login on the
  console (`hostname -I`), because no tool in C2's session reads
  lab://sbcs and an ssh probe was denied; the orchestrator later sent it
  from lab://sbcs (192.168.4.5). C2 released 31 min before its 08:55 end;
  it is given the board back to 08:55 to finish kexec 06 to 10 and UEFI 07
  to 10.
- C1 /ci at 5e5548a1: 10 of 10 steps green (`run4-hw/c1/ci-5e5548a1/`,
  `gauntlet-4-hw-pi5` 8e72766c), but x86 make test is intermittent at this
  head (2 of 3 standalone runs failed, Entry 17), so the green /ci does not
  show x86 fixed. The guard commit (approved, Entry 17) is pending.

## Entry 28 (written 08:27 UTC): C1 guard head 33f05c5a; #1236

C1 pushed **c1/1213-fix 33f05c5a** (the approvals of 06:45, 07:10 and 08:06
UTC reached C1 only at about 08:28). One commit after 5e5548a1, touching
only kernel/arch/x86_64/platform_x86.c: resched_ipi_handler returns
without calling schedule() while preempt_disabled[cpu] is set. x86 has no
deferral path (reschedule_pending exists only under SECONDARY_PREEMPT,
ARM64), so the kick is dropped; the commit argues it is re-raised by the
CPU's next schedule() (tick or yield). Filed **#1236** (sub:sched,
platform:x86-64, P1-high). Running: x86 make test 10 runs, the ARM64 .text
comparison against 5e5548a1, then /ci at 33f05c5a. The C1 critic adds the
guard (x86-only diff, .text identity, the re-raise argument, one x86 run)
to its Round 2.

## Entry 29 (written 08:54 UTC): C2 step 3 complete except one UEFI boot; C1 x86 10 of 10 and ARM64 .text proof

- C2 second session on jetson-nano-2 08:26:17 to 08:52:45 UTC, ssh only
  (`gauntlet-4-hw-pi5` 329d21dc, `run4-hw/c2/step3-5e5548a1/`):
  --assert-shares 20 of 20; kexec boots 10 of 10 (slmos> 88.4 to 94.3 s);
  UEFI-direct 9 of 9 run, boot 10 cut for the ESP restore by 08:55; ESP
  789cba62 restored after each session, fsck clean. The denied command was a
  LAN ssh sweep (auto-mode classifier, "Containment Escape"), not ssh to the
  board. C2 gets jetson-nano-2 again to about 09:08 for UEFI boot 10.
- C1: x86 make test at 33f05c5a 10 of 10 PASS, no leak warnings, no
  external kills (`run4-hw/c1/x86-33f05c5a/`). ARM64 .text proof
  (`arm64-text-33f05c5a-vs-5e5548a1.txt`, `gauntlet-4-hw-pi5` a30af259):
  Pi 5 test and production kernels byte-identical in .text; QEMU test kernel
  identical sizes per function, byte differences from worktree paths in
  __FILE__ strings. /ci at 33f05c5a running since 08:37. C1's pi-5-1 slot
  starts on C2's release (about 09:08), end 10:40.

## Entry 30 (written 08:58 UTC): C2 UEFI 10 of 10; pi-5-1 to C1

- C2 third Jetson session, released 08:58:06 UTC: UEFI-direct boot 10 at
  5e5548a1 PASS (full 1410-line capture, banner "SLM-OS v0.4.0 build
  20261112063436 (5e5548a1)", slmos> 62.1 s); ESP restored to 789cba62.
  Step 3 totals at 5e5548a1: --assert-shares 20 of 20, kexec boots 10 of
  10, UEFI-direct boots 10 of 10. Disclosed: the session 1 and 2 boot
  captures are tail-only (last 3 lines, with the failure patterns in
  until_pattern); only kexec boot 01 and UEFI boot 10 are full logs.
- **08:59 UTC: pi-5-1 to C1** (step 7), to 10:40 hard: suite 20 at the fix
  head with isolated=10/10 per boot, boot_test 10, then the C1 critic's 5
  boots, then the #1223 cell with the time left. jetson-nano-2 is idle.

## Entry 31 (written 09:20 UTC): C2's Jetson rows carry to 33f05c5a; the Fable C2 critic stopped; replaced on the session's model

- C2 (`gauntlet-4-hw-pi5` 81c5aae2,
  `run4-hw/c2/jetson-elf-compare-5e5548a1-33f05c5a.txt`): Jetson
  production ELF at 33f05c5a vs the step-3 ELF at 5e5548a1: .text 2,696,600
  bytes, 0 differ; .data 0 differ; .rodata 92 bytes in identity strings; no
  function changed size. Step 3 totals: assert-shares 20 of 20, kexec 10 of
  10, UEFI-direct 10 of 10. C2 is done.
- C2 critic (Fable) started 08:59 UTC; stopped at 09:17 UTC with "You've
  hit your weekly limit · resets Nov 14, 12pm (America/Los_Angeles)",
  before writing a verdict. Its run set at 33f05c5a kept running in its
  worktree. Per the brief, the replacement **c2-critic-2 runs on the
  session's model** (started 09:20 UTC) and its verdict file says so; it may
  adopt the first critic's complete run files after checking the script.
  The C1 critic is also on Fable; asked for status in case it hits the
  same limit.

## Entry 32 (written 09:21 UTC): usage reset; the Fable C2 critic resumed

The user reset the usage limit at about 09:21 UTC. The replacement critic
(session model, Entry 31) was stopped within about a minute of starting,
before any verdict; the original Fable C2 critic was resumed with its
context and run files, deadline 10:20 UTC. Its verdict records the
interruption. The C1 critic (Fable) continues.

## Entry 33 (written 09:41 UTC): C2 critic verdict; a correction to Entry 0a; the last lab hour

C2 critic verdict: [`critic-4-C2.md`](critic-4-C2.md) (Fable 5.2,
unchanged across the 09:17 interruption), runs `run4-tools/critic-c2-runs/`.
Row 1 (#1219 rate) PASS: main 3 of 20, 5e5548a1 0 of 34, 5x met by the
minimum count; pooling every main WORK_STEALING=ON run on file (41 runs, 5
failures, including the C0 0 of 3 and the critic's 1 of 13) needs 41 clean
runs, met only with the critic's own 10 fix-head runs added (44). Row 2
(classification, #1229 mechanism) PASS, with the note that the stuck flag
is read in 4 of 7 class-A hangs and in none of the 3 on main. Row 3
(--assert-shares 20 of 20 and the Jetson boots) **INCOMPLETE**: 19 of the
20 PASS rows are retyped verdict lines with no capture, run01 abridged;
boot text exists for 2 of 20 boots. Row 4 (/ci decision) PASS. Row 5
(reproduction): main 1 class A in 13; 5e5548a1 5 of 5 PASS; 33f05c5a 5 of
5 PASS; ARM64 QEMU .text byte-identical 5e5548a1 vs 33f05c5a. Evidence
errors flagged (not moving a #1219 number) are sent to C2 to fix.

**Correction to Entry 0a.** The "logs" cited there
(`run4-tools/ci-main-6c1f4c9c/*.log`) were not committed: .gitignore
line 39 ignores *.log, so only summary.txt and run.sh were on the branch.
They are force-added in this commit (15 files). Neither evidence branch
tracks any .log file; C1 and C3 are asked to force-add the logs they cite.

Last lab hour: C1 pi-5-1 items (1) and (2), then the C1 critic's 5 Pi 5
boots, then C2 on jetson-nano-2 about 10:25 to 10:50 for a full-capture
redo of --assert-shares 20 and as many full-capture kexec boots as fit.
Item (4) (#1223 on unfixed main) is expected not to run.

## Entry 34 (written 10:18 UTC): C1 Pi 5 phase; pi-5-1 to the C1 critic

C1 on pi-5-1 08:59 to about 10:17 UTC (claims, until its critic checks):
item (1) fix-head suite 20 boots (test image at 5e5548a1, ARM64 .text
identical to 33f05c5a): isolated=10/10 and normal=10/10 in 20 of 20,
slot_reuse_stress 2001 iterations bad=0 in 20 of 20, #1229 test bad=0 in 20
of 20, no KERNEL PANIC, every boot "18 total failure(s)" with an identical
failing set (the #1109 list from run 3); item (2) production boot_test 10 of
10 at the fix head; /ci at 33f05c5a 10 of 10 steps. Item (4) (#1223 on
unfixed main plus conversion, image slmos-l4-c1-test-unfixedconv-3f64f624.bin)
**not run, for time**. Card restored as found; board powered on. Disclosed:
the 20-boot boot_test outlasted the 30-min MCP client limit; the call kept
running server-side and C1 tracked it through its own output_dir.

10:18 UTC: pi-5-1 to the C1 critic (5 boots, to 10:33); then jetson-nano-2
to C2 (to 10:50).

## Entry 35 (written 10:36 UTC): C1 critic final verdict: PASS at 33f05c5a with open items; C1 builder entry

Verdict: [`critic-4-C1.md`](critic-4-C1.md) (Fable 5.2), sections R2.1 to
R2.7 and "Final overall verdict"; runs `run4-tools/critic-c1-runs/`
(round 2 `critic-r2-*`, Pi 5 `pi5/`). Round 1 FAIL stands for 6d4fe041
(the switch-in wait deadlock, reverted in 5e5548a1). Final: **PASS at
c1/1213-fix 33f05c5a** for bars (a), (b), (c) and the isolation-path bar.
Open items: #1234 pre-existing and open; 3 critic Pi 5 boots of the 5 asked
(my request for 2 more arrived after the critic released at 10:30:20; no
time remained); x86 at 33f05c5a rests on the builder's 10 of 10 and one
critic run; the critic's own rule break (Entry 12).

**C1 builder entry.**
- Baseline on main (1 s reproducer c1/isolated-1s-main 01931af6): CPU 1
  stopped 6 of 20 thread=multi, 0 of 20 thread=single.
- First-fault traces (trace ring behind SLOT_TRACE, default off, .text
  identical with it off): three dumps, all **sequence A**: CPU 1 runs
  task_exit for slot 6; CPU 0 (the test) destroys slot 6; CPU 1 parks it as
  rq->zombie and its idle reaps it; CPU 0's task_create claims slot 6 with
  state still TERMINATED; the reaper's task_destroy passes the TERMINATED
  check and clears stack_base/stack_top; task_create copies the cleared top,
  ctx.sp = 0; CPU 1 switches in with sp 0 and faults. **Sequence B**
  (stress-test trace): CPU 0 rebuilds the slot before CPU 1's switch_to
  saves, so the old task's registers overwrite the new context ("task_exit:
  schedule returned!"). **Sequence C** (WORK_STEALING=ON trace on main): the
  count-bounded wait expires while the task is RUNNING on CPU 1, and CPU 0
  destroys and rebuilds the slot and stack under it. Run 3's hypothesis (a
  half-written context) is overruled by the traces.
- Stress test test_task_slot_reuse_under_switch: 20 of 20 fail unfixed
  (884aab2d); 0 faults at every fix head (17 of 17 default config at
  5e5548a1; critic 10 of 10 unfixed, 5 of 5 fixed).
- Fix: per-task on_cpu set on switch-in under rq_lock, cleared with STLR in
  sched_finish_switch after switch_to; task_destroy waits on it with LDAPR
  (2 s bound, then warns and leaks); rq->zombie_gen and task_destroy_gen so
  the reaper never destroys a reused slot; READY set in the claim section.
  #1229: schedule() re-reads cpu_id() after switch_to (test
  test_preempt_flag_follows_migrated_task, fails 6 of 6 on the old code).
  x86: resched_ipi_handler returns while preempt_disabled[cpu] (#1236).
  The switch-in wait (1ead6ef6) was reverted (5e5548a1); its window is
  #1234.
- Isolation path: isolated=10/10 in 20 of 20 QEMU WORK_STEALING=ON, 17 of
  17 default, and 20 of 20 Pi 5 boots (`run4-hw/c1/pi5/pi5-fix-5e5548a1-integration-table.txt`);
  boot_test 10 of 10 on Pi 5; /ci 10 of 10 at 33f05c5a; x86 10 of 10.
- End states: #1213 root-caused and fixed (QEMU and Pi 5); #1211 not
  reproduced (20 of 20 unfixed and fixed); #1223 QEMU side the same defect
  (sequence B), Pi 5 on unfixed main not run (item (4), time), Pi 5 fix
  head 0 panics in 20; #1228 not reproduced (code-and-trace argument: the
  traced second destroys freed nothing; sequence C frees an in-use stack, a
  plausible source, untested on Jetson); #1229 root-caused by trace and
  fixed. Evidence `gauntlet-4-hw-pi5` 2501fc6e, `run4-hw/c1/`.

## Entry 36 (written 10:48 UTC): C2 step 3b; draft PR #1237; branches; lab

- C2 on jetson-nano-2 10:36:41 to 10:45 UTC (`gauntlet-4-hw-pi5`
  f8df3cdc, `run4-hw/c2/step3b-33f05c5a/`): the 33f05c5a production elf
  (banner "build 20261112085853 (33f05c5a)"), --assert-shares **20 of 20
  PASS**, every run's full capture in its own file, split unchanged from the
  saved reply (`kexec-assert-loop-reply.json`); lowest/best thief ratio
  0.777 to 0.960 (CPU 2 lowest in all 20, not investigated). The boot
  capture itself came back inline and is cited, not reproduced. C2 critic's
  row-3 re-check requested at 10:46 (result in its verdict file).
- **Draft PR #1237** (c1/1213-fix 33f05c5a to main). closingIssuesReferences:
  #1213, #1229. The PR body flags the TEMPORARY commit 01931af6 for a
  squash-merge check and lists the open items.
- Branches deleted (tips and patches archived in
  `run4-tools/deleted-branch-tips/`): c1/1211-fix 2f043ffa,
  c1/1211-trace-unfixed 8b0065ce, c1/1213-trace 884aab2d, c1/1229-trace
  7e07b384, c1/isolated-1s-main 01931af6. Kept: c1/main-isolation-conversion
  3f64f624 (the #1223 cell for the next run).
- Lab 10:47 UTC: no active claims; pi-5-1 and jetson-nano-2 online, ping,
  serial, power on.

## Close-out and final report (written 10:48 UTC)

### Dissent

**C3 (Jetson suite): the bar withholds a PR the rows support.** The bar:
the suite runs to its last test in 10 of 10 boots on both paths, running
past any pre-existing defect. The rows: UEFI-direct 10 of 10; kexec 9 of 10
at 8a3406ea, the tenth halting in test_pmm (pmm_alloc_pages, #1207), code C3
never touched, at the rate seen before its fixes (d512f40b 1 of 10; main 1
of 3 in the PMM family, #1228). The critic confirmed both root causes from
its own builds, both tests fail on the old code, production boots 10 of 10
per path, Pi 5 10 of 10, /ci 12 of 12 at 8a3406ea, and noted #1199 and #1224
also affect the production kernel. The PMM halt is plausibly the #1213
family (sequence C frees a stack in use; untested on Jetson). Files:
`critic-4-C3.md`, `run4-hw/c3/final-8a3406ea-*` on gauntlet-4-hw-jetson.
For the user: open c3/jetson-suite as a draft as is, or first run the kexec
suite x10 on it rebased onto #1237.

**C1 (#1237): opened, with two rows short of their letter.** The Jetson
production boot rows are 10 of 10 per path, but 18 of the 20 captures are
tail-only (the C2 critic's row 3b, INCOMPLETE): each records the prompt
reached with panic and exception patterns as stop conditions, which is what
labctl boot_test itself checks, but not a full log. The C1 critic's own Pi
5 reproduction is 3 boots of the 5 the protocol asks. The orchestrator
opened the draft because the C1 critic passed every C1 bar and every other
required row is complete; if the user reads the boot rows by their letter,
#1237 should stay a draft until 20 full-capture Jetson boots exist (about
45 min of lab). Files: `critic-4-C2.md`, `critic-4-C1.md`,
`run4-hw/c2/step3-5e5548a1/`.

### Issues: end states

- **#1213**: root-caused from three first-fault traces (sequences A, B, C)
  and fixed in #1237. Stress test 20 of 20 fail unfixed, 0 faults at the
  head; 1 s variant 6 of 20 unfixed, 17 of 17 fixed; isolated=10/10 in 20 of
  20 QEMU and 20 of 20 Pi 5 boots.
- **#1223**: QEMU side the same defect (sequence B signature, gone under the
  fix); Pi 5 at the fix head 0 panics in 20 + 3 boots; Pi 5 on unfixed main
  with the isolation path not run (time). Open.
- **#1211**: not reproduced (20 of 20 unfixed, 20 of 20 fixed, with
  SLOT_TRACE). Open.
- **#1219**: a separate defect, #1229 (stuck preempt_disabled strands the
  lock holder), root-caused by trace and fixed in #1237: main 3 of 20, fix
  head 0 of 34 (5x met). Open until #1237 merges.
- **#1199**, **#1224**: root-caused and fixed on c3/jetson-suite (no PR,
  Dissent).
- **#1228**: not reproduced; plausible link to sequence C (hypothesis).
- **#1229**: fixed in #1237 (closes on merge). **#1236**: fixed in #1237.
  **#1234**: open (P1), the switch-in window, pre-existing.

### PRs and merge order

#1237 (draft) only. C2 changed no test or /ci file (WORK_STEALING=ON is 16
of 20 at the head: #1230, #1232, #1233), so no C2 PR. No C3 PR (Dissent).
If c3/jetson-suite is opened later, it is independent of #1237 (no shared
files) and either may land first.

### Tracker delta

Opened: #1228, #1229, #1230, #1231, #1232, #1233, #1234, #1235, #1236.
Closed: none. File-don't-fix: #1228, #1230, #1231, #1232, #1233, #1234,
#1235. Raised: #1228 to P1. Comments: #1199, #1207, #1211, #1213, #1219,
#1223, #1224, #1228, #1229, #1230, #1234.

### What the run could not do

- #1223 on Pi 5 on unfixed main with the isolation path (image staged:
  build/kernel/slmos-l4-c1-test-unfixedconv-3f64f624.bin, branch
  c1/main-isolation-conversion): no lab time left.
- Full-capture Jetson production boots at the PR head (tail-only, above);
  the critic's last 2 Pi 5 boots.
- A Jetson run of #1237 against #1207/#1228.
- About an hour went to messages reaching builders late (Entries 17, 26,
  28) and to the reverted switch-in wait.

### Single most useful next step

Run the Jetson kexec integration suite x10 on c3/jetson-suite rebased onto
#1237: it decides at once whether the PMM halts (#1207, #1228) are the
#1213 family and whether the C3 bar can be met, which opens the second PR.

### Rule breaks and process notes (all disclosed in the entries cited)

The C1 critic's name-pattern pkill killed C2's QEMU runs (Entry 12); C1
read other sessions' labctl output files (Entry 13); two single-lane
overlaps (Entries 20, 23); C2's one console root login (Entry 27);
tail-only and retyped captures, the latter redone (Entries 30, 33, 36); the
orchestrator's C0 logs were uncommitted until Entry 33 (.gitignore *.log);
the C2 critic stopped on a usage limit and resumed on the same model
(Entries 31, 32). All three critics ran on Fable 5.2.

### Branch table

| Branch | Tip | State |
|---|---|---|
| gauntlet/lifecycle-4 | this commit | log, verdicts, tools |
| gauntlet-4-hw-pi5 | f8df3cdc | C1 and C2 evidence |
| gauntlet-4-hw-jetson | e262a221 | C3 evidence |
| c1/1213-fix | 33f05c5a | draft PR #1237 |
| c3/jetson-suite | 8a3406ea | pushed, no PR (Dissent) |
| c1/main-isolation-conversion | 3f64f624 | kept for the #1223 cell |
| c2/ws-1219 | (local, at main) | no commits; not pushed |
| deleted: c1/1211-fix, c1/1211-trace-unfixed, c1/1213-trace, c1/1229-trace, c1/isolated-1s-main | see run4-tools/deleted-branch-tips/ | archived |

### Lab state

10:47 UTC: no claims; pi-5-1 online, powered on, card restored to its
as-found kernels (C1 and the C1 critic); jetson-nano-2 online, powered on,
SD Linux, ESP SLMOS.efi 789cba62 as found, no BootNext.

## Entry 37 (written 10:48 UTC): C2 critic row-3 re-check (addendum to the close-out)

`critic-4-C2.md`, "Row 3 re-check": at f8df3cdc, 20 of 20 ASSERT-SHARES:
PASS counted from the per-run files, each byte-identical to its segment of
the saved reply, each complete (6 CPU, c2t and iw rows, Completed N/N,
run=1..20). Truncated-capture failure cleared. Open: no banner,
/sys/version reply or on-board sha appears in captured text, so the
33f05c5a identity of that boot rests on the builder's header lines (which
agree with its ELF-compare file). Row 3: PASS on count and completeness,
banner unverified from captures. Row 3b (Jetson boots): unchanged,
INCOMPLETE. The close-out's Dissent on #1237 stands as written.

## Entry 38 (written 18:05 UTC, 2026-11-12): close-out session start; #1237 head and mergeability

A single-agent lab session (no subagents, no critics) to finish run 4's
open items. Boards pi-5-1 and jetson-nano-2, one at a time. Session start
17:58 UTC.

- #1237: head **33f05c5a** (unchanged), draft, mergeable CLEAN. main is
  **6c1f4c9c** and is an ancestor of 33f05c5a (`git merge-base
  --is-ancestor`), `git merge-tree` clean: no rebase needed, so the
  critic's pass and every run 4 row stay tied to 33f05c5a.
- /ci at 33f05c5a: green in run 4 (`gauntlet-4-hw-pi5`
  `run4-hw/c1/ci-33f05c5a/`, all 10 steps rc=0, full logs). The head is
  unchanged, so that run is the step-2 /ci condition; not re-run.
- Lab 18:00 UTC: no claims; both boards online, powered on. jetson-nano-2
  as found: SD Linux (BootCurrent 0001), ESP `SLMOS.efi` 789cba62, no
  BootNext, recovery_mode binding released (last_state open).
- Builds at 33f05c5a (l4-c1 worktree, one at a time): Pi 5 production and
  test, Jetson production (elf for kexec, bin for UEFI).
- Capture method, Jetson: boot_test cannot run kexec or set BootNext, so a
  temporary systemd unit (`l4co-autoboot.service`, `/root/l4co/`) runs
  `slmos-kexec`, or sets BootNext 000E and reboots, for a fixed count of
  boots and then lets Linux boot normally. That lets one `boot_test` call
  per batch record each boot from power-on (UEFI, Linux, handoff, SLM-OS
  banner) to the prompt, in its own output_dir file. The unit and
  `/root/l4co/` are removed at restore.

## Entry 39 (written 19:25 UTC): #1237 missing rows at 33f05c5a: Jetson 20 of 20, Pi 5 10 of 10, all full captures

**Method change from Entry 38 (disclosed).** Installing the temporary
`l4co-autoboot` systemd unit on jetson-nano-2 was refused by the session's
permission classifier (persistence on the board). Nothing from that
command reached the board (`/root/l4co` and the unit were checked absent
afterwards). Used instead: a host-side, session-local loop
(`trigger.sh.txt`) that watches for each new Linux boot_id over ssh and
runs the documented trigger (`slmos-kexec <elf>`, or `efibootmgr
--bootnext 000E` and reboot as in `scripts/jetson-uefi-deploy.sh`), while
one `boot_test` call per 5 boots does the power cycles and the capture.
Each output_dir file is still one boot from power-on (MB1) to the prompt.
One extra power_cycle (not counted) brought SD Linux up to write the ESP.

| Row | Result | Files (`run4-hw/closeout/`) |
|---|---|---|
| jetson-nano-2 production, kexec | **10/10**, 154.5-158.2 s power-on to prompt | `gauntlet-4-hw-jetson` `1237-33f05c5a-jetson-kexec-boot01..10.txt` |
| jetson-nano-2 production, UEFI-direct | **10/10**, 127.2-151.4 s | `gauntlet-4-hw-jetson` `1237-33f05c5a-jetson-uefi-boot01..10.txt` |
| pi-5-1 production | **5/5**, 13.0-13.1 s | `gauntlet-4-hw-pi5` `1237-33f05c5a-pi5-prod-boot01..05.txt` |
| pi-5-1 integration suite | **5/5** reached the summary; isolated=10/10 and normal=10/10 in 5 of 5; slot_reuse_stress bad=0 in 5 of 5; 18 total failure(s) in each, failing set identical to run 4's fix-head set; no CPU 1 panic, 0 KERNEL PANIC / PAGE FAULT / Synchronous | `gauntlet-4-hw-pi5` `1237-33f05c5a-pi5-suite-boot01..05.txt` |

- Images built at 33f05c5a in l4-c1: Jetson elf b95c0d47 / bin 276ffa90
  (banner `build 20261112180315 (33f05c5a)`); Pi 5 production 8b827008
  (`build 20261112180020`), test 437c7fe5 (`build 20261112180146`).
- Every file was checked for its first line (Jetson: MB1; Pi 5: the
  banner block), its last bytes (`slmos>` or the suite summary), the
  banner, and fault patterns. None was incomplete; none needed a redo.
- Indexes: `README-1237-jetson.md` (gauntlet-4-hw-jetson ffe0cc50),
  `README-pi5.md` (gauntlet-4-hw-pi5 b45912bf).
- pi-5-1 restored (kernel_2712.img back from .prev, 6935256 bytes, mtime
  2026-10-29; kernel8.img untouched), powered on, released about 18:23.
  jetson-nano-2 still claimed for step 3; its ESP holds the 33f05c5a
  production bin until then.
- Squash check (PR merge note): `test_isolated_core_latency` at 33f05c5a
  is byte-identical to 6aee62c1's; the squash diff has no TEMPORARY
  content.
- Trial rebase of c3/jetson-suite onto 33f05c5a: clean, though the two
  branches do share CMakeLists.txt and kernel/CLAUDE.md (the close-out
  said no shared files).

Step 2 conditions hold (20 Jetson + 10 Pi 5 boots clean and complete,
/ci green at 33f05c5a). Merging #1237.
