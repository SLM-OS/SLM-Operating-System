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

## Per-platform verdicts (hardware-verified 2026-05-15)

| Platform | Cycle counter | Event counters | EL3 trap? | Notes |
|----------|---------------|----------------|-----------|-------|
| QEMU virt (TCG) | works | not modelled | no | Plumbing-only — TCG doesn't emulate architectural events |
| Pi 5 (Cortex-A76) | ✅ verified on pi-5-2 | ✅ verified on pi-5-2 | no | Needed NSH=1 in PMEVTYPER + MDCR_EL2.HPMN=6 in `pmu_enable_self` |
| Jetson Orin Nano (Cortex-A78AE) | ✅ verified on jetson-nano-1 | ✅ verified on jetson-nano-1 | no | **Works at NS-EL2 under stock NVIDIA BL31 — no TF-A patch needed** |

### Pi 5 reference output (pi-5-2)

```
=== PMU Probe ===
Platform: Raspberry Pi 5
pmu_enable_self() -> true (PMCR_EL0.E stuck)
PMCR_EL0          = 0x410b3041   (IMP=ARM, IDCODE=0x0b → A76, N=6)
PMCNTENSET_EL0    = 0x8000003f   (cycle + 6 events enabled)
CurrentEL         = 2
MDCR_EL2          = 0x00000006   (HPMN=6)

Iterations: 100000
Cycle delta      = 551536        (≈5.5 cycles/iter)
[2] INST_RETIRED = 600108        (≈6 instr/iter)
[4] MEM_ACCESS   = 200021        (≈2 accesses/iter)
[3] BR_MIS_PRED  = 9
VERDICT: PMU is fully live on this CPU.
```

### Jetson reference output (jetson-nano-1)

```
=== PMU Probe ===
Platform: Jetson Orin Nano
PMCR_EL0          = 0x41223041   (IMP=ARM, IDCODE=0x22 → A78AE, N=6)
PMCNTENSET_EL0    = 0x8000003f
CurrentEL         = 2
MDCR_EL2          = 0x00000006

Iterations: 100000
Cycle delta      = 500340        (≈5.0 cycles/iter)
[2] INST_RETIRED = 600036        (≈6 instr/iter)
[4] MEM_ACCESS   = 199990        (≈2 accesses/iter)
[5] STALL_BACKEND = 416794       (vs Pi 5's 213855 — A78AE microarch differs)
VERDICT: PMU is fully live on this CPU.
```

A78AE retires the same instruction count in ~10% fewer cycles than
A76 (500k vs 552k) on this microbenchmark, but accumulates 2× more
backend-stall cycles — consistent with A78AE's wider issue and
deeper memory pipeline. Both platforms get the full six-event
preset.

## What hardware verification surfaced

Two issues in the original #874 primitives that only manifested on
real hardware (not under QEMU TCG):

1. **PMEVTYPER<n>_EL0 / PMCCFILTR_EL0 NSH bit** — with NSH=0 (the
   default after PMCR_EL0.P reset), counters DO NOT count NS-EL2
   events. SLM-OS runs at NS-EL2 with VHE on both Pi 5 and Jetson;
   every event counter read RAZ until NSH=1 was set.
2. **MDCR_EL2.HPMN partition** — warm boot under TF-A can leave
   HPMN=0, putting all counters in the EL2-only half. Even from
   NS-EL2 itself this caused RAZ reads on the event counters (cycle
   counter has separate gating).

Both fixed in commit `1fa7aac2` on the #874 branch.

## What's not needed

Stock NVIDIA BL31 on Jetson Orin Nano allows NS-EL2 PMU access
without any of the bits originally listed as fallbacks:

- `MDCR_EL3.TPM` is not set by BL31 (no trap to EL3 needed).
- `MDCR_EL3.TPMCR` is not set by BL31.
- No TF-A patch parallel to
  `tools/tfa-patches/0004-SLM-OS-Jetson-IRQ-routing-patches.patch`
  is required for PMU access. The IRQ-routing patch covers a
  separate concern (GIC group config), unrelated to PMU.

If a future Jetson L4T BSP revision changes this, the fallback
recipe was:

- `MDCR_EL3.TPM = 0` — do not trap PMU sysreg accesses to EL3.
- `MDCR_EL3.TPMCR = 0` — do not trap PMCR_EL0 specifically.

But on the BL31 revision present 2026-05-15 these bits are already
permissive at NS-EL2 entry.
