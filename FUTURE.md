## Future Work & Stretch Goals

This section documents features discussed during development that are beyond the capstone scope but would enhance SLM-OS in future iterations or as time permits.

### Status Summary

| Category | Completed | Pending |
|----------|-----------|---------|
| **Core Infrastructure** | Buddy Allocator, TLB Shootdown, UART Sync | User/Kernel Separation |
| **Filesystem** | LittleFS, VFS, File Commands, Help System | eMMC/SD, Persistent Config |
| **IPC** | Priority Queues, Priority Inheritance Mutex | - |
| **Scheduling** | Deadline Scheduler, Priority Inheritance, ELF Loader | Real-Time Guarantees (RMS) |
| **Shell** | Working Directory, Path Resolution, 30+ Commands, Lua Scripting | POSIX Shell |
| **Components** | Component System, Model Memory, GPU Stub | Sandboxing, Secure Boot |
| **Hardware** | DTB Parser, PE/COFF Boot Header, CI/CD, Networking (QEMU), Pi 5 UART, Jetson EL2 Boot, GPU Probe | Jetson GPU Compute (GSP), USB Serial, Networking (Jetson), SMP (needs UEFI boot) |

**Completed Features:** 26
**Pending Features:** 11

---

### USB Serial Console (TinyUSB + Tegra XUSB)

**Status:** ☐ Unblocked (April 2026) — CBB firewall bypassed via EL2+VHE. See `docs/jetson-el2-bringup.md`.

Eliminate external UART adapter by implementing USB CDC-ACM device mode.

- ⛔ Study Tegra XUSB device controller (`xudc@3550000`) and Linux `tegra-xudc.c` driver — BLOCKED
- ⛔ Initialize XUSB PHY, PLLs, and power rails from bare metal — Unblocked (CBB bypassed via EL2, April 2026)
- ⛔ Write Tegra XUSB Device Controller Driver (DCD) for TinyUSB — BLOCKED
- ☐ Integrate TinyUSB CDC-ACM class with MicroShell — platform-independent, can proceed
- ⛔ Test enumeration on Linux/Windows/macOS hosts — BLOCKED on Jetson
- ⛔ Remove external UART adapter requirement from hardware setup — BLOCKED

**Effort:** 3-5 weeks (CBB resolved via EL2)
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

### Lua Scripting Runtime ✅
Embed Lua interpreter for runtime scripting and configuration.

- ✅ Integrate Lua 5.4 core
  - `kernel/lib/lua/src/` - Full Lua 5.4.7 source (configured for embedded use)
  - Built as separate static library (allows FP operations)
  - Custom `slm_luaconf.h` for freestanding configuration
- ✅ Create C bindings for kernel APIs
  - `slm.print(...)` - Console output
  - `slm.uptime()` - System uptime in milliseconds
  - `slm.mem_stats()` - Memory statistics (total_kb, free_kb, used_kb)
  - `slm.tasks()` - List of running tasks with id, name, state, cpu, priority
  - `slm.sleep(ms)` - Sleep with scheduler yield
  - `slm.yield()` - Yield CPU to scheduler
  - `slm.version()` - SLM-OS version string
  - `slm.cpu_count()`, `slm.cpu_id()` - CPU information
- ✅ Implement Lua REPL as shell alternative
  - Interactive mode with prompt, history navigation
  - `exit` or Ctrl+D to exit, Ctrl+C to cancel line
- ✅ Add `lua` command to MicroShell for interactive use
  - `lua` - Enter interactive REPL
  - `lua -e "code"` - Execute Lua code directly
- ✅ Libc stubs for freestanding environment
  - `kernel/src/lua_stubs.c` - 1MB heap, malloc/free/realloc
  - Math functions via Taylor series approximations
  - `kernel/arch/arm64/setjmp.S` - setjmp/longjmp for error handling
- ✅ Standard Lua libraries enabled: base, table, string, math
- ✅ 22 unit tests covering state management, execution, error handling, bindings
- ✅ Expose component/model management to Lua scripts
  - `slm.component_count()`, `slm.component_list()`, `slm.component_find(name)`
  - `slm.component_run(name)`, `slm.component_hot_swap(old, new)`
  - `slm.model_stats()` — weight and workspace pool statistics
- ✅ Support loading scripts from filesystem
  - `lua_slm_dofile()` reads scripts via VFS and executes them
  - `lua <path>` shell command loads from mounted filesystem

**Effort:** 1 week
**Value:** Runtime configurability, rapid prototyping, user-defined automation
**Status:** Complete (December 2025)

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

**Note:** Secure boot integration is a potential solution to the CBB firewall blocker. If SLM-OS were signed and integrated into the Jetson secure boot chain, it might receive proper CBB permissions. See `docs/jetson-nvidia-support.md`.

Verify system integrity from power-on through component loading.

- ☐ Study Jetson secure boot architecture (BRBCT, BCT) — partially researched, see forum findings
- ☐ Implement kernel image signature verification
- ☐ Verify component signatures before loading
- ☐ Integrate with Jetson fuse-based root of trust
- ☐ Implement secure key storage for model encryption keys
- ☐ Document threat model and security boundaries

**Forum findings:**
- Signing requires `l4t_sign_image.sh` tool
- Cannot sign payloads after Secure Boot is activated (requires full reflash)
- Uses PKC/SBK keys for signing

**Effort:** 4-6 weeks
**Value:** Production deployment security, tamper detection, **potential CBB firewall bypass**

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

### Filesystem Integration (LittleFS) ✅
Add persistent storage for models, logs, and configuration.

- ✅ Integrate LittleFS for flash-friendly filesystem
  - `kernel/lib/littlefs/` - LittleFS v2.8.1 library
  - `kernel/fs/littlefs_slm.c` - SLM-OS wrapper with static buffers
  - Configured for freestanding (no malloc, no libc)
- ✅ RAM disk block device implementation
  - `kernel/drivers/ramdisk.c` - 1MB RAM-backed block device
  - `kernel/drivers/blkdev.c` - block device abstraction layer
  - Flash semantics (erase-before-write, 0xFF erased state)
- ✅ Implement VFS layer for abstraction
  - `kernel/src/vfs.c` - unified virtual filesystem
  - Virtual directories: `/sys/`, `/proc/`, `/components/`
  - Mount points for real filesystems at `/mnt/files/`
  - `vfs_read_path()`, `vfs_get_mount_ctx()` APIs
- ✅ Add filesystem commands to shell
  - Basic: `ls`, `cat`, `write`, `mkdir`, `rm`, `mv`, `df`
  - Advanced: `cp`, `touch`, `stat`, `tree`, `wc`, `hexdump`, `grep`, `find`
  - `truncate`, `append` for file manipulation
- ✅ Support loading components/models from filesystem
  - ELF loader reads from VFS paths
  - Component manifests can reference filesystem paths
- ✅ 26 LittleFS unit tests, 18 VFS tests

**Effort:** 2-3 weeks
**Value:** Persistent storage, standard file operations
**Status:** Complete (December 2025)

---

### Networking (lwIP + VirtIO-Net) ✅
TCP/IP networking for QEMU with lwIP stack and VirtIO-Net driver.

- ✅ Integrate lwIP TCP/IP stack
  - `kernel/lib/lwip/` - lwIP 2.1.3 with custom lwipopts.h
  - NO_SYS=1 mode (single-threaded, polled)
  - TCP, UDP, ICMP, DHCP support
- ✅ VirtIO-Net driver for QEMU
  - `kernel/drivers/virtio_net.c` - MMIO transport at 0x0A000000
  - TX/RX queue management, interrupt handling
  - `kernel/net/lwip_slm.c` - netif adapter
  - `kernel/net/sys_arch.c` - OS abstraction layer
- ✅ Add network shell commands
  - `net init|status` - Initialize network, show status
  - `ping <ip> [count]` - ICMP echo requests
  - `ifconfig` - Display/configure interface
  - `ifconfig dhcp` - Enable DHCP client
  - `ifconfig <ip> <mask> <gw>` - Static IP configuration
  - `netstat` - TX/RX statistics
- ✅ Documentation and tests
  - `docs/networking.md` - comprehensive documentation
  - `kernel/tests/test_net.c` - 13 unit tests
  - Help files for all network commands

**Remaining (not implemented):**
- ⛔ Write Jetson Ethernet driver (EQOS controller) — Unblocked (CBB bypassed via EL2, April 2026)
- ☐ REST API for remote component management
- ☐ Network console (telnet/SSH alternative)

**Effort:** 1 week (QEMU), 2-3 weeks (Jetson if CBB resolved)
**Value:** Remote access, distributed systems, OTA updates
**Status:** Partial (December 2025) - QEMU complete, Jetson Unblocked (CBB bypassed via EL2, April 2026)

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

**Status:** ☐ Partially unblocked — GPU registers accessible from EL2 (probe working). GSP firmware loading pending. See `docs/jetson-el2-bringup.md`.

Enable GPU access to model memory regions.

- ⛔ Implement `gpu_map()` / `gpu_unmap()` for model handles — BLOCKED on Jetson
- ✅ Cache coherency: flush D-cache before GPU access — implemented in `cache.c`
- ✅ Invalidate D-cache after GPU writes — implemented in `cache.c`
- ☐ Fully implement `SHM_GPU_ACCESSIBLE` flag for IPC buffers
- ☐ DMA-friendly buffer allocation with proper alignment

**Effort:** 1-2 weeks (CBB resolved via EL2)
**Value:** Zero-copy model data sharing with GPU
**Prerequisite:** GPU driver initialization, GSP firmware loading

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

**Status:** ⛔ BLOCKED on Jetson — Multiple blockers. See `docs/jetson-nvidia-support.md`.

Actual GPU inference acceleration.

- ⛔ Initialize NVIDIA GSP firmware on Jetson Orin — GSP firmware not publicly documented
- ⛔ Set up CUDA runtime environment — Requires GSP initialization
- ⛔ Integrate TensorRT for optimized inference — Requires CUDA
- ⛔ Coordinate GPU/NPU task scheduling — BLOCKED
- ☐ Batch inference requests for throughput — Can be designed platform-independently

**Blockers:**
1. CBB firewall prevents bare-metal GPU register access
2. GSP (GPU System Processor) firmware required but not documented
3. BPMP communication needed for GPU clocks but corrupted after kexec

**Effort:** 2-3 months (if all blockers resolved)
**Value:** Production-grade inference performance
**Prerequisite:** GPU driver, model loader, NVIDIA support for bare-metal

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

---

### Priority Inheritance Mutex ✅
Prevent priority inversion in real-time task synchronization.

- ✅ Full priority inheritance protocol implementation
  - `kernel/ipc/pi_mutex.c` - priority inheritance mutex
  - When high-priority task blocks on mutex held by low-priority task,
    low-priority task temporarily inherits higher priority
  - Priority restored when mutex released
- ✅ Ownership tracking and validation
  - Mutex records owner task
  - Only owner can unlock
  - Debug assertions for misuse
- ✅ Nested locking support with priority chain tracking
- ✅ Integration with scheduler priority system
- ✅ 8 unit tests (`kernel/tests/test_pi_mutex.c`)

**Effort:** 1 week
**Value:** Bounded priority inversion, real-time correctness
**Status:** Complete (December 2025)

---

### File-Driven Help System ✅
Centralized, filesystem-based help for shell commands.

- ✅ Help text stored in `/mnt/files/help/*.txt`
  - Written at boot by `help_init()`
  - Read on demand by `help <command>`
  - 27 commands with detailed help text
- ✅ `kernel/src/help.c` - centralized help definitions
  - Each command has usage, description, examples
  - Easy to update without recompiling shell
- ✅ Modified `cmd_help` for two modes:
  - `help` - list all commands with brief descriptions
  - `help <cmd>` - show detailed help from file
- ✅ 6 unit tests for help system

**Effort:** 1 day
**Value:** User-friendly documentation, reduced binary size
**Status:** Complete (December 2025)

---

### Component System ✅
Dynamic component lifecycle management.

- ✅ Component registry in Rust (`runtime/src/component/`)
  - `ComponentInfo` with state, priority, version, memory tracking
  - Spinlock-protected registry with fixed capacity (32 components)
  - FFI bindings for C kernel access
- ✅ Component states: Loaded, Initializing, Running, Suspended, Updating, Terminating
- ✅ Priority levels: Critical, High, Normal, Low, Idle
- ✅ Shell commands for management:
  - `component list` - show all registered components
  - `component register <name> <version> <type> [priority]`
  - `component unregister <index>`
  - `component status <name|index>`
- ✅ Virtual filesystem exposure at `/components/`
- ✅ 12 component system tests

**Effort:** 2 weeks
**Value:** Runtime extensibility, modular system design
**Status:** Complete (December 2025)

---

### Model Memory Allocator ✅
Dedicated memory pools for ML model weights and workspace.

- ✅ Rust-based allocator (`runtime/src/model_mem.rs`)
  - Weight pool (256 MB) for model parameters
  - Workspace pool (128 MB) for inference scratch space
  - Block-based allocation with free list
- ✅ C FFI interface (`kernel/include/slm_ffi.h`)
  - `rust_model_mem_init()` - initialize pools
  - `slm_weight_alloc()` / `slm_weight_free()`
  - `slm_workspace_alloc()` / `slm_workspace_free()`
  - `rust_model_mem_stats()` - pool statistics
- ✅ `model` shell command shows pool status
- ✅ 10 unit tests for allocator

**Effort:** 1 week
**Value:** Efficient memory management for ML inference
**Status:** Complete (December 2025)

---

### GPU Stub Driver ✅
Placeholder GPU driver for QEMU testing.

- ✅ GPU abstraction layer (`kernel/gpu/gpu.c`)
  - `gpu_init()`, `gpu_compute_submit()`, `gpu_sync()`
  - Driver registration mechanism
- ✅ Stub driver for QEMU (`kernel/gpu/gpu_stub.c`)
  - Simulates GPU operations with delays
  - Returns success for all operations
- ✅ Cache coherency utilities (`kernel/gpu/cache.c`)
  - D-cache clean/invalidate for GPU buffers
  - Proper barriers for DMA operations
- ✅ 22 GPU subsystem tests

**Effort:** 1 week
**Value:** GPU API ready for real driver, testable in QEMU
**Status:** Complete (December 2025)

---

### ELF Loader ✅
Load and execute ELF binaries as tasks.

- ✅ ELF64 parser (`kernel/src/elf.c`)
  - Header validation (magic, class, endianness, machine type)
  - Program header parsing for LOAD segments
  - Entry point extraction
- ✅ Memory allocation for loaded programs
  - Pages allocated from PMM for each segment
  - Proper alignment handling
  - Memory ownership tracked for cleanup
- ✅ Task creation from ELF
  - New task created with ELF entry as start function
  - Stack setup with argc/argv passing
  - Task exit triggers ELF memory cleanup
- ✅ Shell integration
  - `run <name>` - execute program from built-in table
  - `kill <pid>` - terminate running task
  - `elftest` - run ELF loader validation tests

**Effort:** 1 week
**Value:** Run user programs, extensible system
**Status:** Complete (December 2025)

---

### Deadline-Aware Scheduler ✅
Scheduler with deadline and priority support.

- ✅ Priority-based scheduling (8 levels: 0-7)
  - Higher priority tasks preempt lower
  - Round-robin within same priority
- ✅ Deadline scheduling support
  - `task_set_deadline()` - set absolute deadline
  - `slm_task_set_deadline()` FFI for Rust
  - Earliest-deadline-first within priority class
- ✅ CPU affinity and isolation
  - Per-task CPU affinity mask
  - `sched_set_affinity()` / `sched_get_affinity()`
  - CPU isolation for dedicated workloads
- ✅ Task migration between CPUs
  - `sched_migrate_task()` - move task to different CPU
  - Works for READY tasks in run queue
- ✅ Per-CPU run queues with work stealing
- ✅ 29 scheduler tests

**Effort:** 2 weeks
**Value:** Real-time task management, predictable latency
**Status:** Complete (December 2025)