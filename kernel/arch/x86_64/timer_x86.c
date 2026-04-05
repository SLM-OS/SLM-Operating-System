/*
 * timer_x86.c - 8254 PIT timer implementing timer.h for x86-64
 *
 * Configures the Programmable Interval Timer (channel 0) as a rate
 * generator at TIMER_HZ (100 Hz). IRQ 0 fires on vector 32.
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include "timer.h"
#include "config.h"
#include "gic.h"
#include "sched.h"

/* I/O port access */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* PIT I/O ports */
#define PIT_CH0     0x40
#define PIT_CMD     0x43

/* PIT base frequency */
#define PIT_FREQ    1193182UL

/* Tick counter */
volatile uint64_t pit_ticks;

/* IRQ handler callback registered with idt.c */
extern void irq_register(uint8_t irq, void (*handler)(uint8_t));

static void pit_irq_handler(uint8_t irq)
{
    (void)irq;
    pit_ticks++;
    scheduler_tick();
}

void timer_init(void)
{
    uint16_t divisor = PIT_FREQ / TIMER_HZ;

    /* Channel 0, lo/hi byte, rate generator (mode 2) */
    outb(PIT_CMD, 0x34);
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, (divisor >> 8) & 0xFF);

    /* Register IRQ handler (IRQ 0 = PIC offset 0) */
    irq_register(0, pit_irq_handler);
}

void timer_start(void)
{
    gic_enable_irq(TIMER_IRQ);
}

void timer_stop(void)
{
    gic_disable_irq(TIMER_IRQ);
}

void timer_handler(void)
{
    pit_irq_handler(0);
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
    /* No per-CPU init for PIT (single timer, single core) */
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

    /* PIT resolution is 10ms; short sleeps wait at least one tick */
    uint64_t ms = (us + 999) / 1000;
    sleep_ms((uint32_t)ms);
}

void timer_wake_sleepers(void)
{
    /* No-op: busy-wait sleep doesn't use a sleep queue */
}

#endif /* PLATFORM_X86_64 */
