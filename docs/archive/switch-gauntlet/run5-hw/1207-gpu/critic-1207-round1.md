# Blind critic verdict: #1207 lab session (2026-11-16/17)

| | Question | Verdict |
|---|---|---|
| (i) | Mechanism supported by the rows | **PASS, qualified.** The rows show GPU-held stale data reaching DRAM at the first MNIST call, removed by an early UFLUSH. "Dirty L2 lines", "buffers nvgpu freed" and "written by the inherit's flush" are inference. The CPU use-after-free exclusion by PMM_UNMAP is not supported by any row. |
| (ii) | Fix addresses the mechanism with no gap | **FAIL.** It removes the observed damage in the one tested configuration (33 of 33 boots). Gaps: a silent skip path that the regression test accepts, no hardware boot of the commit to be merged, no UEFI-direct boot with the fix, and a second writer left in place. |
| (iii) | Bar met as stated | **FAIL.** Halts: 0 in 27, met. "0 poison damage": measured with full-memory scans in only 5 fix boots, and all 5 have a finding. The window build is not a measure of poison damage. CLAIMS.md C8 says this itself. |
| (iv) | Counts correct | **PASS.** Every count in C1 to C10 reproduces from the serial bodies. Five minor discrepancies, listed below. |

Evidence read: `origin/gauntlet-5-hw-jetson:docs/archive/switch-gauntlet/run5-hw/1207-gpu/` (75 captures), `origin/gauntlet/switch-5` Entries 58 to 65, and the code at d887bed9, 364bd06d, c6ef3dfd, 89068471, d7fb1010, 46aa3b13. Counts were taken from the serial body below `# ---- serial ----`, not from the generated header or the tally file.

## Discrepancies

1. **`e1b-elpgoff-kexec-suite-29418281-boot01.txt` is not a capture from power-on.** Header line 4 reads `power-on line (within first 2000 bytes): NOT FOUND`; the body starts at `[0001.246]` (MB2) and holds 0 MB1 lines. C3 counts it as one of "5 boots" without saying so (Entry 61 does). The halt is boot04, a full capture, so C3's conclusion stands on 4 full captures plus 1 partial.
2. **C8 heading "No CPU write to a free page" outruns its rows.** In none of the 9 PMM_UNMAP boots (su-f378d28b x1, su-d887bed9 x3, fixsu x5) did the #1207 zero damage occur: 0 `[pmm-verify] FINDING` other than the self-test, 0 all-zero scan findings after `test_suite_gpu`. "No fault" in boots where the damage did not happen says nothing about who writes it. The four no-fix unmap boots had about a 43 % chance of showing no damage at the baseline rate ((13/16)^4).
3. **The fix branch states that exclusion as fact.** `origin/fix/1207-early-l2-flush:kernel/CLAUDE.md`, the #1207 paragraph under "Free-list consistency": "ELPG, CPU writes (no fault under `PMM_UNMAP`) and the inherited channel VMs ... were ruled out". When 364bd06d was committed the only unmap row was su-f378d28b boot01, which Entry 62 calls "not a valid row" (secondaries panicked, CPU 0 only).
4. **Stale count in the fix.** `ga10b_handoff_reserve.h` (comment above `enum ga10b_l2_flush_early_rc`) and `kernel/CLAUDE.md` both say "3 of 3 boots clean (E3)"; the evidence is 6 of 6.
5. **Inference written as fact in the fix.** `ga10b_bringup.c` (comment above `ga10b_l2_evict_sysmem`), `main.c` and root `CLAUDE.md` say the L2 "holds DIRTY lines for buffers nvgpu freed before the kexec". No row identifies the buffers or shows they were freed; CLAIMS C6 admits that.
6. **C10 omits an unlisted failure.** `test_l1_block_mapping_detection` fails in `po-kexec-suite-76ec16fd-boot01.txt` and `-boot02.txt` (classify.sh "not in known list"). Entry 60 records it; CLAIMS.md does not. It is a diagnostic-build artefact and does not appear at d887bed9 or later.
7. **Branches named in the brief are not on origin.** `diag/1207-window` (c6ef3dfd) and `diag/1207-l2early` (89068471) exist only locally. The saved patch files match the local commits byte for byte apart from index lines, so the evidence is intact.
8. **`trigger-fixsu-kexec.log.txt` has 3 triggers** (cbcb3d7e); the two 46aa3b13 boots have no trigger-log lines.
9. **Pre-suite device writes into free pages are missing from CLAIMS.md.** `po-...-boot01/02` (0x104f6f0c0, 0x104f950c0, non-zero), `s-...-f378d28b` (0x100341000 non-zero; 0x104fad040..0x104fbe0ff, 512 bytes **all zero**) and `su-...-f378d28b` (0x1023e2000..0x1023f61ff) report them at "before the first suite". From d887bed9 on, arming part 2 runs after SMP is up, so that interval is no longer watched and these vanish from the rows. Entries 60 and 62 record them; C8 speaks of "a second writer" as if there were one.

## Counts I reproduced

| Claim | Result |
|---|---|
| C1 | 16 base captures, all with MB1 in the first 2000 bytes and banner `(b548f29d)`. Halts in 05, 11, 12 (`PMM: order 5/5/7 free list head ...`); FINDING 1 in each is "after test_suite_gpu; last suite boundary: after test_suite_oplib_gpu_as". The other 13: `[FAIL] 10 total failure(s)`, classify.sh "not in known list: none". |
| C2 | 5 x 16/3 = 26.7, so 27. Correct. |
| C3 | 5 captures at 29418281, all with `channel bring-up: inherit`; 1 halt (boot04, heads 0x13b740000 and 0x13b760000). Trigger log shows `elpg=0,aelpg=0` on each. See discrepancy 1. |
| C4 | 150 `reserve range` lines; none overlaps 0x13c670000..0x13c950000 (my script). Both VMs: "0 leaves (0 KB), 0 tables" off-DRAM. Only two VMs are walked. |
| C5 | c6ef3dfd boot01: one all-zero free run, 0x13e4fffc0-0x13e500040, after the first MNIST test; halt head 0x13e500000. boot02: two (pushbuf +0x100000, +0x120000); halt head 0x13b860000. No other all-zero run in any capture. |
| C6 | 6 E3 captures, full, banner `(89068471)`; `l2_flush_dirty done after 1 polls` in all; 0 all-zero runs, 0 printed runs after any MNIST test, 0 halts. |
| C7 | 27 captures, full, banner `(d7fb1010)`; 27 `written back and invalidated` lines (polls 1/2/2/1); 27 `[PASS] test_l2_flush_early_ran_before_pmm_init`; 0 panics; 1 `[pmm-verify] FINDING` each (the self-test); 0 all-zero runs; 27 with 10 listed failures; inherit up in 27; the 400-run print cap never reached (max 8 runs). (13/16)^27 = 0.0037. |
| C8 | 8 captures: SMP 5/5, "0 left mapped", 0 PAGE FAULT, 0 halts, `UNEXPECTED 1` in each. The one finding is 64 non-zero bytes at offset 0x102c0 / 0x1102c0 of the block, i.e. X + 0x102c0 for a 1 MB-aligned X. 46aa3b13 x2 names `test_slm_gpu_run_mnist_returns_negative_off_jetson`. |
| C9 | `builder/final3-qemu-unmap-selftest-serial.txt.txt`: fault at ELR 0x400149a0 = the `str` in `pmm_unmap_selftest_store`, "FAR is in a PMM-FREE page". Poison QEMU runs: `UNEXPECTED 0`. Section compare: "all compared sections identical". |
| C10 | `test_component_cleanup_ctx_is_per_task` in po boot02 and su-d887bed9 boot02. Correct, incomplete (discrepancy 6). |

## Judgement

### (i) Mechanism

What the rows show:

- **Timing.** Zeroed free-list heads appear during the first MNIST call (2 window boots), always at pushbuf + 0x100000 to + 0x120000 (7 damaged boots including run 5's).
- **Stale data over newer data.** In c6ef3dfd boot01, line 0x13e4fffc0 was written non-zero by `test_gpu_alloc_large_buffer` and read zero after the MNIST call. Old zeros replaced a later CPU write. This is the best direct sign of a held write, and CLAIMS.md does not use it.
- **Intervention.** With the four UFLUSH ops before `pmm_init`: 0 damaged in 33 boots (6 E3 + 27 fixwin). Without: 3/16 base, 2/3 window, 1/5 E1b. E3 alone is weak (P(0 of 6) = 0.29 at 3/16); the 33 together are not (about 0.001).

Against the alternatives:

- **CPU use-after-free:** excluded only indirectly, because an early GPU flush would not stop a CPU bug. The unmap rows do not exclude it (discrepancy 2).
- **DMA through the inherited channel VMs:** C4 covers the two hand-off VMs in one boot. Other nvgpu VMs (PMU, system) are never walked. The intervention is what argues against live DMA here.
- **ELPG:** 1 halt in 5 with ELPG off. Not required. Sound, on a GPU path that Entry 61 says differs from the baseline (12 s warmup).
- **The GPU tests' own buffers:** boot02's damage touches no test buffer, and an early flush could not prevent a test's own write.

Inference, not observation: that the data sits in the L2 as dirty lines (versus anything else UFLUSH drains); that the inherit's `ga10b_l2_evict_sysmem()` does the write (C5 places it at the test, not at the call; no row hashes around the flush); that the buffers were freed by nvgpu; that 128 bytes is an L2 line.

### (ii) Fix

- **Silent skip, then a late flush.** `ga10b_kexec_l2_flush_early_with` skips when FECS_CURRENT_CTX reads 0 or 0xbadfxxxx, on the assumption "no inherited L2". `ga10b_bringup_inherit` later waits up to 200 ms for the GR PRI domain to wake (#978) and then flushes after `pmm_init`. A kexec boot that reads poison early and wakes later has the old behaviour. Every base boot already prints `runlist submit_base reads poison (lo=0xbadf1100...)` at that point, so poison in some PRI domain at that time is normal. Not seen in 38 boots; all were jetson-nano-2 at its default power mode with ELPG on.
- **The kernel test accepts that case.** `test_l2_flush_early_ran_before_pmm_init` asserts `rc != TIMEOUT`, so SKIP_POISON and SKIP_NO_CTX pass on a kexec boot whose inherit then succeeds.
- **Timeout.** The early flush prints and continues; the inherit's flush then runs after `pmm_init`. Only a test kernel fails on it.
- **The commit to merge was never booted.** There is no capture of 364bd06d. All fix rows are d7fb1010 (fix + window) or cbcb3d7e / 46aa3b13 (fix + poison).
- **UEFI-direct with the fix was never booted.** The SKIP_UEFI branch and `boot_source_is_uefi()` this early are untested on hardware, and the brief's kexec versus UEFI-direct comparison has no row. CLAIMS.md lists these as pending.
- **Second writer.** One non-zero 64-byte record lands in a PMM-free page during the first MNIST call in 10 of 10 snapshot boots, fix or not. "It cannot land on link words" is true and beside the point: it is a device write into RAM the PMM hands out, and would corrupt live data if the page were allocated.
- No path flushes the L2 between the early flush and `pmm_init`; the other `ga10b_l2_evict_sysmem` callers all run later. That part is sound.

### (iii) Bar

- Halts: 0 in 27 against 3 in 16, like for like (halts and verifier findings are whole-memory detectors in both builds). Met.
- Poison damage: the window build watches 4 MB of about 4.5 GB free, only inside `test_suite_gpu`, prints only free-page or all-zero runs, and cannot see zeros over zeros. It cannot see the 0x102c0 record at all (those addresses are 0.2 to 0.8 GB below the pushbuf). 27 to 936 changed lines per fix boot after the first MNIST call sit in non-free pages and are never shown.
- Full-memory scans with the fix: 5 boots, not 27; 5 of 5 with a finding. The scans stop after `test_suite_gpu`, and pages allocated before part-2 arming (about 610,000) are never checked.
- So the stated bar is not met. CLAIMS.md C8 reports that honestly; C7's heading "The fix meets the bar on the window build" should read "the halt criterion".

### Regression test

- `builder/fix-harness-on-b548f29d.txt.txt`: variant A does not compile; variant B (fix's reserve code, old `main.c`) fails 5 assertions at lines 3977 to 3981. `builder/fix-host-harness.txt.txt`: "all OK" at 364bd06d, and the new tests' `[ga10b-l2-early]` output is in the log (lines 515 to 523). As claimed.
- It catches the call being removed or moved after `pmm_init`, and a change to the op order or poll budgets. It does not catch the skip case above, and nothing in the merged tree detects the write-back itself. The suite-boundary free-list walk remains the only hardware detector, at about 1 boot in 5.

## Step-1 derivations (written before reading the claims)

### (a) `diag/1207-poison` d887bed9

- **PMM_UNMAP.** Part 1 (`pmm_poison_arm`, end of `pmm_init`, boot CPU, IRQs masked) builds the NC alias at PA + 0x20_0000_0000, splits the heap's linear map to 4 KB pages and allocates the state tables. From then every free clears the valid bit of the block's L3 entries, broadcasts a TLBI, DC CIVACs through the alias and writes the pattern; every allocation checks, then sets the entries valid. Part 2 (`pmm_poison_arm_rest`, from the harness after SMP, before the first suite) does the same for pages already free, 512 per lock hold. Pages free between `pmm_init` and part 2 are neither unmapped nor checked.
- **CPU store to a free page.** TTBR0 and TTBR1 share the L1 table, so the identity and kernel-high addresses both fault (translation fault). The dump prints FAR, the reason, and `PMM: FAR is in a PMM-FREE page ... linear map: UNMAPPED`, with the ELR of the store.
- **Paths that still reach a free page.** The PMM's NC alias (any code that computes PA + offset); 2 MB blocks not split (`missed`, 0 on Jetson); the image-tail block on Pi 5; a user mapping or kbuf mapping left in place; any page before part 2.
- **Change detection.** Pattern pages: word compare with `0xF4EE<<48 | pa`, link words skipped on a listed head. Snapshot pages: a 64-bit hash of bytes 16..4095 plus an 8-bit checksum per 64-byte line. Bytes 0..15 are never covered; a write of the same bytes (zeros over zeros) is invisible; a changed line is misplaced 1 time in 256.
- **Scan coverage.** Full scans: before the first suite, before `test_suite_oplib_gpu_as`, before and after `test_suite_gpu`. Per-test scans in those two suites check lines 0 and 63 only (46aa3b13 adds line 11). After `test_suite_gpu` only allocation checks and the list walk remain.
- **Cache hiding.** Under PMM_UNMAP no cacheable mapping of a free page exists after the CIVAC, and the alias is Normal NC, so a non-snooping device write lands in DRAM and the next NC read sees it. Argument holds for unmapped pages. Under PMM_POISON alone a scan can miss it; the allocation check cleans first and is exact.

### (b) `fix/1207-early-l2-flush` 364bd06d

- `ga10b_kexec_l2_flush_early()` runs in `kernel_main` after `vmm_init`, before `ga10b_kexec_handoff_register_reserves()` and `pmm_init`. It records `before_pmm_init = pmm_get_total_pages() == 0`.
- Skips: UEFI-direct (no MMIO at all); FECS_CURRENT_CTX == 0; FECS_CURRENT_CTX == 0xbadfxxxx.
- Otherwise writes 1 to BAR0+0x70000, +0x70010, +0x70004, +0x70000 and polls bits 0 and 1 with budgets 100, 2000, 200, 100, 5 us apart. On timeout it prints, sets a mask bit and continues with the remaining ops; no panic.
- Host tests: op order, values, poll counts, timeout budgets, both skips, and a text check of the call order in `main.c`. Kernel test (Jetson only): `ran`, `before_pmm_init`, `rc != TIMEOUT`.
- `ga10b_l2_evict_sysmem()` is the same four ops with a 100-poll budget each, aborting on the first timeout; called from the inherit (after the GR wake loop and the mailbox checks), after each dispatch, and from the oplib, cold and FFI paths.

### (c) Window (c6ef3dfd) and E3 (89068471)

- Hashes each 64-byte line of [pushbuf - 1 MB, pushbuf + 3 MB) at the start of `test_suite_gpu`; after each test, DC CIVAC then re-hash. Prints a run only if a page at either end is PMM-free or the run is all zero, capped at 400 runs, plus a per-test count.
- Cannot see: anything outside the 4 MB; anything before or after the suite; non-zero changes in allocated pages; same-value writes.
- E3 adds the four UFLUSH ops at the top of `ga10b_kexec_handoff_register_reserves()`, with a 20000-poll budget and an abort on timeout.

## What would change the verdict

- **(i) to unqualified:** one no-fix PMM_UNMAP boot in which the zero damage occurs with no page fault; a hash taken immediately before and after the inherit's `ga10b_l2_evict_sysmem()` in a damaged boot.
- **(ii):** hardware boots of 364bd06d itself on kexec and UEFI-direct; the skip made loud, or the inherit refusing to flush dirty lines when the early flush skipped or timed out, with the kernel test asserting DONE on a kexec boot whose inherit succeeds; a row on jetson-nano-1 or at another power mode; the second writer identified and either reserved or stopped.
- **(iii):** 27 fix boots under full-memory scans with 0 unexpected findings, which needs the second writer resolved first; or the brief's bar restated as halts only.
- **(iv):** nothing needed beyond correcting items 1, 3, 4 and 6.
