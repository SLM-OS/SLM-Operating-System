#!/usr/bin/env bash
# capture_v2.sh — stream trace_pipe to disk so the ring buffer can't overflow.
#
# Also: stop tracing ~300 ms after modprobe starts — SEC2 unlock happens
# in the first handful of MMIO writes, and we don't need the rest of
# nouveau init.
set -euo pipefail

BAR0_RES=$(awk 'NR==1 {print $1 " " $2}' /sys/bus/pci/devices/0000:01:00.0/resource)
BAR0_START=${BAR0_RES%% *}
BAR0_PHYS=$(printf '%x' $((BAR0_START)))
BAR0_SIZE=$(printf '%x' $(( $(awk 'NR==1 {print $2}' /sys/bus/pci/devices/0000:01:00.0/resource) - BAR0_START + 1 )))
echo "BAR0: phys=0x$BAR0_PHYS size=0x$BAR0_SIZE"

# Make sure nouveau is not already loaded (in case of re-run)
if lsmod | grep -q "^nouveau "; then
    echo ">>> Unloading nouveau"
    rmmod nouveau || true
    sleep 1
fi

# Unbind vfio-pci if bound
if [[ -e /sys/bus/pci/devices/0000:01:00.0/driver ]]; then
    DRIVER=$(basename $(readlink /sys/bus/pci/devices/0000:01:00.0/driver))
    echo ">>> Unbinding $DRIVER"
    echo 0000:01:00.0 > /sys/bus/pci/drivers/$DRIVER/unbind || true
    sleep 1
fi

# Pre-nouveau register snapshot
echo ">>> Pre-nouveau SEC2 state"
{
    echo "=== PRE-NOUVEAU ==="
    for off in 840100 8403f8 840040 840030 110100 0; do
        /root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" "$off" 2>&1
    done
} | tee /root/sec2-pre-nouveau.txt

# Arm ftrace
cd /sys/kernel/debug/tracing
echo 0 > tracing_on
echo nop > current_tracer
echo > trace
echo mmiotrace > current_tracer

# Start trace_pipe reader in background — streams events straight to file
echo ">>> Starting trace_pipe reader"
cat trace_pipe > /root/nouveau-mmio.stream &
PIPE_PID=$!

# Brief settle
sleep 0.2
echo 1 > tracing_on
cd - >/dev/null

DMESG_START=$(dmesg | wc -l)

# Load nouveau — do not wait for settled state
echo ">>> modprobe nouveau modeset=1 (async)"
modprobe nouveau modeset=1 &
MODPROBE_PID=$!

# Capture window — kill trace reader after this elapses
# SEC2 unlock should happen within the first ~100 ms of init.
# 1.5s gives plenty of margin and keeps the trace manageable.
sleep 1.5

# Stop tracing + reader
cd /sys/kernel/debug/tracing
echo 0 > tracing_on
cd - >/dev/null
kill $PIPE_PID 2>/dev/null || true
wait $PIPE_PID 2>/dev/null || true

# Wait for modprobe to finish (may take longer than trace window)
wait $MODPROBE_PID 2>/dev/null || echo "modprobe still running or failed: $?"

# Post-nouveau register snapshot
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
wc -l /root/nouveau-mmio.stream /root/nouveau-dmesg.txt
ls -lh /root/nouveau-mmio.stream /root/sec2-pre-nouveau.txt /root/sec2-post-nouveau.txt /root/nouveau-dmesg.txt

# Quick sanity: count W/R events and check for the SEC2 unlock region
echo ""
echo ">>> Trace summary"
echo "Total W entries:    $(grep -c '^W ' /root/nouveau-mmio.stream || echo 0)"
echo "Total R entries:    $(grep -c '^R ' /root/nouveau-mmio.stream || echo 0)"
echo "SEC2 writes (BAR0+0x84xxxx):"
grep -c '^W [0-9] [0-9.]* [0-9] 0x5384' /root/nouveau-mmio.stream || echo "  (none)"
echo "Pre-SEC2 writes to PMC (BAR0+0x000xxx or 0x006xx):"
grep -c -E '^W [0-9] [0-9.]* [0-9] 0x5300(0|6)' /root/nouveau-mmio.stream || echo "  (none)"
