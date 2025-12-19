# Current To Do and Implementation Plan

This file contains the current list of To Dos for the current month's development cycle of this project, or, 
maybe, if I'm lucky and am able to get ahead, next month's.

# SLM-OS Month 1 Plan
## Minimal Kernel (C)
### January 2026

---

## Week 1: Environment Setup & Boot Foundation

### Development Environment
- [ ] Update CLion student license
- [ ] Install CLion and configure for ARM64 cross-compilation
- [ ] Install `aarch64-none-elf-gcc` toolchain (ARM bare-metal)
- [ ] Install Rust toolchain with `aarch64-unknown-none` target
- [ ] Set up QEMU for ARM64 emulation (virt machine)
- [ ] Create GitHub repository with initial structure (see Appendix A of design doc)
- [ ] Set up top-level Makefile to orchestrate builds
- [ ] Configure CMake for kernel build
- [ ] Verify Claude Code integration with CLion

### Boot Process Research
- [ ] Study Jetson Orin Nano boot sequence (UEFI → OS)
- [ ] Review ARM64 exception levels (EL0-EL3)
- [ ] Document which EL the OS will run at initially
- [ ] Identify minimum device tree requirements

### Boot Implementation
- [ ] Write `boot.S` — entry point, stack setup, BSS clear
- [ ] Implement jump to C `kernel_main()`
- [ ] Create linker script (`kernel.ld`) with memory layout
- [ ] Build first bootable image (even if it does nothing)
- [ ] Test boot in QEMU — verify execution reaches `kernel_main()`

---

## Week 2: UART Driver & Debug Output

### UART Research
- [ ] Identify UART hardware on Jetson Orin Nano (likely PL011 compatible)
- [ ] Identify UART hardware on QEMU virt machine (PL011)
- [ ] Document register addresses and offsets
- [ ] Decide on abstraction layer for hardware differences

### UART Implementation
- [ ] Write `uart.c` — init, putc, puts functions
- [ ] Implement `printf`-style formatted output (or minimal subset)
- [ ] Add UART base address configuration (compile-time or runtime)
- [ ] Test "Hello, SLM-OS!" output in QEMU
- [ ] Create debug macros (`DEBUG_PRINT`, `ASSERT`, etc.)

### Early Debug Infrastructure
- [ ] Implement panic handler with UART output
- [ ] Add register dump on panic (for debugging)
- [ ] Create simple spin loop for fatal errors

---

## Week 3: Physical Memory Management

### Memory Map Research
- [ ] Document Jetson Orin Nano physical memory layout
- [ ] Document QEMU virt machine memory layout
- [ ] Identify reserved regions (firmware, MMIO, GPU carveout)
- [ ] Decide how memory map will be discovered (hardcoded vs device tree)

### Physical Memory Allocator
- [ ] Define page size (4KB standard, 2MB huge pages for models)
- [ ] Implement bitmap-based physical page allocator
- [ ] Write `pmm_init()` — initialize from memory map
- [ ] Write `pmm_alloc_page()` — allocate single 4KB page
- [ ] Write `pmm_free_page()` — return page to free pool
- [ ] Write `pmm_alloc_pages(count)` — allocate contiguous pages
- [ ] Add statistics tracking (total pages, free pages, allocated)

### PMM Testing
- [ ] Write unit tests for allocator (alloc/free cycles)
- [ ] Test edge cases (out of memory, double free)
- [ ] Verify no memory leaks after alloc/free sequences
- [ ] Print memory statistics via UART

---

## Week 4: Simple Scheduler

### Task Structure
- [ ] Define `struct task` in C (simplified version of `slm_task`)
- [ ] Implement task state enum (READY, RUNNING, BLOCKED, TERMINATED)
- [ ] Allocate kernel stack per task
- [ ] Create task ID assignment

### Context Switch
- [ ] Write `context.S` — save/restore ARM64 registers
- [ ] Implement `switch_to(old_task, new_task)`
- [ ] Decide on callee-saved vs full register save
- [ ] Handle stack pointer switch

### Round-Robin Scheduler
- [ ] Implement run queue (simple linked list)
- [ ] Write `scheduler_init()`
- [ ] Write `scheduler_add_task(task)`
- [ ] Write `schedule()` — pick next task, context switch
- [ ] Set up timer interrupt for preemption (ARM generic timer)

### Scheduler Testing
- [ ] Create 2-3 test tasks that print to UART
- [ ] Verify tasks run in round-robin order
- [ ] Verify preemption works (tasks don't have to yield)
- [ ] Test task termination and cleanup

---

## End of Month 1 Milestone

### Deliverables
- [ ] Bootable kernel image for QEMU
- [ ] UART output working
- [ ] Physical memory allocator functional
- [ ] 2+ tasks running concurrently with preemptive scheduling
- [ ] All code compiles cleanly with `-Wall -Werror`
- [ ] Basic documentation in `docs/`

### Demo
- [ ] Boot kernel in QEMU
- [ ] Show multiple tasks printing interleaved output
- [ ] Show memory allocation statistics
- [ ] Show clean shutdown or intentional panic with register dump

---

## Outstanding Decisions

### Must Decide Before Starting (Week 1)

| Decision | Options | Considerations | Recommendation |
|----------|---------|----------------|----------------|
| **Exception Level** | EL1 (kernel mode) vs EL2 (hypervisor) | EL1 is simpler; EL2 needed if you want to run Linux as guest later | Start with EL1 |
| **Initial Target** | QEMU only vs QEMU + Jetson | Jetson adds hardware complexity; QEMU lets you iterate faster | QEMU first, Jetson in Month 2-3 |
| **Boot Method** | UEFI stub vs U-Boot chainload vs raw binary | Design doc says U-Boot chainload; QEMU can load ELF directly | QEMU direct load now, U-Boot later |
| **Repository Structure** | Monorepo vs separate kernel/runtime repos | Monorepo simplifies build coordination and FFI boundary | Monorepo |

### Must Decide During Week 2

| Decision | Options | Considerations | Recommendation |
|----------|---------|----------------|----------------|
| **Hardware Abstraction** | Compile-time `#ifdef` vs runtime function pointers | Function pointers add indirection; `#ifdef` is messier but faster | `#ifdef` for now, refactor later if needed |
| **Debug Output Verbosity** | Minimal vs verbose with timestamps | Verbose helps debugging; adds code size | Verbose with compile-time disable option |

### Must Decide During Week 3

| Decision | Options | Considerations | Recommendation |
|----------|---------|----------------|----------------|
| **Page Allocator Algorithm** | Bitmap vs buddy allocator vs free list | Bitmap is simplest; buddy is better for contiguous allocs (models) | Bitmap now, buddy allocator in Month 3 for model memory |
| **Memory Map Source** | Hardcoded vs device tree parsing | Device tree is more flexible but requires parser | Hardcoded for QEMU, device tree stretch goal |

### Must Decide During Week 4

| Decision | Options | Considerations | Recommendation |
|----------|---------|----------------|----------------|
| **Timer Frequency** | 100 Hz vs 1000 Hz | Higher frequency = more responsive but more overhead | 100 Hz (10ms tick) for now |
| **FPU/SIMD State** | Lazy save vs eager save | Design doc discussed this; SLM workloads use SIMD heavily | Eager save (simpler, acceptable overhead) |
| **Task Stack Size** | Fixed vs configurable | Fixed is simpler; configurable needed for inference tasks later | Fixed 16KB for Month 1, configurable in Month 3 |

### Can Defer to Month 2

| Decision | Notes |
|----------|-------|
| Rust toolchain integration | Month 2 explicitly includes "Rust toolchain integration and FFI boundary" |
| Virtual memory specifics | Month 2: "Virtual memory with 2-level page tables" |
| IPC mechanism details | Month 2: "Basic IPC (message queues)" |
| Multi-core boot | Month 2: "Multi-core boot and scheduling" |
| Jetson-specific drivers | Can develop entirely on QEMU for Month 1 |

---

## Risk Mitigation

### High-Risk Items
1. **ARM64 boot sequence** — Lots of ways to get this wrong silently
   - Mitigation: Use known-working examples (osdev.org, Philipp Oppermann's blog, existing hobby OS projects)
   
2. **Context switch correctness** — Bugs here are brutal to debug
   - Mitigation: Start with cooperative switching (tasks call `yield()`), add preemption after that works

3. **QEMU vs real hardware differences** — Something works in QEMU but not on Jetson
   - Mitigation: Defer Jetson testing to Month 2; document all QEMU-specific assumptions

### Schedule Buffer
- Week 4 scheduler work has the most unknowns
- If Weeks 1-3 go smoothly, use buffer time for better testing
- If behind, simplify scheduler to cooperative-only (no timer preemption)

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

*This plan assumes January 2026 start. Adjust dates if timeline shifts.*
