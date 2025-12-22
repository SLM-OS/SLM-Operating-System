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
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding -nostdlib")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=cortex-a78ae -mgeneral-regs-only")

set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mcpu=cortex-a78ae")

# Debug/Release configurations
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# Linker script
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/kernel.ld)

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
    # Boot and core
    kernel/src/boot.S
    kernel/src/main.c
    kernel/src/panic.c

    # Drivers
    kernel/drivers/uart_pl011.c
    # kernel/drivers/uart_tegra.c    # Jetson Orin Nano

    # Interrupt handling
    kernel/src/gic.c
    kernel/src/timer.c
    kernel/src/vectors.S
    kernel/src/exceptions.c
    # kernel/drivers/gpio.c
    # kernel/drivers/i2c.c

    # Architecture-specific (ARM64)
    # kernel/arch/arm64/context.S
    # kernel/arch/arm64/mmu.c

    # Memory management
    kernel/src/pmm.c
    kernel/src/vmm.c
    kernel/src/mmu.S

    # Scheduler
    kernel/src/task.c
    kernel/src/sched.c
    kernel/src/context.S

    # SMP (multi-core support)
    kernel/src/smp.c
    kernel/src/smp_boot.S

    # IPC
    kernel/src/ipc.c

    # Synchronization
    kernel/src/pi_mutex.c

    # FFI (C/Rust interface)
    kernel/src/slm_ffi.c

    # GPU integration
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
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "")
