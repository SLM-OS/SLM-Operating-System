# Jetson Orin Nano — Capstone Execution Plan

**Companion to:** `docs/jetson-capstone-gap-analysis.md`.
**Purpose:** turn the gaps identified in the analysis into an ordered, testable sequence of engineering phases. Each phase below is self-contained: prerequisites, concrete steps, exit criteria, and effort.

**Three parallel tracks:**

- **Track P — Preemption** (unblock timer-IRQ delivery on Jetson, then enable preemption on all CPUs).
- **Track S — SMP polish** (NC-memory placement, observability, work-stealing benchmarks).
- **Track G — GPU/Inference pivot** (accept the GSP blocker; ship CPU NEON inference + cross-platform benchmarks).

Tracks P and G are largely independent and can run in parallel. Track S has phases that depend on P (specifically S3 benchmarking needs P3) but the initial phases (S1, S2) can start immediately.

---

## Critical-Path Dependency Graph

```
P1 (diagnose timer IRQ)
 └─→ P2 (CPU 0 preemption working)
      ├─→ P3 (secondary preemption via trampoline)
      │    └─→ P4 (NC-memory UART lock)
      │         └─→ P5 (DAIF.I invariant audit)
      │              └─→ P6 (regression sweep)
      │
      └─→ S3 (Phase C benchmarks on Jetson hardware)

S1 (deque → NC memory)  ─┐
S2 (steal metrics)       ─┴─→ S3 → S4 (flip default) → S5 (load balancing)

G1 (NEON MatMul)
 └─→ G2 (NEON Conv)
      └─→ G3 (Softmax, LayerNorm, misc)
           └─→ G4 (INT8/FP16 quantization)
                └─→ G5 (cross-platform benchmark)
                     └─→ G6 (thesis framing)
```

The **minimum capstone path** is `G1 → G2 → G3 → G4 → G5 → G6`. Tracks P and S are upgrades; their absence does not invalidate the capstone.

---

## Shared Infrastructure Prerequisites (Week 1)

The Jetson plan runs in parallel with the Pi 5 and x86-64 plans in `docs/pi5-preemption-plan.md` and `docs/x86-64-capstone-closure-plan.md`. Several items touch shared code (`kernel/sched/sched.c`, `kernel/arch/arm64/vectors.S`, `runtime/src/inference/ops.rs`, `kernel/include/config.h`). Landing the four preparatory PRs below in Week 1 — before any platform-specific preemption or inference work begins — eliminates every guaranteed merge conflict between the three plans.

Each is small (15 minutes to a day of scope). Total Week-1 effort: roughly half a day.

### Prereq #1 — `el1_fiq` handler + per-vector NC counters (Pi 5 plan owns) ✅ DONE

Merged as part of `#99` Phase 1 (commit `e98c3d7`). `kernel/arch/arm64/vectors.S` has real `el1_fiq` / `el1_fiq_sp0` handlers dispatching to `el1_fiq_handler` in `exceptions.c` with per-CPU NC counters. Exposed to diagnostics via the `diag gic` shell command (from `#99` Phase 2, commit `1eb5e80`).

**Why Jetson depends on this:** P1.0's fast-path hypothesis is that PPI 30 is being delivered as FIQ (because `GICR_IGROUPR0` is never written, leaving PPIs in Group 0). Without the FIQ counter, the fast-path fix succeeds but the diagnostic signal is invisible. If P1.0 fails, the FIQ counter is the primary diagnostic for ruling out the FIQ hypothesis.

### Prereq #2 — Rename `PI5_SECONDARY_PREEMPT` → `SECONDARY_PREEMPT` ✅ DONE

Done in commit `b820bee`. `CMakeLists.txt` exposes `SECONDARY_PREEMPT` (default OFF) and keeps `PI5_SECONDARY_PREEMPT` as a deprecated alias. The RASPI5-only gate is dropped — any NC-memory ARM64 platform (Pi 5 + Jetson) can opt in. `Makefile` forwards both names. Source guards in `preempt.h`, `preempt.c`, `vectors.S`, `sched.c`, `exceptions.c` all renamed. `kernel/sched/preempt.c` now compiles on all ARM64 platforms (it was RASPI5-only).

**Regression test:** `scripts/tests/verify-secondary-preempt-build-matrix.sh` runs 7 build + `nm` checks (default OFF on each platform, new name ON on Pi 5 / Jetson, legacy alias ON on Pi 5, QEMU silently ignored). All pass.

**Caveat:** enabling `SECONDARY_PREEMPT=ON` on Jetson compiles but is not safe at runtime yet — the trampoline's MPIDR formula collides on cluster 1 (P3 step 2 owns the fix).

### Prereq #3 — `smp_notify_cpu(cpu)` abstraction ✅ DONE

Done in commit `5c61ed6`. `kernel/include/smp.h` declares `void smp_notify_cpu(uint32_t cpu)`; implementations in `kernel/sched/smp.c` (ARM64 — `sev` broadcast) and `kernel/arch/x86_64/platform_x86.c` (x86-64 — no-op stub). `scheduler_add_task_to_cpu()` in `sched.c` now calls it unconditionally instead of the `#if !defined(PLATFORM_X86_64)` block.

ARM64 upgrade to targeted SGI deferred until P3 step 3 fixes `gic_send_sgi()` for dual-cluster MPIDR. x86-64 B1 will replace the stub with `lapic_send_ipi(apic_id_of(cpu), VEC_RESCHED, 0)` plus IDT wiring.

**Functional tests:** `test_smp_notify_cpu_safe` (direct API call on every CPU id) and `test_smp_notify_cpu_wakes_secondary` (queues a probe task on CPU 1 and asserts the sentinel write fires within 3 s) in `kernel/tests/test_integration.c`. Both pass in QEMU ARM64 `make test`. Jetson hardware: `bench smp` still 5/5 COMPLETED, confirming the cross-CPU wake path survived the abstraction.

### Prereq #4 — Inference `ops.rs` dispatch skeleton ✅ DONE

Land the `cfg_if!` scaffolding in `runtime/src/inference/ops.rs` for the operators that G1-G3 and x86-64 C1 will both extend:

```rust
pub fn matmul_f32(a: &[f32], b: &[f32], c: &mut [f32], m: usize, n: usize, k: usize) {
    cfg_if::cfg_if! {
        if #[cfg(target_arch = "aarch64")] { matmul_f32_neon(a, b, c, m, n, k); }
        else if #[cfg(target_arch = "x86_64")] { matmul_f32_sse(a, b, c, m, n, k); }
        else { matmul_f32_scalar(a, b, c, m, n, k); }
    }
}
```

Each platform fills its own `_neon` / `_sse` body in subsequent PRs; the scalar fallback ships in the skeleton.

**Why this is a Week-1 PR:** G1 (NEON) and x86-64 C1 (SSE) both start Week 2-3 and both modify the same functions in `ops.rs`. Whichever lands second eats a merge conflict. Landing the dispatch first makes the two tracks truly independent.

Merged as PR #131 (commit `07c0020`). 7 SIMD helper sites in `ops.rs` now use a three-way `aarch64 / x86_64 / not(any)` cfg split. Each `#[cfg(target_arch = "x86_64")]` block currently holds scalar code marked `TODO(x86-64 C1)`; x86-64's Phase C1 fills these in with SSE.

### Prerequisites status summary

All four Week-1 prereqs are DONE:

| Prereq | Status | Commit |
|---|---|---|
| #1 — FIQ handler + per-vector counters | ✅ (Pi 5 plan) | `e98c3d7` + `1eb5e80` |
| #2 — `SECONDARY_PREEMPT` rename | ✅ (Jetson plan) | `b820bee` |
| #3 — `smp_notify_cpu` abstraction | ✅ (Jetson plan) | `5c61ed6` |
| #4 — `ops.rs` dispatch skeleton | ✅ (Jetson plan, PR #131) | `07c0020` |

After these land, Tracks P/S/G in this plan can run without coordination friction.

---

## Track P — Preemption

### Phase P1 — Cooperative preemption on Jetson ✅ DONE

**Outcome (commit `8de6b00`, 2026-04-13):** `timer_handler_count` on jetson-nano-2 advances at run time (`0 → 18 → 36` across two `bench smp` invocations). Cooperative preemption is functional via the `COOP_PREEMPT` mechanism originally written for Pi 5 (#99 resolution), now extended to Jetson by a CMakeLists-only change.

The original P1.0 fast-path hypothesis (write `GICR_IGROUPR0` / `GICR_IGRPMODR0` from the kernel) was tried first and **falsified on hardware** — the writes are silently ignored because Jetson runs at NS EL2 and the GIC has two security states; the Group config is owned by EL3. Same structural blocker as Pi 5's #99. The IGROUPR writes were kept in `gic_redist_init()` as best-effort hardening for platforms without security extensions.

The full diagnostic sequence (P1.1 below) was never needed because the root cause matched #99's at the GIC layer; the resolution was to apply the same cooperative-preempt workaround.

**What this gives us on Jetson:**

- Scheduler tick (`scheduler_tick()`) runs every ~10 ms of wall-clock per CPU at the next yield/`schedule()` entry on that CPU.
- AI policy, deadline boosts, and migration decisions take effect at the next yield.
- `pit_ticks` and `timer_handler_count` advance on all CPUs that yield.
- A pure CPU-bound loop with no yield still monopolizes its CPU — same caveat Pi 5 has documented.

**What this does NOT give us:**

- No timer-IRQ-driven preemption between yield points. P3 (secondary preemption via the ELR trampoline) is moot on Jetson until the underlying IRQ-delivery issue is fixed (see P1.2 below).

#### P1.0 — Fast-path fix attempt (FALSIFIED on hardware)

Original hypothesis (kept here for the historical record):

> Audit of `gic_redist_init()` confirms it never writes `GICR_IGROUPR0` or `GICR_IGRPMODR0`. If the kexec-inherited state leaves PPI 30 in Group 0 (Secure), the interrupt delivers as **FIQ** and hits the FIQ handler — silent black hole.

Implemented (`gic.c` lines added in this branch's first commit):

Audit of `gic_redist_init()` in `kernel/drivers/gic.c:407-448` confirms it never writes `GICR_IGROUPR0` or `GICR_IGRPMODR0`. If the kexec-inherited state leaves PPI 30 in Group 0 (Secure), the interrupt delivers as **FIQ** and hits `el1_fiq: b hang` — a silent black hole that matches the observed zero-IRQ symptom.

```c
GICR_IGROUPR0(cpu)  = 0xFFFFFFFF;
GICR_IGRPMODR0(cpu) = 0x00000000;
```

**Hardware result on jetson-nano-2:** `timer_handler_count` stayed at 0. The `[JDIAG]` boot-time printout added during this work showed `GICR_IGROUPR0=0x0` after the write — i.e. the write was silently ignored. This is consistent with GICv3 + two security states + NS access: writes to those registers from NS are no-ops. The Pi 5 #99 work documented the same pattern on GICv2 (`GICD_IGROUPR` ignored from NS).

The IGROUPR writes were nevertheless kept in `gic_redist_init()` as best-effort. They cost nothing on platforms that ignore them and provide correct behavior on platforms without security extensions (or platforms running at S EL3).

#### P1.1 — Cooperative preemption (the actual resolution)

Apply the Pi 5 #99 resolution to Jetson. The `coop_preempt_maybe_tick()` helper in `kernel/sched/sched.c` polls `CNTPCT_EL0` at the start of every `schedule()` entry and synthesizes a `scheduler_tick()` call when ≥10 ms of wall-clock has elapsed on that CPU since the last synthetic tick. The implementation is platform-agnostic (uses `timer_get_frequency`, `timer_get_count`, `MAX_CPUS`, `TIMER_HZ`).

CMakeLists.txt change (commit `8de6b00`):

- `option(PI5_COOP_PREEMPT ...)` renamed to `option(COOP_PREEMPT ...)`. Legacy `PI5_COOP_PREEMPT` retained as alias.
- Activation extended from `PLATFORM STREQUAL "RASPI5"` to `RASPI5 OR JETSON_ORIN_NANO`.
- Both `COOP_PREEMPT=1` and `PI5_COOP_PREEMPT=1` macros are defined when active so the existing 12 source sites (in sched.c, docs, test_integration.c, test_coop_preempt.c, etc.) keep building. A follow-up PR can rename the source sites; this PR keeps the diff small.

**Hardware result on jetson-nano-2:** `timer_handler_count` advances `0 → 18 → 36` across two `bench smp` invocations. Per-CPU `Ticks` counter shows non-zero values on CPUs 1-5. `bench smp` still 5/5 COMPLETED — no SMP regression.

#### P1.2 — Future: real hardware timer IRQ on Jetson (deferred)

Same shape as Pi 5's #134. Would require either:
- TF-A reconfiguration to surrender the GIC group bits to Non-secure (unlikely without NVIDIA's source).
- A FIQ-routed timer path à la Pi 5's `PI5_FIQ_TIMER` (the el1_fiq handler from #99 Phase 1 already works; would need GICv3-aware GICC_AIAR-equivalent dispatch).

Neither is required for the capstone — cooperative preemption covers the workload.

#### P1.x — Reference: full diagnostic sequence (if needed for future debug)

1. **Add a `gicdump` shell command** in `kernel/src/shell_sys.c` that prints per-CPU GICv3 state:
   - `GICR_CTLR`, `GICR_WAKER`, `GICR_IGROUPR0`, `GICR_IGRPMODR0`, `GICR_ISENABLER0`, `GICR_IPRIORITYR[7]` (PPI 30's priority byte).
   - `ICC_CTLR_EL1`, `ICC_PMR_EL1`, `ICC_IGRPEN0_EL1`, `ICC_IGRPEN1_EL1`, `ICC_SRE_EL1`.
   - `CNTFRQ_EL0`, `CNTP_CTL_EL0`, `CNTP_CVAL_EL0`, `CNTHCTL_EL2`.
2. **Compare output against a reference.** Capture the same registers from Jetson Linux pre-kexec (via a small kernel module or `/dev/mem` reads with CAP_SYS_RAWIO), or from OP-TEE's GIC init trace.
3. **Force a spurious timer IRQ**: set `CNTP_TVAL_EL0 = 0` and enable (`CNTP_CTL_EL0.ENABLE=1, IMASK=0`). If `timer_handler_count` increments, routing works; the bug is in periodic reprogramming. If not, the bug is in GIC/IRQ acceptance.
4. **Audit `gic_percpu_init`** against the ARM GICv3 programming guide for EL2+VHE:
   - `ICC_SRE_EL2.SRE=1` (system register interface, not memory-mapped).
   - `ICC_PMR_EL1 >= 0xF0` (priority mask accepts all priorities).
   - `ICC_IGRPEN1_EL1=1` (Group 1 NS).
   - PPI 30 enabled in `GICR_ISENABLER0`, priority set, Group 1 bit set in `GICR_IGROUPR0`.
5. **Verify VHE timer redirection.** Under VHE, `CNTP_*_EL0` from EL2 accesses the NS EL1 physical timer (INTID 30). Confirm `timer_init` writes the register whose interrupt is routed to PPI 30 (not the EL2 physical timer at PPI 26 or virtual timer at PPI 27). `CNTHCTL_EL2` layout also changes under VHE — not blocking for EL2-only code but worth noting for future EL0 work.

**Exit criteria:**
- `cpu` shell command shows `timer_handler_count > 0` after several seconds of idle.
- `pit_ticks` advances when CPU 0 is idle (captured via sequential `cpu` invocations).

**Effort estimate:** 1 hour for P1.0; 3-5 days for P1.1. If the issue turns out to be EL2/VHE timer-register semantics or TF-A residual state, up to 2 weeks.

**Tracking:** file new issue "Jetson: timer IRQs not delivered on any CPU post-kexec" (P1-high, platform:jetson, sub:drivers). Link to this plan's P1.0 fast path.

**Fallback / decision gate:** if after 2 weeks the root cause is not clear, pause Track P and proceed only with Tracks S and G. The capstone does not require preemptive scheduling.

---

### Phase P2 — CPU 0 preemption working end-to-end

**Goal:** timer ISR fires periodically on CPU 0; `scheduler_tick()` runs; preemption visibly happens.

**Prerequisites:** P1 complete (exit criteria met).

**Steps:**

1. **Confirm `scheduler_tick()` is called.** Add a per-CPU counter in `sched.c:timer_handler_count` (already exists) — verify it climbs at ~100 Hz on CPU 0.
2. **Confirm preemption visibly switches tasks.** Run `bench context` shell cmd and compare with/without forced yields.
3. **Audit `preempt_disabled[cpu]` flag** — ensure `scheduler_tick` skips `schedule()` when this is set (already implemented).
4. **Verify DAIF state across preemptions.** A task resumed after timer preemption must have `DAIF.I=1` restored correctly from its context. Read task context after a preemption and assert.

**Exit criteria:**
- A long-running CPU-bound task (e.g., a busy-loop for 10s) is observed to yield to the shell task periodically without calling `yield()`.
- `pit_ticks` increments at `TIMER_HZ` (100 Hz) under idle load.
- Existing tests continue to pass (`make test`).

**Effort:** 1-2 days.

---

### Phase P3 — Secondary CPU preemption (port PR #98 to Jetson)

**Goal:** CPUs 1-5 also get timer-driven preemption, via the ELR-trampoline infrastructure that already landed for Pi 5.

**Prerequisites:** P2 complete.

**Steps:**

1. **Confirm Prereq #2 (rename) has landed.** The standalone `SECONDARY_PREEMPT` rename is a Week-1 shared infrastructure PR (see top of this plan). If it has not yet landed, do that first — not as part of P3. The rest of P3 assumes the new symbol exists. Drop the `PLATFORM STREQUAL "RASPI5"` guard in `CMakeLists.txt` here so Jetson builds can enable preemption.
2. **Fix the MPIDR formula in `resched_trampoline`** (`kernel/arch/arm64/vectors.S:313-316`). **This is a correctness blocker for Jetson — the current code produces wrong CPU indices for cluster 1.** Today:
    ```asm
    mrs     x0, mpidr_el1
    and     x1, x0, #0xFF          // Aff0
    ubfx    x2, x0, #8, #8         // Aff1
    orr     x0, x1, x2             // cpu = Aff0 | Aff1
    ```
   Jetson MPIDRs: CPU 0-3 are `0x000, 0x100, 0x200, 0x300` (Aff1 = 0..3, formula correct). CPU 4-5 are `0x10200, 0x10300` (Aff1 = 2, 3) — formula yields **2 and 3**, colliding with CPU 2 and CPU 3's trampoline slots. Result on preemption: corrupted `orig_elr` / `orig_spsr` reads.
   The vectors.S comment at lines 299-312 already flags this and recommends a shared asm macro. Fix options:
   - **Preferred:** add a small NC-memory lookup: compute a hash of the MPIDR affinity bits and index into `cpu_logical_map[]` (already in NC memory per `kernel/sched/smp.c`). Requires loading the map pointer in asm (adrp/ldr) — one-time cost per preemption.
   - **Alternative:** an asm macro `mpidr_to_cpu` that branches on `mpidr & 0x10000` to add 2 when Aff2=1. Works but Jetson-specific; future ports need to touch it again.
   Apply the same fix to the other two `(mpidr & 0xFF) | ((mpidr >> 8) & 0xFF)` sites (task.c task_entry_trampoline, NC trace points in sched.c / exceptions.c / smp.c) — consolidate into the shared macro the comment already asks for.
   **Shared-code impact:** the MPIDR sites in `task.c`, `sched.c`, `exceptions.c`, and `smp.c` are compiled for all ARM64 platforms. The lookup-table approach produces the same result on Pi 5 (where the formula is already correct), but the code path changes. **Test the MPIDR consolidation PR on Pi 5 QEMU and, if available, Pi 5 hardware before merging.** If the Pi 5 plan's Phase 2 validation is in flight when this lands, flag it in the Pi 5 plan's Phase 3 notes for a rebase.
3. **Fix `gic_send_sgi()` for dual-cluster MPIDR** (`kernel/drivers/gic.c:697-712`). The current `(1UL << target_cpu)` target-list assumes Aff0 identifies the CPU. On Jetson, Aff0 is 0 for all CPUs; the target bit must always be bit 0, and the ICC_SGI1R_EL1 value must set `Aff1` (bits 23:16) and `Aff2` (bits 39:32) from the target's MPIDR. Not blocking for timer PPIs (per-CPU), but required before any future cross-CPU IPI notification (e.g., work-stealing wake-ups in Track S).
   Once this is fixed, upgrade the ARM64 `smp_notify_cpu()` implementation (from Prereq #3) from `sev` broadcast to a targeted `gic_send_sgi(cpu, SGI_RESCHED)`.
4. **Enable the trampoline in Jetson builds.** Add `-DSECONDARY_PREEMPT=1` to the Jetson CMake configure, or add a Jetson Makefile option `JETSON_SECONDARY_PREEMPT=ON`.
5. **Add NC diagnostic slots for per-CPU `reschedule_pending`.** The trampoline already uses these for Pi 5; verify no collision with Jetson's NC layout (`0xBDE00000` run queues, `0xBDFFFF00` diag slots).
6. **Test on hardware.** Kick off a CPU-bound task on CPU 4 or 5 specifically (cluster 1) and confirm it is preempted — this exercises the MPIDR fix. Repeat for CPU 1-3.

**Exit criteria:**
- A CPU-bound task on each of CPUs 1-5 is visibly preempted by timer IRQs. Verified by a test that spawns a spin-loop per CPU and checks `task->switches` climbs.
- `bench smp` still 5/5 passes.
- Full test suite passes on Jetson (needs ENABLE_BOOT_TESTS build + kexec).

**Effort:** 5-7 days (was 3-5; MPIDR + SGI fixes add 2 days).

**Tracking:** new issue "Jetson: port PR #98 trampoline for secondary-CPU preemption", blocked by the P1 tracking issue. Sub-tasks: MPIDR formula fix, SGI affinity fix.

---

### Phase P4 — NC-memory cross-CPU UART lock

**Goal:** with preemptive SMP, any CPU may want to print. The current UART lock is IRQ-disable-only (safe cooperative, unsafe preemptive-multi-core). Replace with an NC-memory-based spinlock.

**Prerequisites:**
- P3 complete.
- **Pi 5 plan's UART-ISR-path audit** (Pi 5 Phase 2 pre-condition) has landed — this is a shared prerequisite. The audit strips or gates UART calls from ISR paths across all platforms; it prevents deadlock regardless of lock implementation. P4's NC ticket lock is the *additional* piece needed on ARM64 where `SPINLOCK_SKIP_LOCKING` makes the standard IRQ-disable-only lock insufficient under preemption. x86-64 doesn't need the NC lock — `spin_lock_irqsave` on the UART path is sufficient there, as the x86-64 plan correctly notes.

**Steps:**

1. **Add a small ticket-lock-style structure in NC memory.** Single `uint32_t next` and `uint32_t owner` in `0xBDE06xxx` region. Use `ncmem_alloc`.
2. **Acquire via atomic add on `next`** (on Jetson with `SPINLOCK_SKIP_LOCKING` this must use a fallback — a cross-CPU atomic on NC memory may or may not be supported. Verify first with a small test task before rolling out).
3. **Replace the UART lock's acquire/release paths** in `kernel/drivers/uart_tegra.c` (and `uart_pl011.c` for cross-platform consistency).
4. **Verify under stress.** Spawn 6 tasks that print in tight loops and confirm no interleaving at the character level.

**Exit criteria:**
- A stress test spawns a print-loop task on each CPU for 10 seconds; `bench_shared_buffer`-like verification shows well-formed UART output with no interleaved characters.

**Effort:** 2-3 days. If atomic ops on NC memory trap on Jetson, fall back to a hybrid scheme (atomic in cacheable memory + NC flag) — adds 1-2 days.

**Tracking:** new issue "Jetson: NC-memory cross-CPU UART lock", blocked by P3 tracking issue.

---

### Phase P5 — DAIF.I=1 invariant audit + spinlock discipline

**Goal:** preemptive tasks run with IRQs unmasked between cooperative yields. Many call sites silently assume the old invariant (`DAIF.I=1` throughout task body). Audit each and either relax or explicitly preserve the invariant locally.

**Also:** verify every kernel spinlock path reachable under preemption uses `spin_lock_irqsave`, not plain `spin_lock`. Under `SPINLOCK_SKIP_LOCKING` (Jetson) plain `spin_lock` degrades to `dmb ish` — safe under cooperative but **no mutual exclusion under preemption**. If a preempted critical section is re-entered on the same CPU, both paths run concurrently.

**Prerequisites:** P3 complete.

**Approach — IRQ unmasking mechanism:** adopt the Pi 5 plan's trampoline approach. Keep `context.daif = 0x080` in `task_create_with_priority()` unchanged; the `task_entry_trampoline` in `kernel/sched/task.c` issues `daifclr #2; isb` before calling the task entry function. This matches what Pi 5's Phase 3a already implements and avoids a second, divergent mechanism in shared code.

**Do not** change the default `context.daif` bits in `task_create_with_priority()`. That function is shared across all platforms, and flipping it would change Pi 5's invariant asymmetrically with its own plan.

**Steps:**

1. **Spinlock discipline audit.** Systematically sweep the kernel:
   ```
   grep -rn 'spin_lock(' kernel/ --include='*.c'
   ```
   Every hit that is not wrapped in `irq_save`/`irq_restore` (or isn't `spin_lock_irqsave`) must be either (a) converted to `spin_lock_irqsave`, or (b) documented with a comment explaining why it is safe (e.g., init-time only, never touched from an ISR).
   Known-OK today: `rq_lock_irqsave` in scheduler; `spin_lock_irqsave` in `pi_mutex` (Session B SCHED-H1 fix); `msg_router` Rust SpinGuard (PR #96 IRQ-safe).
2. **Grep all `DAIF.I=1` assumers.** Work outward from `kernel/CLAUDE.md §"Pi 5 Timer IRQs and pit_ticks"`:
   - `msg_router` — now uses `timer_get_count` for timeouts (fixed in #80). OK.
   - `component_runtime` — uses `hw_timeout_*` helpers. OK.
   - Any task body that spins on a flag without yielding. Under preemption, these will work correctly; under cooperative-only, they rely on being scheduled in. Check each for yield() calls.
3. **Verify the trampoline unmask fires on every scheduled task.** Once `SECONDARY_PREEMPT=ON`, a test task that busy-loops should still allow preemption (proving IRQs are unmasked). If it hangs, the trampoline path is not being reached on Jetson — investigate before shipping.
4. **Write `docs/preemptive-multitasking.md`** capturing:
   - New invariant: tasks may be preempted at any instruction.
   - Places that must disable IRQs locally: any read-modify-write on shared state without a lock.
   - Cheat-sheet for `spin_lock_irqsave` vs plain `spin_lock`.
   - The `SPINLOCK_SKIP_LOCKING`-plus-preemption hazard explicitly called out.

**Exit criteria:**
- Spinlock audit complete — every unwrapped `spin_lock()` converted or explicitly justified.
- `docs/preemptive-multitasking.md` published.
- Test suite passes with `SECONDARY_PREEMPT=ON`.

**Effort:** 3-5 days, including audit and doc.

**Tracking:** new issue "Audit DAIF.I=1 invariant + spinlock discipline under preemption".

---

### Phase P6 — Regression sweep and test updates

**Goal:** verify nothing cooperative-only broke, and add tests that specifically exercise preemption.

**Steps:**

1. Run `make test` on QEMU, Pi 5, Jetson with preemption both ON and OFF.
2. Identify tests that assumed cooperative scheduling (e.g., `test_high_priority_runs_first` currently relies on explicit yield ordering). Add `TEST_IGNORE_IF_PREEMPTIVE` guards or rewrite as preemption-agnostic.
3. Add `test_preemption_forces_yield`: a CPU-bound task that never yields; asserts `task->switches > 1` after N ticks.
4. Run `labctl boot_test --count 10` on both Pi 5 and Jetson with preemption ON.

**Exit criteria:**
- All tests pass in both modes.
- Boot reliability >= 9/10 on real hardware.

**Effort:** 2-3 days.

**Total Track P effort:** 2-4 weeks, with P1 being the biggest unknown.

---

## Track S — SMP Polish

### Phase S1 — Move `cpu_steal_deques[]` to NC memory

**Goal:** make work-stealing correct on both Jetson and Pi 5's incoherent L2 by placing the per-CPU deques in NC memory, not cacheable BSS.

**Prerequisites:** none (can start immediately).

**Steps:**

1. In `scheduler_init()` (`kernel/sched/sched.c`), allocate `cpu_steal_deques` from `ncmem_alloc` when `PLATFORM_HAS_NC_MEMORY` is defined. This macro is set on both Pi 5 and Jetson — the change applies to both.
2. `steal_deque_t` is ~280 bytes; with MAX_CPUS=8 that's ~2.2 KB plus cache-line alignment. Fits comfortably in NC memory.
3. Keep the cacheable fallback for x86-64 and any platform without NC memory.
4. Add a regression test that verifies the deques live at a NC address on Pi 5 / Jetson builds.

**Exit criteria:**
- `make test WORK_STEALING=ON` passes on QEMU ARM64 (no NC), Pi 5 QEMU (has NC), and Jetson. Pi 5 hardware testing is desirable but gated on Pi 5 plan's preemption work.
- `cpu` shell command reports deque addresses in NC region on NC platforms.

**Effort:** 1 day.

**Tracking:** follow-up from PR #107 (phase B comment mentions this as TODO).

---

### Phase S2 — Steal success/failure counters

**Goal:** observability for Phase C benchmarking. Closes #105.

**Prerequisites:** none.

**Steps:**

1. Add four per-CPU NC counters next to existing `sched_diag_*` slots:
   - `steal_attempts`, `steal_successes`, `steal_stale_discards`, `steal_empty_victims`.
2. Increment each in `sched_try_steal()` under `CONFIG_WORK_STEALING`.
3. Expose in `cmd_cpu` shell output.

**Exit criteria:**
- `cpu` shell shows new steal columns.
- Counters increment in the expected pattern during `test_work_stealing_distributes_load`.

**Effort:** 0.5 day.

**Tracking:** #105.

---

### Phase S3 — Phase C benchmarks (work-stealing perf data)

**Goal:** collect the numbers that justify flipping `CONFIG_WORK_STEALING` to ON by default (or keeping it OFF).

**Prerequisites:** S1, S2, P3 (preemption on secondaries — stealing only helps when secondaries actually time-slice tasks).

**Steps:**

1. **Extend `bench smp`** with a "load imbalance" scenario:
   - Dispatch N stealable tasks to CPU 1 only.
   - Measure wall-clock time to completion (via `timer_get_count`).
   - Compare with N pinned-to-one-CPU for the baseline.
2. **Run on QEMU, Pi 5, Jetson.** Record p50 and p99 completion time, steal counters, context-switch counts.
3. **Write results into `docs/work-stealing-bench.md`.**

**Exit criteria:**
- Dataset for all three platforms with/without stealing.
- Clear recommendation to flip default (yes/no) based on numbers.

**Effort:** 1 week.

**Tracking:** #59 Phase C (new issue once started).

---

### Phase S4 — Flip `CONFIG_WORK_STEALING` default (multi-platform decision) ✅ DONE (partial)

**Outcome (2026-04-14, PR #167):** flipped to default-ON for QEMU ARM64, Raspberry Pi 5, and x86-64. Jetson stays default-OFF pending #166 (page fault during `bench stealing` after the #158 lock fix).

Implementation landed in `CMakeLists.txt` rather than `kernel/include/config.h` — the per-platform decision stays expressible (Jetson opt-out) with a single `option()` block. The Makefile's `WORK_STEALING` variable was simplified from "explicit ON opt-in" to "ON/OFF override," matching the CMake knob directly and letting empty values defer to the per-platform default.

**Evidence referenced in `docs/work-stealing-bench.md`:**
- QEMU ARM64: `bench stealing 8` → 1.7× (13.4 ms vs 22.9 ms).
- Raspberry Pi 5 hardware: `bench stealing 16` → **3.12×** (12.7 ms vs 39.6 ms), perfect 4/4/4/4 distribution. Captured via labctl on jetson-nano-2's sibling Pi 5.
- x86-64: inherited from Phase B — already ON, cache-coherent SMP + LAPIC IPI.

**Prerequisites (all satisfied):**
- S3 complete ✅ (QEMU + Pi 5 hardware).
- x86-64: ON since Phase B; the proposed B3 coordination point became moot because x86-64 was already green.
- Pi 5 owners: signed off via the Pi 5 hardware capture.

**What was NOT done (deliberate deferrals):**
- Jetson Orin Nano stays OFF-by-default. Opt-in via `make kernel PLATFORM=JETSON_ORIN_NANO WORK_STEALING=ON` continues to work. #166 tracks the residual page fault.
- `kernel/include/config.h` was left unchanged; the CMake `option()` block already defines `CONFIG_WORK_STEALING=1` via `add_compile_definitions`, and config.h remains a fallback default of 0.

**Blockers closed along the way:**
- #139 (ABA race) → per-slot generation counter in `struct task` (PR #145).
- #158 (Pi 5 boot hang) → external cacheable `steal_deque_lock[MAX_CPUS]`, moving the lock out of NC-memory steal_deque_t (PR #167 first commit).

**Known follow-up:**
- #166 (Jetson page fault during bench stealing) — S5 or a later Jetson-focused session.

---

### Phase S5 — Load-balancing enhancements (✅ DONE 2026-04-15)

**Goal:** extend beyond "steal when empty" to "balance proactively on task creation."

**Prerequisites:** S4.

**What landed:**

1. `least_loaded_cpu(fallback, &sum, &active)` helper in
   `kernel/sched/sched.c` scans `cpu_rq(c)->ready_count` for all
   non-isolated CPUs, returning the CPU with the minimum count
   (ties by lowest id) along with the sum and the active-CPU count
   so callers can compute an average without a second pass.
2. In `scheduler_add_task`, after the active policy picks a CPU, an
   override block evaluates whether that CPU is meaningfully
   overloaded: target must have `ready_count >= 2`, the non-isolated
   CPU count must be ≥ 2, and `2 * target_ready * active > 3 * sum`
   (integer form of the plan's `target_ready > 1.5 × avg`
   predicate). Isolated CPUs are excluded on both sides so the
   override never violates `sched_isolate_core`.
3. New regression test
   `test_proactive_load_balance_redirect` in `test_scheduler.c`:
   under a stub policy that always returns CPU 0, adds 6 unpinned
   tasks in an irq-save region and asserts that at least one was
   redirected and CPU 0 did not receive all six.

**Exit criteria (met):**
- Tick-driven rebalancer (pre-S5) and idle-CPU steals handle
  already-imbalanced state; S5 closes the loop by not creating the
  imbalance in the first place. Measurable tail-latency reduction
  is a future benchmark — the override is inert on workloads that
  are already balanced, so no regression risk.

**Notes for future tuning:**
- The threshold `target_ready >= 2` is intentionally conservative
  so the warmth heuristics in `sched_policy_heuristic` (cache
  affinity, deadline boost) stay authoritative for single-task
  additions. Raise the threshold if cache-affinity regressions
  surface under load.

---

**Total Track S effort:** 2-3 weeks, of which S1-S2 (1.5 days) can land immediately; S3 and beyond wait for P3.

---

## Track G — GPU Detection + Inference Pivot

### Phase G1 — NEON MatMul FP32 kernel

**Goal:** efficient CPU MatMul for small-to-medium LLM weights (up to a few hundred MB). Basis for everything downstream.

FP32 only in G1. FP16 and INT8 deferred to G4 where they pair with quantization work — keeping the FP16 intrinsic handling (which may require `unsafe` inline asm rather than stable Rust intrinsics on A78AE) out of the critical path.

**Prerequisites:**
- **Prereq #4 (inference dispatch skeleton) has landed** — see the Shared Infrastructure Prerequisites section at the top of this plan. G1 fills in the `matmul_f32_neon()` body; the `cfg_if!` scaffolding and scalar fallback already exist. Without the skeleton, G1 and x86-64 C1 both modify the same functions in `ops.rs` and eat a merge conflict.

**Steps:**

1. In `runtime/src/inference/ops.rs` (or a new `matmul.rs`), implement NEON-vectorized FP32 MatMul using 128-bit `fmla.4s` intrinsics via `core::arch::aarch64::{vld1q_f32, vfmaq_f32, vst1q_f32}`.
2. Use Rust's `#[cfg_attr(target_arch = "aarch64", target_feature(enable = "neon"))]` on unsafe NEON helpers (`runtime/CLAUDE.md` §SIMD has the pattern, with scalar fallback under `#[cfg(not(target_arch = "aarch64"))]`).
3. **Cache tiling.** Cortex-A78AE has 64 KB L1D, 512 KB L2. Start with a simple 3-level tiled loop (outer K-tile, middle N-tile, inner M-tile); tile the inner product to hold `A_tile + B_tile + C_tile` in ~48 KB. Edge cases for non-tile-aligned matrix dimensions (pad in dispatch or mask loads).
4. Write unit tests comparing NEON output to a scalar reference implementation. Include odd-size matrices (e.g., 257×257) to exercise edge handling.
5. `bench matmul` shell command for quick throughput numbers (GFLOPS) on each platform.

**Exit criteria:**
- Correctness: output matches scalar reference within 1e-5 relative error for random 256×256 and 513×257 FP32 matrices.
- Performance: at least 3-4× speedup over scalar on Jetson (theoretical 4×; tiling losses usually take 10-25% off peak).

**Effort:** 2-3 weeks. The kernel itself is ~200 LOC of unsafe Rust; the tiling logic and edge handling is where the time goes.

**Tracking:** new issue "NEON MatMul FP32 kernel (capstone scope)".

---

### Phase G2 — NEON Conv kernel

**Goal:** Conv for the early layers of vision-capable SLMs. Same treatment as MatMul.

**Prerequisites:** G1 (reuses GEMM primitives via im2col or direct Winograd).

**Steps:**

1. im2col → MatMul for the first pass (reuse G1).
2. Direct NEON Conv for 3×3 stride-1 (most common). Winograd F(2,3) for further speedup on a stretch goal.
3. Unit-test against scalar reference.

**Exit criteria:**
- Correctness + performance (≥ 4× vs scalar for 3×3 Conv).

**Effort:** 1-2 weeks.

---

### Phase G3 — Softmax, LayerNorm, RMSNorm, GELU

**Goal:** remaining primitives for transformer inference. Lower effort each, batched.

**Prerequisites:** G1.

**Steps:**

1. NEON Softmax (exp approximation via polynomial, normalize; tricky numerics, but tractable).
2. NEON LayerNorm / RMSNorm (mean, variance, normalize).
3. NEON GELU (either tanh-based or erf-based approximation).
4. Unit-test each with tolerance epsilon.

**Exit criteria:**
- All primitives implemented with NEON path and scalar fallback.
- Tolerance tests pass (typically 1e-4 relative error).

**Effort:** 1 week.

---

### Phase G4 — INT8 / FP16 quantization

**Goal:** reduce memory footprint and increase throughput for edge inference. INT8 dot-products use NEON `sdot` instructions (ARMv8.2+; available on A78AE). FP16 uses `fmla.h` / `fmlaq_f16` intrinsics — these may require `unsafe` inline assembly rather than stable Rust intrinsics depending on the Rust toolchain version.

**Prerequisites:** G1-G3.

**Steps:**

1. Implement symmetric INT8 quantization for weights (per-tensor or per-channel).
2. INT8 MatMul using `sdot` / `udot`: 4× throughput over FP32.
3. FP16 MatMul path using `fmlaq_f16`. Validate intrinsic availability in the pinned Rust toolchain; fall back to inline asm (`"fmla %0.8h, %1.8h, %2.8h"`) if stable intrinsics are missing.
4. End-to-end validation on MNIST or a small classification model; compare FP32 vs FP16 vs INT8 accuracy.

**Exit criteria:**
- MNIST inference with INT8 weights within 2% accuracy of FP32.
- FP16 path exists with measurable throughput improvement on Jetson.

**Effort:** 2 weeks (was 1-2; allowance for FP16 intrinsic friction).

**Fallback:** if INT8 runs long, ship FP32 + FP16 only. The capstone doesn't require INT8.

---

### Phase G5 — Cross-platform benchmark harness (unified deliverable)

**Goal:** the capstone benchmark deliverable — reproducible inference numbers across Jetson, Pi 5, x86-64, QEMU.

**This phase owns the single unified benchmark doc** `docs/cross-platform-inference-bench.md`. The x86-64 plan's C3 (benchmarks) contributes its platform's rows into this file rather than producing a separate `docs/benchmarks.md`. G5 is sequenced last in the parallel calendar (Week 9-10), so it runs after both G4 (Jetson kernels) and x86-64 C2-C3 (x86-64 kernels + bench) have data ready. Pi 5's numbers drop in from whatever state the Pi 5 plan has reached at that point.

**Prerequisites:** G1-G4. Coordinate with x86-64 C3 owner — expect their SSE numbers as input.

**Note on the GPU allocator on Jetson:** `bench gpu` on Jetson exercises the **stub driver** (registered via `gpu_register_driver(&gpu_stub_driver)` in `kernel/src/main.c`), not the NVIDIA probe driver. The stub allocates via PMM and performs the cache-maintenance calls, but the "GPU address" is the CPU physical address — no GPU DMA happens. Benchmark numbers from `bench gpu` therefore reflect allocator + cache-maintenance overhead, not GPU bandwidth. G5 should either (a) clearly label these as "allocator overhead" in the doc, or (b) switch `bench gpu` to use the NVIDIA unified-memory allocator once the CBB-bypass-at-EL2 path is known safe.

**Steps:**

1. Add a `bench infer` shell command that runs the SLM on a fixed input for N iterations and reports:
   - Tokens per second (if the model is an LLM).
   - Mean / p99 inference latency.
   - Energy on Jetson — captured pre-kexec via Linux `/sys/bus/i2c/devices/.../ina3221*` readings, not from bare-metal (the INA3221 is also behind the CBB firewall). Document this clearly.
2. Add equivalent paths for Pi 5 (no INA3221) and x86-64.
3. Run on all four targets (Jetson, Pi 5, x86-64, QEMU) and collect data.
4. Produce `docs/cross-platform-inference-bench.md` with tables and analysis. Explicitly label each column with the hardware path (NEON FP32 / FP16 / INT8 / x86 AVX-fallback).

**Exit criteria:**
- Single `bench infer` command produces consistent output.
- Comparable numbers across all platforms in the doc, with clear path labels.

**Effort:** 1 week.

**Tracking:** new issue "Cross-platform inference benchmark (capstone deliverable)".

---

### Phase G6 — Thesis framing + GSP documentation

**Goal:** the narrative for the capstone report. Explain GPU-detection infrastructure as foundation work and position CPU inference as the delivered capability.

**Prerequisites:** G5.

**Steps:**

1. Draft a thesis section "GPU Support and the GSP Blocker" explaining:
   - What works: detection, unified memory, cache coherency.
   - What doesn't: compute (blocked on GSP).
   - Why: GSP firmware complexity, closed-source tooling, time budget.
   - Alternative: NEON CPU inference.
2. Update `docs/jetson-nvidia-support.md` with the final state.
3. File a dedicated "Future Work: GSP bare-metal loader" issue referencing the full research in `docs/nvidia-gsp.md`.

**Exit criteria:**
- Thesis section reviewed and approved.
- Future-work issue filed.

**Effort:** 3-4 days.

**Total Track G effort:** 6-8 weeks. Fits capstone scope.

---

## Week-by-Week Calendar (Realistic)

Assuming ~25 hrs/wk on the capstone from 2026-04-14 onward.

| Week | Primary | Secondary (parallel) |
|---|---|---|
| 1 | **Shared infrastructure PRs (half a day total):** Prereq #1 FIQ handler (Pi 5 plan), #2 rename, #3 `smp_notify_cpu`, #4 inference dispatch skeleton. Plus: **P1.0 fast-path** (1 hour); S1 (NC deque migration — 1 day); S2 (steal metrics — 0.5 day); G1 FP32 MatMul begins | — |
| 2 | P1.1 full diagnostic (if P1.0 failed) **or** P2 CPU-0 preemption (if P1.0 worked) | G1 MatMul continues |
| 3 | P2 CPU-0 preemption **or** P3 secondary preemption (including MPIDR + SGI fixes — **test MPIDR consolidation on Pi 5 QEMU before merging**) | G1 MatMul wraps |
| 4 | P3 secondary preemption continues; P4 UART lock (after Pi 5 plan's UART-ISR-path audit has landed) | G2 Conv begins |
| 5 | P5 DAIF + spinlock audit + doc (trampoline approach, no context.daif flip) | G2 Conv wraps |
| 6 | P6 regression sweep on Pi 5 + Jetson | G3 Softmax / LN / GELU |
| 7 | S3 Phase C benchmarks (unblocked by P3) | G4 FP16 + INT8 quantization begins |
| 8 | S4 flip work-stealing default **iff** x86-64 B3 data also supports it (coordinate with x86-64 plan owners) | G4 Quantization wraps |
| 9 | GSP blocker doc polish | G5 Cross-platform benchmark begins (unified doc) |
| 10 | G5 Cross-platform benchmark wraps (incorporates x86-64 C3 data) | (buffer) |
| 11 | G6 Thesis framing | Integration testing, demo prep |
| 12 | Demo rehearsal, polish | — |

**Slack:** one full week (w10) for items that overflow.

**What gets cut first if schedule slips:**

1. P5 (DAIF audit) — defer the "document invariants" part; ship with just the flip.
2. S5 (load-balancing enhancements) — stretch goal.
3. G4 INT8 quantization — ship FP16 only.
4. If P1 takes >2 weeks: **cut all of Track P**. The minimum capstone (G1-G6) still ships with cooperative SMP.

---

## Decision Gates

### Gate 1 — End of Week 2: Preemption feasibility

**Go/No-go criteria:** is P1 root cause identified?

- **Yes, simple fix:** proceed with P2-P6, full Track P.
- **Yes, but fix is >1 week:** proceed with P2 and P6; skip P3-P5 (secondary preemption deferred).
- **No, still investigating:** **cut Track P.** Ship cooperative SMP in the capstone. Track P continues as future work.

### Gate 2 — End of Week 7: Phase C data

**Go/No-go for flipping work-stealing default:**

- **Wins ≥ 10% on at least one platform:** flip default (S4).
- **Wins < 10% or mixed:** keep `CONFIG_WORK_STEALING=0` default; ship the feature as opt-in.

### Gate 3 — End of Week 9: Inference kernels complete

**Go/No-go for Track G scope:**

- **All kernels (G1-G4) working:** proceed with G5 as planned.
- **Quantization (G4) behind schedule:** ship FP16 only; INT8 becomes future work. Does not block capstone.

---

## Success Criteria — Capstone Minimum vs Stretch

### Minimum (sufficient for passing)

- Bare-metal SLM-OS boots on Jetson via kexec, reaches shell.
- 6-core cooperative SMP verified (`bench smp` 5/5).
- GPU detected, unified memory mapped, cache coherency working.
- CPU NEON inference operational across Jetson, Pi 5, x86-64.
- Cross-platform benchmark data published.
- Thesis includes honest framing of GSP blocker.

### Stretch (differentiates capstone)

- Preemptive multi-core scheduling on Jetson.
- Work-stealing default-on with measured performance wins.
- INT8 quantization with measurable accuracy preservation.
- Demo: live inference on Jetson, power consumption measured, side-by-side with Pi 5 and x86-64.

### Beyond capstone (future work)

- GSP firmware bare-metal loader (standalone 6-12 month project).
- TensorRT / CUDA runtime integration.
- EQOS Ethernet, TinyUSB serial console, secure boot.

---

## Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| P1 root cause is in TF-A / EL2 interaction, untractable on-budget | Medium | Delays all of Track P | Cut Track P at Gate 1 (Week 2). Capstone ships cooperative. |
| NEON FP16 accuracy issue for small-model inference | Low | Delays G4 | Ship FP32/FP16 only; defer INT8. |
| Jetson hardware develops a fault (unrelated) | Low | Blocks hardware testing | Fall back to Pi 5 for all hardware-in-the-loop work. Benchmarks degrade but don't stop. |
| Rust borrow-checker friction around NEON intrinsics | Medium | Delays G1 | Use the documented `#[target_feature]` pattern with scalar fallback; accept unsafe blocks where needed (runtime/CLAUDE.md has precedent). |
| Scope creep from committee feedback | Medium | Delays G5-G6 | Timebox each phase. Park non-critical comments as future-work issues. |

---

## Artifacts Produced

Each phase produces one or more of:

- **Code** in a feature branch, PR'd to main.
- **Tests** in `kernel/tests/` or `runtime/tests/`.
- **Docs** in `docs/` — specifically:
  - `docs/preemptive-multitasking.md` (P5)
  - `docs/work-stealing-bench.md` (S3)
  - `docs/cross-platform-inference-bench.md` (G5)
  - `docs/jetson-nvidia-support.md` updates (G6)
- **GH issues** — filed for each new track initiation; closed on phase completion.

---

## Revision Notes

**2026-04-13 v2** — incorporated code-level review findings that cross-referenced the initial plan against the actual `gic.c`, `vectors.S`, `preempt.c`, and `boot.S` sources. Material updates:

- **P1.0 added** — `GICR_IGROUPR0` / `GICR_IGRPMODR0` are not written by `gic_redist_init()`; PPI 30 may be delivered as FIQ and fall through `el1_fiq: b hang`. The two-line fast-path fix is now the first step and may resolve P1 entirely.
- **P3 MPIDR fix** — the `resched_trampoline` in `kernel/arch/arm64/vectors.S:299-316` uses `(Aff0 | Aff1)` which collides on Jetson cluster 1 (CPU 4→slot 2, CPU 5→slot 3). Must be replaced with a cluster-aware lookup before preemption can be enabled on CPUs 4-5.
- **P3 SGI fix** — `gic_send_sgi()` at `kernel/drivers/gic.c:697-712` assumes single-cluster affinity; Aff0 is 0 for all Jetson CPUs. Latent bug that blocks any future cross-CPU IPI.
- **P5 spinlock discipline** — explicit audit of `spin_lock(` vs `spin_lock_irqsave(` added. Under `SPINLOCK_SKIP_LOCKING` (Jetson) + preemption, plain `spin_lock` provides no mutual exclusion.
- **G1 split FP32-only** — FP16 moved to G4 (paired with quantization). Timeline lengthened to 2-3 weeks to reflect tiling complexity and edge-case handling.
- **G5 GPU-path clarification** — `bench gpu` on Jetson exercises the stub driver, not the NVIDIA unified-memory allocator. Doc must label paths clearly.

**2026-04-13 v3** — incorporated cross-plan review findings after the Pi 5 (`docs/pi5-preemption-plan.md`) and x86-64 (`docs/x86-64-capstone-closure-plan.md`) plans merged. Coordination fixes:

- **Shared Infrastructure Prerequisites section added** (between the dependency graph and Track P). Enumerates the four Week-1 PRs that eliminate guaranteed merge conflicts: FIQ handler (Pi 5 owns), `SECONDARY_PREEMPT` rename, `smp_notify_cpu()` abstraction, inference `ops.rs` dispatch skeleton.
- **P3 step 1** — references Prereq #2 rename as a Week-1 standalone PR rather than part of P3.
- **P3 step 2** — MPIDR consolidation touches shared code (`task.c`, `sched.c`, `exceptions.c`, `smp.c` NC trace sites) that also compiles for Pi 5; explicit instruction added to test on Pi 5 QEMU (and hardware if available) before merging.
- **P3 step 3** — once `gic_send_sgi()` dual-cluster fix lands, upgrade the ARM64 `smp_notify_cpu` from `sev` broadcast to targeted SGI.
- **P4** — Pi 5 plan's UART-ISR-path audit is now an explicit prerequisite; it benefits all platforms and must land before P4's NC ticket lock.
- **P5** — dropped the "flip default DAIF" step. Adopt the Pi 5 trampoline approach (`daifclr` in `task_entry_trampoline`, keep `context.daif = 0x080`). Keeping two different DAIF-unmask mechanisms in shared `task.c` would diverge from Pi 5's invariant.
- **S1** — explicit instruction to test the NC deque migration on Pi 5 QEMU in addition to Jetson (both define `PLATFORM_HAS_NC_MEMORY`).
- **S4** — `CONFIG_WORK_STEALING` default flip is now a multi-platform coordination point, not a unilateral Jetson change. Requires x86-64 B3 data before firing, and x86-64 B3 itself should use a CMake override, not edit the shared header.
- **G1** — Prereq #4 (inference dispatch skeleton) is now an explicit prerequisite; G1 fills in the aarch64 arm and x86-64 C1 fills in the x86_64 arm without conflicting.
- **G5** — now the unified cross-platform benchmark deliverable. Merges with x86-64 plan's C3; single doc `docs/cross-platform-inference-bench.md`.
- **Week 1 calendar** — now explicitly starts with the half-day of shared infrastructure PRs before any platform-specific work.

---

**2026-04-13 v4** — recorded the actual outcome of P1 on Jetson hardware. Material updates:

- **P1.0 hypothesis falsified.** `GICR_IGROUPR0`/`GICR_IGRPMODR0` writes from NS EL2 are silently ignored by the GIC because the Group config is owned by EL3. Same structural blocker as Pi 5 #99. Boot-time `[JDIAG]` print captured `GICR_IGROUPR0=0x0` after the write to confirm.
- **P1.1 cooperative-preempt path adopted.** Renamed `PI5_COOP_PREEMPT` → `COOP_PREEMPT` and extended activation to Jetson. Hardware verified: `timer_handler_count` advances `0 → 18 → 36`, per-CPU `Ticks` counter non-zero on CPUs 1-5.
- **P3 secondary preemption is moot** until a future P1.2 fixes hardware timer IRQ delivery (deferred — not required for capstone).
- IGROUPR writes kept in `gic_redist_init()` as best-effort hardening; comment updated to document the Jetson observation.

---

*Plan compiled 2026-04-13, revised 2026-04-13 v2, v3, v4. To be revisited at each Gate.*
