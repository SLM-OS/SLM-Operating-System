# #1207 lab session (2026-11-16/17): claims for the critic

All files named below are in this directory (`gauntlet-5-hw-jetson`,
`docs/archive/switch-gauntlet/run5-hw/1207-gpu/`) unless a branch is named.
Base: main b548f29d. Board: jetson-nano-2. Log: `gauntlet/switch-5`,
`docs/archive/switch-gauntlet/switch-5.md`, Entries 58 onward.

## C1. Baseline rate (main b548f29d, unmodified test kernel, kexec)

3 PMM halts in 16 kexec suite boots: `base-kexec-suite-b548f29d-boot01..16.txt`,
halts in boots 05, 11, 12 (`PMM: order N free list head ... has next link 0x0
with M blocks counted`). In each halt, the free-list walk after
`test_suite_oplib_gpu_as` is clean and the walk after `test_suite_gpu` reports
a listed free-block head whose links and first 64 bytes read zero. The other 13
boots complete with the 10 failures of `scripts/hw-suite/known-failures-jetson-1109.txt`
(main) and nothing unlisted.

## C2. Bar arithmetic

5 x (16 / 3) = 26.7, so at least 27 kexec boots with the fix, with 0 halts and
0 damage.

## C3. ELPG is not required for the damage

E1b (`diag/1207-elpg` 29418281 = main + accept FECS/GPCCS mailbox0 = 0x4;
patch `e1b-diag-patch-29418281.diff.txt`), ELPG and AELPG set to 0 on Linux
before kexec (trigger log `trigger-e1b-kexec.log.txt`): 5 boots with the
inherited MNIST channel up (`e1b-elpgoff-kexec-suite-29418281-boot01..05.txt`),
1 halt (boot 04) with the same signature.

## C4. The inherited channel VMs do not map the damaged window

`vmwalk-kexec-suite-d4682dc3-boot01.txt` (patch `vmwalk-diag-patch-d4682dc3.diff.txt`):
0 off-DRAM leaves or tables in either walked VM; no reserved range in
pushbuf + 0x20000 .. + 0x300000.

## C5. The damage happens during the first MNIST call

Window diagnostic (`diag/1207-window`, patch `window-diag-patch-c6ef3dfd.diff.txt`):
`win-kexec-suite-c6ef3dfd-boot01.txt` and `-boot02.txt` both halt, and in both
the only all-zero free-page lines in the window change after
`test_slm_gpu_run_mnist_returns_negative_off_jetson` (pushbuf + 0x120000 in
boot01; + 0x100000 and + 0x120000 in boot02). That test's first MNIST call is
where the inherit (and its `ga10b_l2_evict_sysmem()`) runs.

## C6. Mechanism (supported by the rows, the buffer identity not observed)

Dirty GA10B L2 lines left by Linux are written back over pages the PMM owns by
SLM-OS's first L2 flush. Rows: E3 (`diag/1207-l2early` 89068471 = window
diagnostic + the four UFLUSH ops before `pmm_init`; patch
`e3-l2early-patch-89068471-over-window.diff.txt`): 6 of 6 kexec boots
(`e3-kexec-suite-89068471-boot01..06.txt`) with no all-zero free-page line after
the first MNIST call and no halt.

## C7. The fix meets the bar on the window build

`fix/1207-early-l2-flush` 364bd06d; bar image `diag/1207-fix-window` d7fb1010 =
364bd06d + the window commits. 27 kexec boots
(`fixwin-kexec-suite-d7fb1010-boot01..27.txt`, tally `bar-tally-fixwin-d7fb1010.txt`):
27 print `GPU L2 written back and invalidated before pmm_init`; 27 pass
`test_l2_flush_early_ran_before_pmm_init`; 0 halts; 0 `[pmm-verify] FINDING`
other than the self-test; 0 all-zero free-page window runs; 27 complete with the
10 listed failures. P(0 of 27 | p = 3/16) = 0.004.

## C8. No CPU write to a free page; a second writer the fix does not cover

Snapshot+unmap builds (`diag/1207-poison` d887bed9; `diag/1207-fix-poison`
cbcb3d7e and 46aa3b13 = d887bed9 + the fix [+ a per-test line-0x2c0 scan]):
`su-kexec-suite-d887bed9-boot01..03.txt`, `fixsu-kexec-suite-cbcb3d7e-boot01..03.txt`,
`fixsu-kexec-suite-46aa3b13-boot01..02.txt`. In all 8 boots: SMP 5/5, every
free page unmapped, no page fault, no halt. In all 8 boots one non-zero 64-byte
line changes at offset 0x102c0 of a 1 MB-aligned region, in a page neither GPU
VM maps; in the two 46aa3b13 boots the per-test scan names
`test_slm_gpu_run_mnist_returns_negative_off_jetson`. With the fix
(cbcb3d7e, 46aa3b13) it still appears. It is always at page offset 0x2c0, so it
cannot land on a free block's link words. **So "0 poison damage", measured with
full-memory scans, is not met with the fix**; the #1207 halt mechanism is (C7).

## C9. Diagnostic-build proofs (QEMU, builder)

Files in `$S/b/` of the session (not on this branch unless copied): a CPU write
to a freed page faults with the writer's ELR and "FAR is in a PMM-FREE page";
raw writes through the PMM alias are reported by allocation and scans; options
off, sections identical to b548f29d on six images at 187dad1b.

## C10. Unlisted failure

`test_component_cleanup_ctx_is_per_task` failed in `po-kexec-suite-76ec16fd-boot02.txt`
and `su-kexec-suite-d887bed9-boot02.txt` (both PMM_POISON builds): filed #1290 as a
test defect (freed ctx page legitimately reused).

## Rows pending when this file was written

UEFI-direct path control (46aa3b13 UEFI image x5), the fix's integration suite
x3 per Jetson path, `boot_test --count 10` per path with the production fix.
