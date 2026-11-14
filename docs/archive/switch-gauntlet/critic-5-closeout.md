Model: claude-fable-5-2

# Critic verdict: switch gauntlet run 5, close-out rows (#1234, #1238, #1242)

Scope: the six close-out rows only, at PR head `c2/switch-window-r2` 6a4cfa93, base main dc51107c. Evidence read from `git archive` of `origin/gauntlet-5-hw-jetson` (aa080076) and `origin/gauntlet-5-hw-pi5` (62a7bbe6), directory `docs/archive/switch-gauntlet/run5-hw/closeout/`; bars from `run5-brief.md` and `switch-5.md` Entries 8, 24, 34 on `origin/gauntlet/switch-5` (eac2e873). All counts below were taken from the serial body below each file's `# ---- serial ----` line, not from headers. `bench-context-summary.txt` was opened only after the critic's own numbers existed; it agrees with them.

## Head check

- `git range-diff 8ec0e330..c0ea0714 dc51107c..6a4cfa93`: the 13 fix commits (43d0f19b..c0ea0714) map `=` (identical patch) onto 6cb4097e..e6368dc5; the four C1 commits are absent because main carries them; 6a4cfa93 is one new docs commit.
- `git diff --stat c0ea0714 6a4cfa93 -- . ':!docs' ':!.github' ':!scripts/ci'` is empty: the kernel tree outside docs is byte-identical between the run-5 fix head and the PR head. dc51107c is an ancestor of 6a4cfa93.
- Every capture's banner reads `(6a4cfa93)`, or `(dc51107c)` for base bench boots. Build stamps: prod 20261114181943, R2 ...182219, R3w ...182340, suite ...182101, Pi 5 prod ...182501, Pi 5 suite ...182612, base Jetson ...181714, base Pi 5 ...181830. The banner carries no dirty marker, so a clean tree is not provable from it.

## Rows

| # | Row | Files | Counted result | Bar | Result | Reason |
|---|---|---|---|---|---|---|
| 1a | Jetson kexec production boot_test x10 | `kexec-prod-6a4cfa93-boot01..10.txt`, `trigger-kexec-prod-a..d.log` | 10 of 10 files: MB1 at serial line 1, `Boot-mode : Coldboot`, `rst_source: 0x0`, one banner 6a4cfa93, `SMP: 5/5 secondary CPUs online`, ends at `slmos>`; 0 `[WARN]`, 0 halts or panics. 10 triggers (3+3+2+2), each reply `669d58d3753f28b2` = header sha 669d58d3. 10 distinct files. | No regression: boot_test --count 10, full captures | PASS | 10 of 10 complete from power-on to prompt |
| 1b | Jetson UEFI-direct production boot_test x10 | `uefi-prod-6a4cfa93-boot01..10.txt`, `trigger-uefi-prod-a..c.log`, `deploy-uefi-prod-6a4cfa93.log` | 10 of 10: Coldboot MB1 first, banner 6a4cfa93, `Boot source: UEFI-direct (EFI stub)`, 5 secondaries, ends at `slmos>`, 0 halts. Boots 02..10 go SD Linux, reboot (`rst_source: 0xb`), SLM-OS; 9 triggers (2+3+4) with `BootNext: 000E`. Boot01 went MB1 straight to SLM-OS on the deploy script's armed BootNext (no trigger), disclosed in its header. Deploy log sha baafe134 = header sha. One `[WARN]` per boot: `RNG firmware seed absent`. | same | PASS | 10 of 10; boot01 is a cold boot into the head image by a different route, disclosed |
| 2 | `bench stealing --sustain 2500 --assert-shares` x20, kexec | `assert-shares-kexec-6a4cfa93-x20.txt` (boot = `kexec-prod-6a4cfa93-boot10.txt`) | Body: 20 `AS-RUN`, 20 `ASSERT-SHARES: PASS`, 0 `FAIL`, `AS-RC 1..20` all 0, `AS-END`, prompt. Lowest thief share 15.8% (run 19), best 17.4% (run 8). | 20 of 20 on Jetson kexec | PASS | 20 of 20. Weakness: the file is one serial_send session with no banner; its tie to boot10 (banner 6a4cfa93, trigger-kexec-prod-d trigger 2 at 19:32:34) rests on the header and the time order. One file for 20 runs. |
| 3 | R3 wake x200, UEFI-direct, 1 boot | `uefi-r3w-6a4cfa93-boot01.txt`, `trigger-uefi-r3w.log`, `deploy-uefi-r3w-6a4cfa93.log` | 200 `[ITER n/200]` lines, sequence 1..200 complete; **199 PASS, 1 FAIL (iteration 116)**; `[FILTER] [FAIL] ... runs=200 passed=199 failed=1`; `[FAIL] 1 total failure(s)`. Iteration 116: `swo wake: ... bad_gen=0 two_cpus=0 NOT-FINISHED`, `FAILED at test_scheduler.c:2700 - swo wake phase`, 2414432 us; `swo: left` lists three `swo_w` workers (ids 2084, 2089, 2090) with state=0 (READY), on_cpu=0, assigned=5 that did not terminate inside the test's 2 s window. bad_gen=0 two_cpus=0 in all 200 lines. Capture complete (Coldboot, banner 6a4cfa93 build 182340, deploy sha 52c1d061 = header). | Fixed: 0 failures in at least 5x the baseline mean iterations to failure (baseline Entry 8: 1 boot, hang at iteration 1; threshold 5) | **FAIL** | The bar is 0 failures; the requested boot has 1 in 200. Extra boots below do not change the requested row. |
| 3x | (supplementary) R3 wake x200, 2 more boots | `uefi-r3w-6a4cfa93-extra-boot01,02.txt`, `trigger-uefi-r3w-extra.log` | 200 of 200 and 200 of 200, complete, banner 6a4cfa93, same image sha. | n/a | info | Combined 599 of 600; with the 5 UEFI suite boots (1 iteration each, PASS) 604 of 605. |
| 4 | R2 x200, kexec, 1 boot | `kexec-r2-6a4cfa93-boot01.txt`, `trigger-kexec-test-a.log` | 200 ITER lines 1..200, 200 PASS, 0 FAIL; 200 `pdm:` lines all `bad=0`; `[FILTER] [PASS] ... passed=200 failed=0`; `All test suites passed!`; Coldboot; banner 6a4cfa93; trigger reply `c841d6614ddaf8ce` = header sha; 0 halts. | Fixed, threshold 5 (Entry 24); re-run at the rebased head per the merge-order rule | PASS | 200 of 200 against a threshold of 5 |
| 5a | Suite x5, Jetson kexec | `kexec-suite-6a4cfa93-boot01..05.txt`, `trigger-kexec-suite-a,b.log` | Each boot: 105 Unity summary lines, Tests 2619 = Passed 2556 + Failed 13 + Ignored 50; 2556 `[PASS] test_` lines; same 13 failing test names in all 5; final `[FAIL] 14 total failure(s)` (13 + `gpu: capabilities detect valid`); 0 halts; Coldboot; banner 6a4cfa93; 5 triggers, replies `53e9b77e23a2a2dc` = header sha. R2 and R3 wake PASS in all 5; `swo migrate` and `swo wake` lines bad_gen=0 two_cpus=0. | 5 boots, every failure with an issue number | PASS | All 13 names plus the two detail lines are the list in open #1109 (read with `gh issue view 1109`). No new name. |
| 5b | Suite x5, Jetson UEFI | `uefi-suite-6a4cfa93-boot01..05.txt`, `trigger-uefi-suite-a,b.log`, `deploy-uefi-suite-6a4cfa93.log` | Each: 2619 = 2548 + 20 + 51; same 20 names in all 5; `[FAIL] 21 total failure(s)`; 0 halts; banner 6a4cfa93; deploy sha 2892f76a = header; 5 triggers. R2, R3 PASS in all 5. | same | PASS | 11 names are in #1109; the other 9 are exactly the 9 named in open #1231. The two #1109 PMM coalescing tests pass on this path. |
| 5c | Suite x5, Pi 5 | `pi5-suite-6a4cfa93-boot01..05.txt` | Boots 01, 02, 03, 05: 2568 = 2516 + 18 + 34, `[FAIL] 18 total failure(s)`. Boot 04: 2568 = 2532 + 3 + 33, `[FAIL] 3 total failure(s)` (the 14 `test_blob_autoload_*`/`test_blob_boot_autoload_*` tests and `test_eviction_run_tests_passes` pass there). Banner 6a4cfa93 is the first console output, as in run 5's `run5-hw/c2/hw-fix/pi5-prod-*` files; all reach the final summary; 0 halts. R2, R3 PASS in all 5. | same | PASS | 17 names are the Pi 5 list in #1109; `test_eviction_run_tests_passes` (sub-check `mlp_finish_nan_never_wins`) is named in #1109 as a flake (here 4 of 5). No trigger log exists for Pi 5; image sha 834dd616 is header-only. |
| 5d | (supplementary) | `kexec-suite-6a4cfa93-INCOMPLETE-timeout330.txt` | 83 summary lines, ends at `[PASS] test_negative_controls_report` with no suite summary; 12 failing names, a subset of the 13. It is trigger 2 of `trigger-kexec-test-a.log` (19:59:05, sha 53e9b77e). | n/a | correctly excluded | Truncated capture; not counted; 5 complete boots stand without it. Its header's statement about a later serial_capture has no file. |
| 6a | `bench context`, Jetson kexec, base vs head | `bench-context-jetson-{base,head}-*.txt` (6 runs per side, 99 round trips each); boots `kexec-bench-base-dc51107c-B1..B3-boot01.txt`, `kexec-prod-6a4cfa93-boot10.txt` (H1), `kexec-bench-head-6a4cfa93-H2,H3-boot01.txt`; `trigger-kexec-bench-*.log` | Base run means 90, 91, 87, 85, 90, 85: mean 88.00 ns, sample sd 2.68. Head 96, 98, 97, 96, 101, 96: mean 97.33 ns, sd 1.97. **Delta +9.33 ns = +10.6%**. p50 96 and p95 128 in all 12 runs; min 64 in all. Order by trigger time: H1 19:32, B1 19:38, H2 19:41, B2 19:45, H3 19:48, B3 19:52 (alternating). Boot banners dc51107c / 6a4cfa93; trigger replies 87bf04ef.. and 669d58d3.. match headers. | Context-switch time within 10% of main's on both boards | **FAIL** | Mean is over 10%. See resolution note. |
| 6b | `bench context`, Pi 5 | `bench-context-pi5-{base,head}-*.txt` (6 per side); boots `pi5-bench-base-dc51107c-B1..B3-boot01.txt`, `pi5-bench-head-6a4cfa93-H1..H3-boot01.txt` | Base means 102, 101, 101, 99, 101, 100: 100.67 ns, sd 1.03. Head 109, 108, 110, 107, 111, 109: 109.00 ns, sd 1.41. Delta +8.33 ns = +8.3%. p50 93 in 6 of 6 base runs, 111 in 6 of 6 head runs (+19%, one counter step). p95 111/112 base; 112 (4 runs) or 129 (2 runs) head. Header times B1 22:22, H1 22:23, B2 22:24, H2 22:25, B3 22:26, H3 22:27 (alternating; header-only). | same | PASS on the mean | +8.3% by mean, the statistic run 5 reported. p50 moved one whole counter step. |

### Resolution note for row 6

- Jetson per-sample values are all multiples of 32 ns (64, 96, 128, 160, 192, 288, 320): resolution 32 ns. Pi 5 values step by about 18.5 ns (92/93, 111/112, 129/130, 148/149, 203/204, 222/223). p50 and p95 can therefore only move in whole steps (33% on Jetson, 20% on Pi 5) and cannot test a 10% bar; the run mean over 99 samples is the only statistic finer than the bar, printed as an integer.
- Run-to-run spread: Jetson base 85 to 91, head 96 to 101 (no overlap); Pi 5 base 99 to 102, head 107 to 111 (no overlap). Welch standard error: Jetson 1.36 ns (t 6.9), Pi 5 0.71 ns (t 11.7). The base-to-head difference exceeds the run-to-run spread on both boards; the slowdown is measured, not noise.
- Jetson 95% interval on the delta (5 df, about ±2.6 SE): +5.8 to +12.8 ns, +6.6% to +14.6%. The point estimate is over the bar and the interval straddles it; the evidence does not show "within 10%". Pi 5 interval +6.5 to +10.2 ns, +6.5% to +10.1%.
- Run 5's two-run figures (Entry 34: Jetson 87/88 to 96/98, Pi 5 106/106 to 110/112) are consistent with the Jetson result; the Pi 5 base here is about 5 ns lower than run 5's.

### How the extra R3 wake boots bear on row 3

- Rate over the three boots: 1 failure in 600 iterations (`uefi-r3w-6a4cfa93-boot01.txt`, `-extra-boot01.txt`, `-extra-boot02.txt`). The baseline on this cell hung at iteration 1 (Entry 8, `uefi-r3w-boot1.txt`), so the head is far better than the base, and the two-CPU and generation checks never fired (0 in 600).
- The one failure is a different signature from the baseline: three READY workers assigned to CPU 5 not run for 2 s after the stop flag. Whether the fix's deferred switch-in causes it is not established by any file here; no base run exists on this cell that survives long enough to show the same symptom. It is filed as open #1262 (P1-high).
- Observed in the same file after iteration 116: `moves=` goes from 5xxxx in 115 of 115 earlier iterations to 6xxxx in 84 of 84 later ones, and `[WARN] Cannot migrate running task 'swo_w'` appears 139 times in boot01 (2 before iteration 116) against 4 in each extra boot. The boot did not return to its earlier behaviour after the failure. Cause not determined.
- The "Fixed" bar reads 0 failures. A failure of the reproducer at the fix head, on a cell that failed at baseline, in the scheduler paths the fix changes, cannot be passed by adding clean boots afterwards.

## Failure modes checked by name

- Selective summary: counts taken from serial bodies; header summaries agree. The failing R3 wake boot is presented as the row, the clean boots are labelled supplementary. Not found.
- Truncated or tail-only capture: every Jetson boot file starts at MB1 Coldboot and reaches its end marker; Pi 5 files start at the banner. One truncated capture exists and is labelled INCOMPLETE and excluded. Bench and assert-shares outputs are serial_send sessions, each tied to a named full boot capture.
- Assembled register file: no register dumps in these rows.
- Redefined criterion: none by the builder. `bench context` stands in for `bench smp` (which prints no time, Entry 34); the request for this session names `bench context`.
- Hypothesis stated as finding: none found in the evidence headers. The cause of iteration 116 is stated nowhere here as a finding.
- Stale base: base is main dc51107c, an ancestor of the head; fix patches identical by range-diff.
- Banner hash mismatch: none. All 61 boot captures carry the expected hash.
- Image sha against trigger replies: all kexec trigger replies and UEFI deploy logs match the headers. Pi 5 image shas are header-only.
- Bar computed from one cell: thresholds per cell from Entries 8 and 24; the UEFI R3 wake baseline is one boot (disclosed in Entry 8).
- Precondition from a prior run: none; every row was run at 6a4cfa93.
- Path never exercised / test kernel at the PR head on every board and path: suite boots ran the test kernel at 6a4cfa93 on kexec, UEFI and Pi 5, with R2 and R3 present and passing in all 15.
- Partial per-CPU table: assert-shares output lists cpu 0 to 5 in each run.
- Number without a file: none in this document.
- Uncommitted tree: not provable from banners (no dirty marker); `FAILED at` paths show the builds came from the Gauntlet-5-Wrap-Up worktree.
- Critic reading the claim first: summary file read last; no post-final-report log text read.
- One file per run or boot: boots yes; assert-shares is one file for 20 runs, and four bench files hold 2 runs each (raw output complete).
- Wrong frame read as fault site, lock graph, forced tick: not applicable to these rows (no fault dumps; no code change since the C2 fix verdict).
- Process kills, board claims by the critic: none; no labctl, ssh or board access used.

## Verdict

Rows 1a, 1b, 2, 4, 5a, 5b, 5c, 6b pass. Row 3 fails as written (1 failure in the requested 200 iterations; bar is 0). Row 6a fails as written (Jetson mean +10.6% against a 10% bar, interval +6.6% to +14.6%).

VERDICT: FAIL (rows: 3 R3 wake UEFI-direct boot01, 6 bench context on Jetson kexec)

Scripts used (critic scratch, not committed): the critic's session scratchpad `critic/scripts/{scan,iters,suite,bench}.py`.
