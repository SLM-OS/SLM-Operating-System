VERDICT: FAIL — narrowly, on text only. No kernel defect, no test defect, every table number in the PR body and the #1262 comment reproduces from the raw captures, and all eight round-2 findings are resolved or carried plainly. It fails because one stated reason is wrong (in the PR body and in the comment already posted on #1284), one bold claim is contradicted by a figure two lines below it, and #1284's title and summary assert as fact what is true under only one of its two readings.

Scope: Part A (#1263), `fix/percpu-cacheline` f3c0d021, phase A2c and the texts. Stage 1 (blind) is `stage1-derivation.md`. J = `gauntlet-5-hw-jetson`, P = `gauntlet-5-hw-pi5`, both `docs/archive/switch-gauntlet/run5-hw/1263-1207/`. PR line numbers are those of `CLAIMS-draft-pr-body.md`.

## Findings

1. **MEDIUM — "far fewer cross-CPU resumes" is wrong.** PR line 180 and the #1284 comment (posted) say the 0 of 1400 padded rows do not bear on the `bad=1` because "in the padded UEFI-direct boots 88 to 92% of wake-ups were on one CPU, so those rows had far fewer cross-CPU resumes".
   Summing `moved=` over every `pdm:` line: padded 8707 cross-CPU resumes in 1400 iterations, unpadded 9071 (UEFI-direct: 3735 against 3963; per boot 1186 to 1306 against 1310 to 1331). The old test stops after 5 moves, so the count per passing iteration is equal by construction. What is lower is the share per wake-up (UEFI 0.15 to 0.21 against 0.62 to 0.71), because padded boots ran 21826 wake-ups against 5969.
   It matters for #1284's two readings: under the kernel-defect reading exposure follows cross-CPU resumes (about equal on both sides); under the test-race reading it follows wake-ups (3.7 times more on the padded side, with 0 bad). The true reason the padded rows decide nothing is that the observation is one event.
   Fix: replace the clause in both texts (a correcting comment on #1284) with the counts above and "one occurrence cannot be compared across rows".

2. **MEDIUM-LOW — "Stealing does not rise by multiples" is contradicted for Pi 5.** PR line 144 (bold), `docs/scheduler.md` (f3c0d021), f3c0d021's commit message. In the one Pi 5 boot that shows the regime, steals per wake-up are 0.64 against 0.32 to 0.36 unpadded: 1.8 to 2.0 times. The PR prints the 0.64 but its parenthesis points only at the two boots that steal less; `docs/scheduler.md` omits the 0.64 and quotes only "two of three padded Pi 5 boots stole less".
   Fix: "rises by at most about 2 times (Pi 5, the one boot in the regime: 0.64 against 0.32 to 0.36), 1.1 to 1.3 times on Jetson; not the 5 to 8 times of the raw totals"; add the Pi 5 figure to `docs/scheduler.md`.

3. **LOW-MEDIUM — #1284's title, summary and "What the assertion means" favour the kernel-defect reading.** Title: "a resumed worker saw preempt_disabled set on its CPU". Summary: "read a nonzero `preempt_disabled[]` for the CPU it was running on". "`(first on cpu 5)` is the CPU the worker was on when it read the flag." Under the test-race reading none of these holds: the worker read the flag of the CPU it had left. PR line 175 repeats "for the CPU it was on". "Bearing: this concerns the switch-window work in PR #1266" also presumes a kernel cause; if the race reading is right it concerns the test. The comment states both readings correctly, but the body and title do not.
   Fix: title and summary to "read `preempt_disabled[c]` nonzero, where `c` is the `cpu_id()` it had read just before, IRQs open"; Bearing to "occurred on the kernel without the padding; a #1266 kernel path or the #1266 test". Same wording in PR line 175. #1284 is P1-high and open: flag to the user.

4. **LOW-MEDIUM — two places still read stronger than the body.**
   - PR line 21: "Whether that costs latency or throughput was not measured beyond `--assert-shares`." Line 189 says correctly that `--assert-shares` does not measure where a sleeping task wakes. Read from code (`shell_sys.c` `s3_assert_shares`, `steal_shares_check`): tasks are created on CPU 1, run once, never sleep; the check is each thief at or above half the best thief. It measures nothing about the regime. Fix: "was not measured (`--assert-shares` passes but does not test it)".
   - The ledger row the user is asked to accept (`docs/perf/context-switch.md:50`) says "(placement only)" and "No difference is shown on either board" with no pointer to the observed change in where woken tasks run. For `bench context` the row is right; as the thing being accepted it should carry one clause pointing at `docs/scheduler.md` "Observed effect on where a woken task runs".

5. **LOW — the unpinned workers never woke on CPU 0, and the text says "any CPU".** Test header comment ("on any CPU"), c3ce3885 commit message, PR line 152. In all captures (Pi 5, UEFI, kexec, both suites, QEMU x200, `/ci`) the per-CPU histograms have 0 at CPU 0 for every worker, about 25,000 wake-ups; `pdm_c` excludes CPU 0 by construction. The flag check has never covered CPU 0. Also the header comment's "nearly every wake-up" is 88 to 95% per boot. Fix: say so in the comment's "cannot see" list and in PR line 157.

6. **LOW — archive file cited without saying it is cut.** PR line 142, "`c2-fix-critic/base-3b369aef-r2-boot02.txt` (3b369aef), 13 of 34 iterations": the capture ends at `[ITER 34/200]` with no summary line and no header; it is a kexec boot. Both archive boots also have `w >= 0` failures (1 and 2) beside the `pdm_moved > 0` ones. Fix: "capture ends at iteration 34 of 200; kexec".

7. **LOW — PR wording.**
   - Line 151 "Critic round 2 found": an internal process reference in a public PR (round 2 objected to "orchestrator" for the same reason).
   - Line 184 "plus `make test WORK_STEALING=ON SWO_DIAG=ON`": the step ran `make test SECONDARY_PREEMPT=ON WORK_STEALING=ON SWO_DIAG=ON` (`J/A-ci-c3ce3885-step12-*.txt:2`); the flag has no effect in QEMU (`test_preempt_state_one_line_per_cpu` is IGNOREd there), consistent with line 192, but the command should be quoted as run.
   - Line 180 "every other `pdm:` line of this session" has `bad=0`: except the mutation run, by design.
   - "Not measured" lacks: the reworked test on the unpadded kernel on hardware (all three A2c boots are padded, so the 76 to 98% one-CPU shares of the three-worker test have no unpadded counterpart).

8. **LOW — test, not blocking.**
   - `irq_save()` masks IRQ only. In a `PI5_FIQ_TIMER` build (option, default OFF) the tick is an FIQ and the pair read is not atomic; `schedule()` uses `sched_irq_save()` for that case. One comment line or the same helper.
   - `cpu_count < 4` returns through `TEST_ASSERT(1)`: a silent pass, not an IGNORE. Pre-existing.
   - 7881056d is labelled "Test only" (PR line 12) but adds `slot_trace_percpu_layout()` to `kernel/sched/slot_trace.c`, compiled only under `CONFIG_SLOT_TRACE` and `ENABLE_BOOT_TESTS`. Line 14 covers it; line 12 could say "test hook in `slot_trace.c`".

## Round-2 findings

| # | Status | Basis |
|---|---|---|
| 1 `bad=1`: no issue, attribution, comparison | Resolved, with findings 1 and 3 above | #1284 open, linked from #1266 (one comment there names it); both readings in the comment and PR 175-180; "orchestrator" gone |
| 2 test dropped the steal-exposed leg | Resolved | three workers, `pdm_a`/`pdm_b` unpinned; limits in the comment and PR 154-159; mutation run exists; CPUs 4-5 reached only through the unpinned workers (79 wake-ups on UEFI), stated in PR 171 |
| 3 four wrong statements | Resolved | "one passes, the other is ignored" (PR 184); `/ci` at d09b063b and c3ce3885 listed; `make test SLOT_TRACE=ON` run at committed c3ce3885, the test PASSes; steal ranges now 131 to 136 and 157 to 162 for exact 100/100 iterations: reproduced |
| 4 same failure on unpadded kernels before #1234 | Resolved, with finding 6 | PR 142, `docs/scheduler.md`; 88d4d71b 38 failed, 37 `moved=0`; 3b369aef 13 of 34: reproduced; both commits precede 04a0c904 |
| 5 behaviour change treated as test fragility | Resolved, with findings 2 and 4 | PR 21 and section "Behaviour change", "Not measured" line, `--assert-shares` on both kernels and both Jetson paths |
| 6 unmasked pair read | Resolved in code | `test_scheduler.c:2347-2350`; caveat in finding 8 |
| 7 evidence defects | Resolved or carried plainly | `A-ci.sh.txt` removes the log before each step; c3ce3885 steps 9 and 10 have no serial section; d09b063b's stale sections disclosed (PR 184); A2c slmstore records have read times; Pi 5 order "rests on headers" stated (PR 128) |
| 8 documents | Resolved | `spinlock.h` "Inferred, not measured"; `docs/scheduler.md` describes `layout_ok()`; PR line 1 tells the merger to use the body as the squash message |

## Answers

1. Yes, with findings 2 and 4. The PR opens "What changes" with the outcome change in bold, gives it its own section, and lists the cost as not measured. "Placement only" is true of the code and is now qualified in the PR; the ledger row is not. A reviewer would be misled on two points only: that stealing barely changed on Pi 5, and that `--assert-shares` is a partial measure of the cost.
2. Acceptable, and described accurately apart from "any CPU". Correctness: with IRQs masked in task context the task's own CPU flag is legitimately 0 (`schedule()` clears it before its unlock, `resched_trampoline` before `eret`), so nonzero is a defect; the mask does not hide #1229, since a flag left at 1 stops ticks from rescheduling and stays until that CPU's next `schedule()`. Each worker writes only its own 64-byte row. `(last % 3) + 1` never equals `last`; `moved == runs` for `pdm_c` in every iteration of every capture. Vacuous only below 4 CPUs. Not built on x86-64 (`CMakeLists.txt:1200`), before or after. Unpinned exposure is small and timing-dependent, and the PR says so.
3. Not quite: the comment is balanced, the title, summary and Bearing are not (finding 3), and the "far fewer" reason is wrong (finding 1). The PR handles it correctly otherwise: no closing keyword, both readings, "removes the second reading from the test but not the first".
4. "Closes #1263" (line 3) is the only closing keyword in the PR body; #1262, #1284, #1279, #1274, #1230, #1229, #1234 appear without one. The #1262 comment claims no fix and says twice the 2 s failure was not reproduced.
5. Nothing unsafe in the kernel. Non-test change since c0612fa7: `slot_trace_percpu_layout()` in `kernel/sched/slot_trace.c` and its prototype, both under `ENABLE_BOOT_TESTS`; `spinlock.h` comment only. Conditions unchanged: #1266 merges first; the ledger row awaits the user; the diag and mutation trees are not for merge.

## Matches (re-derived from raw captures)

- A2c table (PR 167-169), all cells: Pi 5 4392 / 414 (9.4%) / 26 and 27 / cpu3 91.1%; UEFI 4471 / 150 (3.4%) / 4 and 4 / 79 / cpu3 98.2%; kexec 5602 / 2526 (45.1%) / 61 and 61 / 1324 / cpu1 76.4%; 200 of 200 and `bad=0` in each; banners c3ce3885; bodies byte-identical to the labctl source files.
- Suites at c3ce3885: 21 (UEFI) and 14 (kexec) total failures, failing names identical to the 6a4cfa93 close-out sets, 0 halts, pdm PASS; UEFI suite `a=[0,0,0,7,7,0]`, 14 moves in 14.
- Mutation: one-line diff at `sched.c:2931`, the line 5b28d065 introduced for #1229; 20 of 20 fail at the `bad` assertion (line 2456); `bad == moved` for each worker (219, 218, 220); iteration 1 has bad reads on all three.
- `--assert-shares`: four files, 20 of 20 PASS each, AS-RC 0 x20; lowest thief share min / median / max 15.7 / 16.3 / 16.5 (kexec U), 14.8 / 16.1 / 16.6 (kexec P), 14.1 / 15.0 / 15.9 (UEFI U), 15.3 / 15.95 / 16.4 (UEFI P at c3ce3885); boot banners match the headers.
- QEMU: pdm3 x200 200 of 200; SLOT_TRACE run at c3ce3885 clean banner, `test_slot_trace_arrays_one_line_per_cpu` PASS; `/ci` c3ce3885 steps 1 to 12 exit 0 after the step 6 rerun; attempt 1 failed on `test_litmus_patterns_pass` only (#1230 is that flake, open); 308 and 328 runtime tests.
- A2b: 83 of 1400 against 0; 4 of 7 against 0 of 7; shares 88 to 95% and at most 56%; steals per wake-up 0.79 to 0.85 / 0.64 to 0.76 / 0.77 / 0.65 / 0.19, 0.64, 0.16 / 0.32 to 0.36; `bad=1` line and its numbers; 19-line print.
- R3 table: slow finishes, prelude counts, slowest iteration, 1000 PASS and 0 NOT-FINISHED for all 12 boots; 6701 us at `A-uefi-r3w-U-ae1399ba-x1000-boot01.txt:3349`.
- Pi 5 A2c slmstore: 9 entries and 4 hashes identical, read times on both sides of the boot (boot file written 02:18:47Z); restore-check banner b42b2c1d. Jetson A2c pre and post state: same ESP image hash, no BootNext, no session files left.

## Not checkable

- The cause of `bad=1`; which worker saw it.
- Which CPU steals in the one-CPU regime; the number of stolen resumes in A2c (the three-worker test prints no steal count).
- Detection of the #1229 mutation on hardware (QEMU only, stated).
- Kernel identity of the `--assert-shares` runs beyond the header's "boot =" pointer (the output files hold no banner).
- `files.lfs` and `files.lfs.delta` contents on the Pi 5 card (listed only; their mtimes never change).
- Pi 5 boot order in A2b; "4 of the 109 slow finishes have no prelude report", the 14 / 8 / 265 wait breakdown, the bench-context means, the 822 archived runs: checked in rounds 1 and 2, not re-derived here.
- Board end state (lab not touched).
