/*
 * shell_io.c - Backend-independent helpers for shell_io
 *
 * shell_io_puts, shell_io_printf, shell_io_vprintf format into a local
 * buffer and call io->write. Each backend (UART, TCP) supplies the
 * vtable; these helpers keep formatting code out of every backend.
 */

#include "shell_io.h"
#include "uart.h"   /* for uart_vsnprintf */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "string.h"

/* Size of the stack-formatting buffer used by shell_io_printf.
 *
 * Interactive dashboards like the IPC demo render an entire frame in one
 * printf call, which can easily exceed SHELL_MAX_LINE. Keep this buffer
 * comfortably above a full telnet-screen payload so frames are not silently
 * truncated mid-line.
 */
#define SHELL_IO_PRINTF_BUF 4096

void shell_io_puts(struct shell_io *io, const char *s)
{
    if (!io || !s) {
        return;
    }

    size_t len = strlen(s);
    if (len > 0) {
        io->write(io, s, len);
    }
}

int shell_io_vprintf(struct shell_io *io, const char *fmt, va_list args)
{
    if (!io || !fmt) {
        return 0;
    }

    char buf[SHELL_IO_PRINTF_BUF];
    int n = uart_vsnprintf(buf, sizeof(buf), fmt, args);
    if (n < 0) {
        return 0;
    }

    /* uart_vsnprintf returns characters that would have been written;
     * clamp to buffer capacity. */
    size_t written = (size_t)n;
    if (written >= sizeof(buf)) {
        written = sizeof(buf) - 1;
    }

    if (written > 0) {
        io->write(io, buf, written);
    }
    return (int)written;
}

int shell_io_printf(struct shell_io *io, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = shell_io_vprintf(io, fmt, args);
    va_end(args);
    return n;
}
