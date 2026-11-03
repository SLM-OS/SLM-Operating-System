# Lab Operations Guide

This document describes how to interact with the embedded development lab hardware. All lab operations use **labctl** (Embedded Lab Control).

**labctl documentation:** [github.com/johnjezl/Embedded-Lab-Control](https://github.com/johnjezl/Embedded-Lab-Control)

---

## Quick Reference

| Operation | Command |
|-----------|---------|
| Power on | `labctl power on <sbc>` |
| Power off | `labctl power off <sbc>` |
| Power cycle | `labctl power cycle <sbc>` (waits the board's stored minimum cycle delay) |
| Serial console | `labctl connect <sbc>` (the SBC name resolves to its console port) |
| Lab status | `labctl status` |
| Health check | `labctl health-check` |

---

## labctl Is the Source of Truth for Lab Configuration

This document does **not** record lab wiring or configuration. For
each SBC, labctl owns:

- network addresses
- serial ports, aliases and ser2net TCP ports
- power plugs and outlets
- SDWire devices and which board each one is assigned to
- relay actuators and bindings

Read them live (MCP: the `lab://sbcs` resource, `bindings_list`,
`sdwire_discover`; CLI: `labctl status`). Placeholders such as
`<JETSON_IP>`, `<PI_IP>` and `<TEST_PC_IP>` below stand for the address
labctl reports for that board. If a document disagrees with labctl,
labctl is right and the document should be fixed.

What follows is per-board information that labctl does not hold:
hardware, firmware, installed software and deploy model.

---

## Lab Hardware

The current SBC list is labctl's (`lab://sbcs`, `labctl status`).

### Raspberry Pi 5 boards

| Component | Details |
|-----------|---------|
| Board | Raspberry Pi 5 (BCM2712), 4GB RAM |
| EEPROM | Sep 2024 firmware (do NOT update — see `docs/pi5-baremetal-status.md`) |
| Deploy model | SDWire-first on a board with an SDWire assigned in labctl ([`docs/deploy/pi5-sdwire.md`](deploy/pi5-sdwire.md)); maintenance-OS dual boot otherwise ([`docs/deploy/pi5-sdcard.md`](deploy/pi5-sdcard.md), [`docs/pi5-dual-boot-setup.md`](pi5-dual-boot-setup.md)) |
| SD card | Dual-boot SLM-OS + Pi OS Lite cards are supported on both models — see `docs/pi5-dual-boot-setup.md` |

### x86-64 PC (`test-pc`)

| Component | Details |
|-----------|---------|
| Board | Gigabyte H610M S2H V2, i7-6700, 16 GB DDR4, NVIDIA RTX 3050 (GA107) |
| Linux | Ubuntu on an external SSD (SSH as root, key auth); boots when no bootable SD card is switched to `test-pc` |
| SLM-OS | UEFI disk image on the SDWire-switched SD card, first in the boot order |
| Deploy guide | [`docs/deploy/x86-64-ssd.md`](deploy/x86-64-ssd.md) |

### Jetson Orin Nano boards

Both boards use the same layout (`jetson-nano-1` was reprovisioned to
match `jetson-nano-2` on 2026-11-03, #1043): JetPack on the microSD
card is the default cold boot and the deploy host, the NVMe holds a
copy of that rootfs plus a small `SLMOS-ESP` partition for the
UEFI-direct image. Both the kexec path
([`docs/deploy/jetson-kexec.md`](deploy/jetson-kexec.md)) and the
UEFI-direct path ([`docs/deploy/jetson-uefi-direct.md`](deploy/jetson-uefi-direct.md))
work on either board. How `jetson-nano-1` was provisioned from scratch
is recorded in
[`docs/deploy/jetson-uefi-direct.md` §Provisioning a board from scratch](deploy/jetson-uefi-direct.md#provisioning-a-board-from-scratch).

#### `jetson-nano-1`

| Component | Details |
|-----------|---------|
| Board | Jetson Orin Nano Super Developer Kit (flashed with `jetson-orin-nano-devkit-super`; DT `p3768-0000+p3767-0005-super`) |
| Status | 🟢 Working lab install (hostname `slmos-1`), reprovisioned 2026-11-03 (#1043). kexec 10/10 and UEFI-direct 10/10 to `slmos>` |
| QSPI firmware | UEFI 36.4.7 on both bootloader slots; patched BL31 (`tools/tfa-patches/0004-*`) in `A_secure-os` and `B_secure-os` |
| Linux | JetPack R36.4.7 (R36.4.4 flash + apt OTA), kernel 5.15.148-tegra, 25 W power mode |
| microSD | 64 GB, 15-partition JetPack layout, `root=/dev/mmcblk0p1`. extlinux menu: `primary kernel` (default), `Linux -> auto-kexec SLM-OS` |
| NVMe | 512 GB Samsung MZVL4512HBLU: `p1` ext4 `APP` (476.4 GiB, copy of the microSD rootfs with `root=/dev/nvme0n1p1`) and `p2` FAT32 `SLMOS-ESP` (512 MiB) holding `\EFI\SLMOS\SLMOS.efi`. UEFI entry `SLM-OS NVMe` points at it, outside BootOrder. No `BOOTAA64.efi` there, so cold boots fall through to the microSD |
| SLM-OS extras | `slmos-kexec`, `/root/gpu-mnist` (MNIST, sched-MLP and eviction producers, shaders, weights), `arm_smmu_noshutdown.ko`, patched `nvgpu.ko` (stock kept as `nvgpu.ko.stock`), `slmos-autokexec.service` (enabled) — on both media |
| Recovery strap | labctl `recovery_mode` latch binding wired to J14 pins 9-10. Must read `released` for a normal boot. After a Linux `poweroff`, power-cycle with a delay of at least 15 s: with the 5 s default the module doesn't see the power loss and stays off |
| Network | USB RTL8153 dongle for Linux and SLM-OS; the DHCP reservation is the address labctl lists |
| SSH | `ssh root@<JETSON_IP>` (key only; no root password set). User `slmos-1` / `slmos-1` with sudo |
| Camera | The IMX219 `JetsonIO` overlay used by the camera work was not reapplied after the reflash; see `docs/jetson-camera-imx219-plan.md` |
| Deploy | SSH + kexec workflow (see below), or UEFI-direct (`scripts/jetson-uefi-deploy.sh`) |

#### `jetson-nano-2`

| Component | Details |
|-----------|---------|
| Board | Jetson Orin Nano Super Developer Kit |
| Status | 🟢 Working lab install (hostname `gradient-desktop`); boots to the Linux login prompt in about 65 s |
| QSPI firmware | UEFI 36.4.7; patched BL31 (`tools/tfa-patches/0004-*`, built 2026-05-09) |
| Linux | JetPack R36.4.7, kernel 5.15.148-tegra, 25 W power mode |
| microSD | 238 GB, 15-partition JetPack layout, `root=/dev/mmcblk0p1`. extlinux menu: `primary kernel` (default), `SLM-OS`, `Linux -> auto-kexec SLM-OS` |
| NVMe | 119 GB SPCC: `p1` ext4 `APP` (118.7 GiB, spare L4T rootfs) and `p2` FAT32 `SLMOS-ESP` (512 MiB) holding the UEFI-direct image `\EFI\SLMOS\SLMOS.efi`. UEFI entry `SLM-OS NVMe` points at it, outside BootOrder; boot it one-shot with `scripts/jetson-uefi-deploy.sh` (#26, `docs/deploy/jetson-uefi-direct.md`). No `BOOTAA64.efi` there, so cold boots still fall through to the microSD |
| SLM-OS extras | `slmos-kexec`, `/root/gpu-mnist` helpers, `arm_smmu_noshutdown.ko`, patched `nvgpu.ko`, `slmos-autokexec.service` (enabled) all present. Not yet staged: `gpu-kernel-eviction-qnet` (PR-6 producer) |
| Recovery strap | labctl `recovery_mode` latch binding (applied before power-on). Must read `released` for a normal boot |
| Network | Linux and SLM-OS both use the USB RTL8153 dongle (on-board Ethernet and WiFi are down), so both get the same DHCP reservation — the address labctl lists |
| SSH | `ssh root@<JETSON_IP>` (key or password `slmos`) |
| Deploy | SSH + kexec workflow (see below), or UEFI-direct (`scripts/jetson-uefi-deploy.sh`) |

---

## Power Control

Power is switched through the smart-plug outlet labctl has assigned to
each board:

```bash
labctl power on <sbc>       # Turn on
labctl power off <sbc>      # Turn off
labctl power cycle <sbc>    # Off, wait the board's minimum delay, on
labctl power cycle <sbc> --delay 5  # Longer delay (smaller values are raised to the board's minimum)
```

**Important:** Boards must be configured to power on when AC power is
applied (on `test-pc` this is a BIOS setting). Otherwise a plug cycle
leaves them off.

---

## Serial Console

### Via labctl (recommended)

```bash
labctl connect <sbc>
```

This connects to the ser2net TCP port using `nc`. Press `Ctrl+]` then `q` to disconnect.

### Direct access (emergency only — labctl must be stopped)

labctl is the only supported hardware interface: never run `picocom`
or `nc` against the serial ports directly while ser2net is up (see
CLAUDE.md §"labctl Is the Only Hardware Interface" — bypassing it
causes device conflicts with other projects sharing the lab). Direct
access is a last resort for when labctl/ser2net is itself broken:

```bash
sudo systemctl stop ser2net             # release the port first
sudo picocom -b 115200 /dev/lab/<port>  # device path from labctl
# when done:
sudo systemctl start ser2net            # restore labctl access
```

Restart ser2net immediately afterward, and report the missing labctl
capability rather than adopting direct access as a workflow.

---

## Deploying to Raspberry Pi 5

Pi 5 boards in the lab intentionally use **two** deploy models:

- SDWire-first deploy, on a board with an SDWire assigned in labctl
- maintenance-OS dual boot, on a board without one

Which board uses which model follows the labctl SDWire assignments,
which change as devices move between boards. Preserve both models. Do
not assume every Pi 5 workflow should be rewritten to match the other.

### SDWire-first deploy

The SD card is connected through an SDWire which switches it between
the host machine (for flashing) and the Pi 5 (for booting) without
physical intervention.

### SDWire Commands (emergency only — labctl must be stopped)

Normal operation goes through labctl (`sdwire_to_host`,
`sdwire_to_dut`, `sdwire_update` — MCP tools or CLI equivalents).
Never drive the raw `sdwire` tool or `/dev/sd*` devices while labctl
is managing the lab: several SDWire devices exist and manual access
risks switching or writing the wrong card (see CLAUDE.md §"labctl Is
the Only Hardware Interface"). For recovery when labctl itself is
down, the raw `sdwire` tool takes the device serial number recorded in
labctl (`sdwire list` shows the serials of the attached devices). It is
installed in labctl's virtualenv, not on `PATH`; `head -1 "$(command -v
labctl)"` shows that venv's Python, and `sdwire` sits beside it.

```bash
sudo <labctl venv>/bin/sdwire switch -s "<serial>" host  # SD to host (flashing)
sudo <labctl venv>/bin/sdwire switch -s "<serial>" dut   # SD to the board (booting)
sudo <labctl venv>/bin/sdwire state -s "<serial>"        # current state
sudo <labctl venv>/bin/sdwire list                       # all SDWire devices
```

**Note:** The SDWire block device can change (e.g., `/dev/sdc` → `/dev/sdd`) when USB devices are plugged/unplugged. Always check labctl `sdwire_discover` (or `sdwire list`) for the current device.

### Full Deploy Workflow

All steps route through labctl — `sdwire_update` handles the SDWire
switch to host, the mount, the kernel copy, the unmount, and the
switch back to the Pi. Prefer the `/deploy-and-test` skill, which
orchestrates this whole cycle.

```bash
# 1. Build for Pi 5
make kernel-clean && make kernel PLATFORM=RASPI5

# 2. Flash SD card: labctl's sdwire_update tool (MCP) powers the board
#    off, switches the card to the host, copies build/kernel/slmos.bin
#    to kernel_2712.img (keep a .prev backup), and switches it back.

# 3. Boot and monitor
labctl power cycle <sbc>
sleep 15
labctl connect <sbc>
```

Do NOT manually mount `/dev/sd*` or `cp` onto the card. If
`sdwire_update` lacks a needed capability, request the labctl feature
instead of working around it (CLAUDE.md §"labctl Is the Only
Hardware Interface"). Manual recovery with labctl stopped is covered
under "SDWireC Commands" above.

### SD Card Contents

The boot partition (FAT32, labeled SLMOS) contains:
- `kernel_2712.img` — SLM-OS binary (`build/kernel/slmos.bin`)
- `armstub8-2712.bin` — EL3 stub for GIC group configuration
- `config.txt` — bare-metal boot config
- Standard Pi firmware: `bootcode.bin`, `start*.elf`, `fixup*.dat`, DTBs, `overlays/`

### Maintenance-OS dual boot (no SDWire)

A Pi 5 without an SDWire uses the maintenance-OS dual-boot model. That
means:

- Linux maintenance access is a supported part of the workflow
- dual-boot card layout is not an accident; it is a first-class deploy path
- native ingress work should not erase the usefulness of this model

Use these guides instead of trying to force an SDWire workflow onto it:

- [`docs/deploy/pi5-sdcard.md`](deploy/pi5-sdcard.md)
- [`docs/pi5-dual-boot-setup.md`](pi5-dual-boot-setup.md)

---

## Deploying to Jetson (SSH + kexec)

🟢 **Working** — boots to shell at EL2 via UARTC on both jetson-nano-1 and jetson-nano-2.

Jetson deploys do not use an SDWire. All deployment is via SSH + kexec, with serial
console and power control via labctl. No physical handling is required after initial setup.

SLM-OS has no partition on either boot medium: the kernel is the file
`/root/slmos.elf` in the Linux root filesystem, and `slmos-kexec`
launches it from Linux. Provisioning (NVMe development install, microSD
recovery install, QSPI caveats) is covered in
[`docs/deploy/jetson-kexec.md` §First-time provisioning](deploy/jetson-kexec.md#first-time-provisioning-dual-media-layout).

### Board State

| Board | Current state (2026-11-03) | Layout |
|-------|----------------------------|--------|
| jetson-nano-1 | 🟢 Working — reprovisioned 2026-11-03 (#1043) | microSD JetPack with SLM-OS extras (default boot) + NVMe rootfs copy + NVMe `SLMOS-ESP` |
| jetson-nano-2 | 🟢 Working — microSD JetPack with SLM-OS extras | microSD JetPack with SLM-OS extras (default boot) + NVMe spare rootfs + NVMe `SLMOS-ESP` |

Serial ports, IP addresses and power: use what labctl reports.

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
labctl serial capture <sbc> --timeout 30

# 5. Recover back to Linux (power cycle)
labctl power cycle <sbc>
```

### End-to-End Smoke Validation

For the current `jetson-nano-2` USB networking regression check, use:

```bash
scripts/tests/test-jetson-kexec-networking-smoke.sh \
    --ssh-target root@<JETSON_IP> \
    --console-port <ser2net TCP port from labctl> \
    --expected-ip <JETSON_IP> \
    --expected-gateway <GATEWAY> \
    --ping-target <GATEWAY>
```

`<GATEWAY>` is the board's default gateway, as Linux on the board reports
it (`ip route`). Pass all three address flags: the script's built-in
defaults are fixed addresses from an earlier lab setup, not values read
from labctl.

This drives the working lab path end-to-end from the host:

- deploy the current `slmos.elf` + `slmos-kexec`
- trigger Linux -> `kexec` -> SLM-OS
- wait for `slmos>` over ser2net
- run `net init`, send one `ifconfig dhcp`, poll `ifconfig` until `DHCP(bound)`, and then ping the gateway
- verify that DHCP bound the expected address and gateway

It is not a generic Jetson automation harness; it is the repeatable
smoke test for the validated `nano-2` USB hub + RTL8153 lab setup.

The `slmos-kexec` helper script (`scripts/jetson-kexec-slmos.sh` in the repo)
suspends the GPU via BPMP runtime PM before calling `kexec`. Without the GPU
suspend, stale nvgpu DMA operations trigger a TF-A RAS Uncorrectable Error that
kills the CPU core. See GitHub issue #9 for root-cause analysis.

### Recovery

After kexec into SLM-OS, the Jetson cannot be reached via SSH. To recover:

1. `labctl power cycle <sbc>` — power cycles back to Linux
2. Wait for Linux to boot — about 65 s from power-on to the login
   prompt on `jetson-nano-2`. A board still silent well past that is
   most likely held in USB Force Recovery; see
   [Troubleshooting](#jetson-silent-console-and-no-network-after-power-on)
3. SSH is available again

**Note:** Serial input (RX) works via TCU HSP mailbox on the USB-C debug port.
Serial output uses UARTC at 0x0C280000 with DSB-guarded LSR reads.

---

## Troubleshooting

### Verify live board state first

Board state drifts away from what this document records: reinstalls,
actuator settings, address changes and other users' sessions all
change it without touching the repo. Before debugging any "board does
not respond" symptom, and before any hardware verification run, check
the live state through labctl rather than trusting the tables above:

1. **Power, ping, serial:** `labctl status` / `labctl health-check` (MCP:
   `run_health_check`).
2. **Actuator bindings:** list them for the board (MCP:
   `bindings_list`) and read each one's `actuation_status`. A latched
   `recovery_mode` binding with `desired_state: asserted` holds a Jetson
   in USB Force Recovery on every power-up — see the next entry.
3. **Serial port ownership:** a capture that returns `Port already in
   use` immediately means another console session (for example a
   forgotten `labctl console`) holds the port.
4. **The install itself,** once SSH works: `hostname`,
   `/etc/nv_tegra_release`, `lsblk`, and the SLM-OS extras checklist in
   [`docs/deploy/jetson-kexec.md`](deploy/jetson-kexec.md#slm-os-extras-on-the-development-install).

Record any difference found in this document (addresses go in labctl
only).

### Jetson: silent console and no network after power-on

Symptom: labctl reports power on and the serial port opens, but a
capture after the power cycle shows no boot output at all (at most a
stray character), and the board never answers ping or SSH. A healthy
board prints MB1/MB2/UEFI/Linux output from the first second and
reaches the Linux login prompt about 65 s after power-on
(`jetson-nano-2`, 2026-10-29).

1. **Recovery strap asserted.** Check `actuation_status <sbc>
   recovery_mode`. `desired_state: asserted` (relay channel `closed`)
   means the boot ROM enters USB Force Recovery and waits for a flash
   host; UEFI and Linux never run, so nothing appears on the console.
   Release the binding (MCP: `exit_recovery` / `release`) and power-cycle.
2. **Confirm from the lab host:** `lsusb | grep 0955`. A board in Force
   Recovery enumerates on its USB-C port as `0955:7523 NVIDIA Corp. APX`
   (Orin Nano 8 GB) about 3 s after power-on and disappears at power-off.
   A normally booting board shows no APX device.
3. **Actuator access:** if `actuator_probe` reports `Permission denied`
   on `/dev/lab/relay-*`, the labctl process lacks the `dialout` group
   (group changes need a fresh login, and a long-running `labctl mcp`
   keeps the groups it started with). Binding state can still be read,
   but the relay cannot be changed from that process.
4. Otherwise continue with the serial checks below.

### No serial output

1. Check power: `labctl power cycle <sbc>`
2. Check serial connection: `labctl connect <sbc>`
3. On an SDWire board, check that the card is switched to the DUT (labctl `sdwire_to_dut`)
4. Check USB: `lsusb | grep -i "CH340\|cp210"` for serial adapters

### Smart plug authentication error

A smart plug occasionally rejects labctl's authentication (seen with Kasa plugs when the cloud token expires). Retry the command; it usually succeeds on the second attempt. If it keeps failing, the plug's credentials in labctl need attention.

### SDWire not detected

1. Check USB: labctl `sdwire_discover` lists every attached SDWire with its serial number
2. Check cable connections
3. The SDWireC uses `0bda:0316` (Realtek); the original SDWire is an FTDI device

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
*Updated: 29 October 2026 — Jetson board state refreshed, dual-media provisioning cross-reference, `sync` step in the deploy workflow; nano-2 corrected to working, live-state and recovery-strap troubleshooting added*
*Updated: 2 November 2026 — Removed lab configuration owned by labctl (serial ports, ser2net ports, SDWire serials and assignments, relay channels, addresses); added `test-pc`*
