/*
 * xhci.c - SLM-OS xHCI host controller driver (#266 Phase 3A)
 *
 * Phase 3A Step 2 + 3: controller scaffolding, capability probe, and
 * halt/reset sequence. No rings, no ports, no transfers yet — those
 * come in Steps 4-7.
 *
 * Only built on Jetson; every other platform sees this file compiled
 * out by the #if at the top (no usb_hcd registration happens).
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "xhci.h"
#include "xhci_regs.h"
#include "debug.h"
#include "timer.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Base pointers                                                               */
/* -------------------------------------------------------------------------- */

/*
 * TEGRA_XHCI_HCD_BASE is the start of the standard xHCI aperture
 * (Intel-spec). The VMM already maps a 2 MB block at 0x03600000
 * covering both the Tegra FPCI prefix and this standard block.
 */
static volatile uint8_t *xhci_cap_base;
static volatile uint8_t *xhci_op_base;
static volatile uint8_t *xhci_rt_base;
static volatile uint8_t *xhci_db_base;

static struct xhci_caps xhci_caps_cached;
static bool             xhci_live;

/* -------------------------------------------------------------------------- */
/* Register access helpers                                                     */
/* -------------------------------------------------------------------------- */

static uint8_t  r8(const volatile uint8_t *base, uint32_t off)
{
    return *(const volatile uint8_t *)(base + off);
}
static uint16_t r16(const volatile uint8_t *base, uint32_t off)
{
    return *(const volatile uint16_t *)(base + off);
}
static uint32_t r32(const volatile uint8_t *base, uint32_t off)
{
    return *(const volatile uint32_t *)(base + off);
}
static void w32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

/* -------------------------------------------------------------------------- */
/* Halt + reset                                                                */
/* -------------------------------------------------------------------------- */

/*
 * Spin-poll a register bit until (val & mask) == expected or the
 * timeout expires. Uses CNTPCT-based wall-clock so a locked-up
 * controller cannot wedge the boot path.
 *
 * Returns 0 on success, -1 on timeout.
 */
static int poll_reg32(volatile uint8_t *base, uint32_t off,
                      uint32_t mask, uint32_t expected,
                      uint32_t timeout_ms)
{
    uint64_t start = timer_get_count();
    uint64_t freq  = timer_get_frequency();
    uint64_t ticks = (freq * timeout_ms) / 1000;

    for (;;) {
        if ((r32(base, off) & mask) == expected)
            return 0;
        if (timer_get_count() - start > ticks)
            return -1;
    }
}

static int xhci_halt(void)
{
    uint32_t cmd = r32(xhci_op_base, XHCI_OP_USBCMD);
    if (cmd & XHCI_CMD_RUN) {
        /* Clear Run/Stop — controller takes up to 16 ms to halt per
         * xHCI 1.2 §4.1. */
        w32(xhci_op_base, XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RUN);
    }
    if (poll_reg32(xhci_op_base, XHCI_OP_USBSTS,
                   XHCI_STS_HCH, XHCI_STS_HCH, 50) != 0) {
        WARN("xhci: halt timeout — USBSTS=0x%08x",
             (unsigned)r32(xhci_op_base, XHCI_OP_USBSTS));
        return -1;
    }
    return 0;
}

/*
 * Tegra-aware "reset". The standard xHCI HCRST bit does NOT work on
 * Tegra from NS EL2 — empirical result from this session:
 *
 *   Before HCRST: HCIVERSION=0x0120, HCCPARAMS1=0x0180ff05 (live)
 *   After HCRST:  the ENTIRE aperture reads 0xffffffff — the
 *                 controller has entered a Falcon-level reset that
 *                 only BPMP can complete, and BPMP is unreachable.
 *
 * Writing HCRST is therefore a one-way trip to a dead controller
 * post-kexec. We must NOT touch it. Instead:
 *
 *   1. Confirm the controller is already halted by Linux (HCH=1).
 *      Everything we'll do in Step 4 (DCBAAP / CRCR / interrupter
 *      setup) assumes a halted controller, and Linux's runtime-PM
 *      dance leaves exactly that state.
 *   2. Later steps will clear Linux's ring pointers by writing the
 *      relevant registers directly. That doesn't require HCRST.
 *
 * Tracked as #282 — if Step 4+ finds Linux's halted state is too
 * dirty to build on, we'll need a wrapper-level reset (likely via
 * the pre-kexec BPMP debugfs path rather than from SLM-OS itself).
 *
 * Returns 0 on success, negative if the controller isn't halted
 * (which would be surprising given the kexec helper's state).
 */
static int xhci_reset(void)
{
    uint32_t cmd = r32(xhci_op_base, XHCI_OP_USBCMD);
    uint32_t sts = r32(xhci_op_base, XHCI_OP_USBSTS);
    INFO("xhci: pre-reset USBCMD=0x%08x USBSTS=0x%08x", (unsigned)cmd, (unsigned)sts);

    if (!(sts & XHCI_STS_HCH)) {
        WARN("xhci: controller not halted (USBSTS=0x%08x) — "
             "the kexec helper should have left HCH set", (unsigned)sts);
        return -1;
    }
    INFO("xhci: skipping HCRST on Tegra (Falcon-reset is BPMP-only); "
         "using Linux's halted state as the init baseline");
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Capability parse                                                            */
/* -------------------------------------------------------------------------- */

static void xhci_parse_caps(struct xhci_caps *c)
{
    c->cap_length  = r8 (xhci_cap_base, XHCI_CAP_CAPLENGTH);
    c->hci_version = r16(xhci_cap_base, XHCI_CAP_HCIVERSION);
    c->hcs_params1 = r32(xhci_cap_base, XHCI_CAP_HCSPARAMS1);
    c->hcs_params2 = r32(xhci_cap_base, XHCI_CAP_HCSPARAMS2);
    c->hcs_params3 = r32(xhci_cap_base, XHCI_CAP_HCSPARAMS3);
    c->hcc_params1 = r32(xhci_cap_base, XHCI_CAP_HCCPARAMS1);
    c->db_off      = r32(xhci_cap_base, XHCI_CAP_DBOFF);
    c->rts_off     = r32(xhci_cap_base, XHCI_CAP_RTSOFF);
    c->hcc_params2 = r32(xhci_cap_base, XHCI_CAP_HCCPARAMS2);

    c->max_slots = (uint8_t) XHCI_HCS1_MAX_SLOTS(c->hcs_params1);
    c->max_ports = (uint8_t) XHCI_HCS1_MAX_PORTS(c->hcs_params1);
    c->max_intrs = (uint16_t)XHCI_HCS1_MAX_INTRS(c->hcs_params1);
    c->ac64      = XHCI_HCC1_AC64(c->hcc_params1) != 0;
    c->ctx_64    = XHCI_HCC1_CSZ(c->hcc_params1) != 0;
    c->xecp      = (uint16_t)XHCI_HCC1_XECP(c->hcc_params1);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

int xhci_init(void)
{
    xhci_live = false;

    /* VMM mapping for XHCI MMIO was landed in #266 Phase 0 (commit
     * fb12937). Grab the virtual pointer — identity-mapped on
     * Jetson so the physical address doubles as a VA. */
    xhci_cap_base = (volatile uint8_t *)(uintptr_t)TEGRA_XHCI_HCD_BASE;

    /* Sanity check: if the clocks aren't held, CAPLENGTH reads as
     * 0xFF (low byte of 0xFFFFFFFF). Bail early with a clear
     * message rather than writing to a dead aperture. */
    uint32_t first = r32(xhci_cap_base, 0);
    if (first == 0xFFFFFFFFu) {
        WARN("xhci: aperture reads 0xffffffff — clocks gated "
             "(did slmos-kexec run without --no-usb-hold?)");
        return -1;
    }

    uint8_t caplen = (uint8_t)(first & 0xFF);
    if (caplen < 0x20 || caplen > 0x80) {
        WARN("xhci: implausible CAPLENGTH 0x%02x — bail", caplen);
        return -1;
    }

    xhci_op_base = xhci_cap_base + caplen;
    xhci_parse_caps(&xhci_caps_cached);

    xhci_rt_base = xhci_cap_base + xhci_caps_cached.rts_off;
    xhci_db_base = xhci_cap_base + xhci_caps_cached.db_off;

    INFO("xhci: HCI v%x.%x, %u slots, %u ports, %u intrs, %s addr, %u-byte ctx",
         (xhci_caps_cached.hci_version >> 8) & 0xFF,
         xhci_caps_cached.hci_version & 0xFF,
         xhci_caps_cached.max_slots,
         xhci_caps_cached.max_ports,
         xhci_caps_cached.max_intrs,
         xhci_caps_cached.ac64 ? "64-bit" : "32-bit",
         xhci_caps_cached.ctx_64 ? 64 : 32);

    if (xhci_halt() != 0)
        return -1;
    if (xhci_reset() != 0)
        return -1;

    xhci_live = true;
    INFO("xhci: controller ready for Step 4 (rings)");
    return 0;
}

bool xhci_dump_info(void)
{
    if (!xhci_live)
        return false;

    const struct xhci_caps *c = &xhci_caps_cached;
    uart_printf("xhci @ %p (op @ +0x%02x, rt @ +0x%x, db @ +0x%x)\r\n",
                xhci_cap_base, (unsigned)c->cap_length,
                (unsigned)c->rts_off, (unsigned)c->db_off);
    uart_printf("  HCIVERSION %x.%x  HCCPARAMS1 0x%08x  HCCPARAMS2 0x%08x\r\n",
                (c->hci_version >> 8) & 0xFF, c->hci_version & 0xFF,
                (unsigned)c->hcc_params1, (unsigned)c->hcc_params2);
    uart_printf("  HCSPARAMS1 0x%08x  HCSPARAMS2 0x%08x  HCSPARAMS3 0x%08x\r\n",
                (unsigned)c->hcs_params1, (unsigned)c->hcs_params2,
                (unsigned)c->hcs_params3);
    uart_printf("  %u slots, %u ports, %u interrupters, %s addressing, %u-byte contexts\r\n",
                c->max_slots, c->max_ports, c->max_intrs,
                c->ac64 ? "64-bit" : "32-bit",
                c->ctx_64 ? 64 : 32);
    uart_printf("  USBCMD 0x%08x  USBSTS 0x%08x  PAGESIZE 0x%08x\r\n",
                (unsigned)r32(xhci_op_base, XHCI_OP_USBCMD),
                (unsigned)r32(xhci_op_base, XHCI_OP_USBSTS),
                (unsigned)r32(xhci_op_base, XHCI_OP_PAGESIZE));
    return true;
}

void xhci_get_caps(struct xhci_caps *out)
{
    if (out == NULL)
        return;
    if (xhci_live)
        memcpy(out, &xhci_caps_cached, sizeof(*out));
    else
        memset(out, 0, sizeof(*out));
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

/*
 * Non-Jetson stubs. The Phase-3A XHCI driver is Jetson-only; other
 * platforms get a driver that reports "not live" so any shared code
 * that queries xhci_dump_info() compiles and behaves consistently.
 */

#include "xhci.h"
#include <string.h>

int  xhci_init(void)                     { return 0; }
bool xhci_dump_info(void)                { return false; }
void xhci_get_caps(struct xhci_caps *out) { if (out) memset(out, 0, sizeof(*out)); }

#endif /* PLATFORM_JETSON_ORIN_NANO */
