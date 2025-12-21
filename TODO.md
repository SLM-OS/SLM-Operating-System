# Phase 2: Core Features (C + Rust Scaffolding)

This document tracks Phase 2 implementation of SLM-OS.

**Status:** Not started

**Goals:**
- Virtual memory with 2-level page tables
- Multi-core boot and scheduling
- Basic IPC (message queues)
- Rust toolchain integration and FFI boundary

**Carried from Phase 1:**
- Task stack cleanup/memory reclamation

---

## Milestone 1: Virtual Memory System

### MMU Research
- [ ] Study ARM64 MMU architecture (TCR_EL1, TTBR0/TTBR1, MAIR)
- [ ] Review translation table formats (4KB granule, 2-level for 2MB pages)
- [ ] Document memory attribute options (Normal, Device, Non-cacheable)
- [ ] Plan kernel vs user address space split (TTBR0 vs TTBR1)

### Page Table Implementation
- [ ] Define `slm_pte_t` structure (as specified in design doc)
- [ ] Implement Level 1 table (512 entries × 1GB regions)
- [ ] Implement Level 2 table (512 entries × 2MB pages)
- [ ] Write `vmm_init()` — create initial kernel mappings
- [ ] Write `vmm_map_page(virt, phys, flags)` — map single 2MB page
- [ ] Write `vmm_unmap_page(virt)` — remove mapping
- [ ] Write `vmm_map_region(virt, phys, size, flags)` — map contiguous region

### MMU Enable Sequence
- [ ] Set up MAIR_EL1 with memory attributes
- [ ] Configure TCR_EL1 for 2-level translation
- [ ] Populate initial page tables (identity map + kernel high map)
- [ ] Write `mmu_enable()` — flush caches, set TTBR, enable MMU
- [ ] Handle transition from physical to virtual addressing
- [ ] Update linker script for virtual addresses

### Model Memory Flags (SLM-Specific)
- [ ] Implement `gpu_mapped` flag handling
- [ ] Implement `model_page` flag handling
- [ ] Implement `inference_hot` flag handling
- [ ] Test flag preservation across map/unmap cycles

### VMM Testing
- [ ] Verify kernel code runs correctly after MMU enable
- [ ] Test mapping/unmapping pages dynamically
- [ ] Test page fault handling (basic — panic with useful info)
- [ ] Verify UART still works after MMU enable (device memory mapping)

---

## Milestone 2: Multi-Core Support

### SMP Research
- [ ] Study ARM64 PSCI (Power State Coordination Interface)
- [ ] Document QEMU virt machine SMP boot method
- [ ] Review spin-table vs PSCI boot protocols
- [ ] Plan per-CPU data structures

### Secondary Core Bring-Up
- [ ] Implement PSCI `CPU_ON` call
- [ ] Write `smp_boot.S` — secondary core entry point
- [ ] Set up per-core stacks
- [ ] Initialize per-core GIC CPU interface
- [ ] Implement `smp_init()` — bring up all secondary cores
- [ ] Add core ID detection (`mpidr_el1` parsing)

### Per-Core Scheduler
- [ ] Create per-core run queues
- [ ] Implement core affinity in task structure
- [ ] Update `schedule()` for multi-core awareness
- [ ] Add spinlocks for scheduler data structures
- [ ] Implement `sched_migrate_task(task, target_core)`

### Synchronization Primitives
- [ ] Implement spinlock (`spin_lock`, `spin_unlock`)
- [ ] Implement ticket lock (fairer than simple spinlock)
- [ ] Add memory barriers where needed (`dmb`, `dsb`, `isb`)
- [ ] Test lock correctness under contention

### SMP Testing
- [ ] Verify all cores boot and reach idle loop
- [ ] Run tasks on different cores simultaneously
- [ ] Test cross-core task migration
- [ ] Stress test with many tasks across all cores
- [ ] Verify no deadlocks or race conditions

---

## Milestone 3: Basic IPC

### IPC Design
- [ ] Define message structure (`struct slm_message` from design doc)
- [ ] Define shared buffer structure (`struct slm_shared_buffer`)
- [ ] Plan message queue implementation (ring buffer vs linked list)
- [ ] Decide on blocking vs non-blocking semantics

### Message Queue Implementation
- [ ] Implement `struct msg_queue` — fixed-size ring buffer
- [ ] Write `msg_queue_create(capacity)` — allocate queue
- [ ] Write `msg_queue_destroy(queue)` — free queue
- [ ] Write `msg_send(queue, msg, timeout)` — send message
- [ ] Write `msg_recv(queue, msg, timeout)` — receive message
- [ ] Implement blocking with task sleep/wake

### Shared Buffer Implementation
- [ ] Write `shared_buffer_create(size, flags)` — allocate shared memory
- [ ] Write `shared_buffer_map(buffer, task)` — map into task's address space
- [ ] Write `shared_buffer_unmap(buffer, task)` — remove mapping
- [ ] Implement reference counting for safe cleanup
- [ ] Add `GPU_ACCESSIBLE` flag support (for future GPU integration)

### IPC Testing
- [ ] Test single-producer single-consumer messaging
- [ ] Test multiple producers, single consumer
- [ ] Test blocking behavior (sender blocks when full, receiver blocks when empty)
- [ ] Test shared buffer mapping into multiple tasks
- [ ] Verify no memory leaks after IPC teardown

---

## Milestone 4: Rust Integration

### Rust Toolchain Setup
- [ ] Verify Rust `aarch64-unknown-none` target is installed
- [ ] Create `runtime/` crate with `Cargo.toml`
- [ ] Configure `no_std` and `no_main` for freestanding environment
- [ ] Set up `.cargo/config.toml` for cross-compilation
- [ ] Create custom target JSON if needed for bare-metal specifics
- [ ] Add `panic = "abort"` to avoid unwinding

### FFI Boundary Definition
- [ ] Create `kernel/include/slm_ffi.h` — C function declarations
- [ ] Create `runtime/src/kernel_ffi.rs` — Rust extern declarations
- [ ] Ensure struct layouts match exactly (`#[repr(C)]`)
- [ ] Define error codes for FFI functions
- [ ] Document FFI calling conventions and ownership rules

### FFI Functions (C Side)
- [ ] Implement `slm_map_region()` — wrapper around VMM
- [ ] Implement `slm_unmap_region()` — wrapper around VMM
- [ ] Implement `slm_alloc_pages()` — wrapper around PMM
- [ ] Implement `slm_free_pages()` — wrapper around PMM
- [ ] Implement `slm_get_time_ns()` — current time in nanoseconds
- [ ] Implement `slm_task_create()` — create task from Rust
- [ ] Implement `slm_msg_send()` / `slm_msg_recv()` — IPC wrappers

### FFI Functions (Rust Side)
- [ ] Create safe wrappers in `runtime/src/kernel_ffi.rs`
- [ ] Implement `Result` return types for error handling
- [ ] Add `MemFlags` bitflags type for memory flags
- [ ] Add `KernelError` enum for error codes
- [ ] Write unit tests for FFI type sizes and alignments

### Build Integration
- [ ] Update top-level Makefile to build Rust runtime
- [ ] Link Rust static library (`.a`) with C kernel
- [ ] Handle Rust symbols in linker script
- [ ] Verify combined binary boots in QEMU

### First Rust Code
- [ ] Write simple Rust function callable from C
- [ ] Call Rust function from `kernel_main()` as proof of concept
- [ ] Print "Hello from Rust!" via FFI to UART
- [ ] Verify Rust panic handler works (calls C panic)

---

## Milestone 5: Deferred Cleanup from Phase 1

### Task Stack Reclamation
- [ ] Implement `task_destroy()` — full cleanup including stack
- [ ] Add stack to free list on task termination
- [ ] Prevent use-after-free of terminated task structures
- [ ] Test rapid task create/destroy cycles for leaks

### General Cleanup
- [ ] Review and fix any TODO comments from Phase 1
- [ ] Add missing error handling
- [ ] Improve debug output where needed
- [ ] Update documentation

---

## Phase 2 Completion Checklist

### Deliverables
- [ ] Kernel runs with MMU enabled (virtual addresses)
- [ ] All CPU cores boot and run tasks
- [ ] Tasks can communicate via message queues
- [ ] Rust code compiles and links with C kernel
- [ ] At least one Rust function callable from C kernel
- [ ] All code compiles cleanly with `-Wall -Werror` (C) and `#![deny(warnings)]` (Rust)
- [ ] Documentation updated in `docs/`

### Demo
- [ ] Boot kernel in QEMU with MMU enabled
- [ ] Show tasks running on different cores
- [ ] Show inter-task communication via IPC
- [ ] Show Rust code executing (print from Rust)
- [ ] Show memory statistics including virtual memory

---

## Outstanding Decisions

### Milestone 1 — Virtual Memory

| Decision | Options | Considerations | Deadline |
|----------|---------|----------------|----------|
| **Page Granule** | 4KB vs 16KB vs 64KB | 4KB is standard; 64KB reduces TLB pressure for large models | Before starting M1 |
| **Address Space Split** | TTBR0 only vs TTBR0/TTBR1 split | Split is cleaner for future user/kernel separation | Before starting M1 |
| **Initial Mapping Strategy** | Identity map only vs identity + high kernel | High kernel is more realistic for future | Before MMU enable |

### Milestone 2 — Multi-Core

| Decision | Options | Considerations | Deadline |
|----------|---------|----------------|----------|
| **Boot Protocol** | PSCI vs spin-table | PSCI is more portable; spin-table simpler for QEMU | Before starting M2 |
| **Scheduler Lock Granularity** | Global lock vs per-queue locks | Per-queue scales better but more complex | Before per-core scheduler |
| **Load Balancing** | None vs periodic rebalancing | None is simpler; rebalancing helps utilization | Can defer to Phase 3 |

### Milestone 3 — IPC

| Decision | Options | Considerations | Deadline |
|----------|---------|----------------|----------|
| **Message Queue Type** | Ring buffer vs linked list | Ring buffer is cache-friendly; linked list is flexible | Before starting M3 |
| **Blocking Implementation** | Busy-wait vs sleep/wake | Sleep/wake is correct; busy-wait is simpler | Before starting M3 |
| **Max Message Size** | Fixed vs variable | Fixed simplifies allocation; variable more flexible | Before starting M3 |

### Milestone 4 — Rust Integration

| Decision | Options | Considerations | Deadline |
|----------|---------|----------------|----------|
| **Allocator** | None vs `linked_list_allocator` vs custom | Need allocator for `Vec`, `String`; can start without | Before runtime crate grows |
| **Panic Strategy** | Abort vs custom handler calling C panic | Custom handler provides better diagnostics | Before first Rust code |
| **FFI Error Handling** | Integer codes vs tagged union | Integer codes are simpler for C; Rust wraps in Result | Before FFI design |

### Deferred to Phase 3

| Decision | Notes |
|----------|-------|
| Model memory management | Phase 3: "Model memory management" |
| GPU initialization | Phase 3: "GPU initialization (Jetson)" |
| Deadline-aware scheduler | Phase 3: "Deadline-aware scheduler (policy in Rust)" |
| Component system | Phase 4 |
| Jetson hardware testing | Can continue on QEMU for Phase 2 |

---

## Risk Mitigation

### High-Risk Items

1. **MMU Enable Sequence**
    - Many ways to triple-fault or hang silently
    - Mitigation: Enable MMU with identity mapping first, then switch to high kernel
    - Mitigation: Add extensive debug output before/after each step
    - Fallback: Keep non-MMU boot path for debugging

2. **Multi-Core Synchronization**
    - Race conditions are hard to reproduce and debug
    - Mitigation: Start with global scheduler lock, optimize later
    - Mitigation: Use ARM's exclusive load/store for atomics
    - Mitigation: Stress test with many cores and tasks

3. **Rust/C Linking**
    - Symbol visibility, name mangling, ABI mismatches
    - Mitigation: Start with one trivial function, verify it works
    - Mitigation: Use `extern "C"` and `#[no_mangle]` everywhere
    - Fallback: If linking fails, Rust can be deferred to Phase 3

4. **Rust Borrow Checker vs Kernel Patterns**
    - Kernel data structures often have complex ownership
    - Mitigation: Start with simple, obviously-safe Rust code
    - Mitigation: Use `unsafe` sparingly and document why
    - Fallback: Fall back to C++ if Rust becomes a blocker

### Dependencies Between Milestones

```
M1 (VMM) ─────┬──────> M4 (Rust) ──> requires VMM for slm_map_region
              │
              └──────> M3 (IPC) ──> shared buffers need VMM
              
M2 (SMP) ────────────> M3 (IPC) ──> IPC needs locks from SMP work

M5 (Cleanup) ─────────> Can happen in parallel with M1-M4
```

Recommended order: M1 → M2 → M3 → M4 (with M5 throughout)

---

## Resources

### Virtual Memory
- ARM Architecture Reference Manual — Chapter D5 (Address Translation)
- [OS Dev Wiki — ARM Paging](https://wiki.osdev.org/ARM_Paging)
- Linux kernel `arch/arm64/mm/` for reference (not to copy, but to understand)

### Multi-Core
- ARM Architecture Reference Manual — Chapter D1 (AArch64 System Registers)
- PSCI Specification (ARM DEN0022)
- [OS Dev Wiki — SMP](https://wiki.osdev.org/SMP)

### Rust Embedded
- [The Embedonomicon](https://docs.rust-embedded.org/embedonomicon/)
- [rust-embedded/cortex-m](https://github.com/rust-embedded/cortex-m) — ARM Cortex-M, but patterns transfer
- [Writing an OS in Rust](https://os.phil-opp.com/) — x86, but Rust OS patterns are useful

### FFI
- [The Rustonomicon — FFI](https://doc.rust-lang.org/nomicon/ffi.html)
- [Rust Reference — `extern` functions](https://doc.rust-lang.org/reference/items/external-blocks.html)

---

*Created: December 2025*
*Target: Complete before Month 3 (AI Infrastructure)*
