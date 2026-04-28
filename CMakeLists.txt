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
# docs/archive/investigations/pi5-secondary-cpu-preemption.md.
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

# Verbose per-load / per-submit Hailo context-switch wire dumps.
# Default ON for the active Phase 8 investigation (#253) because the
# full-context hex dump + per-descriptor VDMA state are what drive
# byte-level diffs against HailoRT's reference capture. Default OFF
# (audit F-09, 2026-04-24) — verbose polling and descriptor readback
# can perturb the timing/cache behavior #253 is trying to measure.
# Single-line error paths (timeouts, submit_and_wait rc, load-stage
# markers) stay unconditional regardless of this flag. Enable
# explicitly with `make kernel HAILO_WIRE_DEBUG=ON` for byte diffs.
option(HAILO_WIRE_DEBUG "Emit verbose Hailo context-switch + VDMA diagnostic dumps" OFF)
if(HAILO_WIRE_DEBUG)
    add_compile_definitions(HAILO_WIRE_DEBUG=1)
endif()

# Replicate Linux hailo_pcie's post-boot D0->D3hot->D0 power-state
# round-trip. Linux does this between firmware load and user open;
# SLM-OS's tests (2026-04-25) show it shifts the bit-12 CPU_ECC
# trigger out of the load + pre-submit paths (cleaner state) but
# does not by itself unblock the boundary submit. Default ON because
# it brings SLM-OS closer to Linux's expected init flow at trivial
# cost; turn OFF for A/B comparison or on platforms where the PCI
# PM cap is unreliable. See kernel/ai_accel/hailo/hailo_core.c at
# the post-boot site for the call-out, hailo_pi5.c:pi5_set_power_state
# for the actual transition.
option(HAILO_D3HOT_AT_BOOT "Replicate Linux's D0->D3hot->D0 cycle after fw boot" ON)
if(HAILO_D3HOT_AT_BOOT)
    add_compile_definitions(HAILO_D3HOT_AT_BOOT=1)
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

# Embed Lua demo scripts (scripts/demo.lua, scripts/demo_menu.lua) into the
# kernel image via .incbin (#13, #14). When ON (the default), the scripts
# are written to /mnt/files at boot so users can `lua /mnt/files/demo.lua`.
# When OFF, demo_init() becomes a no-op and no demo content is embedded,
# saving kernel binary size. The Lua interpreter and slm.* bindings remain
# fully functional in either case.
option(EMBED_DEMO_SCRIPTS "Embed scripts/*.lua demo scripts in the kernel image" ON)
if(EMBED_DEMO_SCRIPTS)
    add_compile_definitions(EMBED_DEMO_SCRIPTS=1)
endif()

# Networking: ON for platforms with drivers (QEMU_VIRT virtio-mmio,
# X86_64 virtio-pci, RASPI5 Cadence MACB via RP1, JETSON_ORIN_NANO
# RTL8168 via PCIe C8). Default OFF for platforms still awaiting a
# native NIC driver.
#
# JETSON_ORIN_NANO caveat (#25): the RTL8168 driver is Stage-1
# scaffolding — probe() is a no-op and no driver registers with lwIP.
# Networking=ON on Jetson pulls the lwIP stack into the image
# (~50 KB) without providing working networking. Kept ON so the
# Stage-2+ work lands as additive changes; pass ENABLE_NETWORKING=OFF
# explicitly to opt out of the stack weight. Blocker is documented
# in docs/jetson-pcie-investigation.md.
if(PLATFORM STREQUAL "QEMU_VIRT" OR PLATFORM STREQUAL "X86_64"
   OR PLATFORM STREQUAL "RASPI5" OR PLATFORM STREQUAL "JETSON_ORIN_NANO")
    option(ENABLE_NETWORKING "Enable TCP/IP networking stack (lwIP)" ON)
else()
    option(ENABLE_NETWORKING "Enable TCP/IP networking stack (lwIP)" OFF)
endif()
if(ENABLE_NETWORKING)
    add_compile_definitions(ENABLE_NETWORKING=1)
endif()

# Jetson EL1 smoke test — one-shot diagnostic that drops to EL1 at boot
# and reads PCIe APPL + XHCI MMIO. Used to answer whether CBB firewall
# gates by EL (Path 1 viable) or kernel signature (Path 1 dead). See
# docs/jetson-pcie-investigation.md.
option(JETSON_EL1_SMOKE "Run Jetson EL1-drop smoke test at boot (one-shot)" OFF)
if(JETSON_EL1_SMOKE)
    add_compile_definitions(JETSON_EL1_SMOKE=1)
endif()

# Jetson XHCI one-shot diagnostic: after a successful NO_OP round-trip,
# immediately PSCI-reset back to Linux. Useful when serial capture is
# unavailable but we still need to prove the post-kexec XHCI path reached
# RUN=1 + command DMA on real hardware.
option(JETSON_XHCI_REBOOT_ON_NOOP
       "Reboot Jetson immediately after xHCI NO_OP succeeds (one-shot diagnostic)"
       OFF)
if(JETSON_XHCI_REBOOT_ON_NOOP)
    add_compile_definitions(JETSON_XHCI_REBOOT_ON_NOOP=1)
endif()

# Auto-start DHCP at boot (issue #197).
#
# When ON, net_init() calls dhcp_start() before returning; the shell
# comes up with a DHCP-assigned IP without user intervention. If DHCP
# doesn't answer within the compile-time timeout, lwip_slm.c falls
# back to the static IP (10.0.2.15 in the QEMU default config). Set
# to OFF to restore the previous manual-ifconfig-dhcp behavior.
option(NET_DHCP_AT_BOOT "Auto-start DHCP during net_init (lwIP)" ON)
if(ENABLE_NETWORKING AND NET_DHCP_AT_BOOT)
    add_compile_definitions(NET_DHCP_AT_BOOT=1)
endif()

# Telnetd auto-start at boot (plan Phase 3 §3.2).
#
# When ON, shell_init calls telnetd_autostart at the end of boot.
# That function reads /etc/telnetd.conf (if present) or uses the
# compile-time default (enabled=true when NET_TELNETD_AUTOSTART=ON);
# if the resolved config says enabled it calls net_init + starts
# the TCP listener on port 2323.
#
# Default is ON for Pi 5 + Jetson lab/demo images and OFF elsewhere.
# This is a cache default only: an explicit -DNET_TELNETD_AUTOSTART=OFF
# must win, especially on real hardware where the listener is
# unauthenticated. Jetson is in the demo set because the canonical
# kexec workflow leaves the device on a non-interactive serial
# console, and telnet is the primary way the demo runbook
# (`docs/demo-admin.md`) reaches the shell.
if(PLATFORM STREQUAL "RASPI5" OR PLATFORM STREQUAL "JETSON_ORIN_NANO")
    if(NOT DEFINED NET_TELNETD_AUTOSTART)
        set(NET_TELNETD_AUTOSTART ON CACHE BOOL
            "Start telnetd at boot (unauthenticated TCP shell)")
    endif()
else()
    option(NET_TELNETD_AUTOSTART "Start telnetd at boot (unauthenticated TCP shell)" OFF)
endif()
if(ENABLE_NETWORKING AND NET_TELNETD_AUTOSTART)
    add_compile_definitions(NET_TELNETD_AUTOSTART=1)
endif()

# Telemetry feed TCP server (port 2325). Bridges the in-process `tel.*`
# msg_router topics out to the network so a host-side monitor can
# subscribe over plain TCP. Default OFF — explicit opt-in for lab /
# demo builds. Same trust-the-LAN posture as telnetd.
option(NET_TELEMETRYD_AUTOSTART "Start telemetryd at boot (unauthenticated TCP telemetry feed)" OFF)
if(ENABLE_NETWORKING AND NET_TELEMETRYD_AUTOSTART)
    add_compile_definitions(NET_TELEMETRYD_AUTOSTART=1)
endif()

# Linker script - platform-specific
if(PLATFORM STREQUAL "X86_64")
    # Three x86-64 layouts — KEXEC_BUILD and BZIMAGE_BUILD are mutually
    # exclusive (the Makefile's kernel-kexec and kernel-bzimage targets
    # use separate build directories to keep them that way). Setting
    # both is a configuration error:
    #
    # * BZIMAGE_BUILD=1 — link at 0x20000000 AND place a Linux-bzImage
    #   entry stub at file offset 0x200 (where kexec's bzImage64
    #   loader jumps). Produces an ELF suitable for wrapping into a
    #   bzImage via scripts/make-bzimage.py.
    # * KEXEC_BUILD=1 — link at 0x20000000 for Multiboot2 kexec
    #   handoff (old kexec_load syscall). No bzImage stub.
    # * neither — link at 0x100000 for bare-metal UEFI/GRUB
    #   Multiboot2 boot (the default build).
    if(KEXEC_BUILD AND BZIMAGE_BUILD)
        message(FATAL_ERROR
            "KEXEC_BUILD and BZIMAGE_BUILD are mutually exclusive. "
            "BZIMAGE_BUILD already implies the kexec load address; "
            "pass only one of the two.")
    endif()
    if(BZIMAGE_BUILD)
        set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/kernel-x86_64-bzimage.ld)
        message(STATUS "X86_64 BZIMAGE_BUILD=1: linking at 0x20000000 "
                       "with bzImage entry stub at offset 0x200")
    elseif(KEXEC_BUILD)
        set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/kernel-x86_64-kexec.ld)
        message(STATUS "X86_64 KEXEC_BUILD=1: linking at 0x20000000 for kexec")
    else()
        set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/kernel/kernel-x86_64.ld)
    endif()
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
    # Jetson-only: nvgpu-native bringup skeleton (parallel to bringup.c's
    # GSP-RM path, for GA10B's ACR+FECS+GPCCS+PMU boot model).
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/gpu/nvidia/ga10b_bringup.c>
    # Jetson: full GSP platform shim — MMIO at GPU_BASE, unified memory
    # DMA, cache maintenance, firmware via .incbin. Replaces the old
    # nvidia_gsp_platform_stub.c now that CBB firewall is bypassed.
    # NOTE: nvidia_gsp_firmware.S is added via target_sources() below,
    # gated by ENABLE_GSP_FIRMWARE, since it requires firmware blobs.
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/arch/arm64/nvidia_gsp_platform.c>

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
    kernel/sched/task_sleep.c
    kernel/sched/sched.c
    kernel/sched/sched_heuristic.c
    kernel/sched/steal_deque.c
    kernel/src/sched_trace.c
    kernel/src/latency_hist.c
    kernel/src/rate_ewma.c
    kernel/src/gpu_consumer.c
    kernel/src/admin_telemetry.c
    kernel/src/model_engine.c
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

    # BPMP IPC stack — Jetson only.
    # kernel/drivers/bpmp/bpmp.c provides stubs on non-Jetson platforms,
    # so it is always compiled. The HSP/IVC/MRQ internals are Jetson-
    # specific and gated on PLATFORM_JETSON_ORIN_NANO inside each file.
    kernel/drivers/bpmp/bpmp.c
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/drivers/bpmp/hsp.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/drivers/bpmp/ivc.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/drivers/bpmp/mrq.c>

    # Tegra234 HSI2C controller driver (#396 Hardware Task 1) — single
    # file, gated on PLATFORM_JETSON_ORIN_NANO inside via #ifdef so a
    # cross-platform build links a stub. Used for IMX219 sensor
    # control over the cam_i2c bus.
    kernel/drivers/i2c/i2c_tegra.c

    # Tegra234 per-pin GPIO driver (#396 Hardware Task 2) — same
    # cross-platform-stub pattern. Used by the IMX219 sensor driver
    # to release the sensor reset, enable the regulator GPIO, and
    # position the cam_i2cmux selector.
    kernel/drivers/gpio/gpio_tegra.c

    # IMX219 camera sensor driver (#396 Hardware Task 2) — wraps
    # HSI2C + GPIO + extperiph1 XCLK to power up the IMX219 and
    # verify CHIP_ID = 0x0219. Cross-platform stub branch returns
    # -1 from every function.
    kernel/drivers/camera/imx219.c

    # NVCSI receiver driver (#396 Hardware Task 3) — direct-MMIO
    # bring-up of the Tegra234 MIPI CSI-2 receiver for the IMX219's
    # 2-lane DPHY RAW10 stream. Mirrors the historical T194 csi4_fops
    # programming sequence; T234 keeps the same hardware register
    # layout despite L4T R35 routing everything through RTCPU.
    kernel/drivers/camera/nvcsi.c

    # Camera RTCPU (RCE) HSP-VM transport (#396 Hardware Task 3
    # Option B) — the IPC layer SLM-OS needs to ask the camera
    # firmware to do anything (NVCSI bring-up, VI capture, etc.).
    # camrtc.c does HELLO/PROTOCOL/RESUME + CH_SETUP; camrtc_ivc.c
    # rides on top, exposing tegra-IVC ring read/write helpers
    # for the capture-control / capture / etc. channels CH_SETUP
    # binds.
    kernel/drivers/camrtc/camrtc.c
    kernel/drivers/camrtc/camrtc_ivc.c
    kernel/drivers/camrtc/camrtc_capture.c

    # Interrupt controller and timer (ARM64 only — x86-64 uses pic.c + timer_x86.c)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/drivers/gic.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/drivers/timer.c>

    # Block device abstraction and RAM disk
    kernel/drivers/blkdev.c
    kernel/drivers/ramdisk.c

    # PCIe host-controller subsystem (ARM64 only — x86-64 has its own pci.c)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/drivers/pcie/pcie_core.c>
    $<$<STREQUAL:${PLATFORM},QEMU_VIRT>:kernel/drivers/pcie/pcie_qemu_gpex.c>
    $<$<STREQUAL:${PLATFORM},RASPI5>:kernel/drivers/pcie/pcie_bcm2712.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/drivers/pcie/pcie_stub.c>
    $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/drivers/pcie/pcie_tegra194.c>

    # Inference-device abstraction (dispatcher always compiled;
    # CPU-MLP backend added under the ai_sched library when AI_SCHED=ON)
    kernel/inference/inference_device.c
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/inference/inference_device_hailo.c>

    # Hailo NPU driver (platform-independent core + shell cmd always
    # compiled on ARM64; Pi 5 platform shim only on RASPI5; stub
    # elsewhere so `hailo_platform_install` always links)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/ai_accel/hailo/hailo_core.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/ai_accel/hailo/hailo_control.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/ai_accel/hailo/hailo_cs_builder.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/ai_accel/hailo/hailo_cs_translator.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/ai_accel/hailo/hailo_tensor.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/ai_accel/hailo/hailo_vdma.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/ai_accel/hailo/hailo_infer.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/ai_accel/hailo/hailo_shell.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/ai_accel/hailo/hef_header.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/ai_accel/hailo/hef_parser.c>
    $<$<STREQUAL:${PLATFORM},RASPI5>:kernel/ai_accel/hailo/hailo_pi5.c>
    $<$<AND:$<NOT:$<STREQUAL:${PLATFORM},X86_64>>,$<NOT:$<STREQUAL:${PLATFORM},RASPI5>>>:kernel/ai_accel/hailo/hailo_stub.c>

    # ==========================================================================
    # Core Kernel
    # ==========================================================================
    kernel/src/main.c
    kernel/src/panic.c
    kernel/src/kprintf.c
    kernel/src/string.c

    # Shell (core + command modules)
    kernel/src/shell.c
    kernel/src/shell_history.c
    kernel/src/shell_io.c
    kernel/src/shell_io_uart.c
    kernel/src/shell_session.c
    kernel/src/shell_sys.c
    kernel/src/shell_fs.c
    kernel/src/shell_exec.c
    kernel/src/shell_component.c
    kernel/src/shell_top.c
    kernel/src/component_runtime.c
    # msg_router: Rust implementation in runtime/src/msg_router.rs
    # (linked via --whole-archive from Rust staticlib)
    kernel/src/help.c
    kernel/src/demo_init.c
    kernel/src/blob_autoload.c
    kernel/src/runtime_blob_file.c
    # .incbin'ed Lua demo scripts (gated on EMBED_DEMO_SCRIPTS)
    $<$<BOOL:${EMBED_DEMO_SCRIPTS}>:kernel/src/demo_scripts.S>

    # Scheduler MLP .hef embed (gated on SCHEDULER_HEF_BLOB). Stub
    # sched_hef_init.c always links so main.c doesn't need #ifdef on
    # the call site; the .S file compiles only when the blob is set.
    kernel/src/sched_hef_init.c
    $<$<BOOL:${SCHEDULER_HEF_BLOB}>:kernel/src/sched_hef_embed.S>
    # Phase 8: user .hef embed (gated on USER_HEF_BLOB). Same stub
    # pattern as the scheduler HEF above. Writes to /mnt/files/user.hef.
    kernel/src/user_hef_init.c
    $<$<BOOL:${USER_HEF_BLOB}>:kernel/src/user_hef_embed.S>
    # MNIST test-digit fixtures (gated on MNIST_DIGITS_DIR). Same stub
    # pattern. Writes 10 fp32 digit binaries to /mnt/files/digits/ so
    # slm.model_infer_file() has a known-good demo set out of the box.
    kernel/src/mnist_digit_init.c
    $<$<BOOL:${MNIST_DIGITS_DIR}>:kernel/src/mnist_digit_embed.S>
    # Camera capture API + mock backend. The mock RAW10 frame embed
    # itself (camera_mock_embed.S) is added later via target_sources
    # because the MOCK_CAMERA_FRAME option is defined further down.
    kernel/src/camera.c
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/src/dtb.c>
    # General-purpose FDT reader — path + property lookup for any
    # driver. Built on every ARM64 target; x86-64 builds skip it
    # because there's no DTB there.
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/lib/fdt/fdt.c>
    kernel/src/elf.c
    kernel/src/vfs.c

    # ==========================================================================
    # Utility libraries
    # ==========================================================================
    # MD5 (public domain, Solar Designer). Used by the Hailo firmware
    # control channel for request/response stamping. NOT a security
    # primitive.
    kernel/lib/md5.c
    kernel/lib/sha256.c

    # SHA-256 (public domain, Brad Conte / B-Con/crypto-algorithms).
    # Used by the dynamic-kernel-replace `kernel stage` command (#370)
    # to compute the `tryboot.sha` sidecar. Stored alongside the
    # staged kernel image; not validated end-to-end today, so this is
    # a content-checksum primitive, not a security boundary.
    kernel/lib/sha256.c

    # ==========================================================================
    # Filesystem
    # ==========================================================================
    # LittleFS library
    kernel/lib/littlefs/lfs.c
    kernel/lib/littlefs/lfs_util.c

    # LittleFS wrapper and VFS adapter
    kernel/fs/littlefs_slm.c
    kernel/fs/littlefs_vfs.c

    # FatFs library (Elm-Chan R0.16 vendored under kernel/lib/fatfs/).
    # Used by the dynamic-kernel-replace path (#368) to read/write
    # the FAT32 boot partition. Single-disk binding lives in
    # diskio.c, which forwards FatFs sectors to a `struct blkdev`.
    kernel/lib/fatfs/ff.c
    kernel/lib/fatfs/ffunicode.c
    kernel/lib/fatfs/diskio.c

    # SDHCI / SD Host Controller driver (#369, dynamic-kernel-replace
    # Stage 3). Targets QEMU `-device sdhci-pci` and BCM2712 EMMC2.
    # Presents a `struct blkdev` so FatFs's diskio shim plugs in
    # unchanged. PIO data path; ADMA2 + BCM2712 cfginit deferred to
    # Stage 5 (#371).
    kernel/drivers/sdhci.c
    kernel/drivers/persistent_lfs_store.c
    kernel/src/boot_media.c

    # `kernel` admin shell command (#370, dynamic-kernel-replace
    # Stage 4). Wires the Stage 1/2/3 building blocks into a
    # status / stage / activate / promote / rollback subcommand
    # surface. Compiles on every platform; non-Pi-5 platforms get
    # the no-op activate path.
    kernel/src/kernel_cmd.c

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
    # Networking (enabled on platforms with drivers)
    # ==========================================================================
    # VirtIO-Net MMIO driver (ARM64 QEMU only)
    $<$<AND:$<BOOL:${ENABLE_NETWORKING}>,$<STREQUAL:${PLATFORM},QEMU_VIRT>>:kernel/drivers/virtio_net.c>
    # VirtIO-Net PCI driver (x86-64 only)
    $<$<AND:$<BOOL:${ENABLE_NETWORKING}>,$<STREQUAL:${PLATFORM},X86_64>>:kernel/drivers/virtio_net_pci.c>
    # Cadence MACB/GEM driver (Pi 5 only, via RP1 southbridge) — #202
    $<$<AND:$<BOOL:${ENABLE_NETWORKING}>,$<STREQUAL:${PLATFORM},RASPI5>>:kernel/drivers/macb.c>
    # BCM2712 VideoCore mailbox — supplies the board MAC to macb.c (#250)
    $<$<AND:$<BOOL:${ENABLE_NETWORKING}>,$<STREQUAL:${PLATFORM},RASPI5>>:kernel/drivers/bcm_mailbox.c>
    # Realtek RTL8169/RTL8168 driver (Jetson Orin Nano, via Tegra PCIe C8) — #25
    $<$<AND:$<BOOL:${ENABLE_NETWORKING}>,$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>>:kernel/drivers/eth_rtl8169.c>
    # USB core (Phase 1 of #266). Platform-neutral; HCD drivers
    # (Phase 3A XHCI) register against it and are gated separately.
    kernel/usb/core/usb_core.c
    # CDC-ECM class driver (Phase 2 of #266). Gated on networking so
    # it stays linked out on platforms that don't build lwIP.
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/usb/class/cdc_ecm.c>
    # Tegra XHCI host-controller driver (Phase 3A of #266). File
    # itself is always compiled for layout-consistency; the body
    # is #if'd behind PLATFORM_JETSON_ORIN_NANO so non-Jetson
    # platforms link a harmless stub.
    kernel/drivers/usb/xhci/xhci.c
    kernel/drivers/usb/xhci/xhci_device.c
    kernel/drivers/usb/xhci/xhci_ring.c
    kernel/drivers/usb/xhci/xhci_xfer.c
    # Jetson EL1 smoke test (one-shot diagnostic) — JETSON_EL1_SMOKE
    $<$<AND:$<BOOL:${JETSON_EL1_SMOKE}>,$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>>:kernel/tests/jetson_el1_smoke.S>
    # Platform-independent networking stack
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/net/sys_arch.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/net/lwip_slm.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/src/net_http.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/src/net_shell.c>
    # TCP shell (multi-session) — requires lwIP; Plan §1.3 + §2
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/src/shell_io_tcp.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/src/tcp_shell_server.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/src/tcp_telemetry_server.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/src/telnet.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/src/telnetd_config.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/src/telnetd_autostart.c>

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
    kernel/tests/test_sched_trace.c
    kernel/tests/test_latency_hist.c
    kernel/tests/test_gpu_consumer.c
    kernel/tests/test_admin_telemetry.c
    kernel/tests/test_telemetry_feed.c
    kernel/tests/test_model_engine.c
    kernel/tests/test_gpu.c
    kernel/tests/test_vfs.c
    kernel/tests/test_shell.c
    kernel/tests/test_shell_session.c
    kernel/tests/test_shell_history.c
    kernel/tests/test_blob_autoload.c
    kernel/tests/test_boot_media.c
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/tests/test_telnet.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/tests/test_telnetd_config.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/tests/test_telnetd_cmd.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/tests/test_tcp_telemetry_server.c>
    kernel/tests/test_pmm.c
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_pcie.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_bpmp.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_inference_device.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_hailo.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_hef.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_hef_parser.c>
    kernel/tests/test_littlefs.c
    kernel/tests/test_x86_boot.c
    kernel/tests/test_elf.c
    # Platform-neutral protocol tests (the proto helpers are pure
    # buffer construction and compile on every platform, even though
    # the bcm_mailbox.c MMIO transport is RASPI5-only).
    kernel/tests/test_bcm_mailbox.c
    # FatFs / FAT32 integration tests — exercises the diskio shim
    # against the ramdisk stub block driver. Stage 2 of #368.
    # Intentionally NOT platform-gated: FatFs and the ramdisk are
    # platform-neutral (no MMIO, only PMM + memcpy), so the same
    # tests are valuable on every test target including x86-64.
    kernel/tests/test_fat32.c
    # SDHCI block driver tests — exercise the QEMU `-device sdhci-pci`
    # path. ARM64-only (uses `pcie.h`, not the legacy x86 `pci.h`).
    # Skips cleanly if QEMU is started without `-device sdhci-pci`.
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_sdhci.c>
    # `kernel` admin command surface tests — Stage 4 of #370. Same
    # ARM64-only gate as test_sdhci.c (it's the same backend stack).
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_kernel_cmd.c>
    # Camera C-API tests + Tegra234 clock-id static_asserts (#396).
    # Pure compile-time + small-buffer tests; runs on every platform.
    kernel/tests/test_camera.c
    # SHA-256 NIST-vector tests for the vendored library — Stage 4
    # of #370 too, but platform-neutral (pure computation, no MMIO).
    kernel/tests/test_sha256.c
    # DTB is ARM64 only (x86-64 doesn't build kernel/src/dtb.c)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_dtb.c>
    # FDT general-purpose reader (kernel/lib/fdt) — ARM64 only for
    # the same reason as test_dtb.c above
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_fdt.c>
    # ARM64-specific test files (use inline asm not available on x86-64)
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_scheduler.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_model_mem.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_eviction.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_model_loader.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_syscall.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_component.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_vmm.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/tests/test_net.c>
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/tests/test_net_http.c>
    # USB core tests (Phase 1 of #266). Platform-neutral — the mock
    # HCD has no real hardware dependency.
    kernel/tests/test_usb_core.c
    # CDC-ECM class driver tests (Phase 2 of #266). Gated on networking
    # because the class driver itself is.
    $<$<BOOL:${ENABLE_NETWORKING}>:kernel/tests/test_cdc_ecm.c>
    # XHCI ring primitive tests (Phase 3A of #266, mothballed). The
    # init-with-buffer ring variants don't pull ncmem, so tests drive
    # them with stack-allocated TRB arrays on every target.
    kernel/tests/test_xhci_ring.c
    # Tegra234 XHCI wrapper + CSB-paging offset tests (Phase 3A.2 of
    # #266). Pure-logic CSB paging math + Linux-reference register
    # offset pinning; runs on every target since xhci_tegra.h is
    # platform-neutral.
    kernel/tests/test_xhci_tegra.c
    # PORTSC decode + context layout (Phase 3A Step 5 of #266).
    kernel/tests/test_xhci_device.c
    # TRB builders for control / bulk (Phase 3A Step 6 + 7b of #266).
    kernel/tests/test_xhci_xfer.c
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_lua.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_integration.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_msg_router.c>
    $<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:kernel/tests/test_coop_preempt.c>
)

# lwIP library sources (QEMU networking only)
set(LWIP_SOURCES
    kernel/lib/lwip/src/core/altcp.c
    kernel/lib/lwip/src/core/altcp_alloc.c
    kernel/lib/lwip/src/core/altcp_tcp.c
    kernel/lib/lwip/src/core/init.c
    kernel/lib/lwip/src/core/def.c
    kernel/lib/lwip/src/core/dns.c
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
    kernel/lib/lwip/src/apps/http/http_client.c
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
    ${CMAKE_SOURCE_DIR}/kernel/inference
    ${CMAKE_SOURCE_DIR}/kernel/lib/littlefs
    ${CMAKE_SOURCE_DIR}/kernel/lib/lwip/src/include
    ${CMAKE_SOURCE_DIR}/kernel/lib/lua/src
    ${CMAKE_SOURCE_DIR}/kernel/drivers/usb/xhci
    ${CMAKE_SOURCE_DIR}/kernel/drivers/bpmp
    ${CMAKE_SOURCE_DIR}/kernel/drivers/pcie
)

# Per-platform arch include paths so test code under kernel/tests
# can pull arch-private headers (e.g. x86_lapic_consts.h) without
# resorting to relative includes.
if(PLATFORM STREQUAL "X86_64")
    list(APPEND KERNEL_INCLUDES ${CMAKE_SOURCE_DIR}/kernel/arch/x86_64)
endif()

# Build kernel ELF — include lwIP when networking is enabled
if(ENABLE_NETWORKING)
    add_executable(slmos.elf ${KERNEL_SOURCES} ${TEST_SOURCES} ${LWIP_SOURCES})
else()
    add_executable(slmos.elf ${KERNEL_SOURCES} ${TEST_SOURCES})
endif()
target_include_directories(slmos.elf PRIVATE ${KERNEL_INCLUDES})

# ---------------------------------------------------------------------------
# build_info.h — version + UTC stamp + git SHA, regenerated every build.
# Emitted under ${CMAKE_BINARY_DIR}/include/ so parallel multi-platform
# builds (e.g. simultaneous QEMU_VIRT and RASPI5 from separate build dirs)
# never race-write the same file. The custom target depends on no inputs,
# so CMake re-runs the generator on every invocation; the script itself
# only rewrites the file when the content changes (avoids no-op relinks
# inside the same UTC second). Both `slmos.elf` and the `lua` static lib
# pick it up via the build-dir include path added below.
#
# Cost: when the UTC second has advanced since the previous build, the
# stamp changes, the header is rewritten, and the three consumer TUs
# (main.c, vfs.c, lua_slm.c) recompile, forcing a relink. That is by
# design — the whole purpose of the stamp is to embed a fresh build
# identity into the kernel image — and is the reason for the
# copy-if-different write above (so back-to-back builds inside the same
# UTC second do not pay the relink cost).
# ---------------------------------------------------------------------------
set(BUILD_INFO_DIR    ${CMAKE_BINARY_DIR}/include)
set(BUILD_INFO_HEADER ${BUILD_INFO_DIR}/build_info.h)
file(MAKE_DIRECTORY ${BUILD_INFO_DIR})
add_custom_target(generate_build_info ALL
    COMMAND ${CMAKE_COMMAND}
        -DSRC_DIR=${CMAKE_SOURCE_DIR}
        -DOUT_FILE=${BUILD_INFO_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/gen_build_info.cmake
    BYPRODUCTS ${BUILD_INFO_HEADER}
    COMMENT "Generating build_info.h"
    VERBATIM
)
add_dependencies(slmos.elf generate_build_info)
target_include_directories(slmos.elf PRIVATE ${BUILD_INFO_DIR})

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

    # BZIMAGE_BUILD: include the 64-bit bzImage entry stub. The linker
    # script (kernel-x86_64-bzimage.ld) places its .bzimage_entry
    # section at exactly KERNEL_PHYS + 0x200 — where kexec's bzImage64
    # loader jumps after copying the payload. The section is empty in
    # non-bzImage builds so leaving it out of the source list would
    # produce an "empty section" at 0x200 followed by .text drifting
    # down; we include it only when BZIMAGE_BUILD so the other two
    # x86-64 builds stay byte-identical to before.
    if(BZIMAGE_BUILD)
        target_sources(slmos.elf PRIVATE
            kernel/arch/x86_64/bzimage_entry.S)
    endif()

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

# Feed absolute paths of the demo Lua scripts to demo_scripts.S so GAS's
# .incbin can resolve them regardless of the assembler's working directory.
# OBJECT_DEPENDS makes the .S file rebuild when the scripts change so the
# embedded blob stays in sync with the repo source of truth.
if(EMBED_DEMO_SCRIPTS)
    set_source_files_properties(
        kernel/src/demo_scripts.S
        PROPERTIES
        COMPILE_DEFINITIONS "DEMO_LUA_PATH=${CMAKE_SOURCE_DIR}/scripts/demo.lua;DEMO_MENU_LUA_PATH=${CMAKE_SOURCE_DIR}/scripts/demo_menu.lua;DEMO_AUTO_LUA_PATH=${CMAKE_SOURCE_DIR}/scripts/demo_auto.lua;DEMO_MULTIPROC_LUA_PATH=${CMAKE_SOURCE_DIR}/scripts/multiproc_demo.lua;DEMO_HAILO_LUA_PATH=${CMAKE_SOURCE_DIR}/scripts/demo_hailo.lua;DEMO_ADMIN_LUA_PATH=${CMAKE_SOURCE_DIR}/scripts/admin.lua"
        OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/scripts/demo.lua;${CMAKE_SOURCE_DIR}/scripts/demo_menu.lua;${CMAKE_SOURCE_DIR}/scripts/demo_auto.lua;${CMAKE_SOURCE_DIR}/scripts/multiproc_demo.lua;${CMAKE_SOURCE_DIR}/scripts/demo_hailo.lua"
    )
endif()

# ============================================================================
# Scheduler MLP .hef (optional .incbin — Phase 6.2c delivery path)
# ============================================================================
#
# Set -DSCHEDULER_HEF_BLOB=path/to/scheduler_mlp_pi5.hef to embed the
# compiled scheduler MLP into the kernel image. sched_hef_init.c writes
# the bytes into /mnt/files/scheduler_mlp.hef at boot so
# `hailo load /mnt/files/scheduler_mlp.hef sched` can reach them. The
# .hef is produced by scripts/hailo/compile_hef.sh (see docs/setup.md
# §Hailo Toolchain).
set(SCHEDULER_HEF_BLOB "" CACHE FILEPATH
    "Path to scheduler_mlp_*.hef to embed via .incbin (Phase 6.2c)")
if(SCHEDULER_HEF_BLOB)
    if(NOT EXISTS "${SCHEDULER_HEF_BLOB}")
        message(FATAL_ERROR "SCHEDULER_HEF_BLOB set but file does not exist: "
                            "${SCHEDULER_HEF_BLOB}")
    endif()
    add_compile_definitions(ENABLE_SCHEDULER_HEF_EMBED=1)
    set_source_files_properties(
        kernel/src/sched_hef_embed.S
        PROPERTIES
        COMPILE_DEFINITIONS "SCHEDULER_HEF_PATH=${SCHEDULER_HEF_BLOB}"
        OBJECT_DEPENDS "${SCHEDULER_HEF_BLOB}"
    )
    message(STATUS "Embedding scheduler MLP .hef: ${SCHEDULER_HEF_BLOB}")
endif()

# ============================================================================
# MNIST test-digit fixtures (optional .incbin — demo / model_infer_file)
# ============================================================================
#
# Set -DMNIST_DIGITS_DIR=path/to/digits/ to embed 10 fp32 digit binaries
# (one per class 0..9, named digit_<class>_idx<N>.bin and produced by
# scripts/mnist-extract-test-digits.py). mnist_digit_init.c writes them
# to /mnt/files/digits/digit_<n>.bin at boot so slm.model_infer_file()
# has a known-good demo set immediately after kexec.
set(MNIST_DIGITS_DIR "" CACHE PATH
    "Path to a directory of digit_<class>_idx<N>.bin fp32 fixtures to embed")
if(MNIST_DIGITS_DIR)
    if(NOT IS_DIRECTORY "${MNIST_DIGITS_DIR}")
        message(FATAL_ERROR "MNIST_DIGITS_DIR set but is not a directory: "
                            "${MNIST_DIGITS_DIR}")
    endif()
    add_compile_definitions(ENABLE_MNIST_DIGITS_EMBED=1)
    set_source_files_properties(
        kernel/src/mnist_digit_embed.S
        PROPERTIES
        COMPILE_DEFINITIONS "MNIST_DIGITS_DIR=${MNIST_DIGITS_DIR}"
    )
    message(STATUS "Embedding MNIST test digits from: ${MNIST_DIGITS_DIR}")
endif()

# ============================================================================
# Mock camera frame (.incbin — QEMU-side CI for camera Lua bindings)
# ============================================================================
#
# Generated at build time by scripts/generate-mock-camera-frame.py from
# scripts/fixtures/mock_camera_digit.bin (a 28x28 fp32 MNIST digit).
# Output is 1640x1232 RAW10 RGGB packed, 2,525,600 bytes. The mock
# backend in kernel/src/camera.c returns pointers into this .rodata
# region so Lua scripts can exercise the capture + preprocess +
# inference path on QEMU without IMX219 hardware
# (docs/jetson-camera-imx219-plan.md "QEMU-Side Tasks").
#
# Default ON — production hardware builds that don't need the QEMU
# coverage can drop the 2.5 MB by passing -DMOCK_CAMERA_FRAME=OFF.
option(MOCK_CAMERA_FRAME
    "Embed the QEMU mock camera RAW10 frame (default ON, ~2.5 MB)" ON)
if(MOCK_CAMERA_FRAME)
    set(MOCK_CAMERA_FRAME_DIGIT
        "${CMAKE_SOURCE_DIR}/scripts/fixtures/mock_camera_digit.bin"
        CACHE FILEPATH
        "28x28 fp32 source digit for the mock camera frame")
    set(MOCK_CAMERA_FRAME_BIN "${CMAKE_BINARY_DIR}/mock_camera_frame.bin")
    if(NOT EXISTS "${MOCK_CAMERA_FRAME_DIGIT}")
        message(FATAL_ERROR
            "MOCK_CAMERA_FRAME_DIGIT does not exist: "
            "${MOCK_CAMERA_FRAME_DIGIT}")
    endif()
    # The OBJECT_DEPENDS on camera_mock_embed.S below is what wires the
    # generation into the build — CMake will fire this custom_command
    # whenever the .bin output is missing or older than the digit
    # fixture / the generator script.
    add_custom_command(
        OUTPUT  "${MOCK_CAMERA_FRAME_BIN}"
        COMMAND ${CMAKE_COMMAND} -E echo
                "[gen] mock_camera_frame.bin ← ${MOCK_CAMERA_FRAME_DIGIT}"
        COMMAND python3
                "${CMAKE_SOURCE_DIR}/scripts/generate-mock-camera-frame.py"
                --digit  "${MOCK_CAMERA_FRAME_DIGIT}"
                --output "${MOCK_CAMERA_FRAME_BIN}"
        DEPENDS "${MOCK_CAMERA_FRAME_DIGIT}"
                "${CMAKE_SOURCE_DIR}/scripts/generate-mock-camera-frame.py"
        VERBATIM
    )
    target_sources(slmos.elf PRIVATE kernel/src/camera_mock_embed.S)
    set_source_files_properties(
        kernel/src/camera_mock_embed.S
        PROPERTIES
        COMPILE_DEFINITIONS "MOCK_CAMERA_FRAME_PATH=${MOCK_CAMERA_FRAME_BIN}"
        OBJECT_DEPENDS      "${MOCK_CAMERA_FRAME_BIN}"
    )
    message(STATUS "Embedding mock camera frame from: ${MOCK_CAMERA_FRAME_DIGIT}")
endif()

# ============================================================================
# User .hef (optional .incbin — Phase 8 real-inference deploy path)
# ============================================================================
#
# Set -DUSER_HEF_BLOB=path/to/model.hef to embed a user-supplied Hailo HEF
# into the kernel image. user_hef_init.c writes the bytes to
# /mnt/files/user.hef at boot so Lua and the shell can reach them without
# a FAT driver. Used for Phase 8 hardware bring-up with a real compiled
# HEF (e.g. resnet_v1_18 for Hailo-8L).
set(USER_HEF_BLOB "" CACHE FILEPATH
    "Path to a user HEF to embed via .incbin (Phase 8)")
if(USER_HEF_BLOB)
    if(NOT EXISTS "${USER_HEF_BLOB}")
        message(FATAL_ERROR "USER_HEF_BLOB set but file does not exist: "
                            "${USER_HEF_BLOB}")
    endif()
    add_compile_definitions(ENABLE_USER_HEF_EMBED=1)
    set_source_files_properties(
        kernel/src/user_hef_embed.S
        PROPERTIES
        COMPILE_DEFINITIONS "USER_HEF_PATH=${USER_HEF_BLOB}"
        OBJECT_DEPENDS "${USER_HEF_BLOB}"
    )
    message(STATUS "Embedding user .hef: ${USER_HEF_BLOB}")
endif()

# ============================================================================
# Hailo-8 boot firmware (optional .incbin)
# ============================================================================
#
# Set -DHAILO_FW_BLOB=path/to/hailo8_fw.bin to embed the Hailo boot
# firmware into the kernel image. Without the option, hailo_shell.c's
# weak externs resolve to NULL and `hailo boot` reports "firmware not
# embedded". The blob is distributed with the Linux HailoRT driver
# (open redistribution) and lives at `hailo8_fw.bin` in that tree.
set(HAILO_FW_BLOB "" CACHE FILEPATH "Path to hailo8_fw.bin to embed via .incbin")
if(HAILO_FW_BLOB)
    if(NOT EXISTS "${HAILO_FW_BLOB}")
        message(FATAL_ERROR
            "HAILO_FW_BLOB=${HAILO_FW_BLOB} does not exist. "
            "Point it at a valid hailo8_fw.bin or leave it unset.")
    endif()
    target_sources(slmos.elf PRIVATE kernel/ai_accel/hailo/hailo_fw.S)
    set_source_files_properties(
        kernel/ai_accel/hailo/hailo_fw.S
        PROPERTIES
        COMPILE_DEFINITIONS "HAILO_FW_BLOB_PATH=${HAILO_FW_BLOB}"
        OBJECT_DEPENDS "${HAILO_FW_BLOB}"
    )
    message(STATUS "  Hailo firmware blob: ${HAILO_FW_BLOB}")
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

# Configure lwIP for freestanding use
if(ENABLE_NETWORKING)
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
target_include_directories(lua PRIVATE ${KERNEL_INCLUDES} kernel/lib/lua/src ${BUILD_INFO_DIR})
add_dependencies(lua generate_build_info)

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

# Pin the radix character to '.' (ASCII 46) so the float-to-string
# path in lobject.c doesn't deref localeconv()->decimal_point. The
# freestanding stub returns a static lconv with decimal_point=".",
# but a runtime memory-corruption bug has been observed (#538)
# overwriting default_lconv with a bogus pointer between boot and
# the first Lua "%f" formatting call. Bypassing the deref entirely
# makes Lua immune regardless of whether default_lconv is intact.
#
# Function-like macro defines aren't supported by
# target_compile_definitions (CMake silently strips the parens), so
# pass the -D directly via target_compile_options.
target_compile_options(lua PRIVATE
    "-Dlua_getlocaledecpoint()=46"
)

# Link with Lua library
target_link_libraries(slmos.elf PRIVATE lua)

# ==========================================================================
# nanopb — freestanding protobuf runtime (for .hef parsing)
# ==========================================================================
# Built as a separate static library so it can take its own compile
# flags: relaxed warnings (nanopb is upstream code, SLM-OS treats
# warnings as errors) and `-DPB_SYSTEM_HEADER="pb_syshdr.h"` so nanopb
# skips <string.h> / <stdlib.h> / <limits.h> and uses the freestanding
# glue in kernel/lib/nanopb/pb_syshdr.h.
#
# ARM64 only for now: x86-64 has its own GPU driver path and doesn't
# need .hef parsing. Enable when a consumer appears.
if(NOT PLATFORM STREQUAL "X86_64")
    # hef.pb.c is the nanopb-generated schema for hef.proto; it links
    # into the same static library as the runtime so it picks up the
    # relaxed-warnings compile options (generated code uses patterns
    # SLM-OS's -Werror would otherwise reject).
    add_library(nanopb STATIC
        kernel/lib/nanopb/pb_common.c
        kernel/lib/nanopb/pb_decode.c
        kernel/lib/nanopb/pb_encode.c
        kernel/ai_accel/hailo/hef.pb.c
    )
    target_include_directories(nanopb PUBLIC
        kernel/lib/nanopb
        kernel/ai_accel/hailo)
    target_compile_options(nanopb PRIVATE
        -ffreestanding
        -nostdlib
        -mcpu=${TARGET_CPU}
        -mgeneral-regs-only
        -fno-pie
        -Wno-unused-parameter -Wno-unused-variable
        -Wno-redundant-decls -Wno-error
    )
    target_compile_definitions(nanopb PRIVATE
        "PB_SYSTEM_HEADER=\"pb_syshdr.h\""
        $<$<STREQUAL:${PLATFORM},QEMU_VIRT>:PLATFORM_QEMU_VIRT=1>
        $<$<STREQUAL:${PLATFORM},RASPI5>:PLATFORM_RASPI5=1>
        $<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:PLATFORM_JETSON_ORIN_NANO=1>
    )
    target_link_libraries(slmos.elf PRIVATE nanopb)
    target_include_directories(slmos.elf PRIVATE
        kernel/lib/nanopb
        kernel/ai_accel/hailo)
    target_compile_definitions(slmos.elf PRIVATE
        "PB_SYSTEM_HEADER=\"pb_syshdr.h\""
    )
endif()

# ==========================================================================
# AI Scheduler Library (optional)
# Uses floating-point/NEON — compiled WITHOUT -mgeneral-regs-only.
# Same pattern as Lua library.
# ==========================================================================
# ==========================================================================
# kprintf_float — freestanding %f/%g/%e formatter for kprintf.
#
# Compiled WITHOUT -mgeneral-regs-only so it can do real `double`
# arithmetic. Linked into slmos.elf so the rest of the kernel can
# stay under the FP-register ban (no save/restore on context switch
# or IRQ entry). Float formatting fires only from diagnostic /
# Lua-driven output paths, where the FP-register touch is acceptable.
# ==========================================================================
add_library(kprintf_float STATIC kernel/src/kprintf_float.c)
target_include_directories(kprintf_float PRIVATE ${KERNEL_INCLUDES})

if(PLATFORM STREQUAL "X86_64")
    target_compile_options(kprintf_float PRIVATE
        -ffreestanding
        -nostdlib
        -mno-red-zone -fno-stack-protector -fno-pic
        -msse -msse2
        -O2
    )
else()
    # ARM64: omit -mgeneral-regs-only so FP/NEON is available.
    target_compile_options(kprintf_float PRIVATE
        -ffreestanding
        -nostdlib
        -mcpu=${TARGET_CPU}
        -fno-pie
        -O2
    )
endif()
target_link_libraries(slmos.elf PRIVATE kprintf_float)

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
#   - Jetson Orin Nano: 3 consecutive clean `bench stealing 16` runs
#     on jetson-nano-2 (20.9 ms, 2/4/4/4/1/1 distribution across all
#     6 CPUs) after #166 was resolved by moving Jetson onto the Pi 5
#     runtime `spinlock_hw_enabled` model. Default ON.
#
# Override path (either direction) is the Makefile `WORK_STEALING=ON`
# / `WORK_STEALING=OFF` variable, which maps to
# `-DENABLE_WORK_STEALING=ON/OFF`.
if(PLATFORM STREQUAL "X86_64" OR PLATFORM STREQUAL "RASPI5" OR
   PLATFORM STREQUAL "JETSON_ORIN_NANO")
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
    # GA10B does NOT ship the discrete-Ampere GSP-RM firmware stack
    # (gsp-535.113.01.bin et al.). L4T R36.4.7 /lib/firmware/nvidia/ga10b/
    # contains only nvgpu-native blobs (acr-gsp, FECS/GPCCS, PMU). The
    # 7-phase GSP bringup therefore does not apply on Jetson. Default OFF
    # to keep the build quiet and make the architectural difference
    # explicit. GSP_CHIP is still set so the shared-code paths that
    # reference it compile, but the firmware extraction block is skipped.
    set(GSP_CHIP "ga10b")
    set(_gsp_default OFF)
else()
    set(GSP_CHIP "")
    set(_gsp_default OFF)
endif()
option(ENABLE_GSP_FIRMWARE "Embed NVIDIA GSP firmware blobs (discrete Ampere only)" ${_gsp_default})

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

if(ENABLE_GSP_FIRMWARE AND GSP_CHIP)
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
    set(GSP_FW_COMPILE_DEFS "ENABLE_GSP_FIRMWARE=1;GSP_FW_GSP_PATH=${GSP_FW_DIR}/gsp.bin;GSP_FW_BOOTLOADER_PATH=${GSP_FW_DIR}/bootloader.bin;GSP_FW_BOOTER_LOAD_PATH=${GSP_FW_DIR}/booter_load.bin;GSP_FW_BOOTER_UNLOAD_PATH=${GSP_FW_DIR}/booter_unload.bin")

    if(PLATFORM STREQUAL "X86_64")
        set_source_files_properties(
            kernel/arch/x86_64/nvidia_gsp_firmware.S
            PROPERTIES
            COMPILE_DEFINITIONS "${GSP_FW_COMPILE_DEFS}"
            OBJECT_DEPENDS "${GSP_FW_STAMP}"
        )
    elseif(PLATFORM STREQUAL "JETSON_ORIN_NANO")
        # Firmware .S is not in the main source list (requires firmware
        # blobs to exist). Add it here, gated behind ENABLE_GSP_FIRMWARE.
        target_sources(slmos.elf PRIVATE kernel/arch/arm64/nvidia_gsp_firmware.S)
        set_source_files_properties(
            kernel/arch/arm64/nvidia_gsp_firmware.S
            PROPERTIES
            COMPILE_DEFINITIONS "${GSP_FW_COMPILE_DEFS}"
            OBJECT_DEPENDS "${GSP_FW_STAMP}"
        )
    endif()
    add_compile_definitions(ENABLE_GSP_FIRMWARE=1)

    message(STATUS "  GSP firmware: ${GSP_FW_DIR} (chip ${GSP_CHIP})")
endif()

# ============================================================================
# GA10B nvgpu-native firmware (Jetson Orin Nano — integrated Ampere)
# ============================================================================
#
# GA10B does NOT use the discrete-Ampere GSP-RM stack. It uses nvgpu-native
# firmware: ACR-GSP, FECS/GPCCS, PMU, NET runtime images, safety scheduler.
# The firmware lives on the Jetson at /lib/firmware/nvidia/ga10b/; pull it
# to the build host with scripts/tools/fetch-ga10b-firmware.sh.
#
# Gate: set -DGA10B_FIRMWARE_DIR=<path> at configure time. Without
# firmware the tree builds fine; the bringup code detects the missing
# blobs and fails Phase 0 cleanly.
#
# First-configure auto-detect (Jetson only): if the user has not
# already provided GA10B_FIRMWARE_DIR via -D or a prior cache entry,
# and the canonical fetch-script destination
# ($ENV{HOME}/jetson-ga10b-firmware, matching the default in
# scripts/tools/fetch-ga10b-firmware.sh) exists on the build host,
# seed the cache with that path so the canonical demo build picks it
# up automatically. Explicit `-DGA10B_FIRMWARE_DIR=` (empty) keeps
# the firmware embed disabled because the cache entry already exists
# at that point — `DEFINED CACHE{NAME}` (CMake ≥ 3.14; this project
# pins ≥ 3.20 at the top of the file) guards the auto-pick to the
# truly-fresh-configure case. The else branch only runs when the
# cache already holds a value, in which case the trailing
# `set(... CACHE PATH ...)` without FORCE is a no-op — i.e. the
# `_ga10b_initial = ""` assignment can never clobber a user override.
if(PLATFORM STREQUAL "JETSON_ORIN_NANO" AND NOT DEFINED CACHE{GA10B_FIRMWARE_DIR})
    set(_ga10b_default_dir "$ENV{HOME}/jetson-ga10b-firmware")
    if(IS_DIRECTORY "${_ga10b_default_dir}")
        set(_ga10b_initial "${_ga10b_default_dir}")
        message(STATUS "  GA10B firmware: auto-detected at ${_ga10b_default_dir}")
    else()
        set(_ga10b_initial "")
    endif()
else()
    set(_ga10b_initial "")
endif()
# CACHE without FORCE: writes only when the cache entry doesn't exist
# yet, so a user-supplied -DGA10B_FIRMWARE_DIR=... (even empty) is
# preserved across reconfigures.
set(GA10B_FIRMWARE_DIR "${_ga10b_initial}" CACHE PATH
    "Directory containing ga10b nvgpu firmware (see scripts/tools/fetch-ga10b-firmware.sh)")

if(PLATFORM STREQUAL "JETSON_ORIN_NANO" AND GA10B_FIRMWARE_DIR)
    # Verify the required files exist. If the user points us at an empty or
    # incomplete dir, fail fast with a clear message rather than produce a
    # build that's missing ucode.
    set(_ga10b_required
        "acr-gsp.text.encrypt.bin.prod"
        "acr-gsp.data.encrypt.bin.prod"
        "acr-gsp.manifest.encrypt.bin.out.bin.prod"
        "fecs_encrypt_prod.bin"
        "fecs_pkc_sig_encrypt.bin"
        "gpccs_encrypt_prod.bin"
        "gpccs_pkc_sig_encrypt.bin"
        "gpmu_ucode_next_prod_image.bin"
        "gpmu_ucode_next_prod_desc.bin"
        "pmu_pkc_prod_sig.bin"
        "NETA_img_prod_encrypted.bin"
        "NETB_img_prod_encrypted.bin"
        "NETC_img_prod_encrypted.bin"
        "NETD_img_prod_encrypted.bin"
        "safety-scheduler.text.encrypt.bin.prod"
        "safety-scheduler.data.encrypt.bin.prod"
        "safety-scheduler.manifest.encrypt.bin.out.bin.prod")
    set(_ga10b_missing "")
    foreach(_f ${_ga10b_required})
        if(NOT EXISTS "${GA10B_FIRMWARE_DIR}/${_f}")
            list(APPEND _ga10b_missing "${_f}")
        endif()
    endforeach()
    if(_ga10b_missing)
        message(WARNING
            "GA10B_FIRMWARE_DIR=${GA10B_FIRMWARE_DIR} is set but required "
            "files are missing: ${_ga10b_missing}. "
            "Run scripts/tools/fetch-ga10b-firmware.sh to pull from the Jetson. "
            "Disabling GA10B firmware embedding.")
    else()
        target_sources(slmos.elf PRIVATE kernel/arch/arm64/nvidia_ga10b_firmware.S)
        set(_ga10b_defs
            "GA10B_FW_ACR_TEXT_PATH=${GA10B_FIRMWARE_DIR}/acr-gsp.text.encrypt.bin.prod"
            "GA10B_FW_ACR_DATA_PATH=${GA10B_FIRMWARE_DIR}/acr-gsp.data.encrypt.bin.prod"
            "GA10B_FW_ACR_MANIFEST_PATH=${GA10B_FIRMWARE_DIR}/acr-gsp.manifest.encrypt.bin.out.bin.prod"
            "GA10B_FW_FECS_PATH=${GA10B_FIRMWARE_DIR}/fecs_encrypt_prod.bin"
            "GA10B_FW_FECS_SIG_PATH=${GA10B_FIRMWARE_DIR}/fecs_pkc_sig_encrypt.bin"
            "GA10B_FW_GPCCS_PATH=${GA10B_FIRMWARE_DIR}/gpccs_encrypt_prod.bin"
            "GA10B_FW_GPCCS_SIG_PATH=${GA10B_FIRMWARE_DIR}/gpccs_pkc_sig_encrypt.bin"
            "GA10B_FW_PMU_IMAGE_PATH=${GA10B_FIRMWARE_DIR}/gpmu_ucode_next_prod_image.bin"
            "GA10B_FW_PMU_DESC_PATH=${GA10B_FIRMWARE_DIR}/gpmu_ucode_next_prod_desc.bin"
            "GA10B_FW_PMU_SIG_PATH=${GA10B_FIRMWARE_DIR}/pmu_pkc_prod_sig.bin"
            "GA10B_FW_NET_A_PATH=${GA10B_FIRMWARE_DIR}/NETA_img_prod_encrypted.bin"
            "GA10B_FW_NET_B_PATH=${GA10B_FIRMWARE_DIR}/NETB_img_prod_encrypted.bin"
            "GA10B_FW_NET_C_PATH=${GA10B_FIRMWARE_DIR}/NETC_img_prod_encrypted.bin"
            "GA10B_FW_NET_D_PATH=${GA10B_FIRMWARE_DIR}/NETD_img_prod_encrypted.bin"
            "GA10B_FW_SAFETY_TEXT_PATH=${GA10B_FIRMWARE_DIR}/safety-scheduler.text.encrypt.bin.prod"
            "GA10B_FW_SAFETY_DATA_PATH=${GA10B_FIRMWARE_DIR}/safety-scheduler.data.encrypt.bin.prod"
            "GA10B_FW_SAFETY_MANIFEST_PATH=${GA10B_FIRMWARE_DIR}/safety-scheduler.manifest.encrypt.bin.out.bin.prod")
        set_source_files_properties(
            kernel/arch/arm64/nvidia_ga10b_firmware.S
            PROPERTIES
            COMPILE_DEFINITIONS "${_ga10b_defs}"
        )
        add_compile_definitions(ENABLE_GA10B_FIRMWARE=1)
        message(STATUS "  GA10B firmware: ${GA10B_FIRMWARE_DIR}")
    endif()
endif()

if(ENABLE_AI_SCHEDULER)
    add_compile_definitions(CONFIG_AI_SCHEDULER=1)

    add_library(ai_sched STATIC
        kernel/sched/ai/ai_inference.c
        kernel/sched/ai/ai_state.c
        kernel/sched/ai/sched_ai.c
        kernel/sched/ai/runtime_model.c
        kernel/sched/ai/inference_cpu.c
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
# Eviction Policies (optional model weights)
#
# Policies live entirely in the Rust runtime; CMake only threads the
# feature flags through to cargo via the top-level Makefile. The
# compile definitions are exported so C shell/test code can guard
# calls to the Rust FFI on `CONFIG_AI_EVICTION`.
# ==========================================================================
option(DISABLE_EVICTION "Disable the pluggable eviction-policy framework" OFF)
option(ENABLE_EVICTION_MODELS "Include trained XGBoost + MLP weights (~1.3 MB)" OFF)
set(EVICTION_DEFAULT_POLICY "lru" CACHE STRING
    "Compiled-in default eviction policy when eviction is enabled")
set_property(CACHE EVICTION_DEFAULT_POLICY PROPERTY STRINGS
    lru lfu arc slm xgboost mlp cacheus cacheus_all5 first_candidate)

if(ENABLE_EVICTION_MODELS AND DISABLE_EVICTION)
    message(FATAL_ERROR
        "ENABLE_EVICTION_MODELS requires eviction to be enabled. "
        "Remove -DDISABLE_EVICTION=ON or use DISABLE_EVICTION=OFF EVICTION_MODELS=ON.")
endif()

if(NOT DISABLE_EVICTION)
    add_compile_definitions(CONFIG_AI_EVICTION=1)
    if(ENABLE_EVICTION_MODELS)
        add_compile_definitions(CONFIG_AI_EVICTION_MODELS=1)
        message(STATUS "  Eviction: ENABLED (default=${EVICTION_DEFAULT_POLICY}, trained models)")
    else()
        message(STATUS "  Eviction: ENABLED (default=${EVICTION_DEFAULT_POLICY}, classical + stub ML policies)")
    endif()
else()
    message(STATUS "  Eviction: disabled")
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
