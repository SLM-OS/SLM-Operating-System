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
#include "vmm.h"

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
static bool xhci_probe_advance_adopted_devctx_after_first_short = false;
static bool xhci_probe_switch_adopted_dcbaa_after_first_short = false;
static bool xhci_probe_noop_after_first_short = true;

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

static uint32_t xhci_ep0_mps_for_speed(enum usb_speed speed)
{
    switch (speed) {
    case USB_SPEED_LOW:
        return 8;
    case USB_SPEED_FULL:
        return 64;
    case USB_SPEED_HIGH:
        return 64;
    case USB_SPEED_SUPER:
        return 512;
    default:
        return 8;
    }
}

static uint32_t xhci_td_size_for_single_data_trb(const struct usb_urb *urb)
{
    if (urb == NULL || urb->length == 0 || urb->buffer == NULL)
        return 0;

    uint32_t mps = xhci_ep0_mps_for_speed(urb->dev->speed);
    if (mps == 0)
        return 0;

    /*
     * Match Linux's xhci_td_remainder() rule for the common control-TD
     * shape we use here: one Data Stage TRB carrying the entire payload.
     * In that case the last data TRB's TD size is zero, even though the
     * Status Stage TRB still follows in the TD.
     */
    if (urb->length <= 31U * mps)
        return 0;

    /*
     * Longer control payloads are out of scope today. Keep the helper
     * safe if we ever reach them before teaching the driver to split
     * data across multiple TRBs.
     */
    uint32_t packets = (urb->length + mps - 1U) / mps;
    return packets > 31U ? 31U : packets;
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

static void xhci_prepare_first_control_transfer(const struct usb_urb *urb,
                                                const struct xhci_device *d)
{
    if (urb == NULL || d == NULL)
        return;
    if (!xhci_urb_is_get_descriptor(urb))
        return;

    uint32_t before = xhci_op_r32(XHCI_OP_USBSTS);
    int drained = xhci_event_ring_drain();
    uint32_t ack = before & (XHCI_STS_EINT | XHCI_STS_PCD);
    if (ack != 0)
        xhci_op_w32(XHCI_OP_USBSTS, ack);
    uint32_t after = xhci_op_r32(XHCI_OP_USBSTS);

    INFO("xhci: pre-EP0 cleanup slot=%u drained=%d usbsts=0x%08x ack=0x%08x -> 0x%08x",
         (unsigned)d->slot_id, drained,
         (unsigned)before, (unsigned)ack, (unsigned)after);

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

static void xhci_log_post_short_control_state(const struct usb_urb *urb,
                                              const struct xhci_urb_slot *slot,
                                              uintptr_t event_trb_phys,
                                              uint8_t cc)
{
    if (urb == NULL || slot == NULL || slot->ring == NULL)
        return;
    if (urb->transfer_type != USB_XFER_CONTROL || cc != XHCI_CC_SHORT_PACKET)
        return;
    if (event_trb_phys != slot->data_trb_phys)
        return;

    struct xhci_device *d = (struct xhci_device *)urb->dev->hcd_private;
    if (d == NULL || d->slot_id == 0)
        return;

    struct xhci_ring *r = slot->ring;
    uintptr_t next_trb_phys = r->phys + (uintptr_t)r->enqueue * sizeof(struct xhci_trb);
    uint32_t ring_ctrl = 0;
    if (r->trbs != NULL && r->enqueue < r->num_trbs)
        ring_ctrl = r->trbs[r->enqueue].control;

    INFO("xhci: post-short ctrl slot=%u dci=%u adopted=%u next=0x%lx enqueue=%u pcs=%u "
         "first=0x%lx data=0x%lx last=0x%lx ctrl=0x%08x",
         (unsigned)d->slot_id, (unsigned)slot->dci,
         (unsigned)d->adopted_inherited,
         (unsigned long)next_trb_phys,
         (unsigned)r->enqueue, (unsigned)r->cycle_state,
         (unsigned long)slot->first_trb_phys,
         (unsigned long)slot->data_trb_phys,
         (unsigned long)slot->last_trb_phys,
         (unsigned)ring_ctrl);

    if (d->dev_ctx != NULL) {
        bool cz = xhci_caps_cached.ctx_64;
        uint32_t slot0 = *xhci_dev_slot_dw(d->dev_ctx, 0);
        uint32_t slot1 = *xhci_dev_slot_dw(d->dev_ctx, 1);
        uint32_t slot3 = *xhci_dev_slot_dw(d->dev_ctx, 3);
        uint32_t ep00  = *xhci_dev_ep_dw(d->dev_ctx, XHCI_DCI_EP0, 0, cz);
        uint32_t ep01  = *xhci_dev_ep_dw(d->dev_ctx, XHCI_DCI_EP0, 1, cz);
        uint32_t ep02  = *xhci_dev_ep_dw(d->dev_ctx, XHCI_DCI_EP0, 2, cz);
        uint32_t ep03  = *xhci_dev_ep_dw(d->dev_ctx, XHCI_DCI_EP0, 3, cz);

        INFO("xhci: post-short devctx slot route=0x%x speed=%u root=%u addr=%u state=%u "
             "dw0=0x%08x dw1=0x%08x dw3=0x%08x",
             (unsigned)(slot0 & XHCI_SLOT_DW0_ROUTE_MASK),
             (unsigned)((slot0 & XHCI_SLOT_DW0_SPEED_MASK) >> XHCI_SLOT_DW0_SPEED_SHIFT),
             (unsigned)((slot1 & XHCI_SLOT_DW1_ROOT_PORT_MASK) >> XHCI_SLOT_DW1_ROOT_PORT_SHIFT),
             (unsigned)(slot3 & XHCI_SLOT_DW3_ADDR_MASK),
             (unsigned)((slot3 & XHCI_SLOT_DW3_STATE_MASK) >> XHCI_SLOT_DW3_STATE_SHIFT),
             (unsigned)slot0, (unsigned)slot1, (unsigned)slot3);
        INFO("xhci: post-short devctx ep0 state=%u type=%u mps=%u tr=0x%lx dcs=%u "
             "dw0=0x%08x dw1=0x%08x dw2=0x%08x dw3=0x%08x",
             (unsigned)(ep00 & XHCI_EP_DW0_STATE_MASK),
             (unsigned)((ep01 & XHCI_EP_DW1_EPTYPE_MASK) >> XHCI_EP_DW1_EPTYPE_SHIFT),
             (unsigned)((ep01 & XHCI_EP_DW1_MAXPKT_MASK) >> XHCI_EP_DW1_MAXPKT_SHIFT),
             (unsigned long)((((uint64_t)ep03) << 32) | (ep02 & ~0xFULL)),
             (unsigned)(ep02 & 0x1U),
             (unsigned)ep00, (unsigned)ep01, (unsigned)ep02, (unsigned)ep03);
    }

    xhci_dump_runtime_state_slot("post-short", d->slot_id);

}

static void xhci_log_adopted_submit_state(const struct usb_urb *urb,
                                          const struct xhci_device *d,
                                          const struct xhci_ring *r)
{
    if (urb == NULL || d == NULL || r == NULL)
        return;
    if (!d->adopted_inherited || urb->transfer_type != USB_XFER_CONTROL)
        return;

    uintptr_t next_trb_phys = r->phys + (uintptr_t)r->enqueue * sizeof(struct xhci_trb);
    uint32_t ring_ctrl = 0;
    if (r->trbs != NULL && r->enqueue < r->num_trbs)
        ring_ctrl = r->trbs[r->enqueue].control;

    INFO("xhci: adopted submit slot=%u addr=%u req=0x%02x len=%u next=0x%lx enqueue=%u pcs=%u ctrl=0x%08x",
         (unsigned)d->slot_id,
         (unsigned)urb->dev->address,
         (unsigned)urb->setup.bRequest,
         (unsigned)urb->setup.wLength,
         (unsigned long)next_trb_phys,
         (unsigned)r->enqueue,
         (unsigned)r->cycle_state,
         (unsigned)ring_ctrl);

    xhci_dump_runtime_state_slot("pre-submit", d->slot_id);

    if (d->dev_ctx != NULL) {
        bool cz = xhci_caps_cached.ctx_64;
        uint32_t slot0 = *xhci_dev_slot_dw(d->dev_ctx, 0);
        uint32_t slot1 = *xhci_dev_slot_dw(d->dev_ctx, 1);
        uint32_t slot3 = *xhci_dev_slot_dw(d->dev_ctx, 3);
        uint32_t ep00  = *xhci_dev_ep_dw(d->dev_ctx, XHCI_DCI_EP0, 0, cz);
        uint32_t ep01  = *xhci_dev_ep_dw(d->dev_ctx, XHCI_DCI_EP0, 1, cz);
        uint32_t ep02  = *xhci_dev_ep_dw(d->dev_ctx, XHCI_DCI_EP0, 2, cz);
        uint32_t ep03  = *xhci_dev_ep_dw(d->dev_ctx, XHCI_DCI_EP0, 3, cz);

        INFO("xhci: adopted submit devctx slot route=0x%x speed=%u root=%u addr=%u state=%u "
             "dw0=0x%08x dw1=0x%08x dw3=0x%08x",
             (unsigned)(slot0 & XHCI_SLOT_DW0_ROUTE_MASK),
             (unsigned)((slot0 & XHCI_SLOT_DW0_SPEED_MASK) >> XHCI_SLOT_DW0_SPEED_SHIFT),
             (unsigned)((slot1 & XHCI_SLOT_DW1_ROOT_PORT_MASK) >> XHCI_SLOT_DW1_ROOT_PORT_SHIFT),
             (unsigned)(slot3 & XHCI_SLOT_DW3_ADDR_MASK),
             (unsigned)((slot3 & XHCI_SLOT_DW3_STATE_MASK) >> XHCI_SLOT_DW3_STATE_SHIFT),
             (unsigned)slot0, (unsigned)slot1, (unsigned)slot3);
        INFO("xhci: adopted submit devctx ep0 state=%u type=%u mps=%u tr=0x%lx dcs=%u "
             "dw0=0x%08x dw1=0x%08x dw2=0x%08x dw3=0x%08x",
             (unsigned)(ep00 & XHCI_EP_DW0_STATE_MASK),
             (unsigned)((ep01 & XHCI_EP_DW1_EPTYPE_MASK) >> XHCI_EP_DW1_EPTYPE_SHIFT),
             (unsigned)((ep01 & XHCI_EP_DW1_MAXPKT_MASK) >> XHCI_EP_DW1_MAXPKT_SHIFT),
             (unsigned long)((((uint64_t)ep03) << 32) | (ep02 & ~0xFULL)),
             (unsigned)(ep02 & 0x1U),
             (unsigned)ep00, (unsigned)ep01, (unsigned)ep02, (unsigned)ep03);
    }
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
    uintptr_t data_phys = 0;
    uint32_t trt  = 0U;
    /*
     * xHCI 1.0+ consumes the Setup Stage TRT field for control transfers
     * with a data phase. Leaving TRT at "No Data" on a v1.20 controller
     * makes our GET_DESCRIPTOR path diverge from Linux's queueing rules
     * before the data stage ever starts.
     */
    if (xhci_caps_cached.hci_version >= 0x0100 && has_data)
        trt = data_in ? 3U : 2U;

    if (has_data) {
        data_phys = (uintptr_t)vmm_virt_to_phys((uintptr_t)urb->buffer);
        if (data_phys == 0) {
            WARN("xhci: submit_control unmapped data buffer virt=0x%lx len=%u",
                 (unsigned long)(uintptr_t)urb->buffer,
                 (unsigned)urb->length);
            return -USB_URB_IO_ERROR;
        }
    }

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
        INFO("xhci: ctrl setup packet bmRequestType=0x%02x bRequest=0x%02x "
             "wValue=0x%04x wIndex=0x%04x wLength=%u trt=%u has_data=%u data_in=%u",
             (unsigned)urb->setup.bmRequestType,
             (unsigned)urb->setup.bRequest,
             (unsigned)urb->setup.wValue,
             (unsigned)urb->setup.wIndex,
             (unsigned)urb->setup.wLength,
             (unsigned)trt,
             (unsigned)has_data,
             (unsigned)data_in);
    }

    xhci_log_adopted_submit_state(urb, d, r);

    xhci_prepare_first_control_transfer(urb, d);

    struct xhci_trb tmpl;
    struct xhci_trb *slot_trb;
    /* 1. Setup Stage. */
    xhci_build_setup_stage(&tmpl, &urb->setup, trt, 0);
    if (xhci_urb_is_get_descriptor(urb)) {
        INFO("xhci: setup trb param_lo=0x%08x param_hi=0x%08x status=0x%08x "
             "control=0x%08x",
             (unsigned)tmpl.param_lo, (unsigned)tmpl.param_hi,
             (unsigned)tmpl.status, (unsigned)tmpl.control);
    }
    slot_trb = xhci_ring_put(r, &tmpl);
    if (slot_trb == NULL) goto fail;
    slot->first_trb_phys = (uintptr_t)slot_trb;

    /* 2. Optional Data Stage. */
    if (has_data) {
        xhci_build_data_stage(&tmpl, data_phys,
                              urb->length, data_in, 0);
        tmpl.status |= xhci_td_size_for_single_data_trb(urb)
                       << XHCI_TRB_STATUS_TD_SIZE_SHIFT;
        if (xhci_urb_is_get_descriptor(urb)) {
            INFO("xhci: data trb param_lo=0x%08x param_hi=0x%08x status=0x%08x "
                 "control=0x%08x buf_virt=0x%lx buf_phys=0x%lx",
                 (unsigned)tmpl.param_lo, (unsigned)tmpl.param_hi,
                 (unsigned)tmpl.status, (unsigned)tmpl.control,
                 (unsigned long)(uintptr_t)urb->buffer,
                 (unsigned long)data_phys);
        }
        slot_trb = xhci_ring_put(r, &tmpl);
        if (slot_trb == NULL) goto fail;
        slot->data_trb_phys = (uintptr_t)slot_trb;
    }

    /* 3. Status Stage — Direction is opposite of the Data Stage. For
     *    a no-data control transfer it must be IN per §4.11.2.2. */
    bool status_in = has_data ? !data_in : true;
    xhci_build_status_stage(&tmpl, status_in, 0);
    if (xhci_urb_is_get_descriptor(urb)) {
        INFO("xhci: status trb status=0x%08x control=0x%08x status_dir_in=%u",
             (unsigned)tmpl.status, (unsigned)tmpl.control,
             (unsigned)status_in);
    }
    slot_trb = xhci_ring_put(r, &tmpl);
    if (slot_trb == NULL) goto fail;
    slot->last_trb_phys = (uintptr_t)slot_trb;

    urb->hcd_private    = slot;

    /* Kick EP0. */
    xhci_ring_doorbell(d->slot_id, XHCI_DCI_EP0);
    if (urb->transfer_type == USB_XFER_CONTROL)
        xhci_dump_runtime_state_slot("post-doorbell", d->slot_id);
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

    uintptr_t buf_phys = (uintptr_t)vmm_virt_to_phys((uintptr_t)urb->buffer);
    if (buf_phys == 0) {
        WARN("xhci: submit bulk/int unmapped buffer virt=0x%lx len=%u dci=%u",
             (unsigned long)(uintptr_t)urb->buffer,
             (unsigned)urb->length,
             dci);
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
    xhci_build_normal(&tmpl, buf_phys, urb->length, 0);
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

int xhci_hcd_submit_urb(struct usb_urb *urb)
{
    if (!xhci_live || urb == NULL || urb->dev == NULL)
        return -USB_URB_IO_ERROR;
    struct xhci_device *d = (struct xhci_device *)urb->dev->hcd_private;
    if (d == NULL || d->slot_id == 0)
        return -USB_URB_IO_ERROR;

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
    uint8_t  slot_id   = XHCI_TRB_SLOT_GET(evt->control);
    uint8_t  ep_id     = (uint8_t)((evt->control >> XHCI_TRB_EP_SHIFT) & 0x1Fu);

    struct xhci_urb_slot *slot = xhci_urb_slot_find_by_trb(trb_phys);
    if (slot == NULL) {
        /* Could be a stale event from a cancelled URB. Control-transfer
         * chains match any TRB in their contiguous Setup/Data/Status
         * range; anything else is genuinely unexpected in Phase 3A. */
        INFO("xhci: unmatched transfer event trb=0x%lx cc=%u slot=%u ep=%u "
             "status=0x%08x control=0x%08x residual=%u",
             (unsigned long)trb_phys, cc,
             (unsigned)slot_id, (unsigned)ep_id,
             (unsigned)evt->status, (unsigned)evt->control,
             (unsigned)residual);
        return;
    }

    struct usb_urb *urb = slot->urb;
    urb->status        = xhci_cc_to_urb_status(cc);
    /* The transfer-event residual reports the byte count not transferred
     * for the TRB that completed. For bulk transfers that is the full
     * payload. For control transfers the IOC event may arrive on the Data
     * Stage (short packet/error) or on the Status Stage (success). In both
     * cases the requested payload is slot->requested_len, so the same
     * "requested - residual" rule gives the right byte count: short
     * control-IN data completions surface the bytes received, while a
     * successful Status Stage with residual=0 reports the full request
     * length. */
    uint32_t actual = (residual <= slot->requested_len)
                      ? (slot->requested_len - residual)
                      : 0U;
    urb->actual_length = actual;

    if (urb->transfer_type == USB_XFER_CONTROL && cc != XHCI_CC_SUCCESS)
        xhci_log_control_urb("event", urb, slot, trb_phys, cc, residual);

    xhci_log_post_short_control_state(urb, slot, trb_phys, cc);

    if (xhci_probe_advance_adopted_devctx_after_first_short &&
        urb->transfer_type == USB_XFER_CONTROL &&
        cc == XHCI_CC_SHORT_PACKET &&
        trb_phys == slot->data_trb_phys) {
        struct xhci_device *d = (struct xhci_device *)urb->dev->hcd_private;
        if (d != NULL && d->adopted_inherited && d->dev_ctx != NULL &&
            slot->ring != NULL) {
            bool cz = xhci_caps_cached.ctx_64;
            uintptr_t next_trb_phys = slot->ring->phys +
                                      (uintptr_t)slot->ring->enqueue *
                                      sizeof(struct xhci_trb);
            uint32_t *ep02 = xhci_dev_ep_dw(d->dev_ctx, XHCI_DCI_EP0, 2, cz);
            uint32_t *ep03 = xhci_dev_ep_dw(d->dev_ctx, XHCI_DCI_EP0, 3, cz);
            *ep02 = (uint32_t)(next_trb_phys & 0xFFFFFFFFu) | 0x1U;
            *ep03 = (uint32_t)(next_trb_phys >> 32);
            xhci_probe_advance_adopted_devctx_after_first_short = false;
            INFO("xhci: advanced adopted devctx ep0 tr to next ring slot "
                 "(slot=%u next=0x%lx)",
                 (unsigned)d->slot_id,
                 (unsigned long)next_trb_phys);
        }
    }

    if (xhci_probe_switch_adopted_dcbaa_after_first_short &&
        urb->transfer_type == USB_XFER_CONTROL &&
        cc == XHCI_CC_SHORT_PACKET &&
        trb_phys == slot->data_trb_phys) {
        struct xhci_device *d = (struct xhci_device *)urb->dev->hcd_private;
        if (d != NULL && d->adopted_inherited && d->slot_id != 0 &&
            d->dev_ctx_phys != 0) {
            uint8_t adopted_slot = d->slot_id;
            xhci_dcbaa[adopted_slot] = (uint64_t)d->dev_ctx_phys;
            dsb(sy);
            xhci_probe_switch_adopted_dcbaa_after_first_short = false;
            INFO("xhci: switched adopted slot %u DCBAA to local devctx mirror "
                 "after first short packet (0x%lx)",
                 (unsigned)adopted_slot,
                 (unsigned long)d->dev_ctx_phys);
        }
    }

    if (xhci_probe_noop_after_first_short &&
        urb->transfer_type == USB_XFER_CONTROL &&
        cc == XHCI_CC_SHORT_PACKET &&
        trb_phys == slot->data_trb_phys) {
        struct xhci_device *d = (struct xhci_device *)urb->dev->hcd_private;
        xhci_probe_noop_after_first_short = false;
        if (d != NULL) {
            INFO("xhci: deferring post-short NO_OP until after event retirement "
                 "(slot=%u)", (unsigned)d->slot_id);
            xhci_defer_post_short_noop(d->slot_id);
        }
    }

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
    if (urb->transfer_type == USB_XFER_CONTROL && urb->dev != NULL) {
        struct xhci_device *d = (struct xhci_device *)urb->dev->hcd_private;
        if (d != NULL && d->adopted_inherited) {
            INFO("xhci: cancel_urb on adopted control slot=%u req=0x%02x len=%u trb=0x%lx",
                 (unsigned)d->slot_id,
                 (unsigned)urb->setup.bRequest,
                 (unsigned)urb->length,
                 (unsigned long)slot->last_trb_phys);
            xhci_dump_runtime_state_slot("cancel-adopted", d->slot_id);
            xhci_dump_device_state();
        }
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
    xhci_run_deferred_probes();
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

int  xhci_hcd_submit_urb(struct usb_urb *u)  { (void)u; return -1; }
int  xhci_hcd_cancel_urb(struct usb_urb *u)  { (void)u; return -1; }
void xhci_hcd_poll(void)                     { }
void xhci_xfer_on_transfer_event(const struct xhci_trb *e) { (void)e; }

#endif /* PLATFORM_JETSON_ORIN_NANO */
