# test-pc: Ubuntu 24.04 LTS + VFIO passthrough for GSP development

One-time setup to make test-pc a fast iteration environment for GPU
compute work (Phase E / GSP-RM bringup). Dual-use with bare-metal
SLM-OS via labctl SD-wire — the SD card is a separate boot device, so
flipping between Linux and SLM-OS doesn't disturb either install.

**Target:** Gigabyte H610M S2H V2 + i7-6700 + NVIDIA RTX 3050 (GA107).
VT-d must be ON in UEFI — confirmed already done.

**Goal:** When this guide finishes, the RTX 3050 is bound to `vfio-pci`
instead of `nouveau` / `nvidia`. Linux does NOT touch the GPU past
UEFI POST. Our userspace harness (`host-tools/gsp-harness/`) can then
`mmap` BAR0 and BAR1 and run the same GSP-RM bringup code that SLM-OS
would run bare-metal.

**Automation:** Sections §2, §4, and §6 are packaged as
[`test-pc-vfio-postinstall.sh`](./test-pc-vfio-postinstall.sh) —
run it as root after a fresh Ubuntu Desktop install to apply the
VFIO config, IOMMU cmdline, and serial console in one shot. The
manual steps below remain the reference.

---

## 1. Install Ubuntu 24.04 LTS (on the external SSD)

test-pc now has three storage devices:

- **Internal disk** — carries an older SLM-OS install. Untouched.
- **SD-wire SD card** — labctl's managed SD for bare-metal SLM-OS
  deployment. Untouched.
- **External SSD** — new, for Linux. **This is the install target.**

Standard Ubuntu desktop install targeting the external SSD:

- Ubuntu desktop ISO, current 24.04.x point release.
- At partitioning, select **"Something else"** (not "Erase disk and
  install Ubuntu"). Explicitly pick the external SSD — typically
  `/dev/sdb` or `/dev/nvme1n1`, double-check with `lsblk` at the
  installer's terminal before confirming. It's the one with no
  existing ESP + SLM-OS signature.
- Install the GRUB bootloader **to the external SSD**, not the
  internal disk. The installer's bootloader-location dropdown
  defaults to the first disk; change it explicitly.
- At first boot, pick the external SSD in BIOS boot order (or via
  F12/F11 boot menu each time — depends on your preference for
  day-to-day default).
- Use nouveau / Xorg defaults at install time — you need a GUI for
  the install UI. Drivers get blacklisted in §4 after Linux is
  running.

After install, log in and update:

```bash
sudo apt update && sudo apt full-upgrade -y
sudo apt install -y build-essential gcc pkg-config linux-headers-$(uname -r) \
                    pciutils dkms zstd
```

Keep the `libnvidia-compute-535` package that already ships — we need
the firmware blobs from it.

---

## 2. Confirm IOMMU and VT-d are live

```bash
dmesg | grep -i -e "dmar\|iommu"
```

Expected lines (exact wording varies by kernel version):

```
DMAR: IOMMU enabled
DMAR: Host address width 39
DMAR: DRHD base: 0x000000fed90000 flags: 0x1
```

If `IOMMU enabled` is missing, edit `/etc/default/grub` and append
`intel_iommu=on iommu=pt` to `GRUB_CMDLINE_LINUX_DEFAULT`, then
`sudo update-grub && sudo reboot`.

`iommu=pt` enables IOMMU passthrough mode — the CPU's DMA traffic
bypasses IOMMU translation (fast), but devices bound to vfio-pci still
go through it. That's what we want.

---

## 3. Identify the RTX 3050

```bash
lspci -nn | grep -i nvidia
```

Expected:

```
01:00.0 VGA compatible controller [0300]: NVIDIA Corporation GA107 [GeForce RTX 3050] [10de:2584] (rev a1)
01:00.1 Audio device [0403]: NVIDIA Corporation GA107 High Definition Audio Controller [10de:228e] (rev a1)
```

Two functions share the IOMMU group — GPU at `01:00.0` and its HDMI
audio at `01:00.1`. Both need to bind to vfio-pci together. Note the
PCI IDs: `10de:2584` for the GPU, `10de:228e` for the audio.

**Variant note — RTX 3050 6 GB (2024):** The newer 6 GB variant shares
the GA107 GPU device ID (`10de:2584`) but its HDMI audio function uses
a different ID: `10de:2291` instead of `10de:228e`. Always check the
`01:00.1` line on the installed card and adjust §4b accordingly, or
list both audio IDs in `vfio.conf` so the same config works across
variants (unused IDs are silently ignored by vfio-pci).

Check the IOMMU group (they MUST be alone or with each other only):

```bash
for d in /sys/kernel/iommu_groups/*/devices/*; do
    n=$(basename "$(dirname "$(dirname "$d")")")
    echo "Group $n: $(basename "$d")"
done | sort -n -k2 | grep "01:00"
```

If the GPU group contains other unrelated devices, PCIe ACS patching
via `pcie_acs_override` may be needed. On the H610M chipset this is
usually clean — the GPU sits in its own group.

---

## 4. Bind the GPU to vfio-pci at boot

Two-step: blacklist the default drivers, then tell the initrd to
bind vfio-pci first.

### 4a. Blacklist nouveau and nvidia

```bash
sudo tee /etc/modprobe.d/blacklist-nvidia.conf <<'EOF'
blacklist nouveau
blacklist nvidia
blacklist nvidia_drm
blacklist nvidia_modeset
blacklist nvidia_uvm
options nouveau modeset=0
EOF
```

### 4b. Tell vfio-pci which PCI IDs to claim

```bash
sudo tee /etc/modprobe.d/vfio.conf <<'EOF'
# Grab the RTX 3050 (10de:2584) and its HDMI audio
# before nouveau/nvidia can touch them.
# Both 228e (8 GB) and 2291 (6 GB) audio IDs are listed so this
# config is variant-agnostic; vfio-pci ignores IDs not present.
options vfio-pci ids=10de:2584,10de:228e,10de:2291
EOF
```

### 4c. Load vfio-pci early

```bash
sudo tee /etc/modules-load.d/vfio.conf <<'EOF'
vfio
vfio_iommu_type1
vfio_pci
EOF
```

### 4d. Rebuild the initrd + GRUB

```bash
sudo update-initramfs -u
sudo update-grub
sudo reboot
```

---

## 5. Verify vfio-pci owns the GPU

After reboot:

```bash
lspci -k -s 01:00.0
```

Expected:

```
01:00.0 VGA compatible controller: NVIDIA Corporation GA107 ...
        Kernel driver in use: vfio-pci
        Kernel modules: nouveau
```

The key line is `Kernel driver in use: vfio-pci`. If it says `nouveau`
instead, the blacklist didn't take — re-check `/etc/modprobe.d/`
files and rebuild initrd.

Also confirm BAR0/BAR1 are readable from userspace:

```bash
ls -la /sys/bus/pci/devices/0000:01:00.0/resource{0,1}
sudo cat /sys/bus/pci/devices/0000:01:00.0/resource | head -2
```

You should see `resource0` (~16 MB, BAR0 MMIO) and `resource1`
(~256 MB, BAR1 VRAM aperture).

---

## 6. Serial console setup (optional but recommended)

If you want to headless-drive test-pc during GSP iteration:

```bash
sudo tee -a /etc/default/grub <<'EOF'

# Serial console on ttyS0, 115200 baud — mirrors the SLM-OS serial
# config so the same labctl capture setup works for both OSes.
GRUB_TERMINAL="console serial"
GRUB_SERIAL_COMMAND="serial --unit=0 --speed=115200"
EOF
```

Append `console=tty0 console=ttyS0,115200n8` to
`GRUB_CMDLINE_LINUX_DEFAULT` in the same file, then
`sudo update-grub && sudo reboot`.

---

## 7. Running the harness

Once `vfio-pci` owns the GPU, clone/pull the SLM-OS tree and build:

```bash
cd SLM-OS
make gsp-harness PLATFORM=X86_64
sudo ./build/host-tools/gsp-harness --probe
```

Expected:

```
[GSP-HARNESS] BAR0 mapped at 0x7f8e... size 16777216
[GSP-HARNESS] BAR1 mapped at 0x7f8d... size 268435456
[GSP] firmware loaded (version 535.113.01):
[GSP]   gsp: 38061600 bytes
[GSP]   bootloader: 20588 bytes
[GSP]   booter_load: 59768 bytes
[GSP]   booter_unload: 39544 bytes
[GSP-HARNESS] BOOT_42: 0x177A1000 → Ampere, chip 0x177, rev 10.1
[GSP-HARNESS] engine registers: 0xBADF5040 (GSP not loaded yet — expected)
```

That's the baseline state. From there the harness can attempt each
GSP boot phase with immediate feedback — no reboots needed unless the
Falcon ucode hangs the device (recovery: `echo 1 > /sys/bus/pci/.../reset`).

---

## 8. Switching back to SLM-OS

Three disks, one active at a time:

- To run Linux: boot order prefers the external SSD (or press F12
  at POST and pick it). Linux GRUB → Ubuntu.
- To run bare-metal SLM-OS: `labctl sdwire_to_dut` + power-cycle.
  BIOS/UEFI sees the SD-wire card; our UEFI disk image's GRUB →
  SLM-OS kernel.
- The internal disk (old SLM-OS) is never touched. If BIOS prefers
  it, deselect it in the boot order — this setup intentionally
  leaves it dormant.

The three installs don't see each other: Linux never mounts the SD
card (labctl owns it), SLM-OS never mounts the external SSD (no
filesystem driver for ext4), and the internal disk is inert until
BIOS is told otherwise.

---

## Troubleshooting

**"`lspci -k` still shows nouveau after reboot"**
The module blacklist applies to runtime module-loading, but nouveau
may be built into the initrd. Confirm by:
`lsinitramfs /boot/initrd.img-$(uname -r) | grep -i nouveau`.
If found, rebuild initrd after re-checking `/etc/modprobe.d/blacklist-nvidia.conf`.

**"FLR (Function Level Reset) doesn't recover a hung GSP Falcon"**
Some Falcon hangs require a PCIe power cycle, not just FLR. Use
`sudo sh -c 'echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove && echo 1 > /sys/bus/pci/rescan'`
to force-remove + rescan. Worst case, `sudo reboot`.

**"IOMMU group has unrelated devices"**
The H610M's PCIe topology usually keeps the x16 slot isolated, but if
not, add `pcie_acs_override=downstream,multifunction` to
`GRUB_CMDLINE_LINUX_DEFAULT` (requires a patched kernel on some
distros; Ubuntu 24.04's mainline kernel has the override). Alternative:
use an older kernel without ACS enforcement.

**"vfio-pci binds but `resource0` is zero-length"**
Check BIOS: "Above 4G decoding" must be enabled. Without it, the GPU's
BARs don't get a 64-bit address and Linux can't map them.
