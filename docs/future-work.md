# SLM-OS Future Work

Post-capstone development roadmap. These items were identified during Phases 1-6 and explicitly deferred to maintain focus on the core capstone deliverables.

---

## 1. GPU Compute

**Current state (2026-04-16):** GPU memory integration works (cache coherency, alloc/free). GPU probe detects NVIDIA hardware. **FWSEC-FRTS (E3.4) succeeds on retail Ampere on test-pc** (3/3 runs, WPR2 populated). Downstream Phase E steps are blocked by a hardware-security boundary on the VFIO-userspace path (SEC2 priv-lock, issue #185). See `docs/x86-64-gpu-inference-status.md` for the live handoff and ranked paths forward (Jetson, bare-metal SLM-OS).

- **E1** — firmware embedding via `.incbin`: ✅ shipped.
- **E2** — shared VBIOS BIT-table parser (`kernel/gpu/nvidia/nvidia_vbios.{h,c}`): ✅ shipped, validated on real GTX 1070 + RTX 3050.
- **E2.5** — FWSEC ucode extraction from VBIOS: ✅ shipped + closed (#143).
- **E3.1** — Falcon v4 register map + DMA/halt/start primitives: ✅ shipped, hardware-validated.
- **E3.2** — VFIO IOMMU + IOMMU-mapped DMA: ✅ shipped, hardware-validated.
- **E3.3** — NVIDIA HS firmware container parser (nvfw_bin_hdr): ✅ shipped + validated against real R535 booter_load.bin.
- **E3.4** — FWSEC-FRTS bringup: ✅ **succeeds on real RTX 3050** (3/3 runs, 2026-04-16). Required two fixes on top of the scaffolding: poll BSI DEVINIT recovery after vfio FLR, and skip `falcon_reset` when the Falcon is already idle.
- **Linux userspace harness** (`host-tools/gsp-harness/`): ✅ shipped with diagnostic actions (`--probe`, `--vbios`, `--falcons`, `--dma-test`, `--fwsec-frts`, `--fwsec-sb`, `--fwsec-trace`, `--check-devinit`, `--sec2-plm-scan`, `--booter-load`, `--riscv-start`, `--bringup`).
- **E3.4.d** — Booter Load on SEC2: 🚫 **blocked on x86-64 VFIO** by SEC2 priv-lock (#185). 865 / 1024 SEC2 register offsets are priv-locked post-BSI-DEVINIT. The code is shipped and unit-tested but cannot run against this hardware under VFIO.
- **E3.4.e / E4 / E5 / E6** — RISC-V startup, RPC ring completion, compute engine, full pipeline: transitively blocked by E3.4.d on this platform.
- **Jetson GA10B / bare-metal paths:** the portable bringup code (VBIOS parse, Falcon driver, DMEMMAPPER patcher, sig-index, RPC skeleton) transfers directly and does not suffer the priv-lock. See `docs/x86-64-gpu-inference-status.md` §4.

### TensorRT Integration (still post-capstone)
- Requires working GSP + CUDA runtime
- Pre-compiled TensorRT engines for supported models
- Fallback to CPU inference when GPU unavailable

**Plan:** `docs/archive/plans/x86-64-capstone-gap-closure-plan.md` §E.

---

## 2. Inference Optimization

### FP16/INT8 Quantization (2-3 weeks)
- Half-precision (FP16) for ARM64 NEON and x86 F16C
- INT8 quantization with calibration dataset
- Per-layer mixed precision (FP32 accumulation, FP16/INT8 operands)
- Accuracy validation against FP32 reference

### Dynamic Batching (2-3 weeks)
- Collect inference requests up to configurable batch size or timeout
- Execute batched inference (single MatMul with larger batch dimension)
- Distribute results back to individual requesters
- Measure throughput improvement vs single requests

### SIMD Optimization
- Explicit NEON intrinsics for ARM64 (partially done for MatMul)
- SSE/AVX for x86-64 AI scheduler (done), extend to inference engine
- Cache-friendly tiling (64x64 or 128x128 blocks) for large matrices
- Prefetching for weight access patterns

---

## 3. Security

### Secure Boot Chain (4-6 weeks)
- Kernel image signature verification at boot
- Integration with Jetson fuse-based root of trust
- Component signature verification before loading
- Key management for model encryption keys

### Encrypted Model Storage (2-3 weeks)
- AES-256 decryption for model files at load time
- Hardware-accelerated decryption via Jetson SE (Security Engine)
- Per-model encryption keys with secure storage
- Zero model memory on unload

### Component Sandboxing
- Per-component address spaces via TTBR0_EL1 (ARM64) / CR3 (x86)
- Capability-based access control for IPC
- Resource limits (memory, CPU time) per component
- Automatic component restart on fault

**Note:** EL0/Ring3 transition is partially implemented (syscall dispatch works, EL0 execution deferred due to QEMU MMU issue with AP[1]=1). See `docs/component-isolation.md`.

---

## 4. Distributed Operation

### Network-Transparent IPC (4-6 weeks)
- Extend message router to forward across network boundaries
- Topic-based routing with node discovery
- Serialization protocol for cross-node messages
- Transparent to components (same publish/subscribe API)

### Multi-Board Pipeline (6-8 weeks)
- Split large models across multiple boards (pipeline parallelism)
- Intermediate tensor transfer via network
- Synchronization barriers for pipeline stages
- Failover: migrate pipeline stages when a node fails

**Prerequisite:** Networking stack (lwIP integration complete for QEMU; Jetson EQOS driver needed for hardware).

---

## 5. Power Management

### DVFS Integration (2-3 weeks)
- CPU frequency scaling via platform-specific registers
- Jetson: BPMP mailbox for clock management
- Pi 5: VideoCore mailbox for frequency control
- Profile-guided frequency selection

### Inference-Aware Power Modes (3-4 weeks)
- High-performance mode during active inference
- Low-power idle when no inference requests pending
- GPU power gating when not in use
- Thermal throttling integration (temperature sensor monitoring)

---

## 6. Developer Ecosystem

### Component Development SDK (4-6 weeks)
- Project template (Cargo/CMake) for new components
- Component packaging tool (bundle code + manifest + models)
- Emulator mode: run components on host for testing
- Documentation generator for component APIs

### Debugging and Profiling
- GDB stub for kernel debugging via serial
- Trace framework: record scheduler/IPC events for analysis
- Performance profiler: CPU time, memory, inference latency per component
- Crash dump analysis tools

### Component Marketplace (8-12 weeks)
- Component registry with metadata, versioning, dependencies
- Signed component distribution
- CLI tool for `slm install <component>`
- Update mechanism with rollback support

---

## 7. Scheduler Enhancements

### Secondary CPU Timer Preemption (1-2 weeks)
- Deferred scheduling via ELR_EL1 trampoline (design documented in `docs/pi5-secondary-cpu-preemption.md`)
- Enables true preemptive multi-core scheduling on Pi 5
- Requires preemption-safe test suite updates

### Real AI Scheduler Weights
- Integrate Plan A exported weights (MLP, PPO models)
- Validate inference latency < 50 us on Cortex-A78
- Compare scheduling quality (AI vs heuristic) under realistic workloads

### XGBoost Inference (2-3 weeks)
- Tree traversal for cascaded classification
- Derived feature computation
- Core assignment, priority, and preemption decisions

### Lock-Free Task Stealing (1-2 weeks)
- Work-stealing between per-CPU run queues
- Reduces load imbalance without global locking
- Profile first to confirm need

### Rate Monotonic Scheduling
- Formal real-time scheduling policy option
- Priority ceiling protocol for mutex
- Worst-case execution time (WCET) analysis tooling

### pi_mutex Sleep Queue (1-2 weeks)
- Replace the spin-wait loop in `pi_mutex_lock` with a wait-queue model: waiter blocks (TASK_BLOCKED), owner's `pi_mutex_unlock` wakes one waiter
- Removes the single-CPU contended livelock documented in `docs/scheduler.md` ("Known Limitation — Single-CPU Contended Priority Inheritance")
- Also eliminates the need for `DAIF`-aware timer preemption to make progress on contended mutexes
- Blocked by timer-driven wakeup on secondary CPUs on Pi 5 (see "Secondary CPU Timer Preemption" above); once that lands, the sleep queue is a straightforward addition

### ✅ Timer-Driven Busy-Wait Helper (small) — Done (#94)
- `timer_busy_wait_us(us)` added as inline in `kernel/include/timer.h` — uses CNTPCT_EL0 (ARM64) / TSC (x86-64)
- Both volatile delay loops in `kernel/sched/smp.c` replaced; `git grep "for (volatile int d" kernel/sched/` returns no hits
- Portable across QEMU / Pi 5 / Jetson / x86-64

---

## Effort Summary

| Category | Items | Total Effort |
|----------|-------|-------------|
| GPU Compute | 3 items | 10-14 weeks |
| Inference | 3 items | 6-8 weeks |
| Security | 3 items | 8-11 weeks |
| Distributed | 2 items | 10-14 weeks |
| Power | 2 items | 5-7 weeks |
| Ecosystem | 3 items | 16-24 weeks |
| Scheduler | 5 items | 6-10 weeks |
| **Total** | **21 items** | **61-88 weeks** |

---

## Priority Recommendations

**High priority (immediate post-capstone):**
1. Secondary CPU preemption — unblocks multi-core integration tests
2. Real AI scheduler weights — validates the AI-first thesis
3. FP16 quantization — practical inference speedup

**Medium priority (next semester):**
4. Secure boot + encrypted models — production deployment requirement
5. Component sandboxing — required for untrusted components
6. Network-transparent IPC — enables distributed demos

**Lower priority (future work):**
7. GPU compute — high effort, requires NVIDIA cooperation
8. Developer SDK — needed for external adoption
9. Power management — useful for battery deployments

---

## 22. Profiler Integration (2-3 weeks)

**Current state:** Basic timing via hardware counters (`slm_get_time_ns()`, `timer_get_count()`). Inference statistics tracked (total/min/max/avg latency). No per-operator profiling or system-wide tracing.

### Per-Operator Profiling
- Instrument each ONNX operator dispatch with start/end timestamps
- Track per-operator cumulative time, call count, and percentage of total inference
- Report via `model stats` shell command
- Identify bottleneck operators (likely MatMul/Gemm for MNIST)

### System-Wide Tracing
- Lightweight trace buffer (ring buffer in NC memory for cross-CPU visibility)
- Trace points: context switch, IPC send/receive, model load, inference start/end
- Export via serial as structured text (parseable by host-side tools)
- Minimal overhead target: < 100 ns per trace point

### Performance Counters (ARM PMU)
- Read ARM Performance Monitor Unit counters (PMCCNTR_EL0, PMEVCNTR_EL0)
- Track: cache misses, branch mispredictions, instructions retired
- Correlate with inference phases for optimization guidance
- x86-64: RDPMC for equivalent counters

**Prerequisite:** None — builds on existing timing infrastructure. Useful for optimizing inference hot paths (MatMul tiling, memory access patterns).

---

*Created: April 2026*
*Consolidated from: FUTURE.md, TODO.md deferred items, Phase 5 deferred list*
