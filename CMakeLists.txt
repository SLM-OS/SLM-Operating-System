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

# Kernel source files (add as you implement them)
set(KERNEL_SOURCES
    # Architecture-specific (ARM64)
    # kernel/arch/arm64/boot.S
    # kernel/arch/arm64/context.S
    # kernel/arch/arm64/mmu.c

    # Memory management
    # kernel/mm/pmm.c
    # kernel/mm/vmm.c
    # kernel/mm/allocator.c

    # Scheduler
    # kernel/sched/core.c
    # kernel/sched/task.c
    # kernel/sched/smp.c

    # IPC
    # kernel/ipc/message.c
    # kernel/ipc/shared_buffer.c
    # kernel/ipc/queue.c

    # Drivers
    # kernel/drivers/uart.c
    # kernel/drivers/gpio.c
    # kernel/drivers/i2c.c

    # GPU integration
    # kernel/gpu/jetson_gpu.c
    # kernel/gpu/memory_map.c
)

# Include directories
set(KERNEL_INCLUDES
    ${CMAKE_SOURCE_DIR}/kernel/include
    ${CMAKE_SOURCE_DIR}/kernel/arch/arm64/include
)

# Build kernel ELF (uncomment when you have source files)
# add_executable(kernel.elf ${KERNEL_SOURCES})
# target_include_directories(kernel.elf PRIVATE ${KERNEL_INCLUDES})
# target_link_options(kernel.elf PRIVATE
#     -T${LINKER_SCRIPT}
#     -nostdlib
#     -lgcc
# )

# Create binary image from ELF (for booting)
# add_custom_command(
#     TARGET kernel.elf POST_BUILD
#     COMMAND ${CMAKE_OBJCOPY} -O binary kernel.elf kernel.bin
#     COMMENT "Creating kernel.bin"
# )

# Print configuration summary
message(STATUS "")
message(STATUS "SLM-OS Build Configuration")
message(STATUS "  Version:    ${PROJECT_VERSION}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "")
