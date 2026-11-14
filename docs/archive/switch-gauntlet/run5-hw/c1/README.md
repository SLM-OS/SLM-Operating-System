# Run 5, C1 evidence (harness: test filter and serialized fault dumps)

Branch c1/harness. Final head **165cb992558855c858ef075ee7d57da0efe2eeeb**
(6e39f425 plus a check-braces exemption for C generated under build/ and
docs). The kernel sources of 6e39f425 and 165cb992 are identical
(`git diff 6e39f425 165cb992 --stat`: docs/testing.md, scripts/check-c-braces.py).
All QEMU runs: `make test` (systemd-run MemoryMax=3G, CPUQuota=200%,
timeout). Every .log starts with a header line naming head, command and
load average; the rest is the unedited build/test-output.log (serial from
QEMU start) or make output.

## Filter x50 (QEMU)

- filter-x50-qemu-165cb992.log: `make test TEST_FILTER=test_preempt_flag_follows_migrated_task TEST_REPEAT=50`,
  banner (165cb992), 50 `[ITER k/50]` lines, 50 of 50 PASS, exit 0.
- filter-x50-qemu-precommit-dirty.log: the same run on the uncommitted tree
  before 6e39f425 (banner 8ec0e330-dirty); superseded, kept for the record.
- pre-commit-try1-unfiltered.log: first attempt, `-append` on QEMU; QEMU's
  ELF boot passes no DTB (`DTB parsing failed`), so the run was unfiltered.
  It also exposed two bugs fixed before 6e39f425 (index missed generator-
  expression test files; `ktest` had no help entry) and one #1230 failure.

## Unfiltered output unchanged (diff against base)

- unfiltered-base-8ec0e330.log (PASS), unfiltered-c1-6e39f425.log (PASS).
- unfiltered-diff-summary-format.txt: diff of the summary-format lines
  (`[TEST]`, `  [PASS|FAIL|IGNORE]`, `Tests:`, `[PASS] All`, `[FAIL]`,
  `====`, `Test Suite Summary`, `[INFO] Exiting`): 0 lines removed or
  changed, 25 added, all in the two new suites (Test Filter Tests 12,
  Fault Dump Lock Tests 7). The final summary block (last 6 lines) is
  byte-identical.
- unfiltered-diff-raw.txt (raw, paths normalized) and
  unfiltered-diff-normalized.txt (numbers and hex normalized, 97 lines):
  besides the new suites, `ktest` in three help listings, build banner and
  sha, and run-dependent lines (latency histogram bars, litmus rows,
  slm-runner and counter component interleaving, one PMM order line).
- unfiltered-base-8ec0e330-run1-1230.log: a base run that failed
  test_litmus_patterns_pass (#1230).

## Forced concurrent faults (QEMU, FAULT_DUMP_SELFTEST=ON)

- double-fault-run1..5.log at 6e39f425: CPU 1 data abort (page-fault dump)
  and CPU 2 BRK (panic dump) released at the same instant. All 5: two
  contiguous blocks, one per CPU, every line prefixed `[cpuN] `, both
  `System halted.`, `dumps completed: 2 of 2`, exit 0. Order varies
  (cpu2 first in runs 1, 4, 5). check_dumps.py.txt is the checker.
- double-fault-control-nolock-run1..3.log: control with the dump lock
  disabled by control-nolock.patch (applied for these 3 runs only, never
  committed; banner 6e39f425-dirty). The dumps interleave line by line
  (12, 18 and more blocks per run per check_dumps.py).

## /ci

ci-6e39f425/: every step's full log at 6e39f425. ci-165cb992/: reruns at
165cb992 (braces; make test and make test AI_SCHED=ON).

| Step | Result | File |
|---|---|---|
| 1 ARM64 build | rc 0 | ci-6e39f425/01-arm64-build.log.txt |
| 2 x86-64 build | rc 0 | ci-6e39f425/02-x86-build.log.txt |
| 3 Jetson real weights, main + test | rc 0 | ci-6e39f425/03-jetson-real.log.txt |
| 4 make test | FAIL at 6e39f425 (#1230 only); FAIL at 165cb992 04r (#1230 only); PASS at 165cb992 litmus-ab/head1 | ci-6e39f425/04-*, ci-165cb992/04r*, litmus-ab/head1.* |
| 4b make test X86_64 | PASSED | ci-6e39f425/04b-* |
| 5 make test AI_SCHED=ON | FAIL at 6e39f425 and 165cb992 05r (#1230 only); PASS at 165cb992 05r2 | ci-6e39f425/05-*, ci-165cb992/05r* |
| 6 runtime-test | 308 and 328 passed, 0 failed | ci-6e39f425/06-runtime-test.log.txt |
| 7 braces | rc 2 at 6e39f425 (generated index unparsed); 0 violations, 0 unparsed at 165cb992 | ci-6e39f425/07-*, ci-165cb992/07-braces-rerun.log |
| 8 build order | All build-order tests passed | ci-6e39f425/08-build-order.log.txt |
| 9 Jetson layout | All checks passed. | ci-6e39f425/09-* |
| 10 diff | no build artifacts | ci-6e39f425/10-diff.log.txt |

The 6e39f425 step runner recorded rc 0 for steps 4, 4b and 5 because the
copy after make was the last command; read the PASSED/FAILED line in each
step log, not the rc.

## #1230 under host load (litmus-ab/)

Interleaved full `make test`, base 8ec0e330 and head 165cb992, load average
3.8 to 5.7 (C2 and C3 QEMU runs on the host): base 2 of 2 failed
test_litmus_patterns_pass (MP weak rows 1, 1), head 1 of 2 failed (0, 1).
Across all C1 runs at this load the test failed on both trees; it is the
#1230 flake, not a C1 regression. No other test failed in any run.
