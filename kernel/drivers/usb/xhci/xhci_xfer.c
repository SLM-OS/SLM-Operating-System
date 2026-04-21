/*
 * xhci_xfer.c - Phase 3A Step 6 (control transfers) + Step 7b (bulk +
 *               cancel + completion dispatch) of #266.
 *
 * Implements:
 *   - xhci_hcd_submit_urb     (control + bulk TRB builders)
 *   - xhci_hcd_cancel_urb     (best-effort Stop Endpoint)
 *   - xhci_hcd_poll           (drives the event ring from the shell)
 *   - xhci_xfer_on_transfer_event  (event-ring dispatch hook from xhci.c)
 *
 * The submit path is synchronous at the producer-ring layer: we
 * enqueue the TRBs, write the doorbell, and return 0. usb_core +
 * cdc_ecm drive completion either via the blocking usb_control_msg
 * poll loop or via net_poll → usb_core_poll → xhci_hcd_poll.
 *
 * URB → TRB mapping:
 *   Control    Setup Stage + (optional) Data Stage + Status Stage
 *                with IOC on the Status Stage TRB, so the Transfer
 *                Event the HC writes back references the Status Stage.
 *   Bulk / Int  One Normal TRB per URB (buffers fit in 64 KB; CDC-ECM
 *                in Phase 3A uses 2 KB RX + TX slots, well within the
 *                per-TRB cap). IOC + ISP (interrupt on short) set on
 *                the single TRB.
 *
 * URB tracking:
 *   A small fixed-size slot array matches Transfer Events back to the
 *   submitting URB. The slot is keyed on `last_trb_phys` (the TRB
 *   with IOC set), which is what xHCI reports in the event's
 *   Parameter field. Phase 3A has at most ~10 in-flight URBs (CDC-ECM
 *   RX+TX pool plus a one-shot control transfer during enumeration).
 */

#include "xhci_internal.h"
#include "xhci_regs.h"
#include "xhci_ring.h"
#include "xhci_trb.h"
#include "xhci_trb_build.h"
#include "xhci_ctx.h"
#include "usb.h"
#include "debug.h"
#include "timer.h"
#include "spinlock.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if defined(PLATFORM_JETSON_ORIN_NANO)

/* -------------------------------------------------------------------------- */
/* In-flight URB tracking                                                      */
/* -------------------------------------------------------------------------- */

#define XHCI_MAX_INFLIGHT_URBS      16

struct xhci_urb_slot {
    bool             in_use;
    struct usb_urb  *urb;
    uintptr_t        first_trb_phys;
    uintptr_t        data_trb_phys;
    uintptr_t        last_trb_phys;
    struct xhci_ring *ring;
    unsigned         dci;
    uint32_t         requested_len;
};

static struct xhci_urb_slot xhci_urbs[XHCI_MAX_INFLIGHT_URBS];

static struct xhci_urb_slot *xhci_urb_slot_alloc(void)
{
    for (unsigned i = 0; i < XHCI_MAX_INFLIGHT_URBS; i++) {
        if (!xhci_urbs[i].in_use) {
            xhci_urbs[i].in_use = true;
            return &xhci_urbs[i];
        }
    }
    return NULL;
}

static void xhci_urb_slot_free(struct xhci_urb_slot *s)
{
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
}

static struct xhci_urb_slot *xhci_urb_slot_find_by_trb(uintptr_t trb_phys)
{
    for (unsigned i = 0; i < XHCI_MAX_INFLIGHT_URBS; i++) {
        if (!xhci_urbs[i].in_use)
            continue;
        if (xhci_urbs[i].first_trb_phys <= trb_phys &&
            trb_phys <= xhci_urbs[i].last_trb_phys &&
            ((trb_phys - xhci_urbs[i].first_trb_phys) %
             sizeof(struct xhci_trb) == 0))
            return &xhci_urbs[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* URB → TRB builders                                                          */
/* -------------------------------------------------------------------------- */

static enum usb_urb_status xhci_cc_to_urb_status(uint8_t cc)
{
    switch (cc) {
    case XHCI_CC_SUCCESS:            return USB_URB_OK;
    case XHCI_CC_SHORT_PACKET:       return USB_URB_SHORT;
    case XHCI_CC_STALL:              return USB_URB_STALL;
    case XHCI_CC_DATA_BUF_ERR:       return USB_URB_IO_ERROR;
    case XHCI_CC_BABBLE:             return USB_URB_IO_ERROR;
    case XHCI_CC_USB_TRANSACTION_ERR: return USB_URB_IO_ERROR;
    default:                         return USB_URB_IO_ERROR;
    }
}

static bool xhci_urb_is_get_descriptor(const struct usb_urb *urb)
{
    return urb != NULL &&
           urb->transfer_type == USB_XFER_CONTROL &&
           urb->setup.bmRequestType == (USB_DIR_IN | USB_TYPE_STANDARD |
                                        USB_RECIP_DEVICE) &&
           urb->setup.bRequest == USB_REQ_GET_DESCRIPTOR;
}

static void xhci_log_control_urb(const char *tag, const struct usb_urb *urb,
                                 const struct xhci_urb_slot *slot,
                                 uintptr_t event_trb_phys,
                                 uint8_t cc, uint32_t residual)
{
    if (urb == NULL || urb->transfer_type != USB_XFER_CONTROL)
        return;

    const char *stage = "unknown";
    if (slot != NULL) {
        if (event_trb_phys == slot->first_trb_phys) {
            stage = "setup";
        } else if (slot->data_trb_phys != 0 &&
                   event_trb_phys == slot->data_trb_phys) {
            stage = "data";
        } else if (event_trb_phys == slot->last_trb_phys) {
            stage = "status";
        }
    }

    INFO("xhci: %s ctrl req=0x%02x type=0x%02x wValue=0x%04x wIndex=0x%04x "
         "wLength=%u stage=%s trb=0x%lx cc=%u residual=%u actual=%u status=%d",
         tag,
         (unsigned)urb->setup.bRequest,
         (unsigned)urb->setup.bmRequestType,
         (unsigned)urb->setup.wValue,
         (unsigned)urb->setup.wIndex,
         (unsigned)urb->setup.wLength,
         stage,
         (unsigned long)event_trb_phys,
         (unsigned)cc,
         (unsigned)residual,
         (unsigned)urb->actual_length,
         (int)urb->status);
}

/* TRB builders live in xhci_trb_build.h (shared with test_xhci_xfer.c). */

/* -------------------------------------------------------------------------- */
/* Submit helpers                                                              */
/* -------------------------------------------------------------------------- */

/*
 * Enqueue a pre-built TRB on the given ring, matching the ring's
 * current cycle state. Returns the virtual address of the slot in the
 * ring (which, for NC memory, equals the physical address the HC will
 * see on completion events). NULL on ring-full.
 *
 * xhci_ring_enqueue overwrites the `control` dword's cycle bit with
 * the ring's PCS, so the template we pass in can safely carry cycle=0.
 */
static struct xhci_trb *xhci_ring_put(struct xhci_ring *r,
                                      const struct xhci_trb *t)
{
    return xhci_ring_enqueue(r, t);
}

static int xhci_submit_control(struct usb_urb *urb, struct xhci_device *d)
{
    struct xhci_ring *r = d->ep_rings[XHCI_DCI_EP0];
    if (r == NULL) {
        WARN("xhci: submit_control but EP0 ring is NULL");
        return -USB_URB_IO_ERROR;
    }

    bool has_data = urb->length > 0 && urb->buffer != NULL;
    bool data_in  = (urb->setup.bmRequestType & USB_DIR_IN) != 0;
    uint32_t trt  = 0U;
    if (has_data) trt = data_in ? 3U : 2U;

    struct xhci_urb_slot *slot = xhci_urb_slot_alloc();
    if (slot == NULL)
        return -USB_URB_IO_ERROR;
    slot->urb            = urb;
    slot->ring           = r;
    slot->dci            = XHCI_DCI_EP0;
    slot->requested_len  = urb->length;

    if (xhci_urb_is_get_descriptor(urb)) {
        INFO("xhci: submit ctrl GET_DESCRIPTOR type=0x%02x index=%u len=%u "
             "addr=%u route=0x%x root_port=%u speed=%u",
             (unsigned)(urb->setup.wValue >> 8),
             (unsigned)(urb->setup.wValue & 0xFF),
             (unsigned)urb->setup.wLength,
             (unsigned)urb->dev->address,
             (unsigned)urb->dev->route_string,
             (unsigned)d->root_port,
             (unsigned)urb->dev->speed);
    }

    struct xhci_trb tmpl;
    struct xhci_trb *slot_trb;

    /* 1. Setup Stage. */
    xhci_build_setup_stage(&tmpl, &urb->setup, trt, 0);
    slot_trb = xhci_ring_put(r, &tmpl);
    if (slot_trb == NULL) goto fail;
    slot->first_trb_phys = (uintptr_t)slot_trb;

    /* 2. Optional Data Stage. */
    if (has_data) {
        xhci_build_data_stage(&tmpl, (uintptr_t)urb->buffer,
                              urb->length, data_in, 0);
        slot_trb = xhci_ring_put(r, &tmpl);
        if (slot_trb == NULL) goto fail;
        slot->data_trb_phys = (uintptr_t)slot_trb;
    }

    /* 3. Status Stage — Direction is opposite of the Data Stage. For
     *    a no-data control transfer it must be IN per §4.11.2.2.
     *
     * Error completions may reference any TRB in the chain, not just
     * the IOC-bearing Status Stage. Track the whole contiguous span so
     * a cc=4 on Setup or Data still completes the URB instead of timing
     * out in usb_wait_urb().
     */
    bool status_in = has_data ? !data_in : true;
    xhci_build_status_stage(&tmpl, status_in, 0);
    slot_trb = xhci_ring_put(r, &tmpl);
    if (slot_trb == NULL) goto fail;

    slot->last_trb_phys = (uintptr_t)slot_trb;
    urb->hcd_private    = slot;

    /* Kick EP0. */
    xhci_ring_doorbell(d->slot_id, XHCI_DCI_EP0);
    return 0;

fail:
    xhci_urb_slot_free(slot);
    return -USB_URB_IO_ERROR;
}

static int xhci_submit_bulk_int(struct usb_urb *urb, struct xhci_device *d)
{
    unsigned dci = xhci_dci_ep(urb->endpoint);
    if (dci < 2 || dci > 31)
        return -USB_URB_IO_ERROR;
    struct xhci_ring *r = d->ep_rings[dci];
    if (r == NULL) {
        WARN("xhci: submit bulk but dci %u has no ring", dci);
        return -USB_URB_IO_ERROR;
    }

    struct xhci_urb_slot *slot = xhci_urb_slot_alloc();
    if (slot == NULL)
        return -USB_URB_IO_ERROR;
    slot->urb            = urb;
    slot->ring           = r;
    slot->dci            = dci;
    slot->requested_len  = urb->length;

    struct xhci_trb tmpl;
    xhci_build_normal(&tmpl, (uintptr_t)urb->buffer, urb->length, 0);
    struct xhci_trb *slot_trb = xhci_ring_put(r, &tmpl);
    if (slot_trb == NULL) {
        xhci_urb_slot_free(slot);
        return -USB_URB_IO_ERROR;
    }
    slot->last_trb_phys = (uintptr_t)slot_trb;
    urb->hcd_private    = slot;

    xhci_ring_doorbell(d->slot_id, (uint8_t)dci);
    return 0;
}

/*
 * Intercept USB standard SET_ADDRESS (bmRequestType=0x00,
 * bRequest=0x05) and convert it into a synchronous success without
 * actually submitting TRBs.
 *
 * Why: xHCI's ADDRESS_DEVICE command (issued during device_open with
 * BSR=1) has already taken the device out of the default state and
 * set up the slot context. Replaying a user-driven SET_ADDRESS on
 * EP0 would at best duplicate that work — and at worst (BSR=0 path)
 * diverge the HC's view of the address from the device's. The xHCI
 * spec explicitly forbids the user from issuing SET_ADDRESS on the
 * EP0 ring because the HC owns the address assignment.
 *
 * usb_core_enumerate() sends SET_ADDRESS(1) as part of its generic
 * enumeration sequence (Phase 1 design: addresses come from the
 * caller so simple HCDs don't need to know about it). Returning
 * synthetic success keeps usb_core's state machine happy; the HC
 * and the device continue to agree on whatever address the
 * ADDRESS_DEVICE picked, and subsequent transfers route via the
 * Slot Context rather than dev->address.
 */
static bool xhci_is_set_address(const struct usb_urb *urb)
{
    return urb->transfer_type == USB_XFER_CONTROL &&
           urb->setup.bmRequestType == (USB_DIR_OUT | USB_TYPE_STANDARD |
                                        USB_RECIP_DEVICE) &&
           urb->setup.bRequest == USB_REQ_SET_ADDRESS;
}

int xhci_hcd_submit_urb(struct usb_urb *urb)
{
    if (!xhci_live || urb == NULL || urb->dev == NULL)
        return -USB_URB_IO_ERROR;
    struct xhci_device *d = (struct xhci_device *)urb->dev->hcd_private;
    if (d == NULL || d->slot_id == 0)
        return -USB_URB_IO_ERROR;

    if (xhci_is_set_address(urb)) {
        INFO("xhci: SET_ADDRESS(%u) intercepted — xHCI handled it via "
             "ADDRESS_DEVICE in device_open", (unsigned)urb->setup.wValue);
        urb->status        = USB_URB_OK;
        urb->actual_length = 0;
        urb->hcd_private   = NULL;
        if (urb->complete) urb->complete(urb);
        return 0;
    }

    switch (urb->transfer_type) {
    case USB_XFER_CONTROL:   return xhci_submit_control(urb, d);
    case USB_XFER_BULK:
    case USB_XFER_INTERRUPT: return xhci_submit_bulk_int(urb, d);
    default:
        WARN("xhci: unsupported transfer type %u", urb->transfer_type);
        return -USB_URB_IO_ERROR;
    }
}

/* -------------------------------------------------------------------------- */
/* Transfer-event dispatch (called from xhci.c's event drain)                  */
/* -------------------------------------------------------------------------- */

void xhci_xfer_on_transfer_event(const struct xhci_trb *evt)
{
    if (evt == NULL) return;

    uintptr_t trb_phys = (uintptr_t)evt->param_lo |
                         ((uintptr_t)evt->param_hi << 32);
    uint32_t residual  = evt->status & 0x00FFFFFFu;   /* bits 23:0 */
    uint8_t  cc        = XHCI_CC_GET(evt->status);

    struct xhci_urb_slot *slot = xhci_urb_slot_find_by_trb(trb_phys);
    if (slot == NULL) {
        /* Could be a stale event from a cancelled URB. Control-transfer
         * chains match any TRB in their contiguous Setup/Data/Status
         * range; anything else is genuinely unexpected in Phase 3A. */
        INFO("xhci: unmatched transfer event trb=0x%lx cc=%u",
             (unsigned long)trb_phys, cc);
        return;
    }

    struct usb_urb *urb = slot->urb;
    urb->status        = xhci_cc_to_urb_status(cc);
    /* For control transfers the residual applies to the Data Stage;
     * the Status Stage TRB we IOC'd always has its own status=0 and
     * residual=0. To get bytes transferred for a control transfer we
     * therefore report requested_len — the Data Stage short-packet
     * case raises cc=SHORT_PACKET on the Data TRB, which we currently
     * don't hook (Phase 3A doesn't exercise short control-IN for
     * CDC-ECM). Tracked in #316 for when class drivers beyond
     * CDC-ECM (HID, string descriptors) need the true byte count.
     * For bulk, residual is the byte count not transferred;
     * actual = requested - residual. */
    if (urb->transfer_type == USB_XFER_CONTROL) {
        urb->actual_length = (cc == XHCI_CC_SUCCESS) ? slot->requested_len : 0U;
    } else {
        uint32_t actual = (residual <= slot->requested_len)
                          ? (slot->requested_len - residual)
                          : 0U;
        urb->actual_length = actual;
    }

    if (urb->transfer_type == USB_XFER_CONTROL && cc != XHCI_CC_SUCCESS)
        xhci_log_control_urb("event", urb, slot, trb_phys, cc, residual);

    usb_urb_complete_fn cb = urb->complete;
    xhci_urb_slot_free(slot);
    urb->hcd_private = NULL;
    if (cb) cb(urb);
}

/* -------------------------------------------------------------------------- */
/* cancel + poll                                                               */
/* -------------------------------------------------------------------------- */

int xhci_hcd_cancel_urb(struct usb_urb *urb)
{
    /*
     * Best-effort cancel. Phase 3A's usb_control_msg treats a return
     * of 0 as "cancellation was arranged" — for our polled event path
     * an already-completed URB still has its completion callback fire
     * before the caller's timeout expires, so "cancel" here mostly
     * means "stop tracking this slot and let usb_core see the
     * cancelled state".
     *
     * A spec-correct cancel would issue Stop Endpoint + Set TR
     * Dequeue Pointer, skipping the ring TRB that hasn't completed.
     * We keep the slot cleanup path — if the URB hasn't completed,
     * clear its slot so a future stale transfer event is ignored.
     * Hardware-reset of a stuck endpoint is deferred to a future
     * step; Phase 3A uses cancel only on the stack-URB timeout path
     * of usb_control_msg, and that path runs before the dongle has
     * any reason to stall.
     */
    if (urb == NULL)
        return -1;
    struct xhci_urb_slot *slot = (struct xhci_urb_slot *)urb->hcd_private;
    if (slot == NULL) {
        /* Already completed or never submitted. */
        return 0;
    }
    urb->status       = USB_URB_CANCELLED;
    urb->hcd_private  = NULL;
    xhci_urb_slot_free(slot);
    return 0;
}

void xhci_hcd_poll(void)
{
    if (!xhci_live) return;
    (void)xhci_event_ring_drain();
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

int  xhci_hcd_submit_urb(struct usb_urb *u)  { (void)u; return -1; }
int  xhci_hcd_cancel_urb(struct usb_urb *u)  { (void)u; return -1; }
void xhci_hcd_poll(void)                     { }
void xhci_xfer_on_transfer_event(const struct xhci_trb *e) { (void)e; }

#endif /* PLATFORM_JETSON_ORIN_NANO */
