# Resolved Blockers — Historical Record

Closed-issue history that used to live inline in the fact sheets.
Captured here so the fact sheets can stay at-a-glance current-state.
Listed by subsystem, newest first.

---

## Preemptive Multitasking

- **#380 — Jetson hardware quantum preemption (RESOLVED 2026-05-09).**
  Five-PR chain landed end-to-end:
  - PR #647 replaced the legacy `(Aff0|Aff1)` MPIDR fold with a
    `cpu_logical_map[]` lookup so dual-cluster CPUs 4/5
    (`MPIDR=0x10200, 0x10300`) resolve correctly.
  - PR #746 introduced the patched tegra234 BL31
    (`SCR_EL3.IRQ/FIQ` cleared for NS context, `GICR_IGROUPR0=0xFFFFFFFF`
    written from both `tegra_gic_init` and `tegra_gic_pcpu_init`)
    and the in-kernel `JETSON_HW_TICK` build gate.
  - PR #752 added the NULL-safe entry guard to
    `maybe_arm_resched_trampoline` (the actual #750 root cause —
    Linux's xudc IRQ 198 fires the moment `mmu_enable` unmasks IRQs
    and dereferences `reschedule_pending` before `preempt_init`
    allocates it; symptom was a BL31 power-off via DFSC=0x17 / RAS).
  - PR #753 expanded the IRQ trap-frame to save q0-q31 + FPCR + FPSR
    (272 → 800 bytes) so AAPCS64 caller-save clobbers in
    `el1_irq_handler` can't corrupt interrupted-task FP state.
  - PR #755 flipped `JETSON_HW_TICK=ON` to default for
    `PLATFORM=JETSON_ORIN_NANO`.

  Hardware verification: `boot_test --count 10` 10/10 on
  jetson-nano-2 (2026-05-09).

- **Jetson MPIDR-fold collision (`SECONDARY_PREEMPT` Jetson-side,
  RESOLVED 2026-04 via #647).** The asm sites in `vectors.S`
  (`DIAG_BUMP_VEC` and `resched_trampoline`) now use the shared
  `ARM64_GET_LOGICAL_CPU` macro from `kernel/include/cpu_id_asm.h`;
  C-side fold sites use `cpu_logical_id()`. `preempt_check_cpu_mpidr`
  remains as a boot-time invariant check.

- **Jetson CPU 0 idle WFI under HW ticks (RESOLVED 2026-05-09 via
  PR #746 / #755).** PR #739's spin-yield workaround for the
  cooperative-only path was retained behind `JETSON_HW_TICK=OFF`;
  under the default HW-tick build, CPU 0 idle does the standard
  `daifclr; wfi` because PPI 26 wakes it.

- **#742 — `SECONDARY_PREEMPT` on Pi 5 viable at EL2/VHE (RESOLVED
  2026-05-08).** Two surgical fixes:
  1. `timer.c` writes `cnthp_*_el2` directly under `PLATFORM_RASPI5`
     because `CNTP_*_EL0` accesses from EL2 with
     `HCR_EL2.{E2H,TGE}=1` are RES0 (the VHE redirect only covers
     `_EL1` register names).
  2. `maybe_arm_resched_trampoline` accepts both EL1h (0x5) and
     EL2h (0x9) as kernel-mode SPSR values.

  Hardware verification (pi-5-2): two non-yielding tasks on CPU 0
  alternate at ~133 switches/sec; EL0 paths (`usertest`, `mmaptest`,
  `userelf`) all green; `bench context` 2166 ns avg.

- **#672 — GIC pin assertion wedge under NS-EL1 (RESOLVED
  2026-05-08).** Closed by the EL2/VHE pivot (#683) — IRQs now
  route through `VBAR_EL2` on PPI 26 (CNTHP) instead of NS-EL1
  PPI 30. The trampoline path was never the wedge; the GIC pin
  was.

- **#134 — Pi 5 hardware timer IRQs (RESOLVED 2026-05-06).** Custom
  TF-A `bl31` (PRs #640/#641/#649) clears `SCR_EL3.IRQ/FIQ` and
  writes `GICD_IGROUPR[0]=0xFFFFFFFF`, restoring NS access to GIC
  group routing.

---

## SMP

- **#166 — Jetson `pmm_free_pages` page fault under `bench
  stealing` (closed 2026-04-15).** Root cause was Jetson's
  hardcoded `SPINLOCK_SKIP_LOCKING` making every cacheable
  spinlock a no-op. Fixed by adopting Pi 5's
  `spinlock_hw_enabled` runtime-flag model (set by `vmm_init`
  after MMU enable).

- **#158 — Pi 5 boot hang with `WORK_STEALING=ON` (closed).**
  Root cause was the steal-deque lock living in NC memory
  (LDAXR/STXR never actually ran on NC pages on BCM2712). Fixed
  via external cacheable lock (`steal_deque_lock[MAX_CPUS]`).

---

*Last updated: 9 May 2026*
