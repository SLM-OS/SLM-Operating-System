# Kbuild wrapper for arm-smmu-noshutdown (SLM-OS #266 Phase 3A A.5).
#
# Build against the running L4T kernel's headers. Produces an
# arm_smmu_noshutdown.ko that slmos-kexec can insmod before firing
# the kexec handoff to preserve the xusb stream's SMMU translations.

obj-m += arm_smmu_noshutdown.o
obj-m += smmu_probe.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
