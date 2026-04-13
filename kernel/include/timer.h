/*
 * timer.h - ARM Generic Timer for SLM-OS
 *
 * Uses the ARM architectural timer for preemptive scheduling.
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "config.h"

/*
 * Tick counters — updated by the timer ISR on platforms with working
 * timer IRQ delivery, or synthesized from CNTPCT_EL0 under
 * PI5_COOP_PREEMPT. Exposed so the scheduler and diag code can poke
 * them without re-declaring extern at function scope.
 */
extern volatile uint32_t timer_handler_count;
extern volatile uint64_t pit_ticks;

/*
 * Initialize the timer.
 *
 * Configures the ARM generic timer for periodic interrupts.
 * Does NOT start the timer - call timer_start() for that.
 */
void timer_init(void);

/*
 * Start the timer.
 *
 * Begins generating periodic interrupts at TIMER_HZ.
 */
void timer_start(void);

/*
 * Stop the timer.
 *
 * Disables timer interrupts.
 */
void timer_stop(void);

/*
 * Timer interrupt handler.
 *
 * Called from the IRQ vector when timer fires.
 * Reloads the timer and calls scheduler_tick().
 */
void timer_handler(void);

/*
 * Get current timer counter value.
 */
uint64_t timer_get_count(void);

/*
 * Get timer frequency in Hz.
 */
uint64_t timer_get_frequency(void);

/*
 * Per-CPU timer initialization.
 * Called by each secondary CPU after boot.
 * Enables the timer interrupt for this CPU but does not start the timer.
 */
void timer_percpu_init(void);

/*
 * Sleep the current task for a given number of milliseconds.
 *
 * Blocks the calling task and yields to the scheduler. The task is
 * automatically woken by the timer tick handler when the sleep time
 * has elapsed. Minimum effective sleep is one timer tick (~10ms).
 *
 * Must not be called from interrupt context.
 *
 * @ms: Sleep duration in milliseconds (0 returns immediately)
 */
void sleep_ms(uint32_t ms);

/*
 * Sleep the current task for a given number of microseconds.
 *
 * For durations under ~10ms, precision is limited by the timer tick
 * rate (TIMER_HZ). For very short sleeps (< 1 tick), the task will
 * wake on the next timer tick.
 *
 * Must not be called from interrupt context.
 *
 * @us: Sleep duration in microseconds (0 returns immediately)
 */
void sleep_us(uint64_t us);

/*
 * Check sleeping tasks and wake any whose deadline has passed.
 *
 * Called from scheduler_tick() on every timer interrupt.
 * Moves expired tasks from the sleep queue back to the run queue.
 */
void timer_wake_sleepers(void);

#endif /* TIMER_H */
