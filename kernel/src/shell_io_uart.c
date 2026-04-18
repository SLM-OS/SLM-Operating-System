/*
 * shell_io_uart.c - UART backend for shell_io
 *
 * Routes shell read/write through the UART driver. This is the default
 * backend used by the console session and preserves the pre-refactor
 * behavior: echo, blocking reads, synchronized output.
 */

#include "shell_io.h"
#include "uart.h"

#include <stdarg.h>
#include <stddef.h>

static int uart_read_char(struct shell_io *io)
{
    (void)io;
    return (int)(unsigned char)uart_getc();
}

static int uart_try_read_char(struct shell_io *io)
{
    (void)io;
    return uart_try_getc();
}

static void uart_write(struct shell_io *io, const char *buf, size_t len)
{
    (void)io;
    /* uart_puts acquires a lock and handles \n -> \r\n conversion.
     * Writing char-by-char via uart_putc would bypass both. To avoid
     * an extra copy we loop on uart_putc and emit \r manually. */
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') {
            uart_putc('\r');
        }
        uart_putc(c);
    }
}

static void uart_flush(struct shell_io *io)
{
    (void)io;
    /* UART has no software buffer. */
}

static void uart_close(struct shell_io *io)
{
    (void)io;
    /* Console UART is never closed. */
}

static bool uart_is_open(struct shell_io *io)
{
    (void)io;
    return true;
}

static struct shell_io uart_io = {
    .read_char     = uart_read_char,
    .try_read_char = uart_try_read_char,
    .write         = uart_write,
    .flush         = uart_flush,
    .close         = uart_close,
    .is_open       = uart_is_open,
    .ctx           = NULL,
};

struct shell_io *shell_io_uart(void)
{
    return &uart_io;
}
