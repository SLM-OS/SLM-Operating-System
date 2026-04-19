/*
 * test_usb_core.c - Phase 1 USB core unit tests
 *
 * Exercises the URB framework, descriptor parsing, and enumeration
 * state machine against a mock HCD that replays a canned device —
 * one CDC control interface plus an Ethernet data interface with two
 * bulk endpoints. Runs on every platform the test harness builds for.
 */

#include "unity.h"
#include "usb.h"
#include "test_harness.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Mock device blob — one configuration, two interfaces, three endpoints.     */
/* Modelled after what a Realtek RTL8153 reports (CDC-ECM variant).           */
/* -------------------------------------------------------------------------- */

static const struct usb_device_descriptor mock_dev_desc = {
    .bLength            = 18,
    .bDescriptorType    = USB_DT_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x02,   /* CDC */
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
 * Raw config descriptor: 9 bytes config + 9 bytes iface0 (control) +
 * 7 bytes interrupt EP + 9 bytes iface1 alt0 (data, no EPs) + 9 bytes
 * iface1 alt1 (data, 2 EPs — deliberately skipped by the parser since
 * we only bind default settings) + 9 bytes iface1 alt0 data with 2
 * EPs + 7*2 bytes bulk EPs. The shape tests both the primary parse
 * path and the "skip alternate settings != 0" rule.
 */
#define MOCK_CONFIG_TOTAL_LENGTH 66
static const uint8_t mock_config[MOCK_CONFIG_TOTAL_LENGTH] = {
    /* CONFIGURATION (9 bytes) */
    0x09, USB_DT_CONFIG, MOCK_CONFIG_TOTAL_LENGTH, 0x00,
    0x02,                 /* bNumInterfaces */
    0x01,                 /* bConfigurationValue */
    0x00, 0xC0, 0x32,     /* iConfig, bmAttributes, bMaxPower */

    /* INTERFACE 0 — CDC control (bInterfaceClass=0x02, subclass=0x06 ECM) */
    0x09, USB_DT_INTERFACE, 0x00, 0x00, 0x01, 0x02, 0x06, 0x00, 0x00,

    /* INTERFACE 0, EP 0x81 — interrupt IN, 16 bytes, bInterval=8 */
    0x07, USB_DT_ENDPOINT, 0x81, USB_XFER_INTERRUPT, 0x10, 0x00, 0x08,

    /* INTERFACE 1 alt 0 — CDC data (no endpoints) */
    0x09, USB_DT_INTERFACE, 0x01, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,

    /* INTERFACE 1 alt 1 — CDC data (bulk IN + bulk OUT, 2 EPs)
     * The parser must SKIP this alt-setting per Phase-1 rules. */
    0x09, USB_DT_INTERFACE, 0x01, 0x01, 0x02, 0x0A, 0x00, 0x00, 0x00,

    /* These endpoints belong to alt 1 — must NOT appear in the parsed
     * endpoint table. */
    0x07, USB_DT_ENDPOINT, 0x88, USB_XFER_BULK, 0x00, 0x04, 0x00,
    0x07, USB_DT_ENDPOINT, 0x08, USB_XFER_BULK, 0x00, 0x04, 0x00,
};

/* -------------------------------------------------------------------------- */
/* Mock HCD state                                                              */
/* -------------------------------------------------------------------------- */

#define MOCK_DEFERRED_URB_SLOTS 4

struct mock_hcd_state {
    bool             started;
    bool             port_connected;
    enum usb_speed   port_speed;
    uint8_t          device_addr;
    uint8_t          device_configured;
    int              endpoints_configured;
    int              submit_count;
    int              cancel_count;
    int              poll_count;
    int              device_close_count;   /* # of times device_close fired */
    int              device_open_count;    /* # of times device_open fired */

    /* Error-injection hooks. Negative value = short-circuit that op. */
    int              port_reset_rc;
    int              device_open_rc;
    int              endpoint_configure_rc;
    /* How many control transfers to fail with IO_ERROR before succeeding. */
    int              fail_next_control;
    /* Fail SET_CONFIGURATION specifically (count). */
    int              fail_next_set_config;

    /* When true, submit_urb() leaves non-control URBs PENDING rather
     * than completing synchronously. Lets cancel tests exercise the
     * pending→cancelled transition. */
    bool             defer_non_control;
    /* Same, but for control URBs — exercises the usb_wait_urb
     * timeout branch. Decremented per deferred control URB. */
    int              defer_next_control;
    struct usb_urb  *deferred[MOCK_DEFERRED_URB_SLOTS];
};

static struct mock_hcd_state mock;

static int mock_start(void)
{
    mock.started = true;
    return 0;
}

static bool mock_port_status(uint8_t port, bool *connected, enum usb_speed *speed)
{
    (void)port;
    *connected = mock.port_connected;
    *speed     = mock.port_speed;
    return true;
}

static int mock_port_reset(uint8_t port)
{
    (void)port;
    return mock.port_reset_rc;
}

static int mock_device_open(struct usb_device *dev)
{
    (void)dev;
    mock.device_open_count++;
    return mock.device_open_rc;
}

static void mock_device_close(struct usb_device *dev)
{
    (void)dev;
    mock.device_close_count++;
}

static int mock_endpoint_configure(struct usb_device *dev,
                                   const struct usb_endpoint *ep)
{
    (void)dev; (void)ep;
    if (mock.endpoint_configure_rc)
        return mock.endpoint_configure_rc;
    mock.endpoints_configured++;
    return 0;
}

static void complete_control(struct usb_urb *urb,
                             const void *resp, uint32_t resp_len,
                             enum usb_urb_status status)
{
    if (resp && urb->buffer && urb->length > 0) {
        uint32_t n = (resp_len < urb->length) ? resp_len : urb->length;
        memcpy(urb->buffer, resp, n);
        urb->actual_length = n;
        if (n < urb->length && status == USB_URB_OK)
            urb->status = USB_URB_SHORT;
        else
            urb->status = status;
    } else {
        urb->actual_length = 0;
        urb->status = status;
    }
    if (urb->complete)
        urb->complete(urb);
}

/* Store a pending URB for later cancellation. */
static int mock_defer(struct usb_urb *urb)
{
    for (int i = 0; i < MOCK_DEFERRED_URB_SLOTS; i++) {
        if (mock.deferred[i] == NULL) {
            mock.deferred[i] = urb;
            return 0;
        }
    }
    return -1;
}

static int mock_submit_urb(struct usb_urb *urb)
{
    mock.submit_count++;

    if (urb->transfer_type != USB_XFER_CONTROL) {
        if (mock.defer_non_control) {
            if (mock_defer(urb) != 0) {
                urb->status = USB_URB_IO_ERROR;
                if (urb->complete) urb->complete(urb);
            }
            /* status stays PENDING */
            return 0;
        }
        urb->status = USB_URB_OK;
        urb->actual_length = urb->length;
        if (urb->complete) urb->complete(urb);
        return 0;
    }

    if (mock.defer_next_control > 0) {
        mock.defer_next_control--;
        if (mock_defer(urb) != 0) {
            urb->status = USB_URB_IO_ERROR;
            if (urb->complete) urb->complete(urb);
        }
        /* Leave status PENDING so usb_wait_urb has to time out. */
        return 0;
    }

    if (mock.fail_next_control > 0) {
        mock.fail_next_control--;
        complete_control(urb, NULL, 0, USB_URB_IO_ERROR);
        return 0;
    }

    uint8_t req = urb->setup.bRequest;
    uint8_t desc_type  = (uint8_t)(urb->setup.wValue >> 8);
    uint8_t desc_index = (uint8_t)(urb->setup.wValue & 0xFF);

    switch (req) {
    case USB_REQ_GET_DESCRIPTOR:
        if (desc_type == USB_DT_DEVICE && desc_index == 0) {
            complete_control(urb, &mock_dev_desc, sizeof(mock_dev_desc), USB_URB_OK);
        } else if (desc_type == USB_DT_CONFIG && desc_index == 0) {
            complete_control(urb, mock_config, sizeof(mock_config), USB_URB_OK);
        } else {
            complete_control(urb, NULL, 0, USB_URB_IO_ERROR);
        }
        break;

    case USB_REQ_SET_ADDRESS:
        mock.device_addr = (uint8_t)urb->setup.wValue;
        complete_control(urb, NULL, 0, USB_URB_OK);
        break;

    case USB_REQ_SET_CONFIGURATION:
        if (mock.fail_next_set_config > 0) {
            mock.fail_next_set_config--;
            complete_control(urb, NULL, 0, USB_URB_STALL);
        } else {
            mock.device_configured = (uint8_t)urb->setup.wValue;
            complete_control(urb, NULL, 0, USB_URB_OK);
        }
        break;

    default:
        complete_control(urb, NULL, 0, USB_URB_IO_ERROR);
        break;
    }
    return 0;
}

static int mock_cancel_urb(struct usb_urb *urb)
{
    mock.cancel_count++;
    for (int i = 0; i < MOCK_DEFERRED_URB_SLOTS; i++) {
        if (mock.deferred[i] == urb) {
            mock.deferred[i] = NULL;
            urb->status = USB_URB_CANCELLED;
            if (urb->complete) urb->complete(urb);
            return 0;
        }
    }
    /* Not found — already completed, reflect that. */
    return 0;
}

static void mock_poll(void)
{
    mock.poll_count++;
}

static const struct usb_hcd mock_hcd_ops = {
    .name               = "mock-hcd",
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

/* -------------------------------------------------------------------------- */
/* Test fixtures                                                               */
/* -------------------------------------------------------------------------- */

static void reset_mock_and_core(void)
{
    memset(&mock, 0, sizeof(mock));
    mock.port_connected = true;
    mock.port_speed     = USB_SPEED_HIGH;
    usb_core_register_hcd(&mock_hcd_ops);
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                       */
/* -------------------------------------------------------------------------- */

static void test_urb_status_strings(void)
{
    TEST_ASSERT_EQUAL_STRING("OK",        usb_urb_status_str(USB_URB_OK));
    TEST_ASSERT_EQUAL_STRING("PENDING",   usb_urb_status_str(USB_URB_PENDING));
    TEST_ASSERT_EQUAL_STRING("CANCELLED", usb_urb_status_str(USB_URB_CANCELLED));
    TEST_ASSERT_EQUAL_STRING("STALL",     usb_urb_status_str(USB_URB_STALL));
    TEST_ASSERT_EQUAL_STRING("TIMEOUT",   usb_urb_status_str(USB_URB_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("SHORT",     usb_urb_status_str(USB_URB_SHORT));
    TEST_ASSERT_EQUAL_STRING("IO_ERROR",  usb_urb_status_str(USB_URB_IO_ERROR));
    /* Out-of-range falls through to "UNKNOWN". */
    TEST_ASSERT_EQUAL_STRING("UNKNOWN",
                             usb_urb_status_str((enum usb_urb_status)99));
}

static void test_hcd_registration(void)
{
    reset_mock_and_core();
    TEST_ASSERT_EQUAL_PTR(&mock_hcd_ops, usb_core_get_hcd());
}

static void test_start_and_enumerate(void)
{
    reset_mock_and_core();
    int rc = usb_core_start();
    TEST_ASSERT_EQUAL_INT(0, rc);

    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_TRUE(mock.started);
    TEST_ASSERT_EQUAL_UINT8(1, mock.device_addr);
    TEST_ASSERT_EQUAL_UINT8(1, dev->address);
    TEST_ASSERT_EQUAL_UINT8(1, mock.device_configured);
    TEST_ASSERT_EQUAL_INT(USB_STATE_CONFIGURED, dev->state);
    TEST_ASSERT_EQUAL_UINT16(0x0BDA, dev->dev_desc.idVendor);
    TEST_ASSERT_EQUAL_UINT16(0x8153, dev->dev_desc.idProduct);
    /* Two alt-0 interfaces; iface0 has 1 EP (interrupt IN); iface1
     * alt-0 has 0 EPs — the EPs hanging off alt 1 must be skipped. */
    TEST_ASSERT_EQUAL_INT(1, mock.endpoints_configured);
}

static void test_enumerate_no_device(void)
{
    reset_mock_and_core();
    mock.port_connected = false;

    int rc = usb_core_start();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
}

static void test_enumerate_speed_low_propagates(void)
{
    reset_mock_and_core();
    mock.port_speed = USB_SPEED_LOW;
    TEST_ASSERT_EQUAL_INT(0, usb_core_start());
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_LOW, dev->speed);
}

static void test_enumerate_port_reset_failure(void)
{
    reset_mock_and_core();
    mock.port_reset_rc = -1;
    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_UINT8(0, mock.device_addr);
    TEST_ASSERT_EQUAL_INT(0, mock.endpoints_configured);
}

static void test_enumerate_device_open_failure(void)
{
    reset_mock_and_core();
    mock.device_open_rc = -1;
    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_UINT8(0, mock.device_addr);
}

static void test_enumerate_set_config_failure(void)
{
    /*
     * All pre-config steps succeed (device descriptor, SET_ADDRESS,
     * config tree, parse). Only SET_CONFIGURATION itself fails. The
     * device must not be left in CONFIGURED state and must not be
     * exposed via usb_core_first_device().
     */
    reset_mock_and_core();
    mock.fail_next_set_config = 1;

    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_UINT8(1, mock.device_addr);         /* got this far */
    TEST_ASSERT_EQUAL_UINT8(0, mock.device_configured);   /* but not past */
    TEST_ASSERT_EQUAL_INT(0, mock.endpoints_configured);
}

static void test_enumerate_endpoint_configure_failure(void)
{
    reset_mock_and_core();
    mock.endpoint_configure_rc = -1;
    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
    /* SET_CONFIGURATION succeeded before endpoint_configure ran. */
    TEST_ASSERT_EQUAL_UINT8(1, mock.device_configured);
}

static void test_enumerate_control_failure(void)
{
    /*
     * Fail the first 8-byte GET_DESCRIPTOR so enumeration bails early.
     * Device must NOT be marked present and must NOT have been
     * addressed.
     */
    reset_mock_and_core();
    mock.fail_next_control = 1;

    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_UINT8(0, mock.device_addr);
}

static void test_enumerate_resets_previous_device(void)
{
    /*
     * Enumerate succeeds once, then a subsequent enumerate failure
     * must clear the cached device — otherwise a reboot-recovery
     * flow would keep reporting the stale device.
     */
    reset_mock_and_core();
    TEST_ASSERT_EQUAL_INT(0, usb_core_start());
    TEST_ASSERT_NOT_NULL(usb_core_first_device());

    /* Now fail the second enumeration. */
    mock.fail_next_control = 1;
    int rc = usb_core_enumerate();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
}

static void test_descriptor_parse(void)
{
    reset_mock_and_core();
    TEST_ASSERT_EQUAL_INT(0, usb_core_start());
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);

    /* Interface 0: CDC control, alt 0, 1 endpoint. */
    TEST_ASSERT_TRUE(dev->ifaces[0].valid);
    TEST_ASSERT_EQUAL_UINT8(0, dev->ifaces[0].number);
    TEST_ASSERT_EQUAL_UINT8(0, dev->ifaces[0].alt_setting);
    TEST_ASSERT_EQUAL_UINT8(0x02, dev->ifaces[0].class_code);
    TEST_ASSERT_EQUAL_UINT8(0x06, dev->ifaces[0].subclass);
    TEST_ASSERT_EQUAL_UINT8(1, dev->ifaces[0].num_endpoints);

    /* Interface 1 alt 0: CDC data, 0 endpoints (alt 1 skipped). */
    TEST_ASSERT_TRUE(dev->ifaces[1].valid);
    TEST_ASSERT_EQUAL_UINT8(1, dev->ifaces[1].number);
    TEST_ASSERT_EQUAL_UINT8(0, dev->ifaces[1].alt_setting);
    TEST_ASSERT_EQUAL_UINT8(0x0A, dev->ifaces[1].class_code);

    /* Only iface 0's interrupt IN endpoint should be present. */
    const struct usb_endpoint *intr_in =
        usb_find_endpoint(dev, 0, USB_DIR_IN, USB_XFER_INTERRUPT);
    TEST_ASSERT_NOT_NULL(intr_in);
    TEST_ASSERT_EQUAL_UINT8(0x81, intr_in->address);
    TEST_ASSERT_EQUAL_UINT16(16, intr_in->max_packet);
    TEST_ASSERT_EQUAL_UINT8(8, intr_in->interval);
}

static void test_find_endpoint_direction_filter(void)
{
    /*
     * Drive descriptor parse directly with a custom blob that has
     * both an IN and OUT bulk endpoint on the same interface, so the
     * direction filter is exercised unambiguously.
     */
    static const uint8_t blob[] = {
        0x09, USB_DT_CONFIG, 0x23, 0x00, 0x01, 0x01, 0x00, 0xC0, 0x32,
        0x09, USB_DT_INTERFACE, 0x03, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
        0x07, USB_DT_ENDPOINT, 0x83, USB_XFER_BULK, 0x00, 0x02, 0x00,
        0x07, USB_DT_ENDPOINT, 0x03, USB_XFER_BULK, 0x00, 0x02, 0x00,
    };
    struct usb_device dev = {0};
    memcpy(dev.raw_config, blob, sizeof(blob));
    dev.raw_config_len = sizeof(blob);
    TEST_ASSERT_EQUAL_INT(0, usb_parse_configuration(&dev));

    const struct usb_endpoint *in  = usb_find_endpoint(&dev, 3, USB_DIR_IN,  USB_XFER_BULK);
    const struct usb_endpoint *out = usb_find_endpoint(&dev, 3, USB_DIR_OUT, USB_XFER_BULK);
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_EQUAL(in->address, out->address);
    TEST_ASSERT_EQUAL_UINT8(0x83, in->address);
    TEST_ASSERT_EQUAL_UINT8(0x03, out->address);
}

static void test_find_endpoint_mismatch(void)
{
    reset_mock_and_core();
    TEST_ASSERT_EQUAL_INT(0, usb_core_start());
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);

    /* Iface 0 has no bulk endpoints — only interrupt IN. */
    TEST_ASSERT_NULL(usb_find_endpoint(dev, 0, USB_DIR_IN, USB_XFER_BULK));
    /* Iface 1 (alt 0) has no endpoints at all. */
    TEST_ASSERT_NULL(usb_find_endpoint(dev, 1, USB_DIR_IN, USB_XFER_BULK));
    /* Unknown iface number. */
    TEST_ASSERT_NULL(usb_find_endpoint(dev, 9, USB_DIR_IN, USB_XFER_BULK));
    /* NULL device — must not crash. */
    TEST_ASSERT_NULL(usb_find_endpoint(NULL, 0, USB_DIR_IN, USB_XFER_INTERRUPT));
}

static void test_descriptor_parse_invalid_header(void)
{
    struct usb_device dev = {0};
    /* Not a CONFIGURATION descriptor — parse must reject. */
    dev.raw_config[0] = 0x09;
    dev.raw_config[1] = USB_DT_INTERFACE; /* wrong type */
    dev.raw_config_len = 9;
    TEST_ASSERT_NOT_EQUAL(0, usb_parse_configuration(&dev));
}

static void test_descriptor_parse_too_short(void)
{
    struct usb_device dev = {0};
    dev.raw_config_len = 3;  /* shorter than config descriptor */
    TEST_ASSERT_NOT_EQUAL(0, usb_parse_configuration(&dev));
}

static void test_descriptor_parse_interface_overflow(void)
{
    /*
     * USB_MAX_INTERFACES_PER_DEV interfaces fit; the one after that
     * is dropped with a warning. Endpoints that belong to the dropped
     * interface must NOT smear onto the last accepted interface —
     * cur_iface_slot is reset to -1 so they fall through.
     */
    uint8_t blob[USB_MAX_CONFIG_DESC_BYTES];
    unsigned off = 0;
    blob[off++] = 0x09;
    blob[off++] = USB_DT_CONFIG;
    blob[off++] = 0x00;  /* wTotalLength low, filled in below */
    blob[off++] = 0x00;
    blob[off++] = USB_MAX_INTERFACES_PER_DEV + 1;
    blob[off++] = 0x01;
    blob[off++] = 0x00;
    blob[off++] = 0xC0;
    blob[off++] = 0x32;
    for (unsigned i = 0; i < USB_MAX_INTERFACES_PER_DEV + 1; i++) {
        blob[off++] = 0x09;
        blob[off++] = USB_DT_INTERFACE;
        blob[off++] = (uint8_t)i;   /* bInterfaceNumber */
        blob[off++] = 0x00;         /* alt 0 */
        blob[off++] = 0x00;         /* 0 endpoints */
        blob[off++] = 0x0A;
        blob[off++] = 0x00;
        blob[off++] = 0x00;
        blob[off++] = 0x00;
    }
    /* Append one EP that would hang off the overflowed iface — must be skipped. */
    blob[off++] = 0x07;
    blob[off++] = USB_DT_ENDPOINT;
    blob[off++] = 0x88;
    blob[off++] = USB_XFER_BULK;
    blob[off++] = 0x00;
    blob[off++] = 0x02;
    blob[off++] = 0x00;
    blob[2] = (uint8_t)(off & 0xFF);
    blob[3] = (uint8_t)(off >> 8);

    struct usb_device dev = {0};
    memcpy(dev.raw_config, blob, off);
    dev.raw_config_len = (uint16_t)off;
    TEST_ASSERT_EQUAL_INT(0, usb_parse_configuration(&dev));

    /* All USB_MAX_INTERFACES_PER_DEV slots filled. */
    unsigned accepted = 0;
    for (unsigned i = 0; i < USB_MAX_INTERFACES_PER_DEV; i++) {
        if (dev.ifaces[i].valid) accepted++;
    }
    TEST_ASSERT_EQUAL_UINT(USB_MAX_INTERFACES_PER_DEV, accepted);

    /* Overflowed iface's endpoint must not have been attached anywhere. */
    for (unsigned i = 0; i < USB_MAX_INTERFACES_PER_DEV; i++) {
        TEST_ASSERT_NULL(usb_find_endpoint(&dev,
                                           dev.ifaces[i].number,
                                           USB_DIR_IN,
                                           USB_XFER_BULK));
    }
}

static void test_descriptor_parse_endpoint_overflow(void)
{
    /*
     * Pile USB_MAX_ENDPOINTS_PER_DEV + 2 endpoints onto a single
     * interface. First USB_MAX_ENDPOINTS_PER_DEV are accepted; the
     * rest are dropped with a warning. The interface's num_endpoints
     * field still reports the original claim (parse copies from the
     * descriptor), but ep_index[] saturates.
     */
    uint8_t blob[USB_MAX_CONFIG_DESC_BYTES];
    unsigned off = 0;
    /* CONFIGURATION */
    blob[off++] = 0x09; blob[off++] = USB_DT_CONFIG;
    blob[off++] = 0x00; blob[off++] = 0x00;
    blob[off++] = 1;    blob[off++] = 1;
    blob[off++] = 0;    blob[off++] = 0xC0; blob[off++] = 0x32;
    /* INTERFACE */
    const uint8_t total_eps = USB_MAX_ENDPOINTS_PER_DEV + 2;
    blob[off++] = 0x09; blob[off++] = USB_DT_INTERFACE;
    blob[off++] = 7;    blob[off++] = 0;
    blob[off++] = total_eps;
    blob[off++] = 0x0A; blob[off++] = 0; blob[off++] = 0; blob[off++] = 0;
    /* Endpoints — alternate IN/OUT bulk. */
    for (uint8_t e = 0; e < total_eps; e++) {
        blob[off++] = 0x07;
        blob[off++] = USB_DT_ENDPOINT;
        blob[off++] = (uint8_t)((e & 1 ? USB_DIR_IN : 0) | (e + 1));
        blob[off++] = USB_XFER_BULK;
        blob[off++] = 0x00; blob[off++] = 0x02; blob[off++] = 0x00;
    }
    blob[2] = (uint8_t)(off & 0xFF);
    blob[3] = (uint8_t)(off >> 8);

    struct usb_device dev = {0};
    memcpy(dev.raw_config, blob, off);
    dev.raw_config_len = (uint16_t)off;
    TEST_ASSERT_EQUAL_INT(0, usb_parse_configuration(&dev));

    unsigned valid = 0;
    for (unsigned e = 0; e < USB_MAX_ENDPOINTS_PER_DEV; e++) {
        if (dev.endpoints[e].valid) valid++;
    }
    TEST_ASSERT_EQUAL_UINT(USB_MAX_ENDPOINTS_PER_DEV, valid);

    /* The interface's ep_index[] should have entries for the first
     * USB_MAX_ENDPOINTS_PER_DEV endpoints only (all slots filled). */
    unsigned attached = 0;
    for (unsigned e = 0; e < USB_MAX_ENDPOINTS_PER_DEV; e++) {
        if (dev.ifaces[0].ep_index[e] >= 0) attached++;
    }
    TEST_ASSERT_EQUAL_UINT(USB_MAX_ENDPOINTS_PER_DEV, attached);
}

static void test_descriptor_parse_orphan_endpoint(void)
{
    /* Endpoint descriptor before any interface — must be skipped, not crash. */
    static const uint8_t blob[] = {
        0x09, USB_DT_CONFIG, 0x16, 0x00, 0x01, 0x01, 0x00, 0xC0, 0x32,
        0x07, USB_DT_ENDPOINT, 0x84, USB_XFER_BULK, 0x00, 0x02, 0x00,
        /* Interface that should be parsed normally after the orphan. */
        0x09, USB_DT_INTERFACE, 0x05, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
    };
    struct usb_device dev = {0};
    memcpy(dev.raw_config, blob, sizeof(blob));
    dev.raw_config_len = sizeof(blob);
    TEST_ASSERT_EQUAL_INT(0, usb_parse_configuration(&dev));
    TEST_ASSERT_TRUE(dev.ifaces[0].valid);
    TEST_ASSERT_EQUAL_UINT8(5, dev.ifaces[0].number);
    /* Endpoint must NOT have been attached to iface 5. */
    TEST_ASSERT_NULL(usb_find_endpoint(&dev, 5, USB_DIR_IN, USB_XFER_BULK));
}

static void test_cancel_pending_urb(void)
{
    reset_mock_and_core();
    TEST_ASSERT_EQUAL_INT(0, usb_core_start());
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);

    /* Switch the mock to the deferred path so a bulk URB stays pending. */
    mock.defer_non_control = true;

    /* Real stack buffer — defensive against a future mock change that
     * might actually dereference urb->buffer. */
    uint8_t scratch[128] = {0};
    struct usb_urb urb = {0};
    urb.dev           = dev;
    urb.endpoint      = 0x82;
    urb.transfer_type = USB_XFER_BULK;
    urb.buffer        = scratch;
    urb.length        = (uint32_t)sizeof(scratch);

    int sub = usb_submit_urb(&urb);
    TEST_ASSERT_EQUAL_INT(0, sub);
    TEST_ASSERT_EQUAL_INT(USB_URB_PENDING, urb.status);

    int rc = usb_cancel_urb(&urb);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, mock.cancel_count);
    TEST_ASSERT_EQUAL_INT(USB_URB_CANCELLED, urb.status);
}

static void test_cancel_with_no_hcd(void)
{
    /*
     * Error contract: negative return so `if (rc < 0)` catches it
     * uniformly with HCD-reported errors. Status-enum values are
     * never returned as positive.
     */
    usb_core_register_hcd(NULL);
    struct usb_device dev = {0};
    struct usb_urb urb = {0};
    urb.dev = &dev;
    int rc = usb_cancel_urb(&urb);
    TEST_ASSERT_EQUAL_INT(-USB_URB_IO_ERROR, rc);
    rc = usb_cancel_urb(NULL);
    TEST_ASSERT_EQUAL_INT(-USB_URB_IO_ERROR, rc);
}

static void test_submit_urb_no_hcd(void)
{
    /* Clear active HCD by registering NULL. */
    usb_core_register_hcd(NULL);

    struct usb_device dev = {0};
    struct usb_urb urb = {0};
    urb.dev = &dev;
    int rc = usb_submit_urb(&urb);
    TEST_ASSERT_EQUAL_INT(-USB_URB_IO_ERROR, rc);
    /* urb->status is still the pre-submit sentinel (IO_ERROR the core
     * wrote before returning). This is informational — callers key on
     * the return value, not on the pre-completion status. */
    TEST_ASSERT_EQUAL_INT(USB_URB_IO_ERROR, urb.status);
}

static void test_submit_urb_null_args(void)
{
    reset_mock_and_core();
    TEST_ASSERT_EQUAL_INT(-USB_URB_IO_ERROR, usb_submit_urb(NULL));

    struct usb_urb urb = {0};  /* urb.dev == NULL */
    TEST_ASSERT_EQUAL_INT(-USB_URB_IO_ERROR, usb_submit_urb(&urb));
}

/*
 * A deliberately-misbehaving HCD whose submit_urb returns a positive
 * usb_urb_status enum value (contract violation — HCDs must return 0
 * or a negative errno). Used by test_submit_urb_hcd_positive_normalized
 * to confirm usb_submit_urb coerces the garbage to -USB_URB_IO_ERROR
 * so downstream callers' "if (rc < 0)" checks still work.
 */
static int positive_return_submit_urb(struct usb_urb *urb)
{
    (void)urb;
    return (int)USB_URB_IO_ERROR;   /* 6 — positive, violates contract */
}

static const struct usb_hcd positive_return_hcd = {
    .name       = "positive-return-hcd",
    .submit_urb = positive_return_submit_urb,
};

static void test_submit_urb_hcd_positive_normalized(void)
{
    usb_core_register_hcd(&positive_return_hcd);
    struct usb_device dev = {0};
    struct usb_urb urb = {0};
    urb.dev = &dev;
    int rc = usb_submit_urb(&urb);
    TEST_ASSERT_TRUE(rc < 0);
    TEST_ASSERT_EQUAL_INT(-USB_URB_IO_ERROR, rc);
    /* Regression guard: a caller who polls urb->status instead of the
     * return value must not be left observing PENDING on a transfer
     * the HCD already rejected. */
    TEST_ASSERT_EQUAL_INT(USB_URB_IO_ERROR, urb.status);
}

static void test_poll_no_hcd_is_safe(void)
{
    /* usb_core_poll with no HCD must be a no-op rather than crashing. */
    usb_core_register_hcd(NULL);
    usb_core_poll();  /* must not crash */
    TEST_PASS();
}

static void test_poll_calls_hcd(void)
{
    reset_mock_and_core();
    usb_core_poll();
    usb_core_poll();
    TEST_ASSERT_EQUAL_INT(2, mock.poll_count);
}

static void test_control_msg_timeout(void)
{
    /*
     * Force a control URB to stay pending forever; usb_wait_urb must
     * eventually cancel and report TIMEOUT. Covers the one usb_urb_status
     * value that no other test produces, and the cancel-on-timeout path.
     */
    reset_mock_and_core();
    TEST_ASSERT_EQUAL_INT(0, usb_core_start());
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);

    /* Defer the very next control transfer so it stays pending. */
    mock.defer_next_control = 1;

    /* Use a tiny timeout so the test finishes in a few poll rounds. */
    int n = usb_control_msg(dev,
                            USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                            USB_REQ_GET_DESCRIPTOR,
                            (uint16_t)(USB_DT_DEVICE << 8), 0,
                            NULL, 0, 1 /* ms */);
    TEST_ASSERT_EQUAL_INT(-USB_URB_TIMEOUT, n);
    /* usb_wait_urb must have called cancel once. */
    TEST_ASSERT_TRUE(mock.cancel_count > 0);
}

static void test_enumerate_closes_device_on_control_failure(void)
{
    /*
     * Once device_open has run, any later failure must fall through to
     * the cleanup path so the HCD doesn't leak the opened slot.
     */
    reset_mock_and_core();
    mock.fail_next_control = 1;   /* fail the 8-byte GET_DESCRIPTOR */

    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
    /* device_close must have fired exactly once. */
    TEST_ASSERT_EQUAL_INT(1, mock.device_close_count);
}

static void test_enumerate_closes_device_on_set_config_failure(void)
{
    /* Same guarantee, deeper in the enumeration pipeline. */
    reset_mock_and_core();
    mock.fail_next_set_config = 1;

    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_INT(1, mock.device_close_count);
}

static void test_enumerate_no_close_on_port_reset_failure(void)
{
    /*
     * device_close must NOT fire when device_open hasn't run yet —
     * port_reset failure is the one path that bails before opening.
     */
    reset_mock_and_core();
    mock.port_reset_rc = -1;

    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL_INT(0, mock.device_close_count);
}

static void test_enumerate_no_close_on_device_open_failure(void)
{
    /*
     * If device_open itself failed, the HCD is expected to own the
     * cleanup of any partial state it created. The core must NOT call
     * device_close on a slot that was never successfully opened.
     */
    reset_mock_and_core();
    mock.device_open_rc = -1;

    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL_INT(0, mock.device_close_count);
}

static void test_descriptor_parse_oversized_rejected(void)
{
    /*
     * Defensive cap: raw_config_len > sizeof(raw_config) means the
     * walker would read past the buffer into adjacent struct fields.
     * Parser must refuse this up front.
     */
    struct usb_device dev = {0};
    /* Well-formed config header; the lie is the length. */
    dev.raw_config[0] = 0x09;
    dev.raw_config[1] = USB_DT_CONFIG;
    dev.raw_config[2] = 0x09;
    dev.raw_config[3] = 0x00;
    dev.raw_config_len = sizeof(dev.raw_config) + 1;
    TEST_ASSERT_NOT_EQUAL(0, usb_parse_configuration(&dev));
}

static void test_descriptor_parse_exact_cap_accepted(void)
{
    /*
     * Mirror of the oversized test: at exactly sizeof(raw_config) the
     * parser must still accept. Guards the off-by-one direction of
     * the defensive cap.
     */
    struct usb_device dev = {0};
    dev.raw_config[0] = 0x09;
    dev.raw_config[1] = USB_DT_CONFIG;
    dev.raw_config[2] = 0x09;
    dev.raw_config[3] = 0x00;
    dev.raw_config[4] = 0x00;   /* 0 interfaces — nothing to walk */
    dev.raw_config[5] = 0x01;
    dev.raw_config[6] = 0x00;
    dev.raw_config[7] = 0xC0;
    dev.raw_config[8] = 0x32;
    dev.raw_config_len = sizeof(dev.raw_config);
    TEST_ASSERT_EQUAL_INT(0, usb_parse_configuration(&dev));
}

static void test_hcd_reregister_same_is_silent(void)
{
    /*
     * The test harness re-registers the mock HCD on every fixture
     * reset; the core must treat same-pointer re-registration as a
     * no-op rather than logging a warning on every test. This test
     * doesn't assert log output (the harness doesn't capture it), but
     * exercises the code path so any future regression that adds
     * observable side-effects will show up in adjacent state (submit
     * counts, etc.).
     */
    reset_mock_and_core();
    const struct usb_hcd *first = usb_core_get_hcd();
    TEST_ASSERT_NOT_NULL(first);
    /* Re-register the same pointer — must be a no-op. */
    usb_core_register_hcd(first);
    TEST_ASSERT_EQUAL_PTR(first, usb_core_get_hcd());
    /* Clear via NULL — also silent per the new contract. */
    usb_core_register_hcd(NULL);
    TEST_ASSERT_NULL(usb_core_get_hcd());
}

static void test_descriptor_parse_bad_config_blength(void)
{
    /*
     * A device that returns a 7-byte "config" header is malformed. The
     * parser must reject rather than advancing p into whatever garbage
     * follows.
     */
    struct usb_device dev = {0};
    dev.raw_config[0] = 0x07;   /* bLength — wrong */
    dev.raw_config[1] = USB_DT_CONFIG;
    dev.raw_config[2] = 0x09;   /* wTotalLength */
    dev.raw_config[3] = 0x00;
    dev.raw_config_len = 9;
    TEST_ASSERT_NOT_EQUAL(0, usb_parse_configuration(&dev));
}

static void test_control_msg_returns_actual_length(void)
{
    /*
     * After enumeration the mock is happy to serve GET_DESCRIPTOR
     * with a real payload; the helper must return the count, not 0.
     */
    reset_mock_and_core();
    TEST_ASSERT_EQUAL_INT(0, usb_core_start());
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);

    uint8_t buf[18] = {0};
    int n = usb_get_descriptor(dev, USB_DT_DEVICE, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(18, n);
    /* First byte of a DEVICE descriptor is bLength. */
    TEST_ASSERT_EQUAL_UINT8(18, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(USB_DT_DEVICE, buf[1]);
}

static void test_control_msg_unknown_request_returns_error(void)
{
    reset_mock_and_core();
    TEST_ASSERT_EQUAL_INT(0, usb_core_start());
    struct usb_device *dev = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(dev);

    int n = usb_control_msg(dev,
                            USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                            0xFE /* undefined */, 0, 0, NULL, 0, 500);
    TEST_ASSERT_LESS_THAN(0, n);
}

/* -------------------------------------------------------------------------- */
/* Hot-plug poll (#309 re-plug driver entry point)                             */
/*                                                                             */
/* usb_core keeps a `root_device_present` flag that persists across tests —    */
/* clear it via the public usb_core_reset() hook before each case so test      */
/* ordering can't pollute state.                                               */
/* -------------------------------------------------------------------------- */

static void reset_mock_and_clear_device_state(bool port_connected,
                                              enum usb_speed speed)
{
    memset(&mock, 0, sizeof(mock));
    mock.port_connected = port_connected;
    mock.port_speed     = speed;
    usb_core_register_hcd(&mock_hcd_ops);
    usb_core_reset();
    TEST_ASSERT_NULL(usb_core_first_device());
}

static void test_core_reset_clears_device_without_touching_hcd(void)
{
    /* After a successful enumeration, usb_core_reset() must:
     *   - fire hcd->device_close exactly once so HCD-side per-device
     *     state is released (xHCI slot, NC contexts on the real path)
     *   - wipe the device slot (first_device returns NULL)
     *   - leave the registered HCD bound so subsequent tests don't
     *     have to re-register it */
    reset_mock_and_clear_device_state(true, USB_SPEED_HIGH);
    TEST_ASSERT_EQUAL_INT(0, usb_core_start());
    TEST_ASSERT_NOT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_PTR(&mock_hcd_ops, usb_core_get_hcd());
    int close_count_before = mock.device_close_count;

    usb_core_reset();
    TEST_ASSERT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_PTR(&mock_hcd_ops, usb_core_get_hcd());
    TEST_ASSERT_EQUAL_INT(close_count_before + 1, mock.device_close_count);
}

static void test_core_reset_no_device_is_safe(void)
{
    /* With no device enumerated, usb_core_reset must NOT call
     * device_close (there's nothing to close). Regression guard for
     * the `root_device_present` branch in usb_core_reset. */
    reset_mock_and_clear_device_state(false, USB_SPEED_UNKNOWN);
    TEST_ASSERT_EQUAL_INT(0, mock.device_close_count);

    usb_core_reset();
    TEST_ASSERT_EQUAL_INT(0, mock.device_close_count);
    TEST_ASSERT_NULL(usb_core_first_device());
}

static void test_hotplug_poll_no_hcd_is_safe(void)
{
    /* Must be safe before any HCD is registered — net_poll calls it on
     * every tick and should not crash on platforms without USB. */
    usb_core_register_hcd(NULL);
    int rc = usb_core_hotplug_poll();
    TEST_ASSERT_EQUAL_INT(0, rc);
}

static void test_hotplug_poll_no_device_connected(void)
{
    /* Mock HCD attached but port reports disconnected — hot-plug poll
     * must not attempt to enumerate. */
    reset_mock_and_clear_device_state(false, USB_SPEED_UNKNOWN);

    int rc = usb_core_hotplug_poll();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(0, mock.device_open_count);
    TEST_ASSERT_NULL(usb_core_first_device());
}

static void test_hotplug_poll_enumerates_on_attach(void)
{
    /* Mock HCD reports connected — hot-plug poll must drive the full
     * enumeration sequence, same as usb_core_start would. */
    reset_mock_and_clear_device_state(true, USB_SPEED_HIGH);

    int rc = usb_core_hotplug_poll();
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_NOT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_UINT8(1, mock.device_configured);
}

static void test_hotplug_poll_idempotent_after_enumeration(void)
{
    /* Once a device is present, subsequent polls are no-ops. This is
     * what makes calling hotplug_poll from net_poll every tick safe —
     * no repeated enumeration, no side effects on the mock HCD. */
    reset_mock_and_clear_device_state(true, USB_SPEED_HIGH);

    TEST_ASSERT_EQUAL_INT(1, usb_core_hotplug_poll());
    int opens_after_first = mock.device_open_count;

    TEST_ASSERT_EQUAL_INT(0, usb_core_hotplug_poll());
    TEST_ASSERT_EQUAL_INT(0, usb_core_hotplug_poll());
    TEST_ASSERT_EQUAL_INT(0, usb_core_hotplug_poll());

    /* No further device_open calls once the first enumeration took. */
    TEST_ASSERT_EQUAL_INT(opens_after_first, mock.device_open_count);
}

static void test_hotplug_poll_propagates_enumerate_failure(void)
{
    /* If the port goes connected but the HCD fails enumeration (e.g.,
     * device_open returns error), hotplug_poll must surface the
     * failure to the caller rather than silently succeeding. */
    reset_mock_and_clear_device_state(true, USB_SPEED_HIGH);
    mock.device_open_rc = -1;

    int rc = usb_core_hotplug_poll();
    TEST_ASSERT_LESS_THAN(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
}

/* -------------------------------------------------------------------------- */
/* Suite entry point                                                           */
/* -------------------------------------------------------------------------- */

int test_suite_usb_core(void);

int test_suite_usb_core(void)
{
    UnityBegin("test_usb_core.c");

    RUN_TEST(test_urb_status_strings);
    RUN_TEST(test_hcd_registration);
    RUN_TEST(test_start_and_enumerate);
    RUN_TEST(test_enumerate_no_device);
    RUN_TEST(test_enumerate_speed_low_propagates);
    RUN_TEST(test_enumerate_port_reset_failure);
    RUN_TEST(test_enumerate_device_open_failure);
    RUN_TEST(test_enumerate_set_config_failure);
    RUN_TEST(test_enumerate_endpoint_configure_failure);
    RUN_TEST(test_enumerate_control_failure);
    RUN_TEST(test_enumerate_resets_previous_device);
    RUN_TEST(test_descriptor_parse);
    RUN_TEST(test_find_endpoint_direction_filter);
    RUN_TEST(test_find_endpoint_mismatch);
    RUN_TEST(test_descriptor_parse_invalid_header);
    RUN_TEST(test_descriptor_parse_too_short);
    RUN_TEST(test_descriptor_parse_interface_overflow);
    RUN_TEST(test_descriptor_parse_endpoint_overflow);
    RUN_TEST(test_descriptor_parse_orphan_endpoint);
    RUN_TEST(test_cancel_pending_urb);
    RUN_TEST(test_cancel_with_no_hcd);
    RUN_TEST(test_submit_urb_no_hcd);
    RUN_TEST(test_submit_urb_null_args);
    RUN_TEST(test_submit_urb_hcd_positive_normalized);
    RUN_TEST(test_poll_no_hcd_is_safe);
    RUN_TEST(test_poll_calls_hcd);
    RUN_TEST(test_control_msg_timeout);
    RUN_TEST(test_enumerate_closes_device_on_control_failure);
    RUN_TEST(test_enumerate_closes_device_on_set_config_failure);
    RUN_TEST(test_enumerate_no_close_on_port_reset_failure);
    RUN_TEST(test_enumerate_no_close_on_device_open_failure);
    RUN_TEST(test_descriptor_parse_bad_config_blength);
    RUN_TEST(test_descriptor_parse_oversized_rejected);
    RUN_TEST(test_descriptor_parse_exact_cap_accepted);
    RUN_TEST(test_hcd_reregister_same_is_silent);
    RUN_TEST(test_control_msg_returns_actual_length);
    RUN_TEST(test_control_msg_unknown_request_returns_error);
    RUN_TEST(test_core_reset_clears_device_without_touching_hcd);
    RUN_TEST(test_core_reset_no_device_is_safe);
    RUN_TEST(test_hotplug_poll_no_hcd_is_safe);
    RUN_TEST(test_hotplug_poll_no_device_connected);
    RUN_TEST(test_hotplug_poll_enumerates_on_attach);
    RUN_TEST(test_hotplug_poll_idempotent_after_enumeration);
    RUN_TEST(test_hotplug_poll_propagates_enumerate_failure);

    return UnityEnd();
}
