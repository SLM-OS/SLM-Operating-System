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

#endif /* TIMER_H */
