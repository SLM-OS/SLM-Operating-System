/*
 * test_tegra_fan.c - Pure helpers of the Jetson fan driver (#1105).
 *
 * Reference values were read from Linux on jetson-nano-2 (L4T R36.4.7):
 * pwm-fan level 60 -> PWM3 CSR 0x803c0000 with PWM3 at 11027028 Hz,
 * and tachometer TACH0 0x0200c7e0 -> 1185 rpm in hwmon. The curve is
 * nvfancontrol.conf's default "quiet" profile with TMARGIN.
 */
#include "unity.h"
#include "tegra_fan.h"
#include "test_harness.h"

#include <stdint.h>

static void test_curve_follows_nvfancontrol_quiet_profile(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, tegra_fan_curve_pwm(20000));
    TEST_ASSERT_EQUAL_UINT32(0, tegra_fan_curve_pwm(35000));    /* margin 70 */
    TEST_ASSERT_EQUAL_UINT32(48, tegra_fan_curve_pwm(45000));   /* idle board */
    TEST_ASSERT_EQUAL_UINT32(94, tegra_fan_curve_pwm(54500));   /* midpoint */
    TEST_ASSERT_EQUAL_UINT32(187, tegra_fan_curve_pwm(74000));  /* margin 31 */
    TEST_ASSERT_EQUAL_UINT32(187, tegra_fan_curve_pwm(94999));  /* margin 11 */
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_curve_pwm(95000));  /* margin 10 */
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_curve_pwm(110000));
    TEST_ASSERT_EQUAL_UINT32(0, tegra_fan_curve_pwm(-5000));
    /* Monotonic over the whole range. */
    uint32_t prev = 0;
    for (int32_t t = -10000; t <= 110000; t += 250) {
        uint32_t p = tegra_fan_curve_pwm(t);
        TEST_ASSERT_TRUE(p >= prev);
        prev = p;
    }
}

static void test_ramp_kickstart_and_full_speed(void)
{
    /* A stopped fan is kicked at 64 before it ramps. */
    TEST_ASSERT_EQUAL_UINT32(TEGRA_FAN_KICKSTART_PWM, tegra_fan_next_pwm(0, 20));
    TEST_ASSERT_EQUAL_UINT32(TEGRA_FAN_KICKSTART_PWM, tegra_fan_next_pwm(0, 187));
    /* 10 per poll in either direction, landing exactly on target. */
    TEST_ASSERT_EQUAL_UINT32(74, tegra_fan_next_pwm(64, 187));
    TEST_ASSERT_EQUAL_UINT32(187, tegra_fan_next_pwm(180, 187));
    TEST_ASSERT_EQUAL_UINT32(54, tegra_fan_next_pwm(64, 20));
    TEST_ASSERT_EQUAL_UINT32(20, tegra_fan_next_pwm(25, 20));
    TEST_ASSERT_EQUAL_UINT32(0, tegra_fan_next_pwm(8, 0));
    TEST_ASSERT_EQUAL_UINT32(48, tegra_fan_next_pwm(48, 48));
    /* >= 95 C: full speed at once, from anywhere. */
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_next_pwm(0, 255));
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_next_pwm(100, 400));
    /* Ramping from 0 to 187 settles within 14 polls. */
    uint32_t p = 0;
    for (int i = 0; i < 14; i++) {
        p = tegra_fan_next_pwm(p, 187);
    }
    TEST_ASSERT_EQUAL_UINT32(187, p);
}

static void test_pwm_register_matches_linux(void)
{
    /* Linux at level 60: CSR 0x803c0000, PWM3 clock 11027028 Hz. */
    uint32_t pfm = 99;
    TEST_ASSERT_EQUAL_INT(0, tegra_pwm_pfm(11027028u, TEGRA_FAN_PERIOD_NS, &pfm));
    TEST_ASSERT_EQUAL_UINT32(0, pfm);
    TEST_ASSERT_EQUAL_HEX32(0x803c0000u, tegra_pwm_csr(60, pfm));
    TEST_ASSERT_EQUAL_UINT32(98, tegra_pwm_duty_ticks(98));
    TEST_ASSERT_EQUAL_UINT32(0, tegra_pwm_duty_ticks(0));
    /* 100 %: 256 ticks, i.e. bit 24 (pwm-tegra writes the same). */
    TEST_ASSERT_EQUAL_HEX32(0x81000000u, tegra_pwm_csr(255, 0));
    TEST_ASSERT_EQUAL_HEX32(0x81000000u, tegra_pwm_csr(1000, 0));
    /* Requested source clock: 256 ticks per 45334 ns, rounded up. */
    TEST_ASSERT_EQUAL_UINT64(5646976u, tegra_pwm_required_rate(TEGRA_FAN_PERIOD_NS));
    /* A clock below one period's worth has no divider; a huge one
     * overflows the 13-bit field. */
    TEST_ASSERT_EQUAL_INT(-1, tegra_pwm_pfm(1000000u, TEGRA_FAN_PERIOD_NS, &pfm));
    TEST_ASSERT_EQUAL_INT(-1, tegra_pwm_pfm(1000000000000ull, TEGRA_FAN_PERIOD_NS, &pfm));
    TEST_ASSERT_EQUAL_INT(0, tegra_pwm_pfm(22588000u, TEGRA_FAN_PERIOD_NS, &pfm));
    TEST_ASSERT_EQUAL_UINT32(3, pfm);
}

static void test_tach_rpm_matches_linux(void)
{
    TEST_ASSERT_EQUAL_UINT32(1185, tegra_tach_rpm(0x0200c7e0u));
    /* Stalled (overflow), and the invalid min / max periods. */
    TEST_ASSERT_EQUAL_UINT32(0, tegra_tach_rpm(0x0200c7e0u | TEGRA_TACH_OVERFLOW));
    TEST_ASSERT_EQUAL_UINT32(0, tegra_tach_rpm(0x02000000u));
    TEST_ASSERT_EQUAL_UINT32(0, tegra_tach_rpm(0x0207ffffu));
    /* Faster fan, shorter period. */
    TEST_ASSERT_TRUE(tegra_tach_rpm(0x02006000u) > 2000u);
}

#if !defined(PLATFORM_JETSON_ORIN_NANO)
/* Other platforms get inert stubs: nothing to clock, nothing ready. */
static void test_stubs_off_jetson(void)
{
    struct tegra_fan_status st;
    st.ready = true;
    st.pwm = 77;
    TEST_ASSERT_EQUAL_INT(-1, tegra_fan_init());
    TEST_ASSERT_EQUAL_INT(-1, tegra_fan_start());
    TEST_ASSERT_EQUAL_INT(-1, tegra_fan_set_manual(100));
    tegra_fan_set_auto();
    tegra_fan_poll();
    tegra_fan_get_status(&st);
    TEST_ASSERT_FALSE(st.ready);
    TEST_ASSERT_EQUAL_UINT32(0, st.pwm);
}
#endif

int test_suite_tegra_fan(void)
{
    UnityBegin("test_tegra_fan.c");
    RUN_TEST(test_curve_follows_nvfancontrol_quiet_profile);
    RUN_TEST(test_ramp_kickstart_and_full_speed);
    RUN_TEST(test_pwm_register_matches_linux);
    RUN_TEST(test_tach_rpm_matches_linux);
#if !defined(PLATFORM_JETSON_ORIN_NANO)
    RUN_TEST(test_stubs_off_jetson);
#endif
    return UnityEnd();
}
