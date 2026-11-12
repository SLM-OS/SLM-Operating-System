# Lifecycle gauntlet log, run 4 (#1213, #1223, #1211, #1219, #1199, #1224)

Goal: root-cause and fix #1213 under a three-part bar (first-fault trace,
a stress test that fails on the unfixed kernel at a measured rate, and the
fix passing 5x as many runs as it took on average to see a fault); decide
whether #1223, #1211 and #1219 are the same defect; get the Jetson
integration suite to its last test on kexec and UEFI-direct, 10 of 10 boots
each; convert test_isolated_core_latency's last count-bounded wait so the
isolation path runs on every board. The brief is the user's message of
2026-11-11 (PT). This log is append-only: a correction is a new entry
citing the one it corrects, and every entry carries its own write time.

Boards: pi-5-1 and jetson-nano-2 only. Evidence: `gauntlet-4-hw-pi5`
(Pi 5, plus QEMU under `run4-hw/c1/` and `run4-hw/c2/`) and
`gauntlet-4-hw-jetson`, one file per run under
`docs/archive/lifecycle-gauntlet/run4-hw/c<N>/`, banner hash at the top.

## Budget

- Started: 2026-11-12 01:05:43 UTC (17:05:43 PST, the orchestrator's first
  action). Limit: 10 h, 11:05 UTC.
- Hour marks (UTC): C0 done 01:35; C3 holds jetson-nano-2 01:35 to 04:35;
  C3 critic boots 04:35 to 05:05; C1 stress test and fix in QEMU 04:05 to
  07:05; C2 holds jetson-nano-2 for --assert-shares and C1's Jetson boot
  tests 07:05 to 08:05; C1 holds pi-5-1 08:05 to 10:05 (C1 critic
  reproduction inside it); C2 critic, PRs, close-out 10:05 to 11:05.
- A component not at its mark loses the lab at the next handover and ends
  with its last result.

## Rules summary

- Run 3's rules carry over (`docs/archive/coherency-gauntlet/run3-rules.md`
  and `run3-critic-protocol.md` on `gauntlet/coherency-3`) with the brief's
  deltas: this run may change task lifecycle and scheduler ordering, priced
  by the three-part bar, the critic's independent Arm ARM ordering review and
  --assert-shares 20/20 on Jetson kexec.
- Single lab lane; no labctl call without holding the lab; every handover is
  an entry with its time. Boot and capture in one call. No BL31/TF-A
  flashing. Jetson ESP and Pi 5 card restored, both boards left powered on.
- Trace ring behind a build option, default off, zero text-size cost when off.
- Draft PRs only, no merge to main. Each component owns its files.
- No number without a filename. No em-dashes, no second person, hypotheses
  labelled until tested.
- Critics run on Fable, blind (definition of done, issues, evidence paths,
  heads only); builder entries are written after the critic files.
- Stop conditions per component as in the brief; the run stops only for both
  boards unreachable more than 45 min after the round-trip rule, any
  proposal to flash BL31, patch TF-A or touch another board, or budget.

## Entry 0 (written 01:08 UTC, completed in Entry 0a): hour-0 checks

Read: issues #1213, #1223, #1211, #1219, #1199, #1224 (states below);
coherency-3 log headings. Entries 8, 9, 15 and 26 to 32 are handed to the
builders whose components cite them; the orchestrator does not re-derive.

1. **Reminder self-test.** This session has no `send_later`,
   `list_triggers` or `delete_trigger`; the equivalents are the
   session-only `CronCreate`, `CronList`, `CronDelete`. One-shot job
   `001d9f0e` created 01:05:50 UTC for 01:07 UTC; `CronList` showed it with
   no prompt. Fire result: Entry 0a. A cron job fires only while the session
   is idle, not mid-turn.
2. **First use of every tool family**, no prompt on any: `CronCreate`,
   `CronList`; `gh pr list` (no open PRs, so `gh pr comment` is skipped
   until this run opens one), `gh issue view` on the six issues; labctl
   `list_claims`, `bindings_list`, `claim_sbc`, `run_health_check`,
   `actuation_status`, `release_sbc` on pi-5-1 then jetson-nano-2; `git
   push` of the two evidence branches; background Bash; Agent with model
   fable (item 7). Monitor and SendMessage are loaded; first use in 0a.
3. **Lab state (01:06 UTC).** No active claims. pi-5-1: online, ping,
   serial, power on, no bindings. jetson-nano-2: online, ping, serial, power
   on; `recovery_mode` binding desired `released`, relay-a channel 1
   last_state `open` (changed 2026-11-11 22:41:20 UTC). Both released after
   the check.
4. **main.** Head **6c1f4c9c** ("bench stealing --assert-shares and bench
   idle-wakes (#1216) (#1227)"). Merged: #1208 8a27711d, #1209 cf34214d,
   #1215 73d8deb7, #1217 88b28c15, #1218 e4a33790, ADR 0007 accepted
   e73b9b61, removal #1225 98e17440, --assert-shares #1227 6c1f4c9c.
   Closed: #1226, #1216. Open: #1213, #1223, #1211, #1219, #1199, #1224.
   /ci and 3 runs of `make test WORK_STEALING=ON` on 6c1f4c9c: running from
   01:07 UTC in a clean worktree at main; results and the #1219 baseline
   count in Entry 0a.
5. **Branches.** `gauntlet/lifecycle-4` (this log) from 6c1f4c9c;
   `gauntlet-4-hw-pi5` and `gauntlet-4-hw-jetson` pushed at 6c1f4c9c at
   01:07 UTC.
6. **Lab schedule (single lane).** C3 holds jetson-nano-2 from the end of
   hour 0 (target 01:35 UTC) to 04:35, then the C3 critic to 05:05. C2
   holds jetson-nano-2 when C1 has a fix head (mark 07:05) for
   --assert-shares 20/20 and C1's Jetson boot tests. C1 then holds pi-5-1
   (mark 08:05). pi-5-1 is idle until then; nobody calls labctl without
   holding the lab.
7. **Critic model.** A trivial critic spawned with model fable answered
   "critic-check ok, model: Fable 5.2 (claude-fable-5-2)". Critics run on
   Fable.
8. **Milestone.** Backlog for new issues.
