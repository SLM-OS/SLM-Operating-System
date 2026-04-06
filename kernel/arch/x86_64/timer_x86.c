/*
 * timer_x86.c - LAPIC timer implementing timer.h for x86-64
 *
 * Uses the Local APIC timer in periodic mode, calibrated against the
 * PIT (8254) for frequency measurement. Each CPU has its own LAPIC
 * timer, enabling per-CPU tick interrupts for SMP.
 *
 * Falls back to PIT if LAPIC calibration fails.
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include "timer.h"
#include "config.h"
#include "gic.h"
#include "sched.h"
#include "uart.h"

/* LAPIC timer interface (lapic.c) */
extern uint32_t lapic_timer_calibrate(void);
extern void lapic_timer_setup(uint8_t vector, uint32_t initial_count, uint8_t divide);
extern void lapic_timer_stop(void);
extern void lapic_eoi(void);

/* IDT handler registration (idt.c) */
extern void irq_register(uint8_t irq, void (*handler)(uint8_t));

/* LAPIC timer vector — must not conflict with IOAPIC vectors (32-47) */
#define LAPIC_TIMER_VECTOR  48

/* Tick counter (global, incremented on BSP only for uptime tracking) */
volatile uint64_t pit_ticks;

/* Calibrated LAPIC timer values */
static uint32_t lapic_ticks_per_sec;
static uint32_t lapic_initial_count;

/* Timer ISR — called from IDT for LAPIC timer vector */
static void lapic_timer_irq(uint8_t irq)
{
    (void)irq;
    pit_ticks++;
    scheduler_tick();
}

void timer_init(void)
{
    /* Calibrate LAPIC timer against PIT */
    lapic_ticks_per_sec = lapic_timer_calibrate();

    if (lapic_ticks_per_sec == 0) {
        uart_printf("[TIMER] LAPIC calibration failed, using fallback\n");
        lapic_ticks_per_sec = 100000000;  /* 100 MHz guess */
    }

    /* Calculate initial count for TIMER_HZ with divide-by-16 */
    lapic_initial_count = (lapic_ticks_per_sec / 16) / TIMER_HZ;

    uart_printf("[TIMER] LAPIC timer: %u ticks/sec, initial_count=%u (div16, %u Hz)\n",
                lapic_ticks_per_sec, lapic_initial_count, TIMER_HZ);

    /* Register timer IRQ handler (index in irq_handlers = vector - 32) */
    irq_register(LAPIC_TIMER_VECTOR - 32, lapic_timer_irq);

    /* Disable PIT (no longer needed after calibration) */
    /* PIT channel 0 stops when we don't reload it — IOAPIC pin stays masked */
}

void timer_start(void)
{
    /* Start LAPIC timer in periodic mode */
    lapic_timer_setup(LAPIC_TIMER_VECTOR, lapic_initial_count, 0x03);  /* divide by 16 */
}

void timer_stop(void)
{
    lapic_timer_stop();
}

void timer_handler(void)
{
    lapic_timer_irq(0);
}

uint64_t timer_get_count(void)
{
    return pit_ticks;
}

uint64_t timer_get_frequency(void)
{
    return TIMER_HZ;
}

void timer_percpu_init(void)
{
    /* Each secondary CPU starts its own LAPIC timer */
    lapic_timer_setup(LAPIC_TIMER_VECTOR, lapic_initial_count, 0x03);
}

void sleep_ms(uint32_t ms)
{
    if (ms == 0) return;

    uint64_t target = pit_ticks + ((uint64_t)ms * TIMER_HZ + 999) / 1000;
    while (pit_ticks < target)
        yield();
}

void sleep_us(uint64_t us)
{
    if (us == 0) return;

    uint64_t ms = (us + 999) / 1000;
    sleep_ms((uint32_t)ms);
}

void timer_wake_sleepers(void)
{
    /* No-op: busy-wait sleep doesn't use a sleep queue */
}

#endif /* PLATFORM_X86_64 */
