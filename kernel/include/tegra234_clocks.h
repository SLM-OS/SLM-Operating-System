/*
 * tegra234_clocks.h - Camera-subsystem clock and reset IDs for Tegra234
 *
 * Numeric IDs consumed by the BPMP MRQ_CLK and MRQ_RESET wrappers in
 * kernel/include/bpmp.h. Values are mirrored verbatim from the upstream
 * Linux device-tree bindings:
 *
 *   include/dt-bindings/clock/tegra234-clock.h   (Linux v6.12)
 *   include/dt-bindings/reset/tegra234-reset.h   (Linux v6.12)
 *   include/dt-bindings/power/tegra234-powergate.h (Linux v6.12)
 *
 * Cached source: ~/slmos-ref/linux/linux-dt-bindings-tegra234-{clock,reset,powergate}.h
 *
 * Scope: only the constants required for IMX219 camera bring-up
 * (every I2C controller, NVCSI, NVCSI-LP, VI / VI2, plus the VI and
 * ISPA power-domain IDs). Adding additional clocks, resets, or power
 * domains is a matter of copying the matching #define out of the
 * cached upstream files into the appropriate section below — no other
 * code changes required, since the IDs are passed straight through
 * the BPMP IPC layer.
 *
 * Licence note: Linux's dt-bindings carry "GPL-2.0-only OR MIT".
 * Reproducing the numeric register IDs (which are hardware addresses
 * baked into the SoC, not creative expression) is not a derivative
 * work in any meaningful sense, but the upstream attribution above is
 * preserved out of professional courtesy.
 */

#pragma once

/* ===================================================================== */
/*  I2C controllers                                                      */
/* ===================================================================== */
/*
 * Tegra234 exposes nine I2C controllers (I2C1..I2C9). Each clock ID
 * gates the controller's functional clock; the matching reset ID
 * holds the controller in reset until the driver deasserts it.
 *
 * The IMX219 camera connector on the Jetson Orin Nano carrier board
 * multiplexes onto one of these buses through the camera connector;
 * the exact bus is not yet pinned down (see docs/jetson-camera-imx219-plan.md
 * §"Pre-Hardware Tasks"), so all nine pairs are exposed.
 *
 * Note: I2C5 lives at a different position in the upstream clock
 * header (305) and has no matching reset ID at all -- I2C5 is the
 * BPMP-internal CAM_I2C and is reset/clocked by the BPMP itself, not
 * via MRQ_RESET. Code that walks every (clock, reset) pair must skip
 * the I2C5 reset.
 */
#define TEGRA234_CLK_I2C1            48U
#define TEGRA234_CLK_I2C2            49U
#define TEGRA234_CLK_I2C3            50U
#define TEGRA234_CLK_I2C4            51U
#define TEGRA234_CLK_I2C5           305U   /* CAM_I2C; no MRQ_RESET pair */
#define TEGRA234_CLK_I2C6            52U
#define TEGRA234_CLK_I2C7            53U
#define TEGRA234_CLK_I2C8            54U
#define TEGRA234_CLK_I2C9            55U

#define TEGRA234_RESET_I2C1          24U
#define TEGRA234_RESET_I2C2          29U
#define TEGRA234_RESET_I2C3          30U
#define TEGRA234_RESET_I2C4          31U
#define TEGRA234_RESET_I2C6          32U
#define TEGRA234_RESET_I2C7          33U
#define TEGRA234_RESET_I2C8          34U
#define TEGRA234_RESET_I2C9          35U

/* ===================================================================== */
/*  CSI receiver (NVCSI)                                                 */
/* ===================================================================== */
/*
 * NVCSI is the MIPI CSI-2 receiver block fed by the camera serial
 * lanes. NVCSILP is the low-power calibration / pad-control clock
 * that has to be running during CSI lane bring-up.
 *
 * A single reset ID (TEGRA234_RESET_NVCSI = 43) covers both clocks;
 * the upstream reset header has no separate NVCSILP reset.
 */
#define TEGRA234_CLK_NVCSI           81U
#define TEGRA234_CLK_NVCSILP         82U

#define TEGRA234_RESET_NVCSI         43U

/* ===================================================================== */
/*  Video Input (VI)                                                     */
/* ===================================================================== */
/*
 * VI is the Video Input engine that receives pixels from NVCSI and
 * writes them to system memory. VI2 is the second VI engine (Tegra234
 * has two so two captures can run concurrently).
 *
 * VI_CONST is the constant-rate companion clock VI requires for its
 * internal pipeline; it is the closest thing in the upstream binding
 * to a "VI memory clock" (no VI_M / VI_MEM define exists upstream).
 */
#define TEGRA234_CLK_VI             166U
#define TEGRA234_CLK_VI_CONST       196U

#define TEGRA234_RESET_VI           112U
#define TEGRA234_RESET_VI2          115U

/* Camera RTCPU (RCE). Used by `camrtc_init` to re-engage RCE
 * post-kexec — Linux's `.shutdown()` callback for
 * `tegra-camera-rtcpu` asserts `RESET_RCE_ALL` and disables these
 * three clocks; mirroring the inverse from SLM-OS restarts the
 * firmware in place from its DRAM carveout. See
 * `docs/jetson-camera-rtcpu-ivc-driver-notes.md` "Hardware Task 3
 * Option B — RESOLVED" + closed issue #438. */
#define TEGRA234_CLK_RCE_CPU_NIC    113U
#define TEGRA234_CLK_RCE_NIC        114U
#define TEGRA234_CLK_RCE_CPU        433U
#define TEGRA234_RESET_RCE_ALL      81U

/* ===================================================================== */
/*  External-peripheral clocks (camera XCLK)                             */
/* ===================================================================== */
/*
 * EXTPERIPH1..4 source the SoC's "EXT_PERIPH" pad outputs that
 * camera modules use as their reference clock (XCLK / MCLK). The
 * IMX219 on the Orin Nano carrier consumes EXTPERIPH1 at 24 MHz
 * (`mclk_khz = 24000` per the L4T overlay).
 *
 * No matching reset ID — these are simple clock-divider outputs from
 * pll_p, gated by the standard MRQ_CLK enable/disable.
 */
#define TEGRA234_CLK_EXTPERIPH1      36U
#define TEGRA234_CLK_EXTPERIPH2      37U
#define TEGRA234_CLK_EXTPERIPH3      38U
#define TEGRA234_CLK_EXTPERIPH4      39U

/* ===================================================================== */
/*  Power domains (camera)                                               */
/* ===================================================================== */
/*
 * Power-domain IDs are NOT in the clock or reset binding; they live
 * in include/dt-bindings/power/tegra234-powergate.h upstream (cached
 * locally in ~/slmos-ref/linux/linux-dt-bindings-tegra234-powergate.h).
 *
 * The two IDs reproduced here are the ones SLM-OS needs to power up
 * before exercising the IMX219 path:
 *   VI    -- contains the VI engine itself
 *   ISPA  -- the Image Signal Processor; required even for a raw
 *            CSI capture because it shares fabric with VI on Tegra234
 *
 * Used with bpmp_pg_set_state() (see kernel/include/bpmp.h).
 */
#define TEGRA234_POWER_DOMAIN_VI     28U
#define TEGRA234_POWER_DOMAIN_ISPA   22U
