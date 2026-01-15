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
7. [Console Output](#console-output)
8. [Building](#building)
9. [Testing](#testing)
10. [Key Files](#key-files)
11. [Design Decisions](#design-decisions)
12. [Troubleshooting](#troubleshooting)

---

## Overview

The x86-64 port enables SLM-OS to run on standard PC hardware with x86-64 processors. This port uses:

- **Boot method**: Multiboot2 via GRUB
- **Console**: UEFI GOP framebuffer (no serial port required)
- **Paging**: 4-level page tables with 2MB pages
- **Mode**: 64-bit long mode

### Platform Differences from ARM64

| Feature | ARM64 (Jetson/Pi) | x86-64 |
|---------|-------------------|--------|
| Boot method | kexec / UEFI | GRUB Multiboot2 |
| Console | UART serial | Framebuffer |
| Page size | 4KB/64KB | 4KB/2MB |
| Exception levels | EL0-EL3 | Ring 0-3 |
| Privilege modes | EL1 (kernel) | Ring 0 |

---

## Target Hardware

### Primary Target

- **CPU**: Intel Core i7-6700 (Skylake)
- **GPU**: NVIDIA GeForce RTX 3050 (for future CUDA support)
- **Boot device**: 120GB USB SSD
- **Memory**: 16GB+ DDR4

### QEMU Testing

The x86-64 port is tested in QEMU before deployment to real hardware:

```bash
qemu-system-x86_64 -m 256M -cdrom build/x86_64-test/slmos-x86.iso -serial stdio
```

---

## Boot Sequence

The x86-64 boot process transitions from 32-bit protected mode (GRUB) to 64-bit long mode:

```
┌─────────────────────────────────────────────────────────────────────┐
│  GRUB Bootloader                                                    │
│  - Loads kernel ELF at 1MB (0x100000)                               │
│  - Verifies Multiboot2 header                                       │
│  - Jumps to _start in 32-bit protected mode                         │
│  - Passes magic (0x36D76289) in EAX, info pointer in EBX            │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  32-bit Trampoline (trampoline32.S)                                 │
│  - Validates Multiboot2 magic                                       │
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
│  - Initializes framebuffer console                                  │
│  - Parses Multiboot2 info for framebuffer                           │
│  - Displays boot messages                                           │
│  - Halts (test kernel)                                              │
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

The far jump is critical - it must use segment selector 0x08 (64-bit code segment) to actually switch the CPU into 64-bit mode.

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
  0x00104000               .bss (stack)
  0x00109000               .page_tables (PML4, PDPT, PD)
0x00200000 - 0x3FFFFFFF    Available (identity mapped)
```

### Linker Script Sections

| Section | Purpose | Attributes |
|---------|---------|------------|
| `.multiboot` | Multiboot2 header | Read-only |
| `.text` | Executable code | Read/Execute |
| `.rodata` | Constants, GDT | Read-only |
| `.data` | Initialized data | Read/Write |
| `.bss` | Uninitialized data, stack | Read/Write |
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

The boot code creates an identity mapping of the first 1GB using 2MB pages:

- **PML4[0]** → PDPT (single entry)
- **PDPT[0]** → PD (single entry)
- **PD[0-511]** → 2MB pages (512 entries = 1GB)

This means virtual address == physical address for 0x00000000 - 0x3FFFFFFF.

### Page Table Entry Format (2MB Page)

```
Bit     Description
────────────────────────────────────────
0       Present (P)
1       Read/Write (R/W)
2       User/Supervisor (U/S)
3       Page Write-Through (PWT)
4       Page Cache Disable (PCD)
5       Accessed (A)
6       Dirty (D)
7       Page Size (PS) - must be 1 for 2MB
8       Global (G)
12-20   Reserved (must be 0)
21-51   Physical Address bits 21-51
52-62   Reserved
63      No Execute (NX)
```

### Page Tables in Separate Section

The page tables are placed in a `.page_tables` section separate from `.bss` to prevent them from being zeroed during BSS clearing. This is critical because:

1. BSS clear happens after paging is enabled
2. Zeroing active page tables causes immediate page faults
3. The separate section ensures page tables remain intact

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

Bits 0-15:   Limit low (0xFFFF)
Bits 16-31:  Base low (0x0000)
Bits 32-39:  Base middle (0x00)
Bits 40-47:  Access byte
             Code64: 0x9A (Present, Ring 0, Code, Execute/Read)
             Data64: 0x92 (Present, Ring 0, Data, Read/Write)
Bits 48-51:  Limit high (0xF)
Bits 52-55:  Flags
             Code64: 0xA (64-bit, limit in 4K units)
             Data64: 0xC (32-bit compat, limit in 4K units)
Bits 56-63:  Base high (0x00)
```

---

## Console Output

### Framebuffer Console

The x86-64 port uses a UEFI GOP framebuffer for console output instead of serial UART:

1. GRUB requests framebuffer via Multiboot2 tag
2. Kernel parses Multiboot2 info for framebuffer address
3. `fb_console.c` renders 8x16 bitmap font to framebuffer

### Framebuffer Info (from Multiboot2)

| Field | Description |
|-------|-------------|
| `addr` | Physical address of framebuffer |
| `pitch` | Bytes per scanline |
| `width` | Width in pixels |
| `height` | Height in pixels |
| `bpp` | Bits per pixel (typically 32) |
| `type` | 1 = RGB direct color |

### Serial Debug (COM1)

For debugging, the boot code outputs progress markers to COM1 (0x3F8):

- Boot errors output `!` before halting
- Serial is available even without framebuffer

---

## Building

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt install gcc make grub-pc-bin xorriso qemu-system-x86
```

### Build Commands

```bash
# Build kernel ELF
make -f kernel/arch/x86_64/Makefile.test

# Create bootable ISO
make -f kernel/arch/x86_64/Makefile.test iso

# Run in QEMU (text mode with serial)
make -f kernel/arch/x86_64/Makefile.test run

# Run in QEMU (GUI mode with framebuffer)
make -f kernel/arch/x86_64/Makefile.test run-gui

# Debug with GDB
make -f kernel/arch/x86_64/Makefile.test debug
# In another terminal:
make -f kernel/arch/x86_64/Makefile.test gdb
```

### Build Output

```
build/x86_64-test/
├── kernel-x86.elf        # Kernel ELF binary
├── slmos-x86.iso         # Bootable ISO
├── boot/
│   ├── kernel.elf        # Copy for ISO
│   └── grub/
│       └── grub.cfg      # GRUB configuration
└── *.o                   # Object files
```

---

## Testing

### Functional Tests

The `test_x86_boot.c` test suite verifies:

- **Control registers**: CR0 paging, CR4 PAE, EFER long mode
- **Page tables**: PML4, PDPT, PD structure and entries
- **GDT**: Limit, CS selector (0x08), DS selector (0x10)
- **Memory layout**: Kernel at 1MB, section ordering
- **64-bit mode**: 64-bit operations, RIP-relative addressing
- **Framebuffer**: Console output functionality

### Running Tests

Tests are integrated into the kernel and run during boot when test mode is enabled.

### QEMU Test Command

```bash
# Quick boot test with serial output
timeout 5 qemu-system-x86_64 -m 256M \
    -cdrom build/x86_64-test/slmos-x86.iso \
    -serial stdio \
    -display none
```

Expected output:
```
[SLM-OS x86-64] Boot started
[SLM-OS x86-64] Initializing framebuffer...
```

---

## Key Files

### Assembly

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/trampoline32.S` | 32-bit Multiboot2 entry, mode transition |
| `kernel/arch/x86_64/entry64.S` | 64-bit entry, BSS clear, kernel call |

### C Code

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/main_x86.c` | Test kernel entry point |
| `kernel/drivers/fb_console.c` | Framebuffer console driver |

### Build System

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/Makefile.test` | x86-64 build rules |
| `kernel/kernel-x86_64.ld` | Linker script |

### Tests

| File | Purpose |
|------|---------|
| `kernel/tests/test_x86_boot.c` | Boot and platform tests |

---

## Design Decisions

### Why Multiboot2 Instead of Raw UEFI?

1. **Simpler**: GRUB handles UEFI complexity
2. **Portable**: Same kernel works with BIOS and UEFI
3. **Framebuffer**: GRUB requests GOP framebuffer via Multiboot2 tag
4. **Proven**: Well-documented, widely used

### Why Split Assembly Files?

GAS generates 64-bit instructions (RIP-relative addressing) even with `.code32` directive when the output format is elf64. Solution:

1. `trampoline32.S` compiled with `-m32` (true 32-bit code)
2. `objcopy` converts to elf64-x86-64 format
3. `entry64.S` compiled with `-m64` (native 64-bit)
4. Link both together

### Why 2MB Pages?

1. **Simplicity**: No need for PT level (only PML4→PDPT→PD)
2. **Performance**: Fewer TLB entries needed
3. **Boot speed**: Identity mapping 1GB requires only 512 PD entries
4. **Sufficient**: Full kernel fits in first 2MB

### Why Separate .page_tables Section?

The BSS clear loop (`rep stosq`) runs after paging is enabled. If page tables were in BSS, clearing BSS would zero the active page tables, causing immediate page faults. The separate section ensures page tables remain intact.

---

## Troubleshooting

### Boot Halts with '!' on Serial

The 32-bit trampoline outputs '!' and halts when:
- Multiboot2 magic (0x36D76289) not in EAX
- CPUID not supported
- Long mode not supported

**Solution**: Verify GRUB configuration uses `multiboot2` command.

### Triple Fault / Reboot Loop

Common causes:
1. Invalid page table entries
2. GDT not loaded correctly
3. Far jump target wrong
4. BSS clear zeroing page tables

**Debug**: Add serial output markers between each step to isolate failure point.

### No Framebuffer Output

1. Verify Multiboot2 framebuffer tag in header
2. Check GRUB provides framebuffer info
3. Verify framebuffer address is within mapped memory

### GDT Pointer Relocation Issue

If `lgdt` loads from wrong address, ensure `gdt64_ptr` is declared `.global` in assembly. Without this, the assembler generates a section-relative relocation instead of a symbol relocation.

---

## References

- [Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)
- [AMD64 Architecture Programmer's Manual](https://developer.amd.com/resources/developer-guides-manuals/)
- [Intel 64 and IA-32 Architectures Software Developer's Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [OSDev Wiki - Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)

---

*Last updated: January 2026*
