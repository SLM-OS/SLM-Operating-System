/*
 * tegra_fan.h - Jetson Orin Nano carrier fan control (#1105).
 *
 * Linux drives the fan from userspace: nvfancontrol reads the thermal
 * zones every 2 s and sets pwm-fan's level, and pwm-fan programs PWM
 * controller 3 (pwm-tegra). SLM-OS has neither, so the fan was never
 * driven: after a UEFI-direct boot PWM3 is unclocked, and after kexec
 * Linux has disabled it. This driver clocks PWM3 and the tachometer
 * through BPMP and runs the same curve as nvfancontrol's default
 * "quiet" profile from a kernel task.
 *
 * The helpers below are pure (host-tested on every platform); the
 * driver itself (kernel/drivers/tegra_fan.c) is Jetson-only.
 */
#ifndef TEGRA_FAN_H
#define TEGRA_FAN_H

#include <stdbool.h>
#include <stdint.h>

/* L4T device tree: pwm-fan { pwms = <&pwm3 0 45334>; } */
#define TEGRA_FAN_PERIOD_NS      45334u
/* nvfancontrol.conf: POLLING_INTERVAL 2, STEP_SIZE 10, KICKSTART_PWM 64 */
#define TEGRA_FAN_POLL_MS        2000u
#define TEGRA_FAN_STEP_PWM       10u
#define TEGRA_FAN_KICKSTART_PWM  64u
#define TEGRA_FAN_MAX_PWM        255u
/* Test kernels run no control task: a fixed, safe duty instead. */
#define TEGRA_FAN_FIXED_PWM      128u

/* PWM controller (pwm-tegra.c): CSR bit 31 enable, duty in bits 24:16
 * (ticks out of 256, so 100 % sets bit 24), divider in bits 12:0. */
#define TEGRA_PWM_ENABLE         0x80000000u
#define TEGRA_PWM_DUTY_SHIFT     16
#define TEGRA_PWM_SCALE_MASK     0x1FFFu

/* Tachometer (nvidia-oot pwm-tegra-tachometer.c, L4T DT). */
#define TEGRA_TACH_COUNTER_HZ    1010526u
#define TEGRA_TACH_PULSE_PER_REV 2u
#define TEGRA_TACH_WINDOW        2u
#define TEGRA_TACH_PERIOD_MASK   0x7FFFFu
#define TEGRA_TACH_OVERFLOW      (1u << 24)
#define TEGRA_TACH_WIN_SHIFT     25
#define TEGRA_TACH_WIN_MASK      (0x3u << TEGRA_TACH_WIN_SHIFT)

/* nvfancontrol "quiet" profile with TMARGIN, margin = 105 C - the
 * hottest of CPU / GPU / SOC0-2:
 *   margin 0-10 (>= 95 C)  -> 255
 *   margin 11-31 (74-94 C) -> 187
 *   margin 31-70 (74-35 C) -> 187 down to 0, linearly
 *   margin >= 70 (<= 35 C) -> 0 */
static inline uint32_t tegra_fan_curve_pwm(int32_t temp_mc)
{
    if (temp_mc >= 95000) return 255u;
    if (temp_mc >= 74000) return 187u;
    if (temp_mc <= 35000) return 0u;
    return (uint32_t)(((int64_t)(temp_mc - 35000) * 187 + 19500) / 39000);
}

/* Next PWM on the way from `cur` to `target`: at most STEP_PWM per
 * poll, a stopped fan is kicked at KICKSTART_PWM first, and the
 * >= 95 C level (255) is applied at once. */
static inline uint32_t tegra_fan_next_pwm(uint32_t cur, uint32_t target)
{
    if (target > TEGRA_FAN_MAX_PWM) target = TEGRA_FAN_MAX_PWM;
    if (target == cur) return cur;
    if (target == TEGRA_FAN_MAX_PWM) return TEGRA_FAN_MAX_PWM;
    if (cur == 0u) return TEGRA_FAN_KICKSTART_PWM;
    if (target > cur) {
        return target - cur > TEGRA_FAN_STEP_PWM ? cur + TEGRA_FAN_STEP_PWM : target;
    }
    return cur - target > TEGRA_FAN_STEP_PWM ? cur - TEGRA_FAN_STEP_PWM : target;
}

/* Duty ticks for a 0-255 fan level, as pwm-fan + pwm-tegra compute it:
 * duty_ns = pwm * period / 255, ticks = round(duty_ns * 256 / period). */
static inline uint32_t tegra_pwm_duty_ticks(uint32_t pwm)
{
    if (pwm > TEGRA_FAN_MAX_PWM) pwm = TEGRA_FAN_MAX_PWM;
    return (pwm * 256u + 127u) / 255u;
}

static inline uint32_t tegra_pwm_csr(uint32_t pwm, uint32_t pfm)
{
    return TEGRA_PWM_ENABLE | (tegra_pwm_duty_ticks(pwm) << TEGRA_PWM_DUTY_SHIFT) |
           (pfm & TEGRA_PWM_SCALE_MASK);
}

/* Source clock pwm-tegra asks for: 256 ticks per period, rounded up. */
static inline uint64_t tegra_pwm_required_rate(uint32_t period_ns)
{
    return ((1000000000ull << 8) + period_ns - 1u) / period_ns;
}

/* Divider for the clock BPMP actually set; -1 if it doesn't fit. */
static inline int tegra_pwm_pfm(uint64_t rate_hz, uint32_t period_ns, uint32_t *pfm)
{
    uint64_t r = rate_hz * period_ns / (1000000000ull << 8);
    if (r == 0u || ((r - 1u) >> 13) != 0u) return -1;
    *pfm = (uint32_t)(r - 1u);
    return 0;
}

/* RPM from a TACH0 register value (0 when stalled or invalid). */
static inline uint32_t tegra_tach_rpm(uint32_t tach0)
{
    uint32_t period = tach0 & TEGRA_TACH_PERIOD_MASK;
    if ((tach0 & TEGRA_TACH_OVERFLOW) || period == 0u || period == TEGRA_TACH_PERIOD_MASK) {
        return 0u;
    }
    uint64_t div = (uint64_t)TEGRA_TACH_WINDOW * TEGRA_TACH_COUNTER_HZ;
    uint64_t us = ((uint64_t)(period + 1u) * TEGRA_TACH_PULSE_PER_REV * 1000000ull + div / 2u) / div;
    if (us == 0u) return 0u;
    return (uint32_t)((60000000ull + us / 2u) / us);
}

/* ---- Jetson driver (kernel/drivers/tegra_fan.c) ---- */

struct tegra_fan_status {
    bool ready;            /* PWM clocked and programmed */
    bool manual;           /* level fixed from the shell */
    bool tach_ok;          /* tachometer clocked */
    uint32_t pwm;          /* level applied now (0-255) */
    uint32_t target;       /* curve (or manual) target */
    int32_t temp_mc;       /* hottest zone last poll */
    int temp_rc;           /* 0, or the BPMP error of the last poll */
    uint32_t rpm;
    uint32_t csr;
    uint64_t pwm_clk_hz;
};

/* Clock and program PWM3 and the tachometer through BPMP and apply the
 * curve for the current temperature (TEGRA_FAN_FIXED_PWM in test
 * kernels). Returns 0, or a negative step number on failure (the fan
 * is then left as it was). */
int tegra_fan_init(void);

/* Start the 2 s control task, pinned off CPU 0 so the fan keeps being
 * driven if CPU 0 wedges (#1093). Silent: it never prints. */
int tegra_fan_start(void);

/* One control step: read the zones, move the PWM toward the curve. */
void tegra_fan_poll(void);

/* Shell: fix the level (0-255), or return to the curve. */
int tegra_fan_set_manual(uint32_t pwm);
void tegra_fan_set_auto(void);

void tegra_fan_get_status(struct tegra_fan_status *out);

#endif /* TEGRA_FAN_H */
