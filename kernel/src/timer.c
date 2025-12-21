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

/* Timer IRQ number (from platform.h / GIC) */
#define TIMER_IRQ   GIC_INT_PHYS_TIMER  /* IRQ 30 - physical timer */

/* Timer interval (computed at init) */
static uint64_t timer_interval;

/*
 * Read timer frequency from CNTFRQ_EL0
 */
static inline uint64_t read_cntfrq(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

/*
 * Read current counter value from CNTPCT_EL0
 */
static inline uint64_t read_cntpct(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

/*
 * Read timer control register CNTP_CTL_EL0
 */
static inline uint64_t read_cntp_ctl(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(val));
    return val;
}

/*
 * Write timer control register CNTP_CTL_EL0
 */
static inline void write_cntp_ctl(uint64_t val)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(val));
}

/*
 * Write timer compare value CNTP_CVAL_EL0
 */
static inline void write_cntp_cval(uint64_t val)
{
    __asm__ volatile("msr cntp_cval_el0, %0" :: "r"(val));
}

/*
 * Write timer value CNTP_TVAL_EL0
 */
static inline void write_cntp_tval(int64_t val)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(val));
}

/*
 * CNTP_CTL_EL0 bits
 */
#define CNTP_CTL_ENABLE     (1 << 0)    /* Timer enable */
#define CNTP_CTL_IMASK      (1 << 1)    /* Interrupt mask (1 = masked) */
#define CNTP_CTL_ISTATUS    (1 << 2)    /* Interrupt status */

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
    gic_set_priority(TIMER_IRQ, GIC_PRIORITY_DEFAULT);
    gic_enable_irq(TIMER_IRQ);

    INFO("Timer initialized (not started)");
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
