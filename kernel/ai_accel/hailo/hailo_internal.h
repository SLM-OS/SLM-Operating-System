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

#endif /* AI_ACCEL_HAILO_INTERNAL_H */
