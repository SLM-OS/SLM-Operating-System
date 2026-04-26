/*
 * nvcsi.h — Tegra234 NVCSI (MIPI CSI-2) receiver driver.
 *
 * Configures one CSI port for a 2-lane DPHY RAW10 stream from an
 * IMX219 sensor. After `nvcsi_stream_init` returns 0, NVCSI is armed
 * and waiting for D-PHY packets — actual frames begin once the sensor
 * is told to start streaming over I²C (`imx219_stream_on`, future).
 *
 * Architecture: direct MMIO (Option A from
 * `docs/jetson-camera-imx219-plan.md` §3). The historical T194
 * `csi4_fops` programming sequence in
 * `docs/reference/l4t-csi4_fops.c` applies unchanged on T234 because
 * the underlying NVCSI hardware layout was preserved across SoC
 * generations even though L4T R35 routes everything through RTCPU.
 * The 20-step bring-up procedure is documented in
 * `docs/jetson-camera-nvcsi-driver-notes.md` §"Init sequence".
 *
 * Scope (minimal for #396 Hardware Task 3):
 *   - Single CSI port at a time.
 *   - 2 D-PHY data lanes + 1 D-PHY clock lane.
 *   - RAW10 datatype, no virtual-channel switching, no DPCM.
 *   - Stream on / stream off only; no run-time reconfig.
 *
 * The CSI port the IMX219 actually uses on the Orin Nano dev kit
 * carrier is determined by which connector the camera ribbon plugs
 * into. The L4T DT for the IMX219-A overlay maps connector A (J17)
 * to CSI-A on PHY brick 0 — the default configured by
 * `nvcsi_stream_init_imx219_a` below.
 *
 * Jetson-only (`PLATFORM_JETSON_ORIN_NANO`); other platforms link
 * stubs that return -1 from every function.
 */

#pragma once

#include <stdint.h>

/* Identifies one CSI port at the NVCSI layer. The port maps onto a
 * (PHY brick, CIL half) pair: `{phy=0, cil=A}` is what the L4T
 * IMX219-A overlay uses on the Orin Nano dev kit (J17 connector). */
struct nvcsi_port {
    uint32_t phy_brick;       /* 0..3 — which D-PHY brick */
    uint32_t cil_half;        /* 0 = CIL_A, 1 = CIL_B */
    uint32_t num_data_lanes;  /* 1, 2, or 4 (this driver: 2) */
    uint32_t mipi_clk_mhz;    /* sensor's link clock — used to
                                 derive THS_SETTLE / CLK_SETTLE */
    const char *name;         /* short label for diagnostic prints */
};

/* Pre-configured port matching the L4T IMX219-A overlay: CSI-A on
 * PHY brick 0, 2 lanes, 456 MHz link clock (912 Mbps/lane DPHY). */
extern struct nvcsi_port nvcsi_imx219_a_port;

/*
 * Bring the NVCSI receiver up for the given port. Runs steps 1-16
 * of the bring-up sequence (D-PHY config + stream config + pixel-
 * parser enable). On return:
 *   - port's CIL is configured for the requested lane count.
 *   - Pixel parser is enabled (CFG_PP_EN = 1).
 *   - All CIL interrupts are masked (frame-done is VI's job).
 *   - INTR_STATUS / ERR_INTR_STATUS are cleared of any latched bits.
 *
 * Pre-conditions:
 *   - BPMP has enabled `TEGRA234_CLK_NVCSI` (this function does so
 *     idempotently if not already enabled).
 *   - NVCSI MMIO at TEGRA234_NVCSI_BASE is mapped (vmm.c handles).
 *   - The sensor is out of reset and has stable XCLK
 *     (`imx219_power_on()` returned 0).
 *
 * Returns 0 on success, negative on error:
 *   -1  invalid port pointer or unsupported lane count
 *   -2  BPMP clock enable failed
 *   -3  CIL register readback verification failed
 *      (the write was made but the value didn't latch — typically a
 *      CBB firewall block on a specific register window)
 */
int nvcsi_stream_init(const struct nvcsi_port *port);

/*
 * Stop the NVCSI receiver: clear PP_EN_CTRL, re-assert CIL soft
 * reset for a clean teardown. Errors are logged but not propagated;
 * stream stop is best-effort cleanup. The sensor must be told to
 * stop streaming (write IMX219 `MODE_SELECT = 0` over I²C) BEFORE
 * calling this — disabling NVCSI while packets are in flight will
 * leave the stream parser in an undefined state.
 */
void nvcsi_stream_stop(const struct nvcsi_port *port);

/*
 * Read the live `INTR_STATUS` and `ERR_INTR_STATUS` registers for
 * the port's stream block. Useful as a smoke test after init: both
 * should read 0 when the receiver is idle and waiting. Non-zero on
 * an inactive receiver indicates a CBB firewall block (reads return
 * 0xFFFFFFFF instead of true register state).
 *
 * Returns 0 on success, -1 on bad arguments. Sets *intr and *err to
 * 0xFFFFFFFF on platforms without NVCSI (stub branch).
 */
int nvcsi_get_intr_status(const struct nvcsi_port *port,
                          uint32_t *intr, uint32_t *err);
