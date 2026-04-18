#!/usr/bin/env bash
# capture_v5.sh — stop on "fb: NNNN MiB" dmesg marker (happens after
# SEC2 unlock but before the worst of the VRAM init noise).
set -euo pipefail

BAR0_START=$(awk 'NR==1 {print $1}' /sys/bus/pci/devices/0000:01:00.0/resource)
BAR0_END=$(awk 'NR==1 {print $2}' /sys/bus/pci/devices/0000:01:00.0/resource)
BAR0_PHYS=$(printf '%x' $((BAR0_START)))
BAR0_SIZE=$(printf '%x' $((BAR0_END - BAR0_START + 1)))

if lsmod | grep -q "^nouveau "; then echo "nouveau loaded — reboot"; exit 1; fi
if [[ -L /sys/bus/pci/devices/0000:01:00.0/driver ]]; then
    echo 0000:01:00.0 > /sys/bus/pci/drivers/vfio-pci/unbind
    sleep 1
fi

echo ">>> Pre-nouveau"
for off in 840100 8403f8 840040 840030 110100 0; do
    /root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" "$off"
done | tee /root/sec2-pre-nouveau.txt

cd /sys/kernel/debug/tracing
echo 0 > tracing_on
echo nop > current_tracer
echo 131072 > buffer_size_kb   # 128 MB/CPU x 8 = 1 GB total
echo > trace
echo mmiotrace > current_tracer

DMESG_START=$(dmesg | wc -l)

# Also sample SEC2 CPUCTL from a background loop, so we can narrow the
# unlock time window by CPUCTL value transitions.
(
    while true; do
        VAL=$(/root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" 840100 2>/dev/null | awk '{print $2}')
        TS=$(awk '{print $1}' /proc/uptime)
        echo "$TS $VAL" >> /root/sec2-probe.log
        sleep 0.005
    done
) &
SAMPLER=$!

# Dmesg watcher — trip on "fb: NNNN MiB" which fires AFTER SEC2 unlock.
(
    while true; do
        if dmesg | tail -n +$DMESG_START | grep -qE "nouveau.*fb: [0-9]+ MiB"; then
            echo 0 > /sys/kernel/debug/tracing/tracing_on
            exit 0
        fi
        sleep 0.02
    done
) &
WATCHER=$!

echo 1 > tracing_on
cd - >/dev/null

echo ">>> modprobe nouveau modeset=1"
timeout 15 modprobe nouveau modeset=1 || echo "modprobe returned $?"

# Give watcher 3 s to catch it
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if ! kill -0 $WATCHER 2>/dev/null; then break; fi
    sleep 0.2
done
if kill -0 $WATCHER 2>/dev/null; then
    echo 0 > /sys/kernel/debug/tracing/tracing_on
    kill $WATCHER; wait $WATCHER 2>/dev/null || true
    echo ">>> watcher timed out, forced stop"
else
    echo ">>> watcher stopped trace"
fi

kill $SAMPLER 2>/dev/null || true
wait $SAMPLER 2>/dev/null || true

cd /sys/kernel/debug/tracing
cp trace /root/nouveau-mmio.trace
echo nop > current_tracer
echo 1408 > buffer_size_kb
cd - >/dev/null

sleep 2
echo ">>> Post-nouveau"
for off in 840100 8403f8 840040 840030 110100 0; do
    /root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" "$off" 2>&1
done | tee /root/sec2-post-nouveau.txt

dmesg | tail -n +$DMESG_START > /root/nouveau-dmesg.txt

echo ""
echo ">>> DONE"
wc -l /root/nouveau-mmio.trace /root/nouveau-dmesg.txt /root/sec2-probe.log

echo ""
echo ">>> SEC2 CPUCTL transition points in probe log"
awk '
BEGIN { last = "UNINIT" }
{
    if ($2 != last) {
        print "  " $1 ": " $2 (last=="UNINIT" ? " (first sample)" : " (from " last ")")
        last = $2
    }
}' /root/sec2-probe.log | head -30

echo ""
echo ">>> Header"
head -3 /root/nouveau-mmio.trace

echo ""
echo ">>> Any writes/accesses to SEC2 range 0x5384xxxx?"
grep -E '(^W|^R|^UNKNOWN) .* 0x5384' /root/nouveau-mmio.trace | awk '{print $1}' | sort | uniq -c
grep -E '(^W|^R|^UNKNOWN) .* 0x5384' /root/nouveau-mmio.trace | head -5
echo ""
echo ">>> Any access to GSP 0x5311xxxx?"
grep -E '(^W|^R|^UNKNOWN) .* 0x5311' /root/nouveau-mmio.trace | awk '{print $1}' | sort | uniq -c
grep -E '(^W|^R|^UNKNOWN) .* 0x5311' /root/nouveau-mmio.trace | head -5
echo ""
echo ">>> Any access to PMC 0x5300[0-6]xxx (device_enable/reset)?"
grep -E '(^W|^R|^UNKNOWN) .* 0x5300[0-6]' /root/nouveau-mmio.trace | awk '{print $1}' | sort | uniq -c
grep -E '(^W|^R|^UNKNOWN) .* 0x5300[0-6]' /root/nouveau-mmio.trace | head -5
