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
