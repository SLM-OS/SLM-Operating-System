/*
 * camrtc_capture.h — Capture-control message API over the bound
 * RCE IVC channel.
 *
 * Builds on `camrtc.h` (HSP-VM transport) and `camrtc_ivc.h` (ring
 * transport): exposes typed wrappers for the
 * `docs/reference/l4t-camrtc-capture-messages.h` request/response
 * pairs SLM-OS needs to bring up the IMX219 capture path.
 *
 * Today implements:
 *   - `camrtc_capture_init`  — runs camrtc_init + CH_SETUP +
 *                              ivc_init for the capture-control
 *                              channel. Idempotent.
 *   - `camrtc_capture_phy_stream_open` —
 *     `CAPTURE_PHY_STREAM_OPEN_REQ` → `CAPTURE_PHY_STREAM_OPEN_RESP`.
 *     First-light probe that proves the IVC ring works end-to-end.
 *
 * Future additions land here:
 *   `CAPTURE_CSI_STREAM_SET_CONFIG_REQ` (CSI-2 PHY config),
 *   `CAPTURE_CSI_STREAM_TPG_*` (TPG smoke-test path),
 *   `CAPTURE_CHANNEL_SETUP_REQ` (VI capture channel binding).
 *
 * Jetson-only; non-Jetson stubs return -1.
 */

#pragma once

#include <stdint.h>

/*
 * Initialise the capture-control IVC channel:
 *   1. camrtc_init        — HSP-VM HELLO/PROTOCOL/RESUME (idempotent)
 *   2. camrtc_ch_setup    — RCE binds rx@0xa0001000 + tx@0xa0006080
 *                           to (group=1, service="capture-control")
 *   3. camrtc_ivc_init    — zero ring headers, kick RCE
 *
 * Returns 0 on success, negative on the first failing step. Any
 * failure leaves the global channel state uninitialised; the caller
 * may retry without cleanup.
 */
int camrtc_capture_init(void);

/*
 * CAPTURE_PHY_STREAM_OPEN_REQ → response wrapper.
 *
 *   stream_id  NVCSI stream (0..5; IMX219-A on Orin Nano dev kit
 *              uses NVCSI_STREAM_0).
 *   csi_port   NVCSI physical port (0..7; IMX219-A on Orin Nano
 *              dev kit J20 connector is NVCSI_PORT_A = 0).
 *   phy_type   NVCSI_PHY_TYPE_DPHY (0) for IMX219.
 *   out_result Optional: filled with RCE's `result` field (0 ==
 *              CAPTURE_OK; non-zero is one of the
 *              `capture_result` codes in
 *              `docs/reference/l4t-camrtc-capture-messages.h`).
 *
 * Returns 0 on success (request sent + response received +
 * out_result populated; *the caller must inspect *out_result* for
 * the RCE-side success code*), negative on transport failure:
 *   -1  Bad arguments OR camrtc_capture_init not yet successful.
 *   -2  IVC send failed (ring full or transport error).
 *   -3  IVC recv timed out (RCE didn't respond within ~1 s).
 *   -4  Wrong response opcode (msg_id != PHY_STREAM_OPEN_RESP).
 *   -5  Transaction id mismatch.
 */
int camrtc_capture_phy_stream_open(uint32_t stream_id,
                                   uint32_t csi_port,
                                   uint32_t phy_type,
                                   uint32_t *out_result);
