/*
 * gic.c - GICv2 driver for SLM-OS
 *
 * Generic Interrupt Controller driver for QEMU virt machine.
 */

#include "gic.h"
#include "platform.h"
#include "uart.h"
#include "debug.h"
#include <stddef.h>

/*
 * GIC Distributor registers (GICD)
 */
#define GICD_BASE           GIC_DIST_BASE

#define GICD_CTLR           (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_TYPER          (*(volatile uint32_t *)(GICD_BASE + 0x004))
#define GICD_ISENABLER(n)   (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4 * (n)))
#define GICD_ICENABLER(n)   (*(volatile uint32_t *)(GICD_BASE + 0x180 + 4 * (n)))
#define GICD_ISPENDR(n)     (*(volatile uint32_t *)(GICD_BASE + 0x200 + 4 * (n)))
#define GICD_ICPENDR(n)     (*(volatile uint32_t *)(GICD_BASE + 0x280 + 4 * (n)))
#define GICD_IPRIORITYR(n)  (*(volatile uint32_t *)(GICD_BASE + 0x400 + 4 * (n)))
#define GICD_ITARGETSR(n)   (*(volatile uint32_t *)(GICD_BASE + 0x800 + 4 * (n)))
#define GICD_ICFGR(n)       (*(volatile uint32_t *)(GICD_BASE + 0xC00 + 4 * (n)))
#define GICD_SGIR           (*(volatile uint32_t *)(GICD_BASE + 0xF00))

/* GICD_CTLR bits */
#define GICD_CTLR_ENABLE    (1 << 0)

/*
 * GIC CPU Interface registers (GICC)
 */
#define GICC_BASE           GIC_CPU_BASE

#define GICC_CTLR           (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR            (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_BPR            (*(volatile uint32_t *)(GICC_BASE + 0x008))
#define GICC_IAR            (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR           (*(volatile uint32_t *)(GICC_BASE + 0x010))

/* GICC_CTLR bits */
#define GICC_CTLR_ENABLE    (1 << 0)

/* Special interrupt IDs */
#define GIC_SPURIOUS_INT    1023

/*
 * Initialize the GIC distributor.
 */
static void gic_dist_init(void)
{
    /* Disable distributor while configuring */
    GICD_CTLR = 0;

    /* Get number of interrupt lines */
    uint32_t typer = GICD_TYPER;
    uint32_t num_irqs = ((typer & 0x1F) + 1) * 32;

    DEBUG_PRINT("GIC: %u interrupt lines", num_irqs);

    /* Disable all interrupts */
    for (uint32_t i = 0; i < num_irqs / 32; i++) {
        GICD_ICENABLER(i) = 0xFFFFFFFF;
    }

    /* Clear all pending interrupts */
    for (uint32_t i = 0; i < num_irqs / 32; i++) {
        GICD_ICPENDR(i) = 0xFFFFFFFF;
    }

    /* Set all interrupts to default priority */
    for (uint32_t i = 0; i < num_irqs / 4; i++) {
        GICD_IPRIORITYR(i) = 0x80808080;
    }

    /* Route all SPIs to CPU 0 */
    for (uint32_t i = 8; i < num_irqs / 4; i++) {
        GICD_ITARGETSR(i) = 0x01010101;
    }

    /* Set all interrupts to level-triggered */
    for (uint32_t i = 2; i < num_irqs / 16; i++) {
        GICD_ICFGR(i) = 0;
    }

    /* Enable distributor */
    GICD_CTLR = GICD_CTLR_ENABLE;
}

/*
 * Initialize the GIC CPU interface.
 */
static void gic_cpu_init(void)
{
    /* Set priority mask to allow all priorities */
    GICC_PMR = 0xFF;

    /* No priority grouping (all bits for priority) */
    GICC_BPR = 0;

    /* Enable CPU interface */
    GICC_CTLR = GICC_CTLR_ENABLE;
}

/*
 * Initialize the GIC.
 */
void gic_init(void)
{
    INFO("Initializing GIC");
    DEBUG_PRINT("  Distributor: 0x%lx", (unsigned long)GICD_BASE);
    DEBUG_PRINT("  CPU interface: 0x%lx", (unsigned long)GICC_BASE);

    gic_dist_init();
    gic_cpu_init();

    INFO("GIC initialized");
}

/*
 * Enable a specific interrupt.
 */
void gic_enable_irq(uint32_t irq)
{
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;

    GICD_ISENABLER(reg) = (1 << bit);
}

/*
 * Disable a specific interrupt.
 */
void gic_disable_irq(uint32_t irq)
{
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;

    GICD_ICENABLER(reg) = (1 << bit);
}

/*
 * Set interrupt priority.
 */
void gic_set_priority(uint32_t irq, uint8_t priority)
{
    uint32_t reg = irq / 4;
    uint32_t offset = (irq % 4) * 8;

    uint32_t val = GICD_IPRIORITYR(reg);
    val &= ~(0xFF << offset);
    val |= (priority << offset);
    GICD_IPRIORITYR(reg) = val;
}

/*
 * Acknowledge an interrupt.
 */
uint32_t gic_acknowledge(void)
{
    return GICC_IAR & 0x3FF;
}

/*
 * Signal end of interrupt.
 */
void gic_end_interrupt(uint32_t irq)
{
    GICC_EOIR = irq;
}

/*
 * Check if an interrupt is pending.
 */
int gic_is_pending(uint32_t irq)
{
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;

    return (GICD_ISPENDR(reg) >> bit) & 1;
}

/*
 * Send a software-generated interrupt.
 */
void gic_send_sgi(uint32_t irq, uint32_t target_cpu)
{
    GICD_SGIR = (target_cpu << 16) | (irq & 0xF);
}
