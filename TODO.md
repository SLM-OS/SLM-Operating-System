# Phase 4: Hardware Bring-Up & Component System

This document tracks Phase 4 implementation of SLM-OS.

**Status:** In Progress (Pi 5 bring-up complete, Jetson blocked)

**Summary:** Phase 4 combines hardware bring-up work with the Component System milestone. Pi 5 is now the primary hardware platform with 4-core SMP, preemptive scheduling, and an interactive shell. Jetson remains blocked by the CBB firewall.

**Goals:**
- ✅ Raspberry Pi 5 hardware bring-up (complete — 4-core SMP)
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
- ☐ Run QEMU test suite on Pi 5 hardware
- ☐ Timer-driven sleep/delay functions
- ☐ Interrupt-driven UART (PL011 via RP1 MSI→GIC SPI)
- ☐ Fix task_exit/schedule race on secondary CPUs
- ☐ Investigate SMPEN for proper cache coherency (vs DC CVAC/CIVAC workaround)
- ☐ Re-enable cross-CPU task dispatch (currently pinned to CPU 0 due to SMPEN/cache coherency)

---

## Milestone 1: Jetson Serial Console & Boot

**Priority:** CRITICAL — Unblocks all other Jetson work

**Status:** ⛔ BLOCKED by CBB Firewall — See `docs/jetson-nvidia-support.md`

**Reference:** See `docs/lab-operations.md` for serial console procedures (Ubuntu and Windows).

### ⛔ Critical Blocker: CBB Firewall

All Jetson hardware bring-up is blocked by the Tegra234 Control Backbone (CBB) firewall, which prevents unsigned/unauthenticated code from accessing any peripherals. This is a hardware-enforced security feature.

**Key findings:**
- kexec is explicitly NOT supported by NVIDIA
- Direct UEFI boot has the same CBB restrictions
- L4T bootloader uses kexec internally (extlinux.conf boot also blocked)
- BPMP communication is corrupted after kexec (cannot enable UART clocks)

**Potential solutions (require NVIDIA support):**
1. EL2 hypervisor approach (forum evidence suggests this works)
2. Secure boot integration (sign SLM-OS with PKC/SBK keys)
3. CBB firewall configuration via `tegra234-mb2-bct-scr-*-override.dts`

**Full documentation:** `docs/jetson-nvidia-support.md`

### Serial Console (40-pin Header UART)
- ✅ Connect USB-serial adapter to 40-pin header (Pin 8 TXD, Pin 10 RXD, Pin 6 GND)
- ⛔ Test Tegra UART driver (NS16550-compatible @ 0x03100000) — BLOCKED by CBB firewall
- ⛔ Verify BPMP clock enable for UARTA works — BLOCKED (BPMP IVC corrupted after kexec)
- ✅ Confirm baud rate settings (115200 8N1)
- ✅ Test bidirectional communication (shell input/output) — verified via labctl (Linux only)

**Note:** Serial hardware verified working with Linux (December 2025). SLM-OS serial blocked by CBB firewall.

### Boot Method Validation
- ⛔ Test kexec boot with serial console output — BLOCKED (kexec not supported per NVIDIA)
- ✅ Debug any silent failures with serial visibility — Root cause identified: CBB firewall
- ⛔ Set up SD card boot for standalone SLM-OS — BLOCKED (L4T uses kexec internally)
- ✅ Document working boot sequence in `docs/jetson-boot.md` — Documented blockers instead
- ⛔ U-Boot/UEFI direct boot — BLOCKED (same CBB firewall restrictions)

### Platform Validation
- ⛔ Verify DTB parsing on real Jetson hardware — BLOCKED by boot issues
- ⛔ Confirm memory map matches DTB values — BLOCKED
- ⛔ Remove hardcoded addresses from `platform.h` (use DTB values) — BLOCKED
- ✅ Test on Pi 5 with platform-specific DTB — Pi 5 works, shifted focus here

---

## Milestone 2: Jetson Hardware Validation

**Depends on:** M1 (Serial Console)

**Status:** ⛔ BLOCKED — Depends on M1 which is blocked by CBB firewall

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

**Status:** ⛔ BLOCKED — Depends on M1/M2 which are blocked by CBB firewall

**Additional blocker:** GPU initialization requires GSP (GPU System Processor) firmware, which runs on an on-die RISC-V core. Without GSP firmware documentation or source code, GPU compute is not possible. See `docs/gpu.md` for details.

### GPU Initialization
- ⛔ Write `jetson_gpu_init()` — BLOCKED by CBB firewall + GSP requirement
- ⛔ Enable GPU clocks via BPMP IPC — BLOCKED (BPMP IVC corrupted)
- ⛔ Verify GPU is responsive (read ID registers) — BLOCKED
- ✅ Document initialization sequence in `docs/gpu.md` — Documented blockers

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
- NVIDIA L4T (Linux for Tegra) source code for driver reference
- `docs/lab-operations.md` for serial console procedures and remote lab access
- `docs/jetson-kexec-debugging.md` for kexec boot debugging notes
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

*Created: December 2025*
*Target: Complete hardware bring-up first, then component system*