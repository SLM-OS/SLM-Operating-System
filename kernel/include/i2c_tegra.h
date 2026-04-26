/*
 * i2c_tegra.h — Tegra234 HSI2C controller driver (polled).
 *
 * Minimal API for IMX219 sensor bring-up. Polled-only, no IRQ, no DMA,
 * single-master. Supports only what the camera path needs: 7-bit
 * addressing, 8-bit register values, 16-bit register addresses (the
 * IMX219 layout), standard-mode (100 kHz) bus speed.
 *
 * The Tegra234 controller speaks "packet mode": every transfer is
 * preceded by a 12-byte header (3 × u32 words pushed into TX_FIFO).
 * `i2c-tegra.c` from Linux v6.12 is the reference implementation;
 * the cached copy lives at `docs/reference/linux-i2c-tegra.c`.
 *
 * Lifecycle for the IMX219 use case:
 *
 *   tegra_i2c_init(&tegra_i2c_cam_bus);
 *   uint8_t hi = 0, lo = 0;
 *   tegra_i2c_read_reg16(&tegra_i2c_cam_bus, 0x10, 0x0000, &hi);
 *   tegra_i2c_read_reg16(&tegra_i2c_cam_bus, 0x10, 0x0001, &lo);
 *   uint16_t chip_id = ((uint16_t)hi << 8) | lo;
 *   // expect chip_id == 0x0219
 *
 * Jetson-only (compiled when PLATFORM_JETSON_ORIN_NANO is defined);
 * other platforms link a stub that returns -1 from every function.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "spinlock.h"

/*
 * Description of one HSI2C controller instance. Constructed at module
 * scope (e.g. `tegra_i2c_cam_bus` for the camera bus) — callers don't
 * fill these in by hand.
 *
 *   base       MMIO base of the controller (e.g. TEGRA234_CAM_I2C_BASE).
 *   clk_id     BPMP MRQ_CLK identifier for the controller's functional
 *              clock (e.g. TEGRA234_CLK_I2C2). Enabled idempotently on
 *              first tegra_i2c_init().
 *   reset_id   BPMP MRQ_RESET identifier, or -1 if the controller has
 *              no MRQ_RESET pair (I2C5 / CAM_I2C is BPMP-internal —
 *              see kernel/include/tegra234_clocks.h header comment).
 *   name       Short label, used in error messages and the `imx219`
 *              shell command.
 *   lock       Per-bus spinlock taken around every tegra_i2c_*
 *              public call. Required because each call mutates
 *              controller state (FIFOs, INT_STATUS, CNFG) and any
 *              two concurrent transactions on the same bus would
 *              race. Held across the up-to-100 ms packet poll —
 *              long but acceptable for a polled driver, and the
 *              alternative (releasing across the wait) would
 *              reintroduce the race. BPMP IPC called during init
 *              is itself lock-free polled, so no deadlock window.
 */
struct tegra_i2c_bus {
    uintptr_t   base;
    uint32_t    clk_id;
    int32_t     reset_id;
    const char *name;
    spinlock_t  lock;
};

/*
 * Pre-configured handle for `cam_i2c` (= HSI2C-2 = i2c2 alias on the
 * live Jetson Orin Nano DT). Both J17 (CAM-A) and J20 (CAM-C)
 * connectors share this controller through a GPIO-controlled MUX
 * (cam_i2cmux on AON GPIO line 19); selecting which connector is
 * active is the GPIO MUX's job, not the I²C controller's.
 *
 * Defined in kernel/drivers/i2c/i2c_tegra.c, only on Jetson builds.
 */
extern struct tegra_i2c_bus tegra_i2c_cam_bus;

/*
 * Bring the controller up: enable the BPMP clock, deassert reset (if
 * applicable), program packet mode + standard-mode (100 kHz)
 * timings, flush FIFOs. Idempotent — Linux's pre-kexec configuration
 * is overwritten with SLM-OS's known-good defaults.
 *
 * Returns 0 on success, negative on error:
 *   -1  bus pointer NULL
 *   -2  BPMP clock enable failed
 *   -3  BPMP reset deassert failed
 *   -4  FIFO flush timed out (controller wedged)
 */
int tegra_i2c_init(struct tegra_i2c_bus *bus);

/*
 * Write a single byte to a 16-bit register address on a 7-bit slave.
 * Wire format: START | (slave<<1)|W ack | reg_hi ack | reg_lo ack |
 * val ack | STOP. Matches Sony IMX219 register-write semantics.
 *
 * Returns 0 on success, negative on error:
 *   -1  bus pointer NULL or slave > 0x7F
 *   -2  controller wedged (FIFO flush or completion timeout)
 *   -3  NACK from slave (sensor absent, wrong address, or rejecting)
 *   -4  arbitration lost (multi-master conflict — shouldn't happen)
 */
int tegra_i2c_write_reg16(struct tegra_i2c_bus *bus,
                          uint8_t  slave_7bit,
                          uint16_t reg,
                          uint8_t  val);

/*
 * Read a single byte from a 16-bit register address on a 7-bit slave.
 * Two-message transfer: write reg_hi+reg_lo with REPEAT_START, then
 * read 1 byte and STOP. Same error codes as tegra_i2c_write_reg16,
 * plus -5 if the read FIFO times out delivering the byte.
 */
int tegra_i2c_read_reg16(struct tegra_i2c_bus *bus,
                         uint8_t  slave_7bit,
                         uint16_t reg,
                         uint8_t *out);
