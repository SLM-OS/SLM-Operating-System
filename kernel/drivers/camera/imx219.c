/*
 * imx219.c — Sony IMX219 sensor driver implementation.
 *
 * Wraps tegra_i2c_* (cam_i2c) + gpio_tegra_* (cam_reset, cam_pwr,
 * cam_i2cmux_sel) + bpmp_clk_* (extperiph1 XCLK) into the power-up
 * sequence the Orin Nano carrier needs. See imx219.h for the
 * step-by-step description.
 *
 * Reference: Linux mainline `drivers/media/i2c/imx219.c` (cached at
 * `~/slmos-ref/linux/linux-imx219.c`) and the L4T tegracam IMX219
 * driver wrapper. Notes on what SLM-OS deliberately skips (V4L2
 * machinery, runtime PM, mode tables) live in
 * `docs/jetson-camera-imx219-driver-notes.md`.
 */

#include "imx219.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include <stdint.h>

#include "bpmp.h"
#include "debug.h"
#include "gpio_tegra.h"
#include "i2c_tegra.h"
#include "tegra234_clocks.h"
#include "timer.h"

/* IMX219 datasheet timing. The regulator-settle wait used in the
 * Linux tegracam driver isn't applied here because we deliberately
 * don't drive cam_pwr (PH.03) — see step 2b in `imx219_power_on`
 * for the carrier-specific reason. */
#define IMX219_XCLR_TO_I2C_READY_US  6200u   /* ≥6.2 ms after XCLR HIGH */

/* IMX219 XCLK target. The L4T DT for the Orin Nano IMX219-A overlay
 * specifies mclk_khz=24000; the sensor's PLL is designed for a
 * 24 MHz reference. */
#define IMX219_XCLK_HZ               24000000u

/* The cam_i2cmux is a 2-channel GPIO mux. Per the L4T overlay,
 * channel-0 maps to connector A (J17). Active-high mux GPIO: drive
 * LOW to select channel-0 (the i2c-mux-gpio "deselected" state is
 * channel 0; the mux GPIO HIGH selects channel 1). Adjust if a
 * future deployment uses connector C instead. */
#define IMX219_MUX_SELECT_A          0

/* Diagnostic: read back a GPIO's enable_config + output_value after a
 * drive_output() call to verify the writes actually landed. CBB
 * firewall blocks would leave the readback at the pre-write state. */
static void imx219_dump_gpio(struct gpio_tegra_pin *pin)
{
    uint32_t enable = *(volatile uint32_t *)(pin->base + 0x00);
    uint32_t outval = *(volatile uint32_t *)(pin->base + 0x10);
    uint32_t input  = *(volatile uint32_t *)(pin->base + 0x08);
    INFO("imx219: gpio %s: ENABLE=0x%x OUT=0x%x IN=0x%x",
         pin->name, (unsigned)enable, (unsigned)outval, (unsigned)input);
}

int imx219_power_on(void)
{
    /* Step 1: enable the XCLK source. extperiph1 sources from
     * pll_p; BPMP clk_enable is idempotent if Linux already left it
     * running (the normal post-kexec state when an IMX219 driver was
     * bound to /dev/video0). */
    int rc = bpmp_clk_enable(TEGRA234_CLK_EXTPERIPH1);
    if (rc != 0) {
        WARN("imx219: bpmp_clk_enable(EXTPERIPH1) rc=%d", rc);
        return -1;
    }

    /* Best-effort: also try to set the rate. If Linux already
     * configured 24 MHz this is a no-op; if extperiph1 reverted to
     * some other rate after kexec we re-program it. Failure to set
     * isn't fatal — log and continue, the CHIP_ID read will tell us
     * whether the sensor likes the resulting clock. */
    uint64_t actual = 0;
    int set_rc = bpmp_clk_set_rate(TEGRA234_CLK_EXTPERIPH1,
                                   IMX219_XCLK_HZ, &actual);
    if (set_rc != 0) {
        INFO("imx219: clk_set_rate rc=%d (continuing)", set_rc);
    } else {
        INFO("imx219: extperiph1 = %lu Hz", (unsigned long)actual);
    }

    /* Step 2a: force PH.06 (cam_reset) and PCC.03 (cam_i2cmux_sel)
     * pinmux registers to GPIO mode. Linux's pinctrl framework
     * handles this transparently when the kernel boots; after
     * runtime suspend or kexec the state may revert. Without bit 10
     * (GPIO_SFIO_SEL) cleared, the GPIO controller's OUTPUT_VALUE
     * writes are silently ignored — the pad stays routed to its
     * default SFIO peripheral (UART4_CTS for PH.06, SPI2_CS0 for
     * PCC.03). See `docs/jetson-camera-tegra-gpio-notes.md`. */
    gpio_tegra_pinmux_set_gpio(TEGRA234_PINMUX_MAIN_BASE
                               + TEGRA234_PINMUX_CAM_RESET_OFF);
    gpio_tegra_pinmux_set_gpio(TEGRA234_PINMUX_AON_BASE
                               + TEGRA234_PINMUX_CAM_MUX_OFF);

    /* Step 2b: position the cam_i2cmux selector for connector A.
     * Skip cam_pwr — Linux observation showed PH.03 stays LOW even
     * during active streaming on this carrier, so the camera rails
     * are sourced from a fixed always-on regulator and PH.03 controls
     * something else. Driving it HIGH risks unrelated side effects.
     *
     * Issue #518 troubleshooting (2026-04-27) verified: driving PH.03
     * HIGH does NOT unblock the no-SOF symptom. So the original
     * comment is correct — PH.03 doesn't control CSI-2 PHY power. */
    rc = gpio_tegra_drive_output(&gpio_tegra_cam_mux_sel,
                                 IMX219_MUX_SELECT_A);
    if (rc != 0) {
        WARN("imx219: gpio_drive cam_mux_sel rc=%d", rc);
        return -1;
    }
    imx219_dump_gpio(&gpio_tegra_cam_mux_sel);

    /* Step 3: release the IMX219 from reset. */
    rc = gpio_tegra_drive_output(&gpio_tegra_cam_reset, 1);
    if (rc != 0) {
        WARN("imx219: gpio_drive cam_reset rc=%d", rc);
        return -1;
    }
    imx219_dump_gpio(&gpio_tegra_cam_reset);

    /* Step 4: wait for the sensor's I²C interface to come up. */
    timer_busy_wait_us(IMX219_XCLR_TO_I2C_READY_US);

    /* Step 5: bring the I²C controller up (idempotent; Linux's
     * pre-kexec state is overwritten with our known-good config) and
     * read CHIP_ID. */
    rc = tegra_i2c_init(&tegra_i2c_cam_bus);
    if (rc != 0) {
        WARN("imx219: tegra_i2c_init rc=%d", rc);
        return -1;
    }

    uint16_t chip_id = 0;
    rc = imx219_read_chip_id(&chip_id);
    if (rc != 0) {
        WARN("imx219: chip_id read rc=%d", rc);
        struct tegra_i2c_regdump_entry regs[10] = {0};
        tegra_i2c_dump_status(&tegra_i2c_cam_bus, regs, 10);
        for (uint32_t i = 0; i < 10; i++) {
            if (regs[i].name)
                INFO("imx219: i2c %s = 0x%08x",
                     regs[i].name, (unsigned)regs[i].value);
        }
        return -2;
    }
    if (chip_id != IMX219_CHIP_ID) {
        WARN("imx219: unexpected CHIP_ID 0x%04x (expected 0x0219)",
             (unsigned)chip_id);
        return -3;
    }

    /* Force CSI-2 D-PHY lanes into LP-11 (low-power, both lanes
     * pulled HIGH via termination). Per L4T linux-imx219.c:1145-1162:
     *
     *   "Sensor doesn't enter LP-11 state upon power up until and
     *    unless streaming is started, so upon power up switch the
     *    modes to: streaming -> standby"
     *
     * NVCSI's DPHY block trains its receivers by observing
     * LP-11 → HS line-state transitions. Without an established
     * LP-11 baseline before NVCSI is brought up by RCE
     * (CAPTURE_PHY_STREAM_OPEN_REQ), the DPHY never locks and
     * the VI Falcon scheduler reports FRAME_START_TIMEOUT — the
     * exact symptom from issue #518.
     *
     * Two writes with 100-110 µs delays each, matching the L4T
     * usleep_range pattern. Errors logged + propagated; the
     * sensor is responsive on I²C at this point so a failure
     * here means something is fundamentally wrong with the bus. */
    rc = tegra_i2c_write_reg16(&tegra_i2c_cam_bus, IMX219_I2C_ADDR,
                               IMX219_REG_MODE_SELECT,
                               IMX219_MODE_STREAMING);
    if (rc != 0) {
        WARN("imx219: LP-11 force step 1 (MODE_SELECT=1) rc=%d", rc);
        return -4;
    }
    timer_busy_wait_us(110u);
    rc = tegra_i2c_write_reg16(&tegra_i2c_cam_bus, IMX219_I2C_ADDR,
                               IMX219_REG_MODE_SELECT,
                               IMX219_MODE_STANDBY);
    if (rc != 0) {
        WARN("imx219: LP-11 force step 2 (MODE_SELECT=0) rc=%d", rc);
        return -4;
    }
    timer_busy_wait_us(110u);
    INFO("imx219: LP-11 forced (MODE_SELECT 1→0); CSI-2 lanes parked");

    return 0;
}

void imx219_power_off(void)
{
    /* Reverse the bring-up: assert reset, gate XCLK. cam_pwr is left
     * alone (Linux's observed state has PH.03 LOW even during active
     * streaming, so we never drove it). Errors logged but not
     * propagated — power-off is best-effort cleanup. */
    (void)gpio_tegra_set_value(&gpio_tegra_cam_reset, 0);
    int rc = bpmp_clk_disable(TEGRA234_CLK_EXTPERIPH1);
    if (rc != 0) {
        INFO("imx219: clk_disable(EXTPERIPH1) rc=%d", rc);
    }
}

int imx219_read_chip_id(uint16_t *out_chip_id)
{
    if (!out_chip_id) return -1;

    uint8_t hi = 0, lo = 0;
    int rc = tegra_i2c_read_reg16(&tegra_i2c_cam_bus, IMX219_I2C_ADDR,
                                  IMX219_REG_CHIP_ID_HI, &hi);
    if (rc != 0) return rc;
    rc = tegra_i2c_read_reg16(&tegra_i2c_cam_bus, IMX219_I2C_ADDR,
                              IMX219_REG_CHIP_ID_LO, &lo);
    if (rc != 0) return rc;

    *out_chip_id = (uint16_t)(((uint16_t)hi << 8) | lo);
    return 0;
}

/* IMX219 register-init sequence — width-tagged table walked by
 * `imx219_write_table`. Mirrors L4T's `cci_reg_sequence` pattern
 * but with width carried explicitly per entry instead of via the
 * CCI_REG8 / CCI_REG16 macros (which encode width in the upper
 * bits of the address — fine for kernel regmap, overkill here). */
struct imx219_reg_seq {
    uint16_t reg;        /* 16-bit register address */
    uint16_t val;        /* up to 16-bit value */
    uint8_t  width;      /* 1 or 2 — bytes to write big-endian */
};

#define R8(addr,  v)  { (addr), (v), 1 }
#define R16(addr, v)  { (addr), (v), 2 }

/* Common-init table — 31 writes copied verbatim from L4T's
 * `imx219_common_regs` (`~/slmos-ref/linux/linux-imx219.c:161-203`).
 * Sensor-mode-independent: PLL clock, undocumented tuning
 * registers, frame-bank baseline. The numeric multipliers here
 * (PLL_VT_MPY=57, PLL_OP_MPY=114, etc.) are tuned for a
 * 24 MHz EXTPERIPH1 reference — see IMX219_XCLK_FREQ_HZ. */
static const struct imx219_reg_seq imx219_common_regs[] = {
    R8 (IMX219_REG_MODE_SELECT, 0x00),     /* into standby */

    /* "To access addresses 0x3000-0x5fff" magic sequence. */
    R8 (0x30eb, 0x05),
    R8 (0x30eb, 0x0c),
    R8 (0x300a, 0xff),
    R8 (0x300b, 0xff),
    R8 (0x30eb, 0x05),
    R8 (0x30eb, 0x09),

    /* PLL clock table (24 MHz EXTPERIPH1 reference). */
    R8 (0x0301, 5),                         /* VTPXCK_DIV */
    R8 (0x0303, 1),                         /* VTSYCK_DIV */
    R8 (0x0304, 3),                         /* PREPLLCK_VT_DIV  (AUTO) */
    R8 (0x0305, 3),                         /* PREPLLCK_OP_DIV  (AUTO) */
    R16(0x0306, 57),                        /* PLL_VT_MPY */
    R8 (0x030b, 1),                         /* OPSYCK_DIV */
    R16(0x030c, 114),                       /* PLL_OP_MPY */

    /* Undocumented tuning (datasheet §11; values from L4T). */
    R8 (0x455e, 0x00),
    R8 (0x471e, 0x4b),
    R8 (0x4767, 0x0f),
    R8 (0x4750, 0x14),
    R8 (0x4540, 0x00),
    R8 (0x47b4, 0x14),
    R8 (0x4713, 0x30),
    R8 (0x478b, 0x10),
    R8 (0x478f, 0x10),
    R8 (0x4793, 0x10),
    R8 (0x4797, 0x0e),
    R8 (0x479b, 0x0e),

    /* Frame Bank Register Group "A" baseline. */
    R16(0x0162, 3448),                      /* LINE_LENGTH_A */
    R8 (0x0170, 1),                         /* X_ODD_INC_A */
    R8 (0x0171, 1),                         /* Y_ODD_INC_A */

    /* Output setup. */
    R8 (0x0128, 0x00),                      /* DPHY_CTRL = TIMING_AUTO */
    R16(0x012a, (IMX219_XCLK_FREQ_HZ / 1000000u) * 256u), /* EXCK_FREQ = MHz × 256 = 24×256=6144 */
};

/* Mode-specific writes for binning mode 1640×1232 RAW10 — derived
 * from L4T's `imx219_set_framefmt` (`linux-imx219.c:586-662`)
 * with crop = full pixel array and 2× digital binning H+V.
 *
 * Computed values:
 *   X_ADD_STA_A = (8 - 8) = 0,  X_ADD_END_A = 0 + 3280 - 1 = 3279
 *   Y_ADD_STA_A = (8 - 8) = 0,  Y_ADD_END_A = 0 + 2464 - 1 = 2463
 *   BINNING_MODE_H/V = 0x01 (X2 digital)
 *   X_OUTPUT_SIZE = 1640, Y_OUTPUT_SIZE = 1232
 *   TP_WINDOW_W/H  = 1640, 1232 (test-pattern window matches output)
 *   CSI_DATA_FORMAT_A = (10 << 8) | 10 = 0x0A0A
 *   OPPXCK_DIV = 10 (= bpp) */
static const struct imx219_reg_seq imx219_mode_binning_1640x1232[] = {
    R16(0x0164, 0),                         /* X_ADD_STA_A */
    R16(0x0166, 3279),                      /* X_ADD_END_A */
    R16(0x0168, 0),                         /* Y_ADD_STA_A */
    R16(0x016a, 2463),                      /* Y_ADD_END_A */
    R8 (0x0174, IMX219_BINNING_X2),         /* BINNING_MODE_H */
    R8 (0x0175, IMX219_BINNING_X2),         /* BINNING_MODE_V */
    R16(0x016c, 1640),                      /* X_OUTPUT_SIZE */
    R16(0x016e, 1232),                      /* Y_OUTPUT_SIZE */
    R16(0x0624, 1640),                      /* TP_WINDOW_WIDTH */
    R16(0x0626, 1232),                      /* TP_WINDOW_HEIGHT */
    R16(0x018c, 0x0A0Au),                   /* CSI_DATA_FORMAT_A — RAW10 */
    R8 (0x0309, 10),                        /* OPPXCK_DIV = bpp */
};

/* Lane mode — 2-lane D-PHY for IMX219-A on the Orin Nano dev kit.
 * Single write between common-init and mode-specific blocks. */
#define IMX219_REG_CSI_LANE_MODE     0x0114u
#define IMX219_LANE_MODE_2LANE       0x01u

/* Default control values L4T applies via __v4l2_ctrl_handler_setup
 * (`linux-imx219.c:705`) AFTER the mode-init table and BEFORE
 * MODE_SELECT=1. Without these the sensor uses internal defaults
 * that prevent streaming — specifically VTS=0 means "can't compute
 * frame timing", so the sensor never emits a SOF.
 *
 * For binning mode 1640×1232, VTS=1763 (per supported_modes[2]
 * .vts_def in L4T). EXPOSURE=0x640 (1600 lines, IMX219_EXPOSURE_DEFAULT).
 * DIGITAL_GAIN=0x0100 ("1.0x", IMX219_DGTL_GAIN_DEFAULT).
 * ANALOG_GAIN=0 (IMX219_ANA_GAIN_DEFAULT). */
static const struct imx219_reg_seq imx219_default_ctrls_binning[] = {
    R8 (0x0157, 0),                         /* ANALOG_GAIN */
    R16(0x0158, 0x0100),                    /* DIGITAL_GAIN = 1.0x */
    R16(0x015a, 0x0640),                    /* EXPOSURE = 1600 lines */
    R16(0x0160, 1763),                      /* VTS — binning-mode 30 fps frame timing */
};

#undef R8
#undef R16

/* Walk a register table, writing each entry via the appropriate
 * I²C primitive. Returns 0 on success, or the first failing
 * write's negative rc — leaves the sensor in a half-configured
 * state on failure (caller should power-cycle). */
static int imx219_write_table(const struct imx219_reg_seq *seq, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int rc;
        if (seq[i].width == 2u) {
            rc = tegra_i2c_write_reg16_val16(&tegra_i2c_cam_bus,
                                             IMX219_I2C_ADDR,
                                             seq[i].reg,
                                             seq[i].val);
        } else {
            rc = tegra_i2c_write_reg16(&tegra_i2c_cam_bus,
                                       IMX219_I2C_ADDR,
                                       seq[i].reg,
                                       (uint8_t)seq[i].val);
        }
        if (rc != 0) {
            WARN("imx219: I²C write reg=0x%04x val=0x%04x w=%u failed rc=%d "
                 "at table index %zu",
                 (unsigned)seq[i].reg, (unsigned)seq[i].val,
                 (unsigned)seq[i].width, rc, i);
            return rc;
        }
    }
    return 0;
}

int imx219_set_mode_binning_1640x1232(void)
{
    /* 1. Common init (31 writes). */
    int rc = imx219_write_table(imx219_common_regs,
                                sizeof(imx219_common_regs)
                                / sizeof(imx219_common_regs[0]));
    if (rc != 0) return rc;

    /* 2. Lane mode — 2 D-PHY lanes for IMX219-A. */
    rc = tegra_i2c_write_reg16(&tegra_i2c_cam_bus, IMX219_I2C_ADDR,
                               IMX219_REG_CSI_LANE_MODE,
                               IMX219_LANE_MODE_2LANE);
    if (rc != 0) {
        WARN("imx219: CSI_LANE_MODE write failed rc=%d", rc);
        return rc;
    }

    /* 3. Mode-specific (12 writes) for 1640×1232 RAW10 binning. */
    rc = imx219_write_table(imx219_mode_binning_1640x1232,
                            sizeof(imx219_mode_binning_1640x1232)
                            / sizeof(imx219_mode_binning_1640x1232[0]));
    if (rc != 0) return rc;

    /* 4. Default controls (4 writes) — VTS for frame timing,
     * EXPOSURE / GAIN / DIGITAL_GAIN for AE baseline. Without
     * VTS the sensor never produces a SOF (verified blocker on
     * jetson-nano-1, this PR's iteration 1). */
    rc = imx219_write_table(imx219_default_ctrls_binning,
                            sizeof(imx219_default_ctrls_binning)
                            / sizeof(imx219_default_ctrls_binning[0]));
    if (rc != 0) return rc;

    INFO("imx219: mode-init OK — 1640x1232 RAW10 binning, MODE_SELECT=standby");
    return 0;
}

int imx219_streaming_enable(void)
{
    return tegra_i2c_write_reg16(&tegra_i2c_cam_bus, IMX219_I2C_ADDR,
                                 IMX219_REG_MODE_SELECT,
                                 IMX219_MODE_STREAMING);
}

int imx219_streaming_disable(void)
{
    return tegra_i2c_write_reg16(&tegra_i2c_cam_bus, IMX219_I2C_ADDR,
                                 IMX219_REG_MODE_SELECT,
                                 IMX219_MODE_STANDBY);
}

#else /* !PLATFORM_JETSON_ORIN_NANO — stubs for cross-platform builds */

int imx219_power_on(void) { return -1; }
void imx219_power_off(void) { /* no-op */ }
int imx219_read_chip_id(uint16_t *out)
{
    if (out) *out = 0;
    return -1;
}
int imx219_set_mode_binning_1640x1232(void) { return -1; }
int imx219_streaming_enable(void)  { return -1; }
int imx219_streaming_disable(void) { return -1; }

#endif /* PLATFORM_JETSON_ORIN_NANO */
