/*
 * xusb_t234_cold.c - Bring XUSB up from cold after a UEFI-direct boot (#1084)
 *
 * The kexec path inherits a running controller from Linux. After a
 * UEFI-direct boot the firmware's XhciControllerDxe has halted the
 * controller, powered the pads down and power-gated XUSBA/XUSBC in its
 * ExitBootServices handler, so SLM-OS has to repeat the bring-up
 * itself. The sequence follows NVIDIA's UEFI driver, which brings the
 * same controller up without Linux:
 *
 *   ~/slmos-ref/tegra-l4t/edk2-nvidia-usb/XhciControllerDxe__XhciControllerDxe.c
 *     DeviceDiscoveryNotify(DriverBindingStart)
 *   ~/slmos-ref/tegra-l4t/edk2-nvidia-usb/UsbPadCtlDxe__UsbPadCtlTegra234.c
 *     InitUsbHw234
 *
 *   1. BPMP: deassert the padctl reset; enable the fuse, PLL and XUSB
 *      clocks listed by the DT padctl and xhci nodes.
 *   2. BPMP: power XUSBA (SuperSpeed partition) and XUSBC (host).
 *   3. Pads: fuse calibration, bias pad + tracking, USB2 pads for the
 *      host ports, VBUS/ID override.
 *   4. FPCI: BAR0/BAR2 decode, AXI config, memory space + bus master.
 *   5. Wait for USBSTS.CNR to clear.
 *
 * No falcon firmware is loaded here. Tegra234 boots the XUSB falcon
 * from IFR firmware (Linux's tegra234_soc has no .firmware and calls
 * tegra_xusb_init_ifr_firmware, which only waits for CNR; the UEFI
 * driver does the same), so CNR clearing after power-up is the signal
 * that the controller firmware is running.
 *
 * SuperSpeed lanes stay clamped: their UPHY PLL is a BPMP service the
 * kexec path never needed, and the CDC-ECM adapter on the lab topology
 * enumerates at high speed behind the onboard hub's USB2 half (usb2-1).
 * The USB-C port (usb2-0 / usb3-1) is a device-mode port and is left
 * alone.
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "xusb_t234_cold.h"
#include "xusb_t234_padctl.h"
#include "xhci_tegra.h"
#include "xhci_regs.h"
#include "bpmp.h"
#include "tegra234_clocks.h"
#include "debug.h"
#include "timer.h"
#include "spinlock.h"   /* dsb() */

#include <stdint.h>
#include <stdbool.h>

/* Clocks the DT xhci (usb@3610000) and padctl nodes list, in the
 * order Linux's tegra_xusb_clk_enable takes them, plus the fuse clock
 * (the calibration words are read from FUSE MMIO). CLK_M is always on. */
static const struct {
    uint32_t id;
    const char *name;
} cold_clocks[] = {
    { TEGRA234_CLK_FUSE,            "fuse" },
    { TEGRA234_CLK_PLLE,            "pll_e" },
    { TEGRA234_CLK_UTMIP_PLL,       "utmip_pll" },
    { TEGRA234_CLK_USB2_TRK,        "usb2_trk" },
    { TEGRA234_CLK_XUSB_CORE_HOST,  "xusb_core_host" },
    { TEGRA234_CLK_XUSB_CORE_SS,    "xusb_core_ss" },
    { TEGRA234_CLK_XUSB_FALCON,     "xusb_falcon" },
    { TEGRA234_CLK_XUSB_FS,         "xusb_fs" },
    { TEGRA234_CLK_XUSB_SS,         "xusb_ss" },
};

static int cold_last_step;
static uint32_t cold_last_usbsts;

static inline uint32_t mmio_r32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void mmio_w32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t pad_r(uint32_t off)
{
    return mmio_r32(TEGRA_XUSB_PADCTL_BASE + off);
}

static inline void pad_w(uint32_t off, uint32_t val)
{
    mmio_w32(TEGRA_XUSB_PADCTL_BASE + off, val);
}

static inline uint32_t fpci_rd(uint32_t off)
{
    return mmio_r32(TEGRA_XHCI_FPCI_BASE + off);
}

static inline void fpci_wr(uint32_t off, uint32_t val)
{
    mmio_w32(TEGRA_XHCI_FPCI_BASE + off, val);
}

/* Step 1: padctl reset + clocks. */
static int cold_clocks_on(void)
{
    int rc = bpmp_reset_deassert(TEGRA234_RESET_XUSB_PADCTL);
    if (rc != 0) {
        WARN("xusb-cold: padctl reset deassert rc=%d", rc);
        return -1;
    }
    for (unsigned i = 0; i < sizeof(cold_clocks) / sizeof(cold_clocks[0]); i++) {
        rc = bpmp_clk_enable(cold_clocks[i].id);
        if (rc != 0) {
            WARN("xusb-cold: clk %s (%u) enable rc=%d",
                 cold_clocks[i].name, (unsigned)cold_clocks[i].id, rc);
            return -1;
        }
    }
    return 0;
}

/* Step 2: power the SuperSpeed and host partitions. */
static int cold_partitions_on(void)
{
    int rc = bpmp_pg_set_state(TEGRA234_POWER_DOMAIN_XUSBA, true);
    if (rc != 0) {
        WARN("xusb-cold: XUSBA power-on rc=%d", rc);
        return -1;
    }
    rc = bpmp_pg_set_state(TEGRA234_POWER_DOMAIN_XUSBC, true);
    if (rc != 0) {
        WARN("xusb-cold: XUSBC power-on rc=%d", rc);
        return -1;
    }
    return 0;
}

/* Step 3: pads. */
static void cold_pads_init(void)
{
    struct xusb_t234_calib cal;
    uint32_t calib0 = mmio_r32(T234_FUSE_MMIO_BASE + T234_FUSE_USB_CALIB_0);
    uint32_t calib_ext = mmio_r32(T234_FUSE_MMIO_BASE + T234_FUSE_USB_CALIB_EXT_0);
    xusb_t234_decode_calib(calib0, calib_ext, &cal);
    INFO("xusb-cold: fuse USB_CALIB=0x%08x EXT=0x%08x squelch=%u rpd=%u",
         (unsigned)calib0, (unsigned)calib_ext,
         (unsigned)cal.hs_squelch, (unsigned)cal.rpd_ctrl);

    /* Bias pad + tracking (InitBiasPad). */
    pad_w(XP_USB2_BIAS_PAD_CTL0,
          xusb_t234_bias_pad_ctl0(pad_r(XP_USB2_BIAS_PAD_CTL0), cal.hs_squelch));
    pad_w(XP_USB2_BIAS_PAD_CTL1,
          xusb_t234_bias_pad_ctl1_timers(pad_r(XP_USB2_BIAS_PAD_CTL1)));
    timer_busy_wait_us(1);
    pad_w(XP_USB2_BIAS_PAD_CTL1, pad_r(XP_USB2_BIAS_PAD_CTL1) & ~XP_PD_TRK);
    timer_busy_wait_us(200);
    pad_w(XP_USB2_BIAS_PAD_CTL1, pad_r(XP_USB2_BIAS_PAD_CTL1) | XP_TRK_COMPLETED);
    pad_w(XP_USB2_BIAS_PAD_CTL2, pad_r(XP_USB2_BIAS_PAD_CTL2) &
          ~(XP_TRK_HW_MODE | XP_CYA_TRK_CODE_UPDATE_ON_IDLE));

    /* USB2 pads of the host ports (InitUsb2PadX). */
    for (uint32_t i = 0; i < T234_XUSB_NUM_USB2_PADS; i++) {
        if ((XUSB_T234_COLD_USB2_PORT_MASK & (1u << i)) == 0)
            continue;
        pad_w(XP_USB2_PAD_MUX, xusb_t234_pad_mux(pad_r(XP_USB2_PAD_MUX), i));
        pad_w(XP_USB2_PORT_CAP,
              xusb_t234_port_cap_host(pad_r(XP_USB2_PORT_CAP), i));
        pad_w(XP_USB2_OC_MAP,
              xusb_t234_oc_map_disabled(pad_r(XP_USB2_OC_MAP), i));
        pad_w(XP_USB2_OTG_PAD_CTL0(i),
              xusb_t234_otg_pad_ctl0(pad_r(XP_USB2_OTG_PAD_CTL0(i)),
                                     cal.hs_curr_level[i]));
        pad_w(XP_USB2_OTG_PAD_CTL1(i),
              xusb_t234_otg_pad_ctl1(pad_r(XP_USB2_OTG_PAD_CTL1(i)),
                                     cal.hs_term_range_adj[i], cal.rpd_ctrl));
        pad_w(XP_USB2_BATTERY_CHRG_OTGPAD_CTL1(i),
              xusb_t234_battery_chrg_ctl1(
                  pad_r(XP_USB2_BATTERY_CHRG_OTGPAD_CTL1(i))));
    }

    /* VBUS/ID: report host mode, then clear the latched changes
     * (VbusIdOverride). VBUS itself comes from always-on regulators on
     * the developer kit (vdd_5v0_sys, vdd_1v1_hub). */
    pad_w(XP_USB2_VBUS_ID, xusb_t234_vbus_id_host(pad_r(XP_USB2_VBUS_ID)));
    pad_w(XP_USB2_VBUS_ID, pad_r(XP_USB2_VBUS_ID) |
          XP_IDDIG_STATUS_CHANGE | XP_VBUS_VALID_ST_CHANGE);
    dsb(sy);
}

/* Step 4: FPCI wrapper (XHCI DriverBindingStart). */
static void cold_fpci_config(void)
{
    uint32_t v = fpci_rd(XUSB_CFG_4);
    v &= ~(XUSB_BASE_ADDR_MASK << XUSB_BASE_ADDR_SHIFT);
    v |= (uint32_t)TEGRA_XHCI_HCD_BASE & (XUSB_BASE_ADDR_MASK << XUSB_BASE_ADDR_SHIFT);
    fpci_wr(XUSB_CFG_4, v);
    timer_busy_wait_us(200);

    v = fpci_rd(XUSB_CFG_7);
    v &= ~(XUSB_BASE2_ADDR_MASK << XUSB_BASE2_ADDR_SHIFT);
    v |= (uint32_t)TEGRA_XHCI_BAR2_BASE & (XUSB_BASE2_ADDR_MASK << XUSB_BASE2_ADDR_SHIFT);
    fpci_wr(XUSB_CFG_7, v);
    timer_busy_wait_us(200);

    fpci_wr(XUSB_CFG_AXI_CFG, 0x5);
    timer_busy_wait_us(100);

    fpci_wr(XUSB_CFG_1, fpci_rd(XUSB_CFG_1) | XUSB_MEM_SPACE_EN | XUSB_BUS_MASTER_EN);
    dsb(sy);
}

/* Step 5: controller ready. */
static int cold_wait_cnr(void)
{
    uintptr_t hcd = TEGRA_XHCI_HCD_BASE;
    uint32_t caplen = mmio_r32(hcd) & 0xFF;
    uint32_t sts = 0;
    for (int i = 0; i < 200; i++) {
        sts = mmio_r32(hcd + caplen + XHCI_OP_USBSTS);
        if ((sts & XHCI_STS_CNR) == 0)
            break;
        timer_busy_wait_us(1000);
    }
    cold_last_usbsts = sts;
    if (sts & (XHCI_STS_CNR | XHCI_STS_HCE)) {
        WARN("xusb-cold: controller not ready, USBSTS=0x%08x", (unsigned)sts);
        return -1;
    }
    INFO("xusb-cold: controller ready, CAPLENGTH=0x%02x USBSTS=0x%08x",
         (unsigned)caplen, (unsigned)sts);
    return 0;
}

int xusb_t234_cold_step(int step)
{
    int rc = 0;
    switch (step) {
    case 1: rc = cold_clocks_on(); break;
    case 2: rc = cold_partitions_on(); break;
    case 3: cold_pads_init(); break;
    case 4: cold_fpci_config(); break;
    case 5: rc = cold_wait_cnr(); break;
    default: return -1;
    }
    if (rc == 0)
        cold_last_step = step;
    return rc;
}

int xusb_t234_cold_bringup(void)
{
    if (!bpmp_is_available()) {
        WARN("xusb-cold: BPMP unavailable — cannot power XUSB");
        return -1;
    }
    for (int step = cold_last_step + 1; step <= XUSB_T234_COLD_STEPS; step++) {
        /* Announce each step before it runs: an access to an unpowered
         * block stops the board in EL3, and the last line on the
         * console then names the step that did it. */
        INFO("xusb-cold: step %d/%d", step, XUSB_T234_COLD_STEPS);
        if (xusb_t234_cold_step(step) != 0) {
            WARN("xusb-cold: step %d failed", step);
            return -1;
        }
    }
    return 0;
}

int xusb_t234_cold_progress(void)
{
    return cold_last_step;
}

uint32_t xusb_t234_cold_last_usbsts(void)
{
    return cold_last_usbsts;
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

/* Other platforms have no XUSB; keep the translation unit non-empty. */
typedef int xusb_t234_cold_unused_t;

#endif /* PLATFORM_JETSON_ORIN_NANO */
