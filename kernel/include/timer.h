/*
 * timer.h - ARM Generic Timer for SLM-OS
 *
 * Uses the ARM architectural timer for preemptive scheduling.
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/*
 * Timer configuration
 */
#define TIMER_HZ    100     /* 100 Hz = 10ms tick */

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

#endif /* TIMER_H */
