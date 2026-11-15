# Critic, switch run 5 follow-up session (#1262, #1244)

Fable 5.2, blind, read-only, one agent, two rounds, 2026-11-15. Scope: only
the rows of Entry 41 (Part A and Part B tables and the section "What the
dumps show"), checked against the capture files under
`docs/archive/switch-gauntlet/run5-hw/1262-1244/` on `gauntlet-5-hw-jetson`
(47993ea4) and `gauntlet-5-hw-pi5` (5cff7385). The critic was given the
tables and the evidence paths, not the session's reasoning. This file is the
session's record of the two reports; the measured numbers are the critic's.

## Round 1: VERDICT FAIL

"The fail is narrow: one Part B row misdescribes two boots, and the
'typical dump' sentence contradicts the dump it quotes. Every count,
address, banner and pass total I could measure matches the claims file."

All 53 capture files exist, one per boot, each with exactly one SLM-OS
banner and no `NOT FOUND` header; no two bodies are identical.

| Row | Result | Note |
|---|---|---|
| A1 UEFI x200, 29d58c22, 10 boots | PASS | 200/200 x10, 0 NOT-FINISHED; nothing in the bodies shows SWO_DIAG was active (header only) |
| A2 UEFI x200, a6c65318, boots 11-15 | PASS | 2 to 7 undumped slow-finish lines per boot (16 to 27 ms) |
| A3 kexec x200, a6c65318, 5 boots | PASS | one 30 ms dump, boot 05, iteration 53 |
| A4 kexec x1000, 6a0192d9, 2 boots | PASS | banner `-dirty`, disclosed; 6 and 14 dumps |
| A5 kexec x1000, 1441f690, 2 boots | PASS | row omitted the 13 and 3 slow finishes |
| A6 kexec x1000, ae1399ba | PASS | 14 lock-wait lines, all lock word 0, no owner |
| A7 UEFI x1000, ae1399ba | PASS | 30 dumps, 49 prelude lines, 83 lock-wait lines |
| A8 padded locks, 741497c7, 2 boots | PASS | different kernel by banner; 0 dumps, 0 prelude lines, max 315234 / 315244 us |
| B1 main + check, 665fb18e | PASS | WARN line matches character for character |
| B2 kexec production x10 | PASS | DTB-clear line x10, 0 halts |
| B3 UEFI production | PASS | exact line |
| B4 kexec suite x10 | **FAIL** | boots 06 and 09 did not stop in the named test: the panic is in `pdm_a` / `pdm_b`, that CPU halts, the test reports FAIL, 239 more PASS lines follow, and the capture ends in the coherency litmus |
| B5 Pi 5 production x2 | PASS | exact line |
| B6 QEMU | not checked | no capture exists for the row |

Also from round 1:

- The 2 PMM / 5 #1238 / 3 complete split holds. `pmm_alloc_pages` and the
  test name `test_exhaustion_recovery` are not in the fault dumps (an
  ordering inference and an `addr2line` result the critic did not run).
- `test_coalesce_enables_larger_allocation` and `test_recursive_coalescing`
  fail in all 10 suite boots, and the row did not say so.
- Dumps section: counts correct (71 dumps, 846 per-CPU lines all
  `switch_in_waiting=NULL`, 187 task lines all 255, `pd=1` only on CPUs 4
  and 5). Wrong or overstated: "does not yet hold its lock" (the line
  prints `rq_lock=0x1`); "almost all of that time in the rq_lock wait" (19
  of 140 entries are not); "typical" (about half); an undisclosed
  `age=554189327 us` artefact; 26 lines at phase 1 with the lock word set.
- Inference (a), not the deferral path: **partly supported**. True of the
  dumped stalls; #1262 itself was never captured; single snapshots; no
  pre-fix control.
- Inference (b), exclusive-store starvation on a shared lock line: **partly
  supported**. Strong: 0 slow iterations in 2000 padded against 40 in 5000.
  Weak: the mechanism is inferred, the counter does not split held from
  store-failed, six waits have 2 to 8 failed try-locks, the experiment pads
  two arrays and shifts their neighbours, CPU 0's loop is source reasoning,
  equal durations also fit a cluster-wide stall, two kexec boots only.
- The 2 s statement is labelled a hypothesis; one point against it: every
  measured wait is at most 6.64 ms, including 33 iterations with a slow
  prelude and no dump.
- UEFI-direct captures start at power-on, boot SD Linux, reboot, then start
  SLM-OS: a warm reset inside a power-on capture.

## Round 2, after the text was corrected: VERDICT PASS

"Every row in both tables now matches the captures, and the suite row is
correct boot by boot. One number in the rewritten dumps section is still
wrong (the artefact count: 43 lines, not 38), and a few claims rest on
things outside the captures; none changes a row result."

Re-measured and matching: 846 per-CPU lines; 93 and 94 task lines; 47 and
30 `pd=1` lines; 140 preludes over 1 ms split 121 / 17 / 2 (at a 90%
threshold); range 1061 to 6637 us; 97 own-lock waits, all lock word 0 and
no owner; the six low-count waits; 14,989; 26 lines; the example dump;
"about half" (43 of 82 kexec, 29 of 60 UEFI); iterations over 320 ms 6, 14,
17, 5, 10 against 0 and 0; the extra numbers in rows A2, A5, A6, A7; the
warm-reset disclosure; the suite row boot by boot; the five new tests pass
in boots 01, 02, 03, 05, 06, 08, 09, 10 and are absent from 04 and 07.

Remaining discrepancy: the artefact is on 43 lines (8 read ...327, 35 read
...328), corrected in Entry 41 after this round.

Not checkable from the captures: `pmm_alloc_pages` by `addr2line`; #1109
list membership and the Entry 39 comparisons; the "unsigned wrap"
explanation of the artefact; that main hangs at R3 wake iteration 1; the
QEMU row; that SWO_DIAG was on in the 29d58c22 and 741497c7 images (the
padded result does not depend on it: the zero iterations over 320 ms come
from the test's own timing).

Inferences as rewritten: (1) "now stated no more strongly than the evidence
allows"; (2) "now stated within the evidence", with two residues: "one
variable" is slightly generous for a change that pads two arrays, and "the
lock is at times held during a wait" is one reading of a snapshot.
