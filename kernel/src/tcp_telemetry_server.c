/*
 * tcp_telemetry_server.c — Network bridge for the admin telemetry feed.
 *
 * See tcp_telemetry_server.h for the design + wire protocol. Pattern is
 * deliberately close to kernel/src/tcp_shell_server.c (lifecycle) and
 * kernel/src/shell_io_tcp.c (per-client TX ring + drop-oldest), with
 * three things removed because the server is push-only:
 *
 *   - No telnet IAC parser (subscribers are machine consumers).
 *   - No per-session task — the line-edit REPL doesn't apply.
 *   - No CRLF normalization — every record we emit ends with a single
 *     '\n' the consumer can split on.
 *
 * The bridge is itself a msg_router subscriber on `tel.*`, drained from
 * net_pump context (tcp_telemetry_server_poll) so the publisher's
 * mailbox never fills. Slow clients drop oldest from their per-client
 * TX ring; the server NEVER stalls msg_router_publish (which would
 * propagate up to ACK_TIMEOUT_SECS=5 stalls into the eviction allocator).
 */

#include "tcp_telemetry_server.h"

#include "spinlock.h"
#include "string.h"
#include "uart.h"

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "arch/sys_arch.h"   /* sys_now() */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * msg_router FFI (Rust). Function-only externs to keep the include graph
 * small — same pattern admin_telemetry.c uses.
 * ============================================================================ */
extern int  msg_router_subscribe(const uint8_t *topic_name, int component_idx);
extern void msg_router_unsubscribe_all(int component_idx);
extern const char *msg_router_receive(int component_idx, char *topic_out);
extern void msg_router_ack(int component_idx);

/* Component slot. The msg_router's MAX_COMPONENTS=64 is split today as:
 *   0..15:  COMPONENT_MAX_COUNT — native components (component.c)
 *   16..48: LUA_MSG_COMPONENT_BASE..MAX (16 + 1 + 16 = 33 slots; lua_slm.c)
 *   49..63: free
 * We park telemetryd at 49. If the Lua range grows past 48 the
 * compile-time guard in lua_slm.c (LUA_MSG_COMPONENT_MAX <= 64) fires
 * and forces a coordinated bump of MAX_COMPONENTS in msg_router.rs;
 * this file's only constraint is that the chosen slot is unique. */
#define TELEMETRY_COMPONENT_IDX 49

/* Topic-name buffer matches msg_router's TOPIC_NAME_LEN. */
#define TELEMETRY_TOPIC_LEN     16

/* Scratch buffer used to format one sample line before fanout. Sized to
 * comfortably hold "<topic> seq=<20> ts=<10> <60-byte payload>\n\0".
 * msg_router caps payload at MAX_MSG_LEN=60 incl. nul, so 16 + 6 + 20 +
 * 4 + 10 + 1 + 60 + 2 ≈ 119 bytes; round up. */
#define TELEMETRY_LINE_MAX      192

/* Per-client TX ring. 4 KB matches shell_io_tcp's TCP_SHELL_RING_SIZE
 * for the same reasons (fits one MTU's worth of pending bytes plus
 * headroom). Must be a power of two. */
#define TX_RING_SIZE            4096
#define TX_RING_MASK            (TX_RING_SIZE - 1)
_Static_assert((TX_RING_SIZE & TX_RING_MASK) == 0,
               "TX_RING_SIZE must be a power of two");

/* Inbound (client → server) line buffer. We only accept tiny commands:
 * `SUB <pattern>\n` and `BYE\n`. 32 bytes is enough for "SUB " + 16-char
 * pattern + slack. Anything larger is rejected with an "unknown" reply. */
#define RX_LINE_MAX             32

/* ============================================================================
 * Per-session state
 * ============================================================================ */

struct tel_session {
    bool              in_use;
    bool              synthetic;        /* true when used by the unit-test seam */
    volatile bool     closed;

    struct tcp_pcb   *pcb;              /* NULL for synthetic sessions */
    uint32_t          session_id;
    uint32_t          peer_ip;
    uint16_t          peer_port;
    uint32_t          connected_at_ms;

    /* Server-side glob filter. Prefix-match: a trailing '*' wildcards.
     * Empty pattern matches nothing; default "tel.*" set on accept. */
    char              filter[TELEMETRY_FILTER_LEN];

    /* TX ring (server → peer). Producer = poll fanout path; consumer =
     * poll's lwIP drain (or the test drain helper). Both run on the
     * same task (net_pump) so no spinlock is needed; the volatile
     * `closed` flag is the only cross-context bit. */
    uint8_t           tx_buf[TX_RING_SIZE];
    uint32_t          tx_head;          /* next write index */
    uint32_t          tx_tail;          /* next read index */

    /* RX line buffer (peer → server) — accumulates until '\n'. */
    char              rx_line[RX_LINE_MAX];
    uint16_t          rx_len;
    bool              rx_overflow;      /* drop input until next '\n' */

    /* Counters surfaced via tcp_telemetry_server_foreach(). */
    uint64_t          samples_sent;
    uint64_t          bytes_sent;
    uint64_t          drops;
};

/* ============================================================================
 * Module state
 * ============================================================================ */

static struct tcp_pcb       *g_listen_pcb;
static uint16_t              g_listen_port;
static struct tel_session    g_sessions[MAX_TELEMETRY_SESSIONS];
static uint32_t              g_next_session_id = 1;
static uint64_t              g_seq;

/* Aggregate counters — read by tcp_telemetry_server_get_stats. */
static uint32_t g_sessions_opened;
static uint32_t g_sessions_closed;
static uint64_t g_samples_dequeued;
static uint64_t g_samples_delivered;
static uint64_t g_samples_dropped;
static uint64_t g_accept_rejects;

/* ============================================================================
 * Helpers
 * ============================================================================ */

static size_t tel_strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void tel_strcpy_bounded(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Format `value` as decimal into `buf[*pos..cap]`. Returns 0 on success,
 * -1 if it would overflow. Leaves room for caller to append more. */
static int tel_append_uint(char *buf, size_t cap, size_t *pos, uint64_t value)
{
    char digits[24];
    int dlen = 0;
    if (value == 0) {
        digits[dlen++] = '0';
    } else {
        while (value > 0) {
            digits[dlen++] = (char)('0' + (value % 10ull));
            value /= 10ull;
        }
    }
    if (*pos + (size_t)dlen > cap) return -1;
    while (dlen > 0) buf[(*pos)++] = digits[--dlen];
    return 0;
}

/* Append a literal string. Returns 0 / -1 like tel_append_uint. The
 * buffer is not NUL-terminated by these helpers — callers append the
 * trailing '\n' themselves and the framed length goes into tx_enqueue
 * verbatim — so the guard checks for one byte of write room only. */
static int tel_append_str(char *buf, size_t cap, size_t *pos, const char *s)
{
    while (s && *s) {
        if (*pos >= cap) return -1;
        buf[(*pos)++] = *s++;
    }
    return 0;
}

/* Prefix-match against the per-client filter. Mirrors msg_router's
 * wildcard semantics: pattern ending in '*' matches any topic that
 * begins with the prefix. An exact pattern (no '*') requires equality. */
static bool topic_matches_filter(const char *filter, const char *topic)
{
    if (!filter || !*filter) return false;
    /* Find the position of '*' (if any) within the filter. */
    size_t plen = 0;
    while (filter[plen] && filter[plen] != '*') plen++;
    bool wildcard = (filter[plen] == '*');

    /* Compare prefix bytes. */
    for (size_t i = 0; i < plen; i++) {
        if (filter[i] != topic[i]) return false;
        if (topic[i] == '\0') return false;     /* topic shorter than prefix */
    }
    if (wildcard) return true;
    /* Exact match — topic must terminate at plen. */
    return topic[plen] == '\0';
}

/* ============================================================================
 * Per-client TX ring
 *
 * Producer / consumer both run on net_pump (poll), so no locking is
 * required between them. The volatile `closed` bit handles the only
 * cross-context interaction (an lwIP error callback marking the
 * session dead from the net_pump tcp_err callback, also on net_pump).
 * ============================================================================ */

static uint32_t tx_used(const struct tel_session *s)
{
    return (s->tx_head - s->tx_tail) & TX_RING_MASK;
}

static uint32_t tx_free(const struct tel_session *s)
{
    return TX_RING_MASK - tx_used(s);
}

/* Drop oldest bytes from the head of the ring until at least `need` bytes
 * are free. Producer uses this when the ring is full. Drops in
 * line-sized chunks (advances tail to next '\n' + 1) so the consumer
 * never reads a truncated line.
 *
 * Returns the number of complete sample lines evicted. A "no newline
 * found" branch (the ring contains a single partial line — never reached
 * today because every byte the producer writes is part of a full
 * '\n'-terminated sample line, but defended for the future) counts as
 * one drop and resets the ring. */
static uint32_t tx_drop_oldest_for(struct tel_session *s, uint32_t need)
{
    uint32_t dropped = 0;
    while (tx_free(s) < need) {
        if (s->tx_tail == s->tx_head) break;   /* empty — should not happen */
        bool found = false;
        for (uint32_t walked = 0; walked < tx_used(s); walked++) {
            uint32_t idx = (s->tx_tail + walked) & TX_RING_MASK;
            if (s->tx_buf[idx] == '\n') {
                s->tx_tail = (s->tx_tail + walked + 1) & TX_RING_MASK;
                found = true;
                dropped++;
                break;
            }
        }
        if (!found) {
            /* No newline in the ring — partial line stuck. Reset and
             * count it so the operator-visible drop counter doesn't
             * silently lose this branch. */
            s->tx_tail = s->tx_head;
            dropped++;
            break;
        }
    }
    return dropped;
}

/* Queue `len` bytes into the ring. Drops oldest line(s) on overflow,
 * incrementing both `s->drops` and `g_samples_dropped` by the actual
 * number of evicted lines (not once-per-enqueue) so the operator-visible
 * counters match reality under sustained back-pressure. Returns true
 * on success; a `false` return means the line itself exceeds the ring
 * capacity (statically prevented by TELEMETRY_LINE_MAX < TX_RING_SIZE). */
static bool tx_enqueue(struct tel_session *s, const uint8_t *buf, uint32_t len)
{
    if (len > TX_RING_SIZE - 1u) return false;
    if (tx_free(s) < len) {
        uint32_t evicted = tx_drop_oldest_for(s, len);
        s->drops          += evicted;
        g_samples_dropped += evicted;
    }
    if (tx_free(s) < len) return false;
    for (uint32_t i = 0; i < len; i++) {
        s->tx_buf[(s->tx_head + i) & TX_RING_MASK] = buf[i];
    }
    s->tx_head = (s->tx_head + len) & TX_RING_MASK;
    s->bytes_sent += len;
    return true;
}

/* ============================================================================
 * Session pool
 * ============================================================================ */

static struct tel_session *session_alloc(void)
{
    for (uint32_t i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        struct tel_session *s = &g_sessions[i];
        if (!s->in_use) {
            /* Wipe everything on alloc so a stale ring from a previous
             * occupant cannot leak across sessions. */
            memset(s, 0, sizeof(*s));
            s->in_use = true;
            tel_strcpy_bounded(s->filter, "tel.*", sizeof(s->filter));
            return s;
        }
    }
    return NULL;
}

static void session_free(struct tel_session *s)
{
    if (!s) return;
    s->in_use = false;
    s->closed = false;
    s->pcb = NULL;
}

/* ============================================================================
 * Inbound command parsing (peer → server)
 * ============================================================================ */

static void session_emit_comment(struct tel_session *s, const char *line)
{
    /* Format: "# <line>\n". Use the same TX ring as samples. */
    char buf[64];
    size_t pos = 0;
    if (tel_append_str(buf, sizeof(buf), &pos, "# ") != 0) return;
    if (tel_append_str(buf, sizeof(buf), &pos, line) != 0) return;
    if (pos + 2u > sizeof(buf)) return;
    buf[pos++] = '\n';
    (void)tx_enqueue(s, (const uint8_t *)buf, (uint32_t)pos);
}

static void session_emit_banner(struct tel_session *s)
{
    static const char banner[] =
        "# SLM-OS telemetryd v1\n"
        "# subscribe with: SUB <pattern>\n"
        "# default filter: tel.*\n";
    (void)tx_enqueue(s, (const uint8_t *)banner, (uint32_t)sizeof(banner) - 1u);
}

static void session_handle_command(struct tel_session *s)
{
    if (s->rx_overflow) {
        session_emit_comment(s, "unknown: <line too long>");
        return;
    }
    /* rx_line is NUL-terminated. Strip CR if present. */
    if (s->rx_len > 0 && s->rx_line[s->rx_len - 1] == '\r') {
        s->rx_line[--s->rx_len] = '\0';
    }
    if (s->rx_len == 0) return;   /* blank line — ignore */

    /* SUB <pattern> */
    if (s->rx_len >= 4 &&
        s->rx_line[0] == 'S' && s->rx_line[1] == 'U' &&
        s->rx_line[2] == 'B' && s->rx_line[3] == ' ') {
        const char *pat = &s->rx_line[4];
        /* Skip any extra spaces. */
        while (*pat == ' ') pat++;
        if (*pat == '\0') {
            session_emit_comment(s, "unknown: SUB needs a pattern");
            return;
        }
        tel_strcpy_bounded(s->filter, pat, sizeof(s->filter));
        char ack[48];
        size_t pos = 0;
        (void)tel_append_str(ack, sizeof(ack), &pos, "filter=");
        (void)tel_append_str(ack, sizeof(ack), &pos, s->filter);
        ack[pos < sizeof(ack) ? pos : sizeof(ack) - 1] = '\0';
        session_emit_comment(s, ack);
        return;
    }

    if (s->rx_len == 3 &&
        s->rx_line[0] == 'B' && s->rx_line[1] == 'Y' && s->rx_line[2] == 'E') {
        s->closed = true;
        return;
    }

    /* Anything else: echo as a comment so a stray byte doesn't kill the
     * session. Keeps the output stream parseable for the consumer. */
    char msg[RX_LINE_MAX + 16];
    size_t pos = 0;
    (void)tel_append_str(msg, sizeof(msg), &pos, "unknown: ");
    (void)tel_append_str(msg, sizeof(msg), &pos, s->rx_line);
    msg[pos < sizeof(msg) ? pos : sizeof(msg) - 1] = '\0';
    session_emit_comment(s, msg);
}

static void session_feed_input(struct tel_session *s, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == '\n') {
            s->rx_line[s->rx_len < RX_LINE_MAX - 1 ? s->rx_len : RX_LINE_MAX - 1] = '\0';
            session_handle_command(s);
            s->rx_len = 0;
            s->rx_overflow = false;
            continue;
        }
        if (s->rx_overflow) continue;
        if (s->rx_len + 1u >= RX_LINE_MAX) {
            s->rx_overflow = true;
            continue;
        }
        s->rx_line[s->rx_len++] = (char)b;
    }
}

/* ============================================================================
 * lwIP callbacks
 * ============================================================================ */

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct tel_session *s = (struct tel_session *)arg;
    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        if (s) s->closed = true;
        return ERR_OK;
    }
    if (p == NULL) {
        if (s) s->closed = true;
        return ERR_OK;
    }
    if (s) {
        struct pbuf *q = p;
        while (q) {
            session_feed_input(s, (const uint8_t *)q->payload, q->len);
            q = q->next;
        }
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)err;
    struct tel_session *s = (struct tel_session *)arg;
    if (!s) return;
    s->pcb = NULL;        /* lwIP has freed the pcb */
    s->closed = true;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }
    tcp_backlog_accepted(newpcb);

    struct tel_session *s = session_alloc();
    if (!s) {
        g_accept_rejects++;
        const char *msg = "# server full\n";
        (void)tcp_write(newpcb, msg, (uint16_t)tel_strlen(msg), TCP_WRITE_FLAG_COPY);
        tcp_output(newpcb);
        tcp_close(newpcb);
        return ERR_MEM;
    }

    s->pcb              = newpcb;
    s->session_id       = g_next_session_id++;
    s->peer_ip          = newpcb->remote_ip.addr;
    s->peer_port        = newpcb->remote_port;
    s->connected_at_ms  = sys_now();

    tcp_arg(newpcb, s);
    tcp_recv(newpcb, on_recv);
    tcp_err(newpcb, on_err);

    session_emit_banner(s);
    g_sessions_opened++;
    return ERR_OK;
}

/* ============================================================================
 * Per-tick poll: msg_router drain → fanout → lwIP TX drain
 * ============================================================================ */

/* Format one sample into `out`. Returns the number of bytes written
 * (excluding the NUL), or 0 if the topic / payload was malformed. */
static size_t format_sample(char *out, size_t cap,
                            const char *topic, const char *payload, uint64_t seq,
                            uint32_t ts_ms)
{
    size_t pos = 0;
    if (tel_append_str(out, cap, &pos, topic) != 0) return 0;
    if (tel_append_str(out, cap, &pos, " seq=") != 0) return 0;
    if (tel_append_uint(out, cap, &pos, seq) != 0) return 0;
    if (tel_append_str(out, cap, &pos, " ts=") != 0) return 0;
    if (tel_append_uint(out, cap, &pos, (uint64_t)ts_ms) != 0) return 0;
    if (tel_append_str(out, cap, &pos, " ") != 0) return 0;
    if (tel_append_str(out, cap, &pos, payload) != 0) return 0;
    if (pos >= cap) return 0;
    out[pos++] = '\n';
    return pos;
}

/* Walk the pool, queue the formatted line into every active session
 * whose filter matches. Returns the number of clients that received it. */
static uint32_t fanout_sample(const char *topic,
                              const uint8_t *line, uint32_t line_len)
{
    uint32_t delivered = 0;
    for (uint32_t i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        struct tel_session *s = &g_sessions[i];
        if (!s->in_use || s->closed) continue;
        if (!topic_matches_filter(s->filter, topic)) continue;
        if (tx_enqueue(s, line, line_len)) {
            s->samples_sent++;
            delivered++;
        }
    }
    return delivered;
}

static void drain_to_lwip(struct tel_session *s)
{
    if (!s->pcb) return;
    while (tx_used(s) > 0) {
        uint16_t sndbuf = tcp_sndbuf(s->pcb);
        if (sndbuf == 0) break;
        uint32_t avail = tx_used(s);
        uint32_t chunk = (avail < sndbuf) ? avail : sndbuf;
        uint32_t tail_idx  = s->tx_tail & TX_RING_MASK;
        uint32_t contiguous = TX_RING_SIZE - tail_idx;
        if (chunk > contiguous) chunk = contiguous;
        err_t werr = tcp_write(s->pcb, &s->tx_buf[tail_idx],
                               (uint16_t)chunk, TCP_WRITE_FLAG_COPY);
        if (werr != ERR_OK) break;
        s->tx_tail = (s->tx_tail + chunk) & TX_RING_MASK;
    }
    if (s->pcb) tcp_output(s->pcb);
}

void tcp_telemetry_server_poll(void)
{
    /* When the listener is not running there is nothing to drain — tests
     * exercise the fanout path directly via tcp_telemetry_server_test_inject
     * and do not depend on this tick. */
    if (!g_listen_pcb) return;

    /* Step 1: drain the msg_router subscription. */
    char topic_buf[TELEMETRY_TOPIC_LEN];
    for (;;) {
        const char *payload = msg_router_receive(TELEMETRY_COMPONENT_IDX,
                                                 topic_buf);
        if (!payload) break;
        g_samples_dequeued++;

        uint8_t line[TELEMETRY_LINE_MAX];
        uint32_t line_len = (uint32_t)format_sample((char *)line, sizeof(line),
                                                   topic_buf, payload,
                                                   ++g_seq, sys_now());
        msg_router_ack(TELEMETRY_COMPONENT_IDX);
        if (line_len == 0) continue;

        uint32_t delivered = fanout_sample(topic_buf, line, line_len);
        if (delivered > 0) g_samples_delivered++;
    }

    /* Step 2: drain client TX rings → lwIP, run lifecycle teardown. */
    for (uint32_t i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        struct tel_session *s = &g_sessions[i];
        if (!s->in_use) continue;
        if (s->synthetic) continue;        /* tests handle their own drain */
        if (s->pcb) drain_to_lwip(s);

        if (s->closed && s->pcb) {
            /* Drain remaining ring before closing; if it can't all go
             * out we still close — TCP RST/FIN is acceptable for a
             * peer that triggered the close (BYE) or already left. */
            tcp_arg(s->pcb,  NULL);
            tcp_recv(s->pcb, NULL);
            tcp_err(s->pcb,  NULL);
            (void)tcp_close(s->pcb);
            s->pcb = NULL;
        }
        if (s->closed && s->pcb == NULL) {
            session_free(s);
            g_sessions_closed++;
        }
    }
}

/* ============================================================================
 * Listener lifecycle
 *
 * NOTE: lwIP raw API calls here (tcp_new, tcp_bind, tcp_listen_with_backlog,
 * tcp_accept, tcp_close) run on the shell task when the operator types
 * `telemetry server start` or invokes `slm.telemetryd_start`, *not* on
 * net_pump. This mirrors the existing pattern in tcp_shell_server.c
 * (see its NOTE) and inherits the same pre-existing risk: under SLM-OS's
 * mostly-cooperative scheduling on CPU 0 + IDLE-priority pinning, the
 * shell task and net_pump don't reach true concurrency on this CPU. A
 * timer preemption mid-call is theoretically possible. The
 * NET_TELEMETRYD_AUTOSTART path runs from shell_init (also shell task)
 * and shares the same caveat. A future cleanup could migrate both
 * listener brings-up onto a net_pump-driven init hook.
 * ============================================================================ */

int tcp_telemetry_server_start(uint16_t port)
{
    if (g_listen_pcb) {
        return (g_listen_port == port) ? 0 : -1;
    }
    if (port == 0) port = TCP_TELEMETRY_DEFAULT_PORT;

    if (msg_router_subscribe((const uint8_t *)"tel.*",
                             TELEMETRY_COMPONENT_IDX) != 0) {
        return -2;
    }

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        msg_router_unsubscribe_all(TELEMETRY_COMPONENT_IDX);
        return -3;
    }
    ip_addr_t any = { 0 };
    IP4_ADDR(&any, 0, 0, 0, 0);
    err_t err = tcp_bind(pcb, &any, port);
    if (err != ERR_OK) {
        tcp_close(pcb);
        msg_router_unsubscribe_all(TELEMETRY_COMPONENT_IDX);
        return -4;
    }
    struct tcp_pcb *lpcb = tcp_listen_with_backlog(pcb, MAX_TELEMETRY_SESSIONS);
    if (!lpcb) {
        tcp_close(pcb);
        msg_router_unsubscribe_all(TELEMETRY_COMPONENT_IDX);
        return -5;
    }
    tcp_accept(lpcb, on_accept);
    g_listen_pcb  = lpcb;
    g_listen_port = port;
    uart_printf("[TELD] Listening on 0.0.0.0:%u (telemetry feed)\r\n",
                (unsigned)port);
    return 0;
}

void tcp_telemetry_server_stop(void)
{
    if (!g_listen_pcb) return;
    tcp_close(g_listen_pcb);
    g_listen_pcb  = NULL;
    g_listen_port = 0;

    /* Mark every active session as closed; the next poll will tear
     * them down via lwIP. */
    for (uint32_t i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        if (g_sessions[i].in_use && !g_sessions[i].synthetic) {
            g_sessions[i].closed = true;
        }
    }
    msg_router_unsubscribe_all(TELEMETRY_COMPONENT_IDX);
}

bool     tcp_telemetry_server_running(void) { return g_listen_pcb != NULL; }
uint16_t tcp_telemetry_server_port(void)    { return g_listen_port; }

void tcp_telemetry_server_autostart(void)
{
#if defined(NET_TELEMETRYD_AUTOSTART)
    (void)tcp_telemetry_server_start(0);
#endif
}

/* ============================================================================
 * Stats / introspection
 * ============================================================================ */

void tcp_telemetry_server_get_stats(struct tcp_telemetry_server_stats *out)
{
    if (!out) return;
    uint32_t active = 0;
    for (uint32_t i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        if (g_sessions[i].in_use && !g_sessions[i].synthetic) active++;
    }
    out->port              = g_listen_port;
    out->listening         = (g_listen_pcb != NULL);
    out->sessions_active   = active;
    out->sessions_opened   = g_sessions_opened;
    out->sessions_closed   = g_sessions_closed;
    out->samples_dequeued  = g_samples_dequeued;
    out->samples_delivered = g_samples_delivered;
    out->samples_dropped   = g_samples_dropped;
    out->accept_rejects    = g_accept_rejects;
}

void tcp_telemetry_server_foreach(tcp_telemetry_session_visitor_t visitor,
                                  void *user_ctx)
{
    if (!visitor) return;
    for (uint32_t i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        struct tel_session *s = &g_sessions[i];
        if (!s->in_use || s->synthetic) continue;
        struct tcp_telemetry_session_info info = {
            .session_id      = s->session_id,
            .peer_ip         = s->peer_ip,
            .peer_port       = s->peer_port,
            .connected_at_ms = s->connected_at_ms,
            .samples_sent    = s->samples_sent,
            .bytes_sent      = s->bytes_sent,
            .drops           = s->drops,
        };
        tel_strcpy_bounded(info.filter, s->filter, sizeof(info.filter));
        if (!visitor(&info, user_ctx)) return;
    }
}

bool tcp_telemetry_server_kick(uint32_t session_id)
{
    for (uint32_t i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        struct tel_session *s = &g_sessions[i];
        if (s->in_use && !s->synthetic && s->session_id == session_id) {
            s->closed = true;
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * Test seam
 *
 * Synthetic sessions live alongside real ones in the same pool but
 * never touch lwIP. They expose the fanout + ring + filter + command
 * paths to the unit tests with no QEMU networking required.
 * ============================================================================ */

int tcp_telemetry_server_test_open_session(const char *filter)
{
    struct tel_session *s = session_alloc();
    if (!s) return -1;
    s->synthetic       = true;
    s->session_id      = g_next_session_id++;
    s->connected_at_ms = sys_now();
    if (filter && *filter) {
        tel_strcpy_bounded(s->filter, filter, sizeof(s->filter));
    }
    /* Mirror the on_accept path: real sessions receive the banner as
     * the first thing on the wire. Synthetic ones must too, otherwise
     * the unit tests would miss a banner-format regression. */
    session_emit_banner(s);
    /* Find the slot index. */
    for (uint32_t i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        if (&g_sessions[i] == s) return (int)i;
    }
    return -1;
}

size_t tcp_telemetry_server_test_drain(int slot, char *out, size_t out_cap)
{
    if (slot < 0 || (uint32_t)slot >= MAX_TELEMETRY_SESSIONS) return 0;
    struct tel_session *s = &g_sessions[slot];
    if (!s->in_use || !s->synthetic) return 0;
    size_t n = 0;
    while (tx_used(s) > 0 && n + 1u < out_cap) {
        out[n++] = (char)s->tx_buf[s->tx_tail & TX_RING_MASK];
        s->tx_tail = (s->tx_tail + 1) & TX_RING_MASK;
    }
    if (out && out_cap > 0) out[n] = '\0';
    return n;
}

uint32_t tcp_telemetry_server_test_inject(const char *topic,
                                          const char *payload)
{
    if (!topic || !payload) return 0;
    g_samples_dequeued++;
    uint8_t line[TELEMETRY_LINE_MAX];
    uint32_t line_len = (uint32_t)format_sample((char *)line, sizeof(line),
                                                topic, payload,
                                                ++g_seq, sys_now());
    if (line_len == 0) return 0;
    uint32_t delivered = fanout_sample(topic, line, line_len);
    if (delivered > 0) g_samples_delivered++;
    return delivered;
}

void tcp_telemetry_server_test_feed_input(int slot, const char *bytes, size_t len)
{
    if (slot < 0 || (uint32_t)slot >= MAX_TELEMETRY_SESSIONS) return;
    struct tel_session *s = &g_sessions[slot];
    if (!s->in_use || !s->synthetic || !bytes) return;
    session_feed_input(s, (const uint8_t *)bytes, len);
}

const char *tcp_telemetry_server_test_session_filter(int slot)
{
    if (slot < 0 || (uint32_t)slot >= MAX_TELEMETRY_SESSIONS) return NULL;
    struct tel_session *s = &g_sessions[slot];
    if (!s->in_use || !s->synthetic) return NULL;
    return s->filter;
}

bool tcp_telemetry_server_test_session_closed(int slot)
{
    if (slot < 0 || (uint32_t)slot >= MAX_TELEMETRY_SESSIONS) return false;
    struct tel_session *s = &g_sessions[slot];
    if (!s->in_use || !s->synthetic) return false;
    return s->closed;
}

void tcp_telemetry_server_test_reset(void)
{
    for (uint32_t i = 0; i < MAX_TELEMETRY_SESSIONS; i++) {
        if (g_sessions[i].synthetic) {
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
        }
    }
    g_seq = 0;
    g_samples_dequeued  = 0;
    g_samples_delivered = 0;
    g_samples_dropped   = 0;
}
