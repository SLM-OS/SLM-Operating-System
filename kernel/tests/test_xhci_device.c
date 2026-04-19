/*
 * test_xhci_device.c - Phase 3A Step 5 unit tests.
 *
 * Pure-logic coverage for the port-status decode and the context
 * layout helpers in xhci_ctx.h. Runs on every target since nothing
 * here touches MMIO. Device-open / address-device code paths need
 * hardware; they're exercised in the end-to-end serial capture on
 * jetson-nano-1 documented in the PR description.
 */

#include "unity.h"
#include "test_harness.h"
#include "../drivers/usb/xhci/xhci_internal.h"
#include "../drivers/usb/xhci/xhci_regs.h"
#include "../drivers/usb/xhci/xhci_ctx.h"
#include "../drivers/usb/xhci/xhci_attach.h"
#include "../include/usb.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* PORTSC decode                                                               */
/* -------------------------------------------------------------------------- */

static void test_portsc_disconnected(void)
{
    /* CCS=0 — no device. Speed bits ignored. */
    uint32_t portsc = 0x00000000;
    bool connected = true;
    enum usb_speed sp = USB_SPEED_HIGH;
    TEST_ASSERT_TRUE(xhci_decode_portsc(portsc, &connected, &sp));
    TEST_ASSERT_FALSE(connected);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_UNKNOWN, sp);
}

static void test_portsc_high_speed(void)
{
    uint32_t portsc = XHCI_PORTSC_CCS |
                      ((uint32_t)XHCI_PORTSC_SPEED_HIGH <<
                           XHCI_PORTSC_SPEED_SHIFT);
    bool c = false;
    enum usb_speed s = USB_SPEED_UNKNOWN;
    TEST_ASSERT_TRUE(xhci_decode_portsc(portsc, &c, &s));
    TEST_ASSERT_TRUE(c);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_HIGH, s);
}

static void test_portsc_full_speed(void)
{
    uint32_t portsc = XHCI_PORTSC_CCS |
                      ((uint32_t)XHCI_PORTSC_SPEED_FULL <<
                           XHCI_PORTSC_SPEED_SHIFT);
    bool c; enum usb_speed s;
    TEST_ASSERT_TRUE(xhci_decode_portsc(portsc, &c, &s));
    TEST_ASSERT_TRUE(c);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_FULL, s);
}

static void test_portsc_low_speed(void)
{
    uint32_t portsc = XHCI_PORTSC_CCS |
                      ((uint32_t)XHCI_PORTSC_SPEED_LOW <<
                           XHCI_PORTSC_SPEED_SHIFT);
    bool c; enum usb_speed s;
    TEST_ASSERT_TRUE(xhci_decode_portsc(portsc, &c, &s));
    TEST_ASSERT_TRUE(c);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_LOW, s);
}

static void test_portsc_super_speed(void)
{
    /* Phase 3A doesn't drive SuperSpeed, but decode still recognises
     * it so port_status can log "speed out of scope" rather than
     * "unknown". */
    uint32_t portsc = XHCI_PORTSC_CCS |
                      ((uint32_t)XHCI_PORTSC_SPEED_SUPER <<
                           XHCI_PORTSC_SPEED_SHIFT);
    bool c; enum usb_speed s;
    TEST_ASSERT_TRUE(xhci_decode_portsc(portsc, &c, &s));
    TEST_ASSERT_TRUE(c);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_SUPER, s);
}

static void test_portsc_unknown_speed_id(void)
{
    /* Reserved / unknown speed ID: connected=true, speed=UNKNOWN,
     * decode returns false so port_status can flag it. */
    uint32_t portsc = XHCI_PORTSC_CCS |
                      ((uint32_t)0xF << XHCI_PORTSC_SPEED_SHIFT);
    bool c; enum usb_speed s;
    TEST_ASSERT_FALSE(xhci_decode_portsc(portsc, &c, &s));
    TEST_ASSERT_TRUE(c);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_UNKNOWN, s);
}

static void test_portsc_ignores_change_bits(void)
{
    /* PRC / PEC / CSC set must not perturb the (connected, speed)
     * decode — they're reset-ack bits the driver clears elsewhere. */
    uint32_t portsc = XHCI_PORTSC_CCS |
                      ((uint32_t)XHCI_PORTSC_SPEED_HIGH <<
                           XHCI_PORTSC_SPEED_SHIFT) |
                      XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_PRC;
    bool c; enum usb_speed s;
    TEST_ASSERT_TRUE(xhci_decode_portsc(portsc, &c, &s));
    TEST_ASSERT_TRUE(c);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_HIGH, s);
}

static void test_portsc_null_output_safe(void)
{
    uint32_t portsc = XHCI_PORTSC_CCS |
                      ((uint32_t)XHCI_PORTSC_SPEED_HIGH <<
                           XHCI_PORTSC_SPEED_SHIFT);
    TEST_ASSERT_TRUE(xhci_decode_portsc(portsc, NULL, NULL));
}

/* -------------------------------------------------------------------------- */
/* Context layout / DCI helpers                                                */
/* -------------------------------------------------------------------------- */

static void test_ctx_stride_matches_csz(void)
{
    TEST_ASSERT_EQUAL_UINT32(32, xhci_ctx_stride(false));
    TEST_ASSERT_EQUAL_UINT32(64, xhci_ctx_stride(true));
}

static void test_ctx_dev_bytes(void)
{
    TEST_ASSERT_EQUAL_UINT32(32U * 32U, xhci_ctx_dev_bytes(false));
    TEST_ASSERT_EQUAL_UINT32(32U * 64U, xhci_ctx_dev_bytes(true));
}

static void test_ctx_in_bytes(void)
{
    TEST_ASSERT_EQUAL_UINT32(33U * 32U, xhci_ctx_in_bytes(false));
    TEST_ASSERT_EQUAL_UINT32(33U * 64U, xhci_ctx_in_bytes(true));
}

static void test_ctx_dev_offset(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U,          xhci_ctx_dev_offset(XHCI_DCI_SLOT, false));
    TEST_ASSERT_EQUAL_UINT32(32U,         xhci_ctx_dev_offset(XHCI_DCI_EP0, false));
    TEST_ASSERT_EQUAL_UINT32(31U * 32U,   xhci_ctx_dev_offset(31U, false));

    TEST_ASSERT_EQUAL_UINT32(0U,          xhci_ctx_dev_offset(XHCI_DCI_SLOT, true));
    TEST_ASSERT_EQUAL_UINT32(64U,         xhci_ctx_dev_offset(XHCI_DCI_EP0, true));
    TEST_ASSERT_EQUAL_UINT32(31U * 64U,   xhci_ctx_dev_offset(31U, true));
}

static void test_ctx_in_offset(void)
{
    TEST_ASSERT_EQUAL_UINT32(32U,         xhci_ctx_in_offset(XHCI_DCI_SLOT, false));
    TEST_ASSERT_EQUAL_UINT32(64U,         xhci_ctx_in_offset(XHCI_DCI_EP0, false));
    TEST_ASSERT_EQUAL_UINT32(64U,         xhci_ctx_in_offset(XHCI_DCI_SLOT, true));
    TEST_ASSERT_EQUAL_UINT32(128U,        xhci_ctx_in_offset(XHCI_DCI_EP0, true));
}

static void test_dci_ep_addresses(void)
{
    TEST_ASSERT_EQUAL_UINT32(2U,  xhci_dci_ep(0x01));  /* OUT ep 1 */
    TEST_ASSERT_EQUAL_UINT32(3U,  xhci_dci_ep(0x81));  /* IN  ep 1 */
    TEST_ASSERT_EQUAL_UINT32(4U,  xhci_dci_ep(0x02));  /* OUT ep 2 */
    TEST_ASSERT_EQUAL_UINT32(31U, xhci_dci_ep(0x8F));  /* IN  ep 15 */
}

/* -------------------------------------------------------------------------- */
/* Context field accessor round-trip — writes at DCI N must land at the       */
/* correct byte offset for both CSZ=0 and CSZ=1 and not bleed into neighbours. */
/* -------------------------------------------------------------------------- */

static void do_ctx_accessor_round_trip(bool cz)
{
    uint8_t buf[33 * 64];
    memset(buf, 0, sizeof(buf));

    *xhci_in_control_dw(buf, 0) = 0xDEADBEEFu;  /* Drop flags */
    *xhci_in_control_dw(buf, 1) = 0xCAFEBABEu;  /* Add  flags */

    *xhci_in_slot_dw(buf, 0, cz) = 0x11223344u;
    *xhci_in_ep_dw(buf, XHCI_DCI_EP0, 1, cz) = 0x55667788u;
    *xhci_in_ep_dw(buf, 3, 2, cz) = 0x99AABBCCu;

    uint32_t *dw = (uint32_t *)buf;
    uint32_t stride_dw = xhci_ctx_stride(cz) / 4U;

    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, dw[0]);
    TEST_ASSERT_EQUAL_UINT32(0xCAFEBABEu, dw[1]);
    TEST_ASSERT_EQUAL_UINT32(0x11223344u, dw[stride_dw * 1]);
    TEST_ASSERT_EQUAL_UINT32(0x55667788u, dw[stride_dw * 2 + 1]);
    TEST_ASSERT_EQUAL_UINT32(0x99AABBCCu, dw[stride_dw * 4 + 2]);
}

static void test_ctx_accessor_round_trip_csz0(void) { do_ctx_accessor_round_trip(false); }
static void test_ctx_accessor_round_trip_csz1(void) { do_ctx_accessor_round_trip(true);  }

/* -------------------------------------------------------------------------- */
/* Device context offset neighbours: write at DCI N, confirm adjacent DCIs     */
/* stay zero — catches the CSZ=1 padding-split-across-DCI bug the plan §10.8   */
/* Lessons block explicitly warned about.                                      */
/* -------------------------------------------------------------------------- */

static void do_ctx_dev_neighbours(bool cz)
{
    uint8_t buf[32 * 64];
    memset(buf, 0, sizeof(buf));

    /* Write one dword into each of DCI 3, 5, 7 (arbitrary non-adjacent
     * EPs) and confirm DCI 2, 4, 6, 8 stay all zero afterwards. */
    *xhci_dev_ep_dw(buf, 3, 1, cz) = 0xAAAAAAAAu;
    *xhci_dev_ep_dw(buf, 5, 2, cz) = 0xBBBBBBBBu;
    *xhci_dev_ep_dw(buf, 7, 3, cz) = 0xCCCCCCCCu;

    /* Neighbour DCIs remain zeroed across every dword — catches the
     * "context stride wrong" bug by confirming no bleed into adjacent
     * DCIs. */
    for (unsigned dci_probe = 2; dci_probe <= 8; dci_probe += 2) {
        for (unsigned i = 0; i < xhci_ctx_stride(cz) / 4U; i++) {
            TEST_ASSERT_EQUAL_UINT32(
                0, *xhci_dev_ep_dw(buf, dci_probe, i, cz));
        }
    }

    /* The three touched dwords read back exactly. */
    TEST_ASSERT_EQUAL_UINT32(0xAAAAAAAAu, *xhci_dev_ep_dw(buf, 3, 1, cz));
    TEST_ASSERT_EQUAL_UINT32(0xBBBBBBBBu, *xhci_dev_ep_dw(buf, 5, 2, cz));
    TEST_ASSERT_EQUAL_UINT32(0xCCCCCCCCu, *xhci_dev_ep_dw(buf, 7, 3, cz));
}

static void test_ctx_dev_neighbours_csz0(void) { do_ctx_dev_neighbours(false); }
static void test_ctx_dev_neighbours_csz1(void) { do_ctx_dev_neighbours(true);  }

/* -------------------------------------------------------------------------- */
/* Hot-plug attach state machine (#309 re-plug workaround)                     */
/* -------------------------------------------------------------------------- */

static void test_attach_stale_hides_connected_device(void)
{
    /* STALE + raw CCS=1: hide the pre-kexec device from usb_core. */
    struct xhci_attach_result r =
        xhci_attach_step(XHCI_ATTACH_STALE, true, USB_SPEED_HIGH);
    TEST_ASSERT_EQUAL_INT(XHCI_ATTACH_STALE, r.next_state);
    TEST_ASSERT_FALSE(r.transitioned);
    TEST_ASSERT_FALSE(r.report_connected);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_UNKNOWN, r.report_speed);
}

static void test_attach_stale_unplug_advances_to_wait(void)
{
    /* STALE + raw CCS=0: the stale device has been physically
     * detached — advance to WAIT_RECONNECT and continue hiding. */
    struct xhci_attach_result r =
        xhci_attach_step(XHCI_ATTACH_STALE, false, USB_SPEED_UNKNOWN);
    TEST_ASSERT_EQUAL_INT(XHCI_ATTACH_WAIT_RECONNECT, r.next_state);
    TEST_ASSERT_TRUE(r.transitioned);
    TEST_ASSERT_FALSE(r.report_connected);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_UNKNOWN, r.report_speed);
}

static void test_attach_wait_reports_disconnected_while_empty(void)
{
    /* WAIT_RECONNECT + raw CCS=0: the expected steady state between
     * unplug and re-plug. Stay in WAIT_RECONNECT. */
    struct xhci_attach_result r =
        xhci_attach_step(XHCI_ATTACH_WAIT_RECONNECT, false, USB_SPEED_UNKNOWN);
    TEST_ASSERT_EQUAL_INT(XHCI_ATTACH_WAIT_RECONNECT, r.next_state);
    TEST_ASSERT_FALSE(r.transitioned);
    TEST_ASSERT_FALSE(r.report_connected);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_UNKNOWN, r.report_speed);
}

static void test_attach_wait_reconnect_advances_on_attach(void)
{
    /* WAIT_RECONNECT + raw CCS=1: fresh attach — advance to FRESH
     * and surface the raw speed to usb_core on the same call. */
    struct xhci_attach_result r =
        xhci_attach_step(XHCI_ATTACH_WAIT_RECONNECT, true, USB_SPEED_HIGH);
    TEST_ASSERT_EQUAL_INT(XHCI_ATTACH_FRESH, r.next_state);
    TEST_ASSERT_TRUE(r.transitioned);
    TEST_ASSERT_TRUE(r.report_connected);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_HIGH, r.report_speed);
}

static void test_attach_fresh_passes_through_connected(void)
{
    /* FRESH + raw CCS=1: steady state after enumeration — raw values
     * flow straight through. */
    struct xhci_attach_result r =
        xhci_attach_step(XHCI_ATTACH_FRESH, true, USB_SPEED_FULL);
    TEST_ASSERT_EQUAL_INT(XHCI_ATTACH_FRESH, r.next_state);
    TEST_ASSERT_FALSE(r.transitioned);
    TEST_ASSERT_TRUE(r.report_connected);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_FULL, r.report_speed);
}

static void test_attach_fresh_passes_through_disconnect(void)
{
    /* FRESH + raw CCS=0: device unplugged after a successful
     * enumeration. Report disconnected; do NOT revert to STALE —
     * once FRESH, always trust the raw bit. */
    struct xhci_attach_result r =
        xhci_attach_step(XHCI_ATTACH_FRESH, false, USB_SPEED_UNKNOWN);
    TEST_ASSERT_EQUAL_INT(XHCI_ATTACH_FRESH, r.next_state);
    TEST_ASSERT_FALSE(r.transitioned);
    TEST_ASSERT_FALSE(r.report_connected);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_UNKNOWN, r.report_speed);
}

static void test_attach_full_replug_sequence(void)
{
    /* Walk the complete expected Angle 3 timeline from a kexec boot:
     * stale device present → user unplugs → steady gap → user
     * re-plugs → enumeration runs → steady-state connected. Each
     * step must move usb_core's view exactly once. */
    enum xhci_attach_phase state = XHCI_ATTACH_STALE;
    struct xhci_attach_result r;

    /* t0: stale device still attached. usb_core sees disconnected. */
    r = xhci_attach_step(state, true, USB_SPEED_HIGH);
    TEST_ASSERT_FALSE(r.report_connected);
    state = r.next_state;
    TEST_ASSERT_EQUAL_INT(XHCI_ATTACH_STALE, state);

    /* t1: user unplugs. CCS drops. usb_core still sees disconnected,
     * but the state machine advances. */
    r = xhci_attach_step(state, false, USB_SPEED_UNKNOWN);
    TEST_ASSERT_FALSE(r.report_connected);
    TEST_ASSERT_TRUE(r.transitioned);
    state = r.next_state;
    TEST_ASSERT_EQUAL_INT(XHCI_ATTACH_WAIT_RECONNECT, state);

    /* t2: a few poll cycles later, still no device. */
    r = xhci_attach_step(state, false, USB_SPEED_UNKNOWN);
    TEST_ASSERT_FALSE(r.report_connected);
    TEST_ASSERT_FALSE(r.transitioned);
    state = r.next_state;

    /* t3: user re-plugs. CCS rises. usb_core sees connected + speed
     * on this very call, so enumeration kicks off immediately. */
    r = xhci_attach_step(state, true, USB_SPEED_HIGH);
    TEST_ASSERT_TRUE(r.report_connected);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_HIGH, r.report_speed);
    TEST_ASSERT_TRUE(r.transitioned);
    state = r.next_state;
    TEST_ASSERT_EQUAL_INT(XHCI_ATTACH_FRESH, state);

    /* t4: steady state while the dongle is running. */
    r = xhci_attach_step(state, true, USB_SPEED_HIGH);
    TEST_ASSERT_TRUE(r.report_connected);
    TEST_ASSERT_EQUAL_INT(USB_SPEED_HIGH, r.report_speed);
    TEST_ASSERT_FALSE(r.transitioned);
}

static void test_attach_is_idempotent(void)
{
    /* Calling the step function with the same inputs repeatedly must
     * not drift — transitioned flips to false after the first call
     * and the reported state stays consistent. usb_core calls
     * port_status on every poll tick, so idempotence is load-bearing. */
    struct xhci_attach_result a =
        xhci_attach_step(XHCI_ATTACH_FRESH, true, USB_SPEED_HIGH);
    struct xhci_attach_result b =
        xhci_attach_step(a.next_state, true, USB_SPEED_HIGH);
    struct xhci_attach_result c =
        xhci_attach_step(b.next_state, true, USB_SPEED_HIGH);
    TEST_ASSERT_EQUAL_INT(a.next_state, b.next_state);
    TEST_ASSERT_EQUAL_INT(b.next_state, c.next_state);
    TEST_ASSERT_TRUE(b.report_connected);
    TEST_ASSERT_TRUE(c.report_connected);
    TEST_ASSERT_FALSE(b.transitioned);
    TEST_ASSERT_FALSE(c.transitioned);
}

/* -------------------------------------------------------------------------- */
/* Suite entry                                                                 */
/* -------------------------------------------------------------------------- */

int test_suite_xhci_device(void);

int test_suite_xhci_device(void)
{
    UnityBegin("test_xhci_device.c");
    RUN_TEST(test_portsc_disconnected);
    RUN_TEST(test_portsc_high_speed);
    RUN_TEST(test_portsc_full_speed);
    RUN_TEST(test_portsc_low_speed);
    RUN_TEST(test_portsc_super_speed);
    RUN_TEST(test_portsc_unknown_speed_id);
    RUN_TEST(test_portsc_ignores_change_bits);
    RUN_TEST(test_portsc_null_output_safe);
    RUN_TEST(test_ctx_stride_matches_csz);
    RUN_TEST(test_ctx_dev_bytes);
    RUN_TEST(test_ctx_in_bytes);
    RUN_TEST(test_ctx_dev_offset);
    RUN_TEST(test_ctx_in_offset);
    RUN_TEST(test_dci_ep_addresses);
    RUN_TEST(test_ctx_accessor_round_trip_csz0);
    RUN_TEST(test_ctx_accessor_round_trip_csz1);
    RUN_TEST(test_ctx_dev_neighbours_csz0);
    RUN_TEST(test_ctx_dev_neighbours_csz1);
    RUN_TEST(test_attach_stale_hides_connected_device);
    RUN_TEST(test_attach_stale_unplug_advances_to_wait);
    RUN_TEST(test_attach_wait_reports_disconnected_while_empty);
    RUN_TEST(test_attach_wait_reconnect_advances_on_attach);
    RUN_TEST(test_attach_fresh_passes_through_connected);
    RUN_TEST(test_attach_fresh_passes_through_disconnect);
    RUN_TEST(test_attach_full_replug_sequence);
    RUN_TEST(test_attach_is_idempotent);
    return UnityEnd();
}
