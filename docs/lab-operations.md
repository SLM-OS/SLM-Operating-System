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
| Deploy model | SDWire-first |
| SD card | Dual-boot SLM-OS + Pi OS Lite — see `docs/pi5-dual-boot-setup.md` |

### Raspberry Pi 5 (`pi-5-2`)

| Component | Details |
|-----------|---------|
| Board | Raspberry Pi 5 (BCM2712), 4GB RAM |
| Serial console | labctl-managed serial console |
| Power control | labctl-managed smart plug |
| Deploy model | No SDWire / maintenance-OS dual boot |
| SD card deploy | No SDWire — preserve the dual-boot maintenance workflow |
| Notes | Use `docs/deploy/pi5-sdcard.md` and `docs/pi5-dual-boot-setup.md` as the authoritative workflows |

### Jetson Orin Nano boards

Both Jetsons need reprovisioning (state recorded 2026-10-29). The
target layout for each is an NVMe development install plus a microSD
recovery install — see
[`docs/deploy/jetson-kexec.md` §First-time provisioning](deploy/jetson-kexec.md#first-time-provisioning-dual-media-layout).
Network addresses are deliberately not recorded here: labctl is the
single source of truth (`labctl status`), and `<JETSON_IP>` below
stands for the address it reports.

#### `jetson-nano-1`

| Component | Details |
|-----------|---------|
| Board | Jetson Orin Nano Developer Kit (UEFI banner now reports "Super"; #844 recorded it as a non-Super flash, so it has likely been reflashed since) |
| Status | 🔴 Needs reprovisioning — boots JetPack from microSD; root password and SSH host key no longer match lab records |
| QSPI firmware | UEFI 36.4.4 |
| microSD | JetPack, `root=/dev/mmcblk0p1` |
| NVMe | Samsung MZVL4512HBLU with a single ext4 `APP` partition and no EFI system partition — its UEFI boot entry returns to the menu |
| Serial console | `/dev/lab/port-2-3` → TCP localhost:4007 (via ser2net), labctl alias `jetson-nano-1-console` |
| Power control | Kasa smart plug (via labctl) |
| SD card deploy | No SDWire — SSH + kexec workflow (see below) |

#### `jetson-nano-2`

| Component | Details |
|-----------|---------|
| Board | Jetson Orin Nano Super Developer Kit |
| Status | 🔴 Needs reprovisioning — currently boots a JetPack install left from an unrelated project (hostname `gradient-desktop`) and is not reachable over SSH at any recorded address |
| QSPI firmware | Patched BL31 (`tools/tfa-patches/0004-*`) last verified 2026-05-19 — recheck after any reflash |
| Boot media | microSD per earlier records; NVMe presence not yet confirmed |
| Serial console | `/dev/lab/port-2-1` → TCP localhost:4004 (via ser2net), labctl alias `jetson-console` |
| Power control | Kasa smart plug (via labctl) |
| Network | Linux uses the on-board Ethernet (enP8p1s0) and WiFi (wlP1p1s0); after kexec, SLM-OS takes its own DHCP lease over the USB RTL8153, so the board has different Linux and SLM-OS addresses — check which one labctl lists before using it |
| SSH | `ssh root@<JETSON_IP>` (password `slmos`) once reprovisioned |
| SD card deploy | No SDWire — SSH + kexec workflow (see below) |

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

Pi 5 boards in the lab intentionally use **two** deploy models:

- `pi-5-1`: SDWire-first deploy
- `pi-5-2`: no-SDWire / maintenance-OS dual boot

Preserve both models. Do not assume every Pi 5 workflow should be
rewritten to match the other.

### `pi-5-1`: SDWire-first deploy

The `pi-5-1` SD card is connected through a Badgerd SDWireC which
allows the SD card to be switched between the host machine (for
flashing) and the Pi 5 (for booting) without physical intervention.

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

### `pi-5-2`: no-SDWire / maintenance-OS dual boot

`pi-5-2` should be treated as the lab reference for the no-SDWire Pi 5
model. That means:

- Linux maintenance access is a supported part of the workflow
- dual-boot card layout is not an accident; it is a first-class deploy path
- native ingress work should not erase the usefulness of this model

Use these guides instead of trying to force an SDWire workflow onto it:

- [`docs/deploy/pi5-sdcard.md`](deploy/pi5-sdcard.md)
- [`docs/pi5-dual-boot-setup.md`](pi5-dual-boot-setup.md)

---

## Deploying to Jetson (SSH + kexec)

🟢 **Working** — boots to shell at EL2 via UARTC on both jetson-nano-1 and jetson-nano-2.

The Jetson boards have no SDWire device. All deployment is via SSH + kexec, with serial
console and power control via labctl. No physical handling is required after initial setup.

SLM-OS has no partition on either boot medium: the kernel is the file
`/root/slmos.elf` in the Linux root filesystem, and `slmos-kexec`
launches it from Linux. Provisioning (NVMe development install, microSD
recovery install, QSPI caveats) is covered in
[`docs/deploy/jetson-kexec.md` §First-time provisioning](deploy/jetson-kexec.md#first-time-provisioning-dual-media-layout).

### Lab Configuration

| Board | Serial (labctl alias / TCP) | Current state (2026-10-29) | Target layout |
|-------|-----------------------------|----------------------------|---------------|
| jetson-nano-1 | `jetson-nano-1-console` / 4007 | 🔴 Needs reprovisioning — microSD JetPack boots, NVMe not bootable | NVMe development + microSD recovery |
| jetson-nano-2 | `jetson-console` / 4004 | 🔴 Needs reprovisioning — unrelated JetPack install, no SSH | NVMe development + microSD recovery |

IP addresses: use the ones labctl reports (`labctl status`).

**Serial adapter requirement:** Must be CP2102 (Silicon Labs) for bidirectional
serial. CH340 adapters only work in one direction (TX from board) on the debug
header — input characters don't reach the SPE firmware. CH340s are fine on Pi 5.

### One-time Setup: Install kexec helper on Jetson

The `slmos-kexec` helper handles GPU suspend automatically (required to prevent
the TF-A RAS crash documented in GitHub issue #9).

```bash
# Copy the helper to the Jetson's /usr/local/bin
scp scripts/jetson-kexec-slmos.sh root@<JETSON_IP>:/usr/local/bin/slmos-kexec
ssh root@<JETSON_IP> "chmod +x /usr/local/bin/slmos-kexec"
```

### Optional: install the Jetson auto-kexec boot target

For unattended bring-up, the repo also carries a `systemd` boot target that
automatically runs `slmos-kexec` after Linux reaches `multi-user.target`.
This does **not** replace the normal Linux boot; it is meant to back a
second extlinux/boot-menu entry while leaving `primary` as the default.

Install once:

```bash
scp scripts/systemd/jetson/slmos-autokexec.target root@<JETSON_IP>:/etc/systemd/system/
scp scripts/systemd/jetson/slmos-autokexec.service root@<JETSON_IP>:/etc/systemd/system/
scp scripts/systemd/jetson/slmos-autokexec.env root@<JETSON_IP>:/etc/default/slmos-autokexec
ssh root@<JETSON_IP> 'systemctl daemon-reload && systemctl enable slmos-autokexec.service'
```

Then add a matching `LABEL slmos-autokexec` entry to
`/boot/extlinux/extlinux.conf` that appends:

```text
systemd.unit=slmos-autokexec.target slmos.autokexec=1
```

See [`docs/deploy/jetson-kexec.md`](deploy/jetson-kexec.md) for the full
entry and the expected behavior.

### Deploy Workflow

```bash
# 1. Build for Jetson
make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO

# 2. Copy kernel to Jetson via SSH, then flush it to disk
#    (without sync, the power cycle in step 5 can lose the write and
#    the next boot silently runs the previous kernel)
scp build/kernel/slmos.elf root@<JETSON_IP>:/root/
ssh root@<JETSON_IP> sync

# 3. Boot via kexec (GPU suspend + kexec in one step)
ssh root@<JETSON_IP> 'slmos-kexec /root/slmos.elf'

# 4. Observe output via serial console
labctl serial capture jetson-nano-1 --timeout 30

# 5. Recover back to Linux (power cycle)
labctl power cycle jetson-nano-1
```

### End-to-End Smoke Validation

For the current `jetson-nano-2` USB networking regression check, use:

```bash
scripts/tests/test-jetson-kexec-networking-smoke.sh \
    --ssh-target root@<JETSON_IP> \
    --console-port 4004
```

This drives the working lab path end-to-end from the host:

- deploy the current `slmos.elf` + `slmos-kexec`
- trigger Linux -> `kexec` -> SLM-OS
- wait for `slmos>` over ser2net
- run `net init`, send one `ifconfig dhcp`, poll `ifconfig` until `DHCP(bound)`, and then run `ping 192.168.4.1 2`
- verify DHCP `192.168.4.5/24` and gateway `192.168.4.1`

It is not a generic Jetson automation harness; it is the repeatable
smoke test for the validated `nano-2` USB hub + RTL8153 lab setup.

The `slmos-kexec` helper script (`scripts/jetson-kexec-slmos.sh` in the repo)
suspends the GPU via BPMP runtime PM before calling `kexec`. Without the GPU
suspend, stale nvgpu DMA operations trigger a TF-A RAS Uncorrectable Error that
kills the CPU core. See GitHub issue #9 for root-cause analysis.

### Recovery

After kexec into SLM-OS, the Jetson cannot be reached via SSH. To recover:

1. `labctl power cycle jetson-nano-1` — power cycles back to Linux
2. Wait for Linux to boot — about 40 s on a provisioned board. On
   2026-10-29 both boards took several minutes after a cold power
   cycle before the serial console showed a login prompt (cause not
   yet established; UEFI network-boot fallback is one known source of
   multi-minute delays, see `docs/jetson-boot.md` §Troubleshooting),
   so serial captures after a power cycle should use a long timeout
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
*Updated: 29 October 2026 — Jetson board state refreshed (both need reprovisioning), dual-media provisioning cross-reference, `sync` step in the deploy workflow*
