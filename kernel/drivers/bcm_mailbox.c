/*
 * bcm_mailbox.c — BCM2712 VideoCore property-channel mailbox (Pi 5).
 *
 * Single-purpose today: fetch the board's factory MAC via tag
 * 0x00010003. The protocol is the same as earlier Pi SoCs — only
 * the MMIO base moves (0x107C013880 on BCM2712). Reachable from
 * EL1 with no RP1 indirection.
 *
 * References (cached under docs/reference/):
 *   - linux-bcm2835-mailbox.c      (register layout, status bits)
 *   - linux-rpi-firmware.c         (property-channel consumer)
 *   - linux-bcm2712.dtsi           (mailbox node + soc ranges)
 *   - uboot-bcm283x-mbox.c         (clean reference impl)
 *   - circle-bcmpropertytags.cpp   (tag layout + send sequence)
 *
 * Transport:
 *   MBOX1_WRITE (ARM→VC)  = MAILBOX_BASE + 0x20
 *   MBOX1_STATUS          = MAILBOX_BASE + 0x38  (FULL at bit 31)
 *   MBOX0_READ  (VC→ARM)  = MAILBOX_BASE + 0x00
 *   MBOX0_STATUS          = MAILBOX_BASE + 0x18  (EMPTY at bit 30)
 *
 * Word encoding: low 4 bits = channel (8 for property tags), upper
 * 28 bits = VC bus address of the property buffer, which is why the
 * buffer must be 16-byte aligned.
 *
 * Property buffer layout (GET_BOARD_MAC_ADDRESS):
 *
 *   offset  size  value
 *   0       u32   total_size = 32
 *   4       u32   request    = 0                 (VC writes 0x80000000)
 *   8       u32   tag_id     = 0x00010003
 *   12      u32   val_buf_sz = 8                 (6 MAC bytes, pad to 8)
 *   16      u32   tag_code   = 0                 (VC writes bit31|len=6)
 *   20      u8[8] MAC + 2 bytes pad
 *   28      u32   end_tag    = 0
 */

#include "platform.h"

#if defined(PLATFORM_RASPI5)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "bcm_mailbox.h"
#include "debug.h"
#include "timer.h"          /* timer_busy_wait_us, timer_get_count/_frequency */
#include "cache.h"          /* cache_clean_range / cache_invalidate_range */

/* ---- Mailbox registers ---- */
#define MBOX0_READ              0x00    /* VC → ARM */
#define MBOX0_STATUS            0x18
#define MBOX1_WRITE             0x20    /* ARM → VC */
#define MBOX1_STATUS            0x38

#define MBOX_STATUS_FULL        (1u << 31)
#define MBOX_STATUS_EMPTY       (1u << 30)

#define MBOX_CHANNEL_PROPERTY   8u
#define MBOX_CHANNEL_MASK       0xFu

/* ---- Property-tag protocol ---- */
#define PROP_REQUEST            0x00000000u
#define PROP_RESPONSE_SUCCESS   0x80000000u
#define PROP_TAG_END            0x00000000u
#define PROP_TAG_RESP_SUCCESS   0x80000000u
#define PROP_TAG_RESP_LEN_MASK  0x7FFFFFFFu

#define TAG_GET_BOARD_MAC       0x00010003u

/* MBOX_E_GENERIC / MBOX_E_TAG_UNSUPPORTED come from bcm_mailbox.h. */

/* Fixed buffer size for the one tag we handle. 16-byte aligned so
 * the upper-28-bit bus-address encoding is clean. */
#define PROP_BUF_WORDS          8
#define PROP_BUF_BYTES          (PROP_BUF_WORDS * 4)

/* Property buffer.
 *
 * Kept in cacheable BSS (low kernel memory) with explicit cache
 * maintenance around the call. The Pi 5 VC mailbox uses the legacy
 * 1 GB VC-bus alias at 0xC0000000, which means the buffer must sit
 * in PA 0..0x3FFFFFFF for the `phys | 0xC0000000` encoding to land
 * at the right place — we cannot use NC memory (Pi 5's pool is at
 * 0xFFE00000, well above the legacy alias window and therefore
 * invisible to VC).
 *
 * Alignment 16 bytes both because the property-channel spec requires
 * it and because the low 4 bits of the mailbox word carry the channel
 * number (so bus_addr must have them clear). */
static uint32_t prop_buf[PROP_BUF_WORDS] __attribute__((aligned(16)));

/* ---- MMIO helpers ---- */
static inline uint32_t mbox_readl(uint32_t reg)
{
    return *(volatile uint32_t *)(BCM_MAILBOX_BASE + reg);
}

static inline void mbox_writel(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(BCM_MAILBOX_BASE + reg) = val;
}

/* 100 ms wall-clock budget for any single mailbox operation (status
 * wait, full round-trip response). Centralised here so status waits
 * and the response loop can't drift apart. Chosen as "obviously
 * longer than any real VC response" — real round-trips are sub-
 * millisecond. */
#define MBOX_OP_BUDGET_US   (100 * 1000)

/* Spin until the status bit clears, with a wall-clock deadline so a
 * hung VC can't wedge the caller. Returns 0 on success, -1 on
 * timeout. Uses timer_busy_wait_us (CNTPCT + ARM `yield` hint — NOT
 * scheduler yield), safe to call while holding a spinlock or pre-
 * scheduler (same contract as the MACB TX polled path). */
static int mbox_wait_status(uint32_t status_reg, uint32_t mask,
                            uint32_t desired)
{
    const uint64_t freq     = timer_get_frequency();
    const uint64_t deadline = timer_get_count() +
                              (MBOX_OP_BUDGET_US * freq + 999999ULL) / 1000000ULL;

    while (timer_get_count() < deadline) {
        __asm__ volatile("dsb sy" ::: "memory");
        if ((mbox_readl(status_reg) & mask) == desired) {
            return 0;
        }
        timer_busy_wait_us(10);
    }
    return -1;
}

/* Issue one property call. Caller populates `prop_buf` with a valid
 * property-channel request (header + single tag + end_tag) already
 * in place, then calls this helper. On success, `prop_buf` holds the
 * response in the same buffer (request was 0x00000000, now contains
 * 0x80000000 or 0x80000001; tag response code has bit 31 set). */
static int mbox_property_call(void)
{
    /* Physical address of the buffer. Kernel BSS on Pi 5 lives in
     * the low 1 GB, so the legacy VC bus-address encoding applies
     * and the result fits in 32 bits (required — the mailbox word
     * is only 32 bits wide). Guarded anyway. */
    uintptr_t phys = (uintptr_t)prop_buf;
    if (phys >= 0x100000000ULL) {
        ERROR("mailbox: property buffer above 4 GB (%p)", (void *)phys);
        return -1;
    }
    if ((phys & 0xFu) != 0) {
        ERROR("mailbox: property buffer not 16-byte aligned (%p)",
              (void *)phys);
        return -1;
    }

    uint32_t bus = (uint32_t)BCM_BUS_ADDRESS(phys);

    /* Clean the buffer to DRAM so VC DMA sees the request. The
     * helper already emits `dsb sy` internally (see cache.h), so no
     * additional barrier is needed here. */
    cache_clean_range(prop_buf, PROP_BUF_BYTES);

    if (mbox_wait_status(MBOX1_STATUS, MBOX_STATUS_FULL, 0) < 0) {
        ERROR("mailbox: write path stayed FULL (VC wedged?)");
        return -1;
    }

    mbox_writel(MBOX1_WRITE, bus | MBOX_CHANNEL_PROPERTY);
    /* Explicit barrier here is meaningful: order the MMIO write
     * ahead of the status poll that follows. */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Wait for a response on channel 8. The mailbox is shared with
     * other channels; if we read a word from a different channel we
     * discard it and keep waiting. In practice SLM-OS has no other
     * mailbox user so this rarely matters — but it's the correct
     * thing to do per the property-channel protocol. */
    const uint64_t freq     = timer_get_frequency();
    const uint64_t deadline = timer_get_count() +
                              (MBOX_OP_BUDGET_US * freq + 999999ULL) / 1000000ULL;
    uint32_t response = 0;
    bool got = false;
    while (!got && timer_get_count() < deadline) {
        if (mbox_wait_status(MBOX0_STATUS, MBOX_STATUS_EMPTY, 0) < 0) {
            ERROR("mailbox: no response (status EMPTY throughout budget)");
            return -1;
        }
        uint32_t word = mbox_readl(MBOX0_READ);
        if ((word & MBOX_CHANNEL_MASK) == MBOX_CHANNEL_PROPERTY) {
            response = word;
            got = true;
        }
        /* else: not ours, drop and keep waiting */
    }
    if (!got) {
        ERROR("mailbox: timed out waiting for property-channel response");
        return -1;
    }

    /* The VC returns the same bus address we sent, in the top 28
     * bits; a different address would be a protocol violation. */
    if ((response & ~MBOX_CHANNEL_MASK) != (bus & ~MBOX_CHANNEL_MASK)) {
        ERROR("mailbox: response bus 0x%08x ≠ request 0x%08x",
              response, bus);
        return -1;
    }

    /* Invalidate cache so we see VC's writes to the buffer, not our
     * stale cached copy. (helper emits its own dsb sy) */
    cache_invalidate_range(prop_buf, PROP_BUF_BYTES);

    if (prop_buf[1] == 0x80000001) {
        /* Buffer-level parse error on a structurally-correct request
         * is how Pi 5 EEPROM firmware older than 2025-05-08 reports
         * "this tag isn't implemented on this firmware" (rpi-eeprom
         * issue #698: GET_BOARD_MAC_ADDRESS was added to the Pi 5
         * mailbox subset on 2025-05-08; prior EEPROM revisions don't
         * recognise the tag and fail at the buffer level). */
        return MBOX_E_TAG_UNSUPPORTED;
    }
    if (prop_buf[1] != PROP_RESPONSE_SUCCESS) {
        /* One bounded line — batching into a single ERROR keeps the
         * UART TX FIFO from overflowing on the error path (observed
         * during hardware testing). */
        ERROR("mailbox: unexpected response code 0x%08x; "
              "buf=[%08x %08x %08x %08x %08x %08x %08x %08x]",
              prop_buf[1],
              prop_buf[0], prop_buf[1], prop_buf[2], prop_buf[3],
              prop_buf[4], prop_buf[5], prop_buf[6], prop_buf[7]);
        return -1;
    }

    return 0;
}

int bcm_mailbox_get_board_mac(uint8_t mac[6])
{
    /* Populate the property buffer with a single GET_BOARD_MAC tag. */
    prop_buf[0] = PROP_BUF_BYTES;           /* total_size */
    prop_buf[1] = PROP_REQUEST;
    prop_buf[2] = TAG_GET_BOARD_MAC;
    prop_buf[3] = 8;                        /* tag value buffer size */
    prop_buf[4] = 0;                        /* tag req/resp code */
    prop_buf[5] = 0;                        /* tag data[0] — MAC[0..3] */
    prop_buf[6] = 0;                        /* tag data[1] — MAC[4..5]+pad */
    prop_buf[7] = PROP_TAG_END;

    int rc = mbox_property_call();
    if (rc == MBOX_E_TAG_UNSUPPORTED) {
        /* Silent — the caller will WARN with firmware-version context. */
        return MBOX_E_TAG_UNSUPPORTED;
    }
    if (rc < 0) {
        return rc;
    }

    uint32_t tag_resp = prop_buf[4];
    if (!(tag_resp & PROP_TAG_RESP_SUCCESS)) {
        ERROR("mailbox: GET_BOARD_MAC tag response not success (0x%08x)",
              tag_resp);
        return MBOX_E_GENERIC;
    }
    uint32_t resp_len = tag_resp & PROP_TAG_RESP_LEN_MASK;
    if (resp_len < 6) {
        ERROR("mailbox: GET_BOARD_MAC returned only %u bytes (need 6)",
              resp_len);
        return MBOX_E_GENERIC;
    }

    /* Copy out the 6 MAC bytes. Buffer layout: little-endian within
     * each word, so byte 0 of the MAC is at prop_buf[5] bits [7:0]. */
    mac[0] = (uint8_t)(prop_buf[5] >>  0);
    mac[1] = (uint8_t)(prop_buf[5] >>  8);
    mac[2] = (uint8_t)(prop_buf[5] >> 16);
    mac[3] = (uint8_t)(prop_buf[5] >> 24);
    mac[4] = (uint8_t)(prop_buf[6] >>  0);
    mac[5] = (uint8_t)(prop_buf[6] >>  8);

    /* Sanity check — an all-zero or all-0xFF MAC is almost certainly
     * a protocol error (VC should never assign those). */
    bool all_zero = true, all_ff = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) all_zero = false;
        if (mac[i] != 0xFF) all_ff   = false;
    }
    if (all_zero || all_ff) {
        ERROR("mailbox: GET_BOARD_MAC returned degenerate address %02x:%02x:%02x:%02x:%02x:%02x",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return MBOX_E_GENERIC;
    }

    return 0;
}

#endif /* PLATFORM_RASPI5 */
