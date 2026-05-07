# Pi 5 — EL2 with VHE Refactor Plan

**Status:** Plan stage. Not started.

**Issue:** [#683](https://github.com/SLM-OS/SLM-Operating-System/issues/683) — move SLM-OS to EL2 with VHE on Pi 5 so NS IRQ delivery works.

**Predecessor:** [#672](https://github.com/SLM-OS/SLM-Operating-System/issues/672) — closed; established that NS-EL1 IRQ delivery on Pi 5 is broken regardless of which timer PPI is selected.

**Predecessor:** [#134](https://github.com/SLM-OS/SLM-Operating-System/issues/134) — original "restore Pi 5 hardware timer IRQ delivery" issue.

---

## Why this refactor exists

The Pi 5 hardware-IRQ blocker is **upstream of the kernel**, at the
boundary between EL3 firmware (or GIC-400 SoC integration) and NS-EL1
exception delivery. It is **not** a kernel bug.

#672's investigation produced the following ground truth on pi-5-2 with
the patched TF-A from PR #656 deployed:

| Signal | State | Meaning |
|---|---|---|
| `GICD_ISENABLER0` bit 30 | 1 | Timer PPI enabled in distributor |
| `GICD_ISPENDR0` bit 30 | 1 | Timer pending in distributor |
| `GICC_HPPIR` from NS-EL1 | 30 | Timer **visible at CPU interface** as Group 1 NS — the GIC routing is correct |
| GIC `nIRQ` pin to CPU 0 | asserted | Proven indirectly: `CNTP_CTL=0` or `ICENABLER0` clear unblocks `daifclr`; otherwise it hard-locks |
| EL1 IRQ vector entry | **never** | `vec->irq` stays 0 across all CPUs forever |
| `daifclr #2` at NS-EL1 with pin asserted | hard-lock | CPU 0 wedges with no exception delivered |

PPI 27 (CNTV virtual) and PPI 30 (CNTP physical) were both tested and
both fail identically. **The wedge is not PPI-specific.**

Linux on Pi 5 doesn't hit this because it boots into EL2 with VHE and
takes IRQs at EL2 via `VBAR_EL2`. That is the path Pi firmware actually
validates; NS-EL1 IRQ routing is not. See `arch_timer_select_ppi()` in
`drivers/clocksource/arm_arch_timer.c` — the first branch
(`is_kernel_in_hyp_mode()` → `ARCH_TIMER_HYP_PPI` = PPI 26) is what Pi 5
hits, every time.

The conclusion: stop trying to make NS-EL1 IRQ delivery work and run
SLM-OS where Pi firmware validates IRQ delivery — at EL2 with VHE.

---

## Goal

Run SLM-OS at NS-EL2 with VHE so that:

1. The kernel takes IRQs at EL2 via `VBAR_EL2` — the path Linux uses
   and Pi firmware validates.
2. The timer becomes PPI 26 (Hyp Physical Timer); `CNTP_*_EL1` register
   accesses are silently redirected to `CNTHP_*_EL2` by VHE.
3. Hardware-IRQ delivery becomes real, unblocking `SECONDARY_PREEMPT`
   (#672) and removing the `COOP_PREEMPT` crutch on Pi 5.

VHE (Virtualization Host Extensions) is the load-bearing piece. With
`HCR_EL2.E2H=1` and `HCR_EL2.TGE=1`, a kernel written against the
`*_EL1` ABI runs at EL2h with most EL1 register accesses silently
redirected to their EL2 equivalents. The kernel doesn't need a
register-name rewrite of every `mrs/msr` site — only the explicit
EL1-named ones in vectors / boot / context-save paths need updating.

**Scope clarification:** this is not virtualization. `HCR_EL2.VM=0`,
no stage-2 translation, no nested guests. VHE is used purely for the
register-name redirection so the existing kernel ABI keeps working at
EL2.

---

## Convergence with Jetson

Jetson Orin Nano's bring-up is **already at EL2 with VHE** (see
`docs/jetson-boot.md` and the EL2 setup block in
`kernel/arch/arm64/boot.S` around line 514). The CBB firewall on Jetson
forced this architecture; #683 brings Pi 5 onto the same architecture.

After this refactor lands, both ARM64 hardware platforms run at EL2/VHE
and the kernel has a single primary execution model rather than a
per-platform EL split. QEMU stays at EL1 by default (the QEMU virt
machine doesn't have the Pi 5 firmware quirk, and EL1 builds are still
useful for fast iteration).

---

## What needs to change

### Boot

- `kernel/arch/arm64/boot.S`: stop dropping to EL1 in the Pi 5 path.
  Stay at EL2 after the existing EL2 setup block. The
  `SPSR_EL2`/`ELR_EL2` setup that currently `eret`s to `EL1h` becomes a
  setup for the EL2 "main" entry — load `SPSR_EL2` with `EL2h+DAIF
  masked`, `ELR_EL2` with the kernel main entry, and `eret`.
- Set `HCR_EL2.E2H = 1` (Virtualization Host Extensions — kernel uses
  EL1 ABIs from EL2).
- Set `HCR_EL2.TGE = 1` (general exceptions trap to EL2 — important so
  userspace SVC etc. gets to us at EL2).
- Set `VBAR_EL2` instead of (or in addition to) `VBAR_EL1`. The current
  vector table at `exception_vectors` is laid out for EL1; with E2H+TGE
  the same table format works for EL2, but the symbol name and where
  `boot.S` loads it from need updating.
- `CNTHCTL_EL2` semantics change with `E2H=1` — the bit layout becomes
  the EL1 `CNTKCTL` layout. `EL1PCEN`/`EL1PCTEN` map to
  `EL0PCEN`/`EL0PCTEN`. Re-validate against the ARM ARM (D5).

### Vectors / exception entry

- `kernel/arch/arm64/vectors.S`: `el1_*` handlers stay structurally the
  same but conceptually become "EL2h" handlers. Save/restore registers,
  `eret` semantics work the same. The `mrs/msr` of system registers
  using `*_EL1` names will continue to work via VHE's silent
  redirection, but anything that explicitly references EL1 (e.g.
  `elr_el1`, `spsr_el1`) needs to become `elr_el2` / `spsr_el2`.
- The `resched_trampoline` (gated on `SECONDARY_PREEMPT`) reads/writes
  `elr_el1` and `spsr_el1`. Convert to EL2 equivalents.
- Userspace SVC handling: tasks at EL0 that issue SVC currently go to
  the `EL0_sync` vector (offset 0x400 from `VBAR_EL1`). With us at EL2,
  that becomes the lower-EL-AArch64 sync vector at `VBAR_EL2 + 0x400`.
  The handler dispatch in `el0_sync_handler` may need EL adjustment.
- `kernel/arch/arm64/exceptions.c`: any direct `*_EL1` register reads
  for diagnostic purposes (ESR/FAR/ELR) need EL2 equivalents.
- `kernel/arch/arm64/context.S`: register-context save/restore. Audit
  every `*_EL1` reference.

### Timer

- `kernel/drivers/timer.c`: the physical timer hardware doesn't change,
  but with `E2H=1` the `CNTP_*_EL1` accesses VHE-redirect to
  `CNTHP_*_EL2`. The driver code can keep the `_EL1` register names —
  same hardware register accessed by either name when `E2H=1`. Linux
  uses this property: its driver writes `_EL1` names and relies on VHE.
- `kernel/include/platform.h` Pi 5: `TIMER_IRQ` → 26 (Hyp Physical
  Timer PPI). Drop `PLATFORM_TIMER_USES_VIRTUAL=1` (this issue makes it
  moot for Pi 5 — virtual timer was Linux's HYP-not-available
  fallback, not relevant once we run at HYP).
- `kernel/arch/arm64/exceptions.c`: add a case for
  `HYP_PHYS_TIMER_IRQ = 26` in the IRQ dispatch switch.

### MMU / page tables

- `TTBR0_EL1` → `TTBR0_EL2` (with `E2H=1`, `TTBR0_EL2` has the
  EL1-style layout). `VTCR_EL2` governs second-stage translation but we
  don't use stage-2 (no nested guests).
- `TCR_EL1` → `TCR_EL2` (similar VHE redirection).
- `vmm.c` and `boot.S` MMU setup: use `_EL2` register variants where
  the kernel explicitly names ELs.
- `MAIR_EL1` → `MAIR_EL2`.

### Other ELx-named registers

Audit and convert (or rely on VHE redirection):

- `SCTLR_EL1`, `ESR_EL1`, `FAR_EL1`, `MAIR_EL1`, `ELR_EL1`, `SPSR_EL1`,
  `TPIDR_EL1`.
- `CONTEXTIDR_EL1` — no direct EL2 equivalent. Userspace context
  tracking (currently used for ASID/PCID hints) may need rework or
  can be dropped if not load-bearing.
- `SP_EL1` — we're at EL2h now, `SP_EL2` is in use. Task-switching
  code uses `SP_EL1` for kernel stacks of preempted tasks; this
  becomes `SP_EL2` (the "current EL" SP for EL2h).

### COOP_PREEMPT / SECONDARY_PREEMPT

- `COOP_PREEMPT` stays default ON for Pi 5 throughout the refactor;
  the kernel works without hardware IRQs.
- `SECONDARY_PREEMPT` becomes the path being unblocked. Activation
  is a follow-up step after the refactor lands and hardware IRQ
  delivery is verified.
- Flipping `COOP_PREEMPT` default OFF for Pi 5 is the final
  acceptance step; not in scope of any single PR in the refactor.

---

## 5-PR plan

### PR 1 — Audit + plan (no code change) ✅ MERGED

Landed in #685 (the PR-1 audit was bundled into the same PR as PR-2).



Goal: complete inventory of every `*_EL1` mention in the codebase,
classify each as VHE-redirect-OK vs needs-rewrite-to-`*_EL2`, and
document the EL1 vs EL2 scheme for the codebase.

Deliverable: a section appended to this doc (or a sibling
`docs/pi5-el2-vhe-audit.md`) listing each site with a one-line
disposition. No code change.

Scope of audit:

- `kernel/arch/arm64/*.S` (boot, vectors, smp_boot, context, cache)
- `kernel/arch/arm64/*.c` (exceptions, mmu)
- `kernel/drivers/timer.c`
- `kernel/include/platform.h` (per-platform timer + IRQ defs)
- `kernel/sched/preempt.c` (resched trampoline driver)
- `kernel/vmm.c` (MMU setup)

Output for each: file:line, register name, disposition (`VHE-OK`,
`rewrite-EL2`, `rewrite-add-platform-guard`, `drop`).

### PR 2 — Boot at EL2 with E2H=1 (single-CPU) ✅ MERGED (#685)

Goal: stay at EL2 in `primary_cpu`, set up `VBAR_EL2` +
`HCR_EL2.E2H/TGE`, run scheduler at EL2 on CPU 0 only. Verify shell
still comes up.

Touches: `kernel/arch/arm64/boot.S`, `kernel/arch/arm64/vectors.S`,
the EL1→EL2 register rewrites identified in PR 1 that are necessary
for boot to complete.

**Outcome:** Merged 2026-05-07 as commit `8c27da3f`. pi-5-2 boots to
shell at EL2h with HCR_EL2.E2H=1 (10/10 boot_test). Boot banner
reports "Running at EL2 (VHE) on Raspberry Pi 5". The PR included
the smp_boot.S secondary EL2 block, so PR-3's secondary-CPU work
was effectively scope-collapsed into PR-2; PR-3 then only needed
verification + the per-CPU EL diagnostic.

### PR 3 — SMP at EL2 ✅ (in flight as of 2026-05-07)

Goal: confirm all 4 CPUs come up at EL2h with VBAR_EL2 installed,
and add the per-CPU EL diagnostic that proves it. The bulk of the
secondary-CPU EL2 setup landed with PR-2; PR-3 is the verification
+ diagnostic layer.

Touches: `kernel/sched/smp.c` (per-CPU CurrentEL capture via NC
slot), `kernel/include/smp.h` (cpu_record_current_el /
cpu_get_current_el API), `kernel/src/shell_sys.c` (cpu shell column
showing per-CPU EL), `kernel/arch/arm64/smp_boot.S` (defensive
pre-flip CNTHCTL_EL2 write on Pi 5 secondary path, mirroring the
empirically-load-bearing primary write in boot.S).

**Outcome:** pi-5-2 boots all 4 CPUs at EL2h (`cpu` shell shows
EL2h for all four). 10/10 boot_test. `bench smp` cross-CPU
dispatch passes (smp1/2/3 ran on CPUs 1/2/3). Existing SMP tests
unaffected.

### PR 4 — Timer to PPI 26

Goal: switch the Pi 5 timer to PPI 26 (Hyp Physical Timer). Verify
`vec->irq` increments on CPU 0 — the long-standing 0 from #672.

Touches: `kernel/include/platform.h`,
`kernel/arch/arm64/exceptions.c` (PPI 26 dispatch case),
`kernel/drivers/timer.c` (no register-name change with VHE; just
ensure the PPI number is right).

Acceptance: `irqtest` no longer wedges and reports IRQ delivered.
Per-CPU IRQ counter in `diag` shows non-zero on all CPUs. The
trampoline path runs to completion.

### PR 5 — Userspace EL0 → EL2 SVC

Goal: re-route the syscall path through the EL2 sync vector. Tasks
at EL0 issuing SVC reach the lower-EL-AArch64 sync handler at
`VBAR_EL2 + 0x400`.

Touches: `kernel/arch/arm64/vectors.S` (lower-EL sync handler),
`kernel/arch/arm64/exceptions.c` (`el0_sync_handler` EL adjustments
if any).

Acceptance: userspace tasks (run via `task` shell command) issue
SVCs and the kernel handles them correctly. Existing tests pass.

### Follow-up (not part of #683)

Flip `COOP_PREEMPT` default OFF for Pi 5 once
`SECONDARY_PREEMPT=ON` is verified working with real timer IRQs.
Tracked separately.

---

## Test plan

For each PR:

- `make test` (QEMU ARM64, EL1) — must pass; confirms QEMU path
  unaffected.
- `make kernel PLATFORM=RASPI5` — must build clean.
- Hardware: `boot_test --count 10` on pi-5-2 — must pass (parity with
  pre-refactor reliability).

Specific to PR 2:

- `diag` shows `CurrentEL = 0xC` (EL2h) on Pi 5.
- Single-CPU shell + basic commands work.

Specific to PR 3:

- `cpu` shell shows all 4 CPUs online at EL2h.
- Cross-CPU benchmarks (`bench smp`, `bench stealing`) pass.

Specific to PR 4:

- `irqtest` (without `noirq`) completes and reports `RESULT: IRQ
  DELIVERED`.
- Per-CPU IRQ counter > 0 on all CPUs after `timer_start`.

Specific to PR 5:

- A test that creates an EL0 task, has it issue SVC, and checks the
  return path works.

---

## Out of scope

- Full virtualization / KVM-style nested guests. We just want VHE for
  the EL register-name redirection; `HCR_EL2.VM=0`, no stage-2
  translation.
- Backwards compatibility with the EL1 path on Pi 5 — once this lands,
  Pi 5 builds are EL2-only.
- Changing `COOP_PREEMPT` to be off by default on Pi 5 — that's a
  follow-up once hardware IRQs are proven reliable.
- QEMU at EL2 — QEMU stays at EL1 by default. The EL2 transition is
  gated on `PLATFORM_RASPI5`.
- Jetson — already at EL2/VHE; no changes required, though convergence
  in shared codepaths is welcome.
- x86-64 — irrelevant.

---

## References

- #672 — investigation that produced this conclusion (NS-EL1 IRQ
  delivery broken regardless of PPI).
- #134 — original "Pi 5 hardware timer IRQ delivery" issue.
- `docs/pi5-armstub-track-c.md` — Stage 1 + Stage 2 history (TF-A
  patches, `SCR_EL3` clear).
- `docs/pi5-stage25-irqtest-findings.md` — Stage 2.5 hang
  investigation that closed the EL1-IRQ-delivery hypothesis space.
- `docs/jetson-boot.md` — Jetson EL2/VHE bring-up; precedent for the
  refactor.
- Linux's `arch_timer_select_ppi()` —
  `drivers/clocksource/arm_arch_timer.c`.
- ARM ARM D5 (VHE), G8 (Generic Timer).

---

## PR 1 audit results

This audit was performed against `main` at the head commit at 2026-05-07.
Every `mrs`/`msr` instruction or inline-asm equivalent in the audit
scope that names an `*_EL0`/`*_EL1`/`*_EL2` register is listed below
with a per-site disposition.

### Disposition codes

| Code | Meaning |
|---|---|
| **VHE-OK** | With `HCR_EL2.E2H=1` at EL2h, the `*_EL1` register name silently redirects to the corresponding `*_EL2` register (EL1 layout). No source change needed. |
| **rewrite-EL2** | The register has no VHE redirect; the `mrs`/`msr` operand must change to the explicit `*_EL2` form (e.g., `elr_el1` → `elr_el2`). |
| **platform-guard** | Site needs to compile differently for `PLATFORM_RASPI5` (EL2/VHE) vs other ARM64 platforms (still EL1 on QEMU). Wrap in `#if defined(PLATFORM_RASPI5) && defined(SLMOS_AT_EL2) ...`. |
| **no-change** | Identifier-style register (MIDR, MPIDR, ID_*, CCSIDR, CSSELR) — same encoding from any EL. |
| **drop-or-rewire** | Instruction or surrounding block goes away in the EL2 path (e.g., the EL2→EL1 `eret` block in the Pi 5 boot sequence). |

The set of registers that VHE redirects when `HCR_EL2.E2H=1` (per ARM
ARM D5.2): `SCTLR`, `CPACR`(→`CPTR_EL2`), `TRFCR`, `TTBR0`, `TTBR1`,
`TCR`, `AFSR0`, `AFSR1`, `ESR`, `FAR`, `MAIR`, `AMAIR`, `VBAR`,
`CONTEXTIDR`, `CNTKCTL`(→`CNTHCTL_EL2`). Generic-timer `CNTP_*_EL0`
accesses redirect to `CNTHP_*_EL2` when running at EL2 with E2H=1.

The set of registers that **do not** auto-redirect (and therefore need
explicit EL2 forms): `ELR_EL1`, `SPSR_EL1`, `TPIDR_EL1`, `SP_EL1`.

### `kernel/arch/arm64/boot.S`

Pre-existing EL2-block code (lines 345–365, 514–525, 669–672, 686, 960–994)
already uses `*_EL2` and is unchanged by this refactor.

| Line | Instruction | Register | Disposition | Notes |
|---|---|---|---|---|
| 325 | `mrs x16, midr_el1` | MIDR_EL1 | no-change | Identifier; readable from any EL. |
| 327 | `mrs x16, id_aa64pfr0_el1` | ID_AA64PFR0_EL1 | no-change | Identifier. |
| 512 | `msr cpacr_el1, x10` | CPACR_EL1 | drop-or-rewire | Inside the Pi 5 EL2-init block. With E2H=1 this becomes `CPTR_EL2` (CPACR-format). The whole "set up to drop to EL1h" block (lines 510–589, the `.Lpi5_in_el1` path) is rewritten to "stay at EL2h with E2H=1". |
| 555 | `msr sctlr_el1, x10` | SCTLR_EL1 | drop-or-rewire | Inside the EL1-drop block. Replace with an SCTLR_EL2 init that clears M/C/I (we'll re-enable in `vmm_init`). |
| 559 | `msr spsr_el2, x10` | SPSR_EL2 | drop-or-rewire | Sets `EL1h` for the eret. New code sets `EL2h` (or eliminates the eret entirely if we just `b primary_cpu`). |
| 577 | `msr elr_el2, x10` | ELR_EL2 | drop-or-rewire | Same as above. |
| 716–730 | (block) | — | drop-or-rewire | The Pi 5/QEMU MPIDR-CPU0-check eret-down landing pad. New code path lands here with `CurrentEL == EL2h`. |
| 719 | `mrs x1, mpidr_el1` | MPIDR_EL1 | no-change | Identifier. |
| 726 | `mrs x1, mpidr_el1` | MPIDR_EL1 | no-change | Identifier (QEMU branch — stays EL1, unaffected). |
| 775 | `msr cpacr_el1, x1` | CPACR_EL1 | VHE-OK | After the EL transition, `cpacr_el1` writes redirect to `CPTR_EL2` (CPACR format) when `E2H=1`. |
| 803 | `msr vbar_el1, x1` | VBAR_EL1 | VHE-OK | Redirects to `VBAR_EL2`. Confirm the kernel-side `exception_vectors` table is at the address loaded here. |

### `kernel/arch/arm64/vectors.S`

| Line | Instruction | Register | Disposition | Notes |
|---|---|---|---|---|
| 118 | `mrs x0, elr_el1` | ELR_EL1 | rewrite-EL2 | Save-regs prologue. At EL2h the exception state is in `ELR_EL2`. |
| 119 | `mrs x1, spsr_el1` | SPSR_EL1 | rewrite-EL2 | Same. |
| 142 | `mrs x16, mpidr_el1` | MPIDR_EL1 | no-change | DIAG_BUMP_VEC macro. |
| 162 | `msr elr_el1, x0` | ELR_EL1 | rewrite-EL2 | restore_regs epilogue. |
| 163 | `msr spsr_el1, x1` | SPSR_EL1 | rewrite-EL2 | Same. |
| 434 | `mrs x0, mpidr_el1` | MPIDR_EL1 | no-change | resched_trampoline. |
| 472 | `mrs x0, mpidr_el1` | MPIDR_EL1 | no-change | resched_trampoline post-schedule. |
| 482 | `msr elr_el1, x3` | ELR_EL1 | rewrite-EL2 | Restore preempted task's PC. |
| 483 | `msr spsr_el1, x4` | SPSR_EL1 | rewrite-EL2 | Restore preempted task's PSTATE. |

The vectors are EL-agnostic in shape (16 entries × 0x80, AArch64 format)
so the table itself doesn't need a layout change — only the
`*_EL1`→`*_EL2` operand rewrites listed above. With `HCR_EL2.TGE=1`,
lower-EL synchronous exceptions (SVC from EL0 userspace) take the
"lower-EL AArch64 sync" vector slot at `VBAR_EL2 + 0x400`. Confirm
that's the slot `el0_sync_handler` is currently bound to in the table
layout.

### `kernel/arch/arm64/smp_boot.S`

Secondary-CPU bring-up. Currently this path **does** drop to EL1
unconditionally on Pi 5 (line 79–81 SPSR_EL2/ELR_EL2 set up for
`EL1h` eret). It must be rewritten in lockstep with `boot.S` PR 2/3.

| Line | Instruction | Register | Disposition | Notes |
|---|---|---|---|---|
| 41 | `msr hcr_el2, x0` | HCR_EL2 | platform-guard | Currently sets `RW=1` only. Pi 5 EL2 path must also set `E2H=1, TGE=1`. |
| 45 | `msr cntp_ctl_el0, xzr` | CNTP_CTL_EL0 | VHE-OK | Disable physical timer; redirects to `CNTHP_CTL_EL2` at EL2/E2H. |
| 46 | `msr cntv_ctl_el0, xzr` | CNTV_CTL_EL0 | VHE-OK | Same for virtual timer. |
| 47 | `msr cnthp_ctl_el2, xzr` | CNTHP_CTL_EL2 | no-change | Already explicit EL2. |
| 56 | `msr cptr_el2, x0` | CPTR_EL2 | no-change | Already EL2. |
| 57 | `msr hstr_el2, xzr` | HSTR_EL2 | no-change | Already EL2. |
| 61 | `msr cnthctl_el2, x0` | CNTHCTL_EL2 | no-change | Already EL2. With E2H=1, semantics change to EL1 CNTKCTL layout — re-validate the value being written. |
| 62 | `msr cntvoff_el2, xzr` | CNTVOFF_EL2 | no-change | Already EL2. |
| 66 | `msr cpacr_el1, x0` | CPACR_EL1 | VHE-OK | Redirects to `CPTR_EL2` (CPACR format). |
| 70 | `msr hcr_el2, x0` | HCR_EL2 | no-change | Final HCR_EL2 write before eret. Pi 5 EL2 path drops the EL1 transition; this setup needs to leave E2H=1, TGE=1. |
| 75 | `msr sctlr_el1, x0` | SCTLR_EL1 | drop-or-rewire | Same as boot.S:555. |
| 79 | `msr spsr_el2, x0` | SPSR_EL2 | drop-or-rewire | EL1h target — rewrite or eliminate eret. |
| 81 | `msr elr_el2, x0` | ELR_EL2 | drop-or-rewire | Same. |
| 123 | `msr mair_el1, x1` | MAIR_EL1 | VHE-OK | Redirects to `MAIR_EL2`. |
| 129 | `msr tcr_el1, x1` | TCR_EL1 | VHE-OK | Redirects to `TCR_EL2` (EL1 layout under E2H=1). |
| 132 | `msr ttbr0_el1, x0` | TTBR0_EL1 | VHE-OK | Redirects to `TTBR0_EL2`. |
| 133 | `msr ttbr1_el1, x0` | TTBR1_EL1 | VHE-OK | Redirects to `TTBR1_EL2` (exists under E2H=1). |
| 145 | `mrs x1, sctlr_el1` | SCTLR_EL1 | VHE-OK | Redirects to `SCTLR_EL2`. |
| 149 | `msr sctlr_el1, x1` | SCTLR_EL1 | VHE-OK | Same. |
| 171, 193 | `msr csselr_el1, x0` | CSSELR_EL1 | no-change | Cache-geometry select; same encoding from any EL. |
| 173, 195 | `mrs x4, ccsidr_el1` | CCSIDR_EL1 | no-change | Cache-geometry read. |
| 238 | `msr cpacr_el1, x1` | CPACR_EL1 | VHE-OK | Redirects to CPTR_EL2 (CPACR format). |
| 245 | `msr vbar_el1, x1` | VBAR_EL1 | VHE-OK | Redirects to `VBAR_EL2`. |

### `kernel/arch/arm64/context.S`

No `*_EL1`/`*_EL2` `mrs`/`msr` accesses — context save/restore is
GPR-only via `CTX_*` offsets into `struct cpu_context`. **No changes
needed.**

### `kernel/arch/arm64/mmu.S`

| Line | Instruction | Register | Disposition | Notes |
|---|---|---|---|---|
| 66 | `msr mair_el1, x2` | MAIR_EL1 | VHE-OK | |
| 73 | `msr tcr_el1, x1` | TCR_EL1 | VHE-OK | |
| 87 | `msr ttbr0_el1, x0` | TTBR0_EL1 | VHE-OK | |
| 88 | `msr ttbr1_el1, x0` | TTBR1_EL1 | VHE-OK | |
| 111 | `mrs x4, sctlr_el1` | SCTLR_EL1 | VHE-OK | |
| 125 | `msr sctlr_el1, x4` | SCTLR_EL1 | VHE-OK | |
| 166, 175 | `mrs/msr ..., sctlr_el1` | SCTLR_EL1 | VHE-OK | MMU on/off helpers. |
| 193, 203, 213 | `mrs ..., sctlr_el1 / tcr_el1 / ttbr1_el1` | various | VHE-OK | Diagnostic readbacks. |

### `kernel/arch/arm64/exceptions.c`

| Line | Instruction | Register | Disposition | Notes |
|---|---|---|---|---|
| 270, 319, 418 | `mrs %0, mpidr_el1` | MPIDR_EL1 | no-change | Identifier. |
| 278, 483, 536, 559 | `mrs %0, esr_el1` | ESR_EL1 | VHE-OK | Redirects to `ESR_EL2`. |
| 279, 537 | `mrs %0, far_el1` | FAR_EL1 | VHE-OK | Redirects to `FAR_EL2`. |
| 444 | `mrs %0, ICC_IAR0_EL1` | ICC_IAR0_EL1 | platform-guard | Jetson GICv3 path. The `*_EL1` is a register-encoding name, not an EL choice — the GIC CPU interface is accessed identically from EL2. **Suggest reviewing whether to use `ICC_IAR1_EL1` (Group 1)**: SLM-OS currently uses Group 0, which routes via FIQ; once at EL2/VHE this stays consistent. Pi 5 uses GICv2 MMIO and never enters this path. |
| 458, 465 | `msr ICC_EOIR0_EL1, %0` | ICC_EOIR0_EL1 | platform-guard | Same as above. |

### `kernel/drivers/timer.c`

| Line | Instruction | Register | Disposition | Notes |
|---|---|---|---|---|
| 30 | `mrs %0, cntfrq_el0` | CNTFRQ_EL0 | no-change | Frequency register; same from any EL. |
| 37 | `mrs %0, cntpct_el0` | CNTPCT_EL0 | no-change | Physical counter; same from any EL. |
| 45 | `mrs %0, cntv_ctl_el0` | CNTV_CTL_EL0 | drop | Virtual-timer accessor — not used on Pi 5 once we move to PPI 26 (Hyp Phys). Kept for QEMU/Jetson; gate the Pi 5 build to prefer the physical-timer path. |
| 51 | `msr cntv_ctl_el0, %0` | CNTV_CTL_EL0 | drop | Same. |
| 56 | `msr cntv_tval_el0, %0` | CNTV_TVAL_EL0 | drop | Same. |
| 62 | `mrs %0, cntp_ctl_el0` | CNTP_CTL_EL0 | VHE-OK | Redirects to `CNTHP_CTL_EL2` at EL2/E2H. **This is the load-bearing register for PR 4** — Pi 5 timer driver uses `cntp_*_el0` operands and they Just Work as Hyp Physical Timer accesses under VHE. |
| 68 | `msr cntp_ctl_el0, %0` | CNTP_CTL_EL0 | VHE-OK | Same. |
| 73 | `msr cntp_cval_el0, %0` | CNTP_CVAL_EL0 | VHE-OK | Redirects to `CNTHP_CVAL_EL2`. |
| 78 | `msr cntp_tval_el0, %0` | CNTP_TVAL_EL0 | VHE-OK | Redirects to `CNTHP_TVAL_EL2`. |

### `kernel/include/platform.h`

| Line | Macro | Current value | Disposition | Notes |
|---|---|---|---|---|
| 769 | `TIMER_IRQ` (Pi 5 block) | 30 (CNTP physical PPI) | platform-guard | PR 4 changes to **26** (Hyp Physical Timer PPI). The PPI 27 switch from #672's branch is not in main yet; #683 PR 4 supersedes it directly with PPI 26. |
| 769 | (no `PLATFORM_TIMER_USES_VIRTUAL` for Pi 5) | absent | no-change | Pi 5 didn't define this. (Jetson sets it for CNTV.) |

### `kernel/sched/preempt.c`

No `mrs`/`msr` of `*_EL1`/`*_EL2` registers. The trampoline driver
only manipulates the NC-memory `orig_elr[cpu]` / `orig_spsr[cpu]`
slots and per-CPU flags. The actual register accesses are in
`vectors.S::resched_trampoline` (already covered above). **No
changes needed in this file.**

### `kernel/mm/vmm.c`

| Line | Instruction | Register | Disposition | Notes |
|---|---|---|---|---|
| 592 | `mrs %0, ttbr1_el1` | TTBR1_EL1 | VHE-OK | Diagnostic read in `vmm_validate_*`. Redirects to `TTBR1_EL2`. |

### Out-of-scope but worth flagging

- `kernel/arch/arm64/user_entry.S:28-31` — `msr elr_el1, x0` / `msr
  spsr_el1, x3` / `msr sp_el0, x1` for entering EL0 userspace. With
  us at EL2h+TGE, this entry path goes via the lower-EL-AArch64
  return — operands become `elr_el2` / `spsr_el2` (rewrite-EL2);
  `sp_el0` is unchanged (no EL2 redirect needed for SP_EL0). **PR 5
  scope.**
- `kernel/arch/arm64/setjmp.S` — no EL register accesses; only GPRs
  and SP. No changes.
- `kernel/arch/arm64/armstub8-2712.S` — runs at EL3 before the kernel
  starts; outside #683 scope.
- `kernel/arch/arm64/efi_stub.c` and `kernel/arch/arm64/nvidia_*` —
  Jetson-only paths; already EL2 today (Jetson is the precedent for
  this refactor).

### Summary by disposition

| Disposition | Sites | PR(s) |
|---|---|---|
| **VHE-OK** (compile clean once at EL2/E2H, no source change) | 23 | n/a — verified at runtime in PR 2/3 |
| **rewrite-EL2** (operand `*_EL1` → `*_EL2`) | 6 | PR 2 (vectors.S 4 sites), PR 5 (user_entry.S 2 sites) |
| **drop-or-rewire** (block restructure) | 9 | PR 2 (boot.S EL1 drop), PR 3 (smp_boot.S EL1 drop), PR 4 (timer.c CNTV) |
| **platform-guard** (per-platform compile gate) | 4 | PR 2 (HCR_EL2 setup), PR 4 (TIMER_IRQ + ICC_*_EL1) |
| **no-change** | 12 | n/a |

The number of source sites that genuinely need editing is small (≤19);
the bulk of the refactor is in two structural changes (`boot.S` and
`smp_boot.S` EL1-drop blocks) plus the timer-driver re-validation. The
"long tail" of `*_EL1` mentions in MMU/SCTLR/TTBR/MAIR/ESR/FAR code
is all VHE-OK and compiles unchanged.

### Decisions captured for PR 2+

1. **Use VHE register-name redirection wherever possible.** Don't
   rewrite a `msr sctlr_el1, x0` to `msr sctlr_el2, x0` if the source
   site is shared with QEMU (which stays at EL1). VHE makes the same
   source compile and execute correctly at both ELs.
2. **The `EL1` operand stays in `*_EL1` form for shared sites; only
   sites that are explicitly Pi 5 EL2-only get rewritten.** This
   keeps QEMU/x86-64/Jetson paths from drifting.
3. **Vectors.S `elr_el1`/`spsr_el1` is the one rewrite that's
   unavoidable** — no VHE redirect for these. Either platform-guard
   the operand (`#if defined(SLMOS_AT_EL2) ... elr_el2 ... #else ...
   elr_el1 ... #endif`) or pick one EL globally for the kernel and
   rewrite everywhere. **Recommendation: platform-guard via a single
   `kernel/include/arch/arm64/el_regs.h` macro** like
   `KERN_ELR` / `KERN_SPSR` that expands to either `elr_el1` /
   `spsr_el1` or `elr_el2` / `spsr_el2`. PR 2 introduces the macro;
   subsequent platforms can opt in by defining the gate.
4. **`HCR_EL2.E2H` flip happens once, in primary boot, before any
   `*_EL1` register write.** The Jetson EL2 block already does this
   correctly; the Pi 5 path inherits the pattern.
5. **`CNTHCTL_EL2` value re-validation is a PR 2 task.** Under E2H=1
   the bit layout becomes EL1 `CNTKCTL` semantics — confirm the
   value being written is correct under the new interpretation.

---

*Last updated: 2026-05-07.*
