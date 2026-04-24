/*
 * usb_core.c - SLM-OS USB core (Phase 1 of #266)
 *
 * Provides the HCD abstraction registration, the URB submit/wait path,
 * descriptor parsing, and the root-port enumeration sequence. Linked
 * on every platform; the HCD implementations that register against
 * this core are platform-gated (XHCI on Jetson in Phase 3A).
 *
 * Scope per docs/jetson-usb-networking-plan.md §6:
 *   - single exposed device
 *   - one upstream USB 2.0 hub allowed as an internal transport detail
 *   - high/full speed
 *   - enumeration runs once at boot; no hot-plug
 */

#include "usb.h"
#include "debug.h"
#include "ncmem.h"
#include "timer.h"
#include <string.h>

#if defined(PLATFORM_JETSON_ORIN_NANO)
int xhci_cmd_noop_probe(void);
int xhci_debug_reprime_adopted_ep0(struct usb_device *dev);
int xhci_sync_child_address_bsr0(struct usb_device *dev);
#endif

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
static struct usb_device     hub_device;
static bool                  hub_device_present;
static bool                  usb_disable_hotplug_retry_after_failure = true;
static bool                  usb_child_address_sync_bsr0 = true;
static bool                  usb_child_initial_desc_bounce = true;
static bool                  usb_child_followup_desc_bounce = true;
static bool                  usb_hotplug_retry_blocked;
static bool                  usb_hotplug_retry_blocked_logged;
static uint8_t              *usb_retained_desc_bounce;
static size_t                usb_retained_desc_bounce_len;

static void *usb_get_retained_desc_bounce(size_t min_len)
{
#if defined(PLATFORM_HAS_NC_MEMORY)
    if (usb_retained_desc_bounce_len < min_len) {
        size_t alloc_len = (min_len < 256u) ? 256u : min_len;
        void *buf = ncmem_alloc(alloc_len, 64);
        if (buf == NULL) {
            WARN("usb_core: retained descriptor bounce alloc failed (%u bytes)",
                 (unsigned)alloc_len);
            return NULL;
        }
        memset(buf, 0, alloc_len);
        usb_retained_desc_bounce = (uint8_t *)buf;
        usb_retained_desc_bounce_len = alloc_len;
        INFO("usb_core: retained descriptor bounce @0x%lx len=%u",
             (unsigned long)(uintptr_t)buf,
             (unsigned)alloc_len);
    }
    return usb_retained_desc_bounce;
#else
    (void)min_len;
    return NULL;
#endif
}

static bool usb_is_root_device(const struct usb_device *dev)
{
    return dev != NULL && dev->route_string == 0;
}

static bool usb_config_is_cdc_ecm_candidate(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len < sizeof(struct usb_config_descriptor))
        return false;

    bool have_ctrl = false;
    bool have_data = false;
    const uint8_t *p = buf + sizeof(struct usb_config_descriptor);
    const uint8_t *end = buf + len;

    while (p + 2 <= end) {
        uint8_t blen = p[0];
        uint8_t btype = p[1];
        if (blen < 2 || p + blen > end)
            break;

        if (btype == USB_DT_INTERFACE &&
            blen >= sizeof(struct usb_interface_descriptor)) {
            const struct usb_interface_descriptor *id = (const void *)p;
            if (id->bInterfaceClass == 0x02 && id->bInterfaceSubClass == 0x06)
                have_ctrl = true;
            if (id->bInterfaceClass == 0x0A)
                have_data = true;
        }

        p += blen;
    }

    return have_ctrl && have_data;
}

static int usb_fetch_config_descriptor(struct usb_device *dev,
                                       uint8_t cfg_index,
                                       struct usb_config_descriptor *cfg_head,
                                       uint8_t *cfg_buf,
                                       size_t cfg_buf_cap)
{
    if (dev == NULL || cfg_head == NULL || cfg_buf == NULL ||
        cfg_buf_cap < sizeof(struct usb_config_descriptor))
        return -1;

    struct usb_config_descriptor *cfg_head_buf = cfg_head;
    uint8_t *bounce = usb_get_retained_desc_bounce(sizeof(*cfg_head));
    if (bounce != NULL) {
        memset(bounce, 0, sizeof(*cfg_head));
        cfg_head_buf = (struct usb_config_descriptor *)bounce;
    }

    int n = usb_get_descriptor(dev, USB_DT_CONFIG, cfg_index,
                               cfg_head_buf, sizeof(*cfg_head));
    if (n >= (int)sizeof(*cfg_head) && cfg_head_buf != cfg_head)
        memcpy(cfg_head, cfg_head_buf, sizeof(*cfg_head));
    if (n < (int)sizeof(*cfg_head))
        return -1;

    uint16_t total = cfg_head->wTotalLength;
    if (total > cfg_buf_cap) {
        WARN("usb_core: config[%u] total %u > cap %u — truncating",
             (unsigned)cfg_index, total, (unsigned)cfg_buf_cap);
        total = (uint16_t)cfg_buf_cap;
    }

    uint8_t *xfer_buf = cfg_buf;
    bounce = usb_get_retained_desc_bounce(total);
    if (bounce != NULL) {
        memset(bounce, 0, total);
        xfer_buf = bounce;
    }

    n = usb_get_descriptor(dev, USB_DT_CONFIG, cfg_index, xfer_buf, total);
    if (n >= (int)total && xfer_buf != cfg_buf)
        memcpy(cfg_buf, xfer_buf, total);
    if (n < (int)total)
        return -1;

    return (int)total;
}

/* -------------------------------------------------------------------------- */
/* Minimal USB 2.0 hub support                                                */
/* -------------------------------------------------------------------------- */

#define USB_CLASS_HUB               0x09u
#define USB_DT_HUB                  0x29u

#define USB_PORT_STAT_CONNECTION    (1u << 0)
#define USB_PORT_STAT_ENABLE        (1u << 1)
#define USB_PORT_STAT_POWER         (1u << 8)
#define USB_PORT_STAT_LOW_SPEED     (1u << 9)
#define USB_PORT_STAT_HIGH_SPEED    (1u << 10)

#define USB_PORT_STAT_C_CONNECTION  (1u << 0)
#define USB_PORT_STAT_C_ENABLE      (1u << 1)
#define USB_PORT_STAT_C_RESET       (1u << 4)

#define USB_PORT_FEAT_POWER         8u
#define USB_PORT_FEAT_RESET         4u
#define USB_PORT_FEAT_C_CONNECTION  16u
#define USB_PORT_FEAT_C_ENABLE      17u
#define USB_PORT_FEAT_C_RESET       20u

struct usb_hub_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;
    uint8_t  bHubContrCurrent;
    uint8_t  device_removable;
    uint8_t  port_pwr_mask;
} __attribute__((packed));

struct usb_port_status {
    uint16_t status;
    uint16_t change;
} __attribute__((packed));

_Static_assert(sizeof(struct usb_hub_descriptor) == 9, "hub desc size");
_Static_assert(sizeof(struct usb_port_status) == 4, "port status size");

static bool usb_device_is_hub(const struct usb_device *dev)
{
    if (dev == NULL)
        return false;
    if (dev->dev_desc.bDeviceClass == USB_CLASS_HUB)
        return true;

    for (unsigned i = 0; i < USB_MAX_INTERFACES_PER_DEV; i++) {
        if (dev->ifaces[i].valid &&
            dev->ifaces[i].class_code == USB_CLASS_HUB)
            return true;
    }
    return false;
}

static enum usb_speed usb_hub_port_speed(uint16_t status)
{
    if (status & USB_PORT_STAT_LOW_SPEED)
        return USB_SPEED_LOW;
    if (status & USB_PORT_STAT_HIGH_SPEED)
        return USB_SPEED_HIGH;
    return USB_SPEED_FULL;
}

static int usb_hub_get_descriptor(struct usb_device *hub,
                                  struct usb_hub_descriptor *desc)
{
    if (hub == NULL || desc == NULL)
        return -1;
    return usb_control_msg(hub,
                           USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                           USB_REQ_GET_DESCRIPTOR,
                           (uint16_t)USB_DT_HUB << 8, 0,
                           desc, sizeof(*desc), 1000);
}

static int usb_hub_get_port_status(struct usb_device *hub, uint8_t port,
                                   struct usb_port_status *st)
{
    if (hub == NULL || st == NULL || port == 0)
        return -1;
    return usb_control_msg(hub,
                           USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
                           USB_REQ_GET_STATUS, 0, port,
                           st, sizeof(*st), 1000);
}

static int usb_hub_set_port_feature(struct usb_device *hub, uint8_t port,
                                    uint16_t feature)
{
    if (hub == NULL || port == 0)
        return -1;
    return usb_control_msg(hub,
                           USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                           USB_REQ_SET_FEATURE, feature, port,
                           NULL, 0, 1000);
}

static int usb_hub_clear_port_feature(struct usb_device *hub, uint8_t port,
                                      uint16_t feature)
{
    if (hub == NULL || port == 0)
        return -1;
    return usb_control_msg(hub,
                           USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                           USB_REQ_CLEAR_FEATURE, feature, port,
                           NULL, 0, 1000);
}

static int usb_hub_reset_port(struct usb_device *hub, uint8_t port,
                              enum usb_speed *speed_out)
{
    if (usb_hub_set_port_feature(hub, port, USB_PORT_FEAT_RESET) < 0)
        return -1;

    uint64_t start = timer_get_count();
    uint64_t freq  = timer_get_frequency();
    uint64_t ticks = freq / 2;   /* 500 ms */
    while (timer_get_count() - start < ticks) {
        struct usb_port_status st = {0};
        int rc = usb_hub_get_port_status(hub, port, &st);
        if (rc < (int)sizeof(st))
            return -1;

        if ((st.change & USB_PORT_STAT_C_RESET) &&
            (st.status & USB_PORT_STAT_CONNECTION) &&
            (st.status & USB_PORT_STAT_ENABLE)) {
            (void)usb_hub_clear_port_feature(hub, port, USB_PORT_FEAT_C_RESET);
            (void)usb_hub_clear_port_feature(hub, port, USB_PORT_FEAT_C_ENABLE);
            if (speed_out)
                *speed_out = usb_hub_port_speed(st.status);
            return 0;
        }
    }
    return -1;
}

static int usb_hub_prepare_ports(struct usb_device *hub,
                                 struct usb_hub_descriptor *desc)
{
    if (hub == NULL || desc == NULL || desc->bNbrPorts == 0)
        return -1;

    for (uint8_t port = 1; port <= desc->bNbrPorts; port++) {
        (void)usb_hub_set_port_feature(hub, port, USB_PORT_FEAT_POWER);
    }

    uint32_t delay_ms = (uint32_t)desc->bPwrOn2PwrGood * 2u;
    if (delay_ms == 0)
        delay_ms = 20;
    uint64_t start = timer_get_count();
    uint64_t ticks = (timer_get_frequency() / 1000u) * delay_ms;
    while (timer_get_count() - start < ticks) { }
    return 0;
}

static int usb_hub_find_child_port(struct usb_device *hub,
                                   const struct usb_hub_descriptor *desc,
                                   uint8_t *port_out,
                                   enum usb_speed *speed_out)
{
    if (hub == NULL || desc == NULL || port_out == NULL || speed_out == NULL)
        return -1;

    for (uint8_t port = 1; port <= desc->bNbrPorts; port++) {
        struct usb_port_status st = {0};
        int rc = usb_hub_get_port_status(hub, port, &st);
        if (rc < (int)sizeof(st))
            continue;

        if (!(st.status & USB_PORT_STAT_CONNECTION))
            continue;

        *port_out = port;
        *speed_out = usb_hub_port_speed(st.status);
        return 0;
    }
    return -1;
}

static int usb_enumerate_one(struct usb_device *dev, bool do_root_reset);
static int usb_try_enumerate_via_hub(struct usb_device *hub);

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

void usb_core_reset(void)
{
    /* If a device is currently enumerated, ask its HCD to release the
     * per-device resources (xHCI slot, NC contexts, transfer rings)
     * before we zero the device struct. Without this, a caller who
     * reached into production code to call usb_core_reset() would
     * strand HCD-side state; the mock HCD treats device_close as a
     * no-op counter increment, so tests see the same "device gone"
     * end-state either way.
     *
     * Keeps the registered HCD bound so tests don't have to
     * re-register on every fixture reset — usb_core_register_hcd(NULL)
     * is the separate knob for that. */
    if (root_device_present && active_hcd != NULL &&
        active_hcd->device_close != NULL) {
        active_hcd->device_close(&root_device);
    }
    if (hub_device_present && active_hcd != NULL &&
        active_hcd->device_close != NULL) {
        active_hcd->device_close(&hub_device);
    }
    memset(&root_device, 0, sizeof(root_device));
    memset(&hub_device, 0, sizeof(hub_device));
    root_device_present = false;
    hub_device_present = false;
    usb_hotplug_retry_blocked = false;
    usb_hotplug_retry_blocked_logged = false;
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

/*
 * Hot-plug retry path — the caller (net_poll()) invokes this at a slow
 * cadence so a USB device that wasn't ready at xhci_init time (or was
 * deliberately hidden as a stale pre-kexec device, see #309) can be
 * enumerated once a fresh attach is observed.
 *
 * Behaviour:
 *   - If a device has already been enumerated (root_device_present),
 *     do nothing. One-shot per boot, same contract as Phase 1.
 *   - Otherwise ask the HCD whether port 0 is reporting a connected
 *     device. If yes, run the full usb_core_enumerate() sequence.
 *
 * Returns 1 if this call successfully enumerated a new device, 0 if
 * no attach change happened, negative on enumeration failure.
 */
int usb_core_hotplug_poll(void)
{
    if (active_hcd == NULL || active_hcd->port_status == NULL)
        return 0;
    if (usb_hotplug_retry_blocked) {
        if (!usb_hotplug_retry_blocked_logged) {
            INFO("usb_core: hotplug retry suppressed after prior enumeration failure");
            usb_hotplug_retry_blocked_logged = true;
        }
        return 0;
    }
    if (root_device_present)
        return 0;

    bool connected = false;
    enum usb_speed speed = USB_SPEED_UNKNOWN;
    if (!active_hcd->port_status(0, &connected, &speed) || !connected)
        return 0;

    int rc = usb_core_enumerate();
    if (rc != 0) {
        WARN("usb_core: hotplug enumerate failed rc=%d", rc);
        if (usb_disable_hotplug_retry_after_failure) {
            usb_hotplug_retry_blocked = true;
            usb_hotplug_retry_blocked_logged = false;
        }
        return rc;
    }
    return root_device_present ? 1 : 0;
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
    if (urb->transfer_type == USB_XFER_CONTROL) {
        WARN("usb_core: control timeout req=0x%02x type=0x%02x "
             "wValue=0x%04x wIndex=0x%04x wLength=%u",
             (unsigned)urb->setup.bRequest,
             (unsigned)urb->setup.bmRequestType,
             (unsigned)urb->setup.wValue,
             (unsigned)urb->setup.wIndex,
             (unsigned)urb->setup.wLength);
    }
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

    void *xfer_buf = data;
    void *bounce = NULL;
    bool data_in = (bmRequestType & USB_DIR_IN) != 0;
    /*
     * On Jetson, xHCI data buffers are not hardware-coherent. The
     * retained-descriptor work proved this for child-device control-IN
     * traffic too, so use the NC bounce for any synchronous control
     * payload, not just root-device descriptor fetches.
     */
    if (data != NULL && wLength > 0) {
        bounce = usb_get_retained_desc_bounce(wLength);
        if (bounce != NULL) {
            if (data_in) {
                memset(bounce, 0, wLength);
            } else {
                memcpy(bounce, data, wLength);
            }
            xfer_buf = bounce;
        }
    }

    struct usb_urb urb = {0};
    urb.dev           = dev;
    urb.endpoint      = 0;     /* control pipe */
    urb.transfer_type = USB_XFER_CONTROL;
    urb.setup.bmRequestType = bmRequestType;
    urb.setup.bRequest      = bRequest;
    urb.setup.wValue        = wValue;
    urb.setup.wIndex        = wIndex;
    urb.setup.wLength       = wLength;
    urb.buffer = xfer_buf;
    urb.length = wLength;

    int sub = usb_submit_urb(&urb);
    if (sub != 0)
        return sub;

    int status = usb_wait_urb(&urb, timeout_ms);
    if (status == USB_URB_OK || status == USB_URB_SHORT) {
        if (bounce != NULL && data_in && data != NULL && urb.actual_length > 0) {
            size_t copy_len = urb.actual_length;
            if (copy_len > wLength)
                copy_len = wLength;
            memcpy(data, bounce, copy_len);
        }
        return (int)urb.actual_length;
    }
    if (urb.transfer_type == USB_XFER_CONTROL) {
        WARN("usb_core: control failed req=0x%02x type=0x%02x "
             "wValue=0x%04x wIndex=0x%04x wLength=%u status=%s actual=%u",
             (unsigned)urb.setup.bRequest,
             (unsigned)urb.setup.bmRequestType,
             (unsigned)urb.setup.wValue,
             (unsigned)urb.setup.wIndex,
             (unsigned)urb.setup.wLength,
             usb_urb_status_str((enum usb_urb_status)status),
             (unsigned)urb.actual_length);
    }
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

            cur_iface_slot = -1;
            for (unsigned i = 0; i < USB_MAX_INTERFACES_PER_DEV; i++) {
                if (dev->ifaces[i].valid &&
                    dev->ifaces[i].number == id->bInterfaceNumber) {
                    cur_iface_slot = (int)i;
                    break;
                }
            }
            if (cur_iface_slot < 0) {
                for (unsigned i = 0; i < USB_MAX_INTERFACES_PER_DEV; i++) {
                    if (!dev->ifaces[i].valid) {
                        cur_iface_slot = (int)i;
                        break;
                    }
                }
            }
            if (cur_iface_slot < 0) {
                WARN("usb_core: no iface slot for ifnum=%u alt=%u",
                     id->bInterfaceNumber, id->bAlternateSetting);
                break;
            }

            struct usb_interface *slot = &dev->ifaces[cur_iface_slot];
            if (slot->valid && slot->number == id->bInterfaceNumber &&
                id->bAlternateSetting < slot->alt_setting) {
                cur_iface_slot = -1;
                break;
            }

            slot->valid         = true;
            slot->number        = id->bInterfaceNumber;
            slot->alt_setting   = id->bAlternateSetting;
            slot->num_endpoints = id->bNumEndpoints;
            slot->class_code    = id->bInterfaceClass;
            slot->subclass      = id->bInterfaceSubClass;
            slot->protocol      = id->bInterfaceProtocol;
            for (unsigned e = 0; e < USB_MAX_ENDPOINTS_PER_DEV; e++)
                slot->ep_index[e] = -1;
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
static int usb_enumerate_one(struct usb_device *dev, bool do_root_reset)
{
    if (dev == NULL || active_hcd == NULL)
        return -1;

    /*
     * Devices always start enumeration at USB address 0. Keep the target
     * post-enumeration address separate so the first 8-byte device
     * descriptor read in Step 3 still goes out on the default address.
     *
     * Phase 1 remains single-device at the class-driver layer, but a
     * one-tier hub is allowed as transport detail. Keep the historical
     * address split: root device = 1, first child behind hub = 2.
     */
    uint8_t target_address = (dev->route_string == 0) ? 1u : 2u;
    dev->address = 0;

    int n;
    int rc;
    bool device_opened = false;
    bool have_full_device_desc = false;

    /*
     * Some devices, especially hubs inherited across kexec, do not
     * answer the very first 8-byte device-descriptor read cleanly even
     * after a successful root-port reset. Mirror Linux's more defensive
     * initial-descriptor strategy with a small reopen/retry loop before
     * giving up on enumeration entirely.
     */
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        if (do_root_reset && active_hcd->port_reset &&
            active_hcd->port_reset(dev->port) != 0) {
            WARN("usb_core: port_reset failed");
            return -1;
        }
        dev->state = USB_STATE_DEFAULT;

        if (active_hcd->device_open) {
            if (active_hcd->device_open(dev) != 0) {
                WARN("usb_core: device_open failed");
                return -1;
            }
            device_opened = true;
        }

        /* Give the freshly reset device a moment to settle before the
         * first EP0 IN transfer. */
        uint64_t settle_start = timer_get_count();
        uint64_t settle_ticks = timer_get_frequency() / 10; /* 100 ms */
        while (timer_get_count() - settle_start < settle_ticks) { }

        /*
         * Step 3: read the first bytes of the device descriptor while the
         * device is still at address 0.
         *
         * Linux's healthy EP0 ring on nano-2 starts with a 64-byte device
         * descriptor read for this Realtek hub path. Keep accepting any
         * reply >= 8 bytes (we only need bMaxPacketSize0 here), but keep
         * the initial request widened so the default-address transaction
         * semantics match the working Linux path as closely as possible.
         */
        uint8_t dd_stub[64];
        uint8_t *dd_stub_buf = dd_stub;
        size_t initial_desc_len = sizeof(dd_stub);
        if (usb_is_root_device(dev)) {
            uint8_t *bounce = usb_get_retained_desc_bounce(initial_desc_len);
            if (bounce != NULL) {
                memset(bounce, 0, initial_desc_len);
                dd_stub_buf = bounce;
                INFO("usb_core: using NC bounce for root initial descriptor "
                     "buf=0x%lx len=%u",
                     (unsigned long)(uintptr_t)dd_stub_buf,
                     (unsigned)initial_desc_len);
            }
        } else if (usb_child_initial_desc_bounce &&
                   dev->route_string != 0) {
            uint8_t *bounce = usb_get_retained_desc_bounce(initial_desc_len);
            usb_child_initial_desc_bounce = false;
            if (bounce != NULL) {
                memset(bounce, 0, initial_desc_len);
                dd_stub_buf = bounce;
                INFO("usb_core: using NC bounce for child initial descriptor "
                     "buf=0x%lx len=%u route=0x%x root_port=%u",
                     (unsigned long)(uintptr_t)dd_stub_buf,
                     (unsigned)initial_desc_len,
                     (unsigned)dev->route_string,
                     (unsigned)dev->root_hub_port);
            }
        }
        n = usb_get_descriptor(dev, USB_DT_DEVICE, 0,
                               dd_stub_buf, initial_desc_len);
        if (n >= 8) {
            if (dd_stub_buf != dd_stub)
                memcpy(dd_stub, dd_stub_buf, (size_t)n);
            /* bMaxPacketSize0 is byte 7. Record it for the HCD if useful later. */
            dev->dev_desc.bMaxPacketSize0 = dd_stub[7];
            if (n >= (int)sizeof(dev->dev_desc)) {
                memcpy(&dev->dev_desc, dd_stub, sizeof(dev->dev_desc));
                have_full_device_desc = true;
            }
            goto got_initial_descriptor;
        }

        WARN("usb_core: short GET_DESCRIPTOR(device, %u) n=%d (attempt %u/3)",
             (unsigned)initial_desc_len, n, attempt + 1);

        if (device_opened && active_hcd->device_close) {
            active_hcd->device_close(dev);
            device_opened = false;
        }
    }

    goto err_close;

got_initial_descriptor:

    /* Step 4: assign the device's non-zero USB address. */
    if (dev->state == USB_STATE_ADDRESS && dev->address != 0) {
        INFO("usb_core: reusing inherited device address %u after initial descriptor",
             (unsigned)dev->address);
    } else {
#if defined(PLATFORM_JETSON_ORIN_NANO)
        if (usb_child_address_sync_bsr0 && dev->route_string != 0) {
            usb_child_address_sync_bsr0 = false;
            /* Fresh child slots need one live ADDRESS_DEVICE(BSR=0) sync
             * before follow-up control traffic and endpoint configure become
             * reliable on the inherited Jetson xHCI handoff path. */
            int addr_rc = xhci_sync_child_address_bsr0(dev);
            INFO("usb_core: child ADDRESS_DEVICE(BSR=0) sync after initial "
                 "descriptor rc=%d route=0x%x root_port=%u target=%u",
                 addr_rc,
                 (unsigned)dev->route_string,
                 (unsigned)dev->root_hub_port,
                 (unsigned)target_address);
            if (addr_rc == 0) {
                dev->address = target_address;
                dev->state = USB_STATE_ADDRESS;
                goto address_assigned;
            }
        }
#endif
        rc = usb_control_msg(dev,
                             USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                             USB_REQ_SET_ADDRESS,
                             target_address, 0, NULL, 0, 500);
        if (rc < 0) {
            WARN("usb_core: SET_ADDRESS failed: %s",
                 usb_urb_status_str((enum usb_urb_status)(-rc)));
            goto err_close;
        }
        dev->address = target_address;
        dev->state = USB_STATE_ADDRESS;
    }

address_assigned:

    /* Step 5: full device descriptor. */
    if (!have_full_device_desc) {
        if (usb_is_root_device(dev)) {
            uint8_t dd_full[64];
            uint8_t *dd_full_buf = dd_full;
            uint8_t *bounce = usb_get_retained_desc_bounce(sizeof(dd_full));
            if (bounce != NULL) {
                memset(bounce, 0, sizeof(dd_full));
                dd_full_buf = bounce;
                INFO("usb_core: using NC bounce for root full device descriptor "
                     "buf=0x%lx len=%u",
                     (unsigned long)(uintptr_t)dd_full_buf,
                     (unsigned)sizeof(dd_full));
            }
            n = usb_get_descriptor(dev, USB_DT_DEVICE, 0,
                                   dd_full_buf, sizeof(dd_full));
            if (n >= (int)sizeof(dev->dev_desc)) {
                if (dd_full_buf != dd_full)
                    memcpy(dd_full, dd_full_buf, sizeof(dd_full));
                memcpy(&dev->dev_desc, dd_full, sizeof(dev->dev_desc));
            } else {
                WARN("usb_core: short GET_DESCRIPTOR(device, 64) n=%d", n);
                goto err_close;
            }
        } else {
            struct usb_device_descriptor *dd_full_buf = &dev->dev_desc;
            if (usb_child_followup_desc_bounce && dev->route_string != 0) {
                uint8_t *bounce = usb_get_retained_desc_bounce(sizeof(dev->dev_desc));
                if (bounce != NULL) {
                    memset(bounce, 0, sizeof(dev->dev_desc));
                    dd_full_buf = (struct usb_device_descriptor *)bounce;
                    INFO("usb_core: using NC bounce for child full device descriptor "
                         "buf=0x%lx len=%u route=0x%x root_port=%u",
                         (unsigned long)(uintptr_t)dd_full_buf,
                         (unsigned)sizeof(dev->dev_desc),
                         (unsigned)dev->route_string,
                         (unsigned)dev->root_hub_port);
                }
            }
            n = usb_get_descriptor(dev, USB_DT_DEVICE, 0,
                                   dd_full_buf,
                                   sizeof(dev->dev_desc));
            if (n >= (int)sizeof(dev->dev_desc) && dd_full_buf != &dev->dev_desc)
                memcpy(&dev->dev_desc, dd_full_buf, sizeof(dev->dev_desc));
        }
        if (n < (int)sizeof(dev->dev_desc)) {
            WARN("usb_core: short GET_DESCRIPTOR(device) n=%d", n);
            goto err_close;
        }
    }

    /* Step 6 + 7: fetch the selected config tree. Default to config
     * index 0, but prefer a later CDC-ECM-capable config when the
     * device exposes one (e.g. RTL8153 vendor config 1 vs CDC config 2). */
    struct usb_config_descriptor cfg_head;
    uint8_t selected_cfg_index = 0;
    n = usb_fetch_config_descriptor(dev, 0, &cfg_head,
                                    dev->raw_config,
                                    sizeof(dev->raw_config));
    if (n < (int)sizeof(cfg_head)) {
        WARN("usb_core: short GET_DESCRIPTOR(config, 0) n=%d", n);
        goto err_close;
    }
    dev->raw_config_len = (uint16_t)n;

    bool selected_is_cdc = usb_config_is_cdc_ecm_candidate(dev->raw_config,
                                                            dev->raw_config_len);
    if (!selected_is_cdc && dev->dev_desc.bNumConfigurations > 1) {
        uint8_t cfg_candidate[USB_MAX_CONFIG_DESC_BYTES];
        for (uint8_t cfg_index = 1;
             cfg_index < dev->dev_desc.bNumConfigurations;
             cfg_index++) {
            struct usb_config_descriptor cand_head;
            int cand_len = usb_fetch_config_descriptor(dev, cfg_index,
                                                       &cand_head,
                                                       cfg_candidate,
                                                       sizeof(cfg_candidate));
            if (cand_len < (int)sizeof(cand_head))
                continue;
            if (!usb_config_is_cdc_ecm_candidate(cfg_candidate, (size_t)cand_len))
                continue;

            memcpy(dev->raw_config, cfg_candidate, (size_t)cand_len);
            dev->raw_config_len = (uint16_t)cand_len;
            cfg_head = cand_head;
            selected_cfg_index = cfg_index;
            selected_is_cdc = true;
            INFO("usb_core: selecting CDC-ECM config index %u value %u over default config index 0",
                 (unsigned)selected_cfg_index,
                 (unsigned)cfg_head.bConfigurationValue);
            break;
        }
    }

    /* Step 8: parse. */
    if (usb_parse_configuration(dev) != 0) {
        WARN("usb_core: parse_configuration failed");
        goto err_close;
    }

    /* Step 9: activate the default configuration. */
    uint8_t cfg_val = cfg_head.bConfigurationValue;
    rc = usb_control_msg(dev,
                         USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                         USB_REQ_SET_CONFIGURATION,
                         cfg_val, 0, NULL, 0, 500);
    if (rc < 0) {
        WARN("usb_core: SET_CONFIGURATION failed: %s",
             usb_urb_status_str((enum usb_urb_status)(-rc)));
        goto err_close;
    }
    dev->current_config = cfg_val;
    dev->state = USB_STATE_CONFIGURED;

    /* Step 10: HCD commits non-EP0 endpoint contexts. */
    if (active_hcd->endpoint_configure) {
        for (unsigned e = 0; e < USB_MAX_ENDPOINTS_PER_DEV; e++) {
            if (!dev->endpoints[e].valid) continue;
            int ec = active_hcd->endpoint_configure(dev, &dev->endpoints[e]);
            if (ec != 0) {
                WARN("usb_core: endpoint_configure(ep 0x%02x) failed: %d",
                     dev->endpoints[e].address, ec);
                goto err_close;
            }
        }
    }

    INFO("usb_core: device configured — vid=0x%04x pid=0x%04x class=0x%02x",
         dev->dev_desc.idVendor,
         dev->dev_desc.idProduct,
         dev->dev_desc.bDeviceClass);
    return 0;

err_close:
    if (device_opened && active_hcd->device_close)
        active_hcd->device_close(dev);
    return -1;
}

static int usb_try_enumerate_via_hub(struct usb_device *hub)
{
    struct usb_hub_descriptor desc = {0};
    int rc = usb_hub_get_descriptor(hub, &desc);
    if (rc < (int)sizeof(desc)) {
        WARN("usb_core: short GET_DESCRIPTOR(hub) n=%d", rc);
        return -1;
    }
    if (usb_hub_prepare_ports(hub, &desc) != 0) {
        WARN("usb_core: hub port power-up failed");
        return -1;
    }

    uint8_t child_port = 0;
    enum usb_speed child_speed = USB_SPEED_UNKNOWN;
    if (usb_hub_find_child_port(hub, &desc, &child_port, &child_speed) != 0) {
        INFO("usb_core: hub has no connected downstream device");
        return 0;
    }

    if (usb_hub_reset_port(hub, child_port, &child_speed) != 0) {
        WARN("usb_core: hub port %u reset failed", child_port);
        return -1;
    }

    memset(&root_device, 0, sizeof(root_device));
    root_device.hcd = active_hcd;
    root_device.address = 0;
    root_device.speed = child_speed;
    root_device.state = USB_STATE_ATTACHED;
    root_device.port = child_port;
    root_device.root_hub_port = hub->root_hub_port;
    root_device.route_string = child_port;

    rc = usb_enumerate_one(&root_device, false);
    if (rc != 0)
        return rc;

    root_device_present = true;
    return 0;
}

int usb_core_enumerate(void)
{
    usb_hotplug_retry_blocked_logged = false;

    /* Start from a clean slate — a retry after a previous failure must
     * not leave the stale root_device visible via usb_core_first_device(). */
    root_device_present = false;
    if (hub_device_present && active_hcd != NULL && active_hcd->device_close) {
        active_hcd->device_close(&hub_device);
        memset(&hub_device, 0, sizeof(hub_device));
        hub_device_present = false;
    }

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
    root_device.address = 0;
    root_device.speed = speed;
    root_device.state = USB_STATE_ATTACHED;
    root_device.port  = 0;

    int rc = usb_enumerate_one(&root_device, true);
    if (rc != 0) {
        if (usb_disable_hotplug_retry_after_failure) {
            usb_hotplug_retry_blocked = true;
            usb_hotplug_retry_blocked_logged = false;
        }
        return rc;
    }

    if (!usb_device_is_hub(&root_device)) {
        usb_hotplug_retry_blocked = false;
        root_device_present = true;
        return 0;
    }

    memcpy(&hub_device, &root_device, sizeof(hub_device));
    memset(&root_device, 0, sizeof(root_device));
    hub_device_present = true;
    INFO("usb_core: root device is a USB hub — probing one downstream child");
    rc = usb_try_enumerate_via_hub(&hub_device);
    if (rc == 0)
        usb_hotplug_retry_blocked = false;
    else if (usb_disable_hotplug_retry_after_failure)
        usb_hotplug_retry_blocked = true;
    return rc;
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
