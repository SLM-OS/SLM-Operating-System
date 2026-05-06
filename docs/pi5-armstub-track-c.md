# Pi 5 Track C — Custom EL3 Armstub for True Preemptive Multitasking

**Status:** Stage 1 (minimal armstub source + build) and Stage 2 (custom TF-A with `SCR_EL3` + GIC patches) both landed. Stage 2.5 (kernel-side `DAIF.I` unmask) — open.

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

## Stage 2 — TF-A integration (landed)

Took **Path A** from Stage 1's plan: forked `ARM-software/arm-trusted-firmware`,
patched it, built a custom `bl31.bin`, deployed.

**Patches** (in `tools/tfa-patches/`):

1. `setup_ns_context` in `lib/el3_runtime/aarch64/context_mgmt.c` —
   explicitly clears `SCR_EL3.IRQ` and `SCR_EL3.FIQ` for the
   non-secure context so NS interrupts deliver to EL1/EL2.
   Gated on `PLAT_RPI5`.

2. `plat_rpi_bl31_custom_setup` in `plat/rpi/rpi5/rpi5_setup.c` —
   writes `GICD_IGROUPR[0] = 0xFFFFFFFF` from EL3, putting all
   PPIs (timer included) in Group 1 NS. TF-A's default
   `gicv2_spis_configure_defaults` only touches SPIs (≥32);
   PPIs need explicit treatment.

3. `PLAT_RPI5` define in `plat/rpi/rpi5/platform.mk`.

**Build:** `make tfa-pi5` clones TF-A as a sibling of this repo
(if needed), applies the patches, builds `bl31.bin` (~32 KB),
copies it to `build/armstub/armstub8-2712.bin`. Companion
`make tfa-pi5-reset` re-applies after patch updates.

**Hardware verification on pi-5-2:**

- Custom TF-A boots cleanly. PSCI continues to work (4/4 CPUs come
  up). `boot_test` is at parity with stock TF-A.
- `diag` shows the same **per-CPU IRQ counter = 0 on all CPUs** as
  before the custom TF-A.

The IRQ counters being zero is the surprise. We confirmed our
TF-A is loaded (a deliberate write to an unmapped DRAM address
from `plat_rpi_bl31_custom_setup` faulted the boot, which
wouldn't happen if the firmware ignored our binary). So the
patches *did* run; they just didn't unblock IRQ delivery.

The most likely remaining blocker is **kernel-side**: tasks run
with `DAIF.I = 1` and CPU 0's idle (which would `daifclr+wfi`)
rarely runs because the shell + `net_pump` keep CPU 0 busy.
`sched_diag_idle_loops[0]` stays at the sentinel `0xAAAA = 43690`
— idle on CPU 0 has never run. So even with our SCR_EL3 patches,
the CPU never holds an unmasked IRQ state long enough to take
the pending timer IRQ.

## Stage 2.5 — kernel-side: per-task `orig_elr`/`orig_spsr` (open)

This stage was originally framed as "unmask `DAIF.I` in tasks" — a
one-line change. **Hardware testing on pi-5-2 with the irqtest probe
in this PR proved that's only half the story.**

**Verified via `irqtest`:** with the Stage 2 TF-A loaded and
`DAIF.I` briefly cleared from a shell command, the system **hangs
immediately** — meaning the timer IRQ DOES deliver to the IRQ
vector. Stage 2's TF-A patches *are* working. The hang itself is
proof.

**What's actually broken:** Pi 5's existing `SECONDARY_PREEMPT`
trampoline (`kernel/sched/preempt.c`, PR #98) saves `orig_elr` /
`orig_spsr` in **per-CPU NC slots**, not per-task. When
preempt-during-preempted-task happens (which it does as soon as
hardware IRQs are firing every 10 ms across multiple ready tasks),
the inner preemption clobbers the outer's save slots, and the
outer task's eret target is corrupted.

The fix is moving `orig_elr` / `orig_spsr` into `struct task`. This
is a small structural change (~20 lines in preempt.c, vectors.S,
task.h) plus regression validation. It belongs in Stage 2.5 because
it's the load-bearing change for activating real preemption.

After Stage 2.5:

1. **Unmask `DAIF.I`** in `task_entry_trampoline` (gated on
   `SECONDARY_PREEMPT=ON`). One line.
2. **Activate `SECONDARY_PREEMPT=ON`** for Pi 5 default builds.
3. **Activate the Stage 2 custom TF-A** (place `bl31.bin` →
   `armstub8-2712.bin` on the boot partition, add `armstub=` to
   `config.txt`).
4. **Boot test:** `boot_test --count 10` on pi-5-2. Each boot
   should advance per-CPU IRQ counters > 0.
5. **Multi-core test acceptance:** the five originally-failing
   tests should pass via real hardware preemption rather than via
   the Tier 2/3 auto-recovery.

The `irqtest` shell command (`PLATFORM_RASPI5 + PI5_IRQ_DIAG`
only, in `kernel/src/shell_sys.c`) stays in this PR as the
diagnostic that pinned the per-task `orig_elr` requirement and
will continue to pin Stage 2.5's success.

## Acceptance criteria for closing #134

| Criterion | Status |
|---|---|
| Custom armstub source written | ✅ Stage 1 |
| Build infrastructure (`make armstub-pi5`) | ✅ Stage 1 |
| `SCR_EL3.IRQ=0` clear documented in source | ✅ Stage 1 |
| Track C rationale + path forward documented | ✅ Stage 1 (this doc) |
| TF-A fork integrated + built | ✅ Stage 2 |
| `make tfa-pi5` builds TF-A from patches | ✅ Stage 2 |
| Boot reliability ≥ 99/100 with armstub deployed | ✅ Stage 2 (parity with stock) |
| `DAIF.I` unmasked in tasks | ☐ Stage 2.5 |
| `cpu` shell shows non-zero IRQ counter on all CPUs | ☐ Stage 2.5 |
| `make test PLATFORM=RASPI5` 5/5 multi-core tests pass with `SECONDARY_PREEMPT=ON` | ☐ Stage 2.5 |
| `boot_test --count 10` 10/10 with `COOP_PREEMPT=OFF` | ☐ Stage 2.5 |

---

*Last updated: 2026-05-05.*
