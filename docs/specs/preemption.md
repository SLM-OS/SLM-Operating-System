# Preemptive Multitasking — Fact Sheet

Timer-driven scheduler ticks, context switches, per-platform mode.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Preemption model | True HW timer | Cooperative (`COOP_PREEMPT`) | Cooperative (`COOP_PREEMPT`) | True HW timer |
| Timer source | GIC PPI 30 | CNTPCT_EL0 polled at `schedule()` entry | CNTPCT_EL0 polled at `schedule()` entry | LAPIC vector 48 |
| Tick rate | 100 Hz | 100 Hz synthesized at yield points | 100 Hz synthesized at yield points | 100 Hz |
| Timer calibration | QEMU-exact | CNTFRQ_EL0 | CNTFRQ_EL0 | PIT-calibrated |
| ISR handler | `timer_handler` direct | Handler registered but IRQ never fires | Same | `lapic_timer_handler` on IST1 stack |
| Context switch trigger | From ISR | From `schedule()` via `coop_preempt_maybe_tick` | Same | From ISR (IST1 stack protects overflow) |
| Save: callee-saved GPRs | x19-x30 | Same | Same | RBX/RSP/RBP/R12-R15 |
| Save: FPU / SIMD | v0-v31, FPCR, FPSR | Same | Same | FXSAVE (512 B) |
| `preempt_disabled` flag | Per-CPU | Per-CPU | Per-CPU | Per-CPU |
| Reentrant-schedule gate | ✅ | ✅ | ✅ | ✅ |
| First-timeslice preemption | ✅ `task_entry_trampoline` clears flag | ✅ | ✅ | ✅ |
| Deadline boost | At tick | At yield | At yield | At tick |
| Migration on tick | ✅ | ✅ (cooperative) | ✅ (cooperative) | ✅ |
| Work stealing | ON | ON | ON | ON |
| `sched_rebalance_tick()` | Periodic | Periodic at yield | Same | Periodic |
| CPU-bound task monopolization | — | ✅ will monopolize its CPU | ✅ same | — |

## Skipped / Blocked

- **#99, #134 — Pi 5 hardware timer IRQs don't deliver.** GIC-400's Group register for PPI 30 is owned by EL3 firmware; Non-secure writes are silently ignored. Every NS-accessible path exhausted. `COOP_PREEMPT` is the permanent workaround; hardware restoration blocked on firmware change.
- **Jetson hardware timer IRQs don't deliver** — same class of blocker as Pi 5 but different root cause. 8-path investigation (`docs/jetson-preemption-investigation.md`): PPIs, SGIs, SPIs all routed to Group 0; ICC_IGRPEN0 reads trap to EL3; SCR_EL3.FIQ=1 routes FIQ to EL3.
- **`SECONDARY_PREEMPT` on Jetson** — ELR-trampoline infrastructure compiled but unsafe due to MPIDR-fold collision on dual-cluster cores 4/5. Boot-time check (`preempt_check_cpu_mpidr`) panics if enabled. Would-be fix: rewrite fold to handle dual-cluster Aff2.Aff1 encoding.
- **`SECONDARY_PREEMPT` on Pi 5** — compiled but inert (timer IRQs don't deliver, so the trampoline path never runs). Kept for future hardware-IRQ restoration.
- **FIQ delivery experiment on Jetson** — `timdiag fiq` subcommand disabled; known to crash the EL3 handler by writing ICC_IGRPEN0.

## See also

- `docs/scheduler.md` (narrative)
- `kernel/CLAUDE.md` §"ARM64 Hardware Timer IRQs — cooperative preemption"
- `kernel/CLAUDE.md` §"Secondary-CPU preemption — SECONDARY_PREEMPT"
- `docs/pi5-preemption-resolution.md`
- `docs/jetson-preemption-investigation.md`
- `docs/archive/investigations/pi5-secondary-cpu-preemption.md`
- Issues: #99, #134 (Pi 5); jetson-preemption-investigation issue set

*Last updated: 18 April 2026*
