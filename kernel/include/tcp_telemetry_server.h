/*
 * tcp_telemetry_server.h — Network bridge for the admin telemetry feed.
 *
 * Bridges the in-process `tel.*` msg_router topics to TCP so a host-side
 * monitor (Python script, `nc`, dashboard backend) can subscribe over
 * the network without speaking telnet shell semantics.
 *
 * Server architecture:
 *   - One static lwIP listen pcb on TCP/2325 (default).
 *   - One msg_router subscription on `tel.*` using a fixed
 *     component_idx (TELEMETRY_COMPONENT_IDX in the .c file). Drained
 *     from net_pump context — see tcp_telemetry_server_poll().
 *   - Per-client state in a fixed pool (MAX_TELEMETRY_SESSIONS slots)
 *     with a TX ring and an optional server-side glob filter.
 *   - Slow-client policy: drop-oldest from the per-client ring. The
 *     server NEVER blocks the publisher path — msg_router_publish
 *     stalls (ACK_TIMEOUT_SECS=5) would otherwise propagate to every
 *     allocator-driven eviction.
 *
 * Wire protocol (newline-terminated ASCII):
 *   Banner on accept:
 *     # SLM-OS telemetryd v1\n
 *     # subscribe with: SUB <pattern>\n
 *     # default filter: tel.*\n
 *   Sample line:
 *     <topic> seq=<n> ts=<ms> <payload>\n
 *     e.g. tel.inf seq=12345 ts=98765 dt=87654 ok=1\n
 *   Client commands (one per line):
 *     SUB <pattern>\n       set per-client server-side filter
 *     BYE\n                 graceful close
 *     <other>               echoed back as `# unknown: <input>\n`
 *
 * See docs/specs/admin-telemetry-suite.md "Network feed" for the
 * design rationale.
 */

#ifndef TCP_TELEMETRY_SERVER_H
#define TCP_TELEMETRY_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TCP_TELEMETRY_DEFAULT_PORT  2325
#define MAX_TELEMETRY_SESSIONS      4
#define TELEMETRY_FILTER_LEN        16   /* matches msg_router TOPIC_NAME_LEN */

/* ============================================================================
 * Listener lifecycle (mirrors tcp_shell_server_*).
 * ============================================================================ */

int      tcp_telemetry_server_start(uint16_t port);   /* 0 ⇒ default port */
void     tcp_telemetry_server_stop(void);
bool     tcp_telemetry_server_running(void);
uint16_t tcp_telemetry_server_port(void);

/* Boot-time hook. No-op unless NET_TELEMETRYD_AUTOSTART was defined. */
void tcp_telemetry_server_autostart(void);

/* ============================================================================
 * Periodic pump — invoke from net_pump (added to net_poll() in
 * kernel/net/lwip_slm.c next to shell_io_tcp_poll()). Drains the
 * msg_router subscription, fans out to client TX rings, then drains
 * the rings to lwIP. Cheap no-op when the server is not running.
 * ============================================================================ */
void tcp_telemetry_server_poll(void);

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

struct tcp_telemetry_server_stats {
    uint16_t port;                   /* 0 when not listening */
    bool     listening;
    uint32_t sessions_active;
    uint32_t sessions_opened;
    uint32_t sessions_closed;
    uint64_t samples_dequeued;       /* msg_router → server */
    uint64_t samples_delivered;      /* server → at least one client ring */
    uint64_t samples_dropped;        /* total per-client ring overflows */
    uint64_t accept_rejects;         /* pool exhaustion or alloc failure */
};

void tcp_telemetry_server_get_stats(struct tcp_telemetry_server_stats *out);

struct tcp_telemetry_session_info {
    uint32_t session_id;
    uint32_t peer_ip;                /* network byte order */
    uint16_t peer_port;
    uint32_t connected_at_ms;        /* sys_now() at accept */
    uint64_t samples_sent;
    uint64_t bytes_sent;
    uint64_t drops;
    char     filter[TELEMETRY_FILTER_LEN];   /* "tel.*" by default */
};

typedef bool (*tcp_telemetry_session_visitor_t)(
    const struct tcp_telemetry_session_info *info, void *user_ctx);

void tcp_telemetry_server_foreach(tcp_telemetry_session_visitor_t visitor,
                                  void *user_ctx);

bool tcp_telemetry_server_kick(uint32_t session_id);

/* ============================================================================
 * Test seam — implementation-internal, exposed only to the unit tests
 * via this header so the suite can drive the server without lwIP.
 *
 * tcp_telemetry_server_test_inject_sample feeds a synthetic
 * (topic, payload) pair as if msg_router had delivered it. Returns
 * the number of clients that received the formatted sample.
 *
 * tcp_telemetry_server_test_open_session installs a fake "session"
 * backed by an in-memory TX sink; tcp_telemetry_server_test_drain
 * returns the bytes that fanout has queued for that session.
 * ============================================================================ */

#ifdef SLM_HOST_HARNESS_OR_TESTS
/* Reserved for a future host harness; the in-tree unit tests use the
 * functions below regardless. */
#endif

/* Open a synthetic session. Returns a non-negative id on success or -1
 * on pool exhaustion. `filter` may be NULL → default "tel.*". The
 * returned id is the per-pool index, NOT the public session_id. */
int tcp_telemetry_server_test_open_session(const char *filter);

/* Drain bytes accumulated for a synthetic session into `out` (truncated
 * to `out_cap`). Returns the number of bytes written. */
size_t tcp_telemetry_server_test_drain(int slot, char *out, size_t out_cap);

/* Hand a synthetic sample to the server's fanout path. Returns the
 * number of synthetic sessions whose ring received the sample. */
uint32_t tcp_telemetry_server_test_inject(const char *topic,
                                          const char *payload);

/* Tear down all synthetic sessions and reset counters. */
void tcp_telemetry_server_test_reset(void);

#endif /* TCP_TELEMETRY_SERVER_H */
