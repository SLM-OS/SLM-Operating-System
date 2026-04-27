/*
 * tcp_shell_server.h - TCP listener + session task glue
 *
 * Brings up a lwIP raw TCP listening pcb on the configured port; when
 * a connection is accepted, allocates a shell_session and a shell_io
 * backed by shell_io_tcp, then spawns a shell task that runs the
 * normal REPL against the new session.
 *
 * Requires:
 *   - ENABLE_NETWORKING at compile time
 *   - net_init() has been called (lwIP is up)
 *
 * Plan §1.3 (raw callback version) + §1.4.
 */

#ifndef TCP_SHELL_SERVER_H
#define TCP_SHELL_SERVER_H

#include <stdint.h>
#include <stdbool.h>

/* Start the TCP shell listener. Safe to call multiple times: a
 * second start on a different port silently no-ops unless stop is
 * called first. Returns 0 on success, negative on failure (net
 * stack not up, port already bound, out of pcbs). */
int tcp_shell_server_start(uint16_t port);

/* Stop accepting new connections. Existing sessions keep running
 * until their users disconnect. */
void tcp_shell_server_stop(void);

/* Diagnostics. */
bool     tcp_shell_server_running(void);
uint16_t tcp_shell_server_port(void);
uint32_t tcp_shell_server_accepted(void);

/*
 * Per-session lifecycle stats + heap-leak detection.
 *
 * `shell_io_tcp_create` stamps `lwip_stats.mem.used` onto the
 * per-connection `tcp_shell_ctx`; `shell_io_tcp_poll` computes the
 * delta vs that snapshot *after* `tcp_close(pcb)` has run, then
 * calls `tcp_shell_server_note_session_close`. Suspicious overshoot
 * (delta > leak_threshold) accumulates into `total_suspicious_leak_bytes`
 * + `leak_warnings`. Use this with the watchdog `heap_used_peak`
 * to correlate session churn with heap pressure.
 *
 * `active = sessions_opened - sessions_closed`. `peak_active` is the
 * high-water mark seen since boot.
 */
struct tcp_shell_server_stats {
    uint32_t sessions_opened;
    uint32_t sessions_closed;
    uint32_t active;
    uint32_t peak_active;
    /* Cumulative observed leak signal. A "leak" is any session that
     * exited with `heap_used` more than NET_SHELL_TCP_LEAK_THRESHOLD_BYTES
     * above its open-time snapshot — adjustable per build. */
    uint32_t leak_warnings;
    uint32_t total_suspicious_leak_bytes;
    /* Rolling-most-recent observation, signed: negative means the
     * heap was *lower* at session close than at open (a TIME_WAIT'd
     * earlier session's data freed during this session's lifetime). */
    int32_t  last_session_heap_delta_bytes;
    int32_t  max_session_heap_delta_bytes;
};

void tcp_shell_server_get_stats(struct tcp_shell_server_stats *out);

/*
 * Note a session-open event.
 *
 * Bumps `sessions_opened` / `active` / `peak_active`. The heap
 * snapshot is owned by `shell_io_tcp` (it lives on `tcp_shell_ctx`,
 * which outlives `shell_session` by one or two net_pump ticks)
 * so the close-time measurement can run *after* tcp_close has
 * actually released the pcb — see `tcp_shell_server_note_session_close`.
 */
void tcp_shell_server_note_session_open(uint32_t session_id);

/*
 * Note a session-close event with a precomputed heap delta.
 *
 * Called from `shell_io_tcp_poll` right before the per-session ctx
 * pool slot is freed (i.e. *after* `tcp_close(pcb)` and lwIP's
 * unacked-TX free path). At that point any sustained positive
 * delta is a real leak rather than TIME_WAIT residue, which is
 * what makes the WARN here actionable. Updates rolling stats and
 * emits a WARN when delta > NET_SHELL_TCP_LEAK_THRESHOLD_BYTES.
 */
void tcp_shell_server_note_session_close(uint32_t session_id,
                                         int32_t  heap_delta_bytes);

#endif /* TCP_SHELL_SERVER_H */
