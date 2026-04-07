# SLM-OS x86-64 Port

This document describes the x86-64 port of SLM-OS, including architecture details, boot sequence, build instructions, and design decisions.

**Status:** Milestones M1–M7 complete. Full OS boots on real hardware with 8-CPU SMP, PCI enumeration, NVIDIA RTX 3050 GPU identification + VRAM access, and topic-based message routing (pub/sub IPC). GSP firmware loading (required for GPU compute) documented as future work.

---

## Table of Contents

0. [Current Status](#current-status)

1. [Overview](#overview)
2. [Target Hardware](#target-hardware)
3. [Boot Sequence](#boot-sequence)
4. [Memory Layout](#memory-layout)
5. [Page Tables](#page-tables)
6. [GDT and Segments](#gdt-and-segments)
7. [IDT and Exceptions](#idt-and-exceptions)
8. [Interrupt Controller (APIC)](#interrupt-controller-apic)
9. [Timer (LAPIC)](#timer-lapic)
10. [SMP (Symmetric Multi-Processing)](#smp-symmetric-multi-processing)
11. [PCI/PCIe Enumeration](#pcipcie-enumeration)
12. [NVIDIA GPU (RTX 3050)](#nvidia-gpu-rtx-3050)
13. [Console Output](#console-output)
14. [Building](#building)
15. [Hardware Deployment](#hardware-deployment)
16. [Testing](#testing)
17. [Platform Abstraction](#platform-abstraction)
18. [Lua Scripting](#lua-scripting)
19. [Key Files](#key-files)
20. [Design Decisions](#design-decisions)
21. [Troubleshooting](#troubleshooting)
22. [Recommended Next Steps](#recommended-next-steps)

---

## Current Status

*Last verified: April 2026 on Gigabyte H610M S2H V2 (i7-6700 + RTX 3050)*

### What Works

| Feature | QEMU | Real Hardware | Notes |
|---------|------|--------------|-------|
| Boot (Multiboot2 → long mode) | ✅ | ✅ | UEFI GRUB via SD card |
| 4-level paging (up to 20 GB) | ✅ 4 GB | ✅ 20 GB | Identity mapped, 2MB pages |
| SMP (INIT-SIPI-SIPI) | ✅ 4 CPUs | ✅ 8 CPUs | All cores + hyperthreads |
| Preemptive scheduler | ✅ | ✅ | LAPIC timer, 100 Hz, per-CPU |
| IDT + LAPIC + IOAPIC | ✅ | ✅ | 64 vectors, exception handling |
| Spinlocks (TTAS atomic) | ✅ | ✅ | Correct under SMP load |
| PCI enumeration | ✅ 6 devices | ✅ 21 devices | ECAM on hardware, legacy I/O in QEMU |
| NVIDIA GPU identification | N/A | ✅ GA107 | BOOT_42 → chip 0x177, Ampere, Rev 10.1 |
| VRAM read/write (BAR1) | N/A | ✅ 5 offsets | 256 MB aperture, pattern verified |
| Shell (30+ commands) | ✅ | ✅ | Including `gpu`, `pci`, `lua` |
| Lua 5.4 scripting | ✅ | ✅ | setjmp/longjmp, SSE for math |
| VFS + LittleFS | ✅ | ✅ | RAM disk, file commands |
| PMM buddy allocator | ✅ | ✅ | 19,916 MB usable on hardware |
| Component system | ✅ | ✅ | Counter, echo, listener services |
| Message router (pub/sub) | ✅ | N/A | Topic-based IPC, yield-based delivery |
| Echo IPC (shared mailbox) | ✅ | ✅ | Atomic mailbox, round-robin scheduling |
| GPU compute / 3D | ❌ | ❌ | Requires GSP firmware (documented) |

### Milestone Completion

| Milestone | Description | Status |
|-----------|-------------|--------|
| M1 | Boot Foundation | ✅ Complete |
| M2 | Memory Management | ✅ Complete |
| M3 | Interrupts & Timer | ✅ Complete |
| M4 | SMP (Multi-Core) | ✅ Complete |
| M5 | PCIe Enumeration | ✅ Complete |
| M6 | NVIDIA GPU Driver | ✅ Complete (GSP deferred) |
| M7 | Platform Abstraction | ✅ Complete (arch.h, Rust linked) |
| M8 | Testing & Validation | ✅ Complete (90 tests, CI, benchmarks) |
| M9 | Documentation | ✅ Complete (GSP, arch comparison, Jetson checklist) |

### Hardware Test Results (i7-6700 + RTX 3050)

```
8/8 CPUs online (4 cores × 2 hyperthreads)
20 GB RAM mapped (19,916 MB usable)
ECAM at 0xC0000000 (ACPI MCFG, 256 buses)
21 PCI devices (Intel H610 chipset + NVIDIA RTX 3050 + Realtek 8168)

NVIDIA RTX 3050 (GA107):
  PCI 01:00.0, device 0x2584
  BOOT_42: 0x177A1000 → Ampere architecture, chip 0x177, rev 10.1
  BAR0: 0x53000000 (16 MB MMIO) — registers readable
  BAR1: 0x40000000 (256 MB VRAM) — read/write verified
  PMC_ENABLE: 0x40000000 (PDISPLAY active from Linux)
  Engine registers: 0xBADF5040 (GSP not loaded)
```

---

## Overview

The x86-64 port enables SLM-OS to run on standard PC hardware with x86-64 processors. This port uses:

- **Boot method**: Multiboot2 via GRUB (UEFI)
- **Console**: COM1 serial (115200 baud, 8N1)
- **Paging**: 4-level page tables with 2MB pages
- **Mode**: 64-bit long mode
- **Interrupts**: LAPIC + IOAPIC (8259 PIC disabled)
- **Timer**: LAPIC timer at 100 Hz (calibrated against PIT)

### Platform Differences from ARM64

| Feature | ARM64 (Pi 5) | x86-64 |
|---------|--------------|--------|
| Boot method | SD card / UEFI | GRUB Multiboot2 (UEFI) |
| Console | UART serial (PL011) | COM1 serial (16550) |
| Page size | 4KB/64KB | 4KB/2MB |
| Exception model | EL0-EL3 | Ring 0-3, IDT |
| Kernel privilege | EL1 | Ring 0 |
| Interrupt controller | GIC-400 (GICv2) | LAPIC + IOAPIC |
| Timer | ARM Generic Timer (CNTP) | LAPIC timer (~1 GHz) |

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
# ISO boot (recommended — works on all QEMU versions)
qemu-system-x86_64 -m 4G -smp 4 -cdrom build/slmos.iso -serial stdio -display none

# SMP testing with more CPUs
qemu-system-x86_64 -m 4G -smp 8 -cdrom build/slmos.iso -serial stdio -display none
```

**Note:** QEMU 8.2.2 on Ubuntu 24.04 does not support Multiboot2 via `-kernel`. Use GRUB ISO (`-cdrom`) instead. Create the ISO with `grub-mkrescue`.

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
│  - Sets up 4-level page tables (identity mapping first 4GB)         │
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
│  C Kernel (platform_x86.c → main.c)                                 │
│  - Initializes COM1 serial (115200 baud)                            │
│  - PMM buddy allocator, VMM extends paging to all RAM               │
│  - SMP: ACPI MADT discovery, INIT-SIPI-SIPI boots all CPUs          │
│  - IDT (64 vectors), disables 8259 PIC, inits LAPIC + IOAPIC        │
│  - LAPIC timer at 100 Hz (calibrated against PIT)                    │
│  - PCI enumeration (legacy I/O or ECAM)                              │
│  - Scheduler, IPC, VFS, LittleFS, Lua, shell                        │
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
0x00000000 - 0x00007FFF    Reserved (real mode IVT, BIOS data)
0x00008000 - 0x00008FFF    AP trampoline (copied at runtime for SMP boot)
0x00100000 - 0x0066FFFF    Kernel image (~5.5 MB with all subsystems)
  0x00100000               .multiboot header
  0x00101000               .text (code, ~250 KB)
  0x0013E000               .rodata (constants, GDT, ~52 KB)
  0x0014C000               .data (initialized data)
  0x0014D000               .bss (stack, IDT, PMM state, ~5 MB)
  0x00658000               .page_tables (PML4, PDPT, 20×PD = 88 KB)
0x00200000 - RAM end       Available (identity mapped, up to 20 GB)
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

The Interrupt Descriptor Table has 64 entries (16 bytes each):

| Vectors | Type | Purpose |
|---------|------|---------|
| 0-31 | Trap gates | CPU exceptions (#DE, #UD, #GP, #PF, etc.) |
| 2 | Interrupt gate | NMI (clears IF) |
| 32-47 | Interrupt gates | IOAPIC device IRQs (clears IF) |
| 48-63 | Interrupt gates | LAPIC timer (48), IPIs, reserved |

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

IRQ handlers are registered via `irq_register(irq, handler)`. The common handler dispatches to the registered callback and sends EOI via LAPIC.

---

## Interrupt Controller (APIC)

### Architecture

The x86-64 port uses the Advanced Programmable Interrupt Controller (APIC):

| Component | Address | Purpose |
|-----------|---------|---------|
| Local APIC (LAPIC) | 0xFEE00000 | Per-CPU interrupt handling, timer, IPI |
| I/O APIC (IOAPIC) | 0xFEC00000 | External device interrupt routing |

The legacy 8259 PIC is disabled at boot (remapped to vectors 0xF0-0xFF, all masked).

### ACPI Discovery

CPU topology and APIC addresses are discovered from the ACPI MADT table:
- RSDP found via Multiboot2 tag (type 14/15) or BIOS memory scan
- MADT provides: LAPIC base, IOAPIC base, CPU APIC IDs, Interrupt Source Overrides
- ISA IRQ 0 (PIT) is typically redirected to IOAPIC pin 2 via an ISO entry

### Interrupt Vector Layout

| Vector Range | Purpose |
|-------------|---------|
| 0-31 | CPU exceptions (trap gates) |
| 32-47 | IOAPIC device IRQs (keyboard, serial, etc.) |
| 48 | LAPIC timer (per-CPU, periodic) |
| 49-63 | Reserved for IPIs and future use |
| 0xFF | Spurious interrupt vector |

## Timer (LAPIC)

The per-CPU LAPIC timer replaces the legacy 8254 PIT:

- **Mode**: Periodic (auto-reload)
- **Frequency**: 100 Hz (TIMER_HZ)
- **Calibration**: Measured against PIT channel 2 (~10ms one-shot)
- **Divide**: 16 (LAPIC timer divide register = 0x03)
- **Vector**: 48

On the i7-6700, the LAPIC timer clock runs at ~1 GHz. Each CPU has its own LAPIC timer for preemptive scheduling. Only the BSP (CPU 0) increments the global `pit_ticks` counter — secondary CPUs call `scheduler_tick()` but don't update the tick counter (prevents N× tick rate on N-CPU systems).

---

## SMP (Symmetric Multi-Processing)

SLM-OS boots all detected CPUs on x86-64 via the standard INIT-SIPI-SIPI sequence.

### CPU Discovery

CPUs are discovered via ACPI MADT parsing (`acpi.c`). The MADT provides:
- Local APIC entries with APIC ID and enabled flag
- I/O APIC address and GSI base
- Interrupt Source Overrides

On the i7-6700: 16 MADT entries, 8 enabled (4 cores × 2 hyperthreads).
On QEMU with `-smp 4`: 4 MADT entries, 4 enabled.

### AP Boot Sequence

1. BSP copies `ap_trampoline.S` blob to physical address 0x8000 (below 1MB)
2. BSP fills boot parameters at 0x8F00: CR3, GDT, IDT, stack, CPU ID, entry point
3. For each AP:
   - Send INIT IPI via LAPIC ICR → resets AP
   - Wait 10ms
   - Send SIPI with vector 0x08 (0x8000 / 4096) → AP wakes at 0x8000
   - AP executes trampoline: real mode → protected mode → long mode
   - AP loads BSP's page tables, GDT, IDT from params
   - AP jumps to `ap_entry_64()` on its own 16KB stack
4. AP initializes: LAPIC, timer, scheduler, then enters `scheduler_start()`

### AP Trampoline (`ap_trampoline.S`)

```
SIPI → real mode (0x8000)
  ├─ Load temporary 32-bit GDT
  ├─ Enable PE (CR0 bit 0)
  └─ Far jump to 32-bit code
       ├─ Enable PAE (CR4 bit 5)
       ├─ Load BSP's PML4 into CR3
       ├─ Enable LME in EFER
       ├─ Enable PG (CR0 bit 31)
       ├─ Load BSP's 64-bit GDT
       └─ Far jump to 64-bit code
            ├─ Set segments, load IDT
            ├─ Load per-CPU stack
            ├─ Signal BSP (flag = 1)
            └─ Jump to ap_entry_64()
```

### Per-CPU Identification

`cpu_id()` reads the current CPU's LAPIC ID (memory-mapped at 0xFEE00020) and looks it up in `cpu_logical_map[]` to get the logical CPU ID (0, 1, 2, ...).

### Key Files

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/ap_trampoline.S` | Real-mode AP startup, mode transitions |
| `kernel/arch/x86_64/platform_x86.c` | `smp_init()`, `boot_ap()`, `ap_entry_64()` |
| `kernel/arch/x86_64/acpi.c` | ACPI MADT parsing for CPU topology |

---

## PCI/PCIe Enumeration

SLM-OS discovers PCI devices during boot using legacy I/O config space access, with optional ECAM (memory-mapped) via ACPI MCFG.

### Config Space Access

Two methods are supported, selected automatically:

| Method | Mechanism | When Used |
|--------|-----------|-----------|
| Legacy I/O | Ports 0xCF8 (address) / 0xCFC (data) | Always available (fallback) |
| ECAM | Memory-mapped, from ACPI MCFG table | If MCFG table present |

### Bus Enumeration

1. Check host bridge at 00:00.0
2. Scan all 32 devices on bus 0 (check multi-function via header type bit 7)
3. For each PCI-PCI bridge (class 06:04), scan the secondary bus
4. Store vendor/device ID, class, subclass, BARs, IRQ for each device

### Shell Command

```
slmos> pci
PCI Devices (6 found):
  BDF       Vendor:Device  Class     Description
  --------  -------------  --------  -----------
  00:00.0   8086:1237      06:00     Host Bridge
  00:01.0   8086:7000      06:01     ISA Bridge
  ...
```

NVIDIA GPUs (vendor 0x10DE) are highlighted with BAR details during boot.

### Key Files

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/pci.c` | Config access, enumeration, shell command |

---

## NVIDIA GPU (RTX 3050)

SLM-OS includes an NVIDIA GPU probe that reads GPU identification registers via PCI BAR0 and maps VRAM via BAR1.

### GPU Identification

After PCI enumeration finds an NVIDIA device (vendor 0x10DE, display class 0x03), the driver:

1. Enables memory space + bus mastering in PCI command register
2. Maps BAR0 (16 MB MMIO) and BAR1 (VRAM aperture) with size probing
3. Reads `NV_PMC_BOOT_42` at BAR0 offset 0xA00 for chip identification:
   - Bits 29:24 = architecture (0x17 = Ampere)
   - Bits 23:20 = implementation (0x07 = GA107)
   - Bits 29:20 = chip ID (0x177)
   - Bits 19:16 = major revision, 15:12 = minor revision

### BAR Layout

| BAR | Type | Typical Size | Content |
|-----|------|-------------|---------|
| BAR0 | 32-bit MMIO | 16 MB | GPU control registers |
| BAR1 | 64-bit prefetchable | 256 MB - 8 GB | VRAM aperture |
| BAR2 | 64-bit | 32 MB | RAMIN / control structures |

### Shell Commands

- `gpu` — shows GPU model, chip ID, architecture, BARs, PMC_ENABLE
- `gpu vram` — runs extended VRAM write/read test across 5 offsets
- `gpu regs` — reads additional BAR0 registers (PTIMER, PBUS, PSTRAPS)
- `pci` — highlights NVIDIA devices with BAR details

### Key Registers Read (BAR0)

| Offset | Register | Purpose |
|--------|----------|---------|
| 0x000 | NV_PMC_BOOT_0 | Legacy GPU identification |
| 0xA00 | NV_PMC_BOOT_42 | Preferred GPU identification |
| 0x200 | NV_PMC_ENABLE | Engine master enable bitmap |

### Hardware Verification (i7-6700 + RTX 3050)

Verified on Gigabyte H610M S2H V2 via labctl (April 2026):

```
NVIDIA GPU: GA107 (Ampere)
  PCI:      01:00.0 (device 0x2584)
  Chip ID:  0x177 (rev 10.1)
  BOOT_0:   0xB77000A1
  BOOT_42:  0x177A1000
  BAR0:     0x53000000 (16 MB MMIO)
  BAR1:     0x40000000 (256 MB VRAM)
  PMC_ENABLE:    0x40000000
  PMC_INTR_HOST: 0xBADF5040
```

System context: 8/8 CPUs online, 20 GB RAM, ECAM at 0xC0000000, 21 PCI devices.

### VRAM Test

`gpu vram` shell command runs an extended VRAM test across 5 offsets (0, 1MB, 16MB, 64MB, 128MB). Each offset writes a 64-word XOR pattern and reads it back.

Verified on real hardware (April 2026): all 5 offsets PASS.

```
slmos> gpu vram
VRAM Test (BAR1 at 0x40000000, 256 MB):
  Offset 0x00000000: PASS (64 words)
  Offset 0x00100000: PASS (64 words)
  Offset 0x01000000: PASS (64 words)
  Offset 0x04000000: PASS (64 words)
  Offset 0x08000000: PASS (64 words)
Result: PASSED
```

### GPU Register Observations

Registers belonging to uninitialized engines (PBUS, PMC_INTR) return 0xBADF5040 — the GPU's default "engine not initialized" response. This indicates GSP firmware has not been loaded. PTIMER and PSTRAPS are readable because they don't require GSP.

### GSP Firmware (Future Work)

Full GPU compute requires loading the GSP (GPU System Processor) firmware — a 38 MB RISC-V binary that runs the GPU Resource Manager. This involves VBIOS parsing, SEC2 Falcon programming, cryptographic verification, and an RPC stack. GSP is mandatory on Ampere; there is no legacy register-programming mode.

See **`docs/nvidia-gsp.md`** for the complete 7-phase boot sequence, register map, firmware file locations, and nouveau source file roadmap.

### Key Files

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/nvidia_gpu.c` | GPU probe, BAR mapping, register decode, VRAM test, shell command |
| `docs/reference/nvidia-nv_ref.h` | Register definitions from open-gpu-kernel-modules |

---

## Message Router (M7)

The message router provides topic-based publish/subscribe IPC for inter-component communication.

### Architecture

Components subscribe to named topics. When a publisher sends a message to a topic, all subscribers receive it via per-subscriber mailboxes with atomic `ready`/`ack` flags. The publisher waits (yield-based) for each subscriber to acknowledge.

```
Publisher (shell)           MessageRouter             Subscriber (listener)
  |                              |                          |
  | msg_router_publish("events") |                          |
  |----------------------------->|                          |
  |                              | mailbox.ready = 1        |
  |                              |------------------------->|
  |                              |        mailbox.ack = 1   |
  |                              |<-------------------------|
  |  returns delivered=1         |                          |
  |<-----------------------------|                          |
```

### Limits

| Parameter | Value |
|-----------|-------|
| Max topics | 8 |
| Max subscribers per topic | 4 |
| Max message length | 60 bytes |
| Topic name length | 16 bytes |
| Publish ack timeout | 5 seconds |

### Shell Commands

| Command | Description |
|---------|-------------|
| `msg send <topic> <data>` | Publish a message to all subscribers |
| `msg list` | Show all topics and their subscribers |
| `msg subscribe <topic> <idx>` | Subscribe a component to a topic |

### Built-in Listener Service

The `listener` component subscribes to the `events` topic on startup and prints all received messages. Start it with `component run listener`, then publish with `msg send events <message>`.

### UART Yield Fix

On x86-64, `uart_getc()` previously busy-looped polling the LSR register. This starved all background tasks (echo, listener, counter) of CPU time. The fix adds `yield()` in the polling loop, allowing the scheduler to round-robin between the shell and component tasks.

### Key Files

| File | Purpose |
|------|---------|
| `runtime/src/msg_router.rs` | Router implementation in Rust (subscribe, publish, receive, ack, list) |
| `kernel/src/component_runtime.c` | Listener service entry point |
| `kernel/src/shell_component.c` | `cmd_msg` shell handler |

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

The `test_x86_boot.c` test suite contains 90 tests across 17 categories:

| Category | Tests | Description |
|----------|-------|-------------|
| Control registers | 4 | CR0 paging, CR4 PAE, EFER long mode, CR3→PML4 |
| Page tables | 6 | PML4, PDPT, PD structure, 4GB boot mapping, PDPT[0..3], VMM RAM detection |
| GDT | 3 | Limit, CS selector (0x08), DS selector (0x10) |
| Memory layout | 3 | Kernel at 1MB, section ordering, within mapping |
| Multiboot2 | 5 | Pointer valid, structure size, memory map, usable RAM, bootloader name |
| IDT | 4 | IDTR loaded, exception entries present, IRQ interrupt gates, exception trap gates |
| Legacy PIC | 3 | OCW3 response, timer unmasked, slave accessible |
| Timer | 3 | IF flag set, ticks incrementing, ~100 Hz rate |
| ACPI + APIC | 6 | CPU count, LAPIC/IOAPIC addresses, LAPIC initialized, EOI safe, timer running |
| SMP | 11 | CPU count, all online, BSP cpu_id, unique APIC IDs, AP stacks, LAPIC ID match, cpu_logical_id found/not-found, logical map, spinlock mutual exclusion, param offsets |
| NVIDIA GPU | 7 | Init ran, no-crash, VRAM test -1 without GPU, accessors safe, BOOT_42 decode, gpu/pci shell commands registered |
| Component runtime | 6 | Run counter, invalid name rejected, list builtins safe, run increases count, shell command registered, ELF x86-64 arch |
| Message router (C tests) | 18 | Init, subscribe (single/multiple/multi-topic/overflow), receive (empty/unsubscribed/null), publish (none/timeout), ack no-pending, reinit, shell commands (list/subscribe/send) |
| Message router (Rust tests) | 10 | Init+subscribe, multi-subscribe, subscriber overflow, topic overflow, receive empty/unsubscribed, publish nonexistent, reinit clears, ack no-pending, list safe |
| Component services | 2 | Listener starts + registers, echo start + send safe |
| PCI | 11 | Host bridge exists, nonexistent 0xFFFF, enumeration count, host/ISA bridge found, device at index, config read8/16, find by ID, find not found, multi-function |
| Platform abstraction | 9 | cpu_context offset/fields/size, platform defines, irq_save/restore, spinlock roundtrip, gic enable/disable, timer frequency/count |
| Scheduler integration | 5 | gic_init loads IDT, task stack, gic_end_interrupt, uart_putc, scheduler_tick |
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
| `spinlock.h` | x86-64 barriers (mfence/lfence/sfence), irq_save (pushfq+cli), spin_lock (TTAS atomic), ticket_lock (lock xaddw) |
| `cache.h` | No-ops (x86-64 fully hardware cache coherent) |
| `smp.h` | `cpu_id()` reads LAPIC ID, looks up in cpu_logical_map[] |

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
| `gic.h` (gic_init, gic_enable_irq, gic_end_interrupt) | `kernel/arch/x86_64/pic.c` — LAPIC + IOAPIC |
| `timer.h` (timer_init, timer_start, timer_handler) | `kernel/arch/x86_64/timer_x86.c` — LAPIC timer |
| `switch_to()` (context.S) | `kernel/arch/x86_64/context.S` — x86-64 registers |
| `smp_init`, `vmm_init`, DTB/Rust stubs | `kernel/arch/x86_64/platform_x86.c` |
| PCI config access, enumeration, shell | `kernel/arch/x86_64/pci.c` |
| ACPI table discovery | `kernel/arch/x86_64/acpi.c` |

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
| `kernel/arch/x86_64/ap_trampoline.S` | AP startup: real mode → protected → long mode |
| `kernel/arch/x86_64/setjmp.S` | `setjmp`/`longjmp` for Lua error handling |

### C Code — x86-64 Platform Layer

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/main_x86.c` | Standalone test kernel entry (not used in integrated build) |
| `kernel/arch/x86_64/idt.c` | IDT setup, exception handler, IRQ dispatch |
| `kernel/arch/x86_64/acpi.c` | ACPI RSDP/MADT parsing (CPU discovery) |
| `kernel/arch/x86_64/lapic.c` | Local APIC driver (init, EOI, IPI, timer) |
| `kernel/arch/x86_64/ioapic.c` | I/O APIC driver (redirection table) |
| `kernel/arch/x86_64/pic.c` | gic.h interface routing to LAPIC/IOAPIC |
| `kernel/arch/x86_64/timer_x86.c` | LAPIC timer (timer.h interface, calibrated vs PIT) |
| `kernel/arch/x86_64/platform_x86.c` | Boot glue, SMP boot (INIT-SIPI), VMM/DTB/Rust stubs |
| `kernel/arch/x86_64/pci.c` | PCI config access, bus enumeration, `pci` shell command |
| `kernel/arch/x86_64/nvidia_gpu.c` | GPU probe, BAR mapping, register decode, VRAM test, `gpu` command |
| `kernel/src/component_runtime.c` | Built-in component execution, `component run/send/builtins` |
| `runtime/src/msg_router.rs` | Topic-based pub/sub message router (Rust, replaces C version) |
| `kernel/drivers/uart_x86.c` | 16550 UART driver (uart.h interface, yield-based getc) |

### Build System

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/Makefile.test` | Standalone + integrated build rules |
| `kernel/kernel-x86_64.ld` | Linker script |

### Documentation

| File | Purpose |
|------|---------|
| `docs/x86-64-port.md` | This document — architecture, build, deploy, test |
| `docs/nvidia-gsp.md` | GSP firmware research: boot sequence, registers, nouveau source map |

### Tests

| File | Purpose |
|------|---------|
| `kernel/tests/test_x86_boot.c` | 90 tests: boot, IDT, APIC, SMP, spinlock, GPU, PCI, component, ELF, Multiboot2, platform, scheduler, setjmp |

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
3. **Boot speed**: Identity mapping 4GB requires only 2048 PD entries (4 pages)

### Why LAPIC Timer Instead of PIT?

The LAPIC timer is per-CPU (essential for SMP) and higher frequency (~1 GHz vs PIT's 1.19 MHz). The PIT is still used once at boot to calibrate the LAPIC timer frequency, then disabled.

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

- QEMU's `-kernel` flag does not support Multiboot2 on Ubuntu 24.04 (QEMU 8.2.2). Use GRUB ISO boot (`-cdrom`) instead.
- NVIDIA GPU engine registers return 0xBADF5040 — this is expected (GSP firmware not loaded). See `docs/nvidia-gsp.md`.
- **x86-64 scheduler reentrance bug (FIXED):** Timer IRQ between `rq_unlock_irqrestore` and `switch_to` caused reentrant `schedule()` deadlock. Fixed with per-CPU `preempt_disabled` flag in `sched.c` — cross-platform solution that works for both ARM64 and x86-64. Multi-task IPC (echo + listener + shell) verified working. See `docs/x86-64-scheduler-investigation.md` for full investigation log.
- **Fixed (April 2026):** SMP timer tick rate was 8× too fast (all 8 CPUs incrementing `pit_ticks`). Now only BSP increments. `sleep_ms` accuracy verified: `sleep 3000` → 3020ms.
- No higher-half kernel mapping — identity mapping only.
- No networking on x86-64 — requires VirtIO-PCI transport (not MMIO).

---

## Recommended Next Steps

### Completed

1. ~~Rust runtime port~~ ✅ — `x86_64-unknown-none` target, real library linked via `--whole-archive`
2. ~~Benchmarks~~ ✅ — RDTSC-based nanosecond timing, context switch 104 ns, IPC 113 ns
3. ~~arch.h~~ ✅ — Architecture-agnostic IRQ control, halt, barriers
4. ~~CI pipeline~~ ✅ — x86-64 build + QEMU boot test in GitHub Actions
5. ~~Scheduler reentrance fix~~ ✅ — per-CPU `preempt_disabled` flag, cross-platform
6. ~~Echo IPC~~ ✅ — Shared mailbox with atomic ops, round-robin scheduling
7. ~~M7 MessageRouter~~ ✅ — Topic-based pub/sub in Rust, listener service, shell commands, 30 tests

### Remaining (Post-Capstone)

5. **Higher-half kernel mapping** — Move kernel to 0xFFFFFFFF80000000
6. **VirtIO-PCI networking** — x86-64 QEMU uses VirtIO-PCI (not MMIO), requires new transport driver
7. **GSP firmware loading** — See `docs/nvidia-gsp.md`
8. **IOMMU / VT-d** — DMA protection for PCI devices

---

## Architecture Comparison: ARM64 vs x86-64

### Feature Parity Matrix

| Feature | ARM64 (Pi 5) | ARM64 (QEMU) | x86-64 (QEMU) | x86-64 (i7-6700) |
|---------|-------------|--------------|----------------|-------------------|
| Boot | ✅ SD card | ✅ `-kernel` | ✅ GRUB ISO | ✅ SD via SDWire |
| SMP | ✅ 4 cores | ✅ 4 cores | ✅ 4 cores | ✅ 8 CPUs (4c×2HT) |
| Preemptive scheduler | ✅ | ✅ | ✅ | ✅ |
| Shell (30+ commands) | ✅ | ✅ | ✅ | ✅ |
| PMM buddy allocator | ✅ | ✅ | ✅ | ✅ (20 GB) |
| VFS + LittleFS | ✅ | ✅ | ✅ | ✅ |
| Lua 5.4 scripting | ✅ | ✅ | ✅ | ✅ |
| Rust runtime | ✅ | ✅ | ✅ | ✅ |
| IPC message queues | ✅ | ✅ | ✅ | ✅ |
| Component system | ✅ | ✅ | ✅ | ✅ |
| Message router (pub/sub) | ✅ | ✅ | ✅ | N/A |
| Echo IPC (mailbox) | ✅ | ✅ | ✅ | ✅ |
| PCI enumeration | N/A | N/A | ✅ (6 devices) | ✅ (21 devices) |
| NVIDIA GPU probe | N/A | N/A | N/A | ✅ (GA107) |
| VRAM access | N/A | N/A | N/A | ✅ (256 MB) |
| Networking (lwIP) | ❌ | ✅ | ❌ | ❌ |
| GPU compute | ❌ | ❌ | ❌ | ❌ (GSP required) |

### Performance Comparison

| Metric | ARM64 (Pi 5, 2.4 GHz) | x86-64 (i7-6700, 3.4 GHz) |
|--------|----------------------|---------------------------|
| Context switch | ~1-5 µs (estimated) | **104 ns** |
| IPC round-trip | ~1-5 µs (estimated) | **113 ns** |
| Timer jitter | ~10 µs | **116 ns** |
| RAM usable | 3,840 MB | **19,916 MB** |
| CPUs online | 4 | **8** |
| PCI devices | N/A | **21** |

### Porting Effort Summary

| Component | Lines Changed | Effort | Notes |
|-----------|--------------|--------|-------|
| Boot assembly | ~500 | New | trampoline32.S, entry64.S, ap_trampoline.S |
| IDT + ISR stubs | ~400 | New | idt.c, idt.S (replaces vectors.S) |
| LAPIC + IOAPIC | ~530 | New | lapic.c, ioapic.c, pic.c (replaces gic.c) |
| LAPIC timer | ~140 | New | timer_x86.c (replaces timer.c) |
| ACPI parsing | ~400 | New | acpi.c (replaces DTB parsing) |
| Context switch | ~110 | New | context.S (different register set) |
| PCI enumeration | ~490 | New | No ARM64 equivalent |
| NVIDIA GPU | ~380 | New | No ARM64 equivalent |
| Platform stubs | ~650 | New | platform_x86.c, SMP, VMM stubs |
| Shared code | 0 | Unchanged | Scheduler, IPC, shell, VFS, Lua, PMM |
| **Total new x86-64** | **~3,600** | | |

---

## Jetson GPU Bring-Up Checklist

Based on x86-64 GPU learnings, these steps apply to Jetson Orin Nano (GA10B, integrated Ampere GPU):

### What Transfers Directly

- [x] GPU register map: NV_PMC_BOOT_0 (0x000), BOOT_42 (0xA00), PMC_ENABLE (0x200)
- [x] BOOT_42 decode: architecture field (0x17 = Ampere), implementation, chip_id
- [x] 0xBADF5040 meaning: engine not initialized, GSP required
- [x] GSP boot sequence: 7-phase chain documented in `docs/nvidia-gsp.md`
- [x] Nouveau source file map for GSP implementation
- [x] VRAM write/read verification pattern

### Jetson-Specific Differences

- [ ] GPU is memory-mapped (no PCIe BAR — integrated GPU at fixed MMIO address)
- [ ] GPU address for Jetson: check device tree for `gpu@` node
- [ ] CBB firewall may block GPU MMIO access (same issue as UART)
- [ ] GSP firmware may be pre-loaded by CBoot/UEFI bootloader
- [ ] Check if GPU engines are already initialized (PMC_ENABLE != 0)
- [ ] TCU serial path may interfere with GPU register access

### Recommended Approach

1. From EL2 (VHE mode), read GPU MMIO registers (NV_PMC_BOOT_0, BOOT_42)
2. If BOOT_42 reads correctly → CBB allows GPU access from EL2
3. Check PMC_ENABLE — if non-zero, some engines are already initialized
4. Read PTIMER to verify register path works
5. If 0xBADF5040 on engine registers → GSP not loaded, same barrier as x86-64
6. If CBB blocks GPU → same barrier as UARTA, documented in `docs/jetson-nvidia-support.md`

---

## References

### Architecture

- [AMD64 Architecture Programmer's Manual](https://developer.amd.com/resources/developer-guides-manuals/)
- [Intel 64 and IA-32 Architectures Software Developer's Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)

### OSDev

- [OSDev Wiki - Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)
- [OSDev Wiki - APIC](https://wiki.osdev.org/APIC)
- [OSDev Wiki - IOAPIC](https://wiki.osdev.org/IOAPIC)
- [OSDev Wiki - PCI Express](https://wiki.osdev.org/PCI_Express)
- [OSDev Wiki - SMP](https://wiki.osdev.org/SMP)

### NVIDIA GPU

- [NVIDIA open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules) — Register definitions, GSP boot reference
- [envytools](https://github.com/envytools/envytools) — Community GPU documentation (PMC, BARs, MMIO map)
- [Linux nouveau driver](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/nouveau) — GSP boot implementation

---

*Last updated: April 2026*
