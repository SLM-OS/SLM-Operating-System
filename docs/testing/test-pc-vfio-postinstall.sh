#!/usr/bin/env bash
# test-pc VFIO post-install configuration.
#
# Run as root on test-pc after a fresh Ubuntu Desktop 24.04 install,
# targeting the external USB-chassis SSD. Applies guide sections
# §1 (dev tools + GSP firmware), §2 (IOMMU), §4 (VFIO binding), and
# §6 (serial console) in one shot.
#
# See test-pc-linux-vfio-setup.md for the full setup context.
#
# Usage:
#   sudo bash test-pc-vfio-postinstall.sh
#
# After it finishes, reboot. First boot with the new config should go
# directly to GDM without any kernel module touching the GPU — vfio-pci
# claims the RTX 3050 at initramfs time.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo bash $0" >&2
    exit 1
fi

echo "[1/7] Enabling multiverse + restricted repos (libnvidia-compute-535 lives here)..."
add-apt-repository -y multiverse restricted

echo "[2/7] apt update + install dev tools + GSP firmware blobs..."
apt update
apt install -y build-essential gcc pkg-config "linux-headers-$(uname -r)" \
               pciutils dkms zstd libnvidia-compute-535 openssh-server

echo "[3/7] Writing /etc/modprobe.d/blacklist-nvidia.conf (guide §4a)..."
printf 'blacklist nouveau\nblacklist nvidia\nblacklist nvidia_drm\nblacklist nvidia_modeset\nblacklist nvidia_uvm\noptions nouveau modeset=0\n' \
    > /etc/modprobe.d/blacklist-nvidia.conf

echo "[4/7] Writing /etc/modprobe.d/vfio.conf (guide §4b)..."
# PCI IDs that vfio-pci should claim ahead of nouveau / nvidia:
#   10de:2584 = NVIDIA GA107 GPU (RTX 3050, both 6 GB and 8 GB variants)
#   10de:228e = HDMI audio function on RTX 3050 8 GB (original)
#   10de:2291 = HDMI audio function on RTX 3050 6 GB (2024 variant)
# Both audio IDs are listed so this script is card-variant-agnostic;
# vfio-pci silently ignores IDs that don't match any present device.
printf 'options vfio-pci ids=10de:2584,10de:228e,10de:2291\n' \
    > /etc/modprobe.d/vfio.conf

echo "[5/7] Writing /etc/modules-load.d/vfio.conf (guide §4c)..."
printf 'vfio\nvfio_iommu_type1\nvfio_pci\n' \
    > /etc/modules-load.d/vfio.conf

echo "[6/7] Adding vfio modules to initramfs..."
grep -q '^vfio_pci$' /etc/initramfs-tools/modules \
    || printf '\n# VFIO passthrough\nvfio\nvfio_iommu_type1\nvfio_pci\n' \
       >> /etc/initramfs-tools/modules

echo "[7/7] Patching /etc/default/grub (IOMMU + serial console)..."
sed -i 's|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT="quiet splash intel_iommu=on iommu=pt console=tty0 console=ttyS0,115200n8"|' /etc/default/grub
sed -i '/^GRUB_TERMINAL=/d; /^GRUB_SERIAL_COMMAND=/d' /etc/default/grub
printf '\n# Serial console on ttyS0, 115200 baud (mirrors SLM-OS serial config)\nGRUB_TERMINAL="console serial"\nGRUB_SERIAL_COMMAND="serial --unit=0 --speed=115200"\n' \
    >> /etc/default/grub

echo "Regenerating initramfs + grub.cfg..."
update-initramfs -u -k all
update-grub

echo
echo "Done. Reboot to apply. After reboot, verify:"
echo "  lspci -k -s 01:00.0                   # Kernel driver in use: vfio-pci"
echo "  lspci -k -s 01:00.1                   # Kernel driver in use: vfio-pci"
echo "  sudo dmesg | grep -i iommu | head     # DMAR: IOMMU enabled"
echo "  ls /sys/bus/pci/devices/0000:01:00.0/resource{0,1}   # 16 MB + 256 MB"
