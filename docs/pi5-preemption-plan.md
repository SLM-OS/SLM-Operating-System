# Pi 5 Preemptive Multitasking + SMP Plan

Execution checklist for bringing Raspberry Pi 5 to full preemptive multitasking and SMP parity with the QEMU_VIRT and JETSON_ORIN_NANO platforms. Subsequent sessions should track progress against the phases below.

## Problem statement

SLM-OS on Pi 5 currently operates in **cooperative multitasking** mode. Timer IRQs are not delivered to EL1 at runtime — `timer_handler_count` stays at `0` even when the idle task loops with `DAIF.I=0` and the GIC reports `HPPIR=30`. Tasks dispatched to CPUs 1-3 via WFE/SEV run to completion but are never preempted mid-execution.

Two blockers together gate preemption + SMP:

- **#99** — Timer IRQs never reach EL1. Suspected causes: GIC Group 1 configuration gap since the `armstub8-2712.bin` was disabled, potential `HCR_EL2.IMO` routing IRQs to EL2, or `SCR_EL3` state left by firmware. `IGROUPR` reads as `0` from non-secure EL1 (documented in commit `5a9b143`), so verifying the group state post-boot is not possible from userland — the setup in `boot.S:338-347` happens only if firmware entered at EL2.
- **#57 residual** — ELR-trampoline infrastructure merged on main (PR #98) but the `PI5_SECONDARY_PREEMPT` CMake option defaults OFF. Once #99 is fixed, activating the option must deliver real preemption across all 4 CPUs. The remaining work is unmasking `DAIF` on tasks without re-triggering the shell-banner hang first documented in commit `ac46e40`.

## Success criteria

1. `timer_handler_count > 0` on all 4 CPUs after `bench smp`.
2. All 5 previously-ignored multi-core integration tests (`test_multicore_basic`, `test_task_migration`, `test_stress_multicpu`, `test_lock_contention`, `test_task_lifecycle`) pass on Pi 5 hardware.
3. A CPU-bound task pinned to CPU 3 with no `yield()` can be preempted mid-execution; the AI scheduler's migration/priority decisions take effect in under ~20 ms.
4. `labctl boot_test --count 10` returns 10/10 on Pi 5 with preemption enabled.

---

## Phase 1 — Root-cause #99 (diagnostic-only, no behavior change)

The blocker has four plausible causes, indistinguishable from EL1 alone because most of the relevant registers (`HCR_EL2`, `SCR_EL3`, `CNTHCTL_EL2`, `IGROUPR` from non-secure) cannot be read post-boot. Capture them at EL2 during boot and expose the snapshot via non-cacheable memory.

### 1a. EL2 register snapshot in `boot.S`

Extend `boot.S` right before the `eret` to EL1 (around line 450) to write, to a fixed NC slot, these `u64` values:

| Offset from `NC_MEM_BASE` | Register |
|---|---|
| `0xFF00` | `HCR_EL2` pre-our-write |
| `0xFF08` | `HCR_EL2` post-our-write |
| `0xFF10` | `CNTHCTL_EL2` |
| `0xFF18` | `CurrentEL` at firmware entry (EL2 or EL1?) |
| `0xFF20` | `MIDR_EL1` (sanity) |
| `0xFF28` | `ID_AA64PFR0_EL1` (confirms EL2 is implemented) |
| `0xFF30` | `GICD_CTLR` pre-our-writes |
| `0xFF38` | `GICC_CTLR` pre-our-writes |

Add a shell command `diag el2` that dumps these slots. No behavior change; only visibility.

### 1b. Per-vector exception counters

Increment a per-CPU NC counter at the very first instruction of `el1_irq`, `el1_sync`, `el1_fiq`, `el1_serror` (before `save_regs`). Expose via the `cpu` shell command. Closes an ambiguity from the previous session: confirms whether *any* exception type is being delivered, even if IRQ is not.

### 1c. Decision table

Force `DAIF.I=0` briefly in `main` (before shell spawns), read diagnostics:

| Observation | Root cause implicated |
|---|---|
| `HCR_EL2.IMO` set post-our-write | IRQs routed to EL2 |
| `CurrentEL != 8` at firmware entry | Firmware entered at EL1; GIC group setup skipped |
| IRQ counter increments but `el1_irq_handler` hangs | Routing works; handler-side bug |
| All exception counters stay 0 | GIC group / distributor blocks delivery |
| `SCR_EL3` unreachable (traps) | Expected; TF-A controls EL3 |

### 1d. External reference comparison

Fetch into `docs/reference/` if not already present (per `CLAUDE.md` reference cache rules):

- `linux/arch/arm64/kernel/entry.S` — EL1 IRQ vector on non-secure EL1.
- `linux/drivers/clocksource/arm_arch_timer.c` — init order for CNTP vs CNTV, `CNTKCTL_EL1` programming.
- `linux/drivers/irqchip/irq-gic.c` — GICv2 init for non-secure EL1 boot, Group 1 preservation.
- U-Boot `arch/arm/cpu/armv8/start.S` for BCM2712 — what firmware state precedes the kernel.

### Phase 1 deliverable

A write-up at `docs/pi5-irq-investigation-2026-04.md` naming the root cause with register evidence. Go/no-go for Phase 2 requires a named cause.

---

## Phase 2 — Fix timer IRQ delivery

Branches based on the Phase 1 finding.

### 2a. If cause is `HCR_EL2.IMO=1` (IRQs routed to EL2)

Change `boot.S:429-431` to write `HCR_EL2 = 0x80000001` with explicit clears of `IMO | FMO | AMO` rather than assuming firmware leaves those bits at 0. Add a barrier + re-read to confirm the write held.

### 2b. If cause is "firmware entered at EL1, GIC group writes skipped"

The EL2 block in `boot.S:299-465` never executes. Options:

- **b.1 — Re-enable `armstub8-2712.bin`.** Root-cause the 60% boot-garble rate (investigated and abandoned per `pi5-baremetal-status.md`) as a separate work stream. Fallback option.
- **b.2 — Configure GIC Group 1 from EL1.** Non-secure EL1 can write `GICD_IGROUPR` when TF-A grants access. Try it; verify via exception delivery. Preferred.
- **b.3 — Move to virtual timer (CNTV, IRQ 27).** Historical commit `12bef99` paired virtual timer with armstub. Requires `CNTVOFF_EL2=0` which must be set at EL2 — not viable if we're stuck at EL1.

### 2c. If cause is "exception delivered but handler hangs"

The `ac46e40` shell-banner hang landed at `[2]SLM-[a]OS` — mid-`uart_puts`. Revisit with Phase 1's new counters in place; likely a DAIF-handling edge case in the IRQ return path. Candidate fixes:

- Verify `gic_end_interrupt(irq)` completes before `scheduler_tick` returns (already done per `exceptions.c:247-264`).
- Check `uart_putc`'s `FR_TXFF` spin is re-entrant across IRQ.
- Confirm the trampoline's `preempt_disabled[cpu]=1`-before-`daifclr` window holds.

### 2d. Validation for Phase 2

- `cpu` shell command shows `timer_handler_count > 0` and `sched_diag_tick > 0` on CPU 0 within ~100 ms of boot.
- A spin-forever task pinned to CPU 0 is preempted — visible because shell responds to input while it runs.
- `labctl boot_test --count 10` returns 10/10.

### Phase 2 deliverable

Branch `fix/99-pi5-timer-irq-delivery`, merged to main, closing issue #99.

---

## Phase 3 — Task `DAIF` unmask (safe to deploy)

With #99 fixed, tasks must run with `DAIF.I=0` so timer IRQs can actually preempt them. Reinstate what `ac46e40` originally proposed:

### 3a. Task entry unmask

Re-enable `daifclr #2 + isb` in `kernel/sched/task.c:task_entry_trampoline`, gated on `PI5_SECONDARY_PREEMPT`. (Present in PR #98 branch history; needs un-commenting.)

### 3b. Context switch DAIF handling

`kernel/arch/arm64/context.S` already restores `DAIF` **last** (PR #98), closing the `ac46e40` "DAIF restored before SP/GPRs → mid-restore IRQ corrupts state" race. Re-verify by inspection after Phase 2 lands.

### 3c. Boot-stack `daifclr` removal

`scheduler_start()` does not `daifclr` on the boot stack before `switch_to(NULL, first_task)` when `PI5_SECONDARY_PREEMPT=ON` (PR #98). Confirm this remains off for Pi 5; an IRQ on the boot stack mid-`switch_to` would be unsafe because the boot stack is not a valid task stack.

### 3d. Activate the kill switch

Flip the CMake option default:

```cmake
option(PI5_SECONDARY_PREEMPT ... OFF)  →  option(PI5_SECONDARY_PREEMPT ... ON)
```

Keep the option available so regression bisects can still turn it off.

### Phase 3 deliverable

Pi 5 boots cleanly to shell with preemption active on CPU 0. No regression in QEMU or Jetson (kill switch gates everything).

---

## Phase 4 — Validate preemption on all 4 CPUs

### 4a. Cross-CPU tick delivery

`bench smp` dispatches tasks to CPUs 1-3 and waits. Each task writes an NC slot and exits. With preemption working:

- Every CPU's `sched_diag_tick[cpu]` must advance (currently only CPU 0's does, and only when idle).
- `bench smp` completes in ≤100 ms (currently ~2 s in cooperative mode).

### 4b. The 5 integration tests

`kernel/tests/test_integration.c` contains five tests previously ignored:

- `test_multicore_basic`
- `test_task_migration`
- `test_stress_multicpu`
- `test_lock_contention`
- `test_task_lifecycle`

Unblock them by removing any `TEST_IGNORE` / conditional skip (grep for `PI5` or `ignore` in the file). Run `make test PLATFORM=RASPI5` (boot-tests build) and confirm all pass.

### 4c. AI scheduler smoke test

Build with `AI_SCHED=ON`. Start two CPU-bound inference tasks pinned to different CPUs. Confirm the AI policy's migration decisions take effect (via shell `sched stats`) in real time, not only at task boundaries.

### 4d. Reliability soak

`labctl boot_test --count 10 --expect "slmos>"` on Pi 5. Must be 10/10.

### Phase 4 deliverable

Test log showing 5/5 previously-ignored tests passing, `bench smp` timing data, 10/10 boot reliability.

---

## Phase 5 — Documentation + issue cleanup

### 5a. Update status docs

- `docs/pi5-baremetal-status.md` — remove the stale "preemptive scheduling active" claim from the top; replace with a current-state section citing this plan's deliverables and the #99 resolution evidence.
- `kernel/CLAUDE.md` — delete the "Pi 5 Timer IRQs and pit_ticks" caveat about tasks running with `DAIF.I=1`. Replace with a note that preemptive scheduling is active on all CPUs via the ELR trampoline.
- `docs/pi5-secondary-cpu-preemption.md` — prepend a "Resolved YYYY-MM-DD" banner; keep the body as historical context.
- `docs/pi5-platform-audit.md` — update Part A rows for Timer IRQ Delivery and Secondary-CPU preemption to reflect working state.

### 5b. Issue cleanup

- Close #99 with evidence from Phase 2: `gh issue close 99 --comment "Fixed in <commit> and verified by boot_test 10/10 plus passing integration tests."`
- Comment on #57 noting the infrastructure is now actively driving preemption (the issue is already closed via PR #98).
- Comment on #59 that work-stealing benchmarks are now meaningful; work remains pending as a separate enhancement.

### Phase 5 deliverable

Clean docs and issue tracker; no stale claims; `kernel/CLAUDE.md` matches reality.

---

## Phase 6 — SMP correctness expansion (optional)

With preemption working, additional SMP behaviors become testable:

- **Task migration mid-execution** — move a running task between CPUs; requires Phase 4's tests plus a targeted migration stress test.
- **Lock contention under preemption** — multi-CPU spinlock stress test that forces preemption inside a critical section (validates `spin_unlock` release semantics).
- **Timer drift across CPUs** — confirm `CNTPCT_EL0` is coherent across cores (expected per ARM architecture timer spec, but record evidence).

Gated on Phases 1-4 succeeding. Not strictly required by the success criteria above.

---

## Work sequence and branching

1. `fix/99-pi5-irq-diagnostic` — Phase 1 diagnostic code + reference fetches. PR even without a fix so snapshot infrastructure lands.
2. `fix/99-pi5-irq-delivery` — Phase 2 fix. Depends on diagnostic findings.
3. `fix/57-activate-preemption` — Phases 3-4. Depends on Phase 2 merged.
4. `docs/pi5-preemption-cleanup` — Phase 5. Can run in parallel with Phase 6 if pursued.

Each branch gets its own PR. QEMU `make test` must stay green on every branch.

---

## Critical files

- `kernel/arch/arm64/boot.S:299-465` — EL2 block, needs register snapshots and possibly `HCR_EL2.IMO` clears.
- `kernel/arch/arm64/vectors.S` — exception vectors, may need per-vector NC counters.
- `kernel/drivers/gic.c` — GIC init, may need non-secure-EL1 `IGROUPR` write attempt.
- `kernel/drivers/timer.c` — confirm `CNTKCTL_EL1` is right.
- `kernel/sched/task.c:task_entry_trampoline` — re-enable `daifclr` in Phase 3.
- `kernel/sched/sched.c:scheduler_start` — confirm boot-stack `daifclr` stays removed.
- `CMakeLists.txt` — flip `PI5_SECONDARY_PREEMPT` default to `ON` in Phase 3.
- `kernel/tests/test_integration.c` — unblock 5 tests in Phase 4.
- `docs/pi5-baremetal-status.md`, `kernel/CLAUDE.md`, `docs/pi5-secondary-cpu-preemption.md`, `docs/pi5-platform-audit.md` — Phase 5 doc updates.

---

## End-to-end verification

Acceptance check sequence after Phase 4:

1. `make test` — QEMU, all tests green (regression safety).
2. `make kernel PLATFORM=RASPI5` — clean build with `PI5_SECONDARY_PREEMPT=ON`.
3. Deploy via `labctl sdwire_update`; capture serial to shell prompt.
4. `cpu` — confirm `timer_handler_count > 0` and `sched_diag_tick[N] > 0` for all `N in 0..3`.
5. `bench smp` — completes quickly with all CPUs showing completions.
6. `make test PLATFORM=RASPI5` (with `ENABLE_BOOT_TESTS`) — all 5 previously-ignored multi-core tests pass.
7. `labctl boot_test --count 10 --expect "slmos>"` — 10/10.
8. AI scheduler smoke: two CPU-bound tasks, verify migration takes effect mid-execution.

---

*Last updated: 2026-04-13.*
