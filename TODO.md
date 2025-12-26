# Phase 4: Hardware Bring-Up & Component System

This document tracks Phase 4 implementation of SLM-OS.

**Status:** In Progress

**Summary:** Phase 4 combines hardware bring-up work deferred from Phase 3 (blocked on serial adapter) with the Component System milestone from the original roadmap.

**Goals:**
- Complete Jetson Orin Nano hardware bring-up
- Jetson GPU driver implementation
- Component hot-swap mechanism
- Component isolation and message routing
- Shell/ELF execution improvements

**Carried from Phase 3:**
- GPU Memory Integration (cache coherency, SHM_GPU_ACCESSIBLE)
- Jetson GPU Driver (init, alloc, submit, fence)
- Hardware bring-up (serial console, GIC, timer, GPIO, MMU testing)
- Shell improvements (virtual filesystem, ELF argc/argv, kill command)
- Lock-free task stealing (optional)

---

## Icon Key

| Icon | Meaning |
|------|---------|
| ☐ | Not started |
| ✅ | Complete |
| ⏸️ | Deferred to later phase |
| 🔗 | Has dependency on another milestone (shown as ⏸️🔗 or ☐🔗) |

---

## Milestone 1: Jetson Serial Console & Boot

**Priority:** CRITICAL — Unblocks all other Jetson work

**Reference:** See `docs/lab-operations.md` for serial console procedures and Cygwin setup.

### Serial Console (40-pin Header UART)
- ☐ Connect USB-serial adapter to 40-pin header (Pin 8 TXD, Pin 10 RXD, Pin 6 GND)
- ☐ Test Tegra UART driver (NS16550-compatible @ 0x03100000)
- ☐ Verify BPMP clock enable for UARTA works
- ☐ Confirm baud rate settings (115200 8N1)
- ☐ Test bidirectional communication (shell input/output)

**Note:** UARTA may require clock enable via BPMP before it can output anything. If no serial output on first boot, this is the likely cause. Debug by verifying Linux can use the same UART first.

### Boot Method Validation
- ☐ Test kexec boot with serial console output
- ☐ Debug any silent failures with serial visibility
- ☐ Set up SD card boot for standalone SLM-OS
- ☐ Document working boot sequence in `docs/jetson-boot.md`
- ⏸️ U-Boot/UEFI direct boot — after kexec is stable

### Platform Validation
- ☐ Verify DTB parsing on real Jetson hardware
- ☐ Confirm memory map matches DTB values
- ☐ Remove hardcoded addresses from `platform.h` (use DTB values)
- ⏸️ Test on Pi 5 with platform-specific DTB — later (Jetson first)

---

## Milestone 2: Jetson Hardware Validation

**Depends on:** M1 (Serial Console)

### GIC and Interrupts
- ☐ Verify GIC configuration for Jetson (may differ from QEMU)
- ☐ Test interrupt delivery on real hardware
- ☐ Verify timer IRQ fires at expected rate (100 Hz)
- ☐ Test GIC affinity settings for core isolation

### Timer
- ☐ Verify ARM generic timer works on Jetson
- ☐ Implement Jetson-specific timer if needed
- ☐ Calibrate timer frequency against real hardware

### Multi-Core
- ☐ Update `MAX_CPUS` in `config.h` from 4 to 6 (or 8 for headroom)
- ☐ Test multi-core boot on Jetson (6 cores vs QEMU's 4)
- ☐ Verify PSCI CPU_ON works for all secondary cores
- ☐ Test per-core scheduling on 6 cores
- ☐ Multi-core stress test on real hardware

### MMU
- ☐ Test MMU with Jetson's actual memory map
- ☐ Verify device memory mappings (UART, GIC, GPU registers)
- ☐ Test model memory regions on real hardware

### GPIO
- ☐ Test GPIO driver on real pins
- ☐ LED blink test for visual confirmation
- ☐ Document available GPIO pins on 40-pin header

---

## Milestone 3: Jetson GPU Driver

**Depends on:** M1 (Serial Console), M2 (Hardware Validation)

### GPU Initialization
- ☐ Write `jetson_gpu_init()` — power on, clock enable, reset sequence
- ☐ Enable GPU clocks via BPMP IPC
- ☐ Verify GPU is responsive (read ID registers)
- ☐ Document initialization sequence in `docs/gpu.md`

### GPU Memory Management
- ☐ Write `jetson_gpu_alloc(size)` — allocate GPU-accessible memory
- ☐ Write `jetson_gpu_free(addr)` — free GPU memory
- ☐ Handle cache coherency (flush before GPU access, invalidate after)
- ☐ Use `SHM_GPU_ACCESSIBLE` flag from Phase 2 shared buffers
- ☐ Integrate with model memory allocator

### GPU Command Submission (Stretch)
- ⏸️ Write `jetson_gpu_submit(cmd_buffer)` — submit work to GPU
- ⏸️ Write `jetson_gpu_wait()` — wait for GPU completion
- ⏸️ Implement basic fence/sync mechanism
- ⏸️ Test simple compute operation (if possible without CUDA)

**Note:** Full GPU compute requires GSP firmware. Memory allocation and cache coherency are the primary goals; actual compute deferred to Phase 5.

### GPU Testing
- ☐ Test GPU initialization on real Jetson hardware
- ☐ Verify GPU memory allocation works
- ☐ Verify CPU can read GPU-written data correctly (cache coherency)
- ☐ Benchmark memory transfer performance

---

## Milestone 4: Performance Benchmarking

**Depends on:** M2 (Hardware Validation)

### Context Switch Performance
- ☐ Measure actual context switch time on Jetson
- ☐ Target: < 10 µs
- ☐ Compare with QEMU measurements
- ☐ Profile and optimize if needed

### Interrupt Latency
- ☐ Measure actual interrupt latency on Jetson
- ☐ Measure worst-case latency under load
- ☐ Document results in `docs/performance.md`

### Scheduler Performance
- ☐ Run scheduler stress tests on real hardware
- ☐ Measure deadline accuracy under load
- ☐ Test core isolation effectiveness

### IPC Performance
- ☐ IPC stress test on real hardware
- ☐ Measure message passing latency
- ☐ Measure shared buffer throughput

### Full Test Suite
- ☐ All Phase 1-3 tests pass on Jetson
- ☐ Document any test failures and fixes
- ☐ Create Jetson-specific test configuration if needed

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
- ☐🔗 Test shell on Jetson (requires M1)
- ☐ Verify all shell commands work on real hardware

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

### Hot-Swap Mechanism
- ⏸️ Design state transfer protocol between old and new component versions — Phase 5+
- ⏸️ Implement `component_hot_swap(old_name, new_component)` — Phase 5+
- ⏸️ Pause old component — Phase 5+
- ⏸️ Transfer state from old to new — Phase 5+
- ⏸️ Update message routing — Phase 5+
- ⏸️ Start new component — Phase 5+
- ⏸️ Cleanup old component — Phase 5+
- ⏸️ Test hot-swap with simple component — Phase 5+

### Shell Commands
- ✅ `component list` — list loaded components
- ✅ `component register` — register component from shell
- ✅ `component unregister` — unregister component
- ✅ `component status` — show component details

---

## Milestone 7: Message Routing

**Depends on:** M6 (Component System)

### Message Router
- ☐ Implement `MessageRouter` in Rust
- ☐ Topic-based publish/subscribe
- ☐ Direct component-to-component messaging
- ☐ Message queue per component

### Routing Table
- ☐ Build routing table from component manifests
- ☐ Update routing table on component load/unload
- ☐ Support wildcard subscriptions

### Integration with IPC
- ☐ Route messages through kernel IPC primitives
- ☐ Zero-copy for large messages (use shared buffers)
- ☐ Message priority support

### Testing
- ☐ Test message routing between two components
- ☐ Test pub/sub with multiple subscribers
- ☐ Test routing table update on hot-swap

---

## Milestone 8: Component Isolation (Stretch)

**Note:** Full isolation requires user/kernel separation. This milestone implements what's possible in kernel space.

### Memory Isolation
- ☐ Separate heap regions per component
- ☐ Prevent components from accessing each other's memory
- ☐ Shared memory only via explicit shared buffers

### Resource Limits
- ☐ Memory limit per component
- ☐ CPU time accounting per component
- ⏸️ CPU time limit enforcement — requires preemption improvements

### Fault Isolation
- ☐ Component crash doesn't crash kernel
- ☐ Automatic component restart on failure
- ☐ Error reporting to system log

---

## Milestone 9: Deferred Polish Items

### Rust Runtime
- ☐ Format `PanicInfo` into buffer for detailed panic messages
- ☐ Improve Rust logging with timestamps

### Scheduler
- ⏸️ Lock-free task stealing between queues — profile first to confirm need
- ☐ Big.LITTLE core assignment (`assign_to_performance_core()`, `assign_to_efficiency_core()`)

### Documentation
- ☐ Update architecture doc with Phase 4 learnings
- ☐ Document component system API
- ☐ Document Jetson-specific setup and configuration
- ☐ Create component development tutorial

---

## Phase 4 Completion Checklist

### Deliverables
- ☐ Kernel boots and runs on real Jetson Orin Nano hardware
- ☐ Serial console working via 40-pin header UART
- ☐ All Phase 1-3 tests pass on Jetson
- ☐ GPU initialized, memory allocation working
- ☐ Performance benchmarks documented
- ☐ At least one example component loading and running
- ☐ Hot-swap demonstrated (same component, new version)
- ☐ Message routing between components working

### Demo
- ☐ Boot on Jetson Orin Nano via serial console
- ☐ Show shell commands working on real hardware
- ☐ Show component load/unload via shell
- ☐ Show hot-swap of a component
- ☐ Show message passing between components
- ☐ Compare QEMU vs Jetson performance

---

## Outstanding Decisions

### Milestone 1 — Serial Console

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Primary debug method** | 40-pin UART vs USB-C TCU | **40-pin UART** — TCU requires SPE firmware cooperation |
| **Baud rate** | 115200 vs higher | **115200** — standard, reliable |

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
    - Local copy: `H:\My Drive\Capstone\Documentation\Orin-TRM_DP10508002_v1.2p.pdf`
- NVIDIA L4T (Linux for Tegra) source code for driver reference
- `docs/lab-operations.md` for serial console procedures and remote lab access
- `docs/jetson-tcu.md` for TCU/HSP research notes
- `docs/platform-abstraction.md` for QEMU vs Jetson differences

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

*Created: December 2025*
*Target: Complete hardware bring-up first, then component system*