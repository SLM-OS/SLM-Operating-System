cmake_minimum_required(VERSION 3.20)

# Project definition
project(slm-os
    VERSION 0.1.0
    LANGUAGES C ASM
)

# Require out-of-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use: cmake -B build")
endif()

# Build configuration
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Compiler flags - strict warning level (warnings are errors)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wshadow -Wcast-align -Wstrict-prototypes")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wformat=2 -Wno-format-nonliteral")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wunused -Wunreachable-code")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding -nostdlib")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=cortex-a78ae -mgeneral-regs-only")

set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mcpu=cortex-a78ae")

# Debug/Release configurations
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# Platform selection (QEMU_VIRT or JETSON_ORIN_NANO)
set(PLATFORM "QEMU_VIRT" CACHE STRING "Target platform (QEMU_VIRT or JETSON_ORIN_NANO)")
set_property(CACHE PLATFORM PROPERTY STRINGS "QEMU_VIRT" "JETSON_ORIN_NANO")

# Add platform define
add_compile_definitions(PLATFORM_${PLATFORM}=1)

# Enable semihosting for QEMU (used for clean test exit)
if(PLATFORM STREQUAL "QEMU_VIRT")
    add_compile_definitions(ENABLE_SEMIHOSTING=1)
endif()

# Linker script - platform-specific
if(PLATFORM STREQUAL "JETSON_ORIN_NANO")
    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/kernel-jetson.ld)
else()
    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/kernel.ld)
endif()

# Rust runtime library
# Built by `make runtime` before `make kernel`
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(RUST_LIB_DIR ${CMAKE_SOURCE_DIR}/runtime/target/aarch64-unknown-none/release)
else()
    set(RUST_LIB_DIR ${CMAKE_SOURCE_DIR}/runtime/target/aarch64-unknown-none/debug)
endif()
set(RUST_LIB ${RUST_LIB_DIR}/libslm_runtime.a)

# Kernel source files
set(KERNEL_SOURCES
    # ==========================================================================
    # Architecture-specific (ARM64)
    # ==========================================================================
    kernel/arch/arm64/boot.S
    kernel/arch/arm64/context.S
    kernel/arch/arm64/mmu.S
    kernel/arch/arm64/vectors.S
    kernel/arch/arm64/smp_boot.S
    kernel/arch/arm64/exceptions.c

    # ==========================================================================
    # Memory Management
    # ==========================================================================
    kernel/mm/pmm.c
    kernel/mm/vmm.c

    # ==========================================================================
    # Scheduler
    # ==========================================================================
    kernel/sched/task.c
    kernel/sched/sched.c
    kernel/sched/smp.c

    # ==========================================================================
    # Inter-Process Communication
    # ==========================================================================
    kernel/ipc/ipc.c
    kernel/ipc/pi_mutex.c

    # ==========================================================================
    # Drivers
    # ==========================================================================
    # UART - platform-specific
    $<$<STREQUAL:${PLATFORM},QEMU_VIRT>:kernel/drivers/uart_pl011.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/drivers/uart_tegra.c>

    # BPMP - Jetson only (required for clock control)
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/drivers/bpmp.c>

    # Interrupt controller and timer
    kernel/drivers/gic.c
    kernel/drivers/timer.c

    # ==========================================================================
    # Core Kernel
    # ==========================================================================
    kernel/src/main.c
    kernel/src/panic.c
    kernel/src/kprintf.c

    # Shell and utilities
    kernel/src/shell.c
    kernel/src/dtb.c
    kernel/src/elf.c
    kernel/src/vfs.c

    # Component system and FFI
    kernel/src/component.c
    kernel/src/slm_ffi.c

    # Semihosting (for QEMU test exit)
    kernel/src/semihosting.c

    # ==========================================================================
    # GPU Integration
    # ==========================================================================
    kernel/gpu/gpu.c
    kernel/gpu/cache.c
    kernel/gpu/gpu_stub.c
    # kernel/gpu/gpu_tegra.c    # Jetson Orin Nano (future)
)

# Test sources (Unity framework)
set(TEST_SOURCES
    kernel/tests/unity.c
    kernel/tests/test_harness.c
    kernel/tests/test_ipc.c
    kernel/tests/test_scheduler.c
    kernel/tests/test_model_mem.c
    kernel/tests/test_pi_mutex.c
    kernel/tests/test_gpu.c
    kernel/tests/test_component.c
    kernel/tests/test_vfs.c
    kernel/tests/test_shell.c
    kernel/tests/test_vmm.c
)

# Include directories
set(KERNEL_INCLUDES
    ${CMAKE_SOURCE_DIR}/kernel/include
    ${CMAKE_SOURCE_DIR}/kernel/tests
    ${CMAKE_SOURCE_DIR}/kernel/gpu
)

# Build kernel ELF
add_executable(slmos.elf ${KERNEL_SOURCES} ${TEST_SOURCES})
target_include_directories(slmos.elf PRIVATE ${KERNEL_INCLUDES})

# Link with Rust runtime (if it exists)
if(EXISTS ${RUST_LIB})
    target_link_libraries(slmos.elf PRIVATE ${RUST_LIB})
    message(STATUS "  Rust lib:   ${RUST_LIB}")
else()
    message(STATUS "  Rust lib:   (not built yet - run 'make runtime' first)")
endif()

target_link_options(slmos.elf PRIVATE
    -T${LINKER_SCRIPT}
    -nostdlib
    -lgcc
)

# Create binary image from ELF (for booting)
add_custom_command(
    TARGET slmos.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary slmos.elf slmos.bin
    COMMENT "Creating slmos.bin"
)

# Print configuration summary
message(STATUS "")
message(STATUS "SLM-OS Build Configuration")
message(STATUS "  Version:    ${PROJECT_VERSION}")
message(STATUS "  Platform:   ${PLATFORM}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "")
