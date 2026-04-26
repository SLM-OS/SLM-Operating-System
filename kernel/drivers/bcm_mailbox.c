/*
 * bcm_mailbox.c — BCM2712 VideoCore property-channel mailbox (Pi 5).
 *
 * Single shared transport (`mbox_property_call`) reused by every tag
 * helper. Same protocol as earlier Pi SoCs — only the MMIO base
 * moves (0x107C013880 on BCM2712). Reachable from EL1 with no RP1
 * indirection.
 *
 * Tag helpers exposed today:
 *   - bcm_mailbox_get_board_mac      (tag 0x00010003) — fetch the
 *     board's factory MAC, consumed by macb_program_mac_address.
 *   - bcm_mailbox_set_reboot_flags   (tag 0x00038064) — bit 0 arms
 *     the firmware tryboot flag on the next boot. Used by the
 *     dynamic-kernel-replace `kernel activate` command (#367).
 *   - bcm_mailbox_notify_reboot      (tag 0x00030048) — empty
 *     payload; tells firmware a reboot is intentional and triggers
 *     the firmware-side restart sequence.
 *
 * Buffer construction for the reboot tags lives in
 * kernel/include/bcm_mailbox_proto.h as platform-neutral pure-inline
 * helpers, so the wire-level tag layout is unit-tested in QEMU virt
 * (kernel/tests/test_bcm_mailbox.c). The MMIO transport here is
 * `PLATFORM_RASPI5`-only — hardware verification of the round-trip
 * is Stage 5 of the dynamic-kernel-replace plan (#371).
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
 * Property buffer layout (single tag, 32 bytes, 16-byte aligned):
 *
 *   word 0  total_size       (= 32, the buffer size)
 *   word 1  request/response (0 on send; 0x80000000 = OK on receive)
 *   word 2  tag_id           (e.g. 0x00010003 for GET_BOARD_MAC)
 *   word 3  val_buf_sz       (size of the tag's payload in bytes)
 *   word 4  tag_code         (0 on send; bit 31 + length on receive)
 *   word 5+ tag payload (size = val_buf_sz, padded up to a word)
 *   ...     end_tag (= 0) immediately after the payload
 *   ...     tail pad (zero) up to total_size
 */

#include "platform.h"

#if defined(PLATFORM_RASPI5)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "bcm_mailbox.h"
#include "bcm_mailbox_proto.h" /* pure buffer-build helpers (also unit-tested) */
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

/* Fixed buffer size shared across every tag we send. 12 words = 48
 * bytes is the smallest 16-byte-aligned size that fits the largest
 * single-tag payload we issue (SET_CLOCK_RATE: 12-byte payload plus
 * end-tag word). 16-byte aligned so the upper-28-bit bus-address
 * encoding is clean.
 *
 * Helpers that send tags with smaller payloads still declare
 * `total_size = 32` in `prop_buf[0]` — VC firmware reads only up to
 * `total_size` bytes, so the tail of the larger physical buffer is
 * never visible to the firmware on those calls. */
#define PROP_BUF_WORDS          12
#define PROP_BUF_BYTES          (PROP_BUF_WORDS * 4)

/* Compile-time check that this driver and the proto header agree
 * on buffer size. If someone shrinks `prop_buf` without revisiting
 * the helpers in bcm_mailbox_proto.h, the build fails here instead
 * of VideoCore silently rejecting a malformed buffer at runtime. */
_Static_assert(BCM_PROP_BUF_WORDS == PROP_BUF_WORDS,
               "bcm_mailbox_proto.h and bcm_mailbox.c disagree on buffer size");

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
    /* Populate the property buffer with a single GET_BOARD_MAC tag.
     * total_size is fixed at 32 (header + 8-byte payload + end_tag);
     * the physical buffer can be larger but firmware reads only up
     * to `total_size` bytes. */
    prop_buf[0] = 32u;                      /* total_size */
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

int bcm_mailbox_set_reboot_flags(uint32_t flags)
{
    bcm_mailbox_build_set_reboot_flags(prop_buf, flags);

    int rc = mbox_property_call();
    if (rc < 0) {
        return rc;
    }

    /* Tag-level response check: bit 31 set means VC processed the tag
     * successfully. The length field doesn't matter for a write tag,
     * but bit 31 is the load-bearing bit. */
    uint32_t tag_resp = prop_buf[4];
    if (!(tag_resp & PROP_TAG_RESP_SUCCESS)) {
        ERROR("mailbox: SET_REBOOT_FLAGS tag response not success (0x%08x)",
              tag_resp);
        return MBOX_E_GENERIC;
    }
    return 0;
}

int bcm_mailbox_notify_reboot(void)
{
    bcm_mailbox_build_notify_reboot(prop_buf);

    int rc = mbox_property_call();
    if (rc < 0) {
        return rc;
    }

    uint32_t tag_resp = prop_buf[4];
    if (!(tag_resp & PROP_TAG_RESP_SUCCESS)) {
        ERROR("mailbox: NOTIFY_REBOOT tag response not success (0x%08x)",
              tag_resp);
        return MBOX_E_GENERIC;
    }
    return 0;
}

int bcm_mailbox_set_power_state(uint32_t device_id, bool on, bool wait)
{
    uint32_t state = (on ? BCM_POWER_STATE_ON : BCM_POWER_STATE_OFF)
                   | (wait ? BCM_POWER_STATE_WAIT : 0u);

    bcm_mailbox_build_set_power_state(prop_buf, device_id, state);

    int rc = mbox_property_call();
    if (rc < 0) {
        return rc;
    }

    uint32_t tag_resp = prop_buf[4];
    if (!(tag_resp & PROP_TAG_RESP_SUCCESS)) {
        ERROR("mailbox: SET_POWER_STATE tag response not success (0x%08x)",
              tag_resp);
        return MBOX_E_GENERIC;
    }

    /* Firmware returns the actual device state in word 6. With WAIT
     * set, this should match the requested on/off bit. Treat a
     * mismatch as failure — the device is not in the state we asked
     * for, so subsequent MMIO will likely hang. */
    uint32_t actual_state = prop_buf[6];
    if (((actual_state & BCM_POWER_STATE_ON) != 0) != on) {
        ERROR("mailbox: SET_POWER_STATE(dev=%u, on=%d) returned "
              "actual_state=0x%08x — device did not transition",
              device_id, (int)on, actual_state);
        return MBOX_E_GENERIC;
    }
    return 0;
}

int bcm_mailbox_set_clock_state(uint32_t clock_id, bool on)
{
    uint32_t state = on ? BCM_CLOCK_STATE_ON : BCM_CLOCK_STATE_OFF;

    bcm_mailbox_build_set_clock_state(prop_buf, clock_id, state);

    int rc = mbox_property_call();
    if (rc < 0) {
        return rc;
    }

    uint32_t tag_resp = prop_buf[4];
    if (!(tag_resp & PROP_TAG_RESP_SUCCESS)) {
        ERROR("mailbox: SET_CLOCK_STATE tag response not success (0x%08x)",
              tag_resp);
        return MBOX_E_GENERIC;
    }

    /* Firmware reports actual state in word 6 with the same bit-0
     * convention as the request, plus bit 1 = "no such clock id".
     * Either condition is a hard failure for our caller — without
     * the clock running, subsequent MMIO to the controller will
     * hang the AXI fabric (#414 root cause). */
    uint32_t actual_state = prop_buf[6];
    if (actual_state & BCM_CLOCK_STATE_NO_DEVICE) {
        ERROR("mailbox: SET_CLOCK_STATE(clk=%u) — firmware reports "
              "no such clock (actual_state=0x%08x)",
              clock_id, actual_state);
        return MBOX_E_GENERIC;
    }
    if (((actual_state & BCM_CLOCK_STATE_ON) != 0) != on) {
        ERROR("mailbox: SET_CLOCK_STATE(clk=%u, on=%d) returned "
              "actual_state=0x%08x — clock did not transition",
              clock_id, (int)on, actual_state);
        return MBOX_E_GENERIC;
    }
    return 0;
}

int bcm_mailbox_get_clock_state(uint32_t clock_id, uint32_t *state_out)
{
    bcm_mailbox_build_get_clock_state(prop_buf, clock_id);
    int rc = mbox_property_call();
    if (rc < 0) return rc;
    if (!(prop_buf[4] & PROP_TAG_RESP_SUCCESS)) return MBOX_E_GENERIC;
    if (state_out) *state_out = prop_buf[6];
    return 0;
}

int bcm_mailbox_get_clock_rate(uint32_t clock_id, uint32_t *hz_out)
{
    bcm_mailbox_build_get_clock_rate(prop_buf, clock_id);
    int rc = mbox_property_call();
    if (rc < 0) return rc;
    if (!(prop_buf[4] & PROP_TAG_RESP_SUCCESS)) return MBOX_E_GENERIC;
    if (hz_out) *hz_out = prop_buf[6];
    return 0;
}

int bcm_mailbox_get_clock_rate_measured(uint32_t clock_id, uint32_t *hz_out)
{
    bcm_mailbox_build_get_clock_rate_measured(prop_buf, clock_id);
    int rc = mbox_property_call();
    if (rc < 0) return rc;
    if (!(prop_buf[4] & PROP_TAG_RESP_SUCCESS)) return MBOX_E_GENERIC;
    if (hz_out) *hz_out = prop_buf[6];
    return 0;
}

int bcm_mailbox_set_clock_rate(uint32_t clock_id, uint32_t requested_hz,
                               uint32_t *actual_hz)
{
    bcm_mailbox_build_set_clock_rate(prop_buf, clock_id, requested_hz,
                                     /*skip_setting_turbo=*/0);

    int rc = mbox_property_call();
    if (rc < 0) {
        return rc;
    }

    uint32_t tag_resp = prop_buf[4];
    if (!(tag_resp & PROP_TAG_RESP_SUCCESS)) {
        ERROR("mailbox: SET_CLOCK_RATE tag response not success (0x%08x)",
              tag_resp);
        return MBOX_E_GENERIC;
    }

    /* Response: word 5 = clock_id (echoed), word 6 = actual rate.
     * 0 means "no such clock" or "rate is fixed and could not be
     * changed" — caller can treat this as advisory. */
    uint32_t programmed = prop_buf[6];
    if (actual_hz != NULL) {
        *actual_hz = programmed;
    }
    return 0;
}

#endif /* PLATFORM_RASPI5 */
