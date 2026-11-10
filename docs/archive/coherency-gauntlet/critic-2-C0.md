# critic-2-C0.md: partial, code half; hardware half pending

Critic: C0, coherency run 2 (#1193), 2026-11-10. Committed by the
orchestrator verbatim from the critic's report; artifact paths rewritten
from the session scratchpad to `run2-critic/c0/` on this branch.

Heads reproduced from: `fix/boot-path-1193` @ 167330f5,
`fix-boot-1194-1197-1198` @ ad8e4fa7, `coherency-remove-nc-axis` @ 7d500e0e
(pre-rebase f87d9e47), `gauntlet/coherency-1193` @ 6c35db00.

## 1. Critic's own count

**/ci on 167330f5** (`run2-critic/c0/ci/fixboot/summary.txt`, which opens
with `HEAD 167330f54e6e...`): 11/11 steps rc=0.
- `04-test`, `04b-test-x86`, `05-test-aisched`: each ends `PASSED - All tests passed`.
- `06-runtime`: `308 passed; 0 failed` and `328 passed; 0 failed`.
- `07-braces`: `0 violation(s), 0 file(s) unparsed`, 12/12 configurations parsed.
- `08-buildorder`: `All build-order tests passed`.
- `09-jetson-layout`: 22 passes, 0 failures, 0 warnings, `All checks passed.`
- `10-diff`: 38 files, no build artifacts.
- Warnings outside the vendored Lua: only the existing `lua_slm.c`
  -Wcomment/-Waddress lines and the x86 ld notes. None in changed files.
- Extra, because the EFI stub changed: `11-qemu-uefi.log` passed (in place,
  relocated, model delivery 5/5).
- Extra: `12-test-secondary-preempt.log` passed, but the #750 test reports
  IGNORE there (see F6).

**/ci on 7d500e0e** (`run2-critic/c0/ci/removal/summary.txt`, which opens
with `HEAD 7d500e0eae0f...`): 11/11 steps rc=0, with the same pass lines.
Runtime 308 and 328 passed. Braces 0 violations, 12/12 configurations; one
`not checked` file, `scripts/arm-smmu-noshutdown/arm_smmu_noshutdown.h`, a
host header that needs a manual brace read. Layout 22 passes, `All checks
passed.` Diff 108 files.

**Fail-before demonstrations** on 167330f5. Each was one sed mutation, then
`make test` (QEMU ARM64), then a revert; `git status` showed 0 changed files
afterwards. Files: `run2-critic/c0/mut/<name>.diff`,
`<name>-test-output.log`, `<name>-make.log`.

| Issue | Mutation | Failing assertion (verbatim) |
|---|---|---|
| #1198 | `m1-1198`: `msr daif, x6` back to main's `msr daifclr, #0xf` | `FAILED at .../kernel/tests/test_vmm.c:1006` / `Expected: 0x3c0` / `Actual:   0x0` / `[FAIL] test_mmu_enable_restores_caller_daif` |
| #1197 | `m2-1197-noloop`: branch over the DC IVAC loop (main's behaviour: no invalidate) | `FAILED at .../test_vmm.c:988 - mmu_enable invalidated no lines` / `[FAIL] test_boot_caches_off_range_covers_caches_off_writes` |
| #1197 | `m2b-1197-narrow`: range shrunk to the image's last 4 KB (first attempt was a -Werror build failure; it was rerun and only the rerun counts) | `FAILED at .../test_vmm.c:964 - range does not cover the whole image` |
| #1196 | `m3a-1196-short`: page loop stops one page short | `test_vmm.c:1086 Expected: 192 Actual: 128` (keeps_prefix) and `test_vmm.c:1121 Expected: 192 Actual: 128` (dc_civac_changes_nothing) |
| #1196 | `m3b-1196-norestore`: prefix restore store removed | `FAILED at .../test_vmm.c:1099 - kept prefix lost its DRAM contents` |
| #1196 | `m3c-1196-notype`: type check removed from `efi_range_os_owned` | `FAILED at .../test_vmm.c:1218 - !(efi_range_os_owned(busy, sizeof(busy), sizeof(busy[0]), nc, len))` |
| #1194 | none possible | No test exists. On QEMU, main's `cpu_has_smpen()` was a stub returning true, so the defect can only be seen on hardware. |

**Rebase.** `git range-diff origin/main..f87d9e47
origin/fix/boot-path-1193..origin/coherency-remove-nc-axis`
(`run2-critic/c0/rangediff.txt`):
- 37 left vs 35 right non-merge commits; with merges counted, 42 to 40.
- 2 commits dropped: 753e78ad (the CPUECTLR bit 6 change, a duplicate of
  #1194; justified) and f87d9e47 (the ADR sync; the ADR now lives on the
  base; justified).
- 7 commits altered: fd1d98d3 to 46bf52d6 (D2 and D9 dropped, D1 and D11
  kept; justified as covered by #1197 and #1194, see F3); af8d01ba to
  7d500e0e (no longer adds the ADR file; references still renamed;
  justified); 20f98822, 071b03a9, 880febaf, bb02e993, 2f23daf7, 4d6b0674:
  conflict context only, every hunk read, removal semantics unchanged.
- The other 28 commits are `=`.

The diff `fix/boot-path-1193...coherency-remove-nc-axis` contains none of
the four fixes: none of `cpu_has_smpen`, `daifclr, #0xf`, `msr daif, x6`,
`dc ivac, x4`, `vmm_boot_caches_off_range`, `vmm_mmu_enable_*`,
`ncmem_clean_inval_range`, `efi_range_os_owned`,
`maybe_arm_resched_trampoline_on`, `daifclr #0xc` / `#0x3` appears on a `+`
or `-` line except in archived audit text. The diff does not touch mmu.S,
boot.S, efi_stub.c or docs/adr. Its main.c and vmm.c hunks are removal-only
(NC lock and init flag removed, shareability helper, D11 assert).

**ADR 0007**, `git show <ref>:docs/adr/0007-cross-cpu-coherency.md |
sha1sum`: `d1d68952a55d19a5ca0eb49d7c547e20e1dd0838` on
`gauntlet/coherency-1193`, `coherency-remove-nc-axis`,
`fix/boot-path-1193`, and also on f87d9e47. 3/3 identical.

**PR metadata** (`run2-critic/c0/pr1208meta.json`, `pr1209meta.json`):
#1208 open, not a draft, base `main`, head ad8e4fa7, closes #1194, #1197,
#1198. #1209 draft, base `fix-boot-1194-1197-1198`, head 167330f5,
`closingIssuesReferences: []`.

## 2. Builder's claim (quoted from coherency-2.md Entry 4 and the PR bodies)

- "/ci passed on 167330f5 and on the rebased removal branch: all steps,
  plus Pi 5 builds and `test-qemu-uefi`"
- "page loop one page short fails both range tests (192 vs 128 lines);
  restore store removed fails ("kept prefix lost its DRAM contents"); EFI
  type check removed fails `test_vmm.c:1218`. #1198 test with main's `msr
  daifclr, #0xf` restored fails `test_vmm.c:1006 Expected: 0x3c0 Actual:
  0x0`."
- "rebased f87d9e47 to 7d500e0e (40 commits; 98 files +7449/-2210 against
  `fix/boot-path-1193`; 108 files +8418/-2279 against main; before: 99
  files +7579/-2263 against main). 753e78ad and f87d9e47 dropped ...;
  fd1d98d3 reduced to D1 and D11 ...; af8d01ba no longer adds the ADR.
  Files outside the fix set identical to the original commits in 40 of 40"
- "ADR 0007 sha1 d1d68952 on" all three branches.
- "#1197 spans 726a7689 and 30f8022d; #1194 has no before/after capture
  cited ...; #1197's reasoning omits that A76 and A78AE execute DC IVAC as
  DC CIVAC"
- PR #1208: "make test (ARM64; also with SECONDARY_PREEMPT=ON) ...: pass";
  "Two temporary mutations fail it: skipping the loop, and passing zero
  bounds"; "the invalidate would be safe even as a true invalidate, because
  all pre-MMU stores are non-allocating"; hardware 10/10 on pi-5-1, Jetson
  kexec and Jetson UEFI.
- PR #1209: "Fixes #1196"; the mutation table; "/ci on 167330f5, every step
  passed".

## 3. Agree or disagree, per number

| Claim | Verdict |
|---|---|
| /ci green on 167330f5 | Agree (11/11, `ci/fixboot/`) |
| /ci green on 7d500e0e | Agree (11/11, `ci/removal/`) |
| "plus Pi 5 builds" | Not reproduced. A full Pi 5 build is not a `/ci` step; only the RASPI5 brace configurations were parsed. |
| test-qemu-uefi pass | Agree (`11-qemu-uefi.log`) |
| 192 vs 128, "kept prefix lost", `test_vmm.c:1218` | Agree, reproduced verbatim (m3a, m3b, m3c) |
| `test_vmm.c:1006 Expected 0x3c0 Actual 0x0` | Agree, reproduced verbatim (m1) |
| #1197 "skipping the loop" fails | Agree (m2). The zero-bounds mutation was not run; a narrowed range (m2b) also fails. |
| 40 commits; 98 files +7449/-2210; 108 files +8418/-2279; 99 files +7579/-2263 | Agree, all four |
| Dropped and reduced commits | Agree |
| "Files outside the fix set identical ... 40 of 40" | Partly. range-diff shows 7 altered entries. The alterations are conflict context or fix-set only, so the substance agrees, but they are not literally identical. |
| ADR sha1 d1d68952 on 3/3 | Agree |
| "#1197's reasoning omits IVAC as CIVAC" | Partly disagree. The mmu.S header added by 726a7689 states it ("On Cortex-A76 and A78AE DC IVAC is executed as DC CIVAC ... the loader-side clean ... is what rules that out"). The commit message and the #1208 body omit it. |
| "safe even as a true invalidate" | Agree. Caches-off stores do not allocate, so there is no live dirty data to lose. |
| "make test ... also with SECONDARY_PREEMPT=ON: pass" | Literally true but vacuous. The #750 test reports `[IGNORE]` there because CMake drops the flag on QEMU (`CMakeLists.txt:159-166`). Filed #1214. |
| PR #1208 hardware 10/10 counts | Not countable. The builder also found no branch holding these captures. Hardware half. |
| "Fixes #1196" (PR #1209) | GitHub does not link it: `closingIssuesReferences` is empty because the base is not `main`. |

## 4. Reproduction

- Runs: `ci/fixboot/` (HEAD 167330f5) and `ci/removal/` (HEAD 7d500e0e),
  each with `summary.txt` plus one log per step.
- Mutations: `mut/m1-1198*`, `mut/m2-1197-noloop*`, `mut/m2b-1197-narrow*`,
  `mut/m3a-1196-short*`, `mut/m3b-1196-norestore*`, `mut/m3c-1196-notype*`.
- Range-diff: `rangediff.txt`.

**#1197 reasoning, checked in the TRMs.**
- A76 TRM 100798 section 7.2.4
  (`~/slmos-ref/arm/arm-cortex-a76-trm-100798-0401-01.txt:2799`): "DC IVAC
  operations in AArch64 state are treated as DC CIVAC except for permission
  checking and watchpoint matching."
- A78AE TRM 101779 section 2.6.2.4
  (`arm-cortex-a78ae-trm-101779-0003-08.txt:2901-2902`): the same sentence.
- Unconditional, independent of exception level and HCR_EL2.
- Arm ARM DDI 0487 D7.5.9.2 (`arm-ddi0487-Md.txt:476187`) also upgrades EL1
  DC IVAC to CIVAC when EL2 is enabled and HCR_EL2.VM=1; irrelevant here,
  since SLM-OS runs at EL2 with E2H=1 and TGE=1 on both boards.

So mmu_enable's invalidate removes stale clean lines and would write a
stale dirty line back over the caches-off data. Dirty lines are ruled out
per boot path:
- Pi 5: cold reset.
- Jetson kexec: `~/slmos-ref/linux/linux/arch/arm64/kernel/relocate_kernel.S:54-70`
  copies each destination page with the MMU on, then runs
  `dcache_by_myline_op_nosync civac` over it, with no later store to it.
  `kexec -l slmos.elf` (`scripts/jetson-kexec-slmos.sh:1204`) loads PT_LOAD
  memsz. `readelf` on the Jetson ELF shows the data PT_LOAD at 0x80870000
  with memsz 0x2354170, ending at 0x82BC4170 = `__stack_top`. BSS, page
  tables and the boot stack lie inside a cleaned segment. Only the padding
  `[__stack_top, __kernel_end)` lies outside, and nothing writes it.
- UEFI-direct: `efi_dcache_clean_inval` covers the destination. boot.S
  clears SCTLR.C on both the E2H=1 and E2H=0 paths (`boot.S:306-319`), so
  the "C already set" skip cannot fire.

The third option satisfies "state which and why" in substance; the commit's
mmu.S header gives the why. Two caveats: the message claims "a true
invalidate can never discard live dirty data", but on these cores the
instruction is never a true invalidate; and the commit does not say why the
brief's two options were rejected ("clean to PoC before mmu_enable" shares
the dirty-line exposure; only "zero with caches on" avoids it).

**#1196 design**, `fix/boot-path-1193`:
- Order: `ncmem_init` runs after `vmm_init`, D/A unmask, GPU reserves,
  `boot_models_reserve`, `pmm_init` (`main.c:385-423`), and before every
  `ncmem_alloc`. IRQ and FIQ stay masked until after `timer_init`.
- Coverage: the whole 2 MB block, every line (stride from CTR_EL0).
- EFI list: `efi_stub.c:736-743` adds the block only if
  `efi_range_os_owned` holds; the real jetson-nano-2 decision is printed at
  boot (hardware half). `ncmem_init` covers the block regardless.
- Handoff pages: kept from the pre-maintenance DRAM copy. Premise holds:
  arm64 `ioremap` returns NULL for linear-map RAM
  (`~/slmos-ref/linux/linux/arch/arm64/mm/ioremap.c:26-29`, mainline), and
  Linux writes the handoff only through `ioremap_wc`
  (`scripts/arm-smmu-noshutdown/arm_smmu_noshutdown.c:85,120`).
- Acknowledged limit: if the core looks up its caches for Non-cacheable
  reads, the copy includes dirty data.
- Removal branch: `ncmem_init` keeps the whole-block clean for the DMA
  block (`PLATFORM_HAS_NC_REGION`), and `NC_MEM_BASE` stays defined, so the
  EFI-stub clean survives too.
- Not covered by QEMU: neither the `ncmem_init` wiring nor the EFI-list
  entry is checked by QEMU `make test`; only by
  `test_ncmem_boot_maint_covers_nc_block` on Pi 5 and Jetson test kernels
  (hardware half).

## 5. Failure-mode table

| Row | Checked how | Result |
|---|---|---|
| Selective summary | Counted step rc lines and pass lines in the full logs; counted range-diff entries; counted PR closing references | Mismatch found: "Fixes #1196" is not linked. All /ci and mutation numbers match. |
| Truncated capture | All critic logs are whole files. Builder hardware captures: none existed at review | n/a for code half; pending |
| Assembled register file | The brief asked for a register read for #1198; the builder added a test instead, and it fails on main's instruction (m1). No register file exists yet | Pending hardware |
| Redefined criterion | #1197 third option (orchestrator ruled it acceptable; judged above). #1208 evidence to go in a comment instead of the body (orchestrator ruling). Neither addresses "in the commit message" for #1194 | Recorded; F1 |
| Hypothesis stated as finding | #1196 labelled "not observed on hardware"; handoff premise cited from Linux source and checked; IVAC-as-CIVAC confirmed in both TRMs | Pass for code half |
| Stale base | Both CI runs record HEAD = the claimed heads; the removal branch descends from 167330f5 | Pass; banner hashes pending hardware |

## 6. Verdict (code half): **FAIL**

Rows of the definition of done that fail:
1. Component requirement "For each of #1194 ... one test or one recorded
   observation that fails before and passes after, in the commit message."
   #1194, commit 6f0385ec: no test and no observation in the message. A
   before-capture exists
   (`gauntlet-1193-hw-pi5:docs/archive/coherency-gauntlet/c2-hw/pi5-bootlog-R7.txt`,
   build 9a13b9ee, line 112 `[WARN] SMP: SMPEN NOT set ...` and lines
   126-128 `[WARN] CPU n: SMPEN NOT set`). It is not cited in the commit.
2. "closed-on-merge PRs": #1196 is not linked to close on any PR
   (`pr1209meta.json`: `closingIssuesReferences: []`). The fix is to
   retarget #1209 to `main` after #1208 merges, or to confirm the merge
   method carries e4eb9d7a's "Fixes #1196" onto `main`.

Passed in the code half: `/ci` green on both branches; ADR 0007 identical
on 3/3; removal rebased with the four fixes gone and removal logic
unchanged; #1194 function deleted, not renamed; #1197 option chosen and
reasoned correctly for A76 and A78AE; #1196 block cleaned in `ncmem_init`
and listed in the EFI clean, with the DMA half kept on the removal branch;
fail-before shown for #1197, #1198 and #1196's routine.

Not blocking, recorded:
- F3: the redundant DC CVAC "flush" in `vmm_init` survives on both
  branches with its incorrect comment. `c3-audit.md` on the removal branch
  still marks D2 and D9 "Fixed" by commits the rebase dropped.
- F4: "one commit" per issue: #1197 spans 726a7689 and 30f8022d; #1198
  spans f9ef5609 and ad8e4fa7.
- F5: the 726a7689 message's "true invalidate" sentence is wrong for these
  cores (the code comment is right).
- F6: the #750 test never runs in any QEMU configuration, so `/ci` never
  runs it (filed #1214).
- F7: the removal branch has a brace "not checked" header,
  `scripts/arm-smmu-noshutdown/arm_smmu_noshutdown.h`, needing a manual
  read.

Hardware half still to do: boot_test 10/10 on pi-5-1, Jetson kexec and
Jetson UEFI-direct, banner hash 167330f5; the #1196 `NC memory: ...
changed` line and handoff hash per path; the EFI-stub `nc block: OS-owned`
decision; `test_ncmem_boot_maint_covers_nc_block` and
`test_maybe_arm_resched_trampoline_null_safe` on hardware test kernels; the
#1194 after-capture.
