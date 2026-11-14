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
