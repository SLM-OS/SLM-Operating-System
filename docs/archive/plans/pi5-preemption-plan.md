# Pi 5 Preemptive Multitasking + SMP Plan

> **[Resolved 2026-04-13]** The Pi 5-specific phases in this plan (Phases 1-4 — root-cause and fix the hardware timer IRQ delivery blocker) were superseded mid-execution. Hardware timer IRQs turned out to be blocked by TF-A/GIC configuration we cannot reach from EL1/EL2. The capstone-shippable resolution is **cooperative preemption** (`PI5_COOP_PREEMPT` in `kernel/sched/sched.c`) — `schedule()` synthesizes a `scheduler_tick()` from `CNTPCT_EL0` every 10 ms per CPU. Full investigation and rationale in `docs/pi5-preemption-resolution.md`; merged via PR #126 (closes #99).
>
> **What still applies from this plan:**
> - The **Cross-platform coordination** section below remains the active reference for the Jetson (ARM64) and x86-64 preemption/SMP tracks. The four prerequisite infrastructure PRs are still worth landing.
> - Phase 5 (doc cleanup) executed as part of PR #126.
> - Phase 6 (SMP correctness expansion — migration stress, lock contention under preemption, timer drift) remains a valid post-capstone follow-up.
>
> **What no longer applies:**
> - Phases 1-4 as written assume the hardware-IRQ-path fix that did not pan out. Issue #134 tracks the deeper armstub / `ICC_SRE` investigation needed to ever restore that path, as post-capstone work. Those phases are retained below as historical record of the approach considered.

Execution checklist for bringing Raspberry Pi 5 to full preemptive multitasking and SMP parity with the QEMU_VIRT and JETSON_ORIN_NANO platforms. Subsequent sessions should track progress against the phases below.

## Problem statement

SLM-OS on Pi 5 currently operates in **cooperative multitasking** mode. Timer IRQs are not delivered to EL1 at runtime — `timer_handler_count` stays at `0` even when the idle task loops with `DAIF.I=0` and the GIC reports `HPPIR=30`. Tasks dispatched to CPUs 1-3 via WFE/SEV run to completion but are never preempted mid-execution.

Two blockers together gate preemption + SMP:

- **#99** — Timer IRQs never reach EL1. Leading hypothesis (see Phase 1): timer IRQs arrive as **FIQ** because they remain in GICv2 Group 0, and the current `el1_fiq: b hang` handler silently eats them. Non-secure state (EL1 or EL2) **cannot** promote a Group 0 interrupt to Group 1 on GICv2 with Security Extensions — only Secure EL3 can. That makes the `GICD_IGROUPR` writes in `boot.S`'s EL2 block almost certainly no-ops, and explains why `IGROUPR` always reads as `0` from non-secure (documented in commit `5a9b143`). Other possible contributors: firmware entering at EL1 and skipping the EL2 block entirely; `HCR_EL2.IMO` routing IRQs to EL2; `SCR_EL3` state left by TF-A.
- **#57 residual** — ELR-trampoline infrastructure merged on main (PR #98) but the `PI5_SECONDARY_PREEMPT` CMake option defaults OFF. Once #99 is fixed, activating the option must deliver real preemption across all 4 CPUs. The remaining work is unmasking `DAIF` on tasks without re-triggering the shell-banner hang first documented in commit `ac46e40`.

## Success criteria

1. `timer_handler_count > 0` on all 4 CPUs after `bench smp`.
2. All 5 previously-ignored multi-core integration tests (`test_multicore_basic`, `test_task_migration`, `test_stress_multicpu`, `test_lock_contention`, `test_task_lifecycle`) pass on Pi 5 hardware.
3. A CPU-bound task pinned to CPU 3 with no `yield()` can be preempted mid-execution; the AI scheduler's migration/priority decisions take effect in under ~20 ms.
4. `labctl boot_test --count 10` returns 10/10 on Pi 5 with preemption enabled.

---

## Cross-platform coordination (prerequisite infrastructure)

A cross-plan review covering the Pi 5, Jetson, and x86-64 preemption/SMP plans identified four small infrastructure PRs that should land in Week 1 — **before** Phase 2+ of this plan proceeds — to eliminate guaranteed merge conflicts with the parallel Jetson and x86-64 tracks. Each is small (15 minutes to ~2 hours of work). The Pi 5 plan's Phase 3-4 work assumes these have landed.

| # | Infrastructure PR | Owner | Pi 5 plan dependency |
|---|---|---|---|
| 1 | **Rename `PI5_SECONDARY_PREEMPT` → `SECONDARY_PREEMPT`** in `CMakeLists.txt`, `kernel/sched/preempt.c`, `kernel/sched/sched.c`, `kernel/arch/arm64/vectors.S`. Keep a backward-compat `#define PI5_SECONDARY_PREEMPT SECONDARY_PREEMPT` so existing build invocations don't break. | Either platform's first activation PR | Phase 3d (CMake flip) and Phase 1b (`vectors.S` `#if` guards) reference the new symbol. The Pi 5 plan defers symbol naming to whichever PR lands first. |
| 2 | **`smp_notify_cpu()` abstraction** in `kernel/include/smp.h` (or new `kernel/include/smp_arch.h`). Wraps platform-specific cross-CPU wake (`SEV` on ARM64, `IPI` on x86-64). Initial bodies are platform-conditional. | Cross-platform | Phase 4a's `bench smp` tests rely on whichever path the abstraction picks for ARM64; if Jetson lands work-stealing-related changes that switch to the new helper, Pi 5 must follow. |
| 3 | **MPIDR → cpu_id helper consolidation.** A single `kernel/include/cpu_id.h` (or extension of `smp.h`) exposing `static inline uint32_t cpu_logical_id_from_mpidr(uint64_t)`. Replaces the inline `(mpidr & 0xFF) | ((mpidr >> 8) & 0xFF)` hack used in seven sites today (`vectors.S` trampoline, `task.c`, `sched.c`, `smp.c`, `exceptions.c`, multiple NC trace points). The Jetson plan's P3 step 2 prescribes the dual-cluster MPIDR fix; the helper hides that platform variation behind one symbol. | Jetson (per Jetson P3) — Pi 5 consumes | Phase 1b's `DIAG_BUMP_VEC` macro and Phase 4a's secondary-CPU paths use the inline hack today; once the helper exists, both should call it instead. Until then, the inline hack continues to work on Pi 5 (`Aff1` only) but must be tested on Pi 5 after the Jetson PR lands so the dual-cluster fix doesn't regress the single-cluster case. |
| 4 | **UART ISR-reentrance audit + per-platform NC lock.** Audit every function reachable from `el1_irq_handler → timer_handler → scheduler_tick` (and the FIQ counterpart) across all platforms; strip `uart_printf`/`uart_puts`/`DEBUG_PRINT` calls or gate behind `uart_trylock`. The Pi 5, Jetson, and QEMU UART drivers each get the same treatment. | Cross-platform | Phase 2's pre-condition (currently scoped Pi-5-only in this plan) should be folded into this shared PR. Removes a duplicated audit between platform tracks. |

**Sequencing.** Items 1, 2, and 4 should land before either platform's preemption activation PR. Item 3 should land before — or as part of — Jetson P3 step 2; the Pi 5 plan can reuse it lazily as Phase 1b's macro and Phase 4a's tests are touched.

**Already-merged infrastructure that other tracks depend on:**

- **ELR trampoline** (`kernel/arch/arm64/vectors.S`, `kernel/sched/preempt.c`) — Pi 5 owns this implementation (PR #98 merged). The Jetson plan should reference it as a dependency rather than re-deriving the same trampoline; both ARM64 platforms share the same `switch_to`-from-exception hazard. The Jetson plan's earlier `context.daif` flip (cross-plan C5) should be dropped in favour of using this trampoline.

---

## Phase 1 — Root-cause #99 (diagnostic-only, no behavior change) *[superseded — executed; results recorded in `docs/pi5-irq-investigation-2026-04.md`]*

The blocker has multiple plausible causes, indistinguishable from EL1 alone because most of the relevant registers (`HCR_EL2`, `SCR_EL3`, `CNTHCTL_EL2`, `IGROUPR` from non-secure) cannot be read post-boot. Capture them at EL2 during boot and expose the snapshot via non-cacheable memory; simultaneously turn the silent `el1_fiq` hang into a first-class diagnostic so we stop missing FIQ deliveries.

### 1a. EL2 register snapshot in `boot.S`

Extend `boot.S` right before the `eret` to EL1 (locate by searching for the `SPSR_EL2` write and `eret` pair — line numbers have drifted from earlier revisions). Write, to a fixed NC slot, these `u64` values:

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
| `0xFF40` | `GICD_IGROUPR[0]` **post**-our-write (readback — critical) |
| `0xFF48` | `GICD_IGROUPR[0]` pre-our-write |

The IGROUPR readback is load-bearing: if we wrote `0xFFFFFFFF` to `GICD_IGROUPR[0]` from non-secure EL2 and the readback still shows `0` (or anything other than what we wrote), the writes are being silently discarded by the GICv2 security model. That directly implicates the FIQ hypothesis below.

Add a shell command `diag el2` that dumps these slots. No behavior change; only visibility.

### 1b. Per-vector exception counters + real FIQ handler

Increment a per-CPU NC counter at the very first instruction of `el1_irq`, `el1_sync`, `el1_fiq`, `el1_serror` (before `save_regs`). Expose via the `cpu` shell command. The counter increment computes the logical CPU index from `MPIDR_EL1`. Use the inline hack `(mpidr & 0xFF) | ((mpidr >> 8) & 0xFF)` for the initial implementation (consistent with every other `PI5_*` site); once Infrastructure PR #3 (cross-platform MPIDR helper) lands, switch to the helper so the dual-cluster Jetson case works the same way.

**Critical:** today `el1_fiq: b hang` (in `kernel/arch/arm64/vectors.S`) silently consumes FIQs. If timer IRQs are arriving as FIQ because they remained in GIC Group 0, this handler is the reason `timer_handler_count` stays `0` without *any* diagnostic trace. Replace with a real handler:

```asm
el1_fiq:
    /* NC counter for diagnostic (same pattern as el1_irq counter). */
    save_regs
    bl      el1_fiq_handler        /* C handler in exceptions.c */
    restore_regs
    eret
```

`el1_fiq_handler()` in `exceptions.c` initially reads the same GIC interrupt-acknowledge register the IRQ path uses (on GICv2 that's `GICC_IAR` for Group 1 and `GICC_AIAR` for Group 0 — the handler must be aware of both). It logs the observed IRQ number via NC trace and returns. This isolates the "FIQ-delivered timer" case; Phase 2d below turns it into a working timer path if that is indeed the cause.

### 1c. Decision table

Force `DAIF.I=0` and `DAIF.F=0` briefly in `main` (before shell spawns), read diagnostics:

| Observation | Root cause implicated |
|---|---|
| `el1_fiq` counter increments; `GICC_AIAR` returns IRQ 30 | **Primary hypothesis confirmed:** Timer IRQs arriving as FIQ — PPI 30 is in Group 0, non-secure IGROUPR writes were ignored |
| `GICD_IGROUPR[0]` post-write readback `≠ 0xFFFFFFFF` | Same as above (GICv2 security model prevents NS→Group-1 promotion) |
| `el1_irq` counter increments but `el1_irq_handler` hangs | Routing works; handler-side bug (revisit `ac46e40`-style regression) |
| `HCR_EL2.IMO` set post-our-write | IRQs routed to EL2 instead of EL1 |
| `CurrentEL != 8` at firmware entry | Firmware entered at EL1; EL2 block skipped entirely |
| All exception counters stay 0 | Distributor-level blocking, `SCR_EL3`, or timer not actually firing |
| `SCR_EL3` unreachable (traps) | Expected; TF-A controls EL3 |

### 1d. External reference comparison

Fetch into `~/slmos-ref/` if not already present (per `CLAUDE.md` reference cache rules):

- `linux/arch/arm64/kernel/entry.S` — EL1 IRQ vector on non-secure EL1.
- `linux/drivers/clocksource/arm_arch_timer.c` — init order for CNTP vs CNTV, `CNTKCTL_EL1` programming.
- `linux/drivers/irqchip/irq-gic.c` — GICv2 init for non-secure EL1 boot, Group 1 preservation.
- U-Boot `arch/arm/cpu/armv8/start.S` for BCM2712 — what firmware state precedes the kernel.

### Phase 1 deliverable

A write-up at `docs/pi5-irq-investigation-2026-04.md` naming the root cause with register evidence. Go/no-go for Phase 2 requires a named cause.

---

## Phase 2 — Fix timer IRQ delivery *[superseded — no viable fix from EL1/EL2; tracked as post-capstone #134]*

Branches based on the Phase 1 finding. Paths are ordered by likelihood per external-review analysis; the first confirmation in Phase 1c drives selection.

### Pre-condition (shared) — UART reentrance audit

**Blocking prerequisite** before any DAIF unmask ships: audit every function reachable from `el1_irq_handler → timer_handler → scheduler_tick` (and the equivalent FIQ path if taken) and confirm **none** call `uart_printf`, `uart_puts`, `DEBUG_PRINT`, `INFO`, `WARN`, or any other UART-emitting routine. A task spinning on `FR_TXFF` in `uart_putc` that gets preempted, then re-enters UART output from IRQ/FIQ context, deadlocks on the implicit UART lock. This is the likely root cause of the `ac46e40` shell-banner hang at `[2]SLM-[a]OS`. Strip hits or gate them behind `uart_trylock`. Record the audit result as a sign-off before Phase 3.

**Cross-platform note:** per the cross-plan review, this audit should be done as **Infrastructure PR #4** (see "Cross-platform coordination" above) covering the Pi 5 PL011 path, the Jetson UARTC/TCU path, the QEMU PL011 path, and the x86-64 16550 path together — not as a Pi-5-only follow-up. Land the shared audit PR before this Phase 2's branch picks up its consequences.

### 2a. If cause is "timer IRQs delivered as FIQ" (leading hypothesis)

Two sub-options:

- **2a.1 — Handle the timer in the FIQ vector (fast path).** `el1_fiq_handler()` dispatches on `GICC_AIAR` (Group 0 ACK); when it sees IRQ 30, invoke the existing `timer_handler()`. `switch_to`-from-exception safety is identical to the IRQ path because the ELR trampoline (PR #98) already decouples scheduling from exception context. Ugly but correct; gets preemption working in one deploy cycle.
- **2a.2 — Re-enable `armstub8-2712.bin` to configure Group 1 from EL3 (clean path).** On GICv2 with Security Extensions, only Secure EL3 can promote an interrupt from Group 0 to Group 1. This is the architecturally correct fix and the only way to run the timer through `el1_irq`. Blocked on resolving the ~60% boot-garble rate that led to the armstub being disabled; that diagnosis is a separate sub-task.

Recommend landing 2a.1 first so preemption + SMP unblock, then 2a.2 as a later cleanup that migrates the timer back to the IRQ vector behind the same `PI5_SECONDARY_PREEMPT` kill switch.

### 2b. If cause is `HCR_EL2.IMO=1` (IRQs routed to EL2)

In `boot.S`'s EL2 block, change the `HCR_EL2` write (locate by searching for `msr hcr_el2` near the `mov ... #(1 << 31)` sequence) to explicitly clear `IMO | FMO | AMO` rather than assuming firmware left them at `0`. Add a `dsb sy` and a readback into the NC snapshot (Phase 1a offset `0xFF08`) to confirm the write held.

### 2c. If cause is "firmware entered at EL1, EL2 block skipped"

The EL2 block (`boot.S`'s `cmp x_, #8` / `b.ne` / … / `eret` region) never executes; nothing configures `HCR_EL2`, `CNTHCTL_EL2`, `IGROUPR`, or `GICD_CTLR`. Options:

- **c.1 — Re-enable `armstub8-2712.bin`.** Same as 2a.2 — the only architecturally correct fix and the only way to reach EL3 to set the right groups.
- **c.2 — Configure GIC Group 1 from non-secure EL1.** **Not viable** on GICv2 with Security Extensions: non-secure writes to `GICD_IGROUPR` cannot promote Group 0 interrupts to Group 1. The initial plan listed this as preferred; the external review correctly flagged it as a no-op on this hardware.
- **c.3 — Move to virtual timer (CNTV, IRQ 27).** Historical commit `12bef99` paired virtual timer with the armstub. Requires `CNTVOFF_EL2=0` which must be set at EL2 — not viable if firmware entered at EL1.

Recommend c.1 (armstub re-enablement); handle boot-garble diagnosis as a dependent sub-task.

### 2d. If cause is "exception delivered but handler hangs" (handler-side bug)

With Phase 1b's counters in place, this case surfaces as `el1_irq` counter incrementing but `timer_handler_count` staying low or frozen. Candidate fixes:

- Verify `gic_end_interrupt(irq)` completes before `scheduler_tick` returns (confirm at the active line in `exceptions.c`; previous investigations located it around the timer case of `el1_irq_handler`).
- Confirm the trampoline's `preempt_disabled[cpu]=1`-before-`daifclr` window holds under repeated fire.
- Apply the UART-reentrance audit from the Phase 2 pre-condition — this is likely the root cause regardless of delivery path.

### 2e. Secondary-CPU GIC configuration

`kernel/sched/smp.c` / `smp_boot.S` perform the EL2→EL1 drop for secondary CPUs but do **not** replicate the primary CPU's GIC configuration. For PPIs (per-CPU private interrupts like timer IRQ 30), `GICD_IGROUPR` bits are banked — each CPU has its own copy — so even a working primary-CPU setup does not carry over. Whatever GIC configuration resolves the primary CPU issue must also run in `secondary_init()` or the secondary-CPU EL2 block, before `scheduler_start()` arms the local timer.

- If the fix path is 2a.1 (FIQ handler), nothing extra here — the vector table is shared across CPUs.
- If the fix is 2a.2 / 2c.1 (armstub), TF-A must configure PPIs for all CPUs; verify in the armstub source.
- If the fix is 2b (`HCR_EL2` clear), replicate in the secondary EL2 block.

### 2f. Validation for Phase 2

- `cpu` shell command shows `timer_handler_count > 0` and `sched_diag_tick[0] > 0` within ~100 ms of boot.
- A spin-forever task pinned to CPU 0 is preempted — visible because shell responds to input while it runs.
- `labctl boot_test --count 10` returns 10/10.

### Phase 2 deliverable

Branch `fix/99-pi5-timer-irq-delivery`, merged to main, closing issue #99.

---

## Phase 3 — Task `DAIF` unmask (safe to deploy) *[superseded — unnecessary under coop-preempt; applies only if Phase 2 is ever delivered]*

With #99 fixed, tasks must run with `DAIF.I=0` so timer IRQs can actually preempt them. Reinstate what `ac46e40` originally proposed:

### 3a. Task entry unmask

Re-enable `daifclr #2 + isb` in `kernel/sched/task.c:task_entry_trampoline`, gated on the secondary-preempt CMake symbol (`SECONDARY_PREEMPT` after Infrastructure PR #1 lands; `PI5_SECONDARY_PREEMPT` if Phase 3 begins before that PR — the backward-compat alias keeps either name working). Present in PR #98 branch history; needs un-commenting.

### 3b. Context switch DAIF handling

`kernel/arch/arm64/context.S` already restores `DAIF` **last** (PR #98), closing the `ac46e40` "DAIF restored before SP/GPRs → mid-restore IRQ corrupts state" race. Re-verify by inspection after Phase 2 lands. The Jetson plan should drop its earlier `context.daif = 0` flip in favour of relying on this same fix; raised under cross-plan C5.

### 3c. Boot-stack `daifclr` removal

`scheduler_start()` does not `daifclr` on the boot stack before `switch_to(NULL, first_task)` when the secondary-preempt symbol is on (PR #98). Confirm this remains off for Pi 5; an IRQ on the boot stack mid-`switch_to` would be unsafe because the boot stack is not a valid task stack.

### 3d. Activate the kill switch

Flip the CMake option default to `ON`. Use the symbol name that exists at the time this phase runs:

```cmake
# After Infrastructure PR #1 lands (the rename):
option(SECONDARY_PREEMPT ... OFF)        →  option(SECONDARY_PREEMPT ... ON)

# Before Infrastructure PR #1 lands (use the original symbol):
option(PI5_SECONDARY_PREEMPT ... OFF)    →  option(PI5_SECONDARY_PREEMPT ... ON)
```

Keep the option available so regression bisects can still turn it off.

### Phase 3 deliverable

Pi 5 boots cleanly to shell with preemption active on CPU 0. No regression in QEMU or Jetson (kill switch gates everything).

---

## Phase 4 — Validate preemption on all 4 CPUs *[partially superseded — the cross-CPU tick validation and `test_coop_preempt.c` tests landed via PR #126. The 5 previously-ignored integration tests passed on QEMU after the yielding `delay()` change; running them on Pi 5 hardware boot-tests remains a follow-up.]*

### 4a. Cross-CPU tick delivery

`bench smp` dispatches tasks to CPUs 1-3 and waits. Each task writes an NC slot and exits. With preemption working:

- Every CPU's `sched_diag_tick[cpu]` must advance (currently only CPU 0's does, and only when idle).
- `bench smp` completes in ≤100 ms (currently ~2 s in cooperative mode).

### 4a.1. Secondary-CPU boot-stack headroom

`smp_boot.S` sets per-CPU boot stacks at 16 KB (`lsl x2, x2, #14`) while task stacks are `STACK_SIZE = 64 KB`. Cooperative mode barely exercises the secondary boot stack, but preemption drives it much harder (ISR entry → trampoline → `schedule()` → potential `switch_to` — all on top of whatever depth `secondary_init()` leaves behind). Before Phase 4b runs, inspect `secondary_init()` stack depth (any allocator / task-creation calls) and raise the per-CPU boot stack to 32 KB — or 64 KB to match task stacks — if the new trampoline + scheduler path meaningfully approaches the limit. Stress `bench smp` with deliberately deep call chains as a sanity check.

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

## Phase 5 — Documentation + issue cleanup *[completed in PR #126]*

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

## Revision notes

- **2026-04-13 (rev 3):** Cross-plan review (Pi 5 + Jetson + x86-64) integrated. Key changes:
  - Added a **"Cross-platform coordination"** section near the top listing four prerequisite infrastructure PRs (rename `PI5_SECONDARY_PREEMPT`, `smp_notify_cpu()` abstraction, MPIDR helper consolidation, shared UART-reentrance audit). All four should land in Week 1 before Phase 2+ proceeds, and the Pi 5 plan defers symbol naming to whichever PR lands first.
  - Phase 1b now references the upcoming MPIDR helper (Infrastructure PR #3) — interim implementation uses the existing inline hack consistent with other `PI5_*` sites.
  - Phase 2 pre-condition (UART-reentrance audit) explicitly redirected to **Infrastructure PR #4** so the audit covers Pi 5, Jetson, QEMU, and x86-64 in one PR rather than being repeated per-platform.
  - Phase 3a/3d updated to reference `SECONDARY_PREEMPT` (post-rename) with a fallback to `PI5_SECONDARY_PREEMPT` if Phase 3 begins before Infrastructure PR #1 lands.
  - Phase 3b notes that the Jetson plan should drop its `context.daif = 0` flip in favour of the same `context.S` DAIF-restore-last fix already in PR #98 (cross-plan C5).
  - Cross-plan ELR-trampoline ownership clarified — Pi 5 owns the implementation (PR #98 merged); Jetson references it as a dependency rather than re-deriving.

- **2026-04-13 (rev 2):** External review pass integrated. Key changes:
  - Elevated the **FIQ-delivery hypothesis** (`el1_fiq: b hang` silently consuming Group 0 timer IRQs) to the primary cause in Phase 1c; added an explicit Phase 2a fix path (FIQ handler as fast path, armstub re-enablement as clean path).
  - Marked the original "configure Group 1 from non-secure EL1" approach (old 2b.2) as **non-viable**: GICv2 with Security Extensions does not allow NS→Group-1 promotion. Only EL3 can.
  - Added **`GICD_IGROUPR[0]` post-write readback** (Phase 1a offset `0xFF40`) so we definitively detect when non-secure group writes are silently discarded.
  - Added a **real `el1_fiq` handler** to Phase 1b — today's `b hang` is a diagnostic black hole regardless of FIQ root cause.
  - Added a **UART-reentrance audit** as the shared Phase 2 pre-condition blocking Phase 3 (likely root cause of the `ac46e40` `[2]SLM-[a]OS` hang).
  - Added Phase 2e — **secondary-CPU GIC configuration** (IGROUPR is banked per-CPU; whatever fix lands must replicate for CPUs 1–3).
  - Added Phase 4a.1 — **secondary-CPU boot-stack headroom** check; 16 KB is tight under preemption.
  - Replaced stale `boot.S:XXX-YYY` line-number references with pattern/symbol searches to survive source drift.

## rev 4 — post-resolution banner (2026-04-13 later)

- Added the top-of-file resolution banner after PR #126 merged. #99 was resolved mid-execution by cooperative preemption (`PI5_COOP_PREEMPT`), not by the hardware-IRQ-path fix Phases 1-4 had prescribed.
- Marked Phases 1-4 inline as superseded or partially-superseded, pointing readers at the artifacts that replaced them (`docs/pi5-preemption-resolution.md`, PR #126, issue #134 for the post-capstone hardware-IRQ investigation).
- Marked Phase 5 as completed.
- Kept the **Cross-platform coordination** section unchanged — its four prerequisite infrastructure PRs still apply to the Jetson and x86-64 tracks and haven't landed.

*Last updated: 2026-04-13 (rev 4).*
