# Critic verdict, switch gauntlet run 5, component C3 (the PMM halt: #1207, #1228)

Critic: `critic-c3`, blind. Model that wrote this verdict: **Fable 5.2**
(model ID `claude-fable-5-2`). Brief received about 06:17 UTC 2026-11-14;
verdict written 06:40 UTC.

**Verdict: PASS, on the "open" branch of the definition of done**, with the
hardware reproduction recorded as not done and six fixes to wording and
follow-up (section 10). The builder claims no root cause and no fix, and
what it does claim holds: the run-4 fault decode is right and is now
checked against rebuilt ELFs (section 3), the hardware counts are exact
(section 2), the instrument is off by default with no change to `pmm.c`
when off (section 4), and the rate statement is correct (section 6). The
"open" state is justified. Three things are overstated or missing: two
hypotheses written as findings, one of the four run-4 captures does not
have the signature the summary gives all four, and the kexec DTB lies
inside the PMM heap in every kexec capture, which the work did not
notice (filed by the critic as #1244).

Heads. Base `8ec0e330dca4d8db5053e0bf2ed2101699a9ff4c` (= `origin/main`).
C1 harness `origin/c1/harness` 2196deb5ac2a067adf434671bbf88f150a1bc769.
C3 `origin/c3/pmm-halt` 321a91beae0268c7e07b68fb5b936fb2d649c157, one
commit on 2196deb5. Hardware rows ran at 5069e658 (one commit on
c1/harness 6e39f425). `git range-diff 6e39f425..5069e658 2196deb5..321a91be`
reports the two commits as identical (`=`), so the C3 change is the same;
the base under it is three C1 commits older (fault-dump and filter
changes, no `kernel/mm` file). Builder evidence: `origin/gauntlet-5-hw-jetson`
@ 93cf0922 and `origin/gauntlet-5-hw-pi5` @ 919d4700, both
`docs/archive/switch-gauntlet/run5-hw/c3/`. Run-4 captures:
`origin/gauntlet-4-hw-jetson` @ 804ad107. No PR exists for `c3/pmm-halt`
(`gh pr list --head c3/pmm-halt`: empty); the brief asks for one only
with a fix.

Critic evidence (not pushed, for the orchestrator to place):
`/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/critic5-c3-log/c3-critic/`
(16 files). File names below without a directory are in that folder;
`run5-hw/c3/...` and `run4-hw/...` are the builder's.

## 1. Blindness and process

Order of work: issues #1207, #1228, #1240 and the brief; the diff of
321a91be; the raw captures counted; the run-4 dumps decoded by hand; the
critic's QEMU runs and ELF rebuilds. `switch-5.md` (C3 parts of Entries
2, 3, 4, 15, 22, 23, 25) was opened after sections 2, 3 and 4 below were
worked out. `run5-builder-notes.md` was not opened.

Leaks, disclosed. (1) The commit message of 321a91be was printed with
the diff, so the critic read "free_counts > 0 with an empty list (the
#1207 signature: splitting from PMM_ALLOC_FAIL)" before decoding the
dumps; the orchestrator's task message stated the same claim. The decode
in section 3 was still done from the register values and the
disassembly. (2) Line 1 of every evidence file is the builder's result
summary and was seen while listing files. Every count below was taken
from line 4 onward.

Process. The critic ran one QEMU at a time (four runs), killed no
process, made no labctl call and held no board. A QEMU from an older
session (pid 3381445, a production ELF under another scratchpad) ran
throughout and was left alone; whether C2 ran QEMU during the critic's
runs was not checked. Jetson ELF builds ran in a separate worktree while
the critic's QEMU ran. The critic filed #1244 (section 7).

## 2. Hardware numbers, counted from the raw files

All 22 capture files start at `I> MB1 (version: 1.4.0.4-t234-...)` (file
line 5) and carry the banner hash 5069e658 in the body.

Filtered loop, `TEST_FILTER=test_checkerboard_fragmentation,test_exhaustion_recovery
TEST_REPEAT=100`, PMM_DEBUG=ON, jetson-nano-2:

| Files | Path | Banner | `[ITER n/100] [PASS]` lines | not PASS | `pmm-debug` lines |
|---|---|---|---|---|---|
| `run5-hw/c3/filt-kexec-a2916b76-boot01..03.txt` | kexec (`kexec_core: Starting new kernel` 1 per file, `Boot source: direct load`) | `20261113233641 (5069e658)` | 200, 200, 200 (100 + 100 per test) | 0, 0, 0 | 0, 0, 0 |
| `run5-hw/c3/filt-uefi-1fefbfdd-boot01..03.txt` | UEFI-direct (`Boot source: UEFI-direct`) | same | 200, 200, 200 | 0, 0, 0 | 0, 0, 0 |

Total 1200 of 1200 iteration lines pass in 6 boots. The filtered kernel
never runs the detector self-test, so "0 pmm-debug lines" alone does not
show the option was on; the heap start does (`Heap range: 0x835c0000`,
against 0x82d90000 for the same tree without the option in
`run4-hw/closeout/c3-1b85b315-jetson-kexec-suite-boot08.txt`: 8.2 MB of
extra BSS). Trigger logs `run5-hw/c3/trig-filt-kexec-triggers.log` and
`trig-filt-uefi-triggers.log`: 3 triggers each, 6 distinct boot ids, sha
reply `a2916b76ad2a9f89` on kexec. The three kexec files end at
`Lua 5.4 scripting initi` (cut mid-word, after the filter summary and
`[PASS] All test suites passed!`).

Full suite, PMM_DEBUG=ON (walk on every operation), kexec,
`run5-hw/c3/suite-kexec-60c548f3-boot01..16.txt`, banner
`20261114015312 (5069e658)` in all 16:

| Count | Value | Boots |
|---|---|---|
| `Isolating CPU` (the #1228 phase in test_shell) reached and passed | 16 of 16 | all |
| PMM suite summary `Tests: 51  Passed: 48  Failed: 2  Ignored: 1` | 16 of 16 | all |
| `[PASS] test_exhaustion_recovery`, `[PASS] test_checkerboard_fragmentation` | 16 of 16 each | all |
| `[PASS] test_pmm_debug_lists_valid` | 2 per boot, 32 of 32 | all |
| `[PASS] test_pmm_debug_detects_stray_write` | 16 of 16 | all |
| `CORRUPT FREE LIST` reports | 1 per boot, each `found by pmm_debug_selftest (expected report)` | all |
| `free order 9 ... recorded alloc order 10` lines | 2 per boot (#1240) | all |
| PMM halt (#1207 or #1228 signature) | **0 of 16** | |
| Reached `Test Suite Summary` | 7 of 16 | 03, 04, 05, 08, 11, 15, 16 |
| Stopped later, in `[TEST] Scheduler Tests` | 9 of 16 | 01, 02, 06, 07, 09, 10, 12, 13, 14 |

The two PMM failures are `test_coalesce_enables_larger_allocation` and
`test_recursive_coalescing` in every boot; the same two fail without the
instrument in `run4-hw/closeout/c3-1b85b315-jetson-kexec-suite-boot08.txt`
(test_pmm.c:264 and :318), so they are not caused by PMM_DEBUG. The suite
order (PMM suite is the 54th `[TEST]` header, after `test_shell.c`,
before `Scheduler Tests`) was read from boot03. Trigger logs: 8 files,
2 triggers each, 16 distinct boot ids, sha reply `60c548f3eef34910`.

Capture ends. 11 of the 16 suite files end mid-line: the 7 complete boots
end at `[FA` or `[F` (the final "N total failure(s)" line is cut), and
boots 02, 06, 07, 09 end inside a fault dump; boot10 ends at a
`Task 'low' exiting` line (timeout). Every file is complete through the
PMM suite summary, which is what C3's count needs. The cut dumps belong
to the Scheduler Tests faults that C3 passed to C2; they do not meet
"all CPUs in every dump" and should not be cited as complete there.

The builder's numbers in `switch-5.md` Entries 15 and 22 (600 of 600 per
path, 0 of 6 then 0 of 16, 48/2/1 per boot, 9 of 16 stopping in
Scheduler Tests) match these counts.

QEMU logs on `gauntlet-5-hw-pi5`: `qemu-pmmdebug-off-5069e658-test-output.log`
(banner 5069e658; 3034 `[PASS]`, 3 `[FAIL]` lines, 19 `[IGNORE]`; PMM
suite 46/44/0/2) and `qemu-pmmdebug-on-a03aaa12-test-output.log` (banner
a03aaa12, the pre-rebase commit on 8ec0e330; 3014, 3, 18; PMM suite
46/45/0/1; one self-test report, two #1240 lines). Both end
`[FAIL] 1 total failure(s)` from `test_litmus_patterns_pass`. Neither is
at 321a91be; the critic's runs in section 5 are.

## 3. The run-4 fault decode, re-derived

The critic rebuilt the Jetson test kernel (`make kernel-test
PLATFORM=JETSON_ORIN_NANO`) at 1b85b315, d512f40b and 6c1f4c9c and
disassembled `pmm_alloc_pages` and `pmm_alloc_pages_low`
(`disasm-jetson-test-<hash>-pmm.txt`, sha256 in `elf-sha256.txt`). In all
three rebuilds `pmm_alloc_pages` is at 0x80024a40 and
`pmm_alloc_pages_low` at 0x80024c8c, and each capture's x30 lands on the
instruction after the expected `bl`, so the text matches the booted
images. Data addresses match exactly for 6c1f4c9c; for 1b85b315 and
d512f40b the captures' data addresses are 0x50000 higher than the
rebuild's (the run-4 images embedded a larger blob), which does not move
the text. 8a3406ea was not rebuilt: `kernel/mm/pmm.c` is identical in
8a3406ea, 1b85b315 and 8ec0e330, and its capture has the same ELR and
data addresses as the 1b85b315 capture.

| Capture | ELR, instruction | Can raise the class | Decode |
|---|---|---|---|
| `run4-hw/closeout/c3-1b85b315-jetson-kexec-suite-boot08.txt` | 0x80024b6c `strb wzr, [x3], #1` | yes, a store (ESR 0x96000044, WnR 1) | x0 = 0xffffffffffffffff (set at 0x80024c7c, `mov x0, #-1`, reached from `cbz x0` on the list head after `free_counts[o]` read non-zero). x15 = heap_start 0x82d90000. x5 = 6, x6 = (0x1000 << 6) + x0 = 0x3ffff, x9 = (x6 - x15) >> 12 = 0x000ffffffff7d2af, x10 = `block_state` 0x809edb28, FAR = x10 + x9 = 0x1000008096add7. x2 = 4, x1 = 0x10: a 16-page request. So `free_counts[7] > 0` with `free_lists[7] == NULL`, and the first split step wrote `block_state[]` at a wrapped index. |
| `run4-hw/c3/final-8a3406ea-kexec-suite-run06.txt` | 0x80024b6c, same | yes | Same path. x5 = 4, x6 = 0xffff, x9 = 0x000ffffffff7d27f, FAR = 0x809edb28 + x9 = 0x1000008096ada7. `free_counts[5] > 0`, list empty, 16-page request. |
| `run4-hw/c3/repro-main-6c1f4c9c-kexec-run01.txt` (#1228) | 0x80024bcc `strb w4, [x10, x3]`, w4 = 1 | yes, a store | No split (`cmp w2, w3; b.cs`): order 6 requested (x0 = 0x40 pages at the caller, `task_create_with_priority`), `free_counts[6] > 0`, list empty, x0 = -1. x17 = (x0 - 0x82cc0000) >> 12 = 0x000ffffffff7d33f, x10 = 0x8099db28 + x17 = 0x1000008091ae67 = FAR. The write is `set_block_state(-1, 6, BLOCK_ALLOCATED)`. |
| `run4-hw/c3/final-d512f40b-kexec-suite-run05.txt` | 0x80024d48 `ldr x2, [x2]` | yes, a load (ESR 0x96000004, WnR 0) | The free-list walk in `pmm_alloc_pages_low`, `blk = blk->next`. x2 = 0x0007000000000000 = FAR, x8 = 12: a `next` link in the **order-12 list** held 0x0007000000000000. x4 = 0x82d90000 (lowest block so far), 4-page request. |

Results.

- "FAR bit 52 is index wraparound": confirmed. The address is
  `block_state + ((PMM_ALLOC_FAIL + size - heap_start) >> 12)`, not a
  free-list pointer with a tag bit. The reading in the #1207 and #1228
  issue comments ("a kernel free-list pointer with bit 52 set") is wrong
  and is still the latest text on both issues.
- "buddy_alloc split from PMM_ALLOC_FAIL": confirmed for boot08 and
  run06. In #1228 there was no split; the failed pop was used directly.
  All three have the same state: a list head NULL with a non-zero count.
- "#1228 is the same defect": the same **signature** is shown. The same
  cause is a hypothesis.
- The fourth capture (d512f40b run05) does not have that signature. A
  list held a non-NULL garbage link, 0x0007000000000000, in the order-12
  list. A count that ran ahead of its list cannot produce that value; a
  write into a free block's header by something other than the list code
  (or a block owned twice) can. `switch-5.md` Entry 2 describes all four
  captures as "a free list shorter than its free_counts"; that is true
  of three.
- The ELR instruction class, left open by the builder (Entry 25), is now
  verified for all four captures.

`run4-hw/c3/repro-main-6c1f4c9c-kexec-run01.txt` does not start at MB1
(it starts in Linux dmesg just before the kexec; the board was already
up). It is a run-4 file and is complete from the SLM-OS banner to
`System halted`.

## 4. The PMM_DEBUG instrument at 321a91be

- Default off: `option(PMM_DEBUG ... OFF)` in `CMakeLists.txt`; the
  Makefile passes `-DPMM_DEBUG=ON` only for `PMM_DEBUG=ON`; x86-64 is
  refused with a FATAL_ERROR.
- Cost when off, checked by the critic: `pmm.c.obj` from the test builds
  at 2196deb5 and 321a91be is `text 7408, bss 361144` in both; the two
  disassemblies have 1383 lines each and differ in 8 instructions, all
  `mov w2, #<line>` immediates (`pmm-obj-diff-off.txt`). The claim "the
  same instructions, only `__LINE__` immediates move" holds. The test
  kernel is not free: `test_pmm.c` gains two tests that run with the
  option off (`test_pmm.c.obj` text 14277 to 14465; ELF text 7377554 to
  7377810, +256 bytes), and the PMM suite goes from 48 to 51 tests in
  every default test kernel. The production kernel does not build
  `test_pmm.c`.
- Cost when on (QEMU, critic build): ELF text +7040, BSS +1.7 MB; on
  Jetson the heap start moves from 0x82d90000 to 0x835c0000.
- Detector, by code reading. Every free-block header gets a tag and a
  seal over next, prev, address and tag; every list operation checks the
  element and its neighbours first; `dbg_walk` checks every list on every
  public operation (seal, prev linkage, `block_state`, length against
  `free_counts`). A link zeroed or overwritten by a non-PMM writer breaks
  the seal and is reported at the next PMM operation as "not the PMM",
  with the block's last allocation and free and the overlapping ring
  events. A count and chain mismatch is caught by the walk ("list shorter
  than free_counts", "list empty but free_counts > 0") and by
  `DBG_POP_EMPTY` in `buddy_alloc`. A block unlinked at the wrong order
  is caught in `free_list_remove` by the tag. So the instrument covers
  the three run-4 signatures. One allocator path the tag check is the
  only guard for: `is_buddy_free` accepts a buddy whose pages are all
  `BLOCK_FREE` without checking that it is one free block of that order,
  and `pmm_add_region` adds a whole page when a region ends inside it
  (the order-0 fallback has no `current + PAGE_SIZE <= end` test). Both
  are hypotheses by code reading, not shown to occur.
- Self-test, run by the critic (section 5): it zeroes `next` of one
  mid-list block, the walk reports `header tag/seal broken`, `writer: not
  the PMM (seal broken)`, the link is restored and the lists validate.
  It exercises one mutation. The count mismatch path, `DBG_POP_EMPTY`
  and the wrong-order classification are not exercised by any test.
- Limits. (a) The report names the block, the window (between two PMM
  operations) and the block's last owner. It names the writer only when
  the writer is that last owner. For a writer that never owned the page
  (device DMA, a wild pointer) it prints "none" and a second experiment
  is needed, so a catch decides the class but may not meet "the writer
  identified". (b) The option moves the heap start by 8.2 MB and makes
  every PMM call walk every list with IRQs off (test_exhaustion_recovery
  takes 22.9 to 31.9 ms per iteration on kexec against 2.8 to 3.1 ms on
  UEFI-direct in the filtered files), so it can change whether the defect
  occurs. The builder says so. (c) Cosmetic: "owner words covering the
  block" prints the same start once per order (10 identical lines in the
  critic's log).
- Documentation: the `CMakeLists.txt` comment points to
  `docs/testing.md`, which has no PMM_DEBUG text; the description is in
  `docs/memory-map.md`.

## 5. Reproduction by the critic (QEMU, Makefile targets, own worktrees)

| Run | Head, banner | Result | File |
|---|---|---|---|
| `make test TEST_FILTER=test_checkerboard_fragmentation,test_exhaustion_recovery TEST_REPEAT=100` | c1/harness, `(2196deb5)` | 200 of 200 iteration lines pass, 0 FAIL, rc 0 | `qemu-filt-c1-2196deb5-test-output.log` |
| same | c3/pmm-halt, `(321a91be)`, option off | 200 of 200, 0 FAIL, rc 0 | `qemu-filt-c3-321a91be-test-output.log` |
| `make test PMM_DEBUG=ON` (full suite) | `(321a91be)` | 3039 `[PASS]`, 0 `[FAIL]`, 18 `[IGNORE]`, `All test suites passed!`, rc 0; PMM suite 46/45/0/1; 1 self-test report, 2 #1240 lines, no other `pmm-debug` line | `qemu-pmmdebug-on-321a91be-test-output.log` |
| filtered loop with `PMM_DEBUG=ON` | `(321a91be)` | 200 of 200, 0 FAIL, 0 `pmm-debug` lines, rc 0 | `qemu-filt-pmmdebug-on-321a91be-test-output.log` |

One run each. The halt has never been seen in QEMU, so these show that
the filter and the instrument work at both heads and nothing about the
defect. **The brief's critic reproduction on hardware (the filtered loop,
one Jetson boot per head) was not done**: C2 holds the boards and the
orchestrator recorded it as not done when it started this critic.

## 6. Is "open" justified, and are the statistics right

- No run-5 boot reproduced the halt: 0 of 16 instrumented suite boots, 0
  of 6 filtered boots. There is no first-fault capture, so "open" is the
  only honest state, and the builder reports it as such.
- Rate. With run 4's 1 of 10 as the rate, P(0 of 16) = 0.9^16 = 0.185
  and P(0 of 6) = 0.531; the builder's 0.19 and 0.53 are right, and "20
  boots for 88%" is right (1 - 0.9^20 = 0.878). At that rate 29 clean
  boots are needed before 0 catches is below 5%. "Weak evidence of a
  lower rate in this build" (Entry 22) is generous: 0.19 is no evidence
  either way. The 1 of 10 is a run-4 number from one cell, and "0 of 13
  UEFI suite boots" (Entry 2) is also a run-4 number; the critic counted
  neither.
- Run 5 has no uninstrumented baseline. The brief's step 1 says to run
  the filtered loop "on the base plus C1's branch"; the builder ran it
  only with PMM_DEBUG=ON (disclosed in every file header). So no run-5
  row shows the halt still occurs on 8ec0e330 at all, and none separates
  "the instrument hides it" from "16 boots were too few".
- Entry 15 states as a finding that "the halt does not appear in
  isolation; it needs state from earlier suites". The filtered boots and
  the suite boots used the same instrumented build, and that build never
  halted in either mode, so the comparison shows nothing. It is a
  hypothesis (a reasonable one: in d512f40b run05 the list was already
  bad at the third PMM test). The brief's "say which predecessor is
  required" was not answered.
- Next experiment (Entry 25): about 20 suite boots with the option off
  for the rate, then `PMM_DEBUG_WALK_EVERY=64`, then a `--no-helper` arm.
  The first step gives the missing baseline and no capture; the step
  that decides is an instrumented catch. The order is sound. The plan
  should add the two items in section 7 and state the stop rule (29 boots
  at the measured rate).

## 7. Found by the critic

**The kexec DTB lies inside the PMM heap (#1244, filed by the critic).**
In every kexec capture the DTB address is the first page after the
kernel's last PT_LOAD segment and `heap_start` is 0x5000 to 0x7000 above
it: 0x82cb9000 and 0x82cc0000 (`run4-hw/c3/repro-main-6c1f4c9c-kexec-run01.txt`),
0x82d8b000 and 0x82d90000 (`run4-hw/closeout/c3-1b85b315-jetson-kexec-suite-boot08.txt`),
0x835bb000 and 0x835c0000 (`run5-hw/c3/suite-kexec-60c548f3-boot01.txt`).
The critic's 6c1f4c9c rebuild has `__stack_top` 0x82cb86f0 and
`__kernel_end` 0x82cc0000, which reproduces the first pair. The blob is
a copy of `/sys/firmware/fdt` (`scripts/jetson-kexec-slmos.sh`), and the
UEFI-direct capture reports `fdt_size = 0xF39DC` on the same board
(`run5-hw/c3/filt-uefi-1fefbfdd-boot01.txt`), so all but its first 20 to
28 KB is in the heap, with nothing reserving it. Every kexec capture read
(4 run-4 files and all 16 run-5 suite boots) prints `xhci: dtb path probe rc:
/=0 /cpus=-4 /aliases=-4` and `dtb root dump aborted (bad token=...)`
after `pmm_init`, although `dtb_init` found `/chosen` before it. Not
verified: the kexec blob's size, and that its reserve map does not cover
itself. No kernel code writes the blob in place, so this is not shown to
cause #1207; it is a second kexec-only place where PMM state and foreign
data share pages.

**Where the inherited GPU pages are.** The `[ga10b-reserve]` lines of
`run4-hw/closeout/c3-1b85b315-jetson-kexec-suite-boot08.txt` put the
kexec-inherited channel's pages at 0x13c25a000 to 0x13cca0000, among the
task stacks of the same boot (0x13c080000 to 0x13cd80000 in the fault
dump's stack inventory), and the count of free pages at `pmm_init`
differs between boots of one image (15 distinct values in the 16 run-5
suite boots, 1801526 to 1801663 pages). A page the live channel writes that the
reserve list misses would give a per-boot, kexec-only, non-CPU writer.
Hypothesis, untested; it is what the builder's `--no-helper` arm would
test, and the garbage link in d512f40b run05 fits it better than a
count defect does.

## 8. Failure modes, by name

| Row | Result |
|---|---|
| Selective summary | Not found. Every run-5 file is in the tables; the 9 Scheduler Tests stops and the 2 PMM failures per boot are reported. |
| Truncated or tail-only capture | All 22 start at MB1. 11 of 16 suite files and the 3 filtered kexec files end mid-line, after the lines C3 counts (section 2). The 4 cut fault dumps are not complete dumps. |
| Assembled register file | Not found. Dumps were read in the capture bodies. |
| Redefined criterion | Step 1 ran with PMM_DEBUG=ON instead of on base plus C1 (disclosed). No bar was redefined; the #1207 bar is applied as "open". |
| Hypothesis stated as finding | Two: "it needs state from earlier suites" (Entry 15) and "#1228 the same defect" (Entry 25; same signature shown, same cause not). Also "all four captures: a list shorter than its free_counts" (Entry 2, labelled hypothesis) is wrong for d512f40b run05. |
| Stale base | Hardware rows at 5069e658, three C1 commits behind 321a91be; same C3 commit by range-diff. Acceptable for diagnosis rows; none is a proof row. |
| Missing critic | This file. |
| Partial per-CPU table | Not applicable to C3; the cut dumps were passed to C2. |
| Number without a file | Entry 2's "0 of 13 UEFI suite boots" and the "1 of 10" rate have no run-5 file; they are run-4 numbers. |
| Uncommitted tree | `c3/pmm-halt` is pushed at 321a91be; critic worktrees built clean checkouts. |
| Critic reading the claim first | Two leaks, disclosed in section 1. |
| Undisclosed rule break | None found. The slot overrun (boots 11 to 16) and the scp before the claim are disclosed in Entries 22 and 23. |
| Banner hash mismatch | None: 22 of 22 hardware files carry 5069e658; the two QEMU logs carry the hashes in their names. |
| Bar computed from one cell | The rate (1 of 10) is one run-4 cell; no bar depends on it. |
| Precondition from a prior run | The halt's existence and rate on this base rest on run 4 only (section 6). |
| Path never exercised | Uninstrumented filtered loop on hardware; any hardware row at 321a91be; the detector's count-mismatch and wrong-order paths; UEFI-direct full suite. |
| Fix "cannot deadlock" (run 4, C1) | No fix. The instrument panics with `pmm_lock` held by design, debug builds only. |
| PR merged on production rows only (run 4b) | No PR, no merge. |
| Wrong frame read as a fault site (run 4b) | Checked: all four ELRs are a store or a load matching WnR (section 3). The builder's own note that boot01's ELR 0x80012180 is not a fault site was not checked (C2's). |
| pkill by name (run 4) | The critic killed nothing. |
| Critic claiming a board (run 4) | No labctl call. |

## 9. Definition of done

| Item | State |
|---|---|
| First-fault capture with the writer identified, or "open" with captures and the next experiment | "Open". Captures: the four run-4 files, decode verified; 22 run-5 boots with no fault. Next experiment stated. Met. |
| If fixed, a draft PR with a failing-on-old-code test | Not applicable, no fix. |
| The critic reproduced the filtered loop on both heads | QEMU: done, 200 of 200 at each head. Hardware (one Jetson boot per head): **not done**, no lab time. |
| #1207 bar | "Open" with the capture and the experiment: met, with the corrections below. |

## 10. Fixes

1. Post the decode to #1207 and #1228 (the issues still carry the
   "pointer with bit 52 set" reading), with the three-plus-one split of
   the signatures from section 3 and the run-5 rows (0 of 16, 0 of 1200,
   P = 0.185).
2. Reword in `switch-5.md` by a new entry: "needs state from earlier
   suites" and "#1228 the same defect" are hypotheses; d512f40b run05 is
   an overwritten link in the order-12 list, not a short list; "weak
   evidence of a lower rate" becomes "no evidence either way".
3. Next-experiment plan: keep the option-off rate boots first (they are
   the missing run-5 baseline), give the stop rule (29 boots for 5% at
   p = 0.1), then add an arm with the DTB range reserved (#1244) and
   print `[dtb, dtb + totalsize)` at boot, before the `--no-helper` arm.
   Add a light arm that keeps the layout: with the option off, panic
   with the list state when `free_list_pop` fails under a non-zero count.
4. Instrument: add self-test cases for a count mismatch and for a block
   unlinked at the wrong order; print each owner word once; fix the
   `docs/testing.md` pointer in `CMakeLists.txt`.
5. If lab time remains after C2: the critic's filtered loop, one Jetson
   kexec boot at 2196deb5 and one at 321a91be, to close the reproduction
   row.
6. If the branch becomes a diagnostics PR: rerun the hardware rows at
   its head and state in the PR that the default test kernel gains two
   PMM tests (+256 bytes of text, 51 tests on Jetson).
