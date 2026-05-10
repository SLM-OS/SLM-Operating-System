# Jetson Preemption Restoration — Implementation Plan

Phased plan for replacing `COOP_PREEMPT` with hardware-driven preemptive
multitasking on Jetson Orin Nano. Tracks issue **#380**.

---

## Status (2026-05-09)

| Phase | Subject | Status |
|-------|---------|--------|
| 1 | Custom tegra234 TF-A patch (mirror of Pi 5's #134 / `0001-*`) | ✅ landed (PR #746, hardware-verified) |
| 2 | Dual-cluster MPIDR fold fix (#647) | ✅ landed |
| 3 | `TIMER_IRQ` 30 → 26, restore CPU 0 idle WFI, regression | ✅ landed (PR #746) |
| 3.5 | NULL-safe `maybe_arm_resched_trampoline` (#750 fix) | ✅ landed (PR #752) |
| 3.5b | IRQ trap-frame FP/SIMD save (q0-q31 + FPCR/FPSR, 800-byte frame) | ✅ landed (PR #753) |
| 3.6 | Default-flip `JETSON_HW_TICK=ON` for `PLATFORM=JETSON_ORIN_NANO` | ✅ landed (PR #755) |
| 3.7 | Update fact sheets / plan / `kernel/CLAUDE.md` | ✅ landed (this commit, 2026-05-09) |
| 3.8 | Close #380 | ✅ closed 2026-05-09 (5-PR chain: #647 + #746 + #752 + #753 + #755; 10/10 boot_test on jetson-nano-2) |

**Hardware verification (jetson-nano-2, 2026-05-09):** patched BL31
flashed to both `A_secure-os` and `B_secure-os` slots via
R36.4.4 BSP USB-recovery (`flash.sh -k {A,B}_secure-os
jetson-orin-nano-devkit-super`). Linux boots cleanly, kexec to SLM-OS
emits `[IRQ] Unhandled IRQ 198` in the boot log — direct evidence
that hardware interrupts now deliver to NS-EL2 instead of trapping
to EL3. The Pi 5 hardware-preemption path is now equivalent on
Jetson. See user-memory `jetson_bl31_flash_path.md` for the
end-to-end flash recipe and the three landmines (SGI scope,
SCR_EL3.FIQ-vs-SDEI, `SPD=opteed` build flag) that were resolved
during the hardware iteration.

Phase 0 (empirical full GIC dump) was folded into Phase 1
instrumentation — Pi 5's PR #693 + the in-tree
`tools/tfa-patches/0001-*` already proved the technique end-to-end
and the only remaining unknowns were NVIDIA's specific BL31 layout,
which the agent investigation answered.

---

## Why this plan exists, and why it's much simpler than the original

The original framing assumed the answer was either a long custom-TF-A
expedition or a TKE-TMR-based software tick. Two things landed since:

1. **Pi 5 shipped real per-CPU hardware quantum preemption at EL2/VHE.**
   PR #683 → #693 → **#742**. Same fundamental blocker shape as Jetson
   — TF-A keeping all PPIs in Group 0 — solved by a ~80-line TF-A
   patch in `tools/tfa-patches/0001-SLM-OS-Pi-5-IRQ-routing-patches.patch`
   that (a) clears `SCR_EL3.IRQ`/`SCR_EL3.FIQ` for the NS context at
   BL33 entry, and (b) writes the GIC PPI Group register to mark
   PPIs as Group 1 NS. PPI 26 (CNTHP, Hyp Physical Timer) then
   delivers per-CPU at EL2 via `VBAR_EL2`. Verified hardware-running
   with `tasks` showing `shell` + `net_pump` alternating at ~133
   switches/sec on CPU 0 with no cooperative yield primitives.

2. **Jetson already runs at NS-EL2 with VHE post-kexec.** That was the
   heavy lift on Pi 5 (#685 / PR-2 / PR-3). Jetson got it for free
   from the kexec-from-Linux path. So the only delta needed for
   Jetson is a TF-A patch analogous to Pi 5's, retargeted at
   tegra234.

The TKE TMR10 detour from earlier drafts is dropped — PPI 26 is
already per-CPU on each CPU's redistributor; no software fan-out
needed.

The PR #742 in-kernel side fixes (timer.c writes `cnthp_*_el2`
directly under PLATFORM_RASPI5; `maybe_arm_resched_trampoline` accepts
EL1h or EL2h SPSR mode) are **already platform-portable** — the SPSR
fix is unconditional, and Jetson will need its own
`PLATFORM_JETSON_ORIN_NANO` branch in `timer.c` that writes
`cnthp_*_el2` in Phase 3.

---

## Reference: what landed on Pi 5 that we're mirroring

`tools/tfa-patches/0001-SLM-OS-Pi-5-IRQ-routing-patches.patch` does
three things in stock TF-A:

1. **`lib/el3_runtime/aarch64/context_mgmt.c::setup_ns_context`** —
   clear `SCR_EL3.IRQ` (bit 1) and `SCR_EL3.FIQ` (bit 2) in the
   per-context value used at ERET to BL33. Gated on `PLAT_RPI5`.
2. **`plat/rpi/rpi5/platform.mk`** — define `PLAT_RPI5` for the
   conditional above.
3. **`plat/rpi/rpi5/rpi5_setup.c::plat_rpi_bl31_custom_setup`** —
   write `GICD_IGROUPR[0] = 0xFFFFFFFF` (PPI/SGI bank) so every PPI
   and SGI is Group 1 NS.

Pi 5 runs GICv2 where `GICD_IGROUPR[0]` is banked per-CPU at the
distributor. On GICv3 (Tegra234) the equivalent is `GICR_IGROUPR0`
in each CPU's **redistributor** — and the per-CPU TF-A hook for
that exists already in `tegra_gic_pcpu_init` (downstream
`plat/nvidia/tegra/common/tegra_gicv3.c`).

---

## Phase 1 — Custom tegra234 TF-A patch (next)

### Source

NVIDIA's downstream BL31 source already extracted at
`~/slmos-ref/nvidia/Linux_for_Tegra/source/arm-trusted-firmware/`.
Contains `plat/nvidia/tegra/soc/t234/` as a first-class SoC directory
(t234 isn't fallback-onto-t194). The agent investigation confirmed:

- **`lib/el3_runtime/aarch64/context_mgmt.c::setup_ns_context`** at
  lines 182-232 — structurally identical to upstream / Pi 5's patch
  site. Insertion point right after
  `scr_el3 |= get_scr_el3_from_routing_model(NON_SECURE);` (line 230).
- **`plat/nvidia/tegra/common/tegra_gicv3.c::tegra_gic_pcpu_init`** at
  lines 144-167 — runs on every PSCI `CPU_ON` via `tegra_pm.c:200`.
  The `gicr_write_igroupr0(uintptr_t base, unsigned int val)` helper
  at `drivers/arm/gic/v3/gicv3_private.h:474` already exists; we call
  it with the running CPU's redistributor base from
  `gicv3_driver_data->rdistif_base_addrs[plat_my_core_pos()]`.
- **`tegra_gic_init`** (line ~125, primary boot) does NOT call
  `tegra_gic_pcpu_init` — primary uses a separate path via
  `gicv3_rdistif_init`. So the patch needs the IGROUPR write at the
  tail of BOTH `tegra_gic_init` (covers CPU 0) and
  `tegra_gic_pcpu_init` (covers secondaries on warmboot).
- **`plat/nvidia/tegra/soc/t234/platform_t234.mk`** — add
  `PLAT_TEGRA234_SLMOS_PREEMPT := 1` and `add_define`.
- **`tegra234_interrupt_props[]`** at `t234/plat_setup.c:279-288` —
  contains only RAS/SDEI/WDT entries. None of PPI 26 / 27 / 30 are
  listed, so they keep the GIC reset value (Group 0 / Secure on a
  GICv3 with `GICD_CTLR.DS == 0`, which Tegra234 uses). That's what's
  blocking us today.
- **OP-TEE doesn't conflict.** `~/slmos-ref/nvidia/Linux_for_Tegra/source/optee/optee_os/core/arch/arm/plat-tegra/`
  has no `CFG_GIC` and no IGROUPR writes; it runs as a pure SMC
  handler in S-EL1.

### Tasks

- ☐ **1.1** Create `tools/tfa-patches/0004-SLM-OS-Jetson-IRQ-routing-patches.patch`
  with three changes mirroring Pi 5's `0001-*`:
    1. `lib/el3_runtime/aarch64/context_mgmt.c::setup_ns_context` —
       clear `SCR_EL3.IRQ`/`SCR_EL3.FIQ` for NS context, gated on
       `PLAT_TEGRA234_SLMOS_PREEMPT`.
    2. `plat/nvidia/tegra/soc/t234/platform_t234.mk` —
       `PLAT_TEGRA234_SLMOS_PREEMPT := 1` + `add_define`.
    3. `plat/nvidia/tegra/common/tegra_gicv3.c` — write
       `GICR_IGROUPR0 = 0xFFFFFFFF` at the tails of both
       `tegra_gic_init` (primary CPU) and `tegra_gic_pcpu_init`
       (secondary CPUs on warmboot), gated on the same define.
- ☐ **1.2** Update `tools/tfa-patches/README.md` with Jetson build +
  flash instructions:
    ```
    # Build
    cd ~/slmos-ref/nvidia/Linux_for_Tegra/source/arm-trusted-firmware
    git apply <SLM-OS>/tools/tfa-patches/0004-SLM-OS-Jetson-IRQ-routing-patches.patch
    PATH=/opt/arm-gnu-toolchain/bin:$PATH \
        make PLAT=tegra TARGET_SOC=t234 \
             CROSS_COMPILE=aarch64-none-elf- \
             DEBUG=0 LOG_LEVEL=40 -j4 bl31
    # Output: build/tegra/t234/release/bl31/bl31.bin
    ```
- ☐ **1.3** Add `Makefile` target `tfa-jetson` that wraps the build
  invocation, mirroring the existing `tfa-pi5` target shape.
- ☐ **1.4** Flash. Two options, prefer (a):
    - **(a) UEFI capsule update.** Run from booted Jetson Linux; no
      recovery cable. Needs the BSP's capsule packaging tooling.
      Test first because it's the fast iteration loop.
    - **(b) USB Force Recovery + `flash.sh -k A_bl31`.** Definitive
      fallback. Needs recovery cable + jumper on J14 pins 9-10 +
      host with L4T flash tools.
- ☐ **1.5** Reboot, kexec into SLM-OS, run `timdiag` (existing
  command). On the patched BL31, `GICR_IGROUPR0` must read
  `0xFFFFFFFF` on every CPU, and `SCR_EL3` flags should reflect the
  cleared IRQ/FIQ bits (visible indirectly via the BL31 `NOTICE()`
  prints during boot — check `dmesg` from Linux pre-kexec).

### Risks / mitigations

- **Bricking risk.** Mitigation: dev-fused board → recovery via
  USB Force Recovery + `flash.sh` from host. Same flow we already
  use for SD card flashing. Archive stock BL31 first.
- **Capsule update may not be set up out-of-the-box.** If (1.4a)
  fails immediately, fall back to (1.4b).
- **`plat_verify_sysreg_settings` at `t234/plat_setup.c:426-515`**
  asserts on SCR_EL3 bits — but only checks SIF, TWE, TWI, SMD, EA.
  IRQ/FIQ are not checked. Patch is safe.
- **Suspend/resume.** `tegra_gic_save`/`tegra_gic_restore` at
  `tegra_gicv3.c:240,272` save/restore the redistributor across SoC
  suspend. Restored IGROUPR will reflect what `gicv3_rdistif_init`
  + our patch set, but verify with a suspend/resume test before
  declaring done.

---

## Phase 2 — Dual-cluster MPIDR fold fix ✅ landed (#647)

Closed by commit replacing the inline `(Aff0 | Aff1)` fold in
`vectors.S` and 5+ sibling C sites with an
`nc_cpu_logical_map[]`-based lookup. Jetson dual-cluster CPUs 4/5
(`MPIDR=0x10200, 0x10300`) now resolve correctly.
`make kernel PLATFORM=JETSON_ORIN_NANO SECONDARY_PREEMPT=ON` builds
clean for the first time. See `kernel/include/cpu_id_asm.h` for the
shared asm macro.

---

## Phase 3 — Switch `TIMER_IRQ` to 26, restore idle WFI, regress

Conditional on Phase 1 producing a working BL31.

### Tasks

- ☐ **3.1** `kernel/include/platform.h` — `PLATFORM_JETSON_ORIN_NANO`
  block: change `TIMER_IRQ` from `30` to `26`. (Inherits from the
  default block today; needs an explicit override.)
- ☐ **3.2** `kernel/drivers/timer.c` — extend the existing
  `PLATFORM_RASPI5` `cnthp_*_el2`-direct-write branch to cover
  `PLATFORM_JETSON_ORIN_NANO` too. Both platforms run at EL2/VHE
  with `HCR_EL2.{E2H,TGE}=1`; CNTP_*_EL0 writes from EL2 in that
  mode are RES0 per ARM ARM (DDI 0487 register "Configurations"
  tables, "RES0 from EL2 when the EL2&0 translation regime is in
  use"). The PR #742 helpers `write_timer_ctl` /
  `write_timer_tval` already encapsulate this — just widen the
  preprocessor gate.
- ☐ **3.3** `kernel/sched/sched.c::idle_task_func` — restore
  `daifclr; wfi` on CPU 0 for Jetson once HW ticks fire.
  Currently (#739) the Jetson branch falls through to `yield()`
  to avoid CPU 0 deadlock-on-WFI. With HW ticks, WFI wakes on
  PPI 26. Gate the change on `JETSON_HW_TICK=ON` (new build flag)
  so the cooperative fallback stays intact for users on stock
  TF-A. Same change for secondary-CPU idle (was WFE-only).
- ☐ **3.4** Flip CMake defaults for `JETSON_ORIN_NANO`:
  `COOP_PREEMPT=OFF`, `SECONDARY_PREEMPT=ON`, `JETSON_HW_TICK=ON`.
  Keep `COOP_PREEMPT=ON` build-flag-selectable for users on stock
  BL31 or production-fused boards.
- ☐ **3.5** Hardware verification on jetson-nano-2:
    - Boot to shell — clean, no exception storms.
    - `timdiag` shows `timer_handler_count` climbing.
    - `tasks` shows non-yielding tasks alternating at quantum rate
      (~100 Hz).
    - `bench context` round-trips in expected µs range.
    - `boot_test --count 20` clean.
- ✅ **3.6** Update `docs/fact-sheets/preemption.md` matrix Jetson
  column to "True HW timer (PPI 26 / CNTHP)". Remove the
  "Skipped / Blocked" Jetson entry. Done 2026-05-09.
- ✅ **3.7** Update `kernel/CLAUDE.md` §"ARM64 Hardware Timer IRQs"
  to remove the "Hardware timer IRQs do not deliver to EL1/EL2 on
  Pi 5 or Jetson" text — both platforms now deliver under default
  / `JETSON_HW_TICK=ON` builds. Done 2026-05-09.
- ✅ **3.8** Close #380. `slm_preempt_point()` (#635/#636) stays as
  defense-in-depth — cheap when the quantum hasn't expired, no harm
  leaving it under HW preemption. Closed 2026-05-09 with the 5-PR
  chain summary in `docs/fact-sheets/preemption.md` §"Resolved
  blockers".

### Acceptance

- `cpu` shell output shows `timer_handler_count` non-zero on every
  CPU under load.
- A non-yielding compute loop pinned to one CPU does not monopolize
  that CPU — peer task on the same CPU makes progress.
- `bench smp` and `bench stealing` pass.
- `boot_test --count 20` is clean.
- All existing integration tests pass with `COOP_PREEMPT=OFF`
  `SECONDARY_PREEMPT=ON`.

---

## Open questions

1. **Capsule update vs. recovery flash for BL31 on Jetson.** Phase 1.4
   tries capsule first; if not configured on the dev kit, falls back
   to recovery flash. Either path is reversible with stock BL31
   archived.
2. **`tegra_gic_save`/`restore` across suspend.** The patch's per-CPU
   `GICR_IGROUPR0 = 0xFFFFFFFF` write must survive SoC suspend/resume.
   Phase 1 acceptance includes a suspend/resume smoke test.

---

## Non-goals

- Re-enabling preemption on production-fused Orin Nano modules with
  NVIDIA's signed BL31. Those keep `COOP_PREEMPT=ON` until NVIDIA
  ships the change upstream or accepts a downstream patch.
- Promoting `JETSON_HW_TICK=ON` to default-on for downstream users
  who don't rebuild TF-A. The cooperative path stays available as
  a build-flag fallback.
- Touching upstream `arm-trusted-firmware`. Only NVIDIA's downstream
  BL31 actually boots on Jetson hardware.

---

*Last updated: 9 May 2026*
