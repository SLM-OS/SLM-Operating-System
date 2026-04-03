/*
 * timer.c - ARM Generic Timer driver for SLM-OS
 *
 * Uses the EL1 physical timer (CNTP) for preemptive scheduling.
 */

#include "timer.h"
#include "gic.h"
#include "sched.h"
#include "uart.h"
#include "debug.h"
#include "platform.h"

/*
 * Timer selection: Pi 5 uses virtual timer (CNTV) because the physical
 * timer IRQ is not being delivered despite correct GIC configuration.
 * Other platforms use physical timer (CNTP).
 */
#if defined(PLATFORM_RASPI5)
#define USE_VIRTUAL_TIMER   1
#define ACTUAL_TIMER_IRQ    27  /* Virtual timer PPI 11 = IRQ 27 */
#else
#define USE_VIRTUAL_TIMER   0
#define ACTUAL_TIMER_IRQ    TIMER_IRQ  /* Physical timer IRQ 30 */
#endif

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

#if USE_VIRTUAL_TIMER
static inline uint64_t read_cntp_ctl(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntv_ctl_el0" : "=r"(val));
    return val;
}

static inline void write_cntp_ctl(uint64_t val)
{
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(val));
}

static inline void write_cntp_tval(int64_t val)
{
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(val));
}
#else
static inline uint64_t read_cntp_ctl(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(val));
    return val;
}

static inline void write_cntp_ctl(uint64_t val)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(val));
}

static inline void write_cntp_cval(uint64_t val)
{
    __asm__ volatile("msr cntp_cval_el0, %0" :: "r"(val));
}

static inline void write_cntp_tval(int64_t val)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(val));
}
#endif

/* Timer control bits (same for CNTP and CNTV) */
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
    write_cntp_ctl(0);

    /* Configure GIC for timer interrupt */
    gic_set_priority(ACTUAL_TIMER_IRQ, GIC_PRIORITY_DEFAULT);
    gic_enable_irq(ACTUAL_TIMER_IRQ);

    INFO("Timer initialized (IRQ %d, %s, not started)",
         ACTUAL_TIMER_IRQ, USE_VIRTUAL_TIMER ? "virtual" : "physical");
}

/*
 * Start the timer.
 */
void timer_start(void)
{
    /* Set initial timer value */
    write_cntp_tval(timer_interval);

    /* Enable timer, unmask interrupt */
    write_cntp_ctl(CNTP_CTL_ENABLE);

    INFO("Timer started (%d Hz)", TIMER_HZ);
}

/*
 * Stop the timer.
 */
void timer_stop(void)
{
    /* Disable timer */
    write_cntp_ctl(0);

    INFO("Timer stopped");
}

/*
 * Timer interrupt handler.
 */
void timer_handler(void)
{
    /* Reload timer for next tick */
    write_cntp_tval(timer_interval);

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
    write_cntp_ctl(0);

    /*
     * Enable timer interrupt in GIC for this CPU.
     * PPI 30 (physical timer) is per-CPU, so each core must enable it.
     */
    gic_set_priority(ACTUAL_TIMER_IRQ, GIC_PRIORITY_DEFAULT);
    gic_enable_irq(ACTUAL_TIMER_IRQ);
}
