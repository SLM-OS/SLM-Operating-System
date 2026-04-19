/*
 * tegra_padctl.h — Tegra234 XUSB padctl registers for per-port UTMI PHY
 *                   power-cycling. Phase-3A-Tier-1 of #266/#309/#334.
 *
 * This file covers ONLY the subset of padctl registers needed to
 * re-power USB 2.0 pads post-kexec. A full padctl driver would also
 * handle SS-pad calibration, HSIC, and lane muxing — those are out of
 * scope here because Linux's boot-time init already programmed them
 * and we inherit the configuration across kexec.
 *
 * Register offsets + bit positions come from Linux's padctl driver
 * (~/slmos-ref/linux/linux-phy-tegra186-xusb.c). The Tegra186 file covers
 * Tegra186/194/234 — Orin (Tegra234) uses the same register layout.
 * Every define below cites the upstream line number so future audits
 * against a newer Linux snapshot can cross-check.
 *
 * Padctl MMIO base is 0x03520000, set in kernel/include/platform.h
 * as TEGRA_XUSB_PADCTL_BASE. The base is directly accessible from
 * NS EL2 (confirmed by the CBB probe in jetson-usb-networking-plan.md
 * §Phase 0).
 */

#ifndef TEGRA_PADCTL_H
#define TEGRA_PADCTL_H

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Register offsets (relative to TEGRA_XUSB_PADCTL_BASE = 0x03520000)          */
/* All line numbers reference ~/slmos-ref/linux/linux-phy-tegra186-xusb.c.     */
/* -------------------------------------------------------------------------- */

/* Per-port USB2 OTG pad control. Stride 0x40 per pad; Tegra234 has 4. */
/* linux-phy-tegra186-xusb.c:71 */
static inline uint32_t XUSB_PADCTL_USB2_OTG_PADX_CTL0(unsigned pad)
{
    return 0x88U + pad * 0x40U;
}
/* linux-phy-tegra186-xusb.c:79 */
static inline uint32_t XUSB_PADCTL_USB2_OTG_PADX_CTL1(unsigned pad)
{
    return 0x8cU + pad * 0x40U;
}

/* Shared USB2 bias pad control. One instance for all USB2 pads. */
/* linux-phy-tegra186-xusb.c:84, 89 */
#define XUSB_PADCTL_USB2_BIAS_PAD_CTL0          0x284U
#define XUSB_PADCTL_USB2_BIAS_PAD_CTL1          0x288U

/* -------------------------------------------------------------------------- */
/* Bit defines                                                                 */
/* -------------------------------------------------------------------------- */

/* USB2_OTG_PADX_CTL0 (linux-phy-tegra186-xusb.c:74-77) */
#define USB2_OTG_PD                             (1U << 26)
#define USB2_OTG_PD2                            (1U << 27)
#define USB2_OTG_PD_ZI                          (1U << 29)

/* USB2_OTG_PADX_CTL1 (linux-phy-tegra186-xusb.c:80) */
#define USB2_OTG_PD_DR                          (1U << 2)

/* USB2_BIAS_PAD_CTL0 (linux-phy-tegra186-xusb.c:85) */
#define BIAS_PAD_PD                             (1U << 11)

/* USB2_BIAS_PAD_CTL1 (linux-phy-tegra186-xusb.c:92) */
#define USB2_PD_TRK                             (1U << 26)

/* -------------------------------------------------------------------------- */
/* Port-index mapping                                                          */
/*                                                                             */
/* Tegra234 xHCI advertises 8 ports: indices 0-3 are the USB 3 lanes and       */
/* 4-7 are the USB 2 lanes (standard xHCI numbering when both protocols are    */
/* present). Padctl USB2 pads are 0-3. Given an xHCI PORTSC index in 4..7,     */
/* the padctl pad index is simply (portsc_index - 4).                          */
/* -------------------------------------------------------------------------- */

#define TEGRA_XUSB_NUM_USB2_PADS                4U
#define TEGRA_XUSB_USB2_PORTSC_BASE             4U  /* xHCI PORTSC[4] == USB2 pad 0 */

static inline bool tegra_padctl_portsc_is_usb2(uint8_t portsc_index)
{
    return portsc_index >= TEGRA_XUSB_USB2_PORTSC_BASE &&
           portsc_index <  TEGRA_XUSB_USB2_PORTSC_BASE + TEGRA_XUSB_NUM_USB2_PADS;
}

static inline unsigned tegra_padctl_portsc_to_pad(uint8_t portsc_index)
{
    return (unsigned)(portsc_index - TEGRA_XUSB_USB2_PORTSC_BASE);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

/*
 * Re-power the UTMI PHY for the given padctl USB2 pad index (0..3).
 *
 * Sequence mirrors Linux's tegra186_utmi_pad_power_{down,up} paths
 * (linux-phy-tegra186-xusb.c:709-764):
 *   1. Set USB2_OTG_PD in CTL0 — power down the PHY's HS transceiver.
 *   2. Set USB2_OTG_PD_DR in CTL1 — power down the Dp/Dn driver.
 *   3. Short delay (Linux uses 2 µs; we use 100 µs to be generous
 *      post-kexec when PHY state is unknown).
 *   4. Clear USB2_OTG_PD in CTL0.
 *   5. Clear USB2_OTG_PD_DR in CTL1.
 *   6. Second 100 µs delay for bias-cell / squelch detect to re-lock.
 *
 * We deliberately do NOT touch the shared BIAS_PAD registers
 * (XUSB_PADCTL_USB2_BIAS_PAD_CTL0/1). Linux programmed them at boot
 * with silicon-specific calibration values (HS_SQUELCH_LEVEL,
 * HS_DISCON_LEVEL from fuse data); re-running that path would need
 * access to the fuse block. Leaving the bias settings alone assumes
 * the bias pad is still powered (which it is on a working Linux boot
 * — confirmed by reading BIAS_PAD_CTL0 in a prior session).
 *
 * Returns true on completion; false if `pad` is out of range. The
 * function always returns before observing any effect — callers are
 * expected to peek PORTSC themselves to confirm behaviour.
 */
bool tegra_padctl_usb2_phy_cycle(unsigned pad);

#endif /* TEGRA_PADCTL_H */
