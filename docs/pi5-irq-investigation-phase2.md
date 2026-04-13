# Pi 5 IRQ-Delivery Investigation — Phase 2 (in progress)

**Date:** 2026-04-13
**Issue:** #99
**Plan:** `docs/pi5-preemption-plan.md` (Phase 2)
**Outcome:** Partial. One corrected misdiagnosis + one partial fix; root cause of IRQ non-delivery is narrower than Phase 1 concluded but still open. This doc records the work so subsequent sessions don't re-run the same experiments.

---

## Phase 1 interpretation required revision

Phase 1's `diag` command prints "writes SILENTLY DISCARDED" when `GICD_IGROUPR[0]` readback reads `0` after writing `0xFFFFFFFF` from non-secure EL2. That interpretation is **wrong**. Per GICv2 IHI 0048B section 4.1: `GICD_IGROUPR0` is banked and **reads as zero and ignores writes from non-secure access** regardless of the actual group state. The readback is therefore uninformative.

`GICC_HPPIR` (non-secure view, offset `0x18`) is authoritative because it only shows interrupts the CPU interface considers Group 1. On Pi 5 the `cpu` shell command consistently shows `HPPIR=30`, meaning timer PPI 30 **is** visible as Group 1 from the non-secure side. Somewhere between `boot.S`'s EL2 IGROUPR writes and the kernel's runtime state, the group flip took effect. The Phase 1 FIQ hypothesis was therefore not proven.

The `el1_fiq` counter remains at `0` even with `DAIF.F=0` in idle, which is consistent with this: timer arrives as IRQ, not FIQ, so the FIQ vector is never entered.

---

## Actual blocker — IRQ not delivered despite pending Group 1

With timer confirmed Group 1 (`HPPIR=30`) and `DAIF.I=0` in idle (`msr daifclr, #2; isb; wfi`), the CPU should take the IRQ and `el1_irq_handler` should run. It does not:

- `timer_handler_count` stays `0`.
- Per-CPU `irq` counter (`diag vec`) stays `0`.
- All vector counters (sync/IRQ/FIQ/SError) stay `0` on all 4 CPUs.
- `cpu` shell output continues to show `HPPIR=30` even after the shell has been running for minutes.

Candidate causes still on the table:

1. **`GICC_CTLR` bypass bits misconfigured.** After boot, `cpu` showed `CTLR=0x1` — only `EnableGrp1` set, `IRQBypDisGrp1=0` (bypass enabled). Fixed in this branch by having `gic_cpu_init` preserve the bypass-disable bits from whatever `boot.S` / firmware left. Post-fix `CTLR=0x21`. Timer still does not deliver, so this is not sufficient on its own, though the preservation is correct hygiene.
2. **GIC-400 integration on BCM2712 routes nIRQ through the bypass path.** On some SoCs the CPU's legacy IRQ input is wired to the GIC bypass output rather than the GIC's direct output. Setting `IRQBypDisGrp1=1` in that configuration would actually *disable* IRQ delivery, not enable it. Needs a BCM2712/GIC-400 integration spec read to confirm direction.
3. **GICD_ISENABLER for PPI 30 not set on the acting CPU.** `timer_percpu_init()` calls `gic_enable_irq(ACTUAL_TIMER_IRQ)`. On GICv2 the PPI enable bits are banked per-CPU, written via the primary CPU's view of `GICD_ISENABLER0`. If the per-CPU banking requires executing the write *from* that CPU and secondary CPUs don't run `timer_percpu_init` in the expected order, their banked enable could be `0`. Worth confirming but unlikely to affect CPU 0.
4. **PPI 30 is enabled but routed to a CPU other than us.** GICv2 PPIs are always local to each CPU; `GICD_ITARGETSR` doesn't apply. Eliminated as a cause.
5. **TF-A or firmware locked `GICD_CTLR` behaviour we can't see.** `GICD_CTLR` pre-boot.S snapshot shows `0x0` (distributor disabled). We then write `0x3` (EnableGrp0+1). Post-state from `cpu` shows `0x1` (EnableGrp1 in the NS view only), which is expected. But if some implementation-defined bit in the Secure `GICD_CTLR` controls whether non-secure Group 1 interrupts reach the CPU interface, we cannot read or change it.

The cleanest remaining path is Phase 2a.2 — re-enable `armstub8-2712.bin` so EL3 can configure both the distributor and the CPU interface correctly, and the kernel starts with a known-working GIC. That is a separate investigation (the armstub was disabled for a ~60% boot-garble rate).

---

## What this branch ships

Even though IRQ delivery is not yet restored, the following changes are correct and should land:

- **`gic.c` `GICC_CTLR` bypass preservation.** Matches the Linux GIC driver pattern (`writel(GICC_ENABLE | existing_bypass_bits, ...)`) rather than clobbering existing bypass config to zero. No behaviour change on platforms where bypass bits were already zero (QEMU, Jetson); on Pi 5 it keeps the boot.S setup intact.
- **Real `el1_fiq_handler` dispatch.** Even though we now believe the timer arrives as IRQ, any future Group 0 interrupt source will be handled rather than silently hanging the system at `el1_fiq: b hang`. `el1_fiq_handler` reads `GICC_AIAR`, dispatches IRQ 30 → `timer_handler()` under `PI5_FIQ_TIMER`, and EOIs via `GICC_AEOIR`. Harmless when no FIQ fires.
- **Idle-loop `DAIF.F` unmask under `PI5_FIQ_TIMER`.** Symmetric with `DAIF.I` unmask. Closes the "FIQ is masked everywhere" gap regardless of whether FIQ is actually used for timer delivery.
- **`maybe_arm_resched_trampoline` hook in `el1_fiq`.** Symmetric with `el1_irq`. Once IRQ delivery works and `PI5_SECONDARY_PREEMPT` activates, the trampoline will arm regardless of which vector the timer came through.
- **Phase 1 `diag` output caveat updated.** The "writes SILENTLY DISCARDED" line is misleading; marking it is useful but the interpretation in the investigation doc needed the correction above.

Tests: `make test` green on QEMU; Pi 5 boots to shell; `boot_test --count 3` is 3/3 on Pi 5. No behavioural regression vs. Phase 1 (which also booted fine but without preemption).

---

## Recommended next steps

**Option A — pivot to armstub.** Re-enable `armstub8-2712.bin`, resolve the boot-garble issue as a sub-task, land the armstub setup. Once EL3 configures the GIC cleanly, timer IRQs should deliver to the IRQ vector without further workarounds. This is Phase 2a.2 in the plan.

**Option B — keep investigating from EL2.** Add a `GICC_CTLR` post-all-inits snapshot to `diag`, a `GICD_ISENABLER0` dump, and a deliberate IRQ trigger (SGI to self from the shell) to narrow down which GIC path is broken. Would add ~a day of experiments.

**Option C — try a direct CPU register test.** Write to `ICC_IGRPEN1_EL1` / `ICC_PMR_EL1` (GICv3 system registers) even though we believe this is GICv2. If GIC-400 on BCM2712 supports a sysreg interface, it would bypass the memory-mapped CPU interface entirely. Low likelihood but cheap to try.

Recommend Option A. The GICv2-with-Security-Extensions / non-secure-only-access constraint fundamentally caps what we can fix from the kernel side. The armstub path is the architecturally correct fix, and the boot-garble issue should be tractable with the new diag infrastructure in place.

---

*Last updated: 2026-04-13.*
