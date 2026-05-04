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

    /* Optional. True iff the line-edit loop should echo printable
     * input bytes back to the peer. Defaults to ON (the equivalent
     * of returning true) when NULL — preserves the UART console's
     * always-echo behavior without forcing every backend to
     * implement this hook. The TCP backend wires it to the telnet
     * IAC ECHO negotiation: when the peer says `DONT ECHO` (which
     * slm-put.py and any other line-mode-buffering client do as
     * default), echo is suppressed entirely, eliminating the per-
     * char `tcp_write_buf` lock+ring traffic that otherwise caps
     * `xput chunk` throughput at ~30 KB/s. */
    bool (*echo_enabled)(struct shell_io *io);

    /* Optional batched read. Drains up to `max_len` bytes from the
     * backend's RX buffer into `dst` in a single backend operation
     * (one lock-cycle on the TCP backend, vs. one per byte for
     * `read_char`). Blocks until at least one byte is available,
     * then returns whatever the backend has queued — could be 1,
     * could be max_len. Returns -1 when the backend is closed and
     * no bytes are queued.
     *
     * Backends without batched-read support set this to NULL;
     * callers fall back to the per-char `read_char` loop via
     * `shell_io_read_buf`.
     *
     * Critical for #597's throughput ceiling: a 32 KB hex
     * `xput chunk` line takes ~32K vtable+lock cycles via
     * read_char; a 1024-byte batched read drops that to ~32 cycles
     * with the same wire-level RX cost. */
    int  (*read_buf)(struct shell_io *io, char *dst, int max_len);

    /* Backend-private data. */
    void *ctx;
};

/* Helper: returns true if `io` should echo input. NULL-safe; uses
 * the vtable's `echo_enabled` callback when present, otherwise
 * defaults to true. Centralized so the line-edit loop and any
 * future caller don't have to repeat the NULL check. */
bool shell_io_echo_enabled(struct shell_io *io);

/* Helper: drain up to `max_len` bytes from `io` into `dst`. Uses
 * the vtable's `read_buf` callback when present, otherwise falls
 * back to a `read_char` loop that reads ONE byte (so callers don't
 * stall waiting for the second byte that may never arrive). Blocks
 * until at least one byte is available; returns -1 on closed.
 *
 * Centralized fallback so callers like `shell_read_command` get the
 * batched-read perf win on backends that support it (TCP) without
 * forcing every backend (UART) to implement the hook. */
int  shell_io_read_buf(struct shell_io *io, char *dst, int max_len);

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
