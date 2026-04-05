# SLM-OS x86-64 Port

This document describes the x86-64 port of SLM-OS, including architecture details, boot sequence, build instructions, and design decisions.

---

## Table of Contents

1. [Overview](#overview)
2. [Target Hardware](#target-hardware)
3. [Boot Sequence](#boot-sequence)
4. [Memory Layout](#memory-layout)
5. [Page Tables](#page-tables)
6. [GDT and Segments](#gdt-and-segments)
7. [IDT and Exceptions](#idt-and-exceptions)
8. [PIC and Timer](#pic-and-timer)
9. [Console Output](#console-output)
10. [Building](#building)
11. [Hardware Deployment](#hardware-deployment)
12. [Testing](#testing)
13. [Platform Abstraction](#platform-abstraction)
14. [Lua Scripting](#lua-scripting)
15. [Key Files](#key-files)
16. [Design Decisions](#design-decisions)
17. [Troubleshooting](#troubleshooting)

---

## Overview

The x86-64 port enables SLM-OS to run on standard PC hardware with x86-64 processors. This port uses:

- **Boot method**: Multiboot2 via GRUB (UEFI)
- **Console**: COM1 serial (115200 baud, 8N1)
- **Paging**: 4-level page tables with 2MB pages
- **Mode**: 64-bit long mode
- **Interrupts**: 8259 PIC, PIT timer at 100 Hz

### Platform Differences from ARM64

| Feature | ARM64 (Pi 5) | x86-64 |
|---------|--------------|--------|
| Boot method | SD card / UEFI | GRUB Multiboot2 (UEFI) |
| Console | UART serial (PL011) | COM1 serial (16550) |
| Page size | 4KB/64KB | 4KB/2MB |
| Exception model | EL0-EL3 | Ring 0-3, IDT |
| Kernel privilege | EL1 | Ring 0 |
| Interrupt controller | GIC-400 (GICv2) | 8259 PIC |
| Timer | ARM Generic Timer (CNTP) | 8254 PIT |

---

## Target Hardware

### Primary Target

- **Board**: Gigabyte H610M S2H V2
- **CPU**: Intel Core i7-6700 (Skylake, 4 cores / 8 threads)
- **GPU**: NVIDIA GeForce RTX 3050 (for future CUDA support)
- **Memory**: 16 GB DDR4
- **Boot media**: SD card via SDWire (USB mass storage to UEFI)
- **Serial**: Native RS-232 COM port → USB-serial adapter (Prolific) to lab server

### Lab Integration

The x86-64 target is managed by labctl as `test-pc`:

| Resource | Assignment |
|----------|------------|
| Power | Kasa smart plug outlet 4 |
| Serial | `/dev/lab/port-2-9` → TCP:4006 (115200 baud) |
| SDWire | `pc-sdwire` (SDWire original, serial `sd-wire_1`) |
| Network | 192.168.4.136 (ethernet) |

### QEMU Testing

```bash
# ISO boot (BIOS GRUB)
qemu-system-x86_64 -m 256M -cdrom build/x86_64-test/slmos-x86.iso -serial stdio -display none

# Direct kernel boot (Multiboot2 — QEMU only)
qemu-system-x86_64 -m 256M -kernel build/x86_64-test/kernel-x86.elf -serial stdio -display none
```

---

## Boot Sequence

The x86-64 boot process transitions from UEFI firmware through GRUB to 64-bit long mode:

```
┌─────────────────────────────────────────────────────────────────────┐
│  UEFI Firmware                                                      │
│  - Discovers SD card as USB mass storage (via SDWire)               │
│  - Finds GPT partition with EFI System Partition                    │
│  - Loads EFI/BOOT/BOOTX64.EFI (GRUB)                               │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  GRUB Bootloader (EFI)                                              │
│  - Runs embedded prefix config (grub-mkimage -c)                    │
│  - Finds /slmos/kernel.elf on EFI partition                         │
│  - Loads kernel ELF via multiboot2 command                          │
│  - Jumps to _start in 32-bit protected mode                        │
│  - Passes multiboot info pointer in EBX                             │
│  - NOTE: UEFI GRUB does not reliably pass magic in EAX             │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  32-bit Trampoline (trampoline32.S)                                 │
│  - Checks CPUID and long mode support                               │
│  - Sets up 4-level page tables (identity mapping first 1GB)         │
│  - Enables PAE in CR4                                               │
│  - Loads PML4 address into CR3                                      │
│  - Enables long mode in EFER MSR                                    │
│  - Enables paging in CR0 (activates long mode)                      │
│  - Loads 64-bit GDT                                                 │
│  - Far jumps to entry64                                             │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  64-bit Entry (entry64.S)                                           │
│  - Sets up 64-bit segment registers                                 │
│  - Initializes 64-bit stack                                         │
│  - Clears BSS section                                               │
│  - Calls kernel_main with multiboot info pointer                    │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  C Kernel (main_x86.c)                                              │
│  - Initializes COM1 serial console (115200 baud)                    │
│  - Loads IDT (48 vectors: exceptions 0-31, IRQs 32-47)             │
│  - Remaps 8259 PIC (IRQ 0-15 → vectors 32-47)                      │
│  - Initializes PIT timer at 100 Hz                                  │
│  - Enables interrupts (STI)                                         │
│  - Parses Multiboot2 info (memory map, bootloader name)             │
└─────────────────────────────────────────────────────────────────────┘
```

### Mode Transition Details

The transition from 32-bit to 64-bit mode requires:

1. **Enable PAE** (CR4 bit 5)
2. **Load PML4** into CR3
3. **Enable Long Mode** in EFER MSR (bit 8)
4. **Enable Paging** in CR0 (bit 31)
5. **Load 64-bit GDT**
6. **Far jump** to 64-bit code segment

The far jump must use segment selector 0x08 (64-bit code segment) to actually switch the CPU into 64-bit mode.

---

## Memory Layout

### Physical Memory Map

```
0x00000000 - 0x000FFFFF    Reserved (real mode, legacy)
0x00100000 - 0x001FFFFF    Kernel image
  0x00100000               Multiboot2 header
  0x00101000               .text (code)
  0x00102000               .rodata (constants, GDT)
  0x00103000               .data (initialized data)
  0x00104000               .bss (stack, IDT)
  0x00109000               .page_tables (PML4, PDPT, PD)
0x00200000 - 0x3FFFFFFF    Available (identity mapped)
```

### Linker Script Sections

| Section | Purpose | Attributes |
|---------|---------|------------|
| `.multiboot` | Multiboot2 header | Read-only |
| `.text` | Executable code (boot, ISR stubs, kernel) | Read/Execute |
| `.rodata` | Constants, GDT | Read-only |
| `.data` | Initialized data | Read/Write |
| `.bss` | Uninitialized data, stack, IDT | Read/Write |
| `.page_tables` | Page tables (not zeroed) | Read/Write |

---

## Page Tables

### 4-Level Paging Structure

x86-64 long mode uses 4-level page tables:

```
CR3 ──► PML4 ──► PDPT ──► PD ──► (2MB pages)
        [512]    [512]   [512]
```

### Identity Mapping

The boot code creates an identity mapping in two stages:

**Stage 1 (trampoline32.S, 32-bit):** Maps the first 4 GB using 4 PD pages.

**Stage 2 (vmm_init, 64-bit C):** Extends the mapping to cover all RAM detected from the Multiboot2 memory map. On the i7-6700 with 16 GB, this maps 20 GB (20 PD pages total).

- **PML4[0]** → PDPT (single entry, covers 512 GB)
- **PDPT[0..N]** → PD[0..N] (one per GB of mapped RAM)
- **PD[i][0-511]** → 2MB pages (512 entries = 1 GB per PD)

Virtual address == physical address for all mapped memory.

### Page Table Entry Format (2MB Page)

```
Bit     Description
────────────────────────────────────────
0       Present (P)
1       Read/Write (R/W)
7       Page Size (PS) - must be 1 for 2MB
21-51   Physical Address bits 21-51
63      No Execute (NX)
```

---

## GDT and Segments

### GDT Structure

```
Offset  Segment         Description
──────────────────────────────────────────
0x00    Null            Required null descriptor
0x08    Code64          64-bit code segment (CS)
0x10    Data64          64-bit data segment (DS, ES, FS, GS, SS)
```

### Segment Descriptor Format

```c
/* Code64: 0x00AF9A000000FFFF */
/* Data64: 0x00CF92000000FFFF */
```

---

## IDT and Exceptions

### IDT Structure

The Interrupt Descriptor Table has 48 entries (16 bytes each):

| Vectors | Type | Purpose |
|---------|------|---------|
| 0-31 | Trap gates | CPU exceptions (#DE, #UD, #GP, #PF, etc.) |
| 2 | Interrupt gate | NMI (clears IF) |
| 32-47 | Interrupt gates | PIC IRQs (clears IF) |

### Exception Handler

When a CPU exception fires, the handler prints a full register dump to serial:

- Exception name and vector number
- Error code (if applicable)
- RIP, CS, RSP, SS, RFLAGS
- All general-purpose registers (RAX-R15)
- CR2 (faulting address) for page faults

### ISR Stack Frame

```
[SS, RSP, RFLAGS, CS, RIP]   ← pushed by CPU
[error_code]                   ← pushed by CPU or dummy 0
[vector]                       ← pushed by ISR stub
[R15..RAX]                     ← pushed by common handler
```

Exceptions 8, 10-14, 17, 21, 29, 30 push a hardware error code; all others get a dummy 0 to keep the stack layout uniform.

### IRQ Dispatch

IRQ handlers are registered via `irq_register(irq, handler)`. The common handler dispatches to the registered callback and sends EOI to the PIC.

---

## PIC and Timer

### 8259 PIC Remapping

The PIC is remapped to avoid conflict with CPU exception vectors:

| PIC | IRQ Range | Vector Range |
|-----|-----------|-------------|
| Master (PIC1) | IRQ 0-7 | Vectors 32-39 |
| Slave (PIC2) | IRQ 8-15 | Vectors 40-47 |

### PIT Timer (8254)

- **Channel**: 0
- **Mode**: Rate generator (mode 2)
- **Frequency**: ~100 Hz (divisor = 1193182 / 100 = 11931)
- **IRQ**: 0 → vector 32

The timer interrupt handler increments a global `pit_ticks` counter used for timing.

---

## Console Output

### Serial Console (COM1)

The x86-64 port uses the 16550 UART on COM1 for all console output:

| Parameter | Value |
|-----------|-------|
| I/O Base | 0x3F8 |
| Baud rate | 115200 |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |

Serial is initialized early in `kernel_main`, before any other subsystem.

### GRUB EFI and Framebuffer

GRUB EFI cannot reliably set a video mode on all hardware. The Multiboot2 header does not request a framebuffer tag, and the kernel does not depend on framebuffer output. All output goes through serial.

---

## Building

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt install gcc make grub-efi-amd64-bin sgdisk dosfstools qemu-system-x86
```

### Build Commands

#### CMake (recommended)

```bash
# Configure
cmake -B build -DPLATFORM=X86_64 -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build -j$(nproc)

# Output: build/slmos.elf, build/slmos.bin
```

#### Standalone Makefile (quick iterations)

```bash
# --- Standalone test kernel (boot + IDT + PIC + PIT only) ---
make -f kernel/arch/x86_64/Makefile.test           # Build standalone ELF
make -f kernel/arch/x86_64/Makefile.test disk       # UEFI disk image
make -f kernel/arch/x86_64/Makefile.test run        # QEMU (serial)

# --- Integrated kernel (full SLM-OS: scheduler, shell, PMM, VFS) ---
make -f kernel/arch/x86_64/Makefile.test integrated # Build integrated ELF
make -f kernel/arch/x86_64/Makefile.test disk-int   # UEFI disk image
make -f kernel/arch/x86_64/Makefile.test run-int    # QEMU (serial)

# --- Other targets ---
make -f kernel/arch/x86_64/Makefile.test iso        # GRUB ISO (QEMU -cdrom)
make -f kernel/arch/x86_64/Makefile.test debug      # QEMU + GDB server
make -f kernel/arch/x86_64/Makefile.test gdb        # Connect GDB
make -f kernel/arch/x86_64/Makefile.test clean      # Remove all build artifacts
```

### Build Output

```
build/x86_64-test/              # Standalone kernel
├── kernel-x86.elf
├── slmos-x86.iso / .img
└── *.o

build/x86_64-integrated/        # Integrated kernel
├── kernel-x86.elf
├── slmos-x86.img
└── kernel/**/*.o               # Mirrored source tree
```

---

## Hardware Deployment

### Deploy Workflow

```bash
# Standalone test kernel
make -f kernel/arch/x86_64/Makefile.test clean disk && \
labctl sdwire flash test-pc build/x86_64-test/slmos-x86.img

# Integrated kernel (full SLM-OS with shell)
make -f kernel/arch/x86_64/Makefile.test disk-int && \
labctl sdwire flash test-pc build/x86_64-integrated/slmos-x86.img

# Capture boot output (standalone: wait for halt; integrated: wait for shell)
labctl serial_capture test-pc --timeout 30 --until "slm-os>"
```

### UEFI Disk Image Structure

The `disk` target creates a 64 MB GPT image with one EFI System Partition (FAT32):

```
GPT Partition Table
└── Partition 1: EFI System (type EF00), FAT32
    ├── EFI/BOOT/BOOTX64.EFI    # GRUB EFI binary (grub-mkimage)
    └── slmos/kernel.elf          # SLM-OS kernel
```

GRUB is built with `grub-mkimage` (not `grub-mkstandalone`) to avoid the `normal` module, which fails to initialize video on some UEFI implementations. The prefix config is embedded directly into the EFI binary.

---

## Testing

### Functional Tests

The `test_x86_boot.c` test suite contains 49 tests across 12 categories:

| Category | Tests | Description |
|----------|-------|-------------|
| Control registers | 4 | CR0 paging, CR4 PAE, EFER long mode, CR3→PML4 |
| Page tables | 6 | PML4, PDPT, PD structure, 4GB boot mapping, PDPT[0..3] populated, vmm_init RAM detection |
| GDT | 3 | Limit, CS selector (0x08), DS selector (0x10) |
| Memory layout | 3 | Kernel at 1MB, section ordering, within mapping |
| Multiboot2 | 5 | Pointer valid, structure size, memory map, usable RAM, bootloader name |
| IDT | 4 | IDTR loaded, exception entries present, IRQ interrupt gates, exception trap gates |
| PIC | 3 | OCW3 response, timer unmasked, slave accessible |
| PIT timer | 3 | IF flag set, ticks incrementing, ~100 Hz rate |
| Platform abstraction | 9 | cpu_context offset/fields/size, platform defines, irq_save/restore, spinlock irqsave roundtrip, gic enable/disable, timer frequency, timer count |
| Scheduler integration | 5 | gic_init loads IDT, task stack within mapping, gic_end_interrupt safe, uart_putc, scheduler_tick callable |
| setjmp/longjmp | 2 | setjmp/longjmp round-trip, longjmp(0) returns 1 |
| Long mode | 2 | 64-bit operations, RIP-relative addressing |

### Running Tests

Tests are integrated into the kernel and run during boot when compiled with the test harness. They use the Unity bare-metal test framework.

---

## Platform Abstraction

The x86-64 port uses `#if defined(PLATFORM_X86_64)` guards in shared kernel headers to replace ARM64-specific inline assembly. Shared kernel code (scheduler, PMM, shell, VFS, IPC) runs unmodified.

### Modified Shared Headers

| Header | x86-64 Changes |
|--------|----------------|
| `platform.h` | RAM_BASE, UART_BASE, TIMER_IRQ, CPU_MAX for x86-64 |
| `task.h` | x86-64 `struct cpu_context` (rbx, rbp, r12-r15, rsp, rip, rflags) |
| `spinlock.h` | x86-64 barriers (mfence/lfence/sfence), irq_save (pushfq+cli), spin_lock (no-op single core) |
| `cache.h` | No-ops (x86-64 fully hardware cache coherent) |
| `smp.h` | `cpu_id()` returns 0 (single core) |

### x86-64 cpu_context Layout

```
struct task offset 0x20 + struct cpu_context:
  +0x00: rbx    +0x08: rbp    +0x10: r12    +0x18: r13
  +0x20: r14    +0x28: r15    +0x30: rsp    +0x38: rip
  +0x40: rflags
```

Total: 72 bytes. Offsets hardcoded in `context.S` as `CTX_RBX`, `CTX_RSP`, etc.

### Platform Driver Mapping

| Main Kernel Interface | x86-64 Implementation |
|-----------------------|----------------------|
| `uart.h` (uart_init, uart_putc, uart_getc) | `kernel/drivers/uart_x86.c` — 16550 COM1 |
| `gic.h` (gic_init, gic_enable_irq, gic_end_interrupt) | `kernel/arch/x86_64/pic.c` — 8259 PIC |
| `timer.h` (timer_init, timer_start, timer_handler) | `kernel/arch/x86_64/timer_x86.c` — 8254 PIT |
| `switch_to()` (context.S) | `kernel/arch/x86_64/context.S` — x86-64 registers |
| `smp_init`, `vmm_init`, DTB/Rust stubs | `kernel/arch/x86_64/platform_x86.c` |

---

## Lua Scripting

The Lua 5.4 runtime is compiled for x86-64 with the following adaptations:

- **setjmp/longjmp**: `kernel/arch/x86_64/setjmp.S` saves x86-64 callee-saved registers (rbx, rbp, r12-r15, rsp, rip). Also provides `_setjmp` alias (POSIX variant used by Lua).
- **SSE enabled**: Lua uses `double` which requires SSE registers on x86-64. The Lua library is compiled with `-msse -msse2` while the kernel remains `-mno-sse`.
- **glibc ABI stubs**: `__errno_location()`, `__ctype_b_loc()` (static classification table), `stdin`/`stdout`/`stderr` as direct symbols.
- **jmp_buf**: Sized to 8 × uint64_t (64 bytes) for x86-64, vs 22 × uint64_t (176 bytes) for ARM64.

Lua commands are available in the shell via `lua <expression>`.

---

## Key Files

### Assembly

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/trampoline32.S` | 32-bit Multiboot2 entry, mode transition |
| `kernel/arch/x86_64/entry64.S` | 64-bit entry, BSS clear, calls `kernel_main_x86` |
| `kernel/arch/x86_64/idt.S` | ISR stubs for exceptions (0-31) and IRQs (32-47) |
| `kernel/arch/x86_64/context.S` | `switch_to()` context switch + `task_entry_wrapper` |
| `kernel/arch/x86_64/setjmp.S` | `setjmp`/`longjmp` for Lua error handling |

### C Code — x86-64 Platform Layer

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/main_x86.c` | Standalone test kernel entry (not used in integrated build) |
| `kernel/arch/x86_64/idt.c` | IDT setup, exception handler, IRQ dispatch |
| `kernel/arch/x86_64/pic.c` | 8259 PIC driver (gic.h interface) |
| `kernel/arch/x86_64/timer_x86.c` | 8254 PIT timer (timer.h interface) |
| `kernel/arch/x86_64/platform_x86.c` | Boot glue, SMP/VMM/DTB/Rust/component stubs |
| `kernel/drivers/uart_x86.c` | 16550 UART driver (uart.h interface) |

### Build System

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/Makefile.test` | Standalone + integrated build rules |
| `kernel/kernel-x86_64.ld` | Linker script |

### Tests

| File | Purpose |
|------|---------|
| `kernel/tests/test_x86_boot.c` | 49 tests: boot, IDT, PIC, PIT, Multiboot2, platform, scheduler, setjmp |

---

## Design Decisions

### Why Multiboot2 Instead of Raw UEFI?

1. **Simpler**: GRUB handles UEFI complexity
2. **Portable**: Same kernel works with BIOS and UEFI GRUB
3. **Proven**: Well-documented, widely used

### Why grub-mkimage Instead of grub-mkstandalone?

`grub-mkstandalone` includes the `normal` module which tries to initialize `gfxterm` before processing the config. On some UEFI implementations this fails with "no suitable video mode found." `grub-mkimage` with `-c` embeds the config as prefix commands that execute immediately without `normal`, avoiding the video initialization entirely.

### Why Skip Multiboot2 Magic Check?

GRUB BIOS passes 0x36D76289 in EAX per the Multiboot2 spec, but GRUB EFI does not reliably set this value. The multiboot info pointer in EBX is valid in both cases, so the magic check is skipped. The kernel validates the multiboot2 info structure itself.

### Why Split Assembly Files?

GAS generates 64-bit instructions (RIP-relative addressing) even with `.code32` when the output format is elf64. Solution:

1. `trampoline32.S` compiled with `-m32` (true 32-bit code)
2. `objcopy` converts to elf64-x86-64 format
3. `entry64.S` and `idt.S` compiled with `-m64` (native 64-bit)
4. Link all together

### Why 2MB Pages?

1. **Simplicity**: No need for PT level (only PML4→PDPT→PD)
2. **Performance**: Fewer TLB entries needed
3. **Boot speed**: Identity mapping 1GB requires only 512 PD entries

### Why 8259 PIC Instead of APIC?

The 8259 PIC is simpler and sufficient for single-core bring-up. APIC/IOAPIC will be needed later for SMP, but the PIC provides a working interrupt path with minimal code.

### Why Separate .page_tables Section?

The BSS clear loop (`rep stosq`) runs after paging is enabled. If page tables were in BSS, clearing BSS would zero the active page tables, causing immediate page faults.

---

## Troubleshooting

### Boot Halts Silently (No Serial Output After BIOS)

The 32-bit trampoline halts when CPUID or long mode checks fail. Since the serial port is not yet initialized at this stage, the halt is silent.

**Debug**: Add early serial output before the failing check (see commit history for diagnostic approach).

### Triple Fault / Reboot Loop

Common causes:
1. **IDT not loaded** — `idt_init()` must be called before any interrupts can fire. In the integrated build, `gic_init()` in `pic.c` calls `idt_init()`. If the IDT is missing, the first timer interrupt causes a triple fault.
2. Invalid page table entries
3. GDT not loaded correctly
4. Far jump target wrong
5. BSS clear zeroing page tables
6. **Premature STI** — enabling interrupts before the context switch to the first task corrupts the boot stack context. Interrupts should be enabled by `task_entry_wrapper` after the task's stack and registers are fully set up.

**Debug**: Add serial output markers between each step to isolate failure point. Output to COM1 (0x3F8) directly from assembly with `outb`.

### GRUB Shows "no suitable video mode found"

This occurs when GRUB's `normal` module fails to initialize `gfxterm` via UEFI GOP. Use `grub-mkimage` instead of `grub-mkstandalone` to avoid loading `normal`.

### GRUB Shows "Unknown command"

The embedded GRUB prefix config uses commands that require specific modules. Ensure all needed modules are listed in the `grub-mkimage` command line (e.g., `part_gpt fat multiboot2 search all_video`).

### Garbled Serial Output from BIOS

The UEFI firmware outputs POST messages on the serial port at a different baud rate (typically 9600). This garbled data appears before SLM-OS initializes the serial port at 115200. It is harmless.

---

## Known Issues

No critical known issues. All 16 GB RAM is mapped and usable.

---

## References

- [Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)
- [AMD64 Architecture Programmer's Manual](https://developer.amd.com/resources/developer-guides-manuals/)
- [Intel 64 and IA-32 Architectures Software Developer's Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [OSDev Wiki - Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)
- [OSDev Wiki - 8259 PIC](https://wiki.osdev.org/8259_PIC)
- [OSDev Wiki - Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer)

---

*Last updated: April 2026*
