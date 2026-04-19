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
    return UnityEnd();
}
