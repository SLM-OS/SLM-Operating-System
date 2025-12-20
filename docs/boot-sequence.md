# SLM-OS Boot Sequence Research

This document contains research notes on boot sequences and ARM64 architecture relevant to SLM-OS development.

---

## Table of Contents

1. [Jetson Orin Nano Boot Sequence](#jetson-orin-nano-boot-sequence)
2. [ARM64 Exception Levels](#arm64-exception-levels)
3. [QEMU virt Machine Boot](#qemu-virt-machine-boot)
4. [SLM-OS Boot Design](#slm-os-boot-design)
5. [Device Tree Requirements](#device-tree-requirements)

---

## Jetson Orin Nano Boot Sequence

### Overview

The Jetson Orin Nano uses NVIDIA's boot architecture with UEFI as the CPU bootloader.

### Boot Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Power On / Reset                           │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  BootROM (BR)                                                       │
│  - Hard-wired into SoC                                              │
│  - Runs on BPMP (Boot and Power Management Processor)               │
│  - Initializes boot media                                           │
│  - Loads BR-BCT, PSCBL1, MB1, MB1-BCT from storage                  │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  PSC ROM (Platform Security Controller)                             │
│  - Hardware component in SoC                                        │
│  - Holds keys for NVIDIA and OEM authentication                     │
│  - Provides authentication and decryption services to BootROM       │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Microboot1 (MB1)                                                   │
│  - Runs on BPMP (R5 core)                                           │
│  - First boot software loaded by BootROM                            │
│  - Initializes CPU and SoC components                               │
│  - Performs security configuration                                  │
│  - Signed and encrypted with NVIDIA key                             │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Microboot2 (MB2)                                                   │
│  - Runs on BPMP (R5 core)                                           │
│  - Detects Jetson device type                                       │
│  - Fetches device information                                       │
│  - Prepares for UEFI handoff                                        │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  UEFI (Unified Extensible Firmware Interface)                       │
│  - Runs on CPU (Cortex-A78AE cores)                                 │
│  - Replaces legacy CBoot                                            │
│  - Initializes remaining hardware                                   │
│  - Provides boot device selection                                   │
│  - Loads and executes OS kernel                                     │
│  - Supports Secure Boot                                             │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Operating System (SLM-OS)                                          │
│  - Kernel entry point                                               │
│  - Running at EL1                                                   │
└─────────────────────────────────────────────────────────────────────┘
```

### Boot Stage Summary

| Stage | Processor | Location | Purpose |
|-------|-----------|----------|---------|
| BootROM | BPMP | SoC (hard-wired) | Initial boot, load MB1 |
| PSC ROM | PSC | SoC | Security, authentication |
| MB1 | BPMP (R5) | QSPI flash | SoC init, CPU bring-up |
| MB2 | BPMP (R5) | QSPI flash | Device detection |
| UEFI | CPU (A78AE) | QSPI flash | Load OS kernel |
| OS | CPU (A78AE) | RAM | SLM-OS code runs here |

### UEFI Details

- UEFI sources available at: https://github.com/NVIDIA/edk2-nvidia
- Stored in A/B cpu-bootloader partitions in QSPI
- L4TLauncher is compiled into UEFI bootloader
- Supports SMBIOS and ACPI for generic OS loading

### Boot Order Customization

Boot order can be changed via:
1. UEFI menu (press ESCAPE at boot prompt)
2. DTBO overlay during flash (e.g., `BootOrderNvme.dtbo`)

### Resources

- [Jetson Orin Series Boot Flow](https://docs.nvidia.com/jetson/archives/r35.4.1/DeveloperGuide/text/AR/BootArchitecture/JetsonOrinSeriesBootFlow.html)
- [UEFI Adaptation Guide](https://docs.nvidia.com/jetson/archives/r35.5.0/DeveloperGuide/SD/Bootloader/UEFI.html)
- [NVIDIA UEFI Source (edk2-nvidia)](https://github.com/NVIDIA/edk2-nvidia/wiki)

---

## ARM64 Exception Levels

### Overview

ARMv8-A defines four Exception Levels (EL0-EL3) with increasing privilege. Exception levels control access to system resources and determine what operations code can perform.

### Exception Level Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│  EL3 - Secure Monitor                                    (Highest)  │
│  - Only level that can switch security states                       │
│  - Typically ARM Trusted Firmware (ATF)                             │
│  - Optional                                                         │
├─────────────────────────────────────────────────────────────────────┤
│  EL2 - Hypervisor                                                   │
│  - Virtualization support                                           │
│  - KVM, VMware, VirtualBox                                          │
│  - Optional                                                         │
├─────────────────────────────────────────────────────────────────────┤
│  EL1 - Operating System Kernel                     ◄── SLM-OS HERE  │
│  - Privileged OS code                                               │
│  - Full hardware access                                             │
│  - Mandatory                                                        │
├─────────────────────────────────────────────────────────────────────┤
│  EL0 - User Applications                                 (Lowest)   │
│  - Unprivileged code                                                │
│  - Restricted access                                                │
│  - Mandatory                                                        │
└─────────────────────────────────────────────────────────────────────┘
```

### Exception Level Details

| Level | Name | Privilege | Typical Use | Required |
|-------|------|-----------|-------------|----------|
| EL0 | User | Lowest | Applications, userspace | Mandatory |
| EL1 | Kernel | Privileged | **OS kernel (SLM-OS)** | Mandatory |
| EL2 | Hypervisor | Higher | Virtualization | Optional |
| EL3 | Secure Monitor | Highest | TrustZone, security | Optional |

### Transitioning Between Levels

**Moving to higher levels (EL0 → EL1 → EL2 → EL3):**
- Only possible via exceptions
- Triggered by: interrupts, system calls, faults

**Moving to lower levels (EL3 → EL2 → EL1 → EL0):**
- Only possible via exception return (`eret` instruction)

### System Call Instructions

| Instruction | From | To | Purpose |
|-------------|------|-----|---------|
| `SVC` | EL0 | EL1 | Supervisor Call (syscall) |
| `HVC` | EL1 | EL2 | Hypervisor Call |
| `SMC` | EL1/EL2 | EL3 | Secure Monitor Call |

### Exception Level Registers

Each exception level has its own banked system registers:

| Register | Purpose |
|----------|---------|
| `SP_ELx` | Stack pointer for ELx |
| `ELR_ELx` | Exception Link Register (return address) |
| `SPSR_ELx` | Saved Program Status Register |
| `VBAR_ELx` | Vector Base Address Register |
| `SCTLR_ELx` | System Control Register |

### Determining Current Exception Level

Read the `CurrentEL` system register:

```asm
mrs x0, CurrentEL       // Read current EL
lsr x0, x0, #2          // Shift to get EL number (0-3)
```

Values:
- `0b0000` (0) = EL0
- `0b0100` (4) = EL1
- `0b1000` (8) = EL2
- `0b1100` (12) = EL3

### Security States

ARM64 supports two security states:
- **Non-Secure (NS)**: Normal world
- **Secure**: TrustZone protected

Only EL3 can switch between security states.

### Resources

- [ARM Learn the Architecture - A-profile](https://www.arm.com/architecture/learn-the-architecture/a-profile)
- [AArch64 Exception Model](https://developer.arm.com/documentation/102412/latest/Privilege-and-Exception-levels/Exception-levels)
- [Exception Model PDF](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Exception%20model.pdf)
- [AArch64 Exception Levels (Medium)](https://medium.com/@om.nara/aarch64-exception-levels-60d3a74280e6)
- [Mike's AArch64 Exception Levels](https://krinkinmu.github.io/2021/01/04/aarch64-exception-levels.html)
- [Pyjama Brah's Exception Levels](https://pyjamabrah.com/posts/arm64-day0-exception-levels/)
- [Linux Kernel AArch64 Booting](https://www.kernel.org/doc/html/v6.4/arm64/booting.html)

---

## QEMU virt Machine Boot

### Overview

For development, we use QEMU's `virt` machine which simplifies the boot process.

### QEMU Boot Sequence

```
┌─────────────────────────────────────────────────────────────────────┐
│  QEMU Initialization                                                │
│  - Sets up virtual hardware                                         │
│  - Initializes CPU at specified exception level                     │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Kernel Load (-kernel option)                                       │
│  - Loads ELF/binary to RAM at 0x40000000                            │
│  - Sets PC to entry point                                           │
│  - No bootloader required                                           │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Kernel Entry                                                       │
│  - Running at EL1 (default)                                         │
│  - MMU disabled                                                     │
│  - Caches disabled                                                  │
└─────────────────────────────────────────────────────────────────────┘
```

### Initial CPU State (QEMU virt)

When QEMU loads the kernel with `-kernel`:

| Register/State | Value |
|----------------|-------|
| Exception Level | EL1 (typically) |
| MMU | Disabled |
| D-Cache | Disabled |
| I-Cache | Disabled |
| PC | Entry point from ELF |
| SP | Undefined (must be set by boot code) |
| x0 | DTB address (if provided) |

### QEMU vs Real Hardware

| Aspect | QEMU virt | Jetson Orin Nano |
|--------|-----------|------------------|
| Boot stages | Direct kernel load | BootROM → MB1 → MB2 → UEFI |
| Initial EL | EL1 | EL1 (after UEFI) |
| Hardware init | Minimal needed | UEFI handles it |
| DTB | Optional via -dtb | Required |

---

## SLM-OS Boot Design

### Design Decisions

Based on TODO.md recommendations:

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Initial Target | QEMU first | Faster iteration, defer Jetson to Month 2-3 |
| Exception Level | EL1 | Standard for OS kernels, simpler than EL2 |
| Boot Method | QEMU direct load | No bootloader needed initially |

### Boot Requirements

**Minimum for QEMU:**
1. Entry point in `boot.S`
2. Stack pointer setup
3. BSS section cleared
4. Jump to C `kernel_main()`

**Additional for Jetson (later):**
1. UEFI-compatible kernel image
2. Device tree handling
3. Hardware initialization beyond UEFI

### Entry Point Checklist

Our `boot.S` implements:

- [x] Be placed at correct address (0x40000000 for QEMU)
- [x] Set up stack pointer
- [x] Clear BSS section
- [x] Disable interrupts initially
- [x] Call `kernel_main()`
- [x] Handle case where `kernel_main()` returns (hang or reset)

### Memory Layout (QEMU virt)

```
0x00000000 - 0x07FFFFFF : Flash (128 MB)
0x08000000 - 0x08FFFFFF : GIC (16 MB)
0x09000000 - 0x09000FFF : UART (PL011)
0x09010000 - 0x09010FFF : RTC
0x0A000000 - 0x0AFFFFFF : Virtio devices (16 MB)
0x40000000 - ...        : RAM (kernel loaded here)
```

---

## Device Tree Requirements

### What is a Device Tree?

A device tree is a data structure that describes hardware to the operating system. On ARM systems, hardware is not self-discoverable like x86 (which uses ACPI and PCI enumeration), so the bootloader passes a device tree blob (DTB) to inform the kernel about available hardware.

**Device tree typically contains:**
- Memory regions (base address, size)
- CPU cores and their properties
- Peripheral addresses (UART, timers, interrupt controller)
- Interrupt mappings
- Clock configurations

### QEMU virt DTB Behavior

When using QEMU with `-kernel`, QEMU can automatically generate and pass a DTB:
- DTB address is provided in register `x0` at kernel entry
- Contains description of the virtual hardware configuration
- Can be overridden with `-dtb <file>` option

### Month 1 Decision: Hardcoded Configuration

> **NOTE:** This is an initial simplification. DTB parsing will be implemented when adding
> Jetson Orin Nano support (Month 2-3).

For Month 1 QEMU-only development, SLM-OS uses hardcoded hardware addresses instead of parsing the device tree. This decision:

**Rationale:**
- Reduces initial complexity
- QEMU virt machine has well-documented, fixed addresses
- Allows focus on core kernel functionality first
- DTB parsing adds significant code for minimal Month 1 benefit

**Implementation approach:**

All hardware addresses are centralized in a single header file (`kernel/include/platform.h`):

```c
// platform.h - Hardware configuration
// NOTE: Replace hardcoded values with DTB parsing (Month 2-3)

#define PLATFORM_QEMU_VIRT  1

#if PLATFORM_QEMU_VIRT
    #define UART_BASE       0x09000000
    #define RAM_BASE        0x40000000
    #define RAM_SIZE        0x08000000  // 128 MB default
    #define GIC_BASE        0x08000000
    #define TIMER_IRQ       30
#endif
```

**Why this approach works:**
- No magic numbers scattered throughout the codebase
- Single file to update when adding new platforms
- Minimal refactoring required when DTB parsing is added (macros become variables)
- Compile-time platform selection via `#if` / `#elif`

**Future transition to DTB:**

When DTB parsing is implemented:
1. Parse DTB at early boot to extract hardware addresses
2. Populate global variables with discovered values
3. Change `platform.h` from compile-time macros to `extern` declarations
4. Remove the NOTE comment from this section and `platform.h`

### QEMU virt Hardcoded Values

These addresses are used for Month 1 development:

| Resource | Address | Size | Notes |
|----------|---------|------|-------|
| Flash | 0x00000000 | 128 MB | Not used initially |
| GIC | 0x08000000 | 16 MB | Interrupt controller |
| UART (PL011) | 0x09000000 | 4 KB | Serial console |
| RTC | 0x09010000 | 4 KB | Real-time clock |
| Virtio | 0x0A000000 | 16 MB | Virtual I/O devices |
| RAM | 0x40000000 | 128 MB+ | Kernel and data |

---

## Next Steps

1. Write `boot.S` with entry point and stack setup
2. Create `kernel.ld` linker script
3. Implement minimal `kernel_main()` that prints to UART
4. Test in QEMU

---

*Last updated: December 2025*
