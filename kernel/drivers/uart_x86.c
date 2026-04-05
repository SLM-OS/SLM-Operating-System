/*
 * uart_x86.c - 16550 UART driver for x86-64
 *
 * Implements the uart.h interface using COM1 (I/O port 0x3F8).
 * Configuration: 115200 baud, 8N1, FIFO enabled.
 */

#include "platform.h"

#if defined(UART_TYPE_16550)

#include <stdint.h>

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

/* 16550 register offsets */
#define UART_RBR    0   /* Receive Buffer (read) */
#define UART_THR    0   /* Transmit Holding (write) */
#define UART_IER    1   /* Interrupt Enable */
#define UART_FCR    2   /* FIFO Control (write) */
#define UART_LCR    3   /* Line Control */
#define UART_MCR    4   /* Modem Control */
#define UART_LSR    5   /* Line Status */
#define UART_DLL    0   /* Divisor Latch Low (DLAB=1) */
#define UART_DLH    1   /* Divisor Latch High (DLAB=1) */

/* Line Status Register bits */
#define LSR_DR      0x01    /* Data Ready */
#define LSR_THRE    0x20    /* Transmit Holding Register Empty */

void uart_init(void)
{
    uint16_t base = (uint16_t)UART_BASE;

    outb(base + UART_IER, 0x00);   /* Disable interrupts */
    outb(base + UART_LCR, 0x80);   /* Enable DLAB */
    outb(base + UART_DLL, 0x01);   /* Divisor low: 115200 baud */
    outb(base + UART_DLH, 0x00);   /* Divisor high */
    outb(base + UART_LCR, 0x03);   /* 8N1, DLAB off */
    outb(base + UART_FCR, 0xC7);   /* Enable FIFO, clear, 14-byte threshold */
    outb(base + UART_MCR, 0x0B);   /* DTR + RTS + OUT2 */
}

void uart_putc(char c)
{
    uint16_t base = (uint16_t)UART_BASE;

    while ((inb(base + UART_LSR) & LSR_THRE) == 0)
        ;
    outb(base + UART_THR, c);
}

char uart_getc(void)
{
    uint16_t base = (uint16_t)UART_BASE;

    while ((inb(base + UART_LSR) & LSR_DR) == 0)
        ;
    return (char)inb(base + UART_RBR);
}

#endif /* UART_TYPE_16550 */
