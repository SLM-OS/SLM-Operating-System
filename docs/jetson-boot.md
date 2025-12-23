# Jetson Orin Nano Boot Process

Boot documentation for SLM-OS on NVIDIA Jetson Orin Nano.

**Status:** Research complete; hardware bring-up pending

---

## Boot Architecture Overview

The Jetson Orin Nano uses a multi-stage boot process. Unlike older Jetson models that used U-Boot, the Orin series uses **UEFI** as the CPU bootloader.

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Jetson Orin Nano Boot Flow                      │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐         │
│   │ BootROM │───►│   MB1   │───►│   MB2   │───►│  UEFI   │         │
│   │  (BR)   │    │ (BPMP)  │    │(CCPLEX) │    │         │         │
│   └─────────┘    └─────────┘    └─────────┘    └────┬────┘         │
│        │              │              │              │               │
│        │              │              │              ▼               │
│   QSPI Flash     Init SDRAM      Flash ops    ┌─────────┐          │
│   (on module)    Init clocks     Cold boot    │ Kernel  │          │
│                  Security                     │(SLM-OS) │          │
│                                               └─────────┘          │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Boot Stages

| Stage | Processor | Storage | Function |
|-------|-----------|---------|----------|
| **BootROM** | BPMP | Hard-wired | Load MB1 from QSPI, hardware init |
| **MB1** | BPMP | QSPI | SDRAM init, security config, load MB2 |
| **MB2** | CCPLEX (CPU) | QSPI | Flashing support, cold-boot setup |
| **UEFI** | CCPLEX | QSPI + rootfs | Load kernel via extlinux.conf |

**Important:** The bootloader stages (BootROM → MB1 → MB2 → UEFI) are stored in **QSPI flash** on the Jetson module itself. The kernel and DTB are loaded from the **rootfs** (SD card, eMMC, NVMe, or USB).

---

## UEFI Boot Configuration

UEFI reads `/boot/extlinux/extlinux.conf` from the rootfs to determine which kernel to load.

### extlinux.conf Format

```
TIMEOUT 30
DEFAULT primary

MENU TITLE Jetson Orin Nano Boot Options

LABEL primary
    MENU LABEL SLM-OS
    LINUX /boot/slmos.elf
    FDT /boot/tegra234-p3768-0000+p3767-0000.dtb
    APPEND console=ttyTCU0,115200n8
```

| Directive | Description |
|-----------|-------------|
| `TIMEOUT` | Seconds to wait before booting default entry |
| `DEFAULT` | Label of default boot entry |
| `LABEL` | Name of boot entry |
| `LINUX` | Path to kernel image (relative to rootfs root) |
| `FDT` | Path to device tree blob |
| `APPEND` | Kernel command line arguments |

### DTB Selection Priority

UEFI selects the Device Tree Blob in this order:
1. `FDT` tag in extlinux.conf
2. kernel-dtb partition (A/B scheme)
3. DTB used by UEFI itself

For SLM-OS, explicitly specify the FDT to ensure the correct DTB is used.

---

## Kernel Image Format

UEFI expects an **ARM64 Linux kernel Image** format (not raw ELF, not compressed).

The kernel Image is a self-decompressing binary with a specific header that UEFI recognizes. For SLM-OS, we have two options:

### Option 1: Use objcopy to Create Image

```bash
aarch64-none-elf-objcopy -O binary slmos.elf slmos.bin
# Then add ARM64 Image header (64 bytes)
```

### Option 2: EFI Stub (Future)

Build SLM-OS as an EFI application that UEFI can load directly. This is more complex but cleaner.

### Option 3: Chainload from Linux (Fallback)

Boot Linux normally, then use kexec to jump to SLM-OS. This is useful for early development.

**Current approach:** Start with Option 3 for debugging, then implement Option 1.

---

## Serial Console

### Hardware Connection

The Jetson Orin Nano has UART available on the 40-pin GPIO header:

| Pin | Function | Connect to |
|-----|----------|------------|
| 6 | GND | Adapter GND |
| 8 | UART1_TX | Adapter RX |
| 10 | UART1_RX | Adapter TX |

**Important:** Use a 3.3V USB-TTL adapter. 5V will damage the Jetson.

### Serial Settings

- Baud rate: 115200
- Data bits: 8
- Parity: None
- Stop bits: 1
- Flow control: None

### Console Device Names

| Device | Description |
|--------|-------------|
| `ttyTCU0` | Tegra Combined UART (debug console via USB-C when Linux runs) |
| `ttyTHS0` | UART1 on GPIO header (Pin 8/10) — use this for bare-metal |
| `ttyTHS1` | UART2 (if enabled) |

For SLM-OS bare-metal, we use **UART1 (ttyTHS0)** at address `0x03100000`.

---

## Memory Map

### Tegra234 Physical Memory Layout

| Region | Start | End | Size | Description |
|--------|-------|-----|------|-------------|
| SYSRAM | 0x00000000 | 0x0003FFFF | 256 KB | System RAM (bootloader use) |
| Peripherals | 0x02000000 | 0x0FFFFFFF | ~224 MB | MMIO devices |
| DRAM | 0x80000000 | varies | 4-8 GB | Main memory |

### Key Peripheral Addresses

| Peripheral | Base Address | Size | Notes |
|------------|--------------|------|-------|
| UARTA | 0x03100000 | 64 KB | GPIO header UART |
| UARTE | 0x03140000 | 64 KB | Additional UART |
| GICv2 Distributor | 0x0F400000 | 64 KB | Interrupt controller |
| GICv2 CPU Interface | 0x0F440000 | 64 KB | Per-CPU interrupt interface |
| ARM Timer | System register | N/A | Generic Timer (CNTPCT_EL0) |

**Note:** These addresses are from device tree and may need verification on hardware.

---

## GIC Configuration

Jetson Orin Nano uses ARM GICv2 (or GICv3 in v2 compat mode):

- **Distributor:** 0x0F400000 (vs 0x08000000 on QEMU)
- **CPU Interface:** 0x0F440000 (vs 0x08010000 on QEMU)

The SPI (Shared Peripheral Interrupt) numbers may also differ from QEMU.

---

## Differences from QEMU virt

| Feature | QEMU virt | Jetson Orin Nano |
|---------|-----------|------------------|
| UART | PL011 @ 0x09000000 | NS16550 @ 0x03100000 |
| GIC Dist | 0x08000000 | 0x0F400000 |
| GIC CPU | 0x08010000 | 0x0F440000 |
| RAM Base | 0x40000000 | 0x80000000 |
| RAM Size | 128 MB (configurable) | 4-8 GB |
| CPU Count | 4 (configurable) | 6 (Cortex-A78AE) |
| Bootloader | Direct load | UEFI → extlinux.conf |
| Timer | Virtual | Physical (system register) |

---

## Boot Checklist

### Prerequisites

- [ ] USB-TTL serial adapter (3.3V)
- [ ] SD card with JetPack image (for initial testing)
- [ ] Serial terminal software (PuTTY, minicom, etc.)

### First Boot Steps

1. [ ] Flash JetPack to SD card (establishes QSPI bootloader)
2. [ ] Connect serial adapter to GPIO pins 6, 8, 10
3. [ ] Boot Jetson with serial console open
4. [ ] Verify Linux boots and serial output works
5. [ ] Modify extlinux.conf to add SLM-OS entry
6. [ ] Copy slmos.bin to /boot/
7. [ ] Reboot and select SLM-OS from menu
8. [ ] Debug via serial console

### Fallback: kexec Method

If direct UEFI boot fails:

```bash
# On running Linux:
sudo kexec -l slmos.bin --append="console=ttyTHS0,115200"
sudo kexec -e
```

---

## Troubleshooting

### No Serial Output

1. Check TX/RX connections (they should be crossed)
2. Verify 3.3V adapter (not 5V)
3. Check baud rate (115200)
4. Try different UART (UARTA vs UARTE)

### UEFI Drops to Shell

- extlinux.conf not found or malformed
- Check path: `/boot/extlinux/extlinux.conf`
- Verify SD card is first in boot order

### Kernel Crashes Immediately

- Wrong kernel format (needs Image header, not raw ELF)
- Wrong load address
- DTB mismatch
- UART driver issue (no output visible)

### Memory Access Faults

- RAM base address wrong (0x80000000, not 0x40000000)
- Peripheral address wrong
- MMU page table issue

---

## References

- [Jetson Orin Series Boot Flow](https://docs.nvidia.com/jetson/archives/r35.4.1/DeveloperGuide/text/AR/BootArchitecture/JetsonOrinSeriesBootFlow.html)
- [UEFI Adaptation Guide](https://docs.nvidia.com/jetson/archives/r36.2/DeveloperGuide/SD/Bootloader/UEFI.html)
- [Tegra234 Device Tree (Linux kernel)](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/nvidia/tegra234.dtsi)
- [edk2-nvidia Wiki](https://github.com/NVIDIA/edk2-nvidia/wiki)

---

*Created: December 2025*
*Last updated: December 2025*
