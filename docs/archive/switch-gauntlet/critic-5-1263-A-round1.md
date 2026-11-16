VERDICT: FAIL — the measurements are sound and every table number reproduces, but one reported number is contradicted by this session's own capture, the R3 baseline carries an undisclosed confound, and the one new suite failure is described more weakly than the archive allows.

Scope: Part A (#1263, `fix/percpu-cacheline` c0612fa7 + d70f93f0). Stage 1 (blind) is in
`stage1-derivation.md`; this file is Stage 2. One correction to Stage 1 made after it was
saved: lock waits with under 100 failed try-locks number 14 of 279, not 13 (8 of them
under 10).

No code defect found. No capture is cut or mislabeled: all 64 `A-*boot*.txt` bodies are
byte-identical to their labctl output files, and the 42 `a1263-*` output directories hold
no run that was not saved (no unreported retries).

## Findings

1. **HIGH — the new suite failure is understated; the archive points at this change.**
   `test_preempt_flag_follows_migrated_task`, `A-uefi-suite-c0612fa7-boot03.txt:10022-10024`
   (`pdm: migrations=0 moved=0 runs=100/100 bad=0`, assert `pdm_moved > 0`,
   `test_scheduler.c:2369`). The description of the capture is accurate. What is missing:
   - On origin/gauntlet-5-hw-jetson there are 822 archived runs of this test on unpadded
     kernels that have the #1234 fix (c0ea0714, 60c548f3, 6a4cfa93; 26 boots, including
     four x200 loops: `c2/hw-fix/{kexec,uefi}-r2-fix-c0ea0714-boot1.txt`,
     `c2-fix-critic/fix-c0ea0714-boot01.txt`, `closeout/kexec-r2-6a4cfa93-boot01.txt`).
     Minimum `moved` in all of them is 5. `moved=0` never occurs. The PR cites only
     "15 of 15 suite boots" and concludes "not shown to be the old rate"; 0 of 822 against
     1 of 6 is much stronger than that.
   - "1 of 9 suite boots" includes 3 Pi 5 boots. Pi 5 has 4 CPUs, so the builder's own
     reading (task on CPU 4 or 5 is never migrated) cannot occur there. The relevant
     denominators are 1 of 6 Jetson boots, 1 of 3 UEFI-direct boots.
   - The earlier failures of this test (`1262-1244/kexec-suite-661204e6-boot06,09`) are a
     different assert (`w >= 0`) on a kernel without the #1234 fix; they are not this mode.
   - The deciding experiment the log schedules (x200, U against P) has not been run:
     `a/img/jet-pdm-{P,U}.elf` were built at 05:43 and there are no rows.
   Nothing in the capture says which CPU the two tasks ran on, so the cause stays
   unproven; but "placement only, regression rows clean except one unexplained flake" is
   not where the evidence sits. Fix: run the experiment before the PR is opened; give the
   0-of-822 figure and the 1-of-6 denominator; if the reading holds, correct the test's
   mover (handle `assigned_cpu >= 4`) in this PR or file it and link it.

2. **MEDIUM — a reported number is wrong.** `CLAIMS-draft-1262-comment.md` last bullet:
   "its point against (no measured wait above 6.64 ms) stands". This session measured
   6701 us: `A-uefi-r3w-U-ae1399ba-x1000-boot01.txt:3349`
   (`swo-diag lockwait cpu5: 6701 us, 12552 failed try-locks`) and the prelude line at
   :3347 (`cpu5 6701/176(120)/6701/1`). 6.64 ms is Entry 41's maximum (6637 us, which I
   reproduced from its file). The argument is unaffected, the number is not. The same
   stale figures are in the PR body line 9, `docs/scheduler.md`, `spinlock.h` and
   `kernel/CLAUDE.md` ("1.1 to 6.6 ms", "1 to 3%"): this session's rows give 1.03 to
   6.70 ms, a report line in 1.3 % (kexec) and 4.5 % (UEFI-direct) of iterations, and slow
   finishes in 0.8 % and 2.8 %. Fix: write 6.70 ms, or say explicitly the figures are
   Entry 41's.

3. **MEDIUM — undisclosed confound in the unpadded R3 baseline.** In `jet-r3w-U.elf`
   (ae1399ba, SWO_DIAG) `rq_lock` is at 0x80c06d00..1f and `swo_rq_owner` at
   0x80c06d20..3f: the same 64-byte line. It is in the builder's own
   `A-layout-check.txt` (section `jet-r3w-U`) and mentioned nowhere. `swo_rq_owner[]` was
   added by ae1399ba and is stored at every rq-lock take and release by every CPU, so the
   baseline has the diagnostic's stores inside the lock's reservation granule. In the
   unpadded production image `rq_lock` has the line to itself (`jet-prod-U`:
   0x80ac6d00, next symbol 0x80ac6d40). The effect is not created by the instrumentation
   (Entry 41's 1441f690 rows, without the owner array, show slow finishes too), but the
   rates and maxima of this A/B belong to the instrumented layout, and no R3 row exists on
   a kernel without SWO_DIAG. Fix: state both in the PR and the #1262 comment.

4. **MEDIUM — evidence about #1262 left out of the #1262 comment.** All in the unpadded
   R3 files:
   - 14 of 279 lock waits have under 100 failed try-locks and 8 have under 10, for 1.06
     to 4.71 ms (`A-kexec-r3w-U-...-boot02`: 2206 us, 1 failed try-lock;
     `A-uefi-r3w-U-...-boot02`: 4707 us, 5). The issue records "2 to 8, 1.1 to 3.0 ms";
     the range is now wider. The code's own comment reads such a wait as "the waiting CPU
     was not executing", which is not the reservation-loss mechanism.
   - One wait began with lock word 1 (`A-uefi-r3w-U-...-boot03`, cpu5, 1903 us); the issue
     says all began with 0.
   - 4 of the 109 slow finishes (2 kexec, 2 UEFI-direct) have no prelude report at all.
     In the one I opened (`A-kexec-r3w-U-...-boot02`, dump t=181105459) CPUs 4 and 5 sit
     in phase 5 (`task_exit`, inside `scheduler_terminate_task`) for 4.2 ms with every rq
     lock word 0: the stall is at a lock site the prelude counters do not time.
   They all vanish with padding, so they do not weaken the A/B; they do bear on the
   mechanism the comment calls "still inferred". Fix: add them.

5. **LOW-MEDIUM — column label.** "Iterations with a `schedule()` prelude over 1 ms"
   (20, 12, 8 / 37, 60, 37) counts `swo-diag prelude` report lines, which print when a
   prelude OR any rq-lock wait exceeded 1 ms (`sched.c:3548`). Counting preludes over 1 ms
   gives 18, 12, 7 / 37, 59, 36. `A-r3sum.sh.txt` words it the same way. It also
   undercounts stalled iterations (finding 4, third item). Same method as Entry 41, so the
   comparison holds. Fix: relabel as "iterations with a prelude report".

6. **LOW-MEDIUM — how far "padding removes the stalls" goes (question 1).** Supported:
   109 slow finishes in 6000 unpadded against 0 in 6000 padded, strictly alternating boots
   (trigger logs confirm), both Jetson boot paths, 3 boots a side. Scope actually covered:
   jetson-nano-2, SWO_DIAG test kernel, wake phase only. Not excluded: which array (the PR
   and comment say so); finding 3; other layout shifts between the two builds (`task_lock`
   left the line it shared with `current_task[1..7]`; `.text` +1696); the production
   kernel; the migrate phase; nano-1. Locks-only attribution rests on Entry 41's two kexec
   boots. The #1262 comment's sentence "The millisecond stalls on CPUs 4 and 5 go away
   when..." should carry "in this test kernel".

7. **LOW-MEDIUM — audit gaps (no per-CPU array is missing from the table).**
   - `sched.context_switches` (`sched.c:2798`, `:2997`) and `sched.timer_ticks` (`:3088`)
     share a line with `sched.isolated_cores` (read in every `sched_try_steal`, `:2064`,
     and at `:1280`, `:1382`) and `sched.initialized` (`:2959`). The doc and PR call the
     counters "true sharing, not placement"; the read-mostly words beside them are false
     sharing and moving them is placement.
   - Standalone spinlocks were not audited though the granule argument applies to them:
     `task_lock` (`task.c:38`; in `jet-prod-P` at 0x80ab93c0, sharing its line with the
     first 48 bytes of `task_table_fallback[0]`), `sleep_queue_lock` (`task_sleep.c:42`,
     the wake path), `blkdev_lock` (same line as `timer_handler_count`, `timer.c:176`).
   Fix: add to the audit or to #1279.

8. **LOW — regression-test gaps.** `slot_trace_idx[]`, `slot_trace_rq_holder[]`,
   `slot_trace_pd_ticks[]` are padded but in no layout table;
   `test_checker_rejects_a_packed_array` restates the conditions and never calls
   `check_ent`; a new packed per-CPU array is caught only if someone lists it. The test
   does fail if `SPINLOCK_ARRAY` or a cell type is packed again. The assembly stride test
   is ignored in every QEMU run, including the step run with `SECONDARY_PREEMPT=ON`
   (disclosed); it passes 8 of 8 in all 9 hardware suite boots.

9. **LOW — wording.** Pi 5 suite "all on the #1109 list": #1109 says "13 `blob_autoload`",
   the captures have 14 blob names (`test_blob_boot_autoload_skips_tampered_managed_blob`
   is the 14th). Not a regression: the 6a4cfa93 close-out suites show the same 20 Pi 5
   names and the same 22 UEFI-direct names. Entry 44 "every padded array is 512 bytes":
   `cpu_steal_deques` is 3584 and `swo_pc` 1024.

10. **LOW — bench (question 3).** Conclusion and arithmetic are right: Jetson 96.83 →
    98.50 ns (+1.67, Welch t 0.96), Pi 5 107.67 → 107.33 (-0.33, t -0.51); ledger total
    97.3 + 1.7 = 99.0 (+12.5 %), 109.0 - 0.3 = 108.7 (+7.9 %). Caveats to add: the 6 runs
    come from 3 boots; with SE 1.74 ns the Jetson interval is about -2 to +5.5 ns, so a
    cost up to about 5 % is not excluded; the running total adds a delta the row itself
    says is not shown.

## Answers to the specific questions

1. Yes for the stated configuration, see finding 6; limits in findings 3 and 6.
2. Yes. The #1262 comment is worded as evidence, says twice that the 2 s NOT-FINISHED
   was not reproduced, and claims no fix. Defects: findings 2 and 4.
3. Yes, finding 10.
4. Accurately described; underweighted, finding 1.
5. No. The only closing keyword in the PR body is "Closes #1263" (line 1). The two commit
   messages have none.

## Matches (checked against raw captures)

R3 table, all 36 cells; 1000/1000 and no NOT-FINISHED in 12 boots; 109 = 24 + 85;
alternation on both paths; Entry 41's 4/10/811552 and 30/49/827300; all 24 bench run
means, both sds, both deltas, p50/p95 statements; CTR_EL0 0x9444c004 on 6 + 4 CPUs
(DminLine, ERG, CWG = 4 → 64 bytes); production 10/10 on three rows; assert-shares 20
PASS, 0 FAIL; suite totals 14/14/14, 21/21/22, 18/18/18 with 0 halts; `/ci` 12 steps,
runtime tests 308 and 328; section deltas (.text +1696 / +1536, .rodata +512, .bss equal);
18 boot captures in A1. Code: every `SPINLOCK_ARRAY` element is an aligned 64-byte line
(nm on five padded ELFs); the four assembly sites in `resched_trampoline` are the only
assembly indexers and are consistent; no stale extern, sizeof or pointer-arithmetic site;
no lock-order or semantic change.

## Not checkable from the evidence

- The bench-context and assert-shares files are the builder's copies of tool-result
  strings, not labctl output files; nothing independent to compare them with.
- The 9 UEFI-only failure names against #1231 (issue text not supplied); checked instead
  against the 6a4cfa93 close-out rows.
- #1279's content.
- Which CPU the two `pdm` tasks ran on in UEFI suite boot 03.
- Board state as left, ESP hashes, deletion of the builder's images (lab not touched).
- Whether the inner timestamps of the UEFI stage lines (e.g. "09:23:13" logged at
  09:30:54) are the Jetson's unsynced clock; the banner of every UEFI capture is correct.
- That Pi 5 captures start from power-on (the board prints no firmware line).
