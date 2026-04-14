# SLM-OS Capstone: Thesis Framing

Draft narrative for the capstone report. Positions the delivered
capability (NEON CPU inference on Jetson / Pi 5 / QEMU) against the
GPU-compute work that remained blocked by proprietary firmware.

**Last updated:** 2026-04-14 (Phase G6).

---

## One-paragraph pitch

SLM-OS is a bare-metal operating system purpose-built to run a small
language model on an embedded accelerator. It boots on QEMU ARM64,
Raspberry Pi 5, and the Jetson Orin Nano; runs a NEON-accelerated
FP32 / FP16 / INT8 inference stack as a first-class kernel
workload; and schedules inference requests using a hybrid deadline /
work-stealing scheduler with optional AI-assisted policy selection.
The capstone deliverable is the cross-platform inference benchmark
(`docs/cross-platform-inference-bench.md`) showing that the same
kernel math runs on three ARM64 targets with predictable latency.

## What we built vs. what we originally proposed

The original proposal had two load-bearing claims:

1. **Bare-metal SLM inference on the Jetson Orin Nano**, including
   use of the Orin's GPU compute engines.
2. **An operating system designed from scratch for AI workloads** —
   scheduler, memory manager, and runtime optimized for inference
   instead of general-purpose workloads.

Claim 2 was fully delivered. Claim 1 was delivered only through CPU
(NEON) paths; GPU compute remains blocked on NVIDIA's GSP firmware
(see §GPU Support and the GSP Blocker).

### Delivered

| Capability | Jetson | Pi 5 | QEMU | x86-64 |
|---|---|---|---|---|
| Boot to shell | ✅ EL2 + VHE | ✅ BCM2712 native | ✅ virt | ✅ multiboot2 |
| SMP (6 / 4 / 4 / 4 cores) | ✅ cooperative | ✅ cooperative + IRQ work | ✅ | ✅ with LAPIC IPI |
| Preemptive scheduling | ⚠️ cooperative only (see §Scheduling) | ⚠️ cooperative + timer IRQ on CPU 0 | ✅ | ✅ with LAPIC timer |
| NEON FP32 MatMul | ✅ | ✅ | ✅ (emulated) | n/a (SSE stub) |
| NEON Conv2D | ✅ | ✅ | ✅ | n/a |
| Softmax / LayerNorm / RMSNorm / GELU | ✅ | ✅ | ✅ | ⚠️ build blocked |
| FP16 matmul (dequant-then-FP32) | ✅ | ✅ | ✅ | ⚠️ build blocked |
| INT8 matmul (scalar INT32 acc) | ✅ | ✅ | ✅ | ⚠️ build blocked |
| GPU detection | ✅ (GA10B via CBB bypass) | n/a (no GPU) | n/a | ✅ (GA107 / RTX 3050) |
| GPU compute | ❌ GSP blocker | n/a | n/a | ❌ GSP blocker |

### Not delivered (and why)

- **GPU compute on Ampere / Orin.** NVIDIA's GSP firmware is now
  mandatory to bring the graphics and compute engines out of reset.
  Loading it from bare metal would require a cryptographically
  verified 38 MB blob, a VBIOS parser, SEC2 Falcon ucode, and a full
  RPC stack — a separate project's worth of work. See §GPU Support
  and the GSP Blocker.
- **Secondary-CPU preemptive scheduling on real ARM64 silicon.**
  Timer IRQs route through the GIC's Group 0/Group 1 config which
  both Pi 5 and Jetson leave under EL3 ownership, so kernel writes
  are silently ignored and PPI 30 never delivers to EL1/EL2. The
  cooperative workaround (CNTPCT polling in `schedule()`) drives
  accounting / migration / AI-policy evaluation on yield points.
  QEMU and x86-64 have full preemptive scheduling — the gap is
  hardware-specific.

## Architecture narrative

### Why a custom OS instead of Linux + PyTorch

A single reason: *predictability*. Linux's scheduler is tuned for
fairness across many workloads; a bare-metal inference OS can
guarantee that no unrelated kernel work preempts a model forward
pass, and that the hot path touches only memory the model actually
needs. The capstone measures this: context-switch latency on Pi 5 is
1.9 µs (`docs/benchmarks.md`), vs. the 5–50 µs typical of desktop
Linux at 1 kHz tick. The same measurement is the control variable
for the scheduler's AI-policy experiments — without a baseline that
isn't noised by competing work, the RL / heuristic / MLP comparisons
are meaningless.

### Scheduler

`kernel/sched/sched.c` implements a three-tier policy: a static
priority round-robin (baseline), a deadline-boost policy for real-time
inference requests (`sched/deadline.*`), and a pluggable AI policy
(`sched_ai/` — heuristic, MLP with trained weights, or RL) that runs
on every policy tick and may migrate or reorder work. Work-stealing
across CPUs is gated by `CONFIG_WORK_STEALING` and uses a
Chase-Lev-inspired deque (`sched/steal_deque.c`) backed by
non-cacheable memory on platforms where the DSU doesn't provide
cross-core coherency (Pi 5 and Jetson both fall into this category
— see §NC memory).

### Memory

Buddy allocator backing the PMM (`kernel/mm/pmm.c`). Model weights
live in a dedicated 2 MB-aligned region managed by a reference-counted
allocator in `runtime/src/mm/model_mem.rs` so a second model load
can share weights with an already-resident model without copying.
Eviction policy is itself pluggable and has RL / MLP backends for the
AI-eviction experiments.

### Inference runtime

Pure Rust, `#![no_std]`, staticlib. Exposes FFI from `lib.rs`
(`runtime/src/lib.rs`). The inference kernels live in
`runtime/src/inference/ops.rs` and follow a three-arm SIMD dispatch
skeleton: aarch64 NEON (implemented), x86_64 SSE (skeleton today,
scalar fallback), and a generic scalar arm for other targets. Kernels
landed in G1 (MatMul), G2 (Conv2D), G3 (Softmax / LayerNorm / RMSNorm
/ GELU), and G4 (FP16 / INT8). See
`docs/cross-platform-inference-bench.md` for the measured numbers.

## GPU Support and the GSP Blocker

> **Plain-English version:** We wanted SLM-OS to use the GPU on the
> Jetson Orin Nano. NVIDIA changed Ampere so that the CPU can't start
> the GPU any more without first loading a 38 MB signed firmware
> blob onto a RISC-V microcontroller inside the GPU. Loading that
> firmware from bare metal is a separate project. We documented the
> full chain, verified on a discrete RTX 3050 that detection and
> VRAM access still work, and shipped CPU NEON inference as the
> delivered capability.

### What works without GSP

Verified on a discrete NVIDIA RTX 3050 (GA107) connected to our x86
development machine:

- PCI enumeration finds the GPU (21 devices via ECAM).
- BOOT_0 / BOOT_42 registers correctly identify the chip as GA107
  (device 0x2584, Ampere family 0x17, rev 10.1).
- BAR1 VRAM read/write works — 5 offsets verified over 0–128 MB.
- PTIMER (GPU timer) and PSTRAPS (strap configuration) are readable
  without GSP.

### What is blocked without GSP

On the same hardware, reading any GPU *engine* register returns
`0xBADF5040`, NVIDIA's hardware-fault sentinel indicating the engine
is in reset. This includes PGRAPH (graphics / compute), PDISPLAY, and
the copy engines. No amount of CPU-side register programming brings
the engines out of reset — the engines read their reset signal from
GSP-RM, which runs on the on-die RISC-V core (LibOS) and is
responsible for all engine bring-up on Ampere and later.

### Why this is specifically bare-metal-hard

A full Linux + nouveau / open-gpu-kernel-modules stack loads GSP via:

1. Kernel firmware loader pulls `gsp-<version>.bin.zst` from
   `/lib/firmware/nvidia/<chip>/gsp/` (38 MB, zstd-compressed
   RISC-V ELF).
2. VBIOS is parsed to discover chip-specific parameters.
3. A Write-Protected Region (WPR) is allocated in VRAM.
4. SEC2 Falcon runs a "Booter Load" HS microcode blob that performs
   cryptographic verification and copies GSP-RM into WPR.
5. The RISC-V core is released from reset.
6. Host CPU and GSP-RM exchange RPCs over a ring buffer for the rest
   of the GPU's life.

A bare-metal OS would need its own VBIOS parser, a DMA allocator
large enough for the WPR, a Falcon microcode execution environment,
an RPC stack, and the firmware blob shipped in its ROM — none of
which are incremental additions to a capstone-scoped kernel. The full
chain is documented at `docs/nvidia-gsp.md`.

### Why Jetson is the same problem

The Jetson Orin Nano uses GA10B — an integrated variant of the same
Ampere architecture. The GSP boot sequence is identical: same RISC-V
core, same firmware format, same Booter Load mechanism. On Jetson the
firmware is pre-loaded by the UEFI / CBoot chain instead of the OS,
but once Linux has kexec'd into SLM-OS, the GSP state is either
suspended (via `slmos-kexec`'s runtime-PM suspend) or in an
undefined-after-kexec state. Re-initialising it without the RPC
stack is the same problem as on discrete Ampere.

### Decision and framing

The GPU work shipped as:

- **Infrastructure** (`kernel/src/gpu.c`, `runtime/src/inference/gpu.rs`):
  unified memory allocator, cache-maintenance helpers, device
  enumeration, capability detection. This code is ready for the day a
  GSP-capable driver appears (via NVIDIA releasing a static FW path,
  a third-party RE effort, or us accepting a much larger project
  scope).
- **Research artifact** (`docs/nvidia-gsp.md`): a full write-up of
  the boot chain, the firmware files involved, and the Linux /
  nouveau references that document the protocol. This is what a
  future contributor reads to pick up the work.
- **Honest framing** in the capstone report: the GPU cannot be the
  delivered inference path on Ampere without a separate firmware-
  loader project. CPU NEON is the delivered path.

The cross-platform benchmark (G5) is the consequence: by measuring
the same kernel math across QEMU, Pi 5, and Jetson, we demonstrate
that the OS is actually platform-portable and that the inference
workload genuinely runs on the embedded target — the original
capstone claim, just via CPU instead of GPU.

## Scheduling — the cooperative preemption story

> **Plain-English version:** On Pi 5 and Jetson, the hardware timer
> interrupt never reaches SLM-OS because the firmware leaves it
> routed to a higher privilege level that we can't write. We work
> around this by checking the hardware clock every time the scheduler
> runs, which lets us do almost everything preemption would normally
> give us — quantum expiry, migration, AI-policy ticks — just at
> yield points instead of at arbitrary instruction boundaries.

### What the GIC does on Pi 5 and Jetson

Both platforms ship a GICv2 / GICv3 configured with two security
states by TF-A (Trusted Firmware). The Group-register bits that
choose whether a PPI delivers as IRQ or FIQ are owned by EL3 on both
platforms. When the kernel tries to write `GICR_IGROUPR0` from NS
EL1 (Pi 5) or NS EL2 (Jetson), the write is silently dropped — we
verified this on Jetson hardware with an early boot-time print:
`GICR_IGROUPR0=0x0` after writing `0xffffffff` to it.

Consequence: PPI 30 (the architected timer) is pending in the GIC —
`GICC_HPPIR=30` during a busy wait confirms it's there — but no
exception ever fires. See `docs/pi5-preemption-resolution.md`.

### The workaround

`coop_preempt_maybe_tick()` in `kernel/sched/sched.c` reads
`CNTPCT_EL0` every time `schedule()` runs. If ≥10 ms has elapsed
since the last tick on that CPU, it synthesizes a `scheduler_tick()`
call. This advances `pit_ticks`, fires deadline / migration /
AI-policy logic, and updates observability counters — everything the
timer IRQ would have done, just driven by yield points rather than
preemptively.

Functionally:
- A well-behaved task that yields regularly gets scheduled as if
  preemption were working.
- A CPU-bound loop that never yields monopolizes its CPU. The
  `delay()` helper in `kernel/tests/test_integration.c` yields every
  ~1k iterations for exactly this reason.
- Cross-CPU task migration works, and `bench smp` validates dispatch
  to all 6 Jetson cores (`timer_handler_count: 0 → 18 → 36` in
  successive bench invocations).

The capstone framing is honest: *SLM-OS demonstrates cooperative
multitasking with policy-tick-driven accounting on Pi 5 and Jetson.
True per-instruction preemption requires either a TF-A modification
(which would re-route PPI 30 to Group 1 NS IRQ) or a hardware
change, neither of which are in the capstone scope.* QEMU and
x86-64 have real preemption; the gap is limited to the two hardware
ARM64 targets.

## Delivered artifacts — where to look

| Artifact | Location | What it demonstrates |
|---|---|---|
| Cross-platform inference bench | `docs/cross-platform-inference-bench.md` | Single-page "here are the numbers" for the capstone |
| Platform bringup | `docs/jetson-el2-bringup.md`, `docs/pi5-preemption-resolution.md`, `docs/x86-64-capstone-closure-plan.md` | Platform-specific investigation logs |
| Scheduler architecture | `docs/architecture.md` §scheduling, `kernel/sched/sched.c` | Three-policy implementation |
| AI scheduler experiments | `docs/ai-scheduler.md`, `kernel/sched_ai/` | MLP / RL vs. heuristic comparisons |
| Inference runtime | `runtime/src/inference/ops.rs`, `runtime/src/inference/engine.rs` | NEON FP32/FP16/INT8 kernels |
| GPU research | `docs/nvidia-gsp.md`, `docs/jetson-nvidia-support.md` | Why GPU compute was descoped |
| Thesis framing (this doc) | `docs/capstone-thesis-framing.md` | Narrative for the capstone report |

## Open questions for the advisor

- **Scope of the GPU narrative in the thesis body.** Current plan is
  one chapter explaining GSP as the blocker, with the detailed
  research (~200 lines) in an appendix. Alternative: a shorter
  section in the "lessons learned" / "future work" chapter, pushing
  the full research to `docs/nvidia-gsp.md` and only citing it. Open
  for input.
- **Cooperative vs. preemptive terminology.** Calling what Pi 5 /
  Jetson ship "cooperative" is technically correct but could read as
  a weakness. An alternative is "policy-tick-driven scheduling" or
  "yield-point multitasking" — honest and more descriptive of what
  the system actually provides. Happy to use whichever phrasing the
  advisor prefers.
- **Benchmarks table — one or many?** Current draft has a single
  table in `docs/cross-platform-inference-bench.md`. The thesis could
  either reproduce that table verbatim or split it per-kernel (one
  table for MatMul, one for Conv, one for quantization) to pace the
  reader better.

---

*Draft 1 — for advisor review. Update the "Open questions" section as
decisions land and merge the final text into the capstone report.*
