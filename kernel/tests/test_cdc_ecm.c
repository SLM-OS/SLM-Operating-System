/*
 * test_cdc_ecm.c - Phase 2 CDC-ECM class driver unit tests.
 *
 * The tests use a local mock HCD that replays a CDC-ECM-shaped
 * device:
 *   - 1 control interface (class 0x02 subclass 0x06 ECM) with the
 *     Ethernet functional descriptor and an interrupt IN notification
 *     endpoint
 *   - 1 data interface (class 0x0A) with a bulk IN + bulk OUT
 *   - A string descriptor at iMACAddress encoding a known 6-byte MAC
 *
 * The mock is intentionally separate from the Phase-1 test mock to
 * keep the two test suites independent (they exercise different
 * layers; sharing state would make regressions hard to isolate).
 */

#include "unity.h"
#include "usb.h"
#include "cdc_ecm.h"
#include "net.h"
#include "net_driver.h"
#include "test_harness.h"
#include <string.h>

extern void sleep_ms(uint32_t ms);
extern void cdc_ecm_set_notify_silence_timeout_ms(uint32_t ms);
extern uint32_t cdc_ecm_get_notify_silence_timeout_ms(void);
extern void cdc_ecm_test_force_notify_wait(uint32_t started_ms, bool active);

/* -------------------------------------------------------------------------- */
/* Canned device blob                                                          */
/* -------------------------------------------------------------------------- */

/* A real Realtek RTL8153's MAC encoded as UTF-16LE of 12 ASCII hex chars. */
static const uint8_t EXPECTED_MAC[6] = {
    0x02, 0x11, 0x22, 0x33, 0x44, 0x55
};
#define MOCK_IMAC_INDEX 3

/*
 * CDC-ECM functional-descriptor subtype. Defined here, before the
 * config blob that references it; the class driver has its own copy
 * in cdc_ecm.c.
 */
#define CDC_ECM_FD_SUBTYPE 0x0F
#define CDC_NOTIFY_NETWORK_CONNECTION      0x00
#define CDC_NOTIFY_CONNECTION_SPEED_CHANGE 0x2A

/* iMACAddress string descriptor (bLength=26, type=STRING, 12 UTF-16LE hex). */
static const uint8_t mac_string_desc[26] = {
    26, USB_DT_STRING,
    '0', 0, '2', 0, '1', 0, '1', 0,
    '2', 0, '2', 0, '3', 0, '3', 0,
    '4', 0, '4', 0, '5', 0, '5', 0,
};

static const struct usb_device_descriptor mock_dev_desc = {
    .bLength            = 18,
    .bDescriptorType    = USB_DT_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x02,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 64,
    .idVendor           = 0x0BDA,
    .idProduct          = 0x8153,
    .bcdDevice          = 0x3000,
    .iManufacturer      = 0,
    .iProduct           = 0,
    .iSerialNumber      = 0,
    .bNumConfigurations = 1,
};

/*
 * Config descriptor layout (total length 75):
 *   9   CONFIGURATION
 *   9   INTERFACE 0 — CDC control, class 0x02 subclass 0x06 (ECM)
 *   5   CS_INTERFACE — header functional descriptor
 *  13   CS_INTERFACE — Ethernet Networking functional descriptor (iMAC=3)
 *   7   INTERRUPT IN endpoint 0x81
 *   9   INTERFACE 1 alt 0 — CDC data (no endpoints)
 *   9   INTERFACE 1 alt 0 — CDC data with 2 bulk endpoints
 *   7   BULK IN  endpoint 0x82
 *   7   BULK OUT endpoint 0x02
 */
#define CFG_TOTAL 66
static const uint8_t mock_config[CFG_TOTAL] = {
    /* CONFIGURATION */
    9, USB_DT_CONFIG, CFG_TOTAL, 0x00, 0x02, 0x01, 0x00, 0xC0, 0x32,

    /* INTERFACE 0 — CDC control */
    9, USB_DT_INTERFACE, 0x00, 0x00, 0x01, 0x02, 0x06, 0x00, 0x00,

    /* CS_INTERFACE header (5 bytes) — keeps the parser walking */
    5, USB_DT_CS_INTERFACE, 0x00, 0x10, 0x01,

    /* CS_INTERFACE Ethernet Networking functional (13 bytes) */
    13, USB_DT_CS_INTERFACE, CDC_ECM_FD_SUBTYPE,
    MOCK_IMAC_INDEX,
    0x00, 0x00, 0x00, 0x00,       /* bmEthernetStatistics */
    0xEA, 0x05,                   /* wMaxSegmentSize = 1514 */
    0x00, 0x00,                   /* wNumberMCFilters */
    0x00,                         /* bNumberPowerFilters */

    /* INTERRUPT IN endpoint 0x81 (16 bytes, interval 8) */
    7, USB_DT_ENDPOINT, 0x81, USB_XFER_INTERRUPT, 0x10, 0x00, 0x08,

    /* INTERFACE 1 alt 0 — CDC data with 2 bulk endpoints */
    9, USB_DT_INTERFACE, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,

    /* BULK IN endpoint 0x82 (512 bytes) */
    7, USB_DT_ENDPOINT, 0x82, USB_XFER_BULK, 0x00, 0x02, 0x00,

    /* BULK OUT endpoint 0x02 (512 bytes) */
    7, USB_DT_ENDPOINT, 0x02, USB_XFER_BULK, 0x00, 0x02, 0x00,
};

/* -------------------------------------------------------------------------- */
/* Mock HCD                                                                    */
/* -------------------------------------------------------------------------- */

#define MOCK_MAX_INFLIGHT 8

static struct {
    uint8_t          device_addr;
    uint8_t          device_configured;
    int              submit_count;
    /* URBs currently submitted by the class driver. Not completed
     * synchronously — the test scripts call mock_complete_next() to
     * drive the RX/TX paths step-by-step. */
    struct usb_urb  *in_flight[MOCK_MAX_INFLIGHT];
    int              bulk_in_submits;
    int              bulk_out_submits;
    int              interrupt_in_submits;
    int              poll_count;
    /* Per-submission payload for the next bulk-IN to return. */
    const uint8_t   *next_rx_payload;
    uint32_t         next_rx_len;
    /* Captured data from the last bulk-OUT URB. */
    uint8_t          last_tx_buf[2048];
    uint32_t         last_tx_len;
} mock;

static int mock_start(void) { return 0; }
static bool mock_port_status(uint8_t port, bool *connected, enum usb_speed *speed)
{
    (void)port;
    *connected = true;
    *speed     = USB_SPEED_HIGH;
    return true;
}
static int mock_port_reset(uint8_t port) { (void)port; return 0; }
static int mock_device_open(struct usb_device *dev) { (void)dev; return 0; }
static void mock_device_close(struct usb_device *dev) { (void)dev; }
static int mock_endpoint_configure(struct usb_device *dev,
                                   const struct usb_endpoint *ep)
{ (void)dev; (void)ep; return 0; }

static void mock_store(struct usb_urb *urb)
{
    for (int i = 0; i < MOCK_MAX_INFLIGHT; i++) {
        if (mock.in_flight[i] == NULL) { mock.in_flight[i] = urb; return; }
    }
}

static int mock_cancel_urb(struct usb_urb *urb)
{
    for (int i = 0; i < MOCK_MAX_INFLIGHT; i++) {
        if (mock.in_flight[i] == urb) {
            mock.in_flight[i] = NULL;
            urb->status = USB_URB_CANCELLED;
            if (urb->complete) urb->complete(urb);
            return 0;
        }
    }
    return 0;
}

static int mock_submit_urb(struct usb_urb *urb)
{
    mock.submit_count++;
    switch (urb->transfer_type) {
    case USB_XFER_CONTROL: {
        uint8_t req = urb->setup.bRequest;
        uint8_t type = urb->setup.wValue >> 8;
        uint8_t idx  = urb->setup.wValue & 0xFF;
        if (req == USB_REQ_GET_DESCRIPTOR && type == USB_DT_DEVICE && idx == 0) {
            uint32_t n = urb->length < sizeof(mock_dev_desc) ? urb->length : sizeof(mock_dev_desc);
            memcpy(urb->buffer, &mock_dev_desc, n);
            urb->actual_length = n;
            urb->status = (n < urb->length) ? USB_URB_SHORT : USB_URB_OK;
        } else if (req == USB_REQ_GET_DESCRIPTOR && type == USB_DT_CONFIG && idx == 0) {
            uint32_t n = urb->length < sizeof(mock_config) ? urb->length : sizeof(mock_config);
            memcpy(urb->buffer, mock_config, n);
            urb->actual_length = n;
            urb->status = (n < urb->length) ? USB_URB_SHORT : USB_URB_OK;
        } else if (req == USB_REQ_GET_DESCRIPTOR && type == USB_DT_STRING && idx == MOCK_IMAC_INDEX) {
            uint32_t n = urb->length < sizeof(mac_string_desc) ? urb->length : sizeof(mac_string_desc);
            memcpy(urb->buffer, mac_string_desc, n);
            urb->actual_length = n;
            urb->status = (n < urb->length) ? USB_URB_SHORT : USB_URB_OK;
        } else if (req == USB_REQ_SET_ADDRESS) {
            mock.device_addr = (uint8_t)urb->setup.wValue;
            urb->status = USB_URB_OK;
        } else if (req == USB_REQ_SET_CONFIGURATION) {
            mock.device_configured = (uint8_t)urb->setup.wValue;
            urb->status = USB_URB_OK;
        } else {
            urb->status = USB_URB_IO_ERROR;
        }
        if (urb->complete) urb->complete(urb);
        return 0;
    }
    case USB_XFER_BULK:
        if (urb->endpoint & USB_DIR_IN) {
            mock.bulk_in_submits++;
            /* Queue for later completion via mock_complete_pending_rx. */
            mock_store(urb);
        } else {
            mock.bulk_out_submits++;
            uint32_t n = urb->length;
            if (n > sizeof(mock.last_tx_buf)) n = sizeof(mock.last_tx_buf);
            memcpy(mock.last_tx_buf, urb->buffer, n);
            mock.last_tx_len = n;
            /* Complete synchronously — real XHCI might defer, but
             * for the test TX path we model immediate hand-off. */
            urb->status = USB_URB_OK;
            urb->actual_length = urb->length;
            if (urb->complete) urb->complete(urb);
        }
        return 0;
    case USB_XFER_INTERRUPT:
        if (urb->endpoint & USB_DIR_IN) {
            mock.interrupt_in_submits++;
            mock_store(urb);
            return 0;
        }
        urb->status = USB_URB_IO_ERROR;
        if (urb->complete) urb->complete(urb);
        return 0;
    default:
        urb->status = USB_URB_IO_ERROR;
        if (urb->complete) urb->complete(urb);
        return 0;
    }
}

static void mock_poll(void) { mock.poll_count++; }

static const struct usb_hcd mock_hcd_ops = {
    .name               = "cdc-ecm-test-hcd",
    .start              = mock_start,
    .port_status        = mock_port_status,
    .port_reset         = mock_port_reset,
    .device_open        = mock_device_open,
    .device_close       = mock_device_close,
    .endpoint_configure = mock_endpoint_configure,
    .submit_urb         = mock_submit_urb,
    .cancel_urb         = mock_cancel_urb,
    .poll               = mock_poll,
};

/*
 * Complete one pending bulk-IN URB with either the canned payload
 * (if set) or a synthetic one. Returns the URB completed or NULL if
 * none was pending.
 */
static struct usb_urb *mock_complete_pending_rx(const uint8_t *payload,
                                                 uint32_t len)
{
    for (int i = 0; i < MOCK_MAX_INFLIGHT; i++) {
        struct usb_urb *urb = mock.in_flight[i];
        if (urb == NULL) continue;
        if (urb->transfer_type != USB_XFER_BULK) continue;
        if (!(urb->endpoint & USB_DIR_IN)) continue;
        uint32_t n = len > urb->length ? urb->length : len;
        if (payload && n > 0)
            memcpy(urb->buffer, payload, n);
        urb->actual_length = n;
        urb->status = USB_URB_OK;
        mock.in_flight[i] = NULL;
        if (urb->complete) urb->complete(urb);
        return urb;
    }
    return NULL;
}

static struct usb_urb *mock_complete_pending_notify(uint8_t type,
                                                    uint16_t wValue,
                                                    const uint8_t *payload,
                                                    uint16_t payload_len)
{
    for (int i = 0; i < MOCK_MAX_INFLIGHT; i++) {
        struct usb_urb *urb = mock.in_flight[i];
        if (urb == NULL) continue;
        if (urb->transfer_type != USB_XFER_INTERRUPT) continue;
        if (!(urb->endpoint & USB_DIR_IN)) continue;

        uint8_t msg[16] = {
            USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
            type,
            (uint8_t)(wValue & 0xFF), (uint8_t)(wValue >> 8),
            0x00, 0x00, /* wIndex = ctrl iface 0 in the mock */
            (uint8_t)(payload_len & 0xFF), (uint8_t)(payload_len >> 8),
        };
        if (payload != NULL && payload_len > 0)
            memcpy(&msg[8], payload, payload_len);

        uint32_t n = (uint32_t)(8 + payload_len);
        if (n > urb->length)
            n = urb->length;
        memcpy(urb->buffer, msg, n);
        urb->actual_length = n;
        urb->status = USB_URB_OK;
        mock.in_flight[i] = NULL;
        if (urb->complete) urb->complete(urb);
        return urb;
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Test fixtures                                                               */
/* -------------------------------------------------------------------------- */

static void reset_all(void)
{
    memset(&mock, 0, sizeof(mock));
    usb_core_register_hcd(&mock_hcd_ops);
    /* Clear any prior probe state — the idempotent guard in
     * cdc_ecm_probe_and_register would otherwise skip the probe
     * on tests that run after an earlier case bound the driver. */
    cdc_ecm_reset();
    /* Run full enumeration so the CDC-ECM probe has something to bind to. */
    int rc = usb_core_start();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(usb_core_first_device());
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                       */
/* -------------------------------------------------------------------------- */

static void test_parse_mac_string_happy(void)
{
    uint8_t mac[6];
    int rc = cdc_ecm_parse_mac_string(mac_string_desc, sizeof(mac_string_desc), mac);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_MEMORY(EXPECTED_MAC, mac, 6);
}

static void test_parse_mac_string_rejects_short_buffer(void)
{
    uint8_t mac[6];
    /* 25-byte descriptor is one byte short of a full MAC string. */
    uint8_t short_desc[25];
    memcpy(short_desc, mac_string_desc, 25);
    TEST_ASSERT_NOT_EQUAL(0,
        cdc_ecm_parse_mac_string(short_desc, sizeof(short_desc), mac));
}

static void test_parse_mac_string_rejects_wrong_type(void)
{
    uint8_t mac[6];
    uint8_t bad[26];
    memcpy(bad, mac_string_desc, sizeof(bad));
    bad[1] = USB_DT_DEVICE;   /* not STRING */
    TEST_ASSERT_NOT_EQUAL(0,
        cdc_ecm_parse_mac_string(bad, sizeof(bad), mac));
}

static void test_parse_mac_string_rejects_non_ascii(void)
{
    uint8_t mac[6];
    uint8_t bad[26];
    memcpy(bad, mac_string_desc, sizeof(bad));
    bad[3] = 0x04;   /* high byte of first UTF-16 unit non-zero */
    TEST_ASSERT_NOT_EQUAL(0,
        cdc_ecm_parse_mac_string(bad, sizeof(bad), mac));
}

static void test_parse_mac_string_rejects_non_hex(void)
{
    uint8_t mac[6];
    uint8_t bad[26];
    memcpy(bad, mac_string_desc, sizeof(bad));
    bad[2] = 'g';   /* not a hex digit */
    TEST_ASSERT_NOT_EQUAL(0,
        cdc_ecm_parse_mac_string(bad, sizeof(bad), mac));
}

static void test_parse_mac_string_null_args(void)
{
    uint8_t mac[6];
    TEST_ASSERT_NOT_EQUAL(0, cdc_ecm_parse_mac_string(NULL, 26, mac));
    TEST_ASSERT_NOT_EQUAL(0, cdc_ecm_parse_mac_string(mac_string_desc, 26, NULL));
}

static void test_probe_binds_and_registers(void)
{
    reset_all();
    int rc = cdc_ecm_probe_and_register();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_MEMORY(EXPECTED_MAC, cdc_ecm_get_mac(), 6);
    TEST_ASSERT_EQUAL_UINT16(1514, cdc_ecm_get_max_segment());

    const struct net_driver *drv = net_get_driver();
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_STRING("usb-cdc-ecm", drv->name);

    /* Driver ops are all plumbed. */
    TEST_ASSERT_NOT_NULL(drv->init);
    TEST_ASSERT_NOT_NULL(drv->send);
    TEST_ASSERT_NOT_NULL(drv->recv);
    TEST_ASSERT_NOT_NULL(drv->get_mac);
    TEST_ASSERT_NOT_NULL(drv->link_status);
    TEST_ASSERT_NOT_NULL(drv->tx_reap);
    /* Notification-capable adapters stay link-down until a real CDC
     * notification arrives; the probe-based fallback is only for
     * devices with no usable notification path. */
    TEST_ASSERT_FALSE(drv->link_status());
}

static void test_probe_skips_when_no_device(void)
{
    /* Fresh core with no device connected: enumerate returns cleanly
     * with no device. cdc_ecm_probe_and_register must no-op. */
    memset(&mock, 0, sizeof(mock));
    static struct usb_hcd empty_hcd = {
        .name = "empty",
        .start = mock_start,
        .port_status = NULL,          /* no device */
        .submit_urb = mock_submit_urb,
        .cancel_urb = mock_cancel_urb,
    };
    usb_core_register_hcd(&empty_hcd);
    cdc_ecm_reset();                  /* clear any prior bound state */
    (void)usb_core_start();
    TEST_ASSERT_NULL(usb_core_first_device());

    int rc = cdc_ecm_probe_and_register();
    TEST_ASSERT_EQUAL_INT(0, rc);   /* disabled, not error */
    TEST_ASSERT_NULL(cdc_ecm_get_mac());   /* probe did not happen */
}

static void test_net_init_queues_rx_urbs(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());

    int before = mock.bulk_in_submits;
    int rc = net_get_driver()->init();
    TEST_ASSERT_EQUAL_INT(0, rc);
    /* One submit per RX slot (4) — exact match, not approximate. */
    TEST_ASSERT_EQUAL_INT(before + 4, mock.bulk_in_submits);
}

static void test_net_init_queues_notification_urb(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());

    int before = mock.interrupt_in_submits;
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());
    TEST_ASSERT_EQUAL_INT(before + 1, mock.interrupt_in_submits);
    TEST_ASSERT_FALSE(net_get_driver()->link_status());
}

static void test_link_status_tracks_network_connection_notification(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());
    TEST_ASSERT_FALSE(net_get_driver()->link_status());

    TEST_ASSERT_NOT_NULL(mock_complete_pending_notify(
        CDC_NOTIFY_NETWORK_CONNECTION, 1, NULL, 0));
    net_get_driver()->tx_reap();
    TEST_ASSERT_TRUE(net_get_driver()->link_status());

    TEST_ASSERT_NOT_NULL(mock_complete_pending_notify(
        CDC_NOTIFY_NETWORK_CONNECTION, 0, NULL, 0));
    net_get_driver()->tx_reap();
    TEST_ASSERT_FALSE(net_get_driver()->link_status());
}

static void test_link_status_infers_up_from_speed_change(void)
{
    static const uint8_t speed_payload[8] = {
        0x00, 0xe1, 0xf5, 0x05, /* 100,000,000 upstream */
        0x00, 0xe1, 0xf5, 0x05, /* 100,000,000 downstream */
    };

    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());
    TEST_ASSERT_FALSE(net_get_driver()->link_status());

    TEST_ASSERT_NOT_NULL(mock_complete_pending_notify(
        CDC_NOTIFY_CONNECTION_SPEED_CHANGE, 0, speed_payload, sizeof(speed_payload)));
    net_get_driver()->tx_reap();
    TEST_ASSERT_TRUE(net_get_driver()->link_status());
}

static void test_link_status_tracks_down_from_zero_speed_change(void)
{
    static const uint8_t up_payload[8] = {
        0x00, 0xe1, 0xf5, 0x05,
        0x00, 0xe1, 0xf5, 0x05,
    };
    static const uint8_t down_payload[8] = { 0 };

    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());
    TEST_ASSERT_FALSE(net_get_driver()->link_status());

    TEST_ASSERT_NOT_NULL(mock_complete_pending_notify(
        CDC_NOTIFY_CONNECTION_SPEED_CHANGE, 0, up_payload, sizeof(up_payload)));
    net_get_driver()->tx_reap();
    TEST_ASSERT_TRUE(net_get_driver()->link_status());

    TEST_ASSERT_NOT_NULL(mock_complete_pending_notify(
        CDC_NOTIFY_CONNECTION_SPEED_CHANGE, 0, down_payload, sizeof(down_payload)));
    net_get_driver()->tx_reap();
    TEST_ASSERT_FALSE(net_get_driver()->link_status());
}

static void test_link_status_falls_back_after_silent_notification_timeout(void)
{
    uint32_t saved_timeout = cdc_ecm_get_notify_silence_timeout_ms();

    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());
    cdc_ecm_set_notify_silence_timeout_ms(10);

    TEST_ASSERT_FALSE(net_get_driver()->link_status());
    sleep_ms(20);
    TEST_ASSERT_TRUE(net_get_driver()->link_status());

    cdc_ecm_set_notify_silence_timeout_ms(saved_timeout);
}

static void test_link_status_falls_back_when_notify_wait_started_at_zero(void)
{
    uint32_t saved_timeout = cdc_ecm_get_notify_silence_timeout_ms();

    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());
    cdc_ecm_set_notify_silence_timeout_ms(0);
    cdc_ecm_test_force_notify_wait(0, true);

    TEST_ASSERT_TRUE(net_get_driver()->link_status());

    cdc_ecm_set_notify_silence_timeout_ms(saved_timeout);
}

static void test_link_status_falls_back_when_notification_endpoint_missing(void)
{
    reset_all();
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);
    /* Remove the control notification endpoint from the parsed iface. */
    dev->ifaces[0].ep_index[0] = -1;
    dev->endpoints[0].valid = false;

    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_TRUE(net_get_driver()->link_status());
}

static void test_send_goes_to_bulk_out(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());

    uint32_t tx_before = cdc_ecm_get_tx_count();
    const uint8_t payload[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* dst MAC (broadcast) */
        0x02, 0x11, 0x22, 0x33, 0x44, 0x55,   /* src MAC */
        0x08, 0x06,                           /* ethertype = ARP */
        0xAA, 0xBB, 0xCC, 0xDD,               /* payload */
    };
    int rc = net_get_driver()->send(payload, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), mock.last_tx_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, mock.last_tx_buf, sizeof(payload));
    /* Mock completes TX synchronously inside submit — the counter
     * delta is exactly 1 per send(). tx_reap clears the slot but
     * doesn't change the completion counter. */
    net_get_driver()->tx_reap();
    TEST_ASSERT_EQUAL_UINT32(tx_before + 1, cdc_ecm_get_tx_count());
}

static void test_send_busy_when_pool_full(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());

    /* The mock bulk-OUT completes synchronously, so ordinarily tx_reap
     * would free each slot. Turn the mock into a deferred-bulk-OUT
     * mode by draining slots right after send without calling tx_reap.
     * Simplest: submit pool+1 times and call tx_reap between each
     * to free each slot, EXCEPT omit the reap on the last one so the
     * pool is guaranteed full on the final submit. */
    const struct net_driver *drv = net_get_driver();
    uint8_t buf[64] = {0};

    /* Fill all 4 slots without reaping. Subsequent send should fail.
     * To do this, we abuse the fact that tx_reap is explicit: send
     * 4 times; without tx_reap, all 4 slots stay in_use. */
    for (int i = 0; i < 4; i++) {
        int rc = drv->send(buf, sizeof(buf));
        TEST_ASSERT_EQUAL_INT(0, rc);
    }
    int rc = drv->send(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(NET_E_BUSY, rc);

    /* Now reap; the 4 completed URBs get returned to the free pool. */
    drv->tx_reap();
    rc = drv->send(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, rc);
}

static void test_send_rejects_oversized(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    uint8_t big[3000] = {0};
    int rc = net_get_driver()->send(big, sizeof(big));
    TEST_ASSERT_EQUAL_INT(NET_E_TOO_LARGE, rc);
}

static void test_send_rejects_null(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    int rc = net_get_driver()->send(NULL, 10);
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, rc);
    uint8_t buf = 0;
    rc = net_get_driver()->send(&buf, 0);
    TEST_ASSERT_EQUAL_INT(NET_E_INVAL, rc);
}

static void test_recv_returns_frame_and_resubmits(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());
    int submits_before = mock.bulk_in_submits;
    uint32_t rx_before = cdc_ecm_get_rx_count();

    /* A 60-byte synthetic frame — minimum Ethernet runt. */
    uint8_t frame[60];
    for (unsigned i = 0; i < sizeof(frame); i++) frame[i] = (uint8_t)i;
    struct usb_urb *completed = mock_complete_pending_rx(frame, sizeof(frame));
    TEST_ASSERT_NOT_NULL(completed);

    uint8_t out[2048];
    int n = net_get_driver()->recv(out, sizeof(out));
    TEST_ASSERT_EQUAL_INT((int)sizeof(frame), n);
    TEST_ASSERT_EQUAL_MEMORY(frame, out, sizeof(frame));
    /* recv() must re-submit that slot's URB so the RX pool stays
     * full. Exactly one extra bulk-IN submit relative to "before". */
    TEST_ASSERT_EQUAL_INT(submits_before + 1, mock.bulk_in_submits);
    TEST_ASSERT_EQUAL_UINT32(rx_before + 1, cdc_ecm_get_rx_count());
}

static void test_recv_zero_when_nothing_ready(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());

    uint8_t out[2048];
    int n = net_get_driver()->recv(out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, n);
}

static void test_recv_handles_small_buffer(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());

    uint8_t frame[128];
    for (unsigned i = 0; i < sizeof(frame); i++) frame[i] = (uint8_t)i;
    TEST_ASSERT_NOT_NULL(mock_complete_pending_rx(frame, sizeof(frame)));

    /* Caller passes a 32-byte buffer; recv() must clamp to max_len
     * and not scribble past the end. Check with a canary after the
     * buffer. */
    uint8_t out[64];
    memset(out, 0xAA, sizeof(out));
    int n = net_get_driver()->recv(out, 32);
    TEST_ASSERT_EQUAL_INT(32, n);
    TEST_ASSERT_EQUAL_MEMORY(frame, out, 32);
    for (int i = 32; i < 64; i++)
        TEST_ASSERT_EQUAL_UINT8(0xAA, out[i]);
}

static void test_mac_fallback_when_imac_zero(void)
{
    /*
     * Fully enumerate, then edit the device's raw_config in place so
     * iMACAddress = 0 before probing. The probe must succeed and
     * synthesise a locally-administered MAC (U/L bit set in byte 0).
     * Relies on enumerate having already copied mock_config into
     * dev->raw_config.
     */
    reset_all();
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);
    /* Ethernet functional descriptor sits at offset 9 (CONFIG) + 9
     * (iface 0) + 5 (CS header) = 23. Its iMACAddress field is byte
     * 3 of the descriptor → overall offset 23 + 3 = 26. */
    dev->raw_config[26] = 0x00;

    int rc = cdc_ecm_probe_and_register();
    TEST_ASSERT_EQUAL_INT(0, rc);
    const uint8_t *mac = cdc_ecm_get_mac();
    TEST_ASSERT_NOT_NULL(mac);
    /* Fallback MAC: byte 0 has the U/L bit set (locally administered,
     * unicast). And it must NOT match the device's canned MAC. */
    TEST_ASSERT_EQUAL_UINT8(0x02, mac[0]);
    TEST_ASSERT_NOT_EQUAL(EXPECTED_MAC[1], mac[1]);
}

static void test_probe_rejects_non_cdc_device(void)
{
    /* Mutate the enumerated device so its interfaces claim a
     * non-CDC class. Probe must refuse AND clear any prior probe
     * state so link_status falls back to false. */
    reset_all();
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);

    /* First prime the module with a successful probe plus an explicit
     * NETWORK_CONNECTION notification so we can verify a subsequent
     * failure clears link_status. */
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());
    TEST_ASSERT_NOT_NULL(mock_complete_pending_notify(
        CDC_NOTIFY_NETWORK_CONNECTION, 1, NULL, 0));
    net_get_driver()->tx_reap();
    TEST_ASSERT_TRUE(net_get_driver()->link_status());

    for (unsigned i = 0; i < 4; i++) {
        dev->ifaces[i].class_code = 0xFF;
        dev->ifaces[i].subclass   = 0x00;
    }
    /* Force a re-probe of the mutated device — the idempotent guard
     * would otherwise skip. */
    cdc_ecm_reset();
    int rc = cdc_ecm_probe_and_register();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    /* Probe cleared the probed flag at entry; link must be down now
     * even though net_register_driver left the earlier registration
     * in place. */
    TEST_ASSERT_FALSE(net_get_driver()->link_status());
    TEST_ASSERT_NULL(cdc_ecm_get_mac());
}

static void test_probe_failure_clears_public_mac(void)
{
    /*
     * After a failed probe, cdc_ecm_get_mac() must return NULL even
     * if an earlier probe succeeded — the probed flag is the source
     * of truth. Guards the "probe resets state at entry" invariant.
     */
    reset_all();
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);
    for (unsigned i = 0; i < 4; i++) {
        dev->ifaces[i].class_code = 0xFF;
        dev->ifaces[i].subclass   = 0x00;
    }
    TEST_ASSERT_NOT_EQUAL(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_NULL(cdc_ecm_get_mac());
}

static void test_net_driver_ops_fail_when_unprobed(void)
{
    /*
     * Register the class driver via a successful probe, THEN break
     * the probe state by re-probing a non-CDC device. The net_driver
     * ops must now refuse every call with NET_E_NOT_INIT — the
     * prior registration doesn't paper over a dead device.
     */
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    const struct net_driver *drv = net_get_driver();
    TEST_ASSERT_NOT_NULL(drv);

    struct usb_device *dev = usb_core_first_device();
    for (unsigned i = 0; i < 4; i++) {
        dev->ifaces[i].class_code = 0xFF;
        dev->ifaces[i].subclass   = 0x00;
    }
    /* Explicit reset: the probe function is idempotent (Phase 4
     * needs that for the net_poll retry loop), so forcing a re-probe
     * of the now-non-CDC device requires clearing the bound flag. */
    cdc_ecm_reset();
    TEST_ASSERT_NOT_EQUAL(0, cdc_ecm_probe_and_register());

    uint8_t buf[64] = {0};
    TEST_ASSERT_EQUAL_INT(NET_E_NOT_INIT, drv->init());
    TEST_ASSERT_EQUAL_INT(NET_E_NOT_INIT, drv->send(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(NET_E_NOT_INIT, drv->recv(buf, sizeof(buf)));
}

static void test_poll_before_probe_is_noop(void)
{
    /*
     * cdc_ecm_poll when not probed must NOT dispatch to the HCD —
     * there's no device to drive. Guards the `if (cdc.probed)` gate.
     */
    reset_all();
    struct usb_device *dev = usb_core_first_device();
    for (unsigned i = 0; i < 4; i++) {
        dev->ifaces[i].class_code = 0xFF;
        dev->ifaces[i].subclass   = 0x00;
    }
    TEST_ASSERT_NOT_EQUAL(0, cdc_ecm_probe_and_register());

    int poll_before = mock.poll_count;
    cdc_ecm_poll();
    TEST_ASSERT_EQUAL_INT(poll_before, mock.poll_count);
}

static void test_poll_after_probe_dispatches_to_hcd(void)
{
    /*
     * After a successful probe, cdc_ecm_poll must dispatch to the
     * HCD's poll op exactly once per call. Phase-4 net_poll wires
     * this into the lwIP mainloop; the test confirms the
     * bookkeeping so that integration doesn't find a regression.
     */
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());

    int poll_before = mock.poll_count;
    cdc_ecm_poll();
    cdc_ecm_poll();
    TEST_ASSERT_EQUAL_INT(poll_before + 2, mock.poll_count);
}

static void test_default_mtu_when_mss_zero(void)
{
    /*
     * If the functional descriptor reports wMaxSegmentSize == 0, the
     * class driver falls back to CDC_ECM_DEFAULT_MTU (1514) rather
     * than honouring a zero. Edit raw_config in place to exercise
     * the fallback without standing up a second mock.
     */
    reset_all();
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);
    /* wMaxSegmentSize is at offset 31-32 within raw_config (see the
     * layout table in the mock blob). Zero both bytes. */
    dev->raw_config[31] = 0x00;
    dev->raw_config[32] = 0x00;

    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_UINT16(1514, cdc_ecm_get_max_segment());
}

static void test_probe_without_functional_descriptor(void)
{
    /*
     * Some dongles omit the Ethernet Networking functional descriptor
     * entirely. Probe must still succeed, synthesise a MAC, and use
     * the default MTU. Simulate this by overwriting the CS_INTERFACE
     * blocks with padding the parser skips (class-specific types the
     * parser does not recognise).
     */
    reset_all();
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);

    /* The CS_INTERFACE header sits at offset 18 (length 5) and the
     * Ethernet functional descriptor at offset 23 (length 13).
     * Turn the functional descriptor into a bland CS_INTERFACE with
     * an unknown subtype so find_cdc_ecm_functional returns NULL. */
    dev->raw_config[23 + 2] = 0xAA;   /* unknown subtype */

    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_UINT16(1514, cdc_ecm_get_max_segment());
    const uint8_t *mac = cdc_ecm_get_mac();
    TEST_ASSERT_NOT_NULL(mac);
    /* Fallback MAC byte 0 = 0x02 (locally administered, unicast). */
    TEST_ASSERT_EQUAL_UINT8(0x02, mac[0]);
}

static void test_rx_completion_error_drops_slot(void)
{
    /*
     * A bulk-IN URB that completes with a non-OK status (STALL,
     * IO_ERROR) must leave the slot un-ready and unconsumed so recv()
     * returns 0, not a frame. Guarantees the class driver never
     * forwards an errored payload to lwIP.
     */
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());

    /* rx_completions is module-level static; snapshot before driving
     * the error path so we can check the delta rather than an
     * absolute value that carries state from earlier tests. */
    uint32_t rx_before = cdc_ecm_get_rx_count();

    /* Find a pending bulk-IN URB and complete it with STALL. */
    bool completed_one = false;
    for (int i = 0; i < MOCK_MAX_INFLIGHT; i++) {
        struct usb_urb *urb = mock.in_flight[i];
        if (urb == NULL) continue;
        if (urb->transfer_type != USB_XFER_BULK) continue;
        if (!(urb->endpoint & USB_DIR_IN)) continue;
        urb->status = USB_URB_STALL;
        urb->actual_length = 0;
        mock.in_flight[i] = NULL;
        urb->complete(urb);
        completed_one = true;
        break;
    }
    TEST_ASSERT_TRUE(completed_one);

    /* Error completion still bumps the counter (driver saw it) but
     * leaves no payload for recv. */
    TEST_ASSERT_EQUAL_UINT32(rx_before + 1, cdc_ecm_get_rx_count());

    uint8_t out[2048];
    int n = net_get_driver()->recv(out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, n);
}

/* -------------------------------------------------------------------------- */
/* Phase 4 — lwIP integration via net_poll()                                    */
/*                                                                             */
/* Verifies that net_poll() drives the Phase-4 hookup: usb_core_hotplug_poll  */
/* picks up new enumerations, then cdc_ecm_probe_and_register binds the        */
/* class driver, leaving active_driver pointing at cdc_ecm_driver. Production */
/* path on Jetson is net_pump_task → net_poll → this chain.                    */
/* -------------------------------------------------------------------------- */

static void test_cdc_ecm_reset_forces_reprobe(void)
{
    /* Pin the test-only contract of cdc_ecm_reset(): after a
     * successful bind, calling reset clears the probed flag so a
     * subsequent cdc_ecm_probe_and_register re-runs the full probe
     * flow (MAC + endpoints + net_driver registration). Without this
     * hook, the idempotent guard in probe_and_register would skip
     * the re-probe and tests couldn't reset fixture state. */
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_NOT_NULL(cdc_ecm_get_mac());
    const struct net_driver *drv_first = net_get_driver();
    TEST_ASSERT_NOT_NULL(drv_first);

    cdc_ecm_reset();
    /* After reset: probed flag cleared, public getters treat us as
     * unbound. net_driver registration pointer stays in place (reset
     * doesn't unregister — it only clears the probed flag). */
    TEST_ASSERT_NULL(cdc_ecm_get_mac());

    /* Re-probe: the full flow runs again. Same mock device, same
     * expected MAC, same registration. */
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_NOT_NULL(cdc_ecm_get_mac());
    TEST_ASSERT_EQUAL_PTR(drv_first, net_get_driver());
}

static void test_net_poll_binds_cdc_ecm_on_enumeration(void)
{
    /* Start from unprobed state with a usb_core that's already seen
     * enumeration (the mock always enumerates on usb_core_start). The
     * first net_poll() tick must invoke cdc_ecm_probe_and_register and
     * end with active_driver populated. Other tests in this suite
     * leave active_driver set, so explicitly clear it first. */
    reset_all();
    net_register_driver(NULL);
    TEST_ASSERT_NULL(net_get_driver());

    net_poll();
    TEST_ASSERT_NOT_NULL(net_get_driver());
    TEST_ASSERT_NOT_NULL(cdc_ecm_get_mac());
}

static void test_net_poll_is_idempotent_after_bind(void)
{
    /* Once bound, further net_poll() ticks must NOT re-enter the
     * probe flow. Verify by snapshotting the driver pointer and
     * confirming many subsequent ticks keep the same value without
     * re-registering (a re-register would log the replacement warn
     * and reset cdc state). */
    reset_all();
    net_poll();
    const struct net_driver *drv_after_first = net_get_driver();
    TEST_ASSERT_NOT_NULL(drv_after_first);

    for (int i = 0; i < 50; i++)
        net_poll();

    TEST_ASSERT_EQUAL_PTR(drv_after_first, net_get_driver());
    /* MAC still matches — the probed state wasn't clobbered. */
    TEST_ASSERT_NOT_NULL(cdc_ecm_get_mac());
}

static void test_net_poll_no_hcd_is_safe(void)
{
    /* net_poll() must be safe to call before any HCD / device exists
     * — the net_pump_task spawns unconditionally on ENABLE_NETWORKING
     * builds and starts before net_init, so every platform hits this
     * cold-start path. No HCD → usb_core_hotplug_poll returns 0 →
     * cdc_ecm_probe_and_register returns 0 (no device) → net_poll's
     * subsequent driver checks bail on !net_initialized. */
    memset(&mock, 0, sizeof(mock));
    usb_core_register_hcd(NULL);
    usb_core_reset();
    cdc_ecm_reset();
    net_register_driver(NULL);

    /* Call several ticks — must not crash, must not register a driver. */
    for (int i = 0; i < 10; i++)
        net_poll();

    TEST_ASSERT_NULL(net_get_driver());
    TEST_ASSERT_NULL(cdc_ecm_get_mac());

    /* Restore a usable fixture state for any test added after this
     * one in the suite — without this, the NULL active_driver and
     * unregistered HCD would poison the next test's reset_all. */
    reset_all();
}

/* -------------------------------------------------------------------------- */
/* Suite entry point                                                           */
/* -------------------------------------------------------------------------- */

int test_suite_cdc_ecm(void);

int test_suite_cdc_ecm(void)
{
    UnityBegin("test_cdc_ecm.c");

    RUN_TEST(test_parse_mac_string_happy);
    RUN_TEST(test_parse_mac_string_rejects_short_buffer);
    RUN_TEST(test_parse_mac_string_rejects_wrong_type);
    RUN_TEST(test_parse_mac_string_rejects_non_ascii);
    RUN_TEST(test_parse_mac_string_rejects_non_hex);
    RUN_TEST(test_parse_mac_string_null_args);
    RUN_TEST(test_probe_binds_and_registers);
    RUN_TEST(test_probe_skips_when_no_device);
    RUN_TEST(test_net_init_queues_rx_urbs);
    RUN_TEST(test_net_init_queues_notification_urb);
    RUN_TEST(test_link_status_tracks_network_connection_notification);
    RUN_TEST(test_link_status_infers_up_from_speed_change);
    RUN_TEST(test_link_status_tracks_down_from_zero_speed_change);
    RUN_TEST(test_link_status_falls_back_after_silent_notification_timeout);
    RUN_TEST(test_link_status_falls_back_when_notify_wait_started_at_zero);
    RUN_TEST(test_link_status_falls_back_when_notification_endpoint_missing);
    RUN_TEST(test_send_goes_to_bulk_out);
    RUN_TEST(test_send_busy_when_pool_full);
    RUN_TEST(test_send_rejects_oversized);
    RUN_TEST(test_send_rejects_null);
    RUN_TEST(test_recv_returns_frame_and_resubmits);
    RUN_TEST(test_recv_zero_when_nothing_ready);
    RUN_TEST(test_recv_handles_small_buffer);
    RUN_TEST(test_mac_fallback_when_imac_zero);
    RUN_TEST(test_probe_rejects_non_cdc_device);
    RUN_TEST(test_probe_failure_clears_public_mac);
    RUN_TEST(test_net_driver_ops_fail_when_unprobed);
    RUN_TEST(test_poll_before_probe_is_noop);
    RUN_TEST(test_poll_after_probe_dispatches_to_hcd);
    RUN_TEST(test_default_mtu_when_mss_zero);
    RUN_TEST(test_probe_without_functional_descriptor);
    RUN_TEST(test_rx_completion_error_drops_slot);
    RUN_TEST(test_cdc_ecm_reset_forces_reprobe);
    RUN_TEST(test_net_poll_binds_cdc_ecm_on_enumeration);
    RUN_TEST(test_net_poll_is_idempotent_after_bind);
    RUN_TEST(test_net_poll_no_hcd_is_safe);

    return UnityEnd();
}
