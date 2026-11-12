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

## Entry 9 (08:05 UTC, written after the critic's #1213 check): #1213 result

- C1 builder (QEMU only, no labctl), `c1/isolated-1s` @ f8ff72d6 (a03fc75b
  plus the temporary 1 s waits; matches run 2's patch), `make test`'s QEMU
  command with `-accel tcg,tb-size=128,thread=<mode>`, QMP PC sampling (10
  samples of all 4 vCPUs, 200 ms apart, from the first isolated-phase
  TIMEOUT) and one gdbstub read per stall, under the CLAUDE.md guard.
- CPU 1 stopped scheduling in 6/15 runs with `thread=multi`
  (`qemu-1213-multi-run{02,03,07,12,14,15}.txt`), 0/15 with `thread=single`
  (`qemu-1213-single-run01..15.txt`), and 0/10 with `thread=multi` and gdb
  attached from boot (`qemu-1213-multi-trace-run01..10.txt`; why is not
  measured).
- In 5/6 stalls CPU 1's PC cycles between the EL1h synchronous vector and
  `el1_sync`+4 with all exceptions masked; in the three with registers (07,
  12, 15) ESR 0x96000045 (EL1 write data abort, level-1 translation
  fault), FAR = SP_EL1, SP far outside RAM and walking down 0x320 per entry,
  and the rest of CPU 1's registers a fresh `task_create` image of the
  latency task (x19 `latency_task_entry`, x30 `task_entry_wrapper`, DAIF
  0x80). SP_EL1 is a multiple of 0x320 in 6/6 readings; that the saved SP
  was 0 is an inference from that, not a measurement. In 1/6 (run14) CPU 1
  panicked with `task_exit: schedule returned!` (task 180, lat_iso). The
  first fault is not in any file.
- CPU 0's waiter calls `yield()` between polls (PC `timer_get_count` 60/60,
  return addresses in `test_wait_until_v`'s two loops 40/40 where
  recorded); no WFE or WFI. CPUs 2 and 3 in `idle_task_func` WFI 60/60.
- **Outcome: a kernel fault on isolated CPU 1, seen only with parallel
  vCPUs (6/15 vs 0/15); not a lock, WFE or WFI hang and not a host
  scheduling delay; a TCG fault cannot be excluded from these files.**
  Hypothesis (untested): a task slot is reused (the test's
  `task_destroy` plus `task_create`) while CPU 1 is still leaving the
  previous task in it, with a second destroyer through `rq->zombie`;
  `task_create` stores x19/x20 and x29/x30 before sp, and `switch_to`
  reads sp first.
- **C1 critic, #1213 (`critic-3-C1.md` section 8): pass**; every number
  agrees; the two wording points above are the critic's.
- Also filed by C1: #1220, #1221 (Entry 7). Evidence `gauntlet-3-hw-pi5` @
  6569a660.

## Entry 10 (09:31 UTC): C2 builder done; jetson-nano-2 to the C2 critic

- C2 builder held jetson-nano-2 07:31 to 09:29Z (claim released 09:29Z),
  board left at SD Linux, powered on. Evidence `gauntlet-3-hw-jetson` @
  68f5690e, `run3-hw/c2/`. ESP at hand-back: BootCurrent 0001, no
  BootNext, SLMOS.efi 405c7b43 (cell F's image), l4t.efi 91db497c, fsck
  rc=0 (`esp-handback.txt`); as found at take-over: SLMOS.efi 789cba62
  (`esp-takeover.txt`). C3 restores it. C2's counts and classifications go
  into the log after its critic files.
- 09:31Z: jetson-nano-2 to the blind C2 critic (`coherency-3-C2-critic`),
  with the definition of done (bars and the Entry 1 rulings), the heads and
  image hashes, and the evidence path only; it reproduces 5 runs each of
  cells C and B by kexec.

## Entry 11 (10:16 UTC): C2 critic verdict: pass

- **C2 critic (`critic-3-C2.md`): pass.** Critic held jetson-nano-2 09:32
  to 09:55Z, left it at SD Linux, powered on, ESP untouched; reproduction
  `gauntlet-3-hw-jetson` @ 4e07dc57, `run3-hw/critic-c2/{C,B}-repro-run01..05.txt`.
  Own counts and classifications saved at 10:06Z before any builder file
  was opened.
- Completeness: C, D, B, A, E, F each 20/20 complete files, banners
  matching the cell heads, every c2t block complete (six CPUs, 48 fields).
  Every nonzero `chk` (and one `att`/`diag_att` mismatch, B run08) is
  off by 1, a snapshot artifact of per-CPU counters copied with plain
  loads while thieves run; every file counts in every cell.
- Question 1: C, D and E all "exposed by the removal" by the bar as
  written; no non-owner CPU at 0 successes in any of their 60 runs.
- Question 2: not shown. Clause 3 fails on A's CPU 3 under every
  convention for 0-success runs (per-CPU reading). G not run; no fix C;
  no fix A or B on any branch or PR.
- Step 0: 10/10 kexec at 59a85926; the 30 minute cap was hit (the tenth
  boot ran about 6 minutes over, disclosed); UEFI not run.
- Reproduction: 5 C runs, question 1 "exposed" (CPU 2 0.344, CPU 3 0.610
  of the best thief: CPU 3 misses the 0.60 share clause by 1.0 point, a
  thin margin, recorded); 5 B runs meet clauses 1 to 3 on the B side. Same
  classification as the 20.
- Disagreement with the builder: its main B tables used 3 of 20 files
  (dropped `chk=-1`, kept `+1`); the critic counts all 20. No answer
  changes. The builder's entry below uses the critic's 20-file B values.
- Disclosed critic breaks: no MCP resource reader, so the board IP came
  from mDNS on the documented hostname after a failed attempt with an IP
  grepped from scratchpad scripts; the first B boot capture was cut by a
  `tail` argument, labelled TRUNCATED, unused; after its verdict it opened
  the first lines of two scratchpad files (copies of run 2's log).
  Disclosed builder breaks: one extra read-only ssh; the step-0 overrun;
  the board IP from run 2's transcript.

## Entry 12 (10:16 UTC, written after the C2 critic filed): C2 builder report

- C2 held jetson-nano-2 07:31 to 09:29Z. Cells on the Entry 0 heads, 20
  runs each, `gauntlet-3-hw-jetson` @ 68f5690e, `run3-hw/c2/`; tables in
  `run3-hw/c2/analysis/` (B tables there use 3 files; the values below
  are the critic's 20-file ones, `critic-3-C2.md` section 2).
- Per cell, median share of CPUs 2, 3 against the best thief, and
  preempt_disabled early-out as a fraction of CPUs 2 and 3's attempts:

  | Cell | Kernel, path | CPU 2 / CPU 3 share (ratio to best thief) | f_pd CPU 2 / 3 | first success median CPU 2 / 3 | 0-success runs (non-owner) |
  |---|---|---|---|---|---|
  | C | NC, kexec | 13.0 / 15.2% (0.713 / 0.830 of 18.27%) | 70.8 / 63.2% | 5.5 / 5.5 ms | 0 of 20 |
  | F | NC, UEFI | 8.1 / 11.3% (0.397 / 0.556 of 20.38%) | 75.5 / 68.7% | 6.2 / 6.2 ms | 0 of 20 |
  | D | removal, UEFI | 16.8 / 16.4% (0.955 / 0.935 of 17.56%) | 11.5 / 15.3% | 4.4 / 5.5 ms | 0 of 20 |
  | E | removal, kexec | 16.7 / 16.9% (0.990 / 1.000 of 16.85%) | 8.2 / 6.4% | 3.5 / 5.0 ms | 0 of 20 |
  | A | NC_AXIS=OFF, kexec | 13.8 / 14.2% (0.748 / 0.767 of 18.47%) | 65.9 / 67.2% | 204.5 / 167.3 ms | CPU 3 in runs 06, 15, 20 |
  | B | A + V1, kexec | 16.8 / 16.9% (CPUs 4, 5: 16.8, 16.9%) | 0.3 / 0.3% | 19 us / 29 us | 0 of 20 |

  Files: `<cell>-runNN.txt` for NN 01..20 in each cell (C
  `C-nc-instr-kexec`, F `F-nc-instr-uefi`, D `D-removal-instr-uefi`, E
  `E-removal-instr-kexec`, A `A-ncoff-instr-kexec`, B
  `B-ncoff-instr-v1-kexec`).
- **Question 1, by the bar as written:**
  - C: "exposed by the removal": CPU 2 at 0.713 and CPU 3 at 0.830 of the
    best thief (CPU 5, 18.27%), and 0 of 20 runs with a non-owner CPU at 0
    successes (`C-nc-instr-kexec-run01..20.txt`).
  - D: "exposed by the removal": 0.955 and 0.935 of CPU 0's 17.56%, 0 of
    20 (`D-removal-instr-uefi-run01..20.txt`).
  - E: "exposed by the removal": 0.990 and 1.000 of CPU 3's 16.85%, 0 of
    20 (`E-removal-instr-kexec-run01..20.txt`).
  - Recorded, not reinterpreted: the stall is strongest in the NC kernel
    (F, NC over UEFI, meets the share clause at 0.397 and 0.556; C carries
    the signature below the bar: 43.6 and 38.8% of CPUs 2 and 3's attempts
    start inside CPU 1's tick window, against 0.1% for CPUs 4 and 5) and
    nearly absent under the removal on both paths (D, E). The label
    "exposed by the removal" is the bar's output, not a finding that the
    removal causes the stall.
- **Question 2: not shown.** Clauses 1 and 4 hold; clause 2's B side
  holds (0.3%) while A is 65.9 and 67.2%, not about 75%; clause 3 fails:
  A's CPU 3 first-success median is 167.3 ms (195.4 ms with its three
  0-success runs counted at 2.5 s or never), below 200 ms
  (`A-ncoff-instr-kexec-run01..20.txt`). H3 stays a hypothesis; the
  mechanism question is open. Recorded for the next run: B removes the
  stall signature (shares within 0.1 point, early-out 0.3%, first
  success 19 and 29 us), and in A 53.3 and 57.1% of CPUs 2 and 3's
  attempts start inside CPU 1's T window with 98.5 and 99.0% of their
  early-outs less than 10 us after CPU 1's tick. G not run; fix C not
  started; fixes A and B: nothing landed.
- Anomalies, unedited, from C2: boot lines in C, A and B (absent in D, E,
  F): `[IRQ] Unhandled IRQ 102 — masking at GIC` and `[preempt] IRQ taken
  pre-preempt_init (elr=0x8002xxxx spsr=0x60000009) — bail` (the quoted
  text carries its own dash); the echoed command line is garbled in most F
  replies, results intact.
- Issues filed by C2: none.

## Entry 13 (10:16 UTC): decision gate

Evaluated once, when both critic verdicts were in (C1 07:44Z after the
row-3 re-check; C2 about 10:15Z), before the 12:16Z mark. Read from the
verdicts and the rows they cite.

| Input | Value | Source |
|---|---|---|
| C1 | **clear**: the critic counted 20/20 NC and 20/20 removal (three tests, no `[test_wait]` timeout), reproduced 5 + 5; #1211 not reproduced (0 of 20) | `critic-3-C1.md` sections 1, 2, 5, 7 |
| C2, question 1 (cell C) | **exposed** (CPU 2 0.713, CPU 3 0.830 of the best thief; no non-owner 0-success run) | `critic-3-C2.md` section 3 |
| C2, question 2 (cell B) | **not shown** (clause 3, A's CPU 3 167.3 ms) | `critic-3-C2.md` section 4 |

**Row: C1 clear; exposed; H3 not shown. C3 opens the Pi 5 PR: removal on
Pi 5 and QEMU, Jetson kept behind PLATFORM_HAS_NC_MEMORY in its platform
header only; ADR "accepted for Pi 5 and QEMU; deferred for Jetson pending
#1216"; only if the macro-kept Jetson passes `boot_test --count 10` on
both paths at the PR head.**

- No bar was redefined. The rows recorded alongside (not changing the
  row): the removal did not show the signature on either Jetson path in
  20 runs each (D 0.955 / 0.935, E 0.990 / 1.000, 0 non-owner 0-success
  runs; `D-removal-instr-uefi-run01..20.txt`, `E-removal-instr-kexec-run01..20.txt`),
  while the NC kernel did (F 0.397 / 0.556; C below the bar). Per the
  brief, the PR description says this with the files.
- How the row maps onto the code. The removal (79c10261) renames the
  macro to `PLATFORM_HAS_NC_REGION` and deletes 212 of the 213 uses of
  `PLATFORM_HAS_NC_MEMORY`, so "Jetson kept behind the macro" cannot be an
  edit of 79c10261. The tree just before the removal commit, 7acee1fc on
  the new base (the removal branch's first 33 commits: coherency verb and
  litmus suite, the secondary-MMU fix, the NC_AXIS and SETWAY_INVAL
  switches), already separates `PLATFORM_HAS_NC_MEMORY` (CPU-shared state
  in the NC block) from `PLATFORM_HAS_NC_REGION` (device DMA). The split
  PR is that tree plus one change: `PLATFORM_HAS_NC_MEMORY` defined for
  Jetson only, so Pi 5 takes the cacheable branches and Jetson keeps the
  NC axis.
- Consequence stated now: the split head's Pi 5 code is not 79c10261's,
  so C1's 20/20 removal count does not carry over to it. C3 boot-tests Pi
  5 at the split head as well (integration suite, `boot_test` runs=10,
  the minimum for a boot claim), and the PR body states that the 20/20 bar
  was met on 79c10261, not on the PR head.
- #1203: not closed by this row (the row that closes it as superseded is
  "pre-existing"); C3 re-scopes it in the PR description and the issue.

## Entry 14 (10:17 UTC): C3 started; both boards to C3

- C3 builder started about 10:25Z with Entry 13's construction
  (`coherency-nc-axis-pi5` from 7acee1fc plus one commit defining
  `PLATFORM_HAS_NC_MEMORY` for Jetson only), `/ci` on its head, and the
  lab in order Jetson (10 kexec + 10 UEFI-direct boots at the PR head, one
  bench and litmus set per path, the integration suite once, ESP restore
  to SLMOS.efi 789cba62), then pi-5-1 (integration suite 10 boots at the
  PR head, one production boot with bench and litmus, card restore). Hard
  stop for lab work 12:50Z. No PR unless the Jetson 10 + 10 pass and `/ci`
  is green. C3 claims as `coherency-3-C3`.
- The bare removal image built in Entry 3 is not used (the gate's row does
  not boot-test 79c10261 on Jetson).

## Entry 15 (12:41 UTC): C3 builder reported; C3 critic started; #1223 filed; cell branches deleted

- C3 builder reported at 12:39Z (both boards released, Jetson at SD Linux
  with the ESP restored, pi-5-1 at the SLM-OS shell on its original card,
  both powered on, per the builder). Its counts go into the log after the
  C3 critic files. It opened draft PR #1222 and set ADR 0007's status line
  on #1209 (fd2a361c). It reported one blocker, which the orchestrator
  filed at once because it is a defect, not a count:
- **#1223** (bug, platform:pi5, sub:sched, P2-medium, Backlog;
  file-don't-fix): Pi 5 KERNEL PANIC on CPU 1 right after the core-latency
  isolation phase at the split head e5b24894, 3 of 10 integration boots
  (`run3-hw/c3/split-pi5-integration-run01..10.txt` on
  `gauntlet-3-hw-jetson` @ 71634eb2); same family as #1213; not seen on
  the NC kernel or the full removal (20 of 20 each). Blocks #1222 being
  ready for review.
- #1199 (out of scope, not worked): one evidence comment for the Jetson
  test-kernel Lua `memcmp` fault at e5b24894
  (`run3-hw/c3/split-kexec-integration.txt`).
- #1203: re-scope comment posted (issuecomment-6796095732).
- 12:43Z: both boards to the blind C3 critic (`coherency-3-C3-critic`),
  verdict due 13:25Z; reproduction is one Jetson kexec boot and one Pi 5
  integration boot at the PR head (UEFI-direct skipped so the restored
  ESP stays as found).
- Deleted `c2/cell-a-ncoff` (6608cbd0), `c2/cell-b-ncoff-v1` (579b155a),
  `c2/cell-de-removal` (8fc7c88a), `c2/cell-g-removal-v1` (0dfaae0c) after
  archiving their tips under `run3-tools/deleted-branch-tips/` (README
  gives bases and order).

## Entry 16 (13:03 UTC): C3 critic verdict (first round); /ci logs pushed

- **C3 critic (`critic-3-C3.md`), filed 13:02Z: fail-on-completeness,
  row 2.** The /ci step logs at e5b24894 were not on the evidence branch:
  `.gitignore` (`*.log`) dropped the 13 logs from 71634eb2, leaving
  `head.txt` and `status.txt`. The critic read the scratchpad copies as
  green (runtime-test 308 and 328 passed, 0 brace violations over 12
  configurations, layout "All checks passed.", the four `make test` logs
  PASSED). Rows 1, 3, 5, 6 pass; row 4 (no bar) counted 7/10 and stated
  faithfully.
  - Row 1: 16 files, +92/-130; code changes only in `ncmem.h` and
    `spinlock.h` (the Pi 5 `SPINLOCK_DRAM_FLUSH`, which at 7acee1fc existed
    only for Pi 5 on the NC axis, so it follows from the definition).
  - Row 3: kexec 10/10, UEFI-direct 10/10, banner e5b24894 in every file,
    "run queues in NC memory" in every boot; two captures noted (kexec
    boot02 split during Linux boot before the trigger; boot09 starts 20 s
    after the trigger but before Linux hands over).
  - Row 4: complete runs 02, 03, 05, 07, 08, 09, 10 (18 known failures,
    three tests pass, no timeout); runs 01, 04, 06 panic after
    `test_rapid_task_exit_no_panic` and "CPU 1: returned to general
    scheduling", inside `test_isolated_core_latency`. With the critic's
    boot: 8 of 11 complete.
  - Reproduction: Jetson kexec 1/1 (own build, ELF 7a5ee64b), Pi 5
    integration 1/1 complete (own build 33d1ca71), `run3-hw/critic-c3/`
    @ ef0caa43; UEFI-direct skipped to keep the restored ESP; card
    restored. Both boards powered on, claims released.
  - Disclosed: commit subject "(7/10)" seen before counting (count from
    raw files agrees); separate power_cycle and capture; two read-only ssh
    checks; a 10 s sleep before `slmos-kexec`; pi-5-1 powered off for
    `sdwire_info`.
  - Recorded by the critic: Entry 15's "same family as #1213" is the
    orchestrator's hypothesis; it stays labelled so (#1223 says
    "resembles").
- 13:02Z: the orchestrator force-added the 13 logs from the scratchpad copy
  to `gauntlet-3-hw-jetson` @ 95e73982 (`status.txt` byte-identical); no
  result changes. The critic re-checks row 2 (Entry 17 or later).

## Entry 17 (13:03 UTC, written after the C3 critic filed): C3 builder report

- C3 held jetson-nano-2 10:23 to 11:54Z and pi-5-1 11:55 to about 12:35Z.
  Branch `coherency-nc-axis-pi5` @ e5b24894 = 7acee1fc plus one commit
  (`PLATFORM_HAS_NC_MEMORY` for Jetson only; the Pi 5 spinlock DRAM flush
  deleted; `test_nc_axis.c` `#error` guards; docs). Draft PR #1222 opened
  with the Pi 5 blocker stated first; ADR 0007 status line on #1209,
  fd2a361c. Evidence `gauntlet-3-hw-jetson` @ 71634eb2 (logs @ 95e73982),
  `run3-hw/c3/`.
- Jetson at e5b24894: kexec 10/10 (`split-kexec-boot01..10.txt`),
  UEFI-direct 10/10 (`split-uefi-boot01..10.txt`), ESP fsck clean after all
  10 writes; coherency litmus PASS on both paths (0 forbidden, 0 timeouts,
  0 weak rows); `bench smp` 5/5 and burst `bench stealing` 16/16 on both
  paths (`split-{kexec,uefi}-bench-*.txt`); `--sustain` is not in this tree
  (it comes with #1217). Test-kernel integration on kexec did not finish:
  Lua `memcmp` page fault (`split-kexec-integration.txt`; #1199).
- Pi 5 at e5b24894: integration 7/10 complete; 3/10 KERNEL PANIC (#1223).
  Production-kernel benches and litmus not run (lab time).
- /ci at e5b24894: 11 steps rc=0 (`run3-hw/c3/ci-e5b24894/status.txt`).
- Restores: ESP SLMOS.efi 789cba62, no BootNext, fsck clean
  (`esp-restore.txt`); pi-5-1 card originals, boot to `slmos>`
  (`pi5-restore-boot.txt`). Left on the boards:
  `/root/slmos-r3-c3-e5b24894.elf`, `/root/slmos-r3-c3-test-e5b24894.elf`
  (Jetson); staged images in the main checkout's `build/kernel/`.
- Disclosed: board IP by mDNS on the documented hostname (no resource
  reader); separate power_cycle and capture throughout; read-only
  `esp-check.sh` ssh and an scp fetch of the as-found ESP image (run 2's
  restore recipe); opening the PR with Pi 5 at 7/10, because the gate's
  only no-PR condition was a Jetson failure.

## Entry 18 (13:05 UTC): C3 critic row-2 re-check: C3 passes; lab state

- **C3 critic re-check (`critic-3-C3.md`, "Row 2 re-check"): pass.** The
  13 logs at `gauntlet-3-hw-jetson` @ 95e73982 are byte-identical to the
  scratchpad copy; every step's decisive line agrees with `status.txt`
  (rc=0). The logs print no commit line; the critic tied them to e5b24894
  through `head.txt` and the diff-check stat (86 files, +8605/-444, the
  stat of `origin/main...e5b24894`; 7acee1fc gives 84 files), labelled
  reconstructed, not printed. Component verdict: pass (rows 1, 2, 3, 5,
  6; row 4, no bar, 7/10 stated faithfully).
- Lab at about 13:04Z (orchestrator, after the last critic released): no
  active claims; pi-5-1 and jetson-nano-2 online, ping, serial, power on;
  jetson-nano-2 `recovery_mode` desired released, relay channel 1 open,
  cycle count 14 (as at 03:46Z).

## Entry 19 (13:05 UTC): branches run 3 created or moved

Heads as of this entry. "Archived" means the tip is under
`run3-tools/deleted-branch-tips/`.

| Branch | Head | Base | Purpose | Disposition |
|---|---|---|---|---|
| `gauntlet/coherency-3` | this commit | `gauntlet/coherency-2` @ b6f0beed | Run-3 log, rules, critic verdicts, tools, /ci logs | Keep |
| `gauntlet-3-hw-pi5` | 6569a660 | main 85f51ffd | Pi 5 and QEMU raw evidence (c1, critic-c1) | Keep |
| `gauntlet-3-hw-jetson` | 95e73982 | main 85f51ffd | Jetson raw evidence (c2, critic-c2, c3, critic-c3) | Keep |
| `coherency-remove-nc-axis` | 79c10261 (moved from e94c251a: re-parented onto 638e297d, Entry 0 item 5) | `c1/boot-path-plus-helper` @ 638e297d | The full removal | Keep |
| `c1/boot-path-plus-helper` | 638e297d (moved from 63eed0c4) | `fix/boot-path-1193` @ 59a85926 | #1209 + #1215's three commits; base of the removal | Keep until #1208, #1209, #1215 merge |
| `fix/boot-path-1193` | fd2a361c (moved from 59a85926: ADR status line) | `fix-boot-1194-1197-1198` @ ad8e4fa7 | #1196; ADR 0007 home | PR #1209 |
| `c2/steal-instrument-1203` | 1b27eea4 (moved from 983bddc9) | main 85f51ffd | Steal trace + victim-window instrumentation | PR #1217 |
| `c2/wake-on-push-v1` | 292eee0a (new) | 1b27eea4 | V1 experiment (idle WFE) | Keep (cited in #1216; a next run starts here) |
| `c1/instrument-1211-removal` | 9f375c19 (new) | 79c10261 | #1211 pre-conversion window, instrumented | Keep (cited in #1211) |
| `c1/isolated-1s` | f8ff72d6 (new name for run 2's object) | a03fc75b | #1213 1 s variant | Keep (cited in #1213) |
| `coherency-nc-axis-pi5` | e5b24894 (new) | 7acee1fc on 638e297d | The gate's split: Pi 5 off the NC axis, Jetson kept | PR #1222 (draft, blocked by #1223) |
| `c2/cell-a-ncoff` | 6608cbd0 | cbab2fc6 | Cell A | Deleted; archived |
| `c2/cell-b-ncoff-v1` | 579b155a | 6608cbd0 | Cell B | Deleted; archived |
| `c2/cell-de-removal` | 8fc7c88a | 79c10261 | Cells D, E | Deleted; archived |
| `c2/cell-g-removal-v1` | 0dfaae0c | 8fc7c88a | Cell G (not run) | Deleted; archived |

Not created or moved by run 3: `fix-boot-1194-1197-1198` (ad8e4fa7, PR
#1208, untouched), `c1/test-wait-helper` (a03fc75b, PR #1215),
`docs/labctl-serializes-calls` (90f0d9e3, PR #1218), main 85f51ffd.
Local-only: the orchestrator's scratchpad worktrees and local branches
(`c2-instr-local`, `rb3-removal`), the critics' `critic3-c1-evidence`;
never pushed except as recorded.

## Close-out and final report (written 13:05 UTC)

The run ended on the gate's row (Entry 13), within budget (started 03:46Z,
limit 13:46Z). Every component has a critic verdict: C1 pass (`critic-3-C1.md`,
after a row-3 re-check), C2 pass (`critic-3-C2.md`), C3 pass (after a row-2 re-check)
(`critic-3-C3.md`).

### The three questions

1. **Does the NC kernel show #1216 (pre-existing or exposed)?** By the bar as
   written, **"exposed by the removal"**: on the NC kernel over kexec (cell
   C), CPUs 2 and 3 reached 0.713 and 0.830 of the best thief's median share
   (bar 0.60) and no non-owner CPU had a 0-success run
   (`run3-hw/c2/C-nc-instr-kexec-run01..20.txt`). The rows run the other way
   from the label: the NC kernel carries the signature on both paths (C below
   the bar; F, NC over UEFI-direct, 0.397 and 0.556,
   `F-nc-instr-uefi-run01..20.txt`), and the removal shows almost none (D,
   UEFI-direct, 0.955 and 0.935; E, kexec, 0.990 and 1.000;
   `D-removal-instr-uefi-run01..20.txt`, `E-removal-instr-kexec-run01..20.txt`).
   The bar was not reinterpreted.
2. **Is the mechanism the wake path (H3)?** **Not shown.** V1 (idle WFE so the
   push SEV wakes idle thieves) removed the signature in cell B (CPUs 2 to 5
   at 16.8 to 16.9%, early-out 0.3% of CPUs 2 and 3's attempts, first success
   19 and 29 us; `B-ncoff-instr-v1-kexec-run01..20.txt`), and in cell A the
   stalled thieves' attempts clustered inside CPU 1's tick window (53.3 and
   57.1% of attempts; 98.5 and 99.0% of their early-outs less than 10 us after
   CPU 1's tick, against 0.05 to 0.07% for CPUs 4 and 5;
   `A-ncoff-instr-kexec-run01..20.txt`). Clause 3 failed: A's CPU 3
   first-success median was 167.3 ms (195.4 ms counting its three 0-success
   runs), not above 200 ms. H3 stays a hypothesis.
3. **Pi 5 bar and #1211.** **20 of 20 under both kernels**: NC (a03fc75b, run
   2's 9 boots on the code-identical 30dc24bc plus 11) and the full removal
   (79c10261), the three tests passing in every boot, no `[test_wait]`
   timeout (`run2-hw/c1/nc-step6-run01..09.txt` on `gauntlet-2-hw-pi5`,
   `run3-hw/c1/nc-step6-run10..20.txt`, `removal-step6-run01..20.txt`),
   reproduced 5 + 5 by the critic. **#1211: not reproduced**, 0 of 20 with the
   pre-conversion window under the removal (`run3-hw/c1/removal-1211-run01..20.txt`);
   open, blocking nothing.

### The PR, merge order, ADR

- Gate row (Entry 13): C1 clear, exposed, H3 not shown: the Pi 5 split PR.
  **Draft PR #1222** (`coherency-nc-axis-pi5` @ e5b24894, base main): the
  removal branch's first 33 commits (7acee1fc) plus `PLATFORM_HAS_NC_MEMORY`
  for Jetson only and the Pi 5 spinlock DRAM flush deleted. Jetson passed the
  gate condition: 10/10 kexec and 10/10 UEFI-direct boots at the head;
  `/ci` green. **Not ready for review:** Pi 5 at the head completed the
  integration suite in 7 of 10 boots; 3 panicked on CPU 1 right after the
  isolation phase (#1223). The full removal's Pi 5 code (79c10261) was 20 of
  20 on the same board.
- Merge order (if it becomes ready): #1208, #1209, #1215, then #1222. No fix C
  PR exists.
- ADR 0007 status on #1209 (fd2a361c): "accepted for Pi 5 and QEMU; deferred
  for Jetson pending #1216; implementation in PR #1222."

### Issues: end states

| Issue | What was run | What the rows said | State |
|---|---|---|---|
| #1202 | 20 + 20 boots, #1215's wait | 20/20 per kernel | open; closes with #1215 |
| #1205 | same | 20/20 per kernel | open; closes with #1215 |
| #1211 | 20 boots, pre-conversion window, removal | 0/20 failed | open, not reproduced |
| #1213 | QEMU, 15 + 15 + 10 runs with PC sampling | kernel fault on isolated CPU 1 under thread=multi (6/15 vs 0/15) | open; retitled as the kernel issue; priority left at P2 for the user |
| #1216 | cells C, D, A, B, E, F, 20 runs each | Q1 exposed by the bar; Q2 not shown | open, independent scheduler defect |
| #1203 | (via #1216) | re-scoped to #1216 | open, re-scoped |
| #1193 | the gate and #1222 | split PR blocked by #1223 | open |

### Tracker delta

Opened 5, closed 0. Opened: #1219 (QEMU batch-stress flake), #1220 and #1221
(run 2's two NC-only extras), #1223 (Pi 5 isolation-phase panic at the split
head); all four file-don't-fix. Draft PR #1222 opened; PR #1217 updated (second
commit 1b27eea4). #1213 retitled. Nothing merged; #1208 untouched.

### What the run could not do

- Step 0's UEFI-direct boots at #1209's head (cap hit after the kexec 10).
- Cell G (question 2 not shown); fix C (same).
- Pi 5 production-kernel bench smp, bench stealing and litmus at #1222's head
  (lab time).
- `bench stealing --sustain 2500` at #1222's head (the sustained bench is in
  #1217, not in that tree).
- A blind critic for #1219 to #1223's filings (filings, not counts).

### Single most useful next experiment

Boot-test the full removal 79c10261 on jetson-nano-2: `boot_test` 10 per path
(kexec and UEFI-direct) and the integration suite, the evidence the gate's
first two rows ask for, on the head whose Pi 5 code is already 20 of 20 and
whose Jetson rows (D, E) show no #1216 signature. Whether that changes which
PR is wanted is the user's decision; the run's gate rules did not allow it.

### Branch table and evidence heads

Entry 19. Evidence at final heads: **`gauntlet-3-hw-pi5` @ 6569a660**,
**`gauntlet-3-hw-jetson` @ 95e73982**.

### Lab state

pi-5-1: SLM-OS shell on its original card, powered on, unclaimed.
jetson-nano-2: SD Linux, ESP SLMOS.efi 789cba62 with no BootNext, recovery
relay released, powered on, unclaimed. Both as found at 03:46Z.

## Entry 20 (16:41 UTC): close-out authorized; #1208 reworded and merged; #1209 rebased

- The user authorized the close-out on 2026-11-11 (land the stack, replace
  #1222 with the full removal, fix C as a standalone draft PR). Board time
  stays serialized; this log continues.
- #1208: 6f0385ec reworded to carry before and after evidence (before:
  `c2-hw/pi5-bootlog-R7.txt` lines 112 and 126 to 128 on
  `gauntlet/coherency-2`, four "SMPEN NOT set" lines, checked; after: 0
  SMPEN lines in `run2-hw/c0/fixboot-167330f5-pi5-boot{01..10}.txt` on
  `gauntlet-2-hw-pi5`, checked, all ten banners 167330f5). The four later
  commits were re-parented with identical trees and authorship; branch
  `fix-boot-1194-1197-1198` ad8e4fa7 to 58cc6832 (lease on ad8e4fa7), tree
  unchanged. **Squash-merged as main 8a27711d** (the repository's recent
  merges are squashes; the squash body carries every commit message,
  including the evidence paragraph). main's tree equals 58cc6832's.
- #1209: `fix/boot-path-1193` re-parented from ad8e4fa7 onto main 8a27711d
  (same trees): fd2a361c to **b849e2ee** (lease); PR base changed to main.
  Jetson image at b849e2ee built (bin 87ab14e5); its UEFI-direct
  `boot_test --count 10` is running (lab agent `coherency-closeout-1209`).
- /ci on main 8a27711d running.

## Entry 21 (17:25 UTC): /ci on 8a27711d; #1209 Jetson UEFI 10/10; #1209 merged

- /ci on main 8a27711d (`run3-tools/ci-main-8a27711d/`): the ci skill's
  ten steps passed (builds; `make test` ARM64, x86-64, `AI_SCHED=ON`;
  runtime-test; check-braces; build order; Jetson UEFI layout; diff
  check). The extra step runs 2 and 3 add, `make test WORK_STEALING=ON`,
  failed with #1219's signature (batch stress, then the 120 s cap) at load
  average 0.5 to 0.9 with nothing else building: the load hypothesis is
  weakened, and the failure predates #1208 (seen on 6608cbd0). Not a ci
  skill step, so not a gate; recorded on #1219 (issuecomment-6802016458)
  and run again after each merge.
- #1209 head b849e2ee on jetson-nano-2, UEFI-direct: **10 of 10** to
  `slmos>`, banner and `/sys/version` b849e2ee, no panic or fault line,
  fsck clean after every ESP write
  (`run3-hw/closeout/pr1209-uefi-boot01..10.txt` on `gauntlet-3-hw-jetson`
  @ d3ba9c1e; lab agent `coherency-closeout-1209`). ESP restored to
  SLMOS.efi 789cba62, no BootNext, fsck clean (`pr1209-esp-restore.txt`);
  board at SD Linux, powered on, released. Disclosed: separate
  power_cycle and capture; one read-only ssh sha check; board IP from run
  3's record (mDNS did not resolve).
- **#1209 marked ready and squash-merged as main cf34214d** (tree equals
  b849e2ee). /ci on cf34214d running.

## Entry 22 (17:45 UTC): /ci on cf34214d green; #1222 closed; #1215 merged; fix C started

- /ci on main cf34214d (`run3-tools/ci-main-cf34214d/`): all ten ci skill
  steps passed (runtime-test 308 and 328 passed, 0 failed), and the extra
  `make test WORK_STEALING=ON` passed this time (0 `[FAIL]` lines).
- #1222 closed with the comment the user specified (gate row followed as
  written; cells D and E show no #1216 signature under the removal, C and
  F show it under NC; the split head panics on Pi 5, #1223, while 79c10261
  was 20/20). Branch `coherency-nc-axis-pi5` kept at e5b24894.
- **#1215 marked ready and squash-merged as main 73d8deb7**, no conflict;
  its tree equals the git-only projection (b849e2ee plus #1215's three
  commits, 73b56bb2). /ci on 73d8deb7 running.
- Rehearsal recorded: on the projected final main (#1209 + #1215 + #1217 +
  #1218, no conflicts), the removal applies cleanly as one squashed commit
  for its 33 pre-removal commits (run 1's history with five hand-resolved
  merges, which no rebase replays) followed by the 7 removal commits
  cherry-picked; the result's tree differs from cell D/E's measured tree
  (8fc7c88a) only by #1218's CLAUDE.md paragraph and the ADR status line.
- Fix C builder started about 17:30Z (offline: `--assert-shares` with a
  regression test, an idle wake-rate measurement, the V1 commit, /ci, four
  Jetson images). Its Jetson time comes after the removal's lab work.

## Entry 23 (18:26 UTC): stack landed; issues closed; removal rebased to 5d70fdbd

- /ci on main 73d8deb7 and on main 88b28c15: every ci skill step passed,
  and the extra `WORK_STEALING=ON` passed on both
  (`run3-tools/ci-main-73d8deb7/`, `run3-tools/ci-main-88b28c15/`).
- **#1217 squash-merged as main 88b28c15**, **#1218 squash-merged as main
  e4a33790**; no conflicts, each marked ready first; both trees equal the
  git-only projection. All five merges are done: #1208 8a27711d, #1209
  cf34214d, #1215 73d8deb7, #1217 88b28c15, #1218 e4a33790. /ci on
  e4a33790 queued after the removal's /ci (one /ci at a time).
- Issues: #1194, #1197, #1198 closed by #1208's merge (16:37Z); #1196 by
  #1209 (17:25Z); #1202 by #1215 (17:44Z). **#1205 did not auto-close**
  (#1215's body linked only #1202 as closing); closed by hand with a
  comment naming 73d8deb7 and the evidence.
- **Removal rebased onto main e4a33790: `coherency-remove-nc-axis` @
  5d70fdbd** (lease on 79c10261), 8 commits: one squashed commit for the
  pre-removal coherency work (net diff 638e297d..7acee1fc, the 28
  non-merge commits listed in its message; 33 with run 1's five merges,
  which no rebase replays), then the 7 removal commits cherry-picked
  without conflict. Its tree equals the rehearsal's, which differs from
  cells D and E's measured tree (8fc7c88a) only by #1218's CLAUDE.md
  paragraph and the ADR status line. The Pi 5 20/20 was on 79c10261 before
  this rebase; the new head gets its own Pi 5 boots. /ci on 5d70fdbd
  running.

## Entry 24 (19:09 UTC): /ci on the removal head and on final main; labs queued

- /ci on removal 5d70fdbd (`run3-tools/ci-removal-5d70fdbd/`): every ci
  skill step passed (runtime-test 308 and 328 passed, 0 failed;
  check-braces 0 violations, 0 unparsed, 12 configurations; layout clean,
  no `!` lines), and the extra `WORK_STEALING=ON` passed.
- /ci on final main e4a33790 (`run3-tools/ci-main-e4a33790/`): every ci
  skill step passed; the extra `WORK_STEALING=ON` failed with #1219's
  signature. #1218 is CLAUDE.md-only on top of 88b28c15, where it passed.
  Tally for the extra step in this close-out: 2 of 6 failed (8a27711d,
  e4a33790), all with the same line; recorded on #1219
  (issuecomment-6804840396). Not a ci skill step.
- 18:47Z: lab to `coherency-closeout-removal` (Jetson steps 1 to 6, then
  Pi 5 steps 7 to 9) at 5d70fdbd. Images (agent's manifest): Jetson prod
  elf e2ad2300 / bin 4de7aa7d, Jetson test elf a6db487a / bin 18fc982f, Pi
  5 prod 172d8e8b, Pi 5 test 750174f8, each banner 5d70fdbd; a main
  e4a33790 Jetson test kernel (elf 9718c97e) prebuilt as a control, used
  only if a Jetson integration run does not complete.
- Fix C offline work done (19:15Z): `c2/wake-on-push-v1` @ 37939418 =
  e4a33790 + A (d7ede5d0, `--assert-shares` and the idle wake-rate
  bench, with a regression test) + B (37939418, idle WFE); removal
  variants `c2/fixc-removal` @ c77156b2 and `c2/fixc-removal-fix` @
  5fe8c70b. /ci on 37939418: the ten ci skill steps passed; the extra
  `WORK_STEALING=ON` failed twice there with #1219's signature and passed
  2 of 2 on A alone; QEMU compiles none of B's code, and the builder is
  comparing the QEMU objects. Four Jetson images built and hashed. Waits
  for the Jetson after the removal lab.
- Noted for the removal PR (not changed, so the measured head stays
  5d70fdbd): one `#if defined(PLATFORM_HAS_NC_MEMORY)` extern block that
  #1217 added to `kernel/src/shell_sys.c` is dead text under the removal
  (the macro is never defined there; the cacheable branch compiles).

## Entry 25 (19:18 UTC): fix C offline report (lab and PR pending)

- Builder handed back about 19:30Z before the Jetson grant; no labctl
  call, no PR. Heads: `c2/wake-on-push-v1` @ 37939418 = e4a33790 + A
  d7ede5d0 (`--assert-shares`, pure `steal_shares_check()` in
  `steal_trace.h`, `kernel/tests/test_steal_shares.c` with 9 cases,
  `bench idle-wakes [ms]`, an `iw mode=sustained` line per CPU, docs) +
  B 37939418 (idle WFI to WFE, production message, comments and docs);
  `c2/fixc-removal` c77156b2, `c2/fixc-removal-fix` 5fe8c70b on 5d70fdbd.
- /ci on 37939418: the ten ci skill steps passed
  (`run3-tools/fixc/ci-37939418/` @ 11788aee). Extra `WORK_STEALING=ON`:
  A+B 2 of 5 passed, every failure #1219's signature; A alone 4 of 4
  passed; the QEMU objects at A and A+B are identical apart from two smp.c
  `__LINE__` constants moved by B's comment (`run3-tools/fixc/ws-compare/`),
  so the failures are #1219, not B. `make test` on A passed. A defect of
  the builder's own (a test expecting 1300 instead of 1400) was caught by
  `make test` and fixed before any result was claimed
  (`test-A-43d07cf1-failed/`).
- Jetson images (`run3-tools/fixc/images/MANIFEST.txt`): nc d7ede5d0
  (elf 9272611b, idle 1 wfi), ncfix 37939418 (9def416e, 1 wfe), removal
  c77156b2 (dad6ca01, 1 wfi), removalfix 5fe8c70b (35a73609, 1 wfe).
- Noted: B also changes Pi 5's default idle wait (same branch); Pi 5 is not
  measured by this step. `kernel/CLAUDE.md` ("Idle Task DAIF") would need
  updating if B lands. Each sustained run is about 15 KB of serial output,
  so the fix C lab runs as one agent per image.

## Entry 26 (21:15 UTC): removal hardware at 5d70fdbd; removal PR #1225; ADR on main

- Lab agent `coherency-closeout-removal` (Jetson 18:50 to about 20:33Z,
  Pi 5 20:33 to about 21:10Z; both released, powered on): evidence
  `run3-hw/closeout/removal-5d70fdbd-*` on `gauntlet-3-hw-jetson` @
  320a3c08 and `gauntlet-3-hw-pi5` @ 45c86b7f; images in
  `removal-5d70fdbd-image-manifest.txt`; every counted banner 5d70fdbd.
  - jetson-nano-2 production: kexec **10 of 10**
    (`...-jetson-kexec-boot-run{01,02,04..11}.txt`; run03 reached the
    shell but its capture was cut by a `tail` argument, filed
    `-run03-truncated.txt`, not counted), UEFI-direct **10 of 10**
    (`...-jetson-uefi-boot-run01..10.txt`, fsck clean after every write).
  - jetson-nano-2 integration suite: did not complete on either path. kexec:
    PAGE FAULT in the slm-runner tests after
    `test_runner_parks_without_a_lock` (#1199's place); UEFI-direct:
    `test_camera_open_mock` assert, then a PAGE FAULT on CPU 0 (filed as
    #1224, bug, platform:jetson, sub:drivers, P3-low). **Controls on main
    e4a33790 fail identically on both paths** (same place, same PASS and
    FAIL counts; `...-control-e4a33790-integration-run01.txt`): both
    failures are pre-existing, not the removal's.
  - pi-5-1: production **10 of 10** (`...-pi5-boot-run01..10.txt`);
    integration **10 of 10 complete and passing by C1's rule** (three named
    tests pass, exactly the 18 known #1109 failures, no `[test_wait]`
    timeout; `...-pi5-integration-run01..10.txt`, `...-pi5-index.txt`).
    The earlier 20 of 20 was on 79c10261, before the rebase.
  - Restores: ESP SLMOS.efi 789cba62, no BootNext, fsck clean; pi-5-1 card
    originals, boot to `slmos>`. A `power_cycle` returned an error once and
    left the Jetson off; one `power_on` recovered it (`...-jetson-power-cycle-error.txt`).
  - Disclosed: separate power_cycle, trigger and capture calls; scripted
    scp/sync/sha/kexec; three capture mistakes of the agent's own, none
    counted; the shared-scratchpad overwrite (nothing lost).
- **Orchestrator decision: "both boards clean" read as "the removal adds no
  failure"**: the Jetson integration suite fails on main identically on
  both paths, so it does not count against the removal. Stated first in
  the PR's evidence section; the user may reverse it.
- **Removal PR #1225** opened ready for review (not a draft), head
  `coherency-remove-nc-axis` @ 5d70fdbd, base main, labels platform:pi5,
  platform:jetson, sub:sched, Backlog; body: what it is, ADR link, run 1's
  cost numbers as a table (from ADR 0007's Evidence), cells C, D, E, F with
  filenames, /ci and the hardware table at the head, closes #1193 and
  #1203 (re-scoped to #1216), the dead-text follow-up. GitHub reports it
  mergeable, closing [1193, 1203]. Not merged.
- **ADR 0007 on main: e73b9b61** (direct commit, authorized): status
  "accepted; implementation in PR #1225", plus a "Run 3 (2026-11-11)"
  paragraph saying the removal cleared #1216's signature on Jetson (with
  the four ratios) and Pi 5 met its bar.
- Fix C Jetson block 1 of 4 (nc) started on jetson-nano-2 after the
  removal lab released it.

## Entry 27 (21:36 UTC): fix C Jetson block 1 of 4 (nc, without the fix)

- Agent `coherency-closeout-fixc-nc`, image nc (d7ede5d0 = main e4a33790 +
  `--assert-shares`, no fix; elf 9272611b, checked on host and board), one
  kexec boot, no reboot; board back at SD Linux, released. Files
  `run3-hw/fixc/nc-*.txt` on `gauntlet-3-hw-jetson` @ bc9303ee (45 files,
  every one with `commit: d7ede5d0`).
- `--assert-shares`: **FAIL 2 of 20** (run01: CPU 2 9.1% against best thief
  CPU 0 18.9%; run12: CPU 2 6.7%, CPU 3 also below half, against CPU 5
  21.9%), PASS 18 of 20 (`nc-sustain-run01..20.txt`). The required failing
  run on main without the fix is shown.
- Median share over 20 sustained runs: CPU 0 17.20%, 1 16.85%, 2 16.05%
  (6.7 to 17.3), 3 15.95% (9.2 to 17.9), 4 17.40%, 5 17.40%. Burst
  `bench stealing` median 10332 us (7303 to 13756), 20 of 20 completed
  16/16 (`nc-burst-run01..20.txt`). `bench smp` 5/5 completed (`nc-smp.txt`).
- Idle wakes per second, at rest (`bench idle-wakes 5000`, median of 3,
  `nc-idlewakes-run01..03.txt`): 100.0 on every CPU (99.8 to 100.1), the
  tick. During the sustained runs (`iw mode=sustained`, median of 20):
  CPU 0 0.0, 1 2.3, 2 14.15, 3 12.25, 4 2.7, 5 3.7.
- Disclosed: separate boot and login captures (only caller); one
  `cat /sys/version` reply written at the top of every file; garbled
  command echoes kept; five replies with a cut duplicate prompt, output
  complete.
- 21:3xZ: block 2 of 4 (ncfix) started.

## Entry 28 (21:58 UTC): fix C Jetson block 2 of 4 (ncfix, with the fix)

- Agent `coherency-closeout-fixc-ncfix`, image ncfix (37939418 = main +
  `--assert-shares` + idle WFE; elf 9def416e, checked), one kexec boot;
  board back at SD Linux, released. Files `run3-hw/fixc/ncfix-*.txt` on
  `gauntlet-3-hw-jetson` @ 9e28b3f7 (45 files, `commit: 37939418` in each).
- `--assert-shares`: **PASS 20 of 20**, FAIL 0. Median share CPU 0 16.5%,
  1 16.3%, 2 16.7%, 3 16.7%, 4 16.8%, 5 16.7%; every CPU's range within
  16.0 to 17.0% (`ncfix-sustain-run01..20.txt`). Burst median 6923 us
  (6917 to 6934), against nc's 10332 (7303 to 13756); 20 of 20 completed.
  `bench smp` 5/5 completed.
- **Idle wake rate (the cost):** at rest 77449, 73477, 72782, 76370, 55864,
  53548 idle-loop passes per second on CPUs 0 to 5 (median of
  `ncfix-idlewakes-run01..03.txt`), against 100.0 on every CPU without the
  fix (nc); ticks 99.9 to 100.1/s in both. During the sustained runs 0.0,
  1737, 2367, 2766, 2535, 2718/s against nc's 0.0 to 14.2/s. WFE returns
  almost at once: idle CPUs effectively poll. Hypothesis, untested: the
  generic-timer event stream (CNTKCTL_EL1.EVNTEN) or an event left
  pending by frequent SEVs. The cost is the user's decision.

## Entry 29 (21:58 UTC): why WFE barely sleeps (hypothesis, from the code)

- Not an event stream: `boot.S` writes `CNTHCTL_EL2` = 3 (EL0PCTEN and
  EL0PCEN only; EVNTEN, bit 2, clear), main e4a33790 around line 710.
- Likely source: `spin_unlock` issues `sev` on every unlock
  (`kernel/include/spinlock.h` lines 281 and 289 on main), and SEV sets
  the event register on every PE, including the issuer. The idle loop's
  `yield()` enters `schedule()`, which takes and releases run-queue locks
  on every pass, so an idle CPU re-arms its own event each pass and its
  next WFE returns at once. Hypothesis, not tested. A cheaper variant
  would clear the self-generated event before sleeping (for example
  `sevl; wfe` to consume it, then `wfe`) or re-check the run queue and
  the steal deques first; a design choice for the user, not made here.
- Fix C block 3 of 4 (removal, no fix) started about 21:58Z.

## Entry 30 (22:22 UTC): fix C Jetson block 3 of 4 (removal, no fix)

- Agent `coherency-closeout-fixc-removal`, image removal (c77156b2 =
  5d70fdbd + `--assert-shares`; elf dad6ca01, checked), one kexec boot;
  board back at SD Linux, released. Files `run3-hw/fixc/removal-*.txt` on
  `gauntlet-3-hw-jetson` @ f94cd467 (45 files, `commit: c77156b2`). Its
  boot log has no "run queues in NC memory" line, as expected.
- `--assert-shares`: **PASS 20 of 20**. Median share 16.60, 16.30, 16.80,
  16.55, 16.70, 16.90% on CPUs 0 to 5 (every range within 15.5 to 17.6%).
  Burst median 10327 us (7856 to 11624), 20 of 20 completed. `bench smp`
  5/5 completed.
- Idle wakes at rest 99.8 to 100.0/s on every CPU (the tick); during the
  sustained runs 0.0 to 5.7/s. The removal alone already balances the
  shares, without the idle-loop cost (agrees with run 3's cells D and E).

## Entry 31 (22:45 UTC): fix C block 4 of 4 (removal + fix); fix C draft PR #1226

- Agent `coherency-closeout-fixc-removalfix`, image removalfix (5fe8c70b =
  5d70fdbd + `--assert-shares` + idle WFE; elf 35a73609, checked), one
  kexec boot; files `run3-hw/fixc/removalfix-*.txt` on
  `gauntlet-3-hw-jetson` @ 20dbc30c (45 files, `commit: 5fe8c70b`).
- `--assert-shares`: **PASS 20 of 20**; median share 16.4, 16.2, 16.8, 16.8,
  16.7, 16.7% (every range within 16.1 to 17.1%). Burst median 11856 us
  with a range of 6916 to 28073 us, against 10327 us (7856 to 11624)
  without the fix on the same kernel. `bench smp` 5/5.
- Idle wakes at rest: CPUs 0 to 3 at 294.8, 231.4, 211.8, 296.6/s with
  their ticks at about 74 to 76/s instead of 100; CPUs 4 and 5 (cluster 1)
  at 203901 and 205744/s. During the sustained runs 0.0, 2248, 11808,
  16335, 13481, 6738/s. Uneven, and different from ncfix; cause not
  determined (hypothesis for the general cost: Entry 29).
- **Fix C draft PR #1226** (head `c2/wake-on-push-v1` @ 37939418, base
  main, refs #1216, labels platform:jetson, sub:sched, Backlog): the two
  commits; a table of all four images with filenames; the cost (idle
  polling at 53k to 77k passes/s per CPU on NC, the uneven pattern and
  slower burst on the removal) labelled the user's decision; the H3
  history as the user asked (clause 3 missed only because its 200 ms
  threshold came from run 2's baseline and run 3's baseline was 167 ms,
  every other clause passed), with clause 2's baseline stated as measured
  (65.9 and 67.2% against the clause's "about 75%"); /ci and the #1219
  analysis. Not merged.
- Summary of fix C on Jetson, kexec, 20 runs each: `--assert-shares`
  passes 18/20 on NC without the fix, 20/20 with it, 20/20 on the removal
  without it, 20/20 on the removal with it. The removal alone balances the
  shares at the tick-rate idle cost.

## Entry 32 (22:45 UTC): branches, worktrees, lab state

| Branch | Head | Disposition |
|---|---|---|
| main | e73b9b61 | #1208 8a27711d, #1209 cf34214d, #1215 73d8deb7, #1217 88b28c15, #1218 e4a33790 squash-merged; ADR 0007 status and run 3 paragraph e73b9b61 |
| `coherency-remove-nc-axis` | 5d70fdbd (moved from 79c10261: rebased onto e4a33790, Entry 23) | PR #1225, ready for review |
| `c2/wake-on-push-v1` | 37939418 (moved from 292eee0a: A + B on main) | PR #1226, draft |
| `coherency-nc-axis-pi5` | e5b24894 | Kept (closed PR #1222), as the user asked |
| `c1/instrument-1211-removal` | 9f375c19 | Kept: cited in open #1211 |
| `c1/isolated-1s` | f8ff72d6 | Kept: cited in open #1213 |
| `gauntlet/coherency-3` | this commit | Keep: log, verdicts, tools, /ci logs |
| `gauntlet-3-hw-pi5` | 45c86b7f | Keep: Pi 5 and QEMU evidence |
| `gauntlet-3-hw-jetson` | 20dbc30c | Keep: Jetson evidence |
| `c2/fixc-removal`, `c2/fixc-removal-fix` | c77156b2, 5fe8c70b | Deleted; tips archived (`run3-tools/deleted-branch-tips/`) |
| `c1/boot-path-plus-helper` | 638e297d | Deleted; content on main |
| `fix-boot-1194-1197-1198`, `fix/boot-path-1193`, `c1/test-wait-helper`, `c2/steal-instrument-1203`, `docs/labctl-serializes-calls` | 58cc6832, b849e2ee, a03fc75b, 1b27eea4, 90f0d9e3 | Deleted after squash-merge; tips remain as the PRs' heads on GitHub |

The deletion record is in `run3-tools/deleted-branch-tips/README.md`. The
orchestrator's scratch worktrees and local temporary branches were
removed; one (`ev-pi5`) held only staged deletions of files already
pushed, and was reset before removal.

Lab at the end: no active claims; pi-5-1 at the SLM-OS shell on its
original card, powered on, online; jetson-nano-2 at SD Linux, ESP
SLMOS.efi 789cba62 with no BootNext, `recovery_mode` released (relay
open, cycle count 14), powered on, online. Both as found.

## Close-out addendum (written 22:45 UTC)

- Merged, in order, each followed by /ci on main with the ci skill's
  steps green: #1208, #1209 (after a 10/10 Jetson UEFI-direct boot test at
  its head), #1215, #1217, #1218. No conflicts. The extra
  `WORK_STEALING=ON` step failed 2 of 6 times on main with #1219's
  signature, independent of the merged changes.
- Issues closed: #1194, #1196, #1197, #1198, #1202 on merge; #1205 by
  hand (73d8deb7). #1222 closed (branch kept).
- Removal PR #1225 (ready): Jetson 10/10 kexec and 10/10 UEFI-direct, Pi 5
  10/10 boots and 10/10 integration at 5d70fdbd; the Jetson integration
  suite fails identically on main (#1199, #1224), so it is not counted
  against the removal (orchestrator's reading, stated in the PR).
- Fix C draft PR #1226: the check fails on main without the fix (2/20)
  and passes with it (20/20); the cost is idle polling (53k to 77k idle
  passes/s per CPU at rest against 100) and is the user's decision.
- Filed during the close-out: #1224 (Jetson test-kernel camera fault, by
  the removal lab agent). Updated: #1219 (twice), #1205.

## Entry 33 (23:25 UTC): item 1, #1225 rebased onto current main; /ci running

- The user authorized merging #1225 and a split-out check PR (no other
  merges; no lab time unless a step needs it).
- main had moved since #1225's base by one commit, e73b9b61 (the ADR 0007
  status and run 3 paragraph), which touches only
  `docs/adr/0007-cross-cpu-coherency.md`; no removal commit touches that
  file (0 commits in e4a33790..5d70fdbd), so nothing outside the removal's
  files can conflict. Re-parented the 8 commits onto e73b9b61 with the ADR
  file from main (`run3-tools/c0/graft.sh.txt`): **#1225 head 0ae17e0f**
  (lease on 5d70fdbd); `git diff <base> <tip>` byte-identical before and
  after; 0ae17e0f's tree differs from 5d70fdbd's only by that file.
- /ci on 0ae17e0f running. GitHub's mergeable state read UNKNOWN before the
  push (still computing); it is read again before the merge.
- Item 2 prepared: the `--assert-shares` commit on `c2/wake-on-push-v1`
  (d7ede5d0) differs from the form that ran in image c77156b2 only by an
  `#if defined(PLATFORM_HAS_NC_MEMORY)` branch around two `extern` lines,
  dead after the removal. The split branch takes the c77156b2 form.

## Entry 34 (00:06 UTC): item 1 done: #1225 merged as 98e17440; #1193, #1203 closed

- /ci on #1225's head 0ae17e0f (`run3-tools/ci-removal-0ae17e0f/`): every ci
  skill step passed (runtime-test 308 and 328 passed; check-braces 0
  violations, 0 unparsed; layout "All checks passed."), and the extra
  `WORK_STEALING=ON` passed. The API read `mergeable: true`,
  `mergeable_state: clean` at head 0ae17e0f.
- **#1225 squash-merged as main 98e17440** (`--match-head-commit`
  0ae17e0f); main's tree equals 0ae17e0f's.
- /ci on main 98e17440 (`run3-tools/ci-main-98e17440/`): every ci skill
  step passed and the extra `WORK_STEALING=ON` passed (0 `[FAIL]` lines).
- #1193 closed on merge (23:46:10Z); #1203 closed on merge about 20 s
  later (CLOSED, COMPLETED). Neither needed a hand close.

## Entry 35 (00:29 UTC): item 2, the check split out and merged (#1227); #1226 closed

- Branch `bench/assert-shares` from main 98e17440 with the
  `--assert-shares` commit in the form that ran in image c77156b2: head
  **d711cb39**. `git diff c77156b2 d711cb39` changes only
  `docs/adr/0007-cross-cpu-coherency.md` (main's ADR update), 0 lines under
  `kernel/`, `runtime/`, `CMakeLists.txt`, `Makefile`: the bench code is
  identical to what ran in c77156b2 (20/20 PASS,
  `run3-hw/fixc/removal-sustain-run01..20.txt`), so no lab time was used.
  Against d7ede5d0 (the commit on `c2/wake-on-push-v1`) it differs only by
  an `#if defined(PLATFORM_HAS_NC_MEMORY)` branch around two `extern`
  lines, dead since #1225.
- /ci on d711cb39 (`run3-tools/ci-check-d711cb39/`): every ci skill step
  passed (runtime-test 308 and 328; check-braces 0 violations, 0 unparsed;
  layout clean), the extra `WORK_STEALING=ON` passed, and
  `test_steal_shares.c` ran.
- **PR #1227** (ready; body: what the check asserts, FAIL 2/20 on the old
  NC kernel `nc-sustain-run01..20.txt`, PASS 20/20 on the removal
  `removal-sustain-run01..20.txt`, the identity with c77156b2, and that it
  guards against #1216 coming back), API `mergeable: true, clean`,
  **squash-merged as main 6c1f4c9c** (tree equals d711cb39's, where /ci was
  green). Branch deleted.
- **#1226 closed without merging** with the user's comment (on the removal
  WFE buys nothing: 20/20 without it, burst 11.9 ms with it against 10.3
  ms without and a wider range; its benefit was only on NC; as written WFE
  polls, 53k to 77k idle passes/s on NC and about 205k/s on cluster 1 with
  the removal, most likely the self-SEV of `spin_unlock`, Entry 29,
  untested). Branch `c2/wake-on-push-v1` kept.

## Entry 36 (00:29 UTC): item 3, #1216 closed; branch table (supersedes Entry 32's)

- #1216: comment with the mechanism as measured (cell A clustering, 98.5 to
  99.0% of CPUs 2 and 3's early-outs within 10 us of CPU 1's tick; removed
  by waking idle thieves, cell B), the removal clearing the signature
  (cells D and E; `--assert-shares` 20/20 on the removal, 2/20 failing on
  NC), the regression check #1227, and the WFE fix not taken (#1226);
  closed as completed, resolved by #1225 (issuecomment-6810695137).
- No lab time in items 1 to 3. Boards untouched: no active claims;
  pi-5-1 and jetson-nano-2 online, powered on, as found.

| Branch | Head | Disposition |
|---|---|---|
| main | 6c1f4c9c | #1225 98e17440, #1227 6c1f4c9c (after #1208 8a27711d, #1209 cf34214d, #1215 73d8deb7, #1217 88b28c15, #1218 e4a33790, ADR e73b9b61) |
| `c2/wake-on-push-v1` | 37939418 | Kept (closed PR #1226) |
| `coherency-nc-axis-pi5` | e5b24894 | Kept (closed PR #1222) |
| `c1/instrument-1211-removal` | 9f375c19 | Kept: cited in open #1211 |
| `c1/isolated-1s` | f8ff72d6 | Kept: cited in open #1213 |
| `gauntlet/coherency-3` | this commit | Keep |
| `gauntlet-3-hw-pi5` | 45c86b7f | Keep |
| `gauntlet-3-hw-jetson` | 20dbc30c | Keep |
| `coherency-remove-nc-axis` | 0ae17e0f | Deleted after squash-merge (#1225); tip kept as the PR head |
| `bench/assert-shares` | d711cb39 | Deleted after squash-merge (#1227) |
| `c2/fixc-removal`, `c2/fixc-removal-fix`, `c1/boot-path-plus-helper`, and the five stack PR branches | (Entry 32) | Deleted earlier (Entry 32) |

## Final addendum (written 00:29 UTC)

Merged in this round: #1225 (main 98e17440) and #1227 (main 6c1f4c9c),
nothing else. Issues closed: #1193 and #1203 (on #1225's merge), #1216 (by
hand, resolved by #1225). PR #1226 closed unmerged. No lab time; both
boards as found.
