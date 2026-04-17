/*
 * shell_io_tcp.h - TCP-backed shell_io
 *
 * Routes a shell session's I/O over a lwIP raw TCP pcb. Because the
 * project builds lwIP with NO_SYS=1 / LWIP_SOCKET=0, the BSD sockets
 * API is unavailable and all lwIP calls must happen on the single
 * lwIP thread (net_pump). The backend bridges that constraint:
 *
 *   - tcp_recv callback (net_pump ctx) appends bytes into a per-
 *     session RX ring buffer.
 *   - The session's shell task calls read_char / read_line and blocks
 *     by yielding until the ring has data or the session is closed.
 *   - The session's shell task writes into a per-session TX ring
 *     buffer; shell_io_tcp_poll() drains that ring via tcp_write /
 *     tcp_output from net_poll() context.
 *   - A close from either side (peer FIN, tcp_err, shell_io close)
 *     marks the session closed; read_char returns -1 so the REPL
 *     exits, then the session teardown path frees the pool slot.
 *
 * All public entry points are safe to call from their documented
 * context only. Do not call shell_io_tcp_create from outside the
 * accept callback.
 */

#ifndef SHELL_IO_TCP_H
#define SHELL_IO_TCP_H

#include <stdint.h>
#include "shell_io.h"

struct tcp_pcb;

/* Allocate a TCP shell_io slot and wire tcp_recv / tcp_err / tcp_sent
 * callbacks on `pcb`. Returns NULL if the pool is exhausted (in which
 * case the caller should tcp_close(pcb) and drop the connection).
 * The returned pointer is stable for the session's lifetime; the
 * backend will free the slot when both the peer has disconnected and
 * shell_io_tcp_destroy() has been called.
 *
 * Context: net_pump (accept callback). */
struct shell_io *shell_io_tcp_create(struct tcp_pcb *pcb);

/* Tear down a TCP shell_io: marks the slot eligible for reuse and,
 * if the peer is still connected, schedules a close via the drain
 * path. Safe to call from the shell task. */
void shell_io_tcp_destroy(struct shell_io *io);

/* Drain all pending TX and handle deferred close for every active
 * TCP session. Call once per net_poll() tick. No-op when there are
 * no active sessions.
 *
 * Context: net_pump (net_poll). */
void shell_io_tcp_poll(void);

/* Number of currently active TCP sessions (in-use pool slots).
 * Useful for diagnostics. */
uint32_t shell_io_tcp_active_count(void);

#endif /* SHELL_IO_TCP_H */
