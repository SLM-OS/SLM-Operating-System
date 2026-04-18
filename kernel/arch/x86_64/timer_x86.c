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
#include "smp.h"
#include "uart.h"
#include "cpuid.h"

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

/* Bounded busy-wait budget for the PIT calibration fallback path.
 * Mirrors TSC_PIT_TIMEOUT_CYCLES in lapic.c — both paths poll the
 * same PIT-channel-2 OUT line and need the same 200 ms ceiling so a
 * kexec-disabled PIT can't hang boot. Keep these in sync if either
 * is changed. */
#define TSC_PIT_TIMEOUT_CYCLES   600000000ULL

/*
 * Read the Time Stamp Counter (RDTSC) — cycle-accurate, ~GHz resolution.
 */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* TSC frequency (cycles per second), measured during timer_init */
static uint64_t tsc_freq;

/* Calibrated LAPIC timer values */
static uint32_t lapic_ticks_per_sec;
static uint32_t lapic_initial_count;

/* Timer ISR — called from IDT for LAPIC timer vector */
static void lapic_timer_irq(uint8_t irq)
{
    (void)irq;
    /* Only BSP increments the global tick counter (avoid 8× rate with SMP) */
    if (cpu_id() == 0)
        pit_ticks++;
    scheduler_tick();
}

/* Read TSC frequency from CPUID leaves 0x15 and 0x16.
 *
 * Leaf 0x15 (Skylake+): TSC/crystal ratio + core crystal in Hz.
 *   TSC freq = crystal * EBX / EAX (all returned in that leaf)
 *
 * Some CPUs report the ratio but not the crystal frequency; in that
 * case, fall through to leaf 0x16 which gives the processor base
 * frequency in MHz directly (close enough for TSC under
 * constant_tsc, which all Intel chips from Nehalem onward
 * guarantee).
 *
 * Returns 0 if neither leaf yields a usable number — caller then
 * falls back to PIT calibration. */
static uint64_t tsc_freq_from_cpuid(void)
{
    uint32_t eax, ebx, ecx, edx;
    x86_cpuid(0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;

    if (max_leaf >= 0x15) {
        x86_cpuid(0x15, &eax, &ebx, &ecx, &edx);
        if (ebx != 0 && ecx != 0 && eax != 0) {
            return ((uint64_t)ecx * (uint64_t)ebx) / (uint64_t)eax;
        }
    }

    if (max_leaf >= 0x16) {
        x86_cpuid(0x16, &eax, &ebx, &ecx, &edx);
        /* EAX = processor base frequency in MHz */
        if (eax != 0)
            return (uint64_t)eax * 1000000ULL;
    }

    return 0;
}

void timer_init(void)
{
    /* Calibrate LAPIC timer (CPUID first, PIT fallback). */
    lapic_ticks_per_sec = lapic_timer_calibrate();

    /* TSC frequency — prefer CPUID (deterministic, works across
     * kexec handoffs); fall back to PIT with a bounded timeout
     * only when CPUID reports nothing. */
    tsc_freq = tsc_freq_from_cpuid();
    if (tsc_freq != 0) {
        uart_printf("[TIMER] TSC frequency from CPUID: %lu Hz (%lu MHz)\n",
                    (unsigned long)tsc_freq,
                    (unsigned long)(tsc_freq / 1000000));
    } else {
        extern void outb(uint16_t port, uint8_t val);
        extern uint8_t inb(uint16_t port);
        uint16_t pit_count = 11932;  /* ~10 ms */
        outb(0x61, (inb(0x61) & 0xFD) | 0x01);
        outb(0x43, 0xB0);
        outb(0x42, pit_count & 0xFF);
        outb(0x42, (pit_count >> 8) & 0xFF);
        uint8_t tmp = inb(0x61) & 0xFE;
        outb(0x61, tmp);
        outb(0x61, tmp | 0x01);
        uint64_t tsc_start = rdtsc();
        /* Bounded wait — bail after ~200 ms of TSC time if PIT
         * ch2 never fires (kexec-inherited disabled PIT). */
        int pit_ok = 0;
        while (1) {
            if (inb(0x61) & 0x20) { pit_ok = 1; break; }
            if (rdtsc() - tsc_start > TSC_PIT_TIMEOUT_CYCLES) break;
        }
        if (pit_ok) {
            uint64_t tsc_elapsed = rdtsc() - tsc_start;
            tsc_freq = tsc_elapsed * 100;  /* 10ms × 100 = 1 sec */
            uart_printf("[TIMER] TSC calibration (PIT): %lu cycles/10ms, %lu MHz\n",
                        (unsigned long)tsc_elapsed,
                        (unsigned long)(tsc_freq / 1000000));
        } else {
            uart_printf("[TIMER] TSC calibration skipped — no PIT and no "
                        "CPUID frequency. Timing will use pit_ticks/TIMER_HZ.\n");
            /* Leaving tsc_freq = 0 forces timer_get_count() to use
             * pit_ticks (from the LAPIC timer ISR once it's live).
             * sleep_ms on secondary CPUs will have BSP-IRQ jitter
             * but the boot will complete. */
        }
    }

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
    if (tsc_freq > 0)
        return rdtsc();
    return pit_ticks;
}

uint64_t timer_get_frequency(void)
{
    if (tsc_freq > 0)
        return tsc_freq;
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

    /* Drive sleep off the TSC (per-CPU, always advancing) rather
     * than pit_ticks (only incremented on BSP). On an AP-pinned
     * task, pit_ticks advances only via cache-coherent propagation
     * from BSP's ISR store, which adds up to 10 ms of jitter on
     * every sleep. TSC is read locally with rdtsc and is uniform
     * across CPUs on any Intel chip from Nehalem onward
     * (constant_tsc). Fallback: if tsc_freq is zero (calibration
     * failed), timer_get_count() returns pit_ticks and
     * timer_get_frequency() returns TIMER_HZ, so this code still
     * works on BSP. */
    uint64_t freq = timer_get_frequency();
    uint64_t target = timer_get_count() + ((uint64_t)ms * freq + 999) / 1000;
    while (timer_get_count() < target)
        yield();
}

void sleep_us(uint64_t us)
{
    if (us == 0) return;

    uint64_t freq = timer_get_frequency();
    uint64_t target = timer_get_count() + (us * freq + 999999) / 1000000;
    while (timer_get_count() < target)
        yield();
}

void timer_wake_sleepers(void)
{
    /* No-op: busy-wait sleep doesn't use a sleep queue */
}

#endif /* PLATFORM_X86_64 */
