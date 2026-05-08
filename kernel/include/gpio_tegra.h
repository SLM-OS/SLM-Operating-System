/*
 * gpio_tegra.h — Tegra234 GPIO controller driver (per-pin, polled).
 *
 * Minimal driver wrapping the per-pin MMIO windows the SoC exposes.
 * Tegra234's GPIO scheme is per-pin, not per-port — each pin gets a
 * 32-byte (0x20) register window with these offsets:
 *
 *     +0x00  ENABLE_CONFIG    bit0 = ENABLE, bit1 = OUT (direction)
 *     +0x08  INPUT            bit0 = current input level (read)
 *     +0x0C  OUTPUT_CONTROL   bit0 = FLOATED (1=high-Z, 0=drive enabled)
 *     +0x10  OUTPUT_VALUE     bit0 = output level (write)
 *
 * Reference: Linux v6.12 `drivers/gpio/gpio-tegra186.c`, cached at
 * `~/slmos-ref/linux/linux-gpio-tegra186.c`. Address arithmetic and the
 * IMX219-specific pin mapping live in `kernel/include/platform.h` next
 * to the `TEGRA234_GPIO_*` constants. See
 * `docs/jetson-camera-tegra-gpio-notes.md` for the per-pin window
 * derivation and the open-question list around post-kexec ENABLE
 * state.
 *
 * Lifecycle for an output pin (matches Linux's order to avoid glitch):
 *
 *   1. Write OUTPUT_VALUE     to the desired level (preset).
 *   2. Clear OUTPUT_CONTROL.FLOATED so the driver can drive.
 *   3. Set ENABLE_CONFIG.ENABLE | ENABLE_CONFIG.OUT.
 *
 * Both controllers (`gpio_tegra_main` at `TEGRA234_GPIO_MAIN_BASE` and
 * `gpio_tegra_aon` at `TEGRA234_GPIO_AON_BASE`) share the same per-pin
 * register layout; the driver doesn't need to distinguish them.
 *
 * Jetson-only (compiled when `PLATFORM_JETSON_ORIN_NANO` is defined);
 * other platforms link a stub that returns -1 from every function.
 */

#pragma once

#include <stdint.h>

/*
 * Description of one GPIO pin's MMIO window. Constructed at module
 * scope (see the camera-pin instances at the bottom of this file).
 *
 *   base   Absolute MMIO address of the pin's ENABLE_CONFIG register
 *          (= controller_base + bank*0x1000 + port*0x200 + pin*0x20).
 *   name   Short label, used in error messages and diagnostic prints.
 */
struct gpio_tegra_pin {
    uintptr_t   base;
    const char *name;
};

/*
 * Pre-configured handles for the IMX219-related pins on the Jetson
 * Orin Nano dev kit. See `kernel/include/platform.h` for the pin
 * mapping and `docs/jetson-camera-imx219-plan.md` for the demo flow.
 */
extern struct gpio_tegra_pin gpio_tegra_cam_reset;   /* PH.06 */
extern struct gpio_tegra_pin gpio_tegra_cam_pwr;     /* PH.03 */
extern struct gpio_tegra_pin gpio_tegra_cam_mux_sel; /* CC.3  */

/*
 * Configure a pin as a CMOS push-pull output and drive it to `value`
 * (0 or non-zero). Glitch-free: writes the value first, then enables
 * the drive, then enables CPU control of the direction.
 *
 * Returns 0 on success, -1 on bad arguments (NULL pin).
 */
int gpio_tegra_drive_output(struct gpio_tegra_pin *pin, int value);

/*
 * Update the value of an already-configured output pin (skips the
 * direction / drive-enable writes; safe to call back-to-back).
 *
 * Returns 0 on success, -1 on bad arguments.
 */
int gpio_tegra_set_value(struct gpio_tegra_pin *pin, int value);

/*
 * Read the live INPUT register of a pin. Works regardless of whether
 * the pin is currently configured as input or output (the input
 * receiver is independent of the output driver). Result is 0 or 1.
 *
 * Returns 0 on success, -1 on bad arguments.
 */
int gpio_tegra_read_input(struct gpio_tegra_pin *pin, int *out);

/*
 * Force a pad's pinmux register to "GPIO output, input receiver on,
 * pull-none, drive enabled" (0x00000040 — see TEGRA234_PINMUX_GPIO_OUTPUT
 * in `kernel/include/platform.h` for the bit-layout breakdown).
 *
 * Required because the GPIO controller's per-pin OUTPUT_VALUE writes
 * are no-ops if the pad is currently routed to an SFIO peripheral
 * (UART/SPI/I2S) — bit 10 of the pinmux register is the GPIO-vs-SFIO
 * selector. Linux's pinctrl framework handles this transparently;
 * SLM-OS does it explicitly here.
 *
 *   pinmux_reg_abs   Absolute MMIO address of the pad's pinmux
 *                    register (e.g. 0x02434020 for PH.06). Must
 *                    already be mapped by vmm.c's cam_bases[] block.
 *
 * No return value — the write is fire-and-forget. To verify, peek
 * the register from the shell.
 */
void gpio_tegra_pinmux_set_gpio(uintptr_t pinmux_reg_abs);
