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
#include "ncmem.h"
#include "debug.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Constants                                                                   */
/* -------------------------------------------------------------------------- */

/* CDC class / subclass / descriptor identifiers (USB CDC 1.2 spec). */
#define CDC_CONTROL_INTERFACE_CLASS     0x02
#define CDC_DATA_INTERFACE_CLASS        0x0A
#define CDC_SUBCLASS_ECM                0x06
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

/* 8-byte CDC notification header + 8-byte speed-change payload. */
#define CDC_ECM_NOTIFY_BUF_SIZE         16

/* Default MTU if the device omits / zero-fills wMaxSegmentSize. */
#define CDC_ECM_DEFAULT_MTU             1514

/* USB standard langid we ask the device for strings in. 0x0409 = en-US. */
#define CDC_ECM_STRING_LANG_ID          0x0409

/* CDC notification types (USB CDC 1.2). */
#define CDC_NOTIFY_NETWORK_CONNECTION      0x00
#define CDC_NOTIFY_CONNECTION_SPEED_CHANGE 0x2A

/* -------------------------------------------------------------------------- */
/* Internal slot state                                                         */
/* -------------------------------------------------------------------------- */

struct cdc_rx_slot {
#if defined(PLATFORM_HAS_NC_MEMORY)
    uint8_t        *buf;
#else
    uint8_t         buf[CDC_ECM_BUF_SIZE];
#endif
    struct usb_urb  urb;
    /*
     * `ready` is the producer / consumer synchronisation flag.
     * Published with __ATOMIC_RELEASE by cdc_rx_complete after `len`
     * and `buf` are written; observed with __ATOMIC_ACQUIRE by recv
     * before it reads `len` and `buf`. On ARM64 this emits DMB ISH
     * barriers so the flag and the data it advertises can't be seen
     * out of order. Plain `volatile` would not be sufficient.
     */
    bool            ready;
    uint32_t        len;
};

struct cdc_tx_slot {
#if defined(PLATFORM_HAS_NC_MEMORY)
    uint8_t        *buf;
#else
    uint8_t         buf[CDC_ECM_BUF_SIZE];
#endif
    struct usb_urb  urb;
    /*
     * `in_use`  — reservation flag. send() CAS-claims an idle slot
     *             (`in_use` 0→1); tx_reap flips 1→0 once `completed`
     *             is observed. The CAS protects against concurrent
     *             senders racing to claim the same slot.
     *
     * `completed` — set by the IRQ-context completion callback.
     *               tx_reap observes it with ACQUIRE semantics so the
     *               HCD's prior writes (actual_length, status) are
     *               visible before the slot returns to the free pool.
     */
    bool            in_use;
    bool            completed;
};

struct cdc_notify_slot {
#if defined(PLATFORM_HAS_NC_MEMORY)
    uint8_t        *buf;
#else
    uint8_t         buf[CDC_ECM_NOTIFY_BUF_SIZE];
#endif
    struct usb_urb  urb;
    bool            in_use;
    bool            completed;
};

struct usb_cdc_notification {
    uint8_t  bmRequestType;
    uint8_t  bNotificationType;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

/* -------------------------------------------------------------------------- */
/* Module state                                                                */
/* -------------------------------------------------------------------------- */

static struct {
    /*
     * `probed` is read by every data-path op (init / send / recv /
     * poll / link_status / get_mac) and written by
     * probe_and_register. Phase-2 call paths are all single-threaded
     * init context, but Phase 3A / 4 may drive these ops from
     * different CPUs, so reads use __ATOMIC_ACQUIRE and writes use
     * __ATOMIC_RELEASE. ACQUIRE pairs with the RELEASE-store at the
     * end of probe_and_register so any post-probe observer sees the
     * fully populated `dev`, `bulk_in`, `bulk_out`, `mac`, and
     * `max_segment` that precede the final `probed = true` store.
     */
    bool              probed;
    struct usb_device *dev;
    uint8_t           mac[6];
    uint16_t          max_segment;
    uint8_t           ctrl_iface_num;
    const struct usb_endpoint *bulk_in;
    const struct usb_endpoint *bulk_out;
    const struct usb_endpoint *notif_in;
    struct cdc_notify_slot     notif;
    bool              link_ready;
    bool              link_signal_valid;
    uint32_t          upstream_bps;
    uint32_t          downstream_bps;

    struct cdc_rx_slot rx[CDC_ECM_RX_SLOTS];
    struct cdc_tx_slot tx[CDC_ECM_TX_SLOTS];

    /* Diagnostic counters — bumped with __atomic_fetch_add RELAXED so
     * concurrent completions on different CPUs can't lose bumps. Not
     * on the control path; values are only observed by tests and the
     * shell's get_rx_count / get_tx_count accessors. */
    uint32_t          rx_completions;
    uint32_t          tx_completions;
} cdc;

/*
 * Log-rate-limit flag for the "no USB device" message emitted by
 * cdc_ecm_probe_and_register when net_poll retries find no device.
 * File-scope (rather than function-local static) so cdc_ecm_reset()
 * can clear it — tests that want to observe the "no device" log
 * twice rely on that symmetry.
 */
static bool cdc_logged_no_device;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static uint8_t *cdc_rx_buf(struct cdc_rx_slot *slot)
{
    return slot->buf;
}

static uint8_t *cdc_tx_buf(struct cdc_tx_slot *slot)
{
    return slot->buf;
}

static uint8_t *cdc_notify_buf(struct cdc_notify_slot *slot)
{
    return slot->buf;
}

static int cdc_ecm_dma_init(void)
{
#if defined(PLATFORM_HAS_NC_MEMORY)
    static bool logged_dma_pool;

    for (unsigned i = 0; i < CDC_ECM_RX_SLOTS; i++) {
        if (cdc.rx[i].buf == NULL) {
            cdc.rx[i].buf = ncmem_alloc(CDC_ECM_BUF_SIZE, 64);
            if (cdc.rx[i].buf == NULL) {
                WARN("cdc_ecm: NC RX buffer alloc failed (slot %u)", i);
                return NET_E_NO_MEM;
            }
            memset(cdc.rx[i].buf, 0, CDC_ECM_BUF_SIZE);
        }
    }

    for (unsigned i = 0; i < CDC_ECM_TX_SLOTS; i++) {
        if (cdc.tx[i].buf == NULL) {
            cdc.tx[i].buf = ncmem_alloc(CDC_ECM_BUF_SIZE, 64);
            if (cdc.tx[i].buf == NULL) {
                WARN("cdc_ecm: NC TX buffer alloc failed (slot %u)", i);
                return NET_E_NO_MEM;
            }
            memset(cdc.tx[i].buf, 0, CDC_ECM_BUF_SIZE);
        }
    }

    if (cdc.notif.buf == NULL) {
        cdc.notif.buf = ncmem_alloc(CDC_ECM_NOTIFY_BUF_SIZE, 64);
        if (cdc.notif.buf == NULL) {
            WARN("cdc_ecm: NC notify buffer alloc failed");
            return NET_E_NO_MEM;
        }
        memset(cdc.notif.buf, 0, CDC_ECM_NOTIFY_BUF_SIZE);
    }

    if (!logged_dma_pool) {
        INFO("cdc_ecm: NC DMA pools ready (rx=%u tx=%u size=%u notify=%u)",
             (unsigned)CDC_ECM_RX_SLOTS,
             (unsigned)CDC_ECM_TX_SLOTS,
             (unsigned)CDC_ECM_BUF_SIZE,
             (unsigned)CDC_ECM_NOTIFY_BUF_SIZE);
        logged_dma_pool = true;
    }
#endif
    return NET_OK;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
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

    __atomic_fetch_add(&cdc.rx_completions, 1, __ATOMIC_RELAXED);

    if (urb->status == USB_URB_OK || urb->status == USB_URB_SHORT) {
        /* Write the payload metadata first, THEN publish `ready` with
         * RELEASE semantics. A consumer on another CPU that observes
         * `ready == true` via ACQUIRE is then guaranteed to see the
         * matching `len` (and the HCD's writes to `buf`, which
         * happened-before this completion fired). */
        slot->len = urb->actual_length;
        __atomic_store_n(&slot->ready, true, __ATOMIC_RELEASE);
    } else {
        /* Error / cancel: leave the slot unready and re-submit later
         * in cdc_ecm_poll(). Relaxed is fine — no data to publish. */
        slot->len = 0;
        __atomic_store_n(&slot->ready, false, __ATOMIC_RELAXED);
    }
}

static void cdc_tx_complete(struct usb_urb *urb)
{
    struct cdc_tx_slot *slot = (struct cdc_tx_slot *)urb->context;
    __atomic_fetch_add(&cdc.tx_completions, 1, __ATOMIC_RELAXED);
    /* RELEASE pairs with the ACQUIRE in tx_reap: any prior HCD write
     * to the URB (actual_length, status) is visible to the reaper
     * before the slot returns to the free pool. */
    __atomic_store_n(&slot->completed, true, __ATOMIC_RELEASE);
}

static void cdc_notify_complete(struct usb_urb *urb)
{
    struct cdc_notify_slot *slot = (struct cdc_notify_slot *)urb->context;
    __atomic_store_n(&slot->in_use, false, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->completed, true, __ATOMIC_RELEASE);
}

/* -------------------------------------------------------------------------- */
/* RX re-submission                                                            */
/* -------------------------------------------------------------------------- */

static int cdc_rx_submit(struct cdc_rx_slot *slot)
{
    slot->urb.dev           = cdc.dev;
    slot->urb.endpoint      = cdc.bulk_in->address;
    slot->urb.transfer_type = USB_XFER_BULK;
    slot->urb.buffer        = cdc_rx_buf(slot);
    slot->urb.length        = CDC_ECM_BUF_SIZE;
    slot->urb.actual_length = 0;
    slot->urb.complete      = cdc_rx_complete;
    slot->urb.context       = slot;
    slot->urb.status        = USB_URB_PENDING;
    return usb_submit_urb(&slot->urb);
}

static int cdc_notify_submit(void)
{
    if (cdc.notif_in == NULL)
        return NET_OK;

    if (__atomic_load_n(&cdc.notif.in_use, __ATOMIC_ACQUIRE))
        return NET_OK;

    cdc.notif.urb.dev           = cdc.dev;
    cdc.notif.urb.endpoint      = cdc.notif_in->address;
    cdc.notif.urb.transfer_type = USB_XFER_INTERRUPT;
    cdc.notif.urb.buffer        = cdc_notify_buf(&cdc.notif);
    cdc.notif.urb.length        = CDC_ECM_NOTIFY_BUF_SIZE;
    cdc.notif.urb.actual_length = 0;
    cdc.notif.urb.complete      = cdc_notify_complete;
    cdc.notif.urb.context       = &cdc.notif;
    cdc.notif.urb.status        = USB_URB_PENDING;
    __atomic_store_n(&cdc.notif.completed, false, __ATOMIC_RELAXED);
    __atomic_store_n(&cdc.notif.in_use, true, __ATOMIC_RELEASE);

    int rc = usb_submit_urb(&cdc.notif.urb);
    if (rc != 0) {
        __atomic_store_n(&cdc.notif.in_use, false, __ATOMIC_RELAXED);
        WARN("cdc_ecm: notify URB submit failed (ep=0x%02x rc=%d)",
             cdc.notif_in->address, rc);
        return NET_E_GENERIC;
    }

    return NET_OK;
}

static void cdc_notify_handle_event(void)
{
    if (!__atomic_load_n(&cdc.notif.completed, __ATOMIC_ACQUIRE))
        return;

    __atomic_store_n(&cdc.notif.completed, false, __ATOMIC_RELAXED);

    if (cdc.notif.urb.status != USB_URB_OK &&
        cdc.notif.urb.status != USB_URB_SHORT) {
        WARN("cdc_ecm: notification URB completed with status %d",
             cdc.notif.urb.status);
        (void)cdc_notify_submit();
        return;
    }

    if (cdc.notif.urb.actual_length < sizeof(struct usb_cdc_notification)) {
        WARN("cdc_ecm: short notification (%u bytes)",
             cdc.notif.urb.actual_length);
        (void)cdc_notify_submit();
        return;
    }

    const uint8_t *buf = cdc_notify_buf(&cdc.notif);
    const struct usb_cdc_notification *n =
        (const struct usb_cdc_notification *)buf;
    uint16_t wValue  = le16(buf + 2);
    uint16_t wLength = le16(buf + 6);

    switch (n->bNotificationType) {
    case CDC_NOTIFY_NETWORK_CONNECTION: {
        bool up = (wValue != 0);
        bool prev_valid = __atomic_load_n(&cdc.link_signal_valid, __ATOMIC_RELAXED);
        bool prev_up    = __atomic_load_n(&cdc.link_ready, __ATOMIC_RELAXED);
        __atomic_store_n(&cdc.link_ready, up, __ATOMIC_RELAXED);
        __atomic_store_n(&cdc.link_signal_valid, true, __ATOMIC_RELEASE);
        if (!prev_valid || prev_up != up) {
            INFO("cdc_ecm: network connection %s (if=%u)",
                 up ? "up" : "down",
                 (unsigned)cdc.ctrl_iface_num);
        }
        break;
    }

    case CDC_NOTIFY_CONNECTION_SPEED_CHANGE:
        if (wLength >= 8 &&
            cdc.notif.urb.actual_length >= sizeof(struct usb_cdc_notification) + 8) {
            cdc.upstream_bps = le32(buf + 8);
            cdc.downstream_bps = le32(buf + 12);
            if (!__atomic_load_n(&cdc.link_signal_valid, __ATOMIC_RELAXED) &&
                (cdc.upstream_bps != 0 || cdc.downstream_bps != 0)) {
                __atomic_store_n(&cdc.link_ready, true, __ATOMIC_RELAXED);
                __atomic_store_n(&cdc.link_signal_valid, true, __ATOMIC_RELEASE);
                INFO("cdc_ecm: inferred link up from speed change (%u/%u bps)",
                     cdc.upstream_bps, cdc.downstream_bps);
            } else {
                INFO("cdc_ecm: speed change upstream=%u downstream=%u",
                     cdc.upstream_bps, cdc.downstream_bps);
            }
        }
        break;

    default:
        break;
    }

    (void)cdc_notify_submit();
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
    if (!__atomic_load_n(&cdc.probed, __ATOMIC_ACQUIRE))
        return NET_E_NOT_INIT;
    int dma_rc = cdc_ecm_dma_init();
    if (dma_rc != NET_OK)
        return dma_rc;
    for (unsigned i = 0; i < CDC_ECM_RX_SLOTS; i++) {
        int rc = cdc_rx_submit(&cdc.rx[i]);
        if (rc != 0) {
            WARN("cdc_ecm: initial RX submit failed (slot %u rc=%d)", i, rc);
            return NET_E_GENERIC;
        }
    }

    if (cdc.notif_in != NULL) {
        int rc = cdc_notify_submit();
        if (rc != NET_OK) {
            WARN("cdc_ecm: continuing without notification-driven link state");
            cdc.notif_in = NULL;
        }
    }
    return NET_OK;
}

static int cdc_ecm_net_send(const void *buf, size_t len)
{
    if (!__atomic_load_n(&cdc.probed, __ATOMIC_ACQUIRE))
        return NET_E_NOT_INIT;
    if (buf == NULL || len == 0)
        return NET_E_INVAL;
    if (len > CDC_ECM_BUF_SIZE)
        return NET_E_TOO_LARGE;

    for (unsigned i = 0; i < CDC_ECM_TX_SLOTS; i++) {
        struct cdc_tx_slot *slot = &cdc.tx[i];
        /*
         * Atomic compare-exchange reserves the slot. If two callers
         * race on the same `send()` (future multi-writer path), only
         * one wins the CAS; the other moves to the next slot or
         * returns NET_E_BUSY.
         *
         * Success uses ACQUIRE so the CAS pairs with the RELEASE
         * store on `in_use = false` in tx_reap and the fail-path
         * undo below. This makes the ordering explicit at the
         * class-driver layer rather than relying on an
         * HCD-provided DSB later in the submit path. The failure
         * memory order stays RELAXED — a failed CAS observed no
         * published data and needs no synchronisation.
         */
        bool expected = false;
        if (!__atomic_compare_exchange_n(&slot->in_use, &expected, true,
                                         false /* strong */,
                                         __ATOMIC_ACQUIRE,
                                         __ATOMIC_RELAXED))
            continue;

        memcpy(cdc_tx_buf(slot), buf, len);
        slot->urb.dev           = cdc.dev;
        slot->urb.endpoint      = cdc.bulk_out->address;
        slot->urb.transfer_type = USB_XFER_BULK;
        slot->urb.buffer        = cdc_tx_buf(slot);
        slot->urb.length        = (uint32_t)len;
        slot->urb.actual_length = 0;
        slot->urb.complete      = cdc_tx_complete;
        slot->urb.context       = slot;
        slot->urb.status        = USB_URB_PENDING;
        /* Clear completed BEFORE the URB is visible to the HCD so a
         * fast completion can't be observed as "reaped already". */
        __atomic_store_n(&slot->completed, false, __ATOMIC_RELAXED);

        int rc = usb_submit_urb(&slot->urb);
        if (rc != 0) {
            /* Undo reservation if submit rejected us. RELEASE because
             * the slot's buffer was written; a later claimant using
             * RELAXED CAS must see those writes done. */
            __atomic_store_n(&slot->in_use, false, __ATOMIC_RELEASE);
            return NET_E_GENERIC;
        }
        return NET_OK;
    }
    return NET_E_BUSY;
}

static int cdc_ecm_net_recv(void *buf, size_t max_len)
{
    if (!__atomic_load_n(&cdc.probed, __ATOMIC_ACQUIRE))
        return NET_E_NOT_INIT;
    if (buf == NULL)
        return NET_E_INVAL;

    for (unsigned i = 0; i < CDC_ECM_RX_SLOTS; i++) {
        struct cdc_rx_slot *slot = &cdc.rx[i];
        /* ACQUIRE pairs with the RELEASE store in cdc_rx_complete —
         * if we observe `ready == true`, `slot->len` and
         * `slot->buf` are guaranteed to reflect the latest
         * completion. */
        if (!__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE))
            continue;
        uint32_t len = slot->len;
        if (len > max_len)
            len = (uint32_t)max_len;
        if (len > 0)
            memcpy(buf, cdc_rx_buf(slot), len);
        /* Release the slot before re-submitting so a fast completion
         * after re-submit doesn't race us into a double-processed
         * frame. RELAXED is fine — `ready = false` doesn't publish
         * any data; the next completion's RELEASE is what pairs. */
        __atomic_store_n(&slot->ready, false, __ATOMIC_RELAXED);
        slot->len   = 0;
        (void)cdc_rx_submit(slot);
        return (int)len;
    }
    return 0;   /* nothing ready — caller retries on next poll */
}

static void cdc_ecm_net_tx_reap(void)
{
    /* Drive the HCD so completions are visible on poll-only paths. */
    usb_core_poll();
    cdc_notify_handle_event();

    for (unsigned i = 0; i < CDC_ECM_TX_SLOTS; i++) {
        struct cdc_tx_slot *slot = &cdc.tx[i];
        /* ACQUIRE pairs with the RELEASE in cdc_tx_complete: once we
         * see `completed == true`, all HCD writes to the URB before
         * the completion callback ran are visible here. */
        if (!__atomic_load_n(&slot->completed, __ATOMIC_ACQUIRE))
            continue;
        /* Clear completed first, then in_use. RELEASE on in_use
         * ensures a fresh send() that CAS-wins the slot sees the
         * cleared `completed` flag set by this thread. */
        __atomic_store_n(&slot->completed, false, __ATOMIC_RELAXED);
        __atomic_store_n(&slot->in_use,    false, __ATOMIC_RELEASE);
    }
}

static void cdc_ecm_net_get_mac(uint8_t mac[6])
{
    memcpy(mac, cdc.mac, 6);
}

static bool cdc_ecm_net_link_status(void)
{
    if (!__atomic_load_n(&cdc.probed, __ATOMIC_ACQUIRE))
        return false;
    if (cdc.notif_in == NULL)
        return true;
    if (!__atomic_load_n(&cdc.link_signal_valid, __ATOMIC_ACQUIRE))
        return false;
    return __atomic_load_n(&cdc.link_ready, __ATOMIC_ACQUIRE);
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
    /* Idempotent once a probe has successfully bound. Phase 4 drives
     * this from net_poll() on every tick so a device that enumerates
     * after boot (e.g. Jetson post-kexec re-plug, see #309) gets
     * picked up without a dedicated callback path — but we must not
     * re-run the full probe on every tick once a device is bound, or
     * RX URB resubmission + pool counters would drift. ACQUIRE pairs
     * with the RELEASE at the end of a successful probe below. */
    if (__atomic_load_n(&cdc.probed, __ATOMIC_ACQUIRE))
        return NET_OK;

    /* Every probe starts from a clean slate so a subsequent call that
     * finds no device (or a non-CDC device) can't leave stale state
     * visible via cdc_ecm_get_mac() / cdc_ecm_net_link_status().
     * RELAXED is fine here because no data has been published yet —
     * the matching ACQUIRE in readers synchronises against the final
     * RELEASE store at the end of probe. */
    __atomic_store_n(&cdc.probed, false, __ATOMIC_RELAXED);

    struct usb_device *dev = usb_core_first_device();
    if (dev == NULL) {
        /* Log once per boot — repeated "no device" messages from the
         * net_poll retry loop would flood the console. The flag is at
         * file scope (see cdc_logged_no_device above) so cdc_ecm_reset()
         * can clear it for test fixtures. */
        if (!cdc_logged_no_device) {
            INFO("cdc_ecm: no USB device — will retry on hot-plug");
            cdc_logged_no_device = true;
        }
        return NET_OK;   /* "disabled" path is not an error */
    }

    /* Find a CDC-ECM control interface (class 0x02 + subclass 0x06)
     * and the CDC data interface (class 0x0A). CDC-ECM pairs them but
     * the interface numbers are device-specific, so scan. */
    const struct usb_interface *ctrl_iface = NULL;
    const struct usb_interface *data_iface = NULL;
    for (unsigned i = 0; i < sizeof(dev->ifaces) / sizeof(dev->ifaces[0]); i++) {
        if (!dev->ifaces[i].valid) continue;
        uint8_t cls = dev->ifaces[i].class_code;
        uint8_t sub = dev->ifaces[i].subclass;
        if (cls == 0x02 && sub == CDC_SUBCLASS_ECM && ctrl_iface == NULL)
            ctrl_iface = &dev->ifaces[i];
        else if (cls == CDC_DATA_INTERFACE_CLASS && data_iface == NULL)
            data_iface = &dev->ifaces[i];
    }
    if (ctrl_iface == NULL || data_iface == NULL) {
        int ctrl_if = (ctrl_iface != NULL) ? (int)ctrl_iface->number : -1;
        int data_if = (data_iface != NULL) ? (int)data_iface->number : -1;
        WARN("cdc_ecm: no CDC-ECM interface pair (ctrl=%d data=%d)",
             ctrl_if, data_if);
        return -1;
    }

    if (data_iface->alt_setting != 0) {
        int rc = usb_control_msg(dev,
                                 USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
                                 USB_REQ_SET_INTERFACE,
                                 data_iface->alt_setting,
                                 data_iface->number,
                                 NULL, 0, 500);
        if (rc < 0) {
            WARN("cdc_ecm: SET_INTERFACE(if=%u alt=%u) failed: %s",
                 data_iface->number, data_iface->alt_setting,
                 usb_urb_status_str((enum usb_urb_status)(-rc)));
            return rc;
        }
        INFO("cdc_ecm: activated interface %u alt %u",
             data_iface->number, data_iface->alt_setting);
    }

    const struct usb_endpoint *notif =
        usb_find_endpoint(dev, ctrl_iface->number, USB_DIR_IN, USB_XFER_INTERRUPT);
    const struct usb_endpoint *in =
        usb_find_endpoint(dev, data_iface->number, USB_DIR_IN, USB_XFER_BULK);
    const struct usb_endpoint *out =
        usb_find_endpoint(dev, data_iface->number, USB_DIR_OUT, USB_XFER_BULK);
    if (in == NULL || out == NULL) {
        WARN("cdc_ecm: data iface %u alt %u missing bulk IN or OUT",
             data_iface->number, data_iface->alt_setting);
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

    cdc.dev           = dev;
    cdc.ctrl_iface_num = ctrl_iface->number;
    cdc.bulk_in       = in;
    cdc.bulk_out      = out;
    cdc.notif_in      = notif;
    cdc.link_ready    = false;
    cdc.link_signal_valid = false;
    cdc.upstream_bps  = 0;
    cdc.downstream_bps = 0;
    cdc.notif.in_use  = false;
    cdc.notif.completed = false;
    /* Zero slot state — fresh arrays on every probe. Probe runs from
     * single-threaded init context so plain stores are sufficient;
     * the atomic ops kick in once net_init queues the first URBs. */
    for (unsigned i = 0; i < CDC_ECM_RX_SLOTS; i++) {
        cdc.rx[i].ready = false;
        cdc.rx[i].len = 0;
    }
    for (unsigned i = 0; i < CDC_ECM_TX_SLOTS; i++) {
        cdc.tx[i].in_use = false;
        cdc.tx[i].completed = false;
    }
    /* RELEASE: publish cdc.dev / bulk_in / bulk_out / mac / max_segment
     * + the freshly-zeroed slot state before any data-path op sees
     * `probed == true` and dereferences them. Every reader in this
     * file pairs an ACQUIRE load. */
    __atomic_store_n(&cdc.probed, true, __ATOMIC_RELEASE);

    INFO("cdc_ecm: probed — mac=%02x:%02x:%02x:%02x:%02x:%02x mtu=%u "
         "bulk_in=0x%02x bulk_out=0x%02x",
         cdc.mac[0], cdc.mac[1], cdc.mac[2],
         cdc.mac[3], cdc.mac[4], cdc.mac[5],
         cdc.max_segment, in->address, out->address);
    if (notif != NULL) {
        INFO("cdc_ecm: notification endpoint 0x%02x on ctrl iface %u",
             notif->address, (unsigned)ctrl_iface->number);
    } else {
        WARN("cdc_ecm: no notification endpoint on ctrl iface %u; falling back to probe-based link up",
             (unsigned)ctrl_iface->number);
    }

    net_register_driver(&cdc_ecm_driver);
    return NET_OK;
}

void cdc_ecm_poll(void)
{
    if (__atomic_load_n(&cdc.probed, __ATOMIC_ACQUIRE)) {
        usb_core_poll();
        cdc_notify_handle_event();
    }
}

void cdc_ecm_reset(void)
{
    /* Test-only: clear the bound-to-device state so the next probe
     * re-runs from scratch. Production code relies on the idempotent
     * bind contract and should never call this.
     *
     * Also clears the "no device" log-once flag so a test that
     * re-probes an empty bus after reset sees the log message fire
     * again — keeps the reset/log-emit symmetry explicit. */
    if (__atomic_load_n(&cdc.notif.in_use, __ATOMIC_ACQUIRE))
        (void)usb_cancel_urb(&cdc.notif.urb);
    __atomic_store_n(&cdc.probed, false, __ATOMIC_RELEASE);
    cdc_logged_no_device = false;
    cdc.notif_in = NULL;
    __atomic_store_n(&cdc.link_signal_valid, false, __ATOMIC_RELAXED);
    __atomic_store_n(&cdc.link_ready, false, __ATOMIC_RELAXED);
}

const uint8_t *cdc_ecm_get_mac(void)
{
    return __atomic_load_n(&cdc.probed, __ATOMIC_ACQUIRE) ? cdc.mac : NULL;
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
