# GPU Compute Integration

Design and implementation of GPU-accelerated inference in SLM-OS (Phase 5, Milestone 3).

**Status (April 2026):** **GPU MNIST inference is working** on Jetson GA10B
via an inherit-from-Linux kexec handoff. The graceful-CPU-fallback design
described below is still in place; on Jetson the engine's `run_inference`
short-circuits through `gpu::run_mnist_gpu_fastpath` for the MNIST graph
and only falls back to CPU on dispatch failure.

> **What changed in M10 (April 2026):**
>
> - `runtime/src/inference/engine.rs::run_inference` detects an active
>   `mnist` model + `compute_ready=1` and routes the whole graph through
>   `gpu::run_mnist_gpu_fastpath`, which hits `slm_gpu_set_mnist_input`
>   + `slm_gpu_run_mnist`.
> - The kernel-side dispatch in `kernel/gpu/nvidia/ga10b_bringup.c` uses
>   `ga10b_build_launch_kernel_with_sema_pushbuffer` to append a
>   `REPORT_SEMAPHORE_EXECUTE` (OP=RELEASE, ONE_WORD) to each op's
>   pushbuffer. The GPU drains compute and flushes L2 → DRAM before
>   the release fires, giving a real "all output is in DRAM" completion
>   signal. Closes #372 (sentinel-zero polling deadlock) and #390
>   (Conv1 truncation past 4 KB).
> - On Jetson `slm_gpu_get_info`'s `compute_ready` is 1; on every other
>   platform it stays 0 and the original CPU-only fallback runs unchanged.
>
> Demo: `slm.model_infer_file(slm.model_load_mnist(), '/mnt/files/digits/digit_3.bin')`
> returns 3, with the full pipeline executing on GPU. See
> `docs/archive/plans/jetson-gpu-mnist-plan.md` for the M0..M10 plan history
> and `docs/gpu.md` for the broader GPU integration overview.
>
> The "deferred pending GSP firmware" framing below applies to **cold-boot
> GPU bringup** (still tracked as #142 — bare-metal GSP loader). The
> kexec-from-Linux path doesn't need a bare-metal GSP loader because Linux
> bootstraps FECS/GPCCS/PMU before SLM-OS inherits the channel.

---

## Overview

SLM-OS provides a GPU compute integration layer for accelerating inference workloads. The design acknowledges that modern NVIDIA GPUs (Ampere and later) require the GPU System Processor (GSP) firmware for actual compute operations. The framework implements a **graceful CPU fallback**: operator placement decisions are made at runtime based on GPU capabilities. Where the GPU is unavailable or a dispatch fails, operators execute on the CPU backend.

This approach ensures that:
1. The inference engine works correctly on all platforms (QEMU, Pi 5, Jetson, x86-64).
2. On Jetson with the inherit-from-Linux GPU path, the MNIST graph runs end-to-end on GPU; CPU fallback engages on any dispatch error.
3. Cache coherency protocols are in place for CPU/GPU data sharing.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│  Inference Engine (runtime/src/inference/engine.rs)                  │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  Operator Graph Execution (topological order)                 │  │
│  └───────────────────────────┬───────────────────────────────────┘  │
│                              │                                      │
│                              ▼                                      │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  select_backend(op, input_elements, gpu_caps)                 │  │
│  │  Routes each operator to CPU or GPU based on heuristic        │  │
│  └──────────┬────────────────────────────────────┬───────────────┘  │
│             │                                    │                  │
│             ▼                                    ▼                  │
│  ┌─────────────────────┐          ┌─────────────────────────────┐  │
│  │  CPU Backend         │          │  GPU Backend (stub)          │  │
│  │  MatMul, Gemm, Conv  │          │  gpu_execute_matmul()        │  │
│  │  Add, Relu, Softmax  │          │  Returns Err(NotReady)       │  │
│  │  Reshape, Flatten    │          │  → falls back to CPU         │  │
│  └─────────────────────┘          └──────────────┬──────────────┘  │
│                                                  │                  │
│                                                  ▼                  │
│                                   ┌─────────────────────────────┐  │
│                                   │  Kernel GPU Driver           │  │
│                                   │  gpu_submit() / gpu_wait()   │  │
│                                   │  (not implemented — GSP req) │  │
│                                   └─────────────────────────────┘  │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  Cache Coherency (kernel/gpu/cache.c)                         │  │
│  │  cache_clean_range() — DC CVAC — before GPU reads             │  │
│  │  cache_invalidate_range() — DC IVAC — after GPU writes        │  │
│  └───────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

---

## GPU Backend Selection

The `select_backend()` function determines whether an operator should run on CPU or GPU. It takes the operator type, the number of input elements, and the current GPU capabilities.

### Heuristic

| Operator | Threshold | Backend (if compute ready) |
|----------|-----------|---------------------------|
| MatMul | > 4096 elements | GPU |
| Gemm | > 4096 elements | GPU |
| Conv | > 8192 elements | GPU |
| All others | Any size | CPU |

Small operators and element-wise ops (Add, Relu, Softmax, Reshape, Flatten) always run on CPU to avoid transfer overhead. When GPU compute is not available (the current state on all platforms), all operators are routed to CPU regardless of size.

### Backend Enum

```rust
pub enum Backend {
    Cpu,
    Gpu,
}
```

### GpuCapabilities

GPU capabilities are detected at runtime via the kernel FFI:

```rust
pub struct GpuCapabilities {
    pub status: GpuStatus,        // NotAvailable, DetectedNoCompute, or ComputeReady
    pub capabilities: u32,        // GPU_CAP_* flags
    pub cuda_cores: u32,
    pub tensor_cores: u32,
    pub unified_memory: bool,
    pub name: [u8; 32],           // Driver name ("nvidia", "stub", "none")
    pub device: [u8; 64],         // Device description
}
```

---

## Cache Coherency Protocol

On ARM64 systems with unified memory (Jetson Orin), the CPU and GPU share physical RAM but caches are not hardware-coherent. SLM-OS implements explicit cache maintenance to ensure data consistency.

### CPU-to-GPU Transfer (Weight Loading)

When model weights need to be read by the GPU:

1. CPU writes weight data to memory (via model loader)
2. `gpu_map_weights(model_index)` is called
3. Internally calls `cache_clean_range()` (ARM64 `DC CVAC`) to flush dirty cache lines to DRAM
4. GPU can now read correct weight data

### GPU-to-CPU Transfer (Inference Output)

When the GPU has written output data that the CPU needs to read:

1. GPU writes output to shared buffer
2. `gpu_unmap_weights(model_index)` is called
3. Internally calls `cache_invalidate_range()` (ARM64 `DC IVAC`) to discard stale cache lines
4. CPU reads fresh data from DRAM

### Functions

| Function | Purpose |
|----------|---------|
| `gpu_map_weights(model_index)` | Flush CPU caches so GPU sees current weight data |
| `gpu_unmap_weights(model_index)` | Invalidate CPU caches so CPU sees GPU-modified data |
| `cache_clean_range(addr, size)` | Write back dirty cache lines (DC CVAC), line by line |
| `cache_invalidate_range(addr, size)` | Discard cached data (DC IVAC), line by line |

Both `gpu_map_weights` and `gpu_unmap_weights` look up the model's weight allocation from the registry, obtain the pointer and size, and perform the appropriate cache operation via the kernel FFI (`slm_gpu_sync_for_device` / `slm_gpu_sync_for_cpu`).

---

## Shell Command

The `model gpu` shell command displays GPU status, capabilities, and the current inference backend.

```
SLM-OS> model gpu
GPU Status:
  Driver:         stub
  Device:         No GPU hardware
  Compute:        Not available
  Unified memory: no
  Inference:      CPU only
```

On Jetson hardware (with GPU detected but no GSP):

```
SLM-OS> model gpu
GPU Status:
  Driver:         nvidia
  Device:         NVIDIA GA10B (Ampere)
  Compute:        Detected (compute not ready - GSP required)
  CUDA cores:     1024
  Tensor cores:   32
  Unified memory: yes
  Inference:      CPU only
```

---

## GSP Firmware Path (Future)

Enabling actual GPU compute on the Jetson Orin Nano requires loading and initializing the GSP (GPU System Processor), a RISC-V core embedded in the Ampere GPU. This is a significant undertaking that has been deferred.

### Steps Required

1. **GSP Firmware Loading** — Load the GSP firmware binary from the filesystem into GPU-accessible memory. The firmware is typically found at `/lib/firmware/nvidia/` on Linux systems.

2. **GSP RISC-V Boot** — Initialize the GSP core's registers, set the program counter to the firmware entry point, and release it from reset.

3. **GSP RPC Communication** — Establish the command/response mailbox protocol between the CPU and GSP. The GSP firmware expects a specific RPC message format for GPU initialization commands.

4. **PGRAPH Initialization** — Use GSP RPC to initialize the GPU's compute engine (PGRAPH), configure memory controllers, and set up compute contexts.

5. **Compute Command Submission** — Implement `gpu_submit()` and `gpu_wait()` in the kernel GPU driver to send compute commands (shader dispatch, memory operations) to the GPU via Host1x push buffers and synchronize with syncpoints.

### References

For hardware-level GPU details (MMIO registers, probe results, driver architecture), see `docs/gpu.md`. The NVIDIA open-gpu-kernel-modules repository and the nouveau project document GSP firmware loading sequences.

---

## Source Files

| File | Purpose |
|------|---------|
| `runtime/src/inference/gpu.rs` | GPU backend: GpuCapabilities, select_backend, Backend enum, gpu_map/unmap_weights |
| `runtime/src/lib.rs` | FFI entry points: `rust_gpu_print_status()`, `rust_gpu_compute_test()` |
| `kernel/src/slm_ffi.c` | C FFI: `slm_gpu_available()`, `slm_gpu_get_info()` |
| `kernel/include/slm_ffi.h` | C declarations and `RustGpuInfo` struct |
| `kernel/gpu/gpu.c` | GPU driver registration and dispatch |
| `kernel/gpu/cache.c` | ARM64 cache maintenance operations |

---

## Current Status

| Platform | GPU Status | Inference Backend |
|----------|-----------|-------------------|
| QEMU (ARM64/x86-64) | `NotAvailable` — no GPU hardware | CPU |
| Raspberry Pi 5 | `NotAvailable` — no GPU driver | CPU |
| Jetson Orin Nano | `DetectedNoCompute` — GPU probed, GSP not loaded | CPU |

All inference currently runs on the CPU backend. The GPU framework is in place and will activate automatically when GSP firmware support enables `GpuStatus::ComputeReady`.

---

*Created: April 2026*
*See also: `docs/gpu.md` (hardware driver details), `docs/ffi.md` (FFI reference)*
