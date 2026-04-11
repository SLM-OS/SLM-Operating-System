# Lab Operations Guide

This document describes how to interact with the embedded development lab hardware. All lab operations use **labctl** (Embedded Lab Control).

**labctl documentation:** [github.com/johnjezl/Embedded-Lab-Control](https://github.com/johnjezl/Embedded-Lab-Control)

---

## Quick Reference

| Operation | Command |
|-----------|---------|
| Power on | `labctl power on pi-5-1` |
| Power off | `labctl power off pi-5-1` |
| Power cycle | `labctl power cycle pi-5-1 --delay 2` |
| Serial console | `labctl connect pi-5-1-console` |
| Lab status | `labctl status` |
| Health check | `labctl health` |

---

## Lab Hardware

### Raspberry Pi 5 (`pi-5-1`)

| Component | Details |
|-----------|---------|
| Board | Raspberry Pi 5 (BCM2712), 4GB RAM |
| Serial console | `/dev/lab/port-2-2` → TCP localhost:4005 (via ser2net) |
| Power control | Kasa smart plug (via labctl) |
| SD card deploy | SDWireC (Badgerd USB-C model) |
| EEPROM | Sep 2024 firmware (do NOT update — see `docs/pi5-baremetal-status.md`) |

### Jetson Orin Nano (`jetson-nano-2`)

| Component | Details |
|-----------|---------|
| Board | Jetson Orin Nano Super Developer Kit |
| Status | 🟡 Partial — boots to shell at EL2 via UARTC (see `docs/jetson-el2-bringup.md`) |
| Linux | Ubuntu 22.04.5, kernel 5.15.148-tegra, JetPack R36.4.7 |
| Serial console | `/dev/lab/port-2-1` → TCP localhost:4004 (via ser2net) |
| Power control | Kasa smart plug (via labctl) |
| Ethernet | `192.168.4.93` (enP8p1s0) |
| WiFi | `192.168.4.39` (wlP1p1s0) |
| SSH | `ssh root@192.168.4.93` (port 22, password: slmos) |
| SD card deploy | No SDWire — use SSH + kexec workflow (see below) |
| Boot media | microSD card (no NVMe installed) |

---

## Power Control

Power is managed via Kasa smart plugs controlled by labctl:

```bash
labctl power on pi-5-1       # Turn on
labctl power off pi-5-1      # Turn off
labctl power cycle pi-5-1    # Off, wait 2s, on
labctl power cycle pi-5-1 --delay 5  # Custom delay
```

**Important:** Devices are configured to auto-power-on when AC power is applied.

---

## Serial Console

### Via labctl (recommended)

```bash
labctl connect pi-5-1-console
```

This connects to the ser2net TCP port using `nc`. Press `Ctrl+]` then `q` to disconnect.

### Direct access

```bash
sudo picocom -b 115200 /dev/ttyUSB1   # Pi 5 console
```

**Note:** ser2net must be stopped first for direct access (`sudo systemctl stop ser2net`).

### ser2net Configuration

ser2net provides TCP access to serial ports. Configuration is in `/etc/ser2net.yaml`:

| Port | TCP Port | Device | Baud |
|------|----------|--------|------|
| pi-5-1-console | 4005 | `/dev/lab/port-2-2` | 115200 |
| jetson-console | 4004 | `/dev/lab/port-2-1` | 115200 |

---

## Deploying to Raspberry Pi 5

The Pi 5 SD card is connected through a Badgerd SDWireC which allows the SD card to be switched between the host machine (for flashing) and the Pi 5 (for booting) without physical intervention.

### SDWireC Commands

```bash
# Switch SD to host (for flashing)
sudo /tmp/sdwire-venv/bin/sdwire switch -s "20120501030900000.10.3" host

# Switch SD to Pi 5 (for booting)
sudo /tmp/sdwire-venv/bin/sdwire switch -s "20120501030900000.10.3" dut

# Check current state
sudo /tmp/sdwire-venv/bin/sdwire state -s "20120501030900000.10.3"

# List all SDWire devices
sudo /tmp/sdwire-venv/bin/sdwire list
```

**Note:** The SDWireC block device can change (e.g., `/dev/sdc` → `/dev/sdd`) when USB devices are plugged/unplugged. Always check `sdwire list` for the current device.

### Full Deploy Workflow

```bash
# 1. Build for Pi 5
make kernel-clean && make kernel PLATFORM=RASPI5

# 2. Flash SD card
labctl power off pi-5-1
sudo /tmp/sdwire-venv/bin/sdwire switch -s "20120501030900000.10.3" host
sleep 2
sudo mount /dev/sddN /mnt           # Check sdwire list for correct device!
sudo cp build/kernel/slmos.bin /mnt/kernel_2712.img
sudo umount /mnt
sudo /tmp/sdwire-venv/bin/sdwire switch -s "20120501030900000.10.3" dut

# 3. Boot and monitor
labctl power cycle pi-5-1 --delay 2
sleep 15
labctl connect pi-5-1-console
```

### SD Card Contents

The boot partition (FAT32, labeled SLMOS) contains:
- `kernel_2712.img` — SLM-OS binary (`build/kernel/slmos.bin`)
- `armstub8-2712.bin` — EL3 stub for GIC group configuration
- `config.txt` — bare-metal boot config
- Standard Pi firmware: `bootcode.bin`, `start*.elf`, `fixup*.dat`, DTBs, `overlays/`

---

## Deploying to Jetson (SSH + kexec)

🟢 **Working** — boots to shell at EL2 via UARTC on both jetson-nano-1 and jetson-nano-2.

The Jetson boards have no SDWire device. All deployment is via SSH + kexec, with serial
console and power control via labctl. No physical handling is required after initial setup.

### Lab Configuration

| Board | IP | Serial | Status |
|-------|-----|--------|--------|
| jetson-nano-1 | 192.168.4.20 | tcp:4007 (CH340) | L4T 36.4.4, SD=JetPack, SSD=SLM-OS |
| jetson-nano-2 | 192.168.4.93 | tcp:4004 (CP2102) | L4T 36.4.7, SD=SLM-OS+Linux |

### Deploy Workflow

```bash
# 1. Build for Jetson
make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO

# 2. Copy kernel to Jetson via SSH
scp build/kernel/slmos.elf root@192.168.4.20:/root/

# 3. Suspend GPU (REQUIRED — prevents RAS crash after kexec)
ssh root@192.168.4.20 bash -c '
    GPU=/sys/devices/platform/bus@0/17000000.gpu/power
    systemctl stop gdm 2>/dev/null; sleep 2
    fuser -k /dev/nvhost-gpu /dev/nvmap 2>/dev/null; sleep 1
    echo 0 > $GPU/autosuspend_delay_ms
    echo auto > $GPU/control
    sleep 3
'

# 4. Boot via kexec (from SSH session)
ssh root@192.168.4.20 'kexec -l /root/slmos.elf --reuse-cmdline && kexec -e'

# 5. Observe output via serial console
labctl serial capture jetson-nano-1 --timeout 30

# 6. Recover back to Linux (power cycle)
labctl power cycle jetson-nano-1
```

**Important:** Step 3 (GPU suspend) is mandatory. Without it, Linux's nvgpu driver
leaves stale GPU DMA operations that cause a TF-A RAS error after kexec, killing the
CPU core. See GitHub issue #9.

### Recovery

After kexec into SLM-OS, the Jetson cannot be reached via SSH. To recover:

1. `labctl power cycle jetson-nano-1` — power cycles back to Linux
2. Wait ~40s for Linux to boot
3. SSH is available again

**Note:** Serial input (RX) works via TCU HSP mailbox on the USB-C debug port.
Serial output uses UARTC at 0x0C280000 with DSB-guarded LSR reads.

---

## Troubleshooting

### No serial output

1. Check power: `labctl power cycle pi-5-1`
2. Check serial connection: `labctl connect pi-5-1-console`
3. Check SD card: `sudo /tmp/sdwire-venv/bin/sdwire state -s "20120501030900000.10.3"` (should be "Target" for booting)
4. Check USB: `lsusb | grep -i "CH340\|cp210"` for serial adapters

### Kasa authentication error

The Kasa smart plug occasionally returns authentication errors (cloud token expired). Retry the command — it usually succeeds on the second attempt.

### SDWireC not detected

1. Check USB: `sudo /tmp/sdwire-venv/bin/sdwire list`
2. Check cable connections
3. The SDWireC uses `0bda:0316` (Realtek) — different from the old SDWire which used `04e8:6001` (Samsung)

### Build fails for wrong platform

The CMake cache remembers the last platform. Always clean first:
```bash
make kernel-clean && make kernel PLATFORM=RASPI5
```

---

## Hardware Reference

### Pi 5 40-Pin Header Serial

| Pin | Function | Connect to USB-Serial |
|-----|----------|----------------------|
| 6 | GND | GND |
| 8 | GPIO14 (TX) | RX |
| 10 | GPIO15 (RX) | TX |

**Important:** Use 3.3V USB-serial adapter. 5V may damage the Pi.

### Jetson 40-Pin Header Serial

| Pin | Function | Connect to USB-Serial |
|-----|----------|----------------------|
| 6 | GND | GND |
| 8 | UART1_TX | RX |
| 10 | UART1_RX | TX |

**Important:** Use 3.3V USB-serial adapter. 5V will damage the Jetson.

---

*Created: 25 December 2025*
*Updated: 31 March 2026 — Replaced lab-tools with labctl, added SDWireC/Pi 5 workflow*
