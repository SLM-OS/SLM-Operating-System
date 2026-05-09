/*
 * timer.c - ARM Generic Timer driver for SLM-OS
 *
 * Uses the EL1 physical timer (CNTP) for preemptive scheduling.
 */

#include "timer.h"
#include "gic.h"
#include "sched.h"
#include "task.h"       /* task_sleep_ms, task_wake_sleepers (#319) */
#include "uart.h"
#include "debug.h"
#include "platform.h"

/*
 * Timer selection.
 *
 * All ARM64 platforms now drive the timer through the CNTP_*_EL0
 * register names with the CNTPCT_EL0 counter:
 *
 *   - QEMU + Jetson + x86: CNTP_*_EL0 / CNTPCT_EL0 are the
 *     non-secure physical timer registers (CNTP, PPI 30).
 *     CNTHCTL_EL2.EL1PCEN is set in boot.S so EL1 can reach them.
 *   - Pi 5 (#683): SLM-OS boots at EL2 with VHE (HCR_EL2.E2H=1).
 *     Under E2H=1, accesses to the CNTP_*_EL0 register names are
 *     silently redirected by hardware to CNTHP_*_EL2 — the Hyp
 *     Physical Timer's control / cval / tval registers — and the
 *     timer interrupt is delivered on PPI 26 instead of PPI 30.
 *     This is the same path Linux takes via `arch_timer_select_ppi()`
 *     when `is_kernel_in_hyp_mode()` is true. CNTPCT_EL0 itself is
 *     not redirected (the same physical counter feeds CNTP and
 *     CNTHP), so timer_get_count and coop_preempt_maybe_tick keep
 *     working unchanged.
 *
 * The Pi 5 PPI 30 → NS-EL1 routing was broken at the firmware level
 * (#672 / #134); switching to PPI 26 (which TF-A / firmware route to
 * the EL2 vector via VBAR_EL2) is the production fix that #683 is
 * delivering. The platform-specific PPI number is TIMER_IRQ in
 * platform.h.
 */
#define ACTUAL_TIMER_IRQ    TIMER_IRQ

/* Timer interval (computed at init) */
static uint64_t timer_interval;

static inline uint64_t read_cntfrq(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

static inline uint64_t read_cntpct(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

/* Pi 5 boots at EL2 with HCR_EL2.{E2H,TGE} = {1,1} (boot.S:560
 * sets `(1<<34)|(1<<31)|(1<<27)` = E2H|RW|TGE). Per ARM ARM
 * `CNTP_CTL_EL0` / `CNTP_TVAL_EL0` access rules (DDI 0487, register
 * "Configurations" tables — RES0 from EL2 when the EL2&0 translation
 * regime is in use), writes from EL2 in that mode are silently
 * ignored. The VHE `_EL1`→`_EL2` register-name redirect does not
 * cover the `_EL0` names, so writing `CNTP_CTL_EL0` here would be a
 * no-op and the timer would never fire. The Hyp Physical Timer's
 * `CNTHP_*_EL2` registers ARE what the firmware wires to PPI 26,
 * and they have the same bit layout as the `CNTP_*_EL0` versions,
 * so on Pi 5 we drive them directly while QEMU + x86 keep using the
 * `_EL0` names from EL1 where the access is well-defined.
 *
 * Jetson runs at NS-EL2/VHE post-kexec exactly like Pi 5 — the same
 * RES0 rule applies. Under JETSON_HW_TICK=ON we take the Pi 5 path
 * (cnthp_*_el2 + PPI 26); without that flag the kernel-side code is
 * inert (stock NVIDIA BL31 leaves PPIs Group 0 → trapped to EL3, so
 * no timer IRQ delivers regardless), but we still need to avoid the
 * RES0 write — without JETSON_HW_TICK we don't program the timer at
 * all on Jetson, leaving COOP_PREEMPT to drive ticks at yield points.
 */
#if defined(PLATFORM_RASPI5) || \
    (defined(PLATFORM_JETSON_ORIN_NANO) && defined(JETSON_HW_TICK))
#define SLMOS_TIMER_USES_CNTHP_EL2 1
#endif

static inline void write_timer_ctl(uint64_t val)
{
#if defined(SLMOS_TIMER_USES_CNTHP_EL2)
    __asm__ volatile("msr cnthp_ctl_el2, %0" :: "r"(val));
#else
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(val));
#endif
}

static inline void write_timer_tval(int64_t val)
{
#if defined(SLMOS_TIMER_USES_CNTHP_EL2)
    __asm__ volatile("msr cnthp_tval_el2, %0" :: "r"(val));
#else
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(val));
#endif
}

/* Timer control bits (CNTP_CTL_EL0 — also the layout of CNTHP_CTL_EL2
 * that VHE redirects accesses to under HCR_EL2.E2H=1) */
#define CNTP_CTL_ENABLE     (1 << 0)
#define CNTP_CTL_IMASK      (1 << 1)
#define CNTP_CTL_ISTATUS    (1 << 2)

/*
 * Initialize the timer.
 */
void timer_init(void)
{
    /* Get timer frequency */
    uint64_t freq = read_cntfrq();

    /* Calculate interval for desired tick rate */
    timer_interval = freq / TIMER_HZ;

    INFO("Timer initializing");
    DEBUG_PRINT("  Frequency: %lu Hz", freq);
    DEBUG_PRINT("  Interval: %lu ticks (%d Hz)", timer_interval, TIMER_HZ);

    /* Disable timer while configuring */
    write_timer_ctl(0);

    /* Configure GIC for timer interrupt */
    gic_set_priority(ACTUAL_TIMER_IRQ, GIC_PRIORITY_DEFAULT);
    gic_enable_irq(ACTUAL_TIMER_IRQ);

    INFO("Timer initialized (IRQ %d, CNTP_*_EL0, not started)",
         ACTUAL_TIMER_IRQ);
}

/*
 * Start the timer.
 */
void timer_start(void)
{
    /* Set initial timer value */
    write_timer_tval(timer_interval);

    /* Enable timer, unmask interrupt */
    write_timer_ctl(CNTP_CTL_ENABLE);

    /* INFO print removed — called from per-CPU scheduler_start where
     * secondary CPUs cannot safely use uart_lock (L2 incoherency).
     * CPU 0 prints "Timer started" from scheduler_start directly. */
}

/*
 * Stop the timer.
 */
void timer_stop(void)
{
    /* Disable timer */
    write_timer_ctl(0);

    INFO("Timer stopped");
}


/*
 * Timer interrupt handler.
 */
volatile uint32_t timer_handler_count;  /* Diagnostic: total handler calls */
volatile uint64_t pit_ticks;            /* 100 Hz tick counter (cross-platform) */

void timer_handler(void)
{
    timer_handler_count++;
    pit_ticks++;

    /* Reload timer for next tick.
     * Read frequency from system register instead of cacheable timer_interval
     * because secondary CPUs' L2 may have stale data (0) for the variable,
     * causing an infinite IRQ storm (tval=0 → immediate re-fire). */
    write_timer_tval(read_cntfrq() / TIMER_HZ);

    /* Call scheduler tick handler */
    scheduler_tick();
}

/*
 * Get current counter value.
 */
uint64_t timer_get_count(void)
{
    return read_cntpct();
}

/*
 * Get timer frequency.
 */
uint64_t timer_get_frequency(void)
{
    return read_cntfrq();
}

/*
 * Per-CPU timer initialization.
 * Called by each secondary CPU after boot.
 * Enables the timer interrupt for this CPU but does not start the timer.
 */
void timer_percpu_init(void)
{
    /* Disable timer (will be started when scheduler runs on this core) */
    write_timer_ctl(0);

    /*
     * Enable timer interrupt in GIC for this CPU.
     * The Generic Timer PPIs (26/27/30) are all per-CPU, so each
     * core must enable its own copy of the IRQ in the GIC distributor /
     * redistributor.
     */
    gic_set_priority(ACTUAL_TIMER_IRQ, GIC_PRIORITY_DEFAULT);
    gic_enable_irq(ACTUAL_TIMER_IRQ);
}

/*
 * Sleep the current task for the given number of milliseconds.
 *
 * Delegates to task_sleep_ms (#319) — the scheduler-blocking primitive
 * in kernel/sched/task_sleep.c. The old busy-wait implementation (a
 * yield loop on CNTPCT_EL0) left the caller on the run queue the
 * entire time, stealing scheduling slots from genuinely-ready work
 * and preventing the idle task from wfi'ing to save power.
 */
void sleep_ms(uint32_t ms)
{
    task_sleep_ms(ms);
}

/*
 * Sleep the current task for the given number of microseconds.
 */
void sleep_us(uint64_t us)
{
    if (us == 0) {
        return;
    }

    uint64_t freq = read_cntfrq();
    uint64_t target = read_cntpct() + (freq / 1000000) * us;

    while (read_cntpct() < target) {
        yield();
    }
}

/*
 * Wake sleeping tasks — delegates to the scheduler's sleep queue.
 *
 * Historically this was a stub because sleep_ms busy-waited. Now
 * sleep_ms goes through task_sleep_ms (#319) which enqueues on a
 * real sleep queue; this function is kept for API compatibility
 * and forwards to task_wake_sleepers so any caller expecting the
 * old symbol still works.
 */
void timer_wake_sleepers(void)
{
    task_wake_sleepers();
}
