/*
 * imx219.h — IMX219 sensor driver (Jetson Orin Nano carrier).
 *
 * Wraps the existing HSI2C / GPIO / BPMP layers to bring the Sony
 * IMX219 image sensor up from a fully-off state. Today's surface is
 * the minimum needed for the #396 Hardware Task 2 verification gate:
 * power-on + CHIP_ID readback. Streaming, mode-table programming,
 * exposure / gain control all live in the next task.
 *
 * Power-up sequence (matches the L4T tegracam IMX219 driver order):
 *
 *   1. Enable XCLK source (TEGRA234_CLK_EXTPERIPH1 at 24 MHz) via BPMP.
 *   2. Force PH.06 (cam_reset) and PCC.03 (cam_i2cmux_sel) pinmux to
 *      GPIO mode — Linux's pinctrl framework does this transparently
 *      at boot, but the assignment can revert across kexec.
 *   3. Pre-position the cam_i2cmux selector (gpiochip1 CC.3) LOW so the
 *      bus routes to connector A. Linux's i2c-mux-gpio toggles this
 *      per-transaction; SLM-OS just nails it to channel-0.
 *   4. Drive cam_reset GPIO HIGH (release IMX219 from reset).
 *   5. Wait 6.2 ms (per IMX219 datasheet: max time from XCLR=HIGH to
 *      I²C ready).
 *   6. Bring the cam_i2c controller up via tegra_i2c_init.
 *   7. Read CHIP_ID at registers 0x0000+0x0001 over cam_i2c. Expect
 *      0x0219.
 *
 * Power-off reverses 4→1 (drive cam_reset LOW, then disable XCLK).
 * cam_pwr (PH.03) is intentionally NOT touched: live observation on
 * the Orin Nano dev kit showed Linux leaves PH.03 LOW even during
 * active streaming, so the camera rails are fed from a fixed
 * always-on regulator and PH.03 controls something else on this
 * carrier. Driving it would risk an unrelated side effect.
 *
 * Jetson-only (`PLATFORM_JETSON_ORIN_NANO`); other platforms get
 * stubs that return -1.
 */

#pragma once

#include <stdint.h>

/* I²C slave address of the IMX219 on cam_i2c (default; the L4T
 * overlay confirms this for the J17/J20 connectors on the Orin Nano
 * carrier). The IMX219 datasheet allows an alternate 0x36 via the
 * SADDR pin but this carrier strapping uses 0x10. */
#define IMX219_I2C_ADDR              0x10u

/* CHIP_ID register pair and expected value. */
#define IMX219_REG_CHIP_ID_HI        0x0000u
#define IMX219_REG_CHIP_ID_LO        0x0001u
#define IMX219_CHIP_ID               0x0219u

/* MODE_SELECT controls software standby (0x00) vs streaming (0x01).
 * Per IMX219 datasheet §10. The transition from standby → streaming
 * takes effect on the next frame's sensor clock; expect ~33 ms of
 * settling at 30 fps before the first SOF appears on CSI-2. */
#define IMX219_REG_MODE_SELECT       0x0100u
#define IMX219_MODE_STANDBY          0x00u
#define IMX219_MODE_STREAMING        0x01u

/* Pixel array geometry (IMX219 datasheet §6.1).
 * Full active-pixel array is 3280×2464 starting at (8,8). */
#define IMX219_PIXEL_ARRAY_LEFT      8u
#define IMX219_PIXEL_ARRAY_TOP       8u
#define IMX219_PIXEL_ARRAY_WIDTH     3280u
#define IMX219_PIXEL_ARRAY_HEIGHT    2464u

/* Required external clock frequency. The PLL multipliers in
 * `imx219_common_regs` assume exactly 24 MHz at EXTPERIPH1. */
#define IMX219_XCLK_FREQ_HZ          24000000u

/* Binning mode register values for BINNING_MODE_H / BINNING_MODE_V. */
#define IMX219_BINNING_NONE          0x00u
#define IMX219_BINNING_X2            0x01u  /* 2× digital binning, RAW10 path */
#define IMX219_BINNING_X2_ANALOG     0x03u  /* 2× analog binning, RAW8 path */

/*
 * Power the sensor up through the full sequence described in the file
 * header, then force the CSI-2 D-PHY lanes into LP-11. Idempotent —
 * calling twice in a row is safe and runs the sequence each time
 * (BPMP clock_enable is itself idempotent; GPIOs are simply re-driven
 * to the same state).
 *
 * The trailing LP-11 force step writes MODE_SELECT=1 then MODE_SELECT=0
 * with 100-110 µs between (per L4T linux-imx219.c:1145-1162). Without
 * this, NVCSI's DPHY can't train its receivers — see issue #518 for
 * the failure mode it caused before this fix.
 *
 * Returns 0 on success, negative on error:
 *   -1  any prerequisite failed (see kprintf for the specific stage)
 *   -2  CHIP_ID readback failed (sensor not responding after power-up)
 *   -3  CHIP_ID mismatch (sensor responded but with wrong identity)
 *   -4  LP-11 force I²C write failed (sensor wedged after CHIP_ID OK;
 *       sensor state is undefined — caller must power-cycle before
 *       any subsequent use)
 *
 * On success the caller may immediately issue `tegra_i2c_*` reads /
 * writes to `IMX219_I2C_ADDR` on `tegra_i2c_cam_bus`, and the sensor
 * is left in software standby (MODE_SELECT=0) with CSI-2 lanes in
 * LP-11.
 */
int imx219_power_on(void);

/*
 * Power the sensor down: drive cam_reset LOW, then disable
 * extperiph1 XCLK. cam_pwr (PH.03) is intentionally NOT touched —
 * see the file-level comment above for the carrier-specific reason
 * (Linux leaves PH.03 LOW even during streaming, so the camera
 * rails are always-on and PH.03 controls something else).
 *
 * Best-effort: errors logged but ignored, so a partial failure
 * doesn't leave the sensor in a wedged state.
 */
void imx219_power_off(void);

/*
 * Shortcut: assume the sensor is already powered (a previous call to
 * imx219_power_on returned 0) and just read CHIP_ID. Used by the
 * `imx219` shell command for repeat probes without re-running the
 * full power-up sequence.
 *
 * Returns 0 on success, negative on I²C error (see
 * tegra_i2c_read_reg16's rc table). On success *out_chip_id is set
 * to the combined u16 value.
 */
int imx219_read_chip_id(uint16_t *out_chip_id);

/*
 * Apply the full register-bank init sequence for IMX219 binning
 * mode: 1640×1232 RAW10 @ 30 fps. Writes ~45 registers in three
 * groups (per L4T's `imx219_start_streaming` callback in
 * `~/slmos-ref/linux/linux-imx219.c:671`):
 *
 *   1. Common init (31 writes) — PLL clock table, undocumented
 *      tuning registers, frame-bank baseline. Registers in this
 *      block are sensor-mode-independent.
 *   2. Lane mode (1 write) — IMX219 on the Orin Nano dev kit
 *      uses 2 D-PHY lanes (CSI_LANE_MODE = 0x01).
 *   3. Mode-specific (12 writes) — crop window, binning factor,
 *      output dimensions, pixel format, OPPXCK divider for
 *      1640×1232 RAW10 binning mode.
 *
 * Caller must have:
 *   - Run `imx219_power_on()` so the sensor responds on I²C.
 *   - NOT yet called `imx219_streaming_enable()` — this routine
 *     leaves MODE_SELECT at 0 (standby); enable streaming
 *     separately with `imx219_streaming_enable()`.
 *
 * Total runtime: ~45 × ~100 µs = ~5 ms at 400 kHz I²C.
 *
 * Returns 0 on success, negative on the first failing I²C write
 * (see `tegra_i2c_write_reg16` for the rc table). Partial writes
 * leave the sensor in a half-configured state — caller should
 * power-cycle and retry on failure.
 */
int imx219_set_mode_binning_1640x1232(void);

/*
 * Write IMX219 MODE_SELECT (0x0100) to STREAMING (0x01). Sensor
 * starts emitting CSI-2 frames on the next sensor-clock boundary
 * (~33 ms at 30 fps). Caller must have already called
 * `imx219_power_on()` AND `imx219_set_mode_binning_1640x1232()`
 * successfully — without the mode-init the sensor stays in
 * default state and never produces a SOF (verified hardware
 * blocker on jetson-nano-1, PR #513).
 *
 * Returns 0 on success, negative on I²C write error (see
 * `tegra_i2c_write_reg16` for the rc table).
 */
int imx219_streaming_enable(void);

/*
 * Write IMX219 MODE_SELECT (0x0100) to STANDBY (0x00). Use
 * before `imx219_power_off()` to stop CSI emission cleanly.
 *
 * Returns 0 on success, negative on I²C write error.
 */
int imx219_streaming_disable(void);

/* Forward declaration to avoid pulling camera.h into the IMX219
 * header. Defined in `kernel/include/camera.h`. */
struct camera_frame;

/*
 * Capture exactly one frame end-to-end and fill *out with a pointer
 * into the kernel-owned IMX219 frame-buffer carveout at
 * `CAMRTC_FRAME_BUFFER_PHYS`. Each call walks the full bring-up
 * pipeline:
 *
 *   1. imx219_power_on() if the sensor isn't already responsive.
 *   2. camrtc_capture_init() to bring up the RCE HSP/IVC session
 *      (idempotent — returns immediately if already up).
 *   3. CAPTURE_PHY_STREAM_OPEN_REQ on NVCSI_PORT_B.
 *   4. CAPTURE_CSI_STREAM_SET_CONFIG_REQ — D-PHY, 2 lanes,
 *      lp_bypass_mode=1, IMX219-A lane_polarity from the L4T DT.
 *   5. CAPTURE_CHANNEL_SETUP_REQ with EMBDATA enabled.
 *   6. imx219_set_mode_binning_1640x1232() + streaming_enable +
 *      ~50 ms PLL/AGC settle.
 *   7. Build the per-frame descriptor (vi_channel_config +
 *      memoryinfo ring slot 0) and fire CAPTURE_REQUEST_REQ.
 *   8. Block up to 2 s on the STATUS_IND. On
 *      CAPTURE_STATUS_SUCCESS, populate `*out` and return 0.
 *
 * On success `*out` is set to:
 *   data   = `camrtc_frame_buffer_iova()` (kernel-owned; pointer
 *            remains valid until the next call to
 *            `imx219_capture_one_frame`, which overwrites it).
 *   size   = width × height × 2  (T_R16, ~4 MB)
 *   width  = 1640
 *   height = 1232
 *   bayer  = CAMERA_BAYER_RGGB
 *   format = CAMERA_FORMAT_T_R16
 *
 * Returns 0 on success, negative on any failure (errors logged via
 * `WARN(...)` along the way; see the file-level comment in
 * `kernel/drivers/camera/imx219.c` for the specific RCE status
 * codes that can fire).
 *
 * QEMU stub: returns -1 (no hardware) without touching *out.
 */
int imx219_capture_one_frame(struct camera_frame *out);
