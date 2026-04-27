# Pi 5 Preemption Resolution — `PI5_COOP_PREEMPT`

**Date:** 2026-04-13
**Issue:** #99 (Pi 5 timer IRQs not delivered)
**Status:** Resolved for the capstone's preemption + SMP requirements.

---

## TL;DR

Pi 5 cannot deliver timer IRQs to EL1 through any path accessible from kernel-space code on the current firmware/TF-A configuration. Detailed hardware debugging (see below) eliminated every practical EL2/EL1 fix. The resolution is **cooperative preemption driven by `CNTPCT_EL0`**: `schedule()` checks the hardware counter on every entry and synthesizes a `scheduler_tick()` call whenever ≥10 ms has elapsed on that CPU since the last tick. This gives the scheduler the same observability as real timer IRQs — `sched_diag_tick`, `pit_ticks`, `timer_handler_count`, `active_policy->tick()` all fire — for any workload that yields (which is all message-router / lock / UART-polling tasks). Tasks that busy-wait without yielding still won't preempt mid-execution; that is a documented limitation.

All four CPUs now tick; `bench smp` completes in ~ms; the message router ack-timeout path works; boot reliability is 10/10 on `labctl boot_test`.

---

## What was tried and why it didn't work

The plan's Phase 1 diagnostic added an EL2 register snapshot and per-vector exception counters. Phase 2's investigation then eliminated each candidate fix:

| Candidate | Result |
|---|---|
| "Non-secure IGROUPR writes silently discarded → timer arrives as FIQ" (original hypothesis) | **Incorrect.** `GICC_HPPIR=30` at runtime proves timer IRQ **is** Group 1 visible to non-secure. `GICD_IGROUPR` reads as `0` from non-secure regardless of actual state — the readback was a misleading signal. |
| `GICC_CTLR` bypass-disable bits cleared by `gic_init()` (bit 5 = `IRQBypDisGrp1`) | **Partial fix landed.** `gic_cpu_init()` now preserves bypass bits from whatever `boot.S` / firmware left. Post-fix `GICC_CTLR=0x21`. Timer still does not deliver — bypass bits aren't the blocker. |
| FIQ fast-path: el1_fiq dispatches `GICC_AIAR` to `timer_handler()`, idle unmasks `DAIF.F` (`daifclr #3`) | Both counters still zero — FIQ is not being delivered to the CPU either. |
| Probe `ICC_SRE_EL2` to check if GICv3 sysreg interface is forcing IRQ to a non-existent path | **Fatal:** `mrs ICC_SRE_EL2` from EL2 traps to EL3; TF-A's EL3 handler halts the CPU. Empirically confirmed by a bricked boot. The `ICC_SRE_EL3.Enable` bit is `0` — TF-A explicitly disallows lower ELs from reading/writing the register. |
| Re-enable `armstub8-2712.bin` at EL3 to configure GIC groups from Secure state | **Breaks PSCI.** Even a minimal pass-through armstub (no SCR_EL3 modification, no GIC writes) causes `psci_cpu_on()` to hang during secondary-CPU bring-up. Root cause is in how Pi 5 firmware + TF-A hand off to the armstub: the kernel gets stuck at "CPU map: 4 CPUs configured" before any secondary comes up. The SMD-allow fix (`SCR_EL3.SMD=0`) did not resolve it; preserving TF-A's other `SCR_EL3` bits did not resolve it; the `~60% boot garble` issue documented when the armstub was originally disabled remains, and the fix requires reverse-engineering Pi 5 firmware's armstub ABI. |
| Write `ICC_SRE_EL3 = 0` from the armstub to force memory-mapped GIC | **UNDEFs on this CPU variant.** `msr ICC_SRE_EL3, xzr` at EL3 halts the boot before any serial output — the BCM2712 Cortex-A76 configuration apparently lacks the GICv3 sysreg interface, and accessing those system registers is undefined. |

The net of this investigation: **non-hardware-level fixes are blocked by firmware/TF-A policies we cannot change without a working armstub, and getting the armstub working requires yet another investigation sub-track.** Given the capstone timeline, the pragmatic path is to stop waiting for hardware timer IRQs and drive scheduling decisions from a source that already works: the always-running `CNTPCT_EL0` counter.

---

## The resolution — `PI5_COOP_PREEMPT`

New CMake option (default `ON` for `PLATFORM=RASPI5`), gating a single addition in `kernel/sched/sched.c`:

```c
static void coop_preempt_maybe_tick(uint32_t cpu)
{
    uint64_t freq = timer_get_frequency();
    if (freq == 0) return;
    uint64_t period = freq / TIMER_HZ;        /* cycles per 10 ms tick */
    uint64_t now  = timer_get_count();        /* CNTPCT_EL0 — always advances */
    uint64_t last = coop_last_tick_cntpct[cpu];
    if (last == 0) { coop_last_tick_cntpct[cpu] = now; return; }
    if (now - last < period) return;

    coop_last_tick_cntpct[cpu] = last + period;

    timer_handler_count++;
    pit_ticks++;

    /* Preempt-disabled bracket around the tick call so scheduler_tick's
     * legacy "call schedule()" side effect doesn't recurse into schedule().
     * We're already inside schedule() — policy->tick should fire but the
     * tick-driven schedule call itself is redundant. */
    int prev = preempt_disabled[cpu];
    preempt_disabled[cpu] = 1;
    scheduler_tick();
    preempt_disabled[cpu] = prev;
}
```

Called from the top of `schedule()`. No other code changes; the coop-preempt path is additive and kill-switched.

### What this unlocks for the capstone

| Property | Status on Pi 5 |
|---|---|
| `sched_diag_tick[cpu]` advancing on all CPUs | ✅ (was zero on all CPUs) |
| `timer_handler_count` advancing | ✅ 152 ticks after a single `bench smp + cpu` sequence |
| AI scheduler's `active_policy->tick()` running | ✅ (runs from `scheduler_tick`) |
| AI scheduler deadline updates | ✅ |
| Cross-CPU dispatch (`bench smp`) | ✅ 3/3 target CPUs completed their dispatched task |
| Secondary CPUs' ticks advance when they are busy | ✅ CPU 1–3 at 38–39 ticks each after brief workload |
| Message-router ack timeout (`msg send`) | ✅ 5.0 s (uses `CNTPCT` already, per PR #102 / #80) |
| Boot reliability | ✅ `boot_test --count 10` → 10/10, avg 8.6 s |

### What this does **not** unlock

- **Mid-execution preemption of a task with no yield points.** The scheduler can't intervene until the running task calls `yield()`, enters a blocking primitive, or exits. For capstone workloads — inference tasks that poll message queues, UART-driven tasks, tasks that acquire locks — this is already covered because they yield. A pure CPU-bound task with a `nop` busy-wait loop still monopolizes its CPU. The integration tests' `delay()` helper is a busy-wait; tests that rely on "the scheduler forcibly interrupted a running task mid-delay" will not pass under coop-preempt. `test_multicore_basic` / `test_task_lifecycle` / `test_lock_contention` work fine because they use `yield()` or spinlocks at natural boundaries; `test_task_migration` / `test_stress_multicpu` need `delay()` replaced with yielding waits to pass, which is an easy follow-up but tangential to capstone deliverables.

---

## Relation to the existing preemption infrastructure

- **PR #98** (ELR trampoline for `switch_to`-from-IRQ safety) remains correct and on main but is **inert** under coop-preempt — `scheduler_tick` now runs from `schedule()` (task context), not from an exception handler, so there is no "switch_to from exception" situation to trampoline around. The trampoline would activate if a future armstub or sysreg fix re-enables hardware timer IRQs.
- **PR #124** (Phase 1 diagnostics — `diag` shell command, per-vector exception counters, real `el1_fiq` handler) remains a core artifact. The `diag` command is how we'll keep monitoring for regressions or detect if a future firmware update suddenly makes hardware IRQs start working.
- **PR #126** (FIQ dispatch + `GICC_CTLR` bypass preservation) — the bypass-preservation fix in `gic.c` is good hygiene and should land regardless. The FIQ dispatch code is no-op today but harmless.

---

## Known follow-ups (outside capstone scope)

1. Restore true hardware timer IRQ delivery by debugging the armstub boot-garble / PSCI hand-off. Likely requires reverse-engineering Pi 5 firmware's armstub ABI or an upstream firmware fix.
2. Convert `kernel/tests/test_integration.c` `delay()` helper to a yielding variant so the preemption-dependent tests pass under coop-preempt.
3. ~~Extend coop-preempt to Jetson if its secondary-CPU preemption turns out to have analogous issues.~~ **Done (2026-04-15).** Jetson has the same fundamental limitation expressed through GICv3 + TF-A instead of GICv2 + armstub. See `docs/jetson-preemption-investigation.md` for the 8-path investigation and empirical evidence (SCR_EL3=0x3073d with FIQ=1, all PPIs and SPIs in Group 0, ICC_IGRPEN0 reads trapped to EL3). COOP_PREEMPT is the correct mechanism on both platforms.

## Reproducing the Pi 5 diagnostic

The `timdiag` shell command (added 2026-04-15) dumps the live GIC + timer state on any ARM64 platform. Run `timdiag` after boot to see the current group configuration and confirm why hardware IRQ delivery is blocked. On Pi 5 this shows GICv2 state; the same command shows GICv3 state on Jetson. See `docs/shell.md` for the command description and `docs/jetson-preemption-investigation.md` for the GICv3 interpretation.

*Last updated: 2026-04-15.*
