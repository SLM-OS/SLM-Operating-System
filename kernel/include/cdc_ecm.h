/*
 * cdc_ecm.h - CDC-ECM USB class driver (Phase 2 of #266)
 *
 * Binds to an enumerated CDC-ECM USB device (Communication class
 * 0x02 / subclass 0x06 + Data class 0x0A) and presents it as a
 * `struct net_driver` so lwIP can send / receive over bulk
 * endpoints. Scope per docs/jetson-usb-networking-plan.md §6:
 * one device, USB 2.0, connect-at-boot.
 */

#ifndef CDC_ECM_H
#define CDC_ECM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Probe the device currently returned by usb_core_first_device(). On
 * success, registers a net_driver that routes frames through the
 * device's bulk IN / bulk OUT endpoints. Returns 0 on success,
 * negative on failure (no CDC-ECM interface found, MAC retrieval
 * failed, etc.). No-op if no device is present (returns 0 — treat
 * as "USB networking disabled").
 *
 * Idempotent: safe to call repeatedly. Once a device has been
 * successfully bound the function returns 0 immediately without
 * touching any state. Phase 4 drives this from net_poll() so a
 * post-boot enumeration (e.g. the #309 re-plug on Jetson) is picked
 * up without a separate callback path. Call cdc_ecm_reset() to
 * force a re-probe (test-only).
 */
int cdc_ecm_probe_and_register(void);

/*
 * Clear the bound-to-device state so the next call to
 * cdc_ecm_probe_and_register() re-runs the full probe. Test-only
 * hook — production code relies on the idempotent "bind once per
 * boot" contract and never needs to reset.
 */
void cdc_ecm_reset(void);

/*
 * Drives the USB core's poll path so bulk IN completions can be
 * observed on IRQ-less HCDs (the mock and any polled-only XHCI
 * variant). Unwired in Phase 2 — Phase 4 (lwIP netif integration)
 * will call it from net_poll(); exposed here so that hookup is a
 * one-line change.
 */
void cdc_ecm_poll(void);

/* Test-visible accessors (diagnostics + regression tests). */

/* Returns the 6-byte MAC parsed from the Ethernet functional
 * descriptor + string descriptor. Returns NULL if no probe has
 * succeeded yet. */
const uint8_t *cdc_ecm_get_mac(void);

/* Maximum Ethernet segment size the device advertises
 * (wMaxSegmentSize from the functional descriptor). Defaults to
 * 1514 if the field was zero or missing. */
uint16_t cdc_ecm_get_max_segment(void);

/* Number of RX URB completions seen so far. Used by tests to confirm
 * the RX path resubmitted after a frame was consumed. */
uint32_t cdc_ecm_get_rx_count(void);

/* Number of TX URB completions seen so far. */
uint32_t cdc_ecm_get_tx_count(void);

/*
 * Test-only entry point: parse the 6-byte MAC out of a USB string
 * descriptor the way a real CDC-ECM device reports it (UTF-16LE of 12
 * hex characters). Exposed in the header so the unit tests can
 * exercise the parser directly without standing up a full mock HCD.
 * `desc` / `desc_len` are the raw string-descriptor bytes (including
 * the 2-byte header).
 */
int cdc_ecm_parse_mac_string(const uint8_t *desc, size_t desc_len,
                             uint8_t out_mac[6]);

#endif /* CDC_ECM_H */
