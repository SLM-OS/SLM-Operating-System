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
    # kernel/mm/vmm.c
    # kernel/mm/allocator.c

    # Scheduler
    kernel/src/task.c
    kernel/src/sched.c
    kernel/src/context.S

    # IPC
    # kernel/ipc/message.c
    # kernel/ipc/shared_buffer.c
    # kernel/ipc/queue.c

    # GPU integration
    # kernel/gpu/jetson_gpu.c
    # kernel/gpu/memory_map.c
)

# Include directories
set(KERNEL_INCLUDES
    ${CMAKE_SOURCE_DIR}/kernel/include
)

# Build kernel ELF
add_executable(slmos.elf ${KERNEL_SOURCES})
target_include_directories(slmos.elf PRIVATE ${KERNEL_INCLUDES})
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
