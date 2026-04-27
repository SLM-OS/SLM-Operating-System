# GPU Integration

Design and implementation documentation for GPU support in SLM-OS (Phase 3, Milestone 3).

**Status (April 2026):** GPU **compute** is working end-to-end on Jetson GA10B
via the inherit-from-Linux kexec handoff path. Memory abstraction, NVIDIA GPU
probe, and the platform-agnostic GPU HAL are all complete.

> **GPU compute is live (April 2026):** MNIST inference dispatches end-to-end
> on the GA10B GPU from SLM-OS post-kexec for arbitrary user-supplied digit
> images. The dispatch architecture uses `REPORT_SEMAPHORE_EXECUTE` (Volta+
> AMPERE_COMPUTE_B) for op completion — fixed both the polling-zero deadlock
> (closed #372) and the apparent Conv1 4 KB truncation (closed #390) at the
> same time. The whole 8-op pipeline (Conv1 → Add+ReLU → Pool1 → Conv2 →
> Add+ReLU → Pool2 → MatMul → AddBias) runs on GPU with full L2 → DRAM flush
> per op. Demo path: `slm.model_infer_file(slm.model_load_mnist(),
> '/mnt/files/digits/digit_3.bin')` returns 3.
>
> Historical narrative below describes the pre-M10 state where compute was
> deferred. The plan that took us from M0 (multi-CTA dispatch) to M10
> (semaphore-release completion + custom-file demo) is preserved as
> `docs/archive/plans/jetson-gpu-mnist-plan.md`. See also
> `docs/archive/investigations/jetson-el2-bringup.md` for the EL2 bringup that makes BAR0/MMIO
> accessible.

---

## Overview

SLM-OS provides a platform-agnostic GPU abstraction layer that enables:
- GPU-accessible memory allocation
- Cache coherency for CPU/GPU data sharing
- Uniform API across platforms (QEMU stub, NVIDIA Ampere)

### Key Design Decision

Modern NVIDIA GPUs (Turing and later, including Ampere in Jetson Orin) require the **GPU System Processor (GSP)** for initialization. GSP is a RISC-V processor on the GPU that handles:
- Power management and clock configuration
- Memory controller setup
- Firmware loading and verification

This means true bare-metal GPU compute is impractical without a Linux-like environment to load GSP firmware. SLM-OS therefore focuses on:
1. **Memory management** for GPU-accessible buffers
2. **Cache coherency** for CPU/GPU data transfer
3. **Platform abstraction** for future GPU compute via TensorRT (Phase 5)

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                       GPU Subsystem                                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   Application Layer                                                  │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │  gpu_alloc() / gpu_free()                                   │   │
│   │  gpu_sync_for_gpu() / gpu_sync_for_cpu()                    │   │
│   └─────────────────────────────────────────────────────────────┘   │
│                              │                                       │
│                              ▼                                       │
│   GPU Core (gpu.c)                                                   │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │  Driver registration and dispatch                           │   │
│   │  gpu_register_driver() → active driver                      │   │
│   └─────────────────────────────────────────────────────────────┘   │
│                              │                                       │
│              ┌───────────────┴───────────────┐                       │
│              ▼                               ▼                       │
│   ┌─────────────────────┐       ┌─────────────────────┐             │
│   │  gpu_stub_driver    │       │  gpu_nvidia_driver  │             │
│   │  (QEMU - no GPU)    │       │  (Jetson - future)  │             │
│   └─────────────────────┘       └─────────────────────┘             │
│              │                               │                       │
│              ▼                               ▼                       │
│   ┌─────────────────────────────────────────────────────────────┐   │
│   │  cache.c - ARM64 Cache Maintenance (DC CVAC/IVAC/CIVAC)     │   │
│   └─────────────────────────────────────────────────────────────┘   │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## GPU Driver Interface

### Driver Structure

Each GPU driver implements the `struct gpu_driver` interface:

```c
struct gpu_driver {
    const char *name;

    /* Initialization */
    int (*init)(void);
    void (*shutdown)(void);

    /* Information */
    int (*get_info)(gpu_info_t *info);

    /* Memory Management */
    int (*alloc)(size_t size, uint32_t flags, gpu_buffer_t *buf);
    void (*free)(gpu_buffer_t *buf);

    /* Cache Coherency */
    void (*sync_for_gpu)(gpu_buffer_t *buf);
    void (*sync_for_cpu)(gpu_buffer_t *buf);

    /* Command Submission (future) */
    int (*submit)(void *cmd_buffer, size_t size);
    int (*wait)(uint64_t timeout_ns);
};
```

### GPU Buffer

Buffers accessible by both CPU and GPU:

```c
typedef struct gpu_buffer {
    void     *cpu_addr;     /* CPU virtual address */
    uint64_t  gpu_addr;     /* GPU physical/IOVA address */
    size_t    size;         /* Buffer size in bytes */
    uint32_t  flags;        /* Allocation flags */
} gpu_buffer_t;
```

### Memory Flags

```c
#define GPU_MEM_READ        0x01  /* GPU will read */
#define GPU_MEM_WRITE       0x02  /* GPU will write */
#define GPU_MEM_READWRITE   0x03  /* Both */
#define GPU_MEM_CACHED      0x04  /* CPU-cacheable */
#define GPU_MEM_UNCACHED    0x08  /* Not cached */
#define GPU_MEM_ALIGN_2MB   0x10  /* 2MB alignment */
```

---

## Cache Coherency

### The Problem

On ARM64 systems without hardware cache coherency for GPU/DMA:
1. CPU writes to memory may stay in cache (not visible to GPU)
2. GPU writes to memory may be stale in CPU cache

### ARM64 Cache Operations

| Operation | Instruction | Use Case |
|-----------|-------------|----------|
| Clean | `DC CVAC` | Before GPU reads: flush dirty cache lines to memory |
| Invalidate | `DC IVAC` | After GPU writes: discard stale cache data |
| Flush | `DC CIVAC` | Both: clean then invalidate |

### Usage Pattern

```c
gpu_buffer_t buf;
gpu_alloc(size, GPU_MEM_READWRITE, &buf);

/* CPU writes data */
memcpy(buf.cpu_addr, data, size);

/* Sync for GPU (clean cache) */
gpu_sync_for_gpu(&buf);

/* GPU processes data... */

/* Sync for CPU (invalidate cache) */
gpu_sync_for_cpu(&buf);

/* CPU reads GPU output */
result = *(uint32_t *)buf.cpu_addr;
```

### Implementation

Cache operations in `kernel/gpu/cache.c`:

```c
void cache_clean_range(void *addr, size_t size)
{
    for (uintptr_t line = start; line < end; line += 64) {
        __asm__ volatile("dc cvac, %0" : : "r"(line) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

void cache_invalidate_range(void *addr, size_t size)
{
    for (uintptr_t line = start; line < end; line += 64) {
        __asm__ volatile("dc ivac, %0" : : "r"(line) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
```

---

## QEMU Stub Driver

For platforms without GPU hardware, the stub driver provides:
- Memory allocation via PMM (regular pages)
- Cache operations still executed (tests coherency code path)
- No compute capability (GPU_CAP_NONE)

The stub allows SLM-OS code to use the GPU API uniformly, falling back gracefully when no GPU is present.

---

## Jetson Orin Nano GPU (Future)

### Hardware Specifications

| Feature | Value |
|---------|-------|
| Architecture | NVIDIA Ampere |
| CUDA Cores | 1024 |
| Tensor Cores | 32 (3rd gen) |
| Memory | 8GB LPDDR5 (unified with CPU) |
| Bandwidth | 68 GB/s |
| AI Performance | 67 TOPS (INT8) |

### GSP Firmware Requirement

The Orin's Ampere GPU requires GSP (GPU System Processor) firmware:
- GSP is a RISC-V core on the GPU
- Firmware loaded from `/lib/firmware/nvidia/`
- Handles GPU initialization, power management, memory setup
- Required for any GPU operation

This means full GPU compute requires either:
1. Linux L4T (Linux for Tegra) running alongside SLM-OS
2. TensorRT runtime for inference (Phase 5)

### Tegra Host1x

Tegra SoCs use Host1x as the central DMA engine:
- Push buffers: memory regions containing GPU commands
- Syncpoints: 32-bit monotonic counters for synchronization
- Channels: connect push buffers to GPU clients

Key registers:
- `HOST1X_CHANNEL_DMASTART` — buffer start address
- `HOST1X_CHANNEL_DMAEND` — buffer end address
- `HOST1X_CHANNEL_DMAPUT` — written data pointer

### Implementation Plan

1. **Phase 3:** Memory mapping and cache coherency (complete)
2. **Milestone 4:** Basic Host1x initialization on real hardware
3. **Phase 5:** TensorRT integration for actual inference

---

## API Reference

### Initialization

```c
/* Register platform-specific driver */
void gpu_register_driver(const struct gpu_driver *drv);

/* Initialize GPU subsystem */
int gpu_init(void);

/* Shutdown GPU subsystem */
void gpu_shutdown(void);

/* Check if GPU available */
bool gpu_available(void);
```

### Memory Management

```c
/* Allocate GPU-accessible buffer */
int gpu_alloc(size_t size, uint32_t flags, gpu_buffer_t *buf);

/* Free GPU buffer */
void gpu_free(gpu_buffer_t *buf);
```

### Cache Coherency

```c
/* Sync for GPU access (after CPU writes) */
void gpu_sync_for_gpu(gpu_buffer_t *buf);

/* Sync for CPU access (after GPU writes) */
void gpu_sync_for_cpu(gpu_buffer_t *buf);
```

### Low-Level Cache Operations

```c
/* Clean: write back dirty lines */
void cache_clean_range(void *addr, size_t size);

/* Invalidate: discard cached data */
void cache_invalidate_range(void *addr, size_t size);

/* Flush: clean + invalidate */
void cache_flush_range(void *addr, size_t size);
```

### Error Codes

| Code | Meaning |
|------|---------|
| `GPU_OK` (0) | Success |
| `GPU_ERR_NOT_INIT` (-1) | gpu_init() not called |
| `GPU_ERR_NO_DEVICE` (-2) | No GPU hardware |
| `GPU_ERR_NO_MEMORY` (-3) | Out of memory |
| `GPU_ERR_INVALID_PARAM` (-4) | Invalid parameter |
| `GPU_ERR_NOT_SUPPORTED` (-5) | Operation not supported |
| `GPU_ERR_FIRMWARE` (-6) | Firmware error |
| `GPU_ERR_TIMEOUT` (-7) | Timeout |

---

## Integration with Model Memory

The GPU subsystem integrates with model memory (Milestone 1) for AI inference:

```c
/* Allocate model weights with GPU access */
ModelHandle weights = alloc_weights(model_size);
void *ptr = get_ptr(weights);

/* Create GPU buffer from model memory */
gpu_buffer_t gpu_buf = {
    .cpu_addr = ptr,
    .gpu_addr = (uint64_t)(uintptr_t)ptr,  /* Unified memory */
    .size = model_size,
    .flags = GPU_MEM_READ
};

/* Sync and submit to GPU */
gpu_sync_for_gpu(&gpu_buf);
/* ... GPU inference ... */
gpu_sync_for_cpu(&gpu_buf);
```

---

## File Structure

```
kernel/gpu/
├── gpu.h           # Public API and driver interface
├── gpu.c           # Driver registration and dispatch
├── cache.c         # ARM64 cache maintenance operations
├── gpu_stub.c      # QEMU stub driver (no GPU hardware)
├── gpu_nvidia.h    # NVIDIA Ampere register definitions (shared)
└── gpu_nvidia.c    # NVIDIA Ampere driver (Jetson GA10B + x86-64 GA106)
```

### NVIDIA GPU Probe Results (Jetson Orin Nano, April 2026)

The GPU at `0x17000000` is accessible from EL2. Identification registers:

| Register | Value | Meaning |
|----------|-------|---------|
| NV_PMC_BOOT_0 | `0xB7B000A1` | GA10B, rev A1, Ampere |
| NV_PMC_BOOT_42 | `0x17BA1000` | Chip ID 0x17B |
| NV_PMC_ENABLE | `0x50000000` | Some engines active |

This confirms the GPU's MMIO space is not blocked by the CBB firewall at EL2. Next steps: GSP firmware loading, Host1x DMA, compute shader dispatch.

---

## References

- [NVIDIA Jetson Orin Nano Developer Kit](https://developer.nvidia.com/embedded/learn/jetson-orin-nano-devkit-user-guide/hardware_spec.html)
- [Host1x Hardware Description](https://http.download.nvidia.com/tegra-public-appnotes/host1x.html)
- [ARM64 Cache Maintenance Operations](https://developer.arm.com/documentation/ddi0500/e/system-control/aarch64-register-summary/aarch64-cache-maintenance-operations)
- [NVIDIA Open GPU Kernel Modules](https://github.com/NVIDIA/open-gpu-kernel-modules)
- [Linux drm/tegra Driver](https://docs.kernel.org/gpu/tegra.html)

---

*Created: December 2025*
*Last updated: April 2026 — NVIDIA GPU probe working on Jetson at EL2*
