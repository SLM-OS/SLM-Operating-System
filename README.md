# CS-496-Capstone-SLM-Operating-System

This is a capstone project for the Computer Science Undergraduate program at Sonoma State University. It is a SLM Operating System designed to utilize and support the use of Small Language Models.

---

# SLM-OS: Small Language Model Operating System
## Architecture and Design Document
### Version 1.1 - December 2025

---

## Executive Summary

SLM-OS is a purpose-built operating system designed from the ground up to efficiently manage and orchestrate Small Language Model (SLM) workloads on embedded systems. Unlike traditional operating systems retrofitted for AI workloads, SLM-OS implements AI-aware scheduling, memory management, and resource allocation as core kernel primitives. This project targets industrial IoT applications requiring real-time AI inference with strict resource constraints.

### Key Innovations

- AI-first memory management with model-aware virtual memory
- Heterogeneous multi-core scheduling optimized for SLM inference
- Hot-swappable component architecture for runtime flexibility
- Zero-copy model sharing between processes and accelerators
- Deadline-aware scheduling with power optimization

### Target Hardware

- **Primary:** NVIDIA Jetson Orin Nano (6-core ARM Cortex-A78AE)
- **Secondary:** Raspberry Pi 5 (4-core ARM Cortex-A76)
- **Future:** STM32F446RE for ultra-lightweight deployments

---

## 1. System Architecture Overview

### 1.1 Three-Layer Design

```
┌────────────────────────────────────────────────────┐
│           Layer 3: SLM Components (Hot-swappable)  │
│   ┌─────────┐ ┌─────────┐ ┌──────────┐ ┌─────────┐ │
│   │Anomaly  │ │Resource │ │Predictive│ │Security │ │
│   │Detector │ │Scheduler│ │Maint.    │ │Monitor  │ │
│   └─────────┘ └─────────┘ └──────────┘ └─────────┘ │
├────────────────────────────────────────────────────┤
│           Layer 2: SLM Runtime Environment         │
│   ┌─────────────┐ ┌──────────────┐ ┌─────────────┐ │
│   │Model Loader │ │Inference Eng.│ │Component Mgr│ │
│   └─────────────┘ └──────────────┘ └─────────────┘ │
├────────────────────────────────────────────────────┤
│           Layer 1: Microkernel Core                │
│   ┌──────────┐ ┌──────────┐ ┌─────────┐ ┌────────┐ │
│   │Memory Mgr│ │Scheduler │ │IPC      │ │Drivers │ │
│   └──────────┘ └──────────┘ └─────────┘ └────────┘ │
└────────────────────────────────────────────────────┘
                        │
                  ┌─────┴─────┐
                  │ Hardware  │
                  └───────────┘
```

### 1.2 Design Principles

1. **AI-First:** Every kernel primitive considers AI workload characteristics
2. **Modularity:** Components can be developed, tested, and deployed independently
3. **Real-Time:** Predictable latencies for industrial applications
4. **Efficiency:** Minimal overhead for resource-constrained devices
5. **Future-Proof:** Architecture supports future additions (privilege levels, security)

### 1.3 Language Architecture

SLM-OS uses a dual-language approach to balance low-level hardware control with memory safety in higher-level components:

- **C:** Kernel core, hardware abstraction, drivers, and performance-critical primitives
- **Rust:** Runtime environment, component management, and SLM-aware policy logic

This separation places mechanism (how to do something) in C and policy (what to do) in Rust, with a well-defined FFI boundary between them.

---

## 2. Core Kernel Components

### 2.1 Memory Management

#### 2.1.1 Virtual Memory System

AI-Optimized Page Table Entry (C):

```c
typedef struct {
    uint64_t pfn          : 40;  // Physical frame number
    uint64_t present      : 1;   // Page present in memory
    uint64_t writable     : 1;   // Write permission
    uint64_t executable   : 1;   // Execute permission
    uint64_t user         : 1;   // User accessible (future)
    uint64_t cacheable    : 1;   // Model weights cacheable
    uint64_t gpu_mapped   : 1;   // Accessible by GPU/NPU
    uint64_t model_page   : 1;   // Contains model data
    uint64_t inference_hot: 1;   // Frequently accessed
    uint64_t dirty        : 1;   // Page modified
    uint64_t accessed     : 1;   // Page accessed
    uint64_t reserved     : 14;  // Future use
} slm_pte_t;
```

Two-Level Page Tables:

- Level 1: 512 entries × 1GB regions
- Level 2: 512 entries × 2MB pages
- Special support for model memory (large, contiguous allocations)

#### 2.1.2 Model Memory Regions

```c
typedef enum {
    MEM_REGION_KERNEL,     // Kernel code and data
    MEM_REGION_COMPONENT,  // Component code
    MEM_REGION_MODEL,      // Model weights (shareable)
    MEM_REGION_INFERENCE,  // Inference workspace
    MEM_REGION_DEVICE,     // Memory-mapped devices
} mem_region_type_t;

struct mem_region {
    void* base_addr;
    size_t size;
    mem_region_type_t type;
    uint32_t flags;  // SHAREABLE, GPU_ACCESSIBLE, etc.
};
```

#### 2.1.3 Zero-Copy Model Sharing

- Models loaded once, mapped into multiple process spaces
- GPU/NPU direct access without copying
- Reference counting for safe unloading

### 2.2 Multi-Core Scheduler

#### 2.2.1 Core Architecture Support

**Option A: Full big.LITTLE Aware Implementation (C)**

```c
typedef enum {
    CORE_TYPE_EFFICIENCY,   // Low power cores (e.g., Cortex-A55)
    CORE_TYPE_PERFORMANCE,  // High performance (e.g., Cortex-A78)
    CORE_TYPE_ACCELERATOR   // GPU/NPU cores
} core_type_t;

struct cpu_topology {
    uint32_t core_id;
    core_type_t type;
    uint32_t max_freq_mhz;
    uint32_t l2_cache_kb;
    uint32_t cluster_id;    // Cores sharing L2/L3
    float power_weight;     // Relative power consumption
};

// Discovered at boot via ARM registers
void detect_cpu_topology() {
    for (int i = 0; i < num_cores; i++) {
        uint64_t midr = read_sysreg_on_core(MIDR_EL1, i);
        uint32_t part = (midr >> 4) & 0xFFF;

        switch(part) {
            case 0xD41:  // Cortex-A78
                topology[i].type = CORE_TYPE_PERFORMANCE;
                topology[i].power_weight = 1.0;
                break;
            case 0xD05:  // Cortex-A55
                topology[i].type = CORE_TYPE_EFFICIENCY;
                topology[i].power_weight = 0.3;
                break;
        }
    }
}
```

**Option B: Simplified Frequency-Aware Implementation (C)**

```c
struct simple_core_info {
    uint32_t core_id;
    uint32_t current_freq_mhz;
    uint32_t load_percent;
    uint64_t idle_time_ns;
    bool preferred_for_inference;
};

// Simpler but still effective
void update_core_metrics() {
    for (int i = 0; i < num_cores; i++) {
        cores[i].current_freq_mhz = read_cpu_freq(i);
        cores[i].load_percent = calculate_load(i);

        // Simple heuristic: use less loaded, higher freq cores
        cores[i].preferred_for_inference =
            (cores[i].load_percent < 50) &&
            (cores[i].current_freq_mhz > avg_freq);
    }
}
```

#### 2.2.2 SLM-Aware Task Structure (C - Kernel Primitive)

```c
struct slm_task {
    // Standard task fields
    uint32_t pid;
    void* stack_ptr;
    task_state_t state;

    // SLM-specific fields
    model_handle_t loaded_model;  // Handle, not pointer (Rust owns model)
    uint64_t inference_deadline_ns;
    uint32_t inference_priority;
    size_t working_set_size;
    float ops_per_inference;

    // Scheduling hints
    core_type_t preferred_core_type;
    bool can_use_gpu;
    uint32_t max_batch_size;
};
```

#### 2.2.3 SLM-Aware Scheduling Policy (Rust)

```rust
use crate::kernel_ffi::{SlmTask, CoreType, assign_to_core, get_time_ns};

const URGENT_THRESHOLD_NS: u64 = 1_000_000; // 1ms

pub fn schedule_slm_task(task: &SlmTask) -> Result<CoreId, SchedulerError> {
    let time_to_deadline = task
        .inference_deadline_ns
        .saturating_sub(get_time_ns());

    if time_to_deadline < URGENT_THRESHOLD_NS {
        // Urgent: use fastest available core
        assign_to_performance_core(task)
    } else if task.working_set_size < L2_CACHE_SIZE {
        // Small model: efficiency core is fine
        assign_to_efficiency_core(task)
    } else {
        // Normal: load balance
        assign_to_least_loaded_core(task)
    }
}
```

### 2.3 Component System

#### 2.3.1 Component Specification

```yaml
# Example: anomaly_detector.yaml
name: anomaly-detector
version: 1.0.3
type: slm-component

resources:
  memory:
    min: 256MB
    max: 512MB
  cores:
    min: 1
    preferred_type: efficiency
  gpu: optional

models:
  - name: vibration-anomaly
    format: onnx
    size: 45MB
    ops: 1.2GFLOPS

interfaces:
  subscribes:
    - topic: /sensors/vibration
      message_type: float32_array
  publishes:
    - topic: /alerts/anomaly
      message_type: anomaly_event

dependencies:
  - slm-runtime >= 1.0
  - sensor-driver >= 2.0
```

#### 2.3.2 Component Lifecycle Manager (Rust)

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ComponentState {
    Loaded,
    Initializing,
    Running,
    Suspended,
    Updating,
    Terminating,
}

pub struct Component {
    name: String,
    state: ComponentState,
    code_base: *mut u8,
    code_size: usize,
    tasks: Vec<TaskHandle>,
    msg_queues: Vec<MessageQueueHandle>,
    models: Vec<ModelHandle>,
}

impl Component {
    /// Hot-swap a component with a new version
    pub fn hot_swap(old_name: &str, new_comp: Component) -> Result<(), SwapError> {
        let old = ComponentRegistry::find(old_name)?;

        // 1. Load new component
        new_comp.load_code()?;

        // 2. Pause old component
        old.set_state(ComponentState::Updating)?;

        // 3. Transfer state
        new_comp.transfer_state_from(&old)?;

        // 4. Update routing tables
        MessageRouter::update_routes(&old, &new_comp)?;

        // 5. Start new component
        new_comp.set_state(ComponentState::Running)?;

        // 6. Cleanup old
        old.unload()?;

        Ok(())
    }
}
```

### 2.4 Inter-Process Communication

```c
// Lightweight message passing for components (C - kernel primitive)
struct slm_message {
    uint32_t src_component_id;
    uint32_t dst_component_id;
    uint64_t timestamp_ns;
    uint32_t type;
    uint32_t size;
    uint8_t data[];  // Flexible array member
};

// Zero-copy for large model outputs
struct slm_shared_buffer {
    void* addr;
    size_t size;
    uint32_t ref_count;
    uint32_t owner_id;
    bool gpu_accessible;
};
```

---

## 3. SLM Runtime Environment

### 3.1 Model Loader (Rust)

```rust
pub struct ModelLoader {
    /// Supported model formats
    supported_formats: Vec<ModelFormat>,

    /// Memory pools for models
    weight_pool: MemoryPool,      // Read-only weights
    workspace_pool: MemoryPool,   // Inference workspace

    /// GPU integration
    gpu_ctx: Option<GpuContext>,
}

impl ModelLoader {
    /// Lazy loading support - called on page fault
    pub fn load_on_demand(
        &mut self,
        model: &mut Model,
        fault_addr: *const u8,
    ) -> Result<(), LoadError> {
        let offset = fault_addr as usize - model.base_addr as usize;
        let chunk_idx = offset / MODEL_CHUNK_SIZE;

        if !model.chunks[chunk_idx].loaded {
            self.load_chunk(model, chunk_idx)?;
            self.map_to_gpu_if_needed(model, chunk_idx)?;
        }

        Ok(())
    }
}
```

### 3.2 Inference Engine (Rust)

```rust
/// Pluggable inference backends
pub trait InferenceBackend: Send + Sync {
    fn name(&self) -> &str;
    fn init(&mut self, params: &BackendParams) -> Result<(), BackendError>;
    fn load_model(&mut self, model: &Model) -> Result<(), BackendError>;
    fn run_inference(
        &self,
        model: &Model,
        input: &[u8],
        output: &mut [u8],
    ) -> Result<(), BackendError>;
    fn cleanup(&mut self) -> Result<(), BackendError>;
}

/// Deadline-aware inference request
pub struct InferenceRequest {
    pub model: ModelHandle,
    pub input: Buffer,
    pub output: Buffer,
    pub deadline_ns: u64,
    pub callback: Option<Box<dyn FnOnce(InferenceResult) + Send>>,
}

pub struct InferenceScheduler {
    pending: Vec<InferenceRequest>,
}

impl InferenceScheduler {
    pub fn process_batch(&mut self) -> Result<(), SchedulerError> {
        // Sort by deadline
        self.pending.sort_by_key(|r| r.deadline_ns);

        // Group by model for efficiency
        let grouped = self.group_by_model();

        // Process highest priority batch
        self.run_batch(&grouped[0])
    }
}
```

---

## 4. Modular Integration Points

### 4.1 Kernel FFI Boundary

The boundary between C kernel code and Rust runtime is defined through a stable FFI interface:

```c
// kernel/include/slm_ffi.h - C side of FFI boundary

// Memory operations
int slm_map_region(void* virt, uint64_t phys, size_t size, uint32_t flags);
int slm_unmap_region(void* virt, size_t size);
void* slm_alloc_pages(size_t count, uint32_t flags);
void slm_free_pages(void* addr, size_t count);

// Task operations
int slm_task_create(struct slm_task* task);
int slm_task_destroy(uint32_t pid);
int slm_task_set_affinity(uint32_t pid, uint32_t core_mask);

// IPC operations
int slm_msg_send(uint32_t dst_id, struct slm_message* msg);
int slm_msg_recv(uint32_t src_id, struct slm_message* msg, uint64_t timeout_ns);

// Time operations
uint64_t slm_get_time_ns(void);
```

```rust
// runtime/src/kernel_ffi.rs - Rust side of FFI boundary

#[repr(C)]
pub struct SlmTask {
    pub pid: u32,
    pub stack_ptr: *mut u8,
    pub state: TaskState,
    // ... matches C struct exactly
}

extern "C" {
    pub fn slm_map_region(
        virt: *mut u8,
        phys: u64,
        size: usize,
        flags: u32,
    ) -> i32;

    pub fn slm_get_time_ns() -> u64;

    // ... other kernel functions
}

/// Safe wrapper around kernel FFI
pub mod kernel {
    use super::*;

    pub fn get_time_ns() -> u64 {
        unsafe { slm_get_time_ns() }
    }

    pub fn map_region(
        virt: *mut u8,
        phys: u64,
        size: usize,
        flags: MemFlags,
    ) -> Result<(), KernelError> {
        let result = unsafe {
            slm_map_region(virt, phys, size, flags.bits())
        };
        if result == 0 {
            Ok(())
        } else {
            Err(KernelError::from_code(result))
        }
    }
}
```

### 4.2 External Components

```c
// Pluggable subsystems interface (C - for external library integration)
struct kernel_modules {
    // Networking (lwIP integration)
    struct {
        int (*init)(void);
        int (*send)(const void* data, size_t len);
        int (*receive)(void* buffer, size_t len);
    } net;

    // Filesystem (LittleFS integration)
    struct {
        int (*mount)(const char* device);
        int (*read)(int fd, void* buf, size_t count);
        int (*write)(int fd, const void* buf, size_t count);
    } fs;

    // Future: Graphics, USB, etc.
};
```

### 4.3 Driver Framework

```c
// Simple driver interface (C)
struct driver {
    const char* name;
    driver_type_t type;
    int (*probe)(device_t* dev);
    int (*init)(device_t* dev);
    int (*read)(device_t* dev, void* buf, size_t len);
    int (*write)(device_t* dev, const void* buf, size_t len);
    int (*ioctl)(device_t* dev, uint32_t cmd, void* arg);
};
```

---

## 5. Implementation Roadmap

### 5.1 Month-by-Month Plan

**Month 1: Minimal Kernel (C)**

- ARM64 boot process (UEFI stub)
- Basic memory management (physical allocator)
- UART driver for debugging
- Simple round-robin scheduler

**Month 2: Core Features (C + Rust scaffolding)**

- Virtual memory with 2-level page tables
- Multi-core boot and scheduling
- Basic IPC (message queues)
- Rust toolchain integration and FFI boundary

**Month 3: AI Infrastructure (C kernel + Rust runtime)**

- Model memory management
- GPU initialization (Jetson)
- Deadline-aware scheduler (policy in Rust)
- Zero-copy buffer sharing

**Month 4: Component System (Rust)**

- Hot-swap mechanism
- Component isolation
- Message routing
- State transfer

**Month 5: SLM Integration (Rust + C GPU glue)**

- ONNX model loader
- TensorRT Lite integration
- Inference runtime
- First 2-3 components

**Month 6: Demo & Polish**

- Complete industrial demo
- Performance benchmarks
- Documentation
- Final optimizations

### 5.2 Testing Strategy

**Unit Tests:**

- Memory allocator correctness (C tests)
- Scheduler fairness (C tests)
- IPC reliability (C tests)
- Component lifecycle (Rust tests)
- Model loading (Rust tests)

**Integration Tests:**

- Component loading/unloading
- Model inference pipeline
- Multi-core synchronization
- C/Rust FFI boundary

**System Tests:**

- End-to-end industrial scenarios
- Stress testing with multiple models
- Power consumption analysis

---

## 6. Future Enhancements

### 6.1 Security Features

- User/kernel privilege separation
- Component sandboxing
- Secure boot support
- Encrypted model storage

### 6.2 Advanced Features

- Distributed operation across multiple boards
- Dynamic model compilation
- Advanced power management
- Real-time guarantees (RT patches)

### 6.3 Ecosystem

- Component marketplace
- Development SDK
- Debugging tools
- Performance profilers

---

## 7. Key Design Decisions

### 7.1 Technology Choices

| Component | Choice | Rationale |
|-----------|--------|-----------|
| Kernel Language | C | Direct hardware control, proven for bare-metal, extensive ARM64 references |
| Runtime Language | Rust | Memory safety without GC, RAII for resource management, zero-cost abstractions |
| Build System | CMake (C) + Cargo (Rust) | Industry standard tools for each language |
| Bootloader | U-Boot chainload | Simplified early development |
| Initial Filesystem | In-memory only | Focus on core OS features |
| Component Format | Simplified ELF | Easier parsing, sufficient features |

### 7.2 Language Boundary

| Layer | Language | Components |
|-------|----------|------------|
| Boot & HAL | C + Assembly | `boot.S`, `mmu.c`, `context.S`, drivers |
| Kernel Primitives | C | Physical memory, IPC primitives, task struct |
| Scheduling Policy | Rust | Deadline-aware scheduling, load balancing |
| Runtime | Rust | Model loader, inference engine, component manager |
| Components | Rust | Anomaly detector, resource scheduler, etc. |
| GPU Glue | C | CUDA/TensorRT API calls |

### 7.3 Simplifications

1. Single address space initially (but designed for future separation)
2. Cooperative GPU scheduling (models must yield)
3. Fixed-size model pages (2MB "huge" pages for AI)
4. No swap (all models fit in RAM)
5. Simple security model (trust all components initially)

---

## 8. Success Metrics

### 8.1 Performance Goals

- Boot time: < 2 seconds
- Context switch: < 10 microseconds
- Model load time: < 100ms for 50MB model
- Inference scheduling overhead: < 5%

### 8.2 Capability Demonstrations

- Hot-swap component without system restart
- Run 3+ SLMs concurrently on Jetson
- Show deadline-aware scheduling improving latency
- Demonstrate power savings with heterogeneous scheduling

### 8.3 Research Contributions

- Novel SLM-aware memory management
- AI-first scheduler design
- Hot-swappable AI component architecture
- Empirical comparison with Linux for AI workloads

---

## Appendix A: Code Repository Structure

```
slm-os/
├── kernel/                      # C code
│   ├── arch/                    # Architecture-specific (ARM64)
│   │   ├── arm64/
│   │   │   ├── boot.S
│   │   │   ├── mmu.c
│   │   │   └── context.S
│   │   └── include/
│   ├── mm/                      # Memory management
│   │   ├── pmm.c                # Physical memory manager
│   │   ├── vmm.c                # Virtual memory manager
│   │   └── allocator.c
│   ├── sched/                   # Core scheduler primitives
│   │   ├── core.c               # Core scheduler
│   │   ├── task.c               # Task management
│   │   └── smp.c                # Multi-core support
│   ├── ipc/                     # Inter-process communication
│   │   ├── message.c
│   │   ├── shared_buffer.c
│   │   └── queue.c
│   ├── drivers/                 # Device drivers
│   │   ├── uart.c
│   │   ├── gpio.c
│   │   └── i2c.c
│   ├── include/                 # Kernel headers
│   │   ├── slm_ffi.h            # FFI boundary definitions
│   │   └── ...
│   └── gpu/                     # GPU integration (C for NVIDIA APIs)
│       ├── jetson_gpu.c
│       ├── memory_map.c
│       └── cuda_lite.c
│
├── runtime/                     # Rust code
│   ├── Cargo.toml
│   └── src/
│       ├── lib.rs
│       ├── kernel_ffi.rs        # FFI bindings to C kernel
│       ├── loader/              # Model/component loader
│       │   ├── mod.rs
│       │   ├── elf_loader.rs
│       │   ├── onnx_loader.rs
│       │   └── component.rs
│       ├── inference/           # Inference engine
│       │   ├── mod.rs
│       │   ├── engine.rs
│       │   ├── batch.rs
│       │   └── backends/
│       │       ├── mod.rs
│       │       └── tensorrt.rs
│       ├── sched/               # SLM-aware scheduling policy
│       │   ├── mod.rs
│       │   ├── deadline.rs
│       │   └── heterogeneous.rs
│       └── mm/                  # Model memory management
│           ├── mod.rs
│           └── model_mem.rs
│
├── components/                  # Example SLM components (Rust)
│   ├── anomaly-detector/
│   │   ├── Cargo.toml
│   │   ├── manifest.yaml
│   │   ├── src/
│   │   │   └── lib.rs
│   │   └── models/
│   ├── resource-scheduler/
│   │   ├── Cargo.toml
│   │   ├── manifest.yaml
│   │   └── src/
│   │       └── lib.rs
│   └── predictive-maintenance/
│       ├── Cargo.toml
│       ├── manifest.yaml
│       └── src/
│           └── lib.rs
│
├── tools/                       # Build and debug tools
│   ├── mkimage/                 # OS image creator
│   ├── component-pack/          # Component packager
│   └── debugger/                # Kernel debugger
│
├── docs/                        # Documentation
│   ├── architecture.md
│   ├── api/
│   └── tutorials/
│
├── tests/                       # Test suites
│   ├── unit/
│   ├── integration/
│   └── benchmarks/
│
├── CMakeLists.txt               # C kernel build
├── Cargo.toml                   # Workspace root for Rust
└── Makefile                     # Top-level orchestrator
```

---

## Appendix B: Hardware Resources

### NVIDIA Jetson Orin Nano

**CPU:**
- 6-core ARM Cortex-A78AE CPU
- Up to 1.5 GHz
- 32KB L1 I-cache, 64KB L1 D-cache per core
- 256KB L2 cache per core
- 4MB L3 cache shared

**GPU:**
- 1024-core Ampere GPU
- 625 MHz - 1 GHz
- 32 Tensor Cores
- 8 TFLOPS AI performance

**Memory:**
- 8GB 128-bit LPDDR5 @ 102.4 GB/s

**Hardware Accelerators:**
- 2x NVDLA v2.0
- PVA v2.0 (Vision Accelerator)
- 2x Multi-Standard Video Encoders

**I/O Interfaces:**
- 7x UART, 3x SPI, 6x I2C
- 4x USB 3.2, 1x USB 2.0
- PCIe Gen4 x4
- Gigabit Ethernet

### Raspberry Pi 5

**CPU:**
- 4-core ARM Cortex-A76 CPU @ 2.4GHz
- 64KB L1 cache per core
- 512KB L2 cache per core

**GPU:**
- Broadcom VideoCore VII GPU @ 800 MHz

**Memory:**
- 4GB LPDDR4X-4267

**I/O Interfaces:**
- 2x 4-lane MIPI camera/display
- PCIe 2.0 x1
- 2x USB 3.0, 2x USB 2.0
- Gigabit Ethernet
- Standard 40-pin GPIO header

### Development Tools

**Hardware:**
- Digital oscilloscope (debugging hardware timings)
- Logic/Protocol analyzer (I2C, SPI, UART debugging)
- USB-to-UART adapters (console access)
- JTAG debugger (optional but recommended)
- Power measurement setup:
  - Current shunt + multimeter
  - Or dedicated power monitor
- SD cards and USB drives for booting

**Software:**
- Cross-compilation toolchain:
  - aarch64-linux-gnu-gcc
  - LLVM/Clang for ARM64
  - Rust with `aarch64-unknown-none` target
- QEMU for early testing
- GDB with ARM64 support
- Device tree compiler (dtc)

---

## Appendix C: Quick Reference

### Memory Layout (Virtual)

```
0x0000_0000_0000_0000 - 0x0000_0000_3FFF_FFFF : User space (future)
0x0000_0000_4000_0000 - 0x0000_00FF_FFFF_FFFF : Component space
0x0000_0100_0000_0000 - 0x0000_01FF_FFFF_FFFF : Model memory
0xFFFF_0000_0000_0000 - 0xFFFF_7FFF_FFFF_FFFF : Kernel space
0xFFFF_8000_0000_0000 - 0xFFFF_FFFF_FFFF_FFFF : Device memory
```

### Component Message Format

```
[Header: 16 bytes]
  [Type: 4 bytes][Size: 4 bytes][Timestamp: 8 bytes]
[Source ID: 4 bytes][Dest ID: 4 bytes][Flags: 4 bytes][Reserved: 4 bytes]
[Payload: variable]
```

---

*This is a living document that will be updated throughout the development process.*
