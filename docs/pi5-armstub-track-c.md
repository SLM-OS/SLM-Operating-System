# Pi 5 Track C — Custom EL3 Armstub for True Preemptive Multitasking

**Status:** Stage 1 (source + build infrastructure) — landed via PR for `feat/pi5-armstub-track-c`. Stage 2 (TF-A integration) — open.

**Issue:** [#134](https://github.com/SLM-OS/SLM-Operating-System/issues/134) — restore hardware timer IRQ delivery on Pi 5.

**Prerequisite reading:** PR #639 (`feat/pi5-timer-irq-probes`) for the diagnostic data that pinned this register.

---

## Why Track C exists

PR #639's `timdiag sgi` probe established the chain of evidence:

| Signal | State | Meaning |
|---|---|---|
| `GICD_ISENABLER0` bit 30 | **1** | Timer PPI enabled in distributor |
| `GICD_ISPENDR0` bit 30 | **1** | Timer pending in distributor |
| `GICC_HPPIR` | **30** | Timer **visible at CPU interface** as Group 1 NS |
| Per-CPU `diag_vec_counts.irq` | **0** | **No IRQ vector has fired since boot on any CPU** |
| `HCR_EL2.IMO` | 0 | IRQs go to EL1, not EL2 |

The blocker is **between `GICC_HPPIR` and the CPU's IRQ pin**. The
remaining hypothesis is `SCR_EL3.IRQ=1` in TF-A — non-secure IRQs
trap to EL3, the stub TF-A IRQ handler returns without re-injecting
to NS, NS never sees them. `SCR_EL3` is unreadable from non-secure,
so the only way to confirm-and-fix is to take over EL3.

PR #639's `timdiag smc` further confirmed feasibility: a PSCI SMC
round-trips in ~16 cycles (~300 ns at 54 MHz). An EL3 stub that
clears `SCR_EL3.IRQ` and runs in this address space would impose
no measurable per-tick overhead.

---

## What this PR delivers (Stage 1)

1. **`kernel/arch/arm64/armstub8-2712.S`** — minimal EL3 stub source.
   - Configures GIC-400 group registers (only EL3 can write these).
   - Clears `SCR_EL3.IRQ` and `SCR_EL3.FIQ` so NS interrupts deliver
     to EL1/EL2 directly. *This is the load-bearing change.*
   - Sets `NS=1`, `HCE=1`, `RW=1`, `SMD=0`. Preserves implementation-
     defined bits TF-A may rely on.
   - `eret`s to kernel entry at EL2.

2. **`Makefile`** — `make armstub-pi5` target.
   - Assembles the source with `aarch64-none-elf-gcc`.
   - `objcopy`s to a flat binary at `build/armstub/armstub8-2712.bin`.
   - Output is currently **208 bytes**.

3. **This document.**

The binary is **not deployed** by default. Adding it to a Pi 5 boot
partition + `armstub=armstub8-2712.bin` in `config.txt` swaps TF-A's
BL31 for our minimal stub.

---

## What this PR does NOT yet deliver (Stage 2)

**The PSCI gap.** Pi 5's default armstub is TF-A's BL31, which
provides PSCI 0.2 (`CPU_ON`, `CPU_OFF`, `SYSTEM_RESET`,
`AFFINITY_INFO`). Replacing it with our 208-byte stub leaves SMC
instructions trapping to a non-existent EL3 handler. SMP secondary
bring-up — which the kernel does via `psci_cpu_on` from `smp.c` —
will hang at "CPU map: 4 CPUs configured" exactly as documented in
the prior aborted attempt (`docs/archive/investigations/pi5-preemption-resolution.md`).

Two paths forward:

### Path A — Fork TF-A, add the SCR_EL3 clear

Recommended. Cleanest because TF-A's PSCI is well-tested.

Steps:
1. Clone `ARM-software/arm-trusted-firmware`.
2. In `plat/rpi/rpi5/rpi5_bl31_setup.c` (or the shared `plat/rpi/common`
   path), add:
   ```c
   uint64_t scr = read_scr_el3();
   scr &= ~(SCR_IRQ_BIT | SCR_FIQ_BIT);
   write_scr_el3(scr);
   ```
   in the `bl31_plat_arch_setup` or `bl31_platform_setup` hook.
3. Build: `make PLAT=rpi5 CROSS_COMPILE=aarch64-none-elf- bl31`.
4. Output is `build/rpi5/release/bl31.bin`. Rename to
   `armstub8-2712.bin`, deploy.
5. Add `armstub=armstub8-2712.bin` to `deploy/pi5/config.txt`.

**Effort estimate:** 1–2 weeks for a developer comfortable with TF-A
(setting up the build environment is the largest cost).

### Path B — Implement PSCI 0.2 inside our stub

Self-contained but more code. Roughly:
1. Reserve a fixed memory region for the spin table or per-CPU
   release-address slots.
2. Add an EL3 vector table (16 vectors × 0x80 each = 2 KB).
3. Add an SMC handler that decodes `x0[31:0]` against PSCI function
   IDs and dispatches.
4. Implement at minimum:
   - `PSCI_VERSION` (0x84000000) — return 0x10001 (PSCI 1.1).
   - `CPU_ON_64` (0xC4000003) — write context-id + entry to slot,
     SEV the WFE on the target.
   - `CPU_OFF` (0x84000002) — current CPU writes its release
     address to 0 then WFEs forever.
   - `SYSTEM_RESET` (0x84000009) — write the SoC's watchdog reset.
   - `AFFINITY_INFO_64` (0xC4000004) — return 0 (ON) / 1 (OFF) /
     2 (ON_PENDING) per slot.
5. Boot the secondaries via the spin table — kernel side already
   uses `psci_cpu_on` so this is transparent.

Reference for the spin-table model: `raspberrypi/tools/armstubs/armstub8.S`
(historical, used pre-Pi 5).

**Effort estimate:** 3–5 weeks. Requires a working EL3 vector table,
which neither this stub nor SLM-OS currently has.

---

## How to test Stage 1 in isolation

You can verify the SCR_EL3 fix works **without** PSCI by running a
single-CPU build:

1. Build:
   ```
   make armstub-pi5
   make kernel PLATFORM=RASPI5
   ```
2. Add to Pi 5 boot partition:
   - `build/armstub/armstub8-2712.bin` → `/armstub8-2712.bin`
   - Append `armstub=armstub8-2712.bin` to `config.txt`
3. Boot. The PSCI hang appears during secondary CPU bring-up; CPU 0
   should still come up.
4. Run `diag` at the shell — check that per-CPU IRQ counter for
   CPU 0 shows non-zero values (i.e., timer IRQs deliver).

If CPU 0's IRQ counter is non-zero, the SCR_EL3 fix works and
Stage 2 (PSCI integration) becomes the only remaining gate.

If CPU 0's IRQ counter is still zero, the hypothesis is wrong and
Track C is back to investigation. The probes from PR #639 plus an
EL3-side dump of SCR_EL3 (which our armstub can now do, since it
runs at EL3) are the next step.

---

## Boot-garble issue (historical)

Prior armstub attempts triggered a "~60% boot garble rate" per
`docs/archive/investigations/pi5-preemption-resolution.md`. The root
cause was never determined; most-likely candidates:

- UART pinmux racing the firmware's own UART setup (the firmware
  brings up GPIO 14/15 + PL011, then ERETs to the armstub; if the
  armstub's first action races this, characters drop).
- Cache state inherited from VC firmware that the armstub doesn't
  invalidate.

Stage 2 should address this preemptively:
- Add `dc ivau` over `.text` before `eret` to flush stale icache.
- Add a `dsb sy + isb` after each MMIO write in the GIC group
  configuration loop.
- Run `boot_test --count 100` after Stage 2 lands and assert
  ≥ 99/100. The current Stage 1 stub has both barriers in place but
  the boot-garble rate is unverified because Stage 1 isn't deployable
  on its own.

---

## Acceptance criteria for closing #134

| Criterion | Status |
|---|---|
| Custom armstub source written | ✅ Stage 1 |
| Build infrastructure (`make armstub-pi5`) | ✅ Stage 1 |
| `SCR_EL3.IRQ=0` clear documented in source | ✅ Stage 1 |
| Track C rationale + path forward documented | ✅ Stage 1 (this doc) |
| TF-A fork or PSCI implementation integrated | ☐ Stage 2 |
| Boot reliability ≥ 99/100 with armstub deployed | ☐ Stage 2 |
| `cpu` shell shows non-zero IRQ counter on all CPUs | ☐ Stage 2 |
| `make test PLATFORM=RASPI5` 5/5 multi-core tests pass with `SECONDARY_PREEMPT=ON` | ☐ Stage 2 |
| `boot_test --count 10` 10/10 with `COOP_PREEMPT=OFF` | ☐ Stage 2 |

Stage 2 is the work this stage de-risks: when someone takes it on,
the diagnostics from PR #639 and the source/build from this PR mean
the only remaining unknowns are the TF-A build mechanics or the
PSCI implementation details.

---

*Last updated: 2026-05-05.*
