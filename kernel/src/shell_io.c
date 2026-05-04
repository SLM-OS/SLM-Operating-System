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

bool shell_io_echo_enabled(struct shell_io *io)
{
    /* NULL-safe and vtable-optional: callers don't have to repeat the
     * NULL checks, and a backend that doesn't implement the hook
     * keeps echoing (the historical default) — only the TCP backend
     * needs to opt into the negotiation-driven path. */
    if (!io || !io->echo_enabled) {
        return true;
    }
    return io->echo_enabled(io);
}

int shell_io_read_buf(struct shell_io *io, char *dst, int max_len)
{
    if (!io || !dst || max_len <= 0) {
        return -1;
    }

    /* Backends that implement batched read (TCP) drain the RX ring
     * in a single lock cycle. The shell_read_command line-edit loop
     * uses this to avoid the per-char vtable+lock+IRQ-disable cost
     * that capped `xput chunk` throughput at ~125 KB/s on hardware
     * (#597). */
    if (io->read_buf) {
        return io->read_buf(io, dst, max_len);
    }

    /* Fallback for backends without batched-read support (UART):
     * block for ONE byte via read_char and return it. The caller's
     * outer loop will re-call us for each subsequent byte, recovering
     * the per-char path. We deliberately do NOT loop reading more
     * bytes here — read_char blocks until data arrives, so a second
     * read_char after the first might wait indefinitely for a byte
     * the peer hasn't sent yet (e.g., the user is typing one
     * character at a time on the UART console). */
    int c = io->read_char(io);
    if (c < 0) {
        return -1;
    }
    dst[0] = (char)c;
    return 1;
}

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
