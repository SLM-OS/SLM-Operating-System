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

# Verbose Hailo wire-format diagnostic dumps (context hex rows, VDMA
# register / descriptor dumps, per-submit chatter). Default OFF — the
# verbose polling and descriptor readback paths can perturb timing
# and cache ownership exactly where Phase 8 #253 is being measured.
# Audit F-09 (2026-04-24) flipped the default. Enable explicitly when
# capturing wire-byte diffs vs HailoRT: `make kernel HAILO_WIRE_DEBUG=ON`.
HAILO_WIRE_DEBUG ?= OFF

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

# Eviction:
#   DISABLE_EVICTION=ON    — compile out the pluggable eviction framework.
#                            Default OFF, so LRU-style eviction is available.
#   EVICTION_MODELS=ON     — additionally pull in the trained XGBoost +
#                            int8 MLP weights. Run scripts/import_eviction_weights.sh
#                            first to stage the generated files from the sibling
#                            slm-os-page-sim project.
#   EVICTION_DEFAULT_POLICY=<name>
#                          — choose the compiled-in default policy when
#                            eviction is enabled. Default: lru.
DISABLE_EVICTION ?= OFF
EVICTION_MODELS ?= OFF
EVICTION_DEFAULT_POLICY ?= lru

# Back-compat aliases. Keep accepting the old names for now, but map
# them onto the clearer user-facing controls.
ifdef AI_EVICTION
$(warning AI_EVICTION is deprecated; use DISABLE_EVICTION=OFF or ON)
ifeq ($(AI_EVICTION),OFF)
    DISABLE_EVICTION := ON
else
    DISABLE_EVICTION := OFF
endif
endif
ifdef AI_EVICTION_MODELS
$(warning AI_EVICTION_MODELS is deprecated; use EVICTION_MODELS=ON)
EVICTION_MODELS := $(AI_EVICTION_MODELS)
endif

# Embed scripts/*.lua demo scripts via .incbin (#14). Default ON; pass
# EMBED_DEMO_SCRIPTS=OFF to leave them out of the kernel image (saves ~20KB).
# Lua interpreter + slm.* bindings remain functional when OFF.
EMBED_DEMO_SCRIPTS ?= ON
ifeq ($(EVICTION_MODELS),ON)
    # Model weights require the base eviction framework.
    DISABLE_EVICTION := OFF
endif

# Optional escape hatch for kernel-oriented CMake cache entries that do
# not yet have first-class Make variables. Intended for bring-up /
# one-shot diagnostics such as `-DJETSON_XHCI_REBOOT_ON_NOOP=ON`.
#
# Example:
#   make kernel-clean
#   make kernel PLATFORM=JETSON_ORIN_NANO \
#       EXTRA_KERNEL_CMAKE_ARGS=-DJETSON_XHCI_REBOOT_ON_NOOP=ON
EXTRA_KERNEL_CMAKE_ARGS ?=

# Cargo feature list built from the flags above.
CARGO_FEATURES :=
ifeq ($(EVICTION_MODELS),ON)
    CARGO_FEATURES := ai_eviction_models
else ifeq ($(DISABLE_EVICTION),OFF)
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

KERNEL_BUILD_SIGNATURE := PLATFORM=$(PLATFORM);BUILD_TYPE=$(BUILD_TYPE);AI_SCHED=$(AI_SCHED);HAILO_WIRE_DEBUG=$(HAILO_WIRE_DEBUG);WORK_STEALING=$(WORK_STEALING);SECONDARY_PREEMPT=$(SECONDARY_PREEMPT);DISABLE_EVICTION=$(DISABLE_EVICTION);EVICTION_MODELS=$(EVICTION_MODELS);EVICTION_DEFAULT_POLICY=$(EVICTION_DEFAULT_POLICY);EMBED_DEMO_SCRIPTS=$(EMBED_DEMO_SCRIPTS);JETSON_EL1_SMOKE=$(JETSON_EL1_SMOKE);HAILO_FW_BLOB=$(HAILO_FW_BLOB);SCHEDULER_HEF_BLOB=$(SCHEDULER_HEF_BLOB);USER_HEF_BLOB=$(USER_HEF_BLOB)
KERNEL_KEXEC_BUILD_SIGNATURE := PLATFORM=$(PLATFORM);BUILD_TYPE=$(BUILD_TYPE);AI_SCHED=$(AI_SCHED);HAILO_WIRE_DEBUG=$(HAILO_WIRE_DEBUG);WORK_STEALING=$(WORK_STEALING);SECONDARY_PREEMPT=$(SECONDARY_PREEMPT);DISABLE_EVICTION=$(DISABLE_EVICTION);EVICTION_MODELS=$(EVICTION_MODELS);EVICTION_DEFAULT_POLICY=$(EVICTION_DEFAULT_POLICY);EMBED_DEMO_SCRIPTS=$(EMBED_DEMO_SCRIPTS)
KERNEL_TEST_BUILD_SIGNATURE := PLATFORM=$(PLATFORM);BUILD_TYPE=$(BUILD_TYPE);AI_SCHED=$(AI_SCHED);HAILO_WIRE_DEBUG=$(HAILO_WIRE_DEBUG);WORK_STEALING=$(WORK_STEALING);SECONDARY_PREEMPT=$(SECONDARY_PREEMPT);DISABLE_EVICTION=$(DISABLE_EVICTION);EVICTION_MODELS=$(EVICTION_MODELS);EVICTION_DEFAULT_POLICY=$(EVICTION_DEFAULT_POLICY);EMBED_DEMO_SCRIPTS=$(EMBED_DEMO_SCRIPTS)

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

.PHONY: kernel-config-check
kernel-config-check:
	@sig='$(KERNEL_BUILD_SIGNATURE)'; \
	stamp="$(KERNEL_BUILD_DIR)/.build-config"; \
	if [ -f "$$stamp" ] && [ "$$(cat "$$stamp")" = "$$sig" ]; then \
		:; \
	else \
		echo "Kernel build options changed; reconfiguring $(KERNEL_BUILD_DIR)"; \
		rm -rf "$(KERNEL_BUILD_DIR)"; \
		mkdir -p "$(KERNEL_BUILD_DIR)"; \
		printf '%s\n' "$$sig" > "$$stamp"; \
	fi

.PHONY: kernel
kernel: check-build-dir runtime kernel-config-check $(KERNEL_BUILD_DIR)/Makefile
	@echo "Building kernel..."
	$(CMAKE) --build $(KERNEL_BUILD_DIR)

$(KERNEL_BUILD_DIR)/Makefile:
	@echo "Configuring kernel build..."
	$(CMAKE) -G "Unix Makefiles" -B $(KERNEL_BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DPLATFORM=$(PLATFORM) \
		$(if $(filter ON,$(AI_SCHED)),-DENABLE_AI_SCHEDULER=ON) \
		$(if $(filter ON,$(HAILO_WIRE_DEBUG)),-DHAILO_WIRE_DEBUG=ON) \
		$(if $(filter ON,$(WORK_STEALING)),-DENABLE_WORK_STEALING=ON) \
		$(if $(filter OFF,$(WORK_STEALING)),-DENABLE_WORK_STEALING=OFF) \
		$(if $(filter ON,$(SECONDARY_PREEMPT)),-DSECONDARY_PREEMPT=ON) \
		$(if $(filter ON,$(DISABLE_EVICTION)),-DDISABLE_EVICTION=ON) \
		$(if $(filter ON,$(EVICTION_MODELS)),-DENABLE_EVICTION_MODELS=ON) \
		-DEVICTION_DEFAULT_POLICY=$(EVICTION_DEFAULT_POLICY) \
		$(if $(filter OFF,$(EMBED_DEMO_SCRIPTS)),-DEMBED_DEMO_SCRIPTS=OFF) \
		$(if $(filter ON,$(JETSON_EL1_SMOKE)),-DJETSON_EL1_SMOKE=ON) \
		$(if $(HAILO_FW_BLOB),-DHAILO_FW_BLOB=$(HAILO_FW_BLOB)) \
		$(if $(SCHEDULER_HEF_BLOB),-DSCHEDULER_HEF_BLOB=$(SCHEDULER_HEF_BLOB)) \
		$(if $(USER_HEF_BLOB),-DUSER_HEF_BLOB=$(USER_HEF_BLOB)) \
		$(EXTRA_KERNEL_CMAKE_ARGS) \
		$(if $(USER_HEF_BLOB),-DUSER_HEF_BLOB=$(USER_HEF_BLOB)) \
		$(EXTRA_KERNEL_CMAKE_ARGS) \
		$(MAKE_PROGRAM_ARG)

# kernel-kexec: X86_64-only parallel build of slmos.elf linked at
# 0x20000000 (for Linux→SLM-OS kexec). Uses a separate build directory
# so it never clashes with the default 1 MB bare-metal build.
KERNEL_KEXEC_BUILD_DIR := $(BUILD_DIR)/kernel-kexec
KERNEL_KEXEC_ELF := $(KERNEL_KEXEC_BUILD_DIR)/slmos.elf

.PHONY: kernel-kexec
kernel-kexec: runtime kernel-kexec-config-check $(KERNEL_KEXEC_BUILD_DIR)/Makefile
ifneq ($(PLATFORM),X86_64)
	@echo "kernel-kexec requires PLATFORM=X86_64 (got $(PLATFORM))"; exit 1
endif
	@echo "Building kernel (kexec variant, link address 0x20000000)..."
	$(CMAKE) --build $(KERNEL_KEXEC_BUILD_DIR)
	@echo "kexec ELF: $(KERNEL_KEXEC_ELF)"

.PHONY: kernel-kexec-config-check
kernel-kexec-config-check:
	@sig='$(KERNEL_KEXEC_BUILD_SIGNATURE)'; \
	stamp="$(KERNEL_KEXEC_BUILD_DIR)/.build-config"; \
	if [ -f "$$stamp" ] && [ "$$(cat "$$stamp")" = "$$sig" ]; then \
		:; \
	else \
		echo "Kernel kexec build options changed; reconfiguring $(KERNEL_KEXEC_BUILD_DIR)"; \
		rm -rf "$(KERNEL_KEXEC_BUILD_DIR)"; \
		mkdir -p "$(KERNEL_KEXEC_BUILD_DIR)"; \
		printf '%s\n' "$$sig" > "$$stamp"; \
	fi

$(KERNEL_KEXEC_BUILD_DIR)/Makefile:
	@echo "Configuring kexec kernel build..."
	$(CMAKE) -G "Unix Makefiles" -B $(KERNEL_KEXEC_BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DPLATFORM=$(PLATFORM) \
		-DKEXEC_BUILD=1 \
		$(if $(filter ON,$(AI_SCHED)),-DENABLE_AI_SCHEDULER=ON) \
		$(if $(filter ON,$(HAILO_WIRE_DEBUG)),-DHAILO_WIRE_DEBUG=ON) \
		$(if $(filter ON,$(WORK_STEALING)),-DENABLE_WORK_STEALING=ON) \
		$(if $(filter OFF,$(WORK_STEALING)),-DENABLE_WORK_STEALING=OFF) \
		$(if $(filter ON,$(SECONDARY_PREEMPT)),-DSECONDARY_PREEMPT=ON) \
		$(if $(filter ON,$(DISABLE_EVICTION)),-DDISABLE_EVICTION=ON) \
		$(if $(filter ON,$(EVICTION_MODELS)),-DENABLE_EVICTION_MODELS=ON) \
		-DEVICTION_DEFAULT_POLICY=$(EVICTION_DEFAULT_POLICY) \
		$(if $(filter OFF,$(EMBED_DEMO_SCRIPTS)),-DEMBED_DEMO_SCRIPTS=OFF) \
		$(EXTRA_KERNEL_CMAKE_ARGS) \
		$(MAKE_PROGRAM_ARG)

.PHONY: kernel-kexec-clean
kernel-kexec-clean:
	@echo "Cleaning kexec kernel build..."
	rm -rf $(KERNEL_KEXEC_BUILD_DIR)

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

# Verify the kexec-build scaffolding is structurally intact. Checks the
# kexec ELF is linked at 0x20000000, both MB1 and MB2 headers are
# present, the MB2 ENTRY_ADDRESS tag points at _start, and the
# trampoline32.S UART diag ("KEX\r\n") is in the compiled entry. Also
# validates the bzImage wrapper (setup_header fields kexec-tools'
# bzImage64 probe reads) and shellchecks the Linux-side helper
# scripts.
.PHONY: kexec-verify
kexec-verify:
ifneq ($(PLATFORM),X86_64)
	@echo "kexec-verify requires PLATFORM=X86_64 (got $(PLATFORM))"; exit 1
endif
	@scripts/tests/verify-kexec-build.sh

# test-build-stamp: build twice with sleep 1 between, assert
# SLMOS_BUILD_STAMP advances. Exercises the gen_build_info.cmake
# always-runs custom target end-to-end. Issue #360.
#
# Pass BUILD_DIR through so `make BUILD_DIR=out test-build-stamp` finds
# the generated header in the right place. Invoke via `bash` so a missing
# +x bit on the script (rare, but happens with some VCS export workflows)
# does not break the target.
.PHONY: test-build-stamp
test-build-stamp:
	@BUILD_DIR=$(BUILD_DIR) bash scripts/tests/test-build-stamp-advances.sh

# kernel-bzimage: X86_64-only parallel build of a Linux-bzImage wrapper
# around the kernel. Used as the third kexec loader path alongside
# Multiboot2 (unblocked from "Invalid memory segment" but silent after
# --exec, see x86-64-gpu-inference-status §4.2.k). bzImage is
# kexec-tools' most thoroughly-tested x86 loader; this target is the
# next experiment for reaching SLM-OS's _start post-handoff.
KERNEL_BZIMAGE_BUILD_DIR := $(BUILD_DIR)/kernel-bzimage
KERNEL_BZIMAGE_ELF       := $(KERNEL_BZIMAGE_BUILD_DIR)/slmos.elf
KERNEL_BZIMAGE           := $(KERNEL_BZIMAGE_BUILD_DIR)/slmos.bzimage

.PHONY: kernel-bzimage
kernel-bzimage: runtime $(KERNEL_BZIMAGE_BUILD_DIR)/Makefile
ifneq ($(PLATFORM),X86_64)
	@echo "kernel-bzimage requires PLATFORM=X86_64 (got $(PLATFORM))"; exit 1
endif
	@echo "Building kernel (bzImage variant)..."
	$(CMAKE) --build $(KERNEL_BZIMAGE_BUILD_DIR)
	@echo "Wrapping ELF as Linux bzImage..."
	python3 scripts/make-bzimage.py \
		$(KERNEL_BZIMAGE_ELF) $(KERNEL_BZIMAGE)
	@echo "bzImage: $(KERNEL_BZIMAGE)"

$(KERNEL_BZIMAGE_BUILD_DIR)/Makefile:
	@echo "Configuring bzImage kernel build..."
	$(CMAKE) -G "Unix Makefiles" -B $(KERNEL_BZIMAGE_BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DPLATFORM=$(PLATFORM) \
		-DBZIMAGE_BUILD=1 \
		$(if $(filter ON,$(AI_SCHED)),-DENABLE_AI_SCHEDULER=ON) \
		$(if $(filter ON,$(HAILO_WIRE_DEBUG)),-DHAILO_WIRE_DEBUG=ON) \
		$(if $(filter ON,$(WORK_STEALING)),-DENABLE_WORK_STEALING=ON) \
		$(if $(filter OFF,$(WORK_STEALING)),-DENABLE_WORK_STEALING=OFF) \
		$(if $(filter ON,$(SECONDARY_PREEMPT)),-DSECONDARY_PREEMPT=ON) \
		$(if $(filter ON,$(DISABLE_EVICTION)),-DDISABLE_EVICTION=ON) \
		$(if $(filter ON,$(EVICTION_MODELS)),-DENABLE_EVICTION_MODELS=ON) \
		-DEVICTION_DEFAULT_POLICY=$(EVICTION_DEFAULT_POLICY) \
		$(if $(filter OFF,$(EMBED_DEMO_SCRIPTS)),-DEMBED_DEMO_SCRIPTS=OFF) \
		$(EXTRA_KERNEL_CMAKE_ARGS) \
		$(MAKE_PROGRAM_ARG)

.PHONY: kernel-bzimage-clean
kernel-bzimage-clean:
	@echo "Cleaning bzImage kernel build..."
	rm -rf $(KERNEL_BZIMAGE_BUILD_DIR)

# Deploy + kexec SLM-OS onto a running Linux on test-pc. Alternative
# to the UEFI+SDWire bare-metal path (x86-disk → labctl sdwire flash
# → power_cycle), used when SEC2 needs to inherit its nouveau-unlocked
# state (issue #185 / the 2026-04-17 investigation).
#
# Flags (override at make invocation):
#   KEXEC_MODE=mb2|bzimage  — loader flavour (default mb2 = multiboot2).
#                             bzimage uses make-bzimage.py to wrap the
#                             kexec-linked ELF into a Linux bzImage so
#                             kexec-tools' --type=bzImage loader accepts
#                             it.
#   KEXEC_HOST=user@ip      — override default test-pc SSH target.
#   KEXEC_NO_EXEC=1         — scp the artefact but don't fire kexec.
#
# kexec-deploy depends on the kernel target matching KEXEC_MODE so the
# right artefact is always built before the deploy script runs. The
# per-mode variables are populated lazily via MAKECMDGOALS — bogus
# KEXEC_MODE in the environment shouldn't break unrelated targets
# like `make kernel`.
KEXEC_MODE ?= mb2

ifneq (,$(filter kexec-deploy,$(MAKECMDGOALS)))
  ifeq ($(KEXEC_MODE),bzimage)
    KEXEC_DEPLOY_DEP := kernel-bzimage
    KEXEC_DEPLOY_ARGS := --mode bzimage --bzimage $(KERNEL_BZIMAGE)
    KEXEC_DEPLOY_BUILD_DIR := $(KERNEL_BZIMAGE_BUILD_DIR)
  else ifeq ($(KEXEC_MODE),mb2)
    KEXEC_DEPLOY_DEP := kernel-kexec
    KEXEC_DEPLOY_ARGS := --mode mb2 --elf $(KERNEL_KEXEC_ELF)
    KEXEC_DEPLOY_BUILD_DIR := $(KERNEL_KEXEC_BUILD_DIR)
  else
    $(error KEXEC_MODE must be 'mb2' or 'bzimage' (got '$(KEXEC_MODE)'))
  endif
endif

.PHONY: kexec-deploy
kexec-deploy: $(KEXEC_DEPLOY_DEP)
ifneq ($(PLATFORM),X86_64)
	@echo "kexec-deploy requires PLATFORM=X86_64 (got $(PLATFORM))"; exit 1
endif
	@KERNEL_BUILD_DIR=$(KEXEC_DEPLOY_BUILD_DIR) \
	 scripts/x86-kexec-deploy.sh \
	    $(KEXEC_DEPLOY_ARGS) \
	    $(if $(KEXEC_HOST),--host $(KEXEC_HOST),) \
	    $(if $(KEXEC_NO_EXEC),--no-exec,)

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

# ============================================================================
# hailo-ushim — Linux userspace shim for Hailo Phase 8 debugging
# ============================================================================
# Builds a native Linux ELF that drives hailo_pci's ioctl surface
# directly from userspace. Used to replay SLM-OS's exact boundary-
# submit byte sequence against real Hailo-8L hardware on Pi 5 from a
# debuggable Linux context, separating "bug is in our bytes" from
# "bug is in SLM-OS's bare-metal kernel execution". See
# host-tools/hailo-ushim/README.md for the motivation.

HAILO_USHIM_OUT := build/host-tools/hailo-ushim
HAILO_USHIM_SRCS := \
    host-tools/hailo-ushim/main.c \
    host-tools/hailo-ushim/hailo_dev.c

HAILO_USHIM_CFLAGS := \
    -std=c11 -Wall -Wextra -O2 -g \
    -Ihost-tools/hailo-ushim \
    -D_GNU_SOURCE

.PHONY: hailo-ushim
hailo-ushim:
	@mkdir -p $(dir $(HAILO_USHIM_OUT))
	@echo "Building hailo-ushim (Linux userspace)..."
	$(CC) $(HAILO_USHIM_CFLAGS) -o $(HAILO_USHIM_OUT) $(HAILO_USHIM_SRCS) -lcrypto
	@echo "Built $(HAILO_USHIM_OUT)"
	@echo "Run: sudo $(HAILO_USHIM_OUT) --identify"

.PHONY: hailo-ushim-clean
hailo-ushim-clean:
	rm -f $(HAILO_USHIM_OUT)

# Hailo toolchain artifacts (Phase 6.1). The three scripts under
# scripts/hailo/ produce .onnx, .npy, .har, and .hef files plus
# DFC-generated logs into $(HAILO_BUILD_DIR). No source files live
# there, so it's safe to wipe. See scripts/hailo/*.py for producers.
# The Python scripts carry the same default (REPO_ROOT/build/hailo) —
# keep the two in sync if either changes.
HAILO_BUILD_DIR := build/hailo

.PHONY: hailo-clean
hailo-clean:
	@echo "Cleaning Hailo toolchain artifacts..."
	rm -rf $(HAILO_BUILD_DIR)

# Jetson UEFI-direct boot layout regression test. Structural checks
# on the built Jetson kernel ELF that can't be expressed as linker
# ASSERTs — e.g. "efi_stub_entry contains an `msr vbar_el2`
# instruction that loads `jetson_early_vbar_el2`". Catches
# regressions that would only manifest as a silent hang on Jetson
# hardware. No hardware needed — runs on the host.
.PHONY: test-jetson-uefi-layout
test-jetson-uefi-layout:
	@bash scripts/test-jetson-uefi-layout.sh

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

# Jetson GA10B platform shim (nvidia_gsp_platform.c) — portable surfaces.
#
# The shim is under kernel/arch/arm64/ and normally compiled for Jetson
# only. For host testing we define PLATFORM_JETSON_ORIN_NANO explicitly
# and stub the kernel-side dependencies (pmm, cache, uart). AArch64
# inline asm in the shim is guarded by __aarch64__ so the shim itself
# compiles clean on x86 host; cache.h is bypassed under SLM_HOST_HARNESS.
#
# Covers: firmware_get enum dispatch, VBIOS accessors, dma_alloc/free
# alignment math, bar1 early-out when base unset, vtable installation,
# cache/mb dispatch through vtable. Does NOT cover: MMIO read/write
# paths (require real BAR0) or ARM-specific cache maintenance ops
# (no host equivalent).
.PHONY: test-gsp-platform
test-gsp-platform:
	@mkdir -p build/host-tools
	@echo "Building + running Jetson GSP platform shim tests..."
	$(CC) -std=c11 -Wall -Wextra -O2 -g \
	    -Ihost-tools/gsp-harness -Ikernel/gpu/nvidia -Ikernel/include \
	    -DSLM_HOST_HARNESS=1 -DPLATFORM_JETSON_ORIN_NANO=1 \
	    -o build/host-tools/test_nvidia_gsp_platform \
	    host-tools/gsp-harness/test_nvidia_gsp_platform.c \
	    kernel/arch/arm64/nvidia_gsp_platform.c
	@./build/host-tools/test_nvidia_gsp_platform

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
	cd runtime && SLM_DEFAULT_EVICTION_POLICY=$(EVICTION_DEFAULT_POLICY) cargo build $(RUST_TARGET_FLAG) $(if $(filter Release,$(BUILD_TYPE)),--release,) $(CARGO_FEATURES_FLAG)

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
    QEMU_NET := -device virtio-net-pci,netdev=net0 \
                -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2323-:2323
else ifeq ($(PLATFORM),QEMU_VIRT)
    QEMU_NET := -global virtio-mmio.force-legacy=false \
                -device virtio-net-device,netdev=net0 \
                -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2323-:2323
else
    QEMU_NET :=
endif

# PCIe test device for ARM64 virt — lets pcie_init() discover a
# virtio endpoint on the GPEX root complex without depending on the
# networking stack. virtio-rng is cheap and always available.
#
# Also attaches an `sdhci-pci` controller backed by a raw image so
# the dynamic-kernel-replace Stage 3 SDHCI driver (#369) has
# something to probe in QEMU. Image is created on demand below.
#
# Image is 2 GB to land squarely in QEMU's SDHC emulation regime —
# the QEMU sd-card model emits CSD v1.0 / SDSC (byte-addressed)
# below the 2 GB SDHC threshold per SD Physical Layer Spec, which
# breaks FatFs's block-addressed reads. 2 GB is sparse on disk
# (truncate, not dd), so it costs no real space until QEMU writes.
SDHCI_TEST_IMG := $(KERNEL_TEST_BUILD_DIR)/sdhci-test.img
SDHCI_TEST_IMG_SIZE := 4G

ifeq ($(PLATFORM),QEMU_VIRT)
    # The virt machine has no default `sd` interface (that's a
    # raspi-machine quirk), so we wire it up explicitly:
    #   1. -drive id=...,if=none — define the backing file
    #   2. -device sdhci-pci    — the SDHCI controller itself
    #   3. -device sd-card,drive=... — attach the card to the
    #                                  controller's auto-discovered
    #                                  SD bus (sole bus, no
    #                                  `bus=` arg needed).
    QEMU_PCIE_TEST := -device virtio-rng-pci,bus=pcie.0 \
                      -drive id=slmos-sd,if=none,format=raw,file=$(SDHCI_TEST_IMG) \
                      -device sdhci-pci \
                      -device sd-card,drive=slmos-sd
else
    QEMU_PCIE_TEST :=
endif

# Stage the SDHCI test image so QEMU's `sd-card` device has a
# backing file. Idempotent: only re-creates if missing. The image
# is sparse (truncate, not dd) so the on-disk footprint stays tiny
# (~500 KB after a typical QEMU run) until QEMU actually writes
# blocks. Requires a filesystem that supports sparse files — every
# filesystem that ships with stock SLM-OS dev environments (ext4,
# btrfs, zfs, xfs, NTFS via WSL2) does. tmpfs may refuse 4 GB
# allocations; if so, lower SDHCI_TEST_IMG_SIZE to whatever the
# tmpfs can hold (≥ 2 GB to keep QEMU's sd-card model in SDHC mode).
$(SDHCI_TEST_IMG): | $(KERNEL_TEST_BUILD_DIR)
	@if [ ! -f $@ ]; then \
		echo "Creating sparse $@ ($(SDHCI_TEST_IMG_SIZE), SDHC-sized)"; \
		truncate -s $(SDHCI_TEST_IMG_SIZE) $@; \
	fi

$(KERNEL_TEST_BUILD_DIR):
	@mkdir -p $@

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
	$(QEMU_GUARD) $(QEMU) $(QEMU_COMMON) $(QEMU_NET) $(QEMU_BOOT_ARG)

.PHONY: shell
shell: kernel grub-iso
	@echo "Running in QEMU (interactive shell)..."
	@echo "Press Ctrl+A then X to exit QEMU"
	@echo ""
	$(QEMU_GUARD) $(QEMU) $(QEMU_COMMON) $(QEMU_NET) $(QEMU_BOOT_ARG)

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
# Wrapper to enforce memory and CPU limits when systemd --user is available.
# On hosts without a user systemd session (containers, macOS, WSL, many SSH
# environments), fall back to launching QEMU directly.
QEMU_GUARD := $(shell if command -v systemd-run >/dev/null 2>&1 && systemd-run --user --scope -q true >/dev/null 2>&1; then printf '%s' "systemd-run --user --scope -q -p MemoryMax=$(QEMU_MEM_LIMIT) -p CPUQuota=$(QEMU_CPU_LIMIT)"; fi)

# Build kernel with ENABLE_BOOT_TESTS (runs tests at boot and exits)
.PHONY: kernel-test
kernel-test: check-build-dir runtime kernel-test-config-check $(KERNEL_TEST_BUILD_DIR)/Makefile
	@echo "Building test kernel..."
	$(CMAKE) --build $(KERNEL_TEST_BUILD_DIR)

.PHONY: kernel-test-config-check
kernel-test-config-check:
	@sig='$(KERNEL_TEST_BUILD_SIGNATURE)'; \
	stamp="$(KERNEL_TEST_BUILD_DIR)/.build-config"; \
	if [ -f "$$stamp" ] && [ "$$(cat "$$stamp")" = "$$sig" ]; then \
		:; \
	else \
		echo "Kernel test build options changed; reconfiguring $(KERNEL_TEST_BUILD_DIR)"; \
		rm -rf "$(KERNEL_TEST_BUILD_DIR)"; \
		mkdir -p "$(KERNEL_TEST_BUILD_DIR)"; \
		printf '%s\n' "$$sig" > "$$stamp"; \
	fi

$(KERNEL_TEST_BUILD_DIR)/Makefile:
	@echo "Configuring test kernel build..."
	$(CMAKE) -G "Unix Makefiles" -B $(KERNEL_TEST_BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DPLATFORM=$(PLATFORM) \
		-DENABLE_BOOT_TESTS=ON \
		$(if $(filter ON,$(AI_SCHED)),-DENABLE_AI_SCHEDULER=ON) \
		$(if $(filter ON,$(HAILO_WIRE_DEBUG)),-DHAILO_WIRE_DEBUG=ON) \
		$(if $(filter ON,$(WORK_STEALING)),-DENABLE_WORK_STEALING=ON) \
		$(if $(filter OFF,$(WORK_STEALING)),-DENABLE_WORK_STEALING=OFF) \
		$(if $(filter ON,$(SECONDARY_PREEMPT)),-DSECONDARY_PREEMPT=ON) \
		$(if $(filter ON,$(DISABLE_EVICTION)),-DDISABLE_EVICTION=ON) \
		$(if $(filter ON,$(EVICTION_MODELS)),-DENABLE_EVICTION_MODELS=ON) \
		-DEVICTION_DEFAULT_POLICY=$(EVICTION_DEFAULT_POLICY) \
		$(if $(filter OFF,$(EMBED_DEMO_SCRIPTS)),-DEMBED_DEMO_SCRIPTS=OFF) \
		$(EXTRA_KERNEL_CMAKE_ARGS) \
		$(MAKE_PROGRAM_ARG)

.PHONY: kernel-test-clean
kernel-test-clean:
	@echo "Cleaning test kernel build..."
	rm -rf $(KERNEL_TEST_BUILD_DIR)

# x86-64 test ISO path
KERNEL_TEST_ISO := $(KERNEL_TEST_BUILD_DIR)/slmos-test.iso

.PHONY: test
test: kernel-test $(SDHCI_TEST_IMG)
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
		$(QEMU_PCIE_TEST) \
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
