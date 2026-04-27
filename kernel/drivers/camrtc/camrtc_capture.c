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
#include "debug.h"
#include "platform.h"

/* CH_SETUP geometry mirrored from camrtc.c. The CH_SETUP region is
 * laid out as TLV (4 KB) + rx queue + tx queue, with rx_iova at
 * +CAMRTC_IVC_CONFIG_SIZE and tx_iova at
 * +CAMRTC_IVC_CONFIG_SIZE + (TEGRA_IVC_HEADER_SIZE + 64*320). */
#define CAPTURE_CTRL_NFRAMES      64u
#define CAPTURE_CTRL_FRAME_SIZE   320u
#define CAPTURE_CTRL_GROUP        1u
#define CAPTURE_CTRL_QUEUE_BYTES \
    (TEGRA_IVC_HEADER_SIZE + CAPTURE_CTRL_NFRAMES * CAPTURE_CTRL_FRAME_SIZE)

/* ---- Module state ---- */

static struct camrtc_ivc_channel g_ctrl_chan;
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
    uintptr_t rx_iova = region + CAMRTC_IVC_CONFIG_SIZE;
    uintptr_t tx_iova = rx_iova + CAPTURE_CTRL_QUEUE_BYTES;

    rc = camrtc_ivc_init(&g_ctrl_chan, rx_iova, tx_iova,
                         CAPTURE_CTRL_NFRAMES,
                         CAPTURE_CTRL_FRAME_SIZE,
                         CAPTURE_CTRL_GROUP);
    if (rc != 0) {
        WARN("capture_init: ivc_init failed rc=%d", rc);
        return rc;
    }

    g_ctrl_ready = true;
    INFO("capture_init: ready — capture-control channel up "
         "(rx=0x%lx, tx=0x%lx)",
         (unsigned long)rx_iova, (unsigned long)tx_iova);
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
    uint8_t resp_buf[CAPTURE_CTRL_FRAME_SIZE];
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
     * lane_polarity[]=0, error_config zeroed — all correct from
     * the bulk-zero above. */
    req.body.cil_config.num_lanes       = num_lanes;
    req.body.cil_config.mipi_clock_rate = mipi_clock_rate;
    /* lp_bypass_mode=0, t_hs_settle=0/SoC default, t_clk_settle=0,
     * cil_clock_rate=0 (deprecated upstream) — also from bulk zero. */

    INFO("csi_stream_set_config: send REQ tx=0x%x stream=%u port=%u "
         "lanes=%u mipi_kHz=%u",
         (unsigned)tx, (unsigned)stream_id, (unsigned)csi_port,
         (unsigned)num_lanes, (unsigned)mipi_clock_rate);

    int rc = camrtc_ivc_send(&g_ctrl_chan, &req, sizeof(req));
    if (rc != 0) {
        WARN("csi_stream_set_config: ivc_send failed rc=%d", rc);
        return -2;
    }

    uint8_t resp_buf[CAPTURE_CTRL_FRAME_SIZE];
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

#endif /* PLATFORM_JETSON_ORIN_NANO */
