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

/* ---- Capture-control wire constants ----
 *
 * Pinned subset of `docs/reference/l4t-camrtc-capture-messages.h`.
 * These are wire-format ABI between SLM-OS and the RCE firmware;
 * `_Static_assert` block in `kernel/tests/test_camera.c` pins
 * struct sizes and opcode values against accidental drift. */

/* Standard 8-byte header at the front of every capture-control
 * frame (`struct CAPTURE_MSG_HEADER` in the L4T reference). */
struct capture_msg_header {
    uint32_t msg_id;
    uint32_t transaction;   /* anonymous union with channel_id */
};

/* CAPTURE_PHY_STREAM_OPEN_REQ_MSG body — 16 bytes. */
struct capture_phy_stream_open_req {
    uint32_t stream_id;
    uint32_t csi_port;
    uint32_t phy_type;
    uint32_t pad32__;
};

/* CAPTURE_PHY_STREAM_OPEN_RESP_MSG body — 8 bytes. */
struct capture_phy_stream_open_resp {
    uint32_t result;
    uint32_t pad32__;
};

/* Message IDs (request / response). */
#define CAPTURE_PHY_STREAM_OPEN_REQ    0x36u
#define CAPTURE_PHY_STREAM_OPEN_RESP   0x37u
#define CAPTURE_CSI_STREAM_SET_CONFIG_REQ   0x40u
#define CAPTURE_CSI_STREAM_SET_CONFIG_RESP  0x41u

/* Number of lanes per NVCSI brick (CAMRTC_BRICK_NUM_LANES from
 * `docs/reference/l4t-camrtc-capture.h:1432`). Each brick covers
 * 4 D-PHY lanes; the cil_config selects how many of them are used
 * for the active stream. */
#define NVCSI_BRICK_NUM_LANES   4u

/* `struct nvcsi_brick_config` — 16 bytes wire format
 * (`l4t-camrtc-capture.h:1531`). Selects D-PHY vs C-PHY for the
 * brick and sets per-lane swizzle / polarity. */
struct nvcsi_brick_config {
    uint32_t phy_mode;       /* 0=NVCSI_PHY_TYPE_DPHY, 1=CPHY */
    uint32_t lane_swizzle;
    uint8_t  lane_polarity[NVCSI_BRICK_NUM_LANES];
    uint32_t pad32__;
};

/* `struct nvcsi_cil_config` — 16 bytes wire format
 * (`l4t-camrtc-capture.h:1551`). Per-stream lane count, MIPI
 * timing, and MIPI clock rate. `mipi_clock_rate` is in kHz. */
struct nvcsi_cil_config {
    uint8_t  num_lanes;       /* 0..4 */
    uint8_t  lp_bypass_mode;
    uint8_t  t_hs_settle;     /* 0 → SoC default */
    uint8_t  t_clk_settle;
    uint32_t cil_clock_rate;  /* deprecated upstream; pass 0 */
    uint32_t mipi_clock_rate; /* kHz */
    uint32_t pad32__;
};

/* `struct vi_hsm_csimux_error_mask_config` — 8 bytes
 * (`l4t-camrtc-capture.h:1585`). */
struct vi_hsm_csimux_error_mask_config {
    uint32_t error_mask_correctable;
    uint32_t error_mask_uncorrectable;
};

/* `struct nvcsi_error_config` — 56 bytes
 * (`l4t-camrtc-capture.h:1715`). Error-routing masks for HSM/LIC.
 * Set every field to 0 to silence error reporting; non-zero values
 * route specific NVCSI/CIL/host1x errors to LIC or HSM. */
struct nvcsi_error_config {
    uint32_t host1x_intr_mask_lic;
    uint32_t host1x_intr_mask_hsm;
    uint32_t host1x_intr_type_hsm;
    uint32_t status2vi_notify_mask;
    uint32_t stream_intr_mask_lic;
    uint32_t stream_intr_mask_hsm;
    uint32_t stream_intr_type_hsm;
    uint32_t cil_intr_mask_hsm;
    uint32_t cil_intr_type_hsm;
    uint32_t cil_intr0_mask_lic;
    uint32_t cil_intr1_mask_lic;
    uint32_t pad32__;
    struct vi_hsm_csimux_error_mask_config csimux_config;
};

/* CAPTURE_CSI_STREAM_SET_CONFIG_REQ_MSG body — 104 bytes
 * (`l4t-camrtc-capture-messages.h:407`). */
struct capture_csi_stream_set_config_req {
    uint32_t stream_id;
    uint32_t csi_port;
    uint32_t config_flags;
    uint32_t pad32__;
    struct nvcsi_brick_config  brick_config;
    struct nvcsi_cil_config    cil_config;
    struct nvcsi_error_config  error_config;
};

/* CAPTURE_CSI_STREAM_SET_CONFIG_RESP_MSG body — 8 bytes
 * (`l4t-camrtc-capture-messages.h:427`). */
struct capture_csi_stream_set_config_resp {
    uint32_t result;
    uint32_t pad32__;
};

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

/*
 * CAPTURE_CSI_STREAM_SET_CONFIG_REQ → response wrapper. Configures
 * the NVCSI brick + CIL + error masks for a previously-opened
 * stream (must follow `camrtc_capture_phy_stream_open` for the
 * same stream_id/csi_port). After this returns rc=0 *and*
 * *out_result == 0, NVCSI will start receiving CSI-2 packets from
 * the wire — verify with NVCSI INTR_STATUS once the sensor is
 * streaming.
 *
 *   stream_id        NVCSI_STREAM_0 for IMX219-A on Orin Nano.
 *   csi_port         NVCSI_PORT_A (0).
 *   num_lanes        D-PHY data lane count (2 for IMX219 binned).
 *   mipi_clock_rate  MIPI clock in kHz (456000 = 456 MHz, the
 *                    IMX219 default link freq from
 *                    `docs/reference/linux-imx219.c:139`).
 *   out_result       Optional: filled with RCE's `result` field.
 *
 * Returns 0/-1/-2/-3/-4/-5 with the same meaning as
 * `camrtc_capture_phy_stream_open` (see above).
 */
int camrtc_capture_csi_stream_set_config(uint32_t stream_id,
                                         uint32_t csi_port,
                                         uint8_t  num_lanes,
                                         uint32_t mipi_clock_rate,
                                         uint32_t *out_result);
