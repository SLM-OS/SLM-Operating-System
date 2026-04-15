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

# Deferred-scheduling preemption (ELR-trampoline) for secondary CPUs.
# Calling switch_to() directly from an exception handler hangs on real
# ARM64 hardware (abandoned exception frame); the trampoline runs
# schedule() in task context after eret. See issue #57 and
# docs/pi5-secondary-cpu-preemption.md.
#
# Capability-neutral name — works on any ARM64 platform that has this
# problem. Pi 5 historically used `PI5_SECONDARY_PREEMPT`; that spelling
# is kept as a backward-compatible alias below so existing Pi 5
# Makefile / CMake invocations keep working.
#
# Only applicable on platforms that have the exception-frame problem
# AND the NC memory backing for the trampoline's per-CPU state. That is
# the PLATFORM_HAS_NC_MEMORY platforms today — Pi 5 and Jetson. QEMU
# virt does not need the trampoline (its emulated timer IRQ allows
# schedule-from-ISR), and x86-64 has no ELR. If SECONDARY_PREEMPT=ON
# is passed on a non-applicable platform it is silently ignored, with
# a status message for diagnosability.
#
# Default OFF because turning it on requires functional timer IRQ
# delivery on all CPUs. On Pi 5 that is still blocked by #134
# (the hardware timer-IRQ delivery regression); cooperative preemption
# via PI5_COOP_PREEMPT is the current Pi 5 default. On Jetson this
# option becomes relevant after the P1.0 GICR_IGROUPR0 fast-path plus
# the MPIDR formula fix in vectors.S (Jetson plan P3 step 2).
option(SECONDARY_PREEMPT "Enable deferred-scheduling preemption on secondary CPUs (ARM64, NC-memory platforms)" OFF)
# Backward-compat alias: users can still pass -DPI5_SECONDARY_PREEMPT=ON.
option(PI5_SECONDARY_PREEMPT "Deprecated alias for SECONDARY_PREEMPT" OFF)
if(PI5_SECONDARY_PREEMPT)
    set(SECONDARY_PREEMPT ON)
endif()
if(SECONDARY_PREEMPT)
    if(PLATFORM STREQUAL "RASPI5" OR PLATFORM STREQUAL "JETSON_ORIN_NANO")
        add_compile_definitions(SECONDARY_PREEMPT=1)
    else()
        message(STATUS "SECONDARY_PREEMPT=ON ignored on PLATFORM=${PLATFORM} "
                       "(only Pi 5 and Jetson use the ELR trampoline)")
    endif()
endif()
# Pi 5 FIQ-routed timer (Phase 2 of issue #99): el1_fiq_handler dispatches
# GICC_AIAR on IRQ 30 to timer_handler(), because non-secure IGROUPR writes
# cannot promote Group 0 → Group 1 on this GICv2 and the timer therefore
# arrives as FIQ rather than IRQ. Implies SECONDARY_PREEMPT because the
# trampoline is required to avoid switch_to from exception context.
option(PI5_FIQ_TIMER "Route timer IRQ through the FIQ vector on Pi 5 (experimental, not active for coop preempt)" OFF)
if(PLATFORM STREQUAL "RASPI5" AND PI5_FIQ_TIMER)
    add_compile_definitions(PI5_FIQ_TIMER=1)
endif()

# Pi 5 IRQ-delivery diagnostics (issue #99 Phase 1). Adds EL2 register
# snapshot in boot.S, per-CPU per-vector exception counters in vectors.S,
# a real el1_fiq handler that captures the FIQ source, and the `diag`
# shell command. Behavior-preserving; turn OFF once #99 is root-caused
# and the Phase 2 fix is in place.
option(PI5_IRQ_DIAG "Pi 5 IRQ-delivery diagnostics (issue #99 Phase 1)" ON)
if(PLATFORM STREQUAL "RASPI5" AND PI5_IRQ_DIAG)
    add_compile_definitions(PI5_IRQ_DIAG=1)
endif()

# Cooperative-preemption fallback — for ARM64 platforms where the GIC
# security configuration prevents timer IRQs from reaching the kernel.
# See docs/pi5-preemption-resolution.md for the Pi 5 diagnostic chain
# and docs/jetson-capstone-execution-plan.md revision v4 for the
# identical issue on Jetson Orin Nano.
#
# Symptom on both platforms: NS writes to the GIC's group registers
# (GICD_IGROUPR on Pi 5's GICv2, GICR_IGROUPR0 on Jetson's GICv3) are
# silently ignored by the hardware because the GIC runs with two
# security states and the group config is owned by EL3 firmware. Timer
# PPIs therefore stay in Group 0 and arrive as FIQ rather than IRQ; the
# EL1 IRQ vector never fires.
#
# Workaround: `schedule()` checks `CNTPCT_EL0` on every entry and
# synthesizes a `scheduler_tick()` call whenever ≥10 ms has elapsed on
# that CPU since the last synthetic tick. This gives "cooperative
# preemption" — scheduling decisions (AI policy, deadline boosts,
# migration) take effect at the next yield for any workload that yields
# at all, which covers all tasks that poll UART / message queues / locks.
# A pure CPU-bound loop with no yield still monopolizes its CPU.
#
# Enabled by default on platforms where the real timer-IRQ path is
# blocked (Pi 5 and Jetson). The legacy `PI5_COOP_PREEMPT` option is
# kept as a backward-compatible alias.
option(COOP_PREEMPT "Cooperative preemption via CNTPCT-driven scheduler_tick (ARM64 NC-memory platforms)" ON)
option(PI5_COOP_PREEMPT "Deprecated alias for COOP_PREEMPT" ON)
# Alias propagation: either knob being OFF disables the feature. If a
# user sets only one explicitly, respect that.
if(NOT PI5_COOP_PREEMPT)
    set(COOP_PREEMPT OFF)
endif()
if(COOP_PREEMPT AND (PLATFORM STREQUAL "RASPI5" OR PLATFORM STREQUAL "JETSON_ORIN_NANO"))
    add_compile_definitions(COOP_PREEMPT=1)
endif()

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
    # Note: -march=armv8.2-a causes compiler to emit SWPAB/CASALB etc.
    # which may trap on BCM2712 if TF-A doesn't enable LSE pass-through.
    # Use inline assembly for explicit LSE atomics where needed instead.
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
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/arch/arm64/user_entry.S>
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
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/tss.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/sse_kernels.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/nvidia_gsp_platform.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/arch/x86_64/nvidia_gsp_firmware.S>
    # GSP-RM shared core (x86-64 + Jetson — same Ampere architecture)
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/gpu/nvidia/gsp.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/gpu/nvidia/gsp.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/gpu/nvidia/nvidia_vbios.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/gpu/nvidia/nvidia_vbios.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/gpu/nvidia/falcon.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/gpu/nvidia/falcon.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/gpu/nvidia/nvfw.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/gpu/nvidia/nvfw.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/gpu/nvidia/bringup.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/gpu/nvidia/bringup.c>
    $<$<STREQUAL:${PLATFORM},X86_64>:kernel/gpu/nvidia/rpc.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/gpu/nvidia/rpc.c>

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
    kernel/sched/sched_heuristic.c
    kernel/sched/steal_deque.c
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/sched/smp.c>
    # preempt.c body is guarded by #if defined(SECONDARY_PREEMPT); compile
    # for any ARM64 platform so the Jetson SECONDARY_PREEMPT=ON build
    # (post Jetson plan P3) and the Pi 5 SECONDARY_PREEMPT=ON build both
    # link. On X86_64 the trampoline is not needed.
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/sched/preempt.c>

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
    # msg_router: Rust implementation in runtime/src/msg_router.rs
    # (linked via --whole-archive from Rust staticlib)
    kernel/src/help.c
    kernel/src/demo_init.c
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
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/src/syscall.c>

    # Semihosting (for QEMU test exit)
    kernel/src/semihosting.c

    # ==========================================================================
    # GPU Integration
    # ==========================================================================
    kernel/gpu/gpu.c
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/gpu/cache.c>
    kernel/gpu/gpu_stub.c
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
    kernel/tests/test_steal_deque.c
    kernel/tests/test_gpu.c
    kernel/tests/test_vfs.c
    kernel/tests/test_shell.c
    kernel/tests/test_pmm.c
    kernel/tests/test_littlefs.c
    kernel/tests/test_x86_boot.c
    kernel/tests/test_elf.c
    # DTB is ARM64 only (x86-64 doesn't build kernel/src/dtb.c)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_dtb.c>
    # ARM64-specific test files (use inline asm not available on x86-64)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_scheduler.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_model_mem.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_eviction.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_model_loader.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_syscall.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_component.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_vmm.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_net.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_lua.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_integration.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_msg_router.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_coop_preempt.c>
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
    ${CMAKE_SOURCE_DIR}/kernel/sched/ai
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
    # -fno-pie: bare-metal kernel at fixed address doesn't need PIE.
    # PIE causes GOT-relative symbol lookups that fail after kexec on
    # Jetson because the GOT page may not be cache-coherent.
    target_compile_options(slmos.elf PRIVATE -mgeneral-regs-only -fno-pie)
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

# Per-file override: the x86-64 SSE inference kernels need -msse -msse2
# to access SSE intrinsics. The rest of the kernel keeps -mno-sse
# for safety. CR4.OSFXSR / CR4.OSXMMEXCPT / CR0.MP / CR0.EM=0 are set
# in trampoline32.S / ap_trampoline.S (C2 / P3-1) so the SSE
# instructions execute at CPL=0 without #UD / #NM.
if(PLATFORM STREQUAL "X86_64")
    set_source_files_properties(
        kernel/arch/x86_64/sse_kernels.c
        PROPERTIES COMPILE_FLAGS "-msse -msse2"
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

# ==========================================================================
# AI Scheduler Library (optional)
# Uses floating-point/NEON — compiled WITHOUT -mgeneral-regs-only.
# Same pattern as Lua library.
# ==========================================================================
option(ENABLE_AI_SCHEDULER "Include AI scheduling models (MLP/PPO)" OFF)

# Work-stealing scheduler (#59 Phase B, S4 default flip).
#
# Default is ON for the platforms where hardware evidence in
# docs/work-stealing-bench.md supports it; OFF elsewhere. Per
# platform:
#
#   - x86-64: cache-coherent SMP, working reschedule IPI (B1) +
#     TSS IST (A1). Safe by construction. Default ON since Phase B.
#   - Raspberry Pi 5: 3.12× speedup on bench stealing 16 real
#     hardware after the external-lock fix (#158 closed). Default
#     ON.
#   - QEMU ARM64: 1.7× speedup on bench stealing — but several
#     pre-existing integration-test assumptions (strict priority
#     ordering, tight memory-leak thresholds) are incompatible
#     with the more aggressive cross-CPU scheduling work-stealing
#     produces. Default OFF for now so `make test` stays
#     reliably green; flip explicitly with
#     `make kernel WORK_STEALING=ON` / `make test WORK_STEALING=ON`
#     for regression testing. Full test-harness stabilization
#     is out of scope for S4.
#   - Jetson Orin Nano: residual page fault during bench stealing
#     tracked as #166. Default OFF; opt-in via WORK_STEALING=ON.
#
# Override path (either direction) is the Makefile `WORK_STEALING=ON`
# / `WORK_STEALING=OFF` variable, which maps to
# `-DENABLE_WORK_STEALING=ON/OFF`.
if(PLATFORM STREQUAL "X86_64" OR PLATFORM STREQUAL "RASPI5")
    option(ENABLE_WORK_STEALING "Enable per-CPU work-stealing (#59 Phase B)" ON)
else()
    option(ENABLE_WORK_STEALING "Enable per-CPU work-stealing (#59 Phase B)" OFF)
endif()
if(ENABLE_WORK_STEALING)
    add_compile_definitions(CONFIG_WORK_STEALING=1)
endif()

# ============================================================================
# GSP-RM firmware embedding (Phase E / #28 / x86-64 + Jetson)
# ============================================================================
# NVIDIA GSP-RM firmware is proprietary and lives in /lib/firmware/nvidia/
# on any machine with the NVIDIA driver (or JetPack BSP). Build-time
# extraction via scripts/tools/extract-gsp-firmware.sh produces raw
# binaries under ${CMAKE_BINARY_DIR}/gsp_fw/, which kernel/arch/x86_64/
# nvidia_gsp_firmware.S embeds via `.incbin`. Zero proprietary bytes
# live in the repo.
#
# Default: ON for X86_64 and JETSON_ORIN_NANO; OFF otherwise. Also
# auto-off when the firmware source directory is missing, so CI and
# dev machines without NVIDIA drivers still build.
if(PLATFORM STREQUAL "X86_64")
    set(GSP_CHIP "ga107")
    set(_gsp_default ON)
elseif(PLATFORM STREQUAL "JETSON_ORIN_NANO")
    set(GSP_CHIP "ga10b")
    set(_gsp_default ON)
else()
    set(GSP_CHIP "")
    set(_gsp_default OFF)
endif()
option(ENABLE_GSP_FIRMWARE "Embed NVIDIA GSP firmware blobs (Phase E)" ${_gsp_default})

if(ENABLE_GSP_FIRMWARE AND GSP_CHIP)
    if(NOT EXISTS "/lib/firmware/nvidia/${GSP_CHIP}/gsp")
        message(WARNING
            "ENABLE_GSP_FIRMWARE=ON but /lib/firmware/nvidia/${GSP_CHIP}/gsp not found. "
            "Install the NVIDIA driver (apt install linux-firmware) or pass "
            "-DENABLE_GSP_FIRMWARE=OFF. Disabling automatically."
        )
        set(ENABLE_GSP_FIRMWARE OFF)
    endif()
endif()

if(ENABLE_GSP_FIRMWARE AND GSP_CHIP AND PLATFORM STREQUAL "X86_64")
    set(GSP_FW_DIR "${CMAKE_BINARY_DIR}/gsp_fw")
    set(GSP_FW_STAMP "${GSP_FW_DIR}/.stamp")

    add_custom_command(
        OUTPUT ${GSP_FW_STAMP}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GSP_FW_DIR}
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/tools/extract-gsp-firmware.sh
                ${GSP_FW_DIR} ${GSP_CHIP}
        COMMAND ${CMAKE_COMMAND} -E touch ${GSP_FW_STAMP}
        DEPENDS ${CMAKE_SOURCE_DIR}/scripts/tools/extract-gsp-firmware.sh
        COMMENT "Extracting NVIDIA GSP firmware (${GSP_CHIP}) from /lib/firmware"
        VERBATIM
    )
    add_custom_target(gsp_firmware DEPENDS ${GSP_FW_STAMP})

    # Feed per-blob absolute paths as preprocessor defines into
    # the .S file. GAS's `.incbin` takes a string literal — we
    # can't concatenate "<dir>" "/gsp.bin" the way C does, so each
    # blob gets its own fully-qualified define.
    set_source_files_properties(
        kernel/arch/x86_64/nvidia_gsp_firmware.S
        PROPERTIES
        COMPILE_DEFINITIONS "ENABLE_GSP_FIRMWARE=1;GSP_FW_GSP_PATH=${GSP_FW_DIR}/gsp.bin;GSP_FW_BOOTLOADER_PATH=${GSP_FW_DIR}/bootloader.bin;GSP_FW_BOOTER_LOAD_PATH=${GSP_FW_DIR}/booter_load.bin;GSP_FW_BOOTER_UNLOAD_PATH=${GSP_FW_DIR}/booter_unload.bin"
        OBJECT_DEPENDS "${GSP_FW_STAMP}"
    )
    add_compile_definitions(ENABLE_GSP_FIRMWARE=1)

    message(STATUS "  GSP firmware: ${GSP_FW_DIR} (chip ${GSP_CHIP})")
endif()

if(ENABLE_AI_SCHEDULER)
    add_compile_definitions(CONFIG_AI_SCHEDULER=1)

    add_library(ai_sched STATIC
        kernel/sched/ai/ai_inference.c
        kernel/sched/ai/ai_state.c
        kernel/sched/ai/sched_ai.c
        kernel/sched/ai/ai_weights_mlp.c
        kernel/sched/ai/ai_weights_ppo.c
        kernel/sched/ai/ai_test_helpers.c
        $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/sched/ai/fp_context.S>
        $<$<STREQUAL:${PLATFORM},X86_64>:kernel/sched/ai/fp_context.S>
    )

    target_include_directories(ai_sched PRIVATE
        ${KERNEL_INCLUDES}
        ${CMAKE_SOURCE_DIR}/kernel/sched/ai
    )

    if(PLATFORM STREQUAL "X86_64")
        target_compile_options(ai_sched PRIVATE
            -ffreestanding
            -nostdlib
            -mno-red-zone -fno-stack-protector
            -msse -msse2
            -O2
        )
    else()
        # ARM64: omit -mgeneral-regs-only so FP/NEON is available
        target_compile_options(ai_sched PRIVATE
            -ffreestanding
            -nostdlib
            -mcpu=${TARGET_CPU}
            -fno-pie
            -O2
        )
    endif()

    target_compile_definitions(ai_sched PRIVATE
        $<$<STREQUAL:${PLATFORM},X86_64>:PLATFORM_X86_64=1>
        $<$<STREQUAL:${PLATFORM},QEMU_VIRT>:PLATFORM_QEMU_VIRT=1>
        $<$<STREQUAL:${PLATFORM},RASPI5>:PLATFORM_RASPI5=1>
        $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:PLATFORM_JETSON_ORIN_NANO=1>
    )

    target_link_libraries(slmos.elf PRIVATE ai_sched)
    message(STATUS "  AI Scheduler: ENABLED (stub weights)")
else()
    message(STATUS "  AI Scheduler: disabled")
endif()

# ==========================================================================
# AI Eviction Policies (optional)
#
# Policies live entirely in the Rust runtime; CMake only threads the
# feature flags through to cargo via the top-level Makefile. The
# compile definitions are exported so C shell/test code can guard
# calls to the Rust FFI on `CONFIG_AI_EVICTION`.
# ==========================================================================
option(ENABLE_AI_EVICTION "Include pluggable AI eviction policies (Rust)" OFF)
option(ENABLE_AI_EVICTION_MODELS "Include trained XGBoost + MLP weights (~1.3 MB)" OFF)

if(ENABLE_AI_EVICTION_MODELS AND NOT ENABLE_AI_EVICTION)
    message(FATAL_ERROR
        "ENABLE_AI_EVICTION_MODELS requires ENABLE_AI_EVICTION. "
        "Add -DENABLE_AI_EVICTION=ON or use AI_EVICTION=ON AI_EVICTION_MODELS=ON.")
endif()

if(ENABLE_AI_EVICTION)
    add_compile_definitions(CONFIG_AI_EVICTION=1)
    if(ENABLE_AI_EVICTION_MODELS)
        add_compile_definitions(CONFIG_AI_EVICTION_MODELS=1)
        message(STATUS "  AI Eviction: ENABLED (with trained models)")
    else()
        message(STATUS "  AI Eviction: ENABLED (stub models)")
    endif()
else()
    message(STATUS "  AI Eviction: disabled")
endif()

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

# ============================================================================
# x86-64 UEFI disk image (for bare-metal boot via SD card / USB)
# ============================================================================
# Produces build/kernel/slmos-x86.img: a 128 MB GPT disk with a single
# 64 MB EFI System Partition containing GRUB (as BOOTX64.EFI) and the
# kernel ELF. Flash to test-pc with `labctl sdwire flash test-pc ...`.
# Replaces the standalone kernel/arch/x86_64/Makefile.test disk-int
# target (deleted in favour of this) — keeps the kernel source list, C
# standard, and Rust staticlib link in a single place.
if(PLATFORM STREQUAL "X86_64")
    find_program(GRUB_MKIMAGE grub-mkimage)
    find_program(MFORMAT mformat)
    find_program(MCOPY mcopy)
    find_program(MMD mmd)
    find_program(SGDISK sgdisk)

    if(GRUB_MKIMAGE AND MFORMAT AND MCOPY AND MMD AND SGDISK)
        set(X86_DISK_IMG      ${CMAKE_BINARY_DIR}/slmos-x86.img)
        set(X86_GRUB_EFI      ${CMAKE_BINARY_DIR}/bootx64.efi)
        set(X86_GRUB_CFG      ${CMAKE_BINARY_DIR}/grub-embed.cfg)

        # ESP partition layout: 1 MB alignment (sector 2048) + 64 MB ESP.
        # FAT32 requires > 65535 total sectors for a well-formed BPB
        # (otherwise mformat / mkfs.vfat stuff the count into the legacy
        # 16-bit TotSec16 field and leave TotSec32 = 0, which UEFI
        # FAT drivers may reject). 64 MB = 131072 sectors, safely above
        # the FAT32 minimum.
        set(X86_ESP_FIRST_SECTOR 2048)
        set(X86_ESP_LAST_SECTOR  133119) # 2048 + 64*2048 - 1
        set(X86_ESP_SECTOR_COUNT 131072) # 64 MB

        # grub.cfg baked into the EFI binary — loads the kernel as
        # multiboot2 from the /slmos/ directory on the ESP.
        file(WRITE ${X86_GRUB_CFG}
"set timeout=0
set default=0
insmod part_gpt
insmod fat
insmod multiboot2
insmod all_video
set gfxpayload=text
search --file --set=root /slmos/kernel.elf
multiboot2 /slmos/kernel.elf
boot
")

        add_custom_command(
            OUTPUT ${X86_GRUB_EFI}
            COMMAND ${GRUB_MKIMAGE} -O x86_64-efi -o ${X86_GRUB_EFI}
                    -p /boot/grub -c ${X86_GRUB_CFG}
                    part_gpt fat multiboot2 search serial all_video
            DEPENDS ${X86_GRUB_CFG}
            COMMENT "Building GRUB x86_64-efi binary"
            VERBATIM
        )

        # Build a 128 MB GPT disk with one 64 MB ESP formatted in-place
        # via mtools — no sudo / no losetup. `mformat -H` sets the BPB
        # hidden_sectors field so UEFI firmware (which probes the FAT
        # header at the partition offset) finds a correctly-described
        # filesystem; without it OVMF drops into the PXE fallback.
        add_custom_command(
            OUTPUT ${X86_DISK_IMG}
            # 1. Zeroed 128 MB disk, GPT with one 64 MB EFI System
            #    Partition starting at 1 MB (sector 2048).
            COMMAND ${CMAKE_COMMAND} -E rm -f ${X86_DISK_IMG}
            COMMAND dd if=/dev/zero of=${X86_DISK_IMG} bs=1M count=128 status=none
            COMMAND ${SGDISK} --clear
                    --new=1:${X86_ESP_FIRST_SECTOR}:${X86_ESP_LAST_SECTOR}
                    --typecode=1:EF00 --change-name=1:EFI
                    ${X86_DISK_IMG}
            # 2. Format the partition in place at byte offset 1 MB.
            #    -H records the partition start (hidden_sectors); -T is
            #    the FAT total sector count. With 131072 sectors (> 65535)
            #    mformat writes TotSec32 correctly for FAT32.
            COMMAND ${MFORMAT} -i ${X86_DISK_IMG}@@1M
                    -F -v SLMOS -H ${X86_ESP_FIRST_SECTOR} -T ${X86_ESP_SECTOR_COUNT} ::
            COMMAND ${MMD} -i ${X86_DISK_IMG}@@1M ::/EFI
            COMMAND ${MMD} -i ${X86_DISK_IMG}@@1M ::/EFI/BOOT
            COMMAND ${MMD} -i ${X86_DISK_IMG}@@1M ::/slmos
            COMMAND ${MCOPY} -i ${X86_DISK_IMG}@@1M ${X86_GRUB_EFI} ::/EFI/BOOT/BOOTX64.EFI
            COMMAND ${MCOPY} -i ${X86_DISK_IMG}@@1M $<TARGET_FILE:slmos.elf> ::/slmos/kernel.elf
            DEPENDS ${X86_GRUB_EFI} slmos.elf
            COMMENT "Building UEFI disk image ${X86_DISK_IMG}"
            VERBATIM
        )

        add_custom_target(slmos-x86-disk DEPENDS ${X86_DISK_IMG})
        message(STATUS "  x86 disk:   make slmos-x86-disk → ${X86_DISK_IMG}")
    else()
        message(STATUS "  x86 disk:   disabled (missing grub-mkimage / mtools / sgdisk)")
    endif()
endif()

# Print configuration summary
message(STATUS "")
message(STATUS "SLM-OS Build Configuration")
message(STATUS "  Version:    ${PROJECT_VERSION}")
message(STATUS "  Platform:   ${PLATFORM}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "")
