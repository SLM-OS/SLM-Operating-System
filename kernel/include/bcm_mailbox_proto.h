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

/* SET_POWER_STATE device IDs (subset — add more as needed). The
 * Pi firmware exposes power-domain control for these peripherals
 * via the mailbox interface; on Pi 5 / BCM2712 the SD card domain
 * (id 0) controls EMMC2's clock + power. The mapping is the same
 * across Pi generations — it's the firmware-side abstraction, not
 * a SoC-specific register layout. */
#define BCM_POWER_DEVICE_SDCARD      0u

/* SET_POWER_STATE state-word bits. WAIT instructs the firmware to
 * block the response until the power-state transition has fully
 * completed; without it, the response can return before the
 * peripheral clocks are stable, which defeats the purpose of using
 * the mailbox to gate subsequent MMIO. */
#define BCM_POWER_STATE_OFF          0u
#define BCM_POWER_STATE_ON           (1u << 0)
#define BCM_POWER_STATE_WAIT         (1u << 1)

/* Buffer size used by all helpers in this header. The transport in
 * bcm_mailbox.c uses a fixed 32-byte property buffer, so the helpers
 * size their content to fit within that envelope (with end_tag
 * placed correctly and any unused tail words zeroed). */
#define BCM_PROP_BUF_WORDS           8u

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
}

#endif /* BCM_MAILBOX_PROTO_H */
