#!/bin/busybox sh
# /init for the #1193 Pi 5 Linux reference run (initramfs only, no rootfs).
# Mounts proc/sys/devtmpfs, prints the version header, runs the litmus set
# for LITMUS_MINUTES, prints an end marker and idles. Never powers off.
B=/bin/busybox
$B mkdir -p /proc /sys /dev /tmp
$B mount -t proc proc /proc
$B mount -t sysfs sysfs /sys
$B mount -t devtmpfs devtmpfs /dev
$B --install -s /bin
export PATH=/bin
exec </dev/console >/dev/console 2>&1
LITMUS_MINUTES=10

hexval() { od -An -tx1 "$1" | tr -d ' \n'; }

echo "=== COH1193_HEADER pi-5-1 ==="
echo "board_uptime_s: $(cut -d' ' -f1 /proc/uptime)"
echo "--- uname -a"; uname -a
echo "--- /proc/cmdline"; cat /proc/cmdline
echo "--- /sys/devices/system/cpu/online: $(cat /sys/devices/system/cpu/online)"
echo "--- /sys/devices/system/cpu/offline: $(cat /sys/devices/system/cpu/offline)"
echo "--- /proc/cpuinfo (processor, implementer, variant, part, revision, features)"
grep -E '^(processor|CPU implementer|CPU variant|CPU part|CPU revision|Features)' /proc/cpuinfo
echo "--- MIDR_EL1 per cpu (sysfs)"
for c in /sys/devices/system/cpu/cpu[0-9]*; do
	echo "${c##*/} midr=$(cat $c/regs/identification/midr_el1 2>/dev/null) revidr=$(cat $c/regs/identification/revidr_el1 2>/dev/null)"
done
echo "--- cpufreq per cpu"
for c in /sys/devices/system/cpu/cpu[0-9]*; do
	f=$c/cpufreq
	if [ -d $f ]; then
		echo "${c##*/} $(cat $f/scaling_governor) cur=$(cat $f/scaling_cur_freq)"
	else
		echo "${c##*/} no cpufreq driver"
	fi
done
echo "--- /proc/device-tree/model: $(tr '\0' ' ' < /proc/device-tree/model)"
echo "--- /proc/device-tree/chosen/bootloader/*"
for f in /proc/device-tree/chosen/bootloader/*; do
	n=${f##*/}
	[ "$n" = name ] && continue
	echo "$n: str='$(strings -n 1 $f | tr '\n' ' ')' hex=$(hexval $f)"
done
echo "--- /proc/device-tree/system/linux,revision: $(hexval /proc/device-tree/system/linux,revision 2>/dev/null)"
echo "--- dmesg firmware and CPU lines"
dmesg | grep -iE 'psci|smccc|Booting Linux|Machine model|CPU features|erratum|raspberrypi-firmware|firmware: |SMP: |CPU[0-9]+: Booted' | head -60
echo "--- litmus sha256"; sha256sum /coherency/litmus
echo "=== COH1193_HEADER_END ==="

# Keep kernel messages off the console while rows print.
echo 1 > /proc/sys/kernel/printk
# Give the host time to start a fresh serial capture that covers the run.
echo "COH1193_WAIT 20s"
sleep 20
echo "COH1193_LITMUS_START uptime_s=$(cut -d' ' -f1 /proc/uptime)"
/coherency/litmus -m $LITMUS_MINUTES
rc=$?
echo "COH1193_LITMUS_EXIT rc=$rc uptime_s=$(cut -d' ' -f1 /proc/uptime)"
echo "--- dmesg tail after run"
dmesg | tail -15
# Phase 2 (#1193 C0 cleared, reads only): CPUECTLR/CLUSTERECTLR dump.
if [ -f /coherency/ectlr.ko ]; then
	echo "COH1193_ECTLR_BEGIN"
	echo 7 > /proc/sys/kernel/printk
	insmod /coherency/ectlr.ko cluster=1
	echo "insmod rc=$?"
	dmesg | grep ectlr1193
	echo "COH1193_ECTLR_END"
fi
echo "COH1193_END_MARKER"
while :; do sleep 3600; done
