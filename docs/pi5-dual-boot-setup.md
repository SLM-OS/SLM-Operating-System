# Pi 5 dual-boot setup: SLM-OS + Raspberry Pi OS on one SD card

This guide describes the **maintenance-OS dual-boot model** for Pi 5:
a single SD card that can boot either SLM-OS (bare metal) or Raspberry
Pi OS Lite (Linux, via `tryboot`) without rewriting the card each time.
This model is especially useful on boards **without SDWire**, but it is
also valid on SDWire-equipped boards when you want a local Linux
environment on the same card for recovery or instrumentation.

It is complementary to the SDWire-first model in
[`docs/deploy/pi5-sdwire.md`](deploy/pi5-sdwire.md); the project should
preserve both.

---

## TL;DR

SSH into Pi OS (once set up): `ssh pi@192.168.4.25` password `slmos`.

Switch from Pi OS to SLM-OS for one boot:
```bash
sudo reboot 0 tryboot
```

Switch back: any plain power-cycle returns to the default, which is
whichever OS `[all]` points at in `autoboot.txt` on sdc1 (SLMOS
partition). Default today is Pi OS; flip the numbers in that file
to swap defaults.

---

## Partition layout

A 29 GB SLMOS-labelled card carries **three** MBR primary partitions:

| # | Label | Filesystem | Size | Purpose |
|---|-------|------------|------|---------|
| 1 | `SLMOS`     | FAT32 | 7.5 GB  | Pi 5 firmware (`start.elf`, `bootcode.bin`, device trees), SLM-OS's `kernel_2712.img`, any MNIST/HEF files loaded at runtime, and most importantly `autoboot.txt` |
| 2 | `PIOS_BOOT` | FAT32 | 7.4 GB  | Pi OS's `/boot/firmware/` — kernel8, kernel_2712.img (the Pi OS one, distinct from SLM-OS's), initramfs, overlays, cmdline.txt, config.txt |
| 3 | `PIOS_ROOT` | ext4  | 14.2 GB | Pi OS Debian 13 Lite rootfs |

Partition 1 is intentionally oversized (most of the SLM-OS boot files
total less than 50 MB) because it was created from the original
"SLM-OS-only" single-partition card. Space can be reclaimed later
without rebuilding the other partitions.

---

## How `autoboot.txt` + `tryboot` selects the OS

The Pi 5 bootloader (CM EEPROM) reads `autoboot.txt` from the first FAT32
partition **before** it picks which `config.txt`/`kernel_2712.img` to
load. Our file on sdc1 looks like:

```ini
[all]
boot_partition=2

[tryboot]
boot_partition=1
```

- **Normal power-on**: bootloader applies the `[all]` section →
  `boot_partition=2` → reads `/config.txt` and `/kernel_2712.img` from
  sdc2 → Pi OS boots.
- **Tryboot power-on**: bootloader applies the `[tryboot]` section →
  `boot_partition=1` → SLM-OS boots.

The "tryboot flag" is set via a Linux syscall
(`reboot(LINUX_REBOOT_CMD_RESTART2, "tryboot")`). From Pi OS shell:

```bash
sudo reboot 0 tryboot
```

`0` is the reboot magic (legacy, ignored on modern kernels); `tryboot` is
the argument consumed by the Pi 5 firmware on the next reboot. It is
**one-shot**: after that next boot, the flag clears automatically and the
subsequent power-on follows `[all]` again.

SLM-OS is bare-metal and cannot issue `reboot(... "tryboot")`, so there's
no in-SLM-OS command to hop back to Pi OS — a plain power-cycle does
that automatically because SLM-OS is the `[tryboot]` target, not the
`[all]` default.

**If you want SLM-OS as the default instead** (because you're doing
heavy bare-metal iteration and only occasionally visit Pi OS): swap
the `boot_partition` numbers in `autoboot.txt` on sdc1. Then
`reboot 0 tryboot` from Pi OS becomes the path that gets you back
to Pi OS for one boot, and plain power-on lands on SLM-OS. To edit
the file, either SSH into Pi OS and mount sdc1 (FAT32 at
`/dev/mmcblk0p1`), or `labctl sdwire_to_host pi-5-1` and edit from
the dev host.

---

## Using Pi OS for debugging / instrumentation

Having a native Linux environment on the same hardware is handy for:

- Running HailoRT (`hailortcli`) against the AI HAT+ for a known-good
  reference to compare against SLM-OS's own implementation.
- Instrumenting the `hailo_pci` DKMS driver with `pr_info` hooks to
  capture VDMA wire traces (see `../slmos-reference-cache/derivatives/hailort-traces/hailort-v4.23.0-vdma-mnist-pi5.txt`).
- Booting `rpi-eeprom-config` / `vcgencmd` to inspect firmware state.
- Running packet captures on the PCIe link via whatever tooling is
  convenient under Linux.

The Hailo-8L is visible as a PCIe device out of the box once
`dtparam=pciex1` is set in Pi OS's `config.txt`:

```
0001:01:00.0 Co-processor: Hailo Technologies Ltd. Hailo-8 AI Processor (rev 01)
```

To get `/dev/hailo0` and the `hailortcli` binary, install Hailo's
packages — typically the `hailo-all` metapackage or the individual
`.deb`s from Hailo's developer portal (the packaging changes from
release to release; check their site when needed).

---

## Creating a dual-boot card from scratch

These steps assume you have a SLMOS-only card that you want to turn
into a dual-boot card. If the board has SDWire, use that to expose the
card to the host. If the board has no SDWire, do the same partitioning
and copy steps through the host's SD-card reader or from the Linux
maintenance OS already on the board.

### 0. Get the card onto the host

```bash
labctl power_off pi-5-1
labctl sdwire_to_host pi-5-1
lsblk -o NAME,SIZE,FSTYPE,LABEL,PARTUUID
# Find the SLMOS card — it should show up as a ~29 GB disk with a
# FAT32 partition labelled SLMOS. Note the /dev/sdX identifier.
```

Verify the device identifier matches what labctl expects (multiple
SDWires exist in the lab; the label is the safest confirmation).

### 1. Shrink the original SLMOS partition and create two new ones

This assumes the starting card has a single partition filling most of
the card. If the SLMOS partition already leaves tail free-space (as
it does on today's card), skip the shrink step.

```bash
# Shrink sdc1 to ~512 MB (adjust if SLMOS files grow)
sudo umount /dev/sdc1 2>/dev/null || true
sudo fsck.vfat -f /dev/sdc1          # clean filesystem before resize
sudo fatresize -s 512M /dev/sdc1     # or reformat fresh after backup
sudo parted /dev/sdc resizepart 1 513MiB

# Create PIOS_BOOT (FAT32) and PIOS_ROOT (ext4) in the newly freed tail
sudo parted -a optimal /dev/sdc mkpart primary fat32 513MiB  1025MiB
sudo parted -a optimal /dev/sdc mkpart primary ext4  1025MiB 100%
sudo mkfs.vfat -F 32 -n PIOS_BOOT /dev/sdc2
sudo mkfs.ext4 -L PIOS_ROOT /dev/sdc3
```

### 2. Populate the Pi OS partitions from the official image

```bash
# One-time download + decompress
wget https://downloads.raspberrypi.com/raspios_lite_arm64_latest
xz -dc raspios_lite_arm64_latest > raspios-lite.img

# Loop-mount the image's two partitions
LOOPDEV=$(sudo losetup -fP --show raspios-lite.img)
sudo mkdir -p /mnt/piosimg_boot /mnt/piosimg_root
sudo mount ${LOOPDEV}p1 /mnt/piosimg_boot
sudo mount ${LOOPDEV}p2 /mnt/piosimg_root

# Mount our targets
sudo mkdir -p /mnt/sdc2 /mnt/sdc3
sudo mount /dev/sdc2 /mnt/sdc2
sudo mount /dev/sdc3 /mnt/sdc3

# Boot partition: plain cp is fine (FAT32, no special bits)
sudo cp -av /mnt/piosimg_boot/. /mnt/sdc2/

# Rootfs: MUST use rsync with -aHAX --numeric-ids, NOT plain cp.
# Without these flags the setuid bits on sudo/passwd/su are dropped
# and the system ends up with non-functional privilege escalation.
sudo rsync -aHAX --numeric-ids /mnt/piosimg_root/ /mnt/sdc3/
sync
```

**Important: the `rsync -aHAX --numeric-ids` flags are load-bearing.**
This was a ~30 minute detour during the first build of this card.
Verify with:

```bash
sudo find /mnt/sdc3 -perm -4000 -type f | wc -l   # should be ~16
sudo ls -la /mnt/sdc3/usr/bin/sudo                # should show -rwsr-xr-x
```

If the count is 0 or `sudo` lacks the `s` bit, the rootfs is broken —
re-run rsync with the correct flags.

### 3. Rewrite PARTUUIDs in the Pi OS configs

The image carries its original PARTUUIDs (`7d23366b-01/02` on recent
releases). Our card has different ones. Patch them:

```bash
# Grab the real PARTUUIDs from our card
sudo blkid /dev/sdc2 /dev/sdc3
# Example output:
#   /dev/sdc2: ... PARTUUID="1d0c498d-02"
#   /dev/sdc3: ... PARTUUID="1d0c498d-03"

# Update cmdline.txt (root=PARTUUID points at sdc3)
sudo sed -i 's/7d23366b-02/1d0c498d-03/' /mnt/sdc2/cmdline.txt

# Update /etc/fstab (boot + root PARTUUIDs)
sudo sed -i \
  -e 's|PARTUUID=7d23366b-01|PARTUUID=1d0c498d-02|' \
  -e 's|PARTUUID=7d23366b-02|PARTUUID=1d0c498d-03|' \
  /mnt/sdc3/etc/fstab
```

Without this step, Pi OS kernel panics on "VFS: Cannot open root device"
and you're stuck debugging on silent serial.

### 4. Pi OS config.txt additions for the lab

The Pi OS image's default `config.txt` doesn't enable the GPIO UART or
the external PCIe root complex. Append:

```bash
sudo tee -a /mnt/sdc2/config.txt > /dev/null <<'EOF'

# --- SLM-OS lab additions (labctl serial + AI HAT+) ---
enable_uart=1       # console on GPIO14/15 (wired to CP2102 on pi-5-1)
uart_2ndstage=1     # bootloader prints to UART too
dtparam=pciex1      # external PCIe root complex (AI HAT+ slot)
EOF
```

Without `enable_uart=1` the Linux kernel boots silently from labctl's
point of view — the on-SoC "debug UART" on the tiny header is
initialized, but the 40-pin GPIO UART the lab's CP2102 reads is not.

### 5. Enable SSH + create the `pi` user for first boot

```bash
# Empty marker file — Pi OS's init script enables and starts sshd when
# it sees this, then removes it.
sudo touch /mnt/sdc2/ssh

# userconf.txt creates the `pi` user with the given hashed password on
# first boot. The line format is `pi:<crypt(3) SHA-512 hash>`.
HASHED=$(echo 'slmos' | openssl passwd -6 -stdin)
echo "pi:${HASHED}" | sudo tee /mnt/sdc2/userconf.txt > /dev/null
```

If you want a different password, change `'slmos'` above. If you want
SSH key auth instead, drop an `authorized_keys` file into
`/mnt/sdc3/home/pi/.ssh/` and make sure it's mode `0600` owned by uid
1000 (the pi user).

Both the `ssh` marker and `userconf.txt` are **one-shot** — Pi OS
consumes them on first boot. If you re-populate the rootfs from the
image later, you have to recreate them.

### 6. Write `autoboot.txt` to sdc1

```bash
sudo mkdir -p /mnt/sdc1
sudo mount /dev/sdc1 /mnt/sdc1
sudo tee /mnt/sdc1/autoboot.txt > /dev/null <<'EOF'
[all]
boot_partition=2

[tryboot]
boot_partition=1
EOF
```

(Swap the numbers if you want SLM-OS as the default.)

### 7. Clean up and hand the card back

```bash
sync
sudo umount /mnt/sdc1 /mnt/sdc2 /mnt/sdc3 \
           /mnt/piosimg_boot /mnt/piosimg_root
sudo losetup -d "$LOOPDEV"

labctl sdwire_to_dut pi-5-1
labctl power_on pi-5-1
labctl serial_capture pi-5-1 --timeout 60 --until_pattern "raspberrypi login:"
```

On first boot, Pi OS:
1. Processes the `ssh` marker and enables sshd.
2. Processes `userconf.txt` and creates the `pi` user with your password.
3. Resizes the root filesystem to fill the partition if the `resize`
   cmdline flag is present (harmless even if the partition already fills).

You should see `raspberrypi login:` on serial within ~30 s of power-on,
and the system should announce an IP address from DHCP (typically
`192.168.4.25` in our lab).

---

## Gotchas we've hit

In order of how much time each one cost:

1. **Rsync without setuid-preserving flags.** `sudo cp -a` or `rsync -a`
   alone drops the setuid bits in most ext4→ext4 copies. Symptoms:
   `sudo: /usr/bin/sudo must be owned by uid 0 and have the setuid bit
   set` from any user. Fix: `rsync -aHAX --numeric-ids`. Audit with
   `find ... -perm -4000 -type f | wc -l` (expect ~16 on a fresh
   Pi OS Lite).
2. **Stale PARTUUIDs from the image.** `cmdline.txt` and `/etc/fstab`
   reference the image's original PARTUUIDs. Symptom: kernel panic
   "VFS: Cannot open root device". Fix: `sed` the actual PARTUUIDs
   from `blkid /dev/sdc2 /dev/sdc3`.
3. **`enable_uart=1` missing from Pi OS `config.txt`.** Pi OS Lite
   ships with UART disabled on the 40-pin GPIO header by default.
   Symptom: zero serial output from labctl even though the Pi is
   clearly booting (CPU gets warm, activity LED blinks). Fix: add
   `enable_uart=1` to `[all]` in `config.txt`.
4. **`dtparam=pciex1` missing.** Without it Pi OS's pcie1 controller
   is left in reset and `lspci` doesn't show the Hailo-8. Symptom:
   AI HAT+ invisible under Linux despite working under SLM-OS.
5. **One-shot marker files.** Both `ssh` and `userconf.txt` are
   consumed by firstboot services — they *should* disappear after
   the first successful Pi OS boot. Don't panic if you remount the
   card post-boot and find them gone.
6. **`apt install hailo-all` will silently auto-update the Pi 5
   EEPROM** to a recent (post-Jan 2025) bootloader, which then
   breaks SLM-OS's RP1 UART access (kernel boots silently — no
   serial output even though it's running). See
   `docs/pi5-baremetal-status.md:402-408` for the EEPROM-firmware
   investigation. The fix dance, in order:
   - Re-flash Sep 2024 image:
     ```bash
     wget -O /tmp/pieeprom.bin \
       https://raw.githubusercontent.com/raspberrypi/rpi-eeprom/master/firmware-2712/old/default/pieeprom-2024-09-23.bin
     sudo rpi-eeprom-update -d -f /tmp/pieeprom.bin
     sudo reboot
     ```
   - Verify with `vcgencmd bootloader_version` (should show
     `2024/09/23 14:02:56`).
   - Lock down so it doesn't re-upgrade on the next boot:
     ```bash
     sudo systemctl mask rpi-eeprom-update.service
     sudo apt-mark hold rpi-eeprom rpi-eeprom-images
     sudo mv /usr/lib/firmware/raspberrypi/bootloader-2712/default \
            /root/eeprom-backup-default
     sudo mkdir /usr/lib/firmware/raspberrypi/bootloader-2712/default
     ```
   The lockdown is already applied on `pi-5-1`'s current Pi OS
   install. If you re-build the card from scratch, do these steps
   immediately after `apt install hailo-all` succeeds and BEFORE
   the next reboot.

---

## Related files

- `.claude/worktrees/pi-5-ai-hat/` — where the SLM-OS build typically
  happens; the Pi OS card work was done from the same worktree.
- `../slmos-reference-cache/derivatives/hailort-traces/hailort-v4.23.0-vdma-mnist-pi5.txt` — Example wire
  trace captured by instrumenting `hailo_pci` on Pi OS and running
  an MNIST inference. Pattern for future debugging sessions.
- `docs/lab-operations.md` — general labctl workflow.
- `~/raspios-lite.img` — decompressed Pi OS Lite image, re-usable for
  rootfs re-rsync if the card gets wedged.
