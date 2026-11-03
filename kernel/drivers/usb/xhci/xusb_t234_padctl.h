/*
 * xusb_t234_padctl.h - Tegra234 XUSB pad controller: register map and
 * pure value helpers for the UEFI-direct cold bring-up (#1084).
 *
 * After a kexec from Linux the pads are already programmed; after a
 * UEFI-direct boot the firmware's XhciControllerDxe has powered them
 * down again in its ExitBootServices handler. The cold bring-up in
 * xusb_t234_cold.c reprograms them with the sequence NVIDIA's own UEFI
 * driver uses on silicon:
 *
 *   ~/slmos-ref/tegra-l4t/edk2-nvidia-usb/UsbPadCtlDxe__UsbPadCtlTegra234.c
 *     InitUsbHw234 -> ReadFuseCalibration, InitBiasPad, InitUsb2PadX,
 *                     VbusIdOverride, InitUsb3PadX
 *
 * Linux's phy-tegra186-xusb.c (~/slmos-ref/linux/linux-phy-tegra186-xusb.c)
 * programs the same fields; the values these helpers produce match the
 * registers Linux leaves on jetson-nano-2 (see the regression tests in
 * kernel/tests/test_xhci_tegra.c).
 *
 * Everything in this header is platform-neutral arithmetic so the
 * QEMU test suite can pin it; the MMIO accesses live in
 * xusb_t234_cold.c.
 */

#ifndef XUSB_T234_PADCTL_H
#define XUSB_T234_PADCTL_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Fuse calibration words (relative to the FUSE block at 0x03810000)  */
/* ------------------------------------------------------------------ */

#define T234_FUSE_USB_CALIB_0           0x1F0U
#define T234_FUSE_USB_CALIB_EXT_0       0x350U

#define T234_XUSB_NUM_USB2_PADS         4U

struct xusb_t234_calib {
    uint8_t hs_curr_level[T234_XUSB_NUM_USB2_PADS];   /* 6 bits each */
    uint8_t hs_term_range_adj[T234_XUSB_NUM_USB2_PADS]; /* 4 bits each */
    uint8_t hs_squelch;                                /* 3 bits */
    uint8_t rpd_ctrl;                                  /* 5 bits */
};

/*
 * Decode FUSE_USB_CALIB_0 / FUSE_USB_CALIB_EXT_0 exactly as
 * ReadFuseCalibration does: pad 0's HS_CURR_LEVEL is bits [5:0] and pad
 * N>0's is at 11 + 6*(N-1); pad 0's TERM_RANGE_ADJ comes from CALIB_0
 * bits [10:7] while pads N>0 come from CALIB_EXT bits 5 + 4*(N-1).
 */
static inline void xusb_t234_decode_calib(uint32_t calib0, uint32_t calib_ext,
                                          struct xusb_t234_calib *out)
{
    for (uint32_t i = 0; i < T234_XUSB_NUM_USB2_PADS; i++) {
        uint32_t shift = (i == 0) ? 0 : 11 + (i - 1) * 6;
        out->hs_curr_level[i] = (uint8_t)((calib0 >> shift) & 0x3F);
        if (i == 0)
            out->hs_term_range_adj[i] = (uint8_t)((calib0 >> 7) & 0xF);
        else
            out->hs_term_range_adj[i] =
                (uint8_t)((calib_ext >> (5 + (i - 1) * 4)) & 0xF);
    }
    out->hs_squelch = (uint8_t)((calib0 >> 29) & 0x7);
    out->rpd_ctrl   = (uint8_t)(calib_ext & 0x1F);
}

/* ------------------------------------------------------------------ */
/* Pad controller registers (relative to TEGRA_XUSB_PADCTL_BASE)      */
/* ------------------------------------------------------------------ */

#define XP_USB2_PAD_MUX                 0x004U
#define XP_USB2_PORT_CAP                0x008U
#define XP_SS_PORT_CAP                  0x00CU
#define XP_USB2_OC_MAP                  0x010U
#define XP_SS_OC_MAP                    0x014U
#define XP_VBUS_OC_MAP                  0x018U
#define XP_OC_DET                       0x01CU
#define XP_ELPG_PROGRAM_0               0x020U
#define XP_ELPG_PROGRAM_1               0x024U
#define XP_USB2_BATTERY_CHRG_OTGPAD_CTL1(i) (0x084U + (i) * 0x40U)
#define XP_USB2_OTG_PAD_CTL0(i)         (0x088U + (i) * 0x40U)
#define XP_USB2_OTG_PAD_CTL1(i)         (0x08CU + (i) * 0x40U)
#define XP_USB2_BIAS_PAD_CTL0           0x284U
#define XP_USB2_BIAS_PAD_CTL1           0x288U
#define XP_USB2_BIAS_PAD_CTL2           0x28CU
#define XP_USB2_VBUS_ID                 0x360U

/* USB2_OTG_PADx_CTL_0 */
#define XP_OTG_PD_ZI                    (1u << 29)
#define XP_OTG_PD                       (1u << 26)
#define XP_OTG_TERM_SEL                 (1u << 25)
#define XP_OTG_LS_FSLEW(x)              (((uint32_t)(x) & 0xFu) << 21)
#define XP_OTG_LS_RSLEW(x)              (((uint32_t)(x) & 0xFu) << 17)
#define XP_OTG_HS_CURR_LEVEL(x)         ((uint32_t)(x) & 0x3Fu)

/* USB2_OTG_PADx_CTL_1 */
#define XP_OTG_RPD_CTRL(x)              (((uint32_t)(x) & 0x1Fu) << 26)
#define XP_OTG_TERM_RANGE_ADJ(x)        (((uint32_t)(x) & 0xFu) << 3)
#define XP_OTG_PD_DR                    (1u << 2)

/* USB2_BATTERY_CHRG_OTGPADx_CTL1 */
#define XP_VREG_DIR(x)                  (((uint32_t)(x) & 0x3u) << 11)
#define XP_VREG_DIR_OUT                 XP_VREG_DIR(2)
#define XP_VREG_LEV(x)                  (((uint32_t)(x) & 0x3u) << 7)
#define XP_VREG_LEVEL_2A                3u
#define XP_PD_VREG                      (1u << 6)

/* USB2_BIAS_PAD_CTL0 / 1 / 2 */
#define XP_BIAS_PAD_PD                  (1u << 11)
#define XP_HS_DISCON_LEVEL(x)           (((uint32_t)(x) & 0x7u) << 3)
#define XP_HS_SQUELCH_LEVEL(x)          ((uint32_t)(x) & 0x7u)
#define XP_TRK_COMPLETED                (1u << 31)
#define XP_PD_TRK                       (1u << 26)
#define XP_TRK_DONE_RESET_TIMER(x)      (((uint32_t)(x) & 0x7Fu) << 19)
#define XP_TRK_START_TIMER(x)           (((uint32_t)(x) & 0x7Fu) << 12)
#define XP_TRK_HW_MODE                  (1u << 0)
#define XP_CYA_TRK_CODE_UPDATE_ON_IDLE  (1u << 31)

/* USB2_VBUS_ID */
#define XP_ID_OVERRIDE(x)               (((uint32_t)(x) & 0xFu) << 18)
#define XP_ID_OVERRIDE_GROUNDED         XP_ID_OVERRIDE(0)
#define XP_ID_SOURCE_SELECT(x)          (((uint32_t)(x) & 0x3u) << 16)
#define XP_VBUS_SOURCE_SELECT(x)        (((uint32_t)(x) & 0x3u) << 12)
#define XP_IDDIG_STATUS_CHANGE          (1u << 10)
#define XP_VBUS_VALID_ST_CHANGE         (1u << 4)

/* ELPG_PROGRAM_1: per-SS-port wake/clamp bits. */
#define XP_SSP_ELPG_CLAMP_EN(i)         (1u << ((i) * 3))
#define XP_SSP_ELPG_CLAMP_EN_EARLY(i)   (1u << ((i) * 3 + 1))
#define XP_SSP_ELPG_VCORE_DOWN(i)       (1u << ((i) * 3 + 2))

#define XP_PORT_CAP_HOST                1u
#define XP_PAD_MUX_XUSB                 1u
#define XP_OC_PIN_DISABLED              0xFu

/* ------------------------------------------------------------------ */
/* Value helpers                                                      */
/* ------------------------------------------------------------------ */

/* Set the 2-bit field for `port` (stride `stride` bits) to `val`. */
static inline uint32_t xusb_t234_set_field(uint32_t reg, uint32_t port,
                                           uint32_t stride, uint32_t mask,
                                           uint32_t val)
{
    uint32_t shift = port * stride;
    return (reg & ~(mask << shift)) | ((val & mask) << shift);
}

/* USB2_PAD_MUX: route pad `port` to XUSB (2 bits per port). */
static inline uint32_t xusb_t234_pad_mux(uint32_t reg, uint32_t port)
{
    return xusb_t234_set_field(reg, port, 2, 0x3, XP_PAD_MUX_XUSB);
}

/* USB2_PORT_CAP / SS_PORT_CAP: host mode for `port` (4 bits stride). */
static inline uint32_t xusb_t234_port_cap_host(uint32_t reg, uint32_t port)
{
    return xusb_t234_set_field(reg, port, 4, 0x3, XP_PORT_CAP_HOST);
}

/* USB2_OC_MAP: no over-current pin for `port` (4 bits per port). */
static inline uint32_t xusb_t234_oc_map_disabled(uint32_t reg, uint32_t port)
{
    return xusb_t234_set_field(reg, port, 4, 0xF, XP_OC_PIN_DISABLED);
}

/* USB2_OTG_PADx_CTL_0 for host operation of a powered pad. */
static inline uint32_t xusb_t234_otg_pad_ctl0(uint32_t reg,
                                              uint8_t hs_curr_level)
{
    reg &= ~(XP_OTG_PD | XP_OTG_PD_ZI);
    reg |= XP_OTG_TERM_SEL;
    reg &= ~XP_OTG_HS_CURR_LEVEL(~0u);
    reg |= XP_OTG_HS_CURR_LEVEL(hs_curr_level);
    reg &= ~XP_OTG_LS_FSLEW(~0u);
    reg |= XP_OTG_LS_FSLEW(6);
    reg &= ~XP_OTG_LS_RSLEW(~0u);
    reg |= XP_OTG_LS_RSLEW(6);
    return reg;
}

/* USB2_OTG_PADx_CTL_1: clear PD_DR, program fused terminations. */
static inline uint32_t xusb_t234_otg_pad_ctl1(uint32_t reg,
                                              uint8_t term_range_adj,
                                              uint8_t rpd_ctrl)
{
    reg &= ~XP_OTG_PD_DR;
    reg &= ~XP_OTG_TERM_RANGE_ADJ(~0u);
    reg |= XP_OTG_TERM_RANGE_ADJ(term_range_adj);
    reg &= ~XP_OTG_RPD_CTRL(~0u);
    reg |= XP_OTG_RPD_CTRL(rpd_ctrl);
    return reg;
}

/* USB2_BATTERY_CHRG_OTGPADx_CTL1: VBUS protection, host direction, 2 A. */
static inline uint32_t xusb_t234_battery_chrg_ctl1(uint32_t reg)
{
    reg &= ~XP_PD_VREG;
    reg &= ~XP_VREG_DIR(~0u);
    reg |= XP_VREG_DIR_OUT;
    reg &= ~XP_VREG_LEV(~0u);
    reg |= XP_VREG_LEV(XP_VREG_LEVEL_2A);
    return reg;
}

/* USB2_BIAS_PAD_CTL0: power up the bias pad with the fused squelch. */
static inline uint32_t xusb_t234_bias_pad_ctl0(uint32_t reg, uint8_t squelch)
{
    reg &= ~XP_BIAS_PAD_PD;
    reg &= ~XP_HS_SQUELCH_LEVEL(~0u);
    reg |= XP_HS_SQUELCH_LEVEL(squelch);
    reg &= ~XP_HS_DISCON_LEVEL(~0u);
    reg |= XP_HS_DISCON_LEVEL(7);
    return reg;
}

/* USB2_BIAS_PAD_CTL1: tracking timers (before PD_TRK is cleared). */
static inline uint32_t xusb_t234_bias_pad_ctl1_timers(uint32_t reg)
{
    reg &= ~XP_TRK_START_TIMER(~0u);
    reg |= XP_TRK_START_TIMER(0x1E);
    reg &= ~XP_TRK_DONE_RESET_TIMER(~0u);
    reg |= XP_TRK_DONE_RESET_TIMER(0xA);
    return reg;
}

/* USB2_VBUS_ID: software override, ID grounded (host), clear status. */
static inline uint32_t xusb_t234_vbus_id_host(uint32_t reg)
{
    reg &= ~XP_ID_SOURCE_SELECT(~0u);
    reg |= XP_ID_SOURCE_SELECT(1);
    reg &= ~XP_VBUS_SOURCE_SELECT(~0u);
    reg |= XP_VBUS_SOURCE_SELECT(1);
    reg &= ~XP_ID_OVERRIDE(~0u);
    reg |= XP_ID_OVERRIDE_GROUNDED;
    return reg;
}

/* ELPG_PROGRAM_1: release the wake/clamp latches of SS port `i`. */
static inline uint32_t xusb_t234_elpg1_release(uint32_t reg, uint32_t i)
{
    return reg & ~(XP_SSP_ELPG_CLAMP_EN(i) | XP_SSP_ELPG_CLAMP_EN_EARLY(i) |
                   XP_SSP_ELPG_VCORE_DOWN(i));
}

/*
 * Ports the cold bring-up enables on the Orin Nano developer kit
 * (tegra234-p3768-0000+p3767.dtsi):
 *
 *   usb2-0  USB-C (recovery / OTG)  — left alone: device-mode port
 *   usb2-1  onboard Realtek hub      — host
 *   usb2-2  M.2 Key-E (Bluetooth)    — host
 *   usb3-0  onboard hub SuperSpeed   — not enabled (see xusb_t234_cold.c)
 */
#define XUSB_T234_COLD_USB2_PORT_MASK   ((1u << 1) | (1u << 2))

#endif /* XUSB_T234_PADCTL_H */
