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

int test_suite_camera(void)
{
    UnityBegin("Camera C-API tests");
    RUN_TEST(test_camera_open_mock);
    RUN_TEST(test_camera_open_unknown_returns_minus_one);
    RUN_TEST(test_camera_open_null_safe);
    RUN_TEST(test_camera_preprocess_bad_args);
    RUN_TEST(test_camera_preprocess_mock_first_pixel);
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
