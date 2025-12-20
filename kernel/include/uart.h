/*
 * uart.h - UART driver interface for SLM-OS
 *
 * Platform-independent UART interface. The actual implementation
 * is selected at compile time based on the target platform:
 *   - PL011 for QEMU virt and Raspberry Pi 5
 *   - Tegra186-UART for Jetson Orin Nano
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stdarg.h>

/*
 * Initialize the UART hardware.
 * Must be called before any other UART functions.
 */
void uart_init(void);

/*
 * Send a single character.
 * Blocks until the transmit buffer has space.
 */
void uart_putc(char c);

/*
 * Receive a single character.
 * Blocks until a character is available.
 */
char uart_getc(void);

/*
 * Send a null-terminated string.
 * Automatically converts \n to \r\n.
 */
void uart_puts(const char *s);

/*
 * Formatted output (printf-style).
 *
 * Supported format specifiers:
 *   %c  - character
 *   %s  - string
 *   %d  - signed decimal integer
 *   %u  - unsigned decimal integer
 *   %x  - unsigned hexadecimal (lowercase)
 *   %X  - unsigned hexadecimal (uppercase)
 *   %p  - pointer (0x prefixed hex)
 *   %l  - long modifier (ld, lu, lx, lX)
 *   %%  - literal percent sign
 *
 * Returns: number of characters written
 */
int uart_printf(const char *fmt, ...);

/*
 * Formatted output with va_list.
 * Used internally and by debug macros.
 */
int uart_vprintf(const char *fmt, va_list args);

#endif /* UART_H */
