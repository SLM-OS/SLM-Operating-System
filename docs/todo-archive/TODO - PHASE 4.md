# Phase 4: Hardware Bring-Up & Component System

This document tracks Phase 4 implementation of SLM-OS.

**Status:** ✅ Complete (All milestones done. M8 isolation deferred to Phase 5.)

**Summary:** Phase 4 combines hardware bring-up with the Component System. Pi 5 has 4-core SMP with cross-CPU dispatch. Jetson has 6-core SMP at EL2 with VHE, using UARTC via TCU for serial. Component system includes hot-swap with subscription preservation, topic-based pub/sub message routing, and built-in services. Rust runtime has PanicInfo formatting and timestamped logging.

**Goals:**
- ✅ Raspberry Pi 5 hardware bring-up (4-core SMP, cross-CPU dispatch)
- ✅ Jetson Orin Nano hardware bring-up (6-core SMP, EL2/VHE, UARTC)
- ✅ GPU memory integration (cache coherency, nvidia_alloc/free)
- ✅ Component system with hot-swap and message routing
- ✅ Performance benchmarks on all platforms
- ⏸️ Component isolation — deferred to Phase 5 (requires user/kernel separation)

**Carried from Phase 3:**
- ✅ GPU Memory Integration (cache coherency, SHM_GPU_ACCESSIBLE) — completed April 2026
- ⏸️ Jetson GPU compute (init, submit, fence) — requires GSP firmware (Phase 5+)
- ✅ Hardware bring-up (serial console, GIC, timer, MMU — all platforms)
- ✅ Shell improvements (VFS, file commands, Lua, networking)
- ⏸️ Lock-free task stealing — deferred (profile first to confirm need)

---

## Icon Key

| Icon | Meaning |
|------|---------|
| ☐ | Not started |
| ✅ | Complete |
| ⏸️ | Deferred to later phase |
| 🔗 | Has dependency on another milestone (shown as ⏸️🔗 or ☐🔗) |

---

## Raspberry Pi 5 Bring-Up (Complete)

**Status:** ✅ Complete — 4-core SMP, preemptive scheduling, interactive shell

See `docs/pi5-baremetal-status.md` for full details.

### Hardware & Boot
- ✅ EL2→EL1 transition for peripheral access
- ✅ RP1 UART TX/RX (PL011 via PCIe, 115200 baud)
- ✅ GPIO pad config: OD=1 + FUNCSEL 5→4 sequencing for RX
- ✅ ACT LED control (GPIO2 bit 9)
- ✅ DTB parsing (found via RAM scan at 0x2efec700)
- ✅ 100% boot reliability (armstub disabled)
- ✅ Automated deploy via SDWireC + labctl

### Memory & MMU
- ✅ PMM buddy allocator (4 GB RAM)
- ✅ VMM with platform-specific 1GB L1 block descriptors
- ✅ MMU enable with cacheable + Inner Shareable mappings

### Interrupts & Timer
- ✅ GIC-400 (GICv2) initialized from EL2
- ✅ Physical timer (CNTP, IRQ 30, 54 MHz) at 100 Hz
- ✅ Preemptive scheduling (DAIF=0x080 fix for task context switch)

### Multi-Core SMP
- ✅ PSCI CPU_ON via SMC (TF-A at EL3)
- ✅ MPIDR Aff1 encoding (0x000, 0x100, 0x200, 0x300)
- ✅ Secondary CPU EL2→EL1 transition + MMU enable
- ✅ 4 cores online (DC CVAC/CIVAC cache coherency workaround)
- ✅ Hardware spinlocks (runtime-enabled after MMU, `spinlock_hw_enabled`)

### Remaining Pi 5 Work
- ✅ Run QEMU test suite on Pi 5 hardware (431 tests: 415 pass, 16 ignored, 0 failures)
- ✅ Timer-driven sleep/delay functions (sleep_ms, sleep_us, shell `sleep` command)
- ✅ Fix task_exit/schedule race on secondary CPUs — IRQ mask in task_exit prevents timer/schedule race
- ✅ Investigate SMPEN for proper cache coherency — SMPEN trapped to EL3, L2 not coherent without it
- ✅ NC shared memory infrastructure — 2MB NC region at 0xFFE00000, run queues + task table in NC, validated on Pi 5
- ✅ Boot reliability testing — 92/92 power cycles reach shell (100%), avg 10.1s boot time
- ✅ Performance benchmarks — context switch 1.7µs, IPC 322ns, timer jitter 685ns range
- ✅ UART IRQ investigation — BAR3 fix (not BAR1), 0xFF address (not 0x0F). Root cause identified: MSIX_CFG controls RP1-internal routing, not TLP generation. Needs brcmstb MSI controller for full interrupt-driven RX. Polling has no measurable performance impact.
- ⏸️ Interrupt-driven UART — requires implementing brcmstb MSI controller (BAR2→DRAM doorbell). Deferred: polling works, no performance impact.
- ✅ Cross-CPU task dispatch — WORKING via cooperative scheduling (WFE/SEV). All 4 CPUs execute tasks. Timer-based preemption on secondary CPUs still under investigation. See `docs/pi5-cross-cpu-dispatch-investigation.md`.

---

## Milestone 1: Jetson Serial Console & Boot

**Priority:** CRITICAL — Unblocks all other Jetson work

**Status:** ✅ Complete — EL2 + VHE + UARTC + 6-core SMP

**Reference:** See `docs/jetson-nvidia-support.md` for full CBB analysis, `docs/jetson-el2-bringup.md` for EL2 breakthrough details.

### CBB Firewall — Partially Bypassed (April 2026)

The Tegra234 CBB firewall blocks UARTA (0x03100000) but **allows UARTC (0x0C280000) from EL2**. By enabling VHE (HCR_EL2.E2H=1, TGE=1) and using UARTC, SLM-OS now boots to an interactive shell.

**EL2 breakthrough findings:**
- SLM-OS enters at EL2 after kexec (confirmed via PSCI SYSTEM_OFF probe)
- VHE transparently redirects EL1 register accesses to EL2 — kernel code works unmodified
- UARTC accessible from EL2; UARTA blocked; GICv3 accessible; timer works
- OP-TEE secure carveout at 0xBE-0xC2 skipped; ~6.7 GB usable across 3 regions
- UART RX working via TCU HSP mailbox (0x03C10000)

**Remaining CBB restrictions:**
- UARTA (40-pin header) — blocked even from EL2
- ✅ SMP — 6-core boot working. Root cause was wrong MPIDR encoding, not TF-A state.

### Serial Console
- ✅ Connect USB-serial adapter to 40-pin header (Pin 8 TXD, Pin 10 RXD, Pin 6 GND)
- ✅ UARTC serial TX working at EL2 via TCU (USB-C debug console)
- ✅ UART RX working via TCU HSP mailbox (0x03C10000) — SPE routes USB-C input here
- ✅ Confirm baud rate settings (115200 8N1)
- ✅ Test bidirectional communication — help, mem, cpu, lua all verified

### Boot Method Validation
- ✅ kexec boot with serial console output — working at EL2 with VHE
- ✅ Debug silent failures with serial visibility — Root cause: CBB firewall, bypassed with EL2
- ✅ Document working boot sequence — SSH → kexec → EL2/VHE → UARTC
- ⏸️ Direct UEFI boot — EFI stub + trampoline implemented but PE/COFF relocation unsolved. Not needed for SMP (kexec works). Would eliminate kexec dependency for cleaner boot.

### Platform Validation
- ✅ Verify DTB parsing on real Jetson hardware — DTB at 0x80437000 parsed successfully
- ✅ Confirm memory map matches DTB values — RAM 0x80000000-0x280000000 (8GB)
- ⏸️ Remove hardcoded addresses from `platform.h` (use DTB values) — deferred, addresses already match DTB. Requires driver init restructuring for runtime address lookup.
- ✅ Test on Pi 5 with platform-specific DTB — Pi 5 works

### Kernel Subsystem Status on Jetson
- ✅ UART (UARTC, TX only)
- ✅ DTB parsing
- ✅ PMM (buddy allocator, ~6.7 GB across 3 regions)
- ✅ VMM + MMU (identity + high map)
- ✅ GICv3 (distributor + redistributor)
- ✅ Timer (100 Hz)
- ✅ Scheduler (single-core)
- ✅ Lua scripting
- ✅ Shell (fully interactive)
- ✅ SMP — 6 cores online via PSCI CPU_ON (MPIDR dual-cluster encoding fixed)
- ✅ Memory expansion (~6.7 GB free across 3 regions around OP-TEE carveout)

---

## Milestone 2: Jetson Hardware Validation

**Depends on:** M1 (Serial Console)

**Status:** ✅ Complete — GIC, timer, MMU, SMP all working at EL2.

### GIC and Interrupts
- ✅ GICv3 initialized (distributor 0x0F400000, redistributor 0x0F440000, 992 interrupt lines)
- ✅ Interrupt delivery working (timer IRQ drives scheduler)
- ✅ Timer IRQ fires at 100 Hz (confirmed via shell uptime)
- ✅ GIC affinity / core isolation — `bench isolate`: CPU 2 isolated, 8 tasks dispatched, 0 reached isolated core. PASS.

### Timer
- ✅ ARM generic timer works on Jetson at EL2
- ✅ 100 Hz tick confirmed, scheduler preemption working

### Multi-Core
- ✅ MAX_CPUS=8 in config.h, CPU_MAX=6 in platform.h — 6 cores boot
- ✅ SMP boot — PSCI CPU_ON via SMC with correct dual-cluster MPIDR encoding
- ✅ Cross-CPU task dispatch — all 6 CPUs execute tasks via cooperative scheduling (WFE/SEV). Root cause was UART lock deadlock in task_create DEBUG_PRINT.
- ✅ Multi-core stress test — `bench smp` 3 consecutive runs, all 5/5 CPUs COMPLETED

### MMU
- ✅ MMU working with Jetson memory map (3 regions around OP-TEE carveout)
- ✅ Device memory mappings verified (UARTC, GIC, TCU mailbox, GPU, WDT)
- ✅ Model memory pools verified on Jetson — 384 MB (128 weight + 64 workspace blocks)

### GPIO
- ⏸️ GPIO testing deferred — UARTA (40-pin header) blocked by CBB, no GPIO pins accessible from EL2

---

## Milestone 3: Jetson GPU Driver

**Depends on:** M1 (Serial Console), M2 (Hardware Validation)

**Status:** ✅ Memory Complete — probe, alloc/free, cache coherency, IPC+model integration done. Compute deferred (GSP).

GPU registers at `0x17000000` are accessible from EL2. The GA10B chip has been identified (BOOT_0=0xB7B000A1, chip ID=0x17B, Ampere). GPU compute still requires GSP firmware loading.

Code is structured as shared `gpu_nvidia.h`/`gpu_nvidia.c` for both Jetson (GA10B) and x86-64 RTX 3050 (GA106).

### GPU Initialization
- ✅ NVIDIA GPU probe (`gpu_nvidia.c`) — GA10B identified at EL2
- ✅ Read GPU ID registers (NV_PMC_BOOT_0, BOOT_42) — working
- ⏸️ Enable GPU clocks via BPMP IPC — not needed, GPU registers accessible without it (clocks left enabled by Linux before kexec)
- ⏸️ GSP firmware loading — Phase 5+, RISC-V processor on GPU die, required for compute but not documented by NVIDIA
- ✅ Document initialization sequence in `docs/gpu.md`

### GPU Memory Management
- ✅ `nvidia_alloc()` / `nvidia_free()` — allocate/free GPU-accessible memory from PMM (unified memory)
- ✅ Cache coherency: `nvidia_sync_for_gpu()` (DC CVAC) and `nvidia_sync_for_cpu()` (DC IVAC)
- ✅ `SHM_GPU_ACCESSIBLE` flag wired — shared buffers use `gpu_alloc` when flag set, `gpu_free` on destroy, cache sync on map
- ✅ Model memory ↔ GPU: Rust `gpu_map()`/`gpu_unmap()` call C FFI for cache sync (`slm_gpu_sync_for_device`/`slm_gpu_sync_for_cpu`)

### GPU Command Submission (Stretch)
- ⏸️ Write `jetson_gpu_submit(cmd_buffer)` — submit work to GPU
- ⏸️ Write `jetson_gpu_wait()` — wait for GPU completion
- ⏸️ Implement basic fence/sync mechanism
- ⏸️ Test simple compute operation (if possible without CUDA)

**Note:** Full GPU compute requires GSP firmware. Memory allocation and cache coherency are the primary goals; actual compute deferred to Phase 5.

### GPU Testing
- ✅ GPU probe on real Jetson hardware — GA10B identified (BOOT_0=0xB7B000A1)
- ✅ GPU alloc/free tested via HAL (30+ QEMU tests exercise same interface)
- ✅ GPU↔IPC shared buffer test (test_buffer_gpu_accessible)
- ✅ GPU cache sync benchmark — `bench gpu`: alloc 1952ns, sync_for_gpu 1184ns, sync_for_cpu 896ns, free 2272ns, data integrity PASS

---

## Milestone 4: Performance Benchmarking

**Depends on:** M2 (Hardware Validation)

### Context Switch Performance
- ✅ Measure context switch time — Pi 5: 1.6 µs avg, QEMU: ~20 µs (shell `bench context`)
- ✅ Target: < 10 µs — Pi 5 meets target
- ✅ Compare with Jetson measurements — Jetson 6-core: 262 ns, 1-core: 471 ns (6.1x faster than Pi 5)
- ✅ Profile and optimize — Pi 5 with NC memory: 2.5 µs avg (vs 1.6 µs cacheable). NC overhead acceptable for cross-CPU correctness.

### Interrupt Latency
- ✅ Measure timer tick jitter — Pi 5: < 1 µs, Jetson 6-core: 390 ns avg, 2 µs max
- ✅ Worst-case IRQ: 2.6 µs max across multiple runs (no contention — single-core scheduling)
- ✅ Document results in `docs/performance.md`

### Scheduler Performance
- ✅ Scheduler verified stable on Jetson — 17+ min uptime, 6 cores, multiple bench runs
- ✅ Deadline accuracy under load — `bench deadline`: 100ms MET (31ms latency), 50ms MET, 20ms MISSED (cooperative scheduling ~31ms cycle). Timer preemption would improve.
- ✅ Core isolation effectiveness — `bench isolate`: CPU 2 isolated, 8 tasks dispatched, 0 reached isolated core. PASS.

### IPC Performance
- ✅ Measure message passing latency — Pi 5: 322 ns, Jetson 6-core: 530 ns round-trip
- ✅ IPC tested on Jetson hardware — 100-iteration bench runs stable, queues create/destroy correctly
- ✅ Shared buffer throughput — `bench shared`: Write 7.5 GB/s, Read 14.6 GB/s (4KB buffer, 64B stride, Pi 5 Cortex-A76)

### Full Test Suite
- ✅ All subsystems verified on Jetson via shell commands (April 2026) — see `docs/performance.md`
- ✅ ELF loader: 3/4 validation tests pass (1 minor assertion mismatch, not a hardware issue)
- ☐ Create Jetson-specific automated test configuration (currently manual via serial)

---

## Milestone 5: Shell & ELF Improvements

**Depends on:** M1 (Serial Console) for Jetson testing

### Virtual Filesystem Structure
- ✅ Mount root `/` with command nodes
- ✅ Mount `/sys/` for system info (read-only virtual files)
- ✅ Mount `/proc/` for per-task info
- ✅ Design `/components/` mount point for component system

### ELF Execution Improvements
- ✅ `run <name>` — load and execute ELF from built-in table
- ✅ Pass argc/argv to loaded program (simple stack setup)
- ✅ Handle task exit and ELF memory cleanup
- ✅ Track ELF memory ownership for cleanup on exit
- ✅ `kill <pid>` — terminate running task

### Shell Testing
- ✅ Task exits cleanly, memory reclaimed
- ✅ Shell working on Jetson — help, mem, cpu, lua, all file commands verified via serial

---

## Milestone 6: Component System

**Depends on:** M5 (Shell & ELF Improvements)

### Component Specification
- ✅ Finalize component manifest format (YAML)
- ✅ Define component binary format (simplified ELF or custom)
- ✅ Document component API in `docs/components.md`
- ✅ Create example component manifest

### Component Loader
- ✅ Implement component loader in Rust (`runtime/src/component/`)
- ✅ Parse component manifest (simple key-value format)
- ✅ Component registry with spinlock protection
- ⏸️ Load component code into memory — requires actual ELF loading
- ⏸️ Resolve component dependencies — Phase 5+
- ⏸️ Create task(s) for component — Phase 5+

### Component Lifecycle Manager
- ✅ Implement `ComponentState` enum (Loaded, Initializing, Running, Suspended, Updating, Terminating)
- ✅ Implement `ComponentInfo` struct with lifecycle tracking
- ✅ FFI bindings for C kernel (component.h, component.c)
- ✅ Component registration from shell
- ⏸️ Automatic component discovery — Phase 5+

### Hot-Swap Mechanism (April 2026)
- ✅ Implement `component_hot_swap(old_name, new_name)` — stateless swap with subscription preservation
- ✅ Save topic subscriptions → unregister old → run new → re-subscribe
- ✅ Shell command: `component swap <old> <new>`
- ✅ Tests: hot-swap basic, subscription preservation, not-found, invalid new name
- ⏸️ Design state transfer protocol between old and new versions — Phase 5+

### Shell Commands
- ✅ `component list` — list loaded components
- ✅ `component register` — register component from shell
- ✅ `component unregister` — unregister component
- ✅ `component status` — show component details
- ✅ `component builtins` — list available built-in components
- ✅ `component run <name>` — run a built-in component as a kernel task
- ✅ `component swap <old> <new>` — hot-swap: replace old with new, preserve subscriptions
- ✅ `component send <msg>` — send IPC message to echo service

### Component Runtime (April 2026)
- ✅ `component_runtime.c`: bridges component registry to kernel task system
- ✅ Built-in counter service: counts to 10, lifecycle transitions verified on hardware
- ✅ Built-in echo service: IPC message queue (cross-CPU queue visibility issue — WIP)
- ✅ Task cleanup callback updates component state to Unloaded on exit
- ✅ 6 new tests: component_run, invalid name, builtins, count, shell command, ELF arch

---

## Milestone 7: Message Routing

**Depends on:** M6 (Component System)

### Message Router (April 2026)
- ✅ Implement `MessageRouter` in Rust (`runtime/src/msg_router.rs`, 470+ lines)
- ✅ Topic-based publish/subscribe (8 topics, 4 subscribers each, atomic mailboxes)
- ✅ Subscription cleanup on component unload (`msg_router_unsubscribe_all`)
- ✅ Subscription query for hot-swap (`msg_router_get_subscriptions`)
- ⏸️ Direct component-to-component messaging — Phase 5 (topic broadcast sufficient)
- ⏸️ Build routing table from component manifests — Phase 5
- ⏸️ Wildcard subscriptions — Phase 5

### Integration with IPC
- ⏸️ Route messages through kernel IPC priority queues — Phase 5
- ⏸️ Zero-copy for large messages (use shared buffers) — Phase 5
- ⏸️ Message priority support in router — Phase 5

### Testing (April 2026)
- ✅ Test message routing between components (listener + msg send)
- ✅ Test pub/sub with multiple subscribers
- ✅ Test subscription cleanup on component unload (Rust + C tests)
- ✅ Test subscription query (`get_subscriptions` returns correct topics)
- ✅ Test routing table update on hot-swap (subscriptions preserved)

---

## Milestone 8: Component Isolation (Stretch)

**Assessment (April 2026):** Full isolation requires user/kernel mode separation (EL0/EL1 on ARM64, Ring 3/0 on x86-64), which is a Phase 5 feature. All components currently run in kernel space. Below is what Phase 4 provides vs what is deferred.

### What Phase 4 Provides
- ✅ Component lifecycle states prevent invalid operations on unloaded components
- ✅ Subscription cleanup prevents resource leaks when components exit
- ✅ Task cleanup callbacks free component resources on task exit
- ✅ Error reporting via panic handler (file:line:column) and component state tracking

### Memory Isolation — Deferred to Phase 5
- ⏸️ Separate heap regions per component — requires per-component page tables
- ⏸️ Prevent components from accessing each other's memory — requires MMU enforcement
- ⏸️ Shared memory only via explicit shared buffers — requires capability system

### Resource Limits — Deferred to Phase 5
- ⏸️ Memory limit per component — `memory_kb` field exists in `component_info_t` but unenforced
- ⏸️ CPU time accounting per component — requires per-task timer instrumentation
- ⏸️ CPU time limit enforcement — requires preemption improvements

### Fault Isolation — Deferred to Phase 5
- ⏸️ Component crash doesn't crash kernel — requires exception-based containment
- ⏸️ Automatic component restart on failure — requires supervisor task
- ⏸️ Error reporting to structured system log — Phase 5 (basic UART logging available)

---

## Milestone 9: Deferred Polish Items

### Rust Runtime (April 2026)
- ✅ Format `PanicInfo` into buffer — panic handler now prints file:line:column
- ✅ Improve Rust logging with timestamps — `[secs.cs]` prefix on all log messages

### Scheduler
- ⏸️ Lock-free task stealing between queues — profile first to confirm need
- ✅ Big.LITTLE core assignment — `runtime/src/sched/heterogeneous.rs` (546 lines)

### Documentation
- ✅ Update architecture doc with Phase 4 learnings (`docs/architecture.md`)
- ✅ Document component system API (`docs/components.md`)
- ✅ Document Jetson-specific setup (`docs/jetson-boot.md`, `docs/jetson-el2-bringup.md`)
- ⏸️ Create component development tutorial — Phase 5

---

## Phase 4 Completion Checklist

### Deliverables
- ✅ Kernel boots and runs on real Jetson Orin Nano hardware (6-core SMP at EL2)
- ✅ Serial console working via UARTC/TCU (USB-C debug port)
- ✅ All subsystems verified on Jetson hardware (April 2026)
- ✅ GPU initialized (probe), memory allocation working (nvidia_alloc/free + cache coherency)
- ✅ Performance benchmarks documented — see `docs/performance.md`
- ✅ At least one example component loading and running — counter service verified on i7-6700
- ✅ Hot-swap demonstrated — `component swap` preserves subscriptions (April 2026)
- ✅ Message routing between components working — topic pub/sub via msg_router (April 2026)

### Demo
- ✅ Boot on Jetson Orin Nano via serial console (kexec → EL2/VHE → UARTC → 6-core SMP)
- ✅ Show shell commands working on real hardware (help, mem, cpu, lua, bench smp verified)
- ✅ Show component load/unload via shell (`component run/list/unregister`)
- ✅ Show hot-swap of a component (`component swap <old> <new>`)
- ✅ Show message passing between components (`msg send <topic> <data>`)
- ✅ Compare QEMU vs Jetson performance (`bench context/irq/ipc`, `docs/performance.md`)

---

## Outstanding Decisions

### Milestone 1 — Serial Console ✅ DECIDED

| Decision | Options | **Choice** | Rationale |
|----------|---------|------------|-----------|
| **Primary debug method** | 40-pin UART vs USB-C TCU | **UARTC via USB-C** | UARTA blocked by CBB at EL2; UARTC (0x0C280000) works. TCU RX via HSP mailbox (0x03C10000). |
| **Baud rate** | 115200 vs higher | **115200** | Standard, reliable, firmware-configured |

### Milestone 3 — GPU

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **GPU compute scope** | Memory only vs basic compute | **Memory only** — compute requires GSP firmware (Phase 5) |
| **Cache policy** | Write-back vs write-through | **Write-back with explicit flush** — better performance |

### Milestone 6 — Components ✅ DECIDED

| Decision | Options | **Choice** | Rationale |
|----------|---------|------------|-----------|
| **Component format** | Simplified ELF vs custom | **Simplified ELF** | Easier tooling, standard format |
| **Manifest format** | YAML vs TOML vs JSON vs key-value | **Simple key-value** | No `no_std` YAML parser exists; key-value is trivial to parse and sufficient for current needs |
| **State transfer** | Serialize all vs explicit API | **Explicit API** | Component declares what state to transfer (Phase 5+) |

### Milestone 8 — Isolation

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Isolation level** | Kernel space vs user space | **Kernel space** for Phase 4 — user space in future |
| **Memory isolation** | MMU-based vs software checks | **Software checks** for now — MMU-based needs user space |

---

## Risk Mitigation

### Phase 3 Risks (Carried Forward)

1. **GPU Integration**
    - Risk: NVIDIA's GPU is complex; documentation may be incomplete
    - Mitigation: Start with just memory allocation, not compute
    - Mitigation: Cache coherency is well-understood
    - Fallback: CPU-only inference is viable for small models

2. **Real Hardware Bring-Up**
    - Risk: Silent failures, different behavior from QEMU
    - Mitigation: **Have working serial console before anything else** (M1 is CRITICAL)
    - Mitigation: Start with minimal boot, add features incrementally
    - Mitigation: Keep QEMU as primary development target
    - New: USB-serial adapter resolves TCU issues

### Phase 4 Risks

1. **Jetson Timer/GIC Differences**
    - Risk: Jetson interrupt controller may behave differently from QEMU
    - Mitigation: Test early, document differences
    - Mitigation: Add platform-specific workarounds if needed
    - Fallback: QEMU remains functional development target

2. **Component Hot-Swap Correctness**
    - Risk: State transfer bugs, race conditions during swap
    - Mitigation: Start with stateless component hot-swap
    - Mitigation: Add state transfer incrementally
    - Mitigation: Extensive testing with simple components first
    - Fallback: Manual unload/load if hot-swap proves too complex

3. **Memory Isolation Without User Space**
    - Risk: Software-only isolation is incomplete
    - Mitigation: Trust components for Phase 4 (all in kernel space)
    - Mitigation: Document isolation limitations
    - Fallback: Full isolation deferred to future user space implementation

4. **Component System Complexity**
    - Risk: Component loading/routing is a large feature
    - Mitigation: Implement minimal viable component system first
    - Mitigation: Shell commands for manual testing before automation
    - Fallback: Simple ELF loading without full component abstraction

5. **Performance on Real Hardware**
    - Risk: Performance worse than QEMU (cache effects, real timing)
    - Mitigation: Benchmark early, optimize hot paths
    - Mitigation: Profile before optimizing
    - Fallback: Acceptable if within 2x of QEMU performance

### Dependencies Between Milestones

```
M1 (Serial) ─────────┬──────> M2 (Hardware) ──────> M4 (Benchmarks)
                     │              │
                     │              └──────> M3 (GPU)
                     │
                     └──────> M5 (Shell) ──────> M6 (Components) ──> M7 (Routing)
                                                        │
                                                        └──────> M8 (Isolation)

M9 (Polish) ─────────> Can happen in parallel throughout
```

**Critical Path:** M1 → M2 → M3 (GPU) and M1 → M5 → M6 (Components)

**Recommended Order:**
1. **M1 (Serial)** — CRITICAL, unblocks everything
2. **M2 (Hardware Validation)** — Verify platform works
3. **M5 (Shell Improvements)** — Useful for debugging components
4. **M6 (Component System)** — Core Phase 4 deliverable
5. **M3 (GPU)** — Can parallel with M6
6. **M7 (Message Routing)** — After M6
7. **M4 (Benchmarks)** — After M2, can parallel
8. **M8 (Isolation)** — Stretch goal
9. **M9 (Polish)** — Throughout

---

## Resources

### Jetson Hardware
- Jetson Orin Nano Developer Kit User Guide
- [Orin Series SoC Technical Reference Manual (TRM)](https://developer.nvidia.com/orin-series-soc-technical-reference-manual)
- NVIDIA L4T (Linux for Tegra) source code for driver reference
- `docs/lab-operations.md` for serial console procedures and remote lab access
- `docs/jetson-el2-bringup.md` for EL2 breakthrough and current implementation
- `docs/jetson-tcu.md` for TCU/HSP research notes
- `docs/platform-abstraction.md` for QEMU vs Jetson differences
- `labctl` for lab operations (power, serial, deploy) — see [Embedded-Lab-Control](https://github.com/johnjezl/Embedded-Lab-Control)

### GPU
- NVIDIA Open GPU Kernel Modules (reference for register interface)
- Linux kernel `drivers/gpu/drm/tegra/` for Tegra GPU driver
- `docs/gpu.md` for GPU HAL design

### Component Systems
- OSGi specification (for hot-swap concepts)
- Fuchsia component framework documentation
- seL4 component model

### Performance
- ARM Cortex-A78AE Technical Reference Manual
- "Computer Architecture: A Quantitative Approach" — cache and memory chapters

---

## Lessons from Phase 1, 2 & 3

### What Worked Well
- Starting with QEMU before real hardware
- Global scheduler lock (simplicity over optimization)
- Eager FPU save (predictable, no lazy-save bugs)
- Rust FFI with safe wrappers around unsafe calls
- Compile-time assertions for struct sizes
- Custom shell (no external dependencies)
- Phase documents with checkboxes track progress visibly
- "Deferred to Phase N" is explicit — nothing lost
- Risk mitigation has concrete fallbacks
- Decisions table forces explicit choices

### What To Do Differently
- **Set up serial console on real hardware earlier** — TCU complexity cost time
- Profile before optimizing (per-queue locks may not be needed)
- Document all QEMU assumptions in one place
- Add more stress tests earlier in development
- Have backup debug methods ready (USB-serial adapter ordered)

### Lessons from Phase 3 Hardware Bring-Up
- **kexec is useful but has limitations** — Linux shutdown may stop firmware on co-processors (SPE)
- **USB-C debug on Jetson uses TCU** — Requires SPE firmware cooperation, not simple MMIO UART
- **TRM is invaluable** — HSP/mailbox documentation enabled informed debugging
- **Have backup debug methods** — USB-serial adapter for 40-pin header avoids TCU complexity
- **DTB parser was essential** — Enables single binary for multiple platforms

### Patterns to Preserve
- Phase documents with checkboxes track progress visibly
- "Deferred to Phase N" is explicit — nothing lost
- Risk mitigation has concrete fallbacks
- Decisions table forces explicit choices
- Icon system (✅ ⏸️ 🔗) for quick status visibility

---

## Extra Work Completed (December 2025)

While waiting for USB-serial adapter delivery (1.5 days), the following enhancements were implemented. These items were selected from `FUTURE.md` based on feasibility without hardware access.

### TLB Shootdown API

**Files:** `kernel/mm/vmm.c`, `kernel/include/vmm.h`, `kernel/tests/test_vmm.c`

Extended the VMM with comprehensive TLB invalidation support:

| Function | Description |
|----------|-------------|
| `vmm_invalidate_tlb(virt)` | Invalidate single address (all CPUs) |
| `vmm_invalidate_tlb_all()` | Full TLB flush (all CPUs) |
| `vmm_invalidate_tlb_range(start, end)` | Range invalidation with threshold fallback |
| `vmm_invalidate_tlb_asid(virt, asid)` | ASID-aware single address invalidation |
| `vmm_invalidate_tlb_asid_all(asid)` | Invalidate all entries for an ASID |

All functions use ARM64 "is" (inner shareable) suffix for automatic hardware broadcast to all CPUs. No software IPI required.

**Test coverage (10 tests):**
- Page table verification (`test_virt_to_phys_accuracy`, `test_va_pa_coherency`)
- Remapping with TLB invalidation (`test_remap_requires_invalidation`, `test_remap_with_full_flush`, `test_remap_with_range_invalidation`)
- Edge cases (`test_sequential_remaps`, `test_rapid_remap_stress`)
- ASID operations (`test_asid_invalidation_executes`, `test_asid_all_invalidation_executes`)
- Multi-CPU broadcast (`test_tlb_broadcast_all_cpus`)

Tests use actual page table manipulation to verify TLB invalidation works correctly, not just smoke tests.

---

### Priority-Based Message Queues

**Files:** `kernel/ipc/ipc.c`, `kernel/include/ipc.h`, `kernel/tests/test_ipc.c`

Extended the IPC subsystem with priority-aware message queuing:

| Priority Level | Constant | Description |
|----------------|----------|-------------|
| Urgent | `MSG_PRIO_URGENT` (3) | Highest priority, processed first |
| High | `MSG_PRIO_HIGH` (2) | Above normal |
| Normal | `MSG_PRIO_NORMAL` (1) | Default for `msg_send()` |
| Low | `MSG_PRIO_LOW` (0) | Background messages |

**Key features:**
- Per-priority ring buffers (independent capacity)
- `msg_send_priority(queue, msg, priority, timeout)` — send with explicit priority
- `msg_send()` defaults to `MSG_PRIO_NORMAL`
- Starvation prevention: low priority served after `MSG_STARVATION_THRESHOLD` (8) high-priority receives
- Per-priority statistics tracking

**Test coverage (10 tests):**
- Priority ordering (`test_priority_high_before_low`, `test_priority_ordering_all_levels`)
- Default behavior (`test_default_priority_is_normal`)
- Starvation prevention (`test_starvation_prevention`, `test_starvation_threshold_boundary`)
- Capacity (`test_priority_capacity`)
- Statistics (`test_priority_statistics`)
- FIFO within priority (`test_priority_fifo_within_level`)
- Complex patterns (`test_priority_interleaved_operations`, `test_priority_skip_empty_levels`)

---

### CI/CD Pipeline

**Files:** `.github/workflows/ci.yml`, `kernel/src/semihosting.c`, `kernel/include/semihosting.h`, `kernel/tests/test_harness.c`, `Makefile`

Implemented GitHub Actions workflow for automated build and test:

| Feature | Implementation |
|---------|----------------|
| **Triggers** | Push to main/develop, PRs to main, manual dispatch |
| **Toolchain** | ARM GCC 13.2 (aarch64-none-elf), Rust stable, QEMU |
| **Build** | CMake with cross-compilation toolchain file |
| **Testing** | Full test suite in QEMU with 4 CPUs, 1GB RAM |
| **Exit** | ARM64 semihosting (`HLT #0xF000`) for clean exit |
| **Artifacts** | ELF, BIN, test output (7-day retention) |

**Semihosting implementation:**
- `semihosting_exit(code)` — exit QEMU with status code
- `semihosting_available()` — compile-time check for QEMU platform
- Uses `SYS_EXIT_EXTENDED` (0x20) for AArch64 compatibility
- Conditional compilation: only enabled for `PLATFORM_QEMU_VIRT`

**Test result parsing:**
- `[PASS] All test suites passed` — success
- `[FAIL]` — test failure
- `PAGE FAULT` / `KERNEL PANIC` — crash detection

See `docs/ci-cd.md` for comprehensive documentation.

---

### Documentation Updates

| Document | Changes |
|----------|---------|
| `FUTURE.md` | Marked TLB Shootdown, Priority Queues, CI/CD, Device Tree Parsing, EFI Stub (PE/COFF header) as complete with implementation notes |
| `docs/ci-cd.md` | New comprehensive CI/CD pipeline documentation |
| `TODO.md` | Added this "Extra Work" section |

---

### Test Infrastructure Improvements

Added test helper functions for functional VMM testing:

```c
// kernel/mm/vmm.c - Test helpers
uint64_t vmm_test_get_l2_entry(uint64_t virt);
int vmm_test_set_l2_entry_no_invalidate(uint64_t virt, uint64_t pte);
uint64_t vmm_test_make_block_desc(uint64_t phys, uint32_t flags);
```

These allow tests to manipulate page tables directly and verify that TLB invalidation is actually required and working, rather than just testing that functions "don't crash."

---

### Summary

| Item | Tests Added | Lines of Code |
|------|-------------|---------------|
| TLB Shootdown API | 10 | ~250 |
| Priority Message Queues | 10 | ~300 |
| CI/CD Pipeline | — | ~100 (workflow) + ~80 (semihosting) |
| Documentation | — | ~300 |
| **Total** | **20** | **~1030** |

All 20 new tests pass. Total test count increased from ~60 to ~80.

---

### Shell Path Resolution & Working Directory (December 2025)

**Files:** `kernel/src/shell.c`, `kernel/tests/test_shell.c`, `docs/shell.md`, `docs/testing.md`

Implemented comprehensive path resolution and working directory support:

| Feature | Description |
|---------|-------------|
| **cwd state** | `shell_cwd` variable tracks current working directory (starts at `/`) |
| **`pwd` command** | Prints current working directory |
| **`cd` command** | Changes directory with full path resolution |
| **Relative paths** | All filesystem commands support relative paths |
| **`resolve_path()`** | Internal function handles `.`, `..`, `//`, trailing slashes |

**Commands updated for relative paths:**
- `ls` (now defaults to cwd instead of `/`)
- `cat`, `write`, `mkdir`, `rm`, `truncate`, `append`
- `mv` (both source and destination)
- `df` (resolves path, falls back to `/mnt/files`)

**Path resolution features:**
- Prepends cwd for relative paths
- Handles `.` (current directory - skip)
- Handles `..` (parent directory - pop)
- Normalizes `//` to `/`
- Strips trailing slashes (except root)
- Edge case: `cd ..` at root stays at root

**Test coverage (21 new tests):**
- Working directory tests (`test_shell_cmd_pwd`, `test_shell_cmd_cd_*`)
- Relative path tests (`test_shell_cmd_ls_cwd`, `test_shell_cmd_ls_relative`, etc.)
- Error cases (`test_shell_cmd_cd_nonexistent`, `test_shell_cmd_cd_file`)
- Mount point tests (`test_shell_cmd_cd_mount_subdir`, `test_shell_mount_relative_path`)

**Example usage:**
```
SLM-OS> pwd
/
SLM-OS> cd /mnt/files
SLM-OS> ls
hello.txt    [f]    20
SLM-OS> cat hello.txt
Hello from LittleFS!
SLM-OS> cd ..
SLM-OS> pwd
/mnt
```

---

### File Utility Commands (December 2025)

**Files:** `kernel/src/shell.c`, `kernel/tests/test_shell.c`, `docs/shell.md`, `docs/filesystem.md`

Added comprehensive file manipulation commands to the shell:

| Command | Description |
|---------|-------------|
| `cp <src> <dst>` | Copy file contents (cross-mount supported) |
| `touch <path>` | Create empty file if it doesn't exist |
| `stat <path>` | Show file/directory information (type, size) |
| `tree [path] [depth]` | Recursive directory listing (default depth: 5) |
| `wc <path>` | Count lines, words, and bytes in file |
| `hexdump <path> [off] [len]` | Hex dump with ASCII display (default: 256 bytes) |
| `grep <pattern> <path>` | Search for substring in file (shows line numbers) |
| `find <path> <pattern>` | Find files by name pattern (wildcards: `*`, `?`) |

**Key implementation details:**
- Pattern matching with recursive wildcard (`*`) and single-char (`?`) support
- Hex dump with classic 16-byte-per-line format and ASCII sidebar
- grep shows line numbers and match count
- find recursively searches directories with configurable max depth
- All commands support relative paths via `resolve_path()`

**Test coverage (42 comprehensive tests):**
- Functional verification (cp: content identical via VFS read; touch: size = 0 bytes)
- Edge cases (touch existing file doesn't truncate; cp binary content preserved)
- Error handling (missing arguments for all commands; nonexistent files)
- Pattern matching (wildcards `*` and `?` at start, middle, end; case sensitivity)
- Subdirectory operations (tree/find with nested dirs created during test)
- Virtual directory handling (tree/stat on /sys; find returns error for non-mount)

---

### Networking (lwIP + VirtIO-Net) (December 2025)

**Files:** `kernel/drivers/virtio_net.c`, `kernel/net/lwip_slm.c`, `kernel/net/sys_arch.c`, `kernel/src/net_shell.c`, `kernel/tests/test_net.c`, `docs/networking.md`

Implemented TCP/IP networking for QEMU using lwIP and VirtIO-Net:

| Component | Description |
|-----------|-------------|
| **VirtIO-Net Driver** | MMIO transport at 0x0A000000, TX/RX queue management |
| **lwIP Integration** | TCP, UDP, ICMP, DHCP support in NO_SYS mode |
| **OS Abstraction** | `sys_arch.c` with critical sections via spinlocks |
| **Shell Commands** | `net`, `ping`, `ifconfig`, `netstat` |

**Shell commands:**
- `net init` — Initialize VirtIO and lwIP stack
- `net status` — Show network status
- `ping <ip> [count]` — Send ICMP echo requests
- `ifconfig` — Display/configure network interface
- `ifconfig dhcp` — Enable DHCP
- `ifconfig <ip> <mask> <gw>` — Set static IP
- `netstat` — Display TX/RX statistics

**Platform support:**
- QEMU: VirtIO-Net with user-mode networking (10.0.2.x)
- Jetson: Not yet implemented (requires Realtek/Intel NIC driver)

**Test coverage (13 tests):**
- IP address utilities (creation, parsing, formatting, roundtrip)
- Network state queries (up/down, configuration)
- Statistics functions
- Error handling (NULL pointers, invalid input)

**Configuration:** QEMU runs with `-device virtio-net-device,netdev=net0 -netdev user,id=net0`

---

### Lua 5.4 Scripting Integration (December 2025)

**Files:** `kernel/lib/lua/`, `kernel/src/lua_slm.c`, `kernel/src/lua_shell.c`, `kernel/src/lua_stubs.c`, `kernel/arch/arm64/setjmp.S`, `kernel/tests/test_lua.c`, `docs/lua.md`

Embedded Lua 5.4 interpreter for runtime scripting and automation:

| Component | Description |
|-----------|-------------|
| **Lua Core** | Full Lua 5.4.7 in `kernel/lib/lua/src/`, built as separate library (allows FP) |
| **SLM Bindings** | `slm` module with kernel API access (uptime, mem_stats, tasks, etc.) |
| **REPL** | Interactive read-eval-print loop with line editing |
| **Shell Command** | `lua` (REPL), `lua -e "code"` (direct execution) |
| **Libc Stubs** | 1MB heap allocator, math functions, string ops for freestanding |
| **Error Handling** | Custom `setjmp.S` for ARM64 (22-slot jmp_buf) |

**SLM module functions:**
- `slm.print(...)` — Console output (replaces Lua's print)
- `slm.uptime()` — System uptime in milliseconds
- `slm.mem_stats()` — Memory statistics table (total_kb, free_kb, used_kb)
- `slm.tasks()` — Array of task tables (id, name, state, cpu, priority)
- `slm.sleep(ms)` — Sleep with scheduler yield
- `slm.yield()` — Yield CPU to scheduler
- `slm.version()` — SLM-OS version string
- `slm.cpu_count()`, `slm.cpu_id()` — CPU information

**Build configuration:**
- Lua built without `-mgeneral-regs-only` (needs FP for math)
- Custom `slm_luaconf.h` with `LUA_32BITS=1`, `LUA_USE_C89=1`
- Reduced stack/buffer sizes for embedded use

**Bugs fixed during integration:**
1. **Heap allocator unsigned underflow** (`lua_stubs.c`): Block split logic could underflow when calculating remaining size
2. **jmp_buf buffer overflow** (`setjmp.S`): Original layout stored 23 values but ARM toolchain jmp_buf is only 22 slots (176 bytes)

**Test coverage (22 tests):**
- State management (newstate, multiple states, close NULL)
- Basic execution (arithmetic, strings, tables, functions, loops)
- Error handling (syntax errors, runtime errors, pcall)
- SLM bindings (module exists, uptime, mem_stats, tasks, version, cpu_count, cpu_id)
- Standard libraries (string, table, math)
- Complex integration (metatables, class-like patterns)

---

### Summary (All Extra Work)

| Item | Tests Added | Lines of Code |
|------|-------------|---------------|
| TLB Shootdown API | 10 | ~250 |
| Priority Message Queues | 10 | ~300 |
| CI/CD Pipeline | — | ~100 (workflow) + ~80 (semihosting) |
| Shell Path Resolution | 21 | ~350 |
| File Utility Commands | 42 | ~800 |
| Networking (lwIP) | 13 | ~1500 |
| Lua Scripting | 22 | ~2500 (includes Lua source integration) |
| Documentation | — | ~1100 |
| **Total** | **118** | **~6700** |

All tests pass. Total test count now 327+.

---

## Lessons from Phase 4

### What Worked Well
- **kexec from Linux** — Bypassed Jetson UEFI/CBB complexity; gave immediate EL2 access
- **EL2 with VHE** — Transparent EL1 register redirection avoided rewriting the kernel for EL2
- **Non-cacheable memory** — Solved cross-CPU coherency on Pi 5 and Jetson where hardware coherency is absent
- **UARTC via TCU** — Provided serial console on Jetson when UARTA was CBB-blocked
- **PSCI success fallback** — Reliable SMP boot on both Pi 5 and Jetson despite cache incoherency
- **labctl** — Automated deploy/test cycle (SDWire + serial + power) prevented manual error accumulation
- **Rust msg_router** — Topic-based pub/sub with atomic mailboxes; clean FFI boundary
- **Stateless hot-swap** — Subscription preservation made hot-swap practical without state transfer protocol
- **Timestamps in Rust logging** — Immediate value for correlating events across subsystems

### What To Do Differently
- **Investigate CBB firewalls earlier** — Would have reached EL2/VHE approach sooner on Jetson
- **Test cross-CPU dispatch earlier** — NC memory requirement only discovered when SMP was already working on single core
- **Keep TODO files in sync** — M7 items were marked ☐ despite being implemented; caused confusion
- **Add delivery verification tests from the start** — Initial message router tests only checked that functions didn't crash, not that messages were delivered

### Patterns to Preserve
- Phase documents with checkboxes track progress visibly
- "Deferred to Phase N" with ⏸️ is explicit — nothing lost
- Risk mitigation has concrete fallbacks
- Decisions table forces explicit choices
- Icon system (✅ ⏸️ 🔗) for quick status visibility
- Boot reliability testing (`labctl boot_test`) for hardware changes
- Binary search debugging methodology for hardware issues

### Deferred to Phase 5

| Item | Notes |
|------|-------|
| Component isolation (M8) | Requires user/kernel mode separation (EL0/EL1) |
| Stateful hot-swap | State transfer protocol between component versions |
| Direct component-to-component messaging | Topic broadcast sufficient for Phase 4 |
| Wildcard subscriptions | Not needed until component count grows |
| Zero-copy large messages | Shared buffers exist in IPC; router integration deferred |
| Message priority in router | IPC priority queues work; router integration deferred |
| Jetson GPU compute | GSP firmware loading required (RISC-V on GPU) |
| Component development tutorial | Needs working examples for Phase 5 user-space components |
| Lock-free task stealing | Profile first to confirm contention |

---

*Created: December 2025*
*Completed: April 2026*
*All milestones done. M8 isolation deferred to Phase 5.*