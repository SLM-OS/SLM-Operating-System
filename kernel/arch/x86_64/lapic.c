/*
 * lapic.c - Local APIC driver for x86-64
 *
 * Memory-mapped interface to the per-CPU Local APIC.
 * Base address obtained from ACPI MADT (default 0xFEE00000).
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include "uart.h"

/* LAPIC register offsets */
#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080   /* Task Priority Register */
#define LAPIC_EOI           0x0B0   /* End of Interrupt */
#define LAPIC_LDR           0x0D0   /* Logical Destination */
#define LAPIC_DFR           0x0E0   /* Destination Format */
#define LAPIC_SVR           0x0F0   /* Spurious Vector Register */
#define LAPIC_ISR_BASE      0x100   /* In-Service Register (8 × 32-bit) */
#define LAPIC_ESR           0x280   /* Error Status Register */
#define LAPIC_ICR_LO        0x300   /* Interrupt Command Register low */
#define LAPIC_ICR_HI        0x310   /* Interrupt Command Register high */
#define LAPIC_LVT_TIMER     0x320   /* LVT Timer */
#define LAPIC_LVT_LINT0     0x350   /* LVT LINT0 */
#define LAPIC_LVT_LINT1     0x360   /* LVT LINT1 */
#define LAPIC_LVT_ERROR     0x370   /* LVT Error */
#define LAPIC_TIMER_INIT    0x380   /* Timer Initial Count */
#define LAPIC_TIMER_CURRENT 0x390   /* Timer Current Count */
#define LAPIC_TIMER_DIVIDE  0x3E0   /* Timer Divide Configuration */

/* SVR bits */
#define SVR_APIC_ENABLE     (1 << 8)

/* LVT Timer modes */
#define LVT_MASKED          (1 << 16)
#define LVT_TIMER_PERIODIC  (1 << 17)

/* ICR delivery modes */
#define ICR_FIXED            0x00000000
#define ICR_INIT             0x00000500
#define ICR_SIPI             0x00000600
#define ICR_LEVEL_ASSERT     0x00004000
#define ICR_LEVEL_DEASSERT   0x00000000
#define ICR_DEST_ALL_EX_SELF 0x000C0000

/* Spurious interrupt vector (must have bits 0-3 set on some older APICs) */
#define SPURIOUS_VECTOR      0xFF

static volatile uint32_t *lapic_base;

/* ---- Register Access ---- */

uint32_t lapic_read(uint32_t offset)
{
    return lapic_base[offset / 4];
}

void lapic_write(uint32_t offset, uint32_t value)
{
    lapic_base[offset / 4] = value;
}

/* ---- Public Interface ---- */

extern uint32_t acpi_get_lapic_address(void);

void lapic_init(void)
{
    uint32_t addr = acpi_get_lapic_address();
    lapic_base = (volatile uint32_t *)(uintptr_t)addr;

    /* Enable LAPIC via Spurious Vector Register */
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr |= SVR_APIC_ENABLE | SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);

    /* Set Task Priority to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    /* Set flat destination mode */
    lapic_write(LAPIC_DFR, 0xFFFFFFFF);
    lapic_write(LAPIC_LDR, (lapic_read(LAPIC_LDR) & 0x00FFFFFF) | (1 << 24));

    /* Mask all LVT entries initially */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);

    /* Clear any pending errors */
    lapic_write(LAPIC_ESR, 0);
    lapic_read(LAPIC_ESR);

    /* Send EOI to clear any stale interrupts */
    lapic_write(LAPIC_EOI, 0);

    uint32_t id = (lapic_read(LAPIC_ID) >> 24) & 0xFF;
    uint32_t ver = lapic_read(LAPIC_VERSION) & 0xFF;
    uart_printf("[LAPIC] Initialized at 0x%x (ID=%u, version=0x%x)\n",
                addr, id, ver);
}

void lapic_percpu_init(void)
{
    /* Same as lapic_init but for secondary CPUs */
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr |= SVR_APIC_ENABLE | SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);
    lapic_write(LAPIC_ESR, 0);
    lapic_read(LAPIC_ESR);
    lapic_write(LAPIC_EOI, 0);
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_get_id(void)
{
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector, uint32_t flags)
{
    /* Write destination APIC ID to ICR high */
    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    /* Write vector + flags to ICR low (this triggers the IPI) */
    lapic_write(LAPIC_ICR_LO, vector | flags);

    /* Wait for delivery (poll bit 12 = Send Pending) */
    while (lapic_read(LAPIC_ICR_LO) & (1 << 12))
        ;
}

/* ---- LAPIC Timer ---- */

void lapic_timer_setup(uint8_t vector, uint32_t initial_count, uint8_t divide)
{
    /* Set divide value */
    lapic_write(LAPIC_TIMER_DIVIDE, divide);

    /* Configure LVT Timer: periodic mode, unmasked, with specified vector */
    lapic_write(LAPIC_LVT_TIMER, LVT_TIMER_PERIODIC | vector);

    /* Set initial count (starts timer) */
    lapic_write(LAPIC_TIMER_INIT, initial_count);
}

void lapic_timer_stop(void)
{
    lapic_write(LAPIC_TIMER_INIT, 0);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
}

uint32_t lapic_timer_current(void)
{
    return lapic_read(LAPIC_TIMER_CURRENT);
}

/*
 * Calibrate LAPIC timer against PIT.
 * Returns: LAPIC timer ticks per second (approximate).
 */
uint32_t lapic_timer_calibrate(void)
{
    /* I/O port access for PIT calibration */
    extern void outb(uint16_t port, uint8_t val);
    extern uint8_t inb(uint16_t port);

    /* Set LAPIC timer to count down from max, divide by 16 */
    lapic_write(LAPIC_TIMER_DIVIDE, 0x03);  /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    /* Use PIT channel 2 to measure ~10ms */
    /* PIT frequency: 1193182 Hz. For 10ms: 11932 counts */
    uint16_t pit_count = 11932;

    /* Program PIT channel 2 (speaker timer) for one-shot countdown */
    outb(0x61, (inb(0x61) & 0xFD) | 0x01);  /* Enable gate, disable speaker */
    outb(0x43, 0xB0);  /* Channel 2, lobyte/hibyte, mode 0 (one-shot) */
    outb(0x42, pit_count & 0xFF);
    outb(0x42, (pit_count >> 8) & 0xFF);

    /* Reset PIT counter by toggling gate */
    uint8_t tmp = inb(0x61) & 0xFE;
    outb(0x61, tmp);
    outb(0x61, tmp | 0x01);

    /* Wait for PIT to count down (bit 5 of port 0x61 goes high) */
    while (!(inb(0x61) & 0x20))
        ;

    /* Read LAPIC timer count */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);  /* Stop timer */
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURRENT);

    /* elapsed = ticks in ~10ms with divide-by-16
     * Ticks per second = elapsed * 100 * 16 */
    uint32_t ticks_per_sec = elapsed * 100 * 16;

    uart_printf("[LAPIC] Timer calibration: %u ticks/10ms (div16), %u MHz\n",
                elapsed, ticks_per_sec / 1000000);

    return ticks_per_sec;
}

/* outb/inb for PIT calibration (duplicated to avoid header dependency) */
void __attribute__((weak)) outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

uint8_t __attribute__((weak)) inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#endif /* PLATFORM_X86_64 */
