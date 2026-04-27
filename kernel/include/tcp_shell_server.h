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
 * Each successful accept snapshots `lwip_stats.mem.used` onto the
 * session struct; the session task computes the delta on teardown
 * and accumulates suspicious overshoot (delta > leak_threshold) into
 * `total_suspicious_leak_bytes` + `leak_warnings`. Use this with
 * the watchdog `heap_used_peak` to correlate session churn with
 * heap pressure.
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
 * Note a session-open event. Called from `on_accept` *after* the
 * session task is successfully spawned and the session struct is
 * fully wired up. Snapshots the heap onto sess->heap_used_at_open_bytes
 * and bumps the `sessions_opened` / `active` / `peak_active` counters.
 * Internal — exposed in the header only so the lwIP raw callback in
 * tcp_shell_server.c can call it without going through public API.
 */
struct shell_session;
void tcp_shell_server_note_session_open(struct shell_session *sess);

/*
 * Note a session-close event. Called from the session task right
 * before `task_exit` (after `shell_io_tcp_destroy` has marked the
 * io as closed). Computes the heap delta vs the open-time snapshot,
 * updates the rolling stats, and emits a WARN if the delta exceeds
 * NET_SHELL_TCP_LEAK_THRESHOLD_BYTES.
 */
void tcp_shell_server_note_session_close(const struct shell_session *sess);

#endif /* TCP_SHELL_SERVER_H */
