# #1193 coherency litmus harness (Linux user space, AArch64, static).
#
#   make -C scripts/coherency          build litmus and test_litmus
#   make -C scripts/coherency test     run the detector self-test and a
#                                      reduced litmus pass (qemu-user on x86)
#   make -C scripts/coherency clean

CC      ?= aarch64-linux-gnu-gcc
ifeq ($(CC),cc)
CC      := aarch64-linux-gnu-gcc
endif
CFLAGS  := -O2 -static -pthread -Wall -Wextra -Werror
OUT     ?= build

ifeq ($(shell uname -m),aarch64)
RUN :=
else
RUN := qemu-aarch64-static -cpu max
endif

all: $(OUT)/litmus $(OUT)/test_litmus

$(OUT)/%: %.c litmus_patterns.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT):
	mkdir -p $@

test: all
	$(RUN) $(OUT)/test_litmus
	$(RUN) $(OUT)/litmus -d 100 -1 | tee $(OUT)/smoke.txt | tail -12
	grep -q '^VERDICT PASS' $(OUT)/smoke.txt

clean:
	rm -rf $(OUT)

.PHONY: all test clean
