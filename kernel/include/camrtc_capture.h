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
#define CAPTURE_CHANNEL_SETUP_REQ      0x1Eu
#define CAPTURE_CHANNEL_SETUP_RESP     0x11u
#define CAPTURE_REQUEST_REQ            0x01u
#define CAPTURE_STATUS_IND             0x02u

/* Per-request capture_descriptor.capture_flags bits, subset SLM-OS
 * uses. From `l4t-camrtc-capture.h:1258`. */
#define CAPTURE_FLAG_STATUS_REPORT_ENABLE   0x1u  /* RCE sends STATUS_IND on completion */
#define CAPTURE_FLAG_ERROR_REPORT_ENABLE    0x2u  /* RCE sends STATUS_IND on error */

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

/* ---- VI capture channel setup wire structs (PR2 of HW Task 4) ---- */

/* `struct csi_stream_config` — 16 bytes wire format
 * (`l4t-camrtc-capture.h:369`). Identifies a single CSI input. */
struct camrtc_csi_stream_config {
    uint32_t stream_id;
    uint32_t csi_port;
    uint32_t virtual_channel;
    uint32_t pad32__;
};

/* `struct syncpoint_info` — 24 bytes wire format
 * (`l4t-camrtc-capture.h:37`). RCE-side Host1x syncpoint binding
 * for capture-progress / embedded-data / line-timer signalling.
 * Set every field to 0 to skip syncpoint signalling — completion
 * still arrives via CAPTURE_STATUS_IND when
 * CAPTURE_FLAG_STATUS_REPORT_ENABLE is set on the per-request
 * descriptor (handled in PR3). */
struct camrtc_syncpoint_info {
    uint32_t id;
    uint32_t threshold;
    uint8_t  gos_sid;
    uint8_t  gos_index;
    uint16_t gos_offset;
    uint32_t pad_;
    uint64_t shim_addr;
};

/* Number of GOS tables in capture_channel_config (`VI_NUM_GOS_TABLES`
 * from `l4t-camrtc-capture.h:230`). 12 IOVAs of 8 bytes each = 96 B
 * inside the channel config. SLM-OS doesn't use GOS for first-light
 * — `num_vi_gos_tables = 0` and the array stays zeroed. */
#define VI_NUM_GOS_TABLES   12u

/* CAPTURE_CHANNEL_FLAG_* bits, subset SLM-OS uses. The full
 * enumeration lives at `l4t-camrtc-capture.h:274`. RCE checks these
 * bits against the VI channel's hardware capabilities to pick a
 * compatible channel. */
#define CAPTURE_CHANNEL_FLAG_VIDEO    0x0001u  /* video frames */
#define CAPTURE_CHANNEL_FLAG_RAW      0x0002u  /* raw Bayer (no ISP) */
#define CAPTURE_CHANNEL_FLAG_CSI      0x10000u /* CSI source (vs SLVS-EC) */

/* VI unit selector (`l4t-camrtc-capture.h:355`). Orin Nano has VI0
 * only — VI2 doesn't exist on T234, so `VI_UNIT_VI = 0` is the
 * only valid value. */
#define VI_UNIT_VI            0u

/* SLVS-EC stream sentinel for "not used" (`l4t-camrtc-capture.h:266`).
 * IMX219 uses CSI-2 over D-PHY, not SLVS-EC; pass 0xFF for both
 * `slvsec_stream_main` and `slvsec_stream_sub`. */
#define SLVSEC_STREAM_DISABLED  0xFFu

/* `struct capture_channel_config` — 272 bytes wire format
 * (`l4t-camrtc-capture.h:383`). The body of
 * CAPTURE_CHANNEL_SETUP_REQ_MSG. Total field-by-field offsets are
 * pinned by `_Static_assert`s in `kernel/tests/test_camera.c`. */
struct camrtc_capture_channel_config {
    uint32_t channel_flags;
    uint32_t channel_id;            /* RCE-internal — pass 0 */
    uint32_t vi_unit_id;            /* VI_UNIT_VI = 0 */
    uint32_t pad32_a__;
    uint64_t vi_channel_mask;       /* ~0ULL = "any VI channel" */
    uint64_t vi2_channel_mask;      /* 0 = no VI2 (T234 has none) */
    struct camrtc_csi_stream_config csi_stream;        /* 16 B */
    uint64_t requests;              /* IOVA of capture_descriptor[] ring */
    uint64_t requests_memoryinfo;   /* IOVA of memoryinfo[] ring */
    uint32_t queue_depth;
    uint32_t request_size;
    uint32_t request_memoryinfo_size;
    uint32_t reserved2;
    uint8_t  slvsec_stream_main;
    uint8_t  slvsec_stream_sub;
    uint16_t reserved1;
    uint32_t num_vi_gos_tables;
    uint64_t vi_gos_tables[VI_NUM_GOS_TABLES];         /* 96 B */
    struct camrtc_syncpoint_info progress_sp;          /* 24 B */
    struct camrtc_syncpoint_info embdata_sp;           /* 24 B */
    struct camrtc_syncpoint_info linetimer_sp;         /* 24 B */
    uint32_t error_mask_uncorrectable;
    uint32_t error_mask_correctable;
    uint64_t stop_on_error_notify_bits;
};

/* CAPTURE_CHANNEL_SETUP_REQ_MSG body — wraps capture_channel_config
 * (272 B) with no additional fields per
 * `l4t-camrtc-capture-messages.h:129`. */
struct camrtc_capture_channel_setup_req {
    struct camrtc_capture_channel_config channel_config;
};

/* CAPTURE_CHANNEL_SETUP_RESP_MSG body — 16 bytes
 * (`l4t-camrtc-capture-messages.h:144`). */
struct camrtc_capture_channel_setup_resp {
    uint32_t result;            /* CAPTURE_OK (0) or CAPTURE_ERROR_* */
    uint32_t channel_id;        /* RCE-assigned handle for this channel */
    uint64_t vi_channel_mask;   /* bitmask of allocated VI channel(s) */
};

/* Leading 12 bytes of `struct capture_descriptor`
 * (`l4t-camrtc-capture.h:1294`). The full struct is ~448 B and
 * carries vi_channel_config / atomp surfaces / per-frame status
 * — none of which SLM-OS sets today. RCE *does* read the first
 * three fields directly to drive its scheduler:
 *
 *   sequence                   (u32) — caller-assigned frame number,
 *                                       echoed back in the per-frame
 *                                       capture_status.
 *   capture_flags              (u32) — CAPTURE_FLAG_* bitmask
 *                                       (STATUS_REPORT_ENABLE etc).
 *   frame_start_timeout        (u16) — ms; 0 = use channel default.
 *   frame_completion_timeout   (u16) — ms; 0 = use channel default.
 *
 * Exposing just this prefix gives `csidiag` and other callers
 * field-name access to the bytes RCE reads, without porting the
 * full capture_descriptor (which has C bitfields and is fragile
 * for wire-format use). The remaining ~436 B of the slot stay
 * zeroed by the caller's pre-write loop. */
struct camrtc_capture_descriptor_header {
    uint32_t sequence;
    uint32_t capture_flags;
    uint16_t frame_start_timeout;
    uint16_t frame_completion_timeout;
};

/* Number of atom-packer surface slots in `camrtc_vi_channel_config`
 * — fixed at 4 for T194/T234 per L4T `l4t-camrtc-capture.h:231`. */
#define VI_NUM_ATOMP_SURFACES   4u

/* `struct vi_channel_config` — 160 bytes wire format
 * (`l4t-camrtc-capture.h:538`). Per-frame VI-unit register
 * programming RCE applies before triggering a capture: pixel-
 * formatter setup, frame geometry, atom-packer surface IOVAs,
 * DPCM strip layout, and channel-selector mask bits.
 *
 * Carried as the `ch_cfg` field of `struct capture_descriptor`
 * at offset 88 (after the 12 B header + the 76 B deprecated
 * prefence block). RCE reads it directly to drive its scheduler;
 * a zero-init value is what makes today's `csidiag` capture
 * trip RCE-side scheduler errors before any STATUS_IND can be
 * emitted (see `docs/jetson-camera-vi-driver-notes.md`).
 *
 * Bitfield ABI: the leading 13 single-bit flags pack LSB-first
 * into a 32-bit `unsigned` container, matching GCC's AArch64 +
 * Cortex-R5F (RCE) layout. The trailing `pad_flags__:19` closes
 * the container at 32 bits exactly. `_Static_assert`s in
 * `kernel/tests/test_camera.c` pin the bit positions for every
 * flag plus the size + offset of each substruct so a future
 * upstream reorder breaks the build.
 *
 * Anonymous nested structs (C11/C23) keep struct-tag pollution
 * out of file scope; tests reference fields via dotted member
 * paths in `offsetof`.
 *
 * NOT YET POPULATED by `csidiag` — porting the type is one PR;
 * wiring IMX219-specific values (frame_x=1640, frame_y=1232,
 * pixfmt RAW10, atomp surface IOVA) is a follow-on PR. */
struct camrtc_vi_channel_config {
    /* Single-bit flags packed into a 32-bit container. */
    unsigned dt_enable:1;
    unsigned embdata_enable:1;
    unsigned flush_enable:1;
    unsigned flush_periodic:1;
    unsigned line_timer_enable:1;
    unsigned line_timer_periodic:1;
    unsigned pixfmt_enable:1;
    unsigned pixfmt_wide_enable:1;
    unsigned pixfmt_wide_endian:1;
    unsigned pixfmt_pdaf_replace_enable:1;
    unsigned ispbufa_enable:1;
    unsigned ispbufb_enable:1;       /* not valid for T186/T194; T234 OK */
    unsigned compand_enable:1;
    unsigned pad_flags__:19;

    /* VI channel selector — RCE picks frames whose CSI-2 header
     * matches every (value & mask) pair. */
    struct {
        uint8_t  datatype;
        uint8_t  datatype_mask;
        uint8_t  stream;
        uint8_t  stream_mask;
        uint16_t vc;
        uint16_t vc_mask;
        uint16_t frameid;
        uint16_t frameid_mask;
        uint16_t dol;
        uint16_t dol_mask;
    } match;

    uint8_t  dol_header_sel;
    uint8_t  dt_override;
    uint8_t  dpcm_mode;
    uint8_t  pad_dol_dt_dpcm__;

    /* Frame geometry — pre-crop pixel dimensions, embedded-data
     * lines, output skip + crop windows. */
    struct {
        uint16_t frame_x;
        uint16_t frame_y;
        uint32_t embed_x;
        uint32_t embed_y;
        struct {
            uint16_t x;
            uint16_t y;
        } skip;
        struct {
            uint16_t x;
            uint16_t y;
        } crop;
    } frame;

    uint16_t flush;
    uint16_t flush_first;
    uint16_t line_timer;
    uint16_t line_timer_first;

    /* Pixel formatter + Phase Detection AF replacement window. */
    struct {
        uint16_t format;
        uint8_t  pad0_en;
        uint8_t  pad__;
        struct {
            uint16_t crop_left;
            uint16_t crop_right;
            uint16_t crop_top;
            uint16_t crop_bottom;
            uint16_t replace_crop_left;
            uint16_t replace_crop_right;
            uint16_t replace_crop_top;
            uint16_t replace_crop_bottom;
            uint16_t last_pixel_x;
            uint16_t last_pixel_y;
            uint16_t replace_value;
            uint8_t  format;
            uint8_t  pad_pdaf__;
        } pdaf;
    } pixfmt;

    /* DPCM strip + chunk geometry. */
    struct {
        uint16_t strip_width;
        uint16_t strip_overfetch;
        uint16_t chunk_first;
        uint16_t chunk_body;
        uint16_t chunk_body_count;
        uint16_t chunk_penultimate;
        uint16_t chunk_last;
        uint16_t pad__;
        uint32_t clamp_high;
        uint32_t clamp_low;
    } dpcm;

    /* Atom-packer destination surfaces (IOVA per plane) plus
     * per-plane stride and DPCM chunk stride. */
    struct {
        struct {
            uint32_t offset;        /* lo32 of IOVA */
            uint32_t offset_hi;     /* hi32 of IOVA */
        } surface[VI_NUM_ATOMP_SURFACES];
        uint32_t surface_stride[VI_NUM_ATOMP_SURFACES];
        uint32_t dpcm_chunk_stride;
    } atomp;

    uint16_t pad__[2];
} __attribute__((aligned(8)));

/* `struct nvcsi_error_status` — 16 bytes
 * (`l4t-camrtc-capture.h:739`). Embedded in `capture_status` to
 * report NVCSI-side stream / virtual-channel / CIL errors that
 * preceded the captured frame. SLM-OS reads this for diagnostics. */
struct camrtc_nvcsi_error_status {
    uint32_t nvcsi_stream_bits;
    uint32_t nvcsi_virtual_channel_bits;
    uint32_t cil_a_error_bits;
    uint32_t cil_b_error_bits;
};

/* `struct capture_status` — 56 bytes
 * (`l4t-camrtc-capture.h:815`). RCE fills this into the descriptor
 * slot's `status` field after a capture completes (success or
 * error), then sends `CAPTURE_STATUS_IND` to wake the AP. The
 * `status` u32 carries the per-frame outcome — see the
 * `CAPTURE_STATUS_*` defines below; SUCCESS == 1.
 *
 * `notify_bits` is a u64 bitmask of finer-grained event reasons
 * (see `CAPTURE_STATUS_NOTIFY_BIT_*` in the L4T reference). */
struct camrtc_capture_status {
    uint8_t  src_stream;            /* CSI stream number */
    uint8_t  virtual_channel;       /* CSI virtual channel */
    uint16_t frame_id;              /* sequence echoed back from descriptor */
    uint32_t status;                /* CAPTURE_STATUS_* code */
    uint64_t sof_timestamp;         /* TSC ticks at start-of-frame */
    uint64_t eof_timestamp;         /* TSC ticks at end-of-frame */
    uint32_t err_data;              /* status-code-specific error payload */
    uint32_t flags;                 /* CAPTURE_STATUS_FLAG_* bitmask */
    uint64_t notify_bits;           /* CAPTURE_STATUS_NOTIFY_BIT_* mask */
    struct camrtc_nvcsi_error_status nvcsi_err_status;
} __attribute__((aligned(8)));

/* CAPTURE_STATUS_* — per-frame outcome codes returned by RCE in
 * `capture_status.status`. Subset SLM-OS recognises; full list in
 * `l4t-camrtc-capture.h:830` onward. */
#define CAPTURE_STATUS_UNKNOWN          0u
#define CAPTURE_STATUS_SUCCESS          1u
#define CAPTURE_STATUS_CSIMUX_FRAME     2u
#define CAPTURE_STATUS_CSIMUX_STREAM    3u
#define CAPTURE_STATUS_CHANSEL_FAULT    4u
#define CAPTURE_STATUS_CHANSEL_NO_MATCH 6u
#define CAPTURE_STATUS_CHANSEL_TIMEOUT  9u
#define CAPTURE_STATUS_FRAME_DROPPED    10u
#define CAPTURE_STATUS_PIXEL_RUNTIME_FAIL 11u
#define CAPTURE_STATUS_ATOMP_PACKER_OVERFLOW 12u
#define CAPTURE_STATUS_ATOMP_FRAME_TRUNCATED 13u
#define CAPTURE_STATUS_ATOMP_FRAME_TOSSED 14u

/* Offset of `capture_status` within `struct capture_descriptor`
 * (per `l4t-camrtc-capture.h:1294`). Computed from the descriptor
 * layout:
 *   header              (offset 0,   12 B)
 *   prefence_count      (offset 12,  4 B)
 *   prefence[2]         (offset 16,  48 B)   2 × syncpoint_info(24)
 *   ch_cfg              (offset 64,  160 B)  vi_channel_config
 *   pfsd_cfg            (offset 224, 40 B)
 *   engine_status       (offset 264, 8 B)
 *   status              (offset 272, 56 B)   ← THIS
 *   pad32__[14]         (offset 328, 56 B)
 *
 * Until the full `camrtc_capture_descriptor` type is ported,
 * callers reach status via raw byte arithmetic on the descriptor
 * base. */
#define CAMRTC_DESC_CH_CFG_OFFSET       64u
#define CAMRTC_DESC_STATUS_OFFSET       272u

/* CAPTURE_REQUEST_REQ_MSG body — 8 bytes
 * (`l4t-camrtc-capture-messages.h:905`). Identifies which slot in
 * the request_ring (set up by CAPTURE_CHANNEL_SETUP) RCE should
 * pull a capture_descriptor from. */
struct camrtc_capture_request_req {
    uint32_t buffer_index;
    uint32_t pad32__;
};

/* CAPTURE_STATUS_IND_MSG body — 8 bytes
 * (`l4t-camrtc-capture-messages.h:918`). RCE sends one of these
 * on the capture rx ring after a request completes (when
 * CAPTURE_FLAG_STATUS_REPORT_ENABLE is set in the descriptor) or
 * after an error (when CAPTURE_FLAG_ERROR_REPORT_ENABLE is set).
 * The matching slot in the request_ring has its `status` field
 * filled in by RCE before the IND is sent. */
struct camrtc_capture_status_ind {
    uint32_t buffer_index;
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

/*
 * CAPTURE_CHANNEL_SETUP_REQ → response wrapper. Asks RCE to
 * allocate a VI capture channel for the given CSI stream/port.
 * On success, RCE assigns a `channel_id` and a `vi_channel_mask`
 * (which physical VI channel got picked) — both returned in the
 * out-parameters.
 *
 * This wrapper builds a *minimal first-light* config:
 *   channel_flags      = VIDEO | RAW | CSI (no ISP, no PDAF, no
 *                        embedded data, no PFSD)
 *   vi_unit_id         = VI_UNIT_VI (0; T234 has no VI2)
 *   vi_channel_mask    = ~0ULL (let RCE pick any allowed channel)
 *   vi2_channel_mask   = 0
 *   csi_stream         = {stream_id, csi_port, virtual_channel=0}
 *   requests           = `requests_iova`
 *   requests_memoryinfo= `requests_memoryinfo_iova`
 *   queue_depth        = `queue_depth`
 *   request_size       = `request_size`
 *   request_memoryinfo_size = `memoryinfo_size`
 *   slvsec_stream_*    = SLVSEC_STREAM_DISABLED (0xFF)
 *   num_vi_gos_tables  = 0; vi_gos_tables[] = all 0
 *   progress_sp / embdata_sp / linetimer_sp = all 0 (no Host1x
 *                        syncpoint signalling — completion arrives
 *                        via CAPTURE_STATUS_IND when the per-
 *                        request descriptor sets the
 *                        STATUS_REPORT_ENABLE flag)
 *   error_mask_*       = 0 (default HSM error policy)
 *   stop_on_error_notify_bits = 0
 *
 * Caller must allocate the request ring + memoryinfo ring in RCE-
 * accessible memory (inside the VM1 IOVA aperture
 * 0xA0000000..0xC0000000, same constraint as the CH_SETUP region)
 * BEFORE calling this. PR3 will land the descriptor allocator and
 * the CAPTURE_REQUEST_REQ wrapper.
 *
 *   stream_id        NVCSI_STREAM_0 for IMX219-A on Orin Nano.
 *   csi_port         NVCSI_PORT_A (0).
 *   requests_iova    Phys addr of capture_descriptor[] ring.
 *                    Pass 0 for a smoke-test that probes the wire
 *                    format only — RCE will reject with
 *                    CAPTURE_ERROR_INVALID_PARAMETER but the
 *                    round-trip itself proves the message wire
 *                    format is correct.
 *   requests_memoryinfo_iova  Phys addr of memoryinfo[] ring (same
 *                    smoke-test caveat).
 *   queue_depth      Ring buffer depth (set 0 for the smoke-test).
 *   request_size     Bytes per descriptor slot (set 0 for the
 *                    smoke-test).
 *   memoryinfo_size  Bytes per memoryinfo slot (set 0 for the
 *                    smoke-test).
 *   out_result       Optional: filled with RCE's `result` field.
 *   out_channel_id   Optional: RCE-assigned channel handle (used in
 *                    subsequent CAPTURE_REQUEST_REQ messages).
 *   out_vi_channel_mask  Optional: which physical VI channel RCE
 *                    allocated.
 *
 * Returns 0/-1/-2/-3/-4/-5 with the same meaning as
 * `camrtc_capture_phy_stream_open` (see above).
 */
int camrtc_capture_channel_setup(uint32_t stream_id,
                                 uint32_t csi_port,
                                 uint64_t requests_iova,
                                 uint64_t requests_memoryinfo_iova,
                                 uint32_t queue_depth,
                                 uint32_t request_size,
                                 uint32_t memoryinfo_size,
                                 uint32_t *out_result,
                                 uint32_t *out_channel_id,
                                 uint64_t *out_vi_channel_mask);

/*
 * CAPTURE_REQUEST_REQ → CAPTURE_STATUS_IND wrapper. Submits a
 * capture request that points at slot `buffer_index` in the
 * request_ring set up by CAPTURE_CHANNEL_SETUP. The caller must
 * have already populated the slot with a `capture_descriptor`
 * (capture_flags / sequence / vi_channel_config / atomp surfaces
 * etc).
 *
 * Sends over the **capture** IVC channel (NOT capture-control).
 * Polls the capture rx ring for `CAPTURE_STATUS_IND` matching the
 * same `buffer_index`, up to `timeout_us` microseconds.
 *
 * RCE writes the per-frame `capture_status` substruct of the
 * descriptor (status code, frame ID, SOF/EOF timestamps, error
 * notify bits) before sending the IND, so the caller can inspect
 * the descriptor at slot `buffer_index` to get the full result.
 *
 *   buffer_index     Slot in the request_ring (0..queue_depth-1).
 *   timeout_us       Max poll time for STATUS_IND.
 *   out_status_index Optional: buffer_index from the IND (should
 *                    equal the input on a clean round-trip).
 *
 * Returns 0 on success (IND received, buffer_index matched),
 * negative on transport failure:
 *   -1  Bad arguments OR camrtc_capture_init not yet successful.
 *   -2  IVC send failed.
 *   -3  IND not received within `timeout_us`.
 *   -4  Wrong response opcode (msg_id != STATUS_IND).
 *   -5  buffer_index mismatch (RCE replied for a different slot).
 *
 * NOTE: a successful return only means "RCE completed processing
 * of the request and signalled us back". It does NOT mean "frame
 * captured successfully" — the caller MUST inspect the slot's
 * capture_status.status field for `CAPTURE_STATUS_SUCCESS` (1)
 * vs error codes.
 */
int camrtc_capture_request(uint32_t buffer_index,
                           uint32_t *out_status_index,
                           uint32_t timeout_us);
