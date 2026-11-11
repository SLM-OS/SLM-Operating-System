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
