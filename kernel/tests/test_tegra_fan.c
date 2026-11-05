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
    TEST_ASSERT_EQUAL_INT(-1, tegra_pwm_pfm(UINT64_MAX, TEGRA_FAN_PERIOD_NS, &pfm));
    TEST_ASSERT_EQUAL_INT(-1, tegra_pwm_pfm(11027028u, 0u, &pfm));
    /* The two rates fan_clock_pwm can land on: exactly the requested
     * one (pfm 0) and the doubled fallback (pfm 1). */
    uint64_t want = tegra_pwm_required_rate(TEGRA_FAN_PERIOD_NS);
    TEST_ASSERT_EQUAL_INT(0, tegra_pwm_pfm(want, TEGRA_FAN_PERIOD_NS, &pfm));
    TEST_ASSERT_EQUAL_UINT32(0, pfm);
    TEST_ASSERT_EQUAL_INT(0, tegra_pwm_pfm(want * 2u, TEGRA_FAN_PERIOD_NS, &pfm));
    TEST_ASSERT_EQUAL_UINT32(1, pfm);
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

/* ---- control state machine ---- */

static void test_ctl_init_start_level(void)
{
    struct tegra_fan_ctl c;
    TEST_ASSERT_EQUAL_UINT32(70, tegra_fan_ctl_init(&c, 0, 49500));
    TEST_ASSERT_EQUAL_UINT32(70, c.target);
    TEST_ASSERT_FALSE(c.manual);
    /* Curve level below the kickstart: start at 64, target stays. */
    TEST_ASSERT_EQUAL_UINT32(64, tegra_fan_ctl_init(&c, 0, 40000));
    TEST_ASSERT_EQUAL_UINT32(24, c.target);
    /* Cool board: off. Unknown temperature: full speed. */
    TEST_ASSERT_EQUAL_UINT32(0, tegra_fan_ctl_init(&c, 0, 30000));
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_init(&c, -4, 0));
    TEST_ASSERT_FALSE(c.temp_ok);
    /* ... and that counts as lost: a manual 0 is refused, auto keeps
     * 255, and BPMP waits for the back-off retry. */
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_manual(&c, 0));
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_auto(&c, false));
    TEST_ASSERT_FALSE(tegra_fan_ctl_should_read(&c));
}

/* Skip the back-off window the way the driver does. */
static void drain_backoff(struct tegra_fan_ctl *c)
{
    for (uint32_t i = 0; i < TEGRA_FAN_RETRY_POLLS; i++) {
        if (tegra_fan_ctl_should_read(c)) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("back-off never ended");
}

static void test_ctl_auto_ramps_to_curve(void)
{
    struct tegra_fan_ctl c;
    tegra_fan_ctl_init(&c, 0, 49500);                    /* 70 */
    TEST_ASSERT_EQUAL_UINT32(80, tegra_fan_ctl_step(&c, 0, 60000)); /* -> 120 */
    TEST_ASSERT_EQUAL_UINT32(120, c.target);
    TEST_ASSERT_EQUAL_UINT32(90, tegra_fan_ctl_step(&c, 0, 60000));
    /* >= 95 C: straight to 255. */
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_step(&c, 0, 96000));
    /* Cooling: back down 10 per poll. */
    TEST_ASSERT_EQUAL_UINT32(245, tegra_fan_ctl_step(&c, 0, 49500));
    TEST_ASSERT_EQUAL_UINT32(70, c.target);
}

static void test_ctl_read_failures_hold_then_max_then_backoff(void)
{
    struct tegra_fan_ctl c;
    tegra_fan_ctl_init(&c, 0, 49500);                    /* 70 */
    /* One failure: hold the level and target. */
    TEST_ASSERT_TRUE(tegra_fan_ctl_should_read(&c));
    TEST_ASSERT_EQUAL_UINT32(70, tegra_fan_ctl_step(&c, -4, 0));
    TEST_ASSERT_EQUAL_UINT32(70, c.target);
    TEST_ASSERT_TRUE(c.temp_ok);
    /* A success in between resets the count. */
    TEST_ASSERT_EQUAL_UINT32(70, tegra_fan_ctl_step(&c, 0, 49500));
    TEST_ASSERT_EQUAL_UINT32(70, tegra_fan_ctl_step(&c, -4, 0));
    /* Second in a row: temperature lost, 255, back off. */
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_step(&c, -4, 0));
    TEST_ASSERT_FALSE(c.temp_ok);
    for (uint32_t i = 0; i < TEGRA_FAN_RETRY_POLLS - 1u; i++) {
        TEST_ASSERT_FALSE(tegra_fan_ctl_should_read(&c));
    }
    /* The 8th poll retries; still dead: 255 and back off again. */
    TEST_ASSERT_TRUE(tegra_fan_ctl_should_read(&c));
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_step(&c, -4, 0));
    TEST_ASSERT_FALSE(tegra_fan_ctl_should_read(&c));
    /* BPMP back at the next retry: ramp down to the curve. */
    drain_backoff(&c);
    TEST_ASSERT_EQUAL_UINT32(245, tegra_fan_ctl_step(&c, 0, 49500));
    TEST_ASSERT_TRUE(tegra_fan_ctl_should_read(&c));
    TEST_ASSERT_EQUAL_UINT32(70, c.target);
}

static void test_ctl_failed_poll_mid_ramp(void)
{
    struct tegra_fan_ctl c;
    tegra_fan_ctl_init(&c, 0, 30000);                    /* off */
    TEST_ASSERT_EQUAL_UINT32(64, tegra_fan_ctl_step(&c, 0, 60000)); /* -> 120 */
    /* One failed poll: the target holds and the ramp keeps going. */
    TEST_ASSERT_EQUAL_UINT32(74, tegra_fan_ctl_step(&c, -4, 0));
    TEST_ASSERT_EQUAL_UINT32(120, c.target);
    TEST_ASSERT_EQUAL_UINT32(84, tegra_fan_ctl_step(&c, 0, 60000));
}

static void test_ctl_manual_and_safety_override(void)
{
    struct tegra_fan_ctl c;
    tegra_fan_ctl_init(&c, 0, 49500);
    TEST_ASSERT_EQUAL_UINT32(0, tegra_fan_ctl_manual(&c, 0));
    TEST_ASSERT_TRUE(c.manual);
    /* Manual holds against the curve and a single read failure. */
    TEST_ASSERT_EQUAL_UINT32(0, tegra_fan_ctl_step(&c, 0, 80000));
    TEST_ASSERT_EQUAL_UINT32(0, tegra_fan_ctl_step(&c, -4, 0));
    /* ... but not against a lost temperature, */
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_step(&c, -4, 0));
    TEST_ASSERT_TRUE(c.manual);
    /* nor a new manual level while it is lost (backoff window). */
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_manual(&c, 20));
    /* Latched: recovery keeps manual 255 ... */
    drain_backoff(&c);
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_step(&c, 0, 49500));
    TEST_ASSERT_TRUE(c.manual);
    /* ... until a new manual level, now that the temperature is back. */
    TEST_ASSERT_EQUAL_UINT32(40, tegra_fan_ctl_manual(&c, 40));
    /* >= 95 C overrides a manual level too. */
    tegra_fan_ctl_init(&c, 0, 49500);
    tegra_fan_ctl_manual(&c, 30);
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_step(&c, 0, 95000));
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_manual(&c, 30));
}

static void test_ctl_auto(void)
{
    struct tegra_fan_ctl c;
    tegra_fan_ctl_init(&c, 0, 49500);
    tegra_fan_ctl_manual(&c, 200);
    /* With the task: target at once, level ramps on the next poll. */
    TEST_ASSERT_EQUAL_UINT32(200, tegra_fan_ctl_auto(&c, false));
    TEST_ASSERT_FALSE(c.manual);
    TEST_ASSERT_EQUAL_UINT32(70, c.target);
    TEST_ASSERT_EQUAL_UINT32(190, tegra_fan_ctl_step(&c, 0, 49500));
    /* Without it (test kernels): the fixed level, not a stale curve. */
    tegra_fan_ctl_manual(&c, 0);
    TEST_ASSERT_EQUAL_UINT32(TEGRA_FAN_FIXED_PWM, tegra_fan_ctl_auto(&c, true));
    /* Temperature lost: auto goes to 255 at once, either way. */
    tegra_fan_ctl_manual(&c, 0);
    tegra_fan_ctl_step(&c, -4, 0);
    tegra_fan_ctl_step(&c, -4, 0);
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_auto(&c, false));
    TEST_ASSERT_EQUAL_UINT32(255, c.target);
    /* ... and without the task: 255, not the fixed level. */
    tegra_fan_ctl_manual(&c, 0);
    TEST_ASSERT_EQUAL_UINT32(255, tegra_fan_ctl_auto(&c, true));
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
    TEST_ASSERT_EQUAL_INT(-1, tegra_fan_set_auto());
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
    RUN_TEST(test_ctl_init_start_level);
    RUN_TEST(test_ctl_auto_ramps_to_curve);
    RUN_TEST(test_ctl_read_failures_hold_then_max_then_backoff);
    RUN_TEST(test_ctl_failed_poll_mid_ramp);
    RUN_TEST(test_ctl_manual_and_safety_override);
    RUN_TEST(test_ctl_auto);
#if !defined(PLATFORM_JETSON_ORIN_NANO)
    RUN_TEST(test_stubs_off_jetson);
#endif
    return UnityEnd();
}
