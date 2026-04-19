/*
 * test_tegra_padctl.c — Pure-logic tests for the Tegra234 padctl helper.
 *
 * The power-cycle function itself needs real MMIO and can't be unit-
 * tested; these tests pin the register-offset math and port-index
 * mapping so a silent drift in the Linux-reference offsets doesn't
 * get discovered only at the next hardware iteration. Linux-reference
 * line numbers point at ~/slmos-ref/linux/linux-phy-tegra186-xusb.c.
 */

#include "unity.h"
#include "test_harness.h"
#include "../drivers/usb/xhci/tegra_padctl.h"

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* USB2_OTG_PADX_CTL0/1 register offsets (linux-phy-tegra186-xusb.c:71, 79)    */
/* -------------------------------------------------------------------------- */

static void test_usb2_otg_pad_ctl0_pad0(void)
{
    TEST_ASSERT_EQUAL_UINT32(0x88U, XUSB_PADCTL_USB2_OTG_PADX_CTL0(0));
}

static void test_usb2_otg_pad_ctl0_stride(void)
{
    /* Each pad is 0x40 apart. */
    TEST_ASSERT_EQUAL_UINT32(0x88U + 0x40U, XUSB_PADCTL_USB2_OTG_PADX_CTL0(1));
    TEST_ASSERT_EQUAL_UINT32(0x88U + 0x80U, XUSB_PADCTL_USB2_OTG_PADX_CTL0(2));
    TEST_ASSERT_EQUAL_UINT32(0x88U + 0xc0U, XUSB_PADCTL_USB2_OTG_PADX_CTL0(3));
}

static void test_usb2_otg_pad_ctl1_pad0(void)
{
    /* CTL1 sits 4 bytes above CTL0 in the same 0x40 window. */
    TEST_ASSERT_EQUAL_UINT32(0x8cU, XUSB_PADCTL_USB2_OTG_PADX_CTL1(0));
}

static void test_usb2_otg_pad_ctl1_stride(void)
{
    TEST_ASSERT_EQUAL_UINT32(0x8cU + 0x40U, XUSB_PADCTL_USB2_OTG_PADX_CTL1(1));
    TEST_ASSERT_EQUAL_UINT32(0x8cU + 0xc0U, XUSB_PADCTL_USB2_OTG_PADX_CTL1(3));
}

static void test_ctl0_ctl1_adjacent(void)
{
    /* CTL1 is always exactly 4 bytes after CTL0 for the same pad —
     * a regression guard against accidentally reordering the stride
     * constant. */
    for (unsigned i = 0; i < TEGRA_XUSB_NUM_USB2_PADS; i++) {
        TEST_ASSERT_EQUAL_UINT32(
            XUSB_PADCTL_USB2_OTG_PADX_CTL0(i) + 4U,
            XUSB_PADCTL_USB2_OTG_PADX_CTL1(i));
    }
}

/* -------------------------------------------------------------------------- */
/* Bias pad offsets + bit defines (linux-phy-tegra186-xusb.c:84-92)            */
/* -------------------------------------------------------------------------- */

static void test_bias_pad_offsets(void)
{
    TEST_ASSERT_EQUAL_UINT32(0x284U, XUSB_PADCTL_USB2_BIAS_PAD_CTL0);
    TEST_ASSERT_EQUAL_UINT32(0x288U, XUSB_PADCTL_USB2_BIAS_PAD_CTL1);
}

static void test_power_down_bits(void)
{
    /* USB2_OTG_PD = bit 26 in CTL0. USB2_OTG_PD_DR = bit 2 in CTL1.
     * BIAS_PAD_PD = bit 11. USB2_PD_TRK = bit 26 in BIAS CTL1.
     * Pinned against the upstream defines. */
    TEST_ASSERT_EQUAL_UINT32(1u << 26, USB2_OTG_PD);
    TEST_ASSERT_EQUAL_UINT32(1u << 27, USB2_OTG_PD2);
    TEST_ASSERT_EQUAL_UINT32(1u << 29, USB2_OTG_PD_ZI);
    TEST_ASSERT_EQUAL_UINT32(1u <<  2, USB2_OTG_PD_DR);
    TEST_ASSERT_EQUAL_UINT32(1u << 11, BIAS_PAD_PD);
    TEST_ASSERT_EQUAL_UINT32(1u << 26, USB2_PD_TRK);
}

/* -------------------------------------------------------------------------- */
/* PORTSC-to-pad mapping                                                       */
/* -------------------------------------------------------------------------- */

static void test_portsc_usb2_range(void)
{
    /* PORTSC[0..3] are the SS lanes; PORTSC[4..7] are USB2 pads 0..3. */
    TEST_ASSERT_FALSE(tegra_padctl_portsc_is_usb2(0));
    TEST_ASSERT_FALSE(tegra_padctl_portsc_is_usb2(3));
    TEST_ASSERT_TRUE (tegra_padctl_portsc_is_usb2(4));
    TEST_ASSERT_TRUE (tegra_padctl_portsc_is_usb2(5));
    TEST_ASSERT_TRUE (tegra_padctl_portsc_is_usb2(6));
    TEST_ASSERT_TRUE (tegra_padctl_portsc_is_usb2(7));
    TEST_ASSERT_FALSE(tegra_padctl_portsc_is_usb2(8));
    TEST_ASSERT_FALSE(tegra_padctl_portsc_is_usb2(255));
}

static void test_portsc_to_pad(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, tegra_padctl_portsc_to_pad(4));
    TEST_ASSERT_EQUAL_UINT32(1U, tegra_padctl_portsc_to_pad(5));
    TEST_ASSERT_EQUAL_UINT32(2U, tegra_padctl_portsc_to_pad(6));
    TEST_ASSERT_EQUAL_UINT32(3U, tegra_padctl_portsc_to_pad(7));
}

static void test_num_usb2_pads(void)
{
    /* Tegra234 has 4 USB2 pads (inherited from Tegra186). */
    TEST_ASSERT_EQUAL_UINT32(4U, TEGRA_XUSB_NUM_USB2_PADS);
    TEST_ASSERT_EQUAL_UINT32(4U, TEGRA_XUSB_USB2_PORTSC_BASE);
}

/* -------------------------------------------------------------------------- */
/* Suite entry                                                                 */
/* -------------------------------------------------------------------------- */

int test_suite_tegra_padctl(void);

int test_suite_tegra_padctl(void)
{
    UnityBegin("test_tegra_padctl.c");
    RUN_TEST(test_usb2_otg_pad_ctl0_pad0);
    RUN_TEST(test_usb2_otg_pad_ctl0_stride);
    RUN_TEST(test_usb2_otg_pad_ctl1_pad0);
    RUN_TEST(test_usb2_otg_pad_ctl1_stride);
    RUN_TEST(test_ctl0_ctl1_adjacent);
    RUN_TEST(test_bias_pad_offsets);
    RUN_TEST(test_power_down_bits);
    RUN_TEST(test_portsc_usb2_range);
    RUN_TEST(test_portsc_to_pad);
    RUN_TEST(test_num_usb2_pads);
    return UnityEnd();
}
