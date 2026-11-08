#!/bin/sh
# Cross-build ectlr.ko for the Raspberry Pi OS 6.18.60+rpt-rpi-2712 kernel
# on an x86-64 host, from the extracted Debian packages
# linux-headers-6.18.60+rpt-rpi-2712, linux-headers-6.18.60+rpt-common-rpi
# and linux-kbuild-6.18.60+rpt (dpkg-deb -x into one directory).
#
# The packaged kbuild host tools (fixdep, modpost) are arm64 binaries; the
# host's binfmt qemu-aarch64 runs them with the cross glibc sysroot.
# The packaged Makefile includes absolute /usr/src paths, so this script
# calls the common Makefile directly with O= pointing at the flavour dir.
#
# Usage: build-pi5.sh <extract-root> <module-src-dir>
set -eu
X=$1
M=$2
REL=6.18.60+rpt-rpi-2712
H=$X/usr/src/linux-headers-$REL
C=$X/usr/src/linux-headers-6.18.60+rpt-common-rpi
export QEMU_LD_PREFIX=/usr/aarch64-linux-gnu
# The kernel was built with gcc-14; the host cross gcc is 13, which lacks
# -fmin-function-alignment. Clearing CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT
# makes kbuild use its -falign-functions fallback with the same alignment.
make -C "$C" O="$H" M="$M" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
	CC=aarch64-linux-gnu-gcc KERNELRELEASE=$REL \
	CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT= modules
aarch64-linux-gnu-objdump -d "$M/ectlr.ko" > "$M/ectlr.dis"
if grep -iE '\smsr\s' "$M/ectlr.dis"; then
	echo "ectlr.ko contains MSR: refusing"
	exit 1
fi
echo "ectlr.ko: no msr instructions"
modinfo "$M/ectlr.ko" 2>/dev/null | grep -E 'vermagic|parm' || true
