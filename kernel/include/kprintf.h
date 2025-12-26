/*
 * kprintf.h - Kernel printf interface
 *
 * Centralized formatting functions for kernel output. These functions
 * use the platform-specific UART driver for actual character output.
 *
 * Thread Safety:
 *   uart_puts() and uart_printf() are synchronized with a spinlock to prevent
 *   interleaved output from multiple CPUs. Use the _unlocked variants for
 *   panic handlers or very early boot before the lock is safe.
 */

#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>
#include <stddef.h>

/*
 * Send a null-terminated string (synchronized).
 * Acquires lock to prevent interleaved output from multiple CPUs.
 * Converts \n to \r\n for proper terminal display.
 */
void uart_puts(const char *s);

/*
 * Send a null-terminated string (unlocked version).
 * Use for panic handlers or very early boot.
 */
void uart_puts_unlocked(const char *s);

/*
 * Formatted output (synchronized).
 * Acquires lock to prevent interleaved output from multiple CPUs.
 *
 * Supported format specifiers:
 *   %c     - character
 *   %s     - string
 *   %d, %i - signed decimal (32-bit, or 64-bit with 'l')
 *   %u     - unsigned decimal (32-bit, or 64-bit with 'l')
 *   %x, %X - hexadecimal (lowercase/uppercase)
 *   %p     - pointer (prints 0x prefix)
 *   %%     - literal percent
 *
 * Modifiers:
 *   l      - long (64-bit for d/i/u/x/X)
 *
 * Flags:
 *   -      - left justify within width
 *   0      - zero-pad numbers (ignored if '-' present)
 *
 * Width:
 *   [0-9]+ - minimum field width
 */
int uart_printf(const char *fmt, ...);

/*
 * Formatted output (unlocked version).
 * Use for panic handlers or very early boot.
 */
int uart_printf_unlocked(const char *fmt, ...);

/*
 * Formatted output with va_list (unlocked).
 * Used internally and by panic handlers.
 */
int uart_vprintf(const char *fmt, va_list args);

/*
 * Format to buffer with size limit.
 * Returns number of characters that would have been written (excluding null).
 * Always null-terminates if size > 0.
 */
int uart_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

/*
 * Format to buffer (convenience wrapper).
 */
int uart_snprintf(char *buf, size_t size, const char *fmt, ...);

#endif /* KPRINTF_H */
