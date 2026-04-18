# Kbuild wrapper for tegra-xusb-noshutdown (SLM-OS #266 Phase 3A A.4).
#
# Build against the running L4T kernel's headers. Produces a
# tegra_xusb_noshutdown.ko that slmos-kexec can insmod before
# firing the kexec handoff.

obj-m += tegra_xusb_noshutdown.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
