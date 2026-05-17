# x86-64 Capstone Gap Analysis: Preemption, SMP, and GPU Inference

**Date:** 2026-04-13 (original) · 2026-04-16 (refresh — FWSEC-FRTS landed; Booter Load blocked)
**Scope:** State-of-the-art audit of three capstone-critical subsystems on
the x86-64 port — preemptive multitasking, SMP, and GPU-accelerated model
inference. Identifies what is working, what is not, and what is needed to
close each gap.
**Target hardware:** Gigabyte H610M S2H V2 dev PC (i7-6700, 8 logical CPUs,
NVIDIA RTX 3050 / GA107 / Ampere).
**Baseline:** commit after PR #103 merge (`c22431c` + `7c79eea`).

> **Live handoff for GPU work:** See
> `docs/x86-64-gpu-inference-status.md` for the current end-to-end
> GPU-inference status, the SEC2 priv-lock blocker (#185), and
> proposed next steps on Jetson / bare-metal paths. This doc is the
> top-level capstone gap map; the handoff is where the live detail
> lives.

---

## Executive Summary

| Subsystem | Headline status | Capstone readiness |
|---|---|---|
| **Preemptive multitasking** | All six P1 gaps closed (TSS IST, FXSAVE, hardware validation, `sleep_ms` TSC routing, dead-code cleanup). | ✅ **Delivered.** |
| **SMP (8-core)** | All five P2 gaps closed. Reschedule IPI (vector 49), AP preempt integration test, work-stealing enabled by default, periodic rebalance + S5 proactive load-balance override. | ✅ **Delivered.** |
| **GPU inference** | SSE inference kernels landed (P3-1 closed via inline-asm bypass of #72). **FWSEC-FRTS succeeds on hardware** (3/3 runs on test-pc). **Booter Load is hard-blocked** by SEC2 priv-lock raised by BSI after vfio-pci's mandatory FLR (issue #185). Full GPU compute remains out of reach on this platform. | 🟡 **First half of GSP bringup delivered; downstream blocked on a hardware-security boundary.** See `docs/x86-64-gpu-inference-status.md`. |

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
| P1-2 Real-hardware validation | ✅ CLOSED (2026-04-15) | `scripts/tests/x86-multitask-boot-test.sh` + `make x86-hw-validate PLATFORM=X86_64 SLMOS_LABCTL=1`. Observed: 10/10 boot_test (avg 18.5 s), 5/5 sleep-2000 in [2007, 2008] ms under `component run echo` load, 7/7 secondary CPUs dispatched by `bench smp`. Results: `docs/archive/test-runs/x86-hw-validation-2026-04-15.md`. |
| P1-3 Idle `hlt` masks AP timers | ✅ CLOSED | Subsumed by P2-1 (reschedule IPI wakes halted APs). |
| P1-4 `sleep_ms` BSP-only ticks | ✅ CLOSED | `timer_x86.c:sleep_ms` now uses `timer_get_count()` (TSC). Tests: `test_sleep_ms_on_bsp`, `test_sleep_ms_on_ap`. |
| P1-5 Dead `lapic_timer_mask` | ✅ CLOSED | Removed from `lapic.c`; `docs/archive/investigations/x86-64-scheduler-investigation.md` Phase 8 tagged reverted. |
| P1-6 No FXSAVE in `switch_to` | ✅ CLOSED | `cpu_context.fxsave[512]` + `fxsave/fxrstor` in `context.S`. Tests: `test_context_has_fxsave`, `test_fxsave_preserves_xmm_across_preemption`. |
| P2-1 Reschedule IPI | ✅ CLOSED | Vector 49, `smp_notify_cpu()` API (Pre-1 + B1). Tests: `test_resched_ipi_delivers`, `test_resched_ipi_self_is_noop`. |
| P2-2 AP preempt integration test | ✅ CLOSED | `test_all_cpus_timer_preempt_under_load`. |
| P2-3 `CONFIG_WORK_STEALING` on x86-64 | ✅ CLOSED | `CMakeLists.txt` enables on `X86_64`; `sched.c:sched_try_steal` has `preempt_disabled` early-out. Test: `test_work_stealing_enabled`. |
| P2-4 Periodic rebalance | ✅ CLOSED | `sched_rebalance_tick()` in `sched.c` wired via `sched_heuristic.c` `.tick`. Tests: `test_rebalance_respects_affinity`, `test_rebalance_symbol_exposed`. |
| P2-5 Coupled with P2-1 | ✅ CLOSED | Same fix. |
| P3-1 x86-64 CPU SSE | ✅ CLOSED | `kernel/arch/x86_64/sse_kernels.c` (compiled `-msse -msse2`) + extern-C FFI from `ops.rs`. Bit-exact tests `test_sse_{relu,zero,add_scalar,fma_row}_matches_scalar`. |
| P3-2 / P3-3 / P3-4 GSP firmware | 🟡 PARTIAL — see `docs/x86-64-gpu-inference-status.md` | **E3.4 (FWSEC-FRTS) succeeds on hardware, 3/3 runs** as of 2026-04-16. Root cause of earlier failure was vfio-pci's FLR clobbering UEFI DEVINIT on each `/dev/vfio/GROUP` open; fixed by polling for BSI DEVINIT recovery and gating `falcon_reset` on `falcon_is_idle()`. Latent DMEM-mask bug (HWCFG bits 17:9 not 24:16) found and fixed during the audit. **E3.4.d (Booter Load) is blocked** by SEC2 priv-lock (#185) — BSI's DEVINIT re-apply raises SEC2's PLM above our VFIO-userspace access level, confirmed by `--sec2-plm-scan` (865/1024 offsets locked). E3.4.e (RISC-V start), E4 (RPC), E5 (compute), E6 (pipeline) all transitively blocked on this platform. Code-shipped portions of all phases transfer cleanly to Jetson / bare-metal. |
| P3-5 Capability detection | ✅ CLOSED | `GpuCapabilities::detect()` already returns `DetectedNoCompute` gracefully. |

Test count grew from 94 → ~120 in `kernel/tests/test_x86_boot.c`.
x86-64 QEMU baseline: 8 pre-existing failures (PMM/VMM/PCI/GIC
cascade from a pre-existing "Model memory init failed" at boot —
unrelated to this work). ARM64 `make test` still PASSES cleanly.
`make x86-disk-verify` all 9 checks PASS. End-to-end OVMF boot to
`slmos>` shell with 4 CPUs online works.

**Host-side test suites (Phase E, no GPU required):** 117 total as of 2026-04-16 (+12 from the FWSEC-FRTS hardening pass).

| Suite | Cases | Coverage |
|---|---|---|
| `make test-vbios` | 30 | VBIOS BIT parser, PCIR walker, FWSEC discovery |
| `make test-falcon` | **37** | Falcon v4 register protocol — probe, reset/scrub, halt poll, DMA framing, PIO IMEM/DMEM upload, CPUCTL.ALIAS_EN routing, pre-PIO setup, HS-boot BROM sequence, **`falcon_hs_kick` split (3 cases)**, **`falcon_is_priv_locked` 0xbadfXXXX-pattern detection (3 cases)**, **`falcon_wait_halted` priv-lock early-bail (1 case)** |
| `make test-nvfw` | 14 | nvfw_bin_hdr / hs_header_v2 / hs_load_header_v2 framing |
| `make test-bringup` | **25** | Sig-index algorithm, DMEMMAPPER patcher (legacy FRTS wrapper + **generic init_cmd-parameterised path, 3 cases**), **`gsp_bringup_free` null-safety + idempotence (2 cases)**, booter_load + riscv_start state-machine guards, error-constants distinct + negative |
| `make test-rpc` | 17 | RPC ring math, shm region init/dtor, send-rejected-when-not-alive, oversize-rejected, null-arg-rejected |

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
  multi-task reproducer from `docs/archive/investigations/x86-64-scheduler-investigation.md`.
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

The "8/8 CPUs online (4 cores × 2 hyperthreads)" claim in `docs/archive/handoff/x86-64-port.md`
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

> **This section is the long-form audit from 2026-04-13.** For the
> current end-to-end status, the root cause of the Booter Load
> blocker, the Jetson / bare-metal re-scope analysis, and proposed
> next steps, see `docs/x86-64-gpu-inference-status.md`.

### 3.1 What works today

On the RTX 3050 / GA107 (updated 2026-04-16):

| Stage | File | Result |
|---|---|---|
| PCIe ECAM enumeration | `kernel/arch/x86_64/pci.c` | 21 devices on hardware, RTX 3050 at 01:00.0, device ID 0x2584 |
| BAR0 mapping (MMIO registers, 16 MiB) | `kernel/arch/x86_64/nvidia_gpu.c` | 0x53000000 — registers readable |
| `NV_PMC_BOOT_0` / `BOOT_42` decode | `nvidia_gpu.c` | Reports Ampere, chip 0x177, rev 10.1 |
| BAR1 mapping (VRAM aperture, 256 MiB) | `nvidia_gpu.c` | 0x40000000 — R/W verified at 5 offsets with pattern tests |
| `gpu` / `pci` shell commands | `nvidia_gpu.c`, `pci.c` | Functional |
| **VFIO-backed gsp-harness** (userspace under Linux) | `host-tools/gsp-harness/` | VBIOS parse, FWSEC ucode extraction, Falcon DMA upload, BROM program, STARTCPU |
| **FWSEC-FRTS** (E3.4) | `kernel/gpu/nvidia/bringup.c::gsp_bringup_fwsec_frts` | ✅ Halts cleanly on hardware (3/3 runs 2026-04-16), ERR_REG=0, WPR2 registers populated |
| BSI DEVINIT recovery check (`--check-devinit`) | `host-tools/gsp-harness/main.c` | Polls `NV_PGC6_AON_SECURE_SCRATCH_GROUP_05[0]` byte 0 for `0xff` per nouveau `tu102_devinit_wait`. Recovers in ~300 ms post-FLR on test-pc. |

**What is not there yet:** Booter Load (E3.4.d), GSP RISC-V startup
(E3.4.e), RPC connection establishment (E4), compute engine init
(E5), full inference pipeline (E6). All four are **transitively
blocked on this platform** by the SEC2 priv-lock described below.

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

### 3.3 Gaps — 2026-04-16 refresh

| # | Gap | Current status | Priority |
|---|---|---|---|
| P3-1 | x86-64 CPU inference is scalar only (#72). | ✅ **Closed** via inline-asm SSE bypass (`kernel/arch/x86_64/sse_kernels.c`). |  — |
| P3-2 | GSP firmware loader. | 🟡 **Half delivered.** FWSEC-FRTS succeeds on hardware (3/3 runs 2026-04-16). Booter Load blocked by **SEC2 priv-lock (#185)**. Details in `docs/x86-64-gpu-inference-status.md`. | Blocked (#185) |
| P3-3 | VBIOS parser. | ✅ **Closed.** `kernel/gpu/nvidia/nvidia_vbios.c` (690 lines, 30 unit tests) — parses BIT tables, walks PCIR, extracts FWSEC. Hardware-verified on GA107. | — |
| P3-4 | RPC / message-queue stack. | 🟡 **Skeleton shipped** (`kernel/gpu/nvidia/rpc.c`, 216 lines, 17 unit tests). Cannot exercise against a live GSP-RM because E3.4.e (RISC-V start) is blocked by #185. | Blocked (#185) |
| P3-5 | GPU capability detection. | ✅ **Closed.** `GpuCapabilities::detect()` returns `DetectedNoCompute`. | — |

GitHub issues covering this area:

- **#28** (Jetson TensorRT/CUDA): blocked on GSP firmware. Not
  x86-64-specific, but the same blocker applies.
- **#27** (Jetson GPU memory integration, `gpu_map` / `SHM_GPU_ACCESSIBLE`):
  abstractions are in place (`runtime/src/inference/gpu.rs`) but
  unusable without a GPU backend. The x86-64 stub currently no-ops.
- **#72** (x86-64 SSE): blocks P3-1. Detailed investigation in the
  issue comments; requires nightly Rust + custom target spec
  + `-Z build-std` to land.

### 3.4 Realistic paths forward — 2026-04-16 refresh

Options A, B, E from the original 2026-04-13 table all landed. C
(port nouveau's GSP-RM loader) went further than estimated: FWSEC-FRTS,
DMEMMAPPER patching, and the Booter-Load scaffolding all shipped
within ~4 weeks. The remaining blocker is the one that wasn't
anticipated: VFIO's mandatory FLR + BSI DEVINIT raising SEC2's PLM
(#185). See `docs/x86-64-gpu-inference-status.md` §4 for the three
remaining candidate paths (Jetson, bare-metal SLM-OS, kernel-shim).

---

## 4. Prioritized Action Plan — 2026-04-16 refresh

| Rank | Action | Original est | Current status |
|---|---|---|---|
| 1 | Reschedule IPI on x86-64 | 2–3 d | ✅ Delivered |
| 2 | Secondary-CPU preemption integration test | 1 d | ✅ Delivered |
| 3 | Real-hardware multi-task boot test on test-pc | 1 d | ✅ Delivered |
| 4 | Inline-asm SSE matmul (Option A) | 3–5 d | ✅ Delivered |
| 5 | Enable `CONFIG_WORK_STEALING` + benchmark | 2 d | ✅ Delivered |
| 6 | TSS IST for timer vector | 2–3 d | ✅ Delivered |
| 7 | Honest x86-64 benchmarks in `docs/benchmarks.md` | 0.5 d | ✅ Delivered (G5) |
| 8 | Delete unused `lapic_timer_mask()` helpers | 15 min | ✅ Delivered |
| 9 | Periodic policy rebalance | 3–5 d | ✅ Delivered; S5 adds proactive override |
| 10 | Port nouveau GSP-RM (post-capstone) | 6–10 wk | 🟡 ~4-5 weeks in, ~40-60% of the portable bringup done, hard-blocked on #185 for this platform. See `docs/x86-64-gpu-inference-status.md`. |

**All capstone-scope items (1–9) delivered.** Item 10's remaining
path-forward options are documented in the handoff.

---

## 5. Issue Tracking — 2026-04-16 audit

All capstone-scope proposed issues from the original table turned
into delivered work, not filed issues. The one entry still open is
the post-capstone GSP work, which has a dedicated sub-blocker:

| Issue | Status | Scope |
|---|---|---|
| **#185** — SEC2 priv-lock blocks Booter Load under VFIO+FLR on x86-64 | OPEN | The specific hardware-security boundary that gates E3.4.d / E3.4.e / E4 / E5 / E6 on this platform. See `docs/x86-64-gpu-inference-status.md`. |
| #72 — x86-64 SSE intrinsics (Rust toolchain) | OPEN, `blocked` | Bypassed via inline-asm in `kernel/arch/x86_64/sse_kernels.c`; keep the issue open but the P3-1 path is unblocked. |
| #142 — GSP bare-metal loader (future work) | OPEN, `P3-low` | Original 6-12 month scope estimate. What actually happened: ~60% of the portable bringup landed in ~4 weeks; the remaining gap is the #185 blocker on x86-64. |
