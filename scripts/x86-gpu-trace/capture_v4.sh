#!/usr/bin/env bash
# capture_v4.sh — stop tracing as soon as 'nouveau ... GA107' appears in
# dmesg, BEFORE the VRAM enumeration / bulk BAR1 traffic kicks in and
# floods the buffer.
set -euo pipefail

BAR0_START=$(awk 'NR==1 {print $1}' /sys/bus/pci/devices/0000:01:00.0/resource)
BAR0_END=$(awk 'NR==1 {print $2}' /sys/bus/pci/devices/0000:01:00.0/resource)
BAR0_PHYS=$(printf '%x' $((BAR0_START)))
BAR0_SIZE=$(printf '%x' $((BAR0_END - BAR0_START + 1)))

# Require a clean starting state (vfio bound, nouveau not loaded)
if lsmod | grep -q "^nouveau "; then
    echo "nouveau already loaded — reboot first" >&2
    exit 1
fi
if [[ -L /sys/bus/pci/devices/0000:01:00.0/driver ]]; then
    DRV=$(basename $(readlink /sys/bus/pci/devices/0000:01:00.0/driver))
    if [[ $DRV != "vfio-pci" ]]; then
        echo "GPU bound to $DRV (expected vfio-pci) — reboot" >&2
        exit 1
    fi
    echo 0000:01:00.0 > /sys/bus/pci/drivers/vfio-pci/unbind
    sleep 1
fi

echo ">>> Pre-nouveau"
for off in 840100 8403f8 840040 840030 110100 0; do
    /root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" "$off"
done | tee /root/sec2-pre-nouveau.txt

# Mega buffer
cd /sys/kernel/debug/tracing
echo 0 > tracing_on
echo nop > current_tracer
echo 65536 > buffer_size_kb   # 64 MB/CPU × 8 CPU = 512 MB
echo > trace
echo mmiotrace > current_tracer

# Background dmesg watcher that stops tracing as soon as the GPU probe
# line appears. We also stop at any SEC2 / ACR / GSP message in case
# the GA107 line comes very late.
DMESG_START=$(dmesg | wc -l)
(
    while true; do
        if dmesg | tail -n +$DMESG_START | \
            grep -qE "nouveau.*GA107|sec2.*init|acr.*load|gsp.*init"; then
            echo 0 > /sys/kernel/debug/tracing/tracing_on
            exit 0
        fi
        sleep 0.02
    done
) &
WATCHER=$!

echo 1 > tracing_on
cd - >/dev/null

echo ">>> modprobe nouveau modeset=1 debug=all=debug"
timeout 15 modprobe nouveau modeset=1 debug=all=debug || echo "modprobe returned $?"

# Give watcher up to 2 s to catch the line
for i in 1 2 3 4 5 6 7 8 9 10; do
    if ! kill -0 $WATCHER 2>/dev/null; then break; fi
    sleep 0.2
done
# Force stop if watcher hasn't
if kill -0 $WATCHER 2>/dev/null; then
    echo 0 > /sys/kernel/debug/tracing/tracing_on
    kill $WATCHER 2>/dev/null || true
    wait $WATCHER 2>/dev/null || true
    echo ">>> forced trace stop"
else
    echo ">>> watcher stopped trace"
fi

cd /sys/kernel/debug/tracing
cp trace /root/nouveau-mmio.trace
echo nop > current_tracer
echo 1408 > buffer_size_kb
cd - >/dev/null

sleep 2
echo ">>> Post-nouveau"
for off in 840100 8403f8 840040 840030 110100 0; do
    /root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" "$off"
done | tee /root/sec2-post-nouveau.txt

dmesg | tail -n +$DMESG_START > /root/nouveau-dmesg.txt

echo ""
echo ">>> DONE"
wc -l /root/nouveau-mmio.trace /root/nouveau-dmesg.txt

echo ""
echo ">>> Header"
head -3 /root/nouveau-mmio.trace

echo ""
echo ">>> Counts by first token"
awk 'NR>12 {print $1}' /root/nouveau-mmio.trace | sort | uniq -c | sort -rn | head

echo ""
echo ">>> First + last timestamps (mmiotrace events)"
awk 'NR>12 && ($1=="W"||$1=="R"||$1=="MAP"||$1=="UNMAP"||$1=="UNKNOWN"){print $1, $2, $3}' \
    /root/nouveau-mmio.trace | head -1
awk 'NR>12 && ($1=="W"||$1=="R"||$1=="MAP"||$1=="UNMAP"||$1=="UNKNOWN"){print $1, $2, $3}' \
    /root/nouveau-mmio.trace | tail -1

echo ""
echo ">>> W-addresses by aperture (top 3 nibbles)"
grep '^W ' /root/nouveau-mmio.trace | awk '{print substr($5,1,6)}' | sort | uniq -c | sort -rn | head
