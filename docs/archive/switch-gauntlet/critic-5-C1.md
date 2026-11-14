# Critic verdict, switch gauntlet run 5, component C1 (harness: test filter and repeat, serialized fault dumps)

Critic: `c1-critic`, blind. Model that wrote this verdict: **Fable 5.2**
(model ID `claude-fable-5-2`). Brief received about 02:07 UTC 2026-11-14;
verdict written 02:55 UTC.

**Verdict: FAIL.** Two defects in the dump path that the builder's runs
never exercised (section 4), one criterion replaced by a different
mechanism (section 3), one stale kernel document, and no complete /ci
sequence at the PR head (section 6). Both features work as demonstrated:
the critic reproduced the filter x50, the forced double fault 4 of 4, and
the unfiltered summary format. The fix list is short (section 9). C2 and
C3 are not blocked by any of it: the build-time filter works on every
platform the critic could run.

Heads. Base `8ec0e330dca4d8db5053e0bf2ed2101699a9ff4c` (equal to
`origin/main` and to `git merge-base` of the C1 head and main: no stale
base). C1 head `165cb992558855c858ef075ee7d57da0efe2eeeb`
(`origin/c1/harness`). Builder evidence: `origin/gauntlet-5-hw-pi5` @
0068c5be, `docs/archive/switch-gauntlet/run5-hw/c1/`. Critic evidence:
`origin/gauntlet-5-hw-pi5` @ 985c7a8b,
`docs/archive/switch-gauntlet/run5-hw/c1-critic/` (42 files, `INDEX.txt`
first). File names below are in `c1-critic/` unless they start with `c1/`
or `c0/`.

Order of work. The diff `8ec0e330..165cb992` was read first, including
its `docs/testing.md` hunk (part of the change under review; it contains
the builder's own "Deviation" paragraph, so the critic knew that claim
before reproducing). The critic's reproductions ran next. The critic's
count of the builder's raw logs was saved to `own-count-before-readme.txt`
(sha256 18b00c7d2853bda9...) at 02:35 UTC, before `c1/README.md` or any
`c1/*diff*` summary was opened. `switch-5.md` and
`run5-builder-notes.md` were not opened.

Process disclosures. (1) The critic's first QEMU run (`filter-x50-run1`,
started 02:09:43 UTC) overlapped another agent's `make test` QEMU (pid
2227095, seen by the `pgrep` in the same command). Every later run
started with no other `make test` QEMU on the host. A QEMU from an older
session (pid 3381445, a production ELF under another scratchpad) ran
throughout and was left alone. (2) The critic killed no process and made
no labctl call. (3) Five critic patches were applied to the critic's own
worktree for experiments and reverted with `git checkout`; none was
committed to a code branch (`critic-exp-*.patch`).

## Summary

| Definition-of-done row | Verdict |
|---|---|
| 1a. Filter: exact names, comma-separated, N passes, one line per iteration with result and time, then the summary | **PASS** (reproduced: 50 of 50, plus two names, unknown name, bad repeat, failing iteration, x86-64) |
| 1b. Filter from the kernel command line or a boot argument that kexec and UEFI-direct can both pass | **FAIL as written** (redefined criterion: neither Jetson path takes a boot argument; a build-time filter is substituted; the command-line source has never run on any target) |
| 1c. Filter from the shell | **PASS on the critic's run only** (`ktest` works in QEMU with a critic patch; the builder never ran it) |
| 1d. No filter: summary format unchanged byte for byte | **PASS** (reproduced against the critic's own base run) |
| 1e. Documented in docs/testing.md | **PASS** |
| 2. Dedicated dump lock, bounded wait, a faulted holder never blocks another CPU's dump, CPU id on every dump line | **PASS on the bar's words, with two defects to fix** (SLOT_TRACE paths never end the dump; a holder that stops mid-dump silences every other CPU's ordinary prints for good) |
| 3a. make test in all /ci configurations | **Met on retry only** (#1230; rates equal on base and head; no single clean /ci sequence at 165cb992) |
| 3b. One QEMU run with the filter x50, one with two forced concurrent faults | **PASS** (builder 1 and 5; critic 1 and 4) |
| Critic ran the filter and the forced double fault | Done |

## 1. Reproduction (clean tree at 165cb992 unless noted)

All runs are `make test ...` (the Makefile's systemd-run guard and
timeout). Load is the 1-minute average from `/proc/loadavg`, start to end
of the make command, taken from line 1 and the last line of each
`*.make.log`.

| Run | Files | Banner | Result | Load |
|---|---|---|---|---|
| Filter x50: `make test TEST_FILTER=test_preempt_flag_follows_migrated_task TEST_REPEAT=50` | `filter-x50-run1.{make,test-output}.log` | 165cb992 | 50 `[ITER k/50]` lines, 50 of 50 PASS; `runs=50 passed=50 failed=0`; 41244 to 70007 us per iteration (avg 55598); exit 0; 259 console lines | 2.30 to 3.02 |
| Unfiltered at head | `unfiltered-run1.*` | 165cb992 | 3040 `[PASS]`, 0 `[FAIL]`, exit 0, 0 `FILTER` lines, 8642 lines | 2.73 to 3.13 |
| Unfiltered at base (critic's own worktree at 8ec0e330) | `unfiltered-base-run1.*` | 8ec0e330 | 3019 `[PASS]`, 0 `[FAIL]`, exit 0, 8611 lines | 2.66 to 2.58 |
| Forced double fault: `make test FAULT_DUMP_SELFTEST=ON`, 4 runs | `double-fault-run{1..4}.*` | 165cb992 | 4 of 4: 122 dump lines, 0 without a `[cpuN] ` prefix, 2 contiguous blocks (74 lines cpu2, 48 lines cpu1), 2 `System halted.`, `dumps completed: 2 of 2`, exit 0. cpu2 first in runs 1 and 3, cpu1 first in runs 2 and 4 | 1.95 to 2.11; 2.11 to 2.41; 2.41 to 2.35; run 4 end 3.59 (start not recorded) |
| Two names x3 (C3's pair) | `filter-pmm-pair-x3.*` | 165cb992 | 6 ITER lines in order a b a b a b, 6 PASS, exit 0 | 2.73 to 2.71 |
| Unknown name | `filter-bogus.*` | 165cb992 | "not in the test index; entering every suite"; the known test 2 of 2 PASS; `[FAIL] test_no_such_test_xyz: not found (0 runs)`; exit nonzero | 2.71 to 2.44 |
| `TEST_REPEAT=0` | `filter-badrepeat.*` | 165cb992 | `[FILTER] [FAIL] ... unparseable test filter (error -5); running nothing`; exit nonzero | 2.44 to 2.17 |
| x86-64 filtered x3 | `x86-filter-x3.*` | 165cb992 | 6 of 6 PASS; the stall test measures 50150, 50174, 50943 us | 2.75 to 3.80 |
| x86-64 unfiltered | `x86-unfiltered.*` | 165cb992 | 1598 `[PASS]`, 0 `[FAIL]`, exit success | 3.80 to 3.98 |

Summary format (row 1d). The brief named the base's
`c0/ci-base-8ec0e330/04-test.log` as the reference; that file is make
output (1412 lines) and holds no kernel console, only the
`Test Results: PASSED` trailer, which is identical to the critic's
(`diff` of the trailers: empty). The console comparison therefore uses the
critic's own base run. `unfiltered-diff-normalized.txt` (paths, decimal
and hex numbers normalised): 30 hunks, 138 lines. Every hunk is one of:
the two new suites (29 added lines: Test Filter Tests 12 tests, Fault
Dump Lock Tests 7 tests); `ktest` in three help or file listings; banner
hash; run-dependent lines (latency histogram bars, coherency litmus rows,
`[counter]` and Rust task interleaving, one PMM order line). The lines
`[TEST]`, `[PASS]/[FAIL]/[IGNORE]`, `Tests: N Passed: N Failed: N`,
separators and the suite summary differ only by the added suites. The
last 200 bytes of the two console logs are identical (`cmp`), CR LF
included.

Fault sites (run-4b row "wrong frame"). `double-fault-run4`: cpu1 ELR
0x40084a50 is `ldr x0, [x0]` with x0 = 0x7f0000000000 (can raise a data
abort); cpu2 ELR 0x40084a58 is `brk #0x7` (objdump of `fd_fault_task` in
that build).

## 2. The builder's evidence, counted from the raw files

From `own-count-before-readme.txt`:

| File (c1/) | Banner | Count |
|---|---|---|
| `filter-x50-qemu-165cb992.log` | 165cb992 | 50 ITER lines, 50 PASS, 0 FAIL, all-suites line present, 261 lines (2 header lines, then the console from QEMU start) |
| `filter-x50-qemu-precommit-dirty.log` | 8ec0e330-dirty | 50 of 50; superseded |
| `double-fault-run{1..5}.log` | 6e39f425 | 5 of 5: 122 lines, 0 unprefixed, 2 blocks, 2 halts, 2 of 2 |
| `double-fault-control-nolock-run{1..3}.log` | 6e39f425-dirty | 62 to 70 of 124 lines unprefixed, 71 to 98 blocks: the control interleaves |
| `unfiltered-base-8ec0e330.log` | 8ec0e330 | 3019 PASS, 0 FAIL |
| `unfiltered-base-8ec0e330-run1-1230.log` | 8ec0e330 | `test_litmus_patterns_pass` FAIL |
| `unfiltered-c1-6e39f425.log` | 6e39f425 | 3040 PASS, 0 FAIL |

These agree with `c1/README.md`. Two notes. The five double-fault runs
are at 6e39f425, not the PR head; `git diff --stat 6e39f425 165cb992` is
`docs/testing.md` and `scripts/check-c-braces.py` only, and the critic's
4 runs are at 165cb992. Every builder log carries one or two added header
lines above the raw console (disclosed in the README).

## 3. Row 1b: the boot argument (redefined criterion)

The bar: "from the kernel command line or a boot argument that the kexec
and UEFI-direct paths can both pass". What exists: a command-line source
(`slmos.test=`, `slmos.test_repeat=` in `/chosen/bootargs`, ARM64 only)
and a build-time source (`TEST_FILTER=`, `TEST_REPEAT=` baked into the
image). `docs/testing.md` states that neither Jetson path can pass the
command line and that both use the build-time filter.

Checked. `scripts/jetson-kexec-slmos.sh:1197` sets
`KEXEC_CMDLINE="$(cat /proc/cmdline)"` and has no option to append, so the
installed helper cannot pass it; the run's rules forbid installing
anything on the board. The UEFI stub reads no load options
(`grep LoadOptions kernel/arch/arm64/*.c`: none). The builder's statement
of fact holds. Not verified by the critic (no lab): that Pi 5
`cmdline.txt` reaches the parser.

Judgment. This redefines the bar; it does not meet it. A build-time
filter is not a boot argument: each filter needs a rebuild and a
redeploy, and each filtered image is a different binary from the plain
test kernel at the same head. It does deliver what C2 and C3 need in this
run (the critic ran it on QEMU ARM64 and x86-64), and the deviation is
disclosed in `docs/testing.md`. Under "the bar applied as written" the
row fails until either the mechanism exists or the user accepts the
substitute in the log.

Path never exercised. The command-line source has run on no target. QEMU
boots the ELF with no device tree, and the builder's attempt
(`c1/pre-commit-try1-unfiltered.log`) ran unfiltered. The critic tried
`-kernel build/kernel-test/slmos.bin -append ...`
(`exp-cmdline-run1.first300.log`): the device tree parses, but that load
path is unsupported (the image runs at 0x40200000 and faults in VFS init
before the harness), so the attempt is void and says nothing about C1.
Coverage is the 12 parser unit tests. `harness_filter_from_boot()`'s
device-tree branch with a non-empty `slmos.test=` has never executed.

## 4. Row 2: the dump lock

### Lock graph, derived from the code

Locks: D = `dump_lock` (CAS on `owner`, `fault_dump.c`), U = `uart_lock`
(`kprintf.c`). A dumping CPU takes D with a bounded wait, then tries U
with a bounded wait (20 ms), prints, releases U then D in
`fault_dump_end()`. A dumping CPU's own prints skip U. An ordinary
printer takes U alone, unbounded, IRQs masked. No path takes D while
holding U, so there is no D/U cycle. The only unbounded wait in the graph
is an ordinary printer on U.

| Case | Code | Result |
|---|---|---|
| IRQs masked | `panic()`, `el1_sync_handler` (now masks at entry), x86 `exception_handler` (after `cli`), bad-SP entry (masked by the exception) | No wait needs an IRQ; the waits poll the counter |
| Pre-MMU, `spinlock_hw_enabled` 0 | `fault_dump_begin`: NOLOCK branch, plain `|=`, no CAS, no `ldsetal`; `UART_LOCK_IRQSAVE` skipped; `spin_trylock` not reached | No exclusive or LSE access before `vmm_init`. Secondaries enable the MMU (`smp_boot.S:198`) before installing VBAR (`:306`), so no dump is possible on a secondary with the MMU off |
| Holder faults inside its dump | `fault_dump_cpus & bit`, else `owner == me + 1`: NESTED, no wait | Reproduced: `exp-holder2-run1` (critic patch, cpu1 takes a second data abort mid-dump): second dump printed, `System halted`, lock released, cpu2's dump follows, 2 of 2, exit 0 |
| Holder stops mid-dump | Waiter: no change of `owner` or `progress` for 50 ms of counter time: STALLED, dumps without the lock, prefixed | Reproduced: `exp-holder1-run1` (critic patch, cpu1 spins in `wfe` after "Task Context"): cpu2 printed `fault_dump: cpu1 holds the dump lock and printed nothing for 50 ms; dumping without it` and its full dump, every line prefixed. The builder had shown this only on a private lock in a unit test |
| Frozen counter | 10^6 equal reads end both waits (`fault_dump_lock_acquire`, `uart_lock_try_hold`) | Unit test passes on ARM64 and x86-64 (54 to 102 ms in QEMU, `exp-failiter-run1.test-output.log`) |
| UART lock held by a faulted CPU | The dumping CPU skips U; `uart_lock_try_hold` gives up after 20 ms | By code; not forced by the critic |
| Unknown CPU id | `cpu_id()` returns 0 for an unmapped MPIDR or LAPIC id; such a CPU would be tagged `[cpu0]` and treated as nested if CPU 0 holds D | Pre-existing `cpu_id()` behaviour; note only |

The bound. The brief says "about 50 ms". The implementation waits until
the holder has been silent 50 ms, or 5 s in all. A holder that keeps
printing (a long dump, or a holder looping in nested faults) can delay
another dump by up to 5 s, not 50 ms. The critic accepts this reading: a
fixed 50 ms would interleave any two dumps longer than 50 ms, which is
the run-4b symptom, and the wait stays bounded. It is a change to the
letter of the bar and is stated in `fault_dump.h` and `docs/testing.md`.

### Defect A: the SLOT_TRACE paths never end the dump

`slot_trace_bad_sp_fault()` (`kernel/sched/slot_trace.c`) now calls
`fault_dump_begin()` and never `fault_dump_end()`; its caller is
`bl slot_trace_bad_sp_fault; b hang` (`vectors.S:388`). In a SLOT_TRACE
build `el1_sync_handler` calls `fault_dump_begin()` and then
`slot_trace_dump()`, which ends in `semihosting_exit(3); for (;;) wfe`
and never returns, and whose "already frozen" branch also spins forever.
So in every SLOT_TRACE build the first faulting CPU keeps D and U for
good. Other CPUs' dumps are not blocked (50 ms stall), but their
ordinary prints are (Defect B). The builder never ran a SLOT_TRACE
build (no such log in `c1/`). `exp-badsp-run1` (critic patch,
`SLOT_TRACE=ON`, bad SP on cpu1): cpu1's 86 dump lines are all prefixed,
then QEMU exits through `semihosting_exit(3)`; on hardware that call
does not exit. C2's brief step 2 reuses the SLOT_TRACE ring on Jetson.

### Defect B: a holder that stops mid-dump silences every other CPU

The dump holds U for its whole length. If the holder stops before
`fault_dump_end()` (Defect A, or any hang inside a dump), U is never
released, and every other CPU's next `uart_printf`/`uart_puts` spins on U
forever with IRQs masked. Before C1 those CPUs kept printing.
`exp-holder1-run1`: after cpu2's dump the test on CPU 0 prints one line
(`[fault_dump selftest] dumps completed: ...`) at most 20 s after the
faults; that line never appears, and the run ends in
`TIMEOUT - Tests did not complete within 60s`. The bar's words concern
dumps, and dumps are not blocked; but a fault report that ends with the
rest of the console frozen is worse than before for the case the lock
was built for, and it defeats the Jetson policy "CPU off, others
continue" whenever a dump does not finish.

### Other checks

- Every dump line carries its CPU: 0 unprefixed lines of 122 in 4 of 4
  critic runs; the canary and stack-dump helper lines are prefixed. Any
  code that calls `uart_putc()` directly inside a dump would bypass the
  prefix; none of the dump helpers does (`grep` of `panic.c`,
  `exceptions.c`, `slot_trace.c`).
- Forced concurrent faults exist for ARM64 only. Concurrent x86-64 dumps
  are untested (disclosed in `docs/testing.md`).
- QEMU only: a holder vCPU thread throttled by the 200% CPU quota for
  more than 50 ms of host time would look stalled. Not seen in 9 runs
  (builder 5, critic 4).
- `kernel/CLAUDE.md`, section "UART Lock", still says "Panic and
  exception output use the `_unlocked` variants and never take it". A
  dump now holds the UART lock. The document was not updated.

## 5. Files touched outside the owned areas

Owned: `kernel/tests/` runner and registration, the exception and panic
print path. `git diff --stat 8ec0e330..165cb992`: 22 files, 2026
insertions, 188 deletions.

| File | Judgment |
|---|---|
| `kernel/tests/*` (9 files), `scripts/gen-test-index.py` | Owned (runner, registration, tests) |
| `kernel/src/fault_dump.c`, `kernel/include/fault_dump.h`, `kernel/src/panic.c`, `kernel/arch/arm64/exceptions.c`, `kernel/arch/x86_64/idt.c` | Owned (print path) |
| `kernel/src/kprintf.c` | Justified: the prefix and the lock skip must sit under every print |
| `CMakeLists.txt`, `Makefile` | Justified: new sources, the index generator, `TEST_FILTER`, `TEST_REPEAT`, `FAULT_DUMP_SELFTEST`; the flags enter the test build signature, and the critic saw a filtered build followed by an unfiltered one re-configure and print 0 `FILTER` lines |
| `kernel/src/shell.c`, `kernel/src/help.c` | Justified: `ktest` registration (boot-test images only) and the help entry an existing test requires |
| `docs/testing.md` | Required |
| `kernel/sched/slot_trace.c` | C2's directory. Three lines in a fault print function: in scope by substance, but incomplete (Defect A) |
| `scripts/check-c-braces.py` | Exempts everything under `build/` so the generated index does not fail /ci step 7 as "unparsed". Acceptable; no self-test case was added for the new exemption |

Off path. With no dump in progress each printed character costs one load
of `fault_dump_cpus` and one branch (`uart_puts_unlocked` disassembly:
`ldr w1, [x20]; cbnz`), and each locked print one more load. With no
filter, `RUN_TEST` is now a call to `UnityRunTest` and each suite entry
one flag test. `FAULT_DUMP_SELFTEST` off: no `fd_fault_task` or
`test_fault_dump_two_cpus_concurrent` symbol in the image. Production
images gain the lock code (about 1.9 KB of text by symbol sizes) and,
because `TEST_SOURCES` go into every image, the new tests and filter
(`g_test_filter` 1560 bytes and `g_test_filter_stats` 640 bytes of bss;
`cmd_ktest` is present though unregistered). A whole-image size
comparison is not usable: the critic's two production builds differ by
214224 bytes of text in the wrong direction and by 1904 symbols
(`prod-size-compare.txt`), cause not investigated, so no total is
claimed.

## 6. Row 3a: /ci green

Builder's files, counted by the critic:

| Step | At 6e39f425 (`c1/ci-6e39f425/`, banner `6e39f425-dirty`) | At 165cb992 (`c1/ci-165cb992/`) |
|---|---|---|
| 1 to 3 builds, 4b x86 test, 6 runtime-test, 8, 9, 10 | pass | not run |
| 4 `make test` | FAIL (`test_litmus_patterns_pass`) | `04r`: FAIL (same test) |
| 5 `make test AI_SCHED=ON` | FAIL (same test) | `05r`: FAIL (same test); `05r2`: PASS |
| 7 braces | rc 2 (generated file unparsed) | rerun: pass |

`c1/ci-6e39f425/progress.txt` records `rc=0` for steps 4 and 5 while
their logs end in `FAILED` and `make: *** Error 1`. The README says so
and says to read the logs; the rc values in that file are wrong and
should not be quoted.

`test_litmus_patterns_pass` is #1230, out of scope for this run. Default
configuration, every run on file: base 3 failures of 6
(`c0/.../04-test.log` pass, `c1/unfiltered-base-8ec0e330-run1-1230.log`
fail, `c1/unfiltered-base-8ec0e330.log` pass, `c1/litmus-ab/base1` fail,
`base2` fail, critic `unfiltered-base-run1` pass); head 3 failures of 6
(`c1/ci-6e39f425/04` fail, `c1/unfiltered-c1-6e39f425.log` pass,
`c1/ci-165cb992/04r` fail, `c1/litmus-ab/head1` pass, `head2` fail,
critic `unfiltered-run1` pass). AI_SCHED: base 0 of 1, head 2 of 3. No
other test failed in any of these runs. "Not a C1 regression" is
consistent with 3 of 6 against 3 of 6; with 6 runs a side it is not
proven, and it stays a hypothesis.

Judgment. "/ci green" is met only by retry. A passing run of every step
exists (step 4: `litmus-ab/head1` and the critic's `unfiltered-run1`,
both at 165cb992; step 5: `05r2`), and every failure is the one
out-of-scope test that fails at the same rate on the base. But no single
/ci sequence is green at the PR head: steps 1 to 3, 4b, 6, 8 and 9 ran
only at 6e39f425 on a tree with `docs/testing.md` modified
(`10-diff.log.txt`: ` M docs/testing.md`), and step 4 failed in both of
its /ci-form runs. The kernel sources of the two heads are identical, so
the critic does not treat those steps as unknown; the critic's own x86-64
run at 165cb992 passed. Because the fixes below move the head, a full
/ci at the new head is owed anyway, with step 4 and 5 retries counted
and reported as a rate beside the base's.

## 7. Row 1c: the shell

`ktest` is registered only in boot-test images, and a QEMU test kernel
exits through semihosting before the shell runs, so the builder never
ran it. With a critic patch that makes `semihosting_available()` return
0 (`critic-exp-nosemi.patch`), `exp-ktest-run1.log`: after the boot run,
`ktest test_checkerboard_fragmentation,test_exhaustion_recovery 3` gave
6 ITER lines, 6 PASS, `[FILTER] [PASS] ktest`;
`ktest test_preempt_flag_follows_migrated_task 5` gave 5 of 5;
`ktest nosuch_test` gave `not found (0 runs)` and `[FAIL] ktest: 1
failure(s)`; `ktest a,,b` gave `unparseable test filter (error -1)`. 13
ITER lines in all (2 from the boot filter). The command works; it has
not run on hardware.

Failing iteration (`exp-failiter-run1`, critic patch failing pass 2 of
one test): the assertion message, then
`[ITER 2/3]  [FAIL] test_fault_dump_idle_state (596 us)`, passes 1 and 3
PASS, summary `runs=3 passed=2 failed=1`, exit nonzero.

Limits of the filter, for its users: only `RUN_TEST` names can be
selected (the Rust FFI tests and the integration tests that print their
own `[PASS]` lines cannot); suite setup code outside `RUN_TEST` runs
once per pass; an iteration line carries two spaces before `[PASS]`.

## 8. Failure modes, by name

| Failure mode | Finding |
|---|---|
| Selective summary | Not found. The README lists the failing /ci steps. `progress.txt` rc values contradict the logs (section 6), disclosed |
| Truncated or tail-only capture | Not found: every console log runs from the QEMU start banner to the semihosting exit line |
| Assembled register file | Not applicable. Builder logs carry added header lines above raw output, disclosed |
| Redefined criterion | **Found**: row 1b (build-time filter in place of a boot argument). Also the 50 ms bound read as "silent for 50 ms, 5 s cap" (accepted, section 4) |
| Hypothesis stated as finding | "#1230 flake, not a C1 regression": consistent, not proven (6 runs a side). "Pi 5 can use cmdline.txt": untested, and the doc says it was not exercised |
| Stale base | Not found: merge-base is main 8ec0e330 |
| Missing critic | Not applicable |
| Partial per-CPU table | Not found: both CPUs' dumps complete in 9 of 9 runs |
| Number without a file | Not found in the README. `progress.txt` rc values are wrong |
| Uncommitted tree | **Found, minor**: the /ci at 6e39f425 ran on a tree with `docs/testing.md` modified (banner `6e39f425-dirty`) |
| Critic reading the claim first | The diff's doc hunk was read before reproduction (it is part of the change); the README and summaries after the critic's own count |
| Undisclosed rule break | Builder: none found. Critic: one overlapping QEMU run, disclosed above |
| Banner hash mismatch | Builder's double-fault runs and most /ci steps are at 6e39f425, not the PR head; kernel sources identical by `git diff`; critic's runs are at 165cb992 |
| Bar computed from one cell | Not applicable |
| Precondition from a prior run | Not applicable |
| Path never exercised | **Found**: the command-line source on any target; `ktest` (builder); SLOT_TRACE dump paths (Defect A); a stalled or nested holder end to end (builder; the critic ran both); concurrent x86-64 dumps |
| "Cannot deadlock" claim (run 4, C1) | Lock graph derived from code (section 4). Dumps cannot deadlock. An ordinary printer can wait forever on a holder that stopped (Defect B) |
| Production rows only (run 4b) | Not applicable: QEMU only by the brief; test kernels at the PR head |
| Wrong frame read as a fault site (run 4b) | Checked: both ELRs are instructions that raise their exception class (section 1) |
| Kill by name (run 4, C1 critic) | No process killed by the critic |
| Critic claimed a board (run 4, C3 critic) | No labctl call |

## 9. Fixes required for a pass

1. End the dump on the SLOT_TRACE paths. Call `fault_dump_end()` before
   `slot_trace_dump()` stops (before `semihosting_exit(3)` and in its
   "already frozen" branch) so the dump lock and the UART lock are
   released. Run one `SLOT_TRACE=ON FAULT_DUMP_SELFTEST=ON` fault and
   commit the log.
2. Bound what a stopped holder can do to ordinary prints. Either take the
   UART lock per dump line instead of for the whole dump, or let an
   ordinary printer stop waiting on the UART lock once a dump is in
   progress and its holder has been silent for the stall bound. Add a
   test in which the holder stops mid-dump and CPU 0 still prints
   afterwards (`critic-exp-holder.patch` with `CRITIC_EXP=1` is a
   starting point).
3. Row 1b, one of: (a) add the boot argument for real: an option in
   `scripts/jetson-kexec-slmos.sh` to append to the kexec command line
   (it takes effect when the user next installs the helper), and for
   UEFI-direct a source the stub can read (load options or a file on the
   ESP); or (b) the user accepts the build-time filter as the Jetson
   mechanism and the log records the changed bar. Without the user, (b)
   stays open and the PR description must say so in its first paragraph.
4. Update `kernel/CLAUDE.md`, section "UART Lock": dumps skip the lock on
   their own CPU and hold it for other CPUs.
5. Run the whole /ci sequence at the new head on a clean tree, one
   directory, with real exit codes. Report steps 4 and 5 as failures over
   runs beside the base's rate if #1230 fires.

Not required, recorded: a self-test case for the `build/` exemption in
`scripts/check-c-braces.py`; exercising `slmos.test=` on Pi 5 when a
component next holds the board; the double space in the `[ITER]` line.

## Addendum 1 (about 03:05 UTC 2026-11-14): builder evidence at 48bb1be3

The builder added `c1/litmus-ab/base3.*`, `head3.*`, `head3ai.*` and
`TALLY.md` (commit 48bb1be3). That commit is an ancestor of the critic's
evidence commit 985c7a8b, so the files were on the branch when the
critic pushed; they were not counted in section 6. The C1 code head is
unchanged (165cb992). The verdict is unchanged: **FAIL**, same fix list.
The raw logs were counted before `TALLY.md` was opened.

Counted from the raw logs:

| File (c1/litmus-ab/) | Banner | Config | `[PASS]` | `test_litmus_patterns_pass` | MP weak rows | Load at start, as recorded in the file's own header |
|---|---|---|---|---|---|---|
| `base3.log.txt` | 8ec0e330 | default | 3019 | pass | 0 | 2.96 |
| `head3.log.txt` | 165cb992 | default | 3040 | pass | 0 | 1.46 |
| `head3ai.log.txt` | 165cb992 | AI_SCHED | 3128 | pass | 0 | 2.75 |

All three are complete (banner to the semihosting exit line), 0 `[FAIL]`.

Section 6's rates, updated. Default configuration: base 3 failures of 7
(adds `base3`), head 3 of 7 (adds `head3`). AI_SCHED: base 0 of 1, head
2 of 4 (adds `head3ai`). `c1/pre-commit-try1-unfiltered.log` (a failure
on the uncommitted tree before 6e39f425) is not in the head count.

`TALLY.md` checked as a claim:

- Its 16 PASS/FAIL results and its MP-weak-row counts agree with the
  logs. The two rows marked "see file" are the two largest counts on
  file: `ci-165cb992/04r` 9 weak rows and `05r` 6, both on the head; the
  largest on the base is 4 (`unfiltered-base-8ec0e330-run1-1230.log`).
- Its statement that each load is "recorded in each file's header" holds
  for 6 of the 14 rows that give a load: 3.05, 3.74, 2.05 (the three
  `unfiltered-*` logs) and 1.46, 2.75, 2.96 (the three new runs). The
  other 8 loads (try1 3.71; 04r 5.70; 05r 6.93; 05r2 2.56; base1 4.09;
  head1 3.79; base2 4.67; head2 5.67) appear in no log, make file or
  header on the branch (`grep` finds them in `TALLY.md` only). They are
  numbers without a file.
- Its conclusion "under 3.1 all pass, 3.7 and above all but one fail, on
  both trees" therefore rests on file-backed loads for 6 runs only: 5
  passes at 1.46 to 3.05 (head 3, base 2) and 1 failure at 3.74 (base).
  The critic's own runs add 2 passes with recorded loads (head 2.73,
  base 2.66). On file-backed loads the head has passed at low load and
  has no high-load run with a recorded load at all; "the failure tracks
  host load on both trees" stays a hypothesis for the head. It is
  consistent with everything counted, and nothing on file contradicts
  "no change in rate from C1".

Effect on row 3a. Step 4 now has three passing runs at 165cb992 on file
(`head1`, `head3`, critic `unfiltered-run1`) and step 5 two (`05r2`,
`head3ai`). The judgment stands: every step has passed at the head's
kernel sources, the only failing test is #1230 at the base's rate, and
there is still no single /ci sequence at the PR head on a clean tree.
Fix 5 is unchanged; when it is run, the load at the start of every step
belongs in the step's own log.
