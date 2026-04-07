# SLM-OS Top-Level Makefile
# Orchestrates C kernel (CMake) and Rust runtime (Cargo) builds

# ============================================================================
# Configuration
# ============================================================================

# Build type: Debug or Release
BUILD_TYPE ?= Debug

# Platform: QEMU_VIRT, JETSON_ORIN_NANO, or RASPI5
PLATFORM ?= QEMU_VIRT

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
		$(MAKE_PROGRAM_ARG)

.PHONY: kernel-clean
kernel-clean:
	@echo "Cleaning kernel build..."
	rm -rf $(KERNEL_BUILD_DIR)

.PHONY: kernel-rebuild
kernel-rebuild: kernel-clean kernel

# ============================================================================
# Runtime (Rust) targets
# ============================================================================

.PHONY: runtime
runtime:
	@echo "Building runtime..."
	cd runtime && cargo build $(if $(filter Release,$(BUILD_TYPE)),--release,)

.PHONY: runtime-clean
runtime-clean:
	@echo "Cleaning runtime build..."
	cd runtime && cargo clean

.PHONY: runtime-rebuild
runtime-rebuild: runtime-clean runtime

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

.PHONY: run
run: kernel
	@echo "Running in QEMU..."
	$(QEMU) \
		-machine $(QEMU_MACHINE) \
		-cpu $(QEMU_CPU) \
		-smp cores=$(QEMU_CORES) \
		-m $(QEMU_MEMORY) \
		-nographic \
		-kernel $(KERNEL_ELF)

.PHONY: shell
shell: kernel
	@echo "Running in QEMU (interactive shell)..."
	@echo "Press Ctrl+A then X to exit QEMU"
	@echo ""
	$(QEMU) \
		-machine $(QEMU_MACHINE) \
		-cpu $(QEMU_CPU) \
		-smp cores=$(QEMU_CORES) \
		-m $(QEMU_MEMORY) \
		-nographic \
		-kernel $(KERNEL_ELF)

.PHONY: debug
debug: kernel
	@echo "Starting QEMU with GDB server on port 1234..."
	@echo "In another terminal, run: make gdb"
	$(QEMU) \
		-machine $(QEMU_MACHINE) \
		-cpu $(QEMU_CPU) \
		-smp cores=$(QEMU_CORES) \
		-m $(QEMU_MEMORY) \
		-nographic \
		-kernel $(KERNEL_ELF) \
		-S \
		-gdb tcp::1234

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
TEST_TIMEOUT := 60

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
	@timeout $(TEST_TIMEOUT) $(QEMU) \
		-machine $(QEMU_MACHINE) \
		-cpu $(QEMU_CPU) \
		-smp cores=$(QEMU_CORES) \
		-m $(QEMU_MEMORY) \
		-nographic \
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
	@timeout $(TEST_TIMEOUT) $(QEMU) \
		-machine $(QEMU_MACHINE) \
		-cpu $(QEMU_CPU) \
		-smp cores=$(QEMU_CORES) \
		-m $(QEMU_MEMORY) \
		-nographic \
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
