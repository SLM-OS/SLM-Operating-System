# Coherency gauntlet log, run 3 (#1193, #1216)

Goal: answer three questions and open the pull request the answers allow.
(1) Does the NC kernel on main show #1216 under sustained load
(pre-existing or exposed by the removal)? (2) Is #1216's mechanism the
wake path (H3, a hypothesis until cell B's rows are in)? (3) Does the Pi 5
bar close at 20 of 20 under both kernels, and is #1211 timing or
something else? The brief is the user's message of 2026-11-10 (PT); this
log is append-only, a correction is a new entry citing the one it
corrects, and every entry carries its own write time.

Boards: pi-5-1 and jetson-nano-2 only. Evidence: `gauntlet-3-hw-pi5` and
`gauntlet-3-hw-jetson`, one file per run under
`docs/archive/coherency-gauntlet/run3-hw/c<N>/`. Rules:
`run3-rules.md`; critics: `run3-critic-protocol.md` (both in this
directory, deltas over run 2's).

## Budget

- Started: 2026-11-11 03:45:58 UTC (orchestrator's first action). Limit:
  10 h, 13:46 UTC.
- Hour marks (UTC): C0 done 04:16; C1 lab 04:16 to 06:31; C1 critic
  reproduction 06:31 to 07:01; C2 step 0 07:01 to 07:31; C2 cells 07:31
  to 11:16; C2 critic reproduction 11:16 to 12:01; gate at 12:16 at the
  latest; C3 and close-out 12:16 to 13:46.
- A component not at its mark loses the lab at the next handover and ends
  "open" with its last result.
- Stop conditions for the run: both boards unreachable for more than
  45 min after the #1195 round-trip rule; any proposal to flash BL31,
  patch TF-A or touch another board; budget exhausted. Per-component stop
  conditions are in `run3-rules.md`.

## Entry 0 (written 04:05 UTC, completed in Entry 0a): hour-0 checks

Read in the brief's order: ADR 0007 on `fix/boot-path-1193`; run 2's
close-out, wrap-up addendum and Entries 14 to 30; Entry 24's branch
table; #1216, #1213, #1211, #1202, #1205, #1203. Nothing is re-derived.

1. **Reminder self-test: failed; reminders are not used this run.**
   `send_later` at 03:45:58Z for 03:48 (trig_01AofyDRYbDcknzfs71mWwVw).
   It fired at 03:49:02Z with `last_run` FAILED,
   `ROUTINE_RUN_FAILURE_REASON_ACCESS_DENIED`, while this session was in
   an active turn, not waiting on a prompt. This weakens run 2's Entry 19
   hypothesis that a reminder fails only into a session blocked on a
   prompt; the cause is not visible from the session. The orchestrator
   keeps time from `date` and polls builders with background Bash and
   Monitor. `delete_trigger` was exercised on it at about 04:01Z with no
   prompt (it was still `enabled` with `next_run_at` 2026-11-12, as run 2
   saw).
2. **First use of every tool family**, all without a prompt:
   `list_triggers`, `get_trigger`, `delete_trigger`; `gh pr list`,
   `gh issue view 1216`; labctl `list_claims`, `claim_sbc`,
   `run_health_check`, `bindings_list`, `actuation_status`,
   `release_sbc`, `get_claim`, `list_images`, resource `lab://sbcs`;
   ssh to jetson-nano-2's SD Linux (read-only `uname`, `ls /root`);
   `git push` with and without a lease (item 5); background Bash. Agent,
   SendMessage, Monitor, `gh pr edit` and `gh issue comment` are
   exercised before the user is told to leave (Entry 0a).
3. **Lab state (03:46Z).** No active claims. pi-5-1: online, ping,
   serial, power on, no bindings. jetson-nano-2: online, ping, serial,
   power on, at SD Linux (5.15.148-tegra, `slmos-kexec` installed);
   `recovery_mode` binding desired `released`, relay channel 1
   `last_state` open. Both claimed and released by
   `coherency-3-orchestrator` for the health check.
4. **Log and evidence branches.** `gauntlet/coherency-3` from
   `gauntlet/coherency-2` @ b6f0beed; `gauntlet-3-hw-pi5` and
   `gauntlet-3-hw-jetson` from main 85f51ffd.
5. **The stack (heads at 03:46Z).** main 85f51ffd (nothing merged since
   2026-11-09); #1208 `fix-boot-1194-1197-1198` ad8e4fa7 (not touched);
   #1209 `fix/boot-path-1193` 59a85926; #1215 `c1/test-wait-helper`
   a03fc75b (three commits: 4141843a, 30dc24bc, a03fc75b); #1217
   `c2/steal-instrument-1203` 983bddc9; #1218 90f0d9e3; removal
   `coherency-remove-nc-axis` e94c251a on `c1/boot-path-plus-helper`
   63eed0c4. None merged, so the base is the stack, not main.
   - New base `c1/boot-path-plus-helper` **638e297d** = 59a85926 plus
     #1215's three commits cherry-picked (3675de05, 13cdb4ec, 638e297d).
     The old base carried only #1215's first two; the third adds
     `docs/testing.md` only. Pushed with a lease on 63eed0c4.
   - The removal range has 40 commits including 5 merges from run 1, whose
     hand-resolved conflicts neither a plain rebase nor `--rebase-merges`
     replays (both stopped on conflicts in `kernel/src/coherency.c`,
     `kernel/tests/test_coherency.c`, `docs/shell.md`). The base delta
     (63eed0c4 to 638e297d) touches only `docs/adr/0007-cross-cpu-coherency.md`
     and `docs/testing.md`, no removal commit touches either, and the range
     has one root (0f310422, parent 63eed0c4). So every commit was
     re-parented with its tree's two files replaced by the new base's copies
     (`run3-tools/c0/graft.sh.txt`), which is the result a conflict-free
     rebase gives. Checks: the new head descends from 638e297d and 59a85926;
     `git diff <base> <tip>` is byte-identical for old and new; old tip and
     new tip differ only in those two files (+14, +26 lines); 40 commits, 5
     merges.
   - **Removal head for every Pi 5 count and every removal cell this run:
     `coherency-remove-nc-axis` @ 79c10261** (pushed with a lease on
     e94c251a).
   - /ci on 79c10261 (`run3-tools/ci-79c10261/summary.txt`, logs beside
     it): steps 1 to 10 passed (ARM64, x86-64 and Jetson real-weights
     builds; `make test` ARM64, x86-64 and `AI_SCHED=ON`; runtime-test 308
     and 328 passed, 0 failed; check-braces 0 violations, 0 unparsed, 12
     configurations, one "not checked" host-tool header under `scripts/`;
     build order; Jetson UEFI layout `All checks passed.`, no `!` lines;
     diff check, no artifacts). `make test WORK_STEALING=ON`: attempt 1 at
     04:11Z, run while the orchestrator's Jetson cell builds ran, failed
     (`[FAIL] batch: stress workload (4x8) completes + rejects bad args`,
     then the 120 s cap, signal 15;
     `11-test-work-stealing-attempt1-concurrent-builds.log`); attempt 2 at
     04:18Z, alone, `PASSED - All tests passed (exit code 0)`, 0 `[FAIL]`
     lines. Recorded as green on attempt 2, with attempt 1 kept and the
     load explanation labelled a hypothesis (load average was about 2.8 on
     8 cores, which is not heavy). See item 6 for a second occurrence.
6. **Instrumentation head: `c2/steal-instrument-1203` @ 1b27eea4** (PR
   #1217, fast-forward from 983bddc9; body updated). One commit:
   victim-side `preempt_disabled` windows mirrored at every ARM64 write
   site (schedule()'s switch window and resume, coop tick,
   scheduler_start, task_entry_wrapper, the SECONDARY_PREEMPT trampoline)
   with reasons T, C, P, O, B; per-CPU hardware tick times from
   `sched_note_hw_tick()`; per-thief position of each attempt against CPU
   1's window and tick and its own tick; c2t format v2 from the post-run
   snapshot. No branch changed; no lock, atomic or print on the steal path.
   The first version, 26063dfd (never pushed), failed to build without
   work stealing (`STEAL_WIN_BOOT` undeclared, `instr-gate-26063dfd`);
   1b27eea4 includes `steal_trace.h` unconditionally, and every cell was
   rebuilt from it. Gate on 1b27eea4 (`run3-tools/c0/instr-gate/`):
   `make test` default PASSED; `WORK_STEALING=ON` PASSED; `WORK_STEALING=ON
   AI_SCHED=ON` PASSED; x86-64 `WORK_STEALING=ON` PASSED; on cell A's head
   6608cbd0 `NC_AXIS=OFF` PASSED (the switch exists only on run 1's
   lineage, not on main, so this is where it was tested). An extra
   combination, `NC_AXIS=OFF WORK_STEALING=ON` on 6608cbd0, failed twice:
   attempt 1 the same batch-stress failure and 120 s cap as /ci attempt 1;
   attempt 2 (load average 1.3) `test_litmus_patterns_pass`,
   `test_coherency.c:594`, `mp_weak_rows` 1 where 0 is expected (a reader
   that never saw its row in progress; `forbidden` and `timeouts` were 0).
   A control on cbab2fc6 (run 2's cell without this commit) is recorded
   in Entry 1.
7. **Cell images, built 04:03 to 04:27Z** (`run3-tools/c0/cells-MANIFEST.txt`,
   images in the orchestrator's scratchpad `cells/`; one Jetson build per
   cell serves kexec (`.elf`) and UEFI-direct (`.bin`); the banner string
   is what `cat /sys/version` prints):

   | Tag | Cell | Head | Vars | elf sha256 | bin sha256 |
   |---|---|---|---|---|---|
   | fixboot-59a85926 | C2 step 0 | 59a85926 | | 556bb724 | f5df040b |
   | nc-instr | C (kexec), F (UEFI) | 1b27eea4 | | 77803fb2 | 405c7b43 |
   | removal-instr | D (UEFI), E (kexec) | 8fc7c88a = 79c10261 + 983bddc9 + 1b27eea4 | | d740b83f | 061645b3 |
   | ncoff-instr | A | 6608cbd0 = cbab2fc6 + 1b27eea4 | NC_AXIS=OFF | 48eb2cc2 | eec25e09 |
   | ncoff-instr-v1 | B | 579b155a = 6608cbd0 + V1 | NC_AXIS=OFF | d325404d | a75e8850 |
   | removal-instr-v1 | G | 0dfaae0c = 8fc7c88a + V1 | | 94cb467a | 54c4975b |

   Cell A uses run 2's exact NC_AXIS=OFF code (cbab2fc6, 41 commits behind
   main) so its rows compare with run 2's 20 files; B is A plus V1 only.
   **V1 = `c2/wake-on-push-v1` @ 292eee0a**: the idle wait compiled on Pi 5
   and on Jetson with JETSON_HW_TICK (the SECONDARY_PREEMPT branch of
   `idle_task_func`) goes from `wfi` to `wfe`. The function has four other
   `wfi`/`wfe` branches, all preprocessor alternatives for other
   configurations; QEMU compiles none of them. The orchestrator chose V1
   on that basis; C2 confirms or overrides before the first V cell. The
   bare removal image for C3 is built later, offline. Pi 5 images are
   built by C1 (two heads, independent of the instrumentation).
8. **Lab schedule (single lane).** C1 holds pi-5-1 from 04:19Z (started
   before the end of C0, because its builds and lab work do not depend on
   the instrumentation; recorded as an orchestrator decision) until its
   critic's reproduction is done; then C2 holds jetson-nano-2 until its
   critic's reproduction is done; then C3. Nobody else calls labctl while
   another component holds the lab. C2 prepares offline until its grant;
   C1 runs #1213 in QEMU after its lab work. Handovers are log entries.
9. **Milestone for issues filed this run: Backlog.**

## Entry 1 (04:47 UTC): C2 offline report; bar rulings before any Jetson data

- C2 builder offline report received 04:45Z (no labctl call, no board
  ssh). V1 confirmed, no V2: C2 disassembled `idle_task_func` in every
  manifest image; each has exactly one wait instruction, and the V1 images
  differ from their bases only there (`wfi` to `wfe`). On the ncoff and
  removal bases the branch condition stays true under NC_AXIS=OFF. QEMU
  compiles none of the platform branches; `make test` on 292eee0a PASSED.
  C2 notes that V2 as worded (an IPI to the target CPU) would wake CPU 1,
  the owner, not the idle thieves. Runner, analyzer (`analyze.py`,
  `bars.py`) and procedure notes are in the orchestrator's scratchpad
  `c2/bin/`, to be archived under `run3-tools/c2/`. The analyzer
  reproduces #1216's run-2 aggregate table exactly from
  `ncoff-kexec-sustained-run01..20.txt` (shares 11.1, 11.4, 19.8, 19.8%;
  f_pd 75.3 and 74.6%; first success 214.6 and 229.6 ms; CPU 3 ok=0 in
  run 16 only). QEMU smoke at 1b27eea4 with `make run WORK_STEALING=ON`
  printed a complete c2t v2 block (4 lines, CPU 1's 2509 chars with 128
  `wlog` entries).
- **Orchestrator rulings, made before any run-3 Jetson row exists** (they
  interpret the brief's bars; they do not change them):
  - Question 1, share clause: "CPUs 2 and 3 at a median share no better
    than 60% of the best thief's" holds only if each of CPU 2 and CPU 3
    is at or below 60%; both ratios are printed. The 0-success clause is
    an independent "or", as written. (On run 2's rows the reading
    matters: CPU 2 0.545, CPU 3 0.602; run 2's run 16 decides
    "pre-existing" through the 0-success clause either way.)
  - Question 2, clause 1 (shares within 10 points): applied as written.
    Recorded: on run 2's NC_AXIS=OFF rows it already holds (largest gap
    8.95 points), so it does not discriminate; the decision rests on the
    other three clauses, all of which are required.
  - Phase tables carry a uniform-phase reference row (log-spaced bins: 50%
    in 5 to 10 ms, 2% within 200 us of the tick) so "clustered" is read
    against it.
- Instrumentation gate, `NC_AXIS=OFF WORK_STEALING=ON` (not a gate input):
  6608cbd0 failed 2 of 3 (batch stress plus cap; `mp_weak_rows` 1) and
  passed attempt 3 at 04:45Z; control cbab2fc6 passed attempt 1 at 04:42Z
  (`run3-tools/c0/instr-gate/status.txt`). A second control is running.
- 04:48Z: second control on cbab2fc6 PASSED. Totals for `NC_AXIS=OFF
  WORK_STEALING=ON` QEMU: 6608cbd0 1 of 3 passed, cbab2fc6 2 of 2 passed
  (`run3-tools/c0/instr-gate/status.txt`; `5-...-attempt1.log` is attempt
  1, `5-cellA-test-ncoff-ws.log` attempt 2, `8-...` attempt 3). Too few
  runs to tell the instrumentation from the base; not a gate input, and
  cell A is a Jetson measurement build, not merge code. The batch-stress
  failure with the 120 s cap occurred on 79c10261 too, which has no
  instrumentation, so it is not the instrumentation's; filed (Entry 2).
  The `mp_weak_rows` failure occurred once in 3 on run 1's lineage only;
  recorded, not filed.

## Entry 2 (04:50 UTC): filed #1219 (file-don't-fix)

- #1219: QEMU `make test WORK_STEALING=ON`, `batch: stress workload
  (4x8)` fails intermittently and the suite then hits the 120 s cap; two
  occurrences (79c10261 /ci attempt 1; 6608cbd0 gate attempt 1), both
  overlapping other host builds; same configurations passed without the
  overlap. Labels bug, platform:qemu, sub:ai-runtime, P3-low; Backlog.
  Not worked this run.

## Entry 3 (04:50 UTC): C3's bare removal image built

- `removal-bare-79c10261`, head 79c10261, default Jetson vars, banner
  `build 20261111044836 (79c10261)`; elf sha256 502f78ba, bin sha256
  1ef9dc39 (`run3-tools/c0/cells-MANIFEST.txt`). This is the image C3
  boot-tests on both Jetson paths.

## Entry 4 (06:55 UTC): C1 builder done on pi-5-1; lab to the C1 critic

- C1 builder released pi-5-1 at 06:49:47Z (claimed 04:20Z) and reported.
  Evidence `gauntlet-3-hw-pi5` @ 127428e7, `run3-hw/c1/` (51 raw captures
  and two summaries); new branch `c1/instrument-1211-removal` @ 9f375c19.
  Its counts go into the log after the critic files, as the protocol
  requires (Entry 5 or later).
- 06:54Z: pi-5-1 handed to the blind C1 critic (claims as
  `coherency-3-C1-critic`), with the definition of done, the issue
  numbers, the evidence path and the heads only. It reproduces 5 boots
  per kernel from the staged images, then restores the card's originals
  (`*.prec3`) and leaves pi-5-1 at the SLM-OS shell, powered on.
- 06:55Z: C1 builder resumed offline for #1213 (QEMU only, no labctl).
- Schedule: C1's lab work ended 18 min after the 06:31 mark (the mark
  assumed the lab from 04:16; C1 claimed at 04:20). C2's step 0 moves
  from about 07:01 to after the critic, expected about 07:30.

## Entry 5 (07:27 UTC): C1 critic verdict; jetson-nano-2 to C2

- Correction to Entry 4: its times were written at 06:54Z; the critic
  handover was at about 06:53Z, not 06:54 or 06:55.
- **C1 critic verdict (`critic-3-C1.md`): fail, definition-of-done row 3
  only.** Row 1 met and reproduced: NC 20/20 (`run2-hw/c1/nc-step6-run01..09.txt`
  on `gauntlet-2-hw-pi5`, banner 30dc24bc, kernel/runtime diff to a03fc75b
  empty; `run3-hw/c1/nc-step6-run10..20.txt`, banner a03fc75b), removal
  20/20 (`run3-hw/c1/removal-step6-run01..20.txt`, banner 79c10261), the
  three named tests passing in all 40, 0 `[test_wait]` timeout lines.
  Reproduction 5/5 NC and 5/5 removal (`run3-hw/critic-c1/{nc,removal}-repro-run01..05.txt`,
  `gauntlet-3-hw-pi5` @ 9352c948). Row 2 met: #1211 0 of 20
  (`run3-hw/c1/removal-1211-run01..20.txt`, banner 9f375c19), so "not
  reproduced" is the only classification that follows; the branch
  reverts only the `test_task_lifecycle` conversion plus the step-1
  instrumentation. Row 3 not met: two failures beyond the 18 known #1109
  ones, in run 2 boots that count toward the NC 20
  (`test_slm_stop_from_second_session_during_prompt`,
  `run2-hw/c1/nc-step6-run04.txt`;
  `test_proactive_load_balance_runtime_config_aggressive`,
  `run2-hw/c1/nc-step6-run06.txt`), have no issue (run 2 recorded them
  "not filed", Entry 16 of run 2). The critic compared every row of the
  builder's summaries with its own parse: 31 + 20 rows, 0 mismatches.
- Action: the C1 builder files the two issues; the critic then re-checks
  row 3 only. The gate reads the re-verdict. No bar changes: the filing
  is the missing deliverable, not a re-measurement.
- The critic restored pi-5-1's card (one `sdwire_update`: test kernels
  renamed aside, `*.prec3` originals back as `kernel_2712.img` and
  `kernel8.img`, renamed test kernels deleted; nothing else touched),
  booted it to `slmos>` (banner b42b2c1d, `run3-hw/critic-c1/restore-boot.txt`),
  left it powered on and released it at about 07:22Z.
- **07:25Z: jetson-nano-2 to C2** (claims as `coherency-3-C2`), with the
  Entry 1 rulings. C2's lab time ends about 11:15Z.

## Entry 6 (07:27 UTC, written after the C1 critic filed): C1 builder report, step 6 and #1211

- Images (`make kernel-test PLATFORM=RASPI5`, staged in the main
  checkout's `build/kernel/`): NC a03fc75b `slmos-r3-c1-nc-a03fc75b.bin`
  8733328 B sha256 5c10222e; removal 79c10261
  `slmos-r3-c1-removal-79c10261.bin` 8778384 B sha256 41e4319f; #1211
  9f375c19 `slmos-r3-c1-removal1211-9f375c19.bin` 8782480 B sha256
  082933c9. pi-5-1 claimed by `coherency-3-C1` 04:20 to 06:49Z; card
  originals kept as `*.prec3`.
- Step 6, interleaved blocks of 3, `boot_test` runs=3, timeout 330 s,
  04:23 to 05:51Z: NC 11/11 (`run3-hw/c1/nc-step6-run10..20.txt`), with
  run 2's 9/9 NC 20/20; removal 20/20 (`run3-hw/c1/removal-step6-run01..20.txt`);
  no failure beyond the 18 in the 31 run-3 boots (`run3-hw/c1/step6-boots.txt`).
  In `removal-step6-run13.txt` the lifecycle wait was met after 9998 us
  (deficit 192 pages at its start); the other 19 removal boots after 2 to
  3 us, the 11 NC boots after 6 to 14 us. Hypothesis (untested): about one
  10 ms tick, a reaping CPU waiting for its tick; it would have failed the
  old 1.69 ms window.
- #1211 on `c1/instrument-1211-removal` @ 9f375c19 (79c10261 + 375ad4dd
  re-applied as edadfb2e + the lifecycle window reverted and instrumented),
  05:51 to 06:49Z: `test_task_lifecycle` failed 0/20
  (`run3-hw/c1/removal-1211-run01..20.txt`, `removal-1211-boots.txt`):
  not reproduced; #1211 stays open and blocks nothing. Free pages at the
  bound back to the initial count in 20/20; window 1688 to 1691 us.
- Side result on the same branch: the re-applied count-bounded #1202 wait
  failed 8/20 (runs 03, 07, 08, 10, 11, 12, 13, 20); in 8/8 the flag was
  observed 364 to 3032 us after the bound expired (timing).
- Evidence `gauntlet-3-hw-pi5` @ 127428e7. One claim without a file (a
  QEMU `make test` pass on the #1211 tree, labelled dirty) is struck from
  this entry, per the critic.

## Entry 7 (07:42 UTC): C1 critic re-check, row 3: C1 passes

- Correction to Entries 5 and 6: both were written at 07:31Z, not 07:27Z.
- 07:35Z: the C1 builder filed #1220 (`test_slm_stop_from_second_session_during_prompt`,
  `run2-hw/c1/nc-step6-run04.txt`; bug, platform:pi5, sub:shell, P3-low)
  and #1221 (`test_proactive_load_balance_runtime_config_aggressive`,
  `run2-hw/c1/nc-step6-run06.txt`; bug, platform:pi5, sub:sched, P3-low),
  both Backlog, both file-don't-fix.
- **C1 critic re-check (`critic-3-C1.md` section 7): pass.** Both issues
  name their test and file, quote the banner and failure lines verbatim,
  and state NC 1 of 20, removal 0 of 20, which the critic confirmed by grep
  over the 40 step-6 files and its 10 reproduction files. Section 5's fail
  is superseded; the verdict records that row 3 was met only after the
  first verdict.
- **C1 at the gate: clear** (the critic counted 20 of 20 under both
  kernels, and #1211 is not reproduced).

## Entry 8 (07:57 UTC): C1 builder #1213 report received; critic check started

- C1 builder finished #1213 at about 07:56Z (no labctl calls); evidence
  `gauntlet-3-hw-pi5` @ 6569a660, `run3-hw/c1/qemu-1213-*`; branch
  `c1/isolated-1s` @ f8ff72d6 (the original run-2 object, not a
  reconstruction, per the builder). Its counts and outcome go into the log
  after the C1 critic's offline check of the same files (assigned at about
  08:00Z, not a gate input), so that #1213 is not updated on a builder
  claim alone.
