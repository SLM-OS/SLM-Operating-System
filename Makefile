# SLM-OS Top-Level Makefile
# Orchestrates C kernel (CMake) and Rust runtime (Cargo) builds

# ============================================================================
# Configuration
# ============================================================================

# Build type: Debug or Release
BUILD_TYPE ?= Release

# Platform: QEMU_VIRT, JETSON_ORIN_NANO, or RASPI5
PLATFORM ?= QEMU_VIRT

# AI Scheduler: OFF by default, ON to include trained ML models
AI_SCHED ?= OFF

# Work-stealing scheduler (#59 Phase B).
#
# Empty default: let CMakeLists.txt pick the per-platform default
# (ON for x86-64 / QEMU ARM64 / Pi 5; OFF for Jetson — see the
# ENABLE_WORK_STEALING block in CMakeLists.txt). Override explicitly
# with `make kernel ... WORK_STEALING=ON` or `WORK_STEALING=OFF`;
# anything else leaves the platform default in place.
WORK_STEALING ?=

# Secondary-CPU preemption via ELR trampoline: OFF by default. Required
# for Jetson / Pi 5 to get timer-driven preemption on CPUs 1..N. Replaces
# the platform-specific PI5_SECONDARY_PREEMPT; legacy name still works.
SECONDARY_PREEMPT ?= OFF
PI5_SECONDARY_PREEMPT ?= OFF
ifeq ($(PI5_SECONDARY_PREEMPT),ON)
    SECONDARY_PREEMPT := ON
endif

# AI Eviction (Phase AI-Eviction):
#   AI_EVICTION=ON         — compile the pluggable EvictionPolicy trait +
#                            classical Rust policies (stub XGBoost/MLP).
#   AI_EVICTION_MODELS=ON  — additionally pull in the trained XGBoost +
#                            int8 MLP weights. Run scripts/import_eviction_weights.sh
#                            first to stage the generated files from the sibling
#                            slm-os-page-sim project. Implies AI_EVICTION=ON.
AI_EVICTION ?= OFF
AI_EVICTION_MODELS ?= OFF

# Embed scripts/*.lua demo scripts via .incbin (#14). Default ON; pass
# EMBED_DEMO_SCRIPTS=OFF to leave them out of the kernel image (saves ~20KB).
# Lua interpreter + slm.* bindings remain functional when OFF.
EMBED_DEMO_SCRIPTS ?= ON
ifeq ($(AI_EVICTION_MODELS),ON)
    # Models imply the trait layer; callers don't have to set both.
    AI_EVICTION := ON
endif

# Cargo feature list built from the flags above.
CARGO_FEATURES :=
ifeq ($(AI_EVICTION_MODELS),ON)
    CARGO_FEATURES := ai_eviction_models
else ifeq ($(AI_EVICTION),ON)
    CARGO_FEATURES := ai_eviction
endif
ifeq ($(CARGO_FEATURES),)
    CARGO_FEATURES_FLAG :=
else
    CARGO_FEATURES_FLAG := --features $(CARGO_FEATURES)
endif

# Directories
BUILD_DIR := build
KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel
KERNEL_TEST_BUILD_DIR := $(BUILD_DIR)/kernel-test
ifeq ($(PLATFORM),X86_64)
    RUNTIME_BUILD_DIR := runtime/target/x86_64-unknown-none
else
    RUNTIME_BUILD_DIR := runtime/target/aarch64-unknown-none
endif

# Tools - detect OS and use appropriate paths
ifeq ($(OS),Windows_NT)
    CMAKE := "C:/Program Files/CMake/bin/cmake.exe"
    QEMU := "C:/Program Files/qemu/qemu-system-aarch64.exe"
    MAKE_PROGRAM_ARG := -DCMAKE_MAKE_PROGRAM="C:/cygwin64/bin/make.exe"
else
    CMAKE := cmake
    ifeq ($(PLATFORM),X86_64)
        QEMU := qemu-system-x86_64
    else
        QEMU := qemu-system-aarch64
    endif
    MAKE_PROGRAM_ARG :=
endif

# Toolchain (select based on platform)
ifeq ($(PLATFORM),X86_64)
    TOOLCHAIN_FILE := cmake/toolchain-x86_64-none-elf.cmake
else
    TOOLCHAIN_FILE := cmake/toolchain-aarch64-none-elf.cmake
endif

# QEMU settings
ifeq ($(PLATFORM),X86_64)
    QEMU_MACHINE := q35
    QEMU_CPU := max
    QEMU_MEMORY := 256M
    QEMU_CORES := 4
else
    QEMU_MACHINE := virt
    QEMU_CPU := cortex-a76
    QEMU_MEMORY := 1G
    QEMU_CORES := 4
endif

# Output files
KERNEL_ELF := $(KERNEL_BUILD_DIR)/slmos.elf
KERNEL_BIN := $(KERNEL_BUILD_DIR)/slmos.bin
KERNEL_TEST_ELF := $(KERNEL_TEST_BUILD_DIR)/slmos.elf

# ============================================================================
# Default target
# ============================================================================

.PHONY: all
all: kernel
# Note: runtime is built as a dependency of kernel

# ============================================================================
# Kernel (C) targets
# ============================================================================

# Check for stale file locks in build directory (Windows issue with ungraceful QEMU/GDB termination)
# If we can't create slmos.elf, nuke the directory to clear the stale lock
.PHONY: check-build-dir
check-build-dir:
	@if [ -d "$(KERNEL_BUILD_DIR)" ]; then \
		if ! touch "$(KERNEL_BUILD_DIR)/slmos.elf.test" 2>/dev/null; then \
			echo "WARNING: Stale lock detected in $(KERNEL_BUILD_DIR)"; \
			echo "         (Usually from ungraceful QEMU/GDB termination)"; \
			echo "         Cleaning build directory..."; \
			rm -rf "$(KERNEL_BUILD_DIR)"; \
		else \
			rm -f "$(KERNEL_BUILD_DIR)/slmos.elf.test"; \
		fi \
	fi

.PHONY: kernel
kernel: check-build-dir runtime $(KERNEL_BUILD_DIR)/Makefile
	@echo "Building kernel..."
	$(CMAKE) --build $(KERNEL_BUILD_DIR)

$(KERNEL_BUILD_DIR)/Makefile:
	@echo "Configuring kernel build..."
	$(CMAKE) -G "Unix Makefiles" -B $(KERNEL_BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DPLATFORM=$(PLATFORM) \
		$(if $(filter ON,$(AI_SCHED)),-DENABLE_AI_SCHEDULER=ON) \
		$(if $(filter ON,$(WORK_STEALING)),-DENABLE_WORK_STEALING=ON) \
		$(if $(filter OFF,$(WORK_STEALING)),-DENABLE_WORK_STEALING=OFF) \
		$(if $(filter ON,$(SECONDARY_PREEMPT)),-DSECONDARY_PREEMPT=ON) \
		$(if $(filter ON,$(AI_EVICTION)),-DENABLE_AI_EVICTION=ON) \
		$(if $(filter ON,$(AI_EVICTION_MODELS)),-DENABLE_AI_EVICTION_MODELS=ON) \
		$(if $(filter OFF,$(EMBED_DEMO_SCRIPTS)),-DEMBED_DEMO_SCRIPTS=OFF) \
		$(MAKE_PROGRAM_ARG)

.PHONY: kernel-clean
kernel-clean:
	@echo "Cleaning kernel build..."
	rm -rf $(KERNEL_BUILD_DIR)

.PHONY: kernel-rebuild
kernel-rebuild: kernel-clean kernel

# Build the x86-64 UEFI disk image (requires PLATFORM=X86_64).
# Produces $(KERNEL_BUILD_DIR)/slmos-x86.img for labctl sdwire flash.
.PHONY: x86-disk
x86-disk: kernel
ifneq ($(PLATFORM),X86_64)
	@echo "x86-disk requires PLATFORM=X86_64 (got $(PLATFORM))"; exit 1
endif
	@echo "Building x86-64 UEFI disk image..."
	$(CMAKE) --build $(KERNEL_BUILD_DIR) --target slmos-x86-disk
	@echo ""
	@echo "Disk image: $(KERNEL_BUILD_DIR)/slmos-x86.img"
	@echo "Flash with: labctl sdwire flash test-pc $(KERNEL_BUILD_DIR)/slmos-x86.img"

# Run structural checks on the built disk image. Used by CI and by the
# post-build checklist before flashing to test-pc. Validates GPT, FAT32
# BPB, and file presence — the regressions #82 caught.
.PHONY: x86-disk-verify
x86-disk-verify: x86-disk
	@scripts/tests/verify-x86-disk.sh $(KERNEL_BUILD_DIR)/slmos-x86.img

# P1-2 real-hardware validation: flash slmos-x86.img to test-pc via
# labctl, run boot_test --count 10, run the 5× sleep-while-echo
# multi-task preemption check, and verify bench smp dispatch. Gated on
# SLMOS_LABCTL=1 so CI without lab access doesn't attempt it.
.PHONY: x86-hw-validate
x86-hw-validate: x86-disk-verify
ifneq ($(PLATFORM),X86_64)
	@echo "x86-hw-validate requires PLATFORM=X86_64 (got $(PLATFORM))"; exit 1
endif
ifneq ($(SLMOS_LABCTL),1)
	@echo "x86-hw-validate requires SLMOS_LABCTL=1 (lab hardware access)."; exit 1
endif
	@scripts/tests/x86-multitask-boot-test.sh

# ============================================================================
# gsp-harness — Linux userspace tool for GSP-RM development (Phase E).
# ============================================================================
# Builds a native Linux ELF that links the shared GSP-RM core
# (kernel/gpu/nvidia/gsp.c) with a Linux-userspace implementation of
# the gsp_platform_ops vtable. Requires the test-pc one-time setup in
# docs/testing/test-pc-linux-vfio-setup.md.

GSP_HARNESS_OUT := build/host-tools/gsp-harness
GSP_HARNESS_SRCS := \
    host-tools/gsp-harness/main.c \
    host-tools/gsp-harness/linux_platform.c \
    host-tools/gsp-harness/vfio.c \
    kernel/gpu/nvidia/gsp.c \
    kernel/gpu/nvidia/nvidia_vbios.c \
    kernel/gpu/nvidia/falcon.c \
    kernel/gpu/nvidia/nvfw.c \
    kernel/gpu/nvidia/bringup.c \
    kernel/gpu/nvidia/rpc.c

# Native CFLAGS — these differ substantially from the bare-metal
# kernel build. The shared GSP core uses `uart_puts`, `uart_printf`
# for diagnostics; we stub them with printf equivalents.
GSP_HARNESS_CFLAGS := \
    -std=c11 -Wall -Wextra -O2 -g \
    -Ihost-tools/gsp-harness \
    -Ikernel/gpu/nvidia \
    -D_GNU_SOURCE \
    -DSLM_HOST_HARNESS=1

.PHONY: gsp-harness
gsp-harness:
	@mkdir -p $(dir $(GSP_HARNESS_OUT))
	@echo "Building Linux GSP harness..."
	$(CC) $(GSP_HARNESS_CFLAGS) -o $(GSP_HARNESS_OUT) $(GSP_HARNESS_SRCS)
	@echo "Built $(GSP_HARNESS_OUT)"
	@echo "Run: sudo $(GSP_HARNESS_OUT) --probe"

.PHONY: gsp-harness-clean
gsp-harness-clean:
	rm -f $(GSP_HARNESS_OUT)

# VBIOS parser unit tests — runs on the host, no GPU required.
# Synthetic VBIOS image built in the test, no proprietary binaries.
.PHONY: test-vbios
test-vbios:
	@mkdir -p build/host-tools
	@echo "Building + running VBIOS parser tests..."
	$(CC) -std=c11 -Wall -Wextra -O2 -g \
	    -o build/host-tools/test_nvidia_vbios \
	    host-tools/gsp-harness/test_nvidia_vbios.c \
	    kernel/gpu/nvidia/nvidia_vbios.c
	@./build/host-tools/test_nvidia_vbios

# GSP firmware extraction script failure-mode tests.
# Runs on the host; no hardware required. If /lib/firmware/nvidia/ga107
# is installed locally, also exercises the happy path.
.PHONY: test-gsp-extract
test-gsp-extract:
	@echo "Running extract-gsp-firmware.sh functional tests..."
	@bash scripts/tools/test-extract-gsp-firmware.sh

# NVIDIA firmware wrapper parser tests — hand-built synthetic blobs
# exercise both bin_magic variants and every rejection path.
.PHONY: test-nvfw
test-nvfw:
	@mkdir -p build/host-tools
	@echo "Building + running nvfw parser tests..."
	$(CC) -std=c11 -Wall -Wextra -O2 -g \
	    -o build/host-tools/test_nvfw \
	    host-tools/gsp-harness/test_nvfw.c \
	    kernel/gpu/nvidia/nvfw.c
	@./build/host-tools/test_nvfw

# GSP-RM bringup logic unit tests — pure functions only (sig-index
# algorithm, DMEMMAPPER patcher). Hardware integration is exercised
# via `gsp-harness --fwsec-frts` against a real GPU.
.PHONY: test-bringup
test-bringup:
	@mkdir -p build/host-tools
	@echo "Building + running bringup logic tests..."
	$(CC) -std=c11 -Wall -Wextra -O2 -g \
	    -Ihost-tools/gsp-harness -Ikernel/gpu/nvidia \
	    -DSLM_HOST_HARNESS=1 \
	    -o build/host-tools/test_bringup \
	    host-tools/gsp-harness/test_bringup.c \
	    kernel/gpu/nvidia/bringup.c \
	    kernel/gpu/nvidia/falcon.c \
	    kernel/gpu/nvidia/nvfw.c \
	    kernel/gpu/nvidia/nvidia_vbios.c \
	    kernel/gpu/nvidia/gsp.c
	@./build/host-tools/test_bringup

# Falcon v4 driver unit tests — uses a mock BAR0 vtable, no GPU
# required. Exercises probe, reset/scrub, halt polling, DMA protocol,
# alignment checks, 40-bit IOVA splitting.
.PHONY: test-falcon
test-falcon:
	@mkdir -p build/host-tools
	@echo "Building + running Falcon driver tests..."
	$(CC) -std=c11 -Wall -Wextra -O2 -g \
	    -Ihost-tools/gsp-harness -Ikernel/gpu/nvidia \
	    -DSLM_HOST_HARNESS=1 \
	    -o build/host-tools/test_falcon \
	    host-tools/gsp-harness/test_falcon.c \
	    kernel/gpu/nvidia/falcon.c \
	    kernel/gpu/nvidia/gsp.c
	@./build/host-tools/test_falcon

# GA10B (Jetson integrated Ampere) nvgpu-native bringup tests — mock
# vtable + synthetic firmware blobs emitted via inline asm so the
# `_start`/`_end` symbol arithmetic ga10b_bringup.c relies on works.
# Covers: firmware accessor, prepare guards, state-machine ordering,
# ACR sequence plumbing (assert/deassert reset, PIO byte-exact
# transport, BCR_CTRL=0x11, STARTCPU, halt polling, BR_RETCODE).
#
# Note -DENABLE_GA10B_FIRMWARE=1: switches the firmware accessor's
# compile-time guard to the "embedded" branch so it references the
# inline-asm symbols the test TU provides.
.PHONY: test-ga10b-bringup
test-ga10b-bringup:
	@mkdir -p build/host-tools
	@echo "Building + running GA10B bringup tests..."
	$(CC) -std=c11 -Wall -Wextra -O2 -g \
	    -Ihost-tools/gsp-harness -Ikernel/gpu/nvidia \
	    -DSLM_HOST_HARNESS=1 -DENABLE_GA10B_FIRMWARE=1 \
	    -o build/host-tools/test_ga10b_bringup \
	    host-tools/gsp-harness/test_ga10b_bringup.c \
	    kernel/gpu/nvidia/ga10b_bringup.c \
	    kernel/gpu/nvidia/falcon.c \
	    kernel/gpu/nvidia/gsp.c
	@./build/host-tools/test_ga10b_bringup

# GSP-RM RPC ring helper tests — pure ring-pointer arithmetic plus
# a mock-vtable channel init. Hardware integration runs once GSP-RM
# is alive (post-E3.4.e).
.PHONY: test-rpc
test-rpc:
	@mkdir -p build/host-tools
	@echo "Building + running RPC ring tests..."
	$(CC) -std=c11 -Wall -Wextra -O2 -g \
	    -Ihost-tools/gsp-harness -Ikernel/gpu/nvidia \
	    -DSLM_HOST_HARNESS=1 \
	    -o build/host-tools/test_rpc \
	    host-tools/gsp-harness/test_rpc.c \
	    kernel/gpu/nvidia/rpc.c \
	    kernel/gpu/nvidia/gsp.c
	@./build/host-tools/test_rpc

# ============================================================================
# Runtime (Rust) targets
# ============================================================================

# Rust target selection based on platform
ifeq ($(PLATFORM),X86_64)
    RUST_TARGET_FLAG := --target x86_64-unknown-none
else
    RUST_TARGET_FLAG :=
endif

.PHONY: runtime
runtime:
	@echo "Building runtime... (cargo features: $(if $(CARGO_FEATURES),$(CARGO_FEATURES),none))"
	cd runtime && cargo build $(RUST_TARGET_FLAG) $(if $(filter Release,$(BUILD_TYPE)),--release,) $(CARGO_FEATURES_FLAG)

.PHONY: runtime-clean
runtime-clean:
	@echo "Cleaning runtime build..."
	cd runtime && cargo clean

.PHONY: runtime-rebuild
runtime-rebuild: runtime-clean runtime

.PHONY: rustdoc
rustdoc:
	@echo "Generating Rust documentation..."
	cd runtime && cargo doc $(RUST_TARGET_FLAG) --no-deps
	@echo "Documentation generated at runtime/target/$(if $(filter X86_64,$(PLATFORM)),x86_64-unknown-none,aarch64-unknown-none)/doc/slm_runtime/index.html"

# ============================================================================
# Combined targets
# ============================================================================

.PHONY: clean
clean: kernel-clean kernel-test-clean runtime-clean
	@echo "Clean complete."

.PHONY: rebuild
rebuild: clean all

# ============================================================================
# QEMU targets
# ============================================================================

# x86-64 kernel ISO (multiboot2 requires GRUB, can't use -kernel)
KERNEL_ISO := $(KERNEL_BUILD_DIR)/slmos.iso

# Common QEMU arguments
QEMU_COMMON := -machine $(QEMU_MACHINE) -cpu $(QEMU_CPU) -smp cores=$(QEMU_CORES) -m $(QEMU_MEMORY) -nographic

# Networking: add VirtIO-Net device for platforms with driver support.
#
# QEMU virt machine defaults virtio-mmio to legacy (version=1) for
# backwards compat, but our virtio_net driver uses the modern (version=2)
# queue setup (QUEUE_DESC_LOW/HIGH, QUEUE_AVAIL_LOW/HIGH, QUEUE_USED_LOW/HIGH,
# QUEUE_READY). The force-legacy=false global flips the device into modern
# mode, which is what virtio_net.c expects. Without this flag, the device
# accepts all our writes but never processes virtqueue kicks because
# QUEUE_PFN was never written.
ifeq ($(PLATFORM),X86_64)
    QEMU_NET := -device virtio-net-pci,netdev=net0 -netdev user,id=net0
else ifeq ($(PLATFORM),QEMU_VIRT)
    QEMU_NET := -global virtio-mmio.force-legacy=false \
                -device virtio-net-device,netdev=net0 -netdev user,id=net0
else
    QEMU_NET :=
endif

# x86-64 uses GRUB ISO (-cdrom); ARM64 uses direct kernel load (-kernel)
ifeq ($(PLATFORM),X86_64)
    QEMU_BOOT_ARG = -cdrom $(KERNEL_ISO)
else
    QEMU_BOOT_ARG = -kernel $(KERNEL_ELF)
endif

# Build GRUB ISO for x86-64 (no-op for ARM64)
.PHONY: grub-iso
grub-iso:
ifeq ($(PLATFORM),X86_64)
	@mkdir -p $(KERNEL_BUILD_DIR)/iso/boot/grub
	@cp $(KERNEL_ELF) $(KERNEL_BUILD_DIR)/iso/boot/kernel.elf
	@echo 'set timeout=0' > $(KERNEL_BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo 'set default=0' >> $(KERNEL_BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo 'menuentry "SLM-OS" { multiboot2 /boot/kernel.elf; boot; }' >> $(KERNEL_BUILD_DIR)/iso/boot/grub/grub.cfg
	@grub-mkrescue -o $(KERNEL_ISO) $(KERNEL_BUILD_DIR)/iso 2>/dev/null
endif

.PHONY: run
run: kernel grub-iso
	@echo "Running in QEMU..."
	$(QEMU) $(QEMU_COMMON) $(QEMU_NET) $(QEMU_BOOT_ARG)

.PHONY: shell
shell: kernel grub-iso
	@echo "Running in QEMU (interactive shell)..."
	@echo "Press Ctrl+A then X to exit QEMU"
	@echo ""
	$(QEMU) $(QEMU_COMMON) $(QEMU_NET) $(QEMU_BOOT_ARG)

.PHONY: debug
debug: kernel grub-iso
	@echo "Starting QEMU with GDB server on port 1234..."
	@echo "In another terminal, run: make gdb"
	$(QEMU) $(QEMU_COMMON) $(QEMU_NET) $(QEMU_BOOT_ARG) -S -gdb tcp::1234

# GDB connection settings
GDB := aarch64-none-elf-gdb
GDB_PORT := 1234

.PHONY: gdb
gdb:
	@echo "Connecting GDB to QEMU on port $(GDB_PORT)..."
	$(GDB) $(KERNEL_ELF) \
		-ex "target remote localhost:$(GDB_PORT)" \
		-ex "set confirm off"

# ============================================================================
# Test targets
# ============================================================================

# Test output file and timeout (kills QEMU if tests hang to prevent OOM)
TEST_OUTPUT := $(BUILD_DIR)/test-output.log
TEST_TIMEOUT := 120
# Hard memory limit for QEMU process (host RSS, not guest RAM).
# systemd-run enforces this via cgroups — kernel OOM-kills QEMU if exceeded.
# Guest RAM is QEMU_TEST_MEMORY; this caps total process memory including overhead.
QEMU_MEM_LIMIT := 3G
# Guest RAM for test QEMU — must match platform.h defaults (1G for QEMU_VIRT)
# since DTB parsing may fail and kernel falls back to hardcoded RAM size.
QEMU_TEST_MEMORY := $(QEMU_MEMORY)
# CPU limit for QEMU process — prevents a runaway busy-spin test from
# pegging all host cores for the full timeout duration.
QEMU_CPU_LIMIT := 200%
# Wrapper to enforce memory and CPU limits (requires systemd --user)
QEMU_GUARD := systemd-run --user --scope -q -p MemoryMax=$(QEMU_MEM_LIMIT) -p CPUQuota=$(QEMU_CPU_LIMIT)

# Build kernel with ENABLE_BOOT_TESTS (runs tests at boot and exits)
.PHONY: kernel-test
kernel-test: check-build-dir runtime $(KERNEL_TEST_BUILD_DIR)/Makefile
	@echo "Building test kernel..."
	$(CMAKE) --build $(KERNEL_TEST_BUILD_DIR)

$(KERNEL_TEST_BUILD_DIR)/Makefile:
	@echo "Configuring test kernel build..."
	$(CMAKE) -G "Unix Makefiles" -B $(KERNEL_TEST_BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DPLATFORM=$(PLATFORM) \
		-DENABLE_BOOT_TESTS=ON \
		$(if $(filter ON,$(AI_SCHED)),-DENABLE_AI_SCHEDULER=ON) \
		$(if $(filter ON,$(WORK_STEALING)),-DENABLE_WORK_STEALING=ON) \
		$(if $(filter OFF,$(WORK_STEALING)),-DENABLE_WORK_STEALING=OFF) \
		$(if $(filter ON,$(SECONDARY_PREEMPT)),-DSECONDARY_PREEMPT=ON) \
		$(if $(filter ON,$(AI_EVICTION)),-DENABLE_AI_EVICTION=ON) \
		$(if $(filter ON,$(AI_EVICTION_MODELS)),-DENABLE_AI_EVICTION_MODELS=ON) \
		$(if $(filter OFF,$(EMBED_DEMO_SCRIPTS)),-DEMBED_DEMO_SCRIPTS=OFF) \
		$(MAKE_PROGRAM_ARG)

.PHONY: kernel-test-clean
kernel-test-clean:
	@echo "Cleaning test kernel build..."
	rm -rf $(KERNEL_TEST_BUILD_DIR)

# x86-64 test ISO path
KERNEL_TEST_ISO := $(KERNEL_TEST_BUILD_DIR)/slmos-test.iso

.PHONY: test
test: kernel-test
	@echo "Running kernel tests..."
	@rm -f $(TEST_OUTPUT)
ifeq ($(PLATFORM),X86_64)
	@# x86-64: create GRUB ISO and use isa-debug-exit for test termination
	@mkdir -p $(KERNEL_TEST_BUILD_DIR)/iso/boot/grub
	@cp $(KERNEL_TEST_ELF) $(KERNEL_TEST_BUILD_DIR)/iso/boot/kernel.elf
	@echo 'set timeout=0' > $(KERNEL_TEST_BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo 'set default=0' >> $(KERNEL_TEST_BUILD_DIR)/iso/boot/grub/grub.cfg
	@echo 'menuentry "SLM-OS Tests" { multiboot2 /boot/kernel.elf; boot; }' >> $(KERNEL_TEST_BUILD_DIR)/iso/boot/grub/grub.cfg
	@grub-mkrescue -o $(KERNEL_TEST_ISO) $(KERNEL_TEST_BUILD_DIR)/iso 2>/dev/null
	@$(QEMU_GUARD) timeout $(TEST_TIMEOUT) $(QEMU) \
		-machine $(QEMU_MACHINE) \
		-cpu $(QEMU_CPU) \
		-smp cores=$(QEMU_CORES) \
		-m $(QEMU_TEST_MEMORY) \
		-nographic \
		$(QEMU_NET) \
		-device isa-debug-exit,iobase=0x501,iosize=2 \
		-cdrom $(KERNEL_TEST_ISO) \
		> $(TEST_OUTPUT) 2>&1; \
	QEMU_EXIT=$$?; \
	echo ""; \
	echo "Test Results:"; \
	echo "============="; \
	if [ $$QEMU_EXIT -eq 124 ]; then \
		echo "TIMEOUT - Tests did not complete within $(TEST_TIMEOUT)s"; \
		echo "Last output:"; \
		tail -20 $(TEST_OUTPUT); \
		exit 1; \
	elif [ $$QEMU_EXIT -eq 1 ]; then \
		echo "PASSED - All tests passed (isa-debug-exit code 1 = success)"; \
		exit 0; \
	elif grep -F "PAGE FAULT" $(TEST_OUTPUT) > /dev/null 2>&1; then \
		echo "CRASHED - Kernel page fault detected"; \
		grep -A 20 "PAGE FAULT" $(TEST_OUTPUT) | head -25; \
		exit 1; \
	elif grep -F "KERNEL PANIC" $(TEST_OUTPUT) > /dev/null 2>&1; then \
		echo "CRASHED - Kernel panic"; \
		grep -A 20 "KERNEL PANIC" $(TEST_OUTPUT) | head -25; \
		exit 1; \
	elif grep -F "[FAIL]" $(TEST_OUTPUT) > /dev/null 2>&1; then \
		echo "FAILED - Test failures detected:"; \
		grep -F "[FAIL]" $(TEST_OUTPUT); \
		exit 1; \
	else \
		echo "FAILED - Tests failed (exit code $$QEMU_EXIT)"; \
		echo "Check $(TEST_OUTPUT) for details"; \
		tail -30 $(TEST_OUTPUT); \
		exit 1; \
	fi
else
	@$(QEMU_GUARD) timeout $(TEST_TIMEOUT) $(QEMU) \
		-machine $(QEMU_MACHINE) \
		-cpu $(QEMU_CPU) \
		-smp cores=$(QEMU_CORES) \
		-m $(QEMU_TEST_MEMORY) \
		-nographic \
		$(QEMU_NET) \
		-semihosting \
		-kernel $(KERNEL_TEST_ELF) \
		> $(TEST_OUTPUT) 2>&1; \
	QEMU_EXIT=$$?; \
	echo ""; \
	echo "Test Results:"; \
	echo "============="; \
	if [ $$QEMU_EXIT -eq 124 ]; then \
		echo "TIMEOUT - Tests did not complete within $(TEST_TIMEOUT)s"; \
		echo "Last output:"; \
		tail -20 $(TEST_OUTPUT); \
		exit 1; \
	elif [ $$QEMU_EXIT -eq 0 ]; then \
		echo "PASSED - All tests passed (exit code 0)"; \
		exit 0; \
	elif grep -F "PAGE FAULT" $(TEST_OUTPUT) > /dev/null 2>&1; then \
		echo "CRASHED - Kernel page fault detected"; \
		grep -A 20 "PAGE FAULT" $(TEST_OUTPUT) | head -25; \
		exit 1; \
	elif grep -F "KERNEL PANIC" $(TEST_OUTPUT) > /dev/null 2>&1; then \
		echo "CRASHED - Kernel panic"; \
		grep -A 20 "KERNEL PANIC" $(TEST_OUTPUT) | head -25; \
		exit 1; \
	elif grep -F "[FAIL]" $(TEST_OUTPUT) > /dev/null 2>&1; then \
		echo "FAILED - Test failures detected:"; \
		grep -F "[FAIL]" $(TEST_OUTPUT); \
		exit 1; \
	else \
		echo "FAILED - Tests failed (exit code $$QEMU_EXIT)"; \
		echo "Check $(TEST_OUTPUT) for details"; \
		tail -30 $(TEST_OUTPUT); \
		exit 1; \
	fi
endif

# ============================================================================
# Utility targets
# ============================================================================

.PHONY: info
info:
	@echo "SLM-OS Build Information"
	@echo "========================"
	@echo "Platform:       $(PLATFORM)"
	@echo "Build type:     $(BUILD_TYPE)"
	@echo "Kernel ELF:     $(KERNEL_ELF)"
	@echo "Kernel binary:  $(KERNEL_BIN)"
	@echo "QEMU machine:   $(QEMU_MACHINE)"
	@echo "QEMU CPU:       $(QEMU_CPU)"
	@echo "QEMU memory:    $(QEMU_MEMORY)"
	@echo "QEMU cores:     $(QEMU_CORES)"

.PHONY: check-tools
check-tools:
	@echo "Checking required tools..."
	@which aarch64-none-elf-gcc > /dev/null && echo "  [OK] aarch64-none-elf-gcc" || echo "  [MISSING] aarch64-none-elf-gcc"
	@which cmake > /dev/null && echo "  [OK] cmake" || echo "  [MISSING] cmake"
	@which cargo > /dev/null && echo "  [OK] cargo" || echo "  [MISSING] cargo"
	@which $(QEMU) > /dev/null && echo "  [OK] $(QEMU)" || echo "  [MISSING] $(QEMU)"
	@which aarch64-none-elf-gdb > /dev/null && echo "  [OK] aarch64-none-elf-gdb" || echo "  [MISSING] aarch64-none-elf-gdb"

.PHONY: help
help:
	@echo "SLM-OS Build System"
	@echo "==================="
	@echo ""
	@echo "Usage: make [target] [PLATFORM=QEMU_VIRT|JETSON_ORIN_NANO|RASPI5] [BUILD_TYPE=Debug|Release]"
	@echo ""
	@echo "Build targets:"
	@echo "  all            Build kernel and runtime (default)"
	@echo "  kernel         Build C kernel only"
	@echo "  kernel-test    Build test kernel (with ENABLE_BOOT_TESTS)"
	@echo "  runtime        Build Rust runtime only"
	@echo ""
	@echo "Clean targets:"
	@echo "  clean          Clean all build artifacts"
	@echo "  kernel-clean   Clean kernel build only"
	@echo "  kernel-test-clean Clean test kernel build only"
	@echo "  runtime-clean  Clean runtime build only"
	@echo ""
	@echo "Rebuild targets:"
	@echo "  rebuild        Clean and rebuild everything"
	@echo "  kernel-rebuild Clean and rebuild kernel"
	@echo "  runtime-rebuild Clean and rebuild runtime"
	@echo ""
	@echo "Run targets:"
	@echo "  run            Run kernel in QEMU (same as shell)"
	@echo "  shell          Run kernel in QEMU (interactive shell)"
	@echo "  debug          Run kernel in QEMU with GDB server (terminal 1)"
	@echo "  gdb            Connect GDB to running QEMU (terminal 2)"
	@echo ""
	@echo "Test targets:"
	@echo "  test           Run all tests in QEMU (exits on completion)"
	@echo ""
	@echo "Utility targets:"
	@echo "  info           Show build configuration"
	@echo "  check-tools    Verify required tools are installed"
	@echo "  help           Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make                    Build everything (Debug)"
	@echo "  make BUILD_TYPE=Release Build everything (Release)"
	@echo "  make shell              Build and run interactive shell"
	@echo "  make test               Run all tests"
	@echo "  make debug              Start QEMU with GDB server"
	@echo "  make gdb                Connect to QEMU (run in 2nd terminal)"
