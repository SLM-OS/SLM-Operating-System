/*
 * test_bcm_mailbox.c — BCM property-channel buffer construction tests.
 *
 * Covers the build-side of the dynamic-kernel-replace mailbox helpers
 * (#367). The MMIO transport in `bcm_mailbox.c` is `PLATFORM_RASPI5`-
 * gated and not exercisable in QEMU virt; these tests verify the
 * wire-level tag layout that bcm_mailbox.c hands to the VideoCore
 * firmware via the property channel. The proto header is platform-
 * neutral, so the buffer-construction logic is reachable on every
 * test platform.
 */

#include "unity.h"
#include "../include/bcm_mailbox_proto.h"

#include <stdint.h>
#include <string.h>

/* The transport requires a 16-byte-aligned 8-word buffer; mirror
 * that constraint in the tests. */
typedef uint32_t prop_buf_t[BCM_PROP_BUF_WORDS] __attribute__((aligned(16)));

static void fill_sentinel(prop_buf_t buf)
{
    /* Non-zero sentinel so a "forgot to write a word" bug shows up as
     * a non-zero value rather than coincidentally matching the
     * expected zero. */
    memset(buf, 0xAA, sizeof(prop_buf_t));
}

static void test_set_reboot_flags_layout_with_flag_set(void)
{
    prop_buf_t buf;
    fill_sentinel(buf);

    bcm_mailbox_build_set_reboot_flags(buf, 1u);

    TEST_ASSERT_EQUAL_HEX32(32u,                       buf[0]);  /* total_size */
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_REQUEST,          buf[1]);  /* request */
    TEST_ASSERT_EQUAL_HEX32(BCM_TAG_SET_REBOOT_FLAGS,  buf[2]);  /* tag id */
    TEST_ASSERT_EQUAL_HEX32(4u,                        buf[3]);  /* val_buf_sz */
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[4]);  /* tag_code */
    TEST_ASSERT_EQUAL_HEX32(1u,                        buf[5]);  /* payload */
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_TAG_END,          buf[6]);  /* end_tag */
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[7]);  /* tail pad */
}

static void test_set_reboot_flags_layout_with_flag_zero(void)
{
    prop_buf_t buf;
    fill_sentinel(buf);

    bcm_mailbox_build_set_reboot_flags(buf, 0u);

    /* Same layout as the flag-set case, just `flags` payload differs.
     * Check every word — `kernel rollback` clears the tryboot flag if
     * it was set, so this is a real call site, and a regression that
     * accidentally splits the helper into two code paths needs to
     * fail loudly. */
    TEST_ASSERT_EQUAL_HEX32(32u,                       buf[0]);
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_REQUEST,          buf[1]);
    TEST_ASSERT_EQUAL_HEX32(BCM_TAG_SET_REBOOT_FLAGS,  buf[2]);
    TEST_ASSERT_EQUAL_HEX32(4u,                        buf[3]);
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[4]);
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[5]);
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_TAG_END,          buf[6]);
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[7]);
}

static void test_set_reboot_flags_preserves_high_bits(void)
{
    /* The flags word is opaque to SLM-OS — only bit 0 is documented
     * (tryboot), but bits 1+ are reserved for future firmware
     * features. Don't mask or sanitize them in the helper. */
    prop_buf_t buf;
    bcm_mailbox_build_set_reboot_flags(buf, 0xDEADBEEFu);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, buf[5]);
}

static void test_notify_reboot_layout(void)
{
    prop_buf_t buf;
    fill_sentinel(buf);

    bcm_mailbox_build_notify_reboot(buf);

    TEST_ASSERT_EQUAL_HEX32(32u,                       buf[0]);  /* total_size */
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_REQUEST,          buf[1]);
    TEST_ASSERT_EQUAL_HEX32(BCM_TAG_NOTIFY_REBOOT,     buf[2]);
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[3]);  /* empty payload */
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[4]);
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_TAG_END,          buf[5]);  /* end_tag */
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[6]);  /* tail pad */
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[7]);  /* tail pad */
}

static void test_helpers_are_idempotent(void)
{
    /* Calling twice with the same args produces an identical buffer.
     * The transport reuses the same static buffer across calls, so
     * the helpers must always overwrite every word — never leave a
     * stale value from a previous tag. */
    prop_buf_t a, b;

    bcm_mailbox_build_set_reboot_flags(a, 1u);
    bcm_mailbox_build_set_reboot_flags(b, 1u);
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(prop_buf_t));

    bcm_mailbox_build_notify_reboot(a);
    bcm_mailbox_build_notify_reboot(b);
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(prop_buf_t));
}

static void test_helpers_overwrite_stale_buffer(void)
{
    /* Prove that the build helpers don't leak data from a prior call:
     * stage SET_REBOOT_FLAGS into a buffer, then call
     * build_notify_reboot on the same buffer and verify nothing from
     * the SET_REBOOT_FLAGS layout (notably the payload at [5]) is
     * still visible. */
    prop_buf_t buf;

    bcm_mailbox_build_set_reboot_flags(buf, 0xDEADBEEFu);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, buf[5]);

    bcm_mailbox_build_notify_reboot(buf);
    TEST_ASSERT_EQUAL_HEX32(BCM_TAG_NOTIFY_REBOOT, buf[2]);
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_TAG_END,      buf[5]);   /* not 0xDEADBEEF */
}

static void test_tag_ids_match_pi_firmware_subset(void)
{
    /* Pinned to include/soc/bcm2835/raspberrypi-firmware.h
     * (raspberrypi/linux rpi-6.12.y). If these change in a future
     * firmware release the values are still load-bearing — the Pi 5
     * EEPROM mailbox subset is the source of truth. Catching a typo
     * here is much cheaper than chasing a "tryboot did nothing" bug
     * on hardware. */
    TEST_ASSERT_EQUAL_HEX32(0x00038064u, BCM_TAG_SET_REBOOT_FLAGS);
    TEST_ASSERT_EQUAL_HEX32(0x00030048u, BCM_TAG_NOTIFY_REBOOT);
    TEST_ASSERT_EQUAL_HEX32(0x00028001u, BCM_TAG_SET_POWER_STATE);
}

static void test_set_power_state_layout_on_with_wait(void)
{
    uint32_t buf[BCM_PROP_BUF_WORDS] = {0};
    uint32_t state = BCM_POWER_STATE_ON | BCM_POWER_STATE_WAIT;

    bcm_mailbox_build_set_power_state(buf, BCM_POWER_DEVICE_SDCARD, state);

    TEST_ASSERT_EQUAL_HEX32(32u,                       buf[0]);
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_REQUEST,          buf[1]);
    TEST_ASSERT_EQUAL_HEX32(BCM_TAG_SET_POWER_STATE,   buf[2]);
    TEST_ASSERT_EQUAL_HEX32(8u,                        buf[3]);  /* val_buf_sz */
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[4]);
    TEST_ASSERT_EQUAL_HEX32(BCM_POWER_DEVICE_SDCARD,   buf[5]);
    TEST_ASSERT_EQUAL_HEX32(0x3u,                      buf[6]);  /* on | wait */
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_TAG_END,          buf[7]);
}

static void test_set_power_state_layout_off_no_wait(void)
{
    uint32_t buf[BCM_PROP_BUF_WORDS] = {0};

    bcm_mailbox_build_set_power_state(buf, BCM_POWER_DEVICE_SDCARD,
                                      BCM_POWER_STATE_OFF);

    TEST_ASSERT_EQUAL_HEX32(BCM_TAG_SET_POWER_STATE,   buf[2]);
    TEST_ASSERT_EQUAL_HEX32(BCM_POWER_DEVICE_SDCARD,   buf[5]);
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[6]);  /* off */
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_TAG_END,          buf[7]);
}

static void test_set_power_state_state_bits_pinned(void)
{
    /* The state-word bit assignments are part of the Pi mailbox
     * wire format, not arbitrary. Pin them so a typo in the header
     * is caught at build time on every host, not on the next Pi 5
     * boot attempt. */
    TEST_ASSERT_EQUAL_HEX32(0u, BCM_POWER_STATE_OFF);
    TEST_ASSERT_EQUAL_HEX32(1u, BCM_POWER_STATE_ON);
    TEST_ASSERT_EQUAL_HEX32(2u, BCM_POWER_STATE_WAIT);
    TEST_ASSERT_EQUAL_HEX32(0u, BCM_POWER_DEVICE_SDCARD);
}

static void test_set_clock_state_layout_emmc2_on(void)
{
    uint32_t buf[BCM_PROP_BUF_WORDS] = {0};

    bcm_mailbox_build_set_clock_state(buf, BCM_CLOCK_EMMC2,
                                      BCM_CLOCK_STATE_ON);

    TEST_ASSERT_EQUAL_HEX32(32u,                       buf[0]);
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_REQUEST,          buf[1]);
    TEST_ASSERT_EQUAL_HEX32(BCM_TAG_SET_CLOCK_STATE,   buf[2]);
    TEST_ASSERT_EQUAL_HEX32(8u,                        buf[3]);  /* val_buf_sz */
    TEST_ASSERT_EQUAL_HEX32(0u,                        buf[4]);
    TEST_ASSERT_EQUAL_HEX32(BCM_CLOCK_EMMC2,           buf[5]);  /* id 12 */
    TEST_ASSERT_EQUAL_HEX32(BCM_CLOCK_STATE_ON,        buf[6]);
    TEST_ASSERT_EQUAL_HEX32(BCM_PROP_TAG_END,          buf[7]);
}

static void test_set_clock_state_clock_ids_pinned(void)
{
    /* Pin the firmware clock-id values to the canonical
     * raspberrypi-firmware.h table. EMMC2 = 12 is the load-bearing
     * one for #414; if any future maintainer changes the constant
     * the next Pi 5 boot will hang `kernel status` again. */
    TEST_ASSERT_EQUAL_HEX32(1u,  BCM_CLOCK_EMMC);
    TEST_ASSERT_EQUAL_HEX32(12u, BCM_CLOCK_EMMC2);
    TEST_ASSERT_EQUAL_HEX32(0x00038001u, BCM_TAG_SET_CLOCK_STATE);
    /* Same state-word bit semantics as SET_POWER_STATE. */
    TEST_ASSERT_EQUAL_HEX32(0u, BCM_CLOCK_STATE_OFF);
    TEST_ASSERT_EQUAL_HEX32(1u, BCM_CLOCK_STATE_ON);
    TEST_ASSERT_EQUAL_HEX32(2u, BCM_CLOCK_STATE_NO_DEVICE);
}

int test_suite_bcm_mailbox(void)
{
    UnityBegin("BCM Mailbox property-tag buffer tests");

    RUN_TEST(test_set_reboot_flags_layout_with_flag_set);
    RUN_TEST(test_set_reboot_flags_layout_with_flag_zero);
    RUN_TEST(test_set_reboot_flags_preserves_high_bits);
    RUN_TEST(test_notify_reboot_layout);
    RUN_TEST(test_helpers_are_idempotent);
    RUN_TEST(test_helpers_overwrite_stale_buffer);
    RUN_TEST(test_tag_ids_match_pi_firmware_subset);
    RUN_TEST(test_set_power_state_layout_on_with_wait);
    RUN_TEST(test_set_power_state_layout_off_no_wait);
    RUN_TEST(test_set_power_state_state_bits_pinned);
    RUN_TEST(test_set_clock_state_layout_emmc2_on);
    RUN_TEST(test_set_clock_state_clock_ids_pinned);

    return UnityEnd();
}
