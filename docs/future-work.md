# SLM-OS Future Work

Post-capstone development roadmap. These items were identified during Phases 1-6 and explicitly deferred to maintain focus on the core capstone deliverables.

---

## 1. GPU Compute

**Current state:** GPU memory integration works (cache coherency, alloc/free). GPU probe detects NVIDIA hardware. Stub driver provides API on QEMU. GSP firmware loading is the primary blocker for actual compute.

### GSP Firmware Loading (4-6 weeks)
- Load GSP RISC-V firmware from filesystem
- Initialize GSP mailbox communication
- Implement GSP boot sequence (documented in `docs/nvidia-gsp.md`)
- Handle firmware versioning and compatibility

### Compute Kernel Submission (6-8 weeks)
- Submit compute commands via GSP channel
- Implement GPU-side MatMul for large matrices (> 4096 elements)
- Memory mapping for GPU-accessible model weights
- Fence/sync primitives for CPU-GPU coordination

### TensorRT Integration
- Requires working GSP + CUDA runtime
- Pre-compiled TensorRT engines for supported models
- Fallback to CPU inference when GPU unavailable

**Prerequisite:** GSP firmware documentation (partially reverse-engineered from nouveau). See `docs/nvidia-gsp.md` for current understanding.

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

*Created: April 2026*
*Consolidated from: FUTURE.md, TODO.md deferred items, Phase 5 deferred list*
