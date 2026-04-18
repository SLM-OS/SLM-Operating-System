/*
 * shell_io.h - I/O abstraction for shell sessions
 *
 * Provides a virtual stdio for the shell so the same REPL and command
 * handlers can serve either the UART console or a future TCP/telnet
 * session. A shell_io is a thin vtable + private context. Backends:
 *   - shell_io_uart:  wraps uart_getc/putc/printf for the console
 *   - shell_io_tcp:   (future) wraps a lwIP raw-callback TCP pcb
 *
 * Kernel logs (uart_printf from background tasks, panic handlers,
 * driver INFO/WARN) continue to use the uart_* API directly; this
 * interface is for shell command output and input only.
 */

#ifndef SHELL_IO_H
#define SHELL_IO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

struct shell_io {
    /* Read one character. Blocks until one is available. Returns a
     * byte value in 0..255, or -1 on EOF / closed backend. */
    int  (*read_char)(struct shell_io *io);

    /* Non-blocking read. Returns a byte in 0..255, or -1 if no data
     * is available right now (or the backend is closed). */
    int  (*try_read_char)(struct shell_io *io);

    /* Write a buffer. Output must not be interleaved with output from
     * a concurrent writer on the same backend — backends acquire their
     * own lock if needed (UART does; TCP is per-session). */
    void (*write)(struct shell_io *io, const char *buf, size_t len);

    /* Flush any buffered output. May be a no-op. */
    void (*flush)(struct shell_io *io);

    /* Close the backend. May be a no-op (UART). */
    void (*close)(struct shell_io *io);

    /* True iff the backend can still do I/O (e.g. TCP peer connected). */
    bool (*is_open)(struct shell_io *io);

    /* Backend-private data. */
    void *ctx;
};

/* Convenience: write a null-terminated string via io->write. */
void shell_io_puts(struct shell_io *io, const char *s);

/* Convenience: printf-style formatted output via io->write. Uses the
 * same format specifiers as uart_printf (see uart.h). */
int  shell_io_printf(struct shell_io *io, const char *fmt, ...);
int  shell_io_vprintf(struct shell_io *io, const char *fmt, va_list args);

/* Singleton UART-backed shell_io. Initialized lazily on first call;
 * safe to call from any task on CPU 0. */
struct shell_io *shell_io_uart(void);

#endif /* SHELL_IO_H */
