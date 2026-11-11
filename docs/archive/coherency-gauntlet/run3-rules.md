# Rules for every coherency run 3 builder and critic (#1193, #1216)

Read `CLAUDE.md`, `docs/archive/475-gauntlet/builder-rules.md` and
`run2-rules.md` (this directory) first. `run2-rules.md` applies with
`coherency-2` read as `coherency-3`, `gauntlet-2-hw-*` as `gauntlet-3-hw-*`
and `run2-hw/` as `run3-hw/`. Where this file differs, this file wins.

## Lab: one component at a time

- Boards: pi-5-1 and jetson-nano-2 only.
- The lab is a single lane, in the order written in Entry 0 of
  `coherency-3.md`: C1 (pi-5-1), then C2 (jetson-nano-2), then C3
  (jetson-nano-2). A component holds the lab only after the orchestrator
  says so in writing. A component that does not hold the lab makes no
  labctl call at all, not even a read.
- The component that holds the lab claims its board itself with
  `claim_sbc` (`agent_name` `coherency-3-C<n>`, the critic
  `coherency-3-C<n>-critic`), renews as needed and releases it before it
  reports. Never release, renew or force-release a claim it did not make.
- Boot and capture in one call: `boot_test`, or `/deploy-and-test`, which
  captures internally. Never a `power_cycle` followed by a separate
  `serial_capture`: one stdio labctl server runs one call at a time, and
  the capture misses the boot (run 2, Entry 17). If a capture must be
  separate, say in the report that no other component was calling.
- The ssh/scp path is allowed exactly as `/deploy-and-test` and
  `scripts/jetson-uefi-deploy.sh` document it. No other ssh, no manual
  serial, no reading labctl's database or config files, no private boot
  helpers (a disclosed break goes in the verdict; an undisclosed one fails
  the component on process).
- `boot_test --count 10` (runs=10) is the minimum for a claim that a
  kernel boots. A deploy that does not come back gets the #1195 round trip
  once, recorded, then the component moves on.
- No Jetson BL31 or TF-A flashing (#976). Nothing is powered off at the
  end: both boards were found on, at Linux (Jetson) and the SLM-OS card
  (pi-5-1).
- Pi 5 images: labctl reads from
  `/home/john/projects/CS-496-Capstone-SLM-Operating-System/build/kernel/`
  and `/var/lib/labctl/images/`. Copy an image there under a unique name
  (`slmos-r3-<comp>-<tag>.bin`) with a temp-name-then-rename copy; never
  overwrite another name.

## Heads

- The removal head and the instrumentation head are the ones in bold in
  Entry 0. Every count is taken on the head it names. If a head must move,
  it moves once, the log says why, and counts taken on the old head restart
  or are carried with the critic's written agreement.
- Every evidence file names a pushed commit in its banner (the boot banner,
  or `cat /sys/version`). A reconstructed head is labelled reconstructed.

## Evidence

- `gauntlet-3-hw-pi5` (C1, and QEMU evidence) and `gauntlet-3-hw-jetson`
  (C2, C3), under `docs/archive/coherency-gauntlet/run3-hw/c<N>/`, critics
  under `run3-hw/critic-c<N>/`. One file per run; the filename carries
  build, path and run number; the version banner is at the top.
- Save before claiming. A number without a filename is not written
  anywhere: not in a report, an issue, a PR body or a log entry.
- Before every push: `git pull --rebase origin <branch>`, then plain
  `git push`.

## Stop conditions are per component

- A finding that would change deque or scheduler-lock memory ordering ends
  the step that would land it. Record the finding and the proposed change;
  keep measuring.
- A C1 visibility finding (an awaited condition never observed) ends C1's
  lab work and is reported to the orchestrator at once.
- A board unreachable for more than 45 minutes after the round-trip rule
  ends the component that holds it.
- Only these end the run: both boards unreachable; any proposal to flash
  BL31, patch TF-A or touch another board; budget.

## Tools

Only the tool families in run 2's Entry 27, all exercised in C0. A builder
that needs anything else asks the orchestrator in its report instead of
calling it.

## Reporting back

As in `run2-rules.md`. Do not edit `coherency-3.md`; return the log entry
text in the final report.
