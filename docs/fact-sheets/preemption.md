# Preemptive Multitasking — Fact Sheet

Timer-driven scheduler ticks, context switches, per-platform mode.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Preemption model (default) | True HW timer | Cooperative (`COOP_PREEMPT=ON`) | True HW timer (`JETSON_HW_TICK=ON`) | True HW timer |
| Preemption model (opt-in) | — | True HW timer via `SECONDARY_PREEMPT=ON COOP_PREEMPT=OFF` | Cooperative via `JETSON_HW_TICK=OFF` (stock-BL31 fallback) | — |
| Timer source | GIC PPI 30 | GIC PPI 26 (CNTHP, Hyp Physical Timer) at EL2/VHE | GIC PPI 26 (CNTHP, Hyp Physical Timer) at EL2/VHE | LAPIC vector 48 |
| Timer driver | `cntp_*_el0` | `cnthp_*_el2` (the `_EL0` names are RES0 from EL2 with HCR_EL2.{E2H,TGE}=1) | `cnthp_*_el2` (same EL2/VHE register-redirect quirk as Pi 5) | LAPIC MMIO |
| Tick rate | 100 Hz | 100 Hz hardware (opt-in) / synthesized at yield (default) | 100 Hz hardware (default) / synthesized at yield (`JETSON_HW_TICK=OFF`) | 100 Hz |
| Timer calibration | QEMU-exact | CNTFRQ_EL0 | CNTFRQ_EL0 | PIT-calibrated |
| ISR handler | `timer_handler` direct | `timer_handler` direct (opt-in); `coop_preempt_maybe_tick` synth (default) | `timer_handler` direct (default); `coop_preempt_maybe_tick` synth (`JETSON_HW_TICK=OFF`) | `lapic_timer_handler` on IST1 stack |
| Context switch trigger | From ISR | From ISR via ELR trampoline (opt-in); from `schedule()` (default) | From ISR via ELR trampoline (default); from `schedule()` (`JETSON_HW_TICK=OFF`) | From ISR (IST1 stack protects overflow) |
| ELR-trampoline (defers `schedule()` to task ctx) | — (no need) | `resched_trampoline` + `maybe_arm_resched_trampoline`, accepts both EL1h and EL2h SPSR | Same trampoline; `cpu_logical_map[]` lookup for dual-cluster MPIDRs; NULL-safe pre-`preempt_init` | — |
| Save: callee-saved GPRs | x19-x30 | Same | Same | RBX/RSP/RBP/R12-R15 |
| Save: FPU / SIMD | v0-v31, FPCR, FPSR | Same | Same (q0-q31 + FPCR + FPSR saved across IRQ in `vectors.S`) | FXSAVE (512 B) |
| `preempt_disabled` flag | Per-CPU | Per-CPU | Per-CPU | Per-CPU |
| Reentrant-schedule gate | ✅ | ✅ | ✅ | ✅ |
| First-timeslice preemption | ✅ `task_entry_trampoline` clears flag | ✅ | ✅ | ✅ |
| Deadline boost | At tick | At tick (opt-in) / at yield (default) | At tick (default) / at yield (`JETSON_HW_TICK=OFF`) | At tick |
| Migration on tick | ✅ | ✅ HW (opt-in) / ✅ cooperative (default) | ✅ HW (default) / ✅ cooperative (`JETSON_HW_TICK=OFF`) | ✅ |
| Work stealing | ON | ON | ON | ON |
| `sched_rebalance_tick()` | Periodic | Periodic | Periodic | Periodic |
| CPU-bound task monopolization | — | — under HW preempt; ✅ will monopolize under coop default | — under HW default; ✅ will monopolize under `JETSON_HW_TICK=OFF` | — |
| CPU 0 idle WFI safety | ✅ timer wakes | ✅ timer wakes (opt-in); coop ticks at yield (default) | ✅ timer wakes (default); spin-yield under `JETSON_HW_TICK=OFF` | ✅ timer wakes |

## Build flags

**Pi 5 — default cooperative, opt-in hardware preemption:**

```bash
make kernel PLATFORM=RASPI5 SECONDARY_PREEMPT=ON \
    EXTRA_KERNEL_CMAKE_ARGS="-DCOOP_PREEMPT=OFF"
```

The default Pi 5 kernel ships `COOP_PREEMPT=ON / SECONDARY_PREEMPT=OFF`
because cooperative is the broadly-validated path and the policy
flip has not yet been audited across every workload.

**Jetson — default hardware preemption, opt-out cooperative:**

```bash
make kernel PLATFORM=JETSON_ORIN_NANO                       # HW preempt (default)
make kernel PLATFORM=JETSON_ORIN_NANO JETSON_HW_TICK=OFF    # cooperative fallback
```

The Makefile sets `JETSON_HW_TICK ?= ON` for
`PLATFORM=JETSON_ORIN_NANO` and `?= OFF` elsewhere, so existing
Pi 5 / QEMU / x86-64 builds are unchanged. Hardware preemption
on Jetson requires the patched tegra234 BL31 from
`tools/tfa-patches/0004-SLM-OS-Jetson-IRQ-routing-patches.patch`,
flashed via R36.4.4 BSP USB-recovery.

## Skipped / Blocked

- **Default-flip policy decision (Pi 5)** — `SECONDARY_PREEMPT=ON`
  works but is not the default build. Flipping requires a broader
  audit (workload regressions, slm forward path, GPU dispatch under
  hardware preempt). Tracked separately.
- **FIQ delivery experiment on Jetson** — `timdiag fiq` subcommand
  disabled; known to crash the EL3 handler by writing ICC_IGRPEN0.
- **Production-fused Jetson modules** — `JETSON_HW_TICK=ON` requires
  the patched BL31 from `tools/tfa-patches/0004-*`, which only
  flashes onto dev-fused boards via USB-recovery. Production-fused
  modules continue to use the cooperative fallback
  (`JETSON_HW_TICK=OFF`) until NVIDIA either ships the GIC group
  fix-up upstream or accepts a downstream patch.

## See also

- `docs/scheduler.md` (narrative)
- `docs/plans/jetson-preemption-restoration-plan.md`
- `kernel/CLAUDE.md` §"ARM64 Hardware Timer IRQs"
- `kernel/CLAUDE.md` §"Secondary-CPU preemption — SECONDARY_PREEMPT"

*Last updated: 9 May 2026*
