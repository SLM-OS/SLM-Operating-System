/*
 * main.c - SLM-OS kernel main entry point
 */

#include "platform.h"

/*
 * Minimal UART output for early boot debugging.
 * Full UART driver will be implemented in Week 2.
 */
static volatile unsigned int * const UART_DR = (unsigned int *)UART_BASE;

static void uart_putc(char c)
{
    *UART_DR = c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

/*
 * kernel_main - Main kernel entry point
 *
 * Called from boot.S after basic hardware initialization.
 * This function should not return.
 */
void kernel_main(void)
{
    uart_puts("SLM-OS: Boot successful!\n");
    uart_puts("SLM-OS: Kernel running at EL1\n");

    /* Halt - nothing else to do yet */
    uart_puts("SLM-OS: Halting.\n");

    while (1) {
        /* Wait for interrupt (low power) */
        __asm__ volatile("wfi");
    }
}
