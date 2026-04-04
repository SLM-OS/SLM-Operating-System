/*
 * uart.h - UART driver interface for SLM-OS
 *
 * Platform-independent UART interface. The actual implementation
 * is selected at compile time based on the target platform:
 *   - PL011 for QEMU virt and Raspberry Pi 5
 *   - Tegra186-UART for Jetson Orin Nano
 *
 * Thread Safety:
 *   uart_puts() and uart_printf() are synchronized with a spinlock to prevent
 *   interleaved output when multiple CPUs print concurrently. Use the _unlocked
 *   variants in panic handlers or very early boot (before spinlocks are safe).
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "platform.h"

/*
 * Initialize the UART hardware.
 * Must be called before any other UART functions.
 */
void uart_init(void);

/*
 * Send a single character.
 * Blocks until the transmit buffer has space.
 * NOT synchronized - use uart_puts/uart_printf for multi-char output.
 */
void uart_putc(char c);

/*
 * Receive a single character.
 * Blocks until a character is available.
 */
char uart_getc(void);

#if defined(PLATFORM_RASPI5)
/*
 * Initialize UART RX interrupt handling.
 * Enables PL011 RX/timeout interrupts and GIC routing for UART_IRQ.
 * Must be called after gic_init(). Falls back to polling if IRQ never fires.
 */
void uart_irq_init(void);

/*
 * UART RX interrupt handler.
 * Called from el1_irq_handler when UART_IRQ fires.
 * Must NOT call uart output functions (deadlock risk).
 */
void uart_irq_handler(void);
#endif

/*
 * Send a null-terminated string (synchronized).
 * Acquires lock to prevent interleaved output from multiple CPUs.
 * Automatically converts \n to \r\n.
 */
void uart_puts(const char *s);

/*
 * Send a null-terminated string (unlocked).
 * Use for panic handlers or very early boot.
 * Automatically converts \n to \r\n.
 */
void uart_puts_unlocked(const char *s);

/*
 * Formatted output (synchronized, printf-style).
 * Acquires lock to prevent interleaved output from multiple CPUs.
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
 * Formatted output (unlocked, printf-style).
 * Use for panic handlers or very early boot.
 * Returns: number of characters written
 */
int uart_printf_unlocked(const char *fmt, ...);

/*
 * Formatted output with va_list (unlocked).
 * Used internally and by debug macros.
 */
int uart_vprintf(const char *fmt, va_list args);

/*
 * Formatted output to buffer (snprintf-style).
 *
 * Writes at most (size - 1) characters to buf, always null-terminates.
 * Same format specifiers as uart_printf.
 *
 * @buf:  Output buffer
 * @size: Buffer size (including space for null terminator)
 * @fmt:  Format string
 *
 * Returns: Number of characters that would have been written if buf
 *          was large enough (excluding null terminator), or -1 on error.
 */
int uart_snprintf(char *buf, size_t size, const char *fmt, ...);

/*
 * Formatted output to buffer with va_list.
 */
int uart_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

#endif /* UART_H */
