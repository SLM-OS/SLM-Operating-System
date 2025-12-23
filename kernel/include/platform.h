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
#define RAM_SIZE            0x08000000UL    /* 128 MB default */

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

/* Kernel stack */
#define STACK_SIZE          0x4000UL        /* 16 KB per stack */

#endif /* PLATFORM_QEMU_VIRT */

/* ============================================================================
 * NVIDIA Jetson Orin Nano
 *
 * Tegra234 SoC with 6x Cortex-A78AE cores.
 * Uses NS16550-compatible UART and GICv2.
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
#define RAM_SIZE            0x100000000UL   /* 4 GB (Orin Nano 4GB variant) */
                                            /* 8 GB for 8GB variant */

/* UART - Tegra NS16550-compatible (UARTA on GPIO header) */
#define UART_TYPE_TEGRA
#define UART_BASE           0x03100000UL    /* UARTA */
#define UART_CLOCK          408000000UL     /* 408 MHz (Tegra default) */
#define UART_IRQ            (32 + 112)      /* GIC_SPI 112 → IRQ 144 */

/*
 * Alternative UARTs:
 *   UARTE: 0x03140000 (GIC_SPI 116)
 *   UARTI: 0x031D0000 (GIC_SPI 285, ARM SBSA UART - different driver)
 */

/* GIC (Generic Interrupt Controller) v2 */
#define GIC_DIST_BASE       0x0F400000UL
#define GIC_CPU_BASE        0x0F440000UL

/* Timer - ARM Generic Timer */
#define TIMER_IRQ           30              /* PPI (same as QEMU) */

/* CPU configuration */
#define CPU_MAX             6               /* 6x Cortex-A78AE */

/* Kernel stack */
#define STACK_SIZE          0x4000UL        /* 16 KB per stack */

/* GPU (for reference, actual init in kernel/gpu/) */
#define GPU_BASE            0x17000000UL    /* Display controller base */

/*
 * Additional Jetson-specific peripherals (for future use):
 *
 * I2C controllers:
 *   I2C1: 0x03160000
 *   I2C2: 0x0C240000
 *   I2C3: 0x0C250000
 *
 * SPI controllers:
 *   SPI1: 0x03210000
 *   SPI2: 0x03230000
 *
 * GPIO:
 *   GPIO: 0x02200000
 *
 * PWM:
 *   PWM1: 0x03280000
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
