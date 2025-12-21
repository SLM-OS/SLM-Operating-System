/*
 * exceptions.c - Exception handlers for SLM-OS
 *
 * C handlers called from vectors.S
 */

#include "gic.h"
#include "timer.h"
#include "uart.h"
#include "debug.h"
#include <stdint.h>

/* Timer IRQ number */
#define TIMER_IRQ   30  /* Physical timer PPI */

/*
 * Trap frame structure (matches save_regs in vectors.S)
 */
struct trap_frame {
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27, x28, x29;
    uint64_t x30;
    uint64_t elr;
    uint64_t spsr;
};

/*
 * Decode exception class from ESR_EL1
 */
static const char *decode_ec(uint32_t ec)
{
    switch (ec) {
    case 0x00: return "Unknown";
    case 0x15: return "SVC (AArch64)";
    case 0x20: return "Instruction Abort (lower EL)";
    case 0x21: return "Instruction Abort (same EL)";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data Abort (lower EL)";
    case 0x25: return "Data Abort (same EL)";
    case 0x26: return "SP alignment fault";
    case 0x2C: return "Trapped FP exception";
    case 0x3C: return "BRK instruction";
    default:   return "Reserved/Other";
    }
}

/*
 * EL1 Synchronous exception handler
 */
void el1_sync_handler(struct trap_frame *tf)
{
    uint64_t esr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    uint32_t ec = (esr >> 26) & 0x3F;

    panic("EL1 Synchronous Exception\n\n"
          "  EC:    0x%x (%s)\n"
          "  ESR:   0x%lx\n"
          "  FAR:   0x%lx\n"
          "  ELR:   0x%lx\n"
          "  SPSR:  0x%lx",
          ec, decode_ec(ec), esr, far, tf->elr, tf->spsr);
}

/*
 * EL1 IRQ handler
 *
 * Called for all IRQs. Identifies the source and dispatches.
 */
void el1_irq_handler(void)
{
    /* Acknowledge interrupt */
    uint32_t irq = gic_acknowledge();

    /* Spurious interrupt? */
    if (irq == 1023) {
        return;
    }

    /* Dispatch based on IRQ number */
    switch (irq) {
    case TIMER_IRQ:
        timer_handler();
        break;

    default:
        /* Unknown interrupt */
        uart_printf("[IRQ] Unhandled IRQ %u\n", irq);
        break;
    }

    /* Signal end of interrupt */
    gic_end_interrupt(irq);
}

/*
 * EL1 SError handler
 */
void el1_serror_handler(struct trap_frame *tf)
{
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));

    panic("EL1 SError (Asynchronous Exception)\n\n"
          "  ESR:   0x%lx\n"
          "  ELR:   0x%lx\n"
          "  SPSR:  0x%lx",
          esr, tf->elr, tf->spsr);
}
