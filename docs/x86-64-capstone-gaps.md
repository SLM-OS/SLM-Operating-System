# x86-64 Capstone Gap Analysis: Preemption, SMP, and GPU Inference

**Date:** 2026-04-13
**Scope:** State-of-the-art audit of three capstone-critical subsystems on
the x86-64 port — preemptive multitasking, SMP, and GPU-accelerated model
inference. Identifies what is working, what is not, and what is needed to
close each gap.
**Target hardware:** Gigabyte H610M S2H V2 dev PC (i7-6700, 8 logical CPUs,
NVIDIA RTX 3050 / GA107 / Ampere).
**Baseline:** commit after PR #103 merge (`c22431c` + `7c79eea`).

---

## Executive Summary

| Subsystem | Headline status | Capstone readiness |
|---|---|---|
| **Preemptive multitasking** | Works end-to-end post-#91; no open scheduler bugs. Hardening gaps: no ISR stack (TSS IST), no real-hardware validation under multi-task load. | **Ready for demo**, with two HIGH follow-ups. |
| **SMP (8-core)** | Boots 8/8 CPUs. Per-CPU LAPIC timers fire. Per-CPU run queues work. **No reschedule IPI** for cross-CPU wakeup (up to 10 ms dispatch latency to an idle CPU). **Work-stealing compiled but disabled** (`CONFIG_WORK_STEALING=0`). No integration test exercises secondary-CPU preemption under load. | **Functionally works; measurement gap.** |
| **GPU inference** | RTX 3050 identified on PCI, BAR0 registers readable, BAR1 VRAM R/W verified. **No compute.** Engine registers return `0xBADF5040` — GSP firmware never loaded. Inference runs on CPU, and on x86-64 the CPU path is **pure scalar** (NEON-only SIMD; SSE blocked by #72). | **Inference demos on CPU only.** GPU compute is out of scope for capstone unless the team accepts 4–6 weeks of GSP/VBIOS/RPC work. |

Concrete next actions at the bottom of the document.

---

## Closure Log (2026-04-15)

Phases A–D of the gap-closure plan landed on `main` via PR #135.
Phase E E1–E3.4 (scaffolding) shipped via PRs #146, #159, #162, #165.
Phase E E3.4 audit + E3.4.d (Booter Load on SEC2) + E3.4.e (GSP
RISC-V startup) + E4 (RPC ring skeleton) shipped on
`worktree-x86-64-capstone-gap-work`. Status of every gap:

| Gap | Status | Shipped as |
|---|---|---|
| P1-1 TSS IST for timer vector | ✅ CLOSED | `kernel/arch/x86_64/tss.c` + `idt[48].ist=1` in `idt.c`. Tests: `test_tss_loaded`, `test_idt48_uses_ist1`, `test_tss_per_cpu_distinct`. |
| P1-2 Real-hardware validation | ⏳ ENABLED | Code paths hardened; `make x86-disk-verify` is CI-grade; labctl-based hw run is the remaining step (out of this PR's scope). |
| P1-3 Idle `hlt` masks AP timers | ✅ CLOSED | Subsumed by P2-1 (reschedule IPI wakes halted APs). |
| P1-4 `sleep_ms` BSP-only ticks | ✅ CLOSED | `timer_x86.c:sleep_ms` now uses `timer_get_count()` (TSC). Tests: `test_sleep_ms_on_bsp`, `test_sleep_ms_on_ap`. |
| P1-5 Dead `lapic_timer_mask` | ✅ CLOSED | Removed from `lapic.c`; `docs/x86-64-scheduler-investigation.md` Phase 8 tagged reverted. |
| P1-6 No FXSAVE in `switch_to` | ✅ CLOSED | `cpu_context.fxsave[512]` + `fxsave/fxrstor` in `context.S`. Tests: `test_context_has_fxsave`, `test_fxsave_preserves_xmm_across_preemption`. |
| P2-1 Reschedule IPI | ✅ CLOSED | Vector 49, `smp_notify_cpu()` API (Pre-1 + B1). Tests: `test_resched_ipi_delivers`, `test_resched_ipi_self_is_noop`. |
| P2-2 AP preempt integration test | ✅ CLOSED | `test_all_cpus_timer_preempt_under_load`. |
| P2-3 `CONFIG_WORK_STEALING` on x86-64 | ✅ CLOSED | `CMakeLists.txt` enables on `X86_64`; `sched.c:sched_try_steal` has `preempt_disabled` early-out. Test: `test_work_stealing_enabled`. |
| P2-4 Periodic rebalance | ✅ CLOSED | `sched_rebalance_tick()` in `sched.c` wired via `sched_heuristic.c` `.tick`. Tests: `test_rebalance_respects_affinity`, `test_rebalance_symbol_exposed`. |
| P2-5 Coupled with P2-1 | ✅ CLOSED | Same fix. |
| P3-1 x86-64 CPU SSE | ✅ CLOSED | `kernel/arch/x86_64/sse_kernels.c` (compiled `-msse -msse2`) + extern-C FFI from `ops.rs`. Bit-exact tests `test_sse_{relu,zero,add_scalar,fma_row}_matches_scalar`. |
| P3-2 / P3-3 / P3-4 GSP firmware | 🚧 IN PROGRESS | E1, E2, E2.5, E3.1, E3.2, E3.3, E3.4 (scaffolding + audit fixes), E3.4.d (Booter Load), E3.4.e (RISC-V startup), and E4 RPC skeleton shipped. Outstanding: hardware re-test of FWSEC after the BOOTVEC=0 + CPUCTL_ALIAS fixes; full WprMeta layout; RPC marshalling + GSP_INIT_DONE wait. Tracked in #27. |
| P3-5 Capability detection | ✅ CLOSED | `GpuCapabilities::detect()` already returns `DetectedNoCompute` gracefully. |

Test count grew from 94 → ~120 in `kernel/tests/test_x86_boot.c`.
x86-64 QEMU baseline: 8 pre-existing failures (PMM/VMM/PCI/GIC
cascade from a pre-existing "Model memory init failed" at boot —
unrelated to this work). ARM64 `make test` still PASSES cleanly.
`make x86-disk-verify` all 9 checks PASS. End-to-end OVMF boot to
`slmos>` shell with 4 CPUs online works.

**Host-side test suites (Phase E, no GPU required):** 105 total.

| Suite | Cases | Coverage |
|---|---|---|
| `make test-vbios` | 30 | VBIOS BIT parser, PCIR walker, FWSEC discovery |
| `make test-falcon` | 30 | Falcon v4 register protocol — probe, reset/scrub, halt poll, DMA framing, **PIO IMEM/DMEM upload (E3.4.d) with specific GSP_ERR_INVAL on alignment/bounds**, **CPUCTL.ALIAS_EN routing**, **pre-PIO setup**, HS-boot BROM sequence |
| `make test-nvfw` | 14 | nvfw_bin_hdr / hs_header_v2 / hs_load_header_v2 framing |
| `make test-bringup` | 20 | Sig-index algorithm, DMEMMAPPER patcher, **booter_load + riscv_start state-machine guards (specific GSP_ERR_INVAL)**, **error-constants distinct + negative** |
| `make test-rpc` | 17 | RPC ring math, shm region init/dtor, **send-rejected-when-not-alive (GSP_ERR_NOSYS), oversize-rejected (GSP_ERR_INVAL), null-arg-rejected (GSP_ERR_INVAL)** |

**Error codes** (`kernel/gpu/nvidia/gsp.h`): the new shared core
publishes a small set of negative constants — `GSP_ERR_INVAL`,
`GSP_ERR_IO`, `GSP_ERR_NOMEM`, `GSP_ERR_FAULT`, `GSP_ERR_NOSPC`,
`GSP_ERR_NOSYS`, `GSP_ERR_TIMEOUT` — used in place of generic `-1`
returns. Values match common Linux errno mapping so `if (rc < 0)`
callers keep working unchanged. The harness surfaces the exact
returned code via `(rc=%d)` in failure dumps so phase + code
together pin down the failure mode.

---

## 1. Preemptive Multitasking on x86-64

### 1.1 What works today

The full preemption chain is wired and exercised:

| Stage | File | Notes |
|---|---|---|
| LAPIC timer calibration & periodic start | `kernel/arch/x86_64/timer_x86.c:65-143` | PIT-calibrated, 100 Hz, vector 48 |
| Per-CPU timer start for APs | `kernel/arch/x86_64/timer_x86.c:139` | `timer_percpu_init()` called from AP entry |
| ISR stub + IDT interrupt gate | `kernel/arch/x86_64/idt.S:152`, `idt.c:327` | IF auto-cleared on entry |
| Handler + EOI fence | `kernel/arch/x86_64/lapic.c:123-135` | `lock; addl $0, (%rsp)` fence after EOI store |
| `scheduler_tick()` gated by `preempt_disabled[cpu]` | `kernel/sched/sched.c:1041` | Prevents reentrant `schedule()` mid-switch |
| New-task trampoline clears `preempt_disabled[cpu]` | `kernel/sched/task.c:160-165` | #91 fix; new tasks are preemptible on first timeslice |

**Key semantic difference from ARM64:** On x86-64, tasks run with `RFLAGS.IF=1`
(timer IRQs fire *during* task execution). On ARM64 Pi 5, tasks run with
`DAIF.I=1` and rely on the idle task's `daifclr+wfi` to advance `pit_ticks`.
x86-64 preemption is therefore **genuinely preemptive**, not cooperative.

Regression tests in `kernel/tests/test_x86_boot.c` cover the preemption
machinery:

- `test_preempt_disabled_cleared_in_task` — asserts the flag is 0 inside a
  running task.
- `test_new_task_runs_and_yields` — single-task `yield()` × 4.
- `test_two_tasks_yield_both_advance` — two tasks bouncing yields; the
  multi-task reproducer from `docs/x86-64-scheduler-investigation.md`.
- `test_new_task_preemptible_on_first_timeslice` — directly proves
  `task_entry_trampoline` ran.
- `test_lapic_timer_running` — `pit_ticks` advances.

All four pass under QEMU; the x86-64 test-suite's 8 pre-existing failures
(`test_vmm_detected_ram`, `test_pic_timer_unmasked`, `test_platform_defines`,
`test_gic_enable_disable_timer`, `test_timer_get_frequency`,
`test_nvidia_gpu_shell_command_registered`, `test_pci_shell_command_registered`,
`test_pci_multifunction_device`) are all cascades of a PMM-allocation
failure at boot and are **unrelated to scheduling**.

### 1.2 Gaps

| # | Gap | Impact | Priority |
|---|---|---|---|
| P1-1 | **No TSS IST entry for timer vector.** Timer ISR runs on the current task's kernel stack. A task-stack overflow corrupts the ISR frame. Linux uses IST for NMI/DF/timer; SLM-OS has none. | Hardens against task-stack bugs. Not blocking today. | HIGH |
| P1-2 | **Real-hardware validation missing.** All multi-task preemption testing is QEMU-only. i7-6700 has never run two timer-preempted tasks simultaneously end-to-end. Timing races at 3+ GHz vs QEMU's 100 MHz virtual timebase can expose bugs that QEMU hides. | Risk of latent race. | HIGH |
| P1-3 | **Idle task's `hlt` masks secondary-CPU timers until next wake.** `sched.c:427-428` — idle does `sti; hlt`; halted APs only wake on timer tick or IPI. Without a reschedule IPI (see SMP §2.2) newly-dispatched tasks wait up to 10 ms (one timer period). | See SMP §2.2 — same root cause. | MEDIUM |
| P1-4 | **`sleep_ms()` polls BSP-only `pit_ticks`.** `timer_x86.c:145-152` — `sleep_ms()` loops on `pit_ticks`, which is only incremented by BSP's timer ISR (`timer_x86.c:60-62`). A task pinned to an AP therefore relies on cross-CPU cache-coherent propagation of BSP's `pit_ticks` increment — it works, but with 10 ms granularity skew and cache-line latency. **Fix is trivial: route `sleep_ms()` through the already-calibrated TSC via `timer_get_count()` / `timer_get_frequency()`.** | Latent timing bug, affects any future AP-pinned inference task and any bench harness that runs on an AP. | **MEDIUM** |
| P1-5 | **`lapic_timer_mask()` / `unmask()` are unused.** `lapic.c:178-186`. Kept in the tree from a failed #91 approach. | Dead code; remove to reduce confusion. | LOW |
| P1-6 | **Context switch does not save XMM state.** `kernel/arch/x86_64/context.S:34-82` — `switch_to` saves `rbx, rbp, r12-r15, rsp, rip, rflags` only. No FXSAVE/FXRSTOR. If two tasks both use SSE and preempt each other, XMM registers are silently corrupted. Safe *today* because the kernel is built with `-mno-sse` and only one task uses SSE at a time (inference runs single-threaded). Becomes a real bug the moment a second task uses SSE concurrently — which is the scenario unlocked by Phase C (SSE inference kernels). | Latent corruption once multiple tasks use SSE simultaneously. | LOW now / **MEDIUM once Phase C lands**. |

### 1.3 Work to close the preemption gaps

1. **Wire a TSS with an IST entry for the LAPIC timer vector (P1-1).**
   Allocate a 4 KiB per-CPU IST stack in `platform_x86.c`; extend the
   `gdt64` layout with a 16-byte TSS descriptor; set `idt[48].ist = 1`.
   Add a regression test that deliberately smashes a task stack and
   confirms the ISR still runs. **Est: 2–3 days.**

2. **Run `boot_test --count 20` + the multi-task reproducer on test-pc
   via labctl (P1-2).** Requires the `make x86-disk` target from PR
   #103 (already merged). Capture serial traces; look for lost timer
   ticks or stuck `preempt_disabled`. **Est: 1 day.**

3. **Delete the unused `lapic_timer_mask()` / `unmask()` (P1-5).**
   **Est: 15 minutes.**

---

## 2. SMP on x86-64

### 2.1 What works today

AP bringup is the most complete subsystem of the three. Verified sequence:

| Stage | File | Notes |
|---|---|---|
| Real-mode → long-mode AP trampoline | `kernel/arch/x86_64/ap_trampoline.S` | 164-line hand-written sequence; sets CR4.PAE, EFER.LME, CR0.PG |
| ACPI MADT parse for LAPIC IDs | `kernel/arch/x86_64/acpi.c:182-235` | RSDP via Multiboot2 tag or BIOS scan |
| INIT-SIPI-SIPI delivery | `kernel/arch/x86_64/platform_x86.c:212-236` | 1 ms INIT delay, 1 ms SIPI delay, retry on timeout |
| Per-CPU stacks (16 KiB each, order=2) | `platform_x86.c:170` | PMM allocation; stored in `cpu_data[i].stack_top` |
| Per-CPU LAPIC + timer init | `platform_x86.c:127-156` | `lapic_percpu_init()` + `timer_percpu_init()` |
| `scheduler_init_secondary()` on each AP | `sched.c` | NC-memory path (compile-time disabled for x86-64, uses cacheable `sched.cpu_fallback[]` instead) |
| Per-CPU run queues via `cpu_rq(cpu)` | `sched.c:289` | Cacheable on x86-64; hardware cache-coherent |
| Cross-CPU task assignment by policy | `sched_heuristic.c:139-156` | `assign_cpu()` picks least-loaded non-isolated core; respects `task->cpu_affinity` |

Tests that pass: `test_smp_cpu_count`, `test_smp_all_cpus_online`,
`test_smp_bsp_cpu_id`, `test_smp_unique_apic_ids`, `test_smp_ap_stacks_allocated`,
`test_smp_lapic_id_matches_bsp`, `test_smp_cpu_logical_id_found`/`_not_found`,
`test_smp_logical_map_consistent`, `test_smp_trampoline_param_offsets`,
`test_spinlock_mutual_exclusion`.

The "8/8 CPUs online (4 cores × 2 hyperthreads)" claim in `docs/x86-64-port.md`
is accurate and backed by these tests.

### 2.2 Gaps

| # | Gap | Impact | Priority |
|---|---|---|---|
| P2-1 | **No reschedule IPI.** `scheduler_add_task_to_cpu()` at `sched.c:819` places a task on another CPU's run queue, but it never IPIs the target. On ARM64 this uses `SEV` to wake a `WFE` CPU; on x86-64 there is no equivalent. An idle AP sitting in `hlt` (sched.c:428) only notices the new task when its own LAPIC timer next fires — up to 10 ms dispatch latency. | Limits SMP scalability; hurts inference-queue latency if batches are dispatched to idle cores. | HIGH |
| P2-2 | **No integration test for secondary-CPU timer preemption.** Every SMP test checks *boot state*; none checks that an AP actually preempts a running task. If `timer_percpu_init` on AP 3 silently fails, every existing test still passes. | Unknown failures in production SMP paths. | HIGH |
| P2-3 | **Work-stealing compiled but disabled.** `kernel/include/config.h:39-40` sets `CONFIG_WORK_STEALING=0`. `steal_deque.c` + integration in `sched.c:852-1144` exists (landed in PR #97). An idle AP does not pull from a busy AP's queue; it just idles. | No auto load-balancing; uneven AP utilization. | MEDIUM |
| P2-4 | **No task migration under the heuristic policy.** `sched_heuristic.c` picks a CPU at `scheduler_add_task()` time and never migrates. Long-running tasks stay pinned to their initial assignment even if the initial assignment becomes busy. | Uneven utilization; starvation on CPU imbalance. | MEDIUM |
| P2-5 | **APs do not run their own idle `hlt` efficiently.** Confirmed they do (`sched.c:427-428` is platform-shared), but without a reschedule IPI (P2-1) they sleep longer than needed. | Coupled with P2-1. | See P2-1 |

### 2.3 Work to close the SMP gaps

1. **Add a reschedule IPI (P2-1).** Pick a free LAPIC vector (e.g. 0xF1),
   register an IDT entry with a minimal ISR (EOI + `scheduler_tick()`),
   and have `scheduler_add_task_to_cpu()` call `lapic_send_ipi(target,
   RESCHED_VECTOR, 0)` when `target != cpu_id()` and `cpu_rq(target)`
   was previously empty. Add a test that asserts a task dispatched from
   BSP to an idle AP runs within ≤1 ms. **Est: 2–3 days.**

2. **Integration test: secondary-CPU timer preemption (P2-2).** Spawn 8
   worker tasks with `cpu_affinity` set to CPUs 0..7, each running a
   tight loop that increments a per-CPU counter. Parent sleeps 100 ms
   then asserts all 8 counters advanced at ≈100 Hz × 100 ms = 10 ticks
   each (within tolerance). **Est: 1 day.**

3. **Enable and validate `CONFIG_WORK_STEALING` (P2-3).** Flip the
   default in `config.h`; run `bench smp` on test-pc; compare tail
   latency to the pinned baseline. Add a benchmark regression test
   asserting balanced CPU utilization when tasks are created in a
   burst on CPU 0. **Est: 2 days.**

4. **Periodic policy rebalance (P2-4).** On every Nth scheduler tick,
   if load stdev exceeds a threshold, migrate one task from the
   busiest CPU to the idlest. Requires P2-1 (IPI) for low-latency
   migration. **Est: 3–5 days.**

---

## 3. GPU Inference on x86-64

### 3.1 What works today

On the RTX 3050 / GA107:

| Stage | File | Result |
|---|---|---|
| PCIe ECAM enumeration | `kernel/arch/x86_64/pci.c` | 21 devices on hardware, RTX 3050 at 01:00.0, device ID 0x2584 |
| BAR0 mapping (MMIO registers, 16 MiB) | `kernel/arch/x86_64/nvidia_gpu.c` | 0x53000000 — registers readable |
| `NV_PMC_BOOT_0` / `BOOT_42` decode | `nvidia_gpu.c` | Reports Ampere, chip 0x177, rev 10.1 |
| BAR1 mapping (VRAM aperture, 256 MiB) | `nvidia_gpu.c` | 0x40000000 — R/W verified at 5 offsets with pattern tests |
| `gpu` / `pci` shell commands | `nvidia_gpu.c`, `pci.c` | Functional |

**What is not there:** no DMA, no command submission, no engine init, no
GSP firmware upload. The engine registers return `0xBADF5040` — the
well-known "GPU fault / engine not initialised" poison pattern on
Ampere. Without GSP-RM running, no compute can execute.

### 3.2 Inference path on x86-64 today

`shell bench infer` / `component run` → `rust_infer_classify` (`runtime/src/lib.rs:1777`)
→ `Engine::run()` → `execute_node()` (`runtime/src/inference/engine.rs:300`).
The dispatch logic at `engine.rs:307` routes to GPU *if capable*:

```rust
let backend = select_backend(OpType::MatMul, ..., &self.gpu_caps);
if backend == Gpu { if exec_matmul_gpu(&node).is_ok() { return Ok(()); } }
self.exec_matmul(&node)  // CPU fallback
```

`exec_matmul_gpu` is a stub (`runtime/src/inference/gpu.rs:139-147`) that
always returns `GpuError::NotReady`. So every inference run falls through
to CPU.

The CPU path on x86-64 is **pure scalar** — `matmul_inner` at
`runtime/src/inference/ops.rs:627` has only the NEON code path on
aarch64 and a scalar fallback. SSE is blocked upstream by GitHub issue
**#72** (stable Rust on `x86_64-unknown-none` cannot legalize SSE
intrinsics against the target spec's `+soft-float` ABI; see issue
comment for the full investigation).

### 3.3 Gaps

| # | Gap | Impact | Priority |
|---|---|---|---|
| P3-1 | **x86-64 CPU inference is scalar only.** SSE/AVX blocked by #72 (Rust toolchain). ARM64 Pi 5 runs 4-wide NEON matmul at 1.09 ms for MNIST; x86-64 on the same model is ~2–3× slower, pure scalar. | Capstone demo: inference "works but is slow" on x86-64. | HIGH |
| P3-2 | **No GSP firmware loader.** GSP-RM on Ampere is a RISC-V microcontroller that must be booted with a signed proprietary firmware blob (`gsp-535.113.01.bin.zst`, ~38 MB on Linux `/lib/firmware/`). Without it, every GPU engine (graphics, compute, NVDEC, etc.) is locked out. This is the blocker Jetson issue #28 already documents. | Zero GPU compute; x86-64 demo cannot run inference on VRAM. | BLOCKED (firmware access) |
| P3-3 | **No VBIOS parser.** GSP boot requires reading FWSEC from a VBIOS BIT table type 0x85 and parsing register 0x625F04 for the VGA workspace size. The VBIOS is stored in the GPU's SPI flash and mapped into PCI ROM. None of this is implemented. | Prerequisite for P3-2. | BLOCKED by P3-2 |
| P3-4 | **No RPC / message-queue stack for GSP-RM.** Once GSP-RM is running, host communication is via a command-ring + status-ring RPC protocol in FB-mapped pages. Not implemented. | Prerequisite for P3-2. | BLOCKED by P3-2 |
| P3-5 | **GPU capability detection falls through to `NotAvailable`.** `GpuCapabilities::detect()` at `gpu.rs:56` correctly reports the GPU as identified but without compute, and the engine correctly falls back to CPU. This is fine as-is, but tests don't cover the "GPU identified, no compute" branch explicitly. | Minor test coverage gap. | LOW |

GitHub issues covering this area:

- **#28** (Jetson TensorRT/CUDA): blocked on GSP firmware. Not
  x86-64-specific, but the same blocker applies.
- **#27** (Jetson GPU memory integration, `gpu_map` / `SHM_GPU_ACCESSIBLE`):
  abstractions are in place (`runtime/src/inference/gpu.rs`) but
  unusable without a GPU backend. The x86-64 stub currently no-ops.
- **#72** (x86-64 SSE): blocks P3-1. Detailed investigation in the
  issue comments; requires nightly Rust + custom target spec
  + `-Z build-std` to land.

### 3.4 Realistic paths forward

Ranked by effort + demo value:

| Option | Scope | Effort | Capstone impact |
|---|---|---|---|
| **A. Inline-asm SSE matmul + reLU.** Bypass #72 by using `asm!` for the hot kernels (`matmul_simd`, `relu`, `simd_fma_row`) instead of `core::arch::x86_64` intrinsics. Inline asm doesn't flow through LLVM's soft-float legalizer. | ~500 LOC in `ops.rs`, verified against ARM64 outputs. | **3–5 days.** | **HIGH** — closes P3-1, gives 2–3× matmul speedup on demo PC. |
| **B. Accept CPU-only, add an x86-64-specific benchmark page.** Document the scalar baseline numbers in `docs/benchmarks.md` so the capstone story is honest about the x86-64 path. | Doc-only. | **0.5 day.** | Medium — no speedup but closes the "what actually happens on x86-64" story. |
| **C. Port nouveau's GSP-RM loader.** Nouveau (Linux drm subsystem, `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/`) has ~1500 LoC of Ampere GSP init. Port to bare-metal: VBIOS parse, Falcon boot, RISC-V bring-up, RPC ring. | 1500 LoC port + firmware blob handling + Falcon ucode. | **6–10 weeks.** | **HIGH but out of scope** — would land post-capstone. |
| **D. QEMU VirtIO-GPU.** Would let inference run on a virtual GPU under QEMU only (not on the RTX 3050 dev PC). | ~800 LoC driver + inference backend. | **1–2 weeks.** | Low — demos in QEMU, not on real hardware. |
| **E. Stub cleanup + honest capability reporting.** Keep the CPU-only path, but make `GpuCapabilities::detect()` explicitly return `DetectedNoCompute` and log it at boot. Adds a test for the branch. | ~100 LoC. | **0.5 day.** | Low. |

**Recommendation.** For the capstone: **A + B + E**. That gets the x86-64
demo running matmul on SSE (meaningful speedup), documents the scalar
baseline honestly, and makes the "no GPU compute" story explicit and
tested. Option C is the right long-term direction but not reachable in
capstone scope.

---

## 4. Prioritized Action Plan

One table, ranked across all three subsystems, for capstone demo delivery.

| Rank | Action | Est | Unblocks | Gap IDs |
|---|---|---|---|---|
| 1 | **Reschedule IPI on x86-64.** | 2–3 d | Low-latency SMP dispatch; P1-3 secondary idle lag | P2-1, P2-5, P1-3 |
| 2 | **Secondary-CPU preemption integration test.** | 1 d | Proves AP preemption actually works, not just boots | P2-2 |
| 3 | **Real-hardware multi-task boot test on test-pc.** | 1 d | Catches any QEMU-only preemption assumptions | P1-2 |
| 4 | **Inline-asm SSE matmul (Option A).** | 3–5 d | 2–3× x86-64 inference speedup; works around #72 | P3-1 |
| 5 | **Enable `CONFIG_WORK_STEALING` and benchmark.** | 2 d | Load balancing on APs | P2-3 |
| 6 | **TSS IST for timer vector.** | 2–3 d | Hardens ISR against task-stack bugs | P1-1 |
| 7 | **Honest x86-64 benchmarks in `docs/benchmarks.md`.** | 0.5 d | Capstone story coherence | P3-1 docs, P3-5 |
| 8 | **Delete unused `lapic_timer_mask()` helpers.** | 15 min | Cleanup | P1-5 |
| 9 | **Periodic policy rebalance.** | 3–5 d | Fair utilization under long-running workloads | P2-4 |
| 10 | **Port nouveau GSP-RM (post-capstone).** | 6–10 wk | True x86-64 GPU compute | P3-2, P3-3, P3-4 |

**Total effort for capstone-scope items 1–8:** ~10–14 engineer-days.
Items 9–10 are explicitly out-of-scope for capstone and should be
tracked as post-capstone work.

---

## 5. New / Refreshed Issue Tracking

The following issues should be filed to track the gaps above (if they
don't already exist under different titles — cross-check with
`gh issue list --label platform:x86-64`):

| Proposed issue | Labels | Priority |
|---|---|---|
| x86-64: reschedule IPI for cross-CPU task dispatch (P2-1) | `platform:x86-64 sub:sched sub:smp enhancement` | P1-high |
| x86-64: integration test for secondary-CPU timer preemption (P2-2) | `platform:x86-64 sub:sched sub:smp sub:testing` | P1-high |
| x86-64: inline-asm SSE SIMD for inference hot kernels (P3-1) | `platform:x86-64 sub:ai-runtime enhancement` | P1-high (capstone demo) |
| x86-64: TSS IST stack for timer vector (P1-1) | `platform:x86-64 sub:sched enhancement` | P2-medium |
| x86-64: enable + validate `CONFIG_WORK_STEALING` (P2-3) | `platform:x86-64 sub:sched enhancement` | P2-medium |
| x86-64: real-hardware multi-task boot test on test-pc (P1-2) | `platform:x86-64 sub:sched sub:testing` | P2-medium |
| x86-64: periodic scheduler rebalance policy (P2-4) | `platform:x86-64 sub:sched enhancement` | P3-low |
| x86-64: port nouveau GSP-RM for RTX 3050 compute (P3-2..4) | `platform:x86-64 sub:gpu enhancement blocked` | P3-low (post-capstone) |

#72 (SSE intrinsics) is already open and correctly labelled; it should
remain `blocked` since the inline-asm path bypasses it rather than
fixing it.
