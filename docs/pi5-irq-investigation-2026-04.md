# Pi 5 IRQ-Delivery Investigation — Phase 1 Findings

**Date:** 2026-04-13
**Issue:** #99
**Plan:** `docs/pi5-preemption-plan.md` (Phase 1)
**Outcome:** Root cause identified — FIQ hypothesis confirmed. Proceed to Phase 2a.

---

## Diagnostic infrastructure added

1. **EL2 register snapshot in `boot.S`** (gated on `PI5_IRQ_DIAG`). Writes ten key registers to non-cacheable memory at `0xFFE0FF00..+0x58` before the `eret` to EL1, including pre/post-write values for `HCR_EL2` and `GICD_IGROUPR[0]`, and a magic marker at offset `+0x50` so the shell can distinguish "never ran EL2 block" from "ran and recorded all-zeros".
2. **Per-CPU exception counters in `vectors.S`**. A `DIAG_BUMP_VEC` macro increments a u64 NC slot at `0xFFE0FF60 + cpu*0x20 + vector_off` as the first instructions of every EL1/EL0 vector entry, before `save_regs`. Covers sync, IRQ, FIQ, SError for both EL1 and EL0.
3. **Real `el1_fiq_handler` replacing `b hang`**. Reads `GICC_AIAR` (Group 0 ACK), records the IRQ number in the per-CPU FIQ-source trace at `0xFFE0FFE0 + cpu*4`, EOIs via `GICC_AEOIR`. No-op when `PI5_IRQ_DIAG` is undefined.
4. **`diag` shell command** (`shell_sys.c`, registered in `shell.c`). Sub-commands `el2`, `vec`, `fiq`, `all` dump the corresponding NC slots.

QEMU test suite remains green (the diag code is gated on `PLATFORM_RASPI5` + `PI5_IRQ_DIAG`).

---

## Observed state on Pi 5 hardware

Captured by running `diag all` at the shell prompt on a fresh boot (no tasks forced `DAIF.F` unmasked in this first pass — tasks run with both `DAIF.I=1` and `DAIF.F=1`, per the existing `context.daif=0x080` initialisation).

### EL2 snapshot

| Register | Value | Interpretation |
|---|---|---|
| `CurrentEL` at entry | `0x8` (EL2) | Firmware enters at EL2 — our EL2 block **does** execute |
| `MIDR_EL1` | `0x414fd0b1` | Cortex-A76 r4p1 (expected) |
| `ID_AA64PFR0_EL1` | `0x1100000010111112` | EL2 implemented (field [11:8]=1), EL3 implemented (field [15:12]=1) |
| `HCR_EL2` pre | `0x80000000` | RW=1 only; IMO/FMO/AMO=0 — IRQs **not** routed to EL2 |
| `HCR_EL2` post | `0x80000000` | Our write is a no-op (same value firmware left) |
| `CNTHCTL_EL2` | `0x3` | EL1PCEN + EL1PCTEN — EL1 can access physical timer ✓ |
| `GICD_CTLR` pre | `0x0` | Distributor **disabled** when we started |
| `GICC_CTLR` pre | `0x60` | Non-zero; will study separately (bypass-disable bits) |
| **`GICD_IGROUPR[0]` pre** | **`0x0`** | All PPIs (incl. timer IRQ 30) in Group 0 |
| **`GICD_IGROUPR[0]` post** | **`0x0`** | **Writes of `0xFFFFFFFF` were silently discarded** |

### Per-vector exception counters

```
CPU   Sync       IRQ        FIQ        SError
 0          0          0          0          0
 1          0          0          0          0
 2          0          0          0          0
 3          0          0          0          0
```

**No exception of any type has been delivered to any CPU.**

### FIQ source trace

All four slots show "no FIQ observed" — the magic marker for `el1_fiq_handler` entry has never been written.

---

## Interpretation

Four conclusions follow directly from the snapshot:

1. **Firmware entry is EL2, and we transition correctly.** `CurrentEL=0x8`, `HCR_EL2` is writable, `CNTHCTL_EL2` grants EL1 access to the physical timer. The EL2 block in `boot.S` runs as designed.

2. **IRQs are not being routed to EL2.** `HCR_EL2.IMO=0` both before and after our write. If the timer IRQ were being delivered anywhere, EL1 (not EL2) would be the destination.

3. **Timer IRQ 30 is in GICv2 Group 0, and non-secure writes cannot promote it.** The `GICD_IGROUPR[0]` readback (pre=`0x0`, post=`0x0` after we wrote `0xFFFFFFFF`) is the definitive proof. This matches the external review's FIQ hypothesis exactly: on GICv2 with Security Extensions, only Secure state (EL3) can change an interrupt's group. TF-A hands the kernel to non-secure EL2, so our `IGROUPR` writes are no-ops. Timer PPI 30 remains in Group 0, meaning it would arrive as **FIQ**, not IRQ.

4. **FIQ is masked the whole time, so nothing actually fires.** The FIQ counter and trace slots stay at their zero-init values. Tasks are created with `DAIF = 0x080` (I-bit set, F-bit ignored — but PSTATE inherits `F=1` from exception-entry defaults). Idle unmasks IRQ (`daifclr, #2`) but **not** FIQ (`#1`). The net effect is that the pending Group 0 interrupt at the GIC never reaches the CPU even though IRQ delivery is unmasked — because the IRQ line is dead and the FIQ line is masked.

Root cause of #99 is therefore a **double blocker**:

- **Distributor side:** Timer PPI 30 remains in Group 0 because non-secure code cannot promote it to Group 1. Delivery is therefore FIQ, not IRQ.
- **CPU side:** All running contexts mask `DAIF.F`, so even the FIQ line is dead.

---

## Consequences for Phase 2

Phase 2a from the plan is the correct path. Two sub-options, both of which should work once `DAIF.F` is also unmasked:

- **2a.1 (fast):** Dispatch `el1_fiq` on `GICC_AIAR`; when IRQ 30 is seen, call the existing `timer_handler()`. The real `el1_fiq_handler` is already in place from Phase 1b — it just needs to dispatch on IRQ number instead of only recording. Idle's `daifclr` must unmask `F` as well, or the FIQ line stays dead.
- **2a.2 (clean):** Re-enable `armstub8-2712.bin` so that EL3 configures PPIs as Group 1 before the kernel runs. This lets the existing IRQ path handle the timer but depends on resolving the ~60% boot-garble rate the armstub originally triggered.

Phase 2b (`HCR_EL2.IMO` clear) is **not relevant** — `HCR_EL2.IMO` is already 0.
Phase 2c (firmware enters at EL1) is **not relevant** — firmware enters at EL2.
Phase 2d (handler-side bug) is **not relevant** — no handler is ever reached.

The previously-feared "configure Group 1 from non-secure EL1" option (earlier plan's `2b.2`) is also not relevant here because we never even reach EL1 without the discard happening first.

---

## Pre-condition reminder (from the plan's Phase 2)

Before any `DAIF` unmask ships in Phase 3: audit every function reachable from the IRQ/FIQ handler path (`el1_irq_handler → timer_handler → scheduler_tick`, and the new `el1_fiq_handler` dispatch when it routes to `timer_handler`) and strip all `uart_printf` / `uart_puts` / `DEBUG_PRINT` calls, or gate them behind `uart_trylock`. This is the likely root cause of the `ac46e40` `[2]SLM-[a]OS` hang and will resurface under Phase 2a if not addressed.

---

## Artifacts

- `kernel/arch/arm64/boot.S` — EL2 snapshot (`#if defined(PI5_IRQ_DIAG)` blocks).
- `kernel/arch/arm64/vectors.S` — `DIAG_BUMP_VEC` macro, real `el1_fiq` vector.
- `kernel/arch/arm64/exceptions.c` — `el1_fiq_handler`.
- `kernel/include/diag_pi5.h` — NC-memory layout definitions.
- `kernel/src/shell_sys.c` — `cmd_diag` (and registration in `kernel/src/shell.c`).
- `CMakeLists.txt` — `PI5_IRQ_DIAG` option (default `ON` for `RASPI5`).

*Last updated: 2026-04-13.*
