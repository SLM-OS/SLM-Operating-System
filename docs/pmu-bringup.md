# ARM PMU Bring-Up — Status by Platform

Captures the per-platform state of ARMv8-A Performance Monitor Unit
(PMU) access for SLM-OS. Read by the per-op profile harness
(`runtime/src/inference/engine.rs` after #871 lands) and the AI
scheduler `cache_pressure` slot (`kernel/sched/ai/ai_state.c` after #872
lands).

Parent issue: #60. Implementation: #874 (primitives) + #875 (Jetson
verification) + #871 (profile integration) + #872 (cache_pressure
wiring).

---

## Primitives (#874)

- `kernel/include/pmu.h` + `kernel/arch/arm64/pmu.c`.
- Six event counters preset to architectural ARMv8-A encodings:
  L1D_CACHE_REFILL (0x03), L2D_CACHE_REFILL (0x17), INST_RETIRED (0x08),
  BR_MIS_PRED (0x10), MEM_ACCESS (0x13), STALL_BACKEND (0x24).
- Per-CPU init: `pmu_enable_self()` runs from `kernel_main` on the
  primary CPU after `timer_init`, and from `secondary_init` on each
  secondary after `timer_percpu_init`.
- Cycle counter is in 64-bit mode where supported (PMCR_EL0.LC=1).
  Event counters stay 32-bit; the profile harness resets per-op to
  bound overflow exposure to microseconds.

## Diagnostic surface (#875)

- Shell command: `pmu [probe [<iters>]]`. Prints PMCR_EL0 state, runs
  a 100k-iter (default) loop, dumps cycle + event deltas, ends with a
  one-line verdict.
- Test suite: `kernel/tests/test_pmu.c` (`test_suite_pmu`). QEMU TCG
  models PMCCNTR but not event counters, so event-counter assertions
  relax under `PLATFORM_QEMU_VIRT`; hardware verification is the
  authoritative check.

## Per-platform verdicts

| Platform | Cycle counter | Event counters | EL3 trap? | Notes |
|----------|---------------|----------------|-----------|-------|
| QEMU virt (TCG) | works | not modelled | no | Plumbing-only |
| Pi 5 (Cortex-A76) | TBD on hardware | TBD on hardware | no | TF-A clears MDCR_EL3 (`armstub8-2712.S:164`); expect full access |
| Jetson Orin Nano (Cortex-A78AE) | TBD on hardware | TBD on hardware | unknown | NVIDIA BL31 — verified by `pmu probe` per #875 |

Hardware verdicts will be filled in as each board is captured. The
capstone deliverable (#871's before/after-#56 row in
`docs/benchmarks.md`) only requires Pi 5; Jetson is a bonus row when
verification passes.

## If Jetson PMU traps to EL3

The fallback is a TF-A patch parallel to
`tools/tfa-patches/0004-SLM-OS-Jetson-IRQ-routing-patches.patch`. The
required EL3 register state:

- `MDCR_EL3.TPM = 0` — do not trap PMU sysreg accesses to EL3.
- `MDCR_EL3.TPMCR = 0` — do not trap PMCR_EL0 specifically.
- `MDCR_EL2.HPMN = 6` — make all six event counters accessible from
  NS-EL2 (HPMN ≥ N means "every counter is in the non-secure
  partition").

Without these, even a successful `pmu_enable_self` returns nonzero
PMCR_EL0.E but reads from `pmccntr_el0` / `pmevcntr*_el0` either trap
or return zero. The `pmu probe` shell command's verdict line
distinguishes the two cases.

Filing the TF-A patch is out of scope for #875 — the deliverable is
the diagnostic and the documented next step.
