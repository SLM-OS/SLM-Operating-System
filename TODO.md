# Phase 2: Core Features (C + Rust Scaffolding)

This document tracks Phase 2 implementation of SLM-OS.

**Status:** In progress (Milestones 1-3 complete)

**Goals:**
- Virtual memory with 2-level page tables
- Multi-core boot and scheduling
- Basic IPC (message queues)
- Rust toolchain integration and FFI boundary

**Carried from Phase 1:**
- Task stack cleanup/memory reclamation

---

## Milestone 1: Virtual Memory System ✅

**Status:** Complete

### MMU Research ✅
- ✅ Study ARM64 MMU architecture (TCR_EL1, TTBR0/TTBR1, MAIR)
- ✅ Review translation table formats (4KB granule, 2-level for 2MB pages)
- ✅ Document memory attribute options (Normal, Device, Non-cacheable)
- ✅ Plan kernel vs user address space split (TTBR0 vs TTBR1)

See `docs/mmu.md` for comprehensive documentation.

### Page Table Implementation ✅
- ✅ Define page table entry structures in `kernel/include/vmm.h`
- ✅ Implement Level 1 table (512 entries × 1GB regions)
- ✅ Implement Level 2 table (512 entries × 2MB blocks)
- ✅ Write `vmm_init()` — create initial kernel mappings
- ✅ Write `vmm_map_block(virt, phys, flags)` — map single 2MB block
- ✅ Write `vmm_unmap_block(virt)` — remove mapping
- ✅ Write `vmm_map_region(virt, phys, size, flags)` — map contiguous region

### MMU Enable Sequence ✅
- ✅ Set up MAIR_EL1 with memory attributes (Device, Normal NC, Normal WB)
- ✅ Configure TCR_EL1 for 39-bit VA, 4KB granule
- ✅ Populate initial page tables (identity map + kernel high map via shared L1)
- ✅ Write `mmu_enable()` in `kernel/src/mmu.S`
- ✅ Handle transition from physical to virtual addressing
- Deferred: Update linker script for virtual addresses (RWX warning fix)

### Model Memory Flags (SLM-Specific) ✅
- ✅ Implement `gpu_mapped` flag handling (PTE_SW_GPU_MAPPED)
- ✅ Implement `model_page` flag handling (PTE_SW_MODEL_PAGE)
- ✅ Implement `inference_hot` flag handling (PTE_SW_INFERENCE_HOT)
- ✅ Flags defined in vmm.h, preserved in block descriptors

### VMM Testing ✅
- ✅ Verify kernel code runs correctly after MMU enable
- ✅ Test mapping/unmapping pages dynamically
- ✅ Verify UART still works after MMU enable (device memory mapping)
- ✅ Automated test suite with `make test`
- Deferred: Test page fault handling (basic — panic with useful info)

---

## Milestone 2: Multi-Core Support ✅

### SMP Research ✅
- ✅ Study ARM64 PSCI (Power State Coordination Interface)
- ✅ Document QEMU virt machine SMP boot method
- ✅ Review spin-table vs PSCI boot protocols
- ✅ Plan per-CPU data structures

See `docs/smp.md` for comprehensive documentation.

### Secondary Core Bring-Up ✅
- ✅ Implement PSCI `CPU_ON` call
- ✅ Write `smp_boot.S` — secondary core entry point
- ✅ Set up per-core stacks
- ✅ Initialize per-core GIC CPU interface
- ✅ Implement `smp_init()` — bring up all secondary cores
- ✅ Add core ID detection (`mpidr_el1` parsing)

### Per-Core Scheduler ✅
- ✅ Create per-core run queues (`struct cpu_runqueue` in `sched.c`)
- ✅ Implement core affinity in task structure (`cpu_affinity`, `assigned_cpu`)
- ✅ Update `schedule()` for multi-core awareness (per-CPU scheduling)
- ✅ Add spinlocks for scheduler data structures (global `sched.lock`)
- ✅ Implement `sched_migrate_task(task, target_cpu)`
- ✅ Per-CPU current task tracking (`task_current()` uses `cpu_id()`)

### Synchronization Primitives ✅
- ✅ Implement spinlock (`spin_lock`, `spin_unlock`, `spin_trylock`)
- ✅ Implement ticket lock (fairer than simple spinlock)
- ✅ Add memory barriers where needed (`dmb`, `dsb`, `isb`)
- ✅ Add IRQ-safe spinlock variants (`spin_lock_irqsave`, `spin_unlock_irqrestore`)
- ✅ Test lock correctness (9 unit tests in `smp.c`)
- ✅ Multi-core contention stress testing (3 tasks × 50 increments with spinlock)

### SMP Testing ✅
- ✅ Verify all cores boot and reach idle loop (via `smp_run_tests()`)
- ✅ Run tasks on different cores simultaneously (task_a/b/c on CPUs 1/2/3)
- ✅ Test cross-core task migration (`sched_migrate_task`)
- ✅ Stress test with many tasks across all cores (6 tasks, 2 per CPU)
- ✅ Verify no deadlocks or race conditions (lock contention test passes)

---

## Milestone 3: Basic IPC ✅

**Status:** Complete

### IPC Design ✅
- ✅ Define message structure (`struct slm_message` — 64 bytes, cache-line aligned)
- ✅ Define shared buffer structure (`struct shared_buffer` with refcounting)
- ✅ Plan message queue implementation (ring buffer chosen)
- ✅ Decide on blocking vs non-blocking semantics (sleep/wake for blocking)

See `kernel/include/ipc.h` for full API definition.

### Message Queue Implementation ✅
- ✅ Implement `struct msg_queue` — fixed-size ring buffer
- ✅ Write `msg_queue_create(capacity, msg_size)` — allocate queue with configurable message size
- ✅ Write `msg_queue_destroy(queue)` — free queue
- ✅ Write `msg_send(queue, msg, timeout)` — send message
- ✅ Write `msg_recv(queue, msg, timeout)` — receive message
- ✅ Implement blocking with task sleep/wake
- ✅ Write `msg_queue_lookup(id)` — find queue by ID

### Shared Buffer Implementation ✅
- ✅ Write `shared_buffer_create(size, flags)` — allocate shared memory (2MB aligned)
- ✅ Write `shared_buffer_map(buffer, task, perms)` — map into task's address space
- ✅ Write `shared_buffer_unmap(buffer, task)` — remove mapping
- ✅ Implement reference counting for safe cleanup
- ✅ Add `SHM_GPU_ACCESSIBLE` flag support (for future GPU integration)
- ✅ Write `shared_buffer_lookup(id)` — find buffer by ID
- ✅ Write `shared_buffer_phys_addr(buffer)` — get physical address for DMA/GPU

### IPC Testing ✅
- ✅ Test message queue create/destroy
- ✅ Test non-blocking send/recv (empty/full conditions)
- ✅ Test queue lookup by ID
- ✅ Test shared buffer create/destroy
- ✅ Test shared buffer map/unmap with read/write verification
- ✅ Test buffer lookup by ID
- Deferred: Multi-task producer/consumer test (requires more complex test harness)
- Deferred: Memory leak verification (no task cleanup yet)

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

### Linker Script Security
- [ ] Fix RWX segment warning in kernel ELF
- [ ] Separate .text (RX) from .data/.bss (RW) in kernel.ld
- [ ] Use proper MEMORY regions with permissions
- [ ] Important for user/kernel separation

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

### Milestone 1 — Virtual Memory ✅ (Resolved)

| Decision | Options | Choice |
|----------|---------|--------|
| **Page Granule** | 4KB vs 16KB vs 64KB | **4KB** — standard, well-documented, sufficient for Phase 2 |
| **Address Space** | TTBR0 only vs TTBR0 + TTBR1 | **TTBR0 + TTBR1 shared L1** — simpler for boot; will split for user space later |
| **Mapping Strategy** | Separate tables vs shared | **Identity + high kernel (shared table)** — single L1 table serves both |
| **Block Size** | 4KB pages vs 2MB blocks | **2MB** — reduces table depth; sufficient granularity for kernel |

### Milestone 2 — Multi-Core ✅ (Resolved)

| Decision | Options | Choice |
|----------|---------|--------|
| **Boot Protocol** | PSCI vs spin-table | **PSCI** — more portable, works on QEMU and Jetson |
| **Scheduler Lock Granularity** | Global lock vs per-queue locks | **Global lock** for Phase 2; per-queue locks planned for Phase 3 |
| **Load Balancing** | None vs periodic rebalancing | **Deferred to Phase 3** — SLM-based scheduler may supersede |

### Milestone 3 — IPC ✅ (Resolved)

| Decision | Options | Choice |
|----------|---------|--------|
| **Message Queue Type** | Ring buffer vs linked list | **Ring buffer** — cache-friendly, no per-message allocation, predictable latency |
| **Blocking Implementation** | Busy-wait vs sleep/wake | **Sleep/wake** — messages may take ms to arrive; spinning wastes CPU |
| **Max Message Size** | Fixed vs variable | **64 bytes default, per-queue configurable** — cache-line aligned; large data via shared buffers |

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
| Per-queue scheduler locks | Currently using global lock; will implement per-queue locks for scalability |
| Load balancing | Periodic task rebalancing across cores (SLM scheduler may supersede) |
| Component system | Phase 4 |
| Jetson hardware testing | Can continue on QEMU for Phase 2 |

### Future Considerations

| Topic | Notes |
|-------|-------|
| **UART Synchronization** | Currently no locking on UART output, causing garbled interleaved output during concurrent access (especially at boot). Intentionally left unlocked to avoid deadlock risks with panics and nested prints. Future approaches to consider: (1) Per-CPU ring buffers that drain to UART from one CPU, (2) Message-level locking with trylock fallback for direct output, (3) Accept as debug build artifact since SLM workloads won't print much at runtime. |

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
