/*
 * hailo_internal.h — declarations shared between hailo_core.c and
 *                     the hailo unit tests.
 *
 * Not part of the public API (see hailo.h for that). Exposes
 * static-by-default decode helpers so negative-path tests can drive
 * them without stubbing the whole boot state machine.
 */

#ifndef AI_ACCEL_HAILO_INTERNAL_H
#define AI_ACCEL_HAILO_INTERNAL_H

#include "hailo.h"

/*
 * Parse the secure-boot certificate trailer at `cert_off` within a
 * firmware blob whose outer app-FW header has already been validated
 * (hailo_validate_firmware). On success populates *out_cert with the
 * decoded cert_header, points *out_key / *out_content at the start of
 * each payload inside `blob`, and sets *out_cert_end to the byte-
 * past-content offset (used by hailo_decode_core_fw as the start of
 * the core section).
 *
 * All out-parameters are REQUIRED non-NULL. `blob` must be non-NULL.
 * Returns HAILO_ERR_INVAL if any required pointer is NULL, or
 * HAILO_ERR_BAD_FIRMWARE on bounds / alignment / size failure.
 *
 * key_size / content_size must be 4-byte-aligned — BAR4 writes in
 * the platform shim only accept dword-sized writes.
 *
 * The helper performs NO state-machine mutation. Callers that care
 * about HAILO_STATE_FAILED must set it themselves on error return.
 */
int hailo_decode_cert(const uint8_t *blob, size_t fw_size,
                      size_t cert_off,
                      struct hailo_fw_cert_header *out_cert,
                      const uint8_t **out_key,
                      const uint8_t **out_content,
                      size_t *out_cert_end);

/*
 * Parse the core-firmware header + code section at `core_hdr_off`
 * (typically cert_end from hailo_decode_cert). Hailo-8 production
 * firmware always carries a core section after the cert; a blob
 * without one is intentionally rejected.
 *
 * All out-parameters are REQUIRED non-NULL. `blob` must be non-NULL.
 * Returns HAILO_ERR_INVAL if any required pointer is NULL, or
 * HAILO_ERR_BAD_FIRMWARE on magic / header_version / code_size /
 * truncation failures.
 *
 * The helper performs NO state-machine mutation. Callers own the
 * HAILO_STATE_FAILED transition on error return.
 */
int hailo_decode_core_fw(const uint8_t *blob, size_t fw_size,
                         size_t core_hdr_off,
                         struct hailo_firmware_header *out_core_hdr,
                         const uint8_t **out_core_code);

/*
 * Diagnostic helpers implemented in kernel/inference/inference_device_hailo.c
 * but exposed here so kernel/ai_accel/hailo/hailo_shell.c can drive
 * them on demand (`hailo d2h`, `hailo fwlog`, `hailo fwloghex`). The
 * shell previously redeclared each with an inline `extern`; gathered
 * here so signature drift is a compile error rather than a silent
 * link-time mismatch.
 */
void hailo_fw_drain_d2h_notifications(uint32_t max_events);
void hailo_fw_dump_logs(void);
void hailo_fw_dump_logs_hex(uint32_t max_bytes);

/*
 * #682 hypothesis-6 diagnostic. Reads PCIe Link Status (cap+0x12)
 * and Link Capabilities (cap+0x0C) on the Hailo endpoint and prints
 * trained-vs-max speed/width with the supplied label. Used to
 * confirm `dtparam=pciex1_gen=3` actually trained Gen3 at our call
 * sites, and that the link doesn't drop into recovery / Gen1
 * fallback during the boundary-submit poll window. Implemented in
 * the platform shim (Pi 5 = real read; non-Pi5 = no-op stub) so
 * cross-platform builds keep linking.
 */
void hailo_platform_log_link_state(const char *label);

/*
 * #682 hyp-X-1 (DISCONFIRMED 2026-05-09 on pi-5-1, kept as
 * diagnostic). Mirror Linux's hailo_pcie_read_interrupt drain:
 * read BCS_ISTATUS_HOST, then if VDMA_SRC bit set read+W1C
 * BCS_SOURCE_INTERRUPT_PER_CHANNEL (0x400); if VDMA_DEST set
 * read+W1C BCS_DESTINATION_INTERRUPT_PER_CHANNEL (0x500); finally
 * W1C the aggregate ISTATUS_HOST excluding FW_CONTROL_BIT
 * (preserves an in-flight RPC notification). Mirrors the same logic
 * in control_msi_handler.
 *
 * Disconfirmation: with the drain wired at pre-IN-submit, ISTATUS
 * goes 0x02800001→0x00000000 and PER_SRC 0x00000003→0x00000000
 * cleanly, but the boundary IN ch=2 wedge is unchanged (dev_proc
 * stays 0, dev_base[31:16] stays 0). Stale host-side IRQ acks are
 * NOT the gating factor for fw's ch=2 prep. Helper retained because
 * it produces a cleaner post-timeout state for future correlation.
 *
 * Implementation in kernel/ai_accel/hailo/hailo_control.c so it can
 * access control_msi_pending (preserved across the W1C).
 */
void hailo_control_drain_pending_irqs(const char *label);

/*
 * Diagnostic-only (#682 hyp-U / hyp-W): dump RC bridge status /
 * error registers, the Hailo endpoint's standard PCI Status + PCI
 * Express Cap Device Status, and the EP MSI capability. Use after
 * a suspected fw-side DMA stall to see whether the BCM2712 RC has
 * caught a TLP completion timeout, address error, AXI read-error
 * substitution, and whether the endpoint detected an Unsupported
 * Request / Master Abort completion. Pi 5 implementation reads
 * BCM2712 RC MMIO via pcie_bcm2712_dump_status_for_debug(); other
 * platforms = no-op.
 */
void hailo_platform_dump_bridge_errors(const char *label);

/*
 * Diagnostic-only (#682 hyp-W): W1C the EP's PCI_STATUS error bits
 * and PCI Express Cap Device Status error bits, so a subsequent
 * dump shows only errors that fired AFTER this clear. Use to
 * disambiguate stale boot-time errors from runtime errors during
 * a specific window (e.g., clear at pre-IN-submit, dump at
 * post-timeout). Reads back after the clear and logs the
 * post-clear state.
 */
void hailo_platform_clear_bridge_errors(const char *label);

#endif /* AI_ACCEL_HAILO_INTERNAL_H */
