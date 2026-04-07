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
# Note: -mgeneral-regs-only is added to kernel target only (not lua library)

# CPU selection - platform-specific
if(PLATFORM STREQUAL "X86_64")
    # x86-64: no -mcpu, use x86-specific flags
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -fno-stack-protector -fno-pic")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mno-red-zone")
elseif(PLATFORM STREQUAL "JETSON_ORIN_NANO")
    set(TARGET_CPU "cortex-a78ae")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=${TARGET_CPU}")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mcpu=${TARGET_CPU}")
elseif(PLATFORM STREQUAL "RASPI5")
    set(TARGET_CPU "cortex-a76")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=${TARGET_CPU}")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mcpu=${TARGET_CPU}")
else()
    # QEMU_VIRT - use cortex-a76 (common modern ARM core)
    set(TARGET_CPU "cortex-a76")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=${TARGET_CPU}")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -mcpu=${TARGET_CPU}")
endif()

# Debug/Release configurations
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# Platform selection
set(PLATFORM "QEMU_VIRT" CACHE STRING "Target platform (QEMU_VIRT, JETSON_ORIN_NANO, RASPI5, or X86_64)")
set_property(CACHE PLATFORM PROPERTY STRINGS "QEMU_VIRT" "JETSON_ORIN_NANO" "RASPI5" "X86_64")

# Add platform define
add_compile_definitions(PLATFORM_${PLATFORM}=1)

# Enable semihosting for QEMU (used for clean test exit)
if(PLATFORM STREQUAL "QEMU_VIRT")
    add_compile_definitions(ENABLE_SEMIHOSTING=1)
endif()

# Boot tests - run all tests at kernel startup (for CI/testing)
# Default OFF for interactive use; `make test` enables this
option(ENABLE_BOOT_TESTS "Run tests at boot instead of interactive shell" OFF)
if(ENABLE_BOOT_TESTS)
    add_compile_definitions(ENABLE_BOOT_TESTS=1)
endif()

# Linker script - platform-specific
if(PLATFORM STREQUAL "X86_64")
    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/kernel-x86_64.ld)
elseif(PLATFORM STREQUAL "JETSON_ORIN_NANO")
    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/kernel-jetson.ld)
elseif(PLATFORM STREQUAL "RASPI5")
    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/kernel-raspi5.ld)
else()
    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/kernel.ld)
endif()

# Rust runtime library
# Built by `make runtime` before `make kernel`
if(PLATFORM STREQUAL "X86_64")
    set(RUST_TARGET "x86_64-unknown-none")
else()
    set(RUST_TARGET "aarch64-unknown-none")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(RUST_LIB_DIR ${CMAKE_SOURCE_DIR}/runtime/target/${RUST_TARGET}/release)
else()
    set(RUST_LIB_DIR ${CMAKE_SOURCE_DIR}/runtime/target/${RUST_TARGET}/debug)
endif()
set(RUST_LIB ${RUST_LIB_DIR}/libslm_runtime.a)

# Kernel source files
set(KERNEL_SOURCES
    # ==========================================================================
    # Architecture-specific
    # ==========================================================================
    # ARM64 architecture files (excluded for x86-64)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/arch/arm64/boot.S>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/arch/arm64/context.S>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/arch/arm64/mmu.S>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/arch/arm64/vectors.S>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/arch/arm64/smp_boot.S>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/arch/arm64/exceptions.c>
    $<$<AND:$<NOT:$<STREQUAL:${PLATFORM},RASPI5>>,$<NOT:$<STREQUAL:${PLATFORM},X86_64>>>:kernel/arch/arm64/efi_stub.c>
    # x86-64 architecture files (trampoline32.S handled by custom command below)
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/entry64.S>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/idt.S>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/context.S>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/idt.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/acpi.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/lapic.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/ioapic.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/pic.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/timer_x86.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/platform_x86.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/pci.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/nvidia_gpu.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/ap_trampoline.S>

    # ==========================================================================
    # Memory Management
    # ==========================================================================
    kernel/mm/pmm.c
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/mm/vmm.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/mm/ncmem.c>

    # ==========================================================================
    # Scheduler
    # ==========================================================================
    kernel/sched/task.c
    kernel/sched/sched.c
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/sched/smp.c>

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
    $<$<STREQUAL:${PLATFORM},RASPI5>:kernel/drivers/uart_rp1.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/drivers/uart_tegra.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/drivers/uart_x86.c>

    # BPMP - Jetson only (required for clock control)
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/drivers/bpmp.c>

    # Interrupt controller and timer (ARM64 only — x86-64 uses pic.c + timer_x86.c)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/drivers/gic.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/drivers/timer.c>

    # Block device abstraction and RAM disk
    kernel/drivers/blkdev.c
    kernel/drivers/ramdisk.c

    # ==========================================================================
    # Core Kernel
    # ==========================================================================
    kernel/src/main.c
    kernel/src/panic.c
    kernel/src/kprintf.c
    kernel/src/string.c

    # Shell (core + command modules)
    kernel/src/shell.c
    kernel/src/shell_sys.c
    kernel/src/shell_fs.c
    kernel/src/shell_exec.c
    kernel/src/shell_component.c
    kernel/src/component_runtime.c
    kernel/src/msg_router.c
    kernel/src/help.c
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/src/dtb.c>
    kernel/src/elf.c
    kernel/src/vfs.c

    # ==========================================================================
    # Filesystem
    # ==========================================================================
    # LittleFS library
    kernel/lib/littlefs/lfs.c
    kernel/lib/littlefs/lfs_util.c

    # LittleFS wrapper and VFS adapter
    kernel/fs/littlefs_slm.c
    kernel/fs/littlefs_vfs.c

    # Component system and FFI (x86-64 provides stubs in platform_x86.c)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/src/component.c>
    kernel/src/slm_ffi.c

    # Semihosting (for QEMU test exit)
    kernel/src/semihosting.c

    # ==========================================================================
    # GPU Integration
    # ==========================================================================
    kernel/gpu/gpu.c
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/gpu/cache.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>>:kernel/gpu/gpu_stub.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/gpu/gpu_nvidia.c>

    # ==========================================================================
    # Networking (QEMU only for now)
    # ==========================================================================
    $<$<STREQUAL:${PLATFORM},QEMU_VIRT>:kernel/drivers/virtio_net.c>
    $<$<STREQUAL:${PLATFORM},QEMU_VIRT>:kernel/net/sys_arch.c>
    $<$<STREQUAL:${PLATFORM},QEMU_VIRT>:kernel/net/lwip_slm.c>
    $<$<STREQUAL:${PLATFORM},QEMU_VIRT>:kernel/src/net_shell.c>

    # ==========================================================================
    # Lua Scripting - Lua library includes all FP-using code
    # lua_slm.c and lua_shell.c are compiled as part of lua library
    # ==========================================================================
)

# Test sources (Unity framework)
set(TEST_SOURCES
    kernel/tests/unity.c
    kernel/tests/test_harness.c
    kernel/tests/test_ipc.c
    kernel/tests/test_pi_mutex.c
    kernel/tests/test_gpu.c
    kernel/tests/test_vfs.c
    kernel/tests/test_shell.c
    kernel/tests/test_pmm.c
    kernel/tests/test_littlefs.c
    kernel/tests/test_x86_boot.c
    # ARM64-specific test files (use inline asm not available on x86-64)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_scheduler.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_model_mem.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_component.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_vmm.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_net.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_lua.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_integration.c>
)

# lwIP library sources (QEMU networking only)
set(LWIP_SOURCES
    kernel/lib/lwip/src/core/init.c
    kernel/lib/lwip/src/core/def.c
    kernel/lib/lwip/src/core/inet_chksum.c
    kernel/lib/lwip/src/core/ip.c
    kernel/lib/lwip/src/core/mem.c
    kernel/lib/lwip/src/core/memp.c
    kernel/lib/lwip/src/core/netif.c
    kernel/lib/lwip/src/core/pbuf.c
    kernel/lib/lwip/src/core/raw.c
    kernel/lib/lwip/src/core/stats.c
    kernel/lib/lwip/src/core/sys.c
    kernel/lib/lwip/src/core/tcp.c
    kernel/lib/lwip/src/core/tcp_in.c
    kernel/lib/lwip/src/core/tcp_out.c
    kernel/lib/lwip/src/core/timeouts.c
    kernel/lib/lwip/src/core/udp.c
    kernel/lib/lwip/src/core/ipv4/autoip.c
    kernel/lib/lwip/src/core/ipv4/dhcp.c
    kernel/lib/lwip/src/core/ipv4/etharp.c
    kernel/lib/lwip/src/core/ipv4/icmp.c
    kernel/lib/lwip/src/core/ipv4/igmp.c
    kernel/lib/lwip/src/core/ipv4/ip4.c
    kernel/lib/lwip/src/core/ipv4/ip4_addr.c
    kernel/lib/lwip/src/core/ipv4/ip4_frag.c
    kernel/lib/lwip/src/netif/ethernet.c
)

# Lua 5.4 library sources
set(LUA_SOURCES
    kernel/lib/lua/src/lapi.c
    kernel/lib/lua/src/lauxlib.c
    kernel/lib/lua/src/lbaselib.c
    kernel/lib/lua/src/lcode.c
    kernel/lib/lua/src/lcorolib.c
    kernel/lib/lua/src/lctype.c
    kernel/lib/lua/src/ldblib.c
    kernel/lib/lua/src/ldebug.c
    kernel/lib/lua/src/ldo.c
    kernel/lib/lua/src/ldump.c
    kernel/lib/lua/src/lfunc.c
    kernel/lib/lua/src/lgc.c
    kernel/lib/lua/src/linit.c
    kernel/lib/lua/src/llex.c
    kernel/lib/lua/src/lmathlib.c
    kernel/lib/lua/src/lmem.c
    kernel/lib/lua/src/lobject.c
    kernel/lib/lua/src/lopcodes.c
    kernel/lib/lua/src/lparser.c
    kernel/lib/lua/src/lstate.c
    kernel/lib/lua/src/lstring.c
    kernel/lib/lua/src/lstrlib.c
    kernel/lib/lua/src/ltable.c
    kernel/lib/lua/src/ltablib.c
    kernel/lib/lua/src/ltm.c
    kernel/lib/lua/src/lundump.c
    kernel/lib/lua/src/lvm.c
    kernel/lib/lua/src/lzio.c
)

# Include directories
set(KERNEL_INCLUDES
    ${CMAKE_SOURCE_DIR}/kernel/include
    ${CMAKE_SOURCE_DIR}/kernel/tests
    ${CMAKE_SOURCE_DIR}/kernel/gpu
    ${CMAKE_SOURCE_DIR}/kernel/lib/littlefs
    ${CMAKE_SOURCE_DIR}/kernel/lib/lwip/src/include
    ${CMAKE_SOURCE_DIR}/kernel/lib/lua/src
)

# Build kernel ELF
if(PLATFORM STREQUAL "QEMU_VIRT")
    add_executable(slmos.elf ${KERNEL_SOURCES} ${TEST_SOURCES} ${LWIP_SOURCES})
elseif(PLATFORM STREQUAL "X86_64")
    add_executable(slmos.elf ${KERNEL_SOURCES} ${TEST_SOURCES})
else()
    add_executable(slmos.elf ${KERNEL_SOURCES} ${TEST_SOURCES})
endif()
target_include_directories(slmos.elf PRIVATE ${KERNEL_INCLUDES})

if(PLATFORM STREQUAL "X86_64")
    # x86-64: no -mgeneral-regs-only (ARM64-only flag)
    target_include_directories(slmos.elf PRIVATE ${CMAKE_SOURCE_DIR}/kernel/include/compat)
    target_compile_definitions(slmos.elf PRIVATE SLM_INTEGRATED_BUILD=1)

    # Build trampoline32.S with -m32, then objcopy to elf64-x86-64
    set(TRAMPOLINE_OBJ ${CMAKE_BINARY_DIR}/trampoline32.o)
    add_custom_command(
        OUTPUT ${TRAMPOLINE_OBJ}
        COMMAND gcc -c -m32 -I${CMAKE_SOURCE_DIR}/kernel/include -DPLATFORM_X86_64=1
                -o ${CMAKE_BINARY_DIR}/trampoline32_temp.o
                ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/trampoline32.S
        COMMAND objcopy -O elf64-x86-64
                ${CMAKE_BINARY_DIR}/trampoline32_temp.o
                ${TRAMPOLINE_OBJ}
        COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/trampoline32_temp.o
        DEPENDS ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64/trampoline32.S
        COMMENT "Assembling trampoline32.S (32-bit → 64-bit)"
    )
    add_custom_target(trampoline32 DEPENDS ${TRAMPOLINE_OBJ})
    add_dependencies(slmos.elf trampoline32)
    target_link_options(slmos.elf PRIVATE ${TRAMPOLINE_OBJ})

    # Disable PIE (bare-metal kernel loaded at fixed address)
    # -Wl,-n (--nmagic): don't page-align sections — keeps all sections in a
    # single LOAD segment, which is required for QEMU's Multiboot2 loader
    target_link_options(slmos.elf PRIVATE -no-pie -Wl,-n)
else()
    # ARM64: -mgeneral-regs-only avoids FP register pollution
    # -fPIE for position-independent code
    target_compile_options(slmos.elf PRIVATE -mgeneral-regs-only -fPIE)
endif()

# Configure LittleFS for freestanding use (no libc)
# - LFS_NO_MALLOC: We use static buffers
# - LFS_NO_ASSERT: No __assert_func in freestanding
# - LFS_NO_DEBUG/WARN/ERROR: No printf in freestanding
set_source_files_properties(
    kernel/lib/littlefs/lfs.c
    kernel/lib/littlefs/lfs_util.c
    PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-shadow -Wno-cast-align -DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR"
)

# Configure lwIP for freestanding use (QEMU only)
if(PLATFORM STREQUAL "QEMU_VIRT")
    set_source_files_properties(
        ${LWIP_SOURCES}
        PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-variable -Wno-type-limits -Wno-cast-align -Wno-sign-compare -Wno-strict-prototypes -Wno-redundant-decls"
    )
endif()

# Lua library - uses floating-point so it doesn't get -mgeneral-regs-only
# setjmp.S is architecture-specific (saves callee-saved FP regs on ARM64)
if(PLATFORM STREQUAL "X86_64")
    set(LUA_SETJMP kernel/arch/x86_64/setjmp.S)
else()
    set(LUA_SETJMP kernel/arch/arm64/setjmp.S)
endif()

add_library(lua STATIC ${LUA_SOURCES} kernel/src/lua_stubs.c kernel/src/lua_slm.c kernel/src/lua_shell.c ${LUA_SETJMP})
target_include_directories(lua PRIVATE ${KERNEL_INCLUDES} kernel/lib/lua/src)

# Lua-specific compile flags (relaxed warnings for C99 Lua code)
if(PLATFORM STREQUAL "X86_64")
    # Lua needs SSE for double (x86-64 ABI returns doubles in xmm0).
    # Override the global -mno-sse flags; only -mno-red-zone for kernel safety.
    target_compile_options(lua PRIVATE
        -std=gnu11
        -ffreestanding
        -nostdlib
        -mno-red-zone -fno-stack-protector -fno-pic
        -msse -msse2
        -Wno-unused-parameter -Wno-unused-variable -Wno-type-limits
        -Wno-cast-align -Wno-sign-compare -Wno-strict-prototypes
        -Wno-redundant-decls -Wno-shadow -Wno-implicit-fallthrough
        -Wno-unused-but-set-variable -Wno-missing-field-initializers
        -Wno-float-conversion -Wno-conversion -Wno-error
    )
    target_include_directories(lua PRIVATE ${CMAKE_SOURCE_DIR}/kernel/include/compat)
else()
    target_compile_options(lua PRIVATE
        -std=gnu11
        -ffreestanding
        -nostdlib
        -mcpu=${TARGET_CPU}
        -Wno-unused-parameter -Wno-unused-variable -Wno-type-limits
        -Wno-cast-align -Wno-sign-compare -Wno-strict-prototypes
        -Wno-redundant-decls -Wno-shadow -Wno-implicit-fallthrough
        -Wno-unused-but-set-variable -Wno-missing-field-initializers
        -Wno-float-conversion -Wno-conversion -Wno-error
    )
endif()

# Lua configuration defines
target_compile_definitions(lua PRIVATE
    LUA_USE_C89=1
    LUA_32BITS=1
    LUA_USE_LOCALE=0
    LUAI_MAXSTACK=15000
    LUA_MINBUFFER=32
    $<$<STREQUAL:${PLATFORM},X86_64>:PLATFORM_X86_64=1>
)

# Link with Lua library
target_link_libraries(slmos.elf PRIVATE lua)

# Link with Rust runtime (weak C stubs provide fallback if not built)
# --whole-archive forces all Rust symbols to be included, overriding weak C stubs
if(EXISTS ${RUST_LIB})
    target_link_libraries(slmos.elf PRIVATE -Wl,--whole-archive ${RUST_LIB} -Wl,--no-whole-archive)
    message(STATUS "  Rust lib:   ${RUST_LIB}")
else()
    message(STATUS "  Rust lib:   (not built yet - run 'make runtime' first, using C stubs)")
endif()

target_link_options(slmos.elf PRIVATE
    -T${LINKER_SCRIPT}
    -nostdlib
)
# -lgcc must come after object files (provides __popcountdi2 etc.)
target_link_libraries(slmos.elf PRIVATE gcc)

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
