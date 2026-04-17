/*
 * bcm_mailbox.h — BCM2712 VideoCore property-channel mailbox.
 *
 * Minimal driver: issues a single property tag to the VC firmware
 * over mailbox channel 8 and returns the tag's response payload.
 * Today the only consumer is `macb_program_mac_address`, which uses
 * tag 0x00010003 (GET_BOARD_MAC_ADDRESS) to pull the factory-assigned
 * MAC out of VC OTP — the same address the Pi 5 firmware gives Linux
 * via DT's `local-mac-address` property.
 *
 * Not a full property-channel library — no tag batching, no generic
 * argument struct. Extending to more tags (GET_BOARD_SERIAL, clock
 * queries, etc.) means adding a second helper next to `_get_board_mac`.
 *
 * Only available on PLATFORM_RASPI5 — the mailbox MMIO block is
 * BCM2712-specific (at 0x107C013880) and mapped by vmm_setup_platform.
 */

#ifndef BCM_MAILBOX_H
#define BCM_MAILBOX_H

#include <stdint.h>

#if defined(PLATFORM_RASPI5)

/* Return values for bcm_mailbox_get_board_mac (and mbox_property_call
 * internally). Negative = failure, zero = success.
 *   MBOX_E_GENERIC          -1  — transport or protocol-level failure
 *                                  (already logged by the driver)
 *   MBOX_E_TAG_UNSUPPORTED  -2  — VC returned buffer-level parse
 *                                  error on a structurally-valid
 *                                  request, which on Pi 5 specifically
 *                                  means "this EEPROM firmware doesn't
 *                                  implement the tag". On Pi 5, tag
 *                                  0x00010003 was added to the EEPROM
 *                                  mailbox subset on 2025-05-08
 *                                  (rpi-eeprom #698); older revisions
 *                                  can't answer it. Callers treat this
 *                                  as "try another source" rather
 *                                  than a bug. */
#define MBOX_E_GENERIC          (-1)
#define MBOX_E_TAG_UNSUPPORTED  (-2)

/*
 * Read the board-unique MAC address from VideoCore firmware.
 *
 * Returns 0 on success with `mac` populated (6 bytes). Returns
 * MBOX_E_TAG_UNSUPPORTED if the target EEPROM doesn't implement the
 * tag, or MBOX_E_GENERIC on any other failure. Safe to call
 * repeatedly; no init step.
 *
 * Implementation detail: uses a file-static 32-byte property buffer,
 * 16-byte aligned, reused across calls. Not reentrant — call from a
 * single task context, not from an IRQ.
 */
int bcm_mailbox_get_board_mac(uint8_t mac[6]);

#endif /* PLATFORM_RASPI5 */

#endif /* BCM_MAILBOX_H */
