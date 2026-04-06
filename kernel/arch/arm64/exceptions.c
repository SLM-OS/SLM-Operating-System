/*
 * exceptions.c - Exception handlers for SLM-OS
 *
 * C handlers called from vectors.S
 */

#include "gic.h"
#include "timer.h"
#include "uart.h"
#include "debug.h"
#include "task.h"
#include "smp.h"
#include "platform.h"
#include <stdint.h>

/* Timer IRQ numbers */
#define PHYS_TIMER_IRQ  30  /* Physical timer PPI 14 */
#define VIRT_TIMER_IRQ  27  /* Virtual timer PPI 11 */

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
 * Decode Data/Instruction Fault Status Code (DFSC/IFSC)
 * Bits [5:0] of ESR_EL1 for data/instruction aborts
 */
static const char *decode_fault_status(uint32_t fsc)
{
    switch (fsc & 0x3F) {
    /* Address size faults */
    case 0x00: return "Address size fault, level 0";
    case 0x01: return "Address size fault, level 1";
    case 0x02: return "Address size fault, level 2";
    case 0x03: return "Address size fault, level 3";

    /* Translation faults (page not mapped) */
    case 0x04: return "Translation fault, level 0";
    case 0x05: return "Translation fault, level 1";
    case 0x06: return "Translation fault, level 2";
    case 0x07: return "Translation fault, level 3";

    /* Access flag faults */
    case 0x08: return "Access flag fault, level 0";
    case 0x09: return "Access flag fault, level 1";
    case 0x0A: return "Access flag fault, level 2";
    case 0x0B: return "Access flag fault, level 3";

    /* Permission faults */
    case 0x0C: return "Permission fault, level 0";
    case 0x0D: return "Permission fault, level 1";
    case 0x0E: return "Permission fault, level 2";
    case 0x0F: return "Permission fault, level 3";

    /* Synchronous external aborts */
    case 0x10: return "Synchronous external abort, level 0";
    case 0x11: return "Synchronous external abort, level 1";
    case 0x12: return "Synchronous external abort, level 2";
    case 0x13: return "Synchronous external abort, level 3";
    case 0x14: return "Synchronous external abort on translation table walk";

    /* Other faults */
    case 0x21: return "Alignment fault";
    case 0x30: return "TLB conflict abort";
    case 0x31: return "Unsupported atomic hardware update";

    default:   return "Unknown fault status";
    }
}

/*
 * Check if exception class is a data abort
 */
static inline int is_data_abort(uint32_t ec)
{
    return (ec == 0x24 || ec == 0x25);
}

/*
 * Check if exception class is an instruction abort
 */
static inline int is_instruction_abort(uint32_t ec)
{
    return (ec == 0x20 || ec == 0x21);
}

/*
 * Handle page fault (data abort or instruction abort)
 * Provides detailed fault information before panic
 */
static void handle_page_fault(struct trap_frame *tf, uint64_t esr, uint64_t far,
                              uint32_t ec, int is_data)
{
    uint32_t fsc = esr & 0x3F;          /* Fault Status Code (bits 5:0) */
    int is_write = (esr >> 6) & 1;      /* WnR bit (bit 6) - data aborts only */
    int is_cm = (esr >> 8) & 1;         /* CM bit - cache maintenance */

    /* Get current task info */
    struct task *current = task_current();
    uint32_t task_id = current ? current->id : 0xFFFFFFFF;
    const char *task_name = current ? current->name : "<none>";
    uint32_t current_cpu = cpu_id();

    uart_puts("\n\n");
    uart_puts("*********************************\n");
    uart_puts("***       PAGE FAULT          ***\n");
    uart_puts("*********************************\n\n");

    /* Fault type */
    if (is_data) {
        uart_printf("Type:    Data Abort (%s)\n",
                    is_write ? "WRITE" : "READ");
    } else {
        uart_puts("Type:    Instruction Abort (FETCH)\n");
    }

    /* Fault address and reason */
    uart_printf("Address: 0x%lx\n", far);
    uart_printf("Reason:  %s\n", decode_fault_status(fsc));

    /* Task context */
    uart_puts("\nTask Context:\n");
    uart_printf("  Task ID:   %lu\n", task_id);
    uart_printf("  Task Name: %s\n", task_name);
    uart_printf("  CPU:       %lu\n", current_cpu);

    /* Instruction that caused the fault */
    uart_puts("\nFault Location:\n");
    uart_printf("  ELR (PC):  0x%lx\n", tf->elr);

    /* Raw register values for debugging */
    uart_puts("\nRaw Exception State:\n");
    uart_printf("  ESR_EL1:   0x%lx\n", esr);
    uart_printf("  EC:        0x%x (%s)\n", ec, decode_ec(ec));
    uart_printf("  FSC:       0x%x\n", fsc);
    if (is_data) {
        uart_printf("  WnR:       %d (%s)\n", is_write,
                    is_write ? "write" : "read");
        if (is_cm) {
            uart_puts("  Note:      Fault during cache maintenance op\n");
        }
    }
    uart_printf("  SPSR_EL1:  0x%lx\n", tf->spsr);

    uart_puts("\nSystem halted.\n");

    /* Halt */
    while (1) {
        __asm__ volatile("wfi");
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

    /* Handle page faults specially for better diagnostics */
    if (is_data_abort(ec)) {
        /* Disable interrupts */
        __asm__ volatile("msr daifset, #0xF");
        handle_page_fault(tf, esr, far, ec, 1);
        /* Never returns */
    }

    if (is_instruction_abort(ec)) {
        /* Disable interrupts */
        __asm__ volatile("msr daifset, #0xF");
        handle_page_fault(tf, esr, far, ec, 0);
        /* Never returns */
    }

    /* All other synchronous exceptions */
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
    case PHYS_TIMER_IRQ:
    case VIRT_TIMER_IRQ: {
        /*
         * For timer interrupt, we must signal EOI BEFORE calling the handler
         * because timer_handler() -> scheduler_tick() -> schedule() may
         * context switch to a different task and never return here.
         *
         * If we don't signal EOI before the switch, the GIC will think
         * the interrupt is still being handled and won't deliver more
         * timer interrupts to this CPU.
         */
        static volatile uint32_t timer_irq_entered;
        timer_irq_entered++;
        gic_end_interrupt(irq);
        timer_handler();
        return;  /* EOI already done, don't do it again */
    }

#if defined(PLATFORM_RASPI5)
    case UART_IRQ:
        gic_end_interrupt(irq);
        uart_irq_handler();
        return;
#endif

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
