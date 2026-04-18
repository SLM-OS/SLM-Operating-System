/*
 * cdc_ecm.c - SLM-OS CDC-ECM USB class driver (Phase 2 of #266)
 *
 * Binds to an enumerated USB CDC-ECM device, fetches its MAC address,
 * keeps a pool of bulk IN URBs posted for RX, and funnels bulk OUT
 * URBs for TX. Presents a `struct net_driver` so the existing lwIP
 * netif adapter can use it unmodified.
 *
 * Scope (docs/jetson-usb-networking-plan.md §3 Phase 2, §6):
 *   - Single device, no hot-plug.
 *   - CDC-ECM only (not NCM, not RNDIS).
 *   - Static buffer pools — no heap, no dynamic sizing.
 *   - RX/TX completions may fire from IRQ context on Phase 3A XHCI;
 *     the driver is written for that lifetime already (volatile slot
 *     flags, per-slot URBs, no shared mutable state beyond counters).
 *
 * DMA note: the slot buffers below live in cacheable BSS for Phase 2,
 * which is safe because the mock HCD used by the tests doesn't do
 * real DMA. Phase 3A XHCI must either issue DC CVAC / CIVAC around
 * submission / completion or relocate these buffers to NC memory
 * (ncmem_alloc). This choice belongs to the XHCI driver, not the
 * class driver, so no cache maintenance happens here.
 */

#include "cdc_ecm.h"
#include "usb.h"
#include "net.h"
#include "net_driver.h"
#include "debug.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Constants                                                                   */
/* -------------------------------------------------------------------------- */

/* CDC subclass / descriptor identifiers. */
#define CDC_SUBCLASS_ECM                0x06
#define CDC_DATA_INTERFACE_CLASS        0x0A
#define CDC_FUNCTIONAL_ETHERNET         0x0F    /* bDescriptorSubtype */

/*
 * RX: 4 slots is a balance between keeping the pipe full (so the HCD
 * never runs out of IN work) and BSS footprint (4 × 2048 = 8 KB).
 * The Linux cdc_ether driver uses up to 20 RX URBs in flight at high
 * throughput; 4 is enough for Phase-2 correctness + the Phase-3A
 * 480 Mbps ceiling.
 */
#define CDC_ECM_RX_SLOTS                4
#define CDC_ECM_TX_SLOTS                4

/* 1514 Ethernet + a little headroom. Keep a power-of-two for clarity. */
#define CDC_ECM_BUF_SIZE                2048

/* Default MTU if the device omits / zero-fills wMaxSegmentSize. */
#define CDC_ECM_DEFAULT_MTU             1514

/* USB standard langid we ask the device for strings in. 0x0409 = en-US. */
#define CDC_ECM_STRING_LANG_ID          0x0409

/* -------------------------------------------------------------------------- */
/* Internal slot state                                                         */
/* -------------------------------------------------------------------------- */

struct cdc_rx_slot {
    uint8_t         buf[CDC_ECM_BUF_SIZE];
    struct usb_urb  urb;
    /* Set true by the completion callback once data is in `buf`; set
     * false once recv() has consumed it and re-submitted the URB. */
    volatile bool   ready;
    uint32_t        len;
};

struct cdc_tx_slot {
    uint8_t         buf[CDC_ECM_BUF_SIZE];
    struct usb_urb  urb;
    /* in_use == true  → slot holds a submitted URB or one waiting to
     *                   be reaped.
     *
     * completed == true ⇒ completion has fired; tx_reap() must clear
     *                     both flags so send() can re-use it. */
    volatile bool   in_use;
    volatile bool   completed;
};

/* -------------------------------------------------------------------------- */
/* Module state                                                                */
/* -------------------------------------------------------------------------- */

static struct {
    bool              probed;
    bool              registered;
    struct usb_device *dev;
    uint8_t           mac[6];
    uint16_t          max_segment;
    const struct usb_endpoint *bulk_in;
    const struct usb_endpoint *bulk_out;

    struct cdc_rx_slot rx[CDC_ECM_RX_SLOTS];
    struct cdc_tx_slot tx[CDC_ECM_TX_SLOTS];

    volatile uint32_t rx_completions;
    volatile uint32_t tx_completions;
    volatile uint32_t rx_drops;   /* completion found no free slot */
} cdc;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/*
 * Search the device's raw config descriptor bytes for the Ethernet
 * Networking Functional Descriptor. Returns a pointer into raw_config
 * or NULL if not found. The caller has already walked the standard
 * descriptors via usb_parse_configuration(); this is an extra pass
 * looking specifically for the class-specific subtype 0x0F block.
 */
static const uint8_t *find_cdc_ecm_functional(const struct usb_device *dev)
{
    const uint8_t *p   = dev->raw_config;
    const uint8_t *end = p + dev->raw_config_len;

    /* CONFIG header is the first 9 bytes; skip past it. */
    if (end - p < 9) return NULL;
    p += p[0];   /* advance by bLength */

    while (p + 2 <= end) {
        uint8_t blen = p[0];
        uint8_t btype = p[1];
        if (blen < 2 || p + blen > end) break;

        /* CS_INTERFACE (0x24) with the Ethernet Networking subtype
         * (0x0F) identifies the functional descriptor. Its total
         * length is 13 bytes per CDC 1.2 §5.2.3.3. */
        if (btype == USB_DT_CS_INTERFACE && blen >= 13 && p[2] == CDC_FUNCTIONAL_ETHERNET)
            return p;
        p += blen;
    }
    return NULL;
}

/*
 * Parse a 6-byte MAC out of a USB string descriptor. CDC-ECM encodes
 * the MAC as 12 UTF-16LE hex characters (no separators). The
 * descriptor starts with a 2-byte header (bLength, bDescriptorType),
 * so the payload is 24 bytes of UTF-16 spelling the MAC.
 */
int cdc_ecm_parse_mac_string(const uint8_t *desc, size_t desc_len,
                             uint8_t out_mac[6])
{
    if (desc == NULL || out_mac == NULL)
        return -1;
    /* Need header + 12 chars × 2 bytes = 26 bytes. The device's
     * reported bLength must match. */
    if (desc_len < 26 || desc[0] != 26 || desc[1] != USB_DT_STRING)
        return -1;

    uint8_t nibbles[12];
    for (unsigned i = 0; i < 12; i++) {
        uint8_t lo = desc[2 + i * 2];
        uint8_t hi = desc[2 + i * 2 + 1];
        /* Reject anything that isn't pure ASCII — a real CDC-ECM
         * string uses the BMP's ASCII range, so the high byte of
         * each UTF-16 unit must be zero. */
        if (hi != 0)
            return -1;
        uint8_t n;
        if      (lo >= '0' && lo <= '9') n = (uint8_t)(lo - '0');
        else if (lo >= 'a' && lo <= 'f') n = (uint8_t)(lo - 'a' + 10);
        else if (lo >= 'A' && lo <= 'F') n = (uint8_t)(lo - 'A' + 10);
        else return -1;
        nibbles[i] = n;
    }
    for (unsigned i = 0; i < 6; i++)
        out_mac[i] = (uint8_t)((nibbles[i * 2] << 4) | nibbles[i * 2 + 1]);
    return 0;
}

/*
 * Synthesize a locally-administered MAC when the device doesn't
 * supply one. Upper nibble of byte 0: 0x2 sets the U/L bit (locally
 * administered) and keeps the I/G bit (unicast) at 0.
 */
static void generate_fallback_mac(uint8_t mac[6])
{
    static uint32_t counter;
    counter++;
    mac[0] = 0x02;
    mac[1] = 0x53;  /* 'S' — SLM */
    mac[2] = 0x4C;  /* 'L' */
    mac[3] = 0x4D;  /* 'M' */
    mac[4] = (uint8_t)(counter >> 8);
    mac[5] = (uint8_t)counter;
}

/* -------------------------------------------------------------------------- */
/* Completion callbacks (may fire from IRQ context on Phase 3A)                */
/* -------------------------------------------------------------------------- */

static void cdc_rx_complete(struct usb_urb *urb)
{
    struct cdc_rx_slot *slot = (struct cdc_rx_slot *)urb->context;

    cdc.rx_completions++;

    if (urb->status == USB_URB_OK || urb->status == USB_URB_SHORT) {
        slot->len = urb->actual_length;
        slot->ready = true;
    } else {
        /* Error / cancel: leave the slot unready and re-submit later
         * in cdc_ecm_poll(). For Phase 2 cancel only fires at
         * shutdown, so we simply drop the frame. */
        slot->ready = false;
        slot->len = 0;
    }
}

static void cdc_tx_complete(struct usb_urb *urb)
{
    struct cdc_tx_slot *slot = (struct cdc_tx_slot *)urb->context;
    cdc.tx_completions++;
    slot->completed = true;
    /* tx_reap (called from net_poll) clears in_use + completed
     * together so send() doesn't race it half-cleared. */
}

/* -------------------------------------------------------------------------- */
/* RX re-submission                                                            */
/* -------------------------------------------------------------------------- */

static int cdc_rx_submit(struct cdc_rx_slot *slot)
{
    slot->urb.dev           = cdc.dev;
    slot->urb.endpoint      = cdc.bulk_in->address;
    slot->urb.transfer_type = USB_XFER_BULK;
    slot->urb.buffer        = slot->buf;
    slot->urb.length        = CDC_ECM_BUF_SIZE;
    slot->urb.actual_length = 0;
    slot->urb.complete      = cdc_rx_complete;
    slot->urb.context       = slot;
    slot->urb.status        = USB_URB_PENDING;
    return usb_submit_urb(&slot->urb);
}

/* -------------------------------------------------------------------------- */
/* net_driver ops                                                              */
/* -------------------------------------------------------------------------- */

static int cdc_ecm_net_init(void)
{
    /*
     * Queue all RX slots now that the net stack has signalled init.
     * Separating probe (which remembers endpoints + MAC) from init
     * (which starts the data path) mirrors the virtio-net / MACB
     * drivers and lets the shell register the driver without
     * surrendering URBs to the HCD prematurely.
     */
    if (!cdc.probed)
        return NET_E_NOT_INIT;
    for (unsigned i = 0; i < CDC_ECM_RX_SLOTS; i++) {
        int rc = cdc_rx_submit(&cdc.rx[i]);
        if (rc != 0) {
            WARN("cdc_ecm: initial RX submit failed (slot %u rc=%d)", i, rc);
            return NET_E_GENERIC;
        }
    }
    return NET_OK;
}

static int cdc_ecm_net_send(const void *buf, size_t len)
{
    if (!cdc.probed)
        return NET_E_NOT_INIT;
    if (buf == NULL || len == 0)
        return NET_E_INVAL;
    if (len > CDC_ECM_BUF_SIZE)
        return NET_E_TOO_LARGE;

    for (unsigned i = 0; i < CDC_ECM_TX_SLOTS; i++) {
        struct cdc_tx_slot *slot = &cdc.tx[i];
        if (slot->in_use)
            continue;
        memcpy(slot->buf, buf, len);
        slot->urb.dev           = cdc.dev;
        slot->urb.endpoint      = cdc.bulk_out->address;
        slot->urb.transfer_type = USB_XFER_BULK;
        slot->urb.buffer        = slot->buf;
        slot->urb.length        = (uint32_t)len;
        slot->urb.actual_length = 0;
        slot->urb.complete      = cdc_tx_complete;
        slot->urb.context       = slot;
        slot->urb.status        = USB_URB_PENDING;
        slot->completed         = false;
        slot->in_use            = true;

        int rc = usb_submit_urb(&slot->urb);
        if (rc != 0) {
            /* Undo reservation if submit rejected us. */
            slot->in_use = false;
            return NET_E_GENERIC;
        }
        return NET_OK;
    }
    return NET_E_BUSY;
}

static int cdc_ecm_net_recv(void *buf, size_t max_len)
{
    if (!cdc.probed)
        return NET_E_NOT_INIT;
    if (buf == NULL)
        return NET_E_INVAL;

    for (unsigned i = 0; i < CDC_ECM_RX_SLOTS; i++) {
        struct cdc_rx_slot *slot = &cdc.rx[i];
        if (!slot->ready)
            continue;
        uint32_t len = slot->len;
        if (len > max_len)
            len = (uint32_t)max_len;
        if (len > 0)
            memcpy(buf, slot->buf, len);
        /* Release the slot before re-submitting so a fast completion
         * after re-submit doesn't race us into a double-processed
         * frame. */
        slot->ready = false;
        slot->len   = 0;
        (void)cdc_rx_submit(slot);
        return (int)len;
    }
    return 0;   /* nothing ready — caller retries on next poll */
}

static void cdc_ecm_net_tx_reap(void)
{
    for (unsigned i = 0; i < CDC_ECM_TX_SLOTS; i++) {
        struct cdc_tx_slot *slot = &cdc.tx[i];
        if (slot->in_use && slot->completed) {
            slot->completed = false;
            slot->in_use    = false;
        }
    }
    /* Drive the HCD so completions are visible on poll-only paths. */
    usb_core_poll();
}

static void cdc_ecm_net_get_mac(uint8_t mac[6])
{
    memcpy(mac, cdc.mac, 6);
}

static bool cdc_ecm_net_link_status(void)
{
    /* Phase 2 assumes the link is up once the device is probed. The
     * interrupt-IN notification endpoint will drive a real status bit
     * once it's wired in a later phase (#266 out-of-scope for now). */
    return cdc.probed;
}

static const struct net_driver cdc_ecm_driver = {
    .name        = "usb-cdc-ecm",
    .init        = cdc_ecm_net_init,
    .send        = cdc_ecm_net_send,
    .recv        = cdc_ecm_net_recv,
    .get_mac     = cdc_ecm_net_get_mac,
    .link_status = cdc_ecm_net_link_status,
    .tx_reap     = cdc_ecm_net_tx_reap,
};

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

int cdc_ecm_probe_and_register(void)
{
    /* Every probe starts from a clean slate so a subsequent call that
     * finds no device (or a non-CDC device) can't leave stale state
     * visible via cdc_ecm_get_mac() / cdc_ecm_net_link_status(). */
    cdc.probed = false;

    struct usb_device *dev = usb_core_first_device();
    if (dev == NULL) {
        INFO("cdc_ecm: no USB device — skipping probe");
        return NET_OK;   /* "disabled" path is not an error */
    }

    /* Find a CDC-ECM control interface (class 0x02 + subclass 0x06)
     * and the CDC data interface (class 0x0A). CDC-ECM pairs them but
     * the interface numbers are device-specific, so scan. */
    int ctrl_if = -1;
    int data_if = -1;
    for (unsigned i = 0; i < sizeof(dev->ifaces) / sizeof(dev->ifaces[0]); i++) {
        if (!dev->ifaces[i].valid) continue;
        uint8_t cls = dev->ifaces[i].class_code;
        uint8_t sub = dev->ifaces[i].subclass;
        if (cls == 0x02 && sub == CDC_SUBCLASS_ECM && ctrl_if < 0)
            ctrl_if = (int)dev->ifaces[i].number;
        else if (cls == CDC_DATA_INTERFACE_CLASS && data_if < 0)
            data_if = (int)dev->ifaces[i].number;
    }
    if (ctrl_if < 0 || data_if < 0) {
        WARN("cdc_ecm: no CDC-ECM interface pair (ctrl=%d data=%d)",
             ctrl_if, data_if);
        return -1;
    }

    const struct usb_endpoint *in =
        usb_find_endpoint(dev, (uint8_t)data_if, USB_DIR_IN, USB_XFER_BULK);
    const struct usb_endpoint *out =
        usb_find_endpoint(dev, (uint8_t)data_if, USB_DIR_OUT, USB_XFER_BULK);
    if (in == NULL || out == NULL) {
        WARN("cdc_ecm: data iface %d missing bulk IN or OUT", data_if);
        return -1;
    }

    /*
     * Walk the raw config for the Ethernet functional descriptor.
     * iMACAddress and wMaxSegmentSize come from here. If the
     * descriptor is absent or iMACAddress is 0 we synthesise a MAC
     * rather than failing — the plan explicitly calls out this
     * fallback for dongles that don't report one.
     */
    /* CDC 1.2 §5.2.3.3 Ethernet Networking Functional Descriptor:
     *   0: bLength (13)
     *   1: bDescriptorType (CS_INTERFACE)
     *   2: bDescriptorSubtype (0x0F)
     *   3: iMACAddress
     * 4-7: bmEthernetStatistics (4 bytes)
     * 8-9: wMaxSegmentSize
     * ...
     */
    const uint8_t *func = find_cdc_ecm_functional(dev);
    uint8_t  imac = 0;
    uint16_t mss  = 0;
    if (func != NULL) {
        imac = func[3];
        mss  = le16(func + 8);
    }
    cdc.max_segment = (mss != 0) ? mss : CDC_ECM_DEFAULT_MTU;

    bool have_mac = false;
    if (imac != 0) {
        uint8_t str_buf[64];
        int n = usb_control_msg(dev,
                                USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                USB_REQ_GET_DESCRIPTOR,
                                (uint16_t)((USB_DT_STRING << 8) | imac),
                                CDC_ECM_STRING_LANG_ID,
                                str_buf, sizeof(str_buf),
                                500 /* ms */);
        if (n >= 26 &&
            cdc_ecm_parse_mac_string(str_buf, (size_t)n, cdc.mac) == 0) {
            have_mac = true;
        } else {
            WARN("cdc_ecm: iMACAddress=%u did not parse (n=%d) — using fallback",
                 imac, n);
        }
    }
    if (!have_mac)
        generate_fallback_mac(cdc.mac);

    cdc.dev      = dev;
    cdc.bulk_in  = in;
    cdc.bulk_out = out;
    /* Zero slot state — fresh arrays on every probe. */
    for (unsigned i = 0; i < CDC_ECM_RX_SLOTS; i++) {
        cdc.rx[i].ready = false;
        cdc.rx[i].len = 0;
    }
    for (unsigned i = 0; i < CDC_ECM_TX_SLOTS; i++) {
        cdc.tx[i].in_use = false;
        cdc.tx[i].completed = false;
    }
    cdc.probed = true;

    INFO("cdc_ecm: probed — mac=%02x:%02x:%02x:%02x:%02x:%02x mtu=%u "
         "bulk_in=0x%02x bulk_out=0x%02x",
         cdc.mac[0], cdc.mac[1], cdc.mac[2],
         cdc.mac[3], cdc.mac[4], cdc.mac[5],
         cdc.max_segment, in->address, out->address);

    net_register_driver(&cdc_ecm_driver);
    cdc.registered = true;
    return NET_OK;
}

void cdc_ecm_poll(void)
{
    if (cdc.probed)
        usb_core_poll();
}

const uint8_t *cdc_ecm_get_mac(void)
{
    return cdc.probed ? cdc.mac : NULL;
}

uint16_t cdc_ecm_get_max_segment(void)
{
    return cdc.max_segment;
}

uint32_t cdc_ecm_get_rx_count(void)
{
    return cdc.rx_completions;
}

uint32_t cdc_ecm_get_tx_count(void)
{
    return cdc.tx_completions;
}
