# Phase 4X: x86-64 Port with RTX 3050 GPU

This document tracks the x86-64 port of SLM-OS for desktop PC with NVIDIA RTX 3050.

**Status:** M1-M6 Complete (M6 GPU: registers + VRAM verified, GSP documented as future work)

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

## Completed Work Summary

### M1 Boot Foundation — ✅ Complete (April 2026)

**What's Working:**
- x86-64 kernel boots in QEMU and on real i7-6700 hardware
- Full 32-bit to 64-bit long mode transition
- 4-level page tables with 2MB pages (up to 20GB identity mapped)
- 64-bit GDT with code/data segments
- Serial console via COM1 at 115200 baud
- Full SLM-OS kernel boots via shared `kernel_main()` path
- CMake build system integrated (`cmake -DPLATFORM=X86_64`)
- UEFI-bootable disk images via `make -f Makefile.test disk`

**Key Files:**
| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/trampoline32.S` | 32-bit Multiboot2 entry, mode transition |
| `kernel/arch/x86_64/entry64.S` | 64-bit entry, BSS clear, kernel call |
| `kernel/arch/x86_64/platform_x86.c` | Platform stubs, VMM init, SMP init, boot bridge |
| `kernel/arch/x86_64/main_x86.c` | Standalone test kernel (Makefile.test only) |
| `kernel/arch/x86_64/Makefile.test` | Standalone build + UEFI disk image |
| `kernel/kernel-x86_64.ld` | Linker script for x86-64 |
| `kernel/drivers/uart_x86.c` | COM1 16550 UART driver |
| `kernel/tests/test_x86_boot.c` | 84 functional tests |
| `docs/x86-64-port.md` | Comprehensive documentation |

**Technical Challenges Solved:**
1. **32/64-bit assembly split** — GAS generates 64-bit instructions even with `.code32` when targeting elf64; solved by compiling trampoline with `-m32` and converting via `objcopy`
2. **GDT pointer relocation** — Made `gdt64_ptr` global for correct symbol relocation instead of section-relative
3. **Page table preservation** — Moved page tables to separate `.page_tables` section to prevent BSS zeroing from corrupting active paging structures
4. **UEFI boot** — GRUB EFI doesn't pass Multiboot2 magic or set video mode; skip magic check, use serial-only output
5. **Extended paging** — `vmm_init()` detects RAM from Multiboot2 memory map, extends PDPT/PD beyond 4GB

**Build Commands:**
```bash
# CMake integrated build (full SLM-OS kernel)
cmake -B build -DPLATFORM=X86_64 && cmake --build build

# Standalone test build
make -f kernel/arch/x86_64/Makefile.test

# UEFI disk image for real hardware
make -f kernel/arch/x86_64/Makefile.test disk
```

### M2 Memory Management — ✅ Complete (April 2026)

- Multiboot2 memory map parsed to detect RAM end
- PMM buddy allocator running (uses `x86_detected_ram_end` instead of hardcoded `RAM_SIZE`)
- Page tables extended to map up to 20GB (trampoline maps 0-4GB, `vmm_init()` extends)
- Verified: QEMU 4GB → 5113 MB, i7-6700 16GB → 19914 MB
- Identity mapping (virtual == physical), no higher-half yet

**Remaining M2 items (deferred):**
- Higher-half kernel mapping
- Full `vmm_map_page()` / `vmm_unmap_page()` API for x86-64 PTEs
- Model memory flags in PTE available bits

### M3 Interrupts & Timer — ✅ Complete (April 2026)

- **IDT**: 64 entries (exceptions 0-31, IOAPIC IRQs 32-47, LAPIC vectors 48-63)
- **LAPIC**: Initialized from ACPI MADT address, SVR enabled, TPR=0, flat destination mode
- **IOAPIC**: All redirection entries configured, Interrupt Source Overrides applied
- **8259 PIC**: Disabled (remapped to 0xF0-0xFF, all masked)
- **LAPIC timer**: Periodic mode, calibrated against PIT channel 2, 100Hz tick
- **Context switch**: `context.S` saves/restores RBX, RBP, R12-R15, RSP, RIP, RFLAGS
- **Exception handling**: Full register dump on fault, CR2 for page faults
- Preemptive scheduling verified working on QEMU and i7-6700 hardware

**Key files:**
| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/idt.c` + `idt.S` | IDT setup, ISR stubs, exception/IRQ dispatch |
| `kernel/arch/x86_64/lapic.c` | Local APIC driver, timer calibration |
| `kernel/arch/x86_64/ioapic.c` | I/O APIC driver, IRQ routing |
| `kernel/arch/x86_64/pic.c` | gic.h → LAPIC/IOAPIC bridge, 8259 disable |
| `kernel/arch/x86_64/timer_x86.c` | LAPIC timer implementing timer.h |
| `kernel/arch/x86_64/context.S` | x86-64 context switch |

### Additional Completed Work

- **Lua runtime** (`setjmp.S`, ctype/errno stubs, SSE for Lua math)
- **ACPI MADT parsing** (`acpi.c`) — discovers CPUs, LAPIC base, IOAPIC, ISOs
- **Platform abstraction** (`pic.c` bridges gic.h, `platform_x86.c` provides all stubs)
- **Real x86-64 spinlocks** — TTAS with `__atomic_exchange_n`, atomic `cpus_online` increment
- **84 functional tests** across 16 categories

---

## Icon Key

| Icon | Meaning |
|------|---------|
| ☐ | Not started |
| ✅ | Complete |
| ⏸️ | Deferred to later phase |
| 🔗 | Has dependency on another milestone |

---

## Milestone 1: x86-64 Boot Foundation — ✅ Complete

### Development Environment
- ✅ Set up cross-compilation for x86-64 bare-metal (native GCC with -m32/-m64)
- ✅ Install Rust target `x86_64-unknown-none`
- ✅ Create `kernel/arch/x86_64/` directory structure
- ✅ Update CMakeLists.txt for x86-64 target (`cmake -DPLATFORM=X86_64`)
- ☐ Update Cargo.toml for x86-64 target (Rust runtime not yet ported)
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
- ✅ UEFI-bootable disk image via `make disk` (grub-mkimage + GPT + ESP)
- ✅ Verified boot on Gigabyte H610M S2H V2 / i7-6700 via SDWire + labctl
- ✅ Document boot configuration in `docs/x86-64-port.md`

### Testing & Documentation
- ✅ Create functional tests (`test_x86_boot.c` — 84 tests across 16 categories)
  - ✅ Control register tests (CR0, CR4, EFER, CR3)
  - ✅ Page table structure tests (PML4, PDPT, PD entries)
  - ✅ GDT tests (limit, CS/DS selectors)
  - ✅ Memory layout tests (kernel address, section ordering)
  - ✅ 64-bit mode verification tests
  - ✅ IDT structure and exception handler tests
  - ✅ Multiboot2 parsing tests
  - ✅ Platform abstraction tests (cpu_context, spinlock, gic, timer)
  - ✅ Scheduler integration tests
  - ✅ ACPI + LAPIC + IOAPIC tests
  - ✅ setjmp/longjmp tests (Lua support)
  - ✅ VMM RAM detection test
- ✅ Create comprehensive documentation (`docs/x86-64-port.md` — 657 lines)
- ✅ Update test harness header for x86 boot tests

---

## Milestone 2: x86-64 Memory Management — ✅ Core Complete

### Physical Memory
- ✅ Parse Multiboot2 memory map (type 6 tag) to detect RAM end
- ✅ PMM buddy allocator works for x86-64 (uses `x86_detected_ram_end`)
- ✅ Support > 4GB RAM — i7-6700 reports 19914 MB usable
- ✅ `vmm_init()` extends page tables from boot's 4GB to detected RAM (up to 20GB)

### Virtual Memory (x86-64 Paging)
- ✅ 4-level page tables: PML4 → PDPT → PD with 2MB pages
- ✅ Boot maps 0-4GB (trampoline32.S), `vmm_init()` extends PDPT[4..N]
- ✅ Identity mapping (virtual == physical) for all RAM
- ✅ TLB flush via CR3 reload after extending
- ⏸️ `vmm_map_page()` / `vmm_unmap_page()` API — deferred (identity mapping sufficient)
- ⏸️ Higher-half kernel mapping — deferred to post-SMP
- ⏸️ x86-64 PTE bitfield definitions — deferred to when needed

### Model Memory Regions
- ⏸️ Port model memory flags to x86-64 PTE available bits — deferred to M6
- ⏸️ Implement `gpu_mapped`, `model_page`, `inference_hot` flags — deferred to M6
- ⏸️ Allocate GPU-accessible memory — deferred to M5/M6

---

## Milestone 3: x86-64 Interrupts & Timer — ✅ Complete

### Interrupt Controller (APIC)
- ✅ Local APIC: SVR enable, TPR=0, flat destination mode, LVT masking (`lapic.c`)
- ✅ I/O APIC: redirection table, Interrupt Source Override handling (`ioapic.c`)
- ✅ IDT: 64 entries — exceptions 0-31, IOAPIC 32-47, LAPIC 48-63 (`idt.c` + `idt.S`)
- ✅ ISR stubs with register save/restore, error code handling
- ✅ C interrupt dispatch via `exception_handler()` + `irq_handlers[]` callback table
- ✅ 8259 PIC disabled (remapped to 0xF0-0xFF, all masked)
- ✅ gic.h interface bridges to LAPIC+IOAPIC via `pic.c`

### Exception Handling
- ✅ All exceptions 0-31 handled with named descriptions
- ✅ Full register dump on fault (RAX-R15, RIP, RSP, RFLAGS, CS, SS)
- ✅ CR2 decode for page faults
- ✅ Halt after fatal exception

### Timer
- ✅ LAPIC timer in periodic mode (vector 48), calibrated against PIT channel 2
- ✅ 100 Hz tick matching ARM64 configuration
- ✅ `timer_percpu_init()` ready for SMP (starts LAPIC timer on secondary CPUs)
- ✅ `sleep_ms()` / `sleep_us()` via busy-wait on tick counter

### Context Switch
- ✅ `context.S`: saves/restores RBX, RBP, R12-R15, RSP, RIP, RFLAGS
- ✅ `task_entry_wrapper`: enables interrupts (STI), calls entry, calls `task_exit()`
- ✅ TASK_CONTEXT_OFFSET = 0x20 matches task.h
- ⏸️ SSE/AVX state save (FXSAVE/XSAVE) — deferred (kernel is -mno-sse)

---

## Milestone 4: x86-64 Multi-Core (SMP) — ✅ Complete

### SMP Discovery
- ✅ ACPI MADT parsing discovers CPUs, LAPIC base, IOAPIC (`acpi.c`)
- ✅ `smp_init()` populates `cpu_data[]` and `cpu_logical_map[]` from ACPI
- ✅ i7-6700: MADT reports 16 CPUs (8 enabled), APIC IDs stored
- ✅ QEMU: 4 CPUs detected, all brought online
- ✅ `cpu_count` set from ACPI, `CPU_MAX` = 8

### SMP Boot
- ✅ AP trampoline (`ap_trampoline.S`): 16-bit → 32-bit → 64-bit transition
- ✅ Trampoline copied to 0x8000 (below 1MB), SIPI vector = 0x08
- ✅ INIT IPI + SIPI IPI sequence via LAPIC ICR
- ✅ AP loads BSP's page tables (CR3), GDT, IDT from boot params
- ✅ Per-CPU stacks allocated from PMM (16 KB each)
- ✅ `lapic_percpu_init()` + `timer_percpu_init()` called on each AP
- ✅ `scheduler_init_secondary()` + `scheduler_start()` on each AP
- ✅ Verified: 4/4 CPUs online in QEMU, shell responsive

**Key files:**
| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/ap_trampoline.S` | Real-mode AP startup, mode transitions |
| `kernel/arch/x86_64/platform_x86.c` | INIT-SIPI-SIPI sequence, `boot_ap()`, `ap_entry_64()` |

### Per-CPU Data
- ✅ `cpu_id()` reads LAPIC ID, looks up in `cpu_logical_map[]`
- ✅ Per-CPU run queues working via shared scheduler code
- ⏸️ GS-base for per-CPU data access — deferred (LAPIC ID lookup sufficient)

### Synchronization
- ✅ Spinlocks work on x86-64 (coherency always enabled, `spinlock_hw_enabled = 1`)
- ✅ x86 TSO memory model is stronger than ARM — fewer barriers needed
- ✅ Multi-core scheduler verified with 4 CPUs in QEMU

### Tests
- ✅ 11 new SMP + spinlock tests (55 → 66 total):
  - SMP boot: `test_smp_cpu_count`, `test_smp_all_cpus_online`, `test_smp_bsp_cpu_id`
  - APIC IDs: `test_smp_unique_apic_ids`, `test_smp_lapic_id_matches_bsp`
  - CPU lookup: `test_smp_cpu_logical_id_found`, `test_smp_cpu_logical_id_not_found`
  - Data integrity: `test_smp_logical_map_consistent`, `test_smp_ap_stacks_allocated`
  - Spinlock: `test_spinlock_mutual_exclusion`
  - Layout: `test_smp_trampoline_param_offsets`

---

## Milestone 5: PCIe Enumeration — ✅ Complete

### PCIe Config Space Access
- ✅ Legacy I/O access via ports 0xCF8/0xCFC (works everywhere)
- ✅ ECAM (memory-mapped) via ACPI MCFG table (used if available)
- ✅ pci_config_read8/16/32 and pci_config_write32 API

### Bus Enumeration
- ✅ Scan all buses/devices/functions, follow PCI-PCI bridges
- ✅ Store vendor/device ID, class, subclass, BARs, IRQ per device
- ✅ Multi-function device support (header type bit 7)
- ✅ Verified: 6 devices on QEMU i440FX

### Device Discovery
- ✅ `pci_find_device(vendor, device)` — find by vendor:device ID
- ✅ `pci_find_class(class, subclass)` — find by class code
- ✅ NVIDIA GPU detection (vendor 0x10DE, class 0x03) with BAR dump
- ☐ Map GPU BARs into virtual address space — deferred to M6

### Resource Allocation
- ✅ Read existing BAR assignments (UEFI-configured)
- ✅ 64-bit BAR support (BAR type detection: MMIO/IO, 32/64-bit)
- ☐ BAR size probing (write all-ones, read back) — deferred to M6

### Shell Command
- ✅ `pci` — lists all devices with BDF, vendor:device, class, description
- ✅ BAR details shown for display devices (class 0x03)

**Key files:**
| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/pci.c` | Config access, enumeration, shell command |

### Tests
- ✅ 11 PCI tests (66 → 77 total):
  - Config access: `test_pci_host_bridge_exists`, `test_pci_nonexistent_device`, `test_pci_config_read8_class`, `test_pci_config_read16_vendor`
  - Enumeration: `test_pci_enumeration_found_devices`, `test_pci_found_host_bridge`, `test_pci_device_at_index_valid`, `test_pci_found_isa_bridge`
  - Lookup: `test_pci_find_device_by_id`, `test_pci_find_device_not_found`
  - Multi-function: `test_pci_multifunction_device`

---

## Milestone 6: NVIDIA GPU Driver (RTX 3050) — ✅ Complete (GSP Deferred)

### GPU Research
- ✅ Study NVIDIA open-gpu-kernel-modules for GA107 register map
- ✅ Document BAR0 register offsets (NV_PMC_BOOT_0, BOOT_42, ENABLE)
- ✅ Understand PCIe BAR layout:
  - ✅ BAR0: GPU registers (16 MB MMIO, non-prefetchable)
  - ✅ BAR1: VRAM aperture (64-bit, prefetchable, up to 8 GB)
  - ✅ BAR2: RAMIN / control structures (64-bit)
- ✅ Architecture decode: Ampere = 0x17, GA107 = implementation 0x07, chip_id = 0x177

### Basic GPU Initialization
- ✅ `nvidia_gpu_init()`:
  - ✅ Scan PCI bus for NVIDIA vendor (0x10DE) + display class (0x03)
  - ✅ Enable memory space + bus mastering via PCI command register
  - ✅ Map BAR0 (MMIO registers) with size probing
  - ✅ Map BAR1 (VRAM aperture) with 64-bit BAR + size probing
  - ✅ Read NV_PMC_BOOT_0 and NV_PMC_BOOT_42 for chip identification
  - ✅ Decode architecture, implementation, chip ID, revision
  - ✅ Read NV_PMC_ENABLE for engine status
- ✅ `gpu` shell command — shows GPU info, BARs, registers
- ✅ Graceful no-GPU path (QEMU: "No NVIDIA GPU found")
- ✅ Verified on real i7-6700 + RTX 3050: GA107 (0x177), Ampere, Rev 10.1, BAR0=0x53000000, BAR1=0x40000000

### VRAM Access
- ✅ `nvidia_gpu_vram_test()` — write/read pattern to BAR1 VRAM
- ✅ `nvidia_gpu_vram_test_extended()` — tests 5 offsets across 128 MB aperture
- ✅ Verified on real hardware: all 5 offsets PASS (0, 1MB, 16MB, 64MB, 128MB)
- ☐ VRAM size detection via resizable BAR or bar size probing

### GSP Firmware Study
- ✅ Study GSP (GPU System Processor) architecture — RISC-V core, mandatory on Ampere
- ✅ Document GSP boot sequence from open-gpu-kernel-modules (7 phases)
- ✅ Identify GSP firmware files (gsp-535.113.01.bin.zst, 38 MB RISC-V ELF + 3 support blobs)
- ✅ Document GSP mailbox/command interface (Falcon MAILBOX0/1, RPC queues)
- ✅ Document findings in `docs/nvidia-gsp.md`
- ✅ Map nouveau source files for future GSP implementation

**Finding:** GSP is mandatory on Ampere — no legacy mode. Engine registers return
0xBADF5040 without GSP-RM. Full GSP boot requires VBIOS parsing, SEC2 Falcon
programming, cryptographic verification, 38 MB firmware loading, and RPC stack.
This is a project-scale effort deferred to post-capstone.

### GPU Memory Management — Deferred (Requires GSP)
- ⏸️ VRAM allocation through GPU page tables — requires GSP-RM
- ⏸️ `gpu_alloc(size)` / `gpu_free(addr)` — requires GSP-RM
- ⏸️ PCIe DMA for system memory ↔ VRAM — raw BAR1 access works; proper DMA requires GSP

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

## Milestone 7: Platform Abstraction Layer — ~70% Complete

### Architecture Abstraction
- ✅ `kernel/arch/arm64/` and `kernel/arch/x86_64/` clean separation
- ✅ `pic.c` bridges `gic.h` interface to LAPIC+IOAPIC
- ✅ `timer_x86.c` implements `timer.h` via LAPIC timer
- ✅ `platform_x86.c` provides all platform stubs (SMP, VMM, DTB, Rust, GPU, components)
- ✅ `uart_x86.c` implements `uart.h` via COM1 16550
- ✅ `context.S` implements `switch_to()` for x86-64 ABI
- ☐ Create `kernel/include/arch.h` with formal arch-agnostic API
- ☐ `arch_irq_enable()` / `arch_irq_disable()` (currently inline in platform.h)

### Build System
- ✅ CMakeLists.txt: `cmake -DPLATFORM=X86_64` selects x86-64 sources/flags/linker script
- ✅ Conditional compilation: ARM64-only files excluded, x86-64 files included
- ✅ Standalone build: `Makefile.test` for standalone test kernel + UEFI disk image
- ✅ Both architectures build from same source tree

### Shared Code
- ✅ Scheduler compiles and runs (preemptive scheduling verified)
- ✅ IPC (message queues, shared buffers) compiles
- ✅ Shell (30+ commands) compiles and runs
- ✅ ELF loader compiles
- ✅ PMM buddy allocator compiles and runs
- ✅ VFS + LittleFS compiles and runs
- ✅ Lua 5.4 interpreter compiles and runs (with x86-64 setjmp.S)
- ☐ Rust runtime (model memory allocator) — needs x86-64 Cargo target
- ☐ Networking (lwIP + VirtIO) — not yet ported

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
M1 (Boot) ✅ ──> M2 (Memory) ✅ ──> M3 (Interrupts) ✅ ──> M4 (SMP) ✅
                       │
                       └──────> M5 (PCIe) ✅ ──────> M6 (GPU) ✅ (GSP deferred)
                       
M7 (Abstraction) ~70% ──> Incrementally built with M1-M6
M8 (Testing) ──────> 84 tests passing
M9 (Docs) ──────> nvidia-gsp.md complete
```

**All critical path milestones complete.** Remaining work: M7 formal headers, M8 hardware test automation, M9 architecture comparison doc.

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
*Last Updated: April 2026 — M1-M6 complete (GPU registers + VRAM verified on RTX 3050, GSP research documented), M7 ~70%*
*Purpose: Parallel development track for x86-64 + RTX 3050 GPU learning*
*Relationship: Supports Phase 4 (Jetson) and Phase 5 (SLM Integration)*
