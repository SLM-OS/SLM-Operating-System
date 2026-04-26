/*
 * imx219.c — Sony IMX219 sensor driver implementation.
 *
 * Wraps tegra_i2c_* (cam_i2c) + gpio_tegra_* (cam_reset, cam_pwr,
 * cam_i2cmux_sel) + bpmp_clk_* (extperiph1 XCLK) into the power-up
 * sequence the Orin Nano carrier needs. See imx219.h for the
 * step-by-step description.
 *
 * Reference: Linux mainline `drivers/media/i2c/imx219.c` (cached at
 * `docs/reference/linux-imx219.c`) and the L4T tegracam IMX219
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

/* IMX219 datasheet timing (max values). */
#define IMX219_REGULATOR_SETTLE_US   1000u   /* ~1 ms after AVDD/DVDD up */
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
     * something else. Driving it HIGH risks unrelated side effects. */
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

#else /* !PLATFORM_JETSON_ORIN_NANO — stubs for cross-platform builds */

int imx219_power_on(void) { return -1; }
void imx219_power_off(void) { /* no-op */ }
int imx219_read_chip_id(uint16_t *out)
{
    if (out) *out = 0;
    return -1;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
