/*
 * tegra_fan.c - Jetson Orin Nano carrier fan control (#1105).
 * See kernel/include/tegra_fan.h.
 */
#include "tegra_fan.h"

#include <stddef.h>

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "bpmp.h"
#include "emc_hold.h"
#include "debug.h"
#include "platform.h"
#include "sched.h"
#include "smp.h"
#include "spinlock.h"
#include "task.h"

/* The tachometer has no 2 MB block of its own in vmm.c: it is reached
 * through the FUSE block's mapping. Moving either breaks the other. */
_Static_assert((TEGRA234_TACH0_BASE & ~0x1FFFFFUL) == (T234_FUSE_MMIO_BASE & ~0x1FFFFFUL),
               "TACH0 must share the FUSE 2 MB block mapped in vmm.c");

/* Serialises the PWM and tachometer registers and the state below
 * between the control task and the shells. BPMP calls are made outside
 * it. `ready`, `tach_ok`, `pfm` and `pwm_clk_hz` are written once by
 * tegra_fan_init, before the task or any shell exists, so reading them
 * unlocked is safe. */
static spinlock_t g_fan_lock = SPINLOCK_INIT;

static struct {
    bool ready;
    bool tach_ok;
    bool task_started;
    uint32_t pfm;
    uint64_t pwm_clk_hz;
    int temp_rc;               /* last poll's BPMP result, for the shell */
    struct tegra_fan_ctl ctl;  /* tegra_fan.h state machine */
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
static void fan_program_locked(uint32_t pwm)
{
    *pwm_csr() = tegra_pwm_csr(pwm, g_fan.pfm);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Caller holds g_fan_lock. Writes the CSR only when the level changed
 * (pwm_now is what the hardware has). */
static void fan_apply_locked(uint32_t pwm_now, uint32_t pwm)
{
    if (pwm != pwm_now) {
        fan_program_locked(pwm);
    }
}

/* Hottest of the zones nvfancontrol's thermal group uses (CPU, GPU,
 * SOC0-2). Returns 0 with *mc set only when every zone was read;
 * otherwise the BPMP error, so a zone that keeps failing counts as a
 * failed poll (two in a row: temperature lost, 255) instead of quietly
 * dropping out of the max. Stops at the first error: each failed MRQ
 * can spin for its 100 ms timeout with IRQs masked. */
static int fan_read_hottest(int32_t *mc)
{
    static const uint32_t zones[] = {
        TEGRA234_BPMP_THERMAL_ZONE_CPU, TEGRA234_BPMP_THERMAL_ZONE_GPU,
        TEGRA234_BPMP_THERMAL_ZONE_SOC0, TEGRA234_BPMP_THERMAL_ZONE_SOC1,
        TEGRA234_BPMP_THERMAL_ZONE_SOC2,
    };
    int32_t hottest = INT32_MIN;
    for (unsigned i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
        int32_t t = 0;
        int r = bpmp_thermal_get_temp(zones[i], &t);
        if (r != 0) {
            return r;
        }
        if (t > hottest) {
            hottest = t;
        }
    }
    *mc = hottest;
    return 0;
}

/* Returns 0, or -1..-4 for the step that failed. A failure after the
 * clock is on turns it back off: `thermal fan`'s raw view reads the CSR
 * whenever BPMP reports the clock on, and a clocked block still held in
 * reset may fault. */
static int fan_clock_pwm(void)
{
    if (bpmp_clk_enable(TEGRA234_CLK_PWM3) != 0) {
        return -1;
    }
    int rc = 0;
    /* pwm-tegra: ask for 256 ticks per period; if BPMP rounds below
     * that, ask for twice as much and divide it down. */
    uint64_t want = tegra_pwm_required_rate(TEGRA_FAN_PERIOD_NS);
    uint64_t got = 0;
    uint32_t pfm = 0;
    if (bpmp_clk_set_rate(TEGRA234_CLK_PWM3, want, &got) != 0) {
        rc = -2;
        goto fail;
    }
    if (got < want && bpmp_clk_set_rate(TEGRA234_CLK_PWM3, want * 2u, &got) != 0) {
        rc = -2;
        goto fail;
    }
    if (tegra_pwm_pfm(got, TEGRA_FAN_PERIOD_NS, &pfm) != 0) {
        rc = -3;
        goto fail;
    }
    if (bpmp_reset_deassert(TEGRA234_RESET_PWM3) != 0) {
        rc = -4;
        goto fail;
    }
    g_fan.pfm = pfm;
    g_fan.pwm_clk_hz = got;
    return 0;

fail:
    (void)bpmp_clk_disable(TEGRA234_CLK_PWM3);
    return rc;
}

/* Tachometer, as pwm-tegra-tachometer.c probes it: clock at the
 * counter rate, pulse the reset, set the capture window. Optional: a
 * failure only loses the RPM readout. */
static bool fan_clock_tach(void)
{
    uint64_t got = 0;
    if (bpmp_clk_enable(TEGRA234_CLK_TACH0) != 0) {
        return false;
    }
    if (bpmp_clk_set_rate(TEGRA234_CLK_TACH0, TEGRA_TACH_COUNTER_HZ, &got) != 0 ||
        bpmp_reset_assert(TEGRA234_RESET_TACH0) != 0 ||
        bpmp_reset_deassert(TEGRA234_RESET_TACH0) != 0) {
        /* As for PWM3: no clocked block left behind in reset. */
        (void)bpmp_clk_disable(TEGRA234_CLK_TACH0);
        return false;
    }
    uint32_t v = *tach0_reg();
    /* Field = log2(window), as pwm-tegra-tachometer.c's ffs(len) - 1. */
    _Static_assert(TEGRA_TACH_WINDOW == 2u, "window field below encodes 2");
    v = (v & ~TEGRA_TACH_WIN_MASK) | (1u << TEGRA_TACH_WIN_SHIFT);
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

    int32_t mc = 0;
    int trc = fan_read_hottest(&mc);

    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    g_fan.tach_ok = tach_ok;
    g_fan.temp_rc = trc;
    uint32_t start = tegra_fan_ctl_init(&g_fan.ctl, trc, mc);
#if defined(ENABLE_BOOT_TESTS)
    /* No control task in test kernels: a fixed level, unless the
     * safety rules force 255. */
    start = tegra_fan_ctl_auto(&g_fan.ctl, true);
#endif
    fan_program_locked(start);
    g_fan.ready = true;
    spin_unlock_irqrestore(&g_fan_lock, f);

    if (trc != 0) {
        WARN("fan: temperature unreadable at boot (BPMP rc=%d) - fan at full speed", trc);
    }
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
    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    bool read = tegra_fan_ctl_should_read(&g_fan.ctl);
    spin_unlock_irqrestore(&g_fan_lock, f);
    if (!read) {
        return;
    }
    int32_t mc = 0;
    int rc = fan_read_hottest(&mc);

    f = spin_lock_irqsave(&g_fan_lock);
    g_fan.temp_rc = rc;
    bool was_lost = tegra_fan_ctl_temp_lost(&g_fan.ctl);
    uint32_t before = g_fan.ctl.pwm;
    fan_apply_locked(before, tegra_fan_ctl_step(&g_fan.ctl, rc, mc));
    bool lost = tegra_fan_ctl_temp_lost(&g_fan.ctl);
    spin_unlock_irqrestore(&g_fan_lock, f);

    /* Only on the transitions, so a dead BPMP prints once. */
    if (lost && !was_lost) {
        WARN("fan: temperature unreadable (BPMP rc=%d) - fan at full speed", rc);
    } else if (was_lost && !lost) {
        INFO("fan: temperature readable again - back on the curve");
    }
}

/* The CPU for the control task: the highest one that came online, off
 * CPU 0 (net_pump and the shells run there, and a wedged CPU 0 must not
 * stop the fan, #1093). cpu_count is the configured count, so a core
 * that failed to boot would hold the task on a run queue nobody
 * serves. CPU_AFFINITY_ANY if no secondary is up. */
static uint32_t fan_task_cpu(void)
{
    for (uint32_t c = cpu_count; c > 1u; c--) {
        smp_mb();
        if (cpu_data[c - 1u].online) {
            return c - 1u;
        }
    }
    return CPU_AFFINITY_ANY;
}

static void fan_task(void *arg)
{
    (void)arg;
    for (;;) {
        tegra_fan_poll();
        /* The only periodic task on Jetson: also catch a memory clock
         * that fell below its floor (#1161). */
        emc_hold_runtime_check();
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
    t->cpu_affinity = fan_task_cpu();
    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    g_fan.task_started = true;
    spin_unlock_irqrestore(&g_fan_lock, f);
    scheduler_add_task(t);
    return 0;
}

int tegra_fan_set_manual(uint32_t pwm)
{
    if (!g_fan.ready || pwm > TEGRA_FAN_MAX_PWM) {
        return -1;
    }
    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    uint32_t before = g_fan.ctl.pwm;
    fan_apply_locked(before, tegra_fan_ctl_manual(&g_fan.ctl, pwm));
    spin_unlock_irqrestore(&g_fan_lock, f);
    return 0;
}

int tegra_fan_set_auto(void)
{
    if (!g_fan.ready) {
        return -1;
    }
    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    uint32_t before = g_fan.ctl.pwm;
    fan_apply_locked(before, tegra_fan_ctl_auto(&g_fan.ctl, !g_fan.task_started));
    spin_unlock_irqrestore(&g_fan_lock, f);
    return 0;
}

void tegra_fan_get_status(struct tegra_fan_status *out)
{
    if (out == NULL) {
        return;
    }
    irq_flags_t f = spin_lock_irqsave(&g_fan_lock);
    out->ready = g_fan.ready;
    out->manual = g_fan.ctl.manual;
    out->tach_ok = g_fan.tach_ok;
    out->pwm = g_fan.ctl.pwm;
    out->target = g_fan.ctl.target;
    out->temp_mc = g_fan.ctl.temp_mc;
    out->temp_rc = g_fan.temp_rc;
    out->pwm_clk_hz = g_fan.pwm_clk_hz;
    out->csr = g_fan.ready ? *pwm_csr() : 0u;
    uint32_t tach = g_fan.tach_ok ? *tach0_reg() : 0u;
    if (tach & TEGRA_TACH_OVERFLOW) {
        /* Write 1 to clear the stall flag, keeping the window field
         * (pwm-tegra-tachometer.c writes the value back the same way). */
        *tach0_reg() = tach;
    }
    spin_unlock_irqrestore(&g_fan_lock, f);
    out->rpm = tegra_tach_rpm(tach);
}

#else /* !PLATFORM_JETSON_ORIN_NANO */

int  tegra_fan_init(void)              { return -1; }
int  tegra_fan_start(void)             { return -1; }
void tegra_fan_poll(void)              { }
int  tegra_fan_set_manual(uint32_t p)  { (void)p; return -1; }
int  tegra_fan_set_auto(void)          { return -1; }
void tegra_fan_get_status(struct tegra_fan_status *out)
{
    if (out != NULL) {
        *out = (struct tegra_fan_status){ 0 };
    }
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
