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

#include <stdbool.h>
#include <stdint.h>
#include "shell_io.h"

struct tcp_pcb;
struct shell_session;

/* Allocate a TCP shell_io slot and wire tcp_recv / tcp_err / tcp_sent
 * callbacks on `pcb`. Returns NULL if the pool is exhausted (in which
 * case the caller should tcp_close(pcb) and drop the connection).
 * Emits the server's initial telnet option negotiation before
 * returning. The returned pointer is stable for the session's
 * lifetime; the backend will free the slot when both the peer has
 * disconnected and shell_io_tcp_destroy() has been called.
 *
 * Context: net_pump (accept callback). */
struct shell_io *shell_io_tcp_create(struct tcp_pcb *pcb);

/* Associate a shell_session with an already-created TCP shell_io so
 * the telnet parser can update the session's window_cols /
 * window_rows / term_type / interrupt_requested fields as NAWS /
 * TERMINAL-TYPE / IAC IP arrive. The accept callback calls this
 * right after shell_session_alloc. */
void shell_io_tcp_attach_session(struct shell_io *io, struct shell_session *s);

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

/* Metadata about one active TCP session, snapshotted for safe access
 * from outside the shell_io_tcp module. */
struct tcp_session_info {
    uint32_t session_id;    /* matches shell_session.id */
    uint32_t peer_ip;       /* network byte order */
    uint16_t peer_port;
    uint16_t _pad;
    uint32_t connected_at;  /* sys_now() timestamp (ms since boot) */
};

/* Visitor for shell_io_tcp_foreach. Return true to continue iteration,
 * false to stop early. Called once per active session. */
typedef bool (*tcp_session_visitor_t)(const struct tcp_session_info *info,
                                      void *ctx);

/* Iterate every active TCP session and call `visitor` with a snapshot
 * of its metadata. Safe to call from the shell task.
 *
 * Concurrency model:
 *   - Each slot's metadata is snapshotted under pool_lock.
 *   - The visitor is called with the snapshot AFTER the lock is
 *     released for that slot, so the visitor may do arbitrary work
 *     (print, push into a Lua table, ...) without holding a spinlock.
 *   - The snapshot is a value copy; the visitor sees a consistent
 *     set of fields even if the underlying session is freed or
 *     reused between snapshot and callback. */
void shell_io_tcp_foreach(tcp_session_visitor_t visitor, void *ctx);

/* Force-disconnect the session with the given id. Marks the shell_io
 * closed so the session task's REPL exits on its next read; the net
 * pump path then tcp_closes the pcb and frees the pool slot. Returns
 * true if a matching session was found, false otherwise. */
bool shell_io_tcp_kick(uint32_t session_id);

/* Test-only helper: normalize one or two output chunks using the same
 * CRLF carry-over rules as the live TCP backend. Returns the byte count
 * written to `out` (not including any trailing NUL the caller may add). */
size_t shell_io_tcp_test_normalize_output(const char *first,
                                          const char *second,
                                          char *out,
                                          size_t out_len);

/* Test-only helper: override the wall-clock cap used by tcp_write_buf
 * before it bails out when the drain is stuck (#536). Pass 0 to restore
 * the production default. Production builds never call this. */
void shell_io_tcp_test_set_write_timeout_ms(uint32_t ms);

/* Test-only driver for #536: allocate a pool slot, force the TX ring
 * full with no pcb to drain it, call tcp_write_buf, and verify the
 * caller returns within `timeout_override_ms` (+ ~1 s of slop) with
 * the session marked degraded + closed. Returns 0 on success, -1 if
 * no pool slot could be allocated, -2 if the timeout was violated. */
int shell_io_tcp_test_run_write_timeout(uint32_t timeout_override_ms);

/* Test-only driver for #537: allocate a pool slot, mimic the
 * post-tcp_close state, and verify shell_io_tcp_poll defers
 * freeing the slot until the close-settle window has elapsed.
 * Returns 0 on success, -1 if no slot could be allocated, -2 if
 * poll freed prematurely, -3 if poll failed to free after the
 * settle period elapsed. */
int shell_io_tcp_test_run_close_settling(void);

/* Test-only driver for the #537 leak-regression test: drives one
 * full close cycle with a real heap-baseline snapshot (vs.
 * `shell_io_tcp_test_run_close_settling`'s baseline=0), so the
 * close-time delta computation is meaningful. Wrap in a loop +
 * `tcp_shell_server_get_stats` to assert leak_warnings doesn't
 * grow across N cycles. Returns 0 on success, -1 if no slot could
 * be allocated, -2 if the slot wasn't freed by the poll. */
int shell_io_tcp_test_run_clean_close_cycle(void);

/* Test-only driver for #597 / `tcp_read_buf`: primes the rx ring
 * with a known pattern and verifies the batched-read vtable
 * function drains all bytes in a single call. Returns 0 on success,
 * -1 if no slot could be allocated, -2 if the byte count or content
 * didn't match the expected pattern. */
int shell_io_tcp_test_run_read_buf_drains_ring(void);

#endif /* SHELL_IO_TCP_H */
