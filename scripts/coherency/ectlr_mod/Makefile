# #1193 read-only CPUECTLR/CLUSTERECTLR dump module.
#
# Native build on the board (Jetson):
#   make KDIR=/lib/modules/$(uname -r)/build
# Cross build for the Pi 5 against the extracted linux-headers package
# (scripts/ host tools in that package are arm64; binfmt qemu-aarch64 runs
# them on an x86 host):
#   make KDIR=<extract>/usr/src/linux-headers-6.18.60+rpt-rpi-2712 \
#        ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
# check: fails if the object contains any msr instruction.

obj-m := ectlr.o

KDIR ?= /lib/modules/$(shell uname -r)/build
OBJDUMP ?= $(CROSS_COMPILE)objdump

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

check: all
	@if $(OBJDUMP) -d ectlr.ko | grep -iE '\smsr\s'; then \
		echo "ectlr.ko contains MSR: refusing"; exit 1; \
	else echo "ectlr.ko: no msr instructions"; fi

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all check clean
