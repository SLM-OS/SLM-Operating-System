/*
 * test_xhci_xfer.c - Phase 3A Step 6 / 7b TRB builder unit tests.
 *
 * The builders in xhci_trb_build.h are pure logic over a caller-
 * supplied struct xhci_trb — no MMIO, no allocation, no state. Every
 * bit the HC sees on a submitted TRB is set by these functions, so
 * this file exhaustively checks field placement + the invariants
 * xHCI §6.4.1 demands.
 */

#include "unity.h"
#include "test_harness.h"
#include "../drivers/usb/xhci/xhci_trb.h"
#include "../drivers/usb/xhci/xhci_trb_build.h"
#include "../drivers/usb/xhci/xhci_xfer_helpers.h"
#include "../include/usb.h"

#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Setup Stage                                                                 */
/* -------------------------------------------------------------------------- */

static void test_setup_stage_no_data(void)
{
    struct usb_setup_packet setup = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest      = USB_REQ_SET_ADDRESS,
        .wValue        = 1,
        .wIndex        = 0,
        .wLength       = 0,
    };
    struct xhci_trb t;
    xhci_build_setup_stage(&t, &setup, 0 /* TRT: no data */, 1);

    /* param_lo/hi contain the packed SETUP bytes in order. */
    uint8_t bytes[8];
    memcpy(bytes, &t.param_lo, 4);
    memcpy(bytes + 4, &t.param_hi, 4);
    TEST_ASSERT_EQUAL_UINT8(setup.bmRequestType, bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(setup.bRequest,      bytes[1]);
    TEST_ASSERT_EQUAL_UINT8(setup.wValue & 0xFF, bytes[2]);
    TEST_ASSERT_EQUAL_UINT8(setup.wValue >> 8,   bytes[3]);

    TEST_ASSERT_EQUAL_UINT32(8U, t.status);

    /* TRB type = SETUP_STAGE at bits 15:10. */
    TEST_ASSERT_EQUAL_UINT32(XHCI_TRB_SETUP_STAGE,
                             XHCI_TRB_TYPE_GET(t.control));
    /* IDT set, cycle set. */
    TEST_ASSERT_TRUE(t.control & XHCI_TRB_IDT);
    TEST_ASSERT_TRUE(t.control & XHCI_TRB_CYCLE);
    /* TRT field (bits 17:16) = 0. */
    TEST_ASSERT_EQUAL_UINT32(0U, (t.control >> 16) & 0x3U);
}

static void test_setup_stage_data_out(void)
{
    struct usb_setup_packet setup = {
        .bmRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest      = USB_REQ_SET_CONFIGURATION,
        .wValue        = 1, .wIndex = 0, .wLength = 0,
    };
    struct xhci_trb t;
    xhci_build_setup_stage(&t, &setup, 2 /* TRT: OUT */, 0);
    TEST_ASSERT_EQUAL_UINT32(2U, (t.control >> 16) & 0x3U);
    TEST_ASSERT_FALSE(t.control & XHCI_TRB_CYCLE);
}

static void test_setup_stage_data_in(void)
{
    struct usb_setup_packet setup = {
        .bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        .bRequest      = USB_REQ_GET_DESCRIPTOR,
        .wValue        = 0x0100, .wIndex = 0, .wLength = 18,
    };
    struct xhci_trb t;
    xhci_build_setup_stage(&t, &setup, 3 /* TRT: IN */, 1);
    TEST_ASSERT_EQUAL_UINT32(3U, (t.control >> 16) & 0x3U);
}

/* -------------------------------------------------------------------------- */
/* Data Stage                                                                  */
/* -------------------------------------------------------------------------- */

static void test_data_stage_in(void)
{
    uintptr_t buf = 0xBDE04000ULL;
    struct xhci_trb t;
    xhci_build_data_stage(&t, buf, 64, true /* in */, 1);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)(buf & 0xFFFFFFFFu), t.param_lo);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(buf >> 32),         t.param_hi);
    TEST_ASSERT_EQUAL_UINT32(64U, t.status & 0x1FFFFu);
    TEST_ASSERT_EQUAL_UINT32(XHCI_TRB_DATA_STAGE,
                             XHCI_TRB_TYPE_GET(t.control));
    TEST_ASSERT_TRUE(t.control & (1u << 16));   /* DIR = IN */
    TEST_ASSERT_TRUE(t.control & XHCI_TRB_ISP);
    TEST_ASSERT_TRUE(t.control & XHCI_TRB_CYCLE);
}

static void test_data_stage_out(void)
{
    struct xhci_trb t;
    xhci_build_data_stage(&t, 0, 32, false /* out */, 0);
    TEST_ASSERT_FALSE(t.control & (1u << 16));
    TEST_ASSERT_FALSE(t.control & XHCI_TRB_CYCLE);
}

static void test_data_stage_length_truncates(void)
{
    /* Bits above 16:0 of length MUST NOT leak into the TD Size /
     * interrupter target fields in status (§6.4.1.2.2). */
    struct xhci_trb t;
    xhci_build_data_stage(&t, 0, 0xFFFFFFFFu, true, 1);
    TEST_ASSERT_EQUAL_UINT32(0x1FFFFu, t.status & 0x1FFFFu);
    TEST_ASSERT_EQUAL_UINT32(0u,       t.status >> 17);
}

/* -------------------------------------------------------------------------- */
/* Status Stage                                                                */
/* -------------------------------------------------------------------------- */

static void test_status_stage_in(void)
{
    struct xhci_trb t;
    xhci_build_status_stage(&t, true, 1);
    TEST_ASSERT_EQUAL_UINT32(XHCI_TRB_STATUS_STAGE,
                             XHCI_TRB_TYPE_GET(t.control));
    TEST_ASSERT_TRUE(t.control & (1u << 16));
    TEST_ASSERT_TRUE(t.control & XHCI_TRB_IOC);
    TEST_ASSERT_TRUE(t.control & XHCI_TRB_CYCLE);
    /* Status Stage carries no data or length. */
    TEST_ASSERT_EQUAL_UINT32(0U, t.param_lo);
    TEST_ASSERT_EQUAL_UINT32(0U, t.param_hi);
    TEST_ASSERT_EQUAL_UINT32(0U, t.status);
}

static void test_status_stage_out(void)
{
    struct xhci_trb t;
    xhci_build_status_stage(&t, false, 0);
    TEST_ASSERT_FALSE(t.control & (1u << 16));
    TEST_ASSERT_TRUE(t.control & XHCI_TRB_IOC);
    TEST_ASSERT_FALSE(t.control & XHCI_TRB_CYCLE);
}

/* -------------------------------------------------------------------------- */
/* Normal (bulk + interrupt)                                                   */
/* -------------------------------------------------------------------------- */

static void test_normal_sets_ioc_isp(void)
{
    struct xhci_trb t;
    xhci_build_normal(&t, 0x12345678ULL, 1024, 1);
    TEST_ASSERT_EQUAL_UINT32(XHCI_TRB_NORMAL,
                             XHCI_TRB_TYPE_GET(t.control));
    TEST_ASSERT_TRUE(t.control & XHCI_TRB_IOC);
    TEST_ASSERT_TRUE(t.control & XHCI_TRB_ISP);
    TEST_ASSERT_TRUE(t.control & XHCI_TRB_CYCLE);
    TEST_ASSERT_EQUAL_UINT32(0x12345678u, t.param_lo);
    TEST_ASSERT_EQUAL_UINT32(0u,          t.param_hi);
    TEST_ASSERT_EQUAL_UINT32(1024u,       t.status & 0x1FFFFu);
}

static void test_normal_64bit_address(void)
{
    /* High 32 bits of the buffer PA must land in param_hi verbatim.
     * SLM-OS's NC memory is below 4 GB on Jetson but the driver path
     * must not break if that changes. */
    struct xhci_trb t;
    xhci_build_normal(&t, 0xCAFEBABE12345678ULL, 256, 0);
    TEST_ASSERT_EQUAL_UINT32(0x12345678u, t.param_lo);
    TEST_ASSERT_EQUAL_UINT32(0xCAFEBABEu, t.param_hi);
}

static void test_normal_cycle_0(void)
{
    struct xhci_trb t;
    xhci_build_normal(&t, 0, 0, 0);
    TEST_ASSERT_FALSE(t.control & XHCI_TRB_CYCLE);
}

/* -------------------------------------------------------------------------- */
/* Builder invariant: every builder zeroes the scratch TRB it's given — so    */
/* caller stack garbage never bleeds into field bits the HC reads.            */
/* -------------------------------------------------------------------------- */

static void test_builders_zero_scratch(void)
{
    struct xhci_trb t;
    memset(&t, 0xFF, sizeof(t));

    struct usb_setup_packet setup = {0};
    xhci_build_setup_stage(&t, &setup, 0, 0);
    /* Only expected bits survive. The status dword must be exactly 8
     * (Transfer Length field), everything else zero. */
    TEST_ASSERT_EQUAL_UINT32(8U, t.status);

    memset(&t, 0xFF, sizeof(t));
    xhci_build_data_stage(&t, 0, 0, false, 0);
    /* status = 0 (length 0), param_lo/hi = 0, control has type + ISP only. */
    TEST_ASSERT_EQUAL_UINT32(0U, t.param_lo);
    TEST_ASSERT_EQUAL_UINT32(0U, t.param_hi);
    TEST_ASSERT_EQUAL_UINT32(0U, t.status);
    TEST_ASSERT_EQUAL_UINT32(XHCI_TRB_DATA_STAGE,
                             XHCI_TRB_TYPE_GET(t.control));

    memset(&t, 0xFF, sizeof(t));
    xhci_build_status_stage(&t, true, 0);
    TEST_ASSERT_EQUAL_UINT32(0U, t.param_lo);
    TEST_ASSERT_EQUAL_UINT32(0U, t.param_hi);
    TEST_ASSERT_EQUAL_UINT32(0U, t.status);

    memset(&t, 0xFF, sizeof(t));
    xhci_build_normal(&t, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(0U, t.param_lo);
    TEST_ASSERT_EQUAL_UINT32(0U, t.param_hi);
    TEST_ASSERT_EQUAL_UINT32(0U, t.status);
}

/* -------------------------------------------------------------------------- */
/* actual_length-from-residual formula (#316)                                  */
/*                                                                            */
/* Pins the post-Transfer-Event accounting rule that turned a successful      */
/* short-packet control-IN from "actual_length=requested" (over-report) into  */
/* "actual_length = requested - residual". Lives in xhci_xfer_helpers.h so    */
/* the formula is reachable from the host test build (the surrounding event   */
/* dispatcher in xhci_xfer.c is gated under PLATFORM_JETSON_ORIN_NANO).       */
/* -------------------------------------------------------------------------- */

static void test_actual_from_residual_status_stage_success(void)
{
    /* Status Stage success: TRB carries no payload so residual=0, and
     * actual_length must equal the originally-requested wLength. */
    TEST_ASSERT_EQUAL_UINT32(64U,
        xhci_xfer_actual_from_residual(64U, 0U));
    TEST_ASSERT_EQUAL_UINT32(18U,
        xhci_xfer_actual_from_residual(18U, 0U));
}

static void test_actual_from_residual_data_stage_short_packet(void)
{
    /* The original #316 bug: wLength=64, device returns 18 bytes (e.g.
     * truncated GET_DESCRIPTOR), Data Stage SHORT_PACKET event arrives
     * with residual = 64 - 18 = 46. Pre-fix code reported 64; post-fix
     * must report 18. */
    TEST_ASSERT_EQUAL_UINT32(18U,
        xhci_xfer_actual_from_residual(64U, 46U));
    /* String-descriptor-style: requested 255, device returned 24. */
    TEST_ASSERT_EQUAL_UINT32(24U,
        xhci_xfer_actual_from_residual(255U, 231U));
}

static void test_actual_from_residual_zero_received(void)
{
    /* Full-short: device returned no payload at all. residual ==
     * requested → actual=0 (still distinguishable from "completed
     * successfully with N bytes" because urb->status carries the cc). */
    TEST_ASSERT_EQUAL_UINT32(0U,
        xhci_xfer_actual_from_residual(64U, 64U));
}

static void test_actual_from_residual_clamps_overflow(void)
{
    /* Defensive: a malformed event whose residual exceeds the request
     * (hardware bug or a stale event matched against a recycled slot)
     * must clamp to 0 rather than wrap into a multi-GB unsigned value. */
    TEST_ASSERT_EQUAL_UINT32(0U,
        xhci_xfer_actual_from_residual(64U, 65U));
    TEST_ASSERT_EQUAL_UINT32(0U,
        xhci_xfer_actual_from_residual(0U, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U,
        xhci_xfer_actual_from_residual(8U, 0xFFFFFFu));
}

static void test_actual_from_residual_zero_requested(void)
{
    /* Zero-length control transfer (e.g. SET_ADDRESS with no Data
     * Stage): residual must also be 0 and actual must be 0. */
    TEST_ASSERT_EQUAL_UINT32(0U,
        xhci_xfer_actual_from_residual(0U, 0U));
}

/* -------------------------------------------------------------------------- */
/* Suite entry                                                                 */
/* -------------------------------------------------------------------------- */

int test_suite_xhci_xfer(void);

int test_suite_xhci_xfer(void)
{
    UnityBegin("test_xhci_xfer.c");
    RUN_TEST(test_setup_stage_no_data);
    RUN_TEST(test_setup_stage_data_out);
    RUN_TEST(test_setup_stage_data_in);
    RUN_TEST(test_data_stage_in);
    RUN_TEST(test_data_stage_out);
    RUN_TEST(test_data_stage_length_truncates);
    RUN_TEST(test_status_stage_in);
    RUN_TEST(test_status_stage_out);
    RUN_TEST(test_normal_sets_ioc_isp);
    RUN_TEST(test_normal_64bit_address);
    RUN_TEST(test_normal_cycle_0);
    RUN_TEST(test_builders_zero_scratch);
    RUN_TEST(test_actual_from_residual_status_stage_success);
    RUN_TEST(test_actual_from_residual_data_stage_short_packet);
    RUN_TEST(test_actual_from_residual_zero_received);
    RUN_TEST(test_actual_from_residual_clamps_overflow);
    RUN_TEST(test_actual_from_residual_zero_requested);
    return UnityEnd();
}
