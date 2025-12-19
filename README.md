# CS-496-Capstone-SLM-Operating-System
This is a capstone project for the Computer Science Undergraduate program at Sonoma State University. It is a SLM Operating System design to utilize and support the use of Small Language Models
# SLM-OS: Small Language Model Operating System
## Architecture and Design Document
### Version 1.0 - December 2025

---

## Executive Summary

SLM-OS is a purpose-built operating system designed from the ground up to efficiently manage and orchestrate Small Language Model (SLM) workloads on embedded systems. Unlike traditional operating systems retrofitted for AI workloads, SLM-OS implements AI-aware scheduling, memory management, and resource allocation as core kernel primitives. This project targets industrial IoT applications requiring real-time AI inference with strict resource constraints.

**Key Innovations:**
- AI-first memory management with model-aware virtual memory
- Heterogeneous multi-core scheduling optimized for SLM inference
- Hot-swappable component architecture for runtime flexibility
- Zero-copy model sharing between processes and accelerators
- Deadline-aware scheduling with power optimization

**Target Hardware:**
- Primary: NVIDIA Jetson Orin Nano (6-core ARM Cortex-A78AE)
- Secondary: Raspberry Pi 5 (4-core ARM Cortex-A76)
- Future: STM32F446RE for ultra-lightweight deployments

- ## 1. System Architecture Overview

### 1.1 Three-Layer Design

┌─────────────────────────────────────────────────────┐
│  Layer 3: SLM Components (Hot-swappable)            │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐  │
│  │Anomaly  │ │Resource │ │Predictive│ │Security │  │
│  │Detector │ │Scheduler│ │Maint.    │ │Monitor  │  │
│  └─────────┘ └─────────┘ └─────────┘ └─────────┘  │
├─────────────────────────────────────────────────────┤
│  Layer 2: SLM Runtime Environment                    │
│  ┌─────────────┐ ┌──────────────┐ ┌─────────────┐ │
│  │Model Loader │ │Inference Eng.│ │Component Mgr│ │
│  └─────────────┘ └──────────────┘ └─────────────┘ │
├─────────────────────────────────────────────────────┤
│  Layer 1: Microkernel Core                          │
│  ┌──────────┐ ┌──────────┐ ┌─────────┐ ┌────────┐│
│  │Memory Mgr│ │Scheduler │ │IPC      │ │Drivers ││
│  └──────────┘ └──────────┘ └─────────┘ └────────┘│
└─────────────────────────────────────────────────────┘
                          │
                    ┌─────┴─────┐
                    │ Hardware  │
                    └───────────┘

### 1.2 Design Principles

1. **AI-First**: Every kernel primitive considers AI workload characteristics
2. **Modularity**: Components can be developed, tested, and deployed independently
3. **Real-Time**: Predictable latencies for industrial applications
4. **Efficiency**: Minimal overhead for resource-constrained devices
5. **Future-Proof**: Architecture supports future additions (privilege levels, security)




## 2. Core Kernel Components

### 2.1 Memory Management

#### 2.1.1 Virtual Memory System

**AI-Optimized Page Table Entry:**
```c
typedef struct {
    uint64_t pfn : 40;           // Physical frame number
    uint64_t present : 1;        // Page present in memory
    uint64_t writable : 1;       // Write permission
    uint64_t executable : 1;     // Execute permission
    uint64_t user : 1;           // User accessible (future)
    uint64_t cacheable : 1;      // Model weights cacheable
    uint64_t gpu_mapped : 1;     // Accessible by GPU/NPU
    uint64_t model_page : 1;     // Contains model data
    uint64_t inference_hot : 1;  // Frequently accessed
    uint64_t dirty : 1;          // Page modified
    uint64_t accessed : 1;       // Page accessed
    uint64_t reserved : 14;      // Future use
} slm_pte_t;
```

**Two-Level Page Tables:**
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
    uint32_t flags;        // SHAREABLE, GPU_ACCESSIBLE, etc.
};
```

#### 2.1.3 Zero-Copy Model Sharing

- Models loaded once, mapped into multiple process spaces
- GPU/NPU direct access without copying
- Reference counting for safe unloading


### 2.2 Multi-Core Scheduler

#### 2.2.1 Core Architecture Support

**Option A: Full big.LITTLE Aware Implementation**
```c
typedef enum {
    CORE_TYPE_EFFICIENCY,  // Low power cores (e.g., Cortex-A55)
    CORE_TYPE_PERFORMANCE, // High performance (e.g., Cortex-A78)
    CORE_TYPE_ACCELERATOR  // GPU/NPU cores
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
            case 0xD41: // Cortex-A78
                topology[i].type = CORE_TYPE_PERFORMANCE;
                topology[i].power_weight = 1.0;
                break;
            case 0xD05: // Cortex-A55
                topology[i].type = CORE_TYPE_EFFICIENCY;
                topology[i].power_weight = 0.3;
                break;
        }
    }
}
```

**Option B: Simplified Frequency-Aware Implementation**
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

#### 2.2.2 SLM-Aware Scheduling
```c
struct slm_task {
    // Standard task fields
    uint32_t pid;
    void* stack_ptr;
    task_state_t state;
    
    // SLM-specific fields
    model_t* loaded_model;
    uint64_t inference_deadline_ns;
    uint32_t inference_priority;
    size_t working_set_size;
    float ops_per_inference;
    
    // Scheduling hints
    core_type_t preferred_core_type;
    bool can_use_gpu;
    uint32_t max_batch_size;
};

// Deadline-aware scheduler
int schedule_slm_task(struct slm_task* task) {
    uint64_t time_to_deadline = task->inference_deadline_ns - get_time_ns();
    
    if (time_to_deadline < URGENT_THRESHOLD_NS) {
        // Urgent: use fastest available core
        return assign_to_performance_core(task);
    } else if (task->working_set_size < L2_CACHE_SIZE) {
        // Small model: efficiency core is fine
        return assign_to_efficiency_core(task);
    } else {
        // Normal: load balance
        return assign_to_least_loaded_core(task);
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

#### 2.3.2 Component Lifecycle Manager
```c
typedef enum {
    COMP_STATE_LOADED,
    COMP_STATE_INITIALIZING,
    COMP_STATE_RUNNING,
    COMP_STATE_SUSPENDED,
    COMP_STATE_UPDATING,
    COMP_STATE_TERMINATING
} component_state_t;

struct component {
    char name[64];
    component_state_t state;
    void* code_base;
    size_t code_size;
    struct slm_task* tasks[MAX_TASKS_PER_COMPONENT];
    message_queue_t* msg_queues[MAX_QUEUES];
    model_t* models[MAX_MODELS_PER_COMPONENT];
};

// Hot-swap support
int hot_swap_component(const char* name, struct component* new_comp) {
    struct component* old = find_component(name);
    
    // 1. Load new component
    load_component_code(new_comp);
    
    // 2. Pause old component
    set_component_state(old, COMP_STATE_UPDATING);
    
    // 3. Transfer state
    transfer_component_state(old, new_comp);
    
    // 4. Update routing tables
    update_message_routes(old, new_comp);
    
    // 5. Start new component
    set_component_state(new_comp, COMP_STATE_RUNNING);
    
    // 6. Cleanup old
    unload_component(old);
    
    return 0;
}
```

### 2.4 Inter-Process Communication
```c
// Lightweight message passing for components
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


## 3. SLM Runtime Environment

### 3.1 Model Loader
```c
struct model_loader {
    // Supported formats
    model_format_t supported_formats[MAX_FORMATS];
    
    // Memory pools for models
    struct mem_pool* weight_pool;    // Read-only weights
    struct mem_pool* workspace_pool; // Inference workspace
    
    // GPU integration
    gpu_context_t* gpu_ctx;
};

// Lazy loading support
int load_model_on_demand(model_t* model, void* fault_addr) {
    size_t offset = fault_addr - model->base_addr;
    size_t chunk_idx = offset / MODEL_CHUNK_SIZE;
    
    if (!model->chunks[chunk_idx].loaded) {
        load_model_chunk(model, chunk_idx);
        map_to_gpu_if_needed(model, chunk_idx);
    }
    
    return 0;
}
```

### 3.2 Inference Engine
```c
// Pluggable inference backends
struct inference_backend {
    const char* name;
    int (*init)(void* params);
    int (*load_model)(model_t* model);
    int (*run_inference)(model_t* model, void* input, void* output);
    int (*cleanup)(void);
};

// Deadline-aware batch processing
struct inference_request {
    model_t* model;
    void* input;
    void* output;
    uint64_t deadline_ns;
    inference_callback_t callback;
};

int process_inference_batch() {
    // Sort by deadline
    sort_requests_by_deadline(pending_requests);
    
    // Group by model for efficiency
    group_requests_by_model(pending_requests);
    
    // Process highest priority batch
    return run_batch_inference(pending_requests[0]);
}
```

---

## 4. Modular Integration Points

### 4.1 External Components
```c
// Pluggable subsystems interface
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

### 4.2 Driver Framework
```c
// Simple driver interface
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


## 5. Implementation Roadmap

### 5.1 Month-by-Month Plan

**Month 1: Minimal Kernel**
- [ ] ARM64 boot process (UEFI stub)
- [ ] Basic memory management (physical allocator)
- [ ] UART driver for debugging
- [ ] Simple round-robin scheduler

**Month 2: Core Features**
- [ ] Virtual memory with 2-level page tables
- [ ] Multi-core boot and scheduling
- [ ] Basic IPC (message queues)
- [ ] Initial component loader

**Month 3: AI Infrastructure**
- [ ] Model memory management
- [ ] GPU initialization (Jetson)
- [ ] Deadline-aware scheduler
- [ ] Zero-copy buffer sharing

**Month 4: Component System**
- [ ] Hot-swap mechanism
- [ ] Component isolation
- [ ] Message routing
- [ ] State transfer

**Month 5: SLM Integration**
- [ ] ONNX model loader
- [ ] TensorRT Lite integration
- [ ] Inference runtime
- [ ] First 2-3 components

**Month 6: Demo & Polish**
- [ ] Complete industrial demo
- [ ] Performance benchmarks
- [ ] Documentation
- [ ] Final optimizations

### 5.2 Testing Strategy

**Unit Tests:**
- Memory allocator correctness
- Scheduler fairness
- IPC reliability

**Integration Tests:**
- Component loading/unloading
- Model inference pipeline
- Multi-core synchronization

**System Tests:**
- End-to-end industrial scenarios
- Stress testing with multiple models
- Power consumption analysis

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
|-----------|---------|-----------|
| Language | C with minimal C++ | Performance, control, simplicity |
| Build System | CMake + custom | Flexibility for cross-compilation |
| Bootloader | U-Boot chainload | Simplified early development |
| Initial Filesystem | In-memory only | Focus on core OS features |
| Component Format | Simplified ELF | Easier parsing, sufficient features |

### 7.2 Simplifications

1. **Single address space** initially (but designed for future separation)
2. **Cooperative GPU scheduling** (models must yield)
3. **Fixed-size model pages** (2MB "huge" pages for AI)
4. **No swap** (all models fit in RAM)
5. **Simple security model** (trust all components initially)

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
├── kernel/
│   ├── arch/           # Architecture-specific (ARM64)
│   │   ├── arm64/
│   │   │   ├── boot.S
│   │   │   ├── mmu.c
│   │   │   └── context.S
│   │   └── include/
│   ├── mm/             # Memory management
│   │   ├── pmm.c       # Physical memory manager
│   │   ├── vmm.c       # Virtual memory manager
│   │   ├── model_mem.c # Model-specific memory
│   │   └── allocator.c
│   ├── sched/          # Scheduler
│   │   ├── core.c      # Core scheduler
│   │   ├── slm_sched.c # SLM-aware scheduling
│   │   ├── deadline.c  # Deadline scheduler
│   │   └── heterogeneous.c
│   ├── ipc/            # Inter-process communication
│   │   ├── message.c
│   │   ├── shared_buffer.c
│   │   └── queue.c
│   └── drivers/        # Device drivers
│       ├── uart.c
│       ├── gpio.c
│       └── i2c.c
├── runtime/
│   ├── loader/         # Model/component loader
│   │   ├── elf_loader.c
│   │   ├── onnx_loader.c
│   │   └── component.c
│   ├── inference/      # Inference engine
│   │   ├── engine.c
│   │   ├── batch.c
│   │   └── backends/
│   └── gpu/            # GPU integration
│       ├── jetson_gpu.c
│       ├── memory_map.c
│       └── cuda_lite.c
├── components/         # Example SLM components
│   ├── anomaly-detector/
│   │   ├── manifest.yaml
│   │   ├── src/
│   │   └── models/
│   ├── resource-scheduler/
│   │   ├── manifest.yaml
│   │   ├── src/
│   │   └── models/
│   └── predictive-maintenance/
│       ├── manifest.yaml
│       ├── src/
│       └── models/
├── tools/              # Build and debug tools
│   ├── mkimage/        # OS image creator
│   ├── component-pack/ # Component packager
│   └── debugger/       # Kernel debugger
├── docs/               # Documentation
│   ├── architecture.md
│   ├── api/
│   └── tutorials/
└── tests/              # Test suites
    ├── unit/
    ├── integration/
    └── benchmarks/
```

---

## Appendix B: Hardware Resources

**NVIDIA Jetson Orin Nano:**
- 6-core ARM Cortex-A78AE CPU
  - Up to 1.5 GHz
  - 32KB L1 I-cache, 64KB L1 D-cache per core
  - 256KB L2 cache per core
  - 4MB L3 cache shared
- 1024-core Ampere GPU
  - 625 MHz - 1 GHz
  - 32 Tensor Cores
  - 8 TFLOPS AI performance
- 8GB 128-bit LPDDR5 @ 102.4 GB/s
- Hardware accelerators:
  - 2x NVDLA v2.0
  - PVA v2.0 (Vision Accelerator)
  - 2x Multi-Standard Video Encoders
- I/O Interfaces:
  - 7x UART, 3x SPI, 6x I2C
  - 4x USB 3.2, 1x USB 2.0
  - PCIe Gen4 x4
  - Gigabit Ethernet

**Raspberry Pi 5:**
- 4-core ARM Cortex-A76 CPU @ 2.4GHz
  - 64KB L1 cache per core
  - 512KB L2 cache per core
- Broadcom VideoCore VII GPU @ 800 MHz
- 4GB LPDDR4X-4267
- I/O Interfaces:
  - 2x 4-lane MIPI camera/display
  - PCIe 2.0 x1
  - 2x USB 3.0, 2x USB 2.0
  - Gigabit Ethernet
  - Standard 40-pin GPIO header

**Development Tools:**
- Digital oscilloscope (debugging hardware timings)
- Logic/Protocol analyzer (I2C, SPI, UART debugging)
- USB-to-UART adapters (console access)
- JTAG debugger (optional but recommended)
- Power measurement setup:
  - Current shunt + multimeter
  - Or dedicated power monitor
- SD cards and USB drives for booting

**Software Tools:**
- Cross-compilation toolchain:
  - aarch64-linux-gnu-gcc
  - LLVM/Clang for ARM64
- QEMU for early testing
- GDB with ARM64 support
- Device tree compiler (dtc)

---

## Appendix C: Quick Reference

**Memory Layout (Virtual):**
```
0x0000_0000_0000_0000 - 0x0000_0000_3FFF_FFFF : User space (future)
0x0000_0000_4000_0000 - 0x0000_00FF_FFFF_FFFF : Component space
0x0000_0100_0000_0000 - 0x0000_01FF_FFFF_FFFF : Model memory
0xFFFF_0000_0000_0000 - 0xFFFF_7FFF_FFFF_FFFF : Kernel space
0xFFFF_8000_0000_0000 - 0xFFFF_FFFF_FFFF_FFFF : Device memory
```

**Component Message Format:**
```
[Header: 16 bytes]
[Type: 4 bytes][Size: 4 bytes][Timestamp: 8 bytes]
[Source ID: 4 bytes][Dest ID: 4 bytes][Flags: 4 bytes][Reserved: 4 bytes]
[Payload: variable]
```

---

*This is a living document that will be updated throughout the development process.*



