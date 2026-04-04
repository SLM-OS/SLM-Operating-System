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

#if !defined(PLATFORM_QEMU_VIRT) && !defined(PLATFORM_JETSON_ORIN_NANO) && !defined(PLATFORM_RASPI5)
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

/* BPMP reset IDs (from tegra234-reset.h) */
#define TEGRA234_RESET_UARTA  100

/*
 * Watchdog Timer (WDT)
 *
 * Linux starts a hardware watchdog with a 120 second timeout.
 * After kexec, the watchdog continues running and will reset the
 * system unless disabled early in boot.
 *
 * Register offsets (from tegra_wdt.c driver):
 *   WDT_CFG:    0x0
 *   WDT_STS:    0x4
 *   WDT_CMD:    0x8
 *   WDT_UNLOCK: 0xC
 *
 * To disable: write 0xC45A to UNLOCK, then write 0x2 to CMD
 */
#define WDT_BASE            0x02190000UL
#define WDT_CFG             0x0
#define WDT_STS             0x4
#define WDT_CMD             0x8
#define WDT_UNLOCK          0xC
#define WDT_UNLOCK_PATTERN  0xC45A
#define WDT_CMD_DISABLE     0x2

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
 * Raspberry Pi 5 (BCM2712)
 *
 * Quad-core Cortex-A76 @ 2.4 GHz with RP1 I/O controller.
 * Uses PL011 UART and GIC-400 (GICv2).
 *
 * Hardware info from Circle library and Raspberry Pi forums:
 * - RP1 peripherals mapped via PCIe at 0x1F00000000
 * - UART0 (PL011): 0x1F00030000 (GPIO14/15 on 40-pin header)
 * - GIC-400: 0x107FFF8000
 * - Physical memory starts at 0x0
 * - Kernel loaded at 0x80000 by firmware
 *
 * References:
 * - Circle library: https://github.com/rsta2/circle
 * - RP1 Peripherals document
 * - Raspberry Pi 5 forums
 * ============================================================================ */
#if defined(PLATFORM_RASPI5)

/* Platform identification */
#define PLATFORM_NAME       "Raspberry Pi 5"

/* Memory layout */
#define RAM_BASE            0x00000000UL
#define RAM_SIZE            0x100000000UL   /* 4 GB (adjust for 8GB model) */
#define KERNEL_LOAD_ADDR    0x80000UL       /* Firmware loads kernel here */

/*
 * UART - PL011 via RP1
 *
 * UART0 is exposed on 40-pin GPIO header:
 *   Pin 6:  GND
 *   Pin 8:  GPIO14 (TXD) - Pi transmits
 *   Pin 10: GPIO15 (RXD) - Pi receives
 *
 * The RP1 I/O controller is connected via PCIe and peripherals
 * are mapped starting at 0x1F00000000.
 */
/*
 * RP1 PL011 UART driver (hardware TX/RX, interrupt-capable).
 * Uses UART0 on RP1 via PCIe at 0x1F00030000, GPIO14=TXD, GPIO15=RXD.
 */
#define UART_TYPE_RP1
#define UART_BASE           0x1F00030000UL  /* UART0 via RP1 */
#define UART_SIZE           0x00001000UL    /* 4 KB */
#define UART_CLOCK          50000000UL      /* 50 MHz (confirmed by testing; 48 MHz garbles output) */
/*
 * RP1 Interrupt Architecture
 *
 * RP1 peripheral IRQ → RP1 MSI-X engine → PCIe MSI-X write →
 * BCM2712 PCIe RC BAR1 → MIP0 (MSI-X Interrupt Peripheral) → GIC SPI
 *
 * MIP0 maps 64 MSI-X vectors to GIC SPIs starting at SPI 128:
 *   RP1 vector N → MIP0 vector N → GIC SPI (128 + N) → GIC IRQ (160 + N)
 *
 * Three hardware blocks must be configured:
 * 1. PCIe RC BAR1: routes MSI-X writes (PCI addr 0xFF_FFFFF000) to MIP0
 * 2. MIP0: unmasks vectors, converts MSI-X to GIC SPIs
 * 3. RP1 MSIX_CFG: enables per-vector MSI-X forwarding
 */
#define UART_IRQ            (32 + 128 + RP1_INT_UART0) /* = 185: SPI 153 */

/* RP1 PCIE_CFG register block (MSI-X configuration) */
#define RP1_INTC_BASE       0x1F00108000UL
#define RP1_INTC_INTSTATL   0x108           /* Raw interrupt status [31:0] */
#define RP1_INTC_INTSTATH   0x10C           /* Raw interrupt status [63:32] */
#define RP1_INTC_SET        0x800           /* Atomic set alias offset */

/* MSIX_CFG register for a given RP1 vector (0-63) */
#define RP1_MSIX_CFG(n)     (0x008 + (n) * 4)

/* MSIX_CFG bit fields */
#define MSIX_CFG_ENABLE     (1 << 0)
#define MSIX_CFG_IACK       (1 << 2)
#define MSIX_CFG_IACK_EN    (1 << 3)

/* RP1 peripheral interrupt vector numbers (from rp1-peripherals.pdf) */
#define RP1_INT_UART0       25

/* BCM2712 PCIe RC (Root Complex) for pcie2 (RP1's link) */
#define PCIE_RC_BASE        0x1000120000UL
#define PCIE_RC_SIZE        0x10000UL

/* PCIe RC register offsets for BAR1→MIP routing */
#define PCIE_RC_BAR1_CONFIG_LO      0x402C
#define PCIE_RC_BAR1_CONFIG_HI      0x4030
#define PCIE_RC_UBUS_BAR1_REMAP     0x40AC
#define PCIE_RC_UBUS_BAR1_REMAP_HI  0x40B0
#define PCIE_RC_PCIE_STATUS         0x4068  /* Bit 5 = DL_ACTIVE */

/* MIP0 (MSI-X Interrupt Peripheral) */
#define MIP0_BASE           0x1000130000UL
#define MIP0_SIZE           0xC0UL
#define MIP0_BASE_SPI       128             /* MIP vector 0 → GIC SPI 128 */

/* MIP register offsets */
#define MIP_INT_RAISED      0x00
#define MIP_INT_CLEARED     0x10
#define MIP_INT_CFGL_HOST   0x20
#define MIP_INT_CFGH_HOST   0x30
#define MIP_INT_MASKL_HOST  0x40
#define MIP_INT_MASKH_HOST  0x50
#define MIP_INT_MASKL_VPU   0x60
#define MIP_INT_MASKH_VPU   0x70

/* RP1 BAR0: MSI-X table (61 entries × 16 bytes each) */
#define RP1_MSIX_TABLE_BASE 0x1F00410000UL
#define RP1_MSIX_TABLE_SIZE 61              /* RP1_INT_END = 61 vectors */

/* PCIe RC config space access (EXT_CFG mechanism) */
#define PCIE_RC_EXT_CFG_INDEX  0x9000       /* Write bus/devfn selector */
#define PCIE_RC_EXT_CFG_DATA   0x8000       /* Config data (4KB window, per Circle) */

/* MSI-X target address (PCIe address that RC BAR1 routes to MIP0) */
#define MSIX_MSG_ADDR_LO   0xFFFFF000UL
#define MSIX_MSG_ADDR_HI   0x000000FFUL

/*
 * Alternative UARTs (via RP1):
 *   UART1: 0x1F00034000
 *   UART2: 0x1F00038000
 *   UART3: 0x1F0003C000
 *   UART4: 0x1F00040000
 *   UART5: 0x1F00044000
 */

/*
 * GIC (Generic Interrupt Controller) - GIC-400 (v2)
 *
 * IMPORTANT: Device tree shows 0x107FFF9000 but that's GICD offset.
 * The actual GIC base is 0x107FFF8000.
 *
 * Register offsets from GIC base:
 *   GICD: +0x1000
 *   GICC: +0x2000
 */
#define GIC_VERSION         2
#define GIC_BASE            0x107FFF8000UL
#define GIC_DIST_BASE       0x107FFF9000UL  /* GIC_BASE + 0x1000 */
#define GIC_CPU_BASE        0x107FFFA000UL  /* GIC_BASE + 0x2000 */

/* Timer - ARM Generic Timer */
#define TIMER_IRQ           30              /* PPI 14 (CNTV_EL0) */

/* CPU configuration */
#define CPU_MAX             4               /* 4x Cortex-A76 */

/*
 * Additional BCM2712/RP1 peripherals (for reference):
 *
 * RP1 Base:        0x1F00000000
 * GPIO:            0x1F000D0000
 * I2C0:            0x1F00070000
 * I2C1:            0x1F00074000
 * SPI0:            0x1F00050000
 * PWM:             0x1F00098000
 */

#endif /* PLATFORM_RASPI5 */

/* ============================================================================
 * Sanity Checks
 * ============================================================================ */

#ifndef PLATFORM_NAME
#error "No platform selected. Define PLATFORM_QEMU_VIRT, PLATFORM_JETSON_ORIN_NANO, or PLATFORM_RASPI5"
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
