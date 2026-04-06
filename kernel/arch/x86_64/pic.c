/*
 * pic.c - Interrupt controller implementing gic.h for x86-64
 *
 * Uses LAPIC + IOAPIC for interrupt routing. Disables the legacy 8259 PIC.
 * Maps the gic_* function names to LAPIC/IOAPIC operations.
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include "gic.h"

/* I/O port access */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* LAPIC / IOAPIC drivers */
extern void lapic_init(void);
extern void lapic_percpu_init(void);
extern void lapic_eoi(void);
extern void lapic_send_ipi(uint32_t apic_id, uint32_t vector, uint32_t flags);
extern void ioapic_init(void);
extern void ioapic_enable_irq(uint32_t irq);
extern void ioapic_disable_irq(uint32_t irq);

/* IDT must be loaded before any interrupts */
extern void idt_init(void);

/*
 * Disable the 8259 PIC by masking all IRQs.
 * Some chipsets may still route spurious interrupts through the PIC
 * even after IOAPIC is enabled, so we remap to high vectors to avoid
 * conflicts with CPU exceptions.
 */
static void pic_disable(void)
{
    /* Remap PIC to vectors 0xF0-0xFF (out of the way) */
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0xF0); io_wait();  /* Master → vectors 0xF0-0xF7 */
    outb(0xA1, 0xF8); io_wait();  /* Slave → vectors 0xF8-0xFF */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    /* Mask all PIC interrupts */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

/* ---- gic.h Interface ---- */

void gic_init(void)
{
    /* Load IDT (x86-64 interrupt vector table) */
    idt_init();

    /* Disable legacy 8259 PIC */
    pic_disable();

    /* Initialize LAPIC and IOAPIC */
    lapic_init();
    ioapic_init();
}

void gic_enable_irq(uint32_t irq)
{
    ioapic_enable_irq(irq);
}

void gic_disable_irq(uint32_t irq)
{
    ioapic_disable_irq(irq);
}

void gic_set_priority(uint32_t irq, uint8_t priority)
{
    (void)irq;
    (void)priority;
}

uint32_t gic_acknowledge(void)
{
    return 0;
}

void gic_end_interrupt(uint32_t irq)
{
    (void)irq;
    lapic_eoi();
}

int gic_is_pending(uint32_t irq)
{
    (void)irq;
    return 0;
}

void gic_send_sgi(uint32_t irq, uint32_t target_cpu)
{
    extern uint8_t acpi_get_cpu_apic_id(uint32_t logical_id);
    uint32_t apic_id = acpi_get_cpu_apic_id(target_cpu);
    lapic_send_ipi(apic_id, irq, 0);  /* Fixed delivery */
}

void gic_percpu_init(void)
{
    lapic_percpu_init();
}

int gic_set_affinity(uint32_t irq, uint32_t cpu_mask)
{
    (void)irq;
    (void)cpu_mask;
    return 0;
}

uint32_t gic_get_affinity(uint32_t irq)
{
    (void)irq;
    return 1;
}

void gic_exclude_cpu_from_spis(uint32_t cpu)
{
    (void)cpu;
}

void gic_include_cpu_in_spis(uint32_t cpu)
{
    (void)cpu;
}

#endif /* PLATFORM_X86_64 */
