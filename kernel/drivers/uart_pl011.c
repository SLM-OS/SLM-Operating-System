/*
 * uart_pl011.c - PL011 UART driver for SLM-OS
 *
 * Supports: QEMU virt machine, Raspberry Pi 5
 *
 * The PL011 is ARM's PrimeCell UART with 16-entry FIFOs.
 * On QEMU, baud rate configuration is ignored (virtual serial).
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
 */
char uart_getc(void)
{
    /* Wait until receive FIFO is not empty */
    while (UART_REG(PL011_FR) & PL011_FR_RXFE) {
        /* spin */
    }

    return (char)(UART_REG(PL011_DR) & 0xFF);
}

/*
 * Send a null-terminated string.
 * Converts \n to \r\n for proper terminal display.
 */
void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

/* ========================================================================
 * Printf Implementation
 * ======================================================================== */

/* Helper: print a single character, return 1 */
static int print_char(char c)
{
    uart_putc(c);
    return 1;
}

/* Helper: print a string, return length */
static int print_string(const char *s)
{
    int count = 0;
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
        count++;
    }
    return count;
}

/* Helper: print unsigned integer in given base */
static int print_unsigned(uint64_t value, int base, int uppercase)
{
    char buf[24];   /* Enough for 64-bit number in any base */
    char *p = buf + sizeof(buf) - 1;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int count = 0;

    *p = '\0';

    if (value == 0) {
        *--p = '0';
    } else {
        while (value > 0) {
            *--p = digits[value % base];
            value /= base;
        }
    }

    while (*p) {
        uart_putc(*p++);
        count++;
    }

    return count;
}

/* Helper: print signed integer */
static int print_signed(int64_t value, int base)
{
    int count = 0;

    if (value < 0) {
        uart_putc('-');
        count++;
        value = -value;
    }

    count += print_unsigned((uint64_t)value, base, 0);
    return count;
}

/*
 * Formatted output with va_list.
 */
int uart_vprintf(const char *fmt, va_list args)
{
    int count = 0;
    int is_long = 0;

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') {
                uart_putc('\r');
            }
            uart_putc(*fmt++);
            count++;
            continue;
        }

        fmt++;  /* Skip '%' */

        /* Check for long modifier */
        is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            count += print_char((char)va_arg(args, int));
            break;

        case 's':
            count += print_string(va_arg(args, const char *));
            break;

        case 'd':
        case 'i':
            if (is_long) {
                count += print_signed(va_arg(args, int64_t), 10);
            } else {
                count += print_signed(va_arg(args, int32_t), 10);
            }
            break;

        case 'u':
            if (is_long) {
                count += print_unsigned(va_arg(args, uint64_t), 10, 0);
            } else {
                count += print_unsigned(va_arg(args, uint32_t), 10, 0);
            }
            break;

        case 'x':
            if (is_long) {
                count += print_unsigned(va_arg(args, uint64_t), 16, 0);
            } else {
                count += print_unsigned(va_arg(args, uint32_t), 16, 0);
            }
            break;

        case 'X':
            if (is_long) {
                count += print_unsigned(va_arg(args, uint64_t), 16, 1);
            } else {
                count += print_unsigned(va_arg(args, uint32_t), 16, 1);
            }
            break;

        case 'p':
            uart_putc('0');
            uart_putc('x');
            count += 2;
            count += print_unsigned((uint64_t)(uintptr_t)va_arg(args, void *), 16, 0);
            break;

        case '%':
            uart_putc('%');
            count++;
            break;

        case '\0':
            /* Trailing % at end of format string */
            return count;

        default:
            /* Unknown format, print literally */
            uart_putc('%');
            uart_putc(*fmt);
            count += 2;
            break;
        }

        fmt++;
    }

    return count;
}

/*
 * Formatted output (printf-style).
 */
int uart_printf(const char *fmt, ...)
{
    va_list args;
    int count;

    va_start(args, fmt);
    count = uart_vprintf(fmt, args);
    va_end(args);

    return count;
}
