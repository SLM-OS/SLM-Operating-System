# Cross-buildable Linux kernel module for the PCIe clock-hold experiment.
#
# On the Jetson target directly:
#   make
#   sudo insmod pcie_clk_keepalive.ko
#
# On a cross-compile host (with KDIR pointing to the L4T kernel source):
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/kernel-source

obj-m := pcie_clk_keepalive.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
