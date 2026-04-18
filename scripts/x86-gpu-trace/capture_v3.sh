#!/usr/bin/env bash
# capture_v3.sh — big buffer + short capture window approach.
# Fresh-boot assumption: nouveau not loaded, vfio-pci bound.
set -euo pipefail

BAR0_START=$(awk 'NR==1 {print $1}' /sys/bus/pci/devices/0000:01:00.0/resource)
BAR0_END=$(awk 'NR==1 {print $2}' /sys/bus/pci/devices/0000:01:00.0/resource)
BAR0_PHYS=$(printf '%x' $((BAR0_START)))
BAR0_SIZE=$(printf '%x' $((BAR0_END - BAR0_START + 1)))
echo "BAR0: phys=0x$BAR0_PHYS size=0x$BAR0_SIZE"

# Unbind vfio-pci
if [[ -L /sys/bus/pci/devices/0000:01:00.0/driver ]]; then
    DRV=$(basename $(readlink /sys/bus/pci/devices/0000:01:00.0/driver))
    echo ">>> Unbinding $DRV"
    echo 0000:01:00.0 > /sys/bus/pci/drivers/$DRV/unbind
    sleep 1
fi

# Pre-nouveau register snapshot
echo ">>> Pre-nouveau SEC2 state"
{
    echo "=== PRE-NOUVEAU ==="
    for off in 840100 8403f8 840040 840030 110100 0; do
        /root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" "$off"
    done
} | tee /root/sec2-pre-nouveau.txt

# Increase per-cpu buffer to 32 MB — ~8 CPUs x 32 MB = 256 MB total,
# enough for ~3M events.
cd /sys/kernel/debug/tracing
echo 0 > tracing_on
echo nop > current_tracer
echo 32768 > buffer_size_kb
echo > trace
echo mmiotrace > current_tracer

DMESG_START=$(dmesg | wc -l)

echo 1 > tracing_on
cd - >/dev/null

echo ">>> modprobe nouveau modeset=1"
timeout 15 modprobe nouveau modeset=1 || echo "modprobe returned $?"

# Stop tracing immediately — we don't need post-init noise
cd /sys/kernel/debug/tracing
echo 0 > tracing_on
echo ">>> Saving trace"
cp trace /root/nouveau-mmio.trace
echo nop > current_tracer
# Shrink buffer back to avoid leaving large RAM allocation
echo 1408 > buffer_size_kb
cd - >/dev/null

# Wait a beat for nouveau late init to settle, then snapshot post state
sleep 2
echo ">>> Post-nouveau SEC2 state"
{
    echo "=== POST-NOUVEAU ==="
    for off in 840100 8403f8 840040 840030 110100 0; do
        /root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" "$off" 2>&1
    done
} | tee /root/sec2-post-nouveau.txt

dmesg | tail -n +$DMESG_START > /root/nouveau-dmesg.txt

echo ""
echo ">>> DONE"
wc -l /root/nouveau-mmio.trace /root/nouveau-dmesg.txt
ls -lh /root/nouveau-mmio.trace

echo ""
echo ">>> Trace summary"
echo "Total W entries:    $(grep -c '^W ' /root/nouveau-mmio.trace || true)"
echo "Total R entries:    $(grep -c '^R ' /root/nouveau-mmio.trace || true)"
echo "SEC2 writes (BAR0+0x84xxxx):"
grep -c -E '^W [0-9]+ [0-9.]+ [0-9]+ 0x5384' /root/nouveau-mmio.trace || echo "  (none)"
echo "GSP writes (BAR0+0x11xxxx):"
grep -c -E '^W [0-9]+ [0-9.]+ [0-9]+ 0x5311' /root/nouveau-mmio.trace || echo "  (none)"
echo "PMC writes (BAR0+0x000xxx / 0x001xxx):"
grep -c -E '^W [0-9]+ [0-9.]+ [0-9]+ 0x5300[01]' /root/nouveau-mmio.trace || echo "  (none)"
echo "First buffer entry timestamp / last entry timestamp:"
grep -m1 '^W \|^R ' /root/nouveau-mmio.trace | awk '{print "  first: " $3}'
grep '^W \|^R ' /root/nouveau-mmio.trace | tail -1 | awk '{print "  last:  " $3}'
