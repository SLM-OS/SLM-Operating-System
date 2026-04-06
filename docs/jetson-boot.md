# Jetson Orin Nano Boot Process

Boot documentation for SLM-OS on NVIDIA Jetson Orin Nano.

**Status:** Working (April 2026) — SLM-OS boots via kexec at EL2 with VHE. See `docs/jetson-el2-bringup.md`.

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

SLM-OS uses **UARTC** (0x0C280000) for serial output, routed through the TCU to the **USB-C debug port**. No external adapter needed — connect a USB-C cable to the Jetson's debug port and use `labctl serial-capture jetson-nano-2`.

The 40-pin GPIO header UART (UARTA at 0x03100000) is **blocked by the CBB firewall** even at EL2.

### Serial Settings

- Baud rate: 115200 (firmware-configured, preserved by raw mode)
- Data bits: 8
- Parity: None
- Stop bits: 1
- Flow control: None

### Console Device Names

| Device | Address | Description |
|--------|---------|-------------|
| `ttyTCU0` | Via HSP | Tegra Combined UART (USB-C debug) — **used by SLM-OS** |
| `ttyTHS0` | 0x03100000 | UARTA on GPIO header (Pin 8/10) — blocked by CBB at EL2 |

**SLM-OS serial path (April 2026):**
- **TX:** Direct write to UARTC THR at 0x0C280000 → SPE routes through TCU → USB-C debug port
- **RX:** SPE reads USB-C input → writes to TCU HSP mailbox at 0x03C10000 → SLM-OS polls mailbox

The SPE firmware continues running after kexec and handles the TCU multiplexing. See `docs/jetson-tcu.md` for TCU architecture details and `docs/jetson-el2-bringup.md` for the implementation.

---

## Memory Map

### Tegra234 Physical Memory Layout

| Region | Start | End | Size | Description |
|--------|-------|-----|------|-------------|
| SYSRAM | 0x00000000 | 0x0003FFFF | 256 KB | System RAM (bootloader use) |
| Peripherals | 0x02000000 | 0x0FFFFFFF | ~224 MB | MMIO devices |
| DRAM | 0x80000000 | varies | 4-8 GB | Main memory |

### Key Peripheral Addresses (verified on hardware at EL2)

| Peripheral | Base Address | EL2 Access | Notes |
|------------|--------------|------------|-------|
| UARTC (TX) | 0x0C280000 | ✅ Works | Serial output via TCU to USB-C |
| TCU RX Mailbox | 0x03C10000 | ✅ Works | HSP SM0, serial input from USB-C |
| UARTA | 0x03100000 | ❌ Blocked | 40-pin header UART, CBB denies |
| GICv3 Distributor | 0x0F400000 | ✅ Works | 992 interrupt lines |
| GICv3 Redistributor | 0x0F440000 | ✅ Works | Per-CPU, CPU 0 awake |
| GPU (PMC) | 0x17000000 | ✅ Works | GA10B identified |
| Watchdog | 0x02190000 | ✅ Works | Disabled early in boot |
| ARM Timer | System register | ✅ Works | Generic Timer (CNTPCT_EL0), 100 Hz |

See `docs/jetson-el2-bringup.md` for the full CBB firewall peripheral map.

---

## GIC Configuration

Jetson Orin Nano uses ARM GICv3 (verified working at EL2):

- **Distributor (GICD):** 0x0F400000 — 992 interrupt lines
- **Redistributor (GICR):** 0x0F440000 — per-CPU, CPU 0 awake

QEMU uses GICv2 at 0x08000000/0x08010000. The platform.h `#ifdef` handles the difference.

---

## Differences from QEMU virt

| Feature | QEMU virt | Jetson Orin Nano |
|---------|-----------|------------------|
| UART | PL011 @ 0x09000000 | NS16550 UARTC @ 0x0C280000 (via TCU) |
| UART RX | PL011 RBR | TCU HSP mailbox @ 0x03C10000 |
| GIC | GICv2 @ 0x08000000 | GICv3 @ 0x0F400000 |
| RAM Base | 0x40000000 | 0x80000000 |
| RAM Size | 128 MB (configurable) | ~6.7 GB (8 GB minus OP-TEE carveout) |
| CPU Count | 4 (configurable) | 6 (Cortex-A78AE), 1 online (SMP blocked) |
| Exception Level | EL1 (drops from EL3) | EL2 with VHE (stays at EL2) |
| Bootloader | Direct load | UEFI → kexec from Linux |
| Timer | Virtual | Physical (system register) |

---

## Boot Checklist

### Prerequisites

- ✅ Jetson with JetPack R36.4.7 and Linux on SD card
- ✅ USB-C cable for serial console (via TCU debug port)
- ✅ Network access for SSH (192.168.4.93)
- ✅ labctl configured for power control and serial capture

### Deploy and Boot (kexec method)

```bash
# 1. Build for Jetson
make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO

# 2. Deploy via SSH
scp build/kernel/slmos.elf root@192.168.4.93:/root/

# 3. Boot via kexec
ssh root@192.168.4.93 'kexec -l /root/slmos.elf --reuse-cmdline && kexec -e'

# 4. Observe via serial
labctl serial-capture jetson-nano-2 --timeout 60

# 5. Recover (power cycle back to Linux)
labctl power cycle jetson-nano-2 --delay 10
```

---

## Troubleshooting

### No Serial Output After kexec

1. Verify USB-C cable is connected to debug port (not power port)
2. Check labctl serial: `labctl serial-capture jetson-nano-2 --timeout 5`
3. Power cycle: `labctl power cycle jetson-nano-2 --delay 10`
4. If Jetson stuck after PSCI SYSTEM_OFF: power off for 30s, then power on

### UEFI Drops to Shell

- SD card not detected or boot order wrong
- Network boot timeouts add ~5 minutes — wait or press Enter

### CBB Firewall Errors (RAS)

- Accessing a blocked peripheral from EL2
- Check `docs/jetson-el2-bringup.md` for the peripheral access map
- UARTA (0x03100000) is always blocked; use UARTC (0x0C280000)
- UART driver issue (no output visible)

### Memory Access Faults

- RAM base address wrong (0x80000000, not 0x40000000)
- Peripheral address wrong
- MMU page table issue

---

## References

### Project Documentation

- `docs/platform-abstraction.md` — Comprehensive QEMU vs Jetson comparison, abstraction strategy
- `docs/boot-sequence.md` — Boot sequence research, PE/COFF header encoding
- `docs/jetson-tcu.md` — Why TCU doesn't work for bare-metal

### External Resources

- [Jetson Orin Series Boot Flow](https://docs.nvidia.com/jetson/archives/r35.4.1/DeveloperGuide/text/AR/BootArchitecture/JetsonOrinSeriesBootFlow.html)
- [UEFI Adaptation Guide](https://docs.nvidia.com/jetson/archives/r36.2/DeveloperGuide/SD/Bootloader/UEFI.html)
- [Tegra234 Device Tree (Linux kernel)](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/nvidia/tegra234.dtsi)
- [edk2-nvidia Wiki](https://github.com/NVIDIA/edk2-nvidia/wiki)

---

*Created: December 2025*
*Last updated: December 2025*
