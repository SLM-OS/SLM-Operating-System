#!/usr/bin/env bash
# capture.sh — Capture nouveau MMIO sequence that unlocks SEC2 on GA107.
# Run as root on test-pc via SSH.
#
# Outputs:
#   /root/sec2-pre-nouveau.txt    SEC2 register state before nouveau load
#   /root/sec2-post-nouveau.txt   SEC2 register state after nouveau load
#   /root/nouveau-mmio.trace      Full mmiotrace capture
#   /root/nouveau-dmesg.txt       dmesg between start and end

set -euo pipefail

BAR0_RES=$(awk 'NR==1 {print $1 " " $2}' /sys/bus/pci/devices/0000:01:00.0/resource)
BAR0_START=${BAR0_RES%% *}
BAR0_END=${BAR0_RES##* }
BAR0_PHYS=$(printf '%x' $((BAR0_START)))
BAR0_SIZE=$(printf '%x' $((BAR0_END - BAR0_START + 1)))
echo "BAR0: phys=0x$BAR0_PHYS size=0x$BAR0_SIZE"

# SEC2 Falcon regs (GA10x). Adjust if GA107 uses a different base.
SEC2_CPUCTL=840100
SEC2_HWCFG2=8403F8
SEC2_MAILBOX0=840040
SEC2_ENGINE=840030
GSP_CPUCTL=110100

peek_sec2() {
    local label=$1 outfile=$2
    {
        echo "=== $label ==="
        for off in $SEC2_CPUCTL $SEC2_HWCFG2 $SEC2_MAILBOX0 $SEC2_ENGINE $GSP_CPUCTL; do
            printf "  "
            ./sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" "$off" 2>&1
        done
    } | tee "$outfile"
}

echo ">>> Unbinding vfio-pci"
echo 0000:01:00.0 > /sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null || true
sleep 1

echo ">>> Pre-nouveau SEC2 state"
peek_sec2 "PRE-NOUVEAU" /root/sec2-pre-nouveau.txt

echo ">>> Arming mmiotrace"
cd /sys/kernel/debug/tracing
echo 0 > tracing_on
echo nop > current_tracer
echo > trace
# mmiotrace is a tracer, not an event — select it as current_tracer
echo mmiotrace > current_tracer
echo 1 > tracing_on

# record a dmesg marker so we can correlate
DMESG_START=$(dmesg | wc -l)

cd - >/dev/null
echo ">>> modprobe nouveau modeset=1"
modprobe nouveau modeset=1 || echo "modprobe returned $?"
sleep 6  # let late init settle

cd /sys/kernel/debug/tracing
echo 0 > tracing_on
cp trace /root/nouveau-mmio.trace
echo nop > current_tracer
cd - >/dev/null

echo ">>> Post-nouveau SEC2 state (reading via nouveau's BAR mapping — may conflict)"
# Note: BAR0 is now mapped by nouveau. /dev/mem mmap of the same physical
# range should still succeed (it's shared memory). If /dev/mem complains,
# we'll fall back to nouveau's debugfs.
peek_sec2 "POST-NOUVEAU" /root/sec2-post-nouveau.txt || \
    echo "WARN: peek failed post-nouveau — try debugfs"

echo ">>> Collecting dmesg delta"
dmesg | tail -n +$DMESG_START > /root/nouveau-dmesg.txt

echo ">>> DONE"
ls -lh /root/sec2-pre-nouveau.txt /root/sec2-post-nouveau.txt \
       /root/nouveau-mmio.trace /root/nouveau-dmesg.txt
wc -l /root/nouveau-mmio.trace
