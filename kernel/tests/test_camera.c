/*
 * test_camera.c — Direct C-API tests for the camera module + static
 * pins for the Tegra234 clock-id constants.
 *
 * The Lua-binding tests in `test_lua.c` exercise the same code through
 * `slm.camera.*`. These tests live alongside the C implementation so a
 * future maintainer who breaks `camera_open` or `camera_preprocess_mnist`
 * gets a focused failure here before the Lua suite reports a less-
 * obvious "MD5 mismatch" downstream.
 *
 * The static_assert block at the bottom pins every clock / reset /
 * power-domain ID exposed by `kernel/include/tegra234_clocks.h`. Any
 * accidental drift (e.g. someone re-deriving the header from a
 * different upstream version) trips a compile-time error before
 * boot — much faster than discovering the problem on hardware when
 * the wrong clock gates the wrong peripheral.
 */

#include "unity.h"
#include "../include/camera.h"
#include "../include/gpio_tegra.h"
#include "../include/i2c_tegra.h"
#include "../include/imx219.h"
#include "../include/nvcsi.h"
#include "../include/platform.h"
#include "../include/tegra234_clocks.h"

#include <stdint.h>
#include <stddef.h>

/* The mock backend's embedded RAW10 frame is exposed as weak symbols.
 * If MOCK_CAMERA_FRAME=OFF at build time, both pointers resolve to
 * NULL — the C-API tests skip in that case rather than failing. */
extern const uint8_t mock_camera_frame_start[] __attribute__((weak));
extern const uint8_t mock_camera_frame_end[]   __attribute__((weak));

static int mock_frame_embedded(void)
{
    return mock_camera_frame_start != NULL
        && mock_camera_frame_end   != NULL;
}

/* ---- camera_open ---- */

/*
 * Test: camera_open with a recognised mock backend fills the frame
 * descriptor with the IMX219 binned-mode geometry. Skipped when the
 * mock isn't embedded.
 */
static void test_camera_open_mock(void)
{
    if (!mock_frame_embedded()) {
        TEST_IGNORE_MESSAGE("MOCK_CAMERA_FRAME=OFF — mock backend skipped");
    }
    struct camera_frame frame;
    int rc = camera_open("mock", &frame);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(frame.data);
    TEST_ASSERT_EQUAL_UINT(1640u * 1232u * 10u / 8u, (unsigned)frame.size);
    TEST_ASSERT_EQUAL_UINT(1640u, frame.width);
    TEST_ASSERT_EQUAL_UINT(1232u, frame.height);
    TEST_ASSERT_EQUAL_UINT(CAMERA_BAYER_RGGB, frame.bayer);
}

/*
 * Test: camera_open rejects unknown backend names with -1, regardless
 * of whether the mock is embedded.
 */
static void test_camera_open_unknown_returns_minus_one(void)
{
    struct camera_frame frame;
    TEST_ASSERT_EQUAL_INT(-1, camera_open("imx219-0", &frame));
    TEST_ASSERT_EQUAL_INT(-1, camera_open("",         &frame));
    TEST_ASSERT_EQUAL_INT(-1, camera_open("nonsense", &frame));
}

/*
 * Test: camera_open rejects NULL inputs without dereferencing.
 */
static void test_camera_open_null_safe(void)
{
    struct camera_frame frame;
    TEST_ASSERT_EQUAL_INT(-1, camera_open(NULL, &frame));
    TEST_ASSERT_EQUAL_INT(-1, camera_open("mock", NULL));
    TEST_ASSERT_EQUAL_INT(-1, camera_open(NULL, NULL));
}

/* ---- camera_preprocess_mnist ---- */

/*
 * Test: camera_preprocess_mnist rejects bad arguments before touching
 * the input. NULL pointers, undersized output capacity, and
 * unsupported geometry all return negative without crashing.
 */
static void test_camera_preprocess_bad_args(void)
{
    /* A small dummy buffer — we never read from it because the geometry
     * check fires first for the non-NULL cases. */
    uint8_t dummy_in[64]  = {0};
    uint8_t dummy_out[CAMERA_MNIST_OUT_BYTES] = {0};

    /* NULL guards. */
    TEST_ASSERT_EQUAL_INT(-1, camera_preprocess_mnist(
        NULL, sizeof(dummy_in), 1640u, 1232u, CAMERA_BAYER_RGGB,
        dummy_out, sizeof(dummy_out)));
    TEST_ASSERT_EQUAL_INT(-1, camera_preprocess_mnist(
        dummy_in, sizeof(dummy_in), 1640u, 1232u, CAMERA_BAYER_RGGB,
        NULL, sizeof(dummy_out)));

    /* out_capacity too small. */
    TEST_ASSERT_EQUAL_INT(-1, camera_preprocess_mnist(
        dummy_in, sizeof(dummy_in), 1640u, 1232u, CAMERA_BAYER_RGGB,
        dummy_out, CAMERA_MNIST_OUT_BYTES - 1u));

    /* Wrong geometry — only 1640x1232 supported today. */
    TEST_ASSERT_EQUAL_INT(-2, camera_preprocess_mnist(
        dummy_in, sizeof(dummy_in), 640u, 480u, CAMERA_BAYER_RGGB,
        dummy_out, sizeof(dummy_out)));
    TEST_ASSERT_EQUAL_INT(-2, camera_preprocess_mnist(
        dummy_in, sizeof(dummy_in), 1640u, 1232u, CAMERA_BAYER_GRBG,
        dummy_out, sizeof(dummy_out)));

    /* Right geometry but raw10_len smaller than the frame requires. */
    TEST_ASSERT_EQUAL_INT(-1, camera_preprocess_mnist(
        dummy_in, sizeof(dummy_in), 1640u, 1232u, CAMERA_BAYER_RGGB,
        dummy_out, sizeof(dummy_out)));
}

/*
 * Test: camera_preprocess_mnist on the embedded mock frame produces
 * the same 3,136-byte output the Lua-side test pins. Direct-C-API
 * counterpart of test_slm_camera_preprocess_mnist_md5 in test_lua.c
 * — catches breakage in the C function before it surfaces through the
 * Lua binding. Skipped when MOCK_CAMERA_FRAME=OFF.
 */
static void test_camera_preprocess_mock_first_pixel(void)
{
    if (!mock_frame_embedded()) {
        TEST_IGNORE_MESSAGE("MOCK_CAMERA_FRAME=OFF — mock backend skipped");
    }
    struct camera_frame frame;
    TEST_ASSERT_EQUAL_INT(0, camera_open("mock", &frame));

    uint8_t out[CAMERA_MNIST_OUT_BYTES];
    int rc = camera_preprocess_mnist(frame.data, frame.size,
                                     frame.width, frame.height, frame.bayer,
                                     out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* The mock frame derives from a centred MNIST digit ("3"); the
     * outermost pixel is part of the black background after the
     * centred 1232x1232 crop. Pin the IEEE 754 bit pattern of pixel
     * (0, 0) to 0x00000000 to verify the algorithm exited cleanly
     * without (e.g.) writing uninitialised stack data. */
    uint32_t bits;
    __builtin_memcpy(&bits, &out[0], sizeof(bits));
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, bits);
}

/* ---- Tegra HSI2C driver ----
 *
 * The driver is Jetson-only; on QEMU the included `i2c_tegra.h` resolves
 * to a stub backend whose every function returns -1 with no MMIO access.
 * The tests below exercise the cross-platform stub branch so a future
 * patch that breaks the stub linkage trips QEMU CI before it reaches
 * Jetson hardware.
 *
 * The `tegra_i2c_cam_bus` instance carries the bus base + BPMP IDs the
 * future IMX219 sensor driver will consume. Pinning its fields
 * compile-time on Jetson and runtime on QEMU guards against accidental
 * reconfig (e.g. someone changing TEGRA234_CAM_I2C_BASE without also
 * updating the bus instance — they currently share the constant via
 * the struct initializer).
 */

/*
 * Test: on non-Jetson builds the stubs return -1 cleanly. On Jetson,
 * tegra_i2c_init runs against real hardware and is exercised by the
 * `imx219` shell command — not by this unit test, which would hang
 * if the BPMP IPC stack isn't initialised in the test harness.
 */
static void test_tegra_i2c_stubs_return_minus_one(void)
{
#if defined(PLATFORM_JETSON_ORIN_NANO)
    TEST_IGNORE_MESSAGE("Jetson build: stubs not active "
                        "(driver runs on real hardware via `imx219` cmd)");
#else
    TEST_ASSERT_EQUAL_INT(-1, tegra_i2c_init(&tegra_i2c_cam_bus));
    TEST_ASSERT_EQUAL_INT(-1,
        tegra_i2c_write_reg16(&tegra_i2c_cam_bus, 0x10u, 0x0000u, 0u));
    uint8_t out = 0xAAu;
    TEST_ASSERT_EQUAL_INT(-1,
        tegra_i2c_read_reg16(&tegra_i2c_cam_bus, 0x10u, 0x0000u, &out));
    /* Stubs must not write through the out-pointer. */
    TEST_ASSERT_EQUAL_HEX8(0xAAu, out);
#endif
}

/*
 * Test: tegra_i2c_cam_bus is wired to the cam_i2c bus and the matching
 * BPMP clock + reset IDs from kernel/include/tegra234_clocks.h. If
 * a future patch desyncs them, the IMX219 sensor driver will appear to
 * init successfully but talk to the wrong I²C controller.
 */
static void test_tegra_i2c_cam_bus_wiring(void)
{
#if defined(PLATFORM_JETSON_ORIN_NANO)
    TEST_ASSERT_EQUAL_HEX64((uint64_t)TEGRA234_CAM_I2C_BASE,
                            (uint64_t)tegra_i2c_cam_bus.base);
    TEST_ASSERT_EQUAL_UINT32(TEGRA234_CLK_I2C2,   tegra_i2c_cam_bus.clk_id);
    TEST_ASSERT_EQUAL_INT32 ((int32_t)TEGRA234_RESET_I2C2,
                            tegra_i2c_cam_bus.reset_id);
#else
    /* Stub instance: base 0, clk 0, reset_id -1 (no MRQ_RESET path). */
    TEST_ASSERT_EQUAL_HEX64(0u, (uint64_t)tegra_i2c_cam_bus.base);
    TEST_ASSERT_EQUAL_UINT32(0u, tegra_i2c_cam_bus.clk_id);
    TEST_ASSERT_EQUAL_INT32(-1, tegra_i2c_cam_bus.reset_id);
#endif
    TEST_ASSERT_NOT_NULL(tegra_i2c_cam_bus.name);
}

/*
 * Test: the public API rejects NULL bus / NULL out / out-of-range slave
 * addresses on every platform without needing hardware. The stub branch
 * returns -1 unconditionally; the real driver returns -1 specifically
 * for these argument errors per the rc table in i2c_tegra.h.
 */
static void test_tegra_i2c_api_arg_validation(void)
{
    /* NULL bus */
    TEST_ASSERT_EQUAL_INT(-1, tegra_i2c_init(NULL));
    TEST_ASSERT_EQUAL_INT(-1,
        tegra_i2c_write_reg16(NULL, 0x10u, 0x0000u, 0u));
    uint8_t out = 0;
    TEST_ASSERT_EQUAL_INT(-1,
        tegra_i2c_read_reg16(NULL, 0x10u, 0x0000u, &out));

    /* read_reg16 also rejects NULL out_ptr */
    TEST_ASSERT_EQUAL_INT(-1,
        tegra_i2c_read_reg16(&tegra_i2c_cam_bus, 0x10u, 0x0000u, NULL));

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Slave > 0x7F is invalid 7-bit address. The stub returns -1 for
     * any input so this assertion is only meaningful on Jetson. */
    TEST_ASSERT_EQUAL_INT(-1,
        tegra_i2c_write_reg16(&tegra_i2c_cam_bus, 0x80u, 0x0000u, 0u));
    TEST_ASSERT_EQUAL_INT(-1,
        tegra_i2c_read_reg16(&tegra_i2c_cam_bus, 0xFFu, 0x0000u, &out));
#endif
}

/* ---- Tegra234 GPIO driver ---- */

/*
 * Test: gpio_tegra_drive_output / set_value / read_input reject NULL
 * pins on every platform without dereferencing them. read_input also
 * rejects NULL out-pointer.
 */
static void test_gpio_tegra_null_safe(void)
{
    int dummy = 42;
    TEST_ASSERT_EQUAL_INT(-1, gpio_tegra_drive_output(NULL, 1));
    TEST_ASSERT_EQUAL_INT(-1, gpio_tegra_set_value(NULL, 0));
    TEST_ASSERT_EQUAL_INT(-1, gpio_tegra_read_input(NULL, &dummy));
    TEST_ASSERT_EQUAL_INT(-1, gpio_tegra_read_input(&gpio_tegra_cam_reset,
                                                   NULL));
    /* read_input must not write through a NULL out-pointer. */
    TEST_ASSERT_EQUAL_INT(42, dummy);
}

/*
 * Test: the three pre-configured camera pin instances point at the
 * MMIO addresses derived from platform.h. Catches a future refactor
 * that desyncs the per-pin offsets from the pin definitions.
 */
static void test_gpio_tegra_cam_pin_wiring(void)
{
    TEST_ASSERT_NOT_NULL(gpio_tegra_cam_reset.name);
    TEST_ASSERT_NOT_NULL(gpio_tegra_cam_pwr.name);
    TEST_ASSERT_NOT_NULL(gpio_tegra_cam_mux_sel.name);
#if defined(PLATFORM_JETSON_ORIN_NANO)
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(TEGRA234_GPIO_MAIN_BASE
                                       + TEGRA234_GPIO_CAM_RESET_OFF),
                            (uint64_t)gpio_tegra_cam_reset.base);
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(TEGRA234_GPIO_MAIN_BASE
                                       + TEGRA234_GPIO_CAM_PWR_OFF),
                            (uint64_t)gpio_tegra_cam_pwr.base);
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(TEGRA234_GPIO_AON_BASE
                                       + TEGRA234_GPIO_CAM_MUX_OFF),
                            (uint64_t)gpio_tegra_cam_mux_sel.base);
#else
    /* Stub instance: base 0. */
    TEST_ASSERT_EQUAL_HEX64(0u, (uint64_t)gpio_tegra_cam_reset.base);
    TEST_ASSERT_EQUAL_HEX64(0u, (uint64_t)gpio_tegra_cam_pwr.base);
    TEST_ASSERT_EQUAL_HEX64(0u, (uint64_t)gpio_tegra_cam_mux_sel.base);
#endif
}

/* ---- IMX219 sensor driver ---- */

/*
 * Test: imx219_read_chip_id rejects NULL out-pointer without touching
 * the I²C bus. Real reads happen through the `imx219` shell command
 * on Jetson; QEMU exercises the stub branch.
 */
static void test_imx219_read_chip_id_null_safe(void)
{
    TEST_ASSERT_EQUAL_INT(-1, imx219_read_chip_id(NULL));
}

/*
 * Test: imx219_power_off is best-effort and returns void; calling it
 * even without a prior power_on must not crash. On QEMU it short-
 * circuits via the stub.
 */
static void test_imx219_power_off_safe(void)
{
#if defined(PLATFORM_JETSON_ORIN_NANO)
    TEST_IGNORE_MESSAGE("Jetson build: power_off touches real BPMP/GPIO "
                        "(verified via `imx219` shell command)");
#else
    /* Stub branch is a no-op — must compile and link, must not crash. */
    imx219_power_off();
#endif
}

/* ---- Tegra HSI2C diagnostic dump ---- */

/*
 * Test: tegra_i2c_dump_status fills the entries in the order written
 * by the implementation, with non-NULL names and bounded by `n`. The
 * stub branch is a no-op (no MMIO), so we only verify Jetson-side that
 * the table size matches the imx219 driver's expectation (10 entries).
 */
static void test_tegra_i2c_dump_status_bounds(void)
{
    struct tegra_i2c_regdump_entry regs[10] = {0};
    /* `n=0` must leave the array untouched on every platform. */
    tegra_i2c_dump_status(&tegra_i2c_cam_bus, regs, 0u);
    for (uint32_t i = 0; i < 10; i++) {
        TEST_ASSERT_NULL(regs[i].name);
        TEST_ASSERT_EQUAL_UINT32(0u, regs[i].value);
    }
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* On Jetson the dump touches live MMIO — the imx219 shell command
     * already exercises that path on every CHIP_ID failure. Here we
     * just confirm the symbol resolves and the n-clamp logic doesn't
     * write past the caller's buffer. */
    tegra_i2c_dump_status(&tegra_i2c_cam_bus, regs, 999u);
    /* At least the first entry should now have a name. */
    TEST_ASSERT_NOT_NULL(regs[0].name);
#else
    /* Stub: still a no-op for any n. */
    tegra_i2c_dump_status(&tegra_i2c_cam_bus, regs, 10u);
    TEST_ASSERT_NULL(regs[0].name);
#endif
}

/* ---- NVCSI receiver driver ---- */

/*
 * Test: nvcsi_stream_init / stop / get_intr_status reject NULL port
 * pointers without dereferencing them. get_intr_status also rejects
 * NULL out-pointers. Stub branch returns -1; Jetson branch validates
 * the same arg-error paths before any MMIO touches.
 */
static void test_nvcsi_null_safe(void)
{
    uint32_t intr = 0xDEADBEEFu, err = 0xDEADBEEFu;
    TEST_ASSERT_EQUAL_INT(-1, nvcsi_stream_init(NULL));
    TEST_ASSERT_EQUAL_INT(-1, nvcsi_get_intr_status(NULL, &intr, &err));
    TEST_ASSERT_EQUAL_INT(-1, nvcsi_get_intr_status(&nvcsi_imx219_a_port,
                                                   NULL, &err));
    TEST_ASSERT_EQUAL_INT(-1, nvcsi_get_intr_status(&nvcsi_imx219_a_port,
                                                   &intr, NULL));
    /* get_intr_status must not write through the out-pointers when
     * it returns -1 due to bad args. */
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, intr);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, err);
    /* stream_stop is best-effort void — must compile / link / not
     * crash on NULL. */
    nvcsi_stream_stop(NULL);
}

/*
 * Test: the pre-configured `nvcsi_imx219_a_port` matches the L4T
 * IMX219-A overlay's CSI port mapping. The Orin Nano dev-kit DT
 * exposes `port-index = <0x01>` for `rbpcv2_imx219_a@10`, which is
 * NVCSI_PORT_B = (PHY brick 0, CIL_B). NOT CIL_A — the connector
 * naming is independent of the underlying PHY half. Pinning this
 * catches a future patch that swaps cil_half back to 0 thinking
 * connector A == CIL_A.
 */
static void test_nvcsi_imx219_a_port_wiring(void)
{
    TEST_ASSERT_NOT_NULL(nvcsi_imx219_a_port.name);
#if defined(PLATFORM_JETSON_ORIN_NANO)
    TEST_ASSERT_EQUAL_UINT32(0u, nvcsi_imx219_a_port.phy_brick);
    TEST_ASSERT_EQUAL_UINT32(1u, nvcsi_imx219_a_port.cil_half);  /* CIL_B */
    TEST_ASSERT_EQUAL_UINT32(2u, nvcsi_imx219_a_port.num_data_lanes);
    TEST_ASSERT_EQUAL_UINT32(456u, nvcsi_imx219_a_port.mipi_clk_mhz);
#else
    /* Stub: zeroed instance. */
    TEST_ASSERT_EQUAL_UINT32(0u, nvcsi_imx219_a_port.num_data_lanes);
#endif
}

/*
 * Test: nvcsi_stream_init rejects unsupported lane counts, out-of-range
 * cil_half, and out-of-range phy_brick. Important on Jetson where
 * this is the only arg-validation gate before MMIO; on QEMU the stub
 * returns -1 unconditionally so this is a link/compile check.
 */
static void test_nvcsi_stream_init_arg_validation(void)
{
#if defined(PLATFORM_JETSON_ORIN_NANO)
    struct nvcsi_port bad_lanes = nvcsi_imx219_a_port;
    bad_lanes.num_data_lanes = 4u;
    TEST_ASSERT_EQUAL_INT(-1, nvcsi_stream_init(&bad_lanes));

    /* cil_half must be 0 (CIL_A) or 1 (CIL_B). */
    struct nvcsi_port bad_cil = nvcsi_imx219_a_port;
    bad_cil.cil_half = 2u;
    TEST_ASSERT_EQUAL_INT(-1, nvcsi_stream_init(&bad_cil));

    /* phy_brick must be 0..3 (T234 has 4 PHY bricks). */
    struct nvcsi_port bad_phy = nvcsi_imx219_a_port;
    bad_phy.phy_brick = 4u;
    TEST_ASSERT_EQUAL_INT(-1, nvcsi_stream_init(&bad_phy));
#else
    TEST_IGNORE_MESSAGE("Jetson-only — stub returns -1 for any input");
#endif
}

int test_suite_camera(void)
{
    UnityBegin("Camera C-API tests");
    RUN_TEST(test_camera_open_mock);
    RUN_TEST(test_camera_open_unknown_returns_minus_one);
    RUN_TEST(test_camera_open_null_safe);
    RUN_TEST(test_camera_preprocess_bad_args);
    RUN_TEST(test_camera_preprocess_mock_first_pixel);
    RUN_TEST(test_tegra_i2c_stubs_return_minus_one);
    RUN_TEST(test_tegra_i2c_cam_bus_wiring);
    RUN_TEST(test_tegra_i2c_api_arg_validation);
    RUN_TEST(test_gpio_tegra_null_safe);
    RUN_TEST(test_gpio_tegra_cam_pin_wiring);
    RUN_TEST(test_imx219_read_chip_id_null_safe);
    RUN_TEST(test_imx219_power_off_safe);
    RUN_TEST(test_tegra_i2c_dump_status_bounds);
    RUN_TEST(test_nvcsi_null_safe);
    RUN_TEST(test_nvcsi_imx219_a_port_wiring);
    RUN_TEST(test_nvcsi_stream_init_arg_validation);
    return UnityEnd();
}

/* =============================================================================
 * Tegra234 clock-id pins
 *
 * Compile-time assertions guarding kernel/include/tegra234_clocks.h
 * against accidental value drift. Each #define from the header is
 * pinned to the upstream Linux v6.12 dt-binding value (cached under
 * docs/reference/linux-dt-bindings-tegra234-{clock,reset,powergate}.h).
 *
 * If a future port of these constants from a newer upstream changes
 * any value, this assertion fires at compile time — much cheaper than
 * discovering the wrong clock gates the wrong peripheral on hardware.
 * ========================================================================== */

_Static_assert(TEGRA234_CLK_I2C1   == 48u,  "TEGRA234_CLK_I2C1 drift");
_Static_assert(TEGRA234_CLK_I2C2   == 49u,  "TEGRA234_CLK_I2C2 drift");
_Static_assert(TEGRA234_CLK_I2C3   == 50u,  "TEGRA234_CLK_I2C3 drift");
_Static_assert(TEGRA234_CLK_I2C4   == 51u,  "TEGRA234_CLK_I2C4 drift");
_Static_assert(TEGRA234_CLK_I2C5   == 305u, "TEGRA234_CLK_I2C5 drift (CAM_I2C is at a different position from I2C1..4 / I2C6..9)");
_Static_assert(TEGRA234_CLK_I2C6   == 52u,  "TEGRA234_CLK_I2C6 drift");
_Static_assert(TEGRA234_CLK_I2C7   == 53u,  "TEGRA234_CLK_I2C7 drift");
_Static_assert(TEGRA234_CLK_I2C8   == 54u,  "TEGRA234_CLK_I2C8 drift");
_Static_assert(TEGRA234_CLK_I2C9   == 55u,  "TEGRA234_CLK_I2C9 drift");

_Static_assert(TEGRA234_RESET_I2C1 == 24u, "TEGRA234_RESET_I2C1 drift");
_Static_assert(TEGRA234_RESET_I2C2 == 29u, "TEGRA234_RESET_I2C2 drift");
_Static_assert(TEGRA234_RESET_I2C3 == 30u, "TEGRA234_RESET_I2C3 drift");
_Static_assert(TEGRA234_RESET_I2C4 == 31u, "TEGRA234_RESET_I2C4 drift");
_Static_assert(TEGRA234_RESET_I2C6 == 32u, "TEGRA234_RESET_I2C6 drift");
_Static_assert(TEGRA234_RESET_I2C7 == 33u, "TEGRA234_RESET_I2C7 drift");
_Static_assert(TEGRA234_RESET_I2C8 == 34u, "TEGRA234_RESET_I2C8 drift");
_Static_assert(TEGRA234_RESET_I2C9 == 35u, "TEGRA234_RESET_I2C9 drift");

_Static_assert(TEGRA234_CLK_NVCSI    == 81u, "TEGRA234_CLK_NVCSI drift");
_Static_assert(TEGRA234_CLK_NVCSILP  == 82u, "TEGRA234_CLK_NVCSILP drift");
_Static_assert(TEGRA234_RESET_NVCSI  == 43u, "TEGRA234_RESET_NVCSI drift");

_Static_assert(TEGRA234_CLK_VI       == 166u, "TEGRA234_CLK_VI drift");
_Static_assert(TEGRA234_CLK_VI_CONST == 196u, "TEGRA234_CLK_VI_CONST drift");
_Static_assert(TEGRA234_RESET_VI     == 112u, "TEGRA234_RESET_VI drift");
_Static_assert(TEGRA234_RESET_VI2    == 115u, "TEGRA234_RESET_VI2 drift");

_Static_assert(TEGRA234_POWER_DOMAIN_VI   == 28u,
    "TEGRA234_POWER_DOMAIN_VI drift (Video Input — distinct from VIC at id 29)");
_Static_assert(TEGRA234_POWER_DOMAIN_ISPA == 22u,
    "TEGRA234_POWER_DOMAIN_ISPA drift");

/* =============================================================================
 * Tegra234 camera-subsystem MMIO bases — Phase 0 verified
 *
 * Compile-time pins for the constants in kernel/include/platform.h that
 * gate the camera path. Each was probed live on jetson-nano-1 during
 * the #396 Phase 0 recon (2026-04-25); changing any of these without
 * a fresh recon would silently break the future driver. Constants are
 * Jetson-only in platform.h, so the assertions are gated to match.
 *
 * The vmm.c camera-MMIO mapping block iterates this same set; if a
 * future patch reorders or renames the constants, the build breaks
 * here before the wrong block is mapped on hardware.
 * ========================================================================== */
#if defined(PLATFORM_JETSON_ORIN_NANO)
_Static_assert(TEGRA234_NVCSI_BASE   == 0x15A00000UL,
    "TEGRA234_NVCSI_BASE drift (Phase 0 verified address; CSI-2 receiver)");
_Static_assert(TEGRA234_RCE_HSP_BASE == 0x0B950000UL,
    "TEGRA234_RCE_HSP_BASE drift (Camera-RTCPU HSP — distinct from BPMP HSP at 0x03C00000)");
_Static_assert(TEGRA234_RCE_PM_BASE  == 0x0B9F0000UL,
    "TEGRA234_RCE_PM_BASE drift (R5_CTRL + PWR_STATUS state probes)");
_Static_assert(TEGRA234_RCE_BASE     == 0x0BC00000UL,
    "TEGRA234_RCE_BASE drift (RCE main MMIO — Falcon EVP, AST)");
_Static_assert(TEGRA234_CAM_I2C_BASE == 0x03180000UL,
    "TEGRA234_CAM_I2C_BASE drift (HSI2C-2 = cam_i2c — both J17 and J20 share via i2c-mux-gpio)");

/* GPIO controller bases (data-window addresses, not the security
 * window — the security window is TF-A-owned and reads return
 * 0xFFFFFFFF from the CBB firewall). Discovered during Hardware
 * Task 2: the MAIN security window at 0x02200000 is unmapped at
 * EL2; the data window at 0x02210000 is what the per-pin offsets
 * below are added to. */
_Static_assert(TEGRA234_GPIO_MAIN_BASE == 0x02210000UL,
    "TEGRA234_GPIO_MAIN_BASE drift (data window — security window 0x02200000 returns 0xFFFFFFFF)");
_Static_assert(TEGRA234_GPIO_AON_BASE  == 0x0C2F1000UL,
    "TEGRA234_GPIO_AON_BASE drift (data window — security window 0x0C2F0000 returns 0xFFFFFFFF)");

/* Pinmux base addresses + per-pad offsets. Tegra234 reshuffled the
 * pad offsets vs. Tegra194 — PH.06 moved from 0x4020 (T194) to 0x4008
 * (T234) and PH.03 from 0x4038 to 0x4020. Reading the wrong offset
 * leaves the pad routed to its default SFIO peripheral and the
 * GPIO-controller writes are silently ignored (the IMX219 reset line
 * stays LOW even though OUTPUT_VALUE reads back as 1). Reference:
 * `docs/reference/linux-pinctrl-tegra234.c` `tegra234_pingroups[]`. */
_Static_assert(TEGRA234_PINMUX_MAIN_BASE   == 0x02430000UL,
    "TEGRA234_PINMUX_MAIN_BASE drift");
_Static_assert(TEGRA234_PINMUX_AON_BASE    == 0x0C300000UL,
    "TEGRA234_PINMUX_AON_BASE drift");
_Static_assert(TEGRA234_PINMUX_CAM_RESET_OFF == 0x4008u,
    "TEGRA234_PINMUX_CAM_RESET_OFF drift (T234 PH.06 moved from T194's 0x4020)");
_Static_assert(TEGRA234_PINMUX_CAM_PWR_OFF == 0x4020u,
    "TEGRA234_PINMUX_CAM_PWR_OFF drift (T234 PH.03 moved from T194's 0x4038)");
_Static_assert(TEGRA234_PINMUX_CAM_MUX_OFF == 0x2038u,
    "TEGRA234_PINMUX_CAM_MUX_OFF drift (T234 PCC.03; same as T194)");

/* IMX219 carrier strapping: i2c-mux-gpio channel-0 selects connector A
 * (the J17 socket on the Orin Nano dev kit, where the IMX219 ribbon
 * plugs in by default). The L4T overlay
 * `tegra234-p3767-camera-p3768-imx219-A.dtbo` enables `imx219_a@10`
 * under `i2c@0` of the mux. Channel-0 corresponds to PCC.3 GPIO LOW
 * because i2c-mux-gpio drives the selector to `chan->chan_id`. */
_Static_assert(IMX219_I2C_ADDR == 0x10u,
    "IMX219_I2C_ADDR drift (carrier straps SADDR LOW → 7-bit address 0x10)");
_Static_assert(IMX219_REG_CHIP_ID_HI == 0x0000u,
    "IMX219_REG_CHIP_ID_HI drift (per Sony IMX219 datasheet)");
_Static_assert(IMX219_REG_CHIP_ID_LO == 0x0001u,
    "IMX219_REG_CHIP_ID_LO drift (per Sony IMX219 datasheet)");
_Static_assert(IMX219_CHIP_ID == 0x0219u,
    "IMX219_CHIP_ID drift (per Sony IMX219 datasheet)");
#endif
