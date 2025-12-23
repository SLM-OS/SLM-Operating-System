# SLM-OS Architecture

High-level architecture documentation for the Small Language Model Operating System.

**Status:** Phase 3 complete (December 2025)

---

## Overview

SLM-OS is a bare-metal operating system designed for running AI inference workloads on edge devices, specifically targeting the NVIDIA Jetson Orin Nano. The system is built as a hybrid C/Rust kernel with specialized support for AI model memory management and deadline-aware scheduling.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           SLM-OS Architecture                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                    Application Layer (Future)                        │   │
│   │         Components, Model Inference, User Tasks                      │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                         │
│                                    ▼                                         │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                     Rust Runtime Layer                               │   │
│   │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐   │   │
│   │  │  Model Mem   │  │  Scheduler   │  │     Logging/FFI          │   │   │
│   │  │  Allocator   │  │   Policy     │  │                          │   │   │
│   │  └──────────────┘  └──────────────┘  └──────────────────────────┘   │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │ FFI                                     │
│                                    ▼                                         │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                      C Kernel Layer                                  │   │
│   │  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐  │   │
│   │  │  PMM   │ │  VMM   │ │ Sched  │ │  IPC   │ │  SMP   │ │  GPU   │  │   │
│   │  │        │ │  MMU   │ │        │ │        │ │        │ │  HAL   │  │   │
│   │  └────────┘ └────────┘ └────────┘ └────────┘ └────────┘ └────────┘  │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                         │
│                                    ▼                                         │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                     Hardware Abstraction                             │   │
│   │  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐     │   │
│   │  │ UART PL011 │  │  GIC-400   │  │ ARM Timer  │  │  Cache Ops │     │   │
│   │  └────────────┘  └────────────┘  └────────────┘  └────────────┘     │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                         │
│                                    ▼                                         │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                    Hardware (ARM64 / Cortex-A78AE)                   │   │
│   │            QEMU virt (development) │ Jetson Orin Nano (target)       │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Design Principles

### 1. Hybrid C/Rust Architecture

- **C Kernel**: Core OS primitives (memory, scheduling, IPC, hardware drivers)
- **Rust Runtime**: Higher-level policies, model memory management, AI scheduling
- **FFI Boundary**: Clean separation with type-safe wrappers

This hybrid approach leverages:
- C for low-level hardware control and established OS patterns
- Rust for memory-safe higher-level logic and future AI integration

### 2. AI-First Memory Design

- **2MB aligned pools** for model weights (reduces TLB pressure)
- **Separate weight/workspace pools** (weights read-only and shareable)
- **Zero-copy sharing** via reference counting
- **GPU-ready allocation** (cache coherent, aligned for DMA)

### 3. Deadline-Aware Scheduling

- **Hybrid priority/deadline** algorithm
- **8 priority levels** with automatic deadline boost
- **Per-CPU run queues** for scalability
- **Priority inheritance** for mutex operations
- **Core isolation** for latency-sensitive workloads

### 4. Platform Abstraction

- **Compile-time platform selection** via CMake
- **Common driver interfaces** (UART, timer, interrupt controller)
- **QEMU virt** as primary development platform
- **Jetson Orin Nano** as production target

---

## Subsystem Overview

### Memory Management

| Component | File(s) | Purpose |
|-----------|---------|---------|
| PMM | `kernel/src/pmm.c` | Physical page allocation (bitmap) |
| VMM | `kernel/src/vmm.c` | Virtual memory mapping |
| MMU | `kernel/src/mmu.S` | Page table management (ARMv8) |
| Model Memory | `runtime/src/mm/` | 2MB-aligned model weight/workspace pools |

**Phase 3 Learnings:**
- 2MB block alignment dramatically reduces TLB misses for large models
- Separate pools for weights vs workspace simplifies sharing semantics
- Generation counters in handles detect use-after-free

### Scheduling

| Component | File(s) | Purpose |
|-----------|---------|---------|
| Scheduler | `kernel/src/sched.c` | Per-CPU run queues, priority ordering |
| Task Management | `kernel/src/task.c` | Task lifecycle, context switch |
| Context Switch | `kernel/src/context.S` | Register save/restore |
| PI Mutex | `kernel/src/pi_mutex.c` | Priority-inheriting mutex |
| Deadline Policy | `runtime/src/sched/deadline.rs` | Deadline analysis, core hints |
| Heterogeneous | `runtime/src/sched/heterogeneous.rs` | big.LITTLE topology awareness |

**Phase 3 Learnings:**
- Deadline boost thresholds (10ms/50ms/100ms) provide good responsiveness
- Priority inheritance prevents unbounded priority inversion
- Core isolation useful for latency-sensitive inference tasks

### Inter-Process Communication

| Component | File(s) | Purpose |
|-----------|---------|---------|
| Message Queues | `kernel/src/ipc.c` | Synchronous message passing |
| Shared Memory | `kernel/src/ipc.c` | Zero-copy buffer sharing |

**Phase 3 Learnings:**
- Timeout support essential for robust applications
- Statistics tracking helps debug queue sizing issues
- Single-threaded stress tests more reliable than multi-task in early development

### SMP Support

| Component | File(s) | Purpose |
|-----------|---------|---------|
| SMP Boot | `kernel/src/smp.c`, `smp_boot.S` | Secondary CPU initialization |
| Spinlocks | `kernel/include/spinlock.h` | Multi-core synchronization |
| IPI | `kernel/src/gic.c` | Inter-processor interrupts |

**Phase 3 Learnings:**
- PSCI is the standard way to boot secondary cores
- Per-CPU stacks must be allocated before secondary boot
- Spinlock ordering prevents deadlocks (always lock in CPU ID order)

### GPU Subsystem

| Component | File(s) | Purpose |
|-----------|---------|---------|
| GPU HAL | `kernel/gpu/gpu.c` | Driver registration, dispatch |
| Cache Ops | `kernel/gpu/cache.c` | ARM64 cache maintenance |
| Stub Driver | `kernel/gpu/gpu_stub.c` | QEMU (no GPU) fallback |

**Phase 3 Learnings:**
- Modern NVIDIA GPUs require GSP firmware (not bare-metal accessible)
- Focus on memory/cache coherency; defer compute to TensorRT (Phase 5)
- Platform abstraction allows development on QEMU while targeting Jetson

---

## Boot Sequence

```
Power On
    │
    ▼
┌─────────────────┐
│   _start        │  kernel/src/boot.S
│   (EL2 → EL1)   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   kernel_init   │  kernel/src/main.c
│   - UART init   │
│   - PMM init    │
│   - VMM/MMU     │
│   - Scheduler   │
│   - SMP boot    │
│   - Rust init   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   Per-CPU Init  │  Secondary CPUs join
│   - Timer       │
│   - Idle task   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   Test/Shell    │  Or application tasks
└─────────────────┘
```

See `docs/boot-sequence.md` for detailed documentation.

---

## Directory Structure

```
CS-496-SLM-Operating-System/
├── kernel/
│   ├── include/          # C headers
│   ├── src/              # Core kernel (C + assembly)
│   ├── drivers/          # Hardware drivers
│   ├── gpu/              # GPU subsystem
│   └── tests/            # Kernel test suite
├── runtime/
│   └── src/
│       ├── lib.rs        # Rust entry points
│       ├── kernel_ffi.rs # FFI to C kernel
│       ├── log.rs        # Logging infrastructure
│       ├── mm/           # Model memory management
│       └── sched/        # Scheduling policies
├── docs/                 # Documentation
└── build/                # Build output (gitignored)
```

---

## Key Interfaces

### C Kernel → Rust Runtime

```c
// Initialize Rust heap and runtime
void rust_heap_init(void *heap_start, size_t heap_size);
int rust_init(void);  // Returns 42 on success

// Logging
void rust_log_info(const char *msg);
void rust_log_error(const char *msg);

// Scheduling hints
uint8_t rust_select_inference_core(size_t model_size, uint8_t is_urgent);
```

### Rust Runtime → C Kernel

```rust
// Memory allocation
extern "C" fn slm_alloc_pages(count: usize) -> *mut u8;
extern "C" fn slm_free_pages(ptr: *mut u8, count: usize);

// Task management
extern "C" fn slm_task_create(name: *const c_char, entry: fn, arg: *mut c_void) -> u32;
extern "C" fn slm_task_set_priority(task_id: u32, priority: u8) -> i32;
extern "C" fn slm_task_set_deadline(task_id: u32, deadline_ns: u64) -> i32;
```

See `docs/ffi.md` for complete FFI documentation.

---

## Phase 3 Summary

### Completed Features
- Model memory allocator with 2MB blocks (Rust)
- Deadline-aware hybrid scheduler with priority boost
- Priority-inheriting mutex
- GPU platform abstraction (stub driver for QEMU)
- IPC timeout support and statistics
- Heterogeneous CPU topology awareness (skeleton)
- Inference scheduler skeleton (for Phase 5)

### Deferred to Future Phases
- Actual GPU compute (requires TensorRT, Phase 5)
- Model loading and inference (Phase 5)
- User/kernel separation (FUTURE.md)
- Filesystem integration (FUTURE.md)

---

*Last updated: December 2025*
