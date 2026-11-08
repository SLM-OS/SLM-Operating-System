#!/bin/busybox sh
# /init for the #1193 Pi 5 Linux reference run (initramfs only, no rootfs).
#
# Results do not depend on the serial console: everything /init prints
# goes to /www/status.txt, which busybox httpd serves on port 80 once eth0
# is up (DHCP, falling back to the lab address 192.168.4.25/24). A
# background tail copies the same file to /dev/console, so a dead or
# blocking console cannot stall the run.
#
# Sequence: mount proc/sys/devtmpfs, network, httpd, version header,
# litmus for LITMUS_MINUTES, read-only ectlr.ko dump (cluster=1), DONE
# marker, idle. Never powers off.
B=/bin/busybox
$B mkdir -p /proc /sys /dev /tmp /www /etc
$B mount -t proc proc /proc
$B mount -t sysfs sysfs /sys
$B mount -t devtmpfs devtmpfs /dev
$B --install -s /bin
export PATH=/bin
LITMUS_MINUTES=10
OUT=/www/status.txt
: > $OUT
exec < /dev/null > $OUT 2>&1
(tail -f $OUT > /dev/console 2>/dev/null &)

hexval() { od -An -tx1 "$1" | tr -d ' \n'; }

echo "=== COH1193 pi-5-1 status (HTTP + console) ==="
echo "COH1193_NET_START uptime_s=$(cut -d' ' -f1 /proc/uptime)"
cat > /etc/udhcpc.script <<'EOS'
#!/bin/sh
case "$1" in
bound|renew)
	ip addr flush dev "$interface"
	ip addr add "$ip/${mask:-24}" dev "$interface"
	if [ -n "$router" ]; then
		ip route add default via "${router%% *}" dev "$interface"
	fi
	;;
deconfig)
	ip addr flush dev "$interface"
	ip link set "$interface" up
	;;
esac
exit 0
EOS
chmod 755 /etc/udhcpc.script
ip link set lo up
ip link set eth0 up
udhcpc -i eth0 -n -q -t 10 -T 3 -s /etc/udhcpc.script
if ! ip addr show dev eth0 | grep -q 'inet '; then
	echo "no DHCP lease: static 192.168.4.25/24"
	ip addr add 192.168.4.25/24 dev eth0
fi
ip addr show dev eth0
httpd -p 80 -h /www
echo "COH1193_HTTP_UP uptime_s=$(cut -d' ' -f1 /proc/uptime)"

echo "=== COH1193_HEADER pi-5-1 ==="
echo "board_uptime_s: $(cut -d' ' -f1 /proc/uptime)"
echo "--- uname -a"; uname -a
echo "--- /proc/cmdline"; cat /proc/cmdline
echo "--- /sys/devices/system/cpu/online: $(cat /sys/devices/system/cpu/online)"
echo "--- /sys/devices/system/cpu/offline: $(cat /sys/devices/system/cpu/offline)"
echo "--- /proc/cpuinfo"
cat /proc/cpuinfo
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
echo "--- /proc/device-tree/serial-number: $(tr '\0' ' ' < /proc/device-tree/serial-number 2>/dev/null)"
echo "--- litmus sha256"; sha256sum /coherency/litmus
echo "--- dmesg (boot)"
dmesg
echo "=== COH1193_HEADER_END ==="

echo 1 > /proc/sys/kernel/printk
echo "COH1193_LITMUS_START uptime_s=$(cut -d' ' -f1 /proc/uptime)"
/coherency/litmus -m $LITMUS_MINUTES
rc=$?
echo "COH1193_LITMUS_EXIT rc=$rc uptime_s=$(cut -d' ' -f1 /proc/uptime)"
echo "--- dmesg tail after run"
dmesg | tail -20
# Phase 2 (#1193 C0 cleared, reads only): CPUECTLR/CLUSTERECTLR dump.
if [ -f /coherency/ectlr.ko ]; then
	echo "COH1193_ECTLR_BEGIN"
	sha256sum /coherency/ectlr.ko
	echo 7 > /proc/sys/kernel/printk
	insmod /coherency/ectlr.ko cluster=1
	echo "insmod rc=$?"
	dmesg | grep ectlr1193
	echo "COH1193_ECTLR_END"
fi
echo "COH1193_DONE uptime_s=$(cut -d' ' -f1 /proc/uptime)"
echo "COH1193_END_MARKER"
while :; do sleep 3600; done
