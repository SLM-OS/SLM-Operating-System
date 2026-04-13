# Jetson Orin Nano — Capstone Gap Analysis

**Scope:** preemptive multitasking, SMP, and GPU-accelerated model inference.
**Board:** Jetson Orin Nano Super Developer Kit (dual-cluster Cortex-A78AE, Ampere GA10B).
**Audience:** capstone committee / future maintainers.
**Snapshot date:** 2026-04-13, against commit `c22431c` on main.

---

## Executive Summary

| Area | State on Jetson | Capstone risk |
|---|---|---|
| **Preemptive multitasking** | Not functional. Timer IRQs do not fire on any CPU post-kexec (hardware-verified). All scheduling is cooperative. | Medium — fixable in days if the root cause is GIC routing; unbounded if it's a TF-A/EL2 exception-vector interaction. |
| **SMP** | 6-core boot works; cross-CPU task dispatch works cooperatively; cache coherency via NC memory is solid. Secondary preemption is blocked on the same timer-IRQ issue. | Low — the functional surface is sufficient for capstone demos; "full" preemptive SMP is blocked by the preemption issue. |
| **GPU model inference** | Hardware detected (GA10B), unified-memory allocator in place, cache-sync primitives working. No compute path. Blocked on GSP firmware (a 6–12 month standalone project requiring NVIDIA-internal tooling). | **High** — GPU compute is out of reach in a capstone. Requires re-scoping to CPU (NEON) inference with GPU detection as groundwork. |

The short story: **SMP and GPU-detection infrastructure are in good shape. Preemption has a specific, diagnosable blocker. GPU compute needs re-scoping — not fixing.**

---

## 1. Preemptive Multitasking

### 1.1 What is in the code

Source: `kernel/sched/sched.c`, `kernel/drivers/timer.c`, `kernel/drivers/gic.c`, `kernel/sched/preempt.c`.

- ARM Generic Timer per-CPU init (`timer_percpu_init`) and GIC redistributor init (`gic_percpu_init`) are both called from `scheduler_start()`.
- CPU 0's idle task does `msr daifclr, #2` + `wfi` inside its loop (`sched.c:415-424`). Secondary CPUs use `wfe` only.
- The ELR-trampoline infrastructure from PR #98 (deferred scheduling from the timer ISR) is gated `#if defined(PI5_SECONDARY_PREEMPT)` at `kernel/sched/preempt.c:14` and wired into `CMakeLists.txt:72-75` as `PLATFORM=RASPI5` only.
- Tasks start with `DAIF.I=1` (IRQ masked). This is an invariant baked into many call sites — the msg_router, component runtime, and scheduler all assume tasks are not preemptible between cooperative yields.

### 1.2 What hardware says

Live `cpu` shell output on `jetson-nano-2` after kexec (captured 2026-04-13):

```
CPUs: 6 online / 6 total
Per-CPU scheduler diagnostics:
CPU  Ticks     Schedule  Picked    IdleLoops
  0         0         1       2        43690   ← 0xAAAA marker, not a count
  1..5      0         N       2        43690
timer_handler_count: 0                          ← NO timer IRQs since boot
diag_tick val[0]=0x0                            ← scheduler_tick never called
```

Observed behavior confirms `timer_handler_count == 0` across extended uptime. **Timer IRQs never reach the kernel on Jetson** — not on CPU 0, not on any secondary. The `daifclr + wfi` in CPU 0's idle is unmasking IRQs that are never pending.

### 1.3 Gap — why timer IRQs don't fire

This is the primary unresolved blocker. Candidates, in order of likelihood:

1. **`GICR_IGROUPR0` / `GICR_IGRPMODR0` never written (most likely).** Audit of `gic_redist_init()` in `kernel/drivers/gic.c:407-448` confirms it wakes the redistributor, disables all SGIs/PPIs, sets priorities, and configures trigger types — but never writes the Group registers. On GICv3 with two security states, unwritten Group bits may leave PPI 30 in Group 0 (Secure), which is delivered as **FIQ** rather than IRQ. The exception vector at `el1_fiq` in `kernel/arch/arm64/vectors.S` falls through to `b hang` — a black hole that silently drops the interrupt. This matches the observed symptom (zero IRQs, no crash). The fix is a two-line addition inside `gic_redist_init`:
    ```c
    GICR_IGROUPR0(cpu)  = 0xFFFFFFFF;   /* All SGIs/PPIs Group 1 NS */
    GICR_IGRPMODR0(cpu) = 0x00000000;
    ```
   The same diagnosis applies to Pi 5 (#99) via GICv2's `GICD_IGROUPR` — both platforms likely share this root cause.
2. **EL2+VHE interrupt routing.** Jetson runs at EL2 with VHE (E2H=1, TGE=1). Under VHE, `CNTP_*_EL0` accesses from EL2 hit the **NS EL1 physical timer**, which fires INTID 30 (PPI 14). That matches the expected INTID. However, `CNTHCTL_EL2` has a **different bit layout under VHE** than without — `boot.S` on Jetson does not explicitly program `CNTHCTL_EL2` (unlike Pi 5's boot). This is not a blocker for EL2 code itself, but becomes relevant if future user-mode work runs at EL0.
3. **TF-A residual GIC state after kexec.** Linux's kexec leaves the GIC distributor in whatever state Linux configured. SLM-OS re-initializes the distributor, but Linux's CPU-hotplug-offline path clears redistributor state for secondaries — so the code's implicit "inherit Linux's config" assumption is fragile.

The documented secondary-CPU WFE-only workaround (`kernel/CLAUDE.md:171-172`) describes a different failure mode on Pi 5 — "secondary CPUs cause scheduler re-entrancy issues (schedule() called from timer ISR does switch_to, abandoning the exception frame)." That's IRQs firing but crashing, whereas on Jetson the IRQs do not fire at all. The scheduler-reentrance issue is downstream; the Group configuration issue is upstream and applies to both platforms.

### 1.4 What closes the gap

**Fast path (try first — potentially 2-line fix, 1 hour):**

Add `GICR_IGROUPR0(cpu) = 0xFFFFFFFF` and `GICR_IGRPMODR0(cpu) = 0` to `gic_redist_init` in `kernel/drivers/gic.c` and rebuild. If timer IRQs start firing on CPU 0, the diagnosis is confirmed and the remaining work is just downstream (secondary preemption, UART lock). This is a zero-risk attempt — existing behavior (IRQs never firing) cannot get worse.

**Full diagnostic (if the fast path does not fix it):**

1. **Add a `gicdump` shell command** that prints per-CPU registers: `GICR_CTLR`, `GICR_WAKER`, `GICR_IGROUPR0`, `GICR_IGRPMODR0`, `GICR_ISENABLER0`, `GICR_IPRIORITYR[7]` (PPI 30's byte), `ICC_CTLR_EL1`, `ICC_PMR_EL1`, `ICC_IGRPEN1_EL1`, `ICC_SRE_EL1`, `CNTFRQ_EL0`, `CNTP_CTL_EL0`, `CNTP_CVAL_EL0`, `CNTHCTL_EL2`. 0.5 day.
2. **Spuriously raise the timer via short `CNTP_TVAL_EL0 = 0`.** If that fires, timer path works; the bug is in periodic reprogramming. If not, IRQ routing is broken. 0.5 day.
3. **Diff the dump against a known-good EL2/VHE reference** (Jetson Linux pre-kexec via `/proc/interrupts` + a kernel module, or OP-TEE's GIC init). 1 day.
4. **Once CPU 0 IRQs fire:** verify `scheduler_tick` increments `pit_ticks`, idle unmasks correctly across preemptions, `DAIF.I=1` invariant holds on task resumption. 1 day.
5. **Enable secondary-CPU preemption** by porting the PR #98 trampoline. Code-level issues to address during the port:
    - Rename `PI5_SECONDARY_PREEMPT` to `SECONDARY_PREEMPT`; drop the `PLATFORM=RASPI5` gate in `CMakeLists.txt`.
    - **Fix the MPIDR-to-logical-cpu formula in `resched_trampoline`** (`kernel/arch/arm64/vectors.S:299-316`). The current inline `(mpidr & 0xFF) | ((mpidr >> 8) & 0xFF)` assumes Aff0 or Aff1 alone identifies the CPU — correct for Pi 5 and QEMU, **wrong for Jetson cluster-1 CPUs**. For CPU 4 (MPIDR `0x10200`) and CPU 5 (MPIDR `0x10300`) it yields 2 and 3, collisions with the cluster-0 slots. The vectors.S comment at line 309-312 anticipates this. Replace with a lookup in `cpu_logical_map[]` (already in NC memory) or a macro that incorporates Aff2.
    - **Fix `gic_send_sgi` for dual-cluster MPIDR** (`kernel/drivers/gic.c:697-712`). The current `(1UL << target_cpu)` target-list bit is wrong for Jetson — Aff0 is 0 for all CPUs, so the bit index must derive from the CPU's Aff0 offset within its Aff1 group (always 0 on Jetson), and the SGI value must set Aff1 (and Aff2 for cluster 1) from the target's MPIDR. Not blocking for timer PPIs (which are per-CPU), but required before any cross-CPU IPI-based scheduler notification. 2 days.
6. **NC-memory cross-CPU UART lock** — the existing UART lock is IRQ-disable-only (safe under cooperative only). Secondary CPUs must not print until this lands. 1 day.

**Best-case total (fast path works): 1 hour + 3 days for trampoline port.**
**Realistic (full diagnostic): 2-3 weeks with hardware-in-the-loop debugging.**

### 1.5 Open tracking

- No Jetson-specific preemption issue exists today. Filing one as part of this report's follow-up is recommended.
- #57 (merged as #98) addressed the Pi 5 scheduler-reentrance side of this problem. Its trampoline code is reusable but gated off for Jetson.
- #99 tracks the Pi 5 timer-IRQ delivery blocker — same class of issue, different hardware. Findings from that investigation may transfer.

---

## 2. SMP (Multi-Core Boot & Dispatch)

### 2.1 What works today

- **6-core boot via PSCI `CPU_ON`** from the Linux kexec entry at EL2+VHE. MPIDR dual-cluster table in `kernel/sched/smp.c:88-102` (`0x000, 0x100, 0x200, 0x300, 0x10200, 0x10300`). All cores online; verified by `cpu` shell output every run.
- **NC memory at `0xBDE00000`** (2 MB, Inner Shareable, MAIR index 2) holds `cpu_runqueue[MAX_CPUS]`, the `task_table[]`, and — after Session B (SCHED-C2) — `current_task[MAX_CPUS]`. Cross-CPU writes are instantly visible without cache maintenance.
- **`bench smp`** dispatches a task to each of CPUs 1-5; verified 5/5 COMPLETED on every run since PR #95 closed #76.
- **Session B critical-race fixes** (commit `fae1cf9`) resolved three pre-existing SMP hazards: secondary scheduler-init timeout (SCHED-C1), cross-CPU `current_task` visibility (SCHED-C2), atomic task-terminate-and-dequeue (SCHED-C3).
- **Work-stealing scheduler infrastructure** landed behind `CONFIG_WORK_STEALING` (off by default) — PR #97 (data structure) + PR #107 (integration).

### 2.2 Known limitations

1. **No preemption on secondaries** — same root cause as §1; once preemption works on CPU 0, secondaries need the trampoline + UART cross-CPU lock + trampoline MPIDR fix (see §1.4 step 5).
2. **`SPINLOCK_SKIP_LOCKING` active** — LSE atomics and `LDAXR/STXR` are unreliable post-kexec on Cortex-A78AE. All kernel-side spinlocks degrade to `dmb ish` barrier-only. Safe under cooperative scheduling because (a) per-CPU rq locks are uncontended and (b) UART is CPU-0-only. **Not safe under preemption** — if CPU 0 takes a timer IRQ mid-`schedule()` and the handler takes the same `rq_lock`, both paths proceed simultaneously with no mutual exclusion. Today the `rq_lock_irqsave` wrapper masks IRQs and the PR #98 trampoline defers `schedule()` to task context, so this race does not trigger. Every kernel spinlock in a preemption-reachable path must use `spin_lock_irqsave`, never plain `spin_lock`. Rust-side `AtomicBool::compare_exchange_weak` in `msg_router` was explicitly verified to work on Jetson (PR #96).
3. **SGI dual-cluster routing bug** — `gic_send_sgi()` at `kernel/drivers/gic.c:697-712` hard-codes `(1UL << target_cpu)` as the target-list bit, assuming single-cluster MPIDR. On Jetson, all CPUs have `Aff0 == 0`; the target bit must always be bit 0, and `Aff1`/`Aff2` must be populated from the target's MPIDR. Latent bug — not triggered today because no cross-CPU IPI paths exist — but blocks any future IPI-based scheduler notification (work-stealing wake-ups, cross-CPU flush).
4. **Work-stealing deque in cacheable BSS** — the per-CPU `cpu_steal_deques[]` array (PR #107) is in cacheable memory, not NC. On Jetson's incoherent L2, a thief CPU may briefly see stale deque state. Harmless on QEMU; correctness on Jetson is not yet verified (feature gated off). Moving to NC memory is tracked as a follow-up in the PR body.
5. **Steal observability** — no success/failure counters today. Tracked in #105.

### 2.3 What closes the gap

Three staged end-states, each with a concrete gate:

**Stage 1 — Today (cooperative 6-core SMP, verified working).**
Sufficient for capstone demos that tolerate cooperative scheduling: task dispatch, cross-core work, IPC, shell multi-session.

**Stage 2 — Preemptive all-CPU SMP.**
Gate: §1 preemption work. Once that lands, secondary-CPU preemption follows with the PR #98 trampoline, a NC-memory UART lock, and scheduler-test updates. **Effort: 1 week on top of preemption work.**

**Stage 3 — Load-balanced SMP with work stealing default-on.**
Gates: (a) Stage 2, (b) move `cpu_steal_deques[]` to NC memory, (c) Phase C benchmarks showing real wins. **Effort: 2 weeks, mostly benchmarking and tuning, not implementation.**

### 2.4 Open tracking

- #57 — secondary-CPU preemption (Pi 5 infrastructure merged as #98; Jetson port is a follow-up).
- #59 — work-stealing (Phase A #97 merged, Phase B #107 merged, Phase C pending).
- #94 — SMP busy-wait replace with timer (SCHED-L2 deferred, needs pre-scheduler timer).
- #100, #101, #105 — Phase A/B code-review follow-ups.

---

## 3. GPU-Accelerated Model Inference

This is the highest-risk area. The honest assessment is that **GPU compute on Jetson is not achievable in the remainder of the capstone** and the scope needs to pivot.

### 3.1 What works today

| Component | File | Status |
|---|---|---|
| GPU abstraction layer | `kernel/gpu/gpu.h`, `gpu.c` | Complete API (init/alloc/free/sync/info) |
| Stub driver | `kernel/gpu/gpu_stub.c` | Registered on Jetson (`main.c`); reports `GPU_CAP_NONE` |
| NVIDIA probe driver | `kernel/gpu/gpu_nvidia.c` | Compiled, not registered on Jetson. Probe refuses MMIO read at `gpu_nvidia.c:50-59` with a `CBB firewall` comment. |
| Unified-memory allocator | `gpu_nvidia.c:189-228` | Returns CPU+GPU address; cache maintenance via `cache_clean_range` / `cache_invalidate_range`. |
| Rust GPU compute backend | `runtime/src/inference/gpu.rs` | `select_backend()` routes large MatMul/Conv to GPU iff `has_compute()`; falls through to CPU otherwise. `gpu_execute_matmul` returns `Err(NotReady)`. |
| `bench gpu` shell command | `shell_sys.c:861` | Measures alloc / sync / free overhead via the stub driver on Jetson (no compute, no NVIDIA probe path — the stub is what's actually registered). Benchmark numbers reflect PMM allocation + `cache_clean_range` / `cache_invalidate_range` overhead, not GPU DMA. |

GPU identification works at EL2 — `BOOT_0` reads `0xB7B000A1` (GA10B, Ampere). Engine registers return `0xBADF5040` (NVIDIA's "fault" sentinel) because the GSP is not initialized.

### 3.2 The GSP blocker

`docs/nvidia-gsp.md` captures the research. In short:

- On Ampere and later, the GPU's compute/display/3D engines are gated behind the GSP (a RISC-V microcontroller on the GPU die). Without GSP initialization, engine registers are inert.
- GSP initialization requires: decompressing a 38 MB firmware bundle (Zstandard), parsing the VBIOS to extract signing chain, programming the SEC2 Falcon (HS-mode, cryptographically signed ucode), setting up a Write-Protected Region (WPR), resetting the GSP RISC-V core, and establishing an RPC message queue.
- The open-source reference (Linux `nouveau`) spends 3000+ LoC across 6 files just to boot GSP. None of that is ported to SLM-OS; most of it depends on NVIDIA-internal tooling (Falcon ucode signing, VBIOS extraction) that is not redistributable in bare-metal form.
- Even if GSP booted, compute workloads need either (a) CUDA runtime (closed-source, tied to L4T userspace) or (b) TensorRT (closed-source). Neither is viable on bare metal.

**Realistic effort to reach GPU MatMul on Jetson: 6-12 months of standalone work, contingent on NVIDIA engagement.** This is larger than the remaining capstone scope.

### 3.3 Recommended pivot — CPU (NEON) inference

SLM-OS already has the scaffolding for CPU inference:

- `runtime/src/inference/` has `tensor.rs`, `ops.rs`, `engine.rs`, `workspace.rs` — the CPU tensor kernel layer.
- NEON is mandated by ARMv8-A; the Rust `#[target_feature(enable = "neon")]` pattern is already used in the runtime (runtime/CLAUDE.md §SIMD).
- The Jetson has 6 Cortex-A78AE cores — under cooperative SMP this already runs parallel inference workers.

Proposed capstone scope for GPU-related work (honest framing):

1. **GPU hardware identification + unified-memory plumbing** (already done). Frame as "GPU detection and cache-coherency infrastructure."
2. **CPU NEON kernels**: MatMul, Conv, Softmax, LayerNorm. Validated on Pi 5, Jetson, x86-64. **3-4 weeks.**
3. **INT8/FP16 quantization**: reduce memory and multiply cost for small LLMs. **1-2 weeks.**
4. **Cross-platform inference benchmark**: same model across all three platforms, report tokens/s, latency, energy (Jetson has hw power monitoring). **1 week.**
5. **Document the GSP blocker honestly in the thesis.** GPU hardware is detected and addressable; the closed-source firmware stack precludes bare-metal compute without NVIDIA engagement. This is a defensible scope limitation, not a project failure.

**Total pivot effort: 6-7 weeks.** Fits capstone scope.

### 3.4 Open tracking

- #27 — GPU memory integration (`gpu_map`, `SHM_GPU_ACCESSIBLE`). The unified-memory allocator partially addresses this.
- #28 — TensorRT/CUDA Integration (**blocked on GSP firmware**; acknowledge as out-of-scope for capstone).
- #31, #32 — Secure boot and encrypted model storage; orthogonal to GPU compute.

---

## 4. Prioritized Roadmap

Ordering reflects capstone impact per week of effort.

| # | Task | Effort | Blocks |
|---|---|---|---|
| 1 | Try the `GICR_IGROUPR0 = 0xFFFFFFFF` / `GICR_IGRPMODR0 = 0` fast-path fix in `gic_redist_init` | 1 hour | everything downstream if it works |
| 2 | (If #1 doesn't work) full GIC/timer diagnostic — `gicdump` command, spurious-IRQ test, reference diff | 3-5 days | all preemption |
| 3 | Port PR #98 trampoline to Jetson: rename `PI5_SECONDARY_PREEMPT`→`SECONDARY_PREEMPT`, **fix MPIDR formula in `vectors.S:313-316`** to handle cluster 1, enable for Jetson | 3 days | secondary-CPU preemption |
| 4 | Fix dual-cluster SGI routing in `gic_send_sgi()` | 0.5 day | future IPI-based work-stealing notifications |
| 5 | NC-memory cross-CPU UART lock | 2 days | safe secondary-CPU printing |
| 6 | Audit: all kernel `spin_lock()` → `spin_lock_irqsave()` under preemption | 1 day | safe preemption |
| 7 | NEON MatMul FP32 kernel | 1-2 weeks | downstream inference |
| 8 | NEON Conv kernel | 1-2 weeks | inference |
| 9 | NEON Softmax / LayerNorm / GELU | 1 week | transformer inference |
| 10 | FP16 + INT8 quantization paths | 1-2 weeks | inference size/speed on Jetson |
| 11 | Cross-platform inference benchmark | 1 week | capstone deliverable |
| 12 | Move `cpu_steal_deques[]` to NC memory | 1 day | work-stealing on Jetson |
| 13 | Phase C work-stealing benchmark | 1 week | decision to flip `CONFIG_WORK_STEALING` default |
| 14 | Steal success/failure counters (#105) | 0.5 day | Phase C data |

**Minimum viable capstone path (6 weeks):** items 4, 5, 6 alone. Ships with cooperative SMP and CPU-only inference. Honest framing of GPU-detection infrastructure as foundation work.

**Stretch capstone path (10 weeks):** items 1-6. Ships with preemptive SMP on Jetson, CPU inference, and the GSP pivot documented.

**Full scope (15+ weeks):** all nine. Not recommended — item 1 has an unknown tail if the root cause is in TF-A / EL2 interaction.

---

## 5. Capstone Risk Assessment

**What is safe to claim:**
- 6-core SMP bring-up on NVIDIA Ampere hardware from a Linux kexec entry at EL2+VHE. Verified on hardware.
- Cross-CPU task dispatch with NC-memory-based cache coherency. Verified.
- Platform-agnostic GPU abstraction layer with NVIDIA and stub drivers, unified-memory allocator, cache-sync primitives. Verified.
- Portable bare-metal kernel across Jetson, Pi 5, QEMU, and x86-64. Verified.
- Work-stealing scheduler infrastructure (gated off pending Phase C benchmarks).

**What needs honest framing in the thesis:**
- **Preemption is cooperative-only in the current state.** The timer-IRQ-delivery root cause on Jetson is unresolved. Framing: "all scheduling in the reported state is cooperative; preemptive scheduling is implemented in code (ELR-trampoline, PR #98) but the Jetson GIC/timer interaction requires further hardware debugging that was out of scope."
- **GPU compute is blocked by GSP firmware.** Framing: "SLM-OS detects and addresses the GA10B GPU, maps its unified memory, and provides cache-coherent buffer handoff. GPU kernel submission requires initialization of the NVIDIA GPU System Processor (GSP), a multi-month engineering effort involving closed-source firmware, cryptographic signing infrastructure, and VBIOS parsing that is beyond the capstone scope. Inference runs on the ARM CPU via NEON kernels."

**What to avoid claiming:**
- "Real-time preemptive scheduling" — not true until §1 is fixed.
- "GPU-accelerated inference" — not true until GSP lands.
- "Production-ready" anything — this is a research kernel with known limitations.

---

## 6. Follow-up Tickets to File

1. **"Jetson: timer IRQs not delivered on any CPU post-kexec"** — P1-high, `platform:jetson`, `sub:drivers`. Document the current observation and link to this file.
2. **"Jetson: port PR #98 ELR-trampoline for secondary-CPU preemption"** — blocked by #1, tracked separately.
3. **"Jetson: NC-memory cross-CPU UART spinlock"** — blocked by §1 items 1-2.
4. **"Capstone pivot: NEON MatMul + Conv kernels"** — new `sub:ai-runtime` milestone.
5. **"Capstone pivot: cross-platform inference benchmark harness"** — depends on #4.

All above are follow-ups from this analysis; GSP/TensorRT (#28) stays blocked and is explicitly out-of-scope for the capstone per §3.

---

*Analysis compiled against commit `c22431c` on main. Live hardware observations from `jetson-nano-2` session, 2026-04-13.*
