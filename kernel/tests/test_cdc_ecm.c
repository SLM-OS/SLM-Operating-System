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
 *   9   INTERFACE 1 alt 1 — CDC data (2 bulk EPs; parser must SKIP alt 1)
 *   7   BULK IN  endpoint 0x82  (belongs to alt 1, not used by us)
 *   7   BULK OUT endpoint 0x02  (belongs to alt 1, not used by us)
 *
 * Plan §6 says Phase 1 only binds alt 0, so the primary data iface
 * ideally carries endpoints at alt 0. Real RTL8153 uses alt 0 for
 * the 2-EP data iface; we mirror that here to keep the probe
 * meaningful. Replace the "alt 0 empty + alt 1 real" layout above
 * with "alt 0 has the 2 bulk EPs" so our Phase-1 parser can bind
 * them.
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
    default:
        urb->status = USB_URB_IO_ERROR;
        if (urb->complete) urb->complete(urb);
        return 0;
    }
}

static void mock_poll(void) {}

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

/* -------------------------------------------------------------------------- */
/* Test fixtures                                                               */
/* -------------------------------------------------------------------------- */

static void reset_all(void)
{
    memset(&mock, 0, sizeof(mock));
    usb_core_register_hcd(&mock_hcd_ops);
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
    TEST_ASSERT_TRUE(drv->link_status());
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

static void test_send_goes_to_bulk_out(void)
{
    reset_all();
    TEST_ASSERT_EQUAL_INT(0, cdc_ecm_probe_and_register());
    TEST_ASSERT_EQUAL_INT(0, net_get_driver()->init());

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
    /* Mock completes TX synchronously — tx_reap must see the slot
     * and clear it so subsequent sends reuse it. */
    net_get_driver()->tx_reap();
    TEST_ASSERT_EQUAL_UINT32(1, cdc_ecm_get_tx_count());
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
    TEST_ASSERT_EQUAL_UINT32(1, cdc_ecm_get_rx_count());
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
     * non-CDC class. Probe must refuse. */
    reset_all();
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);
    for (unsigned i = 0; i < 4; i++) {
        dev->ifaces[i].class_code = 0xFF;
        dev->ifaces[i].subclass   = 0x00;
    }
    int rc = cdc_ecm_probe_and_register();
    TEST_ASSERT_NOT_EQUAL(0, rc);
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
    RUN_TEST(test_send_goes_to_bulk_out);
    RUN_TEST(test_send_busy_when_pool_full);
    RUN_TEST(test_send_rejects_oversized);
    RUN_TEST(test_send_rejects_null);
    RUN_TEST(test_recv_returns_frame_and_resubmits);
    RUN_TEST(test_recv_zero_when_nothing_ready);
    RUN_TEST(test_recv_handles_small_buffer);
    RUN_TEST(test_mac_fallback_when_imac_zero);
    RUN_TEST(test_probe_rejects_non_cdc_device);

    return UnityEnd();
}
