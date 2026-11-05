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

/* L4T device tree: pwm-fan { pwms = <&pwm3 0 45334>; }. The PWM runs
 * faster than this: BPMP rounds the clock up (11.03 MHz after Linux,
 * 9.6 MHz after UEFI) and the 13-bit divider bottoms out at 0, so the
 * real period is 256 / clock (23 or 27 us). Linux runs it the same way
 * (CSR 0x803c0000 at level 60); the fan follows duty, not frequency. */
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
#define TEGRA_PWM_DUTY_MASK      0x1FFu     /* bits 24:16, 0-256 ticks */

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
 *   margin <= 10 (>= 95 C)    -> 255
 *   margin 11-31 (74-94 C)    -> 187
 *   35-74 C                   -> linear, 0 up to 187 (reached ~73.9 C)
 *   margin >= 70 (<= 35 C)    -> 0 */
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
    if (period_ns == 0u || rate_hz > UINT64_MAX / period_ns) return -1;
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

/* ---- Control state machine (pure; the driver holds its lock) ----
 *
 * Each poll: tegra_fan_ctl_should_read() says whether to ask BPMP at
 * all; tegra_fan_ctl_step() takes the result and moves the level.
 *  - One unreadable poll holds the target (a BPMP timeout is not worth
 *    40 s of full speed); TEGRA_FAN_TEMP_FAILS_TO_MAX in a row mean the
 *    temperature is lost: 255, and BPMP is then retried only every
 *    TEGRA_FAN_RETRY_POLLS-th poll (each failed MRQ spins up to 100 ms
 *    with IRQs masked).
 *  - >= 95 C or a lost temperature forces 255 over a manual level. The
 *    level stays manual at 255 until `auto` or a new manual level
 *    once the temperature is back.
 *  - Auto ramps toward the curve with tegra_fan_next_pwm(). */
#define TEGRA_FAN_TEMP_FAILS_TO_MAX  2u
#define TEGRA_FAN_RETRY_POLLS        8u

struct tegra_fan_ctl {
    bool manual;
    bool temp_ok;          /* temp_mc holds a reading */
    int32_t temp_mc;
    uint32_t pwm;          /* level applied */
    uint32_t target;
    uint32_t temp_fails;   /* consecutive unreadable polls (saturates) */
    uint32_t backoff;      /* polls to skip before retrying BPMP */
};

static inline bool tegra_fan_ctl_temp_lost(const struct tegra_fan_ctl *c)
{
    return c->temp_fails >= TEGRA_FAN_TEMP_FAILS_TO_MAX;
}

/* Level the safety rules force regardless of mode, or 0 for none. */
static inline uint32_t tegra_fan_ctl_forced(const struct tegra_fan_ctl *c)
{
    if (tegra_fan_ctl_temp_lost(c)) return TEGRA_FAN_MAX_PWM;
    if (c->temp_ok && tegra_fan_curve_pwm(c->temp_mc) == TEGRA_FAN_MAX_PWM) return TEGRA_FAN_MAX_PWM;
    return 0u;
}

/* Boot: the start level for a first reading (rc 0 = mc valid). An
 * unknown temperature starts at 255; a low curve level is raised to
 * the kickstart so a stopped fan actually spins up. */
static inline uint32_t tegra_fan_ctl_init(struct tegra_fan_ctl *c, int rc, int32_t mc)
{
    *c = (struct tegra_fan_ctl){ 0 };
    if (rc != 0) {
        /* No reading at boot counts as lost: 255 that a manual level
         * can't lower, and BPMP retried on the back-off schedule. */
        c->temp_fails = TEGRA_FAN_TEMP_FAILS_TO_MAX;
        c->backoff = TEGRA_FAN_RETRY_POLLS - 1u;
    }
    c->temp_ok = rc == 0;
    c->temp_mc = rc == 0 ? mc : 0;
    uint32_t start = rc == 0 ? tegra_fan_curve_pwm(mc) : TEGRA_FAN_MAX_PWM;
    if (start > 0u && start < TEGRA_FAN_KICKSTART_PWM) {
        start = TEGRA_FAN_KICKSTART_PWM;
    }
    c->pwm = start;
    c->target = rc == 0 ? tegra_fan_curve_pwm(mc) : TEGRA_FAN_MAX_PWM;
    return start;
}

/* False while backing off from a dead BPMP (and counts the skip). */
static inline bool tegra_fan_ctl_should_read(struct tegra_fan_ctl *c)
{
    if (c->backoff > 0u) {
        c->backoff--;
        return false;
    }
    return true;
}

/* One poll's result (rc 0 = mc valid). Returns the level to apply. */
static inline uint32_t tegra_fan_ctl_step(struct tegra_fan_ctl *c, int rc, int32_t mc)
{
    if (rc == 0) {
        c->temp_ok = true;
        c->temp_mc = mc;
        c->temp_fails = 0;
    } else {
        if (c->temp_fails < TEGRA_FAN_TEMP_FAILS_TO_MAX) {
            c->temp_fails++;
        }
        if (tegra_fan_ctl_temp_lost(c)) {
            c->temp_ok = false;
            c->backoff = TEGRA_FAN_RETRY_POLLS - 1u;
        }
    }
    uint32_t forced = tegra_fan_ctl_forced(c);
    if (c->manual) {
        if (forced != 0u) {
            c->target = forced;
            c->pwm = forced;
        }
        return c->pwm;
    }
    if (forced != 0u) {
        c->target = forced;
    } else {
        /* temp_ok here: forced covers a lost temperature. After one
         * failed poll temp_mc is the last reading, so the target holds
         * while the level keeps ramping toward it. */
        c->target = tegra_fan_curve_pwm(c->temp_mc);
    }
    c->pwm = tegra_fan_next_pwm(c->pwm, c->target);
    return c->pwm;
}

/* Shell: hold `level` (0-255). Returns the level applied, which is 255
 * instead when the safety rules force it. */
static inline uint32_t tegra_fan_ctl_manual(struct tegra_fan_ctl *c, uint32_t level)
{
    uint32_t forced = tegra_fan_ctl_forced(c);
    c->manual = true;
    c->target = forced != 0u ? forced : level;
    c->pwm = c->target;
    return c->pwm;
}

/* Shell: back to the curve. The target is set at once and the next
 * poll ramps toward it; with no control task to do that (`no_task`,
 * test kernels) the level is set directly: the forced level, or
 * TEGRA_FAN_FIXED_PWM, since nothing refreshes the temperature there.
 * Returns the level to apply. */
static inline uint32_t tegra_fan_ctl_auto(struct tegra_fan_ctl *c, bool no_task)
{
    uint32_t forced = tegra_fan_ctl_forced(c);
    c->manual = false;
    if (forced != 0u) {
        c->target = forced;
        c->pwm = forced;
    } else if (no_task) {
        c->target = TEGRA_FAN_FIXED_PWM;
        c->pwm = TEGRA_FAN_FIXED_PWM;
    } else if (c->temp_ok) {
        c->target = tegra_fan_curve_pwm(c->temp_mc);
    }
    return c->pwm;
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
 * kernels). Returns 0; -1 if BPMP is unavailable or the clock enable
 * failed; -2..-4 for a later PWM3 step, after which PWM3 is left
 * unclocked and the fan undriven. */
int tegra_fan_init(void);

/* Start the 2 s control task, pinned to the highest online CPU other
 * than CPU 0 so the fan keeps being driven if CPU 0 wedges (#1093). It
 * prints only when the temperature is lost or comes back; BPMP
 * timeouts also show as the MRQ layer's WARNs, one per retry (every
 * 16 s once the temperature is lost). */
int tegra_fan_start(void);

/* One control step (see the state machine above). */
void tegra_fan_poll(void);

/* Shell: hold a level (0-255), or return to the curve
 * (tegra_fan_ctl_manual / tegra_fan_ctl_auto). Both return -1 when the
 * driver is not running. */
int tegra_fan_set_manual(uint32_t pwm);
int tegra_fan_set_auto(void);

/* Snapshot for the shell. Also clears a set tachometer overflow (stall)
 * flag, write-1-to-clear, as Linux's capture path does. */
void tegra_fan_get_status(struct tegra_fan_status *out);

#endif /* TEGRA_FAN_H */
