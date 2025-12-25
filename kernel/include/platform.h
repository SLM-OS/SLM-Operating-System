/*
 * platform.h - Hardware configuration for SLM-OS
 *
 * Platform-specific hardware addresses and configuration.
 * Select platform at build time via CMake or Makefile.
 *
 * NOTE: These are hardcoded values. Device Tree parsing will replace
 * these values in Phase 3 Milestone 4 (Required — Deferred).
 */

#ifndef PLATFORM_H
#define PLATFORM_H

/* ============================================================================
 * Platform Selection
 *
 * Define ONE of the following in the build system:
 *   - PLATFORM_QEMU_VIRT        (default, for development)
 *   - PLATFORM_JETSON_ORIN_NANO (for Jetson hardware)
 *
 * If neither is defined, default to QEMU.
 * ============================================================================ */

#if !defined(PLATFORM_QEMU_VIRT) && !defined(PLATFORM_JETSON_ORIN_NANO)
#define PLATFORM_QEMU_VIRT  1
#endif

/* ============================================================================
 * QEMU virt Machine
 *
 * ARM64 virtual machine for development and testing.
 * Uses PL011 UART and GICv2 at standard QEMU virt addresses.
 * ============================================================================ */
#if defined(PLATFORM_QEMU_VIRT)

/* Platform identification */
#define PLATFORM_NAME       "QEMU virt"

/* Memory layout */
#define RAM_BASE            0x40000000UL
#define RAM_SIZE            0x40000000UL    /* 1 GB for large model testing */

/* UART - PL011 */
#define UART_TYPE_PL011
#define UART_BASE           0x09000000UL
#define UART_CLOCK          24000000UL      /* 24 MHz (QEMU default) */
#define UART_IRQ            33              /* SPI 1 */

/* GIC (Generic Interrupt Controller) v2 */
#define GIC_DIST_BASE       0x08000000UL
#define GIC_CPU_BASE        0x08010000UL

/* Timer - ARM Generic Timer */
#define TIMER_IRQ           30              /* PPI (CNTV_EL0) */

/* CPU configuration */
#define CPU_MAX             4               /* QEMU default */

#endif /* PLATFORM_QEMU_VIRT */

/* ============================================================================
 * NVIDIA Jetson Orin Nano
 *
 * Tegra234 SoC with 6x Cortex-A78AE cores.
 * Uses Tegra HSUART and GICv3.
 *
 * Hardware info gathered from device tree on actual hardware:
 * - serial@3100000: nvidia,tegra194-hsuart (UARTA on 40-pin header)
 * - interrupt-controller@f400000: arm,gic-v3
 * - RAM starts at 0x80000000, 8GB total
 *
 * References:
 * - Linux kernel: arch/arm64/boot/dts/nvidia/tegra234.dtsi
 * - NVIDIA Jetson Linux Developer Guide
 * ============================================================================ */
#if defined(PLATFORM_JETSON_ORIN_NANO)

/* Platform identification */
#define PLATFORM_NAME       "Jetson Orin Nano"

/* Memory layout */
#define RAM_BASE            0x80000000UL
#define RAM_SIZE            0x200000000UL   /* 8 GB */

/*
 * UART - Tegra High-Speed UART (HSUART)
 *
 * UARTA is exposed on 40-pin GPIO header:
 *   Pin 6:  GND
 *   Pin 8:  UART1_TX (Jetson transmits)
 *   Pin 10: UART1_RX (Jetson receives)
 *
 * The debug header (J14) uses TCU (Tegra Combined UART) which requires
 * BPMP firmware - not suitable for bare-metal initially.
 */
#define UART_TYPE_TEGRA
#define UART_BASE           0x03100000UL    /* UARTA */
#define UART_SIZE           0x00010000UL    /* 64 KB */
#define UART_CLOCK          408000000UL     /* 408 MHz (Tegra default) */
#define UART_IRQ            (32 + 112)      /* GIC_SPI 112 -> IRQ 144 */

/*
 * Alternative UARTs:
 *   UARTC: 0x03140000 (serial2 alias)
 *   UARTD: 0x031D0000
 */

/*
 * GIC (Generic Interrupt Controller) v3
 *
 * Unlike GICv2, GICv3 has:
 *   - GICD: Distributor (shared, one per system)
 *   - GICR: Redistributor (per-CPU, handles PPIs/SGIs)
 *
 * No GIC CPU interface - CPUs use system registers instead.
 */
#define GIC_VERSION         3
#define GIC_DIST_BASE       0x0F400000UL    /* GICD */
#define GIC_DIST_SIZE       0x00010000UL    /* 64 KB */
#define GIC_REDIST_BASE     0x0F440000UL    /* GICR (per-CPU redistributors) */
#define GIC_REDIST_SIZE     0x00200000UL    /* 2 MB (covers all CPUs) */

/* Timer - ARM Generic Timer */
#define TIMER_IRQ           30              /* PPI 14 (CNTV_EL0) - same as QEMU */

/* CPU configuration */
#define CPU_MAX             6               /* 6x Cortex-A78AE */

/* GPU (for reference, actual init in kernel/gpu/) */
#define GPU_BASE            0x17000000UL

/*
 * BPMP (Boot and Power Management Processor)
 *
 * Required for clock/reset control on Tegra234.
 * Communication uses HSP (Hardware Synchronization Primitives) mailbox
 * and IVC (Inter-VM Communication) protocol over shared SRAM.
 */
#define HSP_TOP_BASE        0x03C00000UL    /* HSP controller for BPMP */
#define SYSRAM_BASE         0x40000000UL    /* Shared SRAM */
#define BPMP_TX_BASE        0x40070000UL    /* CPU->BPMP IVC channel (4KB) */
#define BPMP_RX_BASE        0x40071000UL    /* BPMP->CPU IVC channel (4KB) */

/* BPMP clock IDs (from tegra234-clock.h) */
#define TEGRA234_CLK_UARTA  155

/*
 * Additional Jetson-specific peripherals (from device tree):
 *
 * Timer:           0x02080000
 * GPIO:            0x02200000
 * Pinmux:          0x02430000
 * I2C1:            0x03160000
 * I2C2:            0x03180000
 * I2C3:            0x031B0000
 * SPI1:            0x03210000
 * SPI2:            0x03230000
 * PWM1-4:          0x03280000 - 0x032F0000
 * Memory Ctrl:     0x02C00000
 * Host1x:          0x13E00000
 */

#endif /* PLATFORM_JETSON_ORIN_NANO */

/* ============================================================================
 * Sanity Checks
 * ============================================================================ */

#ifndef PLATFORM_NAME
#error "No platform selected. Define PLATFORM_QEMU_VIRT or PLATFORM_JETSON_ORIN_NANO"
#endif

#ifndef RAM_BASE
#error "RAM_BASE not defined for selected platform"
#endif

#ifndef UART_BASE
#error "UART_BASE not defined for selected platform"
#endif

#ifndef GIC_DIST_BASE
#error "GIC_DIST_BASE not defined for selected platform"
#endif

#endif /* PLATFORM_H */
