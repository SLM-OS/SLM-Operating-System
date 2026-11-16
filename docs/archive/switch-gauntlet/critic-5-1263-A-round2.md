VERDICT: FAIL — no kernel code defect and the A2b tables reproduce, but the PR body has wrong statements (a test reported as passed is IGNORED; `/ci` at d09b063b listed as not run; a steal range), the `bad=1` observation is carried without an issue and with an attribution the evidence does not support, the test change removes the only exposure to the path where `bad=1` was seen without saying so, and the archive holds the same coverage failure on unpadded kernels that the text does not mention.

Scope: Part A (#1263), `fix/percpu-cacheline` d09b063b, phase A2b, the answers to round 1. Stage 1 (blind) is `stage1-derivation.md`; this file is Stage 2. Paths: J = `gauntlet-5-hw-jetson`, P = `gauntlet-5-hw-pi5`, both `docs/archive/switch-gauntlet/run5-hw/1263-1207/`.

## Findings

1. **HIGH — `bad=1` on the unpadded kernel: no issue, unsupported attribution, and the comparison beside it is not like for like.**
   `J/A-uefi-pdmdiag-U-6439476f-x200-boot02.txt:3012-3017`: `pdm: migrations=41654 moved=7 runs=5/3 bad=1 (first on cpu 5)`, `FAILED at ...test_scheduler.c:2390 Expected: 0 Actual: 1`, ITER 16. It is the test's real assertion (a worker back from `task_sleep_ms()` found `preempt_disabled[cpu_id()]` non-zero), on ae1399ba + print. It is the only `bad != 0` in every `pdm:` line under run5-hw on either branch (3431 + 3970 lines, including the 822 archived runs on kernels with the #1234 fix).
   - PR body line 138 says it "is reported to the orchestrator for its own issue". A public PR cannot refer to an orchestrator, and there is no issue number. Log Entry 50 says "No issue filed yet; it bears on PR #1266, not on the padding".
   - "bears on #1266, not on the padding" is not shown. Two readings are open and the capture decides neither: (a) a kernel defect in the resume path; (b) a race in the test itself: the worker reads `c = cpu_id()` and then `preempt_disabled[c]` unpinned with IRQs open; if it is preempted and stolen between the two it reads another CPU's flag, which is legitimately 1 in that CPU's switch window. The diagnostic print widened that window (the histogram increment sits between the two reads, 6439476f/6247f73a only). The same unpinned read is still in the new test for `pdm_b`.
   - "1 of 1400 unpadded, 0 of 1400 padded, 0 of 1000 changed test" reads as if the padded kernel and the new test were cleared. They are not comparable: on the padded UEFI boots 88 to 92% of wake-ups were on one CPU, and the new test's `pdm_a` is pinned and never runs on CPUs 4-5 (finding 2).
   Fix: file the issue before the PR is opened (against the #1266 head, both readings stated, the capture line quoted), link it in the PR body and in #1266, drop "reported to the orchestrator", and say that neither the padded rows nor the changed test bear on it.

2. **HIGH — the test change removes the exposure to the path on which `bad=1` was seen, and the texts do not say so.** `kernel/tests/test_scheduler.c:2298-2330` at d09b063b.
   - `pdm_a` is pinned before every sleep: it cannot be stolen (`sched.c:1229`, `:2162`, `:2493`) and it only ever runs on CPUs 1-3. The `bad=1` was on CPU 5, on an unpinned worker.
   - On Jetson the unpinned `pdm_b` did not move in the new test: `J/A-uefi-pdmnew-d09b063b-x200-boot01.txt` has `moved == runs_a` in 200 of 200 iterations (`moved=6 runs=6/6`), so `pdm_b` changed CPU 0 times in 1200 wake-ups; `J/A-kexec-pdmnew-...` has `moved - runs_a` of 0 or 1 in every iteration. On Pi 5 it does move (boots 01 and 03: 2 to 4 more moves than `runs_a` in every iteration).
   - So on Jetson the new test checks the flag after a pinned wake-up on CPUs 1-3 only: no stolen or policy-moved resume, no cross-cluster resume. The line of `schedule()` it guards (`sched.c:2931-2933`) is still executed and a revert of the #1229 fix would still be caught, so the test is not vacuous; but the PR's "`pdm_b` stays unpinned" and the comment's "placement and work stealing still move it when they happen to" hide that on Jetson they did not, in 2 of 2 boots.
   - "The checks are unchanged" (PR line 139, commit message): the assertions are, but `pdm_moved > 0` is now true by construction (first wake-up of `pdm_a`) and no longer says anything about placement or stealing.
   - No run of the new test against a kernel with the #1229 fix reverted exists in the evidence, so that it still fails on the defect is argued from the source, not shown.
   Fix: state these limits in the PR and the test comment; either keep an unpinned, steal-exposed leg whose CPU read is pinned (IRQs masked across `cpu_id()` and the flag read) and that covers CPUs 4-5 on Jetson, or say plainly that this coverage is dropped and reference the issue of finding 1; add one mutation run (QEMU is enough).

3. **MEDIUM — wrong statements in the PR body.**
   - Line 139: in the d09b063b UEFI suite boot "the two new layout tests pass". `J/A-uefi-suite-d09b063b-boot01.txt:8692` `[PASS] test_checker_rejects_a_packed_array`, `:8715` `[IGNORE] test_slot_trace_arrays_one_line_per_cpu - SLOT_TRACE test kernel only`. One passes, one is ignored.
   - Line 152 ("Not measured"): "`/ci` in full and `make test AI_SCHED=ON` at the commits after c0612fa7". `J/A-ci-d09b063b-step01..12-*.txt` exist, all `# exit code: 0`, and Entry 50 reports them. The PR body is stale against its own evidence; line 143 has the same gap.
   - Line 143: "At the later test-only and document commits: ... `make test SLOT_TRACE=ON` ... pass". The only SLOT_TRACE run is `J/A-qemu-arm-slottrace-make-test-tree-of-7881056d-uncommitted.txt`, banner `5c4bcdda-dirty`: an uncommitted tree, not a commit. `test_slot_trace_arrays_one_line_per_cpu` has never run at a committed head, in `/ci` (IGNORE in steps 4, 5, 6, 12) or on hardware.
   - Line 136: "with 131 to 162 steals" for every iteration that ran the full 100 wake-ups: the range is 131 to 164 (`J/A-uefi-pdmdiag-P-...-boot03.txt`, 162 to 164). Entry 50 gives "131 to 162" for the failing iterations: those are 133 to 136 (Pi 5) and 159 to 162 (UEFI).
   Fix: correct the four statements; run `make test SLOT_TRACE=ON` at d09b063b or say it was a working tree.

4. **MEDIUM — "This points at the change, not at an old flake" leaves out the same failure on unpadded kernels.** PR line 122 counts 822 archived runs "on unpadded kernels with the #1234 fix" with `moved >= 5`. That figure is right (600 + 211 + 11). The same archive has the identical signature (`moved=0 runs=100/100 bad=0`, assert `pdm_moved > 0`) on unpadded kernels without that fix: `J/../c2/hw-base/uefi-r2-boot1.txt` (88d4d71b), 37 iterations, `[FILTER] [FAIL] ... passed=162 failed=38`; `J/../c2-fix-critic/base-3b369aef-r2-boot02.txt`, 13 of 34. So the one-CPU regime is not new with the padding: present before the #1234 fix, absent after it, present again with the padded layout. That supports "timing" and weakens any reading that the padding introduced a new behaviour; either way it belongs in the text. Also unpadded UEFI boots of A2b show the weak form (iterations of 30 and 33 runs, 301 and 333 ms, before 5 moves). Round 1 did not list these files either.
   Fix: add the two files and one sentence; write "follows the padded layout in this pair of kernels".

5. **MEDIUM — the behaviour change is real and the PR treats it only as test fragility (question 1).**
   - Supported: 83 of 1400 padded against 0 of 1400 unpadded with `moved=0` (verified per file: Pi 0/20/0, UEFI 31/3/29, kexec 0; unpadded all 0). The unit that varies is the boot: 4 of 7 padded boots, 0 of 7 unpadded, plus the archived unpadded boots with the #1234 fix (round 1 counted 26; I recounted the 822 runs, not the boots). Kexec is one boot a side, and Pi 5 is 1 of 3.
   - The mechanism ("one CPU wins each freshly woken worker") is an inference, and the PR and Entry 50 label it as one. Which CPU steals is not recorded, only a global count.
   - Entry 50's "steal totals per boot: unpadded 658 to 1515, padded failing boots 4974 to 8145" is true and misleading: failing iterations run 1 s with 200 wake-ups instead of about 50 ms with about 10. Per wake-up: UEFI padded 0.79 to 0.86 against 0.64 to 0.76 unpadded; kexec 0.77 against 0.65; Pi 5 padded 0.16, 0.64, 0.19 against 0.32 to 0.36 (two of three padded Pi boots steal less than unpadded). There is no 5 to 8 times increase in stealing.
   - What does change: where a woken task runs. Padded sticky boots have 88 to 95% of wake-ups on one CPU (kexec padded: all on CPUs 4-5); unpadded at most 56%. The experiment does not show harm and does not exclude it: no latency or throughput row compares the two, and `--assert-shares` x20 was run on kexec only, the one path with no sticky boot.
   - PR line 17 "no ... scheduling decision changes" is true of the code and reads as true of the outcome. "Not measured" has no line for this.
   Fix: qualify line 17 ("no scheduling code changes; the observed wake-up CPU distribution does change, below"), add a "Not measured" line, normalise the steal figures in the log.

6. **LOW-MEDIUM — latent race in the test (carried from #1266, kept by this PR).** `test_scheduler.c:2317-2324`: `c = cpu_id()` then `preempt_disabled[c]`, unpinned, for `pdm_b`. See finding 1(b). Closed for `pdm_a` by the pin, open for `pdm_b`. Fix: mask IRQs across the two reads.

7. **LOW — evidence defects.**
   - `J/A-ci-d09b063b-step09-build-order.txt` and `-step10-jetson-layout.txt` carry a "QEMU serial" section with build id 20261115142945, which is step 6's run: `A-ci.sh.txt` appends `build/test-output.log` whenever it exists. Stale log under a step that did not produce it; the exit codes are unaffected.
   - `P/A-pi5-slmstore-before-a2b.txt`: the four HASH lines say `(read ?)`. The script takes the most recent `sdwire_cat` in the transcript, so the record does not show the "before" hashes were read at the "before" time.
   - Pi 5 alternation order rests on header text and labctl directory names; there is no trigger log for Pi 5.

8. **LOW — documents.**
   - `kernel/include/spinlock.h:88-92` still states the mechanism as fact ("loses its LDAXR reservation every time another CPU stores to lock j ... and its STXR fails although lock i is free"); `docs/scheduler.md` says it is inferred and lists 14 + 1 + 4 cases that do not fit.
   - `docs/scheduler.md` "Regression test": "line-aligned and at least a line apart" understates `layout_ok()` (stride equal to element size, whole lines, `MAX_CPUS` elements).
   - c0612fa7's commit message keeps "1.1 to 6.6 ms ... in 1 to 3%" (Entry 41's figures). It cannot be changed without a rewrite; make sure the squash message uses the PR body.

## Round-1 findings

| # | Status | Checked against |
|---|---|---|
| 1 suite failure understated | Resolved in text (822, 1 of 6, experiment run, test changed); new defects in findings 1, 2, 4, 5 | PR 118-139; 822 = 600 + 211 + 11 recounted |
| 2 stale 6.64 ms | Resolved in PR, #1262 comment, scheduler.md, spinlock.h, kernel/CLAUDE.md; commit message of c0612fa7 not (finding 8) | 6701 us at `A-uefi-r3w-U-...-boot01.txt`; range 1032 to 6701 us; 1.3% / 4.5%; 0.8% / 2.8% re-derived |
| 3 baseline confound | Resolved (PR line 90, comment line 30, scheduler.md) | texts |
| 4 #1262 evidence | Resolved | 14 of 279, 8 under 10, 1057 to 4707 us, one lock word 1 (cpu5, 1903 us, 35) re-derived; "4 of 109" not re-derived |
| 5 column label | Resolved | 20,12,8 / 37,60,37 lines; 18,12,7 / 37,59,35 by the count field; 36 by entry-to-lock time: all re-derived |
| 6 scope | Resolved | PR 86-93, comment line 28 |
| 7 audit gaps | Carried plainly as open (#1279), listed in scheduler.md and PR 65-69 | line numbers checked in the tree |
| 8 test gaps | Resolved in code (7881056d); the new slot-trace test has no run at a commit (finding 3) | diff, captures |
| 9 wording | Resolved | PR line 114, Entry 47 |
| 10 bench caveats | Resolved | 96.83 / 98.50 / 107.67 / 107.33 and sd 3.19 recomputed from the 12 run files |

## Answers to the specific questions

1. Supported for this pair of kernels, in boots (4 of 7 against 0 of 7), with the omission of finding 4. The mechanism is an inference and is labelled so. The experiment shows a change in where woken tasks run, not more stealing per wake-up; whether that matters outside the test is not measured, and the PR should say so (finding 5).
2. Changing the test here is acceptable (it is disclosed in bold, the removed loop never steered a wake-up: `sched.c:1687-1690`, and the bare `cpu_affinity` store is safe: no lock guards that field anywhere, readers act only when the task is BLOCKED or queued). The description is not accurate enough (finding 2).
3. As an open defect report of unknown cause with its own issue against the #1266 head, linked from this PR and from #1266, before this PR is opened. The PR text does not do that yet (finding 1).
4. No. "Closes #1263" (line 1) is the only closing keyword in the PR body; the five commit messages have none. The #1262 comment claims no fix and says twice that the 2 s failure was not reproduced.
5. Nothing in the kernel code (unchanged from c0612fa7 outside `ENABLE_BOOT_TESTS`). Conditions: #1266 must merge first (the branch sits on ae1399ba); the ledger row awaits the user's acceptance; the diag branches are not for merge. The test change should not merge before finding 1's issue exists, because after it no Jetson test reaches that path.

## Matches (re-derived from raw captures)

A2b table, all six rows and per-boot splits; banners and build ids of all 19 A2b captures; 200 ITER lines each, ends at `slmos>`; `bad=0` in 1400 padded and 1000 changed-test iterations; sticky CPUs 3, 2, 4 (Jetson) and 1 (Pi 5); the exact line `steals=162 a=[0,0,0,100,0,0] b=[0,0,0,100,0,0]` 30 times in UEFI padded boot01; UEFI and kexec order from the trigger logs (U,P,U,P,U,P; one trigger, one capture each); staged image hashes equal the `a/img` files; QEMU x200 `moved` 6 to 8; Pi 5 new test `moved` 5 to 8; padded Pi suite `moved` 2, 4, 2 and close-out 5 to 7 within 21 wake-ups; d09b063b UEFI suite 21 failures, 20 names, all in the known list, same 2955 PASS lines as c0612fa7 boot01; `/ci` d09b063b 12 steps exit 0; slmstore records identical in 9 entries and 4 hashes; R3 and bench figures as in the round-1 table above.

## Not checkable from the evidence

- The cause of `bad=1`, and which worker saw it.
- Which CPU performs the steals in the sticky iterations.
- Pi 5 boot order and boot times in A2b (no trigger log); that the "before" slmstore hashes were read before A2b.
- Contents of `files.lfs` and `files.lfs.delta` (listed by size and mtime only).
- That the uncommitted trees of the three `*-uncommitted.txt` QEMU captures equal the later commits.
- "4 of the 109 slow finishes have no prelude report", "37 more instructions in `schedule()`", the `A-fndiff` instruction-identity claim, `task_lock`'s line neighbours: not re-derived this round.
- #1279, #1274, #1231 as described; board end state (lab not touched).
