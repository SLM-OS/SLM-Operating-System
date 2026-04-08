# Tutorial: Performance Tuning

Guide for measuring and optimizing SLM-OS inference and system performance.

**Prerequisites:** A running SLM-OS instance with at least one ONNX model loaded. See `docs/tutorials/models.md` for model loading instructions.

---

## Overview

SLM-OS provides built-in benchmarking tools for both inference latency and core OS operations (context switches, IPC, interrupt latency). These tools use the ARM generic timer (`CNTPCT_EL0`) for nanosecond-precision measurements.

Performance tuning in SLM-OS involves three areas:

1. **Inference latency** -- how fast the CPU executes a model's operator graph
2. **System overhead** -- context switch time, IPC latency, interrupt jitter
3. **Memory usage** -- stack consumption, weight pool utilization, workspace sizing

---

## Inference Benchmarking

### model bench

The `model bench` command runs multiple inference iterations on a loaded model and reports timing statistics:

```
SLM-OS> model bench mnist 10
```

This runs 10 iterations and reports:

- **Min latency:** Fastest single inference
- **Max latency:** Slowest single inference (often the first, due to cache cold-start)
- **Average latency:** Mean across all iterations

A higher iteration count produces more stable averages. For reliable measurements, use at least 10 iterations.

### model stats

The `model stats` command displays cumulative inference statistics across all calls since boot:

```
SLM-OS> model stats
```

Output includes:

| Metric | Description |
|--------|-------------|
| Total inferences | Number of successful inference calls |
| Total time | Cumulative time spent in inference |
| Min time | Fastest individual inference |
| Max time | Slowest individual inference |
| Last time | Most recent inference latency |
| Errors | Number of failed inference attempts |

Statistics are tracked via atomic counters in the Rust inference engine and accumulate across all callers (shell commands, components, etc.). They persist until reboot.

### Interpreting Results

- **First-run penalty:** The first inference after loading a model is typically slower due to instruction cache and data cache misses. Subsequent runs benefit from warmed caches.
- **Workspace reuse:** The bump allocator resets between inference calls, so workspace allocation is O(1) after the first call.
- **Variability sources:** Timer interrupts, other running tasks, and cache state can cause latency variation. For consistent measurements, avoid running other components during benchmarking.

---

## System Benchmarks

### bench command

The `bench` command measures core OS primitive performance:

```
SLM-OS> bench context    # Context switch latency
SLM-OS> bench irq        # Timer interrupt jitter
SLM-OS> bench ipc        # IPC message round-trip
SLM-OS> bench smp        # Cross-CPU task dispatch
SLM-OS> bench all        # Run all benchmarks
```

### Context Switch Latency (`bench context`)

Measures the time for a full task context switch: save callee-saved registers, switch stack pointer, restore registers. The benchmark creates a temporary task, performs a round-trip switch, and destroys it.

Typical results:

| Platform | Latency |
|----------|---------|
| Jetson Orin Nano (A78AE) | ~262 ns |
| Raspberry Pi 5 (A76) | ~1,600 ns |
| QEMU virt (emulated) | ~20,000 ns |

### Interrupt Latency (`bench irq`)

Measures timer tick jitter by comparing consecutive `CNTPCT_EL0` readings across 20 timer interrupts at 100 Hz. Reports min, average, and max inter-tick intervals.

Sub-microsecond average latency is expected on real hardware. QEMU timings are not meaningful due to emulation overhead.

### IPC Round-Trip (`bench ipc`)

Measures the time for a send+receive cycle through the kernel message queue subsystem. Runs 100 iterations and reports average per-operation latency.

Both Jetson and Pi 5 demonstrate sub-microsecond IPC.

### SMP Dispatch (`bench smp`)

Validates cross-CPU task dispatch by sending work to all online CPUs and measuring round-trip completion. This benchmark exercises the non-cacheable run queues, WFE/SEV signaling, and per-CPU scheduling.

---

## Stack Size Considerations

SLM-OS kernel tasks share a fixed stack size configured in `kernel/include/config.h`:

```c
#define STACK_SIZE  0x10000UL  /* 64 KB per stack */
```

### Why Stack Size Matters

The ONNX parser and inference engine use stack-allocated structures to avoid heap fragmentation in a bare-metal environment. Key stack consumers include:

| Structure | Approximate Size | Used By |
|-----------|-----------------|---------|
| `ParsedOnnx` | ~6 KB | ONNX protobuf parser |
| `OperatorGraph` | ~6 KB | Graph construction and inference |
| `WeightTable` | ~2 KB | Weight binding during inference |
| Operator temporaries | Variable | Per-operator scratch space |

With a 64 KB stack, the full parsing and inference call chain fits comfortably. Reducing the stack below 32 KB risks stack overflow during model loading.

### Debug vs. Release Stack Usage

Debug builds (`BUILD_TYPE=Debug`) use more stack due to:

- No inlining of function calls (each call adds a frame)
- Debug assertions and bounds checking
- Unoptimized register allocation (more spills to stack)

Release builds (`BUILD_TYPE=Release`) benefit from:

- Aggressive inlining (fewer call frames)
- Link-time optimization (LTO) across Rust/C boundary
- Optimized register allocation

For stack-constrained scenarios, always test with a Release build:

```bash
make kernel BUILD_TYPE=Release
```

---

## Build Type Impact

### Debug (default)

```bash
make kernel                    # Same as BUILD_TYPE=Debug
make kernel BUILD_TYPE=Debug
```

- **C kernel:** Compiled with `-O0 -g` (no optimization, full debug info)
- **Rust runtime:** Compiled in debug mode (bounds checking, overflow checks)
- **Stack usage:** Higher (no inlining, unoptimized spills)
- **Inference speed:** Slower (no auto-vectorization, no loop unrolling)
- **Use case:** Development and debugging

### Release

```bash
make kernel BUILD_TYPE=Release
```

- **C kernel:** Compiled with `-O2` (standard optimization)
- **Rust runtime:** Compiled with `--release` (LTO, optimization level 3)
- **Stack usage:** Lower (inlining, optimized register allocation)
- **Inference speed:** Faster (auto-vectorization, loop unrolling, LTO)
- **Use case:** Benchmarking and hardware deployment

For meaningful performance measurements, always benchmark with Release builds. Debug build timings are not representative of production performance.

---

## Memory Pool Sizing

### Viewing Pool Statistics

```
SLM-OS> model pools
```

This displays statistics for both the weight pool and workspace pool:

| Metric | Description |
|--------|-------------|
| Total blocks | Number of 2 MB blocks in the pool |
| Free blocks | Currently unallocated blocks |
| Allocated blocks | Blocks in use by loaded models |
| Shared blocks | Blocks referenced by multiple consumers |
| Peak usage | Maximum blocks ever allocated simultaneously |

### Weight Pool

The weight pool stores read-only model parameters. Each model's weights are allocated as a contiguous region from this pool.

- **Sizing guidance:** Each 2 MB block can hold approximately 500K FP32 parameters. MNIST-12 uses ~26 KB of weights (fits in one block).
- **Optimization:** Unload unused models with `model unload <name>` to reclaim weight pool blocks.

### Workspace Pool

The workspace pool provides temporary storage for intermediate tensors during inference. A bump allocator manages this space: it resets to the beginning after each inference call, making allocation effectively free.

- **Sizing guidance:** Workspace usage depends on the largest intermediate tensor in the model. For MNIST-12, the workspace needs are modest (~100 KB for intermediate feature maps).
- **Monitoring:** If inference fails with a `WorkspaceExhausted` error, the model's intermediate tensors exceed the workspace pool capacity.

### Buddy Allocator Statistics

The physical memory allocator provides its own statistics via:

```
SLM-OS> mem
```

This shows the buddy allocator's free block counts at each order (4 KB to 1 GB), along with total allocation and merge counts. High fragmentation (many small free blocks, few large ones) may indicate excessive allocation churn.

---

## Performance Optimization Checklist

1. **Use Release builds** for all benchmarking (`BUILD_TYPE=Release`)
2. **Warm the cache** by running one inference before measuring (`model infer <name>`, then `model bench <name>`)
3. **Minimize background activity** -- stop unnecessary components during benchmarking
4. **Check pool utilization** -- ensure weight and workspace pools are not near capacity
5. **Monitor statistics** -- use `model stats` to track long-running performance trends
6. **Compare platforms** -- use `bench all` to establish baseline OS overhead on each target

---

## Reference: Benchmark Results

See `docs/performance.md` for detailed benchmark results across all supported platforms (QEMU, Raspberry Pi 5, Jetson Orin Nano).

---

## Source Files

| File | Purpose |
|------|---------|
| `kernel/src/shell_sys.c` | `bench` and `model bench` shell commands |
| `runtime/src/inference/engine.rs` | Inference engine with statistics tracking |
| `kernel/include/config.h` | `STACK_SIZE` definition |
| `kernel/mm/pmm.c` | Buddy allocator with statistics |
| `runtime/src/mm/model_mem.rs` | Weight and workspace pool management |
| `docs/performance.md` | Platform benchmark results |

---

*Last updated: April 2026*
