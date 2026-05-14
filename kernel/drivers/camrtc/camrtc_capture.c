/*
 * camrtc_capture.c — Capture-control message wrappers (IMX219
 * bring-up path).
 *
 * Sits above camrtc.c (HSP-VM transport) and camrtc_ivc.c (ring
 * transport). Owns the static channel state for the
 * capture-control IVC channel, the typed message structs, and the
 * request/response correlation logic.
 */

#include "camrtc_capture.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "camrtc.h"
#include "camrtc_channels.h"
#include "camrtc_ivc.h"
#include "camrtc_layout.h"
#include "debug.h"
#include "platform.h"

/* CH_SETUP geometry comes from `camrtc_layout.h`, shared with
 * `camrtc.c`. The control-channel rx/tx IOVAs are computed locally
 * (`region_phys + CAMRTC_IVC_CONFIG_SIZE` and one queue further);
 * the capture-channel IOVAs come from
 * `camrtc_ch_setup_capture_{rx,tx}_iova` accessors so this file
 * doesn't have to track the queue-size arithmetic. */
#define CAPTURE_CTRL_QUEUE_BYTES \
    (TEGRA_IVC_HEADER_SIZE + CAMRTC_CTRL_NFRAMES * CAMRTC_CTRL_FRAME_SIZE)

/* ---- Module state ---- */

static struct camrtc_ivc_channel g_ctrl_chan;
static struct camrtc_ivc_channel g_cap_chan;
static bool                      g_ctrl_ready;
static uint32_t                  g_next_transaction = 1u;

/* ---- byte memcpy / cmp (no <string.h>) ---- */

static void mem_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* ---- Public API ---- */

int camrtc_capture_init(void)
{
    if (g_ctrl_ready) return 0;

    int rc = camrtc_init();
    if (rc != 0) {
        WARN("capture_init: camrtc_init failed rc=%d", rc);
        return rc;
    }

    rc = camrtc_ch_setup_capture_control();
    if (rc != 0) {
        WARN("capture_init: ch_setup failed rc=%d", rc);
        return rc;
    }

    uintptr_t region = camrtc_ch_setup_region_phys();
    uintptr_t ctrl_rx_iova = region + CAMRTC_IVC_CONFIG_SIZE;
    uintptr_t ctrl_tx_iova = ctrl_rx_iova + CAPTURE_CTRL_QUEUE_BYTES;
    uintptr_t cap_rx_iova  = camrtc_ch_setup_capture_rx_iova();
    uintptr_t cap_tx_iova  = camrtc_ch_setup_capture_tx_iova();

    rc = camrtc_ivc_init(&g_ctrl_chan, ctrl_rx_iova, ctrl_tx_iova,
                         CAMRTC_CTRL_NFRAMES,
                         CAMRTC_CTRL_FRAME_SIZE,
                         CAMRTC_CTRL_GROUP);
    if (rc != 0) {
        WARN("capture_init: ivc_init(capture-control) failed rc=%d",
             rc);
        return rc;
    }

    rc = camrtc_ivc_init(&g_cap_chan, cap_rx_iova, cap_tx_iova,
                         CAMRTC_CAP_NFRAMES,
                         CAMRTC_CAP_FRAME_SIZE,
                         CAMRTC_CAP_GROUP);
    if (rc != 0) {
        WARN("capture_init: ivc_init(capture) failed rc=%d", rc);
        return rc;
    }

    g_ctrl_ready = true;
    INFO("capture_init: ready — capture-control rx=0x%lx tx=0x%lx, "
         "capture rx=0x%lx tx=0x%lx",
         (unsigned long)ctrl_rx_iova, (unsigned long)ctrl_tx_iova,
         (unsigned long)cap_rx_iova,  (unsigned long)cap_tx_iova);
    return 0;
}

int camrtc_capture_phy_stream_open(uint32_t stream_id,
                                   uint32_t csi_port,
                                   uint32_t phy_type,
                                   uint32_t *out_result)
{
    if (!g_ctrl_ready) {
        WARN("phy_stream_open: capture_init not run");
        return -1;
    }

    /* Build the request frame: header + body, zero-padded by the
     * IVC send path to fill the 320-byte slot. */
    struct {
        struct capture_msg_header        hdr;
        struct capture_phy_stream_open_req body;
    } req;

    /* Allocate a transaction id RCE will echo back. Skip 0 since
     * some L4T paths treat it as "channel not assigned". */
    uint32_t tx = g_next_transaction++;
    if (g_next_transaction == 0u) g_next_transaction = 1u;

    req.hdr.msg_id      = CAPTURE_PHY_STREAM_OPEN_REQ;
    req.hdr.transaction = tx;
    req.body.stream_id  = stream_id;
    req.body.csi_port   = csi_port;
    req.body.phy_type   = phy_type;
    req.body.pad32__    = 0u;

    INFO("phy_stream_open: send REQ tx=0x%x stream=%u port=%u phy=%u",
         (unsigned)tx, (unsigned)stream_id, (unsigned)csi_port,
         (unsigned)phy_type);

    int rc = camrtc_ivc_send(&g_ctrl_chan, &req, sizeof(req));
    if (rc != 0) {
        WARN("phy_stream_open: ivc_send failed rc=%d", rc);
        return -2;
    }

    /* Wait for the response. capture-control responses arrive on
     * the rx ring after RCE drives the SS[0] FW→VM bit; SLM-OS
     * polls instead of waiting for an interrupt. 1 s ceiling
     * matches the L4T `cmd_timeout` default and is the same
     * upper bound `camrtc_send_msg` uses for CH_SETUP. */
    uint8_t resp_buf[CAMRTC_CTRL_FRAME_SIZE];
    uint32_t resp_len = 0;
    rc = camrtc_ivc_recv_wait(&g_ctrl_chan, resp_buf, sizeof(resp_buf),
                              &resp_len, 1000000u);
    if (rc != 0) {
        WARN("phy_stream_open: ivc_recv_wait failed rc=%d", rc);
        return -3;
    }
    if (resp_len < sizeof(struct capture_msg_header)
                   + sizeof(struct capture_phy_stream_open_resp)) {
        WARN("phy_stream_open: short response (%u bytes)",
             (unsigned)resp_len);
        return -4;
    }

    struct capture_msg_header resp_hdr;
    struct capture_phy_stream_open_resp resp_body;
    mem_copy(&resp_hdr, resp_buf, sizeof(resp_hdr));
    mem_copy(&resp_body, resp_buf + sizeof(resp_hdr), sizeof(resp_body));

    if (resp_hdr.msg_id != CAPTURE_PHY_STREAM_OPEN_RESP) {
        WARN("phy_stream_open: wrong msg_id 0x%x (expected 0x%x)",
             (unsigned)resp_hdr.msg_id,
             (unsigned)CAPTURE_PHY_STREAM_OPEN_RESP);
        return -4;
    }
    if (resp_hdr.transaction != tx) {
        WARN("phy_stream_open: tx mismatch 0x%x (sent 0x%x)",
             (unsigned)resp_hdr.transaction, (unsigned)tx);
        return -5;
    }

    if (out_result != (uint32_t *)0) {
        *out_result = resp_body.result;
    }
    INFO("phy_stream_open: RESP tx=0x%x result=0x%x",
         (unsigned)resp_hdr.transaction, (unsigned)resp_body.result);
    return 0;
}

int camrtc_capture_csi_stream_set_config(uint32_t stream_id,
                                         uint32_t csi_port,
                                         uint8_t  num_lanes,
                                         uint32_t mipi_clock_rate,
                                         uint32_t *out_result)
{
    if (!g_ctrl_ready) {
        WARN("csi_stream_set_config: capture_init not run");
        return -1;
    }
    if (num_lanes == 0u || num_lanes > NVCSI_BRICK_NUM_LANES) {
        WARN("csi_stream_set_config: invalid num_lanes=%u",
             (unsigned)num_lanes);
        return -1;
    }

    /* Build the request frame: header + body. The body is 104 B
     * — see camrtc_capture.h for the layout. The IVC send path
     * zero-pads the rest of the 320-byte slot. */
    struct {
        struct capture_msg_header                   hdr;
        struct capture_csi_stream_set_config_req    body;
    } req;

    /* Zero everything first so any reserved/error-mask field we
     * don't explicitly set lands as 0 (= no error reporting,
     * SoC-default timing). */
    uint8_t *req_bytes = (uint8_t *)&req;
    for (uint32_t i = 0; i < sizeof(req); i++) req_bytes[i] = 0;

    uint32_t tx = g_next_transaction++;
    if (g_next_transaction == 0u) g_next_transaction = 1u;

    req.hdr.msg_id      = CAPTURE_CSI_STREAM_SET_CONFIG_REQ;
    req.hdr.transaction = tx;
    req.body.stream_id  = stream_id;
    req.body.csi_port   = csi_port;
    /* config_flags=0, brick.phy_mode=0 (DPHY), lane_swizzle=0,
     * error_config zeroed — all correct from the bulk-zero above. */
    req.body.cil_config.num_lanes       = num_lanes;
    req.body.cil_config.mipi_clock_rate = mipi_clock_rate;
    /* lp_bypass_mode = 1 for D-PHY, per L4T csi5_stream_set_config
     * (`~/slmos-ref/tegra-l4t/l4t-csi5_fops.c:286`):
     *     cil_config.lp_bypass_mode = is_cphy ? 0 : 1;
     * Tells the CIL receiver to expect D-PHY's LP-11 → HS line-state
     * transitions. With 0 the receiver behaves as if C-PHY and never
     * locks on a D-PHY signal — the symptom is no SOF. Hardcoded
     * here because csidiag is D-PHY-only; productize as a parameter
     * if a C-PHY path ever lands. */
    req.body.cil_config.lp_bypass_mode  = 1u;
    /* Lane polarity per IMX219-A binning-mode DT
     * (`tegra234-p3767-camera-p3768-imx219-A.dtbo:mode3:lane_polarity = "6"`).
     * The DT value packs per-lane polarity bits LSB-first; L4T expands
     * `(lane_polarity >> i) & 1` into `brick_config.lane_polarity[i]`
     * (`~/slmos-ref/tegra-l4t/l4t-csi5_fops.c:279-281`).
     * 6 = 0b0110 → lane[0]=0, lane[1]=1, lane[2]=1, lane[3]=0.
     * Wrong polarity on a connected lane decodes the differential
     * signal inverted, no valid packets reach NVCSI. */
    req.body.brick_config.lane_polarity[0] = 0u;
    req.body.brick_config.lane_polarity[1] = 1u;
    req.body.brick_config.lane_polarity[2] = 1u;
    req.body.brick_config.lane_polarity[3] = 0u;
    /* t_hs_settle=0 / SoC default, t_clk_settle=0, cil_clock_rate=0
     * (deprecated upstream) — left at zero per bulk-zero above. The
     * L4T DT specifies cil_settletime=0 for ALL IMX219 modes
     * (`tegra234-p3767-camera-p3768-imx219-A.dtbo`), so the SoC
     * default is correct. */

    INFO("csi_stream_set_config: send REQ tx=0x%x stream=%u port=%u "
         "lanes=%u mipi_kHz=%u",
         (unsigned)tx, (unsigned)stream_id, (unsigned)csi_port,
         (unsigned)num_lanes, (unsigned)mipi_clock_rate);

    int rc = camrtc_ivc_send(&g_ctrl_chan, &req, sizeof(req));
    if (rc != 0) {
        WARN("csi_stream_set_config: ivc_send failed rc=%d", rc);
        return -2;
    }

    uint8_t resp_buf[CAMRTC_CTRL_FRAME_SIZE];
    uint32_t resp_len = 0;
    rc = camrtc_ivc_recv_wait(&g_ctrl_chan, resp_buf, sizeof(resp_buf),
                              &resp_len, 1000000u);
    if (rc != 0) {
        WARN("csi_stream_set_config: ivc_recv_wait failed rc=%d", rc);
        return -3;
    }
    if (resp_len < sizeof(struct capture_msg_header)
                   + sizeof(struct capture_csi_stream_set_config_resp)) {
        WARN("csi_stream_set_config: short response (%u bytes)",
             (unsigned)resp_len);
        return -4;
    }

    struct capture_msg_header resp_hdr;
    struct capture_csi_stream_set_config_resp resp_body;
    mem_copy(&resp_hdr, resp_buf, sizeof(resp_hdr));
    mem_copy(&resp_body, resp_buf + sizeof(resp_hdr), sizeof(resp_body));

    if (resp_hdr.msg_id != CAPTURE_CSI_STREAM_SET_CONFIG_RESP) {
        WARN("csi_stream_set_config: wrong msg_id 0x%x (expected 0x%x)",
             (unsigned)resp_hdr.msg_id,
             (unsigned)CAPTURE_CSI_STREAM_SET_CONFIG_RESP);
        return -4;
    }
    if (resp_hdr.transaction != tx) {
        WARN("csi_stream_set_config: tx mismatch 0x%x (sent 0x%x)",
             (unsigned)resp_hdr.transaction, (unsigned)tx);
        return -5;
    }

    if (out_result != (uint32_t *)0) *out_result = resp_body.result;
    INFO("csi_stream_set_config: RESP tx=0x%x result=0x%x",
         (unsigned)resp_hdr.transaction, (unsigned)resp_body.result);
    return 0;
}

int camrtc_capture_channel_setup(uint32_t stream_id,
                                 uint32_t csi_port,
                                 uint64_t requests_iova,
                                 uint64_t requests_memoryinfo_iova,
                                 uint32_t queue_depth,
                                 uint32_t request_size,
                                 uint32_t memoryinfo_size,
                                 uint32_t *out_result,
                                 uint32_t *out_channel_id,
                                 uint64_t *out_vi_channel_mask)
{
    if (!g_ctrl_ready) {
        WARN("channel_setup: capture_init not run");
        return -1;
    }

    /* Build the request frame: 8-byte header + 272-byte body =
     * 280 B, fits in the 320 B capture-control frame slot. */
    struct {
        struct capture_msg_header                  hdr;
        struct camrtc_capture_channel_setup_req    body;
    } req;

    /* Bulk-zero so all unused fields (vi2_channel_mask, GOS table
     * entries, syncpoint_info structs, error masks) land at zero
     * — RCE treats those as "unused / default policy". */
    uint8_t *req_bytes = (uint8_t *)&req;
    for (uint32_t i = 0; i < sizeof(req); i++) req_bytes[i] = 0;

    uint32_t tx = g_next_transaction++;
    if (g_next_transaction == 0u) g_next_transaction = 1u;

    req.hdr.msg_id      = CAPTURE_CHANNEL_SETUP_REQ;
    req.hdr.transaction = tx;

    struct camrtc_capture_channel_config *cfg = &req.body.channel_config;
    /* EMBDATA matches L4T's vi5 default_setup
     * (`~/slmos-ref/tegra-l4t/l4t-vi5_fops.c:46-52`); without it, RCE
     * configures the VI channel to reject embedded-data lines and
     * raises CHANSEL_EMBED_INFRINGE on every frame for sensors (like
     * IMX219) that emit metadata lines by default. */
    cfg->channel_flags  = CAPTURE_CHANNEL_FLAG_VIDEO
                        | CAPTURE_CHANNEL_FLAG_RAW
                        | CAPTURE_CHANNEL_FLAG_EMBDATA
                        | CAPTURE_CHANNEL_FLAG_CSI;
    /* channel_id stays 0 — RCE assigns it in the response. */
    cfg->vi_unit_id     = VI_UNIT_VI;
    cfg->vi_channel_mask  = ~(uint64_t)0;   /* let RCE pick any */
    /* vi2_channel_mask = 0 from bulk-zero (T234 has no VI2). */
    cfg->csi_stream.stream_id       = stream_id;
    cfg->csi_stream.csi_port        = csi_port;
    /* csi_stream.virtual_channel = 0 from bulk-zero. */
    cfg->requests              = requests_iova;
    cfg->requests_memoryinfo   = requests_memoryinfo_iova;
    cfg->queue_depth           = queue_depth;
    cfg->request_size          = request_size;
    cfg->request_memoryinfo_size = memoryinfo_size;
    cfg->slvsec_stream_main = SLVSEC_STREAM_DISABLED;
    cfg->slvsec_stream_sub  = SLVSEC_STREAM_DISABLED;
    /* num_vi_gos_tables = 0, vi_gos_tables[] = 0,
     * progress_sp / embdata_sp / linetimer_sp = 0,
     * error_mask_* = 0, stop_on_error_notify_bits = 0
     * — all from bulk-zero. */

    INFO("channel_setup: send REQ tx=0x%x stream=%u port=%u "
         "queue=%u req_size=%u meminfo_size=%u "
         "requests_iova=0x%lx meminfo_iova=0x%lx",
         (unsigned)tx, (unsigned)stream_id, (unsigned)csi_port,
         (unsigned)queue_depth, (unsigned)request_size,
         (unsigned)memoryinfo_size,
         (unsigned long)requests_iova,
         (unsigned long)requests_memoryinfo_iova);

    int rc = camrtc_ivc_send(&g_ctrl_chan, &req, sizeof(req));
    if (rc != 0) {
        WARN("channel_setup: ivc_send failed rc=%d", rc);
        return -2;
    }

    uint8_t resp_buf[CAMRTC_CTRL_FRAME_SIZE];
    uint32_t resp_len = 0;
    rc = camrtc_ivc_recv_wait(&g_ctrl_chan, resp_buf, sizeof(resp_buf),
                              &resp_len, 1000000u);
    if (rc != 0) {
        WARN("channel_setup: ivc_recv_wait failed rc=%d", rc);
        return -3;
    }
    if (resp_len < sizeof(struct capture_msg_header)
                   + sizeof(struct camrtc_capture_channel_setup_resp)) {
        WARN("channel_setup: short response (%u bytes)",
             (unsigned)resp_len);
        return -4;
    }

    struct capture_msg_header                  resp_hdr;
    struct camrtc_capture_channel_setup_resp   resp_body;
    mem_copy(&resp_hdr, resp_buf, sizeof(resp_hdr));
    mem_copy(&resp_body, resp_buf + sizeof(resp_hdr), sizeof(resp_body));

    if (resp_hdr.msg_id != CAPTURE_CHANNEL_SETUP_RESP) {
        WARN("channel_setup: wrong msg_id 0x%x (expected 0x%x)",
             (unsigned)resp_hdr.msg_id,
             (unsigned)CAPTURE_CHANNEL_SETUP_RESP);
        return -4;
    }
    if (resp_hdr.transaction != tx) {
        WARN("channel_setup: tx mismatch 0x%x (sent 0x%x)",
             (unsigned)resp_hdr.transaction, (unsigned)tx);
        return -5;
    }

    if (out_result          != (uint32_t *)0) *out_result          = resp_body.result;
    if (out_channel_id      != (uint32_t *)0) *out_channel_id      = resp_body.channel_id;
    if (out_vi_channel_mask != (uint64_t *)0) *out_vi_channel_mask = resp_body.vi_channel_mask;
    INFO("channel_setup: RESP tx=0x%x result=0x%x channel_id=%u "
         "vi_channel_mask=0x%lx",
         (unsigned)resp_hdr.transaction, (unsigned)resp_body.result,
         (unsigned)resp_body.channel_id,
         (unsigned long)resp_body.vi_channel_mask);
    return 0;
}

int camrtc_capture_request(uint32_t buffer_index,
                           uint32_t *out_status_index,
                           uint32_t timeout_us)
{
    if (!g_ctrl_ready) {
        WARN("capture_request: capture_init not run");
        return -1;
    }

    /* CAPTURE_REQUEST_REQ uses the *capture* IVC channel (g_cap_chan),
     * not the *capture-control* channel that PHY_STREAM_OPEN /
     * CSI_SET_CONFIG / CHANNEL_SETUP ride on. The request frame is
     * just an 8-byte header + 8-byte body — well within the capture
     * channel's 64-byte slot. */
    struct {
        struct capture_msg_header        hdr;
        struct camrtc_capture_request_req body;
    } req;

    req.hdr.msg_id      = CAPTURE_REQUEST_REQ;
    req.hdr.transaction = buffer_index;  /* L4T uses buffer_index as
                                          * the transaction id for
                                          * REQUEST/STATUS pairs;
                                          * RCE echoes it back. */
    req.body.buffer_index = buffer_index;
    req.body.pad32__      = 0u;

    INFO("capture_request: send REQ buffer_index=%u",
         (unsigned)buffer_index);

    int rc = camrtc_ivc_send(&g_cap_chan, &req, sizeof(req));
    if (rc != 0) {
        WARN("capture_request: ivc_send failed rc=%d", rc);
        return -2;
    }

    /* Poll the capture rx ring for STATUS_IND. The capture channel
     * frame size (CAMRTC_CAP_FRAME_SIZE in camrtc_layout.h) is
     * 64 B vs the 320 B on capture-control, so the stack buffer
     * is much smaller. */
    uint8_t resp_buf[CAMRTC_CAP_FRAME_SIZE];
    uint32_t resp_len = 0;
    rc = camrtc_ivc_recv_wait(&g_cap_chan, resp_buf, sizeof(resp_buf),
                              &resp_len, timeout_us);
    if (rc != 0) {
        WARN("capture_request: ivc_recv_wait failed rc=%d "
             "(timeout_us=%u)", rc, (unsigned)timeout_us);
        return -3;
    }
    if (resp_len < sizeof(struct capture_msg_header)
                   + sizeof(struct camrtc_capture_status_ind)) {
        WARN("capture_request: short STATUS_IND (%u bytes)",
             (unsigned)resp_len);
        return -4;
    }

    struct capture_msg_header             resp_hdr;
    struct camrtc_capture_status_ind      resp_body;
    mem_copy(&resp_hdr, resp_buf, sizeof(resp_hdr));
    mem_copy(&resp_body, resp_buf + sizeof(resp_hdr), sizeof(resp_body));

    if (resp_hdr.msg_id != CAPTURE_STATUS_IND) {
        WARN("capture_request: wrong msg_id 0x%x (expected 0x%x)",
             (unsigned)resp_hdr.msg_id, (unsigned)CAPTURE_STATUS_IND);
        return -4;
    }
    if (resp_body.buffer_index != buffer_index) {
        WARN("capture_request: buffer_index mismatch %u (sent %u)",
             (unsigned)resp_body.buffer_index,
             (unsigned)buffer_index);
        return -5;
    }

    if (out_status_index != (uint32_t *)0) {
        *out_status_index = resp_body.buffer_index;
    }
    INFO("capture_request: STATUS_IND buffer_index=%u "
         "(inspect descriptor.capture_status for the per-frame result)",
         (unsigned)resp_body.buffer_index);
    return 0;
}

#else /* !PLATFORM_JETSON_ORIN_NANO — stubs for cross-platform builds */

int camrtc_capture_init(void) { return -1; }
int camrtc_capture_phy_stream_open(uint32_t stream_id,
                                   uint32_t csi_port,
                                   uint32_t phy_type,
                                   uint32_t *out_result)
{
    (void)stream_id; (void)csi_port; (void)phy_type;
    if (out_result) *out_result = 0xFFFFFFFFu;
    return -1;
}
int camrtc_capture_csi_stream_set_config(uint32_t stream_id,
                                         uint32_t csi_port,
                                         uint8_t  num_lanes,
                                         uint32_t mipi_clock_rate,
                                         uint32_t *out_result)
{
    (void)stream_id; (void)csi_port; (void)num_lanes; (void)mipi_clock_rate;
    if (out_result) *out_result = 0xFFFFFFFFu;
    return -1;
}
int camrtc_capture_channel_setup(uint32_t stream_id,
                                 uint32_t csi_port,
                                 uint64_t requests_iova,
                                 uint64_t requests_memoryinfo_iova,
                                 uint32_t queue_depth,
                                 uint32_t request_size,
                                 uint32_t memoryinfo_size,
                                 uint32_t *out_result,
                                 uint32_t *out_channel_id,
                                 uint64_t *out_vi_channel_mask)
{
    (void)stream_id; (void)csi_port;
    (void)requests_iova; (void)requests_memoryinfo_iova;
    (void)queue_depth; (void)request_size; (void)memoryinfo_size;
    if (out_result)          *out_result          = 0xFFFFFFFFu;
    if (out_channel_id)      *out_channel_id      = 0xFFFFFFFFu;
    if (out_vi_channel_mask) *out_vi_channel_mask = 0u;
    return -1;
}
int camrtc_capture_request(uint32_t buffer_index,
                           uint32_t *out_status_index,
                           uint32_t timeout_us)
{
    (void)buffer_index; (void)timeout_us;
    if (out_status_index) *out_status_index = 0xFFFFFFFFu;
    return -1;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
