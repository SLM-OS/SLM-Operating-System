/*
 * gpio_tegra.c — Tegra234 GPIO driver implementation.
 *
 * Per-pin MMIO windows. Each pin's window holds direction, drive
 * enable, and output value at fixed offsets. See gpio_tegra.h for
 * the API contract and the address arithmetic.
 *
 * Written for the IMX219 camera bring-up path on jetson-nano-1
 * (#396 Hardware Task 2): release the cam_reset GPIO, enable the
 * camera regulator, position the cam_i2cmux selector. Generalises
 * cleanly to any other Tegra234 GPIO use case — `gpio_tegra_pin`
 * instances for new pins go in this file alongside the camera ones.
 */

#include "gpio_tegra.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include <stdint.h>

#include "platform.h"

/* Per-pin register offsets (see gpio_tegra.h header comment). */
#define GPIO_REG_ENABLE_CONFIG    0x00u
#define GPIO_REG_INPUT            0x08u
#define GPIO_REG_OUTPUT_CONTROL   0x0Cu
#define GPIO_REG_OUTPUT_VALUE     0x10u

/* ENABLE_CONFIG bits */
#define GPIO_ENABLE               (1u << 0)   /* 1 = pin under CPU control */
#define GPIO_OUT                  (1u << 1)   /* 1 = output direction */

/* OUTPUT_CONTROL bits */
#define GPIO_FLOATED              (1u << 0)   /* 1 = high-Z; 0 = drive enabled */

/* OUTPUT_VALUE / INPUT bits */
#define GPIO_VALUE_BIT            (1u << 0)

/* ---- MMIO accessors ---- */

static inline uint32_t gpio_read(struct gpio_tegra_pin *pin, uint32_t off)
{
    return *(volatile uint32_t *)(pin->base + off);
}

static inline void gpio_write(struct gpio_tegra_pin *pin, uint32_t off,
                              uint32_t val)
{
    *(volatile uint32_t *)(pin->base + off) = val;
    /* Read-back to flush write-buffering and force the access to
     * actually leave the CPU. Mirrors the i2c_tegra.c convention. */
    (void)gpio_read(pin, GPIO_REG_ENABLE_CONFIG);
}

/* ---- Module-scope pin instances ---- */

struct gpio_tegra_pin gpio_tegra_cam_reset = {
    .base = TEGRA234_GPIO_MAIN_BASE + TEGRA234_GPIO_CAM_RESET_OFF,
    .name = "cam_reset_gpio",
};

struct gpio_tegra_pin gpio_tegra_cam_pwr = {
    .base = TEGRA234_GPIO_MAIN_BASE + TEGRA234_GPIO_CAM_PWR_OFF,
    .name = "camera-control-output-low",
};

struct gpio_tegra_pin gpio_tegra_cam_mux_sel = {
    .base = TEGRA234_GPIO_AON_BASE + TEGRA234_GPIO_CAM_MUX_OFF,
    .name = "cam_i2cmux_sel",
};

/* ---- Public API ---- */

int gpio_tegra_drive_output(struct gpio_tegra_pin *pin, int value)
{
    if (!pin) return -1;

    /* Glitch-free output enable per the Linux gpio-tegra186 ordering:
     *   1. preset OUTPUT_VALUE
     *   2. clear OUTPUT_CONTROL.FLOATED via RMW (drive enabled)
     *   3. set ENABLE_CONFIG.ENABLE | OUT via RMW (preserve TRIGGER /
     *      DEBOUNCE / INTERRUPT / TIMESTAMP_FUNC bits Linux may have
     *      left set when its pinctrl driver claimed the pin)
     *
     * Plain writes (clobbering all bits to 0) appeared to "work" for
     * cam_reset on jetson-nano-1 in that ENABLE_CONFIG read back as
     * 0x3 and OUT_VALUE as 0x1 — but the pad's INPUT register stayed
     * at 0, meaning the line wasn't actually driven. Reading +
     * preserving the other bits keeps any pinmux-related state Linux
     * had in place.
     */
    gpio_write(pin, GPIO_REG_OUTPUT_VALUE, value ? GPIO_VALUE_BIT : 0u);

    uint32_t out_ctrl = gpio_read(pin, GPIO_REG_OUTPUT_CONTROL);
    out_ctrl &= ~GPIO_FLOATED;
    gpio_write(pin, GPIO_REG_OUTPUT_CONTROL, out_ctrl);

    uint32_t enable = gpio_read(pin, GPIO_REG_ENABLE_CONFIG);
    enable |= GPIO_ENABLE | GPIO_OUT;
    gpio_write(pin, GPIO_REG_ENABLE_CONFIG, enable);
    return 0;
}

int gpio_tegra_set_value(struct gpio_tegra_pin *pin, int value)
{
    if (!pin) return -1;
    gpio_write(pin, GPIO_REG_OUTPUT_VALUE, value ? GPIO_VALUE_BIT : 0u);
    return 0;
}

int gpio_tegra_read_input(struct gpio_tegra_pin *pin, int *out)
{
    if (!pin || !out) return -1;
    *out = (int)(gpio_read(pin, GPIO_REG_INPUT) & GPIO_VALUE_BIT);
    return 0;
}

void gpio_tegra_pinmux_set_gpio(uintptr_t pinmux_reg_abs)
{
    /* GPIO mode (bit 10 = 0), output drive enabled (bit 4 = 0),
     * input receiver on (bit 6 = 1), pull-none (bits 3:2 = 0),
     * PM = 0 (don't care for GPIO). See TEGRA234_PINMUX_GPIO_OUTPUT
     * in platform.h for the bit-layout breakdown. */
    *(volatile uint32_t *)pinmux_reg_abs = TEGRA234_PINMUX_GPIO_OUTPUT;
    /* Read-back to flush; same convention as gpio_write() above. */
    (void)*(volatile uint32_t *)pinmux_reg_abs;
}

#else /* !PLATFORM_JETSON_ORIN_NANO — stubs for cross-platform builds */

struct gpio_tegra_pin gpio_tegra_cam_reset   = { 0, "stub" };
struct gpio_tegra_pin gpio_tegra_cam_pwr     = { 0, "stub" };
struct gpio_tegra_pin gpio_tegra_cam_mux_sel = { 0, "stub" };

int gpio_tegra_drive_output(struct gpio_tegra_pin *pin, int value)
{ (void)pin; (void)value; return -1; }
int gpio_tegra_set_value(struct gpio_tegra_pin *pin, int value)
{ (void)pin; (void)value; return -1; }
int gpio_tegra_read_input(struct gpio_tegra_pin *pin, int *out)
{ (void)pin; (void)out; return -1; }
void gpio_tegra_pinmux_set_gpio(uintptr_t reg) { (void)reg; }

#endif /* PLATFORM_JETSON_ORIN_NANO */
