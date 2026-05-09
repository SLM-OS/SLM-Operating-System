# Preemptive Multitasking — Fact Sheet

Timer-driven scheduler ticks, context switches, per-platform mode.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Preemption model (default) | True HW timer | Cooperative (`COOP_PREEMPT=ON`) | Cooperative (`COOP_PREEMPT=ON`) | True HW timer |
| Preemption model (opt-in) | — | **True HW timer** via `SECONDARY_PREEMPT=ON COOP_PREEMPT=OFF` (PR #742) | — (MPIDR-fold collision) | — |
| Timer source | GIC PPI 30 | GIC **PPI 26** (CNTHP, Hyp Physical Timer) at EL2/VHE | CNTPCT_EL0 polled at `schedule()` entry | LAPIC vector 48 |
| Timer driver | `cntp_*_el0` | `cnthp_*_el2` (the `_EL0` names are RES0 from EL2 with HCR_EL2.{E2H,TGE}=1) | `cntp_*_el0` | LAPIC MMIO |
| Tick rate | 100 Hz | 100 Hz hardware (opt-in) / synthesized at yield (default) | 100 Hz synthesized at yield points | 100 Hz |
| Timer calibration | QEMU-exact | CNTFRQ_EL0 | CNTFRQ_EL0 | PIT-calibrated |
| ISR handler | `timer_handler` direct | `timer_handler` direct (opt-in); `coop_preempt_maybe_tick` synth (default) | Handler registered but IRQ never fires | `lapic_timer_handler` on IST1 stack |
| Context switch trigger | From ISR | From ISR via ELR trampoline (opt-in); from `schedule()` (default) | From `schedule()` via `coop_preempt_maybe_tick` | From ISR (IST1 stack protects overflow) |
| ELR-trampoline (defers `schedule()` to task ctx) | — (no need) | ✅ `resched_trampoline` + `maybe_arm_resched_trampoline`, accepts both EL1h and EL2h SPSR | Compiles, panic-gated until MPIDR fold rewritten | — |
| Save: callee-saved GPRs | x19-x30 | Same | Same | RBX/RSP/RBP/R12-R15 |
| Save: FPU / SIMD | v0-v31, FPCR, FPSR | Same | Same | FXSAVE (512 B) |
| `preempt_disabled` flag | Per-CPU | Per-CPU | Per-CPU | Per-CPU |
| Reentrant-schedule gate | ✅ | ✅ | ✅ | ✅ |
| First-timeslice preemption | ✅ `task_entry_trampoline` clears flag | ✅ | ✅ | ✅ |
| Deadline boost | At tick | At tick (opt-in) / at yield (default) | At yield | At tick |
| Migration on tick | ✅ | ✅ HW (opt-in) / ✅ cooperative (default) | ✅ (cooperative) | ✅ |
| Work stealing | ON | ON | ON | ON |
| `sched_rebalance_tick()` | Periodic | Periodic | Periodic at yield | Periodic |
| CPU-bound task monopolization | — | — under HW preempt; ✅ will monopolize under coop default | ✅ will monopolize its CPU | — |
| CPU 0 idle WFI safety | ✅ timer wakes | ✅ timer wakes (opt-in); coop ticks at yield (default) | ❌ WFI deadlocks if shell sleeps — CPU 0 idle spin-yields instead (PR #739) | ✅ timer wakes |

## Default vs opt-in build on Pi 5

The default Pi 5 kernel ships `COOP_PREEMPT=ON / SECONDARY_PREEMPT=OFF`
because cooperative is the broadly-validated path and the policy
flip has not yet been audited across every workload. To build with
hardware quantum preemption:

```bash
make kernel PLATFORM=RASPI5 SECONDARY_PREEMPT=ON \
    EXTRA_KERNEL_CMAKE_ARGS="-DCOOP_PREEMPT=OFF"
```

Hardware verification (pi-5-2): two non-yielding tasks on CPU 0
alternate at ~133 switches/sec; EL0 paths (`usertest`, `mmaptest`,
`userelf`) all green; `bench context` 2166 ns avg. See PR #742 commit
message for full reproducer.

## Resolved blockers

- **#134 — Pi 5 hardware timer IRQs (RESOLVED 2026-05-06).** Custom TF-A `bl31` (PRs #640/#641/#649) clears `SCR_EL3.IRQ/FIQ` and writes `GICD_IGROUPR[0]=0xFFFFFFFF`, restoring NS access to GIC group routing.
- **#672 — GIC pin assertion wedge under NS-EL1 (RESOLVED 2026-05-08).** Closed by the EL2/VHE pivot (#683) — IRQs now route through `VBAR_EL2` on PPI 26 (CNTHP) instead of NS-EL1 PPI 30. The trampoline path was never the wedge; the GIC pin was.
- **#742 — `SECONDARY_PREEMPT` on Pi 5 viable at EL2/VHE (RESOLVED 2026-05-08).** Two surgical fixes: (1) `timer.c` writes `cnthp_*_el2` directly under `PLATFORM_RASPI5` because `CNTP_*_EL0` accesses from EL2 with `HCR_EL2.{E2H,TGE}=1` are RES0 (the VHE redirect only covers `_EL1` register names); (2) `maybe_arm_resched_trampoline` accepts both EL1h (0x5) and EL2h (0x9) as kernel-mode SPSR values.

## Skipped / Blocked

- **Jetson hardware timer IRQs don't deliver** — 8-path investigation (`docs/archive/investigations/jetson-preemption-investigation.md`) confirmed every NS-accessible route is blocked: PPIs, SGIs, SPIs all routed to Group 0; ICC_IGRPEN0 reads trap to EL3; SCR_EL3.FIQ=1 routes FIQ to EL3. Cooperative is the permanent path on Jetson.
- **Jetson CPU 0 idle WFI deadlock** — because timer IRQs don't fire, a CPU-0 WFI never wakes when the shell (pinned to CPU 0) blocks via `task_sleep_ms`. PR #739 makes Jetson CPU 0 idle spin-yield instead of WFI; secondary CPUs still WFE.
- **`SECONDARY_PREEMPT` on Jetson** — ELR-trampoline infrastructure compiled but unsafe due to MPIDR-fold collision on dual-cluster cores 4/5. Boot-time check (`preempt_check_cpu_mpidr`) panics if enabled. Would-be fix: rewrite fold to handle dual-cluster Aff2.Aff1 encoding.
- **Default-flip policy decision (Pi 5)** — `SECONDARY_PREEMPT=ON` works but is not the default build. Flipping requires a broader audit (workload regressions, slm forward path, GPU dispatch under hardware preempt). Tracked separately.
- **FIQ delivery experiment on Jetson** — `timdiag fiq` subcommand disabled; known to crash the EL3 handler by writing ICC_IGRPEN0.

## See also

- `docs/scheduler.md` (narrative)
- `kernel/CLAUDE.md` §"ARM64 Hardware Timer IRQs — cooperative preemption"
- `kernel/CLAUDE.md` §"Secondary-CPU preemption — SECONDARY_PREEMPT"
- `docs/archive/investigations/pi5-preemption-resolution.md`
- `docs/archive/investigations/jetson-preemption-investigation.md`
- `docs/archive/investigations/pi5-secondary-cpu-preemption.md`
- Issues: #99, #134, #672, #742 (Pi 5 — closed); jetson-preemption-investigation issue set; #739 (Jetson CPU 0 WFI)

*Last updated: 8 May 2026*
