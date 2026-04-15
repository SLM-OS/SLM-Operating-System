# Phase 1: Minimal Kernel (C) — COMPLETE

This document records the completed Phase 1 implementation of SLM-OS.

**Status:** All milestones complete. Ready for archive.

**What was built:**
- Bootable ARM64 kernel for QEMU virt machine
- UART driver (PL011) with printf-style output
- Physical memory manager (bitmap allocator)
- Preemptive round-robin scheduler with timer interrupts
- GIC and ARM generic timer drivers
- Exception handling with panic infrastructure

**Deferred to Phase 2:**
- Task stack cleanup/memory reclamation
- Virtual memory (MMU)
- Multi-core support
- Rust runtime integration

---

# Phase 1: Minimal Kernel (C)

---

## Milestone 1: Environment Setup & Boot Foundation

### Development Environment
- ✅ Update CLion student license
- ✅ Install CLion and configure for ARM64 cross-compilation
- ✅ Install `aarch64-none-elf-gcc` toolchain (ARM bare-metal)
- ✅ Install Rust toolchain with `aarch64-unknown-none` target
- ✅ Set up QEMU for ARM64 emulation (virt machine)
- ✅ Create GitHub repository with initial structure (see Appendix A of design doc)
- ✅ Set up top-level Makefile to orchestrate builds
- ✅ Configure CMake for kernel build
- ✅ Verify Claude Code integration with CLion

### Boot Process Research
- ✅ Study Jetson Orin Nano boot sequence (UEFI → OS)
- ✅ Review ARM64 exception levels (EL0-EL3)
- ✅ Document which EL the OS will run at initially — **EL1**
- ✅ Identify minimum device tree requirements — **Hardcoded for QEMU** (DTB parsing in Phase 2-3)

### Boot Implementation
- ✅ Write `boot.S` — entry point, stack setup, BSS clear
- ✅ Implement jump to C `kernel_main()`
- ✅ Create linker script (`kernel.ld`) with memory layout
- ✅ Build first bootable image (even if it does nothing)
- ✅ Test boot in QEMU — verify execution reaches `kernel_main()`

---

## Milestone 2: UART Driver & Debug Output

### UART Research
- ✅ Identify UART hardware on Jetson Orin Nano — **Tegra186-UART (NS16550-compatible)**
- ✅ Identify UART hardware on QEMU virt machine — **PL011** (also used by Pi 5)
- ✅ Document register addresses and offsets — see `docs/uart-hardware.md`
- ✅ Decide on abstraction layer for hardware differences — **compile-time `#ifdef`**

### UART Implementation
- ✅ Write `uart.c` — init, putc, getc, puts functions (uart_pl011.c)
- ✅ Implement `printf`-style formatted output (%c, %s, %d, %u, %x, %X, %p, %l, %%)
- ✅ Add UART base address configuration — **compile-time via platform.h**
- ✅ Test "Hello, SLM-OS!" output in QEMU
- ✅ Create debug macros (`DEBUG_PRINT`, `ASSERT`, `INFO`, `WARN`, `ERROR`)

### Early Debug Infrastructure
- ✅ Implement panic handler with UART output (panic.c)
- ✅ Add register dump on panic — SP, ELR, SPSR, ESR (with exception class decode), FAR
- ✅ Create simple spin loop for fatal errors (WFI loop in panic and boot.S)

---

## Milestone 3: Physical Memory Management

### Memory Map Research
- ✅ Document Jetson Orin Nano physical memory layout
- ✅ Document QEMU virt machine memory layout
- ✅ Identify reserved regions (firmware, MMIO, GPU carveout)
- ✅ Decide how memory map will be discovered (hardcoded vs device tree) — **Hardcoded for Phase 1**

### Physical Memory Allocator
- ✅ Define page size (4KB standard, 2MB huge pages for models)
- ✅ Implement bitmap-based physical page allocator
- ✅ Write `pmm_init()` — initialize from memory map
- ✅ Write `pmm_alloc_page()` — allocate single 4KB page
- ✅ Write `pmm_free_page()` — return page to free pool
- ✅ Write `pmm_alloc_pages(count)` — allocate contiguous pages
- ✅ Add statistics tracking (total pages, free pages, allocated)

### PMM Testing
- ✅ Write unit tests for allocator (alloc/free cycles)
- ✅ Test edge cases (out of memory, double free)
- ✅ Verify no memory leaks after alloc/free sequences
- ✅ Print memory statistics via UART

---

## Milestone 4: Simple Scheduler

### Task Structure
- ✅ Define `struct task` in C (simplified version of `slm_task`)
- ✅ Implement task state enum (READY, RUNNING, BLOCKED, TERMINATED)
- ✅ Allocate kernel stack per task (16KB fixed)
- ✅ Create task ID assignment

### Context Switch
- ✅ Write `context.S` — save/restore ARM64 registers (callee-saved + FPU/SIMD)
- ✅ Implement `switch_to(old_task, new_task)`
- ✅ Save FPU/SIMD state eagerly (all V0-V31 registers)
- ✅ Handle stack pointer switch

### Round-Robin Scheduler
- ✅ Implement run queue (simple linked list)
- ✅ Write `scheduler_init()`
- ✅ Write `scheduler_add_task(task)`
- ✅ Write `schedule()` — pick next task, context switch
- ✅ Set up timer interrupt for preemption (ARM generic timer, 100 Hz)

### Interrupt Infrastructure
- ✅ Write GIC driver (`gic.c`) — distributor + CPU interface init
- ✅ Write timer driver (`timer.c`) — ARM generic timer at 100 Hz
- ✅ Write exception vector table (`vectors.S`) — ARM64 aligned to 0x800
- ✅ Implement exception handlers (`exceptions.c`) — IRQ dispatch
- ✅ Configure VBAR_EL1 in `boot.S`

### Scheduler Testing
- ✅ Create 2-3 test tasks that print to UART
- ✅ Verify tasks run in round-robin order
- ✅ Verify preemption works (tasks don't have to yield)
- ✅ Test task termination (cleanup deferred to Phase 2)

---

## Phase 1 Completion Checklist

### Deliverables
- ✅ Bootable kernel image for QEMU
- ✅ UART output working
- ✅ Physical memory allocator functional
- ✅ 2+ tasks running concurrently with preemptive scheduling
- ✅ All code compiles cleanly with `-Wall -Werror`
- ✅ Basic documentation in `docs/`

### Demo
- ✅ Boot kernel in QEMU
- ✅ Show multiple tasks printing interleaved output
- ✅ Show memory allocation statistics
- ✅ Show clean shutdown or intentional panic with register dump

---

## Outstanding Decisions

### Milestone 1 — DECIDED

| Decision | Options | Choice |
|----------|---------|--------|
| **Exception Level** | EL1 (kernel mode) vs EL2 (hypervisor) | **EL1** |
| **Initial Target** | QEMU only vs QEMU + Jetson | **QEMU** (Jetson in Phase 2-3) |
| **Boot Method** | UEFI stub vs U-Boot chainload vs raw binary | **QEMU direct load** (U-Boot later) |
| **Repository Structure** | Monorepo vs separate kernel/runtime repos | **Monorepo** |

### Milestone 2 — DECIDED

| Decision | Options | Choice |
|----------|---------|--------|
| **Hardware Abstraction** | Compile-time `#ifdef` vs runtime function pointers | **Compile-time `#ifdef`** (platform.h, uart_pl011.c) |
| **Debug Output Verbosity** | Minimal vs verbose with timestamps | **Verbose** (DEBUG_PRINT includes file:line; compile-time via DEBUG flag) |

### Milestone 3 — DECIDED

| Decision | Options | Choice |
|----------|---------|--------|
| **Page Allocator Algorithm** | Bitmap vs buddy allocator vs free list | **Bitmap** (buddy allocator deferred to Phase 3) |
| **Memory Map Source** | Hardcoded vs device tree parsing | **Hardcoded** (platform.h; DTB parsing in Phase 2-3) |

### Milestone 4 — DECIDED

| Decision | Options | Choice |
|----------|---------|--------|
| **Timer Frequency** | 100 Hz vs 1000 Hz | **100 Hz** (10ms tick) |
| **FPU/SIMD State** | Lazy save vs eager save | **Eager save** (SLM workloads use SIMD heavily; simpler implementation) |
| **Task Stack Size** | Fixed vs configurable | **Fixed 16KB** (configurable in Phase 3) |

### Deferred to Phase 2

| Decision | Notes |
|----------|-------|
| Rust toolchain integration | Phase 2: "Rust toolchain integration and FFI boundary" |
| Virtual memory specifics | Phase 2: "Virtual memory with 2-level page tables" |
| IPC mechanism details | Phase 2: "Basic IPC (message queues)" |
| Multi-core boot | Phase 2: "Multi-core boot and scheduling" |
| Jetson-specific drivers | Can develop entirely on QEMU for Phase 1 |

---

## Risk Mitigation

### High-Risk Items — Status
1. **ARM64 boot sequence** — ✅ RESOLVED
   - Boot working reliably, exception vectors configured, interrupts functional

2. **Context switch correctness** — ✅ RESOLVED
   - Started with cooperative switching, then added preemption
   - FPU/SIMD alignment issue found and fixed (alignas(16) + assembly offset sync)

3. **QEMU vs real hardware differences** — DEFERRED
   - Jetson testing planned for Phase 2
   - QEMU assumptions documented in platform.h

### Lessons Learned
- Assembly struct offsets must stay synchronized with C struct definitions
- Use `UL` suffix on address constants for 64-bit compatibility
- C11 `alignas()` inserts padding that affects subsequent field offsets

---

## Resources

### Reference Projects
- [philipp-oppermann/blog_os](https://github.com/phil-opp/blog_os) — Rust OS, but boot concepts transfer
- [s-matyukevich/raspberry-pi-os](https://github.com/s-matyukevich/raspberry-pi-os) — ARM64 bare metal tutorials
- [bztsrc/raspi3-tutorial](https://github.com/bztsrc/raspi3-tutorial) — Low-level ARM64 tutorials
- ARM Architecture Reference Manual (ARMv8-A)

### Documentation to Read
- ARM Cortex-A78AE Technical Reference Manual (for Jetson specifics)
- UEFI Specification (if doing UEFI boot)
- PL011 UART Technical Reference Manual

---

*Last updated: December 20, 2025 — Phase 1 complete*
