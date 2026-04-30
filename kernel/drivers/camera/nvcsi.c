/*
 * nvcsi.c — Tegra234 NVCSI (MIPI CSI-2) receiver driver implementation.
 *
 * Direct-MMIO bring-up using the historical T194 csi4_fops sequence
 * cached at `../slmos-reference-cache/tegra-l4t/l4t-csi4_fops.c`. The Tegra234 NVCSI
 * register layout matches T194 even though L4T R35 routes everything
 * through RTCPU — see `docs/jetson-camera-nvcsi-driver-notes.md` for
 * the code-read evidence.
 *
 * Port → (PHY brick, CIL half) mapping (per L4T `csi4_fops`):
 *   csi_port = NVCSI_PORT_A..H (0..7)
 *   phy_brick = csi_port >> 1     (0..3)
 *   cil_half  = csi_port & 1      (0 = CIL_A, 1 = CIL_B)
 *
 * The IMX219-A overlay on the Orin Nano dev kit uses
 * `port-index = <0x01>` = NVCSI_PORT_B = (PHY 0, CIL_B). NOT CIL_A
 * — verified live against `/proc/device-tree/.../nvcsi@15a00000`.
 *
 * Register layout (from `../slmos-reference-cache/tegra-l4t/l4t-csi4_registers.h`):
 *   PHY brick base   = NVCSI_BASE + 0x18000 + brick*0x10000
 *     +0x00 NVCSI_CIL_PHY_CTRL          (0 = DPHY, 1 = CPHY)
 *     +0x04 NVCSI_CIL_CONFIG            (lane count for both halves)
 *     +0x0C NVCSI_CIL_PAD_CONFIG        (PDVCLAMP for unused brick)
 *     CIL_A side: SW_RESET 0x18, PAD_CONFIG 0x20, CONTROL 0x5C
 *     CIL_B side: SW_RESET 0x7C, PAD_CONFIG 0x84, CONTROL 0xC0
 *
 *   Stream block base = NVCSI_BASE + 0x10000 + stream*0x800
 *     +0x08 PP_EN_CTRL                  (CFG_PP_EN bit 0)
 *     +0x20 VC0_DT_OVERRIDE
 *     +0x6C PPFSM_TIMEOUT_CTRL
 *     +0x70 PH_CHK_CTRL                 (header CRC + ECC enable)
 *     +0x74 VC0_DPCM_CTRL
 *     +0x90 ERROR_STATUS2VI_MASK
 *     +0xA4 INTR_STATUS                 (W1C, 0x3FFFF)
 *     +0xA8 INTR_MASK
 *     +0xAC ERR_INTR_STATUS             (W1C, 0x7FFFF)
 *     +0xB0 ERR_INTR_MASK
 *     +0x400 CILA_INTR_STATUS / +0x404 MASK / +0x408 ERR / +0x40C ERR_MASK
 *     +0xC00 CILB_INTR_STATUS / +0xC04 MASK / +0xC08 ERR / +0xC0C ERR_MASK
 */

#include "nvcsi.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include <stdint.h>

#include "bpmp.h"
#include "debug.h"
#include "platform.h"
#include "tegra234_clocks.h"

/* ---- Block bases (from l4t-csi4_registers.h) ---- */
#define NVCSI_PHY_BASE_OFF        0x18000u
#define NVCSI_PHY_STRIDE          0x10000u
#define NVCSI_STREAM_BASE_OFF     0x10000u
#define NVCSI_STREAM_STRIDE       0x800u

/* ---- Per-PHY register offsets (CIL_A and CIL_B halves) ---- */
#define NVCSI_CIL_PHY_CTRL        0x00u   /* 0=DPHY, 1=CPHY */
#define NVCSI_CIL_CONFIG          0x04u
#define NVCSI_CIL_PAD_CONFIG      0x0Cu
#define NVCSI_CIL_A_SW_RESET      0x18u
#define NVCSI_CIL_A_PAD_CONFIG    0x20u
#define NVCSI_CIL_A_CONTROL       0x5Cu
#define NVCSI_CIL_B_SW_RESET      0x7Cu
#define NVCSI_CIL_B_PAD_CONFIG    0x84u
#define NVCSI_CIL_B_CONTROL       0xC0u

/* ---- Per-stream register offsets ---- */
#define NVCSI_PP_EN_CTRL          0x08u
#define NVCSI_VC0_DT_OVERRIDE     0x20u
#define NVCSI_PPFSM_TIMEOUT_CTRL  0x6Cu
#define NVCSI_PH_CHK_CTRL         0x70u
#define NVCSI_VC0_DPCM_CTRL       0x74u
#define NVCSI_ERROR_STATUS2VI_MASK 0x90u
#define NVCSI_INTR_STATUS         0xA4u
#define NVCSI_INTR_MASK           0xA8u
#define NVCSI_ERR_INTR_STATUS     0xACu
#define NVCSI_ERR_INTR_MASK       0xB0u
#define NVCSI_CILA_INTR_STATUS    0x400u
#define NVCSI_CILA_INTR_MASK      0x404u
#define NVCSI_CILA_ERR_INTR_STATUS 0x408u
#define NVCSI_CILA_ERR_INTR_MASK  0x40Cu
#define NVCSI_CILB_INTR_STATUS    0xC00u
#define NVCSI_CILB_INTR_MASK      0xC04u
#define NVCSI_CILB_ERR_INTR_STATUS 0xC08u
#define NVCSI_CILB_ERR_INTR_MASK  0xC0Cu

/* ---- Bit fields ---- */
#define NVCSI_DPHY                0u
#define NVCSI_DATA_LANE_A_SHIFT   0u
#define NVCSI_DATA_LANE_B_SHIFT   8u
#define NVCSI_DATA_LANE_A_MASK    (0x7u << NVCSI_DATA_LANE_A_SHIFT)
#define NVCSI_DATA_LANE_B_MASK    (0x7u << NVCSI_DATA_LANE_B_SHIFT)

#define NVCSI_SW_RESET0_EN        (1u << 0)
#define NVCSI_SW_RESET1_EN        (1u << 1)
#define NVCSI_SW_RESET_BOTH       (NVCSI_SW_RESET0_EN | NVCSI_SW_RESET1_EN)

#define NVCSI_PD_IO0              (1u << 16)
#define NVCSI_PD_IO1              (1u << 17)
#define NVCSI_PD_CLK              (1u << 18)
#define NVCSI_SPARE_IO0           (1u << 0)
#define NVCSI_SPARE_IO1           (1u << 4)
#define NVCSI_PAD_PARK_ALL        (NVCSI_PD_CLK | NVCSI_PD_IO0 | NVCSI_PD_IO1 \
                                   | NVCSI_SPARE_IO0 | NVCSI_SPARE_IO1)

#define NVCSI_E_INPUT_LP_CLK      (1u << 20)
#define NVCSI_E_INPUT_LP_IO0      (1u << 21)
#define NVCSI_E_INPUT_LP_IO1      (1u << 22)
#define NVCSI_E_INPUT_LP_ENABLE   (NVCSI_E_INPUT_LP_CLK | NVCSI_E_INPUT_LP_IO0 \
                                   | NVCSI_E_INPUT_LP_IO1)

#define NVCSI_PDVCLAMP            (1u << 9)

#define NVCSI_CFG_PP_EN           (1u << 0)
#define NVCSI_CFG_PH_ECC_CHK_EN   (1u << 0)
#define NVCSI_CFG_PH_CRC_CHK_EN   (1u << 1)
#define NVCSI_PH_CHK_BOTH         (NVCSI_CFG_PH_CRC_CHK_EN \
                                   | NVCSI_CFG_PH_ECC_CHK_EN)

/* CIL_*_CONTROL field layout (l4t-csi4_registers.h:189-201). */
#define NVCSI_DESKEW_COMPARE_SHIFT  16u
#define NVCSI_DESKEW_SETTLE_SHIFT   20u
#define NVCSI_CLK_SETTLE_SHIFT      8u
#define NVCSI_THS_SETTLE_SHIFT      0u
#define NVCSI_T18X_BYPASS_LP_SEQ_SHIFT 7u

#define NVCSI_DEFAULT_DESKEW_COMPARE  (0x4u << NVCSI_DESKEW_COMPARE_SHIFT)
#define NVCSI_DEFAULT_DESKEW_SETTLE   (0x6u << NVCSI_DESKEW_SETTLE_SHIFT)
#define NVCSI_DEFAULT_DPHY_CLK_SETTLE (0x21u << NVCSI_CLK_SETTLE_SHIFT)
#define NVCSI_DEFAULT_THS_SETTLE      (0x14u << NVCSI_THS_SETTLE_SHIFT)
#define NVCSI_T18X_BYPASS_LP_SEQ      (1u << NVCSI_T18X_BYPASS_LP_SEQ_SHIFT)

/* Mask-all values for the W1C status registers. */
#define NVCSI_INTR_STATUS_ALL     0x3FFFFu
#define NVCSI_ERR_INTR_STATUS_ALL 0x7FFFFu

/* Per-half register offset bundle so the same code path can program
 * either CIL_A or CIL_B without branching at every write. */
struct cil_regs {
    uint32_t sw_reset;
    uint32_t pad_config;
    uint32_t control;
    uint32_t lane_mask;
    uint32_t lane_shift;
    uint32_t intr_status;
    uint32_t intr_mask;
    uint32_t err_intr_status;
    uint32_t err_intr_mask;
};

static const struct cil_regs cil_a_regs = {
    .sw_reset       = NVCSI_CIL_A_SW_RESET,
    .pad_config     = NVCSI_CIL_A_PAD_CONFIG,
    .control        = NVCSI_CIL_A_CONTROL,
    .lane_mask      = NVCSI_DATA_LANE_A_MASK,
    .lane_shift     = NVCSI_DATA_LANE_A_SHIFT,
    .intr_status    = NVCSI_CILA_INTR_STATUS,
    .intr_mask      = NVCSI_CILA_INTR_MASK,
    .err_intr_status = NVCSI_CILA_ERR_INTR_STATUS,
    .err_intr_mask  = NVCSI_CILA_ERR_INTR_MASK,
};

static const struct cil_regs cil_b_regs = {
    .sw_reset       = NVCSI_CIL_B_SW_RESET,
    .pad_config     = NVCSI_CIL_B_PAD_CONFIG,
    .control        = NVCSI_CIL_B_CONTROL,
    .lane_mask      = NVCSI_DATA_LANE_B_MASK,
    .lane_shift     = NVCSI_DATA_LANE_B_SHIFT,
    .intr_status    = NVCSI_CILB_INTR_STATUS,
    .intr_mask      = NVCSI_CILB_INTR_MASK,
    .err_intr_status = NVCSI_CILB_ERR_INTR_STATUS,
    .err_intr_mask  = NVCSI_CILB_ERR_INTR_MASK,
};

static const struct cil_regs *cil_regs_for(uint32_t cil_half)
{
    return cil_half == 0u ? &cil_a_regs : &cil_b_regs;
}

/* ---- Pre-configured ports ---- */

struct nvcsi_port nvcsi_imx219_a_port = {
    .phy_brick      = 0u,
    .cil_half       = 1u,        /* CIL_B — verified from L4T DT
                                  * port-index = <0x01> = NVCSI_PORT_B
                                  * = (PHY 0, CIL_B) on the Orin Nano
                                  * IMX219-A overlay. */
    .num_data_lanes = 2u,
    .mipi_clk_mhz   = 456u,      /* IMX219 binned-mode link clock */
    .name           = "imx219_a",
};

/* ---- MMIO accessors ---- */

static inline uintptr_t nvcsi_phy_addr(const struct nvcsi_port *port,
                                       uint32_t off)
{
    return (uintptr_t)TEGRA234_NVCSI_BASE
         + NVCSI_PHY_BASE_OFF
         + port->phy_brick * NVCSI_PHY_STRIDE
         + off;
}

static inline uintptr_t nvcsi_stream_addr(const struct nvcsi_port *port,
                                          uint32_t off)
{
    /* L4T's csi4_fops uses the csi_port value (= phy*2 + cil_half)
     * as the stream index. PHY 0 / CIL_A → stream 0, PHY 0 / CIL_B
     * → stream 1, PHY 1 / CIL_A → stream 2, etc. */
    uint32_t stream_idx = port->phy_brick * 2u + port->cil_half;
    return (uintptr_t)TEGRA234_NVCSI_BASE
         + NVCSI_STREAM_BASE_OFF
         + stream_idx * NVCSI_STREAM_STRIDE
         + off;
}

static inline void nvcsi_phy_write(const struct nvcsi_port *port,
                                   uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)nvcsi_phy_addr(port, off) = v;
    /* Read-back to flush. */
    (void)*(volatile uint32_t *)nvcsi_phy_addr(port, off);
}

static inline uint32_t nvcsi_phy_read(const struct nvcsi_port *port,
                                      uint32_t off)
{
    return *(volatile uint32_t *)nvcsi_phy_addr(port, off);
}

static inline void nvcsi_stream_write(const struct nvcsi_port *port,
                                      uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)nvcsi_stream_addr(port, off) = v;
    /* Read-back to flush. */
    (void)*(volatile uint32_t *)nvcsi_stream_addr(port, off);
}

static inline uint32_t nvcsi_stream_read(const struct nvcsi_port *port,
                                         uint32_t off)
{
    return *(volatile uint32_t *)nvcsi_stream_addr(port, off);
}

/* ---- Public API ---- */

int nvcsi_stream_init(const struct nvcsi_port *port)
{
    if (!port) return -1;
    if (port->num_data_lanes != 2u) {
        WARN("nvcsi: only 2-lane DPHY supported (got %u)",
             (unsigned)port->num_data_lanes);
        return -1;
    }
    if (port->cil_half > 1u) {
        WARN("nvcsi: cil_half must be 0 (CIL_A) or 1 (CIL_B), got %u",
             (unsigned)port->cil_half);
        return -1;
    }
    if (port->phy_brick > 3u) {
        WARN("nvcsi: phy_brick must be 0..3, got %u",
             (unsigned)port->phy_brick);
        return -1;
    }

    const struct cil_regs *active = cil_regs_for(port->cil_half);
    const struct cil_regs *unused = cil_regs_for(port->cil_half ^ 1u);

    /* Step 1a: enable the VI power domain. Linux's nvhost runtime PM
     * idle-suspends the camera subsystem when no v4l2 client is
     * streaming, and slmos-kexec doesn't keep it powered up across
     * the handoff. Without this the NVCSI MMIO window reads back
     * 0xFFFFFFFF (the bus has no responder while the domain is gated).
     * MRQ_PG is idempotent — if Linux had it on, the call is a no-op. */
    int rc = bpmp_pg_set_state(TEGRA234_POWER_DOMAIN_VI, true);
    if (rc != 0) {
        WARN("nvcsi: bpmp_pg_set_state(VI) rc=%d", rc);
        return -2;
    }

    /* Step 1b: enable NVCSI clock. Idempotent. */
    rc = bpmp_clk_enable(TEGRA234_CLK_NVCSI);
    if (rc != 0) {
        WARN("nvcsi: bpmp_clk_enable(NVCSI) rc=%d", rc);
        return -2;
    }

    /* Step 1c: deassert NVCSI reset. The L4T DT still lists
     * `resets = <&bpmp_resets TEGRA234_RESET_NVCSI>` and
     * nvhost_module_busy() walks the reset list before any MMIO
     * access. Without this, the NVCSI block stays in reset and
     * MMIO reads return 0xFFFFFFFF (Phase-0 thinks "CBB firewall"
     * but the real cause is the gated reset). */
    rc = bpmp_reset_deassert(TEGRA234_RESET_NVCSI);
    if (rc != 0) {
        WARN("nvcsi: bpmp_reset_deassert(NVCSI) rc=%d", rc);
        /* Not fatal — the block may already be out of reset.
         * Continue and let the CIL_CONFIG readback verify. */
    }

    /* Step 2: PHY mode select — DPHY. */
    nvcsi_phy_write(port, NVCSI_CIL_PHY_CTRL, NVCSI_DPHY);

    /* Step 3: clear active half's lane count before reconfiguring. */
    uint32_t cil_config = nvcsi_phy_read(port, NVCSI_CIL_CONFIG);
    cil_config &= ~active->lane_mask;
    nvcsi_phy_write(port, NVCSI_CIL_CONFIG, cil_config);

    /* Step 4: assert active-half soft reset. */
    nvcsi_phy_write(port, active->sw_reset, NVCSI_SW_RESET_BOTH);

    /* Step 5: park active-half pads. */
    nvcsi_phy_write(port, active->pad_config, NVCSI_PAD_PARK_ALL);

    /* Step 6: park unused half (sw-reset + pad-park). */
    nvcsi_phy_write(port, unused->sw_reset, NVCSI_SW_RESET_BOTH);
    nvcsi_phy_write(port, unused->pad_config, NVCSI_PAD_PARK_ALL);

    /* Step 7: power down brick de-serializer (since unused half stays
     * parked). */
    nvcsi_phy_write(port, NVCSI_CIL_PAD_CONFIG, NVCSI_PDVCLAMP);

    /* Step 8: power on de-serializer (now that pad reset has settled). */
    nvcsi_phy_write(port, NVCSI_CIL_PAD_CONFIG, 0u);

    /* Step 9-12: program active-half lane count, LP enable, control word.
     * The L4T defaults (DEFAULT_DPHY_CLK_SETTLE | DEFAULT_THS_SETTLE
     * | T18X_BYPASS_LP_SEQ | DEFAULT_DESKEW_*) match a 912 Mbps/lane
     * IMX219 DPHY stream. csi4_phy_config() in the reference doesn't
     * recompute settle times for sensors below 1.5 Gbps/lane. */
    cil_config = nvcsi_phy_read(port, NVCSI_CIL_CONFIG);
    cil_config = (cil_config & ~active->lane_mask)
               | (port->num_data_lanes << active->lane_shift);
    nvcsi_phy_write(port, NVCSI_CIL_CONFIG, cil_config);

    nvcsi_phy_write(port, active->pad_config, NVCSI_E_INPUT_LP_ENABLE);

    uint32_t control_word =
        NVCSI_DEFAULT_DESKEW_COMPARE
      | NVCSI_DEFAULT_DESKEW_SETTLE
      | NVCSI_DEFAULT_DPHY_CLK_SETTLE
      | NVCSI_T18X_BYPASS_LP_SEQ
      | NVCSI_DEFAULT_THS_SETTLE;
    nvcsi_phy_write(port, active->control, control_word);

    /* Step 13: release active-half soft reset. */
    nvcsi_phy_write(port, active->sw_reset, 0u);

    /* Verify the lane-count write actually landed — this is the
     * cheapest CBB-firewall check we can make. If the readback
     * doesn't match, NVCSI MMIO is being filtered and we should
     * surface that loudly rather than silently failing later. */
    uint32_t verify = nvcsi_phy_read(port, NVCSI_CIL_CONFIG);
    if ((verify & active->lane_mask)
        != (port->num_data_lanes << active->lane_shift)) {
        WARN("nvcsi: CIL_CONFIG readback 0x%x mismatch "
             "(expected lanes=%u in %s)",
             (unsigned)verify, (unsigned)port->num_data_lanes,
             port->cil_half ? "CIL_B" : "CIL_A");
        return -3;
    }

    /* Step 14: clear active-half stream-block status registers (W1C).
     * All CIL interrupts are masked because frame-done is VI's job. */
    nvcsi_stream_write(port, active->intr_status,     0xFFFFFFFFu);
    nvcsi_stream_write(port, active->err_intr_status, 0xFFFFFFFFu);
    nvcsi_stream_write(port, active->intr_mask,       0xFFFFFFFFu);
    nvcsi_stream_write(port, active->err_intr_mask,   0xFFFFFFFFu);
    nvcsi_stream_write(port, NVCSI_INTR_STATUS,          NVCSI_INTR_STATUS_ALL);
    nvcsi_stream_write(port, NVCSI_ERR_INTR_STATUS,      NVCSI_ERR_INTR_STATUS_ALL);
    nvcsi_stream_write(port, NVCSI_ERROR_STATUS2VI_MASK, 0u);
    nvcsi_stream_write(port, NVCSI_INTR_MASK,            0u);
    nvcsi_stream_write(port, NVCSI_ERR_INTR_MASK,        0u);

    /* Step 15: stream config — header check enable, no DPCM, RAW10
     * auto-detect from packet header. */
    nvcsi_stream_write(port, NVCSI_PPFSM_TIMEOUT_CTRL, 0u);
    nvcsi_stream_write(port, NVCSI_PH_CHK_CTRL,        NVCSI_PH_CHK_BOTH);
    nvcsi_stream_write(port, NVCSI_VC0_DPCM_CTRL,      0u);
    nvcsi_stream_write(port, NVCSI_VC0_DT_OVERRIDE,    0u);

    /* Step 16: enable pixel parser. Receiver is now armed. */
    nvcsi_stream_write(port, NVCSI_PP_EN_CTRL, NVCSI_CFG_PP_EN);

    return 0;
}

void nvcsi_stream_stop(const struct nvcsi_port *port)
{
    if (!port) return;

    const struct cil_regs *active = cil_regs_for(port->cil_half);

    /* Disable pixel parser first so any in-flight packet completes
     * before we reset CIL. */
    nvcsi_stream_write(port, NVCSI_PP_EN_CTRL, 0u);

    /* Re-assert active-half soft reset for clean teardown. */
    nvcsi_phy_write(port, active->sw_reset, NVCSI_SW_RESET_BOTH);

    /* Park active-half pads. */
    nvcsi_phy_write(port, active->pad_config, NVCSI_PAD_PARK_ALL);
}

int nvcsi_get_intr_status(const struct nvcsi_port *port,
                          uint32_t *intr, uint32_t *err)
{
    if (!port || !intr || !err) return -1;
    *intr = nvcsi_stream_read(port, NVCSI_INTR_STATUS);
    *err  = nvcsi_stream_read(port, NVCSI_ERR_INTR_STATUS);
    return 0;
}

#else /* !PLATFORM_JETSON_ORIN_NANO — stubs for cross-platform builds */

struct nvcsi_port nvcsi_imx219_a_port = {
    .phy_brick = 0, .cil_half = 0, .num_data_lanes = 0,
    .mipi_clk_mhz = 0, .name = "stub",
};

int nvcsi_stream_init(const struct nvcsi_port *port)
{ (void)port; return -1; }

void nvcsi_stream_stop(const struct nvcsi_port *port)
{ (void)port; }

int nvcsi_get_intr_status(const struct nvcsi_port *port,
                          uint32_t *intr, uint32_t *err)
{
    /* Match the Jetson-side arg-validation contract: bail before
     * writing anything if any required pointer is NULL. */
    if (!port || !intr || !err) return -1;
    *intr = 0xFFFFFFFFu;
    *err  = 0xFFFFFFFFu;
    return -1;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
