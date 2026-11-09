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

# Smoke and injection runs use 4 CPUs so the pair count matches a Pi 5.
SMOKE := taskset -c 0-3 $(RUN) $(OUT)/litmus -d 100 -1

test: all
	$(RUN) $(OUT)/test_litmus
	@echo "== clean reduced pass: expect VERDICT PASS, P4 coverage=ok, S2 rows, SENS lines"
	$(SMOKE) > $(OUT)/smoke.txt; echo "exit=$$?" >> $(OUT)/smoke.txt
	@grep -E '^(TOTAL|VERDICT|exit=)' $(OUT)/smoke.txt
	grep -q '^VERDICT PASS' $(OUT)/smoke.txt
	grep -q '^exit=0' $(OUT)/smoke.txt
	grep -q 'pattern=S2 ' $(OUT)/smoke.txt
	grep -q '^SENS cpus=0>1 ' $(OUT)/smoke.txt
	! grep -q 'coverage=weak' $(OUT)/smoke.txt
	@echo "== inject P6 (sum starts at 1): expect P6 FAIL, VERDICT FAIL, exit 1"
	$(SMOKE) -I P6 > $(OUT)/inject-p6.txt; echo "exit=$$?" >> $(OUT)/inject-p6.txt
	@grep -E '^(TOTAL pattern=P6 |VERDICT|exit=)' $(OUT)/inject-p6.txt
	grep -q '^TOTAL pattern=P6 .*result=FAIL' $(OUT)/inject-p6.txt
	grep -q '^VERDICT FAIL' $(OUT)/inject-p6.txt
	grep -q '^exit=1' $(OUT)/inject-p6.txt
	@echo "== inject P4W (no writes): expect coverage=weak, P4 FAIL, VERDICT FAIL, exit 1"
	$(SMOKE) -I P4W > $(OUT)/inject-p4w.txt; echo "exit=$$?" >> $(OUT)/inject-p4w.txt
	@grep -E '^(ROW pass=1 pattern=P4 cpus=w0|TOTAL pattern=P4 |VERDICT|exit=)' $(OUT)/inject-p4w.txt
	grep -q 'pattern=P4 .*coverage=weak' $(OUT)/inject-p4w.txt
	grep -q '^TOTAL pattern=P4 .*result=FAIL' $(OUT)/inject-p4w.txt
	grep -q '^VERDICT FAIL' $(OUT)/inject-p4w.txt
	grep -q '^exit=1' $(OUT)/inject-p4w.txt
	@echo "== inject STALE (P1 flag holds the previous row's final value, idle writer): expect P1 timeouts, observed=0, VERDICT FAIL, exit 1"
	$(SMOKE) -I STALE > $(OUT)/inject-stale.txt; echo "exit=$$?" >> $(OUT)/inject-stale.txt
	@grep -E '^(ROW pass=1 pattern=P1 cpus=0>1 |TOTAL pattern=P1 |VERDICT|exit=)' $(OUT)/inject-stale.txt
	grep -q '^ROW pass=1 pattern=P1 cpus=0>1 .*timeouts=1 .*observed=0 coverage=weak' $(OUT)/inject-stale.txt
	grep -q '^TOTAL pattern=P1 .*result=FAIL' $(OUT)/inject-stale.txt
	grep -q '^VERDICT FAIL' $(OUT)/inject-stale.txt
	grep -q '^exit=1' $(OUT)/inject-stale.txt
	@echo "== all harness tests passed"

clean:
	rm -rf $(OUT)

.PHONY: all test clean
