# GPU Compute Integration

Design and implementation of GPU-accelerated inference in SLM-OS (Phase 5, Milestone 3).

**Status:** Framework complete; all inference runs on CPU. GPU compute deferred pending GSP firmware.

---

## Overview

SLM-OS provides a GPU compute integration layer for accelerating inference workloads. The design acknowledges that modern NVIDIA GPUs (Ampere and later) require the GPU System Processor (GSP) firmware for actual compute operations, which is not yet implemented. The framework therefore implements a **graceful CPU fallback**: operator placement decisions are made at runtime based on GPU capabilities, and all operators currently execute on the CPU backend.

This approach ensures that:
1. The inference engine works correctly today on all platforms (QEMU, Pi 5, Jetson)
2. When GSP firmware support is added, GPU acceleration activates without changes to the inference pipeline
3. Cache coherency protocols are already in place for CPU/GPU data sharing

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
