# SLM-OS Architecture

High-level architecture documentation for the Small Language Model Operating System.

**Status:** Phase 4+ (April 2026)

---

## Overview

SLM-OS is a bare-metal operating system designed for running AI inference workloads on edge devices. The system is built as a hybrid C/Rust kernel with specialized support for AI model memory management and deadline-aware scheduling. Development targets multiple ARM64 platforms (QEMU virt, Raspberry Pi 5, NVIDIA Jetson Orin Nano) with an experimental x86-64 port.

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
│   │  │  PMM   │ │  VMM   │ │ Sched  │ │  IPC   │ │  VFS   │ │  GPU   │  │   │
│   │  │        │ │  MMU   │ │        │ │        │ │   FS   │ │  HAL   │  │   │
│   │  └────────┘ └────────┘ └────────┘ └────────┘ └────────┘ └────────┘  │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                         │
│                                    ▼                                         │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                     Hardware Abstraction                             │   │
│   │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐   │   │
│   │  │UART PL011│ │ RP1 UART │ │ GIC-400  │ │ARM Timer │ │ Block Dev│   │   │
│   │  └──────────┘ └──────────┘ └──────────┘ └──────────┘ └──────────┘   │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                    │                                         │
│                                    ▼                                         │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                    Hardware (ARM64 / Cortex-A78AE / Cortex-A76)      │   │
│   │       QEMU virt │ Raspberry Pi 5 │ Jetson Orin Nano │ x86-64 (exp.) │   │
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

- **Device Tree parsing** for runtime hardware discovery (with fallback)
- **Compile-time platform selection** via CMake
- **Common driver interfaces** (UART, timer, interrupt controller)
- **QEMU virt** as primary development platform
- **Raspberry Pi 5** (BCM2712) as secondary hardware target — boots to interactive shell
- **Jetson Orin Nano** as original hardware target (blocked by CBB firewall)
- **x86-64** experimental port (multiboot2 boot, serial output)

---

## Subsystem Overview

### Platform Support

| Component | File(s) | Purpose |
|-----------|---------|---------|
| DTB Parser | `kernel/src/dtb.c` | Device Tree parsing for hardware discovery |
| Platform Info | `kernel/include/platform.h` | Compile-time fallback values |
| RP1 UART Driver | `kernel/drivers/uart_rp1_bitbang.c` | Pi 5 UART via RP1 southbridge |
| x86-64 Boot | `kernel/arch/x86_64/boot.S`, `entry64.S` | Multiboot2 boot, long mode entry |
| x86-64 Main | `kernel/arch/x86_64/main_x86.c` | x86-64 kernel entry point |

**Supported Platforms:**

| Platform | Status | Notes |
|----------|--------|-------|
| QEMU virt | Primary development | Full feature set, VirtIO-Net networking |
| Raspberry Pi 5 | Hardware target | Boots to interactive shell, UART TX/RX working, preemptive scheduling at 100 Hz. See `docs/pi5-baremetal-status.md` |
| Jetson Orin Nano | Blocked | CBB firewall prevents bare-metal peripheral access. See `docs/jetson-nvidia-support.md` |
| x86-64 | Experimental | Multiboot2 boot, serial output, basic subsystem init |

**Phase 3 Learnings:**
- DTB passed in x0 by bootloader (U-Boot, UEFI)
- QEMU ELF boot doesn't pass DTB (x0 is NULL)
- Fallback to compile-time defaults ensures reliable boot

### Memory Management

| Component | File(s) | Purpose |
|-----------|---------|---------|
| PMM | `kernel/mm/pmm.c` | Physical page allocation (buddy allocator) |
| VMM | `kernel/mm/vmm.c` | Virtual memory mapping |
| MMU | `kernel/arch/arm64/mmu.S` | Page table management (ARMv8) |
| Model Memory | `runtime/src/mm/` | 2MB-aligned model weight/workspace pools |

**Phase 4 Updates:**
- Buddy allocator replaces bitmap allocator for O(log n) allocation
- Orders 0-18 (4KB to 1GB blocks) with automatic splitting/coalescing
- Reduces fragmentation for large model allocations
- 2MB block alignment dramatically reduces TLB misses for large models
- Separate pools for weights vs workspace simplifies sharing semantics
- Generation counters in handles detect use-after-free

### Scheduling

| Component | File(s) | Purpose |
|-----------|---------|---------|
| Scheduler | `kernel/sched/sched.c` | Per-CPU run queues, priority ordering |
| Task Management | `kernel/sched/task.c` | Task lifecycle, context switch |
| Context Switch | `kernel/arch/arm64/context.S` | Register save/restore |
| PI Mutex | `kernel/ipc/pi_mutex.c` | Priority-inheriting mutex |
| Deadline Policy | `runtime/src/sched/deadline.rs` | Deadline analysis, core hints |
| Heterogeneous | `runtime/src/sched/heterogeneous.rs` | big.LITTLE topology awareness |

**Phase 3 Learnings:**
- Deadline boost thresholds (10ms/50ms/100ms) provide good responsiveness
- Priority inheritance prevents unbounded priority inversion
- Core isolation useful for latency-sensitive inference tasks

### Inter-Process Communication

| Component | File(s) | Purpose |
|-----------|---------|---------|
| Message Queues | `kernel/ipc/ipc.c` | Synchronous message passing |
| Shared Memory | `kernel/ipc/ipc.c` | Zero-copy buffer sharing |

**Phase 3 Learnings:**
- Timeout support essential for robust applications
- Statistics tracking helps debug queue sizing issues
- Single-threaded stress tests more reliable than multi-task in early development

### SMP Support

| Component | File(s) | Purpose |
|-----------|---------|---------|
| SMP Boot | `kernel/sched/smp.c`, `smp_boot.S` | Secondary CPU initialization |
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

### Filesystem Subsystem

| Component | File(s) | Purpose |
|-----------|---------|---------|
| VFS | `kernel/src/vfs.c` | Unified namespace, mount points |
| Block Device | `kernel/drivers/blkdev.c` | Storage device abstraction |
| RAM Disk | `kernel/drivers/ramdisk.c` | Memory-backed block device |
| LittleFS Wrapper | `kernel/fs/littlefs_slm.c` | Flash filesystem integration |
| LittleFS VFS | `kernel/fs/littlefs_vfs.c` | VFS adapter for LittleFS |
| String Functions | `kernel/src/string.c` | Freestanding libc string ops |

**Phase 4 Implementation:**
- Block device abstraction for hardware-independent storage
- RAM disk driver for development/testing (no hardware dependencies)
- LittleFS for flash-friendly persistent storage
- VFS mount point support unifies virtual and persistent files
- Shell access via `ls` and `cat` commands

### Networking Subsystem

| Component | File(s) | Purpose |
|-----------|---------|---------|
| VirtIO-Net | `kernel/drivers/virtio_net.c` | Network driver (QEMU) |
| lwIP Wrapper | `kernel/net/lwip_slm.c` | TCP/IP stack integration |
| OS Abstraction | `kernel/net/sys_arch.c` | lwIP platform layer |
| Shell Commands | `kernel/src/net_shell.c` | ping, ifconfig, netstat |

**Phase 4 Implementation:**
- lwIP TCP/IP stack for ICMP, TCP, UDP, DHCP
- VirtIO-Net driver for QEMU virtual networking
- Shell commands: `net`, `ping`, `ifconfig`, `netstat`
- Static IP and DHCP configuration support

### Lua Scripting Engine

| Component | File(s) | Purpose |
|-----------|---------|---------|
| Lua Integration | `kernel/src/lua_slm.c` | Lua 5.4 state management and kernel bindings |
| Libc Stubs | `kernel/src/lua_stubs.c` | Freestanding libc shims for Lua runtime |
| Shell Commands | `kernel/src/lua_shell.c` | `lua` command: REPL, inline exec, script files |

**Phase 4 Implementation:**
- Lua 5.4 engine running in freestanding kernel environment
- Interactive REPL via `lua` shell command
- Inline execution via `lua -e "code"`
- Script file execution from VFS via `lua <filename>`
- SLM-OS kernel bindings exposed as the `slm` table (`slm.uptime`, `slm.tasks`, `slm.mem`, `slm.print`, etc.)
- Custom libc stub layer routes Lua's C library dependencies through kernel primitives

See `docs/lua.md` for full documentation.

### ELF Loader

| Component | File(s) | Purpose |
|-----------|---------|---------|
| ELF64 Loader | `kernel/src/elf.c` | Parse and load ARM64 ELF binaries |

**Phase 4 Implementation:**
- Minimal ELF64 parser for ARM64 executables
- Loads program segments into allocated memory
- Creates a new task with standard `main(argc, argv)` entry convention
- Accessible from the shell via the `run` command

### Component System

| Component | File(s) | Purpose |
|-----------|---------|---------|
| Component (C) | `kernel/src/component.c` | C helper functions and state names |
| Component (Rust) | `runtime/src/component/` | Core component lifecycle management |

**Phase 4 Implementation:**
- Lifecycle management for SLM workloads (loaded, initializing, running, suspended, updating, terminating, unloaded)
- Shell interface via `component` command (list, register, status)
- Designed as the runtime unit for model inference tasks

---

## Boot Sequence

```
Power On / Bootloader
    │
    │ (x0 = DTB address)
    ▼
┌─────────────────┐
│   _start        │  kernel/src/boot.S
│   (EL2 → EL1)   │  Saves x0→x19, passes to kernel_main
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│   kernel_main   │  kernel/src/main.c
│   - UART init   │
│   - DTB parse   │  (or fallback to platform.h)
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
│   ├── arch/arm64/       # ARM64-specific code
│   ├── arch/x86_64/      # x86-64 experimental port
│   ├── mm/               # Memory management (PMM, VMM)
│   ├── sched/            # Scheduler, tasks, SMP
│   ├── ipc/              # Inter-process communication
│   ├── drivers/          # Hardware drivers (UART, timer, blkdev, VirtIO)
│   ├── fs/               # Filesystem (LittleFS wrapper, VFS adapter)
│   ├── net/              # Networking (lwIP integration)
│   ├── gpu/              # GPU subsystem
│   ├── lib/              # Third-party libraries (LittleFS, Lua 5.4, lwIP)
│   └── tests/            # Kernel test suite
├── runtime/
│   └── src/
│       ├── lib.rs        # Rust entry points
│       ├── kernel_ffi.rs # FFI to C kernel
│       ├── log.rs        # Logging infrastructure
│       ├── mm/           # Model memory management
│       ├── sched/        # Scheduling policies
│       └── component/    # Component system for SLM workloads
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

## Phase Summary

### Phase 3 (Completed)
- Device Tree parser with fallback to compile-time defaults
- Model memory allocator with 2MB blocks (Rust)
- Deadline-aware hybrid scheduler with priority boost
- Priority-inheriting mutex
- GPU platform abstraction (stub driver for QEMU)
- IPC timeout support and statistics
- Heterogeneous CPU topology awareness (skeleton)
- Inference scheduler skeleton (for Phase 5)

### Phase 4 (Completed)
- **Filesystem integration**:
  - Block device abstraction layer
  - RAM disk driver for development
  - LittleFS wrapper (flash-friendly filesystem)
  - VFS mount point support
- **Networking**:
  - VirtIO-Net driver for QEMU virtual networking
  - lwIP TCP/IP stack (ICMP, TCP, UDP, DHCP)
  - Shell commands: `net`, `ping`, `ifconfig`, `netstat`
- **Lua scripting engine**:
  - Lua 5.4 with freestanding libc stubs
  - REPL, inline execution (`lua -e`), script files
  - SLM-OS kernel bindings (`slm.uptime`, `slm.tasks`, `slm.mem`, etc.)
- **ELF loader** for loading and running ARM64 binaries from the shell
- **Component system** for SLM workload lifecycle management (Rust + C)
- **Interactive shell** with 38+ commands (filesystem ops, process management, networking, scripting)

### Phase 4+ (In Progress)
- **Raspberry Pi 5 hardware bring-up**:
  - Boots to fully interactive shell on real hardware
  - RP1 UART TX/RX working (PL011 via RP1 southbridge)
  - GICv2, buddy allocator, VMM, all subsystems operational
  - Preemptive scheduling active at 100 Hz (physical timer, IRQ 30)
  - Automated deploy pipeline via SDWireC and labctl
  - See `docs/pi5-baremetal-status.md` for detailed status
- **Experimental x86-64 port**:
  - Multiboot2 boot sequence (32-bit trampoline to 64-bit long mode)
  - Serial console output
  - Basic subsystem initialization
- **Jetson Orin Nano** hardware bring-up blocked by CBB firewall (see `docs/jetson-nvidia-support.md`)
- **Automated lab infrastructure** via labctl (power control, serial capture, SDWireC management)

### Deferred to Future Phases
- Actual GPU compute (requires TensorRT, Phase 5)
- Model loading and inference (Phase 5)
- User/kernel separation (FUTURE.md)
- eMMC/SD card drivers (Phase 5)
- Pi 5 multi-core (secondary CPUs boot via PSCI SMC, cache coherency for regular writes pending)
- Pi 5 armstub reliability (EL3→EL2 ERET intermittent failure, currently disabled)

---

*Last updated: April 2026*
