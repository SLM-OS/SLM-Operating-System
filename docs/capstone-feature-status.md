# Capstone Feature Status

Cross-platform status of the five core SLM-OS features: SMP, Preemptive
Multitasking, GPU-Based Inference, AI Task Scheduling, and AI Page
Eviction.

**Platforms:** QEMU (ARM64), Raspberry Pi 5, Jetson Orin Nano, x86-64

**Last updated:** 15 April 2026

---

## 1. SMP (Symmetric Multiprocessing)

### Summary

All four platforms boot all available CPUs. Cross-CPU task dispatch is
fully working on QEMU, Jetson, and x86-64. Pi 5 is limited by a
firmware cache-coherency gap.

### Per-Platform Status

| | QEMU | Pi 5 | Jetson | x86-64 |
|--|------|------|--------|--------|
| Cores booted | 4/4 | 4/4 | 6/6 | 8/8 (i7-6700) |
| Boot mechanism | PSCI (HVC) | PSCI (SMC) | PSCI (SMC, EL2 post-kexec) | INIT-SIPI-SIPI + ACPI MADT |
| Cross-CPU dispatch | Full | Full (fixed Apr 16) | Full (fixed Apr 15) | Full |
| IPI / wake | SEV broadcast | SEV broadcast | SEV broadcast | LAPIC IPI (vector 49) |
| HW cache coherency | Automatic | Manual DC CVAC/CIVAC | Manual DC CVAC/CIVAC | Automatic |

### Details

**QEMU:** Baseline platform. Hardware-coherent caches hide the
cache-maintenance complexity present on real hardware. Four Cortex-A72
cores via PSCI HVC. Per-CPU run queues with LDAXR/STXR spinlocks.
Five multi-core integration tests pass. All 393 tests pass.

**Pi 5 (BCM2712, 4x Cortex-A76):** SMP boot is functional via PSCI SMC.
TF-A firmware does not set SMPEN (bit 6 of CPUECTLR_EL1) on secondary
cores, so the OS runs without hardware cache coherency. Workarounds:
NC (non-cacheable) memory at `0xFFE00000` for shared scheduler state
(run queues, task table, current-task pointers, work-steal deques);
DC CIVAC / DSB SY around spinlock LDAXR/STLR on Pi 5 so lock state
goes through DRAM; and DC CVAC/CIVAC helpers for the few remaining
cacheable shared fields. Cross-CPU dispatch is now **full** — tasks
distribute across all 4 CPUs, migration works, work-stealing works.
14 of 15 multi-core integration tests pass every run on hardware;
the 15th (`test_work_stealing_distributes_load`) is documented as
inherently timing-flaky. `bench smp` and `bench stealing` both work.

**Jetson (Tegra234, 6x Cortex-A78AE, dual cluster):** SMP boot
functional post-kexec at EL2. Six cores across two clusters (MPIDR:
0x000, 0x100, 0x200, 0x300, 0x10200, 0x10300). Hardcoded MPIDR table
avoids cache visibility issues during early boot. Cross-CPU dispatch
recently fixed (commit `4b6cda4`, 15 April 2026) by adopting Pi 5's
runtime `spinlock_hw_enabled` model — pre-MMU spinlocks are
barrier-only (LSE atomics fault on non-cacheable memory), post-MMU
LDAXR/STXR provide real mutual exclusion. Prior bug (#166): compile-time
`SPINLOCK_SKIP_LOCKING` disabled all cacheable locks, causing PMM
free-list corruption under concurrent work-stealing. NC memory at
`0xBDE00000` (2 MB, last block before OP-TEE carveout).

**x86-64 (i7-6700, 8 cores):** Full SMP via INIT-SIPI-SIPI sequence.
ACPI MADT parsing discovers CPUs. BSP copies AP trampoline to low memory
(0x8000); each AP transitions through 16-bit real mode, 32-bit protected
mode, and 64-bit long mode before entering `ap_entry_64()`. Per-CPU TSS
with 4 KB IST1 stacks protect timer/IPI frames. Reschedule IPI (vector
49) via LAPIC delivers work to idle APs with sub-millisecond latency.
Hardware cache coherency (x86 MESI protocol) eliminates all manual cache
maintenance.

---

## 2. Preemptive Multitasking

### Summary

QEMU and x86-64 have true hardware-timer-driven preemption. Pi 5 and
Jetson use cooperative preemption (`COOP_PREEMPT`) because TF-A firmware
masks timer IRQs from non-secure EL1/EL2. This is an accepted design
limitation.

### Per-Platform Status

| | QEMU | Pi 5 | Jetson | x86-64 |
|--|------|------|--------|--------|
| Preemption model | True (HW timer) | Cooperative | Cooperative | True (HW timer) |
| Timer source | GIC PPI 30 | CNTPCT_EL0 polled | CNTPCT_EL0 polled | LAPIC timer (vec 48) |
| Tick rate | 100 Hz | 100 Hz synthesized | 100 Hz synthesized | 100 Hz |
| Context switch | From ISR | From `schedule()` | From `schedule()` | From ISR (IST1 stack) |
| FPU save/restore | NEON v0-v31 | NEON v0-v31 | NEON v0-v31 | FXSAVE/FXRSTOR |
| Work stealing | ON | ON | ON | ON |

### Details

**QEMU:** GIC delivers timer IRQ (PPI 30) to EL1. Timer ISR calls
`scheduler_tick()` then `schedule()`, performing involuntary context
switches via `switch_to()`. Callee-saved GPRs (x19-x30), SP, DAIF, and
all 32 SIMD registers (v0-v31, FPCR, FPSR) are saved/restored.

**Pi 5 and Jetson (cooperative preemption):** The GIC on both platforms
runs with two security states; the Group register that routes PPIs to
IRQ vs FIQ is owned by EL3 firmware, and non-secure writes are silently
ignored. Hardware timer IRQs never arrive at EL1/EL2 (issue #99, #134).

Resolution: `COOP_PREEMPT` (CMake option, default ON for both
platforms). `schedule()` checks `CNTPCT_EL0` on every entry; if >= 10 ms
has elapsed since the last tick on that CPU, it synthesizes a
`scheduler_tick()` call. This drives AI scheduling, deadline boosts,
migration, and observability counters at yield points. A pure CPU-bound
loop that never yields still monopolizes its CPU.

`SECONDARY_PREEMPT` (ELR-trampoline infrastructure) is compiled but
inert on Pi 5 (awaiting hardware IRQ restoration). On Jetson it is
unsafe — the MPIDR-folding formula in the trampoline collides on
dual-cluster cores 4/5. A boot-time check (`preempt_check_cpu_mpidr`)
panics if enabled on Jetson.

**x86-64:** LAPIC timer in periodic mode at 100 Hz, calibrated against
PIT (8254). Timer ISR (vector 48, interrupt gate) calls
`scheduler_tick()` then `schedule()`. Per-CPU `preempt_disabled` flag
prevents reentrant scheduling during the stack switch. New tasks enter
via `task_entry_trampoline` which clears the flag, allowing the first
timeslice to be preempted. FXSAVE/FXRSTOR saves the full 512-byte SSE
state. TSC provides high-precision `sleep_ms()` independent of PIT tick
granularity.

---

## 3. GPU-Based Inference

### Summary

All inference currently runs on CPU. The AI scheduler's MLP runs on CPU
with NEON/SSE acceleration. Actual GPU compute is blocked on both
Ampere platforms by hardware security boundaries:

- **x86-64 discrete GA107**: Booter Load blocked by SEC2 priv-lockdown
  raised by VFIO's mandatory PCI FLR (#185)
- **Jetson integrated GA10B**: ACR HS ucode load blocked by GSP Falcon
  priv-lockdown (HWCFG2 bit 13) after kexec-from-Linux

Both are structural — fixing either requires either a privilege-level
unlock path through EL3 or a boot model that doesn't hand the GPU off
from a prior driver (bare-metal x86-64 / UEFI-direct Jetson).

### Per-Platform Status

| | QEMU | Pi 5 | Jetson | x86-64 |
|--|------|------|--------|--------|
| GPU hardware | None | VideoCore (inaccessible) | GA10B (MMIO live at EL2) | GA107/RTX 3050 (PCI, BARs mapped) |
| GPU driver | Stub | Stub | Full NVIDIA shim + scaffolded nvgpu bringup | Full NVIDIA shim + GSP-RM bringup |
| Platform shim (`gsp_platform_ops`) | N/A | N/A | Complete (11/11 fns, `kernel/arch/arm64/nvidia_gsp_platform.c`) | Complete (11/11 fns) |
| Engine reset + PIO upload | N/A | N/A | Working on GSP Falcon | Working on GSP + SEC2 |
| Signed ucode authentication | N/A | N/A | Blocked: GSP priv-lockdown | FWSEC-FRTS 3/3; Booter Load blocked |
| Inference backend | CPU (NEON) | CPU (NEON) | CPU (NEON) | CPU (SSE inline-asm) |
| AI scheduler MLP | CPU | CPU | CPU | CPU |

### GPU Bringup Stack (Portable)

The shared GSP-RM bringup code in `kernel/gpu/nvidia/` is portable
across platforms via `struct gsp_platform_ops` (see `docs/nvidia-gsp.md`
§"Platform Shim Contract"). Jetson adds an **nvgpu-native** parallel
path (`ga10b_bringup.c`) because GA10B ships a different firmware
stack than discrete Ampere.

| Component | File | Tests | Status |
|-----------|------|-------|--------|
| VBIOS BIT-table parser | `nvidia_vbios.c` (700 lines) | 30 | Complete |
| Falcon v4 register protocol | `falcon.c` (500 lines) | 37 | Complete |
| FWSEC/DMEMMAPPER/sig-index (discrete) | `bringup.c` (1050+ lines) | 25 | Complete |
| RPC ring skeleton (discrete) | `rpc.c` | 17 | Skeleton, needs GSP-RM payloads |
| **GA10B nvgpu bringup (Jetson)** | `ga10b_bringup.c` (500 lines) | **10** | Phase 1 (ACR load) wired; BROM-blocked |
| **Total host-side tests** | | **133** | **All passing** |

### Platform-Specific Blockers

**Pi 5:** VideoCore GPU on BCM2712 has no public bare-metal compute
documentation. Accessing it would require reverse-engineering the
VideoCore ISA and firmware. Not feasible within capstone scope.

**Jetson (GA10B):** MMIO confirmed live at EL2 — `NV_PMC_BOOT_0` reads
`0xB7B000A1` (GA10B, Ampere Rev 10.1), `NV_PMC_BOOT_42` reads
`0x17BA1000` (arch=0x17, impl=0xB). The kexec helper force-enables GPU
clocks via BPMP debugfs (`scripts/jetson-kexec-slmos.sh`) so the
engine stays powered through the Linux → SLM-OS handoff.

Architectural finding: GA10B uses the **nvgpu-native firmware stack**
(acr-gsp.*, FECS/GPCCS, PMU, NET images), not the discrete-Ampere
GSP-RM stack. The shim embeds all 17 firmware blobs via `.incbin` when
`-DGA10B_FIRMWARE_DIR=...` is configured. A parallel `ga10b_bringup.c`
implements the nvgpu-style boot sequence alongside the GSP-RM path in
`bringup.c`. See `docs/jetson-nvgpu-bringup-research.md` and
`docs/jetson-nvgpu-acr-analysis.md` for the full decomposition.

Phase 1 (ACR HS load on GSP RISCV) is plumbed: engine reset works
(GA10B-specific assert/deassert pattern), PIO upload loops are wired
to the correct IMEMC/DMEMC registers, BCR_CTRL=0x11 programs
preloaded-mode, STARTCPU kicks the BROM. On hardware the BROM returns
`BR_RETCODE.result = 2 (FAIL)` because `HWCFG2` bit 13
(RISCV_BR_PRIV_LOCKDOWN) is asserted — the GSP Falcon's PRI aperture
is locked to an EL3/secure privilege level, so our EL2-NS PIO writes
to IMEM/DMEM are silently dropped (readback confirms: returns
`0xbadf5620` poison). BROM then authenticates against empty DMEM and
fails cleanly.

Three paths forward:
1. **SMC to TF-A / NVIDIA SiP service** to lower the Falcon's PLM
2. **UEFI direct boot** — bypass kexec; `kernel/arch/arm64/efi_stub.c`
   exists but isn't functional
3. **Reuse Linux-nvgpu's ACR bringup state** — have Linux bring up
   GSP successfully, then kexec without the suspend

None unlock compute in the capstone window, but all have been scoped.

**x86-64:** FWSEC-FRTS succeeds on hardware (3/3 runs VFIO, 4/4 runs
bare-metal SLM-OS — April 15 2026, WPR2 populated at 0x1ffffe00 on
both paths). Booter Load (E3.4.d) is hard-blocked by the SEC2
privilege-level mask (#185). Originally believed to be VFIO-specific
(FLR → BSI → DEVINIT re-runs the VBIOS DEVINIT script, which raises
SEC2 PLM); hardware validation on bare-metal SLM-OS invalidated that
narrow framing. The priv-lock is also present at UEFI handoff on this
board, so bare-metal alone does NOT work around it. Cross-validation
on the same board under Linux + nouveau shows SEC2 accessible
(CPUCTL=0x20) post-driver-load — something nouveau does (suspected
VBIOS DEVINIT replay via its devinit subdev) clears the lock. Candidate
next paths: port nouveau's devinit bytecode interpreter, or Linux-to-
SLM-OS kexec handoff that inherits the unlocked state. Both are
beyond capstone scope. See `docs/testing/x86-gpu-bringup-2026-04-15.md`
for the full hardware validation report.

---

## 4. AI-Based Task Scheduling

### Summary

The AI scheduler is fully implemented across all four platforms with
real trained weights and SIMD-accelerated inference. It is disabled by
default (`AI_SCHED=OFF`) and can be enabled at build time or switched at
runtime via the shell.

### Per-Platform Status

| | QEMU | Pi 5 | Jetson | x86-64 |
|--|------|------|--------|--------|
| Builds with AI_SCHED=ON | Yes | Yes | Yes | Yes |
| Trained weights | Real (MLP + PPO) | Real | Real | Real |
| Inference backend | NEON (auto-vec) | NEON intrinsics | NEON intrinsics | SSE intrinsics |
| Inference latency target | < 50 us | ~42 us measured | < 50 us | < 50 us |
| Runtime policy switch | Yes (`sched policy ai_mlp`) | Yes | Yes | Yes |

### Architecture

**State vector:** 108 dimensions (6 cores x 6 features + 8 tasks x 8
features + 8 global features). Per-core features include utilization,
queue depth, cache pressure. Per-task features include priority, deadline
urgency, working set size, model size, inference duration. Global
features include ready count, deadline miss rate, load imbalance.

**MLP architecture:** 4-layer feedforward (108 -> 256 -> 256 -> 128 ->
N_ACTIONS) with ReLU activations. Weights are ~3 MB per model, embedded
via auto-generated C arrays (`ai_weights_mlp.c`, `ai_weights_ppo.c`).

**Decision output:** Each inference produces a scheduling action
encoding core assignment (0-5 or GPU), priority adjustment (+/-1 level),
and a preempt flag. On Jetson, N_ACTIONS=42 (6 cores + GPU x 3 priority
levels x 2 preempt options).

**Policies:** Pluggable `sched_policy_ops` interface. Available
policies: `default` (heuristic round-robin + deadline boost), `ai_mlp`
(MLP with Plan A trained weights), `ai_ppo` (PPO policy network).
Switchable at runtime via `sched policy <name>`.

**Fallback:** If inference fails or produces an invalid action (e.g.,
assigning to a non-existent core or an isolated core), the policy
reverts to the heuristic scheduler. Fallback count is tracked per-policy
and reported at shutdown. Both `ai_mlp_init()` and `ai_ppo_init()` run a
zero-vector self-test at startup; failure disables the policy.

### Known Gaps

Several state-vector features are stubbed at 0.0f pending integration:
GPU queue depth, weight/workspace pool pressure, per-task working set
size, model size, and inference duration. These require Rust FFI hooks
and GPU driver integration that will become available as GPU compute
comes online.

---

## 5. AI-Based Page Eviction

### Summary

AI-driven page eviction is fully implemented and tested across all four
platforms. The system uses an ensemble of classical and ML-based
policies to select eviction victims from the model weight and workspace
memory pools.

### Per-Platform Status

| | QEMU | Pi 5 | Jetson | x86-64 |
|--|------|------|--------|--------|
| Status | Complete | Complete | Complete | Complete |
| Build flag | AI_EVICTION=ON (stubs) / AI_EVICTION_MODELS=ON (weights) | Same | Same | Same |
| Test coverage | 24 Unity + 91 Rust invariant | Same | Same | Same |
| Binary overhead (stubs) | +48 KB stripped | Same | Same | Same |
| Binary overhead (models) | +254 KB stripped | Same | Same | Same |

### Memory Model

SLM-OS does not use traditional demand paging. Physical memory is
managed by a buddy allocator (PMM) with 2 MB block mappings via MMU. The
model allocator maintains two fixed pools:

- **Weight pool:** read-only, shareable across tasks
- **Workspace pool:** transient, per-inference scratch space

When a pool is full, `alloc_weights()` or `alloc_workspace()` calls
`evict_and_retry()`, which invokes the active eviction policy to select
a victim. There is no swap partition or page-fault-driven demand paging.

### Eviction Policies

**Classical:** LRU, LFU, ARC, SLM-Heuristic (all ported from a sibling
simulator).

**ML-based:**
- **XGBoost:** 200-tree if-else decision chain (~1.3 MB source). Scores
  each candidate on 27 features.
- **int8-quantized MLP:** 4-layer feedforward (~20 KB weights).

**Ensemble (CACHEUS):** Adaptive weighted ensemble combining XGBoost and
MLP with online learning. Multiplicative weight updates based on
re-access feedback (evicted content that is re-admitted scores as a bad
eviction). Default tuning: learning rate 0.4, feedback window 200 ms.
The `ml_only` pool configuration is recommended (0.212 mean normalized
fault rate in benchmarks).

### Feature Extraction

27-dimensional feature vector per eviction candidate:

- **15 per-block features:** recency rank, frequency rank, access count,
  time since last access, reference count, GPU-mapped flag, dirty flag,
  layer index, model priority, eviction cost, and others.
- **12 global features:** pool utilization, GPU-mapped ratio, and 9
  scheduler-feed placeholders.

### Feedback Loop

The `EvictedContentTracker` (256-entry FIFO) records evicted blocks by
pool type, model ID, and layer index. On re-admission of the same
content key, `update_feedback(_, true)` fires (bad eviction). After
200 ms without re-access, `update_feedback(_, false)` confirms a good
eviction. This feedback drives the CACHEUS ensemble weight updates.

### Latency (QEMU Release Build)

| Policy | Per-call latency |
|--------|-----------------|
| LRU | 71 ns |
| XGBoost (real weights) | 190.7 us |
| MLP (real weights) | 1.58 ms |
| CACHEUS ml_only (real) | 1.73 ms |

Real hardware (Cortex-A76/A78) is expected to be significantly faster;
Pi 5 and Jetson hardware benchmarks are pending.

### Documentation

Full specification in `docs/eviction.md` (550+ lines covering trait
interface, policies, feature extraction, build system, latency
benchmarks, FFI contracts, and test coverage).

---

## Cross-Cutting Summary

| Feature | QEMU | Pi 5 | Jetson | x86-64 |
|---------|------|------|--------|--------|
| SMP | Full | Boot OK, dispatch limited | Full (fixed Apr 15) | Full |
| Preemption | True (HW timer) | Cooperative | Cooperative | True (HW timer) |
| GPU inference | N/A | N/A (no bare-metal VC) | CPU-only; best GPU path | CPU-only; FWSEC done, SEC2 blocked |
| AI scheduler | Complete (MLP+PPO) | Complete | Complete | Complete |
| AI page eviction | Complete (CACHEUS) | Complete | Complete | Complete |

**Inference on all platforms is CPU-only today.** The AI scheduler and AI
page eviction subsystems are production-ready with real trained weights.
GPU-accelerated inference is realistic only on Jetson (ARM64 platform
shim needed, ~1-3 weeks) and x86-64 bare-metal (beyond capstone scope).

---

*Generated: 15 April 2026*
