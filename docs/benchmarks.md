# SLM-OS Performance Benchmarks

Performance measurements across all supported platforms.

**Date:** April 2026
**Methodology:** All benchmarks run via the `bench` shell command. Each measurement is the average of multiple iterations (count noted per test). Hardware counter (CNTPCT_EL0 on ARM64, RDTSC on x86) provides nanosecond-resolution timing.

---

## Platform Summary

| Platform | CPU | Cores | Clock | RAM | Status |
|----------|-----|-------|-------|-----|--------|
| QEMU virt | Cortex-A76 (emulated) | 4 | N/A | 1 GB | Primary dev |
| Raspberry Pi 5 | BCM2712 Cortex-A76 | 4 | 2.4 GHz | 4 GB | Hardware verified |
| Jetson Orin Nano | Cortex-A78AE | 6 | 1.5 GHz | 8 GB | Blocked (kexec RAS) |
| x86-64 (QEMU) | max | 4 | N/A | 256 MB | Experimental |

---

## Kernel Benchmarks

### Context Switch Latency

Measures round-trip time for two tasks yielding back and forth on the same CPU. 99 round-trips per measurement.

| Platform | Average | Rating |
|----------|---------|--------|
| Pi 5 | **1.858 us** | Excellent (< 10 us) |
| QEMU ARM64 | ~5-15 us | Good (emulation overhead) |

**Target:** < 10 us. **Achieved on Pi 5.**

### Interrupt Latency (Timer Tick Jitter)

Measures deviation from the expected 10 ms timer tick interval using 20 consecutive samples. The measurement captures the full IRQ path: hardware interrupt assertion, GIC acknowledge, exception vector entry, handler dispatch, timer reload, and counter read. This represents the worst-case overhead added to each timer tick.

| Platform | Min | Average | Max | Jitter (max-min) |
|----------|-----|---------|-----|-------------------|
| Pi 5 | 1.445 us | 1.705 us | 2.055 us | 0.6 us |
| Pi 5 (under load) | 1.482 us | 1.782 us | 3.074 us | 1.6 us |

Timer frequency: 54 MHz (BCM2712 system counter). The "under load" measurement was taken with active tasks and shell output. The sub-2us average confirms that the GIC-to-handler path has minimal overhead on Cortex-A76.

### IPC Latency

Message queue send + receive round-trip, 100 iterations.

| Platform | Total | Average |
|----------|-------|---------|
| Pi 5 | 13.248 us | **132 ns** |

### Shared Buffer Throughput

4 KB buffer, 1000 iterations, 64-byte stride.

| Platform | Write | Read |
|----------|-------|------|
| Pi 5 | **45.8 GB/s** | **48.1 GB/s** |

### Component Benchmarks (Pi 5)

Measured via Lua REPL with `slm.uptime()` timing. Includes UART output overhead.

| Operation | Latency | Notes |
|-----------|---------|-------|
| Component load (`component_run`) | **3 ms** | Register + task create + add to scheduler |
| Component hot-swap | **11 ms** | Unregister old + load new + transfer subscriptions |
| Message publish + process | **6.39 ms** | Publish → route → subscriber receive → process → yield (100-msg avg, includes UART print per message) |
| Message publish (raw) | **~0.2 ms** | Estimated without UART overhead (IPC round-trip is 132 ns) |

### End-to-End Component Pipeline

Measures time from message publish through component processing to result.

| Pipeline | Pi 5 |
|----------|------|
| Publish → sensor_monitor → threshold alert | **7 ms** |
| Publish → route → process → yield (per message, 100-msg avg) | **6.39 ms** |

Includes UART output from the component (serial at 115200 baud accounts for ~4ms of the latency). Without UART output, the raw pipeline would be ~2-3 ms.

### Scheduler Overhead

| Metric | Pi 5 |
|--------|------|
| schedule() call | **2 µs** (measured via 1000 yields) |
| Heuristic policy decision | ~2 µs (included in schedule) |
| AI MLP policy decision | **41.9 µs** (state extraction + inference) |

### Scheduler Throughput

| Platform | Context Switches/sec | Active Tasks |
|----------|---------------------|--------------|
| Pi 5 (fresh boot, 3 tasks) | **69,353** | 3 |
| Pi 5 (after demo, 8 tasks) | **40,368** | 8 |

### Deadline-Boosted Dispatch

Task dispatch latency with deadline boost (priority escalation for urgent tasks).

| Platform | Dispatch Latency |
|----------|-----------------|
| Pi 5 | ~18 us |

### Memory Usage (Pi 5, Runtime)

| Resource | Size |
|----------|------|
| Total RAM | 4,188,256 KB (4 GB) |
| Kernel used | 395,652 KB (387 MB) |
| Free | 3,792,604 KB (3.6 GB) |
| Model weight pool | 256 MB (128 x 2 MB blocks) |
| Model workspace pool | 128 MB (64 x 2 MB blocks) |
| Rust heap | 64 MB (Pi 5; per-platform via `RUST_HEAP_MB` in `<config.h>`) |
| RAM disk (LittleFS) | 1 MB |

---

## Boot Performance

| Platform | Total (power to shell) | Kernel init | Firmware |
|----------|----------------------|-------------|----------|
| Pi 5 | 8.5 s | ~1.6 s | ~5-7 s |
| QEMU ARM64 | ~1 s | ~1 s | N/A |

### Boot Phase Breakdown (Pi 5)

Estimated from serial output timing at 115200 baud. Total kernel init ~1.6s (serial capture), ~3.5s (power-on including firmware).

| Phase | Estimated Time | Notes |
|-------|---------------|-------|
| DTB parse + PMM init | ~100 ms | Buddy allocator setup for 4 GB |
| VMM/MMU enable | ~50 ms | Page table construction (2054 blocks) |
| VMM validation tests | ~50 ms | 6 tests — skip in production |
| GIC + Timer init | ~20 ms | |
| SMP boot (3 secondary CPUs) | ~600 ms | Sequential PSCI CPU_ON with handshake |
| Spinlock + SMP validation | ~100 ms | 16 tests — skip in production |
| Filesystem (ramdisk + LittleFS) | ~200 ms | Format, mount, write 38 help files + demo |
| Rust runtime init | ~50 ms | Heap + FFI validation |
| Component + Model + GPU init | ~100 ms | Pool allocation (384 MB logical) |
| Scheduler + Shell start | ~50 ms | |

### Optimization Opportunities

| Optimization | Estimated Savings |
|-------------|-------------------|
| Skip boot-time validation tests | ~150 ms |
| Parallel SMP boot (simultaneous PSCI) | ~400 ms |
| Lazy help/demo file writing | ~100 ms |
| **Total potential** | **~650 ms** |

**Target:** < 2 s kernel init. Current: ~1.6 s on Pi 5 (measured from first serial output to shell prompt). The 3.5s figure from earlier measurements included serial capture connection latency. With the optimizations above, sub-1s kernel init is achievable.

---

## Binary Size

| Platform | Binary (.bin) | ELF text | ELF data | ELF bss |
|----------|--------------|----------|----------|---------|
| QEMU ARM64 | 973 KB | 912 KB | 62 KB | 1.9 MB |
| Pi 5 | 824 KB | 762 KB | 61 KB | 2.6 MB |
| Jetson | 893 KB | 818 KB | 61 KB | 3.6 MB |
| x86-64 | 610 KB | 544 KB | 59 KB | 5.1 MB |

BSS varies by platform due to: task table size (NC vs BSS), per-CPU data, platform-specific driver buffers. The kernel binary includes: C kernel, Lua 5.4 interpreter, embedded help files, and demo script.

---

## Platform Comparison (QEMU)

Emulated benchmarks on the same host. QEMU ARM64 emulates Cortex-A76; QEMU x86-64 uses host CPU passthrough. Numbers reflect emulation overhead, not native hardware performance.

| Metric | QEMU ARM64 | QEMU x86-64 | Pi 5 (native) | Jetson (native) |
|--------|-----------|-------------|---------------|-----------------|
| Context switch | ~807 ns | ~1,332 ns | **1,858 ns** | **3,601 ns** |
| IPC round-trip | ~1,709 ns | ~761 ns | **132 ns** | **60 ns** |
| Buffer write | 9.7 GB/s | 21.0 GB/s | **45.8 GB/s** | **35.1 GB/s** |
| Buffer read | 8.2 GB/s | 14.1 GB/s | **48.1 GB/s** | **68.8 GB/s** |
| IRQ latency | — | — | **1.7 us** | **3.7 us** |
| MNIST inference | — | — | **1,092 us** | **746 us** |
| Binary size | 973 KB | 610 KB | 824 KB | 893 KB |
| CPUs | 4 | 4 | 4 | 6 |
| RAM | 1 GB | 256 MB | 4 GB | 8 GB |

QEMU numbers vary between runs due to host load and emulation non-determinism. Pi 5 native numbers are the authoritative measurements for capstone evaluation.

---

## Memory Usage

### Model Memory Pools

| Pool | Block Size | Blocks | Total |
|------|-----------|--------|-------|
| Weight (read-only) | 2 MB | 128 | 256 MB |
| Workspace (scratch) | 2 MB | 64 | 128 MB |
| **Total** | | **192** | **384 MB** |

### Kernel Memory (Pi 5)

| Region | Size |
|--------|------|
| Kernel code + data | ~1 MB |
| PMM heap | 4,076 MB (of 4 GB RAM) |
| NC shared memory | 2 MB |
| Rust heap | 64 MB (Pi 5; per-platform via `RUST_HEAP_MB` in `<config.h>`) |
| Model pools | 384 MB |
| RAM disk (LittleFS) | 1 MB |
| Task stacks | 16 KB each (max 32 tasks) |

---

## AI Scheduler Inference

Real trained MLP and PPO weights from the Plan A export pipeline (108→256→256→128→42 architecture, ~3 MB per model).

| Metric | Pi 5 (native) | Target |
|--------|--------------|--------|
| MLP inference latency | **41.9 us** | < 50 us |
| State extraction | 108 dimensions | — |
| Action space | 42 actions (7 cores x 3 priority x 2 preempt) | — |
| Weight size (MLP) | 3.0 MB | — |
| Weight size (PPO) | 3.0 MB | — |
| Kernel binary with AI | 1.9 MB (vs 824 KB without) | — |

**Target achieved:** 41.9 us < 50 us on Cortex-A76 @ 2.4 GHz.

### AI vs Heuristic Scheduler Comparison

| Metric | Heuristic | AI MLP | Ratio |
|--------|-----------|--------|-------|
| Decision latency (Pi 5) | ~2 us | 41.9 us | 21x slower |
| Decision latency (QEMU) | ~2 us | ~1,000 us | 500x slower (emulation) |
| Fallback rate | N/A | 0-50% (depends on isolation) | — |
| CPU assignment | Round-robin | Model-driven (trained on workload patterns) | — |

The AI scheduler adds ~40 µs overhead per scheduling decision on Pi 5 hardware. This is acceptable for inference-heavy workloads where decisions happen infrequently (component dispatch, not per-tick). The heuristic policy remains the default for latency-sensitive cooperative scheduling.

**When to use AI scheduling:**
- Workloads with heterogeneous task requirements (different priority/preemption needs)
- Systems where optimal CPU placement matters more than scheduling overhead
- Evaluation of learned scheduling policies against heuristic baselines

**When to use heuristic scheduling:**
- Latency-sensitive cooperative workloads
- Systems with frequent task creation/destruction
- Benchmarking and debugging (deterministic behavior)

The latency includes: FP context save, state vector extraction (108 floats from kernel data), 4-layer forward pass (NEON-optimized matvec), action decode and validation, FP context restore. Measured via `test_ai_inference_latency` (100 iterations, average reported by the test).

---

## Cross-Platform Inference Latency

Unified view of MNIST classify latency (25 classes, 5,998 parameters)
across every SLM-OS target. Populated by C3 on x86-64 and by the
Jetson plan's G3/G4 on Jetson; Pi 5 numbers carried forward from
earlier measurements.

| Model | Platform | Dtype | Latency (avg) | Backend | Notes |
|-------|----------|-------|---------------|---------|-------|
| MNIST | Raspberry Pi 5 (Cortex-A76 @ 2.4 GHz) | FP32 | **1.09 ms** | Rust + NEON (4-wide `vfmaq_f32`) | Measured on hardware |
| MNIST | Jetson Orin Nano (Cortex-A78AE @ 1.5 GHz) | FP32 | **0.746 ms** | Rust + NEON | Measured on hardware |
| MNIST | test-pc (i7-6700 @ 3.4 GHz), scalar baseline | FP32 | *TBD* | Rust scalar fallback | Pre-C1 measurement; to be captured before removing this row |
| MNIST | test-pc (i7-6700 @ 3.4 GHz), SSE-asm | FP32 | *TBD* | C SSE2 kernel (`kernel/arch/x86_64/sse_kernels.c`) | Post-C1 measurement on real hardware; QEMU numbers also published |
| MNIST | Jetson Linux (Cortex-A78AE) — ONNX Runtime reference | FP32 | 0.117 ms | OpenBLAS + multi-threaded | Production runtime, not SLM-OS |

The two "test-pc" rows will be filled in by `bench infer` runs on
the H610M dev PC once Phase C is deployed via `make x86-disk`. The
scalar baseline row serves as the reference for how much speedup
the SSE kernels delivered — historical before/after data that
future regressions can be judged against. Pre-C1, the x86-64 CPU
path is pure scalar (no SIMD) because of the Rust `x86_64-unknown-none`
toolchain limitation documented in GitHub #72; C1 works around it by
putting the SIMD kernels in a C translation unit built with `-msse`.

### Dispatch selection

| Platform | SIMD backend | Dispatch symbol |
|----------|--------------|-----------------|
| aarch64  | NEON         | `ops.rs` inline `core::arch::aarch64::*` (`vmaxq_f32`, `vfmaq_f32`, …) |
| x86_64   | SSE2         | C kernels via `extern "C"` (`slm_sse_relu_f32` etc.) compiled with `-msse -msse2` |
| Other    | Scalar       | Plain Rust fallback |

The dispatch skeleton (three `#[cfg(target_arch = ...)]` arms per
kernel) is shared — see `runtime/src/inference/ops.rs`.

## ONNX Model Inference (MNIST)

Real ONNX model inference using the built-in MNIST digit classifier (26 KB, 12 operators, 5,998 parameters). Model loaded via `rust_model_load_builtin_mnist()`, inference via `rust_infer_classify()`.

| Metric | Pi 5 (native) |
|--------|--------------|
| Model size | 26 KB (23,982 bytes weights) |
| Parameters | 5,998 |
| Operator nodes | 12 (Conv2D, MaxPool, Gemm, Relu, Reshape, Add, Softmax) |
| Inference latency (min) | **1,092 us** |
| Inference latency (avg) | **1,092 us** |
| Inference latency (max) | **1,094 us** |
| Throughput | **915 inferences/sec** |
| Accuracy | Class 5 for zero input (matches ONNX Runtime reference) |

### Latency Distribution (1000 iterations)

| Percentile | Latency |
|------------|---------|
| p50 | 1,092 us |
| p95 | 1,092 us |
| p99 | 1,092 us |
| p100 (max) | 1,100 us |

Every sample in 1000 iterations measured 1,092 µs except one outlier at 1,100 µs. The 8 µs max jitter demonstrates deterministic inference suitable for real-time edge deployment.

### Per-operator Profile (#56)

The per-op profiling harness in `runtime/src/inference/engine.rs` wraps every `execute_node` dispatch in CNTPCT timestamps and accumulates per-`OpType` counts and latencies. Enable with `model profile on`, run the workload, then `model profile show` (clear with `model profile reset`). Overhead is negligible: 200-iteration MNIST bench on pi-5-2 measured 3,011 µs/inference both profile-on and profile-off (run-to-run jitter swamps the harness cost).

Baseline before #56 micro-kernel work (Pi 5, BUILD_TYPE=Release, pi-5-2, 200 iterations, MNIST). Per-op figures are reported in microseconds; "calls" is the count of `execute_node` invocations across the run.

| Op | Calls | Avg | Min | Max | Notes |
|----|-------|-----|-----|-----|-------|
| Conv | 400 | **1,433 µs** | 1,048 µs | 1,818 µs | Dominant — two conv layers × 200 iters |
| MatMul | 200 | 22 µs | 22 µs | 24 µs | Output Gemm decomposes to one MatMul |
| Relu | 400 | 19 µs | 7 µs | 31 µs | |
| MaxPool | 400 | 15 µs | 6 µs | 24 µs | |
| Add | 600 | 9 µs | 2 µs | 21 µs | Conv-bias + Gemm-bias adds |
| Reshape | 400 | 1 µs | 1 µs | 2 µs | |

Total dispatched-op time per inference (`Σ avg × calls / iters`): ~2,985 µs, accounting for ~99% of the 3,011 µs bench latency. The remaining ~26 µs is engine-side overhead (binding lookups, output copy, weight lease, atomic stats).

`Conv` consumes ~95% of inference latency — the matmul micro-kernel improvements in #56 PR 2 / PR 3 target the im2col→matmul inner loop inside `conv2d`, so the speedup on this column is what the post-optimization rows track. Note the current Pi 5 baseline is **3.01 ms/inference**, higher than the 1.09 ms recorded in the cross-platform table above (which dates from an earlier build); the #56 PRs land before/after numbers against the current measurement so the delta is apples-to-apples.

The Jetson hardware baseline will be captured during PR 2 deployment once the slmos-kexec deploy path is in this agent's reach.

### Per-operator Profile with PMU columns (#60 / #871)

`model profile show` is extended in #871 with three PMU-derived columns
appended to the latency columns above:

- **L1D%** — L1 data cache miss rate over the bucket: `cache_misses /
  cache_references × 100`. Useful for telling memory-bound ops apart
  from compute-bound ones.
- **IPC** — instructions retired per cycle: `instructions_retired /
  cycles`. The compute-density indicator.
- **mispred%** — branch mispredictions per 100 retired instructions:
  `branch_mispredictions / instructions_retired × 100`. Surfaces
  control-flow-heavy ops.

PMU columns are blank (`-`) on QEMU TCG (event counters not modelled)
and on platforms where the PMU has not been enabled on the calling
CPU.

#### Captured PMU profile — pi-5-2 + jetson-nano-1 (2026-05-15)

Captured via `model profile on; model bench mnist {200|100}; model profile show`
after the kernel boots and PMU init completes on every CPU. The
per-op latency columns mirror the pre-#871 table above; the new
columns are L1D miss-rate %, IPC (instructions retired per cycle),
and branch mispredictions per 100 retired instructions.

##### Pi 5 (Cortex-A76, BCM2712 @ ~2.4 GHz, AI_SCHED=ON, 200 iter)

| Op | Calls | Avg µs | Min µs | Max µs | L1D% | IPC | Mispred% |
|----|------:|-------:|-------:|-------:|-----:|----:|---------:|
| Conv | 400 | 1,431 | 1,046 | 1,816 | <1 | **2.46** | <1 |
| MatMul | 200 | 23 | 22 | 24 | <1 | 2.43 | <1 |
| Relu | 400 | 19 | 7 | 31 | <1 | 2.12 | <1 |
| MaxPool | 400 | 15 | 6 | 24 | <1 | 2.75 | <1 |
| Add | 600 | 9 | 2 | 21 | <1 | 1.75 | <1 |
| Reshape | 400 | 1 | 1 | 2 | <1 | 2.09 | <1 |

Total/inf: 3,008 µs.

##### Jetson Orin Nano (Cortex-A78AE @ 1.5 GHz, 100 iter)

| Op | Calls | Avg µs | Min µs | Max µs | L1D% | IPC | Mispred% |
|----|------:|-------:|-------:|-------:|-----:|----:|---------:|
| Conv | 200 | 1,990 | 1,450 | 2,533 | <1 | **3.63** | <1 |
| MatMul | 100 | 32 | 31 | 34 | <1 | 3.72 | <1 |
| Relu | 200 | 24 | 9 | 39 | <1 | 3.49 | <1 |
| MaxPool | 200 | 17 | 7 | 31 | <1 | 5.00 | <1 |
| Add | 300 | 17 | 2 | 42 | <1 | 1.92 | <1 |
| Reshape | 200 | 2 | 1 | 3 | <1 | 3.67 | <1 |

Total/inf: 4,181 µs.

Integer division renders L1D% / Mispred% as 0 when the true rate is
below 1 %; both columns are <1 % across every op on both platforms —
MNIST weights + working set fit comfortably in L1D after warmup.

##### Interpretation

**Where the latency goes is unambiguous: Conv2D dominates at ~95% of
inference on both platforms.** L1D miss rate stays below 1% across
every op on both Cortex-A76 and Cortex-A78AE — MNIST is small enough
that the working set fits in L1D after warmup, so inference is
**compute-bound, not memory-bound**. The `cache_pressure` feature
fed into the AI scheduler (#872) therefore reads low under MNIST and
will only spike under larger models whose working set spills L2.

The IPC delta between the two cores tells the second-order story:
Cortex-A78AE retires 1.5× more instructions per cycle than
Cortex-A76 on Conv2D (3.63 vs 2.46), but Pi 5's higher clock
(2.4 vs 1.5 GHz) more than compensates — Pi 5 finishes Conv2D in
1,431 µs vs Jetson's 1,990 µs. A78AE also accumulates ~2× more
backend-stall cycles than A76 on the same loop (see
`docs/pmu-bringup.md` reference output) — its wider issue width
isn't fully utilised by the current matmul kernel, leaving headroom
for future per-microarch tuning.

**Honest framing on "before/after #56".** The per-op profile harness
*itself* landed under issue #56 (PR #849), so true pre-#56 PMU
numbers don't exist on this codebase — we only have post-#56 PMU
data. What the table demonstrates is that whatever #56's improvements
were, the workload remains firmly compute-bound on both platforms;
further latency gains from this point will come from either a wider
matmul micro-kernel (push IPC higher) or moving Conv2D off the CPU
(Hailo on Pi 5, GA10B GPU on Jetson).

### Model Memory Utilization

| Model | Weight Blocks | Workspace Blocks | Actual Weights |
|-------|--------------|-----------------|----------------|
| MNIST | 1 / 128 (2 MB allocated, 24 KB used) | 1 / 64 (2 MB allocated) | 23,982 bytes |

The 2 MB block granularity means small models waste most of their allocated block. For production deployment with many small models, a sub-block allocator within the weight pool would improve density.

---

## Test Suite Performance

| Platform | Tests | Pass | Fail | Time |
|----------|-------|------|------|------|
| QEMU ARM64 | 620+ | All | 0 | ~15 s |
| Pi 5 | 620+ | 615+ | 5 | ~24 s |
| x86-64 | 434 | 426 | 8 | ~10 s |

Pi 5 failures: 5 multi-core integration tests (secondary CPU preemption limitation).
x86-64 failures: 8 platform-specific tests (PIC, PCI, GPU commands not implemented).

---

## Linux Comparison

Measured on Jetson Orin Nano running Linux 5.15.148-tegra (6x Cortex-A78AE @ 1.5 GHz, 8 GB). Note: different hardware than Pi 5, but both are ARM64. Linux numbers represent a production JetPack deployment, not a minimal kernel.

| Metric | SLM-OS (Pi 5) | SLM-OS (Jetson) | Linux (Jetson) | SLM-OS vs Linux |
|--------|---------------|-----------------|---------------|-----------------|
| Context switch | **1.858 us** | **3.601 us** | 13.6 us (pipe) | **3.8x faster** |
| IPC round-trip | **132 ns** | **60 ns** | 23.7 us (UDS) | **395x faster** |
| Boot to shell | **~1.6 s** | ~3 s | 20.8 s | **7x faster** |
| Kernel binary | **824 KB** | **893 KB** | 41.1 MB | **46x smaller** |
| MNIST inference | **1.09 ms** | **0.746 ms** | 0.117 ms (ONNX RT) | 6.4x slower* |
| CPUs | 4 | 6 | 6 | Same |

*\* ONNX Runtime uses optimized BLAS (OpenBLAS/LAPACK) with cache-optimized tiling and multi-threaded matmul. SLM-OS uses a single-threaded NEON matmul without tiling. The inference engine is functionally correct and deterministic (8 µs jitter), but not performance-competitive with production inference runtimes. Optimization is documented in docs/future-work.md.*

### Why SLM-OS is Faster (except inference)

- **No syscall overhead:** function calls replace trap-based system calls
- **No virtual memory TLB faults:** identity-mapped 2 MB blocks
- **No process isolation:** all code runs in kernel mode (EL1)
- **No scheduler complexity:** 8-priority cooperative + deadline boost vs CFS
- **Purpose-built IPC:** zero-copy message queues in shared address space vs kernel-mediated pipes/sockets
- **No driver framework:** direct register access vs driver model layers

### Tradeoffs

SLM-OS achieves these numbers by trading:
- Process isolation (all components share address space)
- POSIX compatibility (custom API)
- Hardware driver ecosystem (manual driver development)
- Multi-user support (single-purpose)

These tradeoffs are acceptable for dedicated AI edge devices where the OS runs a single, known workload.

---

*SLM-OS measured: April 2026 on Raspberry Pi 5 (BCM2712, 4x Cortex-A76 @ 2.4 GHz, 4 GB LPDDR4X)*
*Linux measured: April 2026 on Jetson Orin Nano (6x Cortex-A78AE @ 1.5 GHz, 8 GB, JetPack 6 / Linux 5.15.148-tegra)*
