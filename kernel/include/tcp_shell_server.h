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

#endif /* TCP_SHELL_SERVER_H */
