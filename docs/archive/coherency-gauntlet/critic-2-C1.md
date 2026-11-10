<!-- Committed by the orchestrator verbatim from the C1 critic report; scratchpad artifacts copied to run2-critic/c1/. -->

# Critic verdict, C1, coherency run 2 (#1193)

**Status: partial. Covers the code half and the step-1 rows; the hardware half is pending.**
Critic: C1 critic (fresh agent, has not seen the builder's chat or reports).
Sections 1 and 4 to 7, including the code review and the verdict, were
written before the builder's claim was opened (protocol step 3). Sections 2
and 3, and the step-4 addendum in section 6, were added afterwards. The
verdict did not change.

Disclosure: the PR branch's commit messages (4141843a, 30dc24bc, a03fc75b)
were read as part of the code review in step 3. They carry the builder's
reasoning for the bounds. `coherency-2.md`, the PR #1215 body and the
2026-11-10 issue comments were not opened until section 6 was written.

## 1. Critic's own count

Source: `origin/gauntlet-2-hw-pi5` @ c5f0665f,
`docs/archive/coherency-gauntlet/run2-hw/c1/`. 33 files: 10
`nc-instr-test-run{01..10}.txt`, 20 `removal-instr-test-run{01..20}.txt`,
`instr-boots.txt`, `removal-rebase-verify.txt` and
`qemu-removal-force-ee2f9c97-make-test.txt`.

**Banners.** All 10 NC files show `build 20261110065059 (b8e71420)`. All 20
removal files show `build 20261110065507 (375ad4dd)`.
- b8e71420 = origin/main 85f51ffd plus one test-only commit (instrumentation).
- 375ad4dd = f87d9e47 plus the same commit. f87d9e47 was the head of
  `coherency-remove-nc-axis` from 2026-11-09 18:49 until 7d500e0e was pushed at
  23:52 (remote reflog). The removal build is stamped 22:54, so it was the head
  at the time.
- The two instrumentation diffs are byte-identical, so the instrumentation was
  the same on both kernels.

**Completeness.** Every file starts at the kernel banner and contains the
`Test Suite Summary` and `[FAIL] N total failure(s)` lines.
- Each NC file has 98 `[TEST]` suites and 2893 test lines. nc-run03 has 2894:
  two lines are garbled by interleaved UART output, and no test is missing.
- Each removal file has 100 suites and 2926 test lines. The two extra suites
  are `test_coherency.c` and `test_nc_axis.c`, which are on the removal branch.
- Every NC file ends `[INFO] Task 'main' (id=5) exiting`. Every removal file
  cuts that same last line mid-word. The cut comes after the summary, and no
  test output is lost.
- None of the captures starts at power-on. That is acceptable for a test run,
  whose bar is that every test name is printed.

**Failures.** "Failures" means `[FAIL] test_*` lines.
- All 30 files contain the 18 known #1109 failures: 2 shell, 14 blob_autoload,
  1 eviction (plus its sub-check `mlp_finish_nan_never_wins`) and 1
  `test_timer_irq_is_physical`.
- NC: 18 in 10/10 files. No other failure.
- Removal: 18 in 16/20 files and 19 in 4/20 files (run01, run03, run05,
  run16). The extra failures:
  - `test_slm_stats_live_from_second_session` (#1202): run03 and run16, 2/20.
  - `test_task_lifecycle` "Memory leak detected"
    (`test_integration.c:576` in the f87d9e47 tree): run01 and run05, 2/20.
    This is beyond the 18 known failures; it is #1211.
  - `test_component_cleanup_ctx_is_per_task` (#1205): 0/20 removal, 0/10 NC.

**Instrumentation lines, #1202** (`[C1-1202]`; one per file, 30/30 present):

| | NC (10 files) | removal (20 files) |
|---|---|---|
| result at 100,000 yields | 10/10 PASS | 18/20 PASS, 2/20 FAIL (run03, run16) |
| waiter CPU | 0->0 in 10/10 | 0->0 in 20/20 |
| helper CPU entry..done | 0..1 (4), 0..2 (1), 0..3 (5) | 0..2 (19), 0..1 (1: run16) |
| yields used | 0 (runs 01, 06, 07: helper done before the loop), 2193 to 4106 | 16,471 to 88,029 on a pass; 100,000 on a fail |
| loop wall time | 0 to 9459 us | 1210 to 7996 us on a pass; 9312 and 9383 us on a fail |
| ns per yield in loop | 2303 to 2304 (7 runs with yields) | 72 to 96 |
| ns per bare yield (1000 timed after) | 2300 to 2306 | 71 to 127 |
| helper done, us after dispatch | 11,336 to 26,492 | 5501 to 12,283 on a pass; 15,152 and 14,968 on a fail |

**Per-yield cost, critic's computation.**
- NC: 2.30 us per yield. 100,000 yields = 230.0 to 230.6 ms (bare and in-loop
  agree to within 0.3 %).
- Removal: 0.071 to 0.127 us bare and 0.072 to 0.096 us in the loop.
  100,000 yields = 7.2 to 9.6 ms, measured 9.31 and 9.38 ms in the two
  failures.
- Ratio: 18x to 32x.

**The two #1202 failure rows** (verbatim, removal-run03 and run16):

```
[C1-1202] live FAIL waiter_cpu=0->0 helper_cpu=0..2 assigned=2 state=0 switches=2003 yields=100000/100000 loop_us=9312 dispatch_to_end_us=13603 ns_per_yield=93 bare_ns_per_yield=71
[C1-1202] live stages_us(from dispatch; loop at 4290, expiry at 13603) entry=56@0 saw=60@0 stats=83@0 y2000=4278@0 stop=4281@0 follow=15152@2 done=15152@2
[C1-1202] live FAIL repoll 1ms=0 10ms=1 100ms=1 first_true_after_expiry_us=1549 stats_rc=0 follow_rc=0 first=1 second=2000 running=1 saw_header=1
[C1-1202] live FAIL waiter_cpu=0->0 helper_cpu=0..1 assigned=2 state=0 switches=2003 yields=100000/100000 loop_us=9383 dispatch_to_end_us=13674 ns_per_yield=93 bare_ns_per_yield=72
[C1-1202] live stages_us(from dispatch; loop at 4291, expiry at 13674) entry=56@0 saw=60@0 stats=82@0 y2000=4279@0 stop=4282@0 follow=14968@1 done=14968@1
[C1-1202] live FAIL repoll 1ms=0 10ms=1 100ms=1 first_true_after_expiry_us=1294 stats_rc=0 follow_rc=0 first=1 second=2000 running=1 saw_header=1
```

What the rows show:
- In both failures the helper was still in `slm stats 0 --follow 1` when the
  bound expired; it had moved to CPU 2 and CPU 1. It set the flag 1549 and
  1294 us after expiry.
- The waiter on CPU 0 first saw the flag 1549 and 1294 us after expiry, which
  equals the helper's own done stamp minus the expiry stamp, to the
  microsecond. A store made on CPU 1/2 was seen by CPU 0 within one poll.
- The flag was false at +1 ms and true at +10 ms and +100 ms.
- The follow stage (stop to follow) lasted 10,871 and 10,686 us in the
  failures, against 1209 to 8006 us in the removal passes and 4 to 9474 us
  under NC. The NC bound (230 ms) covered every NC case with at least 24x
  margin. The removal bound (about 9.3 ms) is shorter than the helper's
  slowest observed path.

**Instrumentation lines, #1205** (`[C1-1205]`; 30/30 present):
- 30/30 PASS at the original 1000-yield bound, on both kernels. Every waiter
  was on CPU 0, and the loop spent 2 or 3 yields.
- Loop wall time was 3478 to 10,600 us on both kernels, in steps of about
  3.5 ms (3.48, 7.04, 10.6 ms).
- Per-yield average in the loop: 1.16 to 5.30 ms on both kernels.
- Bare yield cost right after the loop, in the same test: NC 2298 to 2302 ns;
  removal 70 to 74 ns.
- The stolen slots (CPUs 1 to 3) were cleaned up at about 1.5 ms. The slot that
  stayed on CPU 0 was cleaned up at the loop's end, 3.48 to 10.6 ms.
- No failure means no repoll row, so step 1 produced no observation either for
  or against the timing reading of #1205.

## 2. The builder's claim, quoted

Read after section 6 was written. Sources: `coherency-2.md` on
`origin/gauntlet/coherency-2` (Entries 7 and 12), the PR #1215 body, and the
2026-11-10 comments on #1202 and #1205.

Entry 7, quoted:
- "`test_slm_stats_live_from_second_session` (#1202): NC 0/10, removal 2/20
  (run03, run16). ... first true 1549 and 1294 us after expiry. At expiry the
  helper was READY on CPU 2 in the `--follow` stage (10871 and 10686 us ...);
  the waiter's 100,000 yields lasted 9312 and 9383 us. Builder verdict:
  timing bound, not lost visibility."
- "`test_component_cleanup_ctx_is_per_task` (#1205): NC 0/10, removal 0/20.
  Completion in 2 to 3 yields on both kernels, 3.48 to 10.6 ms after
  dispatch. Cause of the run-1 failure not established."
- "Yield cost: NC bare 2298 to 2306 ns (20 samples), removal 70 to 127 ns
  (40 samples)."
- "`test_task_lifecycle` ... removal 2/20 (run01, run05), NC 0/10, run-1
  captures 0/34."

Entry 12, quoted:
- "draft PR #1215 (`c1/test-wait-helper` @ 30dc24bc, based on main 85f51ffd)"
- "22 waits converted in 7 files; the rest left with a reason each (sweep
  table in the PR body)"
- "`make test` ARM64 and x86-64 passed"
- "Trees outside kernel/tests and CMakeLists.txt match for 40 of 40 commits;
  range-diff without kernel/tests 35 of 35 "="; one kernel/tests conflict
  (fa17ba52, rapid-exit wait; `smp_mb()` kept)"
- "QEMU: 19 of 20 iterations hit the bound, all cleanups fired 0.2 to 8.7 ms
  after expiry, 0 never" (the forced variant)

PR #1215 body, quoted:
- "100,000 yields lasted 230 ms with the NC axis ... and about 9.3 ms without it"
- "250 ms is the measured 230 ms, rounded up"
- "So 1,000 yields lasted at least 1.16 s ... 2 s is 1.16 s, rounded up"
- "Converted (22 call sites in 7 files)", plus a 20-row "Left alone (reason each)" table.

Issues: #1202 and #1205 have no 2026-11-10 comment yet. The latest #1202
comment is from 2026-11-09 (run 1), and #1205 has no comments.

## 3. Agree or disagree, per number

| Claim | Critic | Source |
|---|---|---|
| Banners b8e71420 10/10, 375ad4dd 20/20 | Agree | section 1 |
| #1202 NC 0/10, removal 2/20 (run03, run16) | Agree | `c1/removal-instr-test-run03.txt`, `run16.txt` |
| First true 1549 and 1294 us after expiry; false at 1 ms, true at 10 and 100 ms | Agree | same |
| Follow stage 10871 and 10686 us; loop 9312 and 9383 us | Agree | same |
| Helper "READY on CPU 2" at expiry | Agree for state (state=0) and for `assigned=2` in both. In run16 the helper's done stage ran on CPU 1, so "on CPU 2" holds only at the moment of expiry. | run16 stages line |
| "Builder verdict: timing bound, not lost visibility" | Disagree with the wording at this stage. Clause 1 of the brief's bar is met. Clause 2 (wall-clock 20/20 on both kernels) has not run, so this is a supported hypothesis, not yet a finding. It must not go into #1202 as the cause before step 6. | brief bar |
| #1205 NC 0/10, removal 0/20; 2 to 3 yields; 3.48 to 10.6 ms | Agree (loop 3478 to 10,600 us) | section 1 |
| Bare yield NC 2298 to 2306 ns (20), removal 70 to 127 ns (40) | Agree | section 1 |
| `test_task_lifecycle` removal 2/20 (run01, run05), NC 0/10 | Agree. "run-1 0/34" was not checked. | section 1 |
| 100,000 NC yields = 230 ms, so 250 ms | Agree | section 1 |
| 1,000 NC yields in the c4 loop "at least 1.16 s", so 2 s | Disagree that this was measured. It is 1000 times the average of 2 to 3 yields, and the same test's 1000 bare yields took 2.3 ms. The 2 s bound stands on the orchestrator's ruling, not on this measurement. | code review, Bounds |
| PR head 30dc24bc | Stale. The head is a03fc75b (adds docs/testing.md, allowed by the ruling in Entry 12). The PR body's "Not in this PR: docs/testing.md" is now wrong. | `gh pr view 1215` |
| "22 call sites in 7 files" | 22 agrees. Disagree with 7: the conversions are in 6 files (test_slm_shell, test_component, test_scheduler, test_syscall, test_lua, test_integration); test_wait.{c,h} are new. | PR diff |
| `make test` ARM64 and x86-64 passed | Agree, reproduced at a03fc75b | section 4 |
| Rebase: outside tests equal, one tests conflict (rapid-exit `smp_mb()`) | Agree | section 4 |
| `c1/boot-path-plus-helper` = 63eed0c4 | Agree (`git ls-remote`) | |
| Forced variant QEMU 19/20 hit the bound, 0 never | Not checked in this half | `c1/qemu-removal-force-ee2f9c97-make-test.txt` |
| Sweep "the rest left with a reason each" | Mostly agree. The PR table covers every site in the critic's sweep except four. test_coop_preempt.c:147 and test_net.c:2186 are not listed, though the critic's view is that both can be left. The sleep-count loops test_scheduler.c:3578, test_slm_runner.c:698 and test_pi_mutex.c:241 are not listed either (same category as the listed :1680/:1714). The PR table also lists test_scheduler.c:3593 (`repos_read_queue`), which the critic's sweep missed; the critic disagrees with its reason (below). | section 7 |

**test_scheduler.c:3593 (`repos_read_queue`), disagreement.**
- The loop re-reads CPU 1's run-queue head up to 100,000 times until the head
  is not the spinner ("A tick on CPU 1 can briefly put the spinner at the head
  of its queue"), then returns the last read, which the test asserts on. That
  is a wait for another CPU to finish a tick, bounded by a bare loop count.
- It is also the site whose wall time the removal changes most. Under NC,
  `sched_cpu_rq()` points at non-cacheable memory, so each iteration costs
  several uncached reads. Under the removal they are cached reads, so the same
  count lasts one to two orders of magnitude less. This is a critic estimate,
  not measured.
- The PR's reason ("retries a racy snapshot ... not a wait for progress")
  does not cover the case the comment describes.
- Recommendation: convert it to a CNTPCT bound, or measure its duration on
  both kernels and record the number. Not a fail on its own, because it has
  not failed in any capture: 0 `test_reposition_moves_task_queued_on_other_cpu`
  failures in 30/30 step-1 files.

## 4. Reproduction (code half)

**QEMU, PR head a03fc75b.** The prompt names 30dc24bc as the PR head; the
branch moved to a03fc75b at 01:16. The only difference is `docs/testing.md`
(+26 lines), so the code is the same.
- `make test` (ARM64): `PASSED - All tests passed`, banner `(a03fc75b)`,
  `[PASS] All test suites passed!`, 0 `[FAIL]` lines. Log:
  `scratchpad/c1-critic/qemu-arm64-a03fc75b.log`.
  - helper lines: `slm_live: flag after 5404 us (bound 250000 us)`,
    `c4_multi: cleanups after 310 us (bound 2000000 us)`,
    `task_lifecycle: ... met after 24 us (bound 1000000 us)`.
- `make test PLATFORM=X86_64`: `PASSED - All tests passed (isa-debug-exit code 1
  = success)`, banner `(a03fc75b)`, 0 `[FAIL]` lines. The converted tests are
  not in the x86 suite set, so no `[test_wait]` lines appear. Log:
  `scratchpad/c1-critic/qemu-x86-a03fc75b.log`.
- No compiler warnings from `kernel/tests/` in either build. The 174 ARM64
  warnings are all in the vendored Lua.

**Removal rebase.** e94c251a against the previous head 7d500e0e:
- `git diff 7d500e0e e94c251a -- . ':(exclude)kernel/tests' ':(exclude)CMakeLists.txt'`
  is empty.
- The PR commits were rebased in as 19d1787c and 63eed0c4, directly on 167330f5
  (C0's base) and beneath the removal stack. `git patch-id --stable` is
  identical for 4141843a = 19d1787c (cb16c511) and 30dc24bc = 63eed0c4
  (4d3c0ac0).
- Inside kernel/tests and CMakeLists.txt, the 7d500e0e..e94c251a delta equals
  the PR diff except for one conflict resolution. `rapid_exit_done()` uses
  `smp_mb()` where the PR has `cache_invalidate(&t->state)`, which matches what
  7d500e0e had already done to the old loop.
- `git range-diff 167330f5..7d500e0e 63eed0c4..e94c251a`: 32 of 35 commits are
  `=`. The three `!` commits (564f30f0, 7d7c2edf, b2204e6b) differ only in
  test-file context, the rapid-exit resolution and the `cache.h` include.
- a03fc75b (docs only) is not under the removal branch; it was pushed after the
  rebase. Code-equivalent.
- **Pass.**

**Hardware re-run of at least 5 of the 20:** pending; the board has not been granted.

## Code review of the PR (origin/c1/test-wait-helper)

**Helper (`kernel/tests/test_wait.{h,c}`).**
- CNTPCT use is correct.
  - The elapsed test is `now - start >= limit`, an unsigned difference, which
    is safe across wrap.
  - `ticks_for_us` rounds up, so a bound is never short.
  - `us * f` overflows only beyond about 4600 s at a 4 GHz counter (beyond
    3.4e5 s at Pi 5's 54 MHz). The largest bound used is 5 s.
  - `test_wait_us_between` returns 0 for `b < a`, or when f = 0 (then
    limit = 0 and the wait times out at once, which is safe).
- The condition is evaluated before the deadline check on every pass, so the
  condition is always re-checked after the last yield before a timeout is
  declared. A condition that became true during the final yield counts as met.
- Return value: elapsed us on success, -1 on timeout. Every caller tests `>= 0`.
- On a timeout the helper keeps yielding for 1 s, records the condition (and
  an optional value) at +1, +10, +100 and +1000 ms and the first time it held,
  then logs one line. This meets the brief's 1/10/100 ms repoll and adds
  +1000 ms.
- A pass that used more than a quarter of the bound is logged. #1202, #1205
  and `test_task_lifecycle` log their elapsed time on every run, which is
  useful for step 6.
- Acquire semantics: `cond_flag`, `cond_int_at_least`, `cond_u32_nonzero` and
  `cond_task_terminated` use `__atomic_load_n(..., __ATOMIC_ACQUIRE)`.
- Finding (minor, not blocking): acquire is only half of message passing.
  - The writers of `live_done` and `second_done` (`test_slm_shell.c`) are plain
    volatile stores, not release stores.
  - So the helper's documented "load-acquire" does not by itself order the
    payload (`live_stats_rc`, `live_second`, ...) that the tests assert after
    the flag.
  - #1205's writer uses `__atomic_fetch_add(..., __ATOMIC_RELEASE)`, which is
    correct.
  - Suggested fix: make the converted writers `__atomic_store_n(..., RELEASE)`,
    or note the requirement in `test_wait.h`.
  - This was the same in the old code, so it is not a regression.

**Conversions.** #1202 and #1205, plus the sweep sites in section 5. Each
keeps the original post-wait assertions. Details:
- `test_kernel_pan_not_armed` and `daif_probe_on` discard the helper's return
  and assert the flag afterwards. That is equivalent.
- `daif_probe_on` used to share one 200,000-yield budget across its two waits;
  it now has 1 s each. That is fine.
- `drain_lua_states_until_task_done` now records a failing `msg_drain` and
  stops, instead of asserting inside the loop. The assertion is kept
  (`TEST_ASSERT_EQUAL_INT(0, c.drain_rc)`).
- `sched_all_terminated` runs `test_delay()` before each check, rather than
  after the yield as before. That is harmless.

**Bounds.**
- 250 ms (#1202, and the sibling `test_slm_stop_from_second_session_during_prompt`)
  is **supported by measurement**. NC 100,000 yields = 230.0 to 230.6 ms
  (in-loop 2303 to 2304 ns over 7 runs; bare 2300 to 2306 ns over 10 runs),
  rounded up. The slowest helper completion observed on either kernel was
  10.9 ms after loop start (removal) and 26.5 ms after dispatch (NC), both
  well inside 250 ms.
- 2 s (#1205) is **not a measurement of what 1,000 NC yields took**. The
  1.16 s figure is the average over the 2 or 3 yields that the loop actually
  spent before the cleanups landed, multiplied by 1000. Those yields were slow
  because the cleanups were still pending on CPU 0. In the same test, 1000
  yields timed right after the loop took 2.3 ms under NC. What 1,000 NC yields
  take in that loop with pending work was not measured; a yield that finds
  work costs milliseconds, and one that finds none costs 2.3 us. The measured
  data supports these statements:
  - (a) Cleanups finished within 10.6 ms in 30/30 boots on both kernels.
  - (b) 2 s is a safe ceiling: it will not turn an NC pass into a failure.
  - (c) The per-run elapsed log makes a delayed-visibility case visible below
    2 s.
  The 2 s bound stands as the orchestrator's ruling. Whether it is the "NC
  equivalent" is not established by step 1. The pending forced variant
  (1000-yield bound, tasks pinned to CPUs 1 to 3, repoll after expiry) is the
  experiment that measures the visibility question for #1205.
- The sweep bounds (1 s "floor", 2 s for the FFI waits, 5 s for the Lua drain)
  come from the PR's own rule in `test_wait.h`, not from the brief. The brief
  fixes a bound only for the two tests, so this is not a redefinition. The FFI
  2 s is 500,000 x 2.3 us = 1.15 s rounded up, which is arithmetic, not a
  measurement of that site.

## 5. Failure-mode table

| Row | Checked how | Result |
|---|---|---|
| Selective summary | Counted 30 capture files (10 NC, 20 removal) and every `[FAIL] test_*` line in each; compared with the 30 rows of `instr-boots.txt` (read last). | Pass. 30 rows for 30 files. The 2 #1202 failures and the 2 `test_task_lifecycle` failures appear in both the files and the table. No boot was left out. |
| Truncated capture | Suite and test-line counts per file; checked the end of every file. | Pass for a test run: every test name is present (2893/2894 NC, 2926 removal; 98/100 suites), and every file ends at or after the summary. Removal files cut the post-summary `exiting` line mid-word, so no test output is lost. The captures start at the banner, not at power-on. |
| Assembled register file | No register reads in C1. The instrumentation timestamps are per boot, relative to that boot's dispatch, inside one capture file with its banner. | N/A / pass. |
| Redefined criterion | Compared the bounds with the brief's "NC-equivalent duration ... measured, rounded up". | 250 ms meets it. 2 s does not come from a measurement of 1000 NC yields (it is an extrapolation from 2 to 3 yields, and the same test's 1000 bare yields took 2.3 ms). Recorded; it stands as the orchestrator's ruling, so no fail on this row now. The PR's 1 s floor for sweep sites is a builder rule; the brief sets no bound there. |
| Hypothesis stated as finding | For #1202, the separating experiment named by the brief is: condition observed after expiry, plus a wall-clock variant passing 20/20 on both kernels. | Clause 1 is met: the flag was observed true at +1.29 and +1.55 ms, with the waiter seeing it within a microsecond of the helper's own done stamp. Clause 2 (20+20 at 250 ms) has not run yet. So "timing artifact" is supported but not established. For #1205, step 1 has no failure and no observation; any causal claim is a hypothesis until the forced variant and step 6 run. For #1211, the conversion to a 1 s page wait was made with no step-1-style observation that the pages came back late. Its per-run log will supply that observation; until then the cause is undetermined. |
| Stale base | Matched the banners to commits and the remote reflog. | The step-1 evidence is correctly attributed to b8e71420 (main + instrumentation) and 375ad4dd (f87d9e47 + instrumentation, the removal head when it was built). f87d9e47 is not the current removal base: 7d500e0e/e94c251a sit on C0's 167330f5 and differ from f87d9e47 in 18 files (boot.S, mmu.S, vmm.c, ncmem, ...). Step-1 numbers must not be attributed to e94c251a. The step-6 20+20 series must show banners from the PR head (NC) and from e94c251a or its descendant (removal). |

## 6. Verdict (this half)

**Pass, partial: code half and step-1 rows.**

Definition-of-done rows that pass in this half:
- **Step-1 failure rows exist and are honest.** 30/30 files are complete and
  counted. The failures were found independently: #1202 2/20 removal and 0/10
  NC; #1205 0/30; #1211 2/20 removal and 0/10 NC.
- **The standalone draft PR exists, branched from main.** PR #1215 is a draft
  against `main` at 85f51ffd. It contains the helper and the two conversions,
  builds, and passes `make test` on ARM64 and x86-64 at a03fc75b.
- **The removal branch is rebased onto the PR commits.** Outside kernel/tests
  and CMakeLists.txt it equals 7d500e0e, and the PR patches are patch-id equal.

Open in this half, not fails:
- **Sweep completeness.** The critic's independent sweep (section 7) found
  sites that the PR commit message neither converts nor names. The brief
  requires that each site left alone has a recorded reason. Whether the PR
  body or the log records them is checked in step 4 below.
  - Step-4 addendum: the PR body's table records almost all of them. The
    exceptions are listed in section 3; the substantive one is
    test_scheduler.c:3593.
- **2 s bound.** It is not measured as NC-equivalent; it stands on the ruling.
- **Minor.** The release-store gap on the #1202 writers (code review above).

Pending (the hardware half):
- 20/20 for both tests under both kernels, with banners matching the PR head
  and e94c251a.
- A critic re-run of at least 5 of the 20 on pi-5-1.
- The forced #1205 variant.
- Issue updates on #1202 and #1205 (checked in step 4).

## 7. Critic's independent sweep of origin/main kernel/tests

This is a census of loops that wait on another task and are bounded by a yield
count or a bare loop count, done before the PR's list was opened. Line numbers
are on origin/main 85f51ffd. The PR column reflects 30dc24bc/a03fc75b.

**Converted by the PR (critic agrees each is a count-bounded wait):**

| Site | Old bound | PR |
|---|---|---|
| test_slm_shell.c:889 (#1202) | 100,000 yields | 250 ms |
| test_slm_shell.c:680 (sibling, `second_done`) | 100,000 yields | 250 ms |
| test_component.c:470 (#1205) | 1000 yields | 2 s |
| test_component.c:406 (`c4_observed_value`) | 500 yields | 1 s |
| test_component.c:417 (4 yields, then free-page assert) | 4 yields | 1 s wait for the page |
| test_syscall.c:749 (PAN check per CPU, cross-CPU) | 100,000 yields | 1 s |
| test_scheduler.c:1029, 1072, 1109 (FFI tasks) | 500,000 yields | 2 s |
| test_scheduler.c:1216, 1300, 1383, 1869, 2004, 2349 | 100/200/100/300/300/500 x (yield + test_delay(10000)) | 1 s |
| test_scheduler.c:2089 (rapid exit) | 200 yields | 1 s |
| test_scheduler.c:3623 (repos spinner, CPU 1) | 200,000 yields | 1 s |
| test_scheduler.c:5622 and 5632 (DAIF probe) | 200,000 shared | 1 s each |
| test_scheduler.c:5723 (pin_test cleanup) | 100,000 yields | 1 s |
| test_lua.c:2244 (publisher drain) | 20,000 iterations | 5 s |
| test_integration.c:758 (`test_task_lifecycle`, #1211) | 50 x (yield + delay(50000)) | 1 s |

**Left as a count with a reason in the PR commit message:**
- test_scheduler.c:2185 and 2231 (`test_isolated_core_latency`): #1213. The
  test asserts nothing about completion. The critic accepts the reason; #1213
  tracks the CPU-1 behaviour it hides.

**Found by the critic, not converted and not named in the PR commit message.**
Each needs either a recorded reason or a conversion. The critic's view on each:

| Site | What it is | Critic's view |
|---|---|---|
| test_slm_shell.c:859 | Inside the #1202 helper: 2000 yields, then `live_second > live_first` asserted at :896. A yield-count window for the prompt to make progress. | Count-coupled by design: each yield hands CPU 0 to the prompt task (`second=2000` in both failures), so a faster yield does not shorten it relative to the work. Leave, with that reason recorded. |
| test_scheduler.c:2099 | One yield "to clean up zombie". | Not followed by an assertion. Leave. |
| test_eviction.c:1696 | 16 yields between policy swaps. | Pacing, not a wait; no assertion depends on it. Leave. |
| test_model_loader.c:362, 370 | Single yield "give it a tick". | No assertion depends on it. Leave. |
| test_integration.c:1306, 1481, 1647, 1668, 2036, 2057 | `delay(50000)` in publishers "so the subscriber is scheduled first". | The subscription happens in the test body before the tasks start, and the consumers are wall-clock bounded. Leave. |
| test_coop_preempt.c:147 | 1000 bare nops, then asserts CNTPCT advanced. | Single CPU, independent of switch cost, and 1000 iterations is far more than one 54 MHz tick. Leave. |
| test_gpu_tier.c:127 | 1e6 bare increments. | The assertion accepts either outcome. Leave. |
| test_net.c:2186 | 128 synchronous `net_poll()` calls. | Consumption is synchronous. Leave. |
| test_lua.c:1978 | 1000-yield body of a task that is never scheduled. | Not a wait. Leave. |
| test_pi_mutex.c:136, 185 | Fixed yields under the mutex (exercise, not wait). | Leave. |
| Sleep-count loops: test_scheduler.c:1680, 1714, 3578; test_slm_runner.c:698; test_pi_mutex.c:241; test_x86_boot.c:938, 990, 1094, 1182, 1308, 1381, 1454, 1729, 1806, 1917, 1947 | Bounded by a count of `task_sleep_ms`/`sleep_ms` calls, so each iteration takes at least 1 or 10 ms of wall clock. | Out of scope: a wall-clock lower bound, not a yield count. Leave. |
| Unbounded waits: test_integration.c:293 (`while (1)`); test_pi_mutex.c:180, 360, 417, 435, 485; test_msg_router.c:557; test_slm_shell.c:646, 846; test_scheduler.c:1003, 1151 | No bound at all. | Not "bounded by a count", so outside the brief's sweep. They hang rather than fail; worth a note. |
