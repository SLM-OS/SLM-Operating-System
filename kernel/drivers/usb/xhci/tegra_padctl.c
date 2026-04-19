/*
 * tegra_padctl.c — Per-port UTMI PHY power-cycling for Tegra234.
 *
 * See tegra_padctl.h for the full reference and citations. This file
 * implements only the power-cycle primitive; callers (xhci.c, shell
 * commands) decide when to invoke it.
 */

#include "tegra_padctl.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "platform.h"
#include "timer.h"
#include "debug.h"

#include <stdint.h>
#include <stdbool.h>

static inline volatile uint32_t *padctl_reg(uint32_t offset)
{
    return (volatile uint32_t *)((uintptr_t)TEGRA_XUSB_PADCTL_BASE + offset);
}

static inline uint32_t padctl_r32(uint32_t offset)
{
    return *padctl_reg(offset);
}

static inline void padctl_w32(uint32_t offset, uint32_t value)
{
    *padctl_reg(offset) = value;
}

/* Spin-delay for the given number of microseconds using CNTPCT_EL0.
 * timer_get_frequency() returns the CNTFRQ; on Tegra234 that's 19.2 MHz,
 * well under one tick per microsecond, so even ~100 µs is coarse enough
 * that the extra polling overhead is negligible. */
static void padctl_udelay(uint32_t usec)
{
    uint64_t freq  = timer_get_frequency();
    uint64_t start = timer_get_count();
    uint64_t ticks = ((uint64_t)usec * freq + 999999u) / 1000000u;
    while (timer_get_count() - start < ticks) { }
}

bool tegra_padctl_usb2_phy_cycle(unsigned pad)
{
    if (pad >= TEGRA_XUSB_NUM_USB2_PADS)
        return false;

    uint32_t ctl0_off = XUSB_PADCTL_USB2_OTG_PADX_CTL0(pad);
    uint32_t ctl1_off = XUSB_PADCTL_USB2_OTG_PADX_CTL1(pad);

    uint32_t ctl0_before = padctl_r32(ctl0_off);
    uint32_t ctl1_before = padctl_r32(ctl1_off);

    /* Power down: set PD in CTL0 and PD_DR in CTL1. */
    padctl_w32(ctl0_off, ctl0_before | USB2_OTG_PD);
    padctl_w32(ctl1_off, ctl1_before | USB2_OTG_PD_DR);

    /* Linux uses a 2 µs settle; we're post-kexec with unknown PHY
     * state, so 100 µs leaves plenty of slack. */
    padctl_udelay(100);

    uint32_t ctl0_down = padctl_r32(ctl0_off);
    uint32_t ctl1_down = padctl_r32(ctl1_off);

    /* Power up: clear PD in CTL0 and PD_DR in CTL1. */
    padctl_w32(ctl0_off, ctl0_down & ~USB2_OTG_PD);
    padctl_w32(ctl1_off, ctl1_down & ~USB2_OTG_PD_DR);

    /* Wait for the squelch detector and bias to settle before the
     * Falcon is expected to re-evaluate connect detection. */
    padctl_udelay(200);

    uint32_t ctl0_after = padctl_r32(ctl0_off);
    uint32_t ctl1_after = padctl_r32(ctl1_off);

    INFO("padctl: usb2 pad[%u] phy-cycle "
         "ctl0: 0x%08x → 0x%08x → 0x%08x; "
         "ctl1: 0x%08x → 0x%08x → 0x%08x",
         pad,
         (unsigned)ctl0_before, (unsigned)ctl0_down, (unsigned)ctl0_after,
         (unsigned)ctl1_before, (unsigned)ctl1_down, (unsigned)ctl1_after);
    return true;
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

/* Non-Jetson platforms: stub so the driver compiles on every target
 * (tests live in kernel/tests and import the header directly). */
bool tegra_padctl_usb2_phy_cycle(unsigned pad)
{
    (void)pad;
    return false;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
