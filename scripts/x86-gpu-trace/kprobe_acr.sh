#!/usr/bin/env bash
# kprobe_acr.sh — time the entry/exit of ACR-related nouveau functions
# and sample SEC2 CPUCTL continuously to pin down when unlock happens.
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

echo ">>> Starting bpftrace on ACR/GSP/SEC2 fns"
cat >/tmp/acr.bt <<'EOF'
BEGIN { printf("# ts_ns name\n"); }

kprobe:nvkm_acr_init              { printf("%ld acr_init_enter\n", nsecs); }
kretprobe:nvkm_acr_init           { printf("%ld acr_init_exit rc=%d\n", nsecs, retval); }
kprobe:nvkm_acr_oneinit           { printf("%ld acr_oneinit_enter\n", nsecs); }
kretprobe:nvkm_acr_oneinit        { printf("%ld acr_oneinit_exit rc=%d\n", nsecs, retval); }
kprobe:nvkm_acr_load              { printf("%ld acr_load_enter\n", nsecs); }
kretprobe:nvkm_acr_load           { printf("%ld acr_load_exit rc=%d\n", nsecs, retval); }
kprobe:nvkm_acr_unload            { printf("%ld acr_unload_enter\n", nsecs); }
kprobe:ga102_acr_load             { printf("%ld ga102_acr_load_enter\n", nsecs); }
kretprobe:ga102_acr_load          { printf("%ld ga102_acr_load_exit rc=%d\n", nsecs, retval); }
kprobe:tu102_acr_load             { printf("%ld tu102_acr_load_enter\n", nsecs); }
kretprobe:tu102_acr_load          { printf("%ld tu102_acr_load_exit rc=%d\n", nsecs, retval); }
kprobe:ga102_acr_wpr_build        { printf("%ld wpr_build_enter\n", nsecs); }
kretprobe:ga102_acr_wpr_build     { printf("%ld wpr_build_exit\n", nsecs); }
kprobe:nouveau_run_vbios_init     { printf("%ld run_vbios_init_enter\n", nsecs); }
kretprobe:nouveau_run_vbios_init  { printf("%ld run_vbios_init_exit rc=%d\n", nsecs, retval); }
kprobe:nvkm_device_pci_preinit    { printf("%ld pci_preinit_enter\n", nsecs); }
kretprobe:nvkm_device_pci_preinit { printf("%ld pci_preinit_exit rc=%d\n", nsecs, retval); }
EOF

bpftrace /tmp/acr.bt > /root/bpftrace.out 2>&1 &
BP=$!
sleep 1  # let bpftrace attach

echo ">>> Starting SEC2 CPUCTL sampler"
(
    echo "ns val"
    while true; do
        V=$(/root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" 840100 2>/dev/null | awk '{print $2}')
        printf "%ld %s\n" "$(date +%s%N)" "$V"
    done
) > /root/sec2-probe-ns.log &
SAMPLER=$!

DMESG_START=$(dmesg | wc -l)

echo ">>> modprobe nouveau modeset=1"
timeout 15 modprobe nouveau modeset=1 || echo "modprobe returned $?"

sleep 4

kill $SAMPLER 2>/dev/null || true; wait $SAMPLER 2>/dev/null || true
kill -INT $BP 2>/dev/null || true; wait $BP 2>/dev/null || true

dmesg | tail -n +$DMESG_START > /root/nouveau-dmesg.txt

echo ""
echo ">>> Post-nouveau SEC2"
for off in 840100 8403f8 840040 840030 110100 0; do
    /root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" "$off"
done | tee /root/sec2-post-nouveau.txt

echo ""
echo ">>> bpftrace events (filtered)"
grep -v "^Attaching\|^BEGIN\|^#" /root/bpftrace.out | head -40

echo ""
echo ">>> SEC2 CPUCTL transitions (in ns since epoch)"
awk '
NR==1 { next }
{
    if ($2 != last) {
        print $1 " " $2 (last=="" ? " (first)" : " (from " last ")")
        last = $2
    }
}' /root/sec2-probe-ns.log | head -10

echo ""
echo ">>> Probe log sample count + span"
wc -l /root/sec2-probe-ns.log
awk 'NR==2 {start=$1} {end=$1} END {print "span ns: " (end-start)}' /root/sec2-probe-ns.log
