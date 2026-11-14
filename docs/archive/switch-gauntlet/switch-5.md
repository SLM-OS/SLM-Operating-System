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
