# Phase 2: Core Features (C + Rust Scaffolding)

This document tracks Phase 2 implementation of SLM-OS.

**Status:** Complete (All 5 milestones done)

**Goals:**
- Virtual memory with 2-level page tables
- Multi-core boot and scheduling
- Basic IPC (message queues)
- Rust toolchain integration and FFI boundary
- Task stack cleanup/memory reclamation (carried from Phase 1)

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
- ✅ Write `mmu_enable()` in `kernel/arch/arm64/mmu.S`
- ✅ Handle transition from physical to virtual addressing
- ⏸️ Update linker script for virtual addresses (RWX warning fix) — deferred

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
- ⏸️ Test page fault handling (basic — panic with useful info) — deferred

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
- ⏸️ Multi-task producer/consumer test — deferred (requires more complex test harness)
- ⏸️ Memory leak verification — deferred (no task cleanup yet)

---

## Milestone 4: Rust Integration ✅

**Status:** Complete

### Rust Toolchain Setup ✅
- ✅ Verify Rust `aarch64-unknown-none` target is installed
- ✅ Create `runtime/` crate with `Cargo.toml`
- ✅ Configure `no_std` and `no_main` for freestanding environment
- ✅ Set up `.cargo/config.toml` for cross-compilation
- N/A: Create custom target JSON — standard target works
- ✅ Add `panic = "abort"` to avoid unwinding

### FFI Boundary Definition ✅
- ✅ Create `kernel/include/slm_ffi.h` — C function declarations
- ✅ Create `runtime/src/kernel_ffi.rs` — Rust extern declarations
- ✅ Ensure struct layouts match exactly (`#[repr(C)]`, `#[repr(transparent)]`)
- ✅ Define error codes for FFI functions (`SLM_OK`, `SLM_ERR_*`)
- ✅ Document FFI calling conventions and ownership rules (`docs/ffi.md`)

### FFI Functions (C Side) ✅
- ✅ Implement `slm_map_region()` — wrapper around VMM
- ✅ Implement `slm_unmap_region()` — wrapper around VMM
- ✅ Implement `slm_alloc_pages()` — wrapper around PMM
- ✅ Implement `slm_free_pages()` — wrapper around PMM
- ✅ Implement `slm_get_time_ns()` — current time in nanoseconds
- ✅ Implement `slm_task_create()` — create task from Rust
- ✅ Implement `slm_msg_send()` / `slm_msg_recv()` — IPC wrappers

### FFI Functions (Rust Side) ✅
- ✅ Create safe wrappers in `runtime/src/kernel_ffi.rs`
- ✅ Implement `Result` return types for error handling (`KernelResult<T>`)
- ✅ Add `MemFlags` bitflags type for memory flags
- ✅ Add `KernelError` enum for error codes
- ✅ Write unit tests for FFI type sizes and alignments (compile-time + runtime)

### Build Integration ✅
- ✅ Update top-level Makefile to build Rust runtime
- ✅ Link Rust static library (`.a`) with C kernel
- N/A: Handle Rust symbols in linker script — no special handling needed
- ✅ Verify combined binary boots in QEMU

### First Rust Code ✅
- ✅ Write simple Rust function callable from C (`rust_init()`, `rust_hello()`)
- ✅ Call Rust function from `kernel_main()` as proof of concept
- ✅ Print "Hello from Rust!" via FFI to UART
- ✅ Verify Rust panic handler works (`rust_test_panic()` implemented)

See `docs/ffi.md` for comprehensive FFI documentation.

---

## Milestone 5: Deferred Cleanup from Phase 1 ✅

**Status:** Complete

### Linker Script Security ✅
- ✅ Fix RWX segment warning in kernel ELF
- ✅ Separate .text (RX) from .data/.bss (RW) in kernel.ld
- ✅ Use proper PHDRS with permissions (FLAGS(5) for RX, FLAGS(6) for RW)
- ✅ Ready for user/kernel separation in Phase 3

### Task Stack Reclamation ✅
- ✅ Implement `task_destroy()` — full cleanup including stack
- ✅ Add stack to free list on task termination (via `pmm_free_pages()`)
- ✅ Prevent use-after-free with zombie cleanup mechanism
- ✅ Test rapid task create/destroy cycles (8 lifecycle tasks, memory reclaimed)

### Scheduler Bug Fixes ✅
- ✅ Fixed `ready_count` underflow bug (tasks re-added to queue without increment)
- ✅ Improved test stability with yield() in wait loops

### General Cleanup ✅
- ✅ Updated testing.md with lifecycle test documentation
- ✅ Improved debug output in task_destroy()
- ✅ Reviewed all TODO comments in kernel:
  - ✅ Fixed `slm_get_time_ns()` to use proper timer read
  - ✅ Updated vmm.c comment (linker fixed, MMU granularity is Phase 3)
  - Remaining 3 TODOs are legitimate Phase 3 work (device tree, IPC timeout, user mappings)

---

## Phase 2 Completion Checklist

### Deliverables
- ✅ Kernel runs with MMU enabled (virtual addresses)
- ✅ All CPU cores boot and run tasks
- ✅ Tasks can communicate via message queues
- ✅ Rust code compiles and links with C kernel
- ✅ At least one Rust function callable from C kernel
- ✅ All code compiles cleanly with `-Wall -Werror` (C)
- ✅ Documentation updated in `docs/`

### Demo
- ✅ Boot kernel in QEMU with MMU enabled
- ✅ Show tasks running on different cores
- ✅ Show inter-task communication via IPC
- ✅ Show Rust code executing (print from Rust)
- ✅ Show memory statistics including virtual memory

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

### Milestone 4 — Rust Integration ✅ (Resolved)

| Decision | Options | Choice |
|----------|---------|--------|
| **Allocator** | None vs `linked_list_allocator` vs custom | **`linked_list_allocator`** — set up from the start; avoids refactoring later |
| **Panic Strategy** | Abort vs custom handler calling C panic | **Custom handler** — calls C `panic()` for consistent diagnostics via UART |
| **FFI Error Handling** | Integer codes vs tagged union | **Integer codes** — matches existing C patterns; Rust wraps in `Result` |

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

### Phase 2 Risks (Resolved) ✅

1. **MMU Enable Sequence** — ✅ Resolved
    - Risk: Many ways to triple-fault or hang silently
    - What worked: Identity mapping with shared L1 table for TTBR0/TTBR1, extensive debug output, 2MB block granularity for simplicity
    - Result: MMU enables cleanly, kernel runs at high addresses

2. **Multi-Core Synchronization** — ✅ Resolved
    - Risk: Race conditions are hard to reproduce and debug
    - What worked: Global scheduler lock (simple but sufficient for 4 cores), ARM exclusive load/store for atomics, comprehensive stress tests (6 tasks across 3 CPUs, lock contention test with 150 increments)
    - Result: No race conditions detected, all tests pass consistently

3. **Rust/C Linking** — ✅ Resolved
    - Risk: Symbol visibility, name mangling, ABI mismatches
    - What worked: `extern "C"` and `#[no_mangle]` on all FFI functions, `#[repr(C)]` on shared structs, compile-time size assertions, runtime FFI validation
    - Result: Rust runtime links and runs correctly, FFI tests pass

4. **Rust Borrow Checker vs Kernel Patterns** — ✅ Resolved (for Phase 2 scope)
    - Risk: Kernel data structures often have complex ownership
    - What worked: Keep Rust code simple for Phase 2, use `unsafe` only at FFI boundary, wrap unsafe in safe abstractions
    - Result: No borrow checker issues; pattern scales for Phase 3

### Phase 3 Risks (Upcoming)

1. **User/Kernel Separation**
    - Risk: Privilege escalation bugs, syscall interface design errors
    - Mitigation: Start with minimal syscall set, validate all user pointers
    - Mitigation: Use TTBR0 for user space, TTBR1 for kernel (already prepared)
    - Fallback: Run SLM runtime in kernel mode if user space proves too complex

2. **GPU/NPU Integration (Jetson)**
    - Risk: Undocumented hardware, proprietary drivers, memory coherency issues
    - Mitigation: Start with NVIDIA's documented MMIO interfaces
    - Mitigation: Use shared buffers with proper cache attributes (already have SHM_GPU_ACCESSIBLE)
    - Fallback: CPU-only inference if GPU integration stalls

3. **Real Hardware Differences**
    - Risk: QEMU behavior differs from Jetson Orin (interrupts, timers, cache)
    - Mitigation: Test on real hardware early in Phase 3
    - Mitigation: Abstract platform differences in `platform.h`
    - Fallback: Maintain QEMU as primary development target

4. **Deadline-Aware Scheduling**
    - Risk: Priority inversion, missed deadlines, starvation
    - Mitigation: Start with simple priority levels before full EDF
    - Mitigation: Implement priority inheritance for locks
    - Fallback: Use round-robin with CPU affinity (current approach works)

5. **Model Memory Pressure**
    - Risk: Large models (7B+ parameters) exceed available RAM
    - Mitigation: Implement memory-mapped model loading (stream from storage)
    - Mitigation: Use weight quantization (INT8/INT4) to reduce footprint
    - Fallback: Target smaller models (1-3B parameters)

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
