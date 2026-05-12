#!/bin/bash
# Force-bind hailo_pci to the QEMU `edu` test device.
#
# The `edu` device has PCI vendor:device 0x1234:0x11e8 — not Hailo's
# 0x1e60:0x2864. hailo_pci's id_table only matches Hailo IDs, so without help it
# never probes. The kernel's `new_id` sysfs entry lets us register an extra
# (vendor, device) pair against the driver at runtime and trigger probe.
#
# This is the Task 0.3 placeholder strategy: the edu device's BAR0 R/W returns
# zero for unwritten offsets, which is the "stub-stub" behavior #795's plan calls
# for. The corpus produced under this binding is not a useful protocol artifact —
# its only purpose is to validate that the capture pipeline produces a
# deterministic MMIO stream end-to-end. Replaced by Task 0.2's real stub on
# integration.

set -euo pipefail

HAILO_VENDOR=1e60
HAILO_DEVICE=2864
EDU_VENDOR=1234
EDU_DEVICE=11e8

# The Hailo PCIe driver ships as kernel module `hailo_pci` (modinfo / dkms name)
# but registers itself as PCI driver `hailo` in sysfs (per `DRIVER_NAME` in
# hailo's common/pcie_common.h). The module and driver names differ; this
# script uses the sysfs name.
DRIVER_SYSFS=/sys/bus/pci/drivers/hailo

# 1. Load hailo_pci. With the custom QEMU `hailo-stub-stub` device advertising
#    the Hailo IDs (0x1e60:0x2864), the kernel's PCI subsystem auto-probes via
#    hailo_pci's id_table — no /sys/bus/pci/drivers/hailo/new_id trick needed.
#
#    support_soft_reset=0 makes load_nnc_firmware early-return when the stub's
#    is_firmware_loaded magic register reports loaded. With the default
#    (support_soft_reset=1), the driver would try to soft-reset the device
#    instead, which requires far more magic in the stub.
modprobe hailo_pci support_soft_reset=0 || {
    echo "hailo_pci modprobe failed; dmesg tail:" >&2
    dmesg | tail -30 >&2
    exit 1
}

# Diagnostics, always-on for now.
echo "lsmod | grep hailo:"
lsmod | grep -i hailo || true
echo "PCI devices with Hailo IDs:"
lspci -d "${HAILO_VENDOR}:${HAILO_DEVICE}" || true

if [[ ! -d "${DRIVER_SYSFS}" ]]; then
    echo "Expected PCI driver sysfs path ${DRIVER_SYSFS} not present after modprobe hailo_pci." >&2
    echo "Available PCI drivers:" >&2
    ls /sys/bus/pci/drivers/ >&2
    echo "dmesg tail:" >&2
    dmesg | tail -30 >&2
    exit 1
fi

# 2. Fallback (placeholder-of-placeholder): if the QEMU device exposes the edu
#    IDs instead of Hailo IDs (because the custom QEMU wasn't built), force-bind
#    via new_id. Auto-detect by checking lspci.
if ! lspci -d "${HAILO_VENDOR}:${HAILO_DEVICE}" | grep -q .; then
    echo "No Hailo-IDed device on PCI bus. Falling back to edu force-bind." >&2
    if lspci -d "${EDU_VENDOR}:${EDU_DEVICE}" | grep -q .; then
        echo "${EDU_VENDOR} ${EDU_DEVICE}" > "${DRIVER_SYSFS}/new_id"
    else
        echo "No edu device either. Capture cannot proceed." >&2
        lspci -nn >&2
        exit 1
    fi
fi

# 3. Wait briefly for /dev/hailo0 to appear. udev creates it from the cdev the
#    driver publishes on probe.
for _ in $(seq 1 50); do
    if [[ -e /dev/hailo0 ]]; then
        ls -l /dev/hailo0
        echo "Bound devices under driver:"
        ls -l "${DRIVER_SYSFS}/" | grep '^l' || true
        exit 0
    fi
    sleep 0.1
done

echo "Probe completed but /dev/hailo0 never appeared. State dump:" >&2
echo "  driver sysfs:" >&2
ls -la "${DRIVER_SYSFS}/" >&2
echo "  dmesg tail:" >&2
dmesg | tail -50 >&2
exit 1
