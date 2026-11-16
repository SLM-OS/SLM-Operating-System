VERDICT: FAIL — narrowly, on one claim in the text. All eight round-3 findings are resolved or carried plainly, commit 2be38017 changes nothing functional, and every figure the new texts quote reproduces from the raw captures. It fails because the "not new with the padding" claim (PR line 143, `docs/scheduler.md`, f3c0d021's message) rests on two archive boots in which CPUs 4 and 5 had panicked and halted before the first `moved=0` iteration, and no text says so.

Scope: Part A (#1263), `fix/percpu-cacheline` 2be38017, texts only. PR line numbers are those of `CLAIMS-draft-pr-body.md`. Scripts: `pdm.py`, `pdm3.py`, `arch.py` in this directory.

## Findings

1. **MEDIUM — the archive evidence for "It is not new with the padding" comes only from boots with two halted CPUs, undisclosed.** PR line 143; `docs/scheduler.md` "The same one-CPU regime is in the archive on unpadded kernels from before the #1234 change ... so it is not new with the padding"; f3c0d021's commit message.
   - `c2/hw-base/uefi-r2-boot1.txt` (88d4d71b): `[cpu5] KERNEL PANIC` at line 2965, `[cpu5] System halted.` at 3047, `[cpu4] System halted.` at 3125. The first `moved=0` line is 3138, the first failed iteration is 7 (line 3135). Iterations before the halts have `moved` 6 and 4.
   - `c2-fix-critic/base-3b369aef-r2-boot02.txt` (3b369aef): `[cpu5] System halted.` at 1935, `[cpu4] KERNEL PANIC` at 1942, `[cpu4] System halted.` at 2014. Iteration 1 passes (line 1938); the first `moved=0` is line 2020, the first failed iteration is 2.
   - `arch.py` over every `pdm:` capture under `switch-gauntlet/` outside `1263-1207/`: these two are the only files with `moved=0`, and in both the halts precede it. No unpadded boot with all CPUs alive shows the signature (the 822 later runs, and this session's 1400 unpadded iterations, have 0).
   - So "the regime was present before the #1234 change, absent after it, and present again with the padded layout" compares four-CPU boots after two kernel panics with healthy six-CPU boots. The archive lines carry no per-CPU histogram, so "the same one-CPU regime" is also an inference from the line text `moved=0 runs=100/100 bad=0`. The claim softens the behaviour change for a reviewer.
   - Fix: in PR line 143 and `docs/scheduler.md`, say that the identical line occurs in two pre-#1234 boots, both after CPUs 4 and 5 had panicked and halted, so they do not show the regime on a healthy unpadded kernel; drop "so it is not new with the padding" or reduce it to "the same failing line has been seen before, under different conditions". Add f3c0d021's "same regime is in the archive" to the superseded statements in PR line 1.

2. **LOW — two cited files are not on an evidence branch.** PR line 189 cites `A-qemu-arm-make-test-2be38017.txt` and `A-check-braces-2be38017.txt`; line 78 says all `A-*` files are on the two branches. They exist only in `$SP/a/`. Push them before posting. The braces log shows the result but not the command, so `BRACES_BASE=f3c0d021` cannot be read from it.

3. **LOW — #1284's first two comments still say "critic round 2"** (opening lines of comments 1 and 2), an internal-process reference in a public issue. The PR draft and the #1262 draft have none. Fix: edit the two opening lines.

4. **LOW — PR line 1 lists two superseded commit messages, not three.** c3ce3885's message still says the unpinned workers wake "on any CPU". The squash instruction covers it only if followed; name it in line 1 alongside c0612fa7 and f3c0d021.

5. **LOW — "A2c" is used at PR line 160 before line 168 introduces it.** "A2b" and "A2c" are session labels; one defining phrase at first use is enough.

## 1. Commit 2be38017

- Parent f3c0d021. Three files: `docs/perf/context-switch.md` (one table row), `docs/scheduler.md` (one paragraph), `kernel/tests/test_scheduler.c`.
- Every changed line in `test_scheduler.c` is inside the header block comment of `test_preempt_flag_follows_migrated_task`. Checked independently: both revisions through `gcc -fpreprocessed -dD -E -P` give the same md5 (5243e9df...), so the token stream is identical.
- No functional change anywhere.
- "No .text comparison because `__LINE__` shifts" is reasonable: the comment grows by 14 lines above the test body, and the asserts embed line numbers.
- One `make test` plus check-braces is adequate. Log: banner `SLM-OS v0.4.0 build 20261116025446 (2be38017)`, 3051 `[PASS]`, 0 `[FAIL]`, `[PASS] All test suites passed!`, exit code 0; line 7962 `pdm: moved=24 runs=10/10/11 bad=0`, line 7966 PASS. The sorted PASS/IGNORE name list is identical to `A-ci-c3ce3885-step04-test-arm64.txt` (3051 there too; the only diff is two timestamps). Braces: 0 violations, 0 unparsed.
- The five `coherency: FAIL (N rows)` lines (9453, 9465, 9477, 9485, 9505: 12, 4, 4, 1, 12 rows) are expected output. `kernel/src/coherency.c` prints the verdict; the five fault-injection tests in `kernel/tests/test_coherency.c` (`test_inject_p3_stale_partner_fails`, `..._p4_descending_writer_fails`, `..._p4_weak_row_fails`, `..._p6_sum_offset_fails`, `..._mp_stale_flag_fails`) inject a fault and require exactly that line (`test_coherency.c:649`, `:748`). Each is followed by `[INFO] coherency inject N` and `[PASS]`. The c3ce3885 step-4 log has the same five lines with the same row counts.

## 2. Figures re-derived

Old test, 14 boots (`pdm.py`):

| | padded | unpadded |
|---|---|---|
| `moved`, all boots | 8707 | 9071 |
| `moved`, UEFI-direct (per boot) | 3735 (1243, 1306, 1186) | 3963 (1322, 1331, 1310) |
| wake-ups, all boots | 40704 | 13929 (ratio 2.92) |
| wake-ups, UEFI-direct | 21826 | 5969 (ratio 3.66) |
| moved per wake-up, UEFI-direct | 0.149 to 0.208 | 0.615 to 0.713 |
| steals per wake-up, UEFI-direct | 0.793, 0.835, 0.855 | 0.637, 0.700, 0.757 |
| steals per wake-up, kexec | 0.775 | 0.649 |
| steals per wake-up, Pi 5 | 0.195, 0.636, 0.164 | 0.345, 0.359, 0.325 |
| raw steals, UEFI-direct | 6954, 4974, 6181 | 1489, 1515, 1170 |
| raw steals, Pi 5 | 364, 8145, 300 | 716, 743, 658 |
| `moved=0` iterations | 83 (UEFI 31, 3, 29; Pi 5 boot 02: 20) | 0 |
| largest one-CPU share | 92.0, 88.2, 90.7, 95.1% in the four regime boots | at most 56.1% |

- Ratios: Pi 5 regime boot 1.77 to 1.96 (1.8 to 2.0); Jetson UEFI-direct 1.05 to 1.34, kexec 1.19 (1.0 to 1.3); raw totals UEFI-direct 3.28 to 5.94, Pi 5 regime boot 10.96 to 12.38.
- Regime CPUs: 3, 2, 4 on Jetson, 1 on Pi 5. The example line occurs 30 times in UEFI P boot 01. The largest single iterations in unpadded UEFI boots 01 and 02 are 30 and 33 wake-ups per worker.
- Three-worker lines (`pdm3.py`): 13 files, 25,061 wake-ups of `pdm_a` and `pdm_b`, 0 on CPU 0 (14,465 in the three x200 hardware boots, 50 in the two suites, 5,215 and 4,746 in the two QEMU x200 runs, 482 mutation, 18 SLOT_TRACE, 85 in the four `/ci` logs). Old test: 54,633 wake-ups, 0 on CPU 0. The 2be38017 `make test` adds 20, 0 on CPU 0.
- Archive: `uefi-r2-boot1.txt` is UEFI-direct (`Boot source: UEFI-direct (EFI stub)`), complete, summary `runs=200 passed=162 failed=38`; 37 `pdm_moved > 0` failures, 1 `w >= 0` (`moved=4 runs=39/100`). `base-3b369aef-r2-boot02.txt` is kexec (`kexec_core: Starting new kernel`, `Boot source: direct load`), no header, ends at `[ITER 34/200]` with no summary; 13 `pdm_moved > 0`, 2 `w >= 0`. All 50 `moved=0` lines read `runs=100/100 bad=0`.

All of these match the PR draft, the #1284 correction comment, `docs/scheduler.md`, the test header comment and the 2be38017 commit message.

The four disputed figures: the builder is right in each.
- 3.7 times is UEFI-direct only (21826 / 5969); over all boots it is 2.9 (40704 / 13929).
- Jetson steals-per-wake-up ratio is 1.0 to 1.3, not 1.1 to 1.3: the lowest pairing is 0.793 / 0.757 = 1.05.
- Raw totals are 3.3 to 5.9 times on UEFI-direct and 11 to 12 times in the Pi 5 regime boot, not "5 to 8 times".
- `uefi-r2-boot1.txt` has 198 `pdm:` lines and 198 intact `[ITER` lines under a summary of 200: iterations 2 and 3 are interleaved character by character with the cpu4 and cpu5 panic dumps (see finding 1).

## 3. Round-3 findings

| # | Status | Where |
|---|---|---|
| 1 "far fewer cross-CPU resumes" | Resolved | PR 185 ("The padded rows did not have fewer cross-CPU resumes ... 8707 padded against 9071 ... one occurrence cannot be compared across rows"); #1284 third comment, table and "That, not a difference in exposure, is why" |
| 2 "does not rise by multiples" | Resolved | PR 145; `docs/scheduler.md` "rose by at most about 2 times: on Pi 5 ... 0.64 against 0.32 to 0.36"; PR line 1 names f3c0d021's message |
| 3 #1284 title, summary, Bearing | Resolved | title "read preempt_disabled[c] nonzero, c being the cpu_id() it had just read, IRQs open"; summary "Whether the worker was still on CPU `c` ... is not known"; Bearing "a #1266 kernel path ... or the #1266 test ... not decided"; PR 180 same wording |
| 4 `--assert-shares`, ledger row | Resolved | PR 22 "was not measured (`--assert-shares` passes but does not test it)"; `docs/perf/context-switch.md` row "placement only, which is what `bench context` measures; the layout also changed where a woken task runs ... its cost was not measured" |
| 5 "any CPU", CPU 0 | Resolved, with finding 4 | test header "CPU 0 is not covered ... The flag check has never run on CPU 0", "88 to 95%"; PR 153, 159, 160, 196 |
| 6 cut archive file | Resolved as asked; see finding 1 | PR 143 "a kexec boot whose capture is cut: it ends at iteration 34 of 200 with no summary line ... (2 more failed `w >= 0`)" |
| 7 PR wording | Resolved | no "critic" in the PR; 189 quotes the command as run; 185 excepts the mutation run; 195 lists the missing unpadded three-worker row |
| 8 test notes | Carried plainly | test header "Further limits" (FIQ, fewer than 4 CPUs); PR 161, 162; PR 12 "Not purely test-only" |

## 4. Reading as a reviewer

- PR draft: no wrong number found. "Closes #1263" (line 3) is the only closing keyword; #1262, #1284, #1266, #1279, #1274, #1230, #1229, #1234 carry none. The wake-placement change is in bold in "What changes" and has its own section; the cost is listed as not measured; the stack on #1266 is line 5; the squash instruction is line 1; the ledger row "awaits acceptance by the user" (107, and in the row). The one misleading passage is finding 1.
- #1284 as it stands: title, summary, "What the assertion means" and Bearing are neutral between the two readings; the third comment corrects the first two with correct counts. Finding 3 only.
- #1262 comment draft: evidence only, says twice the 2 s failure was not reproduced, no fix claim, no closing keyword. Its figures (109, 24 and 85, 279, 1.03 to 6.70 ms, 14, 4, table cells) agree with the PR.

## Not checkable

- That check-braces ran with `BRACES_BASE=f3c0d021` (the log has no command line).
- What caused the cpu4 and cpu5 panics in the two archive boots, and whether the halted CPUs are what produced `moved=0` there.
- The cause of `bad=1` in #1284.
- Figures not re-derived this round: the R3 table, bench-context means, the 822 archived runs, suite failure names, the 14 / 8 / 265 wait breakdown (checked in rounds 1 to 3).
- Whether #1284's live text on GitHub equals `CLAIMS-issue-1284.txt` (judged from the file).
