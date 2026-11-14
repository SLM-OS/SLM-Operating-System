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
