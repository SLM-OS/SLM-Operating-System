## Future Work & Stretch Goals

This section documents features discussed during development that are beyond the capstone scope but would enhance SLM-OS in future iterations or as time permits.

---

### USB Serial Console (TinyUSB + Tegra XUSB)
Eliminate external UART adapter by implementing USB CDC-ACM device mode.

- ☐ Study Tegra XUSB device controller (`xudc@3550000`) and Linux `tegra-xudc.c` driver
- ☐ Initialize XUSB PHY, PLLs, and power rails from bare metal
- ☐ Write Tegra XUSB Device Controller Driver (DCD) for TinyUSB
- ☐ Integrate TinyUSB CDC-ACM class with MicroShell
- ☐ Test enumeration on Linux/Windows/macOS hosts
- ☐ Remove external UART adapter requirement from hardware setup

**Effort:** 3-5 weeks  
**Value:** Clean single-cable connection, professional demo setup

---

### Shell Working Directory & Relative Paths ✅
Add current working directory support and relative path resolution.

- ✅ Current working directory (cwd) state tracking
  - `shell_cwd` variable in `kernel/src/shell.c`
  - Starts at `/`, persists across commands
- ✅ `pwd` command to print working directory
- ✅ `cd` command with full path validation
  - Validates target is a directory (not a file)
  - Works with both virtual directories and mount points
  - `cd` with no arg returns to `/`
- ✅ `resolve_path()` internal function
  - Handles `.` (current directory)
  - Handles `..` (parent directory)
  - Normalizes `//` to `/`
  - Strips trailing slashes
- ✅ All filesystem commands updated for relative paths
  - `ls`, `cat`, `write`, `mkdir`, `rm`, `mv`, `df`, `truncate`, `append`
- ✅ 21 unit tests for path resolution and working directory

**Effort:** 1 day
**Value:** Unix-like navigation, less typing for deep paths
**Status:** Complete (December 2025)

---

### Full POSIX Shell
Replace MicroShell with a real shell supporting scripting, pipes, and job control.

- ☐ Implement `fork()` system call (process duplication)
- ☐ Implement `exec()` family (replace process image with ELF)
- ☐ Implement `waitpid()` and process hierarchy
- ☐ Add file descriptor table per process
- ☐ Implement pipes (`pipe()`, `dup2()`)
- ☐ Implement signal handling (`SIGINT`, `SIGTERM`, `SIGCHLD`)
- ☐ Port BusyBox ash or dash shell
- ☐ Add job control (foreground/background, `Ctrl+Z`)

**Effort:** 2-3 months  
**Value:** Full Unix-like environment, standard shell scripting  
**Prerequisite:** User/kernel separation, filesystem

---

### Lua Scripting Runtime
Embed Lua interpreter for runtime scripting and configuration.

- ☐ Integrate Lua 5.4 core (or eLua for smaller footprint)
- ☐ Create C bindings for kernel APIs (`slm.task_create()`, `slm.mem_stats()`)
- ☐ Expose component/model management to Lua scripts
- ☐ Implement Lua REPL as shell alternative
- ☐ Support loading scripts from filesystem or embedded in components
- ☐ Add `lua` command to MicroShell for interactive use

**Effort:** 2-3 weeks  
**Value:** Runtime configurability, rapid prototyping, user-defined automation

---

### User/Kernel Privilege Separation
Implement proper privilege levels for security and stability.

- ☐ Configure ARM64 EL0 (user) vs EL1 (kernel) transitions
- ☐ Split address space: TTBR0 for user, TTBR1 for kernel
- ☐ Implement system call interface (`svc` instruction dispatch)
- ☐ Define minimal syscall set (memory, IPC, task control)
- ☐ Validate all user pointers before kernel access
- ☐ Handle user-mode exceptions gracefully (don't panic kernel)
- ☐ Update components to run in user mode

**Effort:** 3-4 weeks  
**Value:** Security isolation, stability (bad component can't crash kernel)  
**Prerequisite:** Required for component sandboxing

---

### Component Sandboxing
Isolate components from each other and from kernel.

- ☐ Per-component address spaces (separate page tables)
- ☐ Memory protection between components
- ☐ Capability-based access control for IPC
- ☐ Resource limits (memory, CPU time) per component
- ☐ Secure component loading with signature verification
- ☐ Revocation of component privileges

**Effort:** 4-6 weeks  
**Value:** Run untrusted components safely  
**Prerequisite:** User/kernel separation

---

### Secure Boot Chain
Verify system integrity from power-on through component loading.

- ☐ Study Jetson secure boot architecture (BRBCT, BCT)
- ☐ Implement kernel image signature verification
- ☐ Verify component signatures before loading
- ☐ Integrate with Jetson fuse-based root of trust
- ☐ Implement secure key storage for model encryption keys
- ☐ Document threat model and security boundaries

**Effort:** 4-6 weeks  
**Value:** Production deployment security, tamper detection

---

### Encrypted Model Storage
Protect model weights at rest and during loading.

- ☐ Implement AES-256 decryption for model files
- ☐ Integrate with Jetson security engine (SE) for hardware acceleration
- ☐ Key management: secure storage, rotation, per-model keys
- ☐ Decrypt-on-load to model memory region
- ☐ Zero model memory on unload
- ☐ Support encrypted model updates

**Effort:** 2-3 weeks  
**Value:** IP protection for proprietary models

---

### Distributed Operation
Run SLM-OS across multiple boards for larger workloads.

- ☐ Design distributed component communication protocol
- ☐ Implement network-transparent IPC (message routing across nodes)
- ☐ Distributed model loading (split large models across boards)
- ☐ Node discovery and health monitoring
- ☐ Failover: migrate components when node fails
- ☐ Distributed inference coordination (pipeline parallelism)

**Effort:** 2-3 months  
**Value:** Scale beyond single-board memory/compute limits  
**Prerequisite:** Networking (lwIP)

---

### Dynamic Model Compilation
JIT-compile or optimize models for target hardware at load time.

- ☐ Integrate TensorRT or similar optimization framework
- ☐ Cache compiled models for fast reload
- ☐ Profile-guided optimization based on actual inputs
- ☐ Support multiple backends (CPU SIMD, GPU, NPU)
- ☐ Automatic quantization (FP32 → INT8) with calibration

**Effort:** 1-2 months  
**Value:** Optimal inference performance without pre-compilation

---

### Advanced Power Management
Optimize power consumption for battery/thermal-constrained deployments.

- ☐ Implement CPU frequency scaling (DVFS)
- ☐ Core power gating (turn off idle cores completely)
- ☐ Inference-aware power modes (high-perf during inference, low-power idle)
- ☐ GPU/NPU power management
- ☐ Thermal throttling integration
- ☐ Power consumption profiling and reporting

**Effort:** 3-4 weeks  
**Value:** Extended battery life, reduced thermal footprint

---

### Filesystem Integration (LittleFS)
Add persistent storage for models, logs, and configuration.

- ☐ Integrate LittleFS for flash-friendly filesystem
- ☐ Mount eMMC/SD card partitions
- ☐ Implement VFS layer for abstraction
- ☐ Add filesystem commands to shell (`ls`, `cat`, `cp`, `rm`)
- ☐ Support loading components/models from filesystem
- ☐ Persistent configuration storage

**Effort:** 2-3 weeks  
**Value:** Persistent storage, standard file operations  
**Enables:** Loading ELF from disk, logs, config files

---

### Networking (lwIP)
Add TCP/IP networking for remote management and distributed operation.

- ☐ Integrate lwIP TCP/IP stack
- ☐ Write Jetson Ethernet driver (EQOS controller)
- ☐ Implement DHCP client for automatic configuration
- ☐ Add network commands to shell (`ping`, `ifconfig`)
- ☐ REST API for remote component management
- ☐ Network console (telnet/SSH alternative)

**Effort:** 3-4 weeks  
**Value:** Remote access, distributed systems, OTA updates

---

### Buddy Allocator ✅
Replace bitmap allocator with more efficient buddy system.

- ✅ Implement buddy allocator for physical memory
  - `kernel/mm/pmm.c` - complete rewrite with buddy system
  - Free lists per order (0-18), supporting 4KB to 1GB allocations
  - Doubly-linked free lists for O(1) block insertion/removal
- ✅ Support O(log n) allocation of power-of-two pages
  - `log2_ceil()` for order calculation
  - Automatic rounding up of non-power-of-2 requests
- ✅ Efficient coalescing on free
  - Buddy address calculation via XOR
  - Recursive merge up to MAX_ORDER
  - Statistics: merge_count tracks coalesces
- ✅ Statistics and fragmentation monitoring
  - Per-order free counts
  - Split/merge operation counts
  - `pmm_dump_stats()` shows free list distribution
- ✅ 24 unit tests (`kernel/tests/test_pmm.c`)
  - Basic alloc/free, power-of-two rounding
  - Coalescing verification (proves merged blocks enable larger allocations)
  - Block splitting creates correct buddies
  - Statistics tracking (split_count, merge_count)
  - Mixed workload stress tests
  - Memory writability verification

**Effort:** 1 week
**Value:** Faster allocation, less fragmentation for large model allocations
**Status:** Complete (December 2025)

---

### Device Tree Parsing ✅
Replace hardcoded memory maps with runtime device tree discovery.

- ✅ Implement FDT (Flattened Device Tree) parser
  - `kernel/src/dtb.c` - big-endian to little-endian conversion
  - Full structure block walker with property callbacks
  - #address-cells and #size-cells support for varying platforms
- ✅ Discover memory regions from `/memory` node
  - Parses `reg` property for RAM base and size
  - Supports both 32-bit and 64-bit cell formats
- ✅ Discover UART, GIC, timer from device tree
  - Recognizes `pl011@`, `uart@`, `serial@` for UART
  - Recognizes `intc@`, `gic@`, `interrupt-controller@` for GIC
  - Parses timer IRQ from `interrupts` property
- ✅ Support multiple board configurations without recompilation
  - Falls back to platform.h defaults when DTB unavailable
  - `dtb` shell command displays parsed or default config
- ✅ Preserve DTB address from bootloader
  - `boot.S` saves x0 (DTB pointer) through early init
  - Passed to kernel_main for parsing
- ☐ Full U-Boot integration testing (deferred - requires hardware)

**Effort:** 1-2 weeks
**Value:** Single kernel binary for multiple boards, cleaner configuration
**Status:** Complete (December 2025)

---

### UART Output Synchronization ✅
Fix garbled output during concurrent multi-core prints.

- ✅ Option B: Message-level spinlock (implemented)
  - `uart_puts()` and `uart_printf()` acquire spinlock with IRQ save
  - `uart_puts_unlocked()` and `uart_printf_unlocked()` for panic/early boot
  - `uart_putc()` remains unlocked for low-level use
- ✅ Panic output works (uses unlocked variants)
- ☐ Benchmark overhead vs current unlocked approach (optional)

**Effort:** 2-3 days
**Value:** Clean debug output, especially at boot
**Status:** Complete (December 2025)

---

### Real-Time Guarantees
Formal real-time scheduling with provable bounds.

- ☐ Implement Rate Monotonic Scheduling (RMS) policy option
- ☐ Priority ceiling protocol for mutex
- ☐ Worst-case execution time (WCET) analysis tooling
- ☐ Interrupt latency measurement and optimization
- ☐ Document schedulability analysis for demo workloads
- ☐ Consider PREEMPT_RT-style improvements

**Effort:** 3-4 weeks  
**Value:** Certifiable real-time for safety-critical industrial use

---

### Development SDK & Tooling
Make it easy for others to develop SLM-OS components.

- ☐ Component project template (Cargo/CMake)
- ☐ Component packaging tool (bundle code + manifest + models)
- ☐ Emulator mode: run components on host for testing
- ☐ Debug protocol: GDB stub for kernel debugging
- ☐ Trace framework: record scheduler/IPC events for analysis
- ☐ Performance profiler: CPU time, memory, inference latency

**Effort:** 1-2 months  
**Value:** Community adoption, easier development

---

### Component Marketplace Concept
Infrastructure for sharing and deploying SLM components.

- ☐ Component registry specification (metadata, versioning)
- ☐ Dependency resolution for component installation
- ☐ Signed component distribution
- ☐ Web interface for browsing/searching components
- ☐ CLI tool for `slm install <component>`
- ☐ Update mechanism with rollback

**Effort:** 2-3 months
**Value:** Ecosystem growth, reusable components
**Prerequisite:** Networking, filesystem, secure boot

---

### Demand Paging for Model Memory
Lazy allocation and swap support for large models.

- ☐ Implement lazy allocation (map on first access via page fault)
- ☐ Swap cold model weights to storage
- ☐ Prefetch next inference batch
- ☐ Track working set for eviction decisions

**Effort:** 2-3 weeks
**Value:** Support models larger than physical RAM

---

### GPU Memory Integration
Enable GPU access to model memory regions.

- ☐ Implement `gpu_map()` / `gpu_unmap()` for model handles
- ☐ Cache coherency: flush D-cache before GPU access
- ☐ Invalidate D-cache after GPU writes
- ☐ Fully implement `SHM_GPU_ACCESSIBLE` flag for IPC buffers
- ☐ DMA-friendly buffer allocation with proper alignment

**Effort:** 1-2 weeks
**Value:** Zero-copy model data sharing with GPU
**Prerequisite:** GPU driver initialization

---

### Multi-Model Management
Support multiple loaded models with intelligent eviction.

- ☐ Model registry: track loaded models by name/ID
- ☐ LRU eviction: unload least-recently-used models when memory tight
- ☐ Hot-swap: replace model without stopping inference
- ☐ Model pinning: prevent eviction of critical models

**Effort:** 1-2 weeks
**Value:** Efficient memory use with multiple models

---

### TensorRT/CUDA Integration (Phase 5)
Actual GPU inference acceleration.

- ☐ Initialize NVIDIA GSP firmware on Jetson Orin
- ☐ Set up CUDA runtime environment
- ☐ Integrate TensorRT for optimized inference
- ☐ Coordinate GPU/NPU task scheduling
- ☐ Batch inference requests for throughput

**Effort:** 2-3 months
**Value:** Production-grade inference performance
**Prerequisite:** GPU driver, model loader

---

### Raspberry Pi 5 Port
Second platform target for broader hardware support.

- ☐ BCM2712 UART driver
- ☐ BCM2712 interrupt controller driver
- ☐ BCM2712 timer driver
- ☐ VideoCore VII GPU stub (or actual integration)
- ☐ Platform detection and conditional initialization

**Effort:** 2-3 weeks
**Value:** Demonstrates platform abstraction, cheaper dev hardware

---

### EFI Stub Boot
Boot directly from UEFI without U-Boot.

- ✅ Implement PE/COFF header for EFI loading
  - `kernel/arch/arm64/boot.S` includes full PE/COFF header
  - MZ magic via `ccmp x18, #0, #0xd, pl` (encodes to "MZ..")
  - PE32+ optional header with proper ARM64 machine type
  - Section headers for .text (code + data)
  - Works with UEFI firmware that loads ARM64 PE binaries
- ☐ EFI boot services for memory map (deferred)
- ☐ Exit boot services and take over hardware (deferred)
- ☐ Parse ACPI/DTB from EFI configuration table (deferred)

**Effort:** 1-2 weeks (remaining items)
**Value:** Simpler boot chain, faster boot time
**Status:** PE/COFF header complete (December 2025)

---

### CI/CD Pipeline ✅
Automated testing and deployment.

- ✅ GitHub Actions workflow for QEMU tests
  - `.github/workflows/ci.yml` - triggers on push/PR to main
  - Installs ARM GCC toolchain (aarch64-none-elf), Rust, QEMU
  - Builds runtime (Rust) and kernel (CMake)
  - Runs full test suite in QEMU with semihosting
- ✅ Automated build on PR
  - Workflow triggers on `push` to main/develop and `pull_request` to main
  - Manual trigger via `workflow_dispatch` for testing
- ✅ Test result reporting
  - Parses QEMU output for `[PASS]` / `[FAIL]` markers
  - Detects crashes via `PAGE FAULT` detection
  - Uploads build artifacts (ELF, BIN, test output) on all runs
- ✅ QEMU semihosting exit for clean test termination
  - `kernel/src/semihosting.c` - ARM64 semihosting via HLT #0xF000
  - `semihosting_exit(code)` - exits QEMU with specified code
  - Test harness calls semihosting_exit() after all tests complete
  - Eliminates timeout-based test detection
- ☐ Coverage tracking (deferred)
- ☐ Real hardware test farm (deferred - requires physical Jetson setup)

**Effort:** 1 week (basic), 2-3 weeks (with hardware)
**Value:** Catch regressions early, professional development workflow
**Status:** Basic CI complete (December 2025)

---

### TLB Shootdown ✅
Invalidate TLB entries across CPUs for shared page tables.

- ✅ Hardware-broadcast TLB invalidation (TLBI with "is" suffix)
  - Uses inner shareable domain for automatic cross-CPU broadcast
  - No explicit IPIs needed - ARM64 hardware handles synchronization
- ✅ Targeted invalidation (specific VA range)
  - `vmm_invalidate_tlb_range(start, end)` - efficient for small ranges
  - Falls back to full flush for large ranges (> 32 pages)
- ✅ Synchronization barrier after shootdown
  - DSB ISHST before: prior stores visible
  - DSB ISH after: invalidation complete on all CPUs
  - ISB: instruction stream synchronized
- ✅ ASID-aware invalidation for future user space
  - `vmm_invalidate_tlb_asid(virt, asid)` - single VA + ASID
  - `vmm_invalidate_tlb_asid_all(asid)` - all VAs for ASID
- ✅ 18 unit tests verifying TLB operations

**Effort:** 3-5 days
**Value:** Required for shared page tables, user space support
**Prerequisite:** IPI infrastructure (already have)
**Status:** Complete (December 2025)

---

### Priority-Based Message Queues ✅
IPC message ordering by priority.

- ✅ 4 priority levels: LOW, NORMAL, HIGH, URGENT
  - `msg_send_priority(queue, msg, priority, timeout)`
  - `msg_send()` defaults to NORMAL for backward compatibility
- ✅ Separate ring buffers per priority level
  - Each priority has its own capacity slots
  - No priority blocking another (independent per-level full detection)
- ✅ Priority-ordered receive: highest priority first
  - `msg_recv()` automatically returns highest priority available
- ✅ Starvation prevention mechanism
  - After 8 consecutive high-priority receives, low priority gets a turn
  - Configurable via MSG_STARVATION_THRESHOLD
- ✅ Per-priority statistics tracking (`prio_msgs_sent[]`)
- ✅ 6 unit tests for priority queue functionality

**Effort:** 3-5 days
**Value:** QoS for IPC, urgent messages bypass queue
**Status:** Complete (December 2025)