# Critic verdict, lifecycle run 4, component C2 (#1219, #1229, #1230, #1233)

Critic: `c2-critic`, blind. Model that wrote this verdict: **Fable 5.2**
(model ID `claude-fable-5-2`). Brief received 08:59 UTC 2026-11-12.

Order of work. The critic's own count of every QEMU cell was written to
`critic-c2-runs/own-count-before-summaries.txt` at 09:01 UTC (sha256
37b2c36ef9ab512f...) from the raw console text of the 168 run files,
before any `*-summary.txt` file, issue comment or step-3 summary was
opened. Lines 1 to 3 of each run file (the harness header with the
harness's own result word) were visible while counting; the critic's
classifier does not use that word. `lifecycle-4.md` was not opened. The
builder's summaries and the issues were read after that file was saved.

Interruption. The critic's session stopped at about 09:17 UTC on a usage
limit and resumed at about 09:21 UTC with the same model (Fable 5.2,
unchanged). The background run chain started before the stop kept
running; all its files were checked for completeness after the resume
(each has a suite summary and a semihosting exit line, section 2). A
replacement critic ran for under a minute at 09:20; the only trace found
is a stray `c2ev2/docs/` directory in the scratchpad, which was ignored.
No worktree `l4-c2-critic2` exists.

Evidence read: `origin/gauntlet-4-hw-pi5` @ 81c5aae2,
`docs/archive/lifecycle-gauntlet/run4-hw/c2/`. The local branch of that
name was three commits behind (f58b3ac7); the 168 QEMU run files are
identical in both, the step-3 files differ and the newer ones were used.
Critic files: `critic-c2-runs/` in the session scratchpad
(`classify.py`, `runset.sh`, `textcmp.sh`, one file per run). All paths
below are under `run4-hw/c2/` unless they start with `critic-c2-runs/`.

## Summary

| Definition-of-done row | Verdict |
|---|---|
| 1. #1219 rate on main and at the fix head, with files | **PASS** (3 of 20 on main, 0 of 34 at 5e5548a1; counts agree with the files) |
| 2. Classification in one sentence | **PASS** (names a mechanism; the separating cells ran) with two notes |
| 3. `--assert-shares` 20 of 20 at C1's fix head, or the failing rows | **INCOMPLETE** (fail-on-completeness under the protocol as written: 19 of 20 runs have no capture, only a retyped verdict line; 1 is cut and abridged) |
| 3b. Jetson boots 10 per path (a step, not a definition-of-done row) | **INCOMPLETE** (18 of 20 boots have no capture text in the files) |
| 4. /ci decision | **PASS** (16 of 20 in the decision cell, not added, `ci.md` unchanged) |
| 5. Critic reproduced 5 runs on each head | Done: main 5 (plus 8 extra), 5e5548a1 5, 33f05c5a 5 |

Overall: **fail-on-completeness**, on row 3 only
(`step3-5e5548a1/kexec-assert-runs.txt`). Rows 1, 2 and 4 pass on the
critic's own count. Whether the retyped step-3 rows are accepted is the
orchestrator's decision; the protocol as written treats a cut, filtered
or reassembled capture as missing.

## 1. Own count of the QEMU cells (rows 1 and 4)

Classifier (`critic-c2-runs/classify.py`), applied to the console text of
every file: banner hash; any `[FAIL] <test>` line; the
`batch: stress workload (4x8)` line (PASS, FAIL or absent);
`KERNEL PANIC`; every `terminating on signal N from pid P (name)` line;
the `[PASS] All test suites passed!` line; the harness exit code. A run
is PASS only with exit 0, the all-suites line, no FAIL line, no panic,
and the stress line printed as PASS. A kill whose sender is not
`/usr/bin/timeout` makes the run INVALID. Class A = stress line FAIL
(#1219); B = kernel panic; C = `test_litmus_patterns_pass` (#1230).

| Cell (files) | Banner and elf in every file | Files | Valid | PASS | A | B | C | Other failing tests | Stress line PASS | Load 1 min at start |
|---|---|---|---|---|---|---|---|---|---|---|
| `ws-main-6c1f4c9c-run{01..20}.txt` | 6c1f4c9c, 1a96044f | 20 | 20 | 13 | 3 (05, 08, 17) | 2 (02, 13) | 3 (03, 16, 17) | run02: `test_reposition_moves_task_queued_on_other_cpu`, `test_task_entry_unmasks_irq` (with B) | 15 (absent in 02, 13) | 2.74 to 5.67 |
| `ws-main-6c1f4c9c-trial-t{0..3}.txt` | 6c1f4c9c, 1a96044f | 4 | 4 | 3 | 1 (t3) | 0 | 1 (t3) | | 3 | 2.24 to 4.27 |
| `ws-off-main-6c1f4c9c-run{01..20}.txt` (WS=OFF) | 6c1f4c9c, 2afbbf05 | 20 | 20 | 11 | 0 | 0 | 9 | | 20 | 3.46 to 6.75 |
| `ws-on-5be808c5-run{01..20}.txt` | 5be808c5, 8e596404 | 20 | 20 | 11 | 3 (05, 09, 14) | 0 | 4 | `test_benchmark_context_switch` 3, `test_proactive_load_balance_inert_when_light` 1 | 17 | 1.00 to 6.06 |
| `ws-on-364544c1-run{01..34}.txt` | 364544c1, f4020666 | 34 | 34 | 21 | 0 | 0 | 0 | `test_preempt_flag_follows_migrated_task` 13, `test_policy_custom_assign_cpu_called` 1 | 34 | 2.07 to 3.67 |
| `ws-on-6d4fe041-run{01..20}.txt` | 6d4fe041, e9f5045b | 20 | 18 | 8 | 0 | 0 | 10 valid (+1 in INVALID run19) | `test_net_burst_8_sends_async` 1, `test_policy_custom_assign_cpu_called` 1 | 18 | 1.01 to 10.01 |
| `ws-on-9185d4cb-run{01..20}.txt` | 9185d4cb, 70826b52 | 20 | 20 | 18 | 0 | 0 | 1 | `test_runner_parks_without_a_lock` 1 | 20 | 1.17 to 5.12 |
| `ws-on-5e5548a1-run{01..34}.txt` | 5e5548a1, 694402d2 | 34 | 34 | 29 | 0 | 0 | 3 (15, 16, 26) | `test_slm_stats_live_from_second_session` (19), `test_task_pinned_to_cpu0` (20) | 34 | 0.88 to 4.53 |

All 168 run files are distinct (168 md5 sums). Every file of a cell
carries that cell's banner hash and one elf sha256. Every file has
`load_start` and `load_end` on line 1. Consecutive runs chain
(`load_end` of run N equals `load_start` of run N+1) in the main,
5be808c5 and 364544c1 cells; in the 5e5548a1 cell the chain breaks only
before run21, which is the disclosed extension from 20 to 34 runs on the
same elf (run06 to run07 differ by 0.04, two reads of `/proc/loadavg`).
No sign of a dropped run.

INVALID runs: `ws-on-6d4fe041-run17.txt` and `-run19.txt` end with
`terminating on signal 15 from pid 2748681 ()` and `... pid 2755413 ()`,
harness exit 0, no suite summary. Both are excluded from every pass
count in the builder's summary and in the table above. No other file in
any cell has a kill line that does not name `/usr/bin/timeout`. Note:
line 1 of both files still reads `RESULT result=PASS` and
`result=PASS+FAILLINES`; only the summary marks them INVALID.

Path exercised: every run counted PASS in every cell prints
`[PASS] batch: stress workload (4x8) completes + rejects bad args`. On
main the stress test was reached in 18 of the 20 baseline runs (run02 and
run13 panicked earlier), so the #1219 rate among runs that reached it is
3 of 18.

### Builder's numbers against the critic's count

| Builder's number (file) | Critic's count | Agree |
|---|---|---|
| Main: A 3 of 20, B 2 of 20, C 3 of 20, any failure 7 of 20, PASS 13 (`ws-main-6c1f4c9c-summary.txt`) | same | yes |
| Trial t3 A + C, t0 to t2 PASS | same | yes |
| WS=OFF main: A 0, B 0, C 9 of 20 | same | yes |
| 5be808c5: A 3, B 0, C 4, any 9 of 20 | same | yes |
| 364544c1: A 0 of 34, 13 of 34 failed | same | yes |
| 6d4fe041: A 0 of 18 valid, INVALID 2, PASS 8, "Class C: 11 of 18 valid runs" | C is 10 of 18 valid runs, 11 of 20 files (run19 is INVALID and has the litmus FAIL) | no, off by one in the prose; not a #1219 number |
| 9185d4cb: A 0, PASS 18 of 20 | same | yes |
| 5e5548a1: A 0 of 34, B 0, C 3, PASS 29 of 34; decision cell runs 01 to 20: 16 of 20 | same (first 20 files: 16 PASS; failures 15, 16, 19, 20) | yes |
| Load ranges per cell | same | yes |

One prose claim does not match its files: `ws-main-6c1f4c9c-summary.txt`
says of class B (run02 and run13) that both are
`panic("task_exit: schedule returned!")` and that "the panic text never
reached the UART". `ws-main-6c1f4c9c-run02.txt` line 6580 prints
`KERNEL PANIC` and `task_exit: schedule returned!` (task `lat_iso`, CPU
1). `ws-main-6c1f4c9c-run13.txt` line 6634 prints `KERNEL PANIC` with a
different text, `stack overflow: SP is outside current task's stack`
(current `rr_test`, next `idle_3`, CPU 3). The count (B 2 of 20, a panic
PC in 10 of 10 samples on one CPU) stands; the description of run13 as
the task_exit panic does not. Class B is C1's #1213 and no C2 row
depends on it.

### 5x rule (row 1)

Rule as written: fault-free runs at the fix head at least 5 times the
average runs per failure on the unfixed kernel.

| Unfixed baseline | Runs per failure | 5x | Fault-free runs at 5e5548a1 (builder, files) | Met |
|---|---|---|---|---|
| This run's main cell, 3 of 20 (`ws-main-6c1f4c9c-run*.txt`) | 6.67 | 33.3 | 34 | yes, by 0.7 of a run |
| Main cell, only the 18 runs that reached the stress test, 3 of 18 | 6.0 | 30 | 34 | yes |
| Main cell plus trial t0 to t3, 4 of 24 | 6.0 | 30 | 34 | yes |
| Main cell plus 5be808c5 (no #1229 fix), 6 of 40 | 6.67 | 33.3 | 34 | yes |
| Every main WS=ON run with a result on file: builder 20 + trials 4 + Makefile run 1 + orchestrator C0 3 (0 failures) + critic 13 (1 failures): 41 runs, 5 failures | 8.2 | 41 | 34 builder; 44 with the critic's 5 at 5e5548a1 and 5 at 33f05c5a | no with the builder's 34 alone; yes with the critic's 10 added (44) |

The bar is computed from one baseline cell of 20 runs and is met with
34 runs, the smallest count that can meet it. The orchestrator's C0
baseline was 0 failures in 3 runs (`run4-tools/ci-main-6c1f4c9c/summary.txt`
on `gauntlet/lifecycle-4`, rows `10-ws-on-1..3 rc=0`; the
`10-ws-on-*.log` files named in the brief are not in that tree, only
`run.sh` and `summary.txt`). Three runs at a 15 % rate pass together
61 % of the time, so 0 of 3 does not contradict 3 of 20.

Head: the 34 runs are at 5e5548a1, the PR head is 33f05c5a. `git diff
--stat 5e5548a1 33f05c5a` is `kernel/arch/x86_64/platform_x86.c` only.
The critic built the QEMU_VIRT test kernel (WORK_STEALING=ON) at both
heads and compared sections with objcopy
(`critic-c2-runs/textcmp-5e5548a1-33f05c5a.txt`): `.text` 2512120 bytes,
0 differing (sha256 prefix 821582a636b5022f both); `.data` 147456 bytes,
0 differing; `.rodata` 88 differing bytes of 5033962 (the commit and
build-stamp strings). The identity claim holds for the QEMU test image.
The same compare against main (6c1f4c9c) differs in 2237996 `.text`
bytes, so the tool does detect a difference.

## 2. Reproduction (row 5)

Own worktree `.claude/worktrees/l4-c2-critic`, clean tree at each head,
`make kernel-test-clean`, then `make test WORK_STEALING=ON` per run
(CMakeCache `ENABLE_WORK_STEALING:BOOL=ON`). One QEMU of the critic's at
a time. Guard and threading checked on the critic's own running QEMU
(pid 3312450, found by its working directory): cgroup scope with
`memory.max=3221225472` and `cpu.max=200000 100000` (MemoryMax 3G,
CPUQuota 200 %), 7 host threads for 4 vCPUs, no `-accel` flag on the
command line, QEMU 8.2.2, so MTTCG (thread=multi), the Makefile default.
The command line is the builder's `tools/v2/run_one.sh.txt` command
without its `-qmp` and `-gdb` sockets. `make test` relinks the elf on
every run (new build stamp), so each critic run has its own elf sha256;
the source and `.text` are the head's.

| File (`critic-c2-runs/`) | Head (banner) | Load 1 min before | Other QEMU on host at start | Result | Class |
|---|---|---|---|---|---|
| `ws-on-6c1f4c9c-run01.txt` | 6c1f4c9c | 1.46 | 0 | FAIL | C (`test_litmus_patterns_pass`) |
| `ws-on-6c1f4c9c-run02.txt` | 6c1f4c9c | 1.35 | 0 | FAIL | other: `test_policy_custom_assign_cpu_called` (#1233) |
| `ws-on-6c1f4c9c-run03.txt` | 6c1f4c9c | 1.73 | 0 | PASS | |
| `ws-on-6c1f4c9c-run04.txt` | 6c1f4c9c | 1.56 | 0 | PASS | |
| `ws-on-6c1f4c9c-run05.txt` | 6c1f4c9c | 1.35 | 0 | PASS | |
| `ws-on-6c1f4c9c-run06.txt` (extra) | 6c1f4c9c | 2.70 | 0 | PASS | |
| `ws-on-6c1f4c9c-run07.txt` (extra) | 6c1f4c9c | 2.34 | 1 (not the critic's) | PASS | |
| `ws-on-6c1f4c9c-run08.txt` (extra) | 6c1f4c9c | 1.57 | 0 | PASS | |
| `ws-on-6c1f4c9c-run09.txt` (extra) | 6c1f4c9c | 1.91 | 0 | PASS | |
| `ws-on-6c1f4c9c-run10.txt` (extra) | 6c1f4c9c | 1.80 | 0 | PASS | |
| `ws-on-6c1f4c9c-run11.txt` (extra) | 6c1f4c9c | 1.95 | 0 | PASS | |
| `ws-on-6c1f4c9c-run12.txt` (extra) | 6c1f4c9c | 1.71 | 0 | PASS | |
| `ws-on-6c1f4c9c-run13.txt` (extra) | 6c1f4c9c | 1.64 | 0 | FAIL, 120 s cap | A (#1219): `[FAIL] batch: stress workload (4x8)`, then `terminating on signal 15 ... (/usr/bin/timeout)` |
| `ws-on-5e5548a1-run01.txt` | 5e5548a1 | 1.21 | 0 | PASS | |
| `ws-on-5e5548a1-run02.txt` | 5e5548a1 | 1.72 | 0 | PASS | |
| `ws-on-5e5548a1-run03.txt` | 5e5548a1 | 1.76 | 0 | PASS | |
| `ws-on-5e5548a1-run04.txt` | 5e5548a1 | 1.53 | 0 | PASS | |
| `ws-on-5e5548a1-run05.txt` | 5e5548a1 | 1.31 | 0 | PASS | |
| `ws-on-33f05c5a-run01.txt` | 33f05c5a | 1.12 | 0 | PASS | |
| `ws-on-33f05c5a-run02.txt` | 33f05c5a | 1.66 | 0 | PASS | |
| `ws-on-33f05c5a-run03.txt` | 33f05c5a | 1.53 | 0 | PASS | |
| `ws-on-33f05c5a-run04.txt` | 33f05c5a | 1.83 | 1 (not the critic's) | PASS | |
| `ws-on-33f05c5a-run05.txt` | 33f05c5a | 2.95 | 1 (not the critic's) | PASS | |

Every PASS above printed the stress line as PASS; no file has a
`terminating on signal` line other than from `/usr/bin/timeout`.

Result. Fix head: 10 of 10 PASS (5 at 5e5548a1, 5 at 33f05c5a), class A
0 of 10, which agrees with the builder's 0 of 34. Main: the first 5 runs
gave 3 PASS, 1 class C and 1 other, with no class A (at 3 of 20 the
chance of no class A in 5 runs is 44 %). With time left the critic ran 8
more on main after the fix-head sets; run 13 is class A, the #1219
signature exactly (stress FAIL, then the 120 s cap), at load 1.64 with no
other QEMU on the host. Critic total on main: class A 1 of 13, class B 0
of 13, PASS 10 of 13. That is lower than, and not inconsistent with, the
builder's 3 of 20 (Fisher exact, two-sided, p above 0.6). The critic took
no PC samples, so the reproduction confirms the failure and its rate
band, not the mechanism.
The two main failures in the first 5 match classes the builder reports
on main or files under #1233 as present before the fix
(`test_policy_custom_assign_cpu_called` fails here on plain main, which
supports #1233's statement that those tests are not regressions from the
fix).

## 3. Classification against the samples (row 2)

Claim (`ws-main-6c1f4c9c-summary.txt`, repeated in
`ws-on-5e5548a1-summary.txt` and #1229): #1219 is not C1's #1213
mechanism; it is a lock holder stranded on a CPU whose
`preempt_disabled` flag is stuck at 1, because `schedule()` clears the
flag of the pre-switch CPU after a stolen task resumes elsewhere. That
is a mechanism, not "probably the same". Filed as #1229, not fixed by
C2 (kernel code, not test code), which is what the brief asks.

What the hang files show, checked by the critic:

| Item | Files | Found |
|---|---|---|
| All vCPUs sampled, 10 samples at each of two points | `ws-main-6c1f4c9c-run05.txt`, `-run08.txt`, `-run17.txt`, `-trial-t3.txt` | 40 `PCSAMPLE stressfail` and 40 `PCSAMPLE hang` lines in each (10 samples by 4 vCPUs, twice); cpu0 to cpu3 all present |
| Where the vCPUs are | same | Every sampled PC is 0x4023f794, 79c, 7ac or 7b0 (`run_inference_inner`) or 0x40010a80 (`exception_vectors`), on all 4 vCPUs at both points |
| What the stress test waits on | same | `STRESSVAR WORKERS_DONE = 0`, SUCCESS 0, ERROR 0 at both dumps |
| Lock holder | run17, t3 (and 5be808c5 run05, run09, run14) | `ENGINE_LOCK byte = 1`; one infer-stress task READY with sw=1, frame chain `schedule+576 <- el1_irq+164`; t3 `TRAPFRAME slot=14 ... ELR conv2d+1000`, `RQ cpu2 ready_count=1 list=[slot14]` |
| Stuck flag | `-trial-t3.txt` lines 10025 and 11357; `ws-on-5be808c5-run05.txt`, `-run09.txt`, `-run14.txt` | t3: `preempt_disabled=[0, 0, 1, 0]` at 87 s and 100 s, `sched_diag_tick[2]` 7209 to 7941, `sched_diag_schedule[2]` 15244 both times, while CPUs 0, 1 and 3 advance by 723 to 736. 5be808c5: the flag is 1 on the CPU whose queue holds the READY holder (CPU 2, 3, 1) and that CPU's schedule count does not move between the two dumps |
| Source lines cited | `kernel/sched/sched.c` at 6c1f4c9c | 1868 (thief skips a victim with the flag set), 2314, 2464, 2530, 2534, 2722: each is the statement the summary says, within one line |

Notes on row 2:

1. The stuck flag was read in 4 of the 7 class A hangs (t3 and the three
   5be808c5 runs). In the main baseline cell itself it was read in 0 of
   3: run05 and run08 have no lock byte, run queue or flag lines, and
   run17 has the lock byte and frame walk but no flag. Those three show
   the same task-table shape (one READY worker with sw=1 saved in
   `schedule`, on the same CPU as a RUNNING worker with sw=2), which is
   consistent with the mechanism without showing the flag. The summary
   says which runs carry which dump, so this is disclosed.
2. The mechanism paragraph in `ws-main-6c1f4c9c-summary.txt` and in
   #1229 is still headed "Hypothesis (code reading, not yet tested)".
   The separating experiment did run afterwards and is in the later
   cells: 5be808c5 (C1's #1213 fix, no flag fix) A 3 of 20; 364544c1
   (adds 1ead6ef6) A 0 of 34; 5e5548a1 (on_cpu wait reverted, the
   `cpu_id()` re-read kept) A 0 of 34. Between 5be808c5 and 5e5548a1 the
   non-test change in `schedule()` is the re-read (`now_cpu = cpu_id()`
   before the clear, sched.c:2609 to 2617) plus trace hooks, so the
   comparison is close to one variable. #1229's body and #1219 have no
   comment with the fix-head rate yet; the confirmation exists only in
   `ws-on-5e5548a1-summary.txt`.

The 364544c1, 6d4fe041 and 9185d4cb cells contain the later reverted
on_cpu wait; the builder's pooled "0 of 52" and "0 of 72" mix those
kernels. The bar does not need them: it is met at 5e5548a1 alone.

## 4. Step 3 rows (row 3)

Files: `step3-5e5548a1/` at 81c5aae2.

| Row | Builder's result | What the files contain | Critic |
|---|---|---|---|
| `--assert-shares`, 20 runs, one kexec boot | 20 of 20 PASS | `kexec-assert-runs.txt`: 20 lines `runNN PASS ...`, typed by the builder; the literal text `ASSERT-SHARES: PASS` appears once, in `kexec-assert-run01.txt`. Run 01 is a capture with 182 lines elided, six `c2t` lines marked "(line abridged)" and the verdict line cut mid-line at the match. Runs 02 to 20 have no capture; run 02's line is "(cut at match)" with no shares | 20 PASS rows counted; 0 complete captures; INCOMPLETE |
| kexec boots | 10 of 10 | `kexec-boots.txt`: 10 lines `kbootNN PASS slmos> N s`, typed. Capture text exists for boot 01 only (`kexec-assert-boot.txt`, the last 41 lines to `slmos>`; no banner in the captured text) | 10 rows counted; 0 captures from kexec trigger to prompt; INCOMPLETE |
| UEFI boots | 10 of 10 | `uefi-boots.txt`: 10 lines, typed. `uefi-boot10-full.txt` is a full 1410-line capture with the banner `SLM-OS v0.4.0 build 20261112063436 (5e5548a1)` at line 1081, `Boot source: UEFI-direct (EFI stub)`, prompt at line 1413 | 10 rows counted; 1 complete capture; INCOMPLETE |
| ESP restore | restored in 3 sessions | `uefi-esp-asfound.txt` 789cba62..., 2196 clusters; `uefi-esp-restore.txt`: three blocks, each sha 789cba62..., `fsck.fat` 2196 of 130892 clusters with no error line, BootCurrent 0001, no BootNext line; staged elf removed (the session-3 block has no `ls /root/slmos-c2-*` line) | agrees |

Pass criterion: `s3_assert_shares()` at 5e5548a1
(`kernel/src/shell_sys.c:2392`) prints `ASSERT-SHARES: PASS` only when
every thief is at or above half the best thief; the criterion is the
kernel's, not redefined. In the 19 typed lines that carry shares the
lowest thief is 151 to 174 tasks against a best of 172 to 178, far above
half.

Truncated capture, as the protocol is written ("a cut, filtered or
reassembled capture is treated as missing"; "a `bench stealing` capture
has one line per CPU for every CPU"; "a boot log runs from power-on or
kexec trigger to prompt"):

- A tail-only boot capture (tail=3: pattern status, elapsed time, final
  prompt line) can show that the capture stopped on the pattern and
  when. It cannot show which kernel booted (no banner), which
  alternative of `slmos>|PANIC|panic|Unhandled Exception` matched unless
  the tail holds the prompt, or any warning or fault before the prompt.
  The tails themselves are not in the files either; only the builder's
  one-line transcription is.
- A verdict-only bench line can show the kernel's own PASS word and the
  lowest and best thief. It cannot show the per-CPU distribution,
  `Completed: N / N`, the `run=` counter (so that 20 distinct runs of
  one boot are shown), or the banner.
- The banner for the kexec row is a header claim ("/sys/version commit
  5e5548a1") with no captured `cat /sys/version` reply in any file. The
  kexec elf sha256 is shown staged and checked on the board
  (`kexec-assert-boot.txt`: want and got 2964ae29...), which supports
  the head but is not a banner.
- The builder's own procedure (`step3-procedure-5e5548a1.txt`) planned
  one file per run and per boot (`kexec-assert-runNN.txt`,
  `kexec-boot-runNN.txt`, `uefi-boot-runNN.txt`). The summary discloses
  that the full replies were not kept "so the bench time fit the window".

The disclosure is plain and nothing in the files suggests a failing run
was dropped, but the row cannot be counted from captures: 1 of 20 bench
runs and 2 of 20 boots have any capture text, and only 1 of those 3 is
whole.

Head: step 3 ran at 5e5548a1, the PR head is 33f05c5a.
`jetson-elf-compare-5e5548a1-33f05c5a.txt` reports the Jetson production
ELFs identical in `.text` (2696600 bytes) and `.data`, 92 differing
`.rodata` bytes in version strings. The critic did not rebuild the
Jetson images (one build at a time, time), but checked the same claim on
the QEMU image (section 1) and the source diff (one x86-only file not
compiled for ARM64). Accepted as carrying over.

Other step-3 points: the brief says `boot_test 10`; the builder ran a
manual power_cycle, kexec and serial_capture loop for the kexec path
(the procedure says boot_test cannot kexec) and BootNext for UEFI.
Disclosed rule breaks in `step3-summary-5e5548a1.txt`: a root login on
the labctl console to read the board IP in session 1, stopped at the
orchestrator's request; three ESP writes instead of one. No lab
hardware was touched by the critic.

## 5. /ci decision (row 4)

Rule: add WORK_STEALING=ON to /ci only if it passes 20 of 20 in QEMU at
C1's fix head. Decision cell: `ws-on-5e5548a1-run01..20.txt`, 16 of 20
PASS by the critic's count (run15 and run16 litmus #1230, run19
`test_slm_stats_live_from_second_session`, run20
`test_task_pinned_to_cpu0`). Decision: not added, with the reason
stated. That follows from the rule. The summary also gives "18 of 20
excluding #1230" as information and does not use it to change the
decision. `c2/ws-1219` is at 6c1f4c9c with no commits of its own, and
`.claude/commands/ci.md` has no diff against main on `c2/ws-1219` or on
`origin/c1/1213-fix`. The branch exists only locally (no
`origin/c2/ws-1219`).

## 6. Failure modes by name

| Failure mode | Checked how | Result |
|---|---|---|
| Selective summary | 168 files classified from console text and compared with every summary table; load chain between consecutive runs | No omitted failing run. One prose miscount (6d4fe041 C "11 of 18 valid", is 10) |
| Truncated capture | Section 4 | **Found** in step 3: 19 bench runs and 18 boots with no capture, run 01 abridged and cut. QEMU run files are whole (suite summary or a `/usr/bin/timeout` kill line in every valid file) |
| Assembled register file | QMP samples carry label, sample index and time per read; one file per run | Not found |
| Redefined criterion | /ci rule, 5x rule, assert-shares criterion read from source | Not found. "Excluding #1230" views are labelled as information |
| Hypothesis stated as finding | Section 3 | Mechanism was labelled a hypothesis when filed and then separated by the 5be808c5, 364544c1 and 5e5548a1 cells. Untested hypotheses for the other failing tests (#1230 host scheduling, steal races in #1233 tests, `test_task_pinned_to_cpu0`) are labelled untested. Class B prose for run13 does not match its file |
| Stale base | Banner hash in every one of the 168 files against the cell head; heads are ancestors of pushed 33f05c5a or are origin/main | Not found |
| Partial per-CPU table | PC samples counted per vCPU in each hang file; flag and tick arrays have 4 entries | Not found in QEMU. Step-3 run 01 has all 6 CPUs; runs 02 to 20 have no per-CPU rows (under truncated capture) |
| Number without a file | Each number traced | Step-3 timings, `/sys/version` replies and 19 share lines exist only as typed lines. The orchestrator's "0 of 3" has `summary.txt` but not the three logs named in the brief |
| Uncommitted tree | `tools/v2/run_cell.sh.txt` aborts on a dirty tree; the build appends `-dirty` to the banner hash (`cmake/gen_build_info.cmake:84`) and no banner has it | Not found |
| Banner hash mismatch | As stale base; step 3 has a captured banner in 1 of 20 boots | QEMU: none. Step 3: not checkable for 19 boots |
| Bar computed from one cell | Section 1, 5x table | The 5x bar rests on one 20-run baseline cell and is met by the minimum count (34 against 33.3). It holds under four of five baselines tried; under the pooled baseline (41 runs, 5 failures) it needs 41 and holds only with the critic's 10 fix-head runs added |
| Precondition from a prior run | Elf snapshot per cell, clean build per head | Not found. The 34-run cells were extended on the same elf, disclosed |
| Path never exercised | Stress line PASS required in every counted PASS | Exercised in every PASS. On main the stress test was not reached in run02 and run13 |
| Undisclosed rule break | Summaries read for disclosures; INVALID attribution | None found undisclosed. Disclosed: console root login, extra ESP writes, runs are a hand-run copy of the `make test` command with QMP and gdb sockets (line 1 of every file), one true `make test` on main with its QEMU output not kept (`ws-main-6c1f4c9c-makefile-run.txt`) |

## 7. Verdict

**fail-on-completeness**: definition-of-done row 3 (`--assert-shares`
20 of 20 at C1's fix head), file
`step3-5e5548a1/kexec-assert-runs.txt`: 19 of the 20 runs are retyped
verdict lines with no capture and the twentieth is abridged and cut. The
Jetson boot step has the same defect (`kexec-boots.txt`,
`uefi-boots.txt`).

Rows 1, 2 and 4 pass on the critic's own count and reproduction:
#1219 is 3 of 20 on main 6c1f4c9c and 0 of 34 at 5e5548a1 (0 of 10 more
in the critic's runs at 5e5548a1 and 33f05c5a, whose ARM64 `.text` is
byte-identical; 1 of 13 in the critic's runs on main); the classification names the stuck `preempt_disabled`
mechanism and is supported by four dumps and three separating cells; the
/ci step is correctly not added.

Open items for the orchestrator: the step-3 rows need captures (or a
ruling that the typed rows are accepted); #1229 and #1219 carry no
comment with the fix-head rate; the class B paragraph for run13 and the
"11 of 18" figure in the 6d4fe041 summary need correcting; line 1 of the
two INVALID run files still says PASS.

Housekeeping: the critic's worktree `.claude/worktrees/l4-c2-critic` was
removed at 09:39 UTC; no QEMU of the critic's is left running; no branch
was edited; only PIDs started by the critic's own `make test` ran, and
none was killed by the critic.
