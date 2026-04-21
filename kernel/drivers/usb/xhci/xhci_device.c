/*
 * xhci_device.c - Phase 3A Step 5 (port scan + slot / address management)
 *                 and Step 7a (endpoint_configure) of #266.
 *
 * Implements:
 *   - xhci_hcd_port_status / xhci_hcd_port_reset  (Step 5a)
 *   - xhci_hcd_device_open / xhci_hcd_device_close (Step 5b)
 *   - xhci_hcd_endpoint_configure                 (Step 7a)
 *
 * Port and slot ABI references:
 *   xHCI 1.2 §4.19   Port reset sequence + PORTSC semantics
 *   xHCI 1.2 §4.6.3  ENABLE_SLOT
 *   xHCI 1.2 §4.6.5  ADDRESS_DEVICE
 *   xHCI 1.2 §4.6.6  CONFIGURE_ENDPOINT
 *   xHCI 1.2 §6.2    Context layouts (see xhci_ctx.h)
 *
 * Scope:
 *   Single root device on port 0, USB 2.0 high/full/low speed only.
 *   usb_core guarantees single-threaded access, so there is no
 *   locking here beyond what the MMIO write ordering already
 *   provides.
 */

#include "xhci_internal.h"
#include "xhci_regs.h"
#include "xhci_ring.h"
#include "xhci_trb.h"
#include "xhci_ctx.h"
#include "xhci_attach.h"
#include "usb.h"
#include "ncmem.h"
#include "debug.h"
#include "timer.h"
#include "spinlock.h"   /* dsb() */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if defined(PLATFORM_JETSON_ORIN_NANO)

/* -------------------------------------------------------------------------- */
/* Per-device state pool. Minimal hub support needs two live devices at once:
 * the upstream hub and one downstream child. */
/* -------------------------------------------------------------------------- */

static struct xhci_device xhci_dev_pool[2];

/* Scratch transfer-ring storage. Each ring occupies one entry here so
 * device_close has a stable teardown path without dynamic allocation.
 * Index 0 is EP0's ring, 1+ are allocated lazily by endpoint_configure. */
#define XHCI_DEV_MAX_RINGS          8
static struct xhci_ring xhci_ring_pool[XHCI_DEV_MAX_RINGS];
static bool             xhci_ring_in_use[XHCI_DEV_MAX_RINGS];

static struct xhci_ring *xhci_ring_pool_alloc(void)
{
    for (unsigned i = 0; i < XHCI_DEV_MAX_RINGS; i++) {
        if (!xhci_ring_in_use[i]) {
            xhci_ring_in_use[i] = true;
            memset(&xhci_ring_pool[i], 0, sizeof(xhci_ring_pool[i]));
            return &xhci_ring_pool[i];
        }
    }
    return NULL;
}

static void xhci_ring_pool_free(struct xhci_ring *r)
{
    for (unsigned i = 0; i < XHCI_DEV_MAX_RINGS; i++) {
        if (&xhci_ring_pool[i] == r) {
            xhci_ring_in_use[i] = false;
            return;
        }
    }
}

static struct xhci_device *xhci_dev_pool_alloc(void)
{
    for (unsigned i = 0; i < sizeof(xhci_dev_pool) / sizeof(xhci_dev_pool[0]); i++) {
        if (!xhci_dev_pool[i].valid) {
            memset(&xhci_dev_pool[i], 0, sizeof(xhci_dev_pool[i]));
            xhci_dev_pool[i].valid = true;
            return &xhci_dev_pool[i];
        }
    }
    return NULL;
}

static void xhci_dev_pool_free(struct xhci_device *d)
{
    if (d == NULL) return;
    for (unsigned dci = 0; dci < 32; dci++) {
        if (d->ep_rings[dci] != NULL) {
            xhci_ring_pool_free(d->ep_rings[dci]);
            d->ep_rings[dci] = NULL;
        }
    }
    d->valid = false;
}

/* -------------------------------------------------------------------------- */
/* PORTSC decode (Step 5a) — pure logic, unit-testable                         */
/* -------------------------------------------------------------------------- */

bool xhci_decode_portsc(uint32_t portsc,
                        bool *connected,
                        enum usb_speed *speed)
{
    bool c = (portsc & XHCI_PORTSC_CCS) != 0;
    uint32_t sp = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;

    enum usb_speed s = USB_SPEED_UNKNOWN;
    bool known = true;
    if (!c) {
        s = USB_SPEED_UNKNOWN;
    } else {
        switch (sp) {
        case XHCI_PORTSC_SPEED_FULL:  s = USB_SPEED_FULL;  break;
        case XHCI_PORTSC_SPEED_LOW:   s = USB_SPEED_LOW;   break;
        case XHCI_PORTSC_SPEED_HIGH:  s = USB_SPEED_HIGH;  break;
        case XHCI_PORTSC_SPEED_SUPER: s = USB_SPEED_SUPER; break;
        default:
            /* Unknown / reserved — surface as UNKNOWN, not a hard
             * failure, so a flaky reset still lets the caller log and
             * retry. */
            s = USB_SPEED_UNKNOWN;
            known = false;
            break;
        }
    }

    if (connected) *connected = c;
    if (speed)     *speed     = s;
    return known;
}

/*
 * Phase 3A's usb_core has a single-root-device model and always calls
 * port_status(0) / port_reset(0). Tegra234's xHCI exposes 8 ports
 * that mix USB 3.x SuperSpeed with USB 2.0 high/full/low, and the
 * physical dongle can be on any of them. We scan for the first USB 2
 * port with a connected device and remember its 0-based PORTSC
 * index, so the driver-internal "port 0" routes to the right PORTSC
 * register on port_reset as well.
 */
static uint8_t xhci_active_port = 0xFF;  /* 0xFF = not yet located */
static uint32_t xhci_active_portsc = 0;
static uint32_t xhci_stale_portsc_initial = 0;
static enum usb_speed xhci_prereset_speed = USB_SPEED_UNKNOWN;
static bool xhci_skip_next_port_reset = false;

/*
 * Hot-plug state: STALE at boot (assume pre-kexec stale device) →
 * WAIT_RECONNECT once CCS drops → FRESH once CCS rises again.
 * Transitions live in xhci_attach.h so they can be unit-tested.
 * Related: issue #309.
 */
static enum xhci_attach_phase xhci_attach_state = XHCI_ATTACH_STALE;

static const char *xhci_attach_phase_str(enum xhci_attach_phase state)
{
    switch (state) {
    case XHCI_ATTACH_STALE:          return "STALE";
    case XHCI_ATTACH_WAIT_RECONNECT: return "WAIT_RECONNECT";
    case XHCI_ATTACH_FRESH:          return "FRESH";
    }
    return "UNKNOWN";
}

static const char *xhci_speed_str(enum usb_speed speed)
{
    switch (speed) {
    case USB_SPEED_LOW:      return "low";
    case USB_SPEED_FULL:     return "full";
    case USB_SPEED_HIGH:     return "high";
    case USB_SPEED_SUPER:    return "super";
    case USB_SPEED_UNKNOWN:  return "unknown";
    }
    return "?";
}

static bool xhci_stale_signature_changed(uint32_t initial, uint32_t current)
{
    /*
     * Ignore the RW1CS change bits when comparing snapshots; they can
     * flicker transiently and aren't the structural "this is a different
     * device/link state now" signal we care about. Everything else is
     * fair game — on nano-2 the USB-C ethernet dongle changes PORTSC[5]
     * without ever producing a full CCS 1->0->1 cycle, so the stale-hide
     * logic needs another promotion path.
     */
    uint32_t mask = ~XHCI_PORTSC_RW1CS_MASK;
    return (initial & mask) != (current & mask);
}

static bool xhci_stale_port_already_recovered(uint32_t portsc, bool connected)
{
    /*
     * On nano-2, a USB-C ethernet dongle present across kexec can land
     * in a "connected but deconfigured" state before SLM-OS ever polls
     * the root port: CCS stays set, PED is clear, and PEC is already
     * latched. In that case there is no later 1->0->1 transition for
     * the stale-hide state machine to observe, but the controller has
     * already told us the port changed. Surface it as a fresh attach so
     * usb_core can drive the normal port-reset/enumeration sequence.
     *
     * Keep this narrow: the original stale inherited device path keeps
     * PED set, so it still stays hidden until a real re-plug.
     */
    if (!connected)
        return false;

    return !(portsc & XHCI_PORTSC_PED) &&
           !!(portsc & (XHCI_PORTSC_PEC | XHCI_PORTSC_PRC));
}

static bool xhci_stale_port_detached_in_linux(uint32_t portsc, bool connected,
                                              enum usb_speed speed)
{
    /*
     * A Linux-side usb-device "remove" before kexec can leave the
     * physical device still connected but the root port no longer in the
     * old Linux-enumerated state: CCS stays set, PED is clear, and no
     * RW1CS change bits are latched anymore. That is materially different
     * from the original stale inherited case and is a candidate for a
     * fresh SLM-OS enumeration attempt without waiting for a manual
     * unplug/replug.
     */
    if (!connected || (portsc & XHCI_PORTSC_PED) ||
        (portsc & XHCI_PORTSC_RW1CS_MASK))
        return false;

    return speed == USB_SPEED_FULL || speed == USB_SPEED_LOW ||
           speed == USB_SPEED_HIGH;
}

static bool xhci_connected_disabled_port(uint32_t portsc)
{
    bool connected = false;
    enum usb_speed speed = USB_SPEED_UNKNOWN;
    bool decoded = xhci_decode_portsc(portsc, &connected, &speed);

    if (!connected || (portsc & XHCI_PORTSC_PED) || !decoded)
        return false;

    return speed == USB_SPEED_FULL || speed == USB_SPEED_LOW ||
           speed == USB_SPEED_HIGH;
}

static uint32_t xhci_ack_port_changes(uint8_t pidx, uint32_t portsc,
                                      const char *why)
{
    uint32_t change = portsc & XHCI_PORTSC_RW1CS_MASK;
    if (!change)
        return portsc;

    uint32_t ack = (portsc & ~XHCI_PORTSC_RW1CS_MASK) | change;
    xhci_op_w32(XHCI_OP_PORTSC(pidx), ack);

    uint64_t settle_start = timer_get_count();
    uint64_t settle_ticks = timer_get_frequency() / 100;  /* 10 ms */
    while (timer_get_count() - settle_start < settle_ticks) { }

    uint32_t after = xhci_op_r32(XHCI_OP_PORTSC(pidx));
    INFO("xhci: PORTSC[%u] acked change bits for %s (0x%08x -> 0x%08x)",
         (unsigned)pidx, why, (unsigned)portsc, (unsigned)after);
    return after;
}

static uint8_t xhci_locate_usb2_port(void)
{
    for (uint8_t p = 0; p < xhci_caps_cached.max_ports; p++) {
        uint32_t sc = xhci_op_r32(XHCI_OP_PORTSC(p));
        bool c = false;
        enum usb_speed s = USB_SPEED_UNKNOWN;
        bool decoded = xhci_decode_portsc(sc, &c, &s);
        INFO("xhci: port %u portsc=0x%08x connected=%u speed=%u",
             p, (unsigned)sc, (unsigned)c, (unsigned)s);
        if (c && decoded &&
            (s == USB_SPEED_FULL || s == USB_SPEED_LOW ||
             s == USB_SPEED_HIGH)) {
            xhci_active_portsc  = sc;
            xhci_stale_portsc_initial = sc;
            xhci_prereset_speed = s;
            return p;
        }
    }
    return 0xFF;
}

bool xhci_hcd_port_status(uint8_t port, bool *connected, enum usb_speed *speed)
{
    if (!xhci_live) return false;
    if (port != 0) return false;   /* Phase 3A: one logical port only. */

    if (xhci_active_port == 0xFF) {
        xhci_active_port = xhci_locate_usb2_port();
        if (xhci_active_port == 0xFF) {
            /* No device anywhere at init — any future CCS=1 is a
             * fresh attach. Skip the stale-hide wait. */
            xhci_attach_state = XHCI_ATTACH_WAIT_RECONNECT;
            if (connected) *connected = false;
            if (speed)     *speed     = USB_SPEED_UNKNOWN;
            INFO("xhci: no USB 2.0 device found across %u ports",
                 (unsigned)xhci_caps_cached.max_ports);
            return true;
        }
        /* A device already attached at init is either a pre-kexec
         * leftover Linux enumerated (the common case today — see
         * #309) or a device that was physically plugged in before
         * SLM-OS booted directly (the future non-kexec case). The
         * state machine starts in STALE either way, so the user
         * either re-plugs (kexec) or experiences a one-time extra
         * unplug-replug (direct boot). Phrase the log neutrally. */
        INFO("xhci: USB 2.0 device attached at init on PORTSC[%u] — "
             "unplug and re-insert to enumerate (see #309)",
             xhci_active_port);
    }

    uint32_t portsc = xhci_op_r32(XHCI_OP_PORTSC(xhci_active_port));
    xhci_active_portsc = portsc;
    bool c = false;
    enum usb_speed s = USB_SPEED_UNKNOWN;
    (void)xhci_decode_portsc(portsc, &c, &s);

    enum xhci_attach_phase prev = xhci_attach_state;
    if (prev == XHCI_ATTACH_STALE &&
        xhci_stale_port_already_recovered(portsc, c)) {
        INFO("xhci: stale port on PORTSC[%u] is connected with PED=0 "
             "and change latched (0x%08x) — treating as fresh attach",
             (unsigned)xhci_active_port, (unsigned)portsc);
        xhci_attach_state = XHCI_ATTACH_FRESH;
        xhci_prereset_speed = s;
        xhci_skip_next_port_reset = true;
        if (connected) *connected = true;
        if (speed)     *speed     = s;
        return true;
    }

    if (prev == XHCI_ATTACH_STALE &&
        xhci_stale_port_detached_in_linux(portsc, c, s)) {
        INFO("xhci: stale port on PORTSC[%u] is connected-disabled with "
             "no change bits (0x%08x) — treating as fresh attach",
             (unsigned)xhci_active_port, (unsigned)portsc);
        xhci_attach_state = XHCI_ATTACH_FRESH;
        xhci_prereset_speed = s;
        xhci_skip_next_port_reset = true;
        if (connected) *connected = true;
        if (speed)     *speed     = s;
        return true;
    }

    if (prev == XHCI_ATTACH_STALE && c &&
        xhci_stale_signature_changed(xhci_stale_portsc_initial, portsc)) {
        INFO("xhci: stale port signature changed on PORTSC[%u] "
             "(0x%08x -> 0x%08x) — treating as fresh attach",
             (unsigned)xhci_active_port,
             (unsigned)xhci_stale_portsc_initial,
             (unsigned)portsc);
        xhci_attach_state = XHCI_ATTACH_FRESH;
        xhci_prereset_speed = s;
        xhci_skip_next_port_reset = true;
        if (connected) *connected = true;
        if (speed)     *speed     = s;
        return true;
    }

    struct xhci_attach_result r = xhci_attach_step(prev, c, s);
    xhci_attach_state = r.next_state;

    if (r.transitioned) {
        if (prev == XHCI_ATTACH_STALE &&
            r.next_state == XHCI_ATTACH_WAIT_RECONNECT) {
            INFO("xhci: pre-kexec device detached — waiting for re-plug");
        } else if (prev == XHCI_ATTACH_WAIT_RECONNECT &&
                   r.next_state == XHCI_ATTACH_FRESH) {
            INFO("xhci: fresh USB attach on PORTSC[%u]",
                 (unsigned)xhci_active_port);
            xhci_prereset_speed = s;
        }
    }

    if (connected) *connected = r.report_connected;
    if (speed)     *speed     = r.report_speed;
    return true;
}

void xhci_dump_port_state(void)
{
    if (!xhci_live)
        return;

    uart_printf("  attach_state=%s active_port=%s",
                xhci_attach_phase_str(xhci_attach_state),
                xhci_active_port == 0xFF ? "unset" : "");
    if (xhci_active_port != 0xFF) {
        uart_printf("%u stale_portsc=0x%08x last_portsc=0x%08x prereset_speed=%s",
                    (unsigned)xhci_active_port,
                    (unsigned)xhci_stale_portsc_initial,
                    (unsigned)xhci_active_portsc,
                    xhci_speed_str(xhci_prereset_speed));
    }
    uart_puts("\r\n");

    for (uint8_t p = 0; p < xhci_caps_cached.max_ports; p++) {
        uint32_t sc = xhci_op_r32(XHCI_OP_PORTSC(p));
        bool connected = false;
        enum usb_speed speed = USB_SPEED_UNKNOWN;
        bool decoded = xhci_decode_portsc(sc, &connected, &speed);
        uint32_t pls = (sc & XHCI_PORTSC_PLS_MASK) >> XHCI_PORTSC_PLS_SHIFT;
        uint32_t raw_speed = (sc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;

        uart_printf("  PORTSC[%u]=0x%08x ccs=%u ped=%u pp=%u csc=%u pec=%u prc=%u pls=%u raw_speed=%u decoded=%u speed=%s%s\r\n",
                    (unsigned)p, (unsigned)sc,
                    (sc & XHCI_PORTSC_CCS) ? 1u : 0u,
                    (sc & XHCI_PORTSC_PED) ? 1u : 0u,
                    (sc & XHCI_PORTSC_PP)  ? 1u : 0u,
                    (sc & XHCI_PORTSC_CSC) ? 1u : 0u,
                    (sc & XHCI_PORTSC_PEC) ? 1u : 0u,
                    (sc & XHCI_PORTSC_PRC) ? 1u : 0u,
                    (unsigned)pls,
                    (unsigned)raw_speed,
                    decoded ? 1u : 0u,
                    xhci_speed_str(speed),
                    p == xhci_active_port ? "  <active>" : "");
    }
}

int xhci_hcd_port_reset(uint8_t port)
{
    if (!xhci_live) return -1;
    if (port != 0) return -1;
    if (xhci_active_port == 0xFF)
        return -1;

    uint32_t portsc = xhci_op_r32(XHCI_OP_PORTSC(xhci_active_port));
    if (xhci_skip_next_port_reset ||
        (xhci_attach_state == XHCI_ATTACH_FRESH &&
         xhci_connected_disabled_port(portsc))) {
        portsc = xhci_ack_port_changes(xhci_active_port, portsc,
                                       "connected-disabled reuse");
        xhci_skip_next_port_reset = false;
        xhci_active_portsc = portsc;
        INFO("xhci: skipping root-port reset on connected-disabled PORTSC[%u] (0x%08x)",
             (unsigned)xhci_active_port, (unsigned)xhci_active_portsc);
        return 0;
    }

    uint8_t pidx = xhci_active_port;
    uint64_t freq  = timer_get_frequency();
    uint64_t ticks = freq / 2;   /* 500 ms */

    for (unsigned attempt = 1; attempt <= 3; attempt++) {
        portsc = xhci_op_r32(XHCI_OP_PORTSC(pidx));
        portsc = xhci_ack_port_changes(pidx, portsc, "pre-reset cleanup");

        /*
         * PORTSC write discipline per §5.4.8: read-modify-write, clear the
         * RW1CS change bits we don't intend to touch (writing 1 would
         * clear them), then set PR. The controller starts the reset and
         * signals completion by setting PRC.
         */
        uint32_t write = portsc & ~XHCI_PORTSC_RW1CS_MASK;
        write |= XHCI_PORTSC_PR;
        xhci_op_w32(XHCI_OP_PORTSC(pidx), write);

        uint64_t start = timer_get_count();
        while (1) {
            uint32_t sc = xhci_op_r32(XHCI_OP_PORTSC(pidx));
            if ((sc & XHCI_PORTSC_PRC) && !(sc & XHCI_PORTSC_PR)) {
                uint32_t ack = (sc & ~XHCI_PORTSC_RW1CS_MASK) |
                               XHCI_PORTSC_PRC |
                               ((sc & XHCI_PORTSC_PEC) ? XHCI_PORTSC_PEC : 0U) |
                               ((sc & XHCI_PORTSC_CSC) ? XHCI_PORTSC_CSC : 0U);
                xhci_op_w32(XHCI_OP_PORTSC(pidx), ack);

                /* Let the link settle before reading back the final
                 * PORTSC speed field. Tegra234 post-reset speed bits
                 * occasionally lag PRC by a few ms. */
                uint64_t settle_start = timer_get_count();
                uint64_t settle_ticks = timer_get_frequency() / 100;  /* 10 ms */
                while (timer_get_count() - settle_start < settle_ticks) { }
                uint32_t final_sc = xhci_op_r32(XHCI_OP_PORTSC(pidx));
                if (!(final_sc & XHCI_PORTSC_PED)) {
                    /*
                     * On the inherited-kexec path the first read after
                     * PRC frequently lands in PED=0 / PLS=7. Give the link
                     * a little longer to settle before declaring the reset
                     * incomplete.
                     */
                    uint64_t extra_start = timer_get_count();
                    uint64_t extra_ticks = timer_get_frequency() / 5; /* 200 ms */
                    while (timer_get_count() - extra_start < extra_ticks) {
                        uint32_t retry_sc = xhci_op_r32(XHCI_OP_PORTSC(pidx));
                        uint32_t retry_pls =
                            (retry_sc & XHCI_PORTSC_PLS_MASK) >>
                            XHCI_PORTSC_PLS_SHIFT;
                        if ((retry_sc & XHCI_PORTSC_PED) || retry_pls != 7U) {
                            INFO("xhci: PORTSC[%u] post-reset settle (0x%08x -> 0x%08x)",
                                 pidx, (unsigned)final_sc, (unsigned)retry_sc);
                            final_sc = retry_sc;
                            break;
                        }
                    }
                }
                INFO("xhci: PORTSC[%u] reset complete (attempt %u, portsc=0x%08x -> 0x%08x)",
                     pidx, attempt, (unsigned)sc, (unsigned)final_sc);
                if (final_sc & XHCI_PORTSC_PED) {
                    xhci_active_portsc = final_sc;
                    return 0;
                }

                WARN("xhci: PORTSC[%u] reset left PED=0 (0x%08x)%s",
                     pidx, (unsigned)final_sc,
                     attempt < 3 ? " — retrying" : "");
                break;
            }
            if (timer_get_count() - start >= ticks) {
                WARN("xhci: PORTSC[%u] reset timeout (attempt %u, portsc=0x%08x)",
                     pidx, attempt,
                     (unsigned)xhci_op_r32(XHCI_OP_PORTSC(pidx)));
                return -1;
            }
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Context construction helpers (Step 5b + 7a)                                 */
/* -------------------------------------------------------------------------- */

/*
 * Map usb_speed to the Slot / PORTSC speed ID the HC expects. See
 * xHCI §4.3 Table 4-4; USB 2.0 values are 1/2/3 = full/low/high.
 */
static uint32_t xhci_speed_to_id(enum usb_speed s)
{
    switch (s) {
    case USB_SPEED_FULL:  return XHCI_PORTSC_SPEED_FULL;
    case USB_SPEED_LOW:   return XHCI_PORTSC_SPEED_LOW;
    case USB_SPEED_HIGH:  return XHCI_PORTSC_SPEED_HIGH;
    case USB_SPEED_SUPER: return XHCI_PORTSC_SPEED_SUPER;
    default:              return 0;
    }
}

/*
 * Per §4.3 + §6.2.1.1: bMaxPacketSize0 depends on speed:
 *   low   -> 8
 *   full  -> 8, 16, 32, or 64. Linux/xHCI seeds EP0 with 64 here so the
 *                              first 8-byte device-descriptor probe uses
 *                              the controller's normal FS default-pipe
 *                              programming rather than an undersized MPS.
 *   high  -> 64
 *   super -> 512 (out of scope; accept for forward compatibility)
 */
static uint16_t xhci_ep0_max_packet(enum usb_speed s)
{
    switch (s) {
    case USB_SPEED_LOW:   return 8;
    case USB_SPEED_FULL:  return 64;
    case USB_SPEED_HIGH:  return 64;
    case USB_SPEED_SUPER: return 512;
    default:              return 8;
    }
}

/*
 * Populate an Input Context for ADDRESS_DEVICE on a freshly-opened
 * slot. Writes:
 *   - Input Control Ctx: Add = A0 | A1 (Slot + EP0)
 *   - Slot Ctx  DW0: Route = 0, Speed, Context Entries = 1 (EP0 only)
 *   - Slot Ctx  DW1: Root Hub Port Number
 *   - EP0 Ctx   DW0: Interval = 0 (control)
 *   - EP0 Ctx   DW1: CErr = 3, EP Type = CONTROL, Max Packet Size
 *   - EP0 Ctx   DW2: TR Dequeue Pointer (low) | DCS=1
 *   - EP0 Ctx   DW3: TR Dequeue Pointer (high)
 *   - EP0 Ctx   DW4: Average TRB Length = 8 (control-transfer rule-of-thumb)
 */
static void xhci_build_input_ctx_for_address(struct xhci_device *d,
                                             const struct usb_device *dev,
                                             uintptr_t ep0_ring_phys)
{
    bool cz = xhci_caps_cached.ctx_64;
    memset(d->input_ctx, 0, xhci_ctx_in_bytes(cz));

    /* Input Control Context — Add Slot + EP0 DCI=1 context. */
    uint32_t *in_add = xhci_in_control_dw(d->input_ctx, 1);
    *in_add = XHCI_INPUT_ADD_SLOT | XHCI_INPUT_ADD_EP(XHCI_DCI_EP0);

    /* Slot Context. */
    uint32_t *s0 = xhci_in_slot_dw(d->input_ctx, 0, cz);
    uint32_t *s1 = xhci_in_slot_dw(d->input_ctx, 1, cz);
    uint32_t speed_id = xhci_speed_to_id(dev->speed);
    /* Context Entries = 1 (only EP0 is valid until endpoint_configure
     * adds more). Route string comes from usb_core; root devices keep
     * it at 0, one-tier hub children use the downstream port number in
     * the low nibble. */
    *s0 = (dev->route_string & XHCI_SLOT_DW0_ROUTE_MASK) |
          (speed_id << XHCI_SLOT_DW0_SPEED_SHIFT) |
          (1U       << XHCI_SLOT_DW0_CTXENT_SHIFT);
    /* Root Hub Port Number is 1-based per spec. */
    *s1 = ((uint32_t)d->root_port << XHCI_SLOT_DW1_ROOT_PORT_SHIFT);

    /* EP0 Context. */
    uint32_t *e0 = xhci_in_ep_dw(d->input_ctx, XHCI_DCI_EP0, 0, cz);
    uint32_t *e1 = xhci_in_ep_dw(d->input_ctx, XHCI_DCI_EP0, 1, cz);
    uint32_t *e2 = xhci_in_ep_dw(d->input_ctx, XHCI_DCI_EP0, 2, cz);
    uint32_t *e3 = xhci_in_ep_dw(d->input_ctx, XHCI_DCI_EP0, 3, cz);
    uint32_t *e4 = xhci_in_ep_dw(d->input_ctx, XHCI_DCI_EP0, 4, cz);

    uint16_t mps = xhci_ep0_max_packet(dev->speed);
    *e0 = 0;                                  /* state=Disabled, interval=0 */
    *e1 = (3U  << XHCI_EP_DW1_CERR_SHIFT) |   /* CErr = 3 retries */
          (XHCI_EP_TYPE_CONTROL << XHCI_EP_DW1_EPTYPE_SHIFT) |
          ((uint32_t)mps << XHCI_EP_DW1_MAXPKT_SHIFT);
    *e2 = (uint32_t)(ep0_ring_phys & 0xFFFFFFFFu) | 0x1U;  /* DCS = 1 */
    *e3 = (uint32_t)(ep0_ring_phys >> 32);
    *e4 = 8U;                                 /* avg TRB length */
}

/* -------------------------------------------------------------------------- */
/* device_open (Step 5b)                                                       */
/* -------------------------------------------------------------------------- */

int xhci_hcd_device_open(struct usb_device *dev)
{
    if (!xhci_live) return -1;
    if (dev == NULL) return -1;

    struct xhci_device *d = xhci_dev_pool_alloc();
    if (d == NULL) {
        WARN("xhci: device pool exhausted");
        return -1;
    }

    bool cz = xhci_caps_cached.ctx_64;

    /*
     * Allocate Device Context + Input Context. Both must be
     * 64-byte-aligned per xHCI §6.2.{1,5} table footnotes; we use the
     * full context stride (32 or 64) as the alignment.
     *
     * ncmem is a bump allocator with no free (see kernel/include/ncmem.h):
     * if any step of device_open fails, the NC bytes are stranded for
     * the life of the boot. Phase 3A runs device_open exactly once per
     * boot (single device, triggered either at init or via the #309
     * re-plug path), and a failure here is a boot-level problem that
     * warrants a reboot — so the stranded bytes are acceptable. The
     * same applies to the ep0 transfer ring allocated a few lines down.
     */
    d->dev_ctx = ncmem_alloc(xhci_ctx_dev_bytes(cz), 64);
    d->input_ctx = ncmem_alloc(xhci_ctx_in_bytes(cz), 64);
    if (d->dev_ctx == NULL || d->input_ctx == NULL) {
        WARN("xhci: NC alloc failed for device / input ctx");
        goto err;
    }
    memset(d->dev_ctx,   0, xhci_ctx_dev_bytes(cz));
    memset(d->input_ctx, 0, xhci_ctx_in_bytes(cz));
    d->dev_ctx_phys   = (uintptr_t)d->dev_ctx;
    d->input_ctx_phys = (uintptr_t)d->input_ctx;

    /* Allocate the EP0 transfer ring. */
    struct xhci_ring *ep0 = xhci_ring_pool_alloc();
    if (ep0 == NULL) {
        WARN("xhci: transfer-ring pool exhausted");
        goto err;
    }
    if (xhci_ring_alloc(ep0, 64) != 0) {
        xhci_ring_pool_free(ep0);
        goto err;
    }
    d->ep_rings[XHCI_DCI_EP0] = ep0;

    /* Root Hub Port Number for the Slot Context is 1-based (xHCI
     * §6.2.2). usb_core addresses ports as 0-based; the scan above
     * remembered which real PORTSC index actually has our USB 2
     * device, and that's the PORTSC we need to program here. */
    if (xhci_active_port == 0xFF) {
        WARN("xhci: device_open before port scan located a USB 2 port");
        goto err;
    }
    if (dev->root_hub_port != 0) {
        d->root_port = dev->root_hub_port;
    } else {
        d->root_port = (uint8_t)(xhci_active_port + 1U);
        dev->root_hub_port = d->root_port;
    }

    /*
     * Decide which speed to program into the Slot Context.
     *
     * On Tegra234 post-kexec, the speed field in PORTSC right after
     * the reset often reads as FULL even when the device was
     * running at HIGH under Linux — the chirp hasn't fully settled
     * and the value is effectively stale. If the pre-reset scan
     * observed HIGH, prefer that over the immediately-post-reset
     * value: the device was HIGH before, and a successful bus reset
     * should bring it back to HIGH (or it was physically disconnected
     * and we'd fail elsewhere). Control transfers at the wrong
     * signalling speed produce cc=4 (USB transaction error), which
     * is what we saw when trusting the stale FULL.
     *
     * If the pre-reset scan saw FULL / LOW the downgrade path is
     * trusted — we never observed those speeds renegotiate upward.
     */
    if (dev->route_string == 0) {
        bool connected_post = false;
        enum usb_speed speed_post = USB_SPEED_UNKNOWN;
        (void)xhci_decode_portsc(xhci_active_portsc,
                                  &connected_post, &speed_post);
        if (xhci_prereset_speed == USB_SPEED_HIGH &&
            speed_post != USB_SPEED_HIGH) {
            INFO("xhci: pre-reset HIGH but PORTSC reads %d post-reset; "
                 "using HIGH (stale PORTSC on Tegra)", (int)speed_post);
            dev->speed = USB_SPEED_HIGH;
        } else if (speed_post != USB_SPEED_UNKNOWN &&
                   speed_post != dev->speed) {
            INFO("xhci: post-reset speed %d (was %d on pre-reset scan)",
                 (int)speed_post, (int)dev->speed);
            dev->speed = speed_post;
        }
    }

    /* 1. ENABLE_SLOT. */
    struct xhci_trb cmd = {0};
    cmd.control = XHCI_TRB_TYPE(XHCI_TRB_CMD_ENABLE_SLOT);
    uint8_t cc = 0;
    uint8_t slot = 0;
    if (xhci_cmd_submit_and_wait(&cmd, &cc, &slot, 1000) != 0 ||
        cc != XHCI_CC_SUCCESS || slot == 0) {
        WARN("xhci: ENABLE_SLOT cc=%u slot=%u", cc, slot);
        goto err;
    }
    d->slot_id = slot;
    INFO("xhci: slot %u enabled for port %u", slot, d->root_port);

    /* 2. Build Input Context for ADDRESS_DEVICE and write DCBAA[slot]. */
    xhci_build_input_ctx_for_address(d, dev, ep0->phys);
    xhci_dcbaa[slot] = (uint64_t)d->dev_ctx_phys;
    dsb(sy);

    /*
     * 3. ADDRESS_DEVICE. xHCI §4.6.5: BSR=1 ("Block Set Address
     * Request") is the Phase 3A strategy we use: the HC sets up the
     * Slot Context, transitions it to the Addressed / Default state,
     * and does NOT issue a USB SET_ADDRESS control transfer.
     *
     * That matches how usb_core drives enumeration — it sends its
     * own SET_ADDRESS(1) later via usb_control_msg, and xhci_xfer.c
     * intercepts that request (since the xHCI spec doesn't let user
     * code replace the HC's internally-chosen address after BSR=0).
     * Net effect: device is at USB address 0 now and stays there
     * from xHCI's point of view; usb_core believes it's at address
     * 1 for bookkeeping. All subsequent transfers route via the
     * Slot Context, not dev->address, so the divergence is
     * harmless.
     *
     * BSR=0 was tried first and failed with cc=4 (USB transaction
     * error) on the HC's internal SET_ADDRESS — behaviour observed
     * on jetson-nano-1 with a FULL-speed Realtek dongle that was
     * already enumerated by Linux before kexec. BSR=1 sidesteps the
     * transaction error entirely because no USB packet is sent.
     */
    memset(&cmd, 0, sizeof(cmd));
    cmd.param_lo = (uint32_t)(d->input_ctx_phys & 0xFFFFFFFFu);
    cmd.param_hi = (uint32_t)(d->input_ctx_phys >> 32);
    /* control DW, bit 9 = BSR (Block Set Address Request).
     *
     * Keep BSR=1 here. The BSR=0 experiment was worse on nano-2: the
     * HC's internal SET_ADDRESS failed immediately with cc=4, while
     * BSR=1 gets far enough to exercise software-driven EP0 traffic.
     */
    cmd.control  = XHCI_TRB_TYPE(XHCI_TRB_CMD_ADDRESS_DEVICE) |
                   (1u << 9) |
                   ((uint32_t)slot << XHCI_TRB_SLOT_SHIFT);
    if (xhci_cmd_submit_and_wait(&cmd, &cc, NULL, 1000) != 0 ||
        cc != XHCI_CC_SUCCESS) {
        WARN("xhci: ADDRESS_DEVICE(BSR=1) cc=%u", cc);
        goto err_slot;
    }
    INFO("xhci: slot %u addressed (speed=%u, port=%u, BSR=1)",
         slot, (unsigned)dev->speed, d->root_port);

    dev->hcd_private = d;
    return 0;

err_slot: {
        /* Best effort: DISABLE_SLOT so we don't leak the slot. */
        struct xhci_trb dis = {0};
        dis.control = XHCI_TRB_TYPE(XHCI_TRB_CMD_DISABLE_SLOT) |
                      ((uint32_t)slot << XHCI_TRB_SLOT_SHIFT);
        (void)xhci_cmd_submit_and_wait(&dis, NULL, NULL, 500);
        xhci_dcbaa[slot] = 0;
    }
err:
    xhci_dev_pool_free(d);
    return -1;
}

/* -------------------------------------------------------------------------- */
/* device_close                                                                */
/* -------------------------------------------------------------------------- */

void xhci_hcd_device_close(struct usb_device *dev)
{
    if (!xhci_live || dev == NULL)
        return;
    struct xhci_device *d = (struct xhci_device *)dev->hcd_private;
    if (d == NULL)
        return;

    if (d->slot_id != 0) {
        struct xhci_trb cmd = {0};
        cmd.control = XHCI_TRB_TYPE(XHCI_TRB_CMD_DISABLE_SLOT) |
                      ((uint32_t)d->slot_id << XHCI_TRB_SLOT_SHIFT);
        uint8_t cc = 0;
        (void)xhci_cmd_submit_and_wait(&cmd, &cc, NULL, 500);
        xhci_dcbaa[d->slot_id] = 0;
        d->slot_id = 0;
    }

    dev->hcd_private = NULL;
    xhci_dev_pool_free(d);
}

/* -------------------------------------------------------------------------- */
/* endpoint_configure (Step 7a)                                                */
/* -------------------------------------------------------------------------- */

static uint32_t xhci_ep_type_from_usb(uint8_t attributes, uint8_t address)
{
    uint8_t xfer = attributes & USB_XFER_TYPE_MASK;
    bool in = (address & USB_DIR_IN) != 0;
    switch (xfer) {
    case USB_XFER_CONTROL:   return XHCI_EP_TYPE_CONTROL;
    case USB_XFER_BULK:      return in ? XHCI_EP_TYPE_BULK_IN  : XHCI_EP_TYPE_BULK_OUT;
    case USB_XFER_INTERRUPT: return in ? XHCI_EP_TYPE_INT_IN   : XHCI_EP_TYPE_INT_OUT;
    case USB_XFER_ISOC:      return in ? XHCI_EP_TYPE_ISOC_IN  : XHCI_EP_TYPE_ISOC_OUT;
    default:                 return XHCI_EP_TYPE_CONTROL;
    }
}

int xhci_hcd_endpoint_configure(struct usb_device *dev,
                                const struct usb_endpoint *ep)
{
    if (!xhci_live || dev == NULL || ep == NULL || !ep->valid)
        return -1;
    struct xhci_device *d = (struct xhci_device *)dev->hcd_private;
    if (d == NULL)
        return -1;

    bool cz = xhci_caps_cached.ctx_64;
    unsigned dci = xhci_dci_ep(ep->address);
    if (dci < 2 || dci > 31)
        return -1;
    if (d->ep_rings[dci] != NULL) {
        /* Already configured — CDC-ECM doesn't trigger this in Phase 3A,
         * so treat as a caller bug rather than silently succeeding. */
        WARN("xhci: ep 0x%02x (dci %u) already configured", ep->address, dci);
        return -1;
    }

    /* Per-endpoint transfer ring. 64 TRBs is plenty for CDC-ECM bulk. */
    struct xhci_ring *r = xhci_ring_pool_alloc();
    if (r == NULL) {
        WARN("xhci: out of transfer rings for ep 0x%02x", ep->address);
        return -1;
    }
    if (xhci_ring_alloc(r, 64) != 0) {
        xhci_ring_pool_free(r);
        return -1;
    }

    /* Input Context: Add bit for this DCI, keep Slot Context, rewrite
     * EP context for this DCI. Zero the Drop flags (no de-configure). */
    memset(d->input_ctx, 0, xhci_ctx_in_bytes(cz));
    uint32_t *in_add  = xhci_in_control_dw(d->input_ctx, 1);
    *in_add = XHCI_INPUT_ADD_SLOT | XHCI_INPUT_ADD_EP(dci);

    /* Update Slot Context: bump Context Entries to cover the new EP. */
    uint32_t *s0 = xhci_in_slot_dw(d->input_ctx, 0, cz);
    uint32_t *s1 = xhci_in_slot_dw(d->input_ctx, 1, cz);
    uint32_t speed_id = xhci_speed_to_id(dev->speed);
    /* Context Entries in the Slot Context must cover every valid EP
     * DCI. The Input Context was freshly zeroed above, so no prior
     * CONFIGURE_ENDPOINT state survives — `dci` is the new high-water
     * mark for this single-EP add. */
    *s0 = (dev->route_string & XHCI_SLOT_DW0_ROUTE_MASK) |
          (speed_id << XHCI_SLOT_DW0_SPEED_SHIFT) |
          ((uint32_t)dci << XHCI_SLOT_DW0_CTXENT_SHIFT);
    *s1 = ((uint32_t)d->root_port << XHCI_SLOT_DW1_ROOT_PORT_SHIFT);

    /* Endpoint context. */
    uint32_t *e0 = xhci_in_ep_dw(d->input_ctx, dci, 0, cz);
    uint32_t *e1 = xhci_in_ep_dw(d->input_ctx, dci, 1, cz);
    uint32_t *e2 = xhci_in_ep_dw(d->input_ctx, dci, 2, cz);
    uint32_t *e3 = xhci_in_ep_dw(d->input_ctx, dci, 3, cz);
    uint32_t *e4 = xhci_in_ep_dw(d->input_ctx, dci, 4, cz);

    uint32_t ep_type = xhci_ep_type_from_usb(ep->attributes, ep->address);
    uint32_t interval = ep->interval;
    *e0 = (interval << XHCI_EP_DW0_INTERVAL_SHIFT) & XHCI_EP_DW0_INTERVAL_MASK;
    *e1 = (3U << XHCI_EP_DW1_CERR_SHIFT) |
          (ep_type << XHCI_EP_DW1_EPTYPE_SHIFT) |
          ((uint32_t)ep->max_packet << XHCI_EP_DW1_MAXPKT_SHIFT);
    *e2 = (uint32_t)(r->phys & 0xFFFFFFFFu) | 0x1U;  /* DCS = 1 */
    *e3 = (uint32_t)(r->phys >> 32);
    *e4 = (ep->attributes & USB_XFER_TYPE_MASK) == USB_XFER_BULK
          ? ep->max_packet                /* reasonable avg for bulk */
          : 8U;                           /* control / interrupt default */
    dsb(sy);

    /* CONFIGURE_ENDPOINT. */
    struct xhci_trb cmd = {0};
    cmd.param_lo = (uint32_t)(d->input_ctx_phys & 0xFFFFFFFFu);
    cmd.param_hi = (uint32_t)(d->input_ctx_phys >> 32);
    cmd.control  = XHCI_TRB_TYPE(XHCI_TRB_CMD_CONFIGURE_EP) |
                   ((uint32_t)d->slot_id << XHCI_TRB_SLOT_SHIFT);
    uint8_t cc = 0;
    if (xhci_cmd_submit_and_wait(&cmd, &cc, NULL, 1000) != 0 ||
        cc != XHCI_CC_SUCCESS) {
        WARN("xhci: CONFIGURE_ENDPOINT ep 0x%02x cc=%u", ep->address, cc);
        xhci_ring_pool_free(r);
        return -1;
    }

    d->ep_rings[dci] = r;
    INFO("xhci: ep 0x%02x configured (dci=%u, type=%u, mps=%u)",
         ep->address, dci, ep_type, ep->max_packet);
    return 0;
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

/* Non-Jetson stubs so the driver compiles on every target. */
bool xhci_decode_portsc(uint32_t portsc, bool *c, enum usb_speed *s)
{
    /* Same pure logic on every platform — the register map is
     * identical across xHCI 1.2 controllers; only the MMIO base
     * changes. Tests link against this stub path on QEMU/x86/Pi5. */
    bool cc = (portsc & XHCI_PORTSC_CCS) != 0;
    uint32_t sp = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
    enum usb_speed spv = USB_SPEED_UNKNOWN;
    bool known = true;
    if (cc) {
        switch (sp) {
        case XHCI_PORTSC_SPEED_FULL:  spv = USB_SPEED_FULL;  break;
        case XHCI_PORTSC_SPEED_LOW:   spv = USB_SPEED_LOW;   break;
        case XHCI_PORTSC_SPEED_HIGH:  spv = USB_SPEED_HIGH;  break;
        case XHCI_PORTSC_SPEED_SUPER: spv = USB_SPEED_SUPER; break;
        default: spv = USB_SPEED_UNKNOWN; known = false;     break;
        }
    }
    if (c) *c = cc;
    if (s) *s = spv;
    return known;
}

bool xhci_hcd_port_status(uint8_t p, bool *c, enum usb_speed *s)
{
    (void)p; (void)c; (void)s; return false;
}
int  xhci_hcd_port_reset(uint8_t p)              { (void)p; return -1; }
int  xhci_hcd_device_open(struct usb_device *d)   { (void)d; return -1; }
void xhci_hcd_device_close(struct usb_device *d)  { (void)d; }
int  xhci_hcd_endpoint_configure(struct usb_device *d,
                                 const struct usb_endpoint *e)
{ (void)d; (void)e; return -1; }

#endif /* PLATFORM_JETSON_ORIN_NANO */
