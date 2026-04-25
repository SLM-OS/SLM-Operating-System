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

#endif /* BCM_MAILBOX_PROTO_H */
