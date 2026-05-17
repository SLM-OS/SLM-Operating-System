# Deploying SLM-OS to Raspberry Pi 5 — No-SDWire / Maintenance-OS Model

This guide covers the **no-SDWire Pi 5 model**: either physically writing an SD card from the development host, or using a Linux maintenance environment already present on the card. Use this path when the target board has **no SDWire interface** (for example, `pi-5-2` in the capstone lab). For boards equipped with SDWire, see [`pi5-sdwire.md`](pi5-sdwire.md).

Cards prepared with a Raspberry Pi OS maintenance install can also update `kernel_2712.img` locally from that OS. That maintenance environment is a supported deployment model, not just an accident of one card layout. If the maintenance OS is not reachable over SSH or an exclusive serial/login path, the deploy path falls back to sneakernet.

---

## When to use this guide

| Scenario | Guide |
|---|---|
| New Pi 5 board, fresh SD card, no lab integration | This guide |
| Pi 5 with SDWire, iterative kernel updates | [`pi5-sdwire.md`](pi5-sdwire.md) |
| Pi 5 without SDWire, Pi OS maintenance install reachable locally | This guide, use [Local maintenance update](#local-maintenance-update) |
| Pi 5 without SDWire, explicit dual-boot setup desired | [`../pi5-dual-boot-setup.md`](../pi5-dual-boot-setup.md) |
| Moving an existing SLM-OS SD card between boards | This guide, skip to [Kernel update](#kernel-update) |

---

## Prerequisites

| Item | Source / Version |
|---|---|
| Development host with SD card reader | Host with `/dev/mmcblk*` or USB card reader exposed as `/dev/sd*` |
| SD card, ≥ 2 GB | microSD recommended |
| SLM-OS source checkout | `make kernel PLATFORM=RASPI5` succeeds |
| ARM GNU Toolchain | `aarch64-none-elf-gcc` (see `docs/getting-started.md`) |
| Pi 5 board with up-to-date EEPROM | Sep 2024 firmware minimum. See `docs/pi5-baremetal-status.md`. **Do not upgrade past the tested version** without checking the memory note at `pi5_eeprom_findings.md`. |
| USB-to-serial adapter (3.3 V) | Required — Pi 5 has no HDMI output from bare-metal SLM-OS. CH340 or CP2102 adapters both work on Pi 5. |

---

## First-time provisioning

The simplest provisioning path reuses the Raspberry Pi OS boot partition layout. A standard **Raspberry Pi OS Lite** image supplies all the firmware files SLM-OS needs (`bootcode.bin`, `start*.elf`, `fixup*.dat`, DTBs, `overlays/`); SLM-OS replaces only the kernel and `config.txt`.

### Step 1 — Flash Raspberry Pi OS Lite with the Imager

Use the official Raspberry Pi Imager to write **Raspberry Pi OS Lite (64-bit)** to the SD card. The rootfs (ext4) is never touched by SLM-OS during normal kernel updates and is preserved for revert.

### Step 2 — Identify the SD card

After Imager finishes and unmounts the card, re-insert it. Confirm the device:

```bash
lsblk -o NAME,SIZE,TYPE,LABEL,MODEL,TRAN
```

The card appears as a device with two partitions: `bootfs` (FAT32, ~512 MB) and `rootfs` (ext4, remainder). Substitute the actual device name (for example `/dev/sdX`) in the remaining steps. **Double-check the device.** Writing to the wrong `/dev/sd*` destroys unrelated data.

### Step 3 — Build the SLM-OS kernel

```bash
make kernel PLATFORM=RASPI5
```

Output: `build/kernel/slmos.bin` (raw binary, ~5 MB with default features — networking + telnetd + Lua + lwIP + AI runtime; depends on which optional features are enabled at build time).

### Step 4 — Write SLM-OS to the boot partition

The following script backs up the Pi OS kernel and config, installs SLM-OS, and is safe to re-run.

```bash
DEV=/dev/sdX1   # bootfs partition — VERIFY this is correct
MNT=$(mktemp -d)
sudo mount "$DEV" "$MNT"

# Sanity-check that this is actually a Pi boot partition
if [ ! -f "$MNT/bootcode.bin" ] || [ ! -f "$MNT/kernel_2712.img" ]; then
    echo "ABORT: $DEV does not look like a Pi boot partition"
    sudo umount "$MNT"; rmdir "$MNT"
    exit 1
fi

# Preserve original Pi OS files (idempotent)
[ -f "$MNT/config.txt.pios-bak" ] || sudo cp -a "$MNT/config.txt" "$MNT/config.txt.pios-bak"
[ -f "$MNT/cmdline.txt.pios-bak" ] || sudo cp -a "$MNT/cmdline.txt" "$MNT/cmdline.txt.pios-bak"
[ -f "$MNT/kernel_2712.img.pios-bak" ] || sudo cp -a "$MNT/kernel_2712.img" "$MNT/kernel_2712.img.pios-bak"

# Install bare-metal config + tryboot config
# Source of truth: deploy/pi5/{config.txt,tryboot.txt}
# Authoritative spec for individual keys: docs/pi5-baremetal-status.md §Configuration
sudo cp -v deploy/pi5/config.txt   "$MNT/config.txt"
sudo cp -v deploy/pi5/tryboot.txt  "$MNT/tryboot.txt"

# Install the SLM-OS kernel
sudo cp -v build/kernel/slmos.bin "$MNT/kernel_2712.img"

# Verify byte-for-byte match
cmp build/kernel/slmos.bin "$MNT/kernel_2712.img" \
    && echo "kernel_2712.img matches build/kernel/slmos.bin"

sync
sudo umount "$MNT"
rmdir "$MNT"
```

After this completes, the boot partition contains:

| File | Purpose |
|---|---|
| `kernel_2712.img` | SLM-OS binary (the Pi firmware looks for this filename on Pi 5) |
| `config.txt` | Bare-metal boot configuration — copied from `deploy/pi5/config.txt` |
| `tryboot.txt` | Tryboot config (loaded instead of `config.txt` when the bootloader tryboot flag is armed) — copied from `deploy/pi5/tryboot.txt`. See `deploy/pi5/README.md`. |
| `config.txt.pios-bak` | Original Raspberry Pi OS `config.txt` |
| `cmdline.txt.pios-bak` | Original kernel command line (unused by bare-metal SLM-OS) |
| `kernel_2712.img.pios-bak` | Original Linux kernel |
| `bootcode.bin`, `start*.elf`, `fixup*.dat`, DTBs, `overlays/` | Pi firmware — untouched |

The simplest durable layout is:

| Partition | Typical size | Purpose |
|---|---|---|
| `bootfs` (`/dev/sdX1`) | `512M` FAT32 | Pi firmware files, `config.txt`, `kernel_2712.img` |
| `rootfs` (`/dev/sdX2`) | `8G` ext4 | Raspberry Pi OS maintenance environment used for recovery and local updates |
| optional data partition (`/dev/sdX3`) | remainder | staging area for new kernels, logs, captures |

If you want explicit Linux/SLM-OS boot selection semantics instead of
"Linux rootfs happens to be present on the same card", use the full
dual-boot setup in [`../pi5-dual-boot-setup.md`](../pi5-dual-boot-setup.md).

### Step 5 — Wire up the serial console

SLM-OS has no video output. The shell is only reachable over UART.

| Pi 5 pin | Function | USB-serial wire |
|---|---|---|
| 6 | GND | GND |
| 8 | GPIO14 (TX) | RX |
| 10 | GPIO15 (RX) | TX |

Use a **3.3 V** USB-serial adapter. 5 V will damage the Pi. Terminal settings: 115200 baud, 8N1, no flow control.

### Step 6 — Boot and verify

1. Eject the SD card from the host.
2. Insert it into the Pi 5.
3. Connect the serial adapter to the host and open a terminal (e.g. `picocom -b 115200 /dev/ttyUSB0`).
4. Power on the Pi 5.

Expected output:

```
SLM-OS booting on Raspberry Pi 5
...
slm> 
```

The `slm>` prompt confirms boot completed and the shell is accepting input. Type `help` to list shell commands.

---

## Kernel update

Once provisioning is complete, iterative updates only need to replace `kernel_2712.img`. Power the Pi off first:

```bash
# Build
make kernel PLATFORM=RASPI5

# Mount the bootfs (verify device!)
DEV=/dev/sdX1
MNT=$(mktemp -d)
sudo mount "$DEV" "$MNT"

# Write kernel
sudo cp -v build/kernel/slmos.bin "$MNT/kernel_2712.img"
sync
sudo umount "$MNT"
rmdir "$MNT"
```

Re-insert the card and power on.

## Local maintenance update

If the card keeps a bootable Raspberry Pi OS maintenance install and that OS is reachable, you can update SLM-OS without removing the card from the Pi.

This path requires one of:

- SSH access to the Pi OS environment
- exclusive serial/login access to the Pi OS environment

It does **not** work if the board only answers `ping` and neither SSH nor an interactive login path is available.

Typical flow from within Pi OS:

```bash
# Copy the freshly built kernel onto the Pi by whatever path is available.
# Example destination:
cp /path/to/slmos.bin /mnt/slmdata/slmos.bin.new

# Then run the local updater.
sudo /usr/local/sbin/slmos-update
```

Typical updater behavior:

- mounts the Pi boot partition
- preserves the previous `kernel_2712.img`
- installs the new SLM-OS kernel
- verifies the copy
- syncs before returning

If you cannot reach the Pi OS maintenance install, fall back to the host-driven [Kernel update](#kernel-update) path above.

## In-place update from a running SLM-OS

If SLM-OS is already running and reachable over the multi-session
shell (telnet on port 2323), the kernel can be replaced without
touching the host or the maintenance OS. The flow uses the
`kernel` admin command surface and the Pi 5 tryboot one-shot
mechanism:

```bash
# From host: upload the new image to /mnt/files
scripts/tools/slm-put.py pi-5-1 build/kernel/slmos.bin /mnt/files/slmos.bin

# In the SLM-OS shell over telnet:
slmos> kernel stage /mnt/files/slmos.bin     # → 0:/slmstore/staged.img
slmos> kernel activate                        # arm tryboot one-shot
slmos> reboot
# Pi firmware loads tryboot.txt instead of config.txt for ONE boot.
# After SLM-OS comes back up:
slmos> kernel status                          # confirm the staged image booted
slmos> kernel promote                         # copy staged.img → kernel_2712.img
# OR, if anything looked wrong:
slmos> kernel rollback                        # clear flag + remove staged.img
```

This requires `tryboot.txt` to be present on the FAT boot
partition (the first-time provisioning step above installs it
from `deploy/pi5/tryboot.txt`). See
`docs/archive/plans/dynamic-kernel-replace-plan.md` for the design and
`docs/archive/test-runs/pi5-stage-promote-rollback-verification.md` for a full
hardware round-trip log.

---

## Reverting to Raspberry Pi OS

If the card was provisioned via the first-time path above, the backup files are still in place. Restore them:

```bash
DEV=/dev/sdX1
MNT=$(mktemp -d)
sudo mount "$DEV" "$MNT"
sudo cp -a "$MNT/config.txt.pios-bak"       "$MNT/config.txt"
sudo cp -a "$MNT/cmdline.txt.pios-bak"      "$MNT/cmdline.txt"
sudo cp -a "$MNT/kernel_2712.img.pios-bak"  "$MNT/kernel_2712.img"
sync
sudo umount "$MNT"
rmdir "$MNT"
```

The rootfs (`/dev/sdX2`) was never modified, so the resulting card boots Pi OS Lite normally.

If the card uses the maintenance layout above, `rootfs` remains available as a recovery environment even after many SLM-OS kernel updates.

---

## Troubleshooting

### Silence on serial, no output

1. Confirm the serial adapter is 3.3 V and wired TX↔RX, RX↔TX (not TX↔TX).
2. Confirm `pciex4_reset=0`, `uart_2ndstage=1`, and `os_check=0` are all present in `config.txt`. These three together keep the firmware from tearing down PCIe/RP1 or refusing to load the kernel. Missing any one of them produces total UART silence — no kernel output, no firmware banner. See `docs/pi5-baremetal-status.md` §Configuration for the authoritative list.
3. Confirm the card is seated. A loose card prints nothing — there is no firmware-level "no card" error over UART.
4. Try `picocom -b 115200` on the adapter port *before* powering the Pi — a boot-log preamble from the Pi firmware (not SLM-OS) should appear within 2 s of power-on.

### Pi boots Raspberry Pi OS instead of SLM-OS

The most likely cause is that `kernel_2712.img` was not actually overwritten (wrong mount path, or the card was read-only). Re-check:

```bash
cmp build/kernel/slmos.bin /mnt/kernel_2712.img
```

If `cmp` reports "EOF on ...slmos.bin" after N bytes, the kernel was only partially written — retry with an explicit `sync` before unmounting.

### Boot hangs after firmware banner

If the Pi firmware banner appears but SLM-OS never prints, the most common causes are:

1. **EEPROM too new.** Firmware past the Sep 2024 build has changed the RP1 initialization sequence and breaks the bare-metal UART driver. See `docs/pi5-baremetal-status.md`.
2. **Stale initramfs loaded.** If `auto_initramfs=0` is missing from `config.txt`, the firmware tries to load `initramfs_2712` over the kernel load address and corrupts it.

### "Permission denied" mounting the card

The card was not ejected cleanly. Run `sudo umount /dev/sdX*` to force-release it, re-insert, and retry.

---

## Related documents

- `docs/pi5-baremetal-status.md` — EEPROM firmware compatibility and RP1 UART status
- `docs/uart-hardware.md` — Pi 5 RP1 UART pinout and register details
- `docs/deploy/pi5-sdwire.md` — Lab-managed (non-sneakernet) deploy path
- `docs/lab-operations.md` — Power, serial, and labctl reference
