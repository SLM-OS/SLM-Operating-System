/*
 * bcm_mailbox_proto.h — VideoCore property-channel buffer construction.
 *
 * Pure helpers that build property-channel request buffers for the
 * BCM mailbox driver. Separated from `bcm_mailbox.c` so the protocol
 * layout can be unit-tested on any platform: the MMIO transport in
 * bcm_mailbox.c is `PLATFORM_RASPI5`-gated, but the tag layout is
 * platform-neutral and worth covering with build-anywhere tests.
 *
 * Buffer shape (32 bytes, 16-byte aligned, single tag, padded tail):
 *
 *   word 0  total_size       (= 32, the buffer size)
 *   word 1  request/response (0 on send; 0x80000000 = OK on receive)
 *   word 2  tag_id
 *   word 3  val_buf_sz       (size of the tag's payload in bytes)
 *   word 4  tag_code         (0 on send; bit 31 + length on receive)
 *   word 5+ tag payload (size = val_buf_sz, padded up to a word)
 *   ...     end_tag (= 0) immediately after the payload
 *   ...     tail pad (zero) up to total_size
 *
 * Tags handled here cover the dynamic-kernel-replace plan (#367).
 * GET_BOARD_MAC remains in bcm_mailbox.c for now — its history of
 * MAC-validation logic isn't worth disturbing for the Stage 1 PR.
 */

#ifndef BCM_MAILBOX_PROTO_H
#define BCM_MAILBOX_PROTO_H

#include <stdint.h>

/* ---- Property-channel header / response codes (universal). ---- */
#define BCM_PROP_REQUEST             0x00000000u
#define BCM_PROP_RESP_SUCCESS        0x80000000u
#define BCM_PROP_RESP_PARSE_ERR      0x80000001u
#define BCM_PROP_TAG_END             0x00000000u
#define BCM_PROP_TAG_RESP_SUCCESS    0x80000000u
#define BCM_PROP_TAG_RESP_LEN_MASK   0x7FFFFFFFu

/* ---- Tag IDs for the dynamic-kernel-replace path. ---- */
/* Pinned to include/soc/bcm2835/raspberrypi-firmware.h
 * (raspberrypi/linux rpi-6.12.y). The Pi 5 firmware mailbox subset
 * is the source of truth — see docs/dynamic-kernel-replace-plan.md
 * Risk 2 for the trace from `reboot "0 tryboot"` to these tags. */
#define BCM_TAG_SET_REBOOT_FLAGS     0x00038064u
#define BCM_TAG_NOTIFY_REBOOT        0x00030048u
#define BCM_TAG_SET_POWER_STATE      0x00028001u
#define BCM_TAG_GET_CLOCK_STATE      0x00030001u
#define BCM_TAG_GET_CLOCK_RATE       0x00030002u
#define BCM_TAG_SET_CLOCK_STATE      0x00038001u
#define BCM_TAG_SET_CLOCK_RATE       0x00038002u
#define BCM_TAG_GET_CLOCK_RATE_MEASURED  0x00030047u

/* SET_POWER_STATE device IDs (subset — add more as needed). The
 * Pi firmware exposes power-domain control for these peripherals
 * via the mailbox interface. NOTE: device id 0 ("SD card") only
 * controls EMMC on Pi 1-3. On Pi 4/5, EMMC has moved to a separate
 * controller (EMMC2 on Pi 5) that is not in the SET_POWER_STATE
 * device-id table — see Linux's
 * `dt-bindings/power/raspberrypi-power.h` (the Pi 5 entries are
 * 0..22 with no SD/EMMC). For Pi 5 EMMC2, use SET_CLOCK_STATE with
 * BCM_CLOCK_EMMC2 (12) instead. */
#define BCM_POWER_DEVICE_SDCARD      0u

/* SET_POWER_STATE state-word bits. WAIT instructs the firmware to
 * block the response until the power-state transition has fully
 * completed; without it, the response can return before the
 * peripheral clocks are stable, which defeats the purpose of using
 * the mailbox to gate subsequent MMIO. */
#define BCM_POWER_STATE_OFF          0u
#define BCM_POWER_STATE_ON           (1u << 0)
#define BCM_POWER_STATE_WAIT         (1u << 1)

/* SET_CLOCK_STATE clock IDs. The Pi firmware's clock-id table is
 * authoritative — `include/soc/bcm2835/raspberrypi-firmware.h`
 * (rpi-6.12.y) and `drivers/clk/bcm/clk-raspberrypi.c` enumerate
 * 1..16 plus a few above. EMMC2 on Pi 5 / BCM2712 is id 12; this
 * matches Circle's `CLOCK_ID_EMMC2 = 12` in
 * `docs/reference/circle-bcmpropertytags.h`. Linux's sdhci-brcmstb
 * pulls EMMC2 up via `devm_clk_get_optional_enabled` which goes
 * through the firmware-clock framework and ultimately issues
 * SET_CLOCK_STATE(12, on) to the firmware. */
#define BCM_CLOCK_EMMC               1u
#define BCM_CLOCK_EMMC2              12u

/* SET_CLOCK_STATE state-word bits. Note bit-1's semantics are
 * direction-dependent:
 *   request:  bit 1 = WAIT (block until transition completes)
 *   response: bit 1 = "no such clock id"
 * Bit 0 is the on/off bit in both directions. The mailbox helpers
 * use BCM_POWER_STATE_WAIT in the request and BCM_CLOCK_STATE_NO_DEVICE
 * when reading the response — same numeric value, different intent. */
#define BCM_CLOCK_STATE_OFF          0u
#define BCM_CLOCK_STATE_ON           (1u << 0)
#define BCM_CLOCK_STATE_NO_DEVICE    (1u << 1)

/* Buffer size used by all helpers in this header. The 5-word header
 * (total_size, request, tag_id, val_buf_sz, tag_code) plus the
 * largest single-tag payload we issue (SET_CLOCK_RATE: 12 bytes =
 * 3 words) plus the trailing end_tag word adds up to 9 words; round
 * up to 12 (48 bytes) for 16-byte alignment of `total_size`, which
 * the BCM property-channel protocol requires. The matching static
 * `prop_buf` in bcm_mailbox.c is sized to BCM_PROP_BUF_WORDS as well. */
#define BCM_PROP_BUF_WORDS           12u

/* Helper for builders: zero the 4 tail words past the canonical
 * 32-byte buffer envelope. Smaller-payload tags only fill `buf[0..7]`;
 * this clears `buf[8..11]` so the resulting buffer is fully defined
 * and safe to compare byte-for-byte (`test_helpers_are_idempotent`
 * relies on this) and is also safe to send to firmware regardless of
 * what `total_size` declares — the firmware reads only up to that
 * many bytes, but defined zeros make any future "raise total_size"
 * change a one-line edit. */
static inline void bcm_mailbox_pad_tail(uint32_t buf[BCM_PROP_BUF_WORDS])
{
    buf[8]  = 0u;
    buf[9]  = 0u;
    buf[10] = 0u;
    buf[11] = 0u;
}

/*
 * SET_REBOOT_FLAGS (tag 0x00038064): set the firmware reboot-flags
 * register. Bit 0 is the tryboot flag, consumed by the bootloader on
 * the next boot.
 *
 * Layout:
 *   [0] total_size  = 32
 *   [1] request     = 0
 *   [2] tag_id      = 0x00038064
 *   [3] val_buf_sz  = 4
 *   [4] tag_code    = 0
 *   [5] flags       = `flags` (1 = arm tryboot, 0 = clear)
 *   [6] end_tag     = 0
 *   [7] tail pad    = 0
 *
 * `buf` must point at 8 contiguous u32s, 16-byte aligned in caller
 * memory. Helper writes all 8 words (no partial writes — safe to
 * call repeatedly with the same buffer).
 */
static inline void bcm_mailbox_build_set_reboot_flags(uint32_t buf[BCM_PROP_BUF_WORDS],
                                                      uint32_t flags)
{
    buf[0] = 32u;
    buf[1] = BCM_PROP_REQUEST;
    buf[2] = BCM_TAG_SET_REBOOT_FLAGS;
    buf[3] = 4u;
    buf[4] = 0u;
    buf[5] = flags;
    buf[6] = BCM_PROP_TAG_END;
    buf[7] = 0u;
    bcm_mailbox_pad_tail(buf);
}

/*
 * NOTIFY_REBOOT (tag 0x00030048): tell the firmware that a reboot is
 * intentional. The Pi 5 firmware uses this as the signal to run its
 * restart sequence after SET_REBOOT_FLAGS has armed any flags.
 * Empty payload.
 *
 * Layout:
 *   [0] total_size  = 32
 *   [1] request     = 0
 *   [2] tag_id      = 0x00030048
 *   [3] val_buf_sz  = 0
 *   [4] tag_code    = 0
 *   [5] end_tag     = 0
 *   [6] tail pad    = 0
 *   [7] tail pad    = 0
 */
static inline void bcm_mailbox_build_notify_reboot(uint32_t buf[BCM_PROP_BUF_WORDS])
{
    buf[0] = 32u;
    buf[1] = BCM_PROP_REQUEST;
    buf[2] = BCM_TAG_NOTIFY_REBOOT;
    buf[3] = 0u;
    buf[4] = 0u;
    buf[5] = BCM_PROP_TAG_END;
    buf[6] = 0u;
    buf[7] = 0u;
    bcm_mailbox_pad_tail(buf);
}

/*
 * SET_POWER_STATE (tag 0x00028001): turn a device's power domain on
 * or off via the firmware. Used by the BCM2712 SDHCI driver to gate
 * EMMC2's clock + power before the first register touch — issue #414
 * documents the failure mode (one peek to the EMMC2 base hangs the
 * AXI fabric until the firmware has powered the controller up).
 *
 * Layout:
 *   [0] total_size  = 32
 *   [1] request     = 0
 *   [2] tag_id      = 0x00028001
 *   [3] val_buf_sz  = 8           (two u32 payload words)
 *   [4] tag_code    = 0
 *   [5] device_id   = `device_id` (0 = SD card / EMMC2)
 *   [6] state       = `state`     (bit 0: on/off, bit 1: wait)
 *   [7] end_tag     = 0
 *
 * The 8-word buffer is the maximum the shared transport supports; the
 * end-tag falls in the last word with no tail pad. Caller composes
 * `state` from BCM_POWER_STATE_ON | BCM_POWER_STATE_WAIT to get a
 * synchronous power-on.
 */
static inline void bcm_mailbox_build_set_power_state(uint32_t buf[BCM_PROP_BUF_WORDS],
                                                     uint32_t device_id,
                                                     uint32_t state)
{
    buf[0] = 32u;
    buf[1] = BCM_PROP_REQUEST;
    buf[2] = BCM_TAG_SET_POWER_STATE;
    buf[3] = 8u;
    buf[4] = 0u;
    buf[5] = device_id;
    buf[6] = state;
    buf[7] = BCM_PROP_TAG_END;
    bcm_mailbox_pad_tail(buf);
}

/*
 * SET_CLOCK_STATE (tag 0x00038001): turn a firmware-managed clock on
 * or off. Used by the BCM2712 SDHCI driver to bring up EMMC2's
 * clock domain — the right knob on Pi 5 (SET_POWER_STATE has no SD
 * entry on Pi 5).
 *
 * Layout: identical to SET_POWER_STATE (two u32 payload words).
 *   [5] clock_id   (e.g. BCM_CLOCK_EMMC2 = 12)
 *   [6] state      (bit 0: on/off; response bit 1 set = no device)
 *
 * The response (after `mbox_property_call` returns) writes the
 * actual clock state back into [6]; check bit 0 to confirm the
 * transition.
 */
static inline void bcm_mailbox_build_set_clock_state(uint32_t buf[BCM_PROP_BUF_WORDS],
                                                     uint32_t clock_id,
                                                     uint32_t state)
{
    buf[0] = 32u;
    buf[1] = BCM_PROP_REQUEST;
    buf[2] = BCM_TAG_SET_CLOCK_STATE;
    buf[3] = 8u;
    buf[4] = 0u;
    buf[5] = clock_id;
    buf[6] = state;
    buf[7] = BCM_PROP_TAG_END;
    bcm_mailbox_pad_tail(buf);
}

/*
 * SET_CLOCK_RATE (tag 0x00038002): set the firmware-managed clock
 * frequency. 12-byte payload: clock_id, rate (Hz), skip_setting_turbo.
 *
 * Linux's brcmstb sdhci driver pulls clock-frequency from device-tree
 * (or, for the bcm2712 fixed-clock, gets 200 MHz back from
 * `clk_get_rate`) and sends it to the controller via this tag. On
 * Pi 5 EMMC2, 200 MHz is the canonical value (matches the
 * `bcm2712.dtsi` `clk_emmc2: clock-frequency = <200000000>` entry).
 *
 * Buffer layout (12 words = 48 bytes total, all firmware-visible):
 *   [0] total_size  = 48
 *   [1] request     = 0
 *   [2] tag_id      = 0x00038002
 *   [3] val_buf_sz  = 12  (3 payload words)
 *   [4] tag_code    = 0
 *   [5] clock_id
 *   [6] rate (Hz)
 *   [7] skip_setting_turbo  (0 = honor turbo policy)
 *   [8] end_tag     = 0
 *   [9..11] tail pad = 0
 *
 * The 12-word `BCM_PROP_BUF_WORDS` envelope is exactly the size the
 * largest single-tag payload (this one) needs; smaller-payload tags
 * still declare `total_size = 32` and only fill the first 8 words.
 */
/* Query helpers — read-only; same payload shape as the SET versions. */
static inline void bcm_mailbox_build_get_clock_state(uint32_t buf[BCM_PROP_BUF_WORDS],
                                                     uint32_t clock_id)
{
    buf[0] = 32u; buf[1] = BCM_PROP_REQUEST; buf[2] = BCM_TAG_GET_CLOCK_STATE;
    buf[3] = 8u; buf[4] = 0u; buf[5] = clock_id; buf[6] = 0u;
    buf[7] = BCM_PROP_TAG_END;
    bcm_mailbox_pad_tail(buf);
}
static inline void bcm_mailbox_build_get_clock_rate(uint32_t buf[BCM_PROP_BUF_WORDS],
                                                    uint32_t clock_id)
{
    buf[0] = 32u; buf[1] = BCM_PROP_REQUEST; buf[2] = BCM_TAG_GET_CLOCK_RATE;
    buf[3] = 8u; buf[4] = 0u; buf[5] = clock_id; buf[6] = 0u;
    buf[7] = BCM_PROP_TAG_END;
    bcm_mailbox_pad_tail(buf);
}
static inline void bcm_mailbox_build_get_clock_rate_measured(uint32_t buf[BCM_PROP_BUF_WORDS],
                                                              uint32_t clock_id)
{
    buf[0] = 32u; buf[1] = BCM_PROP_REQUEST; buf[2] = BCM_TAG_GET_CLOCK_RATE_MEASURED;
    buf[3] = 8u; buf[4] = 0u; buf[5] = clock_id; buf[6] = 0u;
    buf[7] = BCM_PROP_TAG_END;
    bcm_mailbox_pad_tail(buf);
}

static inline void bcm_mailbox_build_set_clock_rate(uint32_t buf[BCM_PROP_BUF_WORDS],
                                                    uint32_t clock_id,
                                                    uint32_t rate_hz,
                                                    uint32_t skip_setting_turbo)
{
    buf[0]  = 48u;
    buf[1]  = BCM_PROP_REQUEST;
    buf[2]  = BCM_TAG_SET_CLOCK_RATE;
    buf[3]  = 12u;
    buf[4]  = 0u;
    buf[5]  = clock_id;
    buf[6]  = rate_hz;
    buf[7]  = skip_setting_turbo;
    buf[8]  = BCM_PROP_TAG_END;
    buf[9]  = 0u;
    buf[10] = 0u;
    buf[11] = 0u;
}

#endif /* BCM_MAILBOX_PROTO_H */
