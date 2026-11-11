# Critic verdict, coherency run 3, C1 (#1202, #1205, #1211)

Critic: `coherency-3-C1-critic`, 2026-11-11. Bound by `run3-critic-protocol.md`,
`run3-rules.md`, `run2-critic-protocol.md` and `run2-rules.md`. Before this
verdict was written the critic had not opened `coherency-3.md`, the builder's
report, PR bodies, issue comments on #1202, #1205 or #1211, or the summary files
`step6-boots.txt` and `removal-1211-boots.txt`. The critic did read run 2's
header file `run2-hw/c1/force-and-step6-boots.txt` (run 2's builder summary),
as the brief directed, for the method and the known failures.

Paths below are relative to `docs/archive/coherency-gauntlet/`. Run 3 files are
on `gauntlet-3-hw-pi5` @ 127428e7 (builder) and 9352c948 (critic); run 2 files
on `gauntlet-2-hw-pi5`.

## 1. Own count from the raw files

Method: each file was taken from the evidence branch with `git archive` and
parsed by a script that reads the banner hash, every `[PASS]`/`[FAIL] <name>`
line, the `[FAIL] N total failure(s)` line and every `[test_wait]` and
`[C1-1211]` line. Completeness: every file starts at the power-on banner and
ends at the total-failures line; the set of test names was compared across all
files of a kernel. The names that the exact-line match missed in some files
(`test_strtod_integer`, `test_strtol_endptr_stops_at_non_digit`,
`test_lwip_rand_seed_null_or_zero_len_does_not_crash`, `TaskDeadline::NONE`,
`msg_recv`, `test_slm_component_run_and_find`,
`test_shell_io_tcp_write_buf_timeout`) were each found by grep in those files,
split or joined by interleaved `[counter]` or `[INFO]` UART output (for example
`nc-step6-run17.txt:8163`, `removal-1211-run01.txt:8265`). No file is cut,
filtered or reassembled.

The 18 known failures are #1109's Pi 5 list: `test_builtin_commands_grouped_and_sorted`,
`test_every_command_has_help_entry`, 14 `test_blob_autoload_*`/`test_blob_boot_autoload_*`
tests, `test_eviction_run_tests_passes` (with its sub-check `mlp_finish_nan_never_wins`)
and `test_timer_irq_is_physical`.

### Step 6, NC kernel (20 files)

| Files | Banner | Boots | #1202 slm_stats_live | #1205 cleanup_ctx_is_per_task | #1211 task_lifecycle | `[test_wait]` TIMEOUT / over-a-quarter | Total failures | Beyond the 18 |
|---|---|---|---|---|---|---|---|---|
| run2-hw/c1/nc-step6-run01..09.txt | 30dc24bc (9/9) | 9 | 9/9 PASS | 9/9 PASS | 9/9 PASS | 0 / 0 | 18 in 7, 19 in 2 | 2 (below) |
| run3-hw/c1/nc-step6-run10..20.txt | a03fc75b (11/11) | 11 | 11/11 PASS | 11/11 PASS | 11/11 PASS | 0 / 0 | 18 in 11 | 0 |
| Total | | 20 | 20/20 | 20/20 | 20/20 | 0 / 0 | | 2 |

`git diff 30dc24bc a03fc75b -- kernel/ runtime/` is empty, so the 9 boots on
30dc24bc count for the a03fc75b head as the brief allows.

`[test_wait]` values (NC 20): slm_live flag after 0 to 9519 us (bound 250000 us;
max `nc-step6-run20.txt`); c4_multi cleanups after 3481 to 8935 us (bound
2000000 us; max `run2-hw/c1/nc-step6-run06.txt`); task_lifecycle met after 6 to
14 us (bound 1000000 us). Three `[test_wait]` lines per file, 60 in all, none a
TIMEOUT, value-at-bound or over-a-quarter line.

Failures beyond the 18, NC step 6:

| File | Test | Total |
|---|---|---|
| run2-hw/c1/nc-step6-run04.txt | `test_slm_stop_from_second_session_during_prompt` | 19 |
| run2-hw/c1/nc-step6-run06.txt | `test_proactive_load_balance_runtime_config_aggressive` | 19 |

### Step 6, removal kernel (20 files)

| Files | Banner | Boots | #1202 | #1205 | #1211 | TIMEOUT / over-a-quarter | Total failures | Beyond the 18 |
|---|---|---|---|---|---|---|---|---|
| run3-hw/c1/removal-step6-run01..20.txt | 79c10261 (20/20) | 20 | 20/20 PASS | 20/20 PASS | 20/20 PASS | 0 / 0 | 18 in 20 | 0 |

`[test_wait]` values (removal 20): slm_live 0 to 10842 us (max
`removal-step6-run02.txt`); c4_multi 3480 to 10600 us (max
`removal-step6-run19.txt`); task_lifecycle met after 2 to 3 us in 19 files and
9998 us in `removal-step6-run13.txt` (bound 1000000 us, under a quarter).

### #1211, removal with step-1 instrumentation (20 files)

All 20 files `run3-hw/c1/removal-1211-run01..20.txt` carry banner 9f375c19,
the head of `c1/instrument-1211-removal`.

| Row | Count | Files |
|---|---|---|
| `[C1-1211] lifecycle PASS` at the bound | 20/20 | removal-1211-run01..20.txt |
| `[C1-1211] lifecycle FAIL` | 0/20 | none |
| `[C1-1211] FAIL repoll` lines | 0 | none |
| free_at_bound | 853135 in 20/20 (target 853119, initial 853135) | all |
| free_win_start | 853007 in 17, 853071 in 2 (run13, run20), 852815 in 1 (run11) | as named |
| window_us (50 x yield + delay(50000)) | 1688 to 1691 | all |
| waiter CPU | 0->0 in 20/20 | all |
| ran() row | all 8 tasks logged with a CPU and a time in 20/20 | all |
| schedule() row | CPUs 0 to 3 at window start and bound in 20/20 | all |
| `test_task_lifecycle` | 20/20 PASS | all |

Classification that follows from these rows: not reproduced, 0 of 20. No boot
failed at the bound, so neither "timing" nor "visibility" can be decided from
this set.

Branch content check (`git diff 79c10261 9f375c19`): two commits, edadfb2e and
9f375c19. In `test_component.c` and `test_slm_shell.c` the #1202 and #1205 test
functions at 9f375c19 are identical to 375ad4dd's; the only differences from
375ad4dd's files are two other waits that 30dc24bc converted
(`test_component_cleanup_ctx_payload_preserved`,
`test_slm_stop_from_second_session_during_prompt`), which 79c10261 also carries.
In `test_integration.c` the window is the pre-conversion loop (50 x `yield();
delay(50000);`, `final_free >= initial_free - 16`, as at 85f51ffd) with logging
added; the completion wait is 79c10261's. The branch therefore reverts only the
`test_task_lifecycle` conversion plus the step-1 instrumentation, with
`[C1-1211]` logging added to the reverted window.

Other failures in the #1211 boots (not step-6 boots, so not under definition of
done row 3): `test_slm_stats_live_from_second_session` failed in 8/20
(removal-1211-run03, 07, 08, 10, 11, 12, 13, 20; total 19 each). This branch
carries 375ad4dd's yield-bounded #1202 wait. In all 8 the `[C1-1202] live FAIL
repoll` line shows the flag set 364 to 3032 us after the bound expired
(`first_true_after_expiry_us`; 100 ms = 1 in 8/8). Recorded, not judged.

## 2. Reproduction (pi-5-1, claimed as `coherency-3-C1-critic` 06:54 to about 07:22 UTC)

Staged images verified before deploy: `slmos-r3-c1-nc-a03fc75b.bin` 8733328 B,
sha256 5c10222e4774e48c2d5a0de69a534c2e6b0b15859d68f7b5ec13c6fcb115ac79, banner
a03fc75b; `slmos-r3-c1-removal-79c10261.bin` 8778384 B, sha256
41e4319faea88fe8ad2d0ec2a9b1020356cb2390bb57af3dcf05c98a038cc563, banner
79c10261. The critic built both heads (`make kernel-test PLATFORM=RASPI5`, one
at a time): banners a03fc75b (8733328 B) and 79c10261 (8778384 B); sha256
differs only through the build timestamp in the banner.

Card checked with `sdwire_info` and `sdwire_ls` before each write: builder's
image as `kernel_2712.img`/`kernel8.img` (8782480 B), originals as `*.prec3`
(6935256 B). Each kernel went to both names. `boot_test`, expect
`All test suites passed|total failure\(s\)`, timeout 330 s, a fresh output_dir
per call.

| Kernel | Calls (output_dir, runs) | Files (run3-hw/critic-c1/) | Banner | Result |
|---|---|---|---|---|
| NC | c1-r3-critic-nc-a (3), c1-r3-critic-nc-b (2) | nc-repro-run01..05.txt | a03fc75b 5/5 | 5/5 pattern matched; #1202, #1205, #1211 PASS 5/5; total 18 in 5/5; 0 beyond the 18; 0 TIMEOUT |
| Removal | c1-r3-critic-rm-a (3), c1-r3-critic-rm-b (2) | removal-repro-run01..05.txt | 79c10261 5/5 | 5/5 pattern matched; #1202, #1205, #1211 PASS 5/5; total 18 in 5/5; 0 beyond the 18; 0 TIMEOUT |

Values: NC slm_live 5184 to 9493 us, c4_multi 3481 to 3482 us, task_lifecycle 7
to 14 us; removal slm_live 1322 to 10852 us, c4_multi 3479 to 10599 us,
task_lifecycle 2 us. The reproduction agrees with the builder's raw files.

Restore: one `sdwire_update` renamed the two test kernels aside, renamed
`kernel_2712.img.prec3` and `kernel8.img.prec3` to `kernel_2712.img` and
`kernel8.img`, and deleted the two renamed test kernels (a plain rename onto the
existing names was refused by labctl first, changing nothing). Nothing else on
the card was touched. `boot_test` runs=1, expect `slmos>`, output_dir
c1-r3-critic-restore: 1/1 in 12.1 s, banner b42b2c1d,
`run3-hw/critic-c1/restore-boot.txt`. pi-5-1 left powered on at the SLM-OS
shell (health check: power on, ping, serial) and released.

Every critic file is the call's `run_NN.txt` copied unchanged (cmp-verified),
commit 9352c948.

## 3. Builder's claim and agreement

Filled in below, after this verdict was written (section 6).

## 4. Failure modes, one line each

Run 1 (run 2 protocol table):

| Row | Checked how | Result |
|---|---|---|
| Selective summary | Counted files by `git ls-tree`: NC 9 + 11, removal 20, #1211 20; per-file rows built for all 60 | Comparison with the summaries in section 6 |
| Truncated capture | Banner at top and total-failures line at end of every file; test-name sets compared, each absent name found by grep as interleaved text | None truncated |
| Assembled register file | No register dumps in C1; each `[test_wait]` and `[C1-1211]` line read inside a file with its banner | Not applicable; none assembled |
| Redefined criterion | Applied 20/20 per kernel, three named tests in every boot, zero TIMEOUT lines, and "filed" for row 3 as written | Row 3 not met as written (verdict) |
| Hypothesis stated as finding | #1211 classification checked against the rows: 0/20 failures, so only "not reproduced" follows | Checked against the summary in section 6 |
| Stale base | Banner of every file against its head: 9 x 30dc24bc, 11 x a03fc75b, 20 x 79c10261, 20 x 9f375c19; empty kernel/runtime diff 30dc24bc..a03fc75b; critic builds match | All match |

Run 2 additions:

| Row | Checked how | Result |
|---|---|---|
| No critic at all | This file | Verdict present |
| Decisive table with one CPU's row | C1 has no per-CPU benchmark; the #1211 ran() (8 tasks) and schedule() (CPUs 0 to 3) rows were rebuilt for every file | All rows present in 20/20 |
| Number with no file | Every number in this verdict names its file | None without a file |
| Observation tree never committed | Heads resolved on origin (a03fc75b, 79c10261, 9f375c19; 30dc24bc ancestor of `c1/test-wait-helper`); evidence commits 127428e7 and 9352c948 | All pushed; none reconstructed |
| Critic read the builder's claim first | Summary files and `coherency-3.md` not opened before this verdict; run 2's header file was read, as directed | Not void; disclosed |
| labctl database or private boot helper | Raw files have the boot_test layout; builder process otherwise not visible in raw files. Critic's own calls: claim, health check, bindings_list, power_off (3, to let `sdwire_info` read the card before each write), sdwire_info, sdwire_ls, sdwire_update, boot_test, release; no ssh, no serial tools, no labctl files | Critic: none; builder: see section 6 |
| Cell built on a stale head | Banner against claimed head for every file before counting | All match |

## 5. Verdict

**fail**, definition-of-done row 3.

- Row 1 met: NC 20/20 (`run2-hw/c1/nc-step6-run01..09.txt`,
  `run3-hw/c1/nc-step6-run10..20.txt`), removal 20/20
  (`run3-hw/c1/removal-step6-run01..20.txt`), the three named tests PASS in all
  40, zero `[test_wait]` TIMEOUT lines; reproduced 5/5 per kernel.
- Row 2 met on the rows: 0/20 failures (`run3-hw/c1/removal-1211-run01..20.txt`),
  so "not reproduced" is the only classification that follows; the branch
  content is as required.
- Row 3 not met: two failures beyond the 18 occurred in step-6 boots that count
  toward the NC 20, `test_slm_stop_from_second_session_during_prompt` in
  `run2-hw/c1/nc-step6-run04.txt` and
  `test_proactive_load_balance_runtime_config_aggressive` in
  `run2-hw/c1/nc-step6-run06.txt`. They are counted and named (also in run 2's
  header file), but no GitHub issue exists for either: `gh issue list --state all`
  searches on both test names and both filenames return no such issue (the only
  hit, #1044, is a closed QEMU AI_SCHED issue from 2026-11-02), and no issue
  created since 2026-11-08 covers them.

## 6. Builder's claim compared (read after sections 1 to 5 were written)

Read: `run3-hw/c1/step6-boots.txt` and `run3-hw/c1/removal-1211-boots.txt`
(`gauntlet-3-hw-pi5` @ 127428e7). `coherency-3.md`, PR bodies and issue
comments were not opened. A script compared every table row of both files with
the critic's parse of the raw file it names: 31 step-6 rows (banner, total
failures, slm_live us, c4_multi us, lifecycle deficit/met us) and 20 #1211 rows
(banner, total failures, `[C1-1211]` result, waiter CPU, ran CPUs, window us,
dispatch to completed us, dispatch to bound us, free pages at window start and
bound, schedule() calls in the window): 0 mismatches.

| Builder's claim, quoted | Critic's count | Agree |
|---|---|---|
| "NC a03fc75b, this run: 11/11 (nc-step6-run10..20.txt)" | 11/11 | yes |
| "With run 2's 9/9 on 30dc24bc ... NC: 20/20" | 20/20; kernel/runtime diff empty | yes |
| "removal 79c10261: 20/20 (removal-step6-run01..20.txt)" | 20/20 | yes |
| "0 [test_wait] TIMEOUT lines and 0 over-a-quarter lines in 31 files" | 0 and 0 in 31 (also 0 and 0 in run 2's 9) | yes |
| "Failures beyond the 18: none in 31 of 31 boots of this run" | 0 in the 31 run 3 boots | yes |
| "none of the 18 missing in any boot" | 18 present in all 40 step-6 boots | yes |
| "Run 2's two NC-only extras (nc-step6-run04 ..., nc-step6-run06 ...) did not recur" | Same two failures in the NC 20; not in the 31 run 3 boots | yes on the count; no issue is cited for either, and none exists (row 3) |
| "removal-step6-run13.txt task_lifecycle met after 9998 us (deficit 192 pages ...)" | 9998 us, deficit -192 | yes |
| "the other 19 removal boots met after 2 or 3 us and the 11 NC boots after 6 to 14 us" | 2 to 3 us; 6 to 14 us | yes |
| Image sha256 and sizes for both step-6 images | 5c10222e... 8733328 B; 41e4319f... 8778384 B | yes |
| "test_task_lifecycle failed in 0 of 20 boots (removal-1211-run01..20.txt)" | 0/20 | yes |
| "[C1-1211] line read PASS in 20 of 20 and no '[C1-1211] FAIL repoll' line" | 20/20 PASS, 0 repoll lines | yes |
| "deficit at window start 64 to 320 pages"; "window lasted 1688 to 1691 us" | -64 to -320; 1688 to 1691 | yes |
| "completed 9776 to 9842 us after dispatch" | 9776 to 9842 | yes |
| "all on CPU 1 in 17 of 20, all on CPU 2 in 1 of 20 (run20), 7 on CPU 2 and 1 on CPU 1 in 1 of 20 (run13) and on CPUs 1, 2 and 3 in 1 of 20 (run11)" | Same | yes |
| "Classification: not reproduced (0 of 20)" | Follows from the rows | yes |
| Branch content: edadfb2e = 375ad4dd re-applied, conflicts resolved to 375ad4dd's code, other #1215 conversions kept; 9f375c19 reverts only 13cdb4ec's lifecycle conversion | Same (section 1) | yes |
| "#1202 wait ... failed in 8 of 20 (run03, 07, 08, 10, 11, 12, 13, 20); ... first true 364 to 3032 us after expiry" | 8/20, same runs, 364 to 3032 us | yes |
| "The count-bounded #1205 wait passed in 20 of 20" | 20/20 | yes |
| "QEMU: make test passed at that tree (... banner edadfb2e-dirty ...)" | No evidence file named for this run | not checked; no file behind it, labelled dirty by the builder; not a definition-of-done item |

Process disclosed in the summary files: power_off, sdwire_info, sdwire_update,
boot_test with per-call output_dir, call times stamped by a shell command; no
ssh, no labctl database reads, no private helpers mentioned. Nothing in the raw
files contradicts that. No redefinition argument appears in either summary file.

The comparison does not change the verdict: **fail**, definition-of-done row 3,
`run2-hw/c1/nc-step6-run04.txt` (`test_slm_stop_from_second_session_during_prompt`)
and `run2-hw/c1/nc-step6-run06.txt`
(`test_proactive_load_balance_runtime_config_aggressive`) have no issue.
Rows 1 and 2 are met and reproduced.

## 7. Row 3 re-check (after #1220, #1221)

Requested by the orchestrator at 07:41 UTC; done offline, no labctl call. Rows 1
and 2 stand as written in section 5 and were not re-opened. The issues were read
with `gh issue view` (title, state, labels, milestone, body). Both were created
at 07:35 UTC, after the critic's issue search in section 5 (about 07:26 UTC),
which is why that search found neither.

| Check | #1220 | #1221 |
|---|---|---|
| Names its test | `test_slm_stop_from_second_session_during_prompt` (title and body) | `test_proactive_load_balance_runtime_config_aggressive` (title and body) |
| Names the evidence file | `docs/archive/coherency-gauntlet/run2-hw/c1/nc-step6-run04.txt` on `gauntlet-2-hw-pi5` | `docs/archive/coherency-gauntlet/run2-hw/c1/nc-step6-run06.txt` on `gauntlet-2-hw-pi5` |
| Labels | bug, platform:pi5, sub:shell, P3-low | bug, platform:pi5, sub:sched, P3-low |
| Milestone | Backlog | Backlog |
| State | OPEN | OPEN |
| Banner quoted | `SLM-OS v0.4.0 build 20261110091028 (30dc24bc)`: matches line 3 of the file | Same: matches line 3 of the file |
| Failure text quoted | File lines 6763 to 6765: match the file verbatim (`test_slm_shell.c:680`, `stop=stopped`) | File lines 7404 to 7409: match the file verbatim (`test_scheduler.c:4380`, Expected 0, Actual 3) |
| Observed rate stated | NC 1 of 20 (`run2-hw/c1/nc-step6-run01..09.txt`, `run3-hw/c1/nc-step6-run10..20.txt`); removal 0 of 20 (`run3-hw/c1/removal-step6-run01..20.txt`) | Same files, NC 1 of 20, removal 0 of 20 |
| Rate against the files | grep for `[FAIL] <test>` over the 40 step-6 files and the critic's 10 reproduction files: one file, `nc-step6-run04.txt`; `[PASS] <test>` in the other 49. NC 1/20 and removal 0/20 hold | grep the same way: one file, `nc-step6-run06.txt`; `[PASS]` in the other 49. NC 1/20 and removal 0/20 hold |

One remark, not a defect against row 3: both issues give the critic's
reproduction result as "the orchestrator reports 0 failures" with no
filename. The files show it: 0 of 10 for each test in
`run3-hw/critic-c1/nc-repro-run01..05.txt` and `removal-repro-run01..05.txt`
(`gauntlet-3-hw-pi5` @ 9352c948).

Row 3 is now met. The two failures beyond the 18 in the step-6 boots are
counted (section 1), named and filed: #1220 and #1221, each with its evidence
filename.

Verdict for the whole component after the re-check: **pass**. Row 1 holds:
`run2-hw/c1/nc-step6-run01..09.txt` and `run3-hw/c1/nc-step6-run10..20.txt` for
NC, `run3-hw/c1/removal-step6-run01..20.txt` for the removal, reproduced in
`run3-hw/critic-c1/{nc,removal}-repro-run01..05.txt`. Row 2 holds:
`run3-hw/c1/removal-1211-run01..20.txt`, not reproduced, 0 of 20. Row 3 holds:
#1220 and #1221. Section 5's fail is superseded by this section. It is recorded
that row 3 was met only after the critic's first verdict.

## 8. #1213 (QEMU, not a gate input)

Requested by the orchestrator at about 08:00 UTC; offline, no labctl call, no
QEMU run. Files: `gauntlet-3-hw-pi5` @ 6569a660,
`run3-hw/c1/qemu-1213-*.txt`. `qemu-1213-summary.txt` was not opened until
8.1 to 8.3 were written.

### 8.1 Head and method checks

- `origin/c1/isolated-1s` = f8ff72d6, parent a03fc75b. `git diff a03fc75b
  f8ff72d6` is identical to the diff in run 2's
  `run2-hw/c1/qemu-isolated-1s-f8ff72d6.patch` (`gauntlet-2-hw-pi5`), apart
  from one trailing blank line in the patch file: both
  `test_isolated_core_latency` waits (isolated and normal phase) become
  `test_wait_task_terminated(..., TEST_WAIT_S(1))`.
- First line of every run file (15 single, 15 multi, 10 multi-trace, 1
  self-test) names `c1/isolated-1s head f8ff72d6`; all 41 give ELF sha256
  6776d68c0a14...f7b1. `qemu-1213-maketest-build-f8ff72d6.txt` is the build's
  `make test` log (head f8ff72d6).
- `qemu-1213-sampler.sh.txt` holds run.sh, all.sh, sampler.py, trace.sh,
  gdbtrace.py, analyze.py, table.py. The QEMU command is the `make test` one
  plus `-accel tcg,tb-size=128,thread=<mode>`, a QMP socket and a gdbstub,
  under the same systemd-run/timeout 120 guard. The sampler fires at the first
  `iso_latency_isolated: TIMEOUT` line and takes 10 samples, 200 ms apart, of
  every vCPU's PC, x30 and `current_task[cpu]`, symbolized with addr2line and
  objdump, then one gdbstub read of system registers.
- Sampler versions: `multi-run02` and `multi-run03` have no x30 lines, no
  `info registers` dump and no gdbstub block (earlier sampler); runs 07, 12,
  14, 15 have all three.
- The critic built f8ff72d6 (`make kernel-test`, QEMU ARM64; banner
  `f8ff72d6-dirty` from the untracked verdict file only) to disassemble
  `test_wait_until_v`; the instructions after both sampled CPU 0 return
  addresses match the files' objdump text (`sub x2, x0, x23` at 0x4007d1c4,
  `mov x27, x0` at 0x4007d288), so the layout is the same.

### 8.2 Runs and stalls, counted from the files

| Mode | Files | Runs | Stalled (an `iso_latency_isolated: TIMEOUT` line) | QEMU rc 124 |
|---|---|---|---|---|
| thread=single | qemu-1213-single-run01..15.txt | 15 | 0/15 | 0/15 (rc 0, isolated 10/10 and normal 10/10 in 15/15) |
| thread=multi | qemu-1213-multi-run01..15.txt | 15 | 6/15: runs 02, 03, 07, 12, 14, 15 | 6/15, the same six |
| thread=multi, gdb attached from boot (trace) | qemu-1213-multi-trace-run01..10.txt | 10 | 0/10 | 0/10; breakpoint on the EL1h sync vector hit 0 times in 10/10 |
| self-test (trigger on the summary line) | qemu-1213-sampler-selftest-multi-run99.txt | 1 | not a stall run | rc 0 |

Isolated-phase TIMEOUT lines per stalled run: 9 (02), 3 (03), 2 (07), 6 (12),
8 (14), 2 (15). In 6/6 the first one reads `value at bound 1` (task state 1,
RUNNING) at the bound and at +1, +10, +100 and +1000 ms. In 6/6 the stall is
the first isolated-phase iteration after `CPU 1: isolated from general
scheduling` (line 6557 in each file).

Sampling: 10 samples of all 4 vCPUs in 6/6, spanning 2.95 s (02, 03) to 4.0 s
(07, 12, 14, 15) by their timestamps.

### 8.3 Per stalled run

| Run | CPU 1 PCs (count of 10) | CPU 1 PSTATE | CPU 1 function | CPU 1 x30 | CPU 1 SP / ESR / FAR / ELR (sample-01 `info registers`; gdbstub after sampling) | `current_task[1]` | CPU 0 PC; return addresses |
|---|---|---|---|---|---|---|---|
| multi-run02 | 0x40010f94 x8, 0x40010a00 x2 | 0x3c5 (EL1h, DAIF masked) 10/10 | `el1_sync`+4 (`stp x0, x1, [sp]`), `exception_vectors`+0x200 (`b el1_sync`) | not recorded | not recorded | 0x407ce4f0 10/10; reads lat_iso id 180/181, READY | 0x4002fda8 `timer_get_count` 10/10; x30 not recorded |
| multi-run03 | 0x40010a00 x5, 0x40010f94 x5 | 0x3c5 10/10 | same | not recorded | not recorded | 0x407ce4f0 10/10; lat_iso 186, lat_norm 187, READY | `timer_get_count` 10/10; not recorded |
| multi-run07 | 0x40010f94 x5, 0x40010a00 x5 | 0x3c5 10/10 | same | 0x40017ca0 `task_entry_wrapper` 10/10 | SP 0xffffffffa2dff600; ESR 0x96000045, FAR = SP_EL1 = 0xfffffffee23e0860, ELR 0x40010f94 | 0x407ce4f0 10/10; lat_norm 187 to 189, READY | `timer_get_count` 10/10; x30 0x4007d1c4 x6, 0x4007d288 x4 (`test_wait_until_v`); gdb bt `timer_get_count` <- `test_wait_until_v` <- `test_isolated_core_latency` |
| multi-run12 | 0x40010f94 x9, 0x40010a00 x1 | 0x3c5 10/10 | same | `task_entry_wrapper` 10/10 | SP 0xffffffffa1504740; ESR 0x96000045, FAR = SP_EL1 = 0xfffffffede0619e0, ELR 0x40010f94 | 0x407ce4f0 10/10; lat_iso 183 to 185, READY | same; x30 0x4007d1c4 x6, 0x4007d288 x4 |
| multi-run14 | 0x40042b98 x6 (`wfi`), 0x40042b9c x4 (`b panic+0x1f4`) | 0x600003c5 10/10 | `panic` (WFI loop) | 0x40042b94 `panic` | SP 0x421ffed0; ESR 0x0, FAR 0x0; gdb bt `panic` <- `task_exit` <- `task_entry_trampoline` | 0x407ce4f0 10/10; lat_iso 181 to 183, READY | same; x30 0x4007d1c4 x5, 0x4007d288 x5 |
| multi-run15 | 0x40010a00 x7, 0x40010f94 x3 | 0x3c5 10/10 | `el1_sync`/vector | `task_entry_wrapper` 10/10 | SP 0xffffffffa6739c40; ESR 0x96000045, FAR = SP_EL1 = 0xfffffffee3a396c0, ELR 0x40010f94 | 0x407ce4f0 10/10; lat_norm 187 to 189, READY | same; x30 0x4007d1c4 x5, 0x4007d288 x5 |

`multi-run14.txt` also prints, at the stall (line 6560 on), `KERNEL PANIC`
`task_exit: schedule returned!` for task 180 `lat_iso` on CPU 1, SP 0x421ffed0
inside that task's stack [0x421c0000..0x42200000), ELR 0x4001a8c4, all task
canaries intact. No other run file has a `KERNEL PANIC` line.

Reading of the rows:

- CPU 1's PC moves in 6/6, but only inside a two-instruction cycle: the EL1h
  synchronous vector and `el1_sync`+4 in 5/6, `panic`'s WFI loop in 1/6. It
  never returns to task or scheduler code in any sample.
- In the 3 runs with registers (07, 12, 15): ESR 0x96000045 is a data abort
  taken at EL1 (EC 0x25), write, level-1 translation fault; FAR equals SP_EL1;
  ELR is `el1_sync`+4, the `stp` to the stack after `sub sp, sp, #0x320`. SP
  lies far outside RAM and is lower at the gdbstub read than at sample 01 in
  each run (by about 3 GB in 07). CPU 1 is re-entering its own exception
  vector on an invalid stack, each entry 0x320 bytes lower. The first fault is
  not in any file: the trace runs that were to catch it never stalled.
- RUNNING task on CPU 1: no sampled task is RUNNING. `current_task[1]` points
  to the same struct (0x407ce4f0) in all 60 samples, and the task awaited at
  the first timeout was RUNNING (state 1) at the bound in 6/6. Later reads of
  that struct show newer lat_iso/lat_norm tasks in READY state, so the
  struct was reused by later iterations while CPU 1 stayed in its loop.
- CPU 0 executes its poll loop with `yield()` between polls in 4/4 runs with
  x30: 0x4007d1c4 is the main-loop poll (`now - start`, then `bl yield`) and
  0x4007d288 the post-timeout re-poll loop (also `bl yield`); neither loop
  contains WFE or WFI. Every CPU 0 PC sample is in `timer_get_count`, so the
  `yield()` call itself is not sampled; the two alternating call sites and
  the repeating TIMEOUT lines show CPU 0 passing through iterations. Runs 02
  and 03 have no x30, so for them this is from the PC and the code only.
- Kernel fault or TCG artifact: the rows show a guest-visible kernel failure
  on CPU 1, a recursive data abort on an invalid SP in 5/6 and a kernel panic
  `task_exit: schedule returned!` in 1/6, not a vCPU that is merely not being
  scheduled by the host. Thread mode: 6/15 under thread=multi, 0/15 under
  thread=single, 0/10 under thread=multi with gdb attached. Outcome that
  follows: a kernel hang/fault on isolated CPU 1, seen only under MTTCG; a
  TCG fault cannot be excluded from these files, and the cause of the first
  fault is not observed.

Numbers that cannot be traced to a file: none in 8.1 to 8.3 other than the
critic's own build (banner `f8ff72d6-dirty`, local, not committed), used only
to map two return addresses.

### 8.4 Builder's summary compared (read after 8.1 to 8.3 were written)

Read: `run3-hw/c1/qemu-1213-summary.txt` (`gauntlet-3-hw-pi5` @ 6569a660).

| Builder's number or claim | Critic | Agree |
|---|---|---|
| Head f8ff72d6 = a03fc75b + one TEMPORARY commit; patch identical to run 2's | Same (8.1) | yes |
| ELF sha256 6776d68c...f7b1, same ELF in every run | 41/41 files | yes |
| thread=single 0 of 15; brief's set 0 of 5 | 0/15 | yes |
| thread=multi 6 of 15 (02, 03, 07, 12, 14, 15); brief's set 2 of 5 (02, 03) | Same | yes |
| multi with gdb from boot 0 of 10; 0 breakpoint hits | 0/10, 0 hits | yes |
| Per-run table, 40 rows: rc, isolated/normal, TIMEOUT line counts (9, 3, 2, 6, 8, 2), first state at bound 1, CPU 1 PCs and functions, CPU 0 PC, gdbstub ESR/FAR/ELR/SP_EL1 | Same values in every row checked (all 6 stalled rows field by field; the 34 others by rc, sample counts and TIMEOUT count) | yes |
| 5 of 6 alternate 0x40010a00 / 0x40010f94; run14 in `panic` after `task_exit: schedule returned!` (task 180, lat_iso, CPU 1) | Same | yes |
| PSTATE 0x3c5 in the 5 loop runs, 0x600003c5 in run14 | Same | yes |
| ESR 0x96000045, FAR = SP_EL1, ELR = `el1_sync+4` in run07, 12, 15 | Same | yes |
| SP_EL1 = 0 mod 0x320 in 6 of 6 readings, 1.88 to 6.08 million steps below 0 | Recomputed: remainder 0 in 6/6; 1.878 to 6.081 million | yes |
| CPU 1 sample-01 registers identical in run07, 12, 15: x19 = x0 = `latency_task_entry`, x20 = 0, x29 = 0, x30 = `task_entry_wrapper` entry, x2 = 0x407ce510, x3 = 0x80 | Same values in 3/3; 0x40111c80 = `latency_task_entry` and 0x40017ca0 = `task_entry_wrapper`'s entry in the critic's f8ff72d6 build | yes |
| `task_entry_trampoline`'s first instruction is `stp x29, x30, [sp, #-32]!` | Same at 0x400187a8 in the critic's build | yes |
| First timed-out isolated task RUNNING at the bound in 6 of 6; `current_task[1]` 0x407ce4f0 in every sample, slot reused | Same | yes |
| CPU 0 ran `main` in 60/60, PC `timer_get_count` 60/60, x30 in `test_wait_until_v` 40/40, gdb bt 4/4; `bl yield` every pass, no WFE/WFI | Same (22 + 18 = 40 x30 samples) | yes |
| CPUs 2 and 3 in `idle_task_func` after `wfi` in every sample | 60/60 each | yes |
| "Attaching gdb changes the timing enough to hide the stall" | 0/10 under gdb is measured; the reason is not | wording: this is a hypothesis stated as a cause |
| Outcome: "a kernel fault on CPU 1 that is exposed only by truly parallel vCPUs" | Follows from the rows (8.3) | yes |
| Outcome: "CPU 1 switched into a task whose saved SP was 0 while its x19, x20, x29, x30 and DAIF already held a new task's initial values" | The register image is measured; "saved SP was 0" is inferred from the 0 mod 0x320 remainders, which any SP that is a multiple of 0x320 would also give. Strong, but an inference listed under "Measured outcome" | wording; does not change the outcome class |
| Hypothesis (slot reused while CPU 1 is still leaving the old task; two destroyers; `task_create` store order against `switch_to` load order) | Labelled untested and not worked | recorded, not judged |
| Discarded first multi run01 attempt at 06:59 (QMP socket path too long) | No file; not a count | recorded |

### 8.5 Verdict for #1213 (not a gate input)

**pass**. Every item of the definition of done is met from the files:
at least 5 runs per mode (15 thread=single, 15 thread=multi); 6/6 stalled
runs carry 10 symbolized PC samples of all 4 vCPUs over 2.95 to 4.0 s; each
stalled run gives whether CPU 1's PC moves (yes, inside the fault or panic
loop only), its function (`el1_sync`/EL1h sync vector in 5, `panic` in 1),
the RUNNING task (the first awaited lat task, state 1 at the bound, struct
0x407ce4f0 later reused), and CPU 0's loop (`yield()` between polls, no WFE or
WFI; for multi-run02 and 03 from PC and code only, as the summary states);
and the outcome sentence (kernel fault on isolated CPU 1, thread=multi only,
6/15 against 0/15) follows from the rows. Two wording points are recorded
above (gdb hiding the stall, SP = 0 under "measured"); neither redefines the
bar or changes the class. The first fault was not observed, and #1213 stays an
open kernel defect.
