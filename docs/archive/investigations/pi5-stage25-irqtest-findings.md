# Pi 5 Stage 2.5 — Trampoline-path Hang Investigation

**Status:** **Closed.** Investigation produced the conclusion that
NS-EL1 IRQ delivery is broken on Pi 5 regardless of which timer PPI
is selected. The actual production fix is tracked in
[#683](https://github.com/SLM-OS/SLM-Operating-System/issues/683) —
move SLM-OS to EL2 with VHE. See
[`pi5-el2-vhe-plan.md`](../plans/pi5-el2-vhe-plan.md).

**Issue:** [#134](https://github.com/SLM-OS/SLM-Operating-System/issues/134).
**Investigation issue:** [#672](https://github.com/SLM-OS/SLM-Operating-System/issues/672) (closed 2026-05-07).

**Prerequisite:** [PR #641](https://github.com/SLM-OS/SLM-Operating-System/pull/641) (Stage 2 — TF-A patches + `irqtest` shell command).

---

## What this branch adds

- `kernel/src/shell_sys.c:cmd_irqtest` — checkpoint-style trace via
  direct UART writes. Each step in the unmask sequence emits an ASCII
  digit (1–7) before/after the `daifclr`, the `isb`, the spin loop,
  the re-mask. Bypasses the kernel UART lock so it works from any
  context.
- `kernel/arch/arm64/vectors.S:STAGE25_TRACE_CHAR` — asm trace macro
  that writes a single ASCII char to the RP1 PL011 DR. Used at the
  el1_irq vector entry, after `el1_irq_handler`, after
  `maybe_arm_resched_trampoline`, before `eret`, plus probe points
  inside `resched_trampoline`.
- `Makefile` — `STAGE25_TRACE_PI5=ON` propagation (already added in
  Stage 2's CMake option).

All gated on `STAGE25_TRACE_PI5`; no impact on production builds.

---

## Hardware findings (pi-5-2, 2026-05-06)

### Test 1: `irqtest` (with `daifclr`)

```
slmos> irqtest
irqtest probe: 1 2 3   ← hangs forever
```

Checkpoint 4 (after `daifclr`) never prints. **The `msr daifclr, #2`
instruction does not return.** Subsequent shell commands never echo;
the system is genuinely wedged, not just slow.

### Test 2: `irqtest noirq` (skip the `daifclr`)

```
slmos> irqtest noirq
irqtest probe: 1 2 3-skipped.  CPU 0 — diag_vec_counts after:  irq=0 fiq=0
  RESULT: No IRQ or FIQ delivered.
```

Without the `daifclr`, the test completes cleanly. The dot-spin
loop runs and the post-spin `shell_printf` succeeds. (Some
checkpoint chars 4–7 are dropped via PL011 FIFO overflow during the
fast spin — non-essential, just a side-effect of the bypass-UART
trace path. The clean completion is the data point.)

### Test 3: kernel-side IRQ vector trace markers

With `STAGE25_TRACE_CHAR 'V'` at the very first instruction of
`el1_irq` (saving x16/x17 to the kernel stack, then writing 'V' to
the RP1 PL011 DR via movz/movk for the address), running `irqtest`
produces no `'V'` in the output before the hang. Same for the
`'T'` marker at `resched_trampoline` entry, the `'M'` marker after
`maybe_arm_resched_trampoline`, etc.

**The IRQ exception is not being taken at EL1.** Despite our Stage 2
TF-A patches that clear `SCR_EL3.IRQ`/`SCR_EL3.FIQ` in the BL33
context.

---

## What this implies

The `daifclr` does not return AND no EL1 IRQ vector entry is
observed. There are two consistent explanations:

1. **Our TF-A `SCR_EL3.IRQ`/`FIQ` clear is not taking effect at run time.**
   When NS clears `DAIF.I` and an IRQ is pending in the GIC, the IRQ
   still routes to EL3. TF-A's IRQ handler runs at EL3, returns to NS
   at the instruction after `daifclr` — but the IRQ stays pending in
   the GIC, so it traps to EL3 again immediately. Infinite EL3 loop;
   NS makes no forward progress.

2. **A SError or alignment fault is firing instead of an IRQ**, and the
   trace-marker macro itself can't run from that exception path. Less
   likely given the pattern (the "hang" is consistent and silent), but
   not eliminated.

### What rules out for sure

- **Our TF-A binary IS being loaded.** A prior test wrote a sentinel
  to a misaligned DRAM address from `plat_rpi_bl31_custom_setup`;
  that bricked the boot, confirming the firmware loads our binary
  rather than ignoring it. (The sentinel was reverted before
  shipping Stage 2.)
- **Our SCR_EL3 patch IS in the built `bl31.bin`.** Source
  inspection of `lib/el3_runtime/aarch64/context_mgmt.c:setup_ns_context`
  shows the `#ifdef PLAT_RPI5 scr_el3 &= ~(IRQ|FIQ); #endif` lines
  intact. `PLAT_RPI5` is defined in our patched `plat/rpi/rpi5/platform.mk`.

### What's still ambiguous

Whether the patch *runs* and *takes effect* at the right moment.
TF-A has multiple SCR_EL3 manipulations (`SCR_RESET_VAL`,
`get_scr_el3_from_routing_model`, etc.). Our clear could be
overridden later, or the `setup_ns_context` path may not be the one
that loads SCR_EL3 at the actual NS-entry ERET. Verifying this needs
either:

- A diagnostic build of TF-A that prints SCR_EL3 to TF-A's UART
  (BCM2712 PL011 at `0x10_7d001000`) right before `el3_exit`. This
  needs a separate cable to capture — the user-visible serial is
  RP1-side only.
- A debugger attached at EL3 (JTAG / OpenOCD with the right probe).

---

## Final state (after #672 differential probing)

The follow-on `issue/672/secondary-preempt-bringup` branch added 8
commits of differential probes (`single`, `noirq`, `set`, `dbg`,
`isb`, `fmask`, `fiq`, `wfi`, `cmem`, `dsb`, `wait`, `idle`, `match`,
`putsx`, `clr`, `tdis`, `iar`, `iardrain`, `dis`, `linit`). The probes
established:

- **PPI 30 (CNTP) is correctly placed in Group 1 NS by TF-A.**
  `GICC_HPPIR` returns 30 from NS-EL1 (not 1022); NS can ack via
  `IAR`. So the GIC routing is correct — the original "SCR_EL3 isn't
  taking effect" hypothesis is **disconfirmed**.
- **The GIC's `nIRQ` pin is asserted whenever the timer is enabled
  and pending.** Proof by experiment: `CNTP_CTL=0`, `ICENABLER0`
  disable, and IAR-without-EOI all unblock `daifclr`.
- **The EL1 IRQ vector is never entered** from any code path —
  `vec->irq` stays 0 across all CPUs forever, including from idle's
  `daifclr+isb+wfi` loop. The earlier "idle survives, irqtest
  doesn't" framing was a timing flake; both wedge under the right
  pin-asserted timing.
- **`daifclr #2` at NS-EL1 with the pin asserted hard-locks CPU 0**
  with no exception delivered.
- **PPI 27 (CNTV virtual) tested as the alternative** — same wedge
  signature. The blocker is **not PPI-specific**; NS-EL1 IRQ
  delivery on this Pi 5 / BCM2712 / GIC-400 firmware appears broken
  regardless of PPI.

### Why this isn't visible on Linux

Linux on Pi 5 boots into EL2 with VHE and uses PPI 26 (Hyp Physical
Timer) via `arch_timer_select_ppi()`'s first branch. It never
exercises NS-EL1 IRQ delivery. The Pi firmware path that's been
validated end-to-end is EL2 / `VBAR_EL2` / PPI 26.

### Resolution

Tracked in [#683](https://github.com/SLM-OS/SLM-Operating-System/issues/683):
move SLM-OS to EL2 with VHE on Pi 5. See
[`pi5-el2-vhe-plan.md`](../plans/pi5-el2-vhe-plan.md) for the 5-PR refactor
plan. The trampoline infrastructure (PR #656) remains structurally
sound but unexercised on Pi 5 until #683 unblocks hardware IRQ
delivery; `COOP_PREEMPT` is the working preemption mode in the
meantime.

### What's landing from #672 by default

- `19364004` `fix(pi5): switch timer to PPI 27 (CNTV virtual timer)`
  — matches Linux's documented EL1 fallback choice. Not a fix for
  the wedge but not worse than PPI 30 either, and #683 will replace
  it with PPI 26 anyway.
- The diagnostic infra (asm + C trace macros, `irqtest` modes) gated
  behind `STAGE25_TRACE_PI5`. No-op in production builds.

---

## Why this work was still progress

The `irqtest noirq` baseline + checkpoint trace is the first
diagnostic that:

- Distinguishes "system hung pre-`daifclr`" from "system hung in the
  IRQ delivery path" *without* needing to read post-mortem state.
  The presence/absence of checkpoint chars is the ground-truth
  signal.
- Runs the same test under different kernel configs
  (`SECONDARY_PREEMPT` on/off, `COOP_PREEMPT` on/off, custom TF-A
  vs stock, PPI 27 vs PPI 30) so future bisection has a stable
  harness.

Both the asm and C trace macros are gated on `STAGE25_TRACE_PI5` so
production builds are unaffected. The diagnostic harness will
continue to pin #683's correctness — after PR 4 of #683 lands,
`irqtest` should report `RESULT: IRQ DELIVERED` rather than wedging.

---

*Last updated: 2026-05-07.*
