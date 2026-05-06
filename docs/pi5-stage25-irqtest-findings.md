# Pi 5 Stage 2.5 — Trampoline-path Hang Investigation

**Status:** In progress. The original "Stage 2.5 = small kernel-side
fix to unmask `DAIF.I` in tasks" plan was wrong — empirical testing on
pi-5-2 shows the actual problem is deeper.

**Issue:** [#134](https://github.com/SLM-OS/SLM-Operating-System/issues/134).

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

## Next steps

1. **Verify our SCR_EL3 patch is in the runtime path.** Either by
   adding a TF-A-side print right before `el3_exit` (BCM2712 UART —
   needs a hardware UART tap) or via JTAG.
2. **Compare with the [`raspberrypi/tools/armstubs/armstub8.S`](https://github.com/raspberrypi/tools/blob/master/armstubs/armstub8.S)
   approach** — that uses a spin table for SMP and bypasses TF-A
   entirely. NS IRQs would deliver naturally because no EL3
   firmware is running. Trade-off: lose PSCI; the kernel needs the
   spin-table SMP path SLM-OS doesn't currently have.
3. **Check whether the kernel boot.S writes anything to SCR-related
   registers from EL2 that could mask our intent.** EL2 can't write
   SCR_EL3 directly but it can write `HCR_EL2.IMO` etc.

Stage 2.5 is **not** "one DAIF unmask away" from observable hardware
preemption. The wedge is upstream of the kernel — at the boundary
between EL3 firmware and NS exception delivery — and resolving it
needs either deeper TF-A debugging tooling or a different EL3
strategy.

---

## Why this is still progress

The `irqtest noirq` baseline + checkpoint trace is the first
diagnostic that:

- Distinguishes "system hung pre-`daifclr`" from "system hung in the
  IRQ delivery path" *without* needing to read post-mortem state.
  The presence/absence of checkpoint chars is the ground-truth
  signal.
- Runs the same test under different kernel configs (SECONDARY_PREEMPT
  on/off, COOP_PREEMPT on/off, custom TF-A vs stock) so future
  bisection has a stable harness.

Both the asm and C trace macros are gated on `STAGE25_TRACE_PI5` so
production builds are unaffected.

---

*Last updated: 2026-05-06.*
