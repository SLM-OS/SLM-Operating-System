# Switch gauntlet, run 5: brief

The user's brief of 2026-11-13 (PT), transcribed for builders and critics.
Tables are reproduced as written. Where this file and the log disagree on a
fact about state, the log (switch-5.md) is current; where they disagree on a
bar or rule, this file governs.

## Goal

A task can be switched in on one CPU from a saved context that another CPU has
not finished writing. Run 4 named the window (#1234, P1) and reverted the first
attempt to close it, because spinning on next->on_cpu under rq_lock deadlocks:
the outgoing CPU's switch window is open to IRQs, and a tick there takes the
waiter's rq_lock. Run 4's close-out session then showed the window is not
theoretical. test_preempt_flag_follows_migrated_task hits it in roughly 40% of
Jetson kexec suite boots, on every CPU, with or without #1237 (#1238: arm B
without #1237 5 of 9, arm A with it 3 of 8). Signatures: the same idle task
running on two CPUs at once with a PC 0x0 instruction abort; wild PCs out of
the tick path through rq_lock and scheduler_tick+0xa8; SPSR holding a stack
address.

It is reachable in production, not only from the test. sched_migrate_task is
called from task_set_affinity for READY tasks (kernel/sched/sched.c) and from
the Lua binding in kernel/src/lua_slm.c, which the policy layer uses to move
tasks.

This run closes the window, proves it closed on both boards, and also lands the
two small harness changes that slowed runs 4 and 4b. The PMM halt (#1207), now
shown independent of the #1213 fix and of #1237, is a separate component.

## Reference bars

Every bar names every cell that bears on it. Every threshold that depends on a
baseline is computed from this run's baseline, measured before the
intervention, on the same board, path and head family.

- **Reproducers (needed before any fix).** Three, each with a baseline rate
  measured in this run on main 5b28d065 (plus #1239 if it has merged): (R1) run
  4's critic window patch in QEMU thread=multi, which holds the outgoing switch
  window 25 ms; (R2) test_preempt_flag_follows_migrated_task in a repeat loop
  on jetson-nano-2 kexec, using the C1 harness filter; (R3) a new targeted
  stress test that migrates READY tasks between CPUs in a tight loop while
  their previous CPU is switching out, asserting no task is ever RUNNING on two
  CPUs and no context is restored before its save completes. R3 must fail on
  main at a measured rate in QEMU and on at least one board.
- **Fixed.** At the fix head, each reproducer runs 0 failures in at least 5
  times as many iterations as it took, on average, to see a failure at
  baseline, on every board and path where it failed at baseline, and also on
  Pi 5 for R2 and R3 even if Pi 5 did not fail at baseline. A reproducer that
  never failed at baseline on a cell cannot be used to claim that cell fixed.
- **No deadlock.** The fix comes with the window patch kept as a test (R1) that
  hangs or faults on the run-4 reverted design (1ead6ef6 on top of today's
  main, rebuilt) and completes at the fix head, with a tick forced into the
  window. The critic derives the lock graph from code before reading the
  commit message.
- **No regression.** At the fix head: /ci green; make test WORK_STEALING=ON no
  worse than this run's baseline rate on main; boot_test --count 10 on Pi 5 and
  both Jetson paths with full captures; bench stealing --sustain 2500
  --assert-shares 20 of 20 on Jetson kexec; bench smp context-switch time
  within 10% of main's on both boards (the removal's gains are the reason the
  kernel is fast; the fix must not give them back without saying so).
- **#1207.** Root-caused from a first-fault capture, with a test that fails on
  the old code, or "open" with the capture and the experiment that would
  decide it.

## State at the start

Read in this order: issues #1234, #1238, #1207, #1228; in
docs/archive/lifecycle-gauntlet/lifecycle-4.md on gauntlet/lifecycle-4, Entry
11 (the critic's deadlock derivation and window patch), Entries 15 to 17 (the
revert and the x86 IPI guard), Entry 35 (the #1213 fix as merged), and Entries
41 to 43 (#1238, the A/B arms, the fault signatures); critic-4-C1.md sections
on the window patch.

On main (5b28d065 when this was written). The NC-axis removal (#1225),
--assert-shares (#1227), and #1237: per-task on_cpu set on switch-in under
rq_lock and cleared with STLR in sched_finish_switch; task_destroy waits on it
with LDAPR (2 s, then warns and leaks); zombie_gen; the post-switch cpu_id()
re-read (#1229); the x86 resched-IPI guard (#1236). The switch-in wait on
next->on_cpu is not on main; it was reverted. PR #1239 (Jetson suite fixes,
#1199 and #1224) is open; if it has merged, use main as is; if not, C0 decides
with the user whether to base on it.

[Orchestrator note: #1239 was merged in C0 with the user's approval; the base
is main 8ec0e330. See switch-5.md Entry 0.]

## Code and evidence to reuse

| Where | What |
|---|---|
| run4-tools/critic-c1-runs/ on gauntlet/lifecycle-4 | The critic's window patch (holds the outgoing switch window 25 ms), its runs at 6d4fe041 (2 of 2 deadlock) and the control (30 of 30). The basis of R1. |
| run4-tools/deleted-branch-tips/ | 1ead6ef6, the reverted switch-in wait. Needed for the no-deadlock bar. |
| c1/1238-arm-b @ fcf91277 | Arm B: main before #1237 + #1239 + the migration test. |
| gauntlet-4-hw-jetson @ 804ad107, run4-hw/1238/ | 20 boots: the #1238 fault dumps on both arms, README with symbols. |
| run4-hw/closeout/ on both evidence branches | #1237's 30 full-capture boots; the kexec suite at 1b85b315 (#1238 2 of 10, #1207 1 of 10). |
| trigger.sh.txt, trigger-ab.sh.txt (Entries 39, 43) | The host-side kexec/BootNext trigger loop. Nothing is installed on the board. |

## Open, in scope

| # | Known | Owed |
|---|---|---|
| #1234 | P1. The switch-in window; spinning under rq_lock deadlocks; window patch reproduces in QEMU | Closed by a fix meeting every bar |
| #1238 | P1. Jetson, about 40% of suite boots in the migration test, pre-existing; same family as #1234 by signature | Closed with #1234, or shown to be a different defect |
| #1207 | PMM halt: write abort in pmm_alloc_pages from test_exhaustion_recovery, CPU 0, after test_checkerboard_fragmentation; 1 of 10 at 1b85b315, also on a tree without #1237 | Root cause or open with capture |
| #1228 | PMM family, not reproduced | Linked to #1207 or not, if C3's capture says |

Out of scope. #1223 (Pi 5 isolation panic on the old split head), #1211,
#1230, #1231, #1232, #1233, #1235, #1204, #1210, #1212, #1214, #1220, #1221.

## C0: hour-0 checks

The orchestrator does these itself before any builder starts, while the user is
present, one line each in Entry 0.

1. Reminder self-test. send_later 2 minutes out; if it arrives, schedule the
   hour marks; if not by minute 5, run without reminders and poll with
   Monitor. Exercise delete_trigger on it after it fires.
2. First use of every tool family. list_triggers; gh pr list, gh issue view
   1234; labctl list_claims, then claim_sbc, run_health_check, release_sbc on
   each board in turn; git push of the log branch; spawn one trivial critic
   with the model set to fable. Any prompt answered now is one nobody answers
   at 2 AM.
3. Lab state. Both boards unclaimed, online, powered on. Jetson ESP SLMOS.efi
   789cba62, no BootNext. If either is claimed by another session, stop and
   tell the user.
4. main and #1239. Write the head of main in bold. If #1239 is still open, ask
   the user whether to merge it now or base on it; do not merge without a yes.
   Run /ci on the base and three make test WORK_STEALING=ON runs; record the
   counts (this run's #1230-family baseline).
5. Branches. gauntlet/switch-5 from main with
   docs/archive/switch-gauntlet/switch-5.md; gauntlet-5-hw-pi5 and
   gauntlet-5-hw-jetson from main, evidence under run5-hw/c<N>/. Push all
   three.
6. Lab schedule. Single lane, in Entry 0. Hours 0:30 to about 2:00: C1's
   harness changes need no lab; C3 holds jetson-nano-2 for #1207 capture.
   Then C2's Jetson baseline (R2, R3) as soon as C1's filter lands, then C2's
   Pi 5 baseline. Fix-head lab work last. Nobody calls labctl without holding
   the lab.
7. Critic model. Fable for critics; builders and the orchestrator on the
   session's model. If Fable is unavailable or a critic hits a usage limit,
   re-run that critic on the session's model and record it.
8. Milestone. Backlog unless the user names another.

Definition of done: Entry 0 pushed with all eight results and the base head in
bold; the user told they can leave.

## Components

Three components, one builder and one blind critic each. C1 is small and lands
first, because C2 and C3 both need it. C2 has a design checkpoint with its
critic before it writes the fix, so a design that deadlocks is caught on paper
rather than after a day of runs, as in run 4.

| Component | Issues | Owns | Lab |
|---|---|---|---|
| C1 Harness: test filter and repeat; serialized fault dumps | none (infrastructure) | kernel/tests/ runner and registration, the exception and panic print path | none; QEMU only |
| C2 The switch-in window | #1234, #1238 | kernel/sched/, kernel/arch/arm64/ context switch and entry, the task lifecycle, R3 | both boards |
| C3 The PMM halt | #1207, #1228 | kernel/mm/ allocator, kernel/tests/test_pmm* | jetson-nano-2, first slot |

### C1: harness changes (hours 0:30 to about 2:00)

1. Test filter and repeat. The test kernel takes a filter and a repeat count,
   from the kernel command line or a boot argument the kexec and UEFI-direct
   paths can both pass, and from the shell: run only the named tests (exact
   names, comma-separated), N times, printing one line per iteration with its
   result and elapsed time, then the summary. Default behaviour (no filter) is
   unchanged byte for byte in the summary format. Document it in
   docs/testing.md.
2. Serialized fault dumps. Dumps from two CPUs interleave on the UART (run 4b,
   boots 18 and 19 unreadable). Make the exception dump path take a dedicated
   dump lock with a bounded wait (a CPU that cannot get it within, say, 50 ms
   dumps anyway, prefixed with its CPU id on every line), so a faulted holder
   can never block another CPU's dump. Every dump line carries its CPU id.
3. Both changes: make test all /ci configurations; one QEMU run showing the
   filter (test_preempt_flag_follows_migrated_task x50) and one showing two
   forced concurrent faults producing two readable dumps. Draft PR. C2 and C3
   build on this branch until it merges.

Definition of done: draft PR with both changes and their demonstrations; /ci
green; the critic ran the filter and the forced double fault itself.

### C2: the switch-in window

1. Baselines (before any fix). On the base plus C1's branch:
   - R1: the window patch from run4-tools/critic-c1-runs/, ported, as a test
     behind a build option. QEMU thread=multi, 20 runs, rate recorded.
   - R2: test_preempt_flag_follows_migrated_task via C1's filter, x200 per
     boot, 3 boots on jetson-nano-2 kexec, 3 on UEFI-direct, 3 on pi-5-1.
     Iterations to first fault and faults per 1000 iterations, per cell.
   - R3: write the migration stress test (READY tasks bounced between CPUs
     while their previous CPU switches out; assert never RUNNING on two CPUs
     and never restored before its save completes; check via a per-task
     generation stamped at save and verified at restore). QEMU 20 runs; Jetson
     kexec 3 boots x200; Pi 5 3 boots x200.
   - Record every rate in one table with files. A cell that never failed is
     listed as such and cannot be used later to claim a fix.
2. Trace (if R2 or R3 faults are not yet explained). Reuse run 4's SLOT_TRACE
   ring; add migrate enqueue and dequeue with the task's on_cpu at that
   moment. Capture 3 first faults on Jetson. Write the sequence from the
   traces. If it is not the #1234 window, say so; #1238 then splits from
   #1234.
3. Design checkpoint, before code. Write docs/adr/0008-switch-in-window.md
   (proposed): the sequence being closed, the chosen design, the lock graph
   (every lock taken on the switch-out path, in the tick, in migrate, in steal,
   and the order), the ordering argument against the Arm ARM (which store
   publishes the save, which load acquires it), and why it cannot deadlock with
   a tick or IPI anywhere in the window. Candidate shapes, for orientation
   only: (a) migrate and steal refuse a task whose on_cpu is set and leave it
   for its own CPU to place in sched_finish_switch (a deferred-migration
   flag); (b) the waiter spins on on_cpu with no runqueue lock held and
   re-validates after taking it; (c) the outgoing CPU keeps IRQs masked from
   the context save through the on_cpu release. The C2 critic reviews the ADR
   blind against the code and must agree there is no lock cycle before C2
   writes the fix. A disagreement goes back to C2, not to code.
4. Fix and prove. Implement the agreed design. Run R1, R2, R3 at the fix head
   under the 5x rule on every cell that failed at baseline, and R2 and R3 on
   Pi 5 regardless. Run the no-deadlock test: R1 with a tick forced into the
   window, on 1ead6ef6 rebased onto the base (must hang or fault) and at the
   fix head (must complete), 20 runs each.
5. Regression rows at the fix head: /ci; make test WORK_STEALING=ON x20
   against C0's baseline; boot_test --count 10 Pi 5 and both Jetson paths,
   full captures; --assert-shares 20 of 20 Jetson kexec; bench smp
   context-switch time on both boards against the base, with files. Also the
   full integration suite 5 boots on each Jetson path and on Pi 5, every
   failure with an issue number.
6. ADR 0008 status to "accepted" if the critic passes; update kernel/CLAUDE.md
   and docs/scheduler.md with the invariant the fix establishes, in one
   paragraph each.

Definition of done: baseline table; traces if needed; ADR 0008 agreed by the
critic before the fix; the fix; R1 to R3 at 0 under the 5x rule; the
no-deadlock test failing on the reverted design and passing at the fix head;
all regression rows; draft PR closing #1234 and #1238 (or #1234 only, with
#1238 split, if the trace says so); the critic reproduced R2 and R3 on Jetson
(one boot x200 each, both heads) and R1 in QEMU.

### C3: the PMM halt (jetson-nano-2, first slot)

1. Reproduce with the filter. On the base plus C1's branch, run
   test_checkerboard_fragmentation,test_exhaustion_recovery x100 per boot, 3
   boots kexec, 3 UEFI-direct. If the halt never appears in isolation, run the
   suite tests that precede it in order (the run-4b boots halted after
   test_checkerboard_fragmentation, following the scheduler tests) and say
   which predecessor is required: that is itself a finding.
2. Capture. At the fault: ESR, FAR, ELR, x0 to x30 symbolized, the faulting PMM
   structure, and which pages were last freed and by whom (add a small
   free/alloc ring behind a build option if needed). FAR 0x1000008096add7 has
   a high tag-like bit set; check whether it is a corrupted free-list link and
   what wrote it.
3. Decide and fix. Allocator defect, a test that corrupts the free list, or
   memory freed while still in use elsewhere (for example a task stack or DMA
   buffer; run 4's sequence C freed a running stack, which #1237 fixed, but the
   halt survived that fix). Fix with a test that fails on the old code. If the
   cause is in C2's files, hand it to C2 through the orchestrator.
4. Prove. The filtered loop at 0 under the 5x rule on every path that failed;
   the full suite 5 boots kexec.

Definition of done: a first-fault capture with the writer identified, or "open"
with captures and the next experiment; if fixed, a draft PR closing #1207 (and
#1228 if linked) with the failing-on-old-code test; the critic reproduced the
filtered loop on both heads.

## Pull requests, merge order, dissent

Each component opens its own draft PR when its critic passes it. No
cross-component gate.

| PR | Opens when | Based on | Required at its head |
|---|---|---|---|
| C1 harness | C1 critic passes | base | /ci |
| C3 PMM fix | C3 critic passes | C1's branch | /ci; boot_test --count 10 both Jetson paths, full captures; the filtered loop and 5 suite boots |
| C2 window fix | C2 critic passes the ADR and the fix | C1's branch | every row in C2 step 5 |

Merge order for the user: C1, then C3 and C2 in either order (rebasing the
second onto the first and re-running /ci; if C2 is second, its R2 Jetson cell
is re-run once at the rebased head).

The kernel used for every row must be the PR head being claimed, never an
earlier head: run 4b merged #1237 before its Jetson test-kernel suite ran and
found #1238 afterwards. Every PR description lists, per board and path, which
rows ran on the production kernel and which on the test kernel.

Dissent. If the orchestrator or a critic believes the rows support a different
outcome than the bars produced, the final report opens with a Dissent section,
one paragraph per point: what the bar said, what the rows say, the files, and
what the user would have to decide. Nothing is opened or withheld on a dissent
alone.

## Critic protocol

As in run 4 (critic-4-*.md on gauntlet/lifecycle-4 for the format). Critics on
Fable; each verdict names its model. Blindness enforced: the critic gets the
definition of done, the issue numbers, the evidence paths and the heads, after
the builder's final push; builder log entries are written after the critic
files.

C2 has two verdicts. The first is on ADR 0008, before code: the critic derives
the lock graph and the ordering requirement from the code and the ADR's
description of the sequence, before reading the ADR's own lock graph and
argument, then compares. Pass means no lock cycle with a tick or IPI anywhere
in the window, and the publishing store and acquiring load named and correct.
The second is on the fix, as usual, and includes checking that the code
implements the design the first verdict passed.

What a pass requires. Every number traced to a file and counted by the critic.
Every capture complete: full serial log from power-on, every test name or every
filtered iteration line, all CPUs in every dump. The bar applied as written,
with its baseline from this run. Reproduction: C1, the filter and the double
fault in QEMU; C2, R1 in QEMU on both heads and R2 and R3 one Jetson boot x200
on both heads; C3, the filtered loop one boot on both heads. Any fix shown
failing on the old code.

Failure modes, checked by name. All rows from runs 1 to 4 stand: selective
summary, truncated or tail-only capture, assembled register file, redefined
criterion, hypothesis stated as finding, stale base, missing critic, partial
per-CPU table, number without a file, uncommitted tree, critic reading the
claim first, undisclosed rule break, banner hash mismatch, bar computed from
one cell, precondition from a prior run, path never exercised. Run 4 and 4b
add:

| Seen in | What it looked like | What the critic does |
|---|---|---|
| Run 4, C1 | A fix claimed "cannot deadlock"; a tick in the window took the waiter's lock | Derive the lock graph from code before reading the claim; force a tick into every window the fix opens |
| Run 4b | A PR merged on production-kernel rows; its new test faulted on the other board's test kernel afterwards | Confirm every board and path ran the test kernel at the PR head, not only production |
| Run 4b | A wrong frame read as a fault site (an add cannot data-abort) | For every ELR cited as a fault site, check the instruction there can raise that exception class |
| Run 4, C1 critic | pkill by name killed another component's QEMU runs | Any process kill outside the agent's own PIDs is a process fail |
| Run 4, C3 critic | A critic claimed a board on its own | Board time only through the orchestrator's handover |

## Rules

Run 4's rules carry over, with these restated or changed.

Lab
- pi-5-1 and jetson-nano-2 only. One holder at a time, handed over by the
  orchestrator; every handover is a log entry. Critics never claim a board
  themselves. No labctl call without holding the lab; read only your own
  labctl output.
- Full serial captures from power-on, banner included, checked at both ends
  when saved and redone if incomplete. No tail-only captures.
- Jetson boots use the host-side trigger loop from run 4b (Entries 39, 43);
  nothing is installed on the board. boot_test output_dir under
  /var/lib/labctl/output/. At most 2 or 3 boots per boot_test call so each
  call stays under the 30-minute MCP client limit.
- /deploy-and-test and scripts/jetson-uefi-deploy.sh may use ssh/scp as
  documented; no other ssh, no labctl database reads, no private boot helpers.
  No BL31 or TF-A flashing (#976). Restore the Jetson ESP to SLMOS.efi
  789cba62 with no BootNext and the Pi 5 card to its original kernels; leave
  both powered on and released.

Processes
- Kill only processes you started, by PID. Never pkill or killall by name.

Code
- Scheduler and lifecycle ordering changes are allowed, at the price of the
  bars, ADR 0008 passing its critic before the fix is written, and the critic's
  independent lock-graph derivation.
- Trace rings behind build options, default off, zero text-size cost when off.
- No merge to main. Draft PRs only. Each component owns its files.
- A number without a filename is written nowhere. Evidence on gauntlet-5-hw-*,
  one file per run or boot, banner hash at the top; QEMU evidence on the Pi 5
  branch under run5-hw/c<N>/. Add -f for .log files (the repo's .gitignore
  excludes them).
- Log switch-5.md on gauntlet/switch-5, append-only, every entry with its own
  write time. Temporary branches deleted at close-out, tips archived, branch
  table in run 4's format.

Stop conditions are per component.
- C2 stops before writing the fix if its ADR does not pass the critic within
  two rounds; it records the disagreement and the rows, and the run continues.
- C2 splits #1238 from #1234 if the traces show a different sequence, files
  what it finds, and continues on #1234.
- C3 hands off a cause in C2's files rather than fixing it.
- The run stops only for: both boards unreachable for more than 45 minutes
  after the round-trip rule; any proposal to flash BL31, patch TF-A or touch
  another board; budget.

Writing, defects, tools. No em-dashes, no second person, numbers with
denominators and files, hypotheses labelled until tested. Defects found along
the way filed with label, milestone and evidence, not fixed in passing. Tool
families as exercised in C0; a builder needing another asks the orchestrator,
and with the user away the step is marked open rather than left on a prompt.

## Budget, stop conditions, deliverables

Budget. 12 hours wall clock. The design checkpoint and the 5x rule on three
reproducers across three board cells are the long poles; the lab lane is about
6 hours.

| Hour | What should be true |
|---|---|
| 0:00 to 0:30 | C0 done; user told they can leave. |
| 0:30 to 2:00 | C1 builds and proves the harness in QEMU. C2 ports R1, writes R3, baselines R1 and R3 in QEMU. C3 holds jetson-nano-2 from about 1:30 on C1's branch. |
| 2:00 to 3:30 | C1 critic. C3 reproduces and captures the PMM halt. C2 traces if needed (QEMU). |
| 3:30 to 5:00 | C2 holds the lab: R2 and R3 baselines on Jetson (kexec, UEFI) and Pi 5. C2 drafts ADR 0008. |
| 5:00 to 6:00 | C2 critic, design verdict. C3 fixes and proves offline as far as it can. |
| 6:00 to 8:00 | C2 implements; QEMU R1 and R3 under the 5x rule; no-deadlock test. C3 back on Jetson for its proof and suite boots. |
| 8:00 to 10:00 | C2 holds the lab: R2 and R3 at the fix head under the 5x rule; regression rows on both boards. |
| 10:00 to 11:00 | C2 critic, fix verdict, with its Jetson reproduction. C3 critic. |
| 11:00 to 12:00 | PRs that passed; close-out; Dissent if any; final report. |

A component not at its mark loses the lab at the next handover and ends with
its last result in the log.

Stop conditions for the run. Both boards unreachable for more than 45 minutes
after the round-trip rule; any proposal to flash BL31, patch TF-A or touch
another board; budget exhausted (whatever has a critic pass ships as a draft
PR, the rest ends open with its evidence).

## Deliverables

1. docs/archive/switch-gauntlet/switch-5.md on gauntlet/switch-5: Entry 0,
   every handover, every critic verdict linked (two for C2), the baseline and
   fix-head rate tables for R1 to R3 on every cell, the traces if taken, the
   branch table, and a close-out with an end state for #1234, #1238, #1207,
   #1228.
2. docs/adr/0008-switch-in-window.md, proposed or accepted.
3. Evidence branches gauntlet-5-hw-pi5 and gauntlet-5-hw-jetson at final heads.
4. Critic files critic-5-C1.md, critic-5-C2-design.md, critic-5-C2.md,
   critic-5-C3.md, each naming its model.
5. Draft PRs per the PR section, or the log's statement of why none.
6. Issue updates with one sentence and filenames each.
7. A one-page final report, in the log and as the orchestrator's last message:
   Dissent first if any; each issue's end state with the deciding numbers; the
   PRs and merge order; bench smp context-switch time before and after the fix
   on both boards; the tracker delta (opened, closed, file-don't-fix); what the
   run could not do; the single most useful next step; the lab state in the
   last line.
