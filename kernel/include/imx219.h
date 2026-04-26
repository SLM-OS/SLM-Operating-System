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

/*
 * Power the sensor up through the full sequence described in the file
 * header. Idempotent — calling twice in a row is safe and runs the
 * sequence each time (BPMP clock_enable is itself idempotent; GPIOs
 * are simply re-driven to the same state).
 *
 * Returns 0 on success, negative on error:
 *   -1  any prerequisite failed (see kprintf for the specific stage)
 *   -2  CHIP_ID readback failed (sensor not responding after power-up)
 *   -3  CHIP_ID mismatch (sensor responded but with wrong identity)
 *
 * On success the caller may immediately issue `tegra_i2c_*` reads /
 * writes to `IMX219_I2C_ADDR` on `tegra_i2c_cam_bus`.
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
