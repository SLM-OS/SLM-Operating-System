# Preemptive Multitasking — Fact Sheet

Timer-driven scheduler ticks, context switches, per-platform mode.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Preemption model (default) | True HW timer | True HW timer (`SECONDARY_PREEMPT=ON COOP_PREEMPT=OFF`, default since #777) | True HW timer (`JETSON_HW_TICK=ON`) | True HW timer |
| Preemption model (opt-in) | — | Cooperative via `SECONDARY_PREEMPT=OFF` + `COOP_PREEMPT=ON PI5_COOP_PREEMPT=ON` | Cooperative via `JETSON_HW_TICK=OFF` (stock-BL31 fallback) | — |
| Timer source | GIC PPI 30 | GIC PPI 26 (CNTHP, Hyp Physical Timer) at EL2/VHE | GIC PPI 26 (CNTHP, Hyp Physical Timer) at EL2/VHE | LAPIC vector 48 |
| Timer driver | `cntp_*_el0` | `cnthp_*_el2` (the `_EL0` names are RES0 from EL2 with HCR_EL2.{E2H,TGE}=1) | `cnthp_*_el2` (same EL2/VHE register-redirect quirk as Pi 5) | LAPIC MMIO |
| Tick rate | 100 Hz | 100 Hz hardware (default) / synthesized at yield (cooperative build) | 100 Hz hardware (default) / synthesized at yield (`JETSON_HW_TICK=OFF`) | 100 Hz |
| Timer calibration | QEMU-exact | CNTFRQ_EL0 | CNTFRQ_EL0 | PIT-calibrated |
| ISR handler | `timer_handler` direct | `timer_handler` direct (default); `coop_preempt_maybe_tick` synth (cooperative build) | `timer_handler` direct (default); `coop_preempt_maybe_tick` synth (`JETSON_HW_TICK=OFF`) | `lapic_timer_handler` on IST1 stack |
| Context switch trigger | From ISR | From ISR via ELR trampoline (default); from `schedule()` (cooperative build) | From ISR via ELR trampoline (default); from `schedule()` (`JETSON_HW_TICK=OFF`) | From ISR (IST1 stack protects overflow) |
| ELR-trampoline (defers `schedule()` to task ctx) | — (no need) | `resched_trampoline` + `maybe_arm_resched_trampoline`, accepts both EL1h and EL2h SPSR | Same trampoline; `cpu_logical_map[]` lookup for dual-cluster MPIDRs; NULL-safe pre-`preempt_init` | — |
| Save: callee-saved GPRs | x19-x30 | Same | Same | RBX/RSP/RBP/R12-R15 |
| Save: FPU / SIMD | v0-v31, FPCR, FPSR | Same | Same (q0-q31 + FPCR + FPSR saved across IRQ in `vectors.S`) | FXSAVE (512 B) |
| `preempt_disabled` flag | Per-CPU | Per-CPU | Per-CPU | Per-CPU |
| Reentrant-schedule gate | ✅ | ✅ | ✅ | ✅ |
| First-timeslice preemption | ✅ `task_entry_trampoline` clears flag and unmasks IRQs (#1047) | ✅ since #1047 (every CPU; CPU 0 only in the cooperative build) | ✅ since #1047 on CPU 0 (the default build does not set `SECONDARY_PREEMPT`, see #1050); every CPU with `SECONDARY_PREEMPT=ON`; hardware check pending | ✅ |
| Deadline boost | At tick | At tick (default) / at yield (cooperative build) | At tick (default) / at yield (`JETSON_HW_TICK=OFF`) | At tick |
| Migration on tick | ✅ | ✅ HW (default) / ✅ cooperative (cooperative build) | ✅ HW (default) / ✅ cooperative (`JETSON_HW_TICK=OFF`) | ✅ |
| Work stealing | ON | ON | ON | ON |
| `sched_rebalance_tick()` | Periodic | Periodic | Periodic | Periodic |
| CPU-bound task monopolization | — | — under the HW default since #1047 (before it, running tasks were never timer-preempted); ✅ will monopolize under the cooperative build | — under HW default; ✅ will monopolize under `JETSON_HW_TICK=OFF` | — |
| CPU 0 idle WFI safety | ✅ timer wakes | ✅ timer wakes (default); coop ticks at yield (cooperative build) | ✅ timer wakes (default); spin-yield under `JETSON_HW_TICK=OFF` | ✅ timer wakes |

## Build flags

**Pi 5 — default hardware preemption, opt-in cooperative:**

```bash
make kernel PLATFORM=RASPI5                                 # HW preempt (default)
make kernel PLATFORM=RASPI5 SECONDARY_PREEMPT=OFF \
    EXTRA_KERNEL_CMAKE_ARGS="-DCOOP_PREEMPT=ON -DPI5_COOP_PREEMPT=ON"   # cooperative
```

Since #777 the Makefile sets `SECONDARY_PREEMPT ?= ON` for
`PLATFORM=RASPI5` and CMake defaults `COOP_PREEMPT` to OFF there. The
cooperative build needs both CMake spellings: the deprecated
`PI5_COOP_PREEMPT` alias also defaults to OFF on Pi 5, and CMake forces
`COOP_PREEMPT` OFF whenever the alias is OFF.

Until #1047, running tasks on Pi 5 and Jetson stayed IRQ-masked
(`task_create` sets `DAIF.I=1` and nothing on the task path cleared
it), so timer IRQs landed only in idle. `task_entry_trampoline` now
unmasks IRQs before the entry function runs. Pi 5 hardware check
(pi-5-2): 10/10 boots, shell at `DAIF=0x0`, CPU 0 ticking in line with
CPUs 1-3, and a 2 s non-yielding Lua loop completing.

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

- **Jetson hardware check of the task-entry unmask** — QEMU and Pi 5
  verified; Jetson pending (#1047).
- **Utilization accounting while a task yield-polls** — the shell
  waits for input by calling `yield()` in a loop, so every tick sees
  it running and CPU 0 reads 100% busy on Pi 5 and Jetson (#1047).
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

*Last updated: 31 October 2026*
