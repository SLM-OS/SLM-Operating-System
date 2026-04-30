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
 * iface1 alt1 (data, 2 EPs) + 7*2 bytes bulk EPs. The parser now keeps
 * the highest alternate setting it sees for a given interface number, so
 * iface1 ends up bound to alt1 with the two bulk endpoints.
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

    /* INTERFACE 1 alt 1 — CDC data (bulk IN + bulk OUT, 2 EPs) */
    0x09, USB_DT_INTERFACE, 0x01, 0x01, 0x02, 0x0A, 0x00, 0x00, 0x00,

    /* These endpoints belong to the retained alt1 interface. */
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

static bool hub_walk_handle_control(struct usb_urb *urb);

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

    /* Hub-walk extension (#575). When `hub_walk.enabled` is true,
     * intercept hub class requests + child standard requests. Falls
     * through to the existing single-device responses when the
     * extension declines the URB. */
    if (hub_walk_handle_control(urb))
        return 0;

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
    /* iface0 contributes the interrupt endpoint; iface1 resolves to alt1
     * and contributes the bulk IN + OUT endpoints. */
    TEST_ASSERT_EQUAL_INT(3, mock.endpoints_configured);
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

static void test_enumerate_initial_descriptor_retries_once_then_succeeds(void)
{
    /*
     * The initial 8-byte device-descriptor read now has a reopen/retry
     * loop. One transient control failure should trigger a close +
     * reopen and then still let enumeration succeed.
     */
    reset_mock_and_core();
    mock.fail_next_control = 1;

    int rc = usb_core_start();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_UINT8(1, mock.device_addr);
    TEST_ASSERT_EQUAL_INT(2, mock.device_open_count);
    TEST_ASSERT_EQUAL_INT(1, mock.device_close_count);
}

static void test_enumerate_initial_descriptor_fails_after_retry_budget(void)
{
    /*
     * Three consecutive failures on the first 8-byte GET_DESCRIPTOR
     * must exhaust the retry budget and leave no device present.
     */
    reset_mock_and_core();
    mock.fail_next_control = 3;

    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_UINT8(0, mock.device_addr);
    TEST_ASSERT_EQUAL_INT(3, mock.device_open_count);
    TEST_ASSERT_EQUAL_INT(3, mock.device_close_count);
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

    /* Now exhaust the initial-descriptor retry budget on the second
     * enumeration so the retry really fails rather than recovering. */
    mock.fail_next_control = 3;
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

    /* Interface 1 resolves to alt 1, which carries the two bulk endpoints. */
    TEST_ASSERT_TRUE(dev->ifaces[1].valid);
    TEST_ASSERT_EQUAL_UINT8(1, dev->ifaces[1].number);
    TEST_ASSERT_EQUAL_UINT8(1, dev->ifaces[1].alt_setting);
    TEST_ASSERT_EQUAL_UINT8(0x0A, dev->ifaces[1].class_code);
    TEST_ASSERT_EQUAL_UINT8(2, dev->ifaces[1].num_endpoints);

    /* iface 0 keeps the interrupt endpoint. */
    const struct usb_endpoint *intr_in =
        usb_find_endpoint(dev, 0, USB_DIR_IN, USB_XFER_INTERRUPT);
    TEST_ASSERT_NOT_NULL(intr_in);
    TEST_ASSERT_EQUAL_UINT8(0x81, intr_in->address);
    TEST_ASSERT_EQUAL_UINT16(16, intr_in->max_packet);
    TEST_ASSERT_EQUAL_UINT8(8, intr_in->interval);

    /* iface 1 resolves to alt1 and exposes the bulk endpoints. */
    const struct usb_endpoint *bulk_in =
        usb_find_endpoint(dev, 1, USB_DIR_IN, USB_XFER_BULK);
    const struct usb_endpoint *bulk_out =
        usb_find_endpoint(dev, 1, USB_DIR_OUT, USB_XFER_BULK);
    TEST_ASSERT_NOT_NULL(bulk_in);
    TEST_ASSERT_NOT_NULL(bulk_out);
    TEST_ASSERT_EQUAL_UINT8(0x88, bulk_in->address);
    TEST_ASSERT_EQUAL_UINT8(0x08, bulk_out->address);
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
    /* Iface 1 has bulk endpoints, but no interrupt endpoint. */
    TEST_ASSERT_NULL(usb_find_endpoint(dev, 1, USB_DIR_IN, USB_XFER_INTERRUPT));
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
     * Initial-descriptor failures now reopen/retry up to three times.
     * Each failed open must still be closed so the HCD does not leak
     * default-pipe state across retries.
     */
    reset_mock_and_core();
    mock.fail_next_control = 3;   /* fail all 8-byte GET_DESCRIPTOR attempts */

    int rc = usb_core_start();
    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_NULL(usb_core_first_device());
    TEST_ASSERT_EQUAL_INT(3, mock.device_close_count);
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
/* Hub-walk test (#575) — root device is a USB hub with TWO connected         */
/* downstream children; only the second is CDC-ECM. The walker must skip      */
/* port 1 (an HID device, like a keyboard plugged into the RTL8153 dongle's   */
/* pass-through port on jetson-nano-1) and bind the CDC-ECM device on port 2.*/
/* -------------------------------------------------------------------------- */

/* Hub descriptor wire bytes (9): bLength, bDescType=0x29, bNbrPorts=2,
 * wHubChars=0x0009 (per-port power+overcurrent), bPwrOn2PwrGood=10
 * (20 ms), bHubContrCurrent=8, device_removable=0, port_pwr_mask=0xff. */
static const uint8_t hub_walk_hub_desc[9] = {
    9, 0x29, 2, 0x09, 0x00, 10, 8, 0x00, 0xff,
};

static const struct usb_device_descriptor hub_walk_hub_dev_desc = {
    .bLength            = 18,
    .bDescriptorType    = USB_DT_DEVICE,
    .bcdUSB             = 0x0210,
    .bDeviceClass       = 0x09,   /* HUB */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x01,
    .bMaxPacketSize0    = 64,
    .idVendor           = 0x0BDA,
    .idProduct          = 0x5489,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0,
    .iProduct           = 0,
    .iSerialNumber      = 0,
    .bNumConfigurations = 1,
};

/* Minimal hub config: 9-byte CONFIG + 9-byte IFACE (class 0x09) +
 * 7-byte interrupt-IN endpoint. */
static const uint8_t hub_walk_hub_config[25] = {
    /* CONFIG */
    0x09, USB_DT_CONFIG, 25, 0x00, 0x01, 0x01, 0x00, 0xE0, 0x00,
    /* IFACE 0 (hub class) */
    0x09, USB_DT_INTERFACE, 0x00, 0x00, 0x01, 0x09, 0x00, 0x00, 0x00,
    /* EP 1 IN, interrupt, 2 bytes, bInterval=12 */
    0x07, USB_DT_ENDPOINT, 0x81, USB_XFER_INTERRUPT, 0x02, 0x00, 0x0c,
};

/* Port 1: a USB HID keyboard (low-speed). bDeviceClass=0x03 at the
 * device level, IFACE class=0x03 — does NOT pass
 * usb_config_is_cdc_ecm_candidate, so the walker should skip it. */
static const struct usb_device_descriptor hub_walk_port1_dev_desc = {
    .bLength            = 18,
    .bDescriptorType    = USB_DT_DEVICE,
    .bcdUSB             = 0x0110,
    .bDeviceClass       = 0x03,   /* HID */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 8,
    .idVendor           = 0x046D,
    .idProduct          = 0xC077,  /* generic logitech-ish keyboard */
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0,
    .iProduct           = 0,
    .iSerialNumber      = 0,
    .bNumConfigurations = 1,
};

static const uint8_t hub_walk_port1_config[25] = {
    /* CONFIG */
    0x09, USB_DT_CONFIG, 25, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32,
    /* IFACE 0 (HID, class=0x03 — not CDC) */
    0x09, USB_DT_INTERFACE, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    /* EP 1 IN, interrupt, 8 bytes */
    0x07, USB_DT_ENDPOINT, 0x81, USB_XFER_INTERRUPT, 0x08, 0x00, 0x0a,
};

/* Port 2: an RTL8153-shaped CDC-ECM device (high-speed). Reuses the
 * existing top-of-file canned config to keep the test definition
 * focused — that blob has the 0x02/0x06 control + 0x0a data interface
 * pair `usb_config_is_cdc_ecm_candidate` looks for. */
static const struct usb_device_descriptor hub_walk_port2_dev_desc = {
    .bLength            = 18,
    .bDescriptorType    = USB_DT_DEVICE,
    .bcdUSB             = 0x0210,
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

/* Tracks which device the hub-walk mock should respond to next.
 * Set by SET_FEATURE(PORT_RESET, port=N): subsequent
 * GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION traffic against
 * `urb->dev->address != hub_addr` is interpreted as targeting that
 * downstream port's canned device.
 *
 * The hub_addr discriminator (= 1 in this test, since the hub is
 * always the first device the core addresses) lets the same
 * `mock_submit_urb` extension serve both hub-side and child-side
 * standard requests without touching the existing single-device
 * tests. */
#define HUB_WALK_HUB_ADDR 1

static struct {
    bool    enabled;
    uint8_t walking_port;          /* 0 = none, 1/2 = which port's child */
    bool    port_connected[3];     /* 1-indexed; [0] unused */
    bool    port_high_speed[3];
    bool    port_low_speed[3];
    bool    port_c_reset[3];
    bool    port_enabled[3];
} hub_walk;

/* Returns true if the URB was a hub-walk request handled by this
 * extension; false if `mock_submit_urb` should fall through to its
 * usual single-device responses. */
static bool hub_walk_handle_control(struct usb_urb *urb)
{
    if (!hub_walk.enabled)
        return false;

    uint8_t bmReqType = urb->setup.bmRequestType;
    uint8_t req       = urb->setup.bRequest;
    uint16_t wValue   = urb->setup.wValue;
    uint16_t wIndex   = urb->setup.wIndex;

    uint8_t recip = bmReqType & 0x1F;
    uint8_t type  = bmReqType & USB_TYPE_MASK;

    /* Hub class — descriptor + port management. */
    if (type == USB_TYPE_CLASS) {
        uint8_t desc_type = (uint8_t)(wValue >> 8);
        if (req == USB_REQ_GET_DESCRIPTOR && desc_type == 0x29 /* HUB */) {
            complete_control(urb, hub_walk_hub_desc,
                             sizeof(hub_walk_hub_desc), USB_URB_OK);
            return true;
        }
        if (recip == USB_RECIP_OTHER) {
            uint8_t port = (uint8_t)wIndex;
            if (req == USB_REQ_GET_STATUS) {
                uint16_t status = 0, change = 0;
                if (port >= 1 && port <= 2) {
                    if (hub_walk.port_connected[port])
                        status |= 0x0001; /* CONNECTION */
                    if (hub_walk.port_enabled[port])
                        status |= 0x0002; /* ENABLE */
                    status |= 0x0100;     /* POWER */
                    if (hub_walk.port_high_speed[port])
                        status |= 0x0400; /* HIGH_SPEED */
                    else if (hub_walk.port_low_speed[port])
                        status |= 0x0200; /* LOW_SPEED */
                    if (hub_walk.port_c_reset[port])
                        change |= 0x0010; /* C_RESET */
                }
                uint8_t buf[4] = {
                    (uint8_t)(status & 0xff),
                    (uint8_t)(status >> 8),
                    (uint8_t)(change & 0xff),
                    (uint8_t)(change >> 8),
                };
                complete_control(urb, buf, sizeof(buf), USB_URB_OK);
                return true;
            }
            if (req == USB_REQ_SET_FEATURE) {
                if (wValue == 4 /* PORT_RESET */ && port >= 1 && port <= 2) {
                    /* On reset: enable the port, set the change bit,
                     * and switch the mock's child-response stream
                     * to this port's canned device blob. The next
                     * GET_PORT_STATUS will report C_RESET; the
                     * walker then issues CLEAR_FEATURE(C_RESET)
                     * and proceeds to enumerate the child. */
                    hub_walk.walking_port = port;
                    hub_walk.port_enabled[port] = true;
                    hub_walk.port_c_reset[port] = true;
                }
                complete_control(urb, NULL, 0, USB_URB_OK);
                return true;
            }
            if (req == USB_REQ_CLEAR_FEATURE) {
                if (wValue == 20 /* C_RESET */ && port >= 1 && port <= 2)
                    hub_walk.port_c_reset[port] = false;
                complete_control(urb, NULL, 0, USB_URB_OK);
                return true;
            }
        }
        return false;
    }

    /* Standard requests on the hub itself (addr == HUB_WALK_HUB_ADDR)
     * or on the just-attached child (addr == 0 pre-SET_ADDRESS).
     * We discriminate by `urb->dev->address`: the hub is at
     * HUB_WALK_HUB_ADDR after its initial enumeration; downstream
     * children start at 0 and get assigned >1 by the core's
     * SET_ADDRESS path. */
    if (type == USB_TYPE_STANDARD && recip == USB_RECIP_DEVICE) {
        bool is_hub = (urb->dev != NULL &&
                       urb->dev->address == HUB_WALK_HUB_ADDR);
        if (req == USB_REQ_GET_DESCRIPTOR) {
            uint8_t desc_type = (uint8_t)(wValue >> 8);
            if (is_hub) {
                if (desc_type == USB_DT_DEVICE)
                    complete_control(urb, &hub_walk_hub_dev_desc,
                                     sizeof(hub_walk_hub_dev_desc),
                                     USB_URB_OK);
                else if (desc_type == USB_DT_CONFIG)
                    complete_control(urb, hub_walk_hub_config,
                                     sizeof(hub_walk_hub_config),
                                     USB_URB_OK);
                else
                    complete_control(urb, NULL, 0, USB_URB_IO_ERROR);
                return true;
            }
            /* Downstream child — pick the canned blob for the port
             * the walker most recently reset. */
            if (hub_walk.walking_port == 1) {
                if (desc_type == USB_DT_DEVICE)
                    complete_control(urb, &hub_walk_port1_dev_desc,
                                     sizeof(hub_walk_port1_dev_desc),
                                     USB_URB_OK);
                else if (desc_type == USB_DT_CONFIG)
                    complete_control(urb, hub_walk_port1_config,
                                     sizeof(hub_walk_port1_config),
                                     USB_URB_OK);
                else
                    complete_control(urb, NULL, 0, USB_URB_IO_ERROR);
                return true;
            }
            if (hub_walk.walking_port == 2) {
                if (desc_type == USB_DT_DEVICE)
                    complete_control(urb, &hub_walk_port2_dev_desc,
                                     sizeof(hub_walk_port2_dev_desc),
                                     USB_URB_OK);
                else if (desc_type == USB_DT_CONFIG)
                    complete_control(urb, mock_config, sizeof(mock_config),
                                     USB_URB_OK);
                else
                    complete_control(urb, NULL, 0, USB_URB_IO_ERROR);
                return true;
            }
            return false;
        }
        if (req == USB_REQ_SET_ADDRESS) {
            mock.device_addr = (uint8_t)wValue;
            complete_control(urb, NULL, 0, USB_URB_OK);
            return true;
        }
        if (req == USB_REQ_SET_CONFIGURATION) {
            mock.device_configured = (uint8_t)wValue;
            complete_control(urb, NULL, 0, USB_URB_OK);
            return true;
        }
    }
    return false;
}

static void test_enumerate_hub_walks_past_non_cdc_child(void)
{
    /* Regression for #575: the RTL8153 dongle on jetson-nano-1 has
     * pass-through USB-A ports — when something else (a keyboard, a
     * mouse) is plugged in, that device sits on a lower-numbered
     * downstream hub port than the RTL8153 chip itself. The pre-fix
     * usb_core picked the first connected port and bound to the
     * keyboard, never reaching the RTL8153. Verify the walker now
     * skips the non-CDC child and binds the CDC-ECM device. */
    reset_mock_and_core();
    mock.port_connected = true;
    mock.port_speed     = USB_SPEED_HIGH;

    memset(&hub_walk, 0, sizeof(hub_walk));
    hub_walk.enabled            = true;
    hub_walk.port_connected[1]  = true;
    hub_walk.port_low_speed[1]  = true;   /* HID */
    hub_walk.port_connected[2]  = true;
    hub_walk.port_high_speed[2] = true;   /* RTL8153 */

    TEST_ASSERT_EQUAL_INT(0, usb_core_start());
    int rc = usb_core_enumerate();
    TEST_ASSERT_EQUAL_INT(0, rc);

    const struct usb_device *bound = usb_core_first_device();
    TEST_ASSERT_NOT_NULL(bound);
    /* The walker must have committed to port 2's RTL8153, not port
     * 1's HID. Pin the vendor + product as the load-bearing claim. */
    TEST_ASSERT_EQUAL_HEX16(0x0BDA, bound->dev_desc.idVendor);
    TEST_ASSERT_EQUAL_HEX16(0x8153, bound->dev_desc.idProduct);

    /* Port 2 wins, so the HID on port 1 must have been device_close'd
     * (released) before the walker moved on. Pin the lower bound:
     * the HID was actually freed. */
    TEST_ASSERT_TRUE(mock.device_close_count >= 1);

    hub_walk.enabled = false;
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
    RUN_TEST(test_enumerate_initial_descriptor_retries_once_then_succeeds);
    RUN_TEST(test_enumerate_initial_descriptor_fails_after_retry_budget);
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
    RUN_TEST(test_enumerate_hub_walks_past_non_cdc_child);

    return UnityEnd();
}
