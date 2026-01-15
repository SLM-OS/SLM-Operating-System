# Phase 4X: x86-64 Port with RTX 3050 GPU

This document tracks the x86-64 port of SLM-OS for desktop PC with NVIDIA RTX 3050.

**Status:** In Progress (M1 - Boot Foundation ~80% Complete)

**Summary:** Primary development track to port SLM-OS to x86-64 architecture with discrete NVIDIA GPU. This enables GPU driver development on accessible hardware with superior debugging tools.

**Strategic Value:**
- RTX 3050 (GA107) uses same Ampere architecture as Jetson Orin Nano GPU
- Desktop Linux provides superior debugging tools (GDB, perf, nvidia-smi reference)
- GSP firmware communication patterns transfer directly to Jetson
- Provides working demo platform independent of Jetson hardware issues
- External SSD boot enables dual-boot without affecting development environment

**Hardware:**
- **CPU:** Intel Core i7-6700 @ 3.40GHz (4 cores, 8 threads, Skylake)
- **GPU:** NVIDIA GeForce RTX 3050 6GB (GA107, Ampere architecture)
- **GPU Driver:** 535.274.02, CUDA 12.2
- **Boot Device:** MTD120G 120GB USB SSD (dedicated to SLM-OS)
- **Architecture:** x86-64 (AMD64)
- **Host OS:** Ubuntu 24.04 (on internal drives - do not modify)

**Relationship to Other Phases:**
- **Primary focus** until NVIDIA Jetson information received
- GPU learnings directly applicable to Phase 4 M3 (Jetson GPU Driver)
- Does NOT block Phase 5 (SLM Integration) — either platform can host it

---

## Completed Work Summary (January 2026)

### M1 Boot Foundation — ~80% Complete

**What's Working:**
- x86-64 kernel boots in QEMU from GRUB ISO
- Full 32-bit to 64-bit long mode transition
- 4-level page tables with 2MB pages (1GB identity mapped)
- 64-bit GDT with code/data segments
- Framebuffer console with 8x16 bitmap font
- C kernel entry and basic output

**Key Files Created:**
| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/trampoline32.S` | 32-bit Multiboot2 entry, mode transition |
| `kernel/arch/x86_64/entry64.S` | 64-bit entry, BSS clear, kernel call |
| `kernel/arch/x86_64/main_x86.c` | Test kernel entry point |
| `kernel/arch/x86_64/Makefile.test` | Build system with ISO creation |
| `kernel/kernel-x86_64.ld` | Linker script for x86-64 |
| `kernel/drivers/fb_console.c` | Framebuffer console driver |
| `kernel/tests/test_x86_boot.c` | 20 functional tests |
| `docs/x86-64-port.md` | Comprehensive documentation |

**Technical Challenges Solved:**
1. **32/64-bit assembly split** — GAS generates 64-bit instructions even with `.code32` when targeting elf64; solved by compiling trampoline with `-m32` and converting via `objcopy`
2. **GDT pointer relocation** — Made `gdt64_ptr` global for correct symbol relocation instead of section-relative
3. **Page table preservation** — Moved page tables to separate `.page_tables` section to prevent BSS zeroing from corrupting active paging structures

**Test Command:**
```bash
make -f kernel/arch/x86_64/Makefile.test iso
qemu-system-x86_64 -m 256M -cdrom build/x86_64-test/slmos-x86.iso -serial stdio
```

**Remaining for M1:**
- External SSD setup for real hardware boot
- Integration with main CMake build system
- Rust target configuration

---

## Icon Key

| Icon | Meaning |
|------|---------|
| ☐ | Not started |
| ✅ | Complete |
| ⏸️ | Deferred to later phase |
| 🔗 | Has dependency on another milestone |

---

## Milestone 1: x86-64 Boot Foundation

### Development Environment
- ✅ Set up cross-compilation for x86-64 bare-metal (native GCC with -m32/-m64)
- ✅ Install Rust target `x86_64-unknown-none`
- ✅ Create `kernel/arch/x86_64/` directory structure
- ☐ Update CMakeLists.txt for x86-64 target (using Makefile.test for now)
- ☐ Update Cargo.toml for x86-64 target
- ✅ Set up QEMU x86-64 for initial testing (before real hardware)

### Multiboot2/GRUB Boot (chosen over raw UEFI)
- ✅ Research boot options — chose Multiboot2/GRUB (simpler than raw UEFI)
- ✅ Implement Multiboot2 header with framebuffer tag
- ✅ Write `trampoline32.S` — 32-bit entry, validates magic, checks CPUID/long mode
- ✅ Implement 32-bit to 64-bit mode transition:
  - ✅ Set up 4-level page tables (PML4 → PDPT → PD with 2MB pages)
  - ✅ Enable PAE in CR4
  - ✅ Load PML4 into CR3
  - ✅ Enable long mode in EFER MSR
  - ✅ Enable paging in CR0
  - ✅ Load 64-bit GDT
  - ✅ Far jump to 64-bit code segment
- ✅ Write `entry64.S` — 64-bit entry, segment setup, BSS clear, kernel call
- ✅ Create x86-64 linker script (`kernel-x86_64.ld`)
- ✅ Create GRUB bootable ISO with `grub-mkrescue`

### Early Console
- ✅ Implement UEFI GOP framebuffer console (`fb_console.c`)
- ✅ Implement basic font rendering (8x16 bitmap font)
- ✅ Port `printf` infrastructure to x86-64 (`fb_console_puts`, `uart_putc` compat)
- ✅ Test "Hello from SLM-OS x86-64!" in QEMU — **WORKING**

### External SSD Setup
- ☐ Format external SSD with GPT partition table
- ☐ Create EFI System Partition (ESP) — FAT32, ~512MB
- ☐ Create SLM-OS partition (can be raw or minimal filesystem)
- ☐ Install GRUB bootloader for real hardware
- ✅ Document boot configuration in `docs/x86-64-port.md`

### Testing & Documentation
- ✅ Create functional tests (`test_x86_boot.c` — 20 tests)
  - ✅ Control register tests (CR0, CR4, EFER, CR3)
  - ✅ Page table structure tests (PML4, PDPT, PD entries)
  - ✅ GDT tests (limit, CS/DS selectors)
  - ✅ Memory layout tests (kernel address, section ordering)
  - ✅ 64-bit mode verification tests
- ✅ Create comprehensive documentation (`docs/x86-64-port.md`)
- ✅ Update test harness header for x86 boot tests

---

## Milestone 2: x86-64 Memory Management

### Physical Memory
- ☐ Parse UEFI memory map (different format from ARM64 DTB)
- ☐ Adapt PMM bitmap allocator for x86-64 memory layout
- ☐ Handle memory holes and reserved regions
- ☐ Support > 4GB RAM (x86-64 can address much more than ARM64 embedded)

### Virtual Memory (x86-64 Paging)
- ☐ Study x86-64 4-level page tables (PML4 → PDPT → PD → PT)
- ☐ Implement `vmm_x86.c` with x86-64 page table structures
- ☐ Define page table entry format (different from ARM64):
  ```c
  // x86-64 PTE format (simplified)
  typedef struct {
      uint64_t present    : 1;
      uint64_t writable   : 1;
      uint64_t user       : 1;
      uint64_t pwt        : 1;   // Page write-through
      uint64_t pcd        : 1;   // Page cache disable
      uint64_t accessed   : 1;
      uint64_t dirty      : 1;
      uint64_t pat        : 1;   // Page attribute table
      uint64_t global     : 1;
      uint64_t available  : 3;   // OS use — SLM flags here
      uint64_t pfn        : 40;  // Physical frame number
      uint64_t available2 : 7;   // OS use
      uint64_t nx         : 1;   // No execute
  } x86_pte_t;
  ```
- ☐ Implement `vmm_map_page()`, `vmm_unmap_page()` for x86-64
- ☐ Set up identity mapping + higher-half kernel mapping
- ☐ Enable paging (likely already enabled by UEFI, just switch tables)

### Model Memory Regions
- ☐ Port model memory flags to x86-64 PTE available bits
- ☐ Implement `gpu_mapped`, `model_page`, `inference_hot` flags
- ☐ Allocate GPU-accessible memory (requires PCIe BAR understanding)

---

## Milestone 3: x86-64 Interrupts & Timer

### Interrupt Controller (APIC)
- ☐ Study Local APIC and I/O APIC architecture (replaces ARM GIC)
- ☐ Implement Local APIC initialization
- ☐ Implement I/O APIC initialization
- ☐ Set up Interrupt Descriptor Table (IDT) — 256 entries
- ☐ Write interrupt stubs in `vectors_x86.S`
- ☐ Implement interrupt dispatch in C

### Exception Handling
- ☐ Handle x86-64 exceptions (different numbers from ARM64):
  - ☐ #DE (0) — Divide error
  - ☐ #DB (1) — Debug
  - ☐ #NMI (2) — Non-maskable interrupt
  - ☐ #BP (3) — Breakpoint
  - ☐ #OF (4) — Overflow
  - ☐ #UD (6) — Invalid opcode
  - ☐ #NM (7) — Device not available (FPU)
  - ☐ #DF (8) — Double fault
  - ☐ #GP (13) — General protection fault
  - ☐ #PF (14) — Page fault
- ☐ Implement panic handler with x86-64 register dump

### Timer
- ☐ Study x86 timer options: APIC timer, HPET, PIT
- ☐ Implement APIC timer for preemptive scheduling (preferred)
- ☐ Calibrate timer against known reference (PIT or TSC)
- ☐ Set up 100 Hz tick (matching ARM64 configuration)

### Context Switch
- ☐ Write `context_x86.S` — save/restore x86-64 registers
- ☐ Callee-saved: RBX, RBP, R12-R15, RSP
- ☐ Save/restore SSE/AVX state (FXSAVE/FXRSTOR or XSAVE/XRSTOR)
- ☐ Handle stack switch

---

## Milestone 4: x86-64 Multi-Core (SMP)

### SMP Boot
- ☐ Study x86-64 SMP boot (INIT-SIPI-SIPI sequence)
- ☐ Parse ACPI MADT table for CPU topology
- ☐ Implement AP (Application Processor) boot:
  - ☐ Allocate trampoline code below 1MB
  - ☐ Send INIT IPI to target CPU
  - ☐ Send SIPI IPI with trampoline address
  - ☐ AP wakes in real mode, transitions to long mode
- ☐ Set up per-CPU stacks
- ☐ Initialize per-CPU Local APIC

### Per-CPU Data
- ☐ Implement GS-base for per-CPU data access (x86-64 idiom)
- ☐ Port per-CPU run queues to x86-64
- ☐ Implement CPU ID detection (`cpuid` instruction)

### Synchronization
- ☐ Port spinlocks to x86-64 (`lock` prefix, `pause` instruction)
- ☐ Port ticket locks
- ☐ Verify memory ordering (x86 has stronger ordering than ARM)

---

## Milestone 5: PCIe Enumeration

### PCIe Basics
- ☐ Study PCIe configuration space access (Type 0/1 headers)
- ☐ Implement PCIe config read/write via ECAM (memory-mapped) or legacy I/O
- ☐ Find ECAM base address from ACPI MCFG table
- ☐ Enumerate PCIe bus, find all devices

### Device Discovery
- ☐ Scan for NVIDIA GPU (Vendor ID 0x10DE)
- ☐ Identify RTX 3050 (Device ID varies by SKU, likely 0x2507 for GA107)
- ☐ Read BAR (Base Address Registers) for GPU MMIO and VRAM
- ☐ Map GPU BARs into virtual address space

### Resource Allocation
- ☐ Parse existing BAR assignments (UEFI likely configured them)
- ☐ Alternatively: Implement simple BAR allocation if needed
- ☐ Document GPU memory layout

### Shell Command
- ☐ `pci` — list PCIe devices (vendor, device, class, BARs)

---

## Milestone 6: NVIDIA GPU Driver (RTX 3050)

### GPU Research
- ☐ Study NVIDIA open-gpu-kernel-modules for GA107
- ☐ Document register map differences from Jetson (if any)
- ☐ Understand PCIe BAR layout:
  - ☐ BAR0: GPU registers (MMIO)
  - ☐ BAR1: GPU memory aperture (VRAM access)
  - ☐ BAR2/3: Other resources

### GSP Firmware Study
- ☐ Study GSP (GPU System Processor) architecture
- ☐ Document GSP boot sequence from open-gpu-kernel-modules
- ☐ Identify GSP firmware files needed (from NVIDIA driver package)
- ☐ Understand GSP mailbox/command interface
- ☐ Document findings in `docs/nvidia-gsp.md`

### Basic GPU Initialization
- ☐ Implement `rtx3050_gpu_init()`:
  - ☐ Enable GPU via PCIe command register
  - ☐ Map BAR0 (registers) and BAR1 (VRAM)
  - ☐ Read GPU identification registers
  - ☐ Check GPU state (powered on, not in reset)
- ☐ Implement `rtx3050_gpu_info()` — print GPU details

### GPU Memory Management
- ☐ Implement VRAM allocation (simple bump allocator initially)
- ☐ Implement `gpu_alloc(size)` — allocate VRAM
- ☐ Implement `gpu_free(addr)` — free VRAM
- ☐ Map VRAM into CPU address space for data transfer
- ☐ Handle PCIe DMA for system memory ↔ VRAM transfers

### GSP Communication (Advanced)
- ☐ Load GSP firmware into GPU memory
- ☐ Implement GSP mailbox interface
- ☐ Send basic commands to GSP
- ☐ Verify GSP responds
- ⏸️ Full GSP initialization — complex, may defer to Phase 5

### Portable GPU HAL
- ☐ Abstract GPU driver interface to work with both RTX 3050 and Jetson
- ☐ Create `struct gpu_driver` implementation for RTX 3050
- ☐ Ensure model memory API works identically on both platforms
- ☐ Document architecture-specific GPU code in `docs/gpu-porting.md`

---

## Milestone 7: Platform Abstraction Layer

### Architecture Abstraction
- ☐ Create `kernel/arch/` with clean separation:
  ```
  kernel/arch/
  ├── arm64/          # Existing ARM64 code
  │   ├── boot.S
  │   ├── mmu.c
  │   ├── context.S
  │   ├── gic.c
  │   └── timer.c
  └── x86_64/         # New x86-64 code
      ├── boot.S
      ├── mmu.c
      ├── context.S
      ├── apic.c
      └── timer.c
  ```
- ☐ Create architecture-independent headers in `kernel/include/arch.h`
- ☐ Implement arch-specific functions with common API:
  - ☐ `arch_init()` — platform initialization
  - ☐ `arch_irq_enable()` / `arch_irq_disable()`
  - ☐ `arch_get_cpu_id()`
  - ☐ `arch_context_switch()`
  - ☐ `arch_timer_init()` / `arch_timer_get_ns()`

### Build System
- ☐ Update CMakeLists.txt to select architecture
- ☐ Create `ARCH=arm64` and `ARCH=x86_64` build options
- ☐ Ensure both architectures build from same source tree
- ☐ Update Makefile for architecture selection

### Shared Code
- ☐ Verify all shared code compiles for both architectures:
  - ☐ Scheduler (policy)
  - ☐ IPC (message queues, shared buffers)
  - ☐ Shell
  - ☐ ELF loader
  - ☐ Model memory allocator (Rust)
- ☐ Fix any architecture assumptions in "portable" code

---

## Milestone 8: Testing & Validation

### QEMU x86-64 Testing
- ☐ Boot SLM-OS in QEMU x86-64 (`qemu-system-x86_64`)
- ☐ All Phase 1-3 tests pass on x86-64 QEMU
- ☐ Test multi-core on QEMU x86-64 (4+ cores)
- ☐ Test interrupt handling
- ☐ Test context switching

### Real Hardware Boot
- ☐ Boot from external SSD on real PC
- ☐ Serial or framebuffer console working
- ☐ All cores detected and booted
- ☐ Timer interrupts firing correctly

### GPU Testing
- ☐ PCIe enumeration finds RTX 3050
- ☐ GPU registers accessible via BAR0
- ☐ VRAM accessible via BAR1
- ☐ Basic GPU memory allocation works
- ☐ Data transfer CPU ↔ VRAM works

### Performance Comparison
- ☐ Context switch time on x86-64 vs ARM64
- ☐ IPC latency comparison
- ☐ Document performance differences

---

## Milestone 9: Documentation & Knowledge Transfer

### GPU Driver Documentation
- ☐ Document GSP firmware interface
- ☐ Document GPU memory management
- ☐ Document PCIe BAR usage
- ☐ Create `docs/nvidia-gsp.md` with learnings
- ☐ Create `docs/gpu-porting.md` for Jetson application

### Architecture Documentation
- ☐ Document x86-64 vs ARM64 differences
- ☐ Document porting decisions and trade-offs
- ☐ Update architecture doc with multi-arch support

### Transfer to Jetson
- ☐ Document which GPU code transfers directly to Jetson
- ☐ Document Jetson-specific adaptations needed
- ☐ Create checklist for Jetson GPU bring-up using x86 learnings

---

## Phase 4X Completion Checklist

### Deliverables
- ☐ SLM-OS boots on x86-64 PC from external SSD
- ☐ Multi-core scheduling working on x86-64
- ☐ All existing tests pass on x86-64
- ☐ RTX 3050 detected and basic memory allocation works
- ☐ GSP firmware interface documented
- ☐ Portable GPU HAL works on both x86-64 and ARM64 (stub)
- ☐ Architecture abstraction enables single source tree

### Demo
- ☐ Boot SLM-OS on PC, show shell prompt
- ☐ Show PCIe enumeration finding RTX 3050
- ☐ Show GPU memory allocation
- ☐ Show same commands working on QEMU ARM64 and x86-64

### Knowledge Deliverables
- ☐ GSP firmware documentation sufficient to apply to Jetson
- ☐ GPU driver patterns documented for Jetson port
- ☐ Performance baseline established

---

## Outstanding Decisions

### Milestone 1 — Boot Method ✅ DECIDED

| Decision | Options | **Chosen** |
|----------|---------|------------|
| **Boot method** | Raw UEFI vs Multiboot2/GRUB | ✅ **Multiboot2/GRUB** — simpler, well-documented, WORKING |
| **Console** | Serial (COM1) vs Framebuffer | ✅ **UEFI GOP framebuffer** — no physical serial port on target PC |
| **SSD filesystem** | Raw partitions vs FAT32/ext4 | **FAT32 ESP + raw partition** — pending real hardware setup |
| **32→64 transition** | Single file vs split files | ✅ **Split files** — trampoline32.S (-m32) + entry64.S (-m64) |

### Milestone 5 — PCIe

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Config access** | Legacy I/O (0xCF8/0xCFC) vs ECAM | **ECAM** — modern, memory-mapped, supports extended config space |
| **BAR handling** | Use UEFI assignments vs reallocate | **Use UEFI assignments** — simpler, UEFI already configured everything |

### Milestone 6 — GPU

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **GSP scope** | Full init vs memory only | **Memory + basic GSP communication** — full compute is Phase 5 |
| **VRAM allocator** | Bump vs bitmap vs buddy | **Bump allocator** initially — simple, sufficient for testing |

---

## Risk Mitigation

### Phase 4X Risks

1. **x86-64 Port Complexity**
   - Risk: x86-64 has many quirks (A20 gate, legacy modes, etc.)
   - Mitigation: Use UEFI — it handles most legacy complexity
   - Mitigation: Start with QEMU x86-64 before real hardware
   - Mitigation: Multiboot2/GRUB simplifies boot
   - Fallback: Focus on ARM64 if x86-64 proves too complex

2. **PCIe Complexity**
   - Risk: PCIe enumeration and BAR handling is complex
   - Mitigation: UEFI already configures PCIe — just read existing config
   - Mitigation: Start with simple device enumeration
   - Fallback: Hardcode RTX 3050 addresses if enumeration is problematic

3. **NVIDIA GPU Documentation**
   - Risk: GPU internals are complex and partially documented
   - Mitigation: Focus on memory management, not compute
   - Mitigation: open-gpu-kernel-modules provides reference
   - Mitigation: Linux nouveau driver as additional reference
   - Fallback: CPU-only operation if GPU init fails

4. **GSP Firmware Complexity**
   - Risk: GSP is a complex RISC-V core with its own firmware
   - Mitigation: Study open-gpu-kernel-modules extensively
   - Mitigation: Start with basic communication, not full init
   - Mitigation: Document everything for Jetson application
   - Fallback: Memory allocation without GSP (limited functionality)

5. **Time Investment**
   - Risk: x86-64 port is significant work, may delay Jetson
   - Mitigation: Run in parallel with Phase 4 Jetson work
   - Mitigation: GPU learnings directly transfer to Jetson
   - Mitigation: Stop at memory management if time constrained
   - Fallback: x86-64 can be post-capstone work

### Dependencies

```
M1 (Boot) ──────> M2 (Memory) ──────> M3 (Interrupts) ──────> M4 (SMP)
                       │
                       └──────> M5 (PCIe) ──────> M6 (GPU)
                       
M7 (Abstraction) ──────> Depends on M1-M4 being functional

M8 (Testing) ──────> Depends on M1-M6

M9 (Docs) ──────> Ongoing throughout
```

**Critical Path:** M1 → M2 → M3 → M5 → M6 (GPU access)

**Parallel Work:**
- M4 (SMP) can parallel with M5-M6
- M7 (Abstraction) can start after M1-M4 basics work
- M9 (Docs) ongoing

---

## Resources

### x86-64 Architecture
- Intel 64 and IA-32 Architectures Software Developer's Manual (SDM)
- AMD64 Architecture Programmer's Manual
- [OSDev Wiki — x86-64](https://wiki.osdev.org/X86-64)
- [OSDev Wiki — Paging](https://wiki.osdev.org/Paging)

### UEFI Boot
- UEFI Specification (uefi.org)
- [OSDev Wiki — UEFI](https://wiki.osdev.org/UEFI)
- [Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/)

### PCIe
- PCI Express Base Specification
- [OSDev Wiki — PCI](https://wiki.osdev.org/PCI)
- [OSDev Wiki — PCI Express](https://wiki.osdev.org/PCI_Express)

### NVIDIA GPU
- [NVIDIA Open GPU Kernel Modules](https://github.com/NVIDIA/open-gpu-kernel-modules)
- Linux kernel `drivers/gpu/drm/nouveau/` — open-source NVIDIA driver
- [Envytools](https://github.com/envytools/envytools) — NVIDIA GPU documentation project
- GA107 (RTX 3050) specifications

### Reference OS Projects
- [blog_os](https://os.phil-opp.com/) — Rust OS for x86-64, excellent tutorials
- [ToaruOS](https://github.com/klange/toaruos) — x86-64 OS with GPU support
- [Managarm](https://github.com/managarm/managarm) — Modern x86-64 OS

---

## Lessons Applicable from ARM64 Work

### What Transfers Directly
- Scheduler design and implementation (policy is arch-independent)
- IPC (message queues, shared buffers)
- Model memory allocator (Rust — fully portable)
- Shell and commands
- ELF loader (ELF64 format is similar)
- Rust runtime and FFI patterns
- Testing patterns and stress tests

### What Needs Rewrite
- Boot code (`boot.S`)
- MMU/paging (`mmu.c`)
- Context switch (`context.S`)
- Interrupt handling (APIC vs GIC)
- Timer driver (APIC timer vs ARM generic timer)
- Multi-core boot (INIT-SIPI-SIPI vs PSCI)

### What's New
- PCIe enumeration (ARM64 Jetson has memory-mapped GPU)
- ACPI parsing (ARM64 uses DTB)
- x86-64 specific quirks

---

*Created: January 2026*
*Last Updated: January 2026 — M1 boot foundation ~80% complete*
*Purpose: Parallel development track for x86-64 + RTX 3050 GPU learning*
*Relationship: Supports Phase 4 (Jetson) and Phase 5 (SLM Integration)*
