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
 * to NVCSI_PORT_B = (PHY brick 0, CIL_B) — `port-index = <0x01>`
 * in `/proc/device-tree/.../nvcsi@15a00000`. Despite the
 * "connector A" label, the underlying PHY half is CIL_B, not CIL_A.
 * Driven by `nvcsi_stream_init(&nvcsi_imx219_a_port)` below.
 *
 * Jetson-only (`PLATFORM_JETSON_ORIN_NANO`); other platforms link
 * stubs that return -1 from every function.
 */

#pragma once

#include <stdint.h>

/* Identifies one CSI port at the NVCSI layer. The port maps onto a
 * (PHY brick, CIL half) pair: the L4T IMX219-A overlay on the Orin
 * Nano dev kit uses `{phy=0, cil=B}` (NVCSI_PORT_B) for the J17
 * connector — see the file-header comment above for the source. */
struct nvcsi_port {
    uint32_t phy_brick;       /* 0..3 — which D-PHY brick */
    uint32_t cil_half;        /* 0 = CIL_A, 1 = CIL_B */
    uint32_t num_data_lanes;  /* 1, 2, or 4 (this driver: 2) */
    uint32_t mipi_clk_mhz;    /* sensor's link clock — used to
                                 derive THS_SETTLE / CLK_SETTLE */
    const char *name;         /* short label for diagnostic prints */
};

/* Pre-configured port matching the L4T IMX219-A overlay:
 * NVCSI_PORT_B = (PHY brick 0, CIL_B), 2 lanes, 456 MHz link clock
 * (912 Mbps/lane DPHY). The "_a" suffix is the connector name
 * (J17 = camera-A), not the CIL half. */
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
 *   - BPMP IPC is up (this function calls `bpmp_pg_set_state(VI)` +
 *     `bpmp_clk_enable(NVCSI)` + `bpmp_reset_deassert(NVCSI)`
 *     idempotently — Linux's nvhost runtime PM idle-suspends the
 *     camera subsystem when no v4l2 client is streaming, and
 *     slmos-kexec doesn't keep it powered up across the handoff).
 *   - NVCSI MMIO at TEGRA234_NVCSI_BASE is mapped (vmm.c handles).
 *   - The sensor is out of reset and has stable XCLK
 *     (`imx219_power_on()` returned 0).
 *
 * Returns 0 on success, negative on error:
 *   -1  invalid port (NULL, unsupported lane count, cil_half > 1,
 *       or phy_brick > 3)
 *   -2  BPMP power-domain or clock enable failed
 *   -3  CIL register readback verification failed
 *      (the write was made but the value didn't latch — typically
 *      a CBB firewall block, gated power domain, or NVCSI being
 *      held in reset by the Camera RTCPU. On Tegra234 this is the
 *      expected outcome for direct-MMIO access — see
 *      `docs/jetson-camera-nvcsi-driver-notes.md` "Update —
 *      2026-04-26: Option A is blocked".)
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
