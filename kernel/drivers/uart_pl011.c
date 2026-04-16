/*
 * uart_pl011.c - PL011 UART driver for SLM-OS
 *
 * Supports: QEMU virt machine, Raspberry Pi 5
 *
 * The PL011 is ARM's PrimeCell UART with 16-entry FIFOs.
 * On QEMU, baud rate configuration is ignored (virtual serial).
 *
 * This driver provides the low-level hardware interface:
 *   - uart_init()  - Initialize hardware
 *   - uart_putc()  - Send a character
 *   - uart_getc()  - Receive a character
 *
 * Higher-level functions (uart_puts, uart_printf) are in kprintf.c.
 */

#include "platform.h"
#include "uart.h"

#ifndef UART_TYPE_PL011
#error "uart_pl011.c included but UART_TYPE_PL011 not defined"
#endif

/* PL011 Register Offsets */
#define PL011_DR        0x000   /* Data Register */
#define PL011_FR        0x018   /* Flag Register */
#define PL011_IBRD      0x024   /* Integer Baud Rate Divisor */
#define PL011_FBRD      0x028   /* Fractional Baud Rate Divisor */
#define PL011_LCR_H     0x02C   /* Line Control Register */
#define PL011_CR        0x030   /* Control Register */
#define PL011_IMSC      0x038   /* Interrupt Mask Set/Clear */
#define PL011_ICR       0x044   /* Interrupt Clear Register */

/* Flag Register bits */
#define PL011_FR_TXFF   (1 << 5)    /* Transmit FIFO full */
#define PL011_FR_RXFE   (1 << 4)    /* Receive FIFO empty */
#define PL011_FR_BUSY   (1 << 3)    /* UART busy */

/* Line Control Register bits */
#define PL011_LCR_WLEN8 (3 << 5)    /* 8-bit word length */
#define PL011_LCR_FEN   (1 << 4)    /* FIFO enable */

/* Control Register bits */
#define PL011_CR_RXE    (1 << 9)    /* Receive enable */
#define PL011_CR_TXE    (1 << 8)    /* Transmit enable */
#define PL011_CR_UARTEN (1 << 0)    /* UART enable */

/* Register access macros */
#define UART_REG(offset) (*(volatile uint32_t *)(UART_BASE + (offset)))

/*
 * Initialize the PL011 UART.
 * Configures 8N1, enables FIFOs, enables TX/RX.
 */
void uart_init(void)
{
    /* Disable UART while configuring */
    UART_REG(PL011_CR) = 0;

    /* Clear all pending interrupts */
    UART_REG(PL011_ICR) = 0x7FF;

    /*
     * Set baud rate.
     * Divisor = UART_CLOCK / (16 * baud)
     * For 115200 baud with 24MHz clock:
     *   Divisor = 24000000 / (16 * 115200) = 13.0208...
     *   IBRD = 13, FBRD = 0.0208 * 64 = 1
     *
     * Note: QEMU ignores these values for virtual serial.
     */
    UART_REG(PL011_IBRD) = 13;
    UART_REG(PL011_FBRD) = 1;

    /* Configure line: 8 bits, no parity, 1 stop bit, FIFOs enabled */
    UART_REG(PL011_LCR_H) = PL011_LCR_WLEN8 | PL011_LCR_FEN;

    /* Disable all interrupts (polling mode for now) */
    UART_REG(PL011_IMSC) = 0;

    /* Enable UART, TX, and RX */
    UART_REG(PL011_CR) = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;
}

/*
 * Send a single character.
 * Blocks until transmit FIFO has space.
 */
void uart_putc(char c)
{
    /* Wait until transmit FIFO is not full */
    while (UART_REG(PL011_FR) & PL011_FR_TXFF) {
        /* spin */
    }

    UART_REG(PL011_DR) = c;
}

/*
 * Receive a single character.
 * Blocks until receive FIFO has data.
 * Yields to scheduler while waiting so other tasks can run.
 */
char uart_getc(void)
{
    /* Wait until receive FIFO is not empty, yielding to let other tasks run */
    while (UART_REG(PL011_FR) & PL011_FR_RXFE) {
        extern void yield(void);
        yield();
    }

    return (char)(UART_REG(PL011_DR) & 0xFF);
}

int uart_try_getc(void)
{
    if (UART_REG(PL011_FR) & PL011_FR_RXFE) {
        return -1;
    }
    return (int)(UART_REG(PL011_DR) & 0xFF);
}
