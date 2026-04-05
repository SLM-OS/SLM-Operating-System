/*
 * pic.c - 8259 PIC driver implementing gic.h interface for x86-64
 *
 * Maps the gic_* function names to 8259 PIC operations.
 * IRQ numbering: vectors 32-47 map to PIC IRQ 0-15.
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

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* PIC I/O ports */
#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1

/* PIC vector offset (IRQ 0 maps to this vector) */
#define PIC_VECTOR_OFFSET   32

void gic_init(void)
{
    /* Save masks */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: begin init (cascade mode, ICW4 needed) */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, PIC_VECTOR_OFFSET);      io_wait();  /* IRQ 0-7 → 32-39 */
    outb(PIC2_DATA, PIC_VECTOR_OFFSET + 8);  io_wait();  /* IRQ 8-15 → 40-47 */

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04); io_wait();  /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();  /* slave identity */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Restore masks (mask all initially) */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void gic_enable_irq(uint32_t irq)
{
    if (irq < PIC_VECTOR_OFFSET || irq >= PIC_VECTOR_OFFSET + 16)
        return;
    uint8_t pic_irq = irq - PIC_VECTOR_OFFSET;

    if (pic_irq < 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << pic_irq));
    } else {
        outb(PIC2_DATA, inb(PIC2_DATA) & ~(1 << (pic_irq - 8)));
        /* Also unmask cascade (IRQ 2 on master) */
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << 2));
    }
}

void gic_disable_irq(uint32_t irq)
{
    if (irq < PIC_VECTOR_OFFSET || irq >= PIC_VECTOR_OFFSET + 16)
        return;
    uint8_t pic_irq = irq - PIC_VECTOR_OFFSET;

    if (pic_irq < 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) | (1 << pic_irq));
    } else {
        outb(PIC2_DATA, inb(PIC2_DATA) | (1 << (pic_irq - 8)));
    }
}

void gic_set_priority(uint32_t irq, uint8_t priority)
{
    (void)irq;
    (void)priority;
    /* 8259 PIC has no priority control */
}

uint32_t gic_acknowledge(void)
{
    /* On x86-64, the vector is identified by the IDT dispatch.
     * This function is not used in the x86-64 interrupt path. */
    return 0;
}

void gic_end_interrupt(uint32_t irq)
{
    if (irq < PIC_VECTOR_OFFSET)
        return;
    uint8_t pic_irq = irq - PIC_VECTOR_OFFSET;

    if (pic_irq >= 8)
        outb(PIC2_CMD, 0x20);  /* EOI to slave */
    outb(PIC1_CMD, 0x20);      /* EOI to master */
}

int gic_is_pending(uint32_t irq)
{
    (void)irq;
    return 0;
}

void gic_send_sgi(uint32_t irq, uint32_t target_cpu)
{
    (void)irq;
    (void)target_cpu;
}

void gic_percpu_init(void)
{
    /* No per-CPU init for 8259 PIC */
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
    return 1;  /* CPU 0 */
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
