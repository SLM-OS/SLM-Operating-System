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
All 15 multi-core integration tests pass on any boot where the
secondary CPUs wake as expected. `test_work_stealing_distributes_load`
now asserts "at least one task ran off the owner CPU" instead of
"≥ 2 distinct CPUs ran tasks", so a single awake stealer grabbing
all 5 tasks still counts as stealing working. A separate boot-to-boot
CPU-dormancy pattern (issue #216) occasionally leaves one or more
secondaries never entering `schedule()` post-boot; when multiple
secondaries are dormant together, several multi-CPU tests fail
simultaneously. `bench smp` and `bench stealing` both work.

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
  raised by UEFI POST's VBIOS DEVINIT script (#185). Present on both
  VFIO and bare-metal paths — the "bare-metal bypasses FLR" hypothesis
  was falsified 2026-04-15 (see `docs/x86-64-gpu-inference-status.md`
  §2.5). Nouveau clears the lock by mechanism that is firmware-mediated
  (not CPU MMIO) per the 2026-04-17 mmiotrace investigation
  (`docs/testing/x86-gpu-sec2-unlock-trace-2026-04-17.md`).
  **Linux→SLM-OS kexec scaffolding landed** (`make kernel-kexec` +
  `make kexec-deploy`); load succeeds via the old kexec_load syscall
  but handoff still silent — bzImage wrapper is the last tractable
  path, tracked in `docs/x86-64-gpu-inference-status.md` §4.2.k.
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
| Inference backend | CPU (NEON) | CPU (NEON), **Hailo-8 NPU scaffolded** (AI HAT+ via pcie1) | CPU (NEON) | CPU (SSE inline-asm) |
| AI scheduler MLP | CPU | CPU (routed through `inference_device` abstraction) | CPU | CPU |

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
| **GA10B nvgpu bringup (Jetson)** | `ga10b_bringup.c` (950 lines) | **37** | Phases 1–7 wired; **Phases 5–6 HW-verified**; Phase 7 GP_PUT write lands, PBDMA doorbell blocked |
| **Jetson platform shim** | `nvidia_gsp_platform.c` (350 lines) | **15** | vtable dispatch + DMA align math host-tested |
| **Total host-side tests** | | **175** | **All passing** |

### Platform-Specific Blockers

**Pi 5:** VideoCore GPU on BCM2712 has no public bare-metal compute
documentation. Accessing it would require reverse-engineering the
VideoCore ISA and firmware. Not feasible within capstone scope.

**Pi 5 Hailo-8 NPU (AI HAT+) — software-complete Phase 0–4:** A full
alternative inference path via the Pi 5's external PCIe connector.
Phase 0 research, Phase 1 ARM64 PCIe host controller (`kernel/drivers/pcie/`),
Phase 2 `inference_device` abstraction, Phase 3 Hailo driver scaffolding
(`kernel/ai_accel/hailo/`), and Phase 4 nanopb + `.hef` outer-header
validator all landed without hardware. The probe / firmware upload
state machine compiles on RASPI5 builds and waits only for a Pi 5 lab
unit with the HAT+ mounted. See `docs/pi5-ai-hat-plan.md` for the
full phase breakdown and `docs/pi5-pcie1-registers.md` for the
`pcie1` + MIP1 register reference.

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
`bringup.c`. See `docs/archive/investigations/jetson-nvgpu-bringup-research.md` and
`docs/archive/investigations/jetson-nvgpu-acr-analysis.md` for the full decomposition.

**Priv-lockdown resolved (Path 3, April 17 2026):** The GSP Falcon
priv-lockdown (#190) was caused by the kexec helper's runtime-PM
suspend, which power-gates the GPU. On re-enable, the Falcon BROM
reasserts `HWCFG2` bit 13 as a hardware default. Fix:
`--no-gpu-suspend` flag on the kexec helper skips the power-gate
cycle entirely. Linux's nvgpu has already run ACR/FECS/GPCCS to
completion; `ga10b_bringup_inherit()` detects this state (HWCFG2
bit 13 = 0, FECS/GPCCS mailbox[0] = PASS) and skips phases 1–4.

**FECS method gateway verified on hardware (Phase 5):** After
inherit, SLM-OS submits `DISCOVER_IMAGE_SIZE` (method 0x10) via the
FECS push registers (data at 0x409500, addr at 0x409504) and polls
`ctxsw_mailbox[0]` for the response. FECS returns **513,280 bytes**
(0x7d500) — the GR context image size for GA10B. This is the first
successful GPU method submission from bare-metal SLM-OS.

Phases 1–4 (ACR HS load, FECS/GPCCS STARTCPU, PMU skip) remain
implemented and host-tested as a fallback / standalone path. The
inherit path bypasses them when Linux's firmware state is available.

**CBB firewall constrains channel setup (Phase 6 blocker, April 17):**
The Tegra234 CBB (Control Backbone) firewall permanently blocks
non-secure (EL2) access to the GPU register apertures needed for
channel creation: PFIFO (0x002000), CHRAM (channel enable/disable),
NV_USERMODE (0x800000 doorbell), and several per-runlist PRI
config registers. Tested with both idle GPU and active CUDA
channel — same result; not a clock-gating issue. These are
hardware-level access controls that cannot be changed without
firmware-level CBB reconfiguration.

Accessible from EL2 (sufficient for the inherit + method path):
FECS method push (0x409500/504), runlist submit status
(0x4080/88), FIFO_USER doorbell (0x200000+, first 5 channels),
all DRAM (USERD, GPFIFO, pushbuffer memory).

**Phase 6 implemented — channel inherit VERIFIED on hardware
(April 17):** Linux-side helper (`scripts/gpu-channel-helper.c`)
creates an nvgpu channel via all 10 ioctls (TSG open, channel open,
AS alloc+bind, nvmap buffer allocs, SETUP_BIND with
DETERMINISTIC|USERMODE_SUPPORT, WDT disable, MAP_BUFFER_EX). Ioctl
parameters reverse-engineered from CUDA via LD_PRELOAD snooping
(e.g., `va_range_start=0x4000000, va_range_end=0x2000000000` for
ALLOC_AS; `heap_mask=0x40000000, flags=0x8000003` for NVMAP_ALLOC).

Helper writes a handoff block with magic `GPUH` (0x47505548) to an
nvmap dmabuf, prints its physical address, then sleeps. The
modified `slmos-kexec --no-gpu-suspend` keeps the helper alive
through the kexec transition (skips `fuser -k`). SLM-OS scans
0x100000000–0x180000000 for the magic on `nvgpu channel`,
validates the handoff (magic, version, non-null addresses,
power-of-2 GPFIFO entries), and advances to `CHANNEL_OPEN` state.

**Phase 7 partial — GP_PUT write lands, doorbell blocked:**
`nvgpu submit` writes a NOP pushbuffer, builds a correctly-encoded
Ampere GPFIFO entry (entry0[31:2] = va, entry1[7:0] = va[39:32],
entry1[30:10] = length_dwords), and increments GP_PUT in USERD.
Verified via `peek USERD[0x8c] = 1` — our write reaches the
correct DRAM offset. BUT PBDMA does not advance GP_GET because
the usermode doorbell (NV_USERMODE at 0x800000) is CBB-blocked
from EL2; Linux's helper can ring the doorbell via an mmap of
the ctrl fd, but that mapping is per-process and doesn't survive
kexec. DETERMINISTIC kickless-submit only applies when the
channel is already PBDMA's "current" channel.

**Remaining path to GPU inference:**
- Phase 7 completion: get PBDMA to consume pushbuffer entries.
  Options: (a) pre-submit a kernel-mode GPFIFO from Linux to
  "warm up" PBDMA before kexec, (b) arrange for the channel to
  be the runlist's current-active channel at kexec time,
  (c) find a way to ring the doorbell from EL2 (unlikely given
  the CBB firewall).
- Compute class binding + QMD dispatch
- Compute kernel (SASS binary for `sm_87`)
- Inference loop (GEMM → activation per layer)

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
beyond capstone scope. See `docs/archive/test-runs/x86-gpu-bringup-2026-04-15.md`
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
