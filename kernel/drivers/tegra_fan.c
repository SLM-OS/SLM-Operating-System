/*
 * tegra_fan.c - Jetson Orin Nano carrier fan control (#1105).
 * See kernel/include/tegra_fan.h.
 */
#include "tegra_fan.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "bpmp.h"
#include "debug.h"
#include "platform.h"
#include "sched.h"
#include "smp.h"
#include "spinlock.h"
#include "task.h"
#include "uart.h"

/* Serialises the PWM register and the state below between the control
 * task and the shell. BPMP calls are made outside it. */
static spinlock_t g_fan_lock = SPINLOCK_INIT;

static struct {
    bool ready;
    bool manual;
    bool tach_ok;
    uint32_t pfm;
    uint32_t pwm;
    uint32_t target;
    int32_t temp_mc;
    int temp_rc;
    uint64_t pwm_clk_hz;
} g_fan;

static volatile uint32_t *pwm_csr(void)
{
    return (volatile uint32_t *)(uintptr_t)TEGRA234_PWM3_BASE;
}

static volatile uint32_t *tach0_reg(void)
{
    return (volatile uint32_t *)(uintptr_t)TEGRA234_TACH0_BASE;
}

/* Caller holds g_fan_lock. */
static void fan_apply_locked(uint32_t pwm)
{
    *pwm_csr() = tegra_pwm_csr(pwm, g_fan.pfm);
    __asm__ volatile("dsb sy" ::: "memory");
    g_fan.pwm = pwm;
}

/* Hottest of the zones nvfancontrol's thermal group uses (CPU, GPU,
 * SOC0-2). Returns 0 with *mc set, or the last BPMP error when no zone
 * could be read. */
static int fan_read_hottest(int32_t *mc)
{
    static const uint32_t zones[] = {
        TEGRA234_BPMP_THERMAL_ZONE_CPU, TEGRA234_BPMP_THERMAL_ZONE_GPU,
        TEGRA234_BPMP_THERMAL_ZONE_SOC0, TEGRA234_BPMP_THERMAL_ZONE_SOC1,
        TEGRA234_BPMP_THERMAL_ZONE_SOC2,
    };
    int rc = -1;
    bool any = false;
    int32_t hottest = 0;
    for (unsigned i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
        int32_t t = 0;
        int r = bpmp_thermal_get_temp(zones[i], &t);
        if (r != 0) {
            rc = r;
            continue;
        }
        if (!any || t > hottest) {
            hottest = t;
        }
        any = true;
    }
    if (!any) {
        return rc;
    }
    *mc = hottest;
    return 0;
}

static int fan_clock_pwm(void)
{
    if (bpmp_clk_enable(TEGRA234_CLK_PWM3) != 0) {
        return -1;
    }
    /* pwm-tegra: ask for 256 ticks per period; if BPMP rounds below
     * that, ask for twice as much and divide it down. */
    uint64_t want = tegra_pwm_required_rate(TEGRA_FAN_PERIOD_NS);
    uint64_t got = 0;
    if (bpmp_clk_set_rate(TEGRA234_CLK_PWM3, want, &got) != 0) {
        return -2;
    }
    if (got < want && bpmp_clk_set_rate(TEGRA234_CLK_PWM3, want * 2u, &got) != 0) {
        return -2;
    }
    uint32_t pfm = 0;
    if (tegra_pwm_pfm(got, TEGRA_FAN_PERIOD_NS, &pfm) != 0) {
        return -3;
    }
    if (bpmp_reset_deassert(TEGRA234_RESET_PWM3) != 0) {
        return -4;
    }
    g_fan.pfm = pfm;
    g_fan.pwm_clk_hz = got;
    return 0;
}

/* Tachometer, as pwm-tegra-tachometer.c probes it: clock at the
 * counter rate, pulse the reset, set the capture window. Optional: a
 * failure only loses the RPM readout. */
static bool fan_clock_tach(void)
{
    uint64_t got = 0;
    if (bpmp_clk_enable(TEGRA234_CLK_TACH0) != 0 ||
        bpmp_clk_set_rate(TEGRA234_CLK_TACH0, TEGRA_TACH_COUNTER_HZ, &got) != 0 ||
        bpmp_reset_assert(TEGRA234_RESET_TACH0) != 0 ||
        bpmp_reset_deassert(TEGRA234_RESET_TACH0) != 0) {
        return false;
    }
    uint32_t v = *tach0_reg();
    v = (v & ~TEGRA_TACH_WIN_MASK) | (1u << TEGRA_TACH_WIN_SHIFT);   /* window 2 */
    *tach0_reg() = v;
    __asm__ volatile("dsb sy" ::: "memory");
    return true;
}

int tegra_fan_init(void)
{
    if (!bpmp_is_available()) {
        return -1;
    }
    int rc = fan_clock_pwm();
    if (rc != 0) {
        WARN("fan: PWM3 setup failed (step %d) - fan not driven", -rc);
        return rc;
    }
    bool tach_ok = fan_clock_tach();

#if defined(ENABLE_BOOT_TESTS)
    uint32_t start = TEGRA_FAN_FIXED_PWM;
    int32_t mc = 0;
    int trc = 0;
#else
    int32_t mc = 0;
    int trc = fan_read_hottest(&mc);
    /* Unknown temperature: run flat out until the task reads one. */
    uint32_t start = trc == 0 ? tegra_fan_curve_pwm(mc) : TEGRA_FAN_MAX_PWM;
    if (start > 0u && start < TEGRA_FAN_KICKSTART_PWM) {
        start = TEGRA_FAN_KICKSTART_PWM;
    }
#endif

    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    g_fan.tach_ok = tach_ok;
    g_fan.temp_mc = mc;
    g_fan.temp_rc = trc;
    g_fan.target = start;
    fan_apply_locked(start);
    g_fan.ready = true;
    spin_unlock_irqrestore(&g_fan_lock, f);

    INFO("fan: PWM3 at %llu Hz (pfm %u), level %u/255, tachometer %s",
         (unsigned long long)g_fan.pwm_clk_hz, (unsigned)g_fan.pfm, (unsigned)start,
         tach_ok ? "on" : "unavailable");
    return 0;
}

void tegra_fan_poll(void)
{
    if (!g_fan.ready) {
        return;
    }
    int32_t mc = 0;
    int rc = fan_read_hottest(&mc);
    uint32_t target = rc == 0 ? tegra_fan_curve_pwm(mc) : TEGRA_FAN_MAX_PWM;

    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    g_fan.temp_rc = rc;
    if (rc == 0) {
        g_fan.temp_mc = mc;
    }
    if (!g_fan.manual) {
        g_fan.target = target;
        uint32_t next = tegra_fan_next_pwm(g_fan.pwm, target);
        if (next != g_fan.pwm) {
            fan_apply_locked(next);
        }
    }
    spin_unlock_irqrestore(&g_fan_lock, f);
}

static void fan_task(void *arg)
{
    (void)arg;
    for (;;) {
        tegra_fan_poll();
        task_sleep_ms(TEGRA_FAN_POLL_MS);
    }
}

int tegra_fan_start(void)
{
    if (!g_fan.ready) {
        return -1;
    }
    struct task *t = task_create("fan_ctl", fan_task, NULL);
    if (t == NULL) {
        return -1;
    }
    /* Off CPU 0: net_pump and the shells run there, and a wedged CPU 0
     * must not stop the fan (#1093). */
    if (cpu_count > 1) {
        t->cpu_affinity = cpu_count - 1;
    }
    scheduler_add_task(t);
    return 0;
}

int tegra_fan_set_manual(uint32_t pwm)
{
    if (!g_fan.ready || pwm > TEGRA_FAN_MAX_PWM) {
        return -1;
    }
    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    g_fan.manual = true;
    g_fan.target = pwm;
    fan_apply_locked(pwm);
    spin_unlock_irqrestore(&g_fan_lock, f);
    return 0;
}

void tegra_fan_set_auto(void)
{
    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    g_fan.manual = false;
    spin_unlock_irqrestore(&g_fan_lock, f);
}

void tegra_fan_get_status(struct tegra_fan_status *out)
{
    if (out == NULL) {
        return;
    }
    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    out->ready = g_fan.ready;
    out->manual = g_fan.manual;
    out->tach_ok = g_fan.tach_ok;
    out->pwm = g_fan.pwm;
    out->target = g_fan.target;
    out->temp_mc = g_fan.temp_mc;
    out->temp_rc = g_fan.temp_rc;
    out->pwm_clk_hz = g_fan.pwm_clk_hz;
    out->csr = g_fan.ready ? *pwm_csr() : 0u;
    spin_unlock_irqrestore(&g_fan_lock, f);
    uint32_t tach = g_fan.tach_ok ? *tach0_reg() : 0u;
    if (tach & TEGRA_TACH_OVERFLOW) {
        *tach0_reg() = tach;   /* write 1 to clear the stall flag */
    }
    out->rpm = tegra_tach_rpm(tach);
}

#else /* !PLATFORM_JETSON_ORIN_NANO */

int  tegra_fan_init(void)              { return -1; }
int  tegra_fan_start(void)             { return -1; }
void tegra_fan_poll(void)              { }
int  tegra_fan_set_manual(uint32_t p)  { (void)p; return -1; }
void tegra_fan_set_auto(void)          { }
void tegra_fan_get_status(struct tegra_fan_status *out)
{
    if (out != 0) {
        *out = (struct tegra_fan_status){ 0 };
    }
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
