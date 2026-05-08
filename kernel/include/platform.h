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

#if !defined(PLATFORM_QEMU_VIRT) && !defined(PLATFORM_JETSON_ORIN_NANO) && !defined(PLATFORM_RASPI5) && !defined(PLATFORM_X86_64)
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
#define TIMER_IRQ           30              /* PPI 14 — NS Phys Timer (CNTP_*_EL0) */

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
 * UART - Tegra NS16550-compatible UART
 *
 * Uses UARTC (0x0C280000) instead of UARTA (0x03100000) because:
 *   - SLM-OS runs at EL2 after kexec (confirmed via PSCI probe)
 *   - CBB firewall blocks UARTA access but allows UARTC from EL2
 *   - UARTC output is visible via TCU (Tegra Combined UART) on
 *     the USB-C debug serial console
 *
 * UARTA (40-pin header, 0x03100000): BLOCKED by CBB firewall
 * UARTC (0x0C280000): Accessible from EL2, confirmed working
 */
#define UART_TYPE_TEGRA
#define UART_BASE           0x0C280000UL    /* UARTC (EL2 accessible) */
#define UART_SIZE           0x00010000UL    /* 64 KB */
#define UART_CLOCK          408000000UL     /* 408 MHz (Tegra default) */
#define UART_IRQ            (32 + 114)      /* GIC_SPI 114 (UARTC) */

/*
 * Alternative UARTs:
 *   UARTA: 0x03100000 (40-pin header) — BLOCKED by CBB at EL2
 *   UARTD: 0x031D0000
 */

/*
 * TCU (Tegra Combined UART) RX Mailbox
 *
 * The TCU multiplexes serial I/O through the SPE firmware. TX can go
 * directly through UARTC hardware, but RX arrives via HSP shared mailbox.
 * SPE reads bytes from the USB-C physical UART and writes packed messages
 * to TOP0_HSP Shared Mailbox 0 at 0x03C10000.
 *
 * Mailbox format (32-bit):
 *   Bit 31:     Data present (TAG bit, set by SPE)
 *   Bits 25:24: Byte count (1-3)
 *   Bits 23:16: Byte 2 (if count >= 3)
 *   Bits 15:8:  Byte 1 (if count >= 2)
 *   Bits 7:0:   Byte 0
 *
 * After reading, write 0 to clear the mailbox so SPE can send more.
 * Escape protocol: 0xFF followed by tag byte switches RX channel.
 *
 * Source: NVIDIA tegra-combined-uart.c, rt-aux-cpu-demo SPE firmware.
 */
#define TCU_RX_MBOX         0x03C10000UL    /* TOP0_HSP SM0 (SPE → CCPLEX) */
#define TCU_MBOX_TAG_BIT    (1UL << 31)     /* Data present flag */

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
#define TIMER_IRQ           30              /* PPI 14 — NS Phys Timer (CNTP_*_EL0), same as QEMU */

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
#define TEGRA234_CLK_UARTA           155
#define TEGRA234_CLK_PEX2_C8_CORE    172   /* PCIe C8 controller core clock */
#define TEGRA234_CLK_GPUSYS          304   /* GPU system clock */
#define TEGRA234_CLK_GPU_PWR         42    /* GPU power clock */
#define TEGRA234_CLK_GPC0CLK         41    /* GPU GPC0 engine clock */
#define TEGRA234_CLK_GPC1CLK         236   /* GPU GPC1 engine clock */

/* BPMP reset IDs (from tegra234-reset.h) */
#define TEGRA234_RESET_UARTA         100
#define TEGRA234_RESET_PEX2_CORE_8   25    /* PCIe C8 controller reset */
#define TEGRA234_RESET_PEX2_CORE_8_APB  26 /* PCIe C8 APB reset */
#define TEGRA234_RESET_GPU           19

/* BPMP power-domain IDs (from tegra234-powergate.h) */
#define TEGRA234_POWER_DOMAIN_PCIEX4CA  13  /* PCIe C8 domain */

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

/*
 * PCIe Root Complex C8 (pcie@140a0000) — hosts the RTL8168 NIC on the
 * Jetson Orin Nano Super Developer Kit. Integrated Tegra Ethernet
 * controllers (nveqos@2310000 + 4× mgbe@68[0-3]00000) are all marked
 * status="disabled" in the carrier-board DT, so the RJ45 traffic goes
 * out through this PCIe slot instead.
 *
 * Register layout per NVIDIA Tegra 234 TRM and Linux's pcie-tegra194.c:
 *   0x140a0000 (128 KB) — APPL (controller wrapper registers)
 *   0x2A000000 (256 KB) — "config" window (iATU-retargeted, NOT flat
 *                         ECAM; bus 1+ accesses go through this after
 *                         the RC driver reprograms iATU)
 *   0x2A040000 (256 KB) — iATU / eDMA registers
 *   0x2A080000 (256 KB) — DBI (DesignWare native register file; bus
 *                         0 dev 0 fn 0 config space maps here directly)
 *
 * Note: after a Linux kexec the tegra194-pcie driver's .shutdown hook
 * has torn down the RC (LTSSM off, PHY powered down, REFCLK gated,
 * clocks+resets asserted, BPMP told to disable the controller). MMIO
 * reads to DBI/APPL will abort or return 0xFFFFFFFF until either:
 *   (a) slmos-kexec is modified to unbind tegra194-pcie before kexec,
 *       preserving the running RC state through the transition, or
 *   (b) SLM-OS ports the controller bring-up sequence (blocked on #190,
 *       BPMP MRQ permissions at EL2).
 */
#define TEGRA_PCIE_C8_APPL_BASE  0x140A0000UL    /* Controller wrapper regs */
#define TEGRA_PCIE_C8_APPL_SIZE  0x00020000UL    /* 128 KB */
#define TEGRA_PCIE_C8_CFG_BASE   0x2A000000UL    /* iATU-retargeted config */
#define TEGRA_PCIE_C8_CFG_SIZE   0x00040000UL    /* 256 KB */
#define TEGRA_PCIE_C8_ATU_BASE   0x2A040000UL    /* iATU + eDMA */
#define TEGRA_PCIE_C8_ATU_SIZE   0x00040000UL    /* 256 KB */
#define TEGRA_PCIE_C8_DBI_BASE   0x2A080000UL    /* DesignWare DBI regs */
#define TEGRA_PCIE_C8_DBI_SIZE   0x00040000UL    /* 256 KB */

/* RTL8168 BAR window — Linux-assigned. Covers both BAR2 (regs, 4 KB at
 * +0x4000) and BAR4 (ext regs, 16 KB at +0x0000) in a single 2 MB block. */
#define RTL8169_BAR_WINDOW_BASE  0x3528000000UL
#define RTL8169_BAR2_BASE        0x3528004000UL  /* Main MMIO register bank */
#define RTL8169_BAR2_SIZE        0x00001000UL    /* 4 KB */
#define RTL8169_BAR4_BASE        0x3528000000UL  /* Extended registers */
#define RTL8169_BAR4_SIZE        0x00004000UL    /* 16 KB */

/* PCI address of the RTL8168 on PCIe C8: bus 0x01, dev 0x00, fn 0x00 */
#define RTL8169_PCI_BUS          0x01
#define RTL8169_PCI_DEV          0x00
#define RTL8169_PCI_FUNC         0x00
#define RTL8169_PCI_VENDOR       0x10ECU
#define RTL8169_PCI_DEVICE       0x8168U

/*
 * Tegra XHCI USB 3.0 host controller (tegra-xusb). Candidate for USB
 * CDC-ECM-based networking if the PCIe RC stays CBB-firewalled at EL2
 * (see docs/jetson-pcie-investigation.md). All three register banks
 * fit inside the 2 MB block at 0x03600000.
 */
#define TEGRA_XHCI_FPCI_BASE     0x03600000UL    /* Function-PCI regs (64 KB) */
#define TEGRA_XHCI_HCD_BASE      0x03610000UL    /* xHCI operational regs (~256 KB) */
#define TEGRA_XHCI_BAR2_BASE     0x03650000UL    /* xHCI BAR2 regs (64 KB) */

/*
 * Tegra XUDC device controller + XUSB pad controller. Both apertures
 * live in the 2 MB block at 0x03400000 (shared by UPHY padctl at
 * 0x03520000 and XUDC at 0x03550000). Mapped for the #266 Phase 0 CBB
 * reachability probe; see docs/jetson-usb-networking-plan.md §3 Phase 0.
 */
#define TEGRA_XUSB_PADCTL_BASE   0x03520000UL    /* UPHY/XUSB pad controller */
#define TEGRA_XUDC_BASE          0x03550000UL    /* XUDC device controller */

/*
 * Tegra234 camera subsystem MMIO bases. Verified at NS EL2 from the
 * #396 Phase 0 recon session on jetson-nano-1 (2026-04-25): all three
 * primary apertures (NVCSI, RCE HSP, cam_i2c) returned data without
 * raising a CBB external abort. The plan's earlier guesses
 * (`~0x03c00000` for camera-rtcpu HSP, `0x031c0000` for the camera
 * I²C bus) were both wrong — actual values come from the live
 * jetson-nano-1 device tree. See `docs/jetson-camera-imx219-plan.md`
 * §"Phase 0" for the recon results.
 *
 * `kernel/mm/vmm.c` identity-maps the 2 MB block containing each
 * mapped base so the eventual driver code (and the `peek` shell
 * command) can reach them; `kernel/tests/test_camera.c` pins these
 * constants via `_Static_assert` so accidental drift breaks the build.
 *
 * Naming prefix: these new bases use `TEGRA234_*` (matching the
 * already-established `TEGRA234_CLK_*` / `TEGRA234_RESET_*` pool in
 * `kernel/include/tegra234_clocks.h`) rather than the older
 * `TEGRA_*` prefix used by the PCIe-C8 / XHCI / XUSB bases above.
 * The `TEGRA234_*` form is more accurate — these MMIO addresses do
 * differ across Tegra generations (T194 vs T234) — and matches the
 * upstream Linux dt-binding file names. Renaming the older
 * `TEGRA_*` MMIO constants for consistency is a separate refactor;
 * pick one prefix when adding more here.
 */
#define TEGRA234_NVCSI_BASE      0x15A00000UL    /* MIPI CSI-2 receiver */
#define TEGRA234_RCE_HSP_BASE    0x0B950000UL    /* Camera-RTCPU HSP (mailbox/semaphore IPC) */
#define TEGRA234_RCE_PM_BASE     0x0B9F0000UL    /* RCE power-management regs (R5_CTRL, PWR_STATUS) */
/*
 * RCE main MMIO (Falcon EVP, AST). Defined here for completeness and
 * pinned by test_camera.c, but **not currently mapped** by
 * vmm.c — SLM-OS only drives RCE through HSP IPC today, never via
 * direct MMIO (the RCE Cortex-R5 is bootloader-loaded and stays
 * alive across kexec, so SLM-OS has no reason to touch the EVP/AST
 * apertures). If a future driver does need direct RCE access, add
 * `TEGRA234_RCE_BASE` to the `cam_bases[]` array in vmm.c — the
 * mapping pattern is one line.
 */
#define TEGRA234_RCE_BASE        0x0BC00000UL
#define TEGRA234_CAM_I2C_BASE    0x03180000UL    /* HSI2C-2 = `cam_i2c` (J17/J20 via i2c-mux-gpio) */

/*
 * Tegra234 GPIO controller bases (#396 Hardware Task 2 — IMX219
 * sensor power-up). Two controllers: `gpio_main` (gpiochip0 in Linux)
 * for the SoC's general-purpose pins, and `gpio_aon` (gpiochip1) for
 * the always-on / DPD-survivable pins. Same per-pin register layout
 * — every pin gets a 32-byte (0x20) MMIO window. The address arith
 * is documented in `docs/jetson-camera-tegra-gpio-notes.md`:
 *
 *   pin_base = controller_base + bank*0x1000 + port*0x200 + pin*0x20
 *   ENABLE_CONFIG = pin_base + 0x00
 *   INPUT         = pin_base + 0x08
 *   OUTPUT_CONTROL= pin_base + 0x0C
 *   OUTPUT_VALUE  = pin_base + 0x10
 *
 * **Two MMIO regions per controller** (verified live on the
 * jetson-nano-1 device tree, reg-names = "security gpio"):
 *   - reg[0] (security): per-pin SCR / VM window — owned by TF-A /
 *     bootloader, not consumed by SLM-OS. gpio_main 0x02200000,
 *     gpio_aon 0x0C2F0000.
 *   - reg[1] (gpio):      per-pin DATA window — what SLM-OS uses.
 *     gpio_main 0x02210000, gpio_aon 0x0C2F1000.
 * The bases below point at the *gpio* window. An earlier draft
 * pointed at the *security* window and every read returned
 * 0xFFFFFFFF (security regs are EL3-only at runtime).
 *
 * vmm.c maps the 2 MB block containing each base — both windows of
 * each controller fall inside the same block. test_camera.c pins
 * both addresses with _Static_assert.
 */
#define TEGRA234_GPIO_MAIN_BASE  0x02210000UL    /* gpiochip0 data window */
#define TEGRA234_GPIO_AON_BASE   0x0C2F1000UL    /* gpiochip1 data window */

/*
 * Tegra234 pinmux controllers. One 4-byte register per pad selects
 * the pad's function (GPIO vs SFIO peripheral), pull, drive enable,
 * input receiver, and other pad-level config. Per-pad register
 * offsets come from `tegra234_groups[]` in
 * `~/slmos-ref/linux/linux-pinctrl-tegra234.c` — see the per-offset
 * comment block below for why T194's table is *not* a safe source.
 * Pinmux register layout (per PIN_PINGROUP_ENTRY_Y):
 *   bits[1:0]  PM       — special-function select (0..3 = SF1..SF4)
 *   bits[3:2]  PUPD     — 0=none, 1=pull-down, 2=pull-up
 *   bit[4]     TRISTATE — 1 = high-Z; must be 0 to drive output
 *   bit[6]     E_INPUT  — 1 = input receiver enabled
 *   bit[10]    GPIO_SFIO_SEL — 0 = GPIO mode, 1 = SFIO mode
 * "GPIO mode, output drive, input receiver on" = 0x00000040.
 *
 * The MAIN pinmux at 0x02430000 lives in the 2 MB block at
 * 0x02400000; the AON pinmux at 0x0C300000 lives in the 2 MB block
 * at 0x0C200000 (same block as TEGRA234_GPIO_AON_BASE — already
 * mapped). vmm.c's cam_bases[] array adds the MAIN pinmux block.
 */
#define TEGRA234_PINMUX_MAIN_BASE  0x02430000UL  /* main pinmux controller */
#define TEGRA234_PINMUX_AON_BASE   0x0C300000UL  /* AON  pinmux controller */

/*
 * Per-pad pinmux register offsets for the IMX219-related pads (live
 * verification on jetson-nano-1, sourced from pinctrl-tegra234.c).
 * The pad names are misleading — these are conventional UART /
 * SPI pad names that happen to also be routable to GPIO.
 *
 *   PH.06  → uart4_cts_ph6   off 0x4008 (MAIN)  → 0x02434008
 *   PH.03  → uart4_tx_ph3    off 0x4020 (MAIN)  → 0x02434020
 *   PCC.03 → spi2_cs0_pcc3   off 0x2038 (AON)   → 0x0C302038
 *
 * "PCC.03" is the AON GPIO referred to as "CC.3" in the i2c-mux DT
 * binding — Linux's GPIO core renames PCC bits to CC.* for the AON
 * controller's 0..32 line range.
 *
 * **Don't trust pinmux offsets from Tegra194 source for T234.** The
 * pad table layout was reshuffled between T194 and T234; same pad
 * names but different register offsets. These values come from
 * `tegra234_groups[]` in `~/slmos-ref/linux/linux-pinctrl-tegra234.c`,
 * not the T194 table.
 */
#define TEGRA234_PINMUX_CAM_RESET_OFF  0x4008u   /* PH.06 (within MAIN) */
#define TEGRA234_PINMUX_CAM_PWR_OFF    0x4020u   /* PH.03 (within MAIN) */
#define TEGRA234_PINMUX_CAM_MUX_OFF    0x2038u   /* PCC.03 (within AON) */

/*
 * Pinmux register bit values for "GPIO output mode" (bit 6 = E_INPUT
 * for read-back; bit 10 = 0 = GPIO mode; bit 4 = 0 = drive enabled;
 * PM/PUPD = 0 = no pull). 0x00000000 also works for pure output
 * without input loopback; 0x40 is the safer default.
 */
#define TEGRA234_PINMUX_GPIO_OUTPUT    0x00000040u

/*
 * Per-pin window offsets for the three IMX219-related GPIOs on the
 * Jetson Orin Nano dev kit (carrier `p3768-0000+p3767-0005`,
 * verified live on jetson-nano-1 with the IMX219-A overlay loaded).
 * Each offset is the address of the pin's ENABLE_CONFIG register
 * relative to its controller base; the four register offsets above
 * apply on top.
 *
 *   PH.06 (cam_reset_gpio, gpiochip0 line 49 / global 62):
 *       releases the IMX219 from reset when driven HIGH.
 *   PH.03 (camera-control-output-low, gpiochip0 line 46 / global 59):
 *       enables the camera AVDD/DVDD/DOVDD regulator chain when
 *       driven HIGH (active-high regulator-fixed compatible).
 *   CC.3 (cam_i2cmux selector, gpiochip1 line 19):
 *       selects which physical CSI connector (A/J17 vs C/J20) the
 *       cam_i2c bus routes to. Linux's i2c-mux-gpio driver toggles
 *       this per-transaction; SLM-OS pre-positions it to channel-0
 *       (the connector with the camera attached).
 *
 * Address arithmetic example (PH.06):
 *   bank=4 (port H is in bank 4), port=1 (within bank 4), pin=6
 *   offset = 4*0x1000 + 1*0x200 + 6*0x20 = 0x42C0
 *   ENABLE_CONFIG absolute = TEGRA234_GPIO_MAIN_BASE + 0x42C0
 *                          = 0x02210000 + 0x42C0 = 0x022142C0
 *   (the security-window address 0x02200000 + 0x42C0 reads back
 *   as 0xFFFFFFFF — that mistake is what cost a deploy round.)
 */
#define TEGRA234_GPIO_CAM_RESET_OFF   0x42C0u    /* PH.06 ENABLE_CONFIG (within MAIN) */
#define TEGRA234_GPIO_CAM_PWR_OFF     0x4260u    /* PH.03 ENABLE_CONFIG (within MAIN) */
#define TEGRA234_GPIO_CAM_MUX_OFF     0x0460u    /* CC.3  ENABLE_CONFIG (within AON)  */

/*
 * Spinlock policy: use the runtime `spinlock_hw_enabled` flag, same as Pi 5.
 *
 * Before MMU enable, memory is non-cacheable and LSE atomics (SWPALB) cause
 * a Synchronous External Abort on Cortex-A78AE; exclusive monitors (LDAXR /
 * STXR) also require cacheable memory. `spinlock_hw_enabled` stays 0 until
 * `vmm_init()` enables the MMU, so pre-MMU spinlocks fall through to a
 * barrier-only path. Post-MMU, real LDAXR/STXR on cacheable memory works
 * on A78AE and is required for cross-CPU mutual exclusion now that SMP is
 * live (PMM, task-table, run-queue, and steal-deque locks all depend on it).
 *
 * Previously this file defined `SPINLOCK_SKIP_LOCKING=1` unconditionally,
 * which silently disabled every cacheable spinlock post-boot and caused a
 * free-list page fault during `bench stealing` with WORK_STEALING=ON
 * (issue #166). Switching to the runtime flag matches the Pi 5 model.
 *
 * The UART lock remains IRQ-disable-only (see `kernel/src/kprintf.c`) —
 * that path keys off `PLATFORM_HAS_NC_MEMORY`, not this flag.
 */

#endif /* PLATFORM_JETSON_ORIN_NANO */

/* ============================================================================
 * x86-64 PC
 *
 * Standard x86-64 hardware with UEFI boot via GRUB Multiboot2.
 * Uses 16550 UART (COM1) for serial console, LAPIC + IOAPIC for interrupts.
 * ============================================================================ */
#if defined(PLATFORM_X86_64)

/* Platform identification */
#define PLATFORM_NAME       "x86-64"

/* Memory layout */
#define RAM_BASE            0x00200000UL    /* Above kernel image */
#define RAM_SIZE            0x500000000UL   /* 20 GB max — sizes the block_state array in PMM */

/* UART - 16550 COM1 (I/O port, not MMIO) */
#define UART_TYPE_16550
#define UART_BASE           0x3F8UL
#define UART_IRQ            36              /* IRQ 4 → vector 36 */

/* Interrupt controller — LAPIC + IOAPIC (gic.h mapped via pic.c) */
#define GIC_DIST_BASE       0UL             /* Dummy — LAPIC/IOAPIC are memory-mapped */
#define GIC_CPU_BASE        0UL

/* Timer - 8254 PIT */
#define TIMER_IRQ           32              /* IRQ 0 → vector 32 */

/* CPU configuration */
#define CPU_MAX             8               /* i7-6700: 4 cores / 8 threads */

#endif /* PLATFORM_X86_64 */

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
 * BCM2712 PCIe RC BAR3 → MIP0 (MSI-X Interrupt Peripheral) → GIC SPI
 *
 * MIP0 maps 64 MSI-X vectors to GIC SPIs starting at SPI 128:
 *   RP1 vector N → MIP0 vector N → GIC SPI (128 + N) → GIC IRQ (160 + N)
 *
 * Three hardware blocks must be configured:
 * 1. PCIe RC BAR3: routes MSI-X writes (PCI addr 0xFF_FFFFF000) to MIP0
 * 2. MIP0: unmasks vectors, converts MSI-X to GIC SPIs
 * 3. RP1 MSIX_CFG: enables per-vector MSI-X forwarding
 */
#define UART_IRQ            (32 + 128 + RP1_INT_UART0) /* = 185: MIP0 vec 25 → SPI 153 */

/* RP1 PCIE_CFG register block (MSI-X configuration) */
#define RP1_INTC_BASE       0x1F00108000UL
#define RP1_INTC_INTSTATL   0x108           /* Raw interrupt status [31:0] */
#define RP1_INTC_INTSTATH   0x10C           /* Raw interrupt status [63:32] */
#define RP1_INTC_SET        0x800           /* Atomic set alias offset */

/* MSIX_CFG register for a given RP1 vector (0-63) */
#define RP1_MSIX_CFG(n)     (0x008 + (n) * 4)

/* MSIX_CFG bit fields */
#define MSIX_CFG_ENABLE     (1 << 0)
#define MSIX_CFG_TEST       (1 << 1)    /* Force one MSI-X fire (self-clearing) */
#define MSIX_CFG_IACK       (1 << 2)
#define MSIX_CFG_IACK_EN    (1 << 3)

/* RP1 peripheral interrupt vector numbers (from rp1-peripherals.pdf) */
#define RP1_INT_UART0       25
#define RP1_INT_ETH         6       /* Cadence MACB/GEM Ethernet — #202 */

/* RP1 Ethernet controller base addresses — #202.
 *
 * Despite the "BCM" SoC name, the Ethernet MAC on Pi 5 is a Cadence
 * MACB/GEM IP, NOT Broadcom GENET (GENET was used on Pi 4). The
 * Linux device tree identifies it as `raspberrypi,rp1-gem` with
 * `cdns,macb` as the fallback compat string.
 *
 * The IP lives inside the RP1 southbridge, reached through the same
 * PCIe BAR1 window UART/GPIO use. Linux's RP1 bindings header
 * (linux-rpi-dt-bindings-mfd-rp1.h) defines:
 *   RP1_ETH_IP_BASE  = RP1_BAR + 0x100000 = 0x1F00100000
 *   RP1_ETH_CFG_BASE = RP1_BAR + 0x104000 = 0x1F00104000   (not used yet)
 *
 * Both land in the same 2MB block as UART/GPIO (already mapped by
 * vmm_setup_platform for RASPI5), so no additional page-table entry
 * is required. The driver doesn't touch the CFG block today — the
 * define is omitted here to avoid the impression that it's wired up;
 * reintroduce it alongside the first consumer. */
#define RP1_ETH_IP_BASE     0x1F00100000UL

/* RP1 clock controller, at RP1_BAR + 0x18000 per rp1.dtsi. Stage 2
 * of the MACB driver writes CLK_ETH_CTRL / CLK_ETH_TSU_CTRL here to
 * enable the Ethernet clocks. Offsets from Linux drivers/clk/clk-rp1.c
 * (cached at ~/slmos-ref/linux/linux-rpi-clk-rp1.c). */
#define RP1_CLOCKS_BASE         0x1F00018000UL
#define RP1_CLK_ETH_CTRL        0x00064     /* 125 MHz TX clock */
#define RP1_CLK_ETH_TSU_CTRL    0x00134     /*  50 MHz timestamp unit clock */
#define RP1_CLK_CTRL_ENABLE     (1u << 11)

/* MACB interrupt — GIC IRQ 166 via MIP0 vector 6 → GIC SPI 134 */
#define MACB_IRQ            (32 + MIP0_BASE_SPI + RP1_INT_ETH)

/* BCM2712 VideoCore mailbox (property channel at 8). Live at the
 * same SoC peripheral window as earlier Pi SoCs, just remapped to a
 * 40-bit CPU physical. DTS `mailbox@7c013880` + the soc bridge
 * `ranges = <0x7c000000 0x10 0x7c000000 0x04000000>` resolve to
 * CPU phys 0x107C013880. Reachable directly from EL1 — no RP1 or
 * firmware indirection. 0x40 of MMIO; we only touch 4 registers.
 * The 2MB block containing this address is mapped explicitly in
 * vmm_setup_platform for RASPI5. */
#define BCM_MAILBOX_BASE    0x107C013880UL

/* BCM2712 EMMC2 — the SDHCI v3 controller behind the Pi 5's SD-card
 * slot. From rpi-linux-bcm2712.dtsi:1188 (sdio1: mmc@fff000), the
 * `reg = <0x10 0x00fff000  0x0 0x260>` resolves to CPU phys
 * 0x10_00FF_F000, size 0x260. Used by `sdhci_create_bcm2712()` in
 * the dynamic-kernel-replace Stage 5 hardware-bringup path (#371).
 * The 2 MB block containing this address is mapped explicitly in
 * vmm_setup_platform for RASPI5. */
#define BCM2712_EMMC2_BASE  0x1000FFF000UL

/* SDIO_CFG bank — separate MMIO window adjacent to the SDHCI host
 * registers. Linux's sdhci-brcmstb maps it as resource index 1 of
 * the same DT node (`reg = <0x00fff400 0x200>`, alias "cfg"); SLM-OS
 * computes it from the host base since it always lives at host+0x400
 * on BCM2712. cfginit_2712 (force-CD, base-clock advisory) writes
 * to this bank, NOT the SDHCI host registers. Falls in the same
 * 2 MB MMIO block as BCM2712_EMMC2_BASE so no extra page-table entry
 * is needed. */
#define BCM2712_EMMC2_CFG_BASE  (BCM2712_EMMC2_BASE + 0x400UL)

/* SDIO1 bus-isolation register — the SDIO1 node in `bcm2712.dtsi`
 * declares FOUR `reg` banks (host, cfg, busisol, lcpll). The third
 * bank, "busisol" at SoC-bus 0x015040b0 (4 bytes) → CPU phys
 * 0x10_015040B0, is a strap register that controls SD-Express PCIe
 * sideband isolation + SD pin tri-state — see Linux's
 * `bcm2712_init_sd_express` in `sdhci-brcmstb.c` (the only Linux
 * site that touches it).
 *
 * EMPIRICAL on the Pi 5 lab fixture: firmware leaves this register
 * at 0x00006001 at SLM-OS handoff. Per the SD-Express init code,
 * bits 13:14 (mask 0x6000) are PCIe-sideband isolation: SET =
 * SD-card mode, CLEARED = PCIe-sideband mode. Bit 0 is the SD-clock
 * isolation enable. Together 0x6001 is the correct "SD-card mode,
 * controller live" state — the firmware-left value is what we want.
 *
 * **DO NOT WRITE TO THIS REGISTER from the SD-card driver path.**
 * Clearing 0x6000 switches the controller to PCIe-sideband mode,
 * which is the wrong direction for normal SD operation. The
 * constant is exposed only so future SD-Express support can use it
 * with the correct Linux-style sequence; the SDHCI driver does NOT
 * touch it. The 2 MB block containing this address (0x1001400000)
 * is already mapped via `vmm_setup_platform` for the `bcm_reset`
 * controller, so reads of the firmware-left value (for diagnostic
 * purposes) are safe. */
#define BCM2712_SDIO1_BUSISOL   0x10015040B0UL

/* BCM2712 AON GPIO bank 0 base — `gio_aon@7d517c00` per
 * `bcm2712.dtsi`, mapped through the SoC ranges to CPU phys
 * 0x107D517C00. The 2 MB block containing this address is already
 * covered by `vmm_setup_platform` (used by the ACT LED on pin 9 in
 * `kernel/src/main.c`, and by the SD card VCC + IO voltage
 * regulators on pins 4 and 3 from `kernel/drivers/sdhci.c`).
 *
 * Bank 0 register layout matches Linux's
 * `drivers/gpio/gpio-brcmstb.c`:
 *   +0x00 ODEN   — open-drain enable (1 = open-drain; we want push-pull)
 *   +0x04 DATA   — read/write pin levels
 *   +0x08 IODIR  — 0 = output, 1 = input (brcmstb-specific; inverse
 *                  of the legacy bcm2835 GPIO convention)
 *
 * SD-card pin assignments (from `bcm2712-rpi-5-b.dts`):
 *   bit 3  sd_io_1v8_reg (0 = 3.3 V mode, 1 = 1.8 V mode)
 *   bit 4  sd_vcc_reg    (1 = SD card VCC on, 0 = off)
 *   bit 9  ACT LED */
#define BCM2712_AON_GPIO_BASE   0x107D517C00UL
#define BCM2712_AON_GPIO_DATA   (BCM2712_AON_GPIO_BASE + 0x04UL)
#define BCM2712_AON_GPIO_IODIR  (BCM2712_AON_GPIO_BASE + 0x08UL)

/* GPU bus-address encoding on Pi 5 matches the legacy VideoCore
 * convention — the VideoCore sees ARM DRAM via a 1 GB alias at
 * 0xC0000000. Used when passing a property buffer to the mailbox.
 * See Circle's circle-bcm2835.h `GPU_MEM_BASE` which is
 * `GPU_UNCACHED_BASE (= 0xC0000000)` on RASPPI >= 5. */
#define BCM_BUS_ADDRESS(addr)   ((((uintptr_t)(addr)) & ~0xC0000000UL) | 0xC0000000UL)

/* BCM2712 PCIe RC (Root Complex) for pcie2 (RP1's link) */
#define PCIE_RC_BASE        0x1000120000UL
#define PCIE_RC_SIZE        0x10000UL

/* PCIe RC register offsets for BAR3→MIP routing (Linux uses BAR3, not BAR1).
 * BAR1 is used by firmware for RP1 peripheral MMIO — do not modify BAR1. */
#define PCIE_RC_BAR3_CONFIG_LO      0x403C
#define PCIE_RC_BAR3_CONFIG_HI      0x4040
#define PCIE_RC_UBUS_BAR3_REMAP     0x40BC
#define PCIE_RC_UBUS_BAR3_REMAP_HI  0x40C0
#define PCIE_RC_MISC_CTRL           0x4008
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
#define PCIE_RC_EXT_CFG_DATA   0x8000       /* Config data (firmware uses default offset) */

/* MSI-X target address (PCIe address that RC BAR3 routes to MIP0).
 * Device tree value: <0xff 0xfffff000>. Confirmed by Linux register dump. */
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

/* Timer - ARM Generic Timer.
 *
 * #683 PR-4: SLM-OS now boots at EL2 with VHE on Pi 5 (#683 PR-2 +
 * PR-3), so the timer config follows Linux's `arch_timer_select_ppi()`
 * first branch (`is_kernel_in_hyp_mode()` → `ARCH_TIMER_HYP_PPI`):
 *
 *   - TIMER_IRQ = 26 (PPI 10 = Hyp Physical Timer / CNTHP).
 *   - timer.c programs CNTP_*_EL0 — under HCR_EL2.E2H=1 those
 *     accesses are silently redirected to CNTHP_*_EL2 (the Hyp
 *     Physical Timer's control / cval / tval registers), so the
 *     same source code that works at EL1 on QEMU also programs
 *     the right hardware on Pi 5 at EL2.
 *
 * History (closed by PR-4):
 *   - Originally PPI 30 (NS Phys Timer, CNTP). #672 found that
 *     BCM2712 / GIC-400 firmware does not route the NS-EL1 IRQ pin
 *     to the EL1 vector usefully; daifclr with the pin asserted
 *     hard-locked CPU 0.
 *   - PR-1 stopgap: PPI 27 (NS Virt Timer, CNTV) — Linux's
 *     HYP-not-available fallback. Same wedge: NS-EL1 IRQ delivery
 *     is broken regardless of which PPI is selected on Pi 5
 *     firmware.
 *   - PR-2/PR-3 moved the kernel to EL2/VHE. PR-4 (this) takes the
 *     production fix path: PPI 26 + Hyp Phys Timer, the path Linux
 *     and Pi firmware actually validate. IRQs route through
 *     VBAR_EL2.
 */
#define TIMER_IRQ           26              /* PPI 10 — Hyp Physical Timer (CNTHP, via CNTP_*_EL0 + VHE) */

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
#error "No platform selected. Define PLATFORM_QEMU_VIRT, PLATFORM_JETSON_ORIN_NANO, PLATFORM_RASPI5, or PLATFORM_X86_64"
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
