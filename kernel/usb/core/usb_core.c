/*
 * usb_core.c - SLM-OS USB core (Phase 1 of #266)
 *
 * Provides the HCD abstraction registration, the URB submit/wait path,
 * descriptor parsing, and the root-port enumeration sequence. Linked
 * on every platform; the HCD implementations that register against
 * this core are platform-gated (XHCI on Jetson in Phase 3A).
 *
 * Scope per docs/jetson-usb-networking-plan.md §6:
 *   - single device, no hubs
 *   - high/full speed
 *   - enumeration runs once at boot; no hot-plug
 */

#include "usb.h"
#include "debug.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Module state                                                                */
/*                                                                             */
/* Phase 1 is single-HCD, single-root-device, single-writer. Concurrency       */
/* assumption: usb_core_register_hcd() is called from the primary CPU before   */
/* any secondary CPU comes up (platform init ordering on Jetson and Pi 5),     */
/* and only once per boot. After that, `active_hcd` is effectively read-only,  */
/* so submit / cancel / poll can read it without a lock. enumerate() runs in   */
/* the same init context — nobody races it. Phase 3A XHCI will add a           */
/* spinlock if any post-boot mutation becomes legal (e.g. hot-plug).           */
/* -------------------------------------------------------------------------- */

static const struct usb_hcd *active_hcd;
static struct usb_device     root_device;
static bool                  root_device_present;

/* -------------------------------------------------------------------------- */
/* HCD registration                                                            */
/* -------------------------------------------------------------------------- */

void usb_core_register_hcd(const struct usb_hcd *hcd)
{
    /* Re-registering the same HCD is a no-op (test suites do this on
     * every fixture reset) — skip the warning to keep the log clean.
     * Registering NULL is the documented way to clear the slot, also
     * silent. Only a genuine two-HCD fight earns the warning. */
    if (active_hcd != NULL && hcd != NULL && hcd != active_hcd) {
        WARN("usb_core: HCD '%s' replaces '%s' — only one HCD allowed",
             hcd->name, active_hcd->name);
    }
    active_hcd = hcd;
}

const struct usb_hcd *usb_core_get_hcd(void)
{
    return active_hcd;
}

/* -------------------------------------------------------------------------- */
/* URB helpers                                                                 */
/* -------------------------------------------------------------------------- */

const char *usb_urb_status_str(enum usb_urb_status s)
{
    switch (s) {
    case USB_URB_OK:        return "OK";
    case USB_URB_PENDING:   return "PENDING";
    case USB_URB_CANCELLED: return "CANCELLED";
    case USB_URB_STALL:     return "STALL";
    case USB_URB_TIMEOUT:   return "TIMEOUT";
    case USB_URB_SHORT:     return "SHORT";
    case USB_URB_IO_ERROR:  return "IO_ERROR";
    }
    return "UNKNOWN";
}

/*
 * Contract: 0 on accept, negative on sync error. Never returns a
 * usb_urb_status enum value as a positive number.
 */
int usb_submit_urb(struct usb_urb *urb)
{
    if (urb == NULL || urb->dev == NULL)
        return -USB_URB_IO_ERROR;
    if (active_hcd == NULL || active_hcd->submit_urb == NULL) {
        urb->status = USB_URB_IO_ERROR;
        return -USB_URB_IO_ERROR;
    }

    urb->status = USB_URB_PENDING;
    urb->actual_length = 0;
    int rc = active_hcd->submit_urb(urb);
    /* Normalise: a misbehaving HCD that returns a positive status-enum
     * value is remapped to -USB_URB_IO_ERROR so every caller can do a
     * simple "if (rc < 0)" check. The URB's status is also pulled out
     * of PENDING so a caller who polls urb->status instead of the
     * return value isn't left waiting on a dead transfer. */
    if (rc > 0) {
        urb->status = USB_URB_IO_ERROR;
        return -USB_URB_IO_ERROR;
    }
    return rc;
}

int usb_cancel_urb(struct usb_urb *urb)
{
    if (urb == NULL || active_hcd == NULL || active_hcd->cancel_urb == NULL)
        return -USB_URB_IO_ERROR;
    return active_hcd->cancel_urb(urb);
}

/* -------------------------------------------------------------------------- */
/* Polling                                                                     */
/* -------------------------------------------------------------------------- */

void usb_core_poll(void)
{
    if (active_hcd && active_hcd->poll)
        active_hcd->poll();
}

/* -------------------------------------------------------------------------- */
/* Blocking control transfer                                                   */
/* -------------------------------------------------------------------------- */

/*
 * Spin-wait on urb->status. PLACEHOLDER implementation for Phase 1:
 * the caller's timeout_ms is converted to a fixed iteration cap, not
 * a wall-clock deadline. Good enough for the mock HCD (completes in
 * iteration 0) and for the synchronous control paths exercised by
 * the test suite. Phase 3A XHCI must replace this with a CNTPCT-based
 * deadline before relying on timeout_ms on real hardware — the
 * iteration count on a 2 GHz CPU completes in microseconds, not ms.
 * Each round calls hcd->poll so polled completions are observed even
 * when IRQs are not online.
 */
static int usb_wait_urb(struct usb_urb *urb, uint32_t timeout_ms)
{
    const uint32_t max_rounds = (timeout_ms ? timeout_ms : 1) * 1000u;
    for (uint32_t i = 0; i < max_rounds; i++) {
        usb_core_poll();
        if (urb->status != USB_URB_PENDING)
            return urb->status;
    }
    (void)usb_cancel_urb(urb);
    urb->status = USB_URB_TIMEOUT;
    return USB_URB_TIMEOUT;
}

int usb_control_msg(struct usb_device *dev,
                    uint8_t  bmRequestType,
                    uint8_t  bRequest,
                    uint16_t wValue,
                    uint16_t wIndex,
                    void    *data,
                    uint16_t wLength,
                    uint32_t timeout_ms)
{
    if (dev == NULL)
        return -USB_URB_IO_ERROR;

    struct usb_urb urb = {0};
    urb.dev           = dev;
    urb.endpoint      = 0;     /* control pipe */
    urb.transfer_type = USB_XFER_CONTROL;
    urb.setup.bmRequestType = bmRequestType;
    urb.setup.bRequest      = bRequest;
    urb.setup.wValue        = wValue;
    urb.setup.wIndex        = wIndex;
    urb.setup.wLength       = wLength;
    urb.buffer = data;
    urb.length = wLength;

    int sub = usb_submit_urb(&urb);
    if (sub != 0)
        return sub;

    int status = usb_wait_urb(&urb, timeout_ms);
    if (status == USB_URB_OK || status == USB_URB_SHORT)
        return (int)urb.actual_length;
    return -status;
}

int usb_get_descriptor(struct usb_device *dev,
                       uint8_t  desc_type,
                       uint8_t  desc_index,
                       void    *buf,
                       uint16_t length)
{
    uint16_t wValue = ((uint16_t)desc_type << 8) | desc_index;
    return usb_control_msg(dev,
                           USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                           USB_REQ_GET_DESCRIPTOR,
                           wValue,
                           0,             /* language id / iface index */
                           buf,
                           length,
                           1000 /* ms */);
}

/* -------------------------------------------------------------------------- */
/* Descriptor parsing                                                          */
/* -------------------------------------------------------------------------- */

int usb_parse_configuration(struct usb_device *dev)
{
    if (dev == NULL || dev->raw_config_len < sizeof(struct usb_config_descriptor))
        return -1;
    /* Defensive cap: usb_core_enumerate already truncates to this size
     * before writing raw_config_len, but a direct caller (tests, future
     * class-driver probing) could set a larger value. Reject rather
     * than walk `end` past the buffer into adjacent struct fields. */
    if (dev->raw_config_len > sizeof(dev->raw_config))
        return -1;

    const uint8_t *p   = dev->raw_config;
    const uint8_t *end = p + dev->raw_config_len;

    /* Zero out the parsed tables — enumerate_config may be re-called. */
    for (unsigned i = 0; i < USB_MAX_INTERFACES_PER_DEV; i++) {
        dev->ifaces[i].valid = false;
        for (unsigned e = 0; e < USB_MAX_ENDPOINTS_PER_DEV; e++)
            dev->ifaces[i].ep_index[e] = -1;
    }
    for (unsigned e = 0; e < USB_MAX_ENDPOINTS_PER_DEV; e++)
        dev->endpoints[e].valid = false;

    /* The CONFIGURATION descriptor is first. */
    const struct usb_config_descriptor *cfg = (const void *)p;
    if (cfg->bDescriptorType != USB_DT_CONFIG)
        return -1;
    /* USB 2.0 §9.6.3 fixes the config-descriptor header length at 9. A
     * device that reports something else is malformed; bailing here
     * avoids advancing `p` into garbage. */
    if (cfg->bLength != sizeof(struct usb_config_descriptor))
        return -1;

    /* Walk class/standard descriptors after the CONFIGURATION header. */
    int cur_iface_slot = -1;
    int ep_next_slot   = 0;
    p += cfg->bLength;

    while (p + 2 <= end) {
        uint8_t blen = p[0];
        uint8_t btype = p[1];
        if (blen < 2 || p + blen > end)
            break;

        switch (btype) {
        case USB_DT_INTERFACE: {
            if (blen < sizeof(struct usb_interface_descriptor))
                break;
            const struct usb_interface_descriptor *id = (const void *)p;

            /* Skip alternate settings — Phase 1 only binds default (alt=0). */
            if (id->bAlternateSetting != 0) {
                cur_iface_slot = -1;
                break;
            }

            cur_iface_slot = -1;
            for (unsigned i = 0; i < USB_MAX_INTERFACES_PER_DEV; i++) {
                if (!dev->ifaces[i].valid) {
                    cur_iface_slot = (int)i;
                    break;
                }
            }
            if (cur_iface_slot < 0) {
                WARN("usb_core: no iface slot for ifnum=%u", id->bInterfaceNumber);
                break;
            }
            struct usb_interface *slot = &dev->ifaces[cur_iface_slot];
            slot->valid         = true;
            slot->number        = id->bInterfaceNumber;
            slot->alt_setting   = id->bAlternateSetting;
            slot->num_endpoints = id->bNumEndpoints;
            slot->class_code    = id->bInterfaceClass;
            slot->subclass      = id->bInterfaceSubClass;
            slot->protocol      = id->bInterfaceProtocol;
            break;
        }

        case USB_DT_ENDPOINT: {
            if (blen < sizeof(struct usb_endpoint_descriptor))
                break;
            if (cur_iface_slot < 0) {
                /* Orphan endpoint — should not happen on a sane device. */
                break;
            }
            const struct usb_endpoint_descriptor *ed = (const void *)p;

            if (ep_next_slot >= USB_MAX_ENDPOINTS_PER_DEV) {
                WARN("usb_core: too many endpoints (> %d)",
                     USB_MAX_ENDPOINTS_PER_DEV);
                break;
            }
            struct usb_endpoint *ep = &dev->endpoints[ep_next_slot];
            ep->valid       = true;
            ep->address     = ed->bEndpointAddress;
            ep->attributes  = ed->bmAttributes;
            ep->max_packet  = ed->wMaxPacketSize & 0x07FF;
            ep->interval    = ed->bInterval;

            /* Record the endpoint index in the interface's ep table. */
            struct usb_interface *slot = &dev->ifaces[cur_iface_slot];
            for (unsigned e = 0; e < USB_MAX_ENDPOINTS_PER_DEV; e++) {
                if (slot->ep_index[e] < 0) {
                    slot->ep_index[e] = (int8_t)ep_next_slot;
                    break;
                }
            }
            ep_next_slot++;
            break;
        }

        default:
            /* Class-specific + vendor-specific descriptors pass through. */
            break;
        }

        p += blen;
    }

    return 0;
}

const struct usb_endpoint *
usb_find_endpoint(const struct usb_device *dev,
                  uint8_t interface_number,
                  uint8_t direction,
                  uint8_t xfer_type)
{
    if (dev == NULL)
        return NULL;

    for (unsigned i = 0; i < USB_MAX_INTERFACES_PER_DEV; i++) {
        const struct usb_interface *slot = &dev->ifaces[i];
        if (!slot->valid || slot->number != interface_number)
            continue;
        for (unsigned e = 0; e < USB_MAX_ENDPOINTS_PER_DEV; e++) {
            int8_t idx = slot->ep_index[e];
            if (idx < 0) continue;
            const struct usb_endpoint *ep = &dev->endpoints[idx];
            if (!ep->valid) continue;
            if ((ep->address & USB_DIR_IN) != (direction & USB_DIR_IN))
                continue;
            if ((ep->attributes & USB_XFER_TYPE_MASK) != (xfer_type & USB_XFER_TYPE_MASK))
                continue;
            return ep;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Device enumeration                                                          */
/* -------------------------------------------------------------------------- */

struct usb_device *usb_core_first_device(void)
{
    return root_device_present ? &root_device : NULL;
}

/*
 * Root-port enumeration.
 *
 *   1. port_reset()           — bus reset; device lands at address 0
 *   2. device_open()          — HCD opens default-pipe slot
 *   3. GET_DESCRIPTOR(device, 8 bytes) — learn max packet on EP0
 *   4. SET_ADDRESS             — move to a non-zero address
 *   5. GET_DESCRIPTOR(device, 18 bytes) — full device descriptor
 *   6. GET_DESCRIPTOR(config, 9 bytes)  — learn wTotalLength
 *   7. GET_DESCRIPTOR(config, wTotalLength) — full config tree
 *   8. usb_parse_configuration — populate ifaces/endpoints
 *   9. SET_CONFIGURATION      — activate the default config
 *  10. endpoint_configure     — HCD commits non-EP0 endpoint contexts
 *
 * Errors after device_open() fall through to an err_close label that
 * invokes hcd->device_close(). Without this, a Phase-3A XHCI driver
 * that allocates a slot context in device_open would leak it on every
 * failed enumeration.
 */
int usb_core_enumerate(void)
{
    /* Start from a clean slate — a retry after a previous failure must
     * not leave the stale root_device visible via usb_core_first_device(). */
    root_device_present = false;

    if (active_hcd == NULL) {
        WARN("usb_core: no HCD registered");
        return -1;
    }

    bool connected = false;
    enum usb_speed speed = USB_SPEED_UNKNOWN;
    if (!active_hcd->port_status ||
        !active_hcd->port_status(0, &connected, &speed) ||
        !connected) {
        INFO("usb_core: no device on root port");
        root_device_present = false;
        return 0;
    }

    memset(&root_device, 0, sizeof(root_device));
    root_device.hcd   = active_hcd;
    root_device.speed = speed;
    root_device.state = USB_STATE_ATTACHED;

    if (active_hcd->port_reset && active_hcd->port_reset(0) != 0) {
        WARN("usb_core: port_reset failed");
        return -1;   /* device_open has not run yet — no state to undo. */
    }
    root_device.state = USB_STATE_DEFAULT;

    bool device_opened = false;
    if (active_hcd->device_open) {
        if (active_hcd->device_open(&root_device) != 0) {
            WARN("usb_core: device_open failed");
            return -1;
        }
        device_opened = true;
    }

    int n;
    int rc;

    /*
     * Step 3: read the first 8 bytes of the device descriptor. Some
     * devices lie about bMaxPacketSize0 until they've seen the first
     * IN — Linux does this same two-step for the same reason.
     */
    uint8_t dd_stub[8];
    n = usb_get_descriptor(&root_device, USB_DT_DEVICE, 0, dd_stub, 8);
    if (n < 8) {
        WARN("usb_core: short GET_DESCRIPTOR(device, 8) n=%d", n);
        goto err_close;
    }
    /* bMaxPacketSize0 is byte 7. Record it for the HCD if useful later. */
    root_device.dev_desc.bMaxPacketSize0 = dd_stub[7];

    /* Step 4: assign address 1 (single-device policy). */
    rc = usb_control_msg(&root_device,
                         USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                         USB_REQ_SET_ADDRESS,
                         1 /* wValue = address */, 0, NULL, 0, 500);
    if (rc < 0) {
        WARN("usb_core: SET_ADDRESS failed: %s",
             usb_urb_status_str((enum usb_urb_status)(-rc)));
        goto err_close;
    }
    root_device.address = 1;
    root_device.state   = USB_STATE_ADDRESS;

    /* Step 5: full device descriptor. */
    n = usb_get_descriptor(&root_device, USB_DT_DEVICE, 0,
                           &root_device.dev_desc,
                           sizeof(root_device.dev_desc));
    if (n < (int)sizeof(root_device.dev_desc)) {
        WARN("usb_core: short GET_DESCRIPTOR(device) n=%d", n);
        goto err_close;
    }

    /* Step 6: 9-byte config header to learn wTotalLength. */
    struct usb_config_descriptor cfg_head;
    n = usb_get_descriptor(&root_device, USB_DT_CONFIG, 0,
                           &cfg_head, sizeof(cfg_head));
    if (n < (int)sizeof(cfg_head)) {
        WARN("usb_core: short GET_DESCRIPTOR(config, 9) n=%d", n);
        goto err_close;
    }
    uint16_t total = cfg_head.wTotalLength;
    if (total > USB_MAX_CONFIG_DESC_BYTES) {
        WARN("usb_core: config total %u > cap %u — truncating",
             total, USB_MAX_CONFIG_DESC_BYTES);
        total = USB_MAX_CONFIG_DESC_BYTES;
    }

    /* Step 7: full config tree. */
    n = usb_get_descriptor(&root_device, USB_DT_CONFIG, 0,
                           root_device.raw_config, total);
    if (n < (int)total) {
        WARN("usb_core: short GET_DESCRIPTOR(config, %u) n=%d", total, n);
        goto err_close;
    }
    root_device.raw_config_len = total;

    /* Step 8: parse. */
    if (usb_parse_configuration(&root_device) != 0) {
        WARN("usb_core: parse_configuration failed");
        goto err_close;
    }

    /* Step 9: activate the default configuration. */
    uint8_t cfg_val = cfg_head.bConfigurationValue;
    rc = usb_control_msg(&root_device,
                         USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                         USB_REQ_SET_CONFIGURATION,
                         cfg_val, 0, NULL, 0, 500);
    if (rc < 0) {
        WARN("usb_core: SET_CONFIGURATION failed: %s",
             usb_urb_status_str((enum usb_urb_status)(-rc)));
        goto err_close;
    }
    root_device.current_config = cfg_val;
    root_device.state = USB_STATE_CONFIGURED;

    /* Step 10: HCD commits non-EP0 endpoint contexts. */
    if (active_hcd->endpoint_configure) {
        for (unsigned e = 0; e < USB_MAX_ENDPOINTS_PER_DEV; e++) {
            if (!root_device.endpoints[e].valid) continue;
            int ec = active_hcd->endpoint_configure(&root_device,
                                                    &root_device.endpoints[e]);
            if (ec != 0) {
                WARN("usb_core: endpoint_configure(ep 0x%02x) failed: %d",
                     root_device.endpoints[e].address, ec);
                goto err_close;
            }
        }
    }

    root_device_present = true;
    INFO("usb_core: device configured — vid=0x%04x pid=0x%04x class=0x%02x",
         root_device.dev_desc.idVendor,
         root_device.dev_desc.idProduct,
         root_device.dev_desc.bDeviceClass);
    return 0;

err_close:
    if (device_opened && active_hcd->device_close)
        active_hcd->device_close(&root_device);
    return -1;
}

int usb_core_start(void)
{
    if (active_hcd == NULL) {
        INFO("usb_core: no HCD registered — USB disabled");
        return 0;
    }
    if (active_hcd->start) {
        int rc = active_hcd->start();
        if (rc != 0) {
            WARN("usb_core: HCD '%s' start failed: %d", active_hcd->name, rc);
            return rc;
        }
    }
    return usb_core_enumerate();
}
