/*
 * gic.h - Generic Interrupt Controller (GICv2) for SLM-OS
 *
 * Handles interrupt routing and management on ARM64.
 * QEMU virt machine uses GICv2.
 */

#ifndef GIC_H
#define GIC_H

#include <stdint.h>

/*
 * GIC interrupt types
 */
#define GIC_SPI_START       32      /* Shared Peripheral Interrupts start at 32 */
#define GIC_PPI_START       16      /* Private Peripheral Interrupts: 16-31 */
#define GIC_SGI_START       0       /* Software Generated Interrupts: 0-15 */

/*
 * Common interrupt numbers (PPI)
 */
#define GIC_INT_VIRT_TIMER  27      /* Virtual timer (PPI) */
#define GIC_INT_PHYS_TIMER  30      /* Physical timer (PPI) */

/*
 * Interrupt priority levels (lower = higher priority)
 */
#define GIC_PRIORITY_HIGH   0x00
#define GIC_PRIORITY_DEFAULT 0x80
#define GIC_PRIORITY_LOW    0xF0

/*
 * Initialize the GIC.
 *
 * Sets up distributor and CPU interface.
 * Must be called before enabling any interrupts.
 */
void gic_init(void);

/*
 * Enable a specific interrupt.
 *
 * @irq: Interrupt number (0-1019)
 */
void gic_enable_irq(uint32_t irq);

/*
 * Disable a specific interrupt.
 *
 * @irq: Interrupt number
 */
void gic_disable_irq(uint32_t irq);

/*
 * Set interrupt priority.
 *
 * @irq: Interrupt number
 * @priority: Priority (0 = highest, 0xFF = lowest)
 */
void gic_set_priority(uint32_t irq, uint8_t priority);

/*
 * Acknowledge an interrupt (read IAR).
 *
 * Returns the interrupt ID that fired.
 * Must be called at start of IRQ handler.
 */
uint32_t gic_acknowledge(void);

/*
 * Signal end of interrupt (write EOIR).
 *
 * @irq: Interrupt number from gic_acknowledge()
 * Must be called at end of IRQ handler.
 */
void gic_end_interrupt(uint32_t irq);

/*
 * Check if an interrupt is pending.
 *
 * @irq: Interrupt number
 * Returns: 1 if pending, 0 otherwise
 */
int gic_is_pending(uint32_t irq);

/*
 * Send a software-generated interrupt (SGI).
 *
 * @irq: SGI number (0-15)
 * @target_cpu: Target CPU mask
 */
void gic_send_sgi(uint32_t irq, uint32_t target_cpu);

/*
 * Per-CPU GIC initialization.
 * Called by each secondary CPU after boot.
 * Initializes the CPU interface only (distributor is shared).
 */
void gic_percpu_init(void);

#endif /* GIC_H */
