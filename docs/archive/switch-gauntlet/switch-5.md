# Switch gauntlet log, run 5 (#1234, #1238, #1207, #1228)

Goal: close the switch-in window (#1234), in which a task can be switched in
on one CPU from a saved context another CPU has not finished writing; prove it
closed on both boards under the brief's reproducer, 5x, no-deadlock and
no-regression bars; decide whether #1238 is the same defect; root-cause the
PMM halt (#1207, #1228) or leave it open with a capture; land the test filter
and serialized fault dumps. The brief is the user's message of 2026-11-13
(PT), transcribed in `run5-brief.md` next to this file. This log is
append-only: a correction is a new entry citing the one it corrects, and every
entry carries its own write time.

Boards: pi-5-1 and jetson-nano-2 only. Evidence: `gauntlet-5-hw-pi5` (Pi 5,
plus QEMU under `run5-hw/c<N>/`) and `gauntlet-5-hw-jetson`, one file per run
or boot under `docs/archive/switch-gauntlet/run5-hw/c<N>/`, banner hash at the
top.

## Budget

- Started: 2026-11-13 22:15:30 UTC (14:15:30 PST, the orchestrator's first
  action). Limit: 12 h, 2026-11-14 10:15 UTC. About 35 min of C0 went to
  waiting on the user for the #1239 decision and the Jetson ssh permission
  rule; the limit is not moved for it.
- Hour marks (UTC), one-shot session crons: 2:00 = 00:15, 3:30 = 01:45,
  5:00 = 03:15, 6:00 = 04:15, 8:00 = 06:15, 10:00 = 08:15, 11:00 = 09:15,
  end 10:15 (cron at 10:04).

## Entry 0 (written 23:05 UTC): hour-0 checks

1. **Reminder self-test.** This session has no `send_later`,
   `list_triggers` or `delete_trigger`; the equivalents are the session-only
   `CronCreate`, `CronList`, `CronDelete`. One-shot `76a2d0ef` created
   22:16 UTC for 22:17 UTC; `CronList` listed it. It fired at 22:27:03 UTC,
   10 min late, because a session cron fires only when the session is idle
   and the session was busy with the merge and then paused for the user.
   After the fire `CronList` was empty; `CronDelete 76a2d0ef` returned "No
   scheduled job" (one-shots delete themselves). Result: reminders work but
   land late by however long a turn runs; each hour mark is scheduled
   (`b6de6bcf`, `58501331`, `ac842957`, `869eb54f`, `2e11e27d`, `d594cc60`,
   `dec8831e`, `144a6be7`) and lab handovers are also driven by builder
   completion notices, not by the crons alone.
2. **Tool families.** `CronList` ok; `gh pr list` (one open PR, #1239) and
   `gh issue view 1234` (OPEN, P1-high, sub:sched) ok; labctl
   `list_claims` (none), then `claim_sbc` / `run_health_check` /
   `release_sbc` on pi-5-1 (22:15 UTC, online, ping, serial, power on) and
   on jetson-nano-2 (22:15 UTC, same) ok. ssh to jetson-nano-2 was denied by
   the permission classifier at 22:17 UTC; the user added an allow rule and
   the retry at 22:50 UTC succeeded (item 3). Trivial critic spawned with
   model fable: it reported model `claude-fable-5-2` and read the base head
   8ec0e330 from s5-base. git push of the log branch: this entry's push.
3. **Lab state.** No claims at 22:15 or 22:50 UTC. Both boards online and
   powered on. jetson-nano-2 `recovery_mode` binding desired_state released.
   jetson-nano-2 (claimed 22:50, released 22:51 UTC): ESP mounted read-only,
   `SLMOS.efi` sha256 prefix **789cba62**; BootCurrent 0001 (SD Linux); no
   BootNext; BootOrder 0008,0001,0007,0009,000D,000C,000B,000A,0003,0005,
   0002,0004,0000,0006.
4. **main and #1239.** At 22:15 UTC main was 5b28d065 and #1239 was open
   (head 1b85b315, mergeable CLEAN, main an ancestor, closing refs #1199 and
   #1224 only). The user chose "Merge #1239 now"; squash-merged at about
   22:35 UTC. **Base: main 8ec0e330dca4d8db5053e0bf2ed2101699a9ff4c.**
   /ci and three `make test WORK_STEALING=ON` runs on the base: pending,
   started 22:58 UTC in worktree s5-base; results in Entry 0a. No builder
   builds or runs QEMU until that baseline completes, so its rates are
   measured on an otherwise idle host.
5. **Branches.** `gauntlet/switch-5` (this log, `run5-brief.md`),
   `gauntlet-5-hw-pi5`, `gauntlet-5-hw-jetson`, all from 8ec0e330, pushed
   with this entry. Evidence under `docs/archive/switch-gauntlet/run5-hw/c<N>/`.
6. **Lab schedule (single lane, UTC).** 22:45 to 00:15: no lab (C1 QEMU; C2
   QEMU R1 and R3; C3 prepares its capture build). From C1's filter landing
   (target 00:15): C3 holds jetson-nano-2 for the #1207 reproduction and
   capture, to 01:45 at the latest. 01:45 to 03:15: C2 holds the lab (Jetson
   kexec, Jetson UEFI-direct, then pi-5-1) for the R2 and R3 baselines.
   04:15 to 06:15: C3 on jetson-nano-2 for its proof and suite boots if it has
   a fix; otherwise C2 may take it early. 06:15 to 08:15: C2 fix-head R2 and
   R3 and regression rows. 08:15 to 09:15: critic reproductions (handed over
   by the orchestrator, critics never claim). Nobody calls labctl without
   holding the lab; every handover is an entry.
7. **Critic model.** Critics on Fable (`claude-fable-5-2`, verified in item
   2); builders and the orchestrator on the session's model (Opus 5.5). A
   critic that cannot run on Fable is re-run on the session's model and the
   verdict says so.
8. **Milestone.** Backlog (the user named none).

## Entry 0a (written 23:31 UTC): C0 item 4 result; builders released

- /ci on base **8ec0e330**, worktree s5-base, 23:01 to 23:19 UTC, one step at
  a time, host otherwise idle (builders reading only; load 1.6 to 2.8):
  ARM64 build, x86-64 build, Jetson real-weights main and test builds, `make
  test`, `make test PLATFORM=X86_64`, `make test AI_SCHED=ON` (each "PASSED -
  All tests passed (exit code 0)"), `make runtime-test` (308 and 328 passed,
  0 failed), braces ("All check-c-braces tests passed"; "no changed C
  lines"), build order ("All build-order tests passed"), Jetson layout ("All
  checks passed.", no skipped `!` lines), diff check empty. All rc=0.
- `make test WORK_STEALING=ON`: **3 of 3 passed** (23:19 to 23:28 UTC). This
  is the run's #1230-family baseline: 0 failures in 3 runs.
- Files: `gauntlet-5-hw-pi5` 4f86c880,
  `docs/archive/switch-gauntlet/run5-hw/c0/ci-base-8ec0e330/` (summary.txt,
  one log per step, run.sh).
- Builders spawned at 23:10 UTC (C1 `c1-harness`, C2 `c2-window`, C3
  `c3-pmm`, Opus 5.5), held to read/design/edit until this result, released
  at 23:29 UTC.
- C1 design note (23:24 UTC, disclosed deviation for the C1 critic): Jetson
  kexec cannot carry the filter on the command line without a board change
  (`slmos-kexec` passes Linux's /proc/cmdline and has no append option) and
  UEFI-direct boots with the firmware DTB, so both Jetson paths use a
  build-time filter (`TEST_FILTER=`, `TEST_REPEAT=`); Pi 5 and QEMU also take
  `slmos.test=` / `slmos.test_repeat=` on the command line; the shell takes
  `ktest <names> <N>`. The brief asked for a command line or boot argument
  the kexec and UEFI paths can both pass.

C0 definition of done met: Entry 0 and 0a pushed, base head in bold.

## Entry 1 (written 23:36 UTC): C1 filter pushed; lab handover to C3

- C1 pushed `c1/harness` at 6e39f425 (23:34 UTC): test filter and repeat
  (build-time `TEST_FILTER=` / `TEST_REPEAT=` on every platform including
  QEMU; Pi 5 `slmos.test=` in cmdline.txt parser-tested only), plus fault-dump
  code whose proofs are still to come. C1's own QEMU check (50 of 50 with the
  filter) is a builder claim until its evidence file is pushed and the critic
  reproduces it.
- **Handover 23:35 UTC: jetson-nano-2 to C3** (#1207 reproduction and
  capture), until 01:45 UTC at the latest. pi-5-1 unheld. C2 told its window
  is 01:45 to 03:15 UTC.

## Entry 2 (written 00:15 UTC): hour mark 2:00

Pushed heads: `c1/harness` 6e39f425, `c2/switch-window` 6ee2f16d (local
88d4d71b splits R3), `c3/pmm-halt` a03aaa12 (rebasing onto c1/harness).

- C1: on mark. Filter pushed at 23:34 UTC; running /ci, the unfiltered
  byte-for-byte diff and the forced double-fault demo before its critic.
- C2: ahead of mark. R1 and R3 written on top of c1/harness; R3 is split
  into `test_migrate_ready_during_switch_out` and
  `test_wake_during_switch_out` (the wake path reaches the same window:
  `task_sleep_wake` on a task that set TASK_BLOCKED but has not switched
  out). Disclosed strengthening: movers aim using the task's on_cpu field
  (read only), because a plain migrate loop gave 0 hits in 60k migrations
  per run. Builder claim, not yet evidence: one QEMU run with the
  SWITCH_CHECK instrument flagged 29 switch-ins whose previous save was not
  complete and 25 owned by another CPU, then a fault in schedule matching the
  #1238 signature. The 20-run baselines (R3 migrate, R3 wake, R1, run one
  after another, load recorded per run) are in progress.
  Process notes from C2: one run discarded after a rebase in the worktree a
  loop was building in (stopped by its own task id); branch surgery moved to
  a separate worktree.
- C3: on mark; holds jetson-nano-2 since 23:35 UTC (Entry 1), Jetson
  capture build in progress. Its decode of four run-4 captures (labelled
  hypothesis until the capture decides it): #1207 and #1228 are one defect, a
  free list shorter than its free_counts, discovered rather than caused by
  test_exhaustion_recovery; all four on kexec boots, 0 of 13 UEFI suite boots.
- Lab: no change. jetson-nano-2 with C3 to 01:45 UTC; pi-5-1 unheld.

## Entry 3 (written 01:48 UTC): hour mark 3:30; C3 slot unused; lab to C2

- C3's report (01:45 UTC): jetson-nano-2 claimed 23:36 UTC (pre-checks
  run: health, bindings, recovery_mode released, no claims), then **nothing
  ran on the board**. C3 ended its turn to wait on a background Jetson build
  (finished 23:38 UTC); the completion notice did not reach the idle agent
  until the orchestrator's 01:45 message. The claim expired 01:36:48 UTC. No
  deploy, scp, power cycle, boot_test, ESP or BootNext change. Board as found;
  health at 01:45 online. Process failure, disclosed by the builder; the
  orchestrator also missed it, because it did not check pushed heads between
  the 2:00 and 3:30 marks.
- C3 QEMU (builder claim until its files are pushed): PMM_DEBUG=ON full
  `make test` on base + C3 found no free-list corruption; it found two
  mismatched-order frees in `kernel/gpu/gpu_stub.c` (aligned path allocates
  order 10, frees order 9: a leak, not a list corruption; to be filed);
  `test_litmus_patterns_pass` failed once, cause not yet separated from the
  PMM_DEBUG instrument.
- C1: no push since 6e39f425 (23:34 UTC); status requested. C1 critic not
  started: C1 is behind its mark.
- C2: no push since 6ee2f16d; QEMU baselines status requested.
- **Handover 01:47 UTC: jetson-nano-2 and pi-5-1 to C2** for the R2 and R3
  baselines, until 04:15 UTC (the 1 h C3 did not use is given to C2). C3 gets
  jetson-nano-2 at 04:15 UTC for 2 h; C2's ADR critic overlaps it in QEMU
  only.
- Process change (all builders, 01:47 UTC): no ending a turn to wait on a
  background task; wait in the foreground or with Monitor. The orchestrator
  checks pushed heads every 30 min.

## Entry 4 (written 02:05 UTC): C2 QEMU baselines; C1 and C3 status; litmus under load

- C2 QEMU baselines (builder report; files on `gauntlet-5-hw-pi5` 87fadeb5,
  `run5-hw/c2/qemu-base/`; head `c2/switch-window` 88d4d71b = base + C1
  6e39f425 + tests only; `-accel tcg,thread=multi`; one QEMU at a time;
  load average 1.2 to 10.0 at start of runs, recorded per file):
  R3 migrate 20 of 20 failed, mean iterations to first failure 5.55 (each
  iteration a 300 ms phase); R3 wake 20 of 20 failed, mean 1.1 (7 of 20 are
  silent hangs, attributed to the window only by exclusion until the fix head
  completes the same test); R1 11 of 11 failed so far (runs 12 to 20 in
  progress). Critic to count from the files.
- C1 (01:52 UTC): sat idle after its /ci finished at 00:06 UTC (same
  notification failure as C3; disclosed). Head 165cb992. Its /ci at 6e39f425
  had `make test` and `make test AI_SCHED=ON` each fail exactly one test,
  `test_litmus_patterns_pass` (#1230 signature, MP weak rows); rerun at
  165cb992 in progress with load recorded.
- C3: `test_litmus_patterns_pass` also failed with PMM_DEBUG=OFF at 5069e658
  (load 5.92 at start, C2 QEMU running), so it is not C3's instrument
  (files: `gauntlet-5-hw-pi5` b8faefa0, `run5-hw/c3/qemu-pmmdebug-*`).
  Hypothesis, untested: #1230's litmus rows fail under host load from
  concurrent QEMU runs; C0's base /ci passed it at load about 2. Consequence
  for the bars: every PR-head /ci and WORK_STEALING=ON row is run on a quiet
  host (no other QEMU), scheduled by the orchestrator, with load recorded.
  C3 filed **#1240** (gpu_stub order-10 alloc, order-9 free: leak, not list
  corruption; P3-low, Backlog). C3's off-build size compare: every pmm.c
  function identical in address and size; differences are `__LINE__`
  immediates only.
- Lab: C2 holds both boards (Entry 3) until 04:15 UTC.

## Entry 5 (written 02:13 UTC): C1 final push; C1 critic started

- C1 final head `c1/harness` **165cb992** (6e39f425 code; 165cb992
  check-braces exemption for generated C under build/ plus docs, no kernel
  source). Evidence `gauntlet-5-hw-pi5` 0068c5be, `run5-hw/c1/`. The
  builder's report and draft entry are held until the critic files, per the
  protocol.
- C1 critic `critic-c1` spawned at 02:12 UTC on Fable, blind: definition of
  done, the brief's C1, critic protocol and rules sections, heads 8ec0e330 and
  165cb992, evidence paths, the C0 baseline path. No builder reasoning.
  QEMU only, no lab. C1 is about 2 h behind its mark (critic was due by
  01:45 UTC).

## Entry 6 (written 02:55 UTC): C1 critic round 1, FAIL

Correction to Entries 4 and 5: their write times were estimated; Entry 5 was
written at about 02:08 UTC (heartbeat clock 02:08:48), Entry 4 shortly
before.

- Verdict: **FAIL**, Fable 5.2, `critic-5-C1.md` (gauntlet/switch-5
  bff8e775); reproduction logs `gauntlet-5-hw-pi5` 985c7a8b,
  `run5-hw/c1-critic/` (INDEX.txt). Reproduced at 165cb992: filter x50 50 of
  50; forced double fault 4 of 4 readable, 0 unprefixed lines; unfiltered
  summary format unchanged; holder faulting mid-dump completes; holder silent
  50 ms lets the other CPU dump.
- Fixes required: (1) SLOT_TRACE fault paths take the dump lock and never
  release it; (2) a holder stopped mid-dump silences ordinary prints on every
  other CPU forever (UART lock held for the whole dump); (4) kernel/CLAUDE.md
  "UART Lock" stale; (5) /ci at one head on a clean tree with real exit codes
  (step 4 failed in both /ci-form runs on #1230, step 5 passed on retry; the
  runner recorded rc=0 for failed steps; several steps ran only at 6e39f425).
- (3) Redefined criterion: neither Jetson path takes a boot argument; a
  build-time filter is substituted, and `slmos.test=` has run on no target.
  The orchestrator accepted the deviation at 23:29 UTC, but the critic is
  right that the bar is the user's: carried to the final report's Dissent
  section for the user to accept or reject, not fixed this round.
- Round 2 sent to C1 at 02:54 UTC (fixes 1, 2, 4, 5). C2 warned that a
  SLOT_TRACE capture must use C1's fixed head.
- Builder entry for C1 (its draft, written now that the critic has filed):
  c1/harness 165cb992 on 8ec0e330; filter and repeat (exact names,
  pass-major, `[ITER k/N]` per run, build-time TEST_FILTER/TEST_REPEAT on both
  Jetson paths, `slmos.test=` on Pi 5 cmdline.txt unit-tested only, `ktest`
  in the shell); fault dumps serialized by a lock that gives up after 50 ms of
  holder silence; builder counts filter x50 50/50, double fault 5/5, no-lock
  control interleaved 3/3, #1230 litmus load tally (`litmus-ab/TALLY.md`,
  48bb1be3: under load 3.1 head 4 of 4 and base 2 of 2 pass; 3.7 and up head
  fails 5 of 6, base 3 of 3).

## Entry 7 (written 03:16 UTC): hour mark 5:00; C2 hardware baselines in progress

- C2 has held both boards since 01:46:08 UTC (labctl claim stamp). Entry 3
  gave the handover as 01:47 UTC, an estimate written after the message was
  sent; the claim followed the handover message.
- C2 Jetson kexec baseline (builder report 02:54 UTC, test kernels at
  88d4d71b, captures in /var/lib/labctl/output/s5-c2-kexec-base-b1..b3,
  evidence files to be pushed): R2 3 of 3 boots fault in the first
  iterations (EC 0xe illegal execution state on pdm tasks, EC 0 on idle_5:
  #1238 signatures); R3 migrate 1 of 3 boots fail; R3 wake 3 of 3 boots hang
  silently at iteration 1. Builder claims until files are pushed.
- UEFI-direct reduced to what the brief asks: R2 only, 3 boots, one image,
  one ESP write. R3 on UEFI only as an extra cell if time remains. Then
  pi-5-1: R2, R3 migrate, R3 wake, 3 boots each. Stop 04:10, restore,
  release by 04:15.
- C2 is behind the 5:00 mark: ADR 0008 not drafted, because the hardware
  baselines took the C3 slot's hour. Plan: ADR drafted 04:15 to 05:00 UTC,
  design critic 05:00 to 06:00, fix from 06:00 if the design passes.
- C1 round 2: fixes pushed (ef61c69c, then 2196deb5); its PR-head /ci running
  on a quiet host since about 03:10 UTC (C3 holding QEMU and builds). C3:
  rebases onto C1's head and rebuilds after that /ci, for its 04:15 slot.

## Entry 8 (written 03:57 UTC): C2 baseline table; lab released; schedule

C2 lab session 01:46 to 03:53 UTC (2 h 7 min); both boards released at 03:53,
powered on, online. Restore (builder report, to be checked by the C2 critic):
Jetson ESP `SLMOS.efi` 789cba62 (fresh read-only mount sha, `fsck.vfat -n`
clean), no BootNext, BootCurrent 0001, staged elfs removed; pi-5-1 originals
renamed aside and back, boots its prior b42b2c1d kernel.

Baseline rates (builder's table; heads c2/switch-window 88d4d71b = base +
C1 6e39f425 + tests; x200 per boot, one test per image; critic to count):

| Reproducer | Cell | Boots or runs failed | Iterations | Files |
|---|---|---|---|---|
| R1 window patch | QEMU thread=multi | 20 of 20 (14 fault, 6 hang), iteration 1 | | gauntlet-5-hw-pi5 105a3013 run5-hw/c2/qemu-base/r1-base-88d4d71b-run01..20.txt |
| R3 migrate | QEMU | 20 of 20, mean first failure 5.55 | | same, r3m-base-* |
| R3 wake | QEMU | 20 of 20, mean first failure 1.1 | | same, r3w-base-* |
| R2 | Jetson kexec | 3 of 3, iteration 1 (2 dumps EC 0xe pdm_a/pdm_b and EC 0 idle_5, 1 hang) | | gauntlet-5-hw-jetson d3834651 run5-hw/c2/hw-base/kexec-r2-boot1..3.txt |
| R3 migrate | Jetson kexec | 1 of 3 (hang at 54) | 454, 1 failure | kexec-r3m-boot1..3.txt |
| R3 wake | Jetson kexec | 3 of 3, hang at iteration 1 | | kexec-r3w-boot1..3.txt |
| R2 | Jetson UEFI (1 boot) | 38 of 200 iterations, first at 7 | 200 | uefi-r2-boot1.txt |
| R3 wake | Jetson UEFI (1 boot) | 1 of 1, hang at 1 | | uefi-r3w-boot1.txt |
| R3 migrate | Jetson UEFI | not run (time) | | |
| R2 | pi-5-1 | **0 of 3, never failed** | 600 | gauntlet-5-hw-pi5 105a3013 run5-hw/c2/hw-base/pi5-r2-boot1..3.txt |
| R3 migrate | pi-5-1 | **0 of 3, never failed** | 600 | pi5-r3m-boot1..3.txt |
| R3 wake | pi-5-1 | 3 of 3, iteration 1 (1 dump, 2 hangs) | | pi5-r3w-boot1..3.txt |

Never-failed cells (cannot claim a fix): Pi 5 R2, Pi 5 R3 migrate; they are
still run at the fix head. UEFI cells are 1 boot, short of the brief's 3 for
R2: disclosed. Process notes from C2: the UEFI deploy script boots SLM-OS once
uncaptured per image before the captured boot; 10 min lost to a wait loop
whose pgrep matched itself (own task, stopped by id); Pi 5 deployed
`kernel_2712.img` only (config.txt names it). WARN flood measured at about 14
lines per iteration on Jetson kexec R3 migrate.

Schedule from here (UTC): C3 jetson-nano-2 04:00 to 05:30 (moved earlier,
since the board is free and C3's rebuild follows C1's /ci); C2 QEMU traces
of R3 and ADR 0008 draft now; C2 Jetson R2 SLOT_TRACE capture 05:30 to 06:15;
C2 design critic from the ADR push, with the Jetson trace added when it
lands; fix-head lab 06:15 onward.

## Entry 9 (written 03:59 UTC): handover to C3

**Handover 03:58 UTC: jetson-nano-2 to C3** until 05:30 UTC, using C3's
images at 5069e658 (c1/harness 6e39f425 + C3) for reproduction and capture.
Those are diagnosis rows; C3's proof rows will run at its rebased PR head.
pi-5-1 unheld.

## Entry 10 (written 04:12 UTC): ADR 0008 pushed; C2 design critic started

- C2 pushed ADR 0008 (proposed) at `c2/switch-window` d73604f7 and stopped
  before writing fix code. QEMU SLOT_TRACE first-fault dumps (builder claim;
  `gauntlet-5-hw-pi5` f49e67d1, `run5-hw/c2/qemu-trace/`): 6 of 6 are the
  #1234 window (3 requeue then migrate, 3 wake before switch-out). #1238 on
  Jetson is not traced: it is labelled a hypothesis that it is the wake path;
  Jetson R2 trace planned 05:30 to 06:15 UTC.
- C2 filed **#1241** (sched_migrate_task rewrites assigned_cpu of a task
  queued on another CPU; P2, Backlog).
- Temporary branches pushed: `c2/trace` 1b7410f1, `c2/reverted-wait`
  2f03c445 (1ead6ef6's spin on this base, for the no-deadlock bar).
- C2 design critic `critic-c2-design` spawned 04:11 UTC on Fable, blind:
  reads only the ADR's sequence, evidence and design sections, derives the
  lock graph and ordering from code, then reads the ADR's arguments and
  compares. Ahead of the original 5:00 to 6:00 mark despite the lab shift.

## Entry 11 (written 04:14 UTC, from date): hour mark 6:00

Correction to Entry 10: written at about 04:08 UTC, not 04:12. From this
entry on, write times come from `date -u`.

- C2: design critic running since 04:11 UTC; no verdict yet; C2 stopped on fix
  code. Behind the 6:00 mark by the length of the critic.
- C3: holds jetson-nano-2 since 03:58 UTC (Entry 9) until 05:30 UTC,
  reproducing and capturing with its 5069e658 images. No handover at this
  mark.
- C1: my go for its PR-head /ci at 2196deb5 (sent about 03:17 and 03:20 UTC)
  did not reach it: it waited in one long foreground loop and messages are
  delivered only between tool calls. Third go sent at 04:13 UTC; /ci starts
  now. C1 is about 2.5 h behind its mark; C2 and C3 already build on its
  branch, so nothing else is blocked. Round-2 evidence:
  `gauntlet-5-hw-pi5` 0d260e09, `run5-hw/c1/round2/`.
- Process change: builders keep each foreground wait at 10 min or less so
  messages are delivered between waits.

## Entry 12 (written 04:40 UTC, from date): C2 design round 1 FAIL; #1242; evidence-branch incident

- Design verdict round 1: **FAIL**, Fable 5.2, `critic-5-C2-design.md`
  (gauntlet/switch-5 0170ef31). Holds: no lock cycle and no wait under a
  lock by the critic's own graph; publishing STLR in sched_finish_switch and
  acquiring LDAPR under rq_lock named and correct, LDAPR sufficient, x86 TSO
  fine; both named sequences closed at the pick; critic prototype 40 of 40 on
  R3 migrate, R3 wake and R2 in QEMU. Fails: (1, blocking, observed)
  `schedule()` reads this_cpu before rq_lock with IRQs open, so a task
  migrated in that gap acts on another CPU's rq_lock, idle_task and deferred
  flag (probe: 5 STALE in 120 iterations in proto-run01; STALE then panic in
  proto-run02); (2) no stated bound for the idle flag check vs the wait;
  (3) never-released case unstated; (4) argument gaps and an incomplete lock
  list. Seven required changes sent to C2 (round 2 of 2).
- Filed **#1242** (P1-high, sub:sched, Backlog): the stale this_cpu in
  schedule(), with the critic's files. Not shown to be #1238.
- Critic evidence placed by the orchestrator on `gauntlet-5-hw-pi5` under
  `run5-hw/c2-design-critic/` (the critic's own push was refused by the
  permission layer).
- **Incident (orchestrator):** placing those files, commit 9380bd31 on
  `gauntlet-5-hw-pi5` recorded deletions of 205 evidence files (C1, C1
  critic, C2, C3), because the s5-hw-pi5 worktree's branch ref had moved
  under a stale index and the preceding `git pull --rebase` had failed
  without stopping the script. Pushed at about 04:20 UTC; restored in
  39f26650 about 3 min later: origin's tree now equals f49e67d1 plus the 10
  intended files (checked with `git diff --no-renames --name-status
  f49e67d1 origin/gauntlet-5-hw-pi5`). History keeps both commits; no
  force-push. Builders told to re-pull and check status before their next
  commit there. Process change: every orchestrator commit script stops if the
  pull fails, and the stale worktree is no longer used for evidence.

## Entry 13 (written 04:41 UTC, from date): heartbeat; orphan QEMU; C1 /ci not started

- An orphaned `qemu-system-aarch64` (PID 3381445, parent 1, elapsed 1 day
  18 h 54 min at 04:40 UTC, about 101% CPU, cwd the orchestrator's worktree
  bridge-cse_014Q9aryGYwccZqUzMHyre5U) predates this run. It was not started
  by any run 5 agent, so it is not killed (rule: kill only your own PIDs);
  reported to the user. It has used about one host core throughout, including
  during C0's baseline, so every run 5 QEMU rate shares it.
- C1's PR-head /ci at 2196deb5 had not started at 04:41 UTC (no make, cargo or
  QEMU process in s5-c1), although ListAgents showed C1 running. Fourth go
  sent with a request to confirm the start.
- Host load 5.45 at 04:40 UTC: C2's QEMU (s5-c2-aux), the orphan, C3 builds.

## Entry 14 (written 04:57 UTC, from date): second evidence deletion; restore; pre-push rule

- C2's commit 09e691ec on `gauntlet-5-hw-pi5` (no-deadlock old-design
  runs) deleted the 10 `run5-hw/c2-design-critic/` files, by the same
  stale-index mechanism as 9380bd31 (Entry 12). Restored from a fresh
  worktree in 9811702e; `git diff --no-renames --diff-filter=D
  --name-only 39f26650 origin/gauntlet-5-hw-pi5` is empty.
- Pre-push rule added to `run5-builder-notes.md` and sent to every
  agent: rebase (not soft reset), add only the agent's own directory, and an
  empty deleted-file diff against origin before pushing.
- C2 (04:56 UTC): the design verdict of 04:22 had not reached it; resent at
  04:57. C2 ran the old-design half of the no-deadlock bar: R1 as ported
  completed 2 of 4 runs on `c2/reverted-wait` 2f03c445 (so R1 alone does
  not reliably force the tick in), and a new
  `test_switch_window_tick_forced` (CPU 0 holds its window until 2 more
  hardware ticks on CPU 0, each waking sleepers pinned to CPU 3, which takes
  rq_lock[3]) hung 20 of 20 runs on `c2/reverted-wait` 653650d5, no dump,
  120 s timeout (files 09e691ec, `run5-hw/c2/qemu-nodeadlock/`). The spin
  is not yet register-sampled per run; requested.

## Entry 15 (written 05:01 UTC, from date): C3 slot 1 results; handover to C3 again

C3 held jetson-nano-2 03:55 to 04:59 UTC (64 min); board restored (ESP
789cba62 by fresh read-only mount, fsck clean, no BootNext, SD Linux,
powered on). Diagnosis head `c3/pmm-halt` 5069e658 (c1/harness 6e39f425 +
PMM_DEBUG=ON), not a PR head. Files `gauntlet-5-hw-jetson` 27bc1096,
`run5-hw/c3/` (builder report; the C3 critic counts):

- Filtered `test_checkerboard_fragmentation,test_exhaustion_recovery` x100:
  kexec 3 boots 600 of 600 pass, UEFI-direct 3 boots 600 of 600 pass, 0
  PMM_DEBUG reports (`filt-kexec-a2916b76-boot01..03.txt`,
  `filt-uefi-1fefbfdd-boot01..03.txt`). Finding: the halt does not appear
  in isolation; it needs state from earlier suites.
- Full suite, PMM_DEBUG walking every list on every operation, kexec 6
  boots: test_pmm 48 pass, 2 fail (the known kexec-only coalescing tests), 1
  ignored in each; test_exhaustion_recovery 6 of 6 pass; 0 free-list
  corruption reports beyond the self-test's expected one
  (`suite-kexec-60c548f3-boot01..06.txt`). #1207 not reproduced: 0 of 6
  against 1 of 10 at 1b85b315, uninformative (0 of 6 at p = 0.1 has
  probability 0.53). Hypothesis: the debug BSS moves the heap start and the
  walk slows every PMM operation, either of which could hide the trigger.
- For C2: 3 of 6 suite boots halted later in Scheduler Tests with #1238-family
  faults (boot01 CPU 0 idle_4 data abort with ELR 0x80012180, a `mov x0,
  sp`, not the fault site; boot02 CPU 1 idle_1 PC alignment fault ELR 0x0;
  boot06 CPU 3 pdm_b EC 0xe). C1's CPU-tagged dumps were readable.
- The order-10 alloc / order-9 free pattern also exists in gpu_nvidia.c;
  added to #1240.

**Handover 05:05 UTC: jetson-nano-2 to C3** until 06:30 UTC, for more
PMM_DEBUG full-suite kexec boots (about 20 needed for 88% odds of a catch at p
= 0.1; as many as fit). C2 is revising ADR 0008 (round 2) and has no lab need
before 06:30; its Jetson R2 trace moves to 06:30 to 07:15 if still wanted,
then fix-head lab.

## Entry 16 (written 05:01 UTC, from date): lab schedule correction

Correction to Entry 15: C2 held an earlier promise (Entry 8) of jetson-nano-2
from 05:30 to 06:15 UTC for the Jetson R2 SLOT_TRACE capture (image at
`c2/trace` 4812e821, rebased onto c1/harness 2196deb5). Lab now: **C3
05:05 to 05:30**, **C2 05:30 to 06:15** (kexec, up to 3 trace boots, then bench
smp on the base if time remains), **C3 06:15 to 07:15**. C2 heads:
`c2/switch-window` 7a95c012 (adds the #1242 stale-CPU probe to SWITCH_CHECK
and an R3 assertion), new temporary `c2/proto-1242` 1f58b249 (the critic's
prototype, for the #1242 base rate on base plus deferral).

## Entry 17 (written 05:03 UTC, from date): C1 round 2 pushed; orchestrator runs C1's /ci

- C1 round 2 at `c1/harness` **2196deb5** (165cb992 + ef61c69c fixes 1, 2,
  4 + 2196deb5 wrap fix). Evidence `gauntlet-5-hw-pi5` 0d260e09,
  `run5-hw/c1/round2/` (builder claims, for the critic: new selftest 15 of
  15 at 2196deb5, CPU 0 printed in 414 to 664 us while a stopped holder kept
  both locks; control without the bound timed out at 60 s; the ef61c69c
  selftest failed 1 of 3 on a counter-wrap race, fixed in 2196deb5).
- None of the orchestrator's four go messages (about 03:17, 03:20, 04:13,
  04:41 UTC) reached C1, although C1's messages reached the orchestrator. To
  end the stall, the orchestrator is running /ci at 2196deb5 itself, in a
  fresh worktree (s5-ci-c1, clean at start), steps 1 to 9 plus the diff
  check, with each log's first line giving head, step, start time and load,
  and make's own rc in summary.txt. C1 told not to run it. The three
  WORK_STEALING=ON runs are dropped from this C1 /ci (they are C2's
  regression rows). Host is not quiet: C2 runs one QEMU and the orphan QEMU
  (Entry 13) holds a core.

## Entry 18 (written 05:32 UTC, from date): C1 /ci at 2196deb5; both critics round 2

- /ci at `c1/harness` 2196deb5, run by the orchestrator in clean worktree
  s5-ci-c1, 05:02 to 05:29 UTC. Files `gauntlet-5-hw-pi5` f74c1ed5,
  `run5-hw/c0/ci-c1-2196deb5/` (each log's first line: head, step, start,
  load; summary.txt: make's rc). Steps 1 to 3, 4b, 6 (308 and 328 passed),
  7 (0 violations, 0 unparsed, all 12 configurations parsed), 8, 9 ("All
  checks passed.", no skipped lines), diff: rc 0. Step 4 `make test`
  rc 2 at start load 7.42 and step 5 `AI_SCHED=ON` rc 2 at 5.36, each with
  exactly one failure, `test_litmus_patterns_pass` (#1230). Rerun once:
  04r PASSED at 2.33, 05r PASSED at 2.79. "/ci green" is met only with one
  retry of steps 4 and 5 under load; carried to the critic and, if it
  matters to the verdict, to Dissent.
- C1 critic round 2 sent at 05:33 UTC (head 2196deb5; fix 3 carried to
  Dissent, judged only as disclosed and unchanged).
- C2 ADR 0008 round 2 pushed on **new branch names**, because the rebase onto
  2196deb5 needed a force push that the permission system denied:
  `c2/switch-window-r2` 6bc60aa3, `c2/trace-r2` 4812e821,
  `c2/reverted-wait-r2` ef11ee3c, `c2/proto-1242` 1f58b249; the old
  names stay at their pre-rebase heads. Round-2 design (builder summary):
  pin the CPU id by masking the tick's interrupt class before reading cpu_id()
  through the locked section; idle checks with the tick masked, then WFI;
  deferral runs the next ready task in queue order and idles only if none;
  never-released case stated. #1242 base rate on base plus the deferral
  prototype (`c2/proto-1242`): 5 of 5 QEMU runs, 26 of 128 iterations
  stale, first at 1, 1, 4, 1, 5 (`gauntlet-5-hw-pi5` cdb60557,
  `run5-hw/c2/qemu-1242-base/`). Register samples of 2 forced-tick hangs
  on the reverted design: CPU 3 in the LDAPR loop on cw_T->on_cpu holding
  rq_lock[3], CPU 0 in sched_rebalance_tick spinning on rq_lock[3] (91f0f36c,
  `qemu-nodeadlock/forced-reverted-ef11ee3c-sampled-SYMBOLS.txt`).
- C2 design critic round 2 (last allowed) sent at 05:33 UTC.
- Lab: C3 has held jetson-nano-2 since 05:01:41 UTC (claim stamp; the
  handover message of Entry 15 was sent at about that time, not 05:05) and
  is past its 05:30 end; told to release after the current boot_test call.
  Then C2 takes it for the R2 trace.

## Entry 19 (written 05:44 UTC, from date): C2 design verdict round 2, PASS

- Verdict: **PASS**, Fable 5.2, `critic-5-C2-design.md` round-2 section
  (gauntlet/switch-5 e7670dae), on ADR 0008 at `c2/switch-window-r2`
  6bc60aa3. The critic checked: the CPU-id pin adds no lock edge and no wait,
  and DAIF restore handles F under PI5_FIQ_TIMER; the next-ready walk is loads
  under the lock; a higher-priority deferred task can wait up to one tick
  behind a lower-priority one, as stated; masked check-then-WFI is sound and
  the poll calls schedule() at most once per 50 us. The critic counted the
  #1242 base rate itself: 26 of 128 iterations stale in 5 of 5 runs, first at
  1, 1, 4, 1, 5. Its prototype of the revised decision ran 300 of 300 (R3
  migrate, R3 wake, R2 x100 each) with 0 stale-CPU reports; one full make test
  failed only on #1230 litmus with other QEMUs on the host (one run, not
  attributed).
- Four ADR text corrections required, none changing a mechanism or bound: (1)
  the idle branch list is wrong against idle_task_func (every QEMU CPU and Pi 5
  CPU 0 without SECONDARY_PREEMPT unmask and WFI); state the rule by code shape;
  (2) do not rest on scheduler_start() running masked (#1198); (3) say why idle
  may read switch_in_waiting[c] unlocked; (4) x86 kicks in the masked prelude
  are dropped by the #1236 guard, bounded by one tick.
- **Orchestrator ruling** (the critic left it open): the brief's rule fails a
  design whose argument has gaps. These four are factual slips that can be
  decided from the code, not gaps in the no-cycle or ordering argument, so the
  PASS stands, on two conditions: C2 applies all four before fix code, and the
  fix critic checks the idle loop branch by branch against correction 1. If
  the user reads the rule more strictly, correction 1 is the point at issue;
  recorded for the final report.
- Critic round-2 evidence placed: `gauntlet-5-hw-pi5` 5a6d253f,
  `run5-hw/c2-design-critic/r2proto-*`, `critic-proto-adr0008-r2.patch`.
- C2 released to write the fix at 05:44 UTC. Extra fix-verdict rows requested by
  the critic: WORK_STEALING=ON, a COOP build, x86, and a branch-by-branch
  idle-loop check.
- Lab: C3 still holds jetson-nano-2 past 05:30 (last activity 05:42); release
  requested twice.

## Entry 20 (written 05:46 UTC, from date): C1 critic round 2 PASS; draft PR #1243

- C1 critic round 2: **PASS** for everything in scope, Fable 5.2,
  `critic-5-C1.md` Addendum 2 (gauntlet/switch-5 11c77871); reproduction
  `gauntlet-5-hw-pi5` 0c3f26c8, `run5-hw/c1-critic/round2/`. Fixes 1, 2,
  4, 5 done and reproduced (SLOT_TRACE fault run 71 lines, all prefixed, ends
  the dump; round 1's stopped-holder case 2 of 2 now prints; filter x50 50 of
  50; unfiltered 3041 pass, 0 fail, summary format matches the base). /ci
  accepted as green except #1230 (one retry of steps 4 and 5 under load).
  Row 1b (boot argument on the Jetson paths) unchanged and disclosed, still
  failing as written: **Dissent item for the user.**
- Not blocking, from the critic: ktest and `slmos.test=` have not run on
  hardware; a window of a few instructions at dump start can let one ordinary
  line print unlocked; summary.txt's load column is the end-of-step load (the
  start load is in each log's first line); the /ci logs carry no kernel banner.
- **Draft PR #1243** (head 2196deb5, base main, no closing keywords; checked
  with closingIssuesReferences). Its description lists the deviation and says
  every row is QEMU.
- C1 builder's draft entries are in Entry 6 and Entry 17. C1 complete.

## Entry 21 (written 05:51 UTC, from date): message delivery, corrected rule

C1 reports that none of the orchestrator's go messages (about 03:17 to 05:47
UTC) reached it until its turn ended, all at once. So teammate messages are
delivered when the receiving agent's turn ends, not between tool calls (Entry
11's rule of waits of 10 min or less did not help). Corrected rule, sent to
C2 and C3: wait on the agent's own background work in the foreground, but when
waiting for the orchestrator (a go, a handover, a verdict), report and end the
turn, since a message wakes an idle agent. The 03:12 to 05:00 C1 stall and
C3's 05:30 overrun both follow from this. C1 confirmed its 80
`run5-hw/c1/` files are intact on origin and stood down.

## Entry 22 (written 06:09 UTC, from date): C3 slot 2; handover to C2 for the rest

- C3 held jetson-nano-2 05:01 to 06:08 UTC (67 min; 38 min past its 05:30
  end, from the message-delivery problem of Entry 21). Restored: SD Linux,
  ESP 789cba62 by fresh read-only mount, `fsck.vfat -n` clean, no BootNext,
  BootCurrent 0001, staged elf removed. Disclosed slip: the ELF was copied to
  the board a few seconds before `claim_sbc` at about 05:01, inside the
  handover window, with no labctl action before the claim.
- Diagnosis head 5069e658 PMM_DEBUG=ON (60c548f3), 10 more full-suite kexec
  boots (07 to 16): test_pmm 48 pass, 2 fail (kexec coalescing), 1 ignored, and
  test_exhaustion_recovery passed in each; 0 free-list corruption beyond the
  self-test. Total **0 of 16** suite boots (`gauntlet-5-hw-jetson` 93cf0922,
  `run5-hw/c3/suite-kexec-60c548f3-boot01..16.txt`), plus 0 of 1200 filtered
  iterations. Against run 4's 1 of 10, P(0 of 16 | unchanged rate) = 0.19:
  weak evidence of a lower rate in this build, not a fix. The WALK_EVERY=0 and
  --no-helper arms were not run (no catch to discriminate).
- 9 of 16 suite boots ended later in Scheduler Tests with #1238-family faults
  (CPU-tagged dumps readable), several right after
  test_migrate_ready_task_updates_assigned_cpu, with ELRs such as
  0xa00000003, 0x500000003, 0xb00000001 (hypothesis: a packed value, not a
  PC); boot10 a silent hang after [FAIL] test_preempt_flag_follows_migrated_task
  and test_multiple_cores_isolated. Passed to C2.
- **Handover 06:10 UTC: jetson-nano-2 and pi-5-1 to C2** for 1 or 2 R2 trace
  boots (reduced from 3, disclosed) and then fix-head rows, release by about
  08:45 for the fix critic's boots. C3's 06:15 slot deferred: no reproducing
  build, and C2's rows are the run's main deliverable. C3 to finish offline and
  report #1207 as open.
- C2 pushed `c2/switch-window-r2` 4bfc15a5 at 05:51 UTC (ADR corrections or
  fix; to be reported).

## Entry 23 (written 06:14 UTC, from date): hour mark 8:00

- Lab: already with C2 since 06:10 UTC (Entry 22); no change.
- C3 asks that its 05:30 to 06:08 overrun be logged as a C3 rule break: it
  started three boot batches (boots 11 to 16) at 05:32, 05:43 and 05:52, after
  its slot had ended, chaining boot_test calls without ending a turn, so the
  release messages could not reach it. Recorded as a C3 rule break; boots 11
  to 16 are valid diagnosis rows run outside the slot. C3's 06:15 slot is
  cancelled; C3 rebased to `c3/pmm-halt` 321a91be and is writing its final
  report with #1207 open.
- C2: fix in progress on `c2/switch-window-r2`; R2 trace boots then fix-head
  rows. Behind the 8:00 mark (fix not yet reported); remaining budget 4 h.

## Entry 24 (written 06:15 UTC, from date): C2 fix pushed; QEMU proof; lab plan to the end

- ADR corrections applied at 253512a4; fix at `c2/switch-window-r2`
  **4bfc15a5** (sched.c, sched_diag.h), plus test-only 3b369aef (R1 helper
  migrates before it prints; at the fix tree 1 of 30 phase-A migrations had
  been refused because a UART line outlasted the 25 ms window; disclosed; the
  R1 base baseline failed by fault or hang before any assertion, so it stands).
- QEMU at 4bfc15a5 (builder claim, files `gauntlet-5-hw-pi5` 9c8e5928,
  `run5-hw/c2/qemu-fix/`; SWITCH_CHECK=ON SWITCH_WINDOW_TEST=ON, MTTCG):
  20 of 20 runs, each running R1, tick-forced, R3 migrate, R3 wake, R2 x10:
  1000 of 1000 iterations pass, 400 of 400 switch_check lines with
  bad_gen=0 bad_dual=0 stale_cpu=0, 0 panics. 5x thresholds from this run's
  baselines (iterations to first failure): R1 1 (5 needed), R3 migrate 5.55
  (28), R3 wake 1.1 (6), #1242 2.4 (12), no-deadlock 20 runs; each exceeded
  (200 per test).
- Hardware 5x thresholds sent to C2: Jetson kexec R3 migrate 1 failure in 454
  iterations, so at least 2270 clean; Jetson kexec R2 and R3 wake at least 5;
  Jetson UEFI R2 (38 of 200, mean 5.3) at least 27; Pi 5 R3 wake at least 5;
  Pi 5 R2 and R3 migrate never failed (run, cannot claim).
- Lab order to C2 through 08:45 UTC: 1 Jetson R2 trace boot on the base
  (reduced from 3, disclosed), fix-head reproducers, production boot_test x10
  on three paths, --assert-shares, bench smp base vs fix; full-suite boots
  only if time remains. QEMU regression rows run between boots. /ci at the
  final head by the orchestrator on a quiet host.

## Entry 25 (written 06:16 UTC, from date): C3 final report; C3 critic started

- C3 final head `c3/pmm-halt` **321a91be** (c1/harness 2196deb5 + one C3
  commit, PMM_DEBUG instrument). Hardware rows ran at diagnosis head 5069e658
  (c1/harness 6e39f425 + the same commit); none at 321a91be. Proposed end
  state: **#1207 open, #1228 the same defect** (decoded from run 4 captures:
  buddy_alloc split from PMM_ALLOC_FAIL because a free list held fewer blocks
  than free_counts; FAR bit 52 is index wraparound). Next experiment with
  images ready: about 20 PMM_DEBUG-off full-suite kexec boots (cc9a9cd7) for
  the rate, then WALK_EVERY=64 (7baa72d5), then a --no-helper arm for H1 once
  a build reproduces. Open check: the ELR instruction class is not verified
  against a matching ELF.
- No C3 PR: the brief opens one only for a fix. C3 suggests a diagnostics PR
  ("Refs #1207"); the orchestrator leaves it as a branch for the user to
  decide (final report), since the brief's PR table has no row for it.
- C3 critic `critic-c3` spawned on Fable at 06:16 UTC, blind, QEMU only: the
  brief's hardware reproduction (filtered loop, one Jetson boot per head) has
  no lab time while C2 holds the boards; recorded as not done unless time
  remains after 08:45.

## Entry 26 (written 06:29 UTC, from date): C2 fix head c0ea0714; orchestrator runs C2 /ci and WS=ON x20

- C2 fix head `c2/switch-window-r2` **c0ea0714** = 4bfc15a5 fix +
  68f0cb7b (the default build failed at 4bfc15a5 with -Werror: the off-build
  SWITCH_CHECK_CPU left lock_site unused) + c0ea0714 docs (docs/scheduler.md,
  kernel/CLAUDE.md). The 20 instrumented QEMU runs were at 4bfc15a5; C2 will
  re-run the proof cells at the final head. Disclosed: C2's runner wrote 5
  bogus "default config" files after that build failure (copying the
  previous log); deleted before commit; runner now refuses a failed build
  (919d4700).
- Builder rows at the fix head so far (`gauntlet-5-hw-pi5` 919d4700,
  `run5-hw/c2/qemu-fix/`, `fix-regress/`): default-config R3 migrate,
  R3 wake, R2 x20, 5 runs at 68f0cb7b: 300 of 300 pass; full make test at
  c0ea0714: 1 failure, #1230 litmus, load 4.42; x86 make test PASSED (load
  4.58). COOP_PREEMPT exists only on Pi 5 and Jetson builds, so the COOP row
  will be pi-5-1 with SECONDARY_PREEMPT=OFF in C2's lab slot.
- The orchestrator runs /ci steps 1 to 9 plus diff and `make test
  WORK_STEALING=ON` x20 at c0ea0714 in clean worktree s5-ci-c2, from 06:29 UTC
  (about 75 min). C2 and the C3 critic are asked to hold QEMU for it. Load at
  start 5.15 (their hold messages take effect when their turns end).

## Entry 27 (written 06:35 UTC, from date): C3 critic PASS on "open"; corrections to Entries 15, 22, 25

- C3 verdict: **PASS on the "open" branch** of the definition of done,
  Fable 5.2, `critic-5-C3.md` (gauntlet/switch-5 3aaa604d); critic
  evidence placed at `gauntlet-5-hw-pi5` 57cffa92, `run5-hw/c3-critic/`.
  Counted from raw files: filtered loop 1200 of 1200 in 6 boots, 0 PMM_DEBUG
  lines; 0 of 16 suite boots with a PMM halt; all 22 captures start at MB1 with
  banner 5069e658; 5069e658 and 321a91be carry the same C3 commit
  (range-diff). ELF check closed: the critic rebuilt 1b85b315, d512f40b and
  6c1f4c9c; ELRs are `strb` at 0x80024b6c (boot08, run06), `strb` at
  0x80024bcc (#1228) and `ldr` at 0x80024d48 (d512f40b), each matching
  WnR. PMM_DEBUG off: pmm.c.obj identical in size, 8 `__LINE__` immediates
  differ. QEMU at both heads: filtered loop 200 of 200 each; PMM_DEBUG=ON full
  suite at 321a91be 3039 pass, 0 fail.
- **Hardware reproduction by the critic: not done** (no lab time; C2 holds
  the boards). The definition of done's "critic reproduced the filtered loop on
  both heads" is met in QEMU only.
- **Corrections (the critic's fix 2):**
  - Entry 25 "#1228 the same defect" and Entry 15 "the halt needs state from
    earlier suites" were hypotheses written as findings. Corrected: three
    captures (1b85b315 boot08, 8a3406ea run06, 6c1f4c9c run01) share a
    signature, list head NULL with a non-zero count; **d512f40b run05 is
    different**, a next link in the order-12 list holding 0x0007000000000000,
    which a count defect alone cannot produce. That #1228 is #1207 is a
    hypothesis. Because the same instrumented build never halted in either
    mode, the isolated-versus-suite comparison shows nothing.
  - Entry 22 "weak evidence of a lower rate": at P = 0.19 it is no evidence
    either way.
  - Run 5 has no uninstrumented hardware row; all C3 hardware rows ran with
    PMM_DEBUG=ON.
  - 11 of 16 suite captures end mid-line after the PMM lines; the 4 cut fault
    dumps are not complete dumps for C2's use.
- The critic filed **#1244** (P2, Jetson kexec: the DTB blob lies inside the
  PMM heap unreserved, and DTB lookups fail after pmm_init on every kexec boot;
  not shown to cause #1207).
- Issue updates posted: #1207 (decode correction, run-5 rows, next
  experiment: about 29 PMM_DEBUG=OFF suite boots with a stop rule, a
  DTB-reserved arm for #1244, --no-helper, a light option-off check), #1228
  (signature match, labelled a hypothesis).
- Not done this run (the critic's fixes 4 to 6): the instrument's extra self-tests,
  the CMakeLists docs pointer, and the critic's Jetson boots. C3 ends **open**,
  with no PR; `c3/pmm-halt` 321a91be kept for the user to decide.

## Entry 28 (written 07:01 UTC, from date): C2 final-head QEMU proof; lab unused since 06:10

- C2 at final head c0ea0714 (builder claim; `gauntlet-5-hw-pi5` 74fe1a0e,
  `run5-hw/c2/qemu-fix/all-fix-c0ea0714-run01..20.txt`): 20 of 20 runs,
  1000 of 1000 iterations pass (R1, tick-forced, R3 migrate, R3 wake, R2, 200
  each), 400 of 400 switch_check lines with bad_gen, bad_dual and stale_cpu all
  0, load 4.9 at start. Idle branch-by-branch check from the disassembly of 6
  production builds (QEMU, Pi 5 default, Pi 5 SECONDARY_PREEMPT=OFF, Jetson
  default, Jetson JETSON_HW_TICK=OFF, x86) at eb6088a2,
  `fix-regress/idle-branch-check-c0ea0714.txt`. PI5_FIQ_TIMER not built.
- The lab has been C2's since 06:10 UTC, but C2 has not used it: none of the
  orchestrator's messages since 06:10 reached it, because it has not ended a
  turn (Entry 21). Lab time lost so far: about 50 min of the 06:10 to 08:45
  window. Superseding instruction sent at 07:02.
- C2's QEMU runs overlapped the orchestrator's /ci at c0ea0714 (started 06:29)
  because the hold message had not reached it either; /ci load is recorded per
  step.

## Entry 29 (written 07:21 UTC, from date): C2 images built; lab window restated

C2 reported at 07:21 UTC that all fix-head hardware images (c0ea0714) and the
base production kernels are built, and went idle, so the queued messages
reach it now. The lab window was restated as now to 08:45 UTC. Lab time unused
since the 06:10 handover: about 72 min. No claims on either board at 07:08
UTC.

## Entry 30 (written 07:49 UTC, from date): /ci and WORK_STEALING=ON x20 at c0ea0714; A/B started

Files: `gauntlet-5-hw-pi5` ef0bfa0e, `run5-hw/c0/ci-c2-c0ea0714/` (each
log's first line gives head, step, start, load; make's rc in summary.txt).
Run by the orchestrator in clean worktree s5-ci-c2, 06:29 to 07:39 UTC. The
host was not quiet: C2's QEMU proof overlapped (the hold message had not
reached it), plus the orphan QEMU.

- Steps 1 to 3, 4b (x86 PASSED), 6 (308 and 328 passed), 7 (0 violations, 0
  unparsed, 12 configurations), 8, 9 (all checks passed), diff: rc 0.
- Step 4 `make test` rc 2 (start load 5.92) and step 5 `AI_SCHED=ON` rc 2
  (3.83), each with exactly one failure, `test_litmus_patterns_pass`
  (#1230). Reruns pending (A/B below).
- **`make test WORK_STEALING=ON` x20: 13 passed, 7 failed.** All 7 failures
  are `test_litmus_patterns_pass` alone, with 0 panics and no other failing
  test, at start loads 3.44, 4.29, 5.84, 5.23, 5.79, 4.65, 5.49; passes ran at
  3.19 to 6.13. C0's base baseline was 3 of 3 passing at load about 2.1. As
  written, 7 of 20 failing is worse than 0 of 3; the loads are not comparable.
  An interleaved A/B (base 8ec0e330 and fix c0ea0714, WORK_STEALING=ON, 6
  each, alternating, same host conditions), plus reruns of steps 4 and 5 at the
  fix head, started at 07:49 UTC so the bar can be judged on matched load. If
  the A/B does not settle it, it goes to Dissent.

## Entry 31 (written 08:11 UTC, from date): possible regression at c0ea0714, a hang in coherency

- In the interleaved A/B (`gauntlet-5-hw-pi5` 952b04e6,
  `run5-hw/c0/ab-ws-c0ea0714/`), run fix-ws-3 (c0ea0714, `make test
  WORK_STEALING=ON`, default config) hit **TIMEOUT at 120 s**, start load
  4.22, with no dump. The serial (`fix-ws-3.test-output.log`, copied from
  the build directory before the next run cleaned it) ends at `[PASS]
  test_iteration_counts`, so it hung in `test_regs_every_cpu_reports`:
  `coherency_regs_collect` starts a pinned task on each other CPU and waits in
  `coh_wait_done`, capped at `COH_CAP_SECONDS` = 120 s. One CPU's pinned task
  therefore did not run or finish within the remaining time.
- Count so far: 1 timeout in 24 fix-head WORK_STEALING=ON full runs (20 in /ci,
  4 in the A/B), against 0 in the base's 3 (C0) plus 3 (A/B). No timeout was seen
  in the C0, C1 or C2 /ci default runs. Hypothesis, untested: a READY pinned
  task left unrun by the deferral (for example deferred while its CPU's idle
  read a NULL waiting pointer and waited in WFI). Sent to C2 to classify and to
  reproduce in QEMU (filter test_regs_every_cpu_reports,test_litmus_patterns_pass
  with WORK_STEALING=ON) after its lab slot; to be given to the fix critic as an
  open row.

## Entry 32 (written 08:14 UTC, from date): hour mark 10:00; C2 fix critic started

- C3 critic: done (Entry 27, PASS on "open").
- C2 fix critic `critic-c2-fix` spawned on Fable at 08:14 UTC, blind, on fix head
  c0ea0714. It started before C2's hardware rows were final (C2 holds the
  boards until 08:45 UTC), because of the budget: the code head is final
  unless the open hang (Entry 31) leads to a change, and the critic counts the
  hardware rows as they land. Disclosed as a departure from "after the
  builder's final push". It holds QEMU until the A/B ends (about 08:40) and
  gets its Jetson boots (R2 and R3 x200, both heads) through an orchestrator
  handover after 08:45. Verdict due by 09:50 UTC. The hang is given as an open
  row for it to investigate independently.

## Entry 33 (written 08:31 UTC, from date): A/B result

Files `gauntlet-5-hw-pi5` 920a641a, `run5-hw/c0/ab-ws-c0ea0714/`
(summary.txt plus one log per run; start load in each log's first line).
Interleaved base 8ec0e330 and fix c0ea0714, `make test WORK_STEALING=ON`,
07:49 to 08:25 UTC, one at a time, under the same host conditions:

| Tree | Runs | Passed | Litmus-only failures | Timeouts |
|---|---|---|---|---|
| base 8ec0e330 | 6 | 5 | 1 (base-ws-4) | 0 |
| fix c0ea0714 | 6 | 4 | 1 (fix-ws-4) | **1 (fix-ws-3, Entry 31)** |

Combined with the /ci run (Entry 30): at the fix head, litmus-only failures are
8 of 26 WORK_STEALING=ON runs; at the base, 1 of 9 (C0 3 at load about 2, A/B
6). On matched load the litmus rates are equal (1 of 6 each), so no litmus
regression is shown. The one timeout (1 of 26 fix runs, 0 of 9 base runs) is
the open row. /ci step reruns at the fix head: step 4 `make test` failed again
on litmus alone (04r, load 3.51; 0 of 2 at this head), step 5 `AI_SCHED=ON`
passed (05r, load 5.15). The "/ci green" and "WORK_STEALING=ON no worse"
rows are therefore **not met as written**; whether #1230 under load excuses
them goes to the fix critic and to Dissent.

## Entry 34 (written 08:37 UTC, from date): C2 fix-head hardware rows; handover to the fix critic

C2 held both boards 07:24 to 08:35 UTC (71 min of the 06:10 to 08:45 window;
the 06:10 handover reached it at about 07:21). Restored: Jetson ESP 789cba62
(fresh read-only mount, fsck clean), no BootNext, BootCurrent 0001; pi-5-1
originals renamed back (boots b42b2c1d); both powered on. Head c0ea0714
throughout. Files `gauntlet-5-hw-jetson` 1b017c7a and `gauntlet-5-hw-pi5`
e6745ebe, `run5-hw/c2/hw-fix/`. Builder claims, for the critic:

| Cell (fix head, test kernels) | Result | 5x threshold |
|---|---|---|
| Jetson kexec R3 migrate | 3 boots x800, 2400 of 2400 | 2270 |
| Jetson kexec R2 | 1 boot, 200 of 200 | 5 |
| Jetson kexec R3 wake | 1 boot, 200 of 200 | 5 |
| Jetson UEFI R2 | 1 boot, 200 of 200 (capture began before the deploy reboot: Linux shutdown and MB1, not a cold power-on) | 27 |
| Jetson UEFI R3 wake | **not run** (time) | 1-boot baseline |
| Pi 5 R2, R3 migrate (never failed at baseline) | 200 of 200 each | cannot claim |
| Pi 5 R3 wake | 200 of 200 | 5 |
| Pi 5 COOP (SECONDARY_PREEMPT=OFF) R3m+R3w+R2 x100 | 300 of 300 | extra row |

Regression rows (production kernels at c0ea0714): Pi 5 boot_test **10 of 10**;
Jetson kexec boot_test **3 of 3 only** (10 required); Jetson UEFI boot_test and
`--assert-shares` 20 of 20: **not run**. Full integration suite boots at the
fix head: **not run**. Context-switch time (the shell's context bench; `bench
smp` prints no time): Pi 5 base 106/106 ns, fix 110/112 ns (+4 to +6% mean,
p50 111 both); **Jetson kexec base 87/88 ns, fix 96/98 ns (+10 to +13% mean,
over the 10% bar)**, p50 96 and p95 128 identical, counter resolution 32 ns, 2
runs on one boot each. Dissent material.

Base R2 Jetson trace (1 boot, reduced from 3): dumped at the first busy
switch-in, CPU 2 about to switch in pdm task id 10 with on_cpu=1 (the #1234
precondition). Limitation: the SLOT_TRACE dump prints only CPUs 0 to 3
(slot_trace.c caps at 4), so the CPU holding on_cpu (4 or 5) and the enqueue
that queued the task are missing; the #1238 sequence is only partly traced.
C3's 16 suite captures not yet classified by C2.

**Handover 08:37 UTC: jetson-nano-2 to critic-c2-fix** (kexec only, 5 boots,
until 09:20 UTC at the latest). pi-5-1 unheld.

## Entry 35 (written 08:51 UTC, from date): the fix-ws-3 timeout, C2's reproduction and classification

- C2 (builder; `gauntlet-5-hw-pi5` 41b2b495, `run5-hw/c2/qemu-regs-hang/`):
  filter `test_regs_every_cpu_reports,test_litmus_patterns_pass` x200,
  WORK_STEALING=ON, default config. Fix c0ea0714: regs 200 of 200 (max 25 ms),
  litmus 149 of 200 (load 3.0 to 4.1). Base + C1 2196deb5: regs 200 of 200,
  litmus 88 of 200 (load 3.6 to 3.9). No hang in 400 regs iterations; the
  litmus failure rate is not higher at the fix head (one run each).
- C2's classification from code: a fresh coh_remote_start task has on_cpu 0 and
  is pinned, so it cannot be deferred; the masked read excludes idle sleeping
  with switch_in_waiting set; an idle CPU in WFI misses a cross-CPU SEV until its
  next tick, as on the base (bounded by one tick). No new path found; status
  "not explained".
- Correction to Entry 31: fix-ws-3 hit `make test`'s 120 s TEST_TIMEOUT, not
  the coherency cap. Its wall time was 3:31 against 2:45 to 2:55 for the other
  A/B runs, so "hung in test_regs_every_cpu_reports" is not established; it
  may have been a slow run that was in that test at 120 s. Hypothesis either
  way.
- Decision experiment running (C2, until 09:40 UTC): `make test
  WORK_STEALING=ON TEST_TIMEOUT=300` at c0ea0714, about 8 runs, QEMU elapsed
  time recorded, QMP register samples if a run passes 120 s.

## Entry 36 (written 09:03 UTC, from date): C2 final report

C2 final head `c2/switch-window-r2` **c0ea0714** (unchanged since the
orchestrator's /ci). New since Entry 35 (`gauntlet-5-hw-pi5` e1332e6e,
`run5-hw/c2/qemu-regs-hang/`): regs x2000 WORK_STEALING=ON, fix 2000 of
2000 and base 2196deb5 2000 of 2000 (max 27 and 28 ms); 3 full `make test
WORK_STEALING=ON` at TEST_TIMEOUT=240 with a QMP sampler armed, 3 of 3 pass,
never sampled. The fix-ws-3 timeout stays **open, not reproduced**: 1 of 26
fix runs against 0 of 9 base runs at the 120 s limit. C2 proposes #1234 and
#1242 fixed, and #1238 closed with #1234 on the R2 rows (kexec 200 of 200, UEFI
200 of 200), noting the partial trace. Builder's draft entry, written after its
critic files, follows in the close-out. Temporary branch tips archived (patch
and log) under `run5-tools/deleted-branch-tips/` in this commit, for deletion
after the fix critic files.

## Entry 37 (written 09:26 UTC, from date): decision experiment on the fix-ws-3 timeout

C2, `make test WORK_STEALING=ON TEST_TIMEOUT=300` at c0ea0714, 13 runs one at
a time, 09:04 to 09:25 UTC, load 2.8 to 5.0 at start (`gauntlet-5-hw-pi5`
c4416b8d, `run5-hw/c2/qemu-regs-hang/full-ws-t300-c0ea0714-run01..13.txt`,
each ending with its elapsed line): QEMU elapsed 80 to 87 s in every run; none
reached 120 s, so no register sample was taken. 8 of 13 passed; 5 of 13 failed
only on #1230 litmus; 0 timeouts, 0 hangs. Fix head total: 1 timeout in 39
full WORK_STEALING=ON runs. fix-ws-3 was at least 120 s of QEMU time against
80 to 87 s in every measured run, so it was an outlier of 40% or more: a
transient slowdown or a stall that recovered or did not; not decided, since it
was not sampled. Stays an open row, reported, not filed as a defect without a
reproduction.

## Entry 38 (written 09:32 UTC, from date): C2 fix verdict FAIL; close-out

### C2 fix verdict

**FAIL** on the bars as written, not on the fix. Fable 5.2,
`critic-5-C2.md` (gauntlet/switch-5 acb60dee). Holds: the code implements the
round-2 design with all four corrections; every idle branch that unmasks before
its wait masks, reads, waits if NULL and unmasks (source, and the Jetson
disassembly); the critic's own lock graph, written first, has no wait under a
lock and no cycle; no-deadlock bar met; R1, R2, R3 at 0 under the 5x rule on
every cell run; the critic reproduced on both heads in QEMU and Jetson kexec (fix
R2, R3m, R3w 200 of 200 each; base R2 and R3w fail). Fails: (1) missing
production rows: Jetson boot_test x10 (kexec 3, UEFI 0), --assert-shares 20 of
20, full-suite boots on all three paths; (2) Fixed cell missing: Jetson UEFI R3
wake; (3) the bench row is `bench context` without raw capture, Jetson mean +10
to +13%; (4) no green plain `make test` at c0ea0714 (0 of 3, #1230 litmus;
the base fails it at least as often under equal load); (5) the timeout, open,
1 in 48 full fix-head runs against 0 in 9 base, filed as **#1260**. Critic
evidence placed: `gauntlet-5-hw-pi5` 904277d6 and `gauntlet-5-hw-jetson`
33efdf51, `run5-hw/c2-fix-critic/` (ELFs listed by sha256, not committed).
**No C2 PR** (the brief opens one only on a pass). ADR 0008 stays "proposed" on
`c2/switch-window-r2`.

C2 builder entry (its draft, written now that the critic has filed): see Entry
36 and C2's final report; baselines and fix-head table as in Entries 8, 24, 34;
disclosed bar failures as above.

### Branch table

| Branch | Head | State |
|---|---|---|
| gauntlet/switch-5 | this commit | log, brief, notes, critic files, run5-tools/ |
| gauntlet-5-hw-pi5 | 904277d6 | evidence: Pi 5 and QEMU |
| gauntlet-5-hw-jetson | 33efdf51 | evidence: Jetson |
| c1/harness | 2196deb5 | **draft PR #1243** |
| c2/switch-window-r2 | c0ea0714 | kept: fix candidate for #1234/#1242, critic FAIL on rows, no PR |
| c3/pmm-halt | 321a91be | kept: PMM_DEBUG instrument, no fix, no PR; user to decide |
| deleted: c2/proto-1242, c2/reverted-wait, c2/reverted-wait-r2, c2/switch-window, c2/trace, c2/trace-r2 | see run5-tools/deleted-branch-tips/ | archived (c90df115), deleted 09:32 UTC |

Local worktrees from this run (s5-*, critic5-c3-*, critic5c2-*, s5-crit-*) are
left in place for the user; none holds unpushed run-5 evidence except
`s5-crit-c2d`, which is dirty with the design critic's probe (its evidence is
placed). Two worktrees had `gauntlet-5-hw-pi5` checked out at once (s5-hw-pi5,
s5-hw-pi5-c2): the likely cause of both stale-index deletions.

### Lab end state (09:15 UTC check)

jetson-nano-2: online, powered on, released; SD Linux (BootCurrent 0001); ESP
`SLMOS.efi` **789cba62** (read-only mount); no BootNext; BootOrder unchanged;
`/root/slmos.elf` 098f3163; no run-5 files in /root (68 older staged ELFs from
runs 3 and 4 remain). pi-5-1: online, powered on, released; original kernels
renamed back by C2 (boots b42b2c1d), not re-read by the orchestrator. An orphan
QEMU (PID 3381445, about 46 h, not from this run) is still running.

## Final report (written 09:32 UTC, from date)

### Dissent

1. **C2 fix: the bars fail, the rows support the fix.** The bar requires every
   regression row at the fix head; the critic failed C2 on missing rows (Jetson
   boot_test kexec 3 of 10 and UEFI 0 of 10, --assert-shares, suite boots, UEFI
   R3 wake) and on a Jetson context-switch mean of +10 to +13%. The rows that
   exist all favour the fix: every baseline-failing cell run at the fix head is
   at 0 beyond 5x (QEMU 1000 instrumented iterations; Jetson kexec R3 migrate
   2400 of 2400; R2 200 of 200 on kexec and UEFI; Pi 5 and COOP 200 to 300 of
   300), reproduced by the critic on Jetson kexec, and the no-deadlock test hangs
   the reverted design 20 of 20 and completes at the fix head. The Jetson bench
   delta is 2 runs per side at 32 ns resolution with identical p50 and p95. Files:
   `critic-5-C2.md`; `run5-hw/c2/hw-fix/`; `run5-hw/c2-fix-critic/`. For the
   user: open a PR from `c2/switch-window-r2` now with those rows run first, or
   accept the bench delta. Most of the missing rows were lost to message-delivery
   delays (Entries 21, 28, 29: about 70 min of the C2 lab window).
2. **/ci green and WORK_STEALING=ON "no worse".** Plain `make test` failed
   only on `test_litmus_patterns_pass` (#1230) at every head under host load;
   at matched load base and fix fail it equally (A/B 1 of 6 each; critic 5 of 15
   base and 2 of 15 fix). As written, C1's /ci passed only on retry and C2's has
   no green plain run. For the user: whether #1230 under load excuses "/ci
   green". Files: `run5-hw/c0/ci-*`, `run5-hw/c0/ab-ws-c0ea0714/`.
3. **C1 boot argument.** The brief asked for a filter on the command line or a
   boot argument the kexec and UEFI paths can both pass; both Jetson paths use a
   build-time filter instead (no board change possible). C1's critic passed
   everything else and left this row failing as written. PR #1243 states it. The
   user accepts or rejects.
4. **Orchestrator ruling on design round 2** (Entry 19): four ADR text
   corrections were treated as slips, not argument gaps; the fix critic later
   confirmed the code follows correction 1 branch by branch.

### Issues

| Issue | End state | Deciding numbers |
|---|---|---|
| #1234 | **open**, fix candidate on `c2/switch-window-r2` c0ea0714, no PR | critic FAIL on missing rows; all run cells 0 under 5x |
| #1238 | **open**, not split; closes with #1234 | R2 base kexec 3 of 3 boots fail, UEFI 38 of 200; fix 200 of 200 each; trace partial (#1259) |
| #1242 (new) | **open**, fixed on the same branch | base+prototype 26 of 128 stale; fix 0 in 400 |
| #1207 | **open**, capture build ready, no first fault | 0 of 16 instrumented suite boots, 0 of 1200 filtered (P = 0.19 under run 4's rate; no evidence either way) |
| #1228 | **open**, matches #1207 by signature (hypothesis) | 3 of 4 run-4 captures share the signature |

### Pull requests and merge order

- **#1243** (C1, draft, head 2196deb5): merge first. No closing keywords.
- C2: none (critic FAIL). C3: none (no fix). If C2's rows are completed and a PR
  opened, rebase onto main after #1243 and re-run /ci and the Jetson R2 cell.

### Context switch, base 8ec0e330 vs fix c0ea0714 (`bench context`, 2 runs each, one boot)

Pi 5: 106/106 ns to 110/112 ns (+4 to +6%, p50 111 both). Jetson kexec: 87/88
ns to 96/98 ns (+10 to +13%, p50 96 and p95 128 both). `bench smp` prints no
time. Files: `run5-hw/c2/hw-fix/bench-context-*.txt`.

### Tracker delta

Opened: #1240 (gpu_stub/gpu_nvidia half-block leak), #1241 (migrate rewrites
assigned_cpu), #1242 (stale this_cpu in schedule()), #1244 (Jetson kexec DTB
inside the PMM heap), #1259 (SLOT_TRACE dumps only CPUs 0 to 3), #1260 (one
WORK_STEALING=ON timeout at the fix head). Closed: #1199 and #1224 (by merging
#1239 in C0, on the user's approval). Comments: #1207, #1228, #1234, #1238,
#1242. File-don't-fix: all six opened.

### What the run could not do

C2: Jetson boot_test x10 on both paths, --assert-shares 20 of 20, full-suite
boots, UEFI R3 wake at the fix head, R2 base trace beyond 1 partial boot. C3: a
first fault, an uninstrumented rate, and the critic's Jetson reproduction. The
fix-ws-3 timeout is unexplained.

### Single most useful next step

Run the missing C2 rows at c0ea0714 (Jetson boot_test x10 on kexec and UEFI,
--assert-shares 20 of 20, UEFI R3 wake x200, 5 suite boots per path, bench smp
with 5+ raw runs per side) in one uninterrupted lab session, then open the #1234
PR.

### Lab state

Both boards online, powered on and released; jetson-nano-2 on SD Linux with ESP
`SLMOS.efi` 789cba62 and no BootNext; pi-5-1 on its original kernels.

## Entry 39 (written 22:40 UTC, from date): close-out session for #1234 rows; critic FAIL; no PR

Single-agent session (2026-11-14, 17:50 to 22:40 UTC), the user's close-out
request after the final report. Boards one at a time. Evidence
`gauntlet-5-hw-jetson` aa080076 and `gauntlet-5-hw-pi5` 62a7bbe6, both
`docs/archive/switch-gauntlet/run5-hw/closeout/`, one file per boot or run,
banner at the top, written from this session's own labctl output only.

### Head

`c2/switch-window-r2` rebased onto main (dropping the four C1 commits that
#1243 squash-merged): first onto 1c95b412 (cf16b53d), then, because #1245
merged during the session, onto dc51107c; plus one docs commit (ADR 0008
status "accepted", the 0008 row in `docs/adr/README.md`, rule 5 in the
task-lifecycle section of `docs/architecture.md`). **Head 6a4cfa93**,
pushed with lease over c0ea0714. Kernel code identical to c0ea0714 (critic:
range-diff `=` on all 13 fix commits, empty diff outside docs/.github/
scripts/ci). The docs commit was written before the rows so that every
banner names the PR head; with no PR it stays on the branch, status
"accepted" premature (user to decide: keep for a later PR or drop).

### /ci at 6a4cfa93 (`gauntlet-5-hw-pi5` 56d54962, `closeout/ci-6a4cfa93-*.txt`)

All steps pass except step 4 plain `make test`: failed on
`test_litmus_patterns_pass` alone (#1230), load 2.08. Three more plain
`make test` runs, one at a time, loads 1.35 to 2.28: **3 of 3 pass**.
Plain `make test` at the head: 3 of 4, the one failure litmus-only.

### Rows (all at 6a4cfa93; base bench rows at dc51107c)

| Row | Files | Result |
|---|---|---|
| Jetson kexec production boot_test x10 | `kexec-prod-6a4cfa93-boot01..10.txt` | 10 of 10 |
| Jetson UEFI production boot_test x10 | `uefi-prod-6a4cfa93-boot01..10.txt` | 10 of 10 (boot01 cold-booted on the deploy's armed BootNext, disclosed) |
| `--assert-shares` x20, kexec | `assert-shares-kexec-6a4cfa93-x20.txt` | 20 of 20 PASS |
| R3 wake x200, UEFI, 1 boot | `uefi-r3w-6a4cfa93-boot01.txt` | **199 of 200**: iteration 116 NOT-FINISHED, bad_gen=0, two_cpus=0; filed **#1262** (P1). 2 supplementary boots 400 of 400 (`-extra-boot01,02`) |
| R2 x200, kexec | `kexec-r2-6a4cfa93-boot01.txt` | 200 of 200 |
| Suite, Jetson kexec, 5 boots | `kexec-suite-6a4cfa93-boot01..05.txt` | 5 complete, 14 failures each, all #1109; 0 halts (one truncated capture labelled INCOMPLETE, not counted) |
| Suite, Jetson UEFI, 5 boots | `uefi-suite-6a4cfa93-boot01..05.txt` | 5 complete, 21 each: #1109 plus the 9 of #1231; 0 halts |
| Suite, Pi 5, 5 boots | `pi5-suite-6a4cfa93-boot01..05.txt` | 5 complete, 18/18/18/3/18, all #1109; 0 halts |
| bench context, Jetson kexec | `bench-context-jetson-*`, `bench-context-summary.txt` | base 88.0 ns (sd 2.7), head 97.3 ns (sd 2.0), **+10.6%**; p50 96/96, p95 128/128 |
| bench context, Pi 5 | `bench-context-pi5-*` | base 100.7 ns (sd 1.0), head 109.0 ns (sd 1.4), +8.3%; p50 93 to 111, p95 111.5 to 112 |

Bench: 3 boots per side, alternating, 2 runs per boot (6 per side, 99
round trips each). Counter resolution 32 ns (Jetson) and about 18.5 ns
(Pi 5) per sample, so p50/p95 move only in whole steps; the run means
differ by more than the run-to-run spread on both boards (no overlap;
Welch t 6.9 and 11.7). The Jetson slowdown is measured, not noise, and is
over the 10% bar.

The migration test (#1238) and both R3 tests passed in all 15 suite boots.

### #1207 / #1244 (code reading, comments posted)

On kexec the DTB sits 0x5000 to 0x7000 below `__kernel_end`: the arm64
header's `image_size` is the file size (`boot.S:48`,
`kernel-jetson.ld:123`, `.bss`/`.stack` excluded), and kexec-tools places the
DTB at the first free page above `image_base + image_size`
(`kexec-arm64.c:766,820`). The PMM reserves only the blob's own
memreserves and the GA10B/boot-model user reserves (`pmm.c:588-601`), and
heap region 1 starts at `PAGE_ALIGN_UP(__kernel_end)` (`pmm.c:643,717`).
The blob overlaps the heap by about 950 KB on every kexec boot (UEFI-path
blob size; kexec size not printed); the first free-block heads lie inside
it. Relation to #1207 is a hypothesis, untested. Comments: #1244, #1207.

### Critic

Fable 5.2, blind, `critic-5-closeout.md` (this commit). **VERDICT: FAIL**
on two rows: R3 wake UEFI boot01 (1 failure in 200, bar 0) and Jetson
bench context (+10.6%, 95% interval +6.6% to +14.6%, bar 10%). All other
rows pass. It also notes that after iteration 116 the boot's `moves=`
rose from about 5x10^4 to about 6x10^4 per iteration and `Cannot migrate
running task` warnings went to 139 (4 in each extra boot). **No PR**
(the user's rule: PR only on a critic pass).

### Incidents

- labctl stdio server failed every call from about 18:38 to 19:11 UTC (the
  lab's labctl services restarted about 18:35 UTC, not by this session);
  work paused until the user reconnected it. The reconnected session saw
  this session's own claim as foreign; force-released (reason logged) and
  re-claimed at 19:12 UTC, gap under a minute; the trigger loop was
  stopped first. Kexec batch b's first, aborted trigger log was
  overwritten by the rerun's (it had fired nothing).
- Kasa plug timed out once on the Pi 5 restore; nothing written; retried.

### Lab end state (22:35 UTC)

jetson-nano-2: released, powered on, SD Linux (BootCurrent 0001), no
BootNext, ESP `SLMOS.efi` 789cba62 (fresh read-only mount, `fsck.vfat -n`
clean), `/root/slmos.elf` 098f3163; this session's 4 staged ELFs deleted.
`/root` holds 48 `*.elf*` files (not 68): deletion of old ELFs awaits the
user's choice of list. pi-5-1: released, powered on, original
`kernel_2712.img` restored and booted (banner b42b2c1d).

## Entry 40 (written 22:42 UTC, from date): Jetson /root cleanup

On the user's choice (48 `*.elf*` found, not 68): deleted the 34 ELFs dated 2026-11-08 to 11-11 (runs 3 and 4: `slmos-1193*` x10, `slmos-1194*` x3, `slmos-main85f5-test`, `slmos-r3-*` x8, `slmos-closeout-*` x3, `slmos-fixc-*` x4, `slmos-l4-*` x5). Kept `/root/slmos.elf` (098f3163) and the 13 older files. Both boards released and powered on.

## Entry 41 (written 03:51 UTC, from date): follow-up session for #1262 and #1244

Single-agent session (2026-11-14 22:55 UTC to 2026-11-15 03:51 UTC), the
user's follow-up request after Entry 39: #1262 (R3 wake NOT-FINISHED at the
#1234 fix head) and #1244 (kexec DTB inside the PMM heap). Boards one at a
time. Evidence: `gauntlet-5-hw-jetson` 47993ea4 and `gauntlet-5-hw-pi5`
5cff7385, `docs/archive/switch-gauntlet/run5-hw/1262-1244/`, one file per
boot, each from power-on with the banner in the header, written from this
session's own labctl output. Every Jetson boot used the host-side trigger
loop (`trigger.sh.txt`), 1 to 3 boots per `boot_test` call.

The user recorded up front that the context-switch cost at the fix head
(+9 ns; +10.6% Jetson, +8.3% Pi 5) is accepted; the 10% bar of the brief no
longer blocks.

### Heads

| Branch | Head | What |
|---|---|---|
| `c2/switch-window-r2` | ae1399ba | 6a4cfa93 + the `SWO_DIAG` build option (5 commits: 29d58c22 dump, a6c65318 slow-finish dump, 6a0192d9 beacon and reap timing, 1441f690 prelude timing, ae1399ba rq-lock owner and counted try-lock) + documents (scheduler.md, kernel/CLAUDE.md, the cost ledger `docs/perf/context-switch.md`). No kernel change with the option off: `.text` is 2,479,448 bytes (Jetson production) and 2,541,752 bytes (Jetson R3 test kernel) at both ae1399ba and 6a4cfa93, no `swo-diag` string in the image (`textsize-swo-diag-off-ae1399ba.txt`; the same at 29d58c22, `textsize-swo-diag-off.txt`). |
| `fix/kexec-dtb-reserve` | 661204e6 | main dc51107c + the #1244 change |
| `diag/1244-check-on-main` | 665fb18e | main dc51107c + the #1244 boot-time check only; local, evidence only |
| `diag/1262-padded-locks` | 741497c7 | ae1399ba + one cache line per `rq_lock` / `steal_deque_lock` element; local, experiment only |

### Part A rows: R3 wake (`test_wake_during_switch_out`), jetson-nano-2

| Row | Files | Result |
|---|---|---|
| UEFI-direct, x200, 10 boots, `SWO_DIAG` (29d58c22) | `uefi-r3w-diag-29d58c22-boot01..10.txt` | 2000 of 2000; no NOT-FINISHED, no dump |
| UEFI-direct, x200, 5 more boots (a6c65318, adds a dump at 30 ms into the finish) | `uefi-r3w-diag2-a6c65318-boot11..15.txt` | 1000 of 1000; no NOT-FINISHED; no 30 ms dump (2 to 7 finishes of 16 to 27 ms per boot, printed without a dump) |
| kexec, x200, 5 boots (a6c65318) | `kexec-r3w-diag2-a6c65318-boot01..05.txt` | 1000 of 1000; one 30 ms dump (boot 05, iteration 53) |
| kexec, x1000, 2 boots (6a0192d9: beacon, dump at 10 ms) | `kexec-r3w-diag3-6a0192d9-x1000-boot01,02.txt` | 2000 of 2000; 6 and 14 slow finishes |
| kexec, x1000, 2 boots (1441f690: prelude timing) | `kexec-r3w-diag4-1441f690-x1000-boot01,02.txt` | 2000 of 2000; 13 and 3 slow finishes; 17 and 4 iterations with a `schedule()` prelude over 1 ms, all on CPUs 4 and 5 |
| kexec, x1000, 1 boot (ae1399ba: lock owner, counted try-lock) | `kexec-r3w-diag5-ae1399ba-x1000-boot01.txt` | 1000 of 1000; 4 slow finishes; 10 such iterations; all 14 reported own-lock waits began with the lock word 0 and no owner recorded |
| UEFI-direct, x1000, 1 boot (ae1399ba) | `uefi-r3w-diag5-ae1399ba-x1000-boot01.txt` | 1000 of 1000; 30 slow finishes, 49 iterations with a prelude over 1 ms, all on CPUs 4 and 5; all 83 reported own-lock waits began with the lock word 0 and no owner recorded |
| **Experiment**, kexec, x1000, 2 boots (741497c7, padded locks) | `kexec-r3w-padlocks-741497c7-x1000-boot01,02.txt` | 2000 of 2000; **0 slow finishes, 0 preludes over 1 ms**, slowest iteration 315 ms (unpadded boots: 754 to 812 ms) |

**Rate of #1262 (NOT-FINISHED, a 2 s stall).** This session: 0 in 3000
iterations on UEFI-direct at x200 (15 boots) and 0 in 1000 on kexec at x200
(5 boots); 0 in the x1000 boots as well (1000 on UEFI-direct,
5000 on kexec, unpadded). With Entry 39 (1 in 600 on UEFI-direct): 1 in
3600 at x200 on UEFI-direct (1 in 4600 counting the x1000 boot), 0 in 6000
on kexec this session. The failure was not
reproduced, so no dump of the 2 s case exists.

Disclosures. (0) A UEFI-direct capture starts at power-on, boots SD Linux,
and the trigger reboots into SLM-OS through BootNext: SLM-OS starts after a
warm reset, inside a capture that begins at power-on (the method of Entry
39). (1) The first image (29d58c22, UEFI boots 1 to 10) embedded a
32-byte stub operator library instead of the 308 KB one: `kernel-test` in a
clean worktree packs the library only after a Jetson production build has
generated its inputs. Scheduler and test code are the same; `.rodata` is
308 KB smaller. Boots 11 to 15 and every later image carry the full
library. (2) The 6a0192d9 banner says `-dirty`: the tree had uncommitted
documentation only. (3) The diagnostics changed between rows as shown; the
failure condition of the test did not.

### What the dumps show

**Facts** (counts checked by the critic).

- 71 dumps of slow finishes on the unpadded kernels (kexec: 1 at 30 ms and
  40 at 10 ms, 486 per-CPU lines, 93 task lines; UEFI-direct: 30 at 10 ms,
  360 per-CPU lines, 94 task lines). `switch_in_waiting[]` is NULL in all
  846 per-CPU lines and no task line names a waiting CPU. Each dump is one
  snapshot at the 10 or 30 ms mark; a deferral earlier in the wait would
  not appear.
- `preempt_disabled[]` is 1 only on CPUs 4 and 5 (47 kexec lines, 30
  UEFI-direct lines). `resched_trampoline` sets it before it calls
  `schedule()` (`kernel/arch/arm64/vectors.S:593`), so a CPU inside a
  tick-driven `schedule()` shows 1, and thieves skip it for that long
  (`sched_try_steal`'s `preempt_disabled[victim]` early-out).
- The unfinished work is on CPUs 4 and 5 (the second cluster). The prelude
  timing (three kexec boots and the UEFI-direct boot) has 140 per-CPU
  preludes over 1 ms, all on CPUs 4 and 5, 1.06 to 6.64 ms from entering
  `schedule()` to holding the CPU's own `rq_lock`, tick masked (ADR 0008
  part 1 masks from entry). 121 of the 140 spent that time waiting for the
  CPU's own `rq_lock`; 17 spent it in the steal attempt, whose locks are
  not instrumented; 2 in neither alone. The duration is often the same on
  CPU 4 and CPU 5 in one iteration (6256/6256, 2415/2416, 4092/4093 us).
  **No wait is longer than 6.64 ms** on either path.
- The counted try-lock (ae1399ba) reports 97 own-lock waits (14 kexec, 83
  UEFI-direct, 1.1 to 6.6 ms). In all 97 the lock word was 0 when the wait
  began and no owner was recorded then or 1 ms later; the failed try-locks
  number up to 14,989, and six waits of 1.1 to 3.0 ms have only 2 to 8.
  The counter does not split "saw the lock held" from "the exclusive store
  failed", and the lock word is sampled once.
- 26 first-dump lines of CPU 4 or 5 show the CPU still before its lock
  (phase 1) with the lock word reading 1, so the lock is at times held
  (by another CPU, or just taken by this one) during a wait.
- One dump as an example (`kexec-r3w-diag3-6a0192d9-x1000-boot02.txt`,
  t=272834009): CPUs 4 and 5 each have idle as current and two READY
  workers queued, `preempt_disabled` 1 on both; CPU 5 entered `schedule()`
  4757 us earlier and is at phase 1 with its lock word reading 1; CPU 4 is
  at phase 3 (lock held). About half of the first-dump CPU 4/5 lines look
  like this (idle current, flag 1); the rest show a worker still current.
- Diagnostic artefact: `age=554189327 us` or `...328 us` on 43 per-CPU
  lines. Explanation, not an observation: an unsigned wrap, the phase stamp
  being newer than the snapshot's start; read it as about 0.

`SPINLOCK_ARRAY` (`kernel/include/spinlock.h:104`) aligns the array, not
its elements: all `rq_lock[]` words share one 64-byte line, and so do all
`steal_deque_lock[]` words.

**Experiment, one variable** (741497c7: the same kernel with each element
of both arrays on its own cache line): 2000 iterations on kexec with no
slow finish, no prelude over 1 ms and no iteration over 320 ms (slowest
315 ms). The five unpadded x1000 kexec boots had 6, 14, 13, 3 and 4 slow
finishes (40 in 5000) and 6, 14, 17, 5 and 10 iterations over 320 ms; the
three that time the prelude had 17, 4 and 10 iterations with one over
1 ms; slowest iterations 754 to 812 ms (UEFI-direct: 30, 49, 827 ms). Two
boots, one image, kexec only; no padded UEFI-direct run.

**Inferences, and how far the evidence carries them** (the critic rated
both "partly supported").

1. *The slow finishes are not the fix's deferral path.* Supported for the
   71 dumped stalls. Not shown for #1262 itself, which was never captured.
   "Predates the fix" has no control run: main cannot run R3 wake (it hangs
   at iteration 1), so no pre-fix kernel was measured with these
   diagnostics.
2. *The millisecond stalls come from the lock words sharing a cache line.*
   The experiment implicates the shared lines: padding removes the stalls.
   That the mechanism is the `stxr` of the `ldaxr`/`stxr` lock losing its
   reservation to writes of neighbouring lock words is inferred (lock word
   0 at the start, no owner, thousands of failed try-locks), not observed
   directly; which writer does it is source reasoning (the test's task on
   CPU 0 calls `yield()` in a tight loop during the finish wait and takes
   `rq_lock[0]` each time). Not excluded: another hot word in those lines;
   a cluster-wide stall would also give equal durations on CPUs 4 and 5,
   and the six waits with under 10 failed try-locks fit neither a spinning
   CPU nor a held lock.

**Decision for step 3.** The dumps do not put the failure in the fix's
deferral path, and they do not show the 2 s case at all. **Hypothesis, not
shown by a dump:** the 2 s NOT-FINISHED of Entry 39 is the same starvation
at full length, with the READY workers not stolen because CPU 5 was inside
a tick-driven `schedule()` with `preempt_disabled` set. For it: Entry 39's
capture has three READY workers assigned to CPU 5 with `on_cpu` 0, neither
run nor stolen, which is the state of the dumps above. **Against it: every
measured wait, dumped or not, ended within 6.64 ms**, so something ends
these waits that the hypothesis would need to have been absent for 2 s,
and nothing here says what. The cause of #1262 remains not established.
Per step 5 the branch's kernel code is left as is; the findings are on
#1262 and the lock layout is filed as #1263.

### Part B rows: #1244, `fix/kexec-dtb-reserve` 661204e6

Change: the PMM reserves the DTB blob's own range (address and
`totalsize`, page-aligned) on every boot path; `pmm_init` ends with a
free-list walk that prints `PMM: DTB at ... is clear of every free block`
or a WARN; the arm64 header's `image_size` is `__kernel_end - _start`
(`.bss` and boot stack included). UEFI-direct does not read that field
(`SizeOfImage` and `_kernel_file_size` are unchanged). The header change
cannot keep the blob out of the heap, which starts at `__kernel_end`; the
reservation is the fix.

| Row | Files | Result |
|---|---|---|
| main + check only (665fb18e), Jetson kexec, 1 boot | `kexec-maincheck-665fb18e-boot01.txt` | `[WARN] PMM: 2 free block(s) overlap the DTB at 0x82c27000-0x82c65000 (250972 bytes; first block 0x82c30000 order 4; heap starts 0x82c30000)` |
| 661204e6 production, Jetson kexec, `boot_test` x10 | `kexec-prod-661204e6-boot01..10.txt` | 10 of 10 to the shell; `DTB at 0x82c40000-0x82c7e000 ... is clear of every free block` on all 10; 0 halts |
| 661204e6 production, Jetson UEFI-direct, 1 boot | `uefi-prod-661204e6-boot01.txt` | shell; `DTB at 0x2679df000-0x267ad3000 (997852 bytes) is clear of every free block` |
| 661204e6 suite, Jetson kexec, 10 boots | `kexec-suite-661204e6-boot01..10.txt` | **2 PMM halts** (boots 04, 07: #1207's signature, data abort on CPU 0 right after `test_checkerboard_fragmentation` passes, where `test_exhaustion_recovery` runs next; ESR 0x96000044, FAR 0x1000008094addf / 0x1000008094adcf, ELR 0x80024e88 / 0x80024eec, both in `pmm_alloc_pages` by `addr2line` on the image). 5 boots lost to `test_preempt_flag_follows_migrated_task` (#1238, which main has): 01 and 02 panic on CPU 5 (EC 0xe) right after the preceding test, capture cut; 06 and 09 panic in that test's tasks (`pdm_a` on CPU 2, `pdm_b` on CPU 3, EC 0xe), that CPU halts, the test is reported FAIL, the suite runs on and hangs in the coherency litmus at the halted CPU; 08 stalls silently at that test. 3 complete (03, 05, 10) with the same 14 failures, all on the #1109 list, which includes the PMM tests `test_coalesce_enables_larger_allocation` and `test_recursive_coalescing` (they fail the same way in Entry 39's suites without this change). The five new tests (`test_dtb_reserve_range_page_aligns`, `test_carve_dtb_straddling_heap_start`, `test_collect_reserves_includes_live_dtb`, `test_no_free_block_overlaps_live_dtb`, `test_free_overlap_count_sees_a_free_page`) pass in the 8 boots that reached them; boots 04 and 07 halted first. |
| 661204e6 production, Pi 5, 2 boots (`gauntlet-5-hw-pi5`) | `pi5-prod-661204e6-boot01,02.txt` | 2 of 2 to the shell; `DTB at 0x2efec000-0x2f000000 (79954 bytes) is clear of every free block (heap starts 0x2a6f000)`: on Pi 5 the firmware's blob is inside the heap as well |
| QEMU | `make test`, `make test PLATFORM=X86_64` at 661204e6 | pass; `check-braces` 0 violations |

**PMM halt count: 2 in 10 with the DTB reserved** (run 4b: 1 in 10
without). The overlap of #1244 is real and is fixed; it is not the cause of
#1207. Boots 01 and 02 of the suite row were captured with a pattern that
matched the words `KERNEL PANIC` and are cut inside the dump (file names
say so).

### Part C

Condition: no failure in at least 5x the iterations in which #1262 was
seen (1 in 600 on UEFI-direct, so 3000). Met: 3000 of 3000 at x200 on
UEFI-direct, the path and count the brief set (10 boots, then 5 more). The
kernel code is unchanged from 6a4cfa93, so this says the failure is rarer
than 1 in 600, not that it is gone; #1262 stays open and the PR says so.

**PR #1266**, `c2/switch-window-r2` ae1399ba against main, ready for
review. Closes #1234, #1238, #1242 (checked: `closingIssuesReferences` =
1234, 1238, 1242). It carries ADR 0008 as accepted, run 5's tables (Entries
8 and 34), Entry 39's rows, this entry's rows, the bench numbers with the
accepted cost, and the critic's verdict. Added on the user's instruction
during the session: `docs/perf/context-switch.md` (baseline Jetson kexec
88.0 ns and Pi 5 100.7 ns at dc51107c; the rule that every PR touching
`kernel/sched/`, the context switch or the task lifecycle adds a row with
`bench context` before and after on both boards, at least 5 raw runs per
side, files linked, the delta, who accepted it and why; this PR as the
first row, +9.3 ns / +10.6% Jetson and +8.3 ns / +8.3% Pi 5, accepted by
the user; the running total) and one line in `kernel/CLAUDE.md`.

**PR #1265**, `fix/kexec-dtb-reserve` 661204e6 against main, ready for
review. Closes #1244 (checked), references #1207 without closing it, and
states the halt count: 2 in 10. Comment on #1207 posted with the count.

Nothing merged.

### Critic

Fable 5.2, blind, this session's rows only, two rounds
(`critic-5-followup.md`, this commit). **Round 1: FAIL**, narrowly: every
count, address, banner and pass total matched, but the suite row
misdescribed boots 06 and 09, the example-dump sentence contradicted the
dump, and both inferences were rated "partly supported". The rows and the
section above are the corrected text. **Round 2: PASS**: every row matches
the captures and both inferences are "stated no more strongly than the
evidence allows"; its one remaining discrepancy (the artefact count, 43 not
38) is corrected above. Not checkable from the captures, per the critic:
the `addr2line` symbol, #1109 membership, the Entry 39 comparisons, that
main hangs at R3 wake iteration 1, and the QEMU row.

### Tracker

Opened: #1263 (P1: `rq_lock[]` and `steal_deque_lock[]` share one cache
line each), #1264 (P3: stub operator library in a clean worktree's test
kernel). Comments: #1262 (rate, dumps, classification, hypothesis and the
point against it), #1207 (halt count 2 in 10 with the DTB reserved, and an
untested reading of the signature: `free_counts[]` underflow in
`free_list_remove` when the buddy range is free but is not one block of
that order). Still open: #1262, #1207, #1260.

### Incidents

- Suite boots 01 and 02 truncated by this session's own capture pattern
  (above); pattern changed for boots 03 to 10.
- Two ESP staging attempts made right after a power cycle exited without
  output and wrote nothing (the second was the final restore); each rerun
  staged and verified (`deploy-uefi-1244-prod-661204e6.log.txt`,
  `jetson-post-state.txt`). Cause not captured.
- A first `sdwire_update` plan for pi-5-1 would have overwritten the
  original `kernel_2712.img`; it was not confirmed. The original was
  renamed aside first, then the test image copied in.
- `kernel-test` in a clean worktree embeds the stub operator library until
  a Jetson production build has run there (disclosure 1); filed as #1264.

### Lab end state

jetson-nano-2: released, powered on, SD Linux (BootCurrent 0001), no
BootNext, ESP `SLMOS.efi` 789cba62 (fresh read-only mount, `fsck.vfat -n`:
6 files, 2196/130892 clusters, as found), `/root/slmos.elf` 098f3163, 14
`*.elf*` files as found, this session's files deleted. pi-5-1: released,
powered on, original `kernel_2712.img` renamed back and booted (banner
b42b2c1d, `pi5-restore-check-b42b2c1d.txt`). No claims held.

Local only, not pushed: branches `diag/1244-check-on-main` (665fb18e),
`diag/1262-padded-locks` (741497c7), `s1262/diag` (= origin
`c2/switch-window-r2`), and worktrees under
`.claude/worktrees/Issues-1262-and-1244/.wt/`.

## Entry 42 (written 04:52 UTC, from date): lab session for #1263 and #1207, Hour 0

Orchestrator session with two builders (A: #1263, per-CPU locks share a
cache line; B: #1207, PMM halt), started 2026-11-15 04:45 UTC with the user
present. Nothing merges. Evidence goes on `gauntlet-5-hw-jetson` and
`gauntlet-5-hw-pi5` under `docs/archive/switch-gauntlet/run5-hw/1263-1207/`
(files `A-*` and `B-*`).

### State as found

| Item | Value |
|---|---|
| `main` | dc51107c |
| PR #1266 (`c2/switch-window-r2`, ae1399ba) | open, not merged |
| PR #1265 (`fix/kexec-dtb-reserve`, 661204e6) | open, not merged |
| Part A base | `c2/switch-window-r2` ae1399ba, because #1266 is not in main (R3 wake needs its fix); branch `fix/percpu-cacheline` |
| Part B base | 661204e6 = main + #1265; branch `fix/pmm-coalesce` |
| Pushed in Hour 0 | `diag/1262-padded-locks` 741497c7 and `diag/1244-check-on-main` 665fb18e (new branches on origin; the evidence for #1263 and #1244) |
| labctl | no claims; health: jetson-nano-2 and pi-5-1 online, power on, serial ok; `recovery_mode` binding of jetson-nano-2 released, relay open |
| jetson-nano-2 over SSH | SD Linux 5.15.148-tegra, BootCurrent 0001, `/root/slmos.elf` 098f3163, 14 `*.elf*` files (the end state of Entry 41); the ESP hash is read by builder A before its first change |

### Self-tests

- `send_later`: this session has no tool of that name. The two mechanisms
  that stand in for it were tested: a background shell task (25 s sleep;
  its completion notice arrived) and a one-shot scheduled prompt
  (`CronCreate`, set for 20:57 PST; result recorded in the next entry).
- One harmless call per tool family: labctl read tools (`list_claims`,
  `run_health_check`, `bindings_list`, `actuation_status`, `list_images`,
  the `lab://sbcs` resource), `git push` (the two branches above), `gh`
  (issue and PR reads), SSH to the Jetson (read only), `git worktree`
  add and remove, a write and delete in `/var/lib/labctl/images`, agent
  spawn and `SendMessage`.
- Harness limit found: this session's git commands must be single plain
  commands inside its own worktree, so every worktree of the session is
  under `.claude/worktrees/Lab-Session-1263/.wt/` (`a`, `b`, `a-evj`,
  `a-evp`, `b-evj`, `b-evp`, `log`).

### Arrangement

The orchestrator holds the labctl claims on both boards (agent name
`lab-1263-1207`, session hold, taken 04:49 UTC) and hands the lab to one
builder at a time by message; a builder ends its turn at the end of each
phase. Lab order: A1 (Jetson padded and unpadded rows), B1 (Jetson suite),
A2 (Pi 5 and regression rows), B2 (Pi 5 suite). One QEMU at a time on the
host through a shared `flock`. Builders do not write this log.

Addendum to Entry 42 (04:57 UTC, from date): the one-shot scheduled prompt
set for 20:57 PST arrived at 20:57:05 PST (04:57 UTC), while the session
was idle between turns. Both stand-ins for `send_later` work.

## Entry 43 (written 07:40 UTC, from date): builder B phase B0 (#1207, QEMU and code); builder A woken, lab handed to A

Source: builder B's B0 report and its one correction, as received. Branch
`fix/pmm-coalesce` f650b695 (on 661204e6; commits df481476 `PMM_CHECK`
option only, 21303db1 five new tests and two test hooks, e3c22600 allocator
and test fix with documents, f650b695 hooks built into every image).
Evidence: `gauntlet-5-hw-jetson` 932f4e4f, `run5-hw/1263-1207/`, 87 files
`B-*`, index `B-README.txt`. Not yet checked by a critic.

### Correction to the session brief

The two coalescing tests (`test_coalesce_enables_larger_allocation`,
`test_recursive_coalescing`) do not fail on every board. In Entry 39's
captures (`closeout/`, 6a4cfa93) they fail in 5 of 5 Jetson kexec suite
boots and pass in 5 of 5 Pi 5 boots and 5 of 5 Jetson UEFI-direct boots.

### QEMU baseline, 661204e6 unmodified

| Row | Files | Result |
|---|---|---|
| coalesce pair x50 | `B-qemu-base-661204e6-coalesce-x50.txt` | 50 of 50 and 50 of 50 pass |
| checkerboard + exhaustion x50 | `B-qemu-base-661204e6-checker-exhaust-x50.txt` | 50 of 50 and 50 of 50 pass |
| PMM group in suite order x20 | `B-qemu-base-661204e6-fullsuite-run01..20.txt` | PMM suite `Tests: 48 Passed: 45 Failed: 0 Ignored: 3` in 20 of 20. Deviation: the filter holds 16 names and the group has 48, so the whole suite ran 20 times. 4 of 20 runs failed outside the PMM: run14 `test_net_burst_8_sends_async` (#1137/#1163); run15 and run20 `test_litmus_patterns_pass` (#1230); run18 litmus, a Lua GC data abort and the 120 s timeout (new, filed as #1278, P2) |

Nothing in the PMM fails in QEMU on the base.

### The coalescing tests: a test defect (shown)

The tests count splits and merges of single pages, which holds only when
those pages come from one split block. A Jetson kexec boot has 104 to 132
free single pages on the order-0 list (`closeout/kexec-suite-6a4cfa93-boot01..05.txt`,
the `Order 0` line), Pi 5 has 1, QEMU 0, so on Jetson kexec the allocator
hands out lone pages first: no split (`test_pmm.c:264`), nothing to merge
(`:318`). Reproduced in QEMU by creating 8 lone free pages first, with the
same two messages at the same lines (`B-qemu-old-7b0af7d0-newtests.txt`).
Fix: both tests drain the order-0 list first.

### The halt: mechanism shown in QEMU, hardware cause not shown

Shown on the old allocator in QEMU:

1. `is_buddy_free` (`kernel/mm/pmm.c:282-300` at 661204e6) tests that every
   page of the buddy range is free; two smaller free blocks side by side
   pass. `buddy_free` (`:365`) then calls `free_list_remove` on a block that
   is not on that order's list, `free_counts[order]` is decremented for a
   block the list never held (`:221`), and a later `buddy_alloc`
   (`:314-332`) finds a count above 0 with an empty list and writes
   `block_state[]` at an index computed from -1.
   `B-qemu-old-7b0af7d0-halt-signature.txt`: Data Abort (WRITE), ESR
   0x96000044, FAR 0x1000004071633f, x0 0xffffffffffffffff, the signature
   of #1207. `B-qemu-old-7b0af7d0-pmmcheck-on.txt`: with `PMM_CHECK=ON` the
   same run panics at the merge (`buddy_free: buddy range is free but is
   not one listed block of this order (0x420d6000, order 1)`).
2. `pmm_free_pages` (`:1062`) checks only the first page for a double free.
3. `pmm_add_region` (`:695-718`): the order-0 step has no fit test, so a
   region whose end is not page-aligned lists the page holding the end.

**Not shown:** which of these, or something else, happens on jetson-nano-2.
Mechanism 1 needs a free buddy range that is not one block; by the
builder's code reading `pmm_init` cannot produce one when every reserve
boundary is page-aligned, and the firmware memreserve values on Jetson are
in no capture. The builder does not claim #1207 is root-caused on hardware.
The d512f40b run05 capture (`next` = 0x0007000000000000 in the order-12
list) is not explained: mechanism 1 can store a non-pointer `next`, but
that is not shown, and a write into free memory by something other than
the PMM is not excluded. The coalescing failures and the halt are two
defects. #1228: same signature, not shown to be the same defect.

### The change (e3c22600, f650b695)

The first page of a listed free block carries `BLOCK_FREE_HEAD|order` in
`block_state[]` and `is_buddy_free` compares that byte; `pmm_free_pages`
rejects a range holding a page that is not allocated; `pmm_add_region`
adds whole pages only; a count without a list panics by name; `PMM_CHECK=ON`
(default off) adds O(1) assertions, a `pmm_init` line and a free-list walk
after each suite. Option off (`B-text-pmmcheck-off-df481476-vs-661204e6.txt`):
production `.text` size equal on Jetson (2479384), Pi 5 (2237304) and QEMU
(2227160); 16 bytes differ per image, all `__LINE__` immediates in
`pmm.c.obj`; not byte-identical.

Test fails on old code: `B-qemu-old-7b0af7d0-newtests.txt`, 5 of 5 new
tests fail on the old allocator (21303db1's tree); `B-qemu-fix-f650b695-newtests-x50.txt`,
50 of 50 each on the fix.

### QEMU rows at the fix

| Row | Files | Result |
|---|---|---|
| coalesce pair x50 | `B-qemu-fix-f650b695-coalesce-x50.txt` | 50 of 50, 50 of 50 |
| checkerboard + exhaustion x50 | `B-qemu-fix-f650b695-checker-exhaust-x50.txt` | 50 of 50, 50 of 50 |
| full suite x20, e3c22600 | `B-qemu-fix-e3c22600-fullsuite-run01..20.txt` | PMM suite 53/50/0/3 in 20 of 20, 0 faults; **8 of 20** runs fail on `test_litmus_patterns_pass` only (runs 01, 02, 03, 04, 06, 07, 08, 19; the builder's corrected count) |
| full suite x13, f650b695 | `B-qemu-fix-f650b695-fullsuite-run1..3.txt`, `-fullsuite-b-run01..10.txt` | PMM suite clean in 13 of 13; 2 of 13 fail on litmus only |
| `make test PMM_CHECK=ON` | `B-qemu-fix-f650b695-pmmcheck-on-fullsuite.txt` | pass; 107 walks ok |
| `make test PLATFORM=X86_64` | `B-qemu-fix-f650b695-x86_64-fullsuite.txt` | pass |
| `make check-braces` | `B-qemu-fix-f650b695-check-braces.txt` | 0 violations |

The PMM loops are at 0 failures; the full-suite rows are not, because of
the litmus test (#1230): 8 of 20 at e3c22600 against 3 of 20 at the base.
The builder's reading (inference): host load from its own builds. The
orchestrator asked for an interleaved base/fix A/B at equal load.

### Incidents

- Builder B's 21303db1 and e3c22600 did not compile as production kernels
  (test hooks behind `ENABLE_BOOT_TESTS`); found by its own Jetson
  production build and `check-braces`, fixed in f650b695.
- **Builder A lost about 1 h 40 min.** It ended its turn at 05:48 UTC to
  wait for background image builds; they finished at 05:55 UTC
  (`images.txt` in its scratch directory) and nothing resumed it. The
  orchestrator noticed at 07:33 UTC when B's report arrived. The Jetson sat
  unused for that time. Both builders now wait for their own background
  jobs in the foreground in bounded steps.

### Decisions

- 07:35 UTC: lab handed to builder A for A1 (Jetson R3 wake and
  `bench context`, padded against unpadded). A's code is at c0612fa7.
- B1, after A1: 2 kexec suite boots of the old allocator with
  `PMM_CHECK=ON` (`slmos-1207-chk-oldalloc-df481476.elf`), then the
  required row (`slmos-1207-suite-f650b695.elf`, 10 boots), then report.
- Builder B meanwhile: `/ci`, the litmus A/B, and where the Jetson reserve
  ranges can be read.
- #1207 stays P1 and open on the hardware evidence so far.

## Entry 44 (written 09:55 UTC, from date): builder A phases A0 and A1 (#1263, Jetson rows); builder B interim; lab to builder B

Sources: builder A's combined A0 and A1 report and builder B's interim
report, as received; not yet checked by a critic. Part A: branch
`fix/percpu-cacheline` c0612fa7 (one commit on ae1399ba); unpadded side
ae1399ba. Evidence `gauntlet-5-hw-jetson` ecd529e0, `run5-hw/1263-1207/`,
files `A-*`.

### Part A: the change

- `kernel/include/cacheline.h` holds the one `CACHE_LINE_SIZE` (64) and
  `CACHE_LINE_SHIFT`; `cache.h`, `spinlock.h` and `gpu/cache.c` take it
  from there (a second definition in `gpu/cache.c` is removed).
- `SPINLOCK_ARRAY` elements are `spinlock_padded_t` (64 bytes, aligned);
  `SPINLOCK_AT(name, i)` gives the lock. Only `rq_lock[]` and
  `steal_deque_lock[]` use the macro; there is no similar macro.
- `resched_trampoline` (`vectors.S`) indexes `preempt_disabled[]`,
  `orig_elr[]` and `orig_spsr[]` with `lsl #CACHE_LINE_SHIFT`.
- The ten `sched_diag_*[]` arrays are one struct per CPU; the sixteen
  `SWO_DIAG` arrays are `swo_pc[cpu]`. Placement only.
- Test `kernel/tests/test_percpu_layout.c` (8 tests) and static asserts;
  `docs/scheduler.md` section "Per-CPU data layout", `kernel/CLAUDE.md`,
  `docs/smp.md`.

QEMU at the head: `make test` (`A-qemu-arm-make-test-c0612fa7.txt`) and
`make test PLATFORM=X86_64` (`A-qemu-x86-make-test-c0612fa7.txt`) pass;
`AI_SCHED=ON` and `SECONDARY_PREEMPT=ON WORK_STEALING=ON SWO_DIAG=ON`
passed before the last four edits (`A-qemu-arm-ai-sched-make-test-precommit.txt`,
`A-qemu-arm-ws-swodiag-make-test-precommit.txt`; rerun requested);
`check-braces` 0 violations (`A-check-braces-c0612fa7.txt`). Not covered
by QEMU: the trampoline's indexing (`SECONDARY_PREEMPT` is ignored on
QEMU); checked by disassembly and by every padded hardware boot.

Layout (`A-layout-check.txt`, `A-images.txt`): in the padded Jetson and
Pi 5 images every padded array is 512 bytes (8 x 64) at a 64-byte-aligned
address; in the unpadded images `rq_lock` and `steal_deque_lock` are 32
bytes each. Section deltas padded minus unpadded: Jetson production
`.text` +1696, `.rodata` +512; Pi 5 production `.text` +1536, `.rodata`
+512; `.data` and `.bss` totals equal. Not yet itemised: the `.rodata`
+512 and why `.bss` is unchanged (requested).

Line size: `A-jetson-coherency-regs-c0612fa7.txt`, all 6 Jetson CPUs
(MIDR 0x410fd421) read CTR_EL0 0x9444c004 and CCSIDR reports 64-byte lines
at L1D, L2 and L3. Pi 5 is captured in A2 (an earlier capture,
`gauntlet/coherency-2:docs/archive/coherency-gauntlet/c2-hw/pi5-regs.txt`,
shows the same).

### Part A: audit of per-CPU arrays (short form; full table with sizes, writers and readers in `docs/scheduler.md` on the branch)

Padded: `rq_lock[]` `sched.c:100`; `steal_deque_lock[]` `sched.c:134`;
`preempt_disabled[]` `sched.c:93` (owner writes, idle CPUs poll every
victim's flag in `sched_try_steal`; asm `vectors.S:602,627`);
`sched_diag[]` `sched.c:425` (owner writes on every tick; `steal_push_full`
written by the CPU adding the task); `switch_in_waiting[]` `sched.c:603`;
`current_task[]` `task.c:45`; `coop_last_tick_cntpct[]` `sched.c:2235`;
`reschedule_pending_wb` / `orig_elr_wb` / `orig_spsr_wb` `preempt.c:58-60`
(asm `vectors.S:589-596`); `cpu_steal_deques[]` `sched.c:127` (392 to 448
bytes); `pmu_cache_pressure_q16_per_cpu[]`, `pmu_cp_state[]`
`pmu.c:333,341`; `swo_rq_owner[]` `sched.c:160` and `swo_pc[]`
`sched.c:608` (`SWO_DIAG`); `slot_trace_idx[]`, `slot_trace_rq_holder[]`,
`slot_trace_pd_ticks[]` `slot_trace.c:38,41,257` (`SLOT_TRACE`);
`smp_resched_ipi_count[]` `platform_x86.c:46`.

Already whole lines, now under the layout test: `sched.cpu_fallback[]`
`sched.c:234`; `steal_trace[]`, `steal_trace_dq[]`, `steal_pdwin[]`
`sched.c:140,141,149`; `tracer_percpu`.

Not padded: `cpu_data[]`, `cpu_logical_map[]`, `cpu_current_el_fallback[]`
`smp.c:24,27,28` (boot only; the asm table stays packed); `cpu_boot_flag[]`
`smp.c:37` (boot and resurrect); `cpu_stacks`; `cpu_supervisor.c:88-91`
(single writer); `pmu_ready_per_cpu[]` `pmu.c:72` (once per boot);
`gic.c:93,808,815` and `fault_dump.c:27-29` (not on the paths);
`smp_done_wb[]` `shell_sys.c:1775` (bench harness); `slot_trace_ring[][]`;
`switch_check.c` `sc_*[]`.

Found, left unchanged: `sched.context_switches`, `sched.timer_ticks` and
`timer_handler_count` are single global counters that every CPU
increments; true sharing, not placement. An issue is being filed.

### Part A rows on jetson-nano-2

R3 wake (`test_wake_during_switch_out`) x1000, `SWO_DIAG=ON`, order U, P,
U, P, U, P on each path, one boot per `boot_test` call. Counts in
`A-r3w-summary.txt` by `A-r3sum.sh.txt`, with Entry 41's definitions (the
script reproduces Entry 41's 4 / 10 / 811552 and 30 / 49 / 827300 from its
files). All 12 boots: 1000 of 1000, no NOT-FINISHED.

| Path | Side | Files | Slow finishes (over 10 ms) | Iterations with a prelude over 1 ms | Slowest iteration (us) |
|---|---|---|---|---|---|
| kexec | unpadded ae1399ba | `A-kexec-r3w-U-ae1399ba-x1000-boot01..03.txt` | 11, 9, 4 | 20, 12, 8 | 832730, 819249, 833892 |
| kexec | padded c0612fa7 | `A-kexec-r3w-P-c0612fa7-x1000-boot01..03.txt` | 0, 0, 0 | 0, 0, 0 | 315261, 315248, 315244 |
| UEFI-direct | unpadded ae1399ba | `A-uefi-r3w-U-ae1399ba-x1000-boot01..03.txt` | 32, 34, 19 | 37, 60, 37 | 832885, 833243, 827987 |
| UEFI-direct | padded c0612fa7 | `A-uefi-r3w-P-c0612fa7-x1000-boot01..03.txt` | 0, 0, 0 | 0, 0, 0 | 315415, 315259, 319164 |

Unpadded: 109 slow finishes in 6000 iterations (24 kexec, 85 UEFI-direct).
Padded: 0 in 6000. The padded side differs in every padded array of the
audit, not only the two lock arrays, so the rows do not say which array's
padding removes the stalls.

`bench context`, Jetson kexec production, 3 boots per side alternating,
2 runs per boot, 99 round trips per run (`A-bench-context-jetson-base-U{1,2,3}-runs1-2.txt`,
`A-bench-context-jetson-head-P{1,2,3}-runs1-2.txt`, summary
`A-bench-context-summary-jetson.txt`): unpadded run means 97, 96, 98, 99,
100, 91 (mean 96.83 ns, sd 3.19); padded 102, 96, 100, 96, 101, 96 (mean
98.50 ns, sd 2.81). Delta +1.67 ns (+1.7%), ranges overlap; no difference
is shown at this sample size. The ledger row waits for the Pi 5 half.

### Part A incidents and disclosures

- The as-found ESP image was not copied before the first staging. The
  restore used a host copy from the previous session whose sha256 equals
  the as-found hash in `A-jetson-pre-state.txt`.
- UEFI-direct boots used a new `uefistage` mode of the trigger loop
  (`A-trigger.sh.txt`): stage, verify from a fresh read-only mount, arm
  BootNext and reboot in one Linux boot.
- `serial_send` results were saved from the builder's own transcript by
  `A-savesend.py.txt`.
- No boot failed and no capture was cut. 24 boot files, `save.sh` OK on all.
- Board as left (`A-jetson-post-state-A1.txt`, 09:47:47Z): SD Linux,
  BootCurrent 0001, no BootNext, ESP `SLMOS.efi` 789cba62 (fresh read-only
  mount, `fsck.vfat -n` 6 files, 2196/130892 clusters), `/root/slmos.elf`
  098f3163, 14 `*.elf*` files, the builder's files removed.

### Part B interim (no lab)

`fix/pmm-coalesce` is now f55c5293 (f650b695 + 679a09c7, `PMM_CHECK`
prints the reserve list at `pmm_init`, + one document line). New pushed
branch `diag/1207-check-oldalloc` 17daedda (old allocator with the full
check report; not for merge). Evidence `gauntlet-5-hw-jetson` c3935e18.

- `/ci` at f55c5293: every step passes (`B-ci-f55c5293-step01..11-*.txt`,
  `B-ci.out.txt`).
- Litmus A/B at equal load, 10 full-suite runs each, interleaved
  (`B-qemu-ab-base-661204e6-run01..10.txt`, `B-qemu-ab-fix-f55c5293-run01..10.txt`):
  base 10 of 10 pass; fix 8 of 10 (run05 `test_litmus_patterns_pass`,
  #1230; run08 `test_runner_parks_without_a_lock`, #1184). PMM suite clean
  in all 10. No evidence that the PMM change moves the #1230 rate; too few
  runs to exclude a small change.
- The x50 loops, the five new tests x50, `PMM_CHECK=ON` and x86-64 rerun
  at f55c5293, all pass (`B-qemu-fix-f55c5293-*.txt`).
- No existing capture holds the Jetson firmware memreserve values; the
  `PMM_CHECK` build prints them on every boot.

### Decisions

- 09:50 UTC: lab to builder B for B1: 2 kexec suite boots of
  `slmos-1207-chk-oldalloc-17daedda.elf`, then 10 of
  `slmos-1207-suite-f55c5293.elf`.
- Builder A meanwhile, host only: `/ci`, the two option rows at the
  committed head, the section itemisation, the counter issue, Pi 5 images.

## Entry 45 (written 11:40 UTC, from date): builder B phase B1 (#1207, Jetson kexec suite): the halt is not fixed; builder A interim; lab to builder A

Sources: builder B's B1 report and builder A's interim report, as
received; not yet checked by a critic.

### Part B, jetson-nano-2, kexec suite (`gauntlet-5-hw-jetson` d239340e, files `B-kexec-*`, `B-trigger-kexec-*`, `B-jetson-*`)

One boot per `boot_test` call, pattern `Task 'main' \(id=\d+\) exiting`
only, host-side trigger; all 12 files pass the both-ends check.

**Row 1, old allocator with `PMM_CHECK=ON`** (17daedda, ELF 2fae8367),
`B-kexec-chk-oldalloc-17daedda-boot01,02.txt`: both boots ran the whole
suite (`[FAIL] 14 total failure(s)`, Entry 41's 14), no halt, no assertion,
107 free-list walks ok in each. `[pmm-check] reserves: 189 in all (0
firmware /memreserve/ ...), 0 not page-aligned` (boot02: 190) and
`[pmm-check] init: 533 free blocks, 1803154 free pages (1803154 by page
state), 0 unmerged buddy pair(s), 0 region(s) not page-aligned` (boot02:
610 blocks, 1803081 pages, 0, 0). So `pmm_init` leaves no split free range
and no shared page on this board: mechanisms 1 and 3 of Entry 43 have no
source at boot here. Both coalescing tests fail in both boots (old tests).

**Row 2, the required row, fix head f55c5293, `PMM_CHECK` off** (ELF
6fcf403f), `B-kexec-suite-f55c5293-boot01..10.txt`:

| Outcome | Boots | Count |
|---|---|---|
| **PMM halt** in `test_exhaustion_recovery`, right after `[PASS] test_checkerboard_fragmentation` | 01, 05 | **2** |
| Complete, `[FAIL] 12 total failure(s)` | 03, 06, 09, 10 | 4 |
| Lost after the PMM suite at `test_preempt_flag_follows_migrated_task` (the #1238 family; this base lacks #1266) | 02, 04, 07, 08 | 4 |

**2 PMM halts in 10 at the fix head, the same count as before the change
(2 in 10 at 661204e6, Entry 41). #1207 is not fixed.** The halts are now
the named panic: boot01 `PMM: order 7 free list is empty but its count is
29 (free-list corruption, #1207)`, boot05 `PMM: order 5 free list is empty
but its count is 48`. Fact from the same files: the boot-time lists held
30 order-7 blocks (boot01) and 50 order-5 blocks (boot05), so the list
ended after one pop, or at most two, with 29 and 48 blocks still counted: a
block at or next to the head had a NULL `next`. Inference, not shown:
something other than the free-list code cleared the first word of a listed
free block. `PMM_CHECK` was off in these boots, so there is no dump of the
block. 0 in 2 with the option on against 2 in 10 with it off says nothing
about the option's effect.

The two coalescing tests pass in 10 of 10 boots at the fix head; the five
new tests pass in the 8 boots that got through the PMM suite. The 12
failures of the complete boots are Entry 41's 14 minus the two coalescing
tests (compared against `1262-1244/kexec-suite-661204e6-boot03.txt`).

Board as left (`B-jetson-post-state-b1.txt`): SD Linux, BootCurrent 0001,
no BootNext, `/root/slmos.elf` 098f3163, 14 `*.elf*` files as found, ESP
not touched.

### Decisions (orchestrator)

- The Part B PR will not close #1207 or #1228. It carries the
  coalescing-test fix, three allocator defects shown in QEMU with failing
  then passing tests, the named panic and `PMM_CHECK`, and states that
  e3c22600's commit message overstates (the merge defect reproduces the
  signature in QEMU; it is not what halts the board). Pushed history is
  not rewritten because the evidence banners name those commits.
- Further diagnosis, built by builder B while builder A holds the lab: the
  option-off panic dumps the bad block and its neighbours at no layout
  cost, and a light free-list walk (no extra `.bss`) runs at the end of
  `pmm_init`, before the first suite and after each suite, so a cleared
  link is bracketed and its content captured. Hypothesis to test, not
  shown: a writer outside the PMM (for example a device left doing DMA by
  Linux across kexec). A second Jetson row with that image follows A2.

### Part A interim (host only, `gauntlet-5-hw-jetson` 99b4be10)

- `/ci` at c0612fa7: steps 1 to 11 pass, plus an extra `make test
  SECONDARY_PREEMPT=ON WORK_STEALING=ON SWO_DIAG=ON` (`A-ci-c0612fa7-step01..12-*.txt`,
  `A-ci.out.txt`); banners read c0612fa7 without `-dirty`. This replaces
  the two "precommit" rows of Entry 44.
- Section deltas itemised (`A-layout-note.txt`, `A-secdiff-jet-prod.txt`,
  `A-rodiff-jet-prod.txt`): `.bss` symbols grow by 5816 bytes and padding
  between symbols shrinks by 5816; `.text` +1696 (the new test +1408,
  `schedule()` +148, `scheduler_tick` +32, `resched_trampoline` +16);
  `.rodata` +512, of which strings net +365 (including -660 from shorter
  build-directory paths) and 147 bytes of string-section padding are not
  itemised further.
- Issue #1279 filed (P3, Backlog): global scheduler counters written by
  every CPU on every switch and tick.
- Correction to Entry 44: A1 produced 18 boot captures (12 R3 wake and 6
  bench boots), not 24; all 18 saved with verdict OK.

11:36 UTC: lab to builder A for A2 (Pi 5 `bench context`, then the
regression rows on both boards).

## Entry 46 (written 14:20 UTC, from date): builder A phase A2 (#1263: Pi 5 bench, regression rows at the head); builder B diagnostic build; lab to builder B

Sources: builder A's final A2 report and builder B's second interim
report, as received; not yet checked by a critic. Part A: branch
`fix/percpu-cacheline` d70f93f0 = c0612fa7 (the change) + the ledger row
(document only); every image and capture carries banner c0612fa7. Evidence
`gauntlet-5-hw-jetson` 8e81fee7 and `gauntlet-5-hw-pi5` ad1a2cb1,
`run5-hw/1263-1207/`, files `A-*`.

### Part A: `bench context` on Pi 5

Production, order U1, P1, U2, P2, U3, P3, one SD round trip per swap, 2
runs per boot, 99 round trips per run (`A-bench-context-pi5-base-U{1,2,3}-runs1-2.txt`,
`A-bench-context-pi5-head-P{1,2,3}-runs1-2.txt`, summary
`A-bench-context-summary-pi5.txt`): unpadded run means 108, 107, 108, 106,
110, 107 (107.67 ns, sd 1.37); padded 108, 107, 107, 106, 108, 108 (107.33
ns, sd 0.82). Delta -0.33 ns (-0.3%), ranges overlap.

Line size on Pi 5 (`A-pi5-coherency-regs-c0612fa7.txt`): 4 of 4 CPUs
(MIDR 0x414fd0b1) read CTR_EL0 0x9444c004, 64-byte lines at L1D, L2, L3.

### Part A: regression rows at c0612fa7

| Row | Files | Result |
|---|---|---|
| Jetson kexec production, `boot_test` x10 (3+3+3+1 per call) | `A-kexec-prod-c0612fa7-boot01..10.txt` | 10 of 10 |
| Jetson UEFI-direct production, x10 (1+3+3+3) | `A-uefi-prod-c0612fa7-boot01..10.txt` | 10 of 10, `Boot source: UEFI` in all |
| Pi 5 production, x10 | `A-pi5-prod-c0612fa7-boot01..10.txt` | 10 of 10 |
| `--assert-shares` x20, Jetson kexec | `A-assert-shares-kexec-c0612fa7-x20.txt` | 20 PASS, 0 FAIL (the command of Entry 39's `assert-shares-kexec-6a4cfa93-x20.txt`) |
| Suite, Jetson kexec, 3 boots | `A-kexec-suite-c0612fa7-boot01..03.txt`, `A-kexec-suite-classify.txt` | 3 complete, 14 failures each, all on the known list, 0 halts |
| Suite, Jetson UEFI-direct, 3 boots | `A-uefi-suite-c0612fa7-boot01..03.txt`, `A-uefi-suite-classify.txt` | 3 complete; 21, 21, 22 failures; all known **except one in boot 03** (below); 0 halts |
| Suite, Pi 5, 3 boots | `A-pi5-suite-c0612fa7-boot01..03.txt`, `A-pi5-suite-classify.txt` | 3 complete, 18 failures each, all known, 0 halts |
| `/ci` | `A-ci-c0612fa7-step01..12-*.txt` | every step passes (Entry 45) |

**One failure outside the known list, 1 of 9 suite boots:**
`test_preempt_flag_follows_migrated_task` in `A-uefi-suite-c0612fa7-boot03.txt`:
`pdm: migrations=0 moved=0 runs=100/100 bad=0 (first on cpu 0)`, then
`FAILED at .../test_scheduler.c:2369 - pdm_moved > 0`. Neither task changed
CPU in the test's window, so its coverage condition failed; the flag check
found no error (`bad=0`); no halt. The other 8 boots pass it. Entry 39 has
it passing in 15 of 15 suite boots at 6a4cfa93, so it is not a known
flake. Cause not established. The builder's inference, not checked: the
test migrates only when the task's assigned CPU is below 4, so a task
stolen to CPU 4 or 5 before its first run is never migrated, and the
padded kernel changes steal timing. Decision: the deciding experiment
(this test x200 on UEFI-direct, unpadded against padded, 3 boots per side)
runs in a short slot after builder B's phase.

The regression rows were run at the padded head only.

### Part A: ledger row (`docs/perf/context-switch.md`, d70f93f0)

"#1263 (awaits acceptance) | One cache line per per-CPU lock and scheduler
word (placement only) | Jetson 96.83 to 98.50 ns, +1.67 ns / +1.7% (6 runs
per side, sd 3.19 / 2.81, ranges overlap) | Pi 5 107.67 to 107.33 ns,
-0.33 ns / -0.3% (6 runs per side, sd 1.37 / 0.82, ranges overlap) | Awaits
acceptance by the user. No difference is shown on either board at this
sample size." Running total as the previous total plus this row: Jetson
88.0 to 99.0 ns (+12.5%), Pi 5 100.7 to 108.7 ns (+7.9%).

### Part A: incidents and board state

- pi-5-1 prints no firmware line on this UART, so the Pi 5 start check
  accepts the kernel banner block at the top of the capture.
- The Pi 5 restore took two `sdwire_update` calls (delete the test image,
  then rename the original back). Card as found and as left:
  `A-pi5-card-state.txt`.
- The ESP was backed up before the first staging (`A-jetson-pre-state-A2.txt`).
- Lab time 11:36 to about 14:08 UTC (the boards were free from 13:30).
  No boot failed, no capture cut.
- jetson-nano-2 as left (`A-jetson-post-state-A2.txt`): SD Linux, no
  BootNext, ESP 789cba62 (fresh read-only mount, 6 files, 2196/130892
  clusters), `/root/slmos.elf` 098f3163, 14 `*.elf*` files. pi-5-1:
  original `kernel_2712.img` back, restore-check boot
  `A-pi5-restore-check-boot01.txt` shows b42b2c1d. The builder's three
  images are deleted from `/var/lib/labctl/images`.

### Part B: diagnostic build (647c2c33, `gauntlet-5-hw-jetson` 206464d5)

`fix/pmm-coalesce` 647c2c33 = f55c5293 + a free-list verifier in every
build with no per-page table: `free_list_pop` reports a head whose `next`
is NULL while more blocks are counted, before the header is cleared; the
count-without-a-list panic prints every order's count, walked length and
head; `pmm_verify_free_lists()` runs at the end of `pmm_init`, before the
first suite, after every suite, and once a second from CPU 0's tick in
test kernels (try-lock, prints once per finding, keeps running). A finding
prints the time, place, last suite boundary, order, block address, page
states, the block's first 64 bytes and the 64 bytes before it.

- Shown in QEMU with an uncommitted demo patch (`B-qemu-verify-demo-647c2c33.txt`,
  `B-demo-patch.py.txt`): finding from the tick at 2420 ms, from
  `free_list_pop` at 3961 ms, then the named panic. The committed
  self-test prints one finding labelled `verifier self-test` in every
  suite log.
- Layout (`B-sizes-verifier-647c2c33-vs-f55c5293.txt`): `.data` 148480 and
  `.bss` 37512944 equal f55c5293's Jetson suite image and `__kernel_end`
  is the same (0x82d80000), so the heap does not move; `.text` +4352,
  `.rodata` +5120.
- `/ci` at 647c2c33 passes every step (`B-ci-647c2c33-step01..11-*.txt`);
  the x50 loops, the new tests x50, `PMM_CHECK=ON`, x86-64 and 5 full
  suites pass (`B-qemu-fix-647c2c33-*.txt`).
- Candidate writers, code reading only, nothing tested
  (`B-candidates-writers.txt`): (1) the xHCI controller, which
  `slmos-kexec` keeps running across kexec
  (`scripts/jetson-kexec-slmos.sh:1038,1091,1151-1178`) and which SLM-OS
  first halts in `xhci_init` (`main.c:874`) after `pmm_init`
  (`main.c:418`); its Linux-side DCBAA, contexts, event ring and transfer
  buffers are not reserved; (2) the GPU and its firmware, also kept
  running, with buffers outside a channel VM not verified as reserved;
  (3) a stale pointer in SLM-OS code, not surveyed. 189 against 190
  reserves: the channel-VM reservation coalesces pages into runs and the
  run count follows Linux's page placement
  (`ga10b_handoff_reserve.c:808-837`).

### Decisions

- 13:32 UTC: lab to builder B for the last planned lab phase: 10 kexec
  suite boots of `slmos-1207-suite-647c2c33.elf`, then the Pi 5 suite, 3
  boots.
- The blind critic for Part A starts now on the pushed rows; the deciding
  experiment for the suite failure is added to its material when it has
  run.

## Entry 47 (written 20:58 UTC, from date): critic round 1 on Part A (FAIL), the builder's responses, and a 6.5-hour stop on a usage limit

### Corrections to earlier entries

- Entry 46's heading time was a guess. It was written at about 13:40 UTC
  (the hand-over to builder B in it, 13:32 UTC, and the check at 13:48 UTC
  are from `date`). Builder A's "lab time 11:36 to about 14:08 UTC" in the
  same entry is the builder's estimate and is also wrong: its A2 lab work
  ended by 13:30 UTC.
- Entry 44, "every padded array is 512 bytes": 512 bytes each except
  `cpu_steal_deques` (3584) and `swo_pc` (1024).

### Critic A, round 1: FAIL (`critic-A.md`, `stage1-derivation.md`; copied to the log branch at the end of the session)

Fable 5.2, blind: derived the rows from the captures and the code before
reading the claims (log Entries 44 to 46, the draft PR body, the draft
#1262 comment). Every table number reproduces (all 36 R3 cells, 1000 of
1000 in 12 boots, the alternation, all 24 bench run means, CTR_EL0 on 6 + 4
CPUs, production 10 of 10 three times, `--assert-shares` 20, suite totals,
`/ci`, section deltas). All 64 `A-*boot*.txt` bodies are byte-identical to
their labctl output files and the 42 output directories hold no unsaved
run. No code defect: every `SPINLOCK_ARRAY` element is an aligned 64-byte
line (nm on five padded ELFs), the four sites in `resched_trampoline` are
the only assembly indexers and are consistent, no stale extern, sizeof or
pointer-arithmetic site, no lock-order or semantic change.

Findings, in the critic's order:

1. HIGH. The new suite failure is understated. The archive holds 822 runs
   of `test_preempt_flag_follows_migrated_task` on unpadded kernels with
   the #1234 fix (c0ea0714, 60c548f3, 6a4cfa93); minimum `moved` 5, never
   0. Against that: 1 of 6 padded Jetson suite boots (1 of 3 UEFI-direct),
   not "1 of 9".
2. MEDIUM. A stale number: "no measured wait above 6.64 ms" is Entry 41's
   maximum; this session measured 6701 us
   (`A-uefi-r3w-U-ae1399ba-x1000-boot01.txt:3349`). This session: 1.03 to
   6.70 ms; a prelude report in 1.3% (kexec) and 4.5% (UEFI-direct) of
   iterations; slow finishes in 0.8% and 2.8%.
3. MEDIUM. Undisclosed confound in the unpadded R3 baseline: in the
   `SWO_DIAG` test kernel `swo_rq_owner[]` (added by ae1399ba, written at
   every rq-lock take and release) shares the 64-byte line with
   `rq_lock[]`; in unpadded production `rq_lock` has the line to itself.
   Entry 41's 1441f690 rows (no owner array) show slow finishes too, so the
   instrumentation does not create the effect, but no R3 row exists
   without `SWO_DIAG`.
4. MEDIUM. Evidence about #1262 left out of the draft comment: 14 of 279
   lock waits with under 100 failed try-locks (8 under 10), 1.06 to 4.71
   ms; one wait that began with lock word 1; 4 of 109 slow finishes with no
   prelude report, one of them CPUs 4 and 5 in `task_exit` for 4.2 ms with
   every rq lock word 0.
5. LOW-MEDIUM. The column "iterations with a prelude over 1 ms" counts
   report lines (a prelude or any rq-lock wait over 1 ms); strict counts
   18, 12, 7 / 37, 59, 36 (35 by the report's own field, the builder's
   count).
6. LOW-MEDIUM. Scope: supported for jetson-nano-2, the `SWO_DIAG` test
   kernel, the wake phase; not excluded: which array, other layout shifts,
   production kernel, migrate phase, nano-1.
7. LOW-MEDIUM. Audit gaps: `sched.isolated_cores` and `sched.initialized`
   share a line with the global counters; standalone locks `task_lock`
   (`task.c:31`), `sleep_queue_lock` (`task_sleep.c:42`), `blkdev_lock`
   next to `timer_handler_count` (`timer.c:176`).
8. LOW. Regression-test gaps (`slot_trace_*` not in the layout table; the
   checker test did not exercise the checker).
9. LOW. Wording (Pi 5 has 14 blob names where #1109 says 13; the 512-byte
   sentence).
10. LOW. Bench: conclusion and arithmetic right; add that 6 runs come from
    3 boots and that a cost up to about 5% on Jetson is not excluded.

### Builder A's responses (branch `fix/percpu-cacheline` d09b063b; kernel code unchanged from c0612fa7; nothing posted except a comment on #1279)

Commits on ae1399ba: c0612fa7 (the change), d70f93f0 (ledger), 5c4bcdda
(test change, below), 7881056d (layout-test gaps), d09b063b (documents and
one comment in `spinlock.h`). Evidence `gauntlet-5-hw-jetson` 3e935cbe.
Each finding is answered in the documents and drafts (this session's
figures with file names, `A-r3w-detail.txt`; the confound; the added #1262
evidence; the relabelled column with both strict counts; scope words; the
audit table "found and not changed (#1279)"; the layout test; the suite
names compared with the close-out suites by script,
`A-suite-failnames-vs-closeout.txt`; bench caveats).

**The suite failure, what is known so far.** The builder's first reading
(`cur < 4`) is withdrawn. From the source: `pdm_moved` counts a worker
waking on a CPU other than the one it last ran on; the wake path takes the
CPU from the policy's `assign_cpu` (a shared round-robin cursor when CPUs
are idle) and from stealing, and the test's migration of a BLOCKED task
only rewrites `assigned_cpu`, which the wake path does not use. So the
coverage condition rests on incidental jitter. Existing captures: unpadded
6a4cfa93 suites show `moved` 5 to 7 within 21 wake-ups in 15 of 15 boots;
padded Pi 5 suites show `moved` 2, 4, 2 with `runs=100/100` in 3 of 3
boots (they pass); padded Jetson 5 of 6 like the unpadded, 1 with 0.
Disassembly of the unpadded and padded Pi 5 production images
(`A-fndiff-pi5-prod.txt`): `find_target_cpu`, `heuristic_assign_cpu`,
`scheduler_add_task`, `task_wake_sleepers`, the sleep path, `timer_handler`
and the queue helpers are instruction-identical apart from addresses;
`schedule` (with the inlined steal), `scheduler_tick`, `sched_migrate_task`,
`idle_task_func`, `task_current` differ in register allocation, so
equivalence there rests on the source diff (accessor rewrites). No logic
change found; timing is the explanation by elimination, not shown.

Prepared for a lab slot (A2b), not yet run: diag branches
`diag/1263-pdm-U` 6439476f (ae1399ba + a test-only print of steal
successes and each worker's wake CPU) and `diag/1263-pdm-P` 6247f73a
(c0612fa7 + the same 19 lines), x200 images for Pi 5 and Jetson; and the
test change 5c4bcdda: `pdm_a` sets its own affinity to the next of CPUs 1,
2, 3 before each sleep, `pdm_b` stays unpinned, the BLOCKED-task migration
loop is removed, checks unchanged. QEMU at 5c4bcdda: `make test` and x86-64
pass; the test x200 with `WORK_STEALING=ON` 200 of 200, `moved` 6 to 8
(builder's scratch logs, to be pushed with the A2b evidence).

### Stop on a usage limit

Builder B stopped at 14:22:42 UTC and builder A at 14:30:09 UTC with "hit
your Fable limit"; the orchestrator was stopped as well and resumed at
20:55 UTC on the user's "Try again". State found at 20:55 UTC: claims on
both boards still held by this session; pi-5-1 online; jetson-nano-2 power
on, serial ok, no ping (probably still in SLM-OS after builder B's fifth
diagnostic boot); B's trigger log shows 5 triggers and a sixth loop that
ended without a trigger; no trigger process, no QEMU. Builder A's `/ci` at
d09b063b ran to step 12 by 14:40 UTC in its scratch directory (not yet
read). 6.5 hours lost. The 20-minute check-in was cancelled (its prompts
had queued up during the stop). Builder B was resumed at 20:57 UTC to
finish boots 06 to 10 and the Pi 5 suite.

## Entry 48 (written 22:02 UTC, from date): builder B phases B1b and B2 (#1207: the damaged block caught and bracketed to `test_suite_gpu`; Pi 5 suite); the user's `slmstore` rule missed; labctl lost

Sources: builder B's combined B1b and B2 report and its two follow-up
reports, as received; not yet checked by a critic. Branch
`fix/pmm-coalesce` 647c2c33 (unchanged). Evidence `gauntlet-5-hw-jetson`
99bfde86 and `gauntlet-5-hw-pi5` 848b52b9, `run5-hw/1263-1207/`.

### B1b: jetson-nano-2, kexec suite, 647c2c33 (verifier build, `PMM_CHECK` off)

`B-kexec-suite-647c2c33-boot01..10.txt`, `B-trigger-kexec-suite-ver.log.txt`
(10 triggers, ELF 78d13867). Boots 01 to 05 ran before the usage-limit
stop and were already saved; 06 to 10 ran from 21:05 UTC.

| Outcome | Boots | Count |
|---|---|---|
| **PMM halt** (`order 4 free list is empty but its count is 61`) | 04 | **1** |
| Complete, `[FAIL] 12 total failure(s)` | 03, 05, 07, 08, 09 | 5 |
| Lost after the PMM suite to the #1238 family | 01, 02, 06, 10 | 4 |

Both coalescing tests pass in 10 of 10. Verifier findings other than the
self-test: none in 9 boots, two in boot 04.

**Boot 04** (`B-finding-boot04.txt`; line numbers of
`B-kexec-suite-647c2c33-boot04.txt`). Observations:

- No finding at the end of `pmm_init`, before the first suite, or after
  any suite up to `test_suite_oplib_gpu_as`.
- Line 2709 `[TEST] GPU Tests`; line 2755 `[gpu-kind=0] channel bring-up:
  inherit 2072 us ...`, followed by the suite's last four tests (the
  `slm_gpu_run_mnist` / `set_mnist_input` tests, which on kexec run on the
  inherited MNIST channel).
- Line 2768: `[pmm-verify] FINDING 1 at 168048 ms (after test_suite_gpu;
  last suite boundary: after test_suite_oplib_gpu_as)`, order 4, count 64,
  walked 2. The block at 0x139580000 (page state: listed order-4 head) has
  its first 64 bytes all zero, and the 64 bytes before it (0x13957ffc0)
  are all zero.
- Line 5596: FINDING 2 at 178019 ms from `free_list_pop` in
  `test_exhaustion_recovery`, the same block; line 5652 the panic.
- 0x139580000 is in none of the reserved ranges the capture prints (it is
  0x120000 above the MNIST pushbuf at 0x139460000, 65536 bytes). The
  channel-VM pages (84 + 65 ranges) are not printed one by one.
- `xhci_init` printed at line 1816, with many clean walks after it.

So the block was intact after `test_suite_oplib_gpu_as` and damaged by the
end of `test_suite_gpu`: the write happened during `test_suite_gpu`. That
rules out the xHCI hand-over window for this occurrence and is the first
bracket #1207 has had. **Inference, not shown:** the writer is the MNIST
GPU path on the inherited channel (the GPU writing through its page tables
into a page the hand-off reservation does not cover, or SLM-OS code on
that path clearing a buffer whose address or length comes from the
hand-off page). One occurrence; the three earlier halts of this session
have no bytes.

**Halt counts this session, Jetson kexec suite:** 0 of 2 (old allocator,
`PMM_CHECK` on), 2 of 10 (f55c5293), 1 of 10 (647c2c33). The change does
not fix #1207; 1 in 10 against 2 in 10 is no evidence of a change.

### B2: pi-5-1 suite, 647c2c33

`B-pi5-suite-647c2c33-boot01..03.txt` (`gauntlet-5-hw-pi5`): 3 of 3
complete, `[FAIL] 18 total failure(s)` each, the 18 names of Entry 39's
`closeout/pi5-suite-6a4cfa93-boot01.txt`; PMM suite 59 / 58 / 0 / 1; both
coalescing tests and the new tests pass; no finding other than the
self-test. The Pi 5 firmware prints nothing on this UART, so each capture
starts at the kernel banner. Card: original renamed aside in its own call,
restored, `B-pi5-card-and-restore.txt`; restore-check boots
`B-pi5-restore-check-boot01.txt` and `-boot02.txt`, banner b42b2c1d.

### The user's `0:/slmstore/` rule was not met

At about 21:25 UTC the user instructed: before any Pi 5 suite boot, record
a listing and checksums of `0:/slmstore/` on the pi-5-1 card (the
blob-autoload tests write to the real card, #1274), and restore it exactly
afterwards. **Six Pi 5 suite boots ran this session without that record:**
builder A's three (about 12:30 to 13:30 UTC, before the instruction) and
builder B's three (after it: the orchestrator's message reached builder B
only when its turn ended, after B2 had run; the orchestrator knew that
messages are delivered at turn end and should have stopped the builder
first). No before-record exists and nothing in `0:/slmstore/` was
restored.

What was recorded afterwards, through labctl with the board off, card not
modified (`B-pi5-slmstore-after-suite.txt`, `B-pi5-slmstore-after-session-suites.txt`,
`B-pi5-slmstore-files/`, `B-pi5-blob-lines-per-capture.txt`):

| File | Size | sha256 |
|---|---|---|
| `files.lfs` | 8388608 | not fetched (the read closed the labctl connection) |
| `files.lfs.delta` | 6209568 | be716569a7e3b85c44f644ab884803e7168ff288e1bfbac5077c648787bbaab5 |
| `files.lfs.tmp`, `files.lfs.delta.tmp` | 0 | empty |
| `blob_autoload.conf` | 382 | d37ecf936800ad7759ead7ba13ce8ce676efb7f7638896a4f4844d886dfa7f9c |
| `autoload/eviction-xgboost.blob` | 90 | 2cd29947398ce0525c178dc9b8ed359fd2751a97b4f9677c62eff8bbe7dc44e3 |
| `autoload/eviction-mlp.blob` | 17700 | not fetched |
| `autoload/eviction-cacheus_config.blob` | 48 | 74670f63771e45d2d223ab33eac536ec94acc472268432b9cc591d3b04354a91 |

- The mtimes (2026-04-25T00:00:00Z on seven entries) are a fixed stamp:
  `kernel/lib/fatfs/ffconf.h:111-114` (`FF_FS_NORTC 1`, 2026/4/25). They
  cannot show whether a file was written today.
- The conf names three blobs (eviction xgboost, mlp, cacheus_config), the
  three files in `autoload/`.
- From 40 Pi 5 captures on the branch: all 29 production, bench and
  restore-check boots, from the earlier sessions through this one, before
  and after both builders' suite boots, have exactly three
  `blob_autoload: activated` lines (eviction xgboost, mlp, cacheus_config).
  The 11 suite boots have 24 such lines each (21 in close-out boot 04),
  all inside the blob-autoload tests; builder A's three have identical
  blob lines, and so do builder B's three. No capture has a line for a
  store, rename or delete under `0:/slmstore` (the blob code does those
  without printing, `kernel/src/blob_autoload.c:593-679`).
- **Net: the captures neither show nor exclude a write to the card's
  `slmstore` by this session's suite boots. They do show that what a
  production boot loads from it is the same three blobs before and after.**

### labctl lost

Builder B's `sdwire_cat` of `/slmstore/files.lfs` (base64, 8.4 MB)
returned `Connection closed`; since then the labctl MCP server is
disconnected for the whole session (orchestrator included, checked 21:58
UTC). The 6.2 MB file had been read the same way. State at 22:00 UTC,
from SSH and a host listing only: jetson-nano-2 on SD Linux, BootCurrent
0001, no BootNext, `/root/slmos.elf` 098f3163, 14 `*.elf*` files, nothing
of the session in `/tmp`; pi-5-1 powered **off** (builder B powered it off
for the read), original `kernel_2712.img` in place per the restore-check
boots; a 29.1 GB SD card is visible to the host as `sdb`, not mounted,
which may be the pi-5-1 card left on the host side of its SDWire by the
interrupted read (not verified; nothing was done to it). The session's
claims cannot be checked or released; a session hold whose server stopped
expires 30 minutes later. Nothing on the lab can be done from this session
until the server is reconnected: builder A's deciding experiment (A2b),
powering pi-5-1 on, and the release are blocked.

## Entry 49 (written 23:52 UTC, from date): critic round 1 on Part B (FAIL) and the builder's responses; labctl reconnected; lab to builder A for A2b

### Corrections to Entry 48 (from critic B)

- "The three earlier halts of this session have no bytes": two earlier
  halts (f55c5293 boots 01 and 05).
- Boots 06 to 10 of B1b started at 20:56:48 UTC by the trigger log, not
  21:05 UTC.
- **"The write happened during `test_suite_gpu`" and "that rules out the
  xHCI hand-over window" are withdrawn** (finding 2 below).

### Critic B, round 1: FAIL (`critic-B.md`, `stage1-derivation.md`)

Fable 5.2, blind, nothing run. Reproduced from the captures, the code at
647c2c33 and the ELFs: every x50 loop; the full-suite counts (base 4 of 20
failing outside the PMM, e3c22600 8 of 20, f650b695 2 of 13, the A/B 0 of
10 against 2 of 10, 647c2c33 0 of 5; no PMM test fails anywhere); the old
code failing the new tests 5 of 5; the halt signature; both Jetson
old-allocator boots; f55c5293 halts in boots 01 and 05 (2 in 10); 647c2c33
halt in boot 04 (1 in 10) with every quoted line, address and byte; the
coalescing pair failing 2 of 2 on the old allocator and passing 20 of 20
on Jetson and 3 of 3 on Pi 5; the trigger logs (2 + 10 + 10 for 22 saved
boots); Pi 5 3 of 3 with the same 18 names.

Findings:

1. Low. The wrong number above.
2. HIGH. The boot 04 bracket is overstated. The capture shows the header
   at 0x139580000 read intact at the walk after `test_suite_oplib_gpu_as`
   and zero at the walk after `test_suite_gpu`: that bounds when the CPU's
   view changed, not when anything wrote. A device that does not snoop the
   CPU caches can write DRAM under a line the CPU still holds. And
   `test_suite_gpu` is the suite that drops lines: `cache_invalidate_range`
   (`kernel/gpu/cache.c:84-98`) is DC IVAC, discard without write-back; an
   IVAC that reaches a still-dirty header would revert it to old DRAM
   content, a lost CPU write with no writer. Neither is shown; both fit
   every byte.
3. Medium, code. `pmm_dump_block` / `pmm_dump_words` (`pmm.c:683-715`) can
   read unmapped memory on Jetson (the OP-TEE carve-out, above
   0x240000000).
4. Medium, code. `free_list_pop` reports a NULL `next` with blocks still
   counted and carries on, handing out the damaged block while the
   orphaned tail points into it.
5. Medium. In-tree overstatements: "about one suite boot in five" (3 in 20
   this session); "the writer is somewhere else" as fact; the
   `is_buddy_free` comment and `kernel/CLAUDE.md` attribute the hardware
   abort to the merge defect.
6. Low. No hardware row separates the test fix from the allocator change.
7. Low. Litmus failures 3 of 30 at base against 11 of 48 at the fix heads;
   647c2c33 has only 5 full-suite runs.
8. Low. The verifier self-test has no repeat row.
9. Low. Entry 45's "mechanisms 1 and 3 have no source at boot here" rests
   on two non-halting boots.
10. Low. `nvidia_alloc(GPU_MEM_ALIGN_2MB)` allocates order 10 and
    `nvidia_free` returns order 9 (`gpu_nvidia.c:186-227`).
11. Trivial. The start time above.

The critic agrees with: the coalescing failures are a test defect, fixed;
the merge defect is not what halts the board; 2 in 10 then 1 in 10 is not
a fix and no evidence of a rate change; no closing keyword for #1207,
#1228 or #1109.

### Builder B's responses (`fix/pmm-coalesce` 02ebdad8 = 647c2c33 + one commit; `gauntlet-5-hw-jetson` 608ee693; QEMU rows only, the lab was down)

- Finding 2: withdrawn in `B-finding-boot04.txt` and both drafts. The IVAC
  lead read in code (`B-ivac-callers.txt`): two functions are named
  `cache_invalidate_range`; `kernel/include/cache.h:64-74` is DC CIVAC
  (used by the GSP platform layer and `ga10b_bringup.c`, writes back
  first), `kernel/gpu/cache.c:84-98` is DC IVAC. True IVAC sites that can
  run on Jetson: the three GPU tests (the range is the pages the test
  holds, page-aligned, before `gpu_free`); `slm_ffi.c:753` (one line);
  `slm_ffi.c:2102, 2209, 2283, 2462, 2535, 2384` (length rounded up to
  4096 whatever the start offset; not traced to their buffers; may not
  run in the suite); shell commands; boot-time lines. No site is shown to
  reach a listed free block's first line; the last three groups are not
  excluded. No code changed there. Proposed hardware experiment: DC CVAC
  on the header's line after each link write plus a pattern in bytes 16 to
  63 of each listed block's first line, about 30 kexec suite boots.
- Finding 3: `pmm_dump_readable()` reads 64 bytes only inside one free
  heap page; the unmapped holes are marked allocated; test
  `test_verify_dump_reads_only_free_heap_pages`. Not exercised on Jetson.
- Finding 4: the pop panics after the report in the NULL case too
  (`B-qemu-verify-demo-02ebdad8.txt`).
- Finding 5: wording corrected in `pmm.c`, `kernel/CLAUDE.md`,
  `docs/memory-map.md`.
- Finding 7: 15 more interleaved pairs (`B-qemu-ab2-base-661204e6-run01..15.txt`,
  `B-qemu-ab2-fix-02ebdad8-run01..15.txt`): base 11 of 15 pass (4 litmus
  failures), head 14 of 15 (1 litmus). With the earlier 10 pairs: base 4
  of 25 litmus failures, head 2 of 25 plus one #1184. No evidence that the
  change moves the #1230 rate.
- Finding 8: `B-qemu-fix-02ebdad8-verify-x50.txt`, both verifier tests 50
  of 50. Finding 10: comment added on #1240. Findings 6 and 9: stated in
  the PR draft.
- Other rows at 02ebdad8: the x50 loops 50 of 50; `PMM_CHECK=ON` suite 107
  walks ok (the run fails on litmus only); x86-64 pass. **`/ci` did not
  pass in one go:** step 4 (`make test`) failed once on
  `test_litmus_patterns_pass` (`B-ci-02ebdad8-step04-test-arm64-attempt1-litmus.txt`),
  then steps 4 to 11 passed on a resume (`B-ci3b.out.txt`).

Round 2 of critic B was started at 23:50 UTC.

### labctl

The user ran `/mcp reconnect` at about 23:44 UTC. Found: no active claims
(the session holds had lapsed); jetson-nano-2 online; pi-5-1 powered off,
`sdwire_info` reads the card (one FAT32 partition `SLMOS`, clean);
`recovery_mode` relay of jetson-nano-2 open. Both boards re-claimed at
23:45 UTC (session hold, `lab-1263-1207`). Defect to report against
labctl: an `sdwire_cat` of an 8.4 MB file (base64) closed the stdio
connection for the whole session, and there is no tool that returns a
checksum of a card file without returning its content.

23:47 UTC: lab to builder A for A2b (the migration-test experiment on Pi 5
and Jetson UEFI-direct, the changed test x200, one Jetson suite boot at
d09b063b), with the user's `slmstore` rule applied before and after its
Pi 5 boots (small files only; no full-suite boot on Pi 5).

## Entry 50 (written 00:45 UTC on 2026-11-16, from date): critic B rounds 2 and 3 (PASS); builder A phase A2b (the migration-test experiment); boards restored and released; session paused at the user's request

### Critic B

- Round 2 (`critic-5-1207-B-round2.md`): FAIL, narrowly. No code defect
  blocks a merge; every number checked is right; round-1 findings 3, 4, 5,
  8, 9 resolved. Remaining: the drafts said every DC IVAC call site was
  read, and two were missed (`slm_gpu_sync_for_cpu`, `slm_ffi.c:233-243`,
  called from `runtime/src/mm/model_mem.rs:1580` and
  `runtime/src/inference/gpu.rs:259`; the `dc isw` loops in
  `smp_boot.S:246,269`); the tree itself records that Cortex-A76 and
  A78AE execute DC IVAC as DC CIVAC (`mmu.S:46-50`, `smp.c:684-688`,
  `smp_boot.S:205-213`), so a lost header write is excluded on these cores
  if that record holds (the critic notes this corrects its own round-1
  suggestion); the eight boots "lost at
  `test_preempt_flag_follows_migrated_task`" were misdescribed;
  `free_list_remove` still follows a listed block's links unchecked (open
  item); two wordings.
- Builder B's answer: `fix/pmm-coalesce` 122befe0 = 02ebdad8 + comments
  and documents only (`B-text-testkernel-122befe0-vs-02ebdad8.txt`: QEMU
  test kernel `.text` identical, 0 differing bytes; `make test` once,
  `B-qemu-fix-122befe0-fullsuite-run1.txt`, pass; `check-braces` 0
  violations). `/ci` was not re-run at 122befe0 (last full run 02ebdad8,
  with the one litmus retry of Entry 49). Evidence `gauntlet-5-hw-jetson`
  293f8fa8.
- **Round 3 (`critic-5-1207-B-round3.md`): PASS.** All five items resolved
  or carried plainly; the eight per-boot descriptions match the captures
  (all eight fault or stall right after `[PASS]
  test_migrate_ready_task_updates_assigned_cpu`); the diff is comments and
  documents only; no wrong number, no new overstatement, no closing
  keyword. Two optional additions for the PR text: say that `/ci` was not
  re-run at 122befe0; f55c5293 boot08 has two dumps.
- Not checkable by the critic: whether the in-tree statement about DC IVAC
  on these cores is correct; #1240, #1274, #1278 as described.

The Part B PR is not opened yet (pause). It will not close #1207 or #1228.

### Builder A, phase A2b (23:47 to 00:41 UTC): `test_preempt_flag_follows_migrated_task`, padded against unpadded

Source: builder A's report, as received; not yet checked by a critic.
`fix/percpu-cacheline` d09b063b (unchanged). Evidence `gauntlet-5-hw-pi5`
456e4340 and `gauntlet-5-hw-jetson` b5c5dc14. Old test alone,
`TEST_REPEAT=200`, one boot per `boot_test` call; U =
`diag/1263-pdm-U` 6439476f, P = `diag/1263-pdm-P` 6247f73a.

| Row | Files | Result |
|---|---|---|
| Pi 5, U x3 | `A-pi5-pdmdiag-U-6439476f-x200-boot01..03.txt` | 600 of 600 pass; `moved` 5 to 8; `moved=0` 0 |
| Pi 5, P x3 | `A-pi5-pdmdiag-P-6247f73a-x200-boot01..03.txt` | 200 of 200; 180 pass and 20 fail (`moved=0`); 200 of 200 |
| Jetson UEFI-direct, U x3 | `A-uefi-pdmdiag-U-6439476f-x200-boot01..03.txt` | 200 of 200; 199 pass and **1 fail with `bad=1`** (below); 200 of 200; `moved=0` 0 |
| Jetson UEFI-direct, P x3 | `A-uefi-pdmdiag-P-6247f73a-x200-boot01..03.txt` | 31, 3, 29 fail, all `moved=0` |
| Jetson kexec, U x1 | `A-kexec-pdmdiag-U-6439476f-x200-boot01.txt` | 200 of 200 |
| Jetson kexec, P x1 | `A-kexec-pdmdiag-P-6247f73a-x200-boot01.txt` | 200 of 200 |

`moved=0` in 83 of 1400 padded iterations and 0 of 1400 unpadded
(summaries `A-pi5-pdm-summary.txt`, `A-uefi-pdm-summary.txt`). In the
failing iterations both workers wake on one CPU nearly every time with 131
to 162 steals in the iteration (for example `pdm-diag: steals=162
a=[0,0,0,100,0,0] b=[0,0,0,100,0,0]`); the CPU differs per boot. Steal
totals per boot: unpadded 658 to 1515, padded failing boots 4974 to 8145.
Inference (the builder's): with the padded layout one CPU wins each
freshly woken worker, so it never changes CPU. So the old test's coverage
failure follows the padded layout; the flag check found no error on the
padded side (`bad` 0 in 1400).

Changed test (d09b063b), alone x200: Pi 5 x3
(`A-pi5-pdmnew-d09b063b-x200-boot01..03.txt`) 600 of 600; Jetson
UEFI-direct x1 (`A-uefi-pdmnew-d09b063b-x200-boot01.txt`) 200 of 200;
Jetson kexec x1 (`A-kexec-pdmnew-d09b063b-x200-boot01.txt`) 200 of 200;
`bad` 0 in all 1000. Full suite at d09b063b, Jetson UEFI-direct x1
(`A-uefi-suite-d09b063b-boot01.txt`): completes, 21 failures, none outside
the known list, the changed test passes. `/ci` at d09b063b: 12 of 12 steps
(`A-ci-d09b063b-step01..12-*.txt`).

**New observation on the unpadded kernel, not investigated:**
`A-uefi-pdmdiag-U-6439476f-x200-boot02.txt`, iteration 16: `pdm:
migrations=41654 moved=7 runs=5/3 bad=1 (first on cpu 5)`, `FAILED at
test_scheduler.c:2390 Expected 0 Actual 1`. That is the test's real
assertion (a resumed worker saw its CPU's `preempt_disabled` non-zero), on
ae1399ba (the head of PR #1266) plus the print: 1 of 1400 unpadded
iterations, 0 of 1400 padded, 0 of 1000 with the changed test. No issue
filed yet; it bears on PR #1266, not on the padding.

Not done: kexec was one pair, not three; no kexec suite at d09b063b.

### `0:/slmstore/` on pi-5-1 during A2b (the user's rule, applied)

`A-pi5-slmstore-before-a2b.txt` and `A-pi5-slmstore-after-a2b.txt`: the 9
entries (path, size, mtime) and the 4 hashes (the conf and the three
blobs, including `eviction-mlp.blob`, which had no hash before) are
identical by script; nothing to restore. `files.lfs` and
`files.lfs.delta` were listed, not read. The nine filtered-test captures
have no `blob_autoload` or `0:/slmstore` line. No full-suite boot ran on
Pi 5 in A2b. Limit: the after-record precedes the restore-check boot of
the original kernel.

### Lab end state (orchestrator's own checks, 00:42 UTC)

- jetson-nano-2: powered on, online; SD Linux, BootCurrent 0001, no
  BootNext; ESP `SLMOS.efi` 789cba626338af47 from a fresh read-only mount;
  `/root/slmos.elf` 098f3163; 14 `*.elf*` files; nothing of the session in
  `/tmp`. Builder A's record: `A-jetson-post-state-A2b.txt` (fsck 6 files,
  2196/130892 clusters).
- pi-5-1: powered on, online; original `kernel_2712.img` back,
  `A-pi5-restore-check-a2b-boot01.txt` banner b42b2c1d.
- No session image left in `/var/lib/labctl/images`; no trigger or QEMU
  process.
- **Both boards released at 00:43 UTC; no active claims.**

### Paused

The user asked for a pause at about 00:00 UTC; the pause point is here.
Not yet done: critic A round 2 (on the answers to round 1 and the A2b
rows); the Part A draft PR (closing #1263 only) and the #1262 comment; the
Part B draft PR (references #1207 and #1228, closes neither) and the
#1207 comment; marking each PR ready after its critic passes; an issue for
the `bad=1` observation; the final report. Nothing is merged, and no PR or
comment on #1262 or #1207 has been posted by this session.

## Entry 51 (written 01:10 UTC on 2026-11-16, from date): resumed; session model changed to Opus

The user resumed the session at about 01:05 UTC and switched its model
from Fable 5.2 to Opus to conserve Fable usage. From this entry on: the
orchestrator, both builders and any helper agent run on Opus (they inherit
the session model); critics stay on Fable, spawned with the model set
explicitly. If a critic hits a Fable usage limit it is paused and the user
is told; it is not moved to Opus. Everything before this entry (builders,
orchestrator and all critic rounds so far) ran on Fable 5.2.

Started at 01:08 UTC:

- Critic A, round 2: a fresh blind critic on Fable (`critic-a2`), not the
  round-1 critic's session, deriving the A2b rows, the test change and the
  round-1 answers before reading the claims and the round-1 findings.
- Builder B, phase B3: open the Part B PR as a draft (it closes nothing),
  post the #1207 comment, note on the known-failures list.
- Builder A: file the `bad=1` observation as an issue and link it on PR
  #1266; write down why the changed test's direct write of `cpu_affinity`
  is safe and what the pinned worker no longer covers. The Part A PR and
  the #1262 comment stay unposted until the critic reports.

## Entry 52 (written 01:24 UTC on 2026-11-16, from date): PR #1283 open and ready (Part B); issue #1284; critic A round 2 (FAIL); a further Part A slot

### Part B: PR #1283

`fix/pmm-coalesce` 122befe0 against `main`, stacked on #1265, opened by
builder B as a draft and marked ready by the orchestrator at 01:14 UTC
after critic B's round-3 PASS. **It closes nothing:**
`closingIssuesReferences` is empty (checked after creation, after the
comments and after marking ready); #1207, #1228, #1109 and #1244 are open.
The brief asked for a PR closing #1207; the evidence does not support
that (3 PMM halts in 20 kexec suite boots with the change in). Comments
posted: #1207 (halt counts, the boot 04 capture as observations, next
experiments), #1228 (a pointer), #1109 (the two coalescing tests pass in
20 of 20 Jetson kexec suite boots and 3 of 3 Pi 5 boots and can come off
the list when #1283 merges).

### Issue #1284 (P1-high), filed by builder A

The `bad=1` observation of Entry 50: on ae1399ba (the head of PR #1266)
plus a test-only print, one of 1400 iterations of
`test_preempt_flag_follows_migrated_task` on Jetson UEFI-direct had a
resumed worker read `preempt_disabled[]` non-zero for its CPU
(`A-uefi-pdmdiag-U-6439476f-x200-boot02.txt:3012-3017`). Every other `pdm:`
line in the archive reads `bad=0`. Two readings are open: a kernel defect
in the resume path, or the test's own race (the worker reads `cpu_id()`
and then the flag unpinned with interrupts open; the diagnostic print put
an increment between the two reads). Linked on PR #1266.

### Critic A, round 2: FAIL (`critic-5-1263-A-round2.md`)

A fresh blind critic on Fable (not the round-1 critic's session). It
derived the A2b rows, the test change and the documents before reading the
claims and the round-1 findings. Reproduced: the A2b table with all rows
and per-boot splits; banners and build ids of all 19 A2b captures; 200
iterations each; `bad=0` in 1400 padded and 1000 changed-test iterations;
the UEFI and kexec order from the trigger logs with no unsaved boot; the
d09b063b suite boot; `/ci` at d09b063b 12 steps; the slmstore records (9
entries and 4 hashes identical). Round-1 findings: 1 to 6, 9, 10 resolved
in the texts (figures re-derived: 6701 us, 14 of 279, 0 of 822); 7 carried
plainly (#1279); 8 resolved in code. No kernel code defect.

Findings:

1. HIGH. `bad=1`: no issue number in the PR, an unsupported attribution
   ("bears on #1266, not on the padding" is not shown), and a comparison
   that is not like for like (padded boots had 88 to 92% of wake-ups on
   one CPU; the changed test's pinned worker never runs on CPUs 4 and 5).
   The critic wrote before #1284 existed.
2. HIGH. The test change removes exposure to the path where `bad=1` was
   seen, undisclosed: `pdm_a` is pinned (not stealable, CPUs 1 to 3 only),
   and on Jetson the unpinned `pdm_b` did not move in the changed test
   (`A-uefi-pdmnew-d09b063b-x200-boot01.txt`: 0 CPU changes in 1200
   wake-ups). The test is not vacuous, but no run of it on a kernel with
   the #1229 fix reverted exists.
3. MEDIUM. Wrong statements in the PR body: "the two new layout tests
   pass" (one passes, one is IGNORE on that kernel); "`/ci` not measured
   after c0612fa7" (it was, at d09b063b); the `SLOT_TRACE=ON` run was on a
   dirty tree; steal ranges (131 to 164, not 131 to 162).
4. MEDIUM. The same coverage failure is in the archive on unpadded kernels
   without the #1234 fix (`c2/hw-base/uefi-r2-boot1.txt`, 88d4d71b, 38 of
   200; `c2-fix-critic/base-3b369aef-r2-boot02.txt`, 13 of 34). So the
   one-CPU regime is not new with the padding: present before the #1234
   fix, absent after it, present again padded. Round 1 missed these too.
5. MEDIUM. The behaviour change is real and was treated only as test
   fragility. `moved=0` in 83 of 1400 padded against 0 of 1400 unpadded;
   the unit is the boot (4 of 7 padded, 0 of 7 unpadded). **Entry 50's
   steal totals (658 to 1515 against 4974 to 8145) are true and
   misleading:** failing iterations run 1 s with 200 wake-ups, not 50 ms
   with 10. Per wake-up: UEFI padded 0.79 to 0.86 against 0.64 to 0.76;
   kexec 0.77 against 0.65; Pi 5 padded 0.16, 0.64, 0.19 against 0.32 to
   0.36. There is no five- to eight-fold increase in stealing. What
   changes is where a woken task runs: padded sticky boots have 88 to 95%
   of wake-ups on one CPU, unpadded at most 56%. Harm is neither shown nor
   excluded: no latency or throughput row compares them, and
   `--assert-shares` x20 ran on kexec only, the path with no sticky boot.
6. LOW-MEDIUM. The test's flag read is racy for an unpinned worker.
7. LOW. Evidence defects: two `/ci` step files carry another step's QEMU
   section; the slmstore "before" hash lines do not show when they were
   read; the Pi 5 alternation order rests on header text.
8. LOW. Documents: `spinlock.h` states the reservation-loss mechanism as
   fact; c0612fa7's commit message keeps stale figures.

### Decisions (orchestrator)

- The test is reworked (test-only; kernel code stays at c0612fa7): `pdm_a`
  and `pdm_b` unpinned as in the old test, a third worker that rotates its
  affinity through `sched_set_task_affinity()`, the flag read made with
  interrupts masked across `cpu_id()` and the read; one mutation run with
  the #1229 guard reverted.
- The PR text will state the behaviour change as such, with the archive
  fact of finding 4 and per-wake-up steal figures.
- A further lab slot for builder A (A2c), boards re-claimed at 01:20 UTC:
  the reworked test x200 on Pi 5 (filtered test only, with the `slmstore`
  record before and after), Jetson UEFI-direct and kexec; the mutation
  boot if QEMU cannot show it; one suite boot per Jetson path;
  `--assert-shares` x20 on Jetson UEFI-direct, padded against unpadded.
- Then critic A round 3, on Fable.

## Entry 53 (written 02:34 UTC on 2026-11-16, from date): builder A phase A2c (test rework, mutation run, `--assert-shares` on UEFI-direct); boards restored and released again; critic A round 3 started

Source: builder A's A2c report, as received; not yet checked by a critic.
`fix/percpu-cacheline` f3c0d021 = c3ce3885 (the three-worker test and
comments) + one document-only commit; the A2c hardware rows are at
c3ce3885. Kernel code outside tests is c0612fa7's apart from one comment
in `spinlock.h`. Evidence `gauntlet-5-hw-jetson` 20d62916 and
`gauntlet-5-hw-pi5` 067161f2.

### The test, reworked (c3ce3885, test only)

`pdm_a` and `pdm_b` are unpinned as in the original test; a third worker
`pdm_c` rotates its affinity over CPUs 1 to 3 through
`sched_set_task_affinity()` and carries the coverage condition; the run
ends after 10 `pdm_c` wake-ups or 1 s. All three workers read `cpu_id()`
and the flag with interrupts masked. The output gives per-worker wake-ups,
moves, bad reads and wake-ups per CPU.

Mutation run (QEMU, `A-qemu-mutation-1229-reverted-pdm3-x20-ws.txt`,
carries the source diff): with `sched.c:2931` reverted to `now_cpu =
this_cpu`, the test fails 20 of 20, bad reads on all three workers in
iteration 1. Not booted on hardware.

QEMU at c3ce3885: the test x200 with `WORK_STEALING=ON` 200 of 200
(`A-qemu-pdm3-c3ce3885-x200-ws.txt`); `make test SLOT_TRACE=ON` passes
with `test_slot_trace_arrays_one_line_per_cpu` PASS at a committed head
(`A-qemu-arm-slottrace-make-test-c3ce3885.txt`); `/ci` 12 steps
(`A-ci-c3ce3885-step01..12-*.txt`), of which step 6 (`make test
AI_SCHED=ON`) **failed once** on `test_litmus_patterns_pass` (#1230,
`A-ci-c3ce3885-step06-test-ai-sched-attempt1-FAILED.txt`) and passed on a
rerun of the same build. `/ci` was not rerun at f3c0d021 (documents only).

### Lab rows at c3ce3885, one boot each, x200

| Row | File | Result |
|---|---|---|
| Pi 5 | `A-pi5-pdm3-c3ce3885-x200-boot01.txt` | 200 of 200, bad 0; `pdm_a` 2196 wake-ups, 195 moves in 26 iterations; `pdm_b` 2196, 219 moves in 27; unpinned 91.1% on CPU 3 |
| Jetson UEFI-direct | `A-uefi-pdm3-c3ce3885-x200-boot01.txt` | 200 of 200, bad 0; `pdm_a` 2237, 78 moves in 4 iterations; `pdm_b` 2234, 72 moves in 4; 79 unpinned wake-ups on CPUs 4 and 5; unpinned 98.2% on CPU 3 |
| Jetson kexec | `A-kexec-pdm3-c3ce3885-x200-boot01.txt` | 200 of 200, bad 0; `pdm_a` 2800, 1261 moves in 61 iterations; `pdm_b` 2802, 1265 moves in 61; 1324 unpinned wake-ups on CPUs 4 and 5; unpinned 76.4% on CPU 1 |
| Suite, Jetson UEFI-direct | `A-uefi-suite-c3ce3885-boot01.txt` | completes, 21 failures, none outside the known list; the test passes; 0 halts |
| Suite, Jetson kexec | `A-kexec-suite-c3ce3885-boot01.txt` | completes, 14 failures, none outside the known list; the test passes; 0 halts |

The reworked test passes everywhere, and the unpinned workers still stick
to one CPU on the padded kernel: on Jetson UEFI-direct they moved in 4 of
200 iterations. `pdm_c` gives the #1229 cross-CPU resume on every boot;
the unpinned steal and placement exposure is real on kexec (61 of 200
iterations, reaching CPUs 4 and 5) and small on UEFI-direct and Pi 5 in
these single boots.

`--assert-shares` x20 (`A-assert-shares-compare.txt`), lowest thief share
min / median / max:

| Path | Kernel | Result |
|---|---|---|
| Jetson UEFI-direct | padded c3ce3885 | 20 of 20 pass; 15.3 / 15.9 / 16.4% |
| Jetson UEFI-direct | unpadded ae1399ba | 20 of 20 pass; 14.1 / 15.0 / 15.9% |
| Jetson kexec | unpadded ae1399ba | 20 of 20 pass; 15.7 / 16.3 / 16.5% |
| Jetson kexec | padded c0612fa7 (A2, Entry 46) | 20 of 20 pass; 14.8 / 16.1 / 16.6% |

The check measures each thief's share of stolen tasks under a sustained
supply from CPU 1. It does not measure where a sleeping task wakes, nor
latency or throughput, so it cannot see the sticky-wake behaviour. **No
row of this session compares latency or throughput between a sticky and a
non-sticky boot.**

Correction to Entry 52 (the critic's and the builder's counts): steal
counts for iterations of exactly 100 wake-ups per worker are 131 to 136 on
Pi 5 and 157 to 162 on UEFI-direct; the 164 upper bound includes
iterations of 101 wake-ups (`A-pdm-normalise.txt`).

### Incident: scratch files edited by another writer

Builder A reports that files in its scratch directory (the PR-body
template, its patch script, the #1262 comment draft) were changed on disk
by someone else while it worked, and that it re-read and layered its
changes on top. The orchestrator did not edit them. Who did is not
established (possibly a helper agent the builder itself started). Checked
at 02:33 UTC: #1262 has no new comment (1 comment, dated 2026-11-15
03:50 UTC), no PR exists from `fix/percpu-cacheline`, #1284 is open with
one comment. The texts are read in full by the critic before anything is
posted.

### `0:/slmstore/` on pi-5-1 during A2c

Recorded before and after the one filtered-test boot
(`A-pi5-slmstore-before-a2c.txt`, `A-pi5-slmstore-after-a2c.txt`, with
read timestamps this time); restore-check boot
`A-pi5-restore-check-a2c-boot01.txt`. The comparison is checked by the
critic.

### Lab end state (orchestrator's checks, 02:33 UTC)

jetson-nano-2: online; SD Linux, BootCurrent 0001, no BootNext; ESP
`SLMOS.efi` 789cba626338af47 from a fresh read-only mount;
`/root/slmos.elf` 098f3163; 14 `*.elf*` files; nothing of the session in
`/tmp`. pi-5-1: online, powered on. No session image in
`/var/lib/labctl/images`. Both boards released at about 02:34 UTC.

Critic A round 3 started at about 02:34 UTC: a fresh blind critic on Fable
(`critic-a3`).
