/*
 * tcp_shell_server.c - TCP listener + per-connection session task
 *
 * See tcp_shell_server.h for the design. This file owns the
 * listening pcb and the accept callback; shell_io_tcp.c owns the
 * per-session I/O ring buffers; shell_session.c owns the session
 * pool; shell.c owns the REPL.
 */

#include "tcp_shell_server.h"
#include "shell.h"
#include "shell_io.h"
#include "shell_io_tcp.h"
#include "shell_session.h"
#include "task.h"
#include "sched.h"
#include "uart.h"
#include "string.h"
#include "debug.h"

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/stats.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

static struct tcp_pcb *listen_pcb   = NULL;
static uint16_t         listen_port = 0;
static uint32_t         accepted_count = 0;

/* -------------------------------------------------------------------------- */
/* Session lifecycle stats + leak detection                                   */
/* -------------------------------------------------------------------------- */

/* Per-session leak threshold. Sessions normally retain a bit of
 * heap state until the pcb fully tears down (TIME_WAIT footprint
 * is ~120 bytes); this threshold is set well above that so a clean
 * close never trips the WARN. A real TX-COPY-buffer leak or pbuf
 * reference-count off-by-one will show up as multi-KB deltas. */
#ifndef NET_SHELL_TCP_LEAK_THRESHOLD_BYTES
#define NET_SHELL_TCP_LEAK_THRESHOLD_BYTES  1024u
#endif

static uint32_t sessions_opened          = 0;
static uint32_t sessions_closed          = 0;
static uint32_t peak_active              = 0;
static uint32_t leak_warnings            = 0;
static uint32_t total_suspicious_leak    = 0;
static int32_t  last_session_heap_delta  = 0;
static int32_t  max_session_heap_delta   = 0;

/*
 * Diagnostic snapshot. Each individual field is read with a
 * single-copy-atomic load (naturally aligned uint32_t / int32_t on
 * AArch64 and x86-64), but the snapshot as a whole is not
 * transactional — a reader on a different CPU can observe an
 * intermediate writer state (e.g. `sessions_closed` already
 * incremented but `last_session_heap_delta` not yet updated).
 * Same shape and rationale as `net_watchdog_get`. Acceptable
 * because nothing branches on this snapshot — `netstat` and the
 * M3 telemetry feed are the only consumers.
 */
void tcp_shell_server_get_stats(struct tcp_shell_server_stats *out) {
    if (!out) return;
    out->sessions_opened              = sessions_opened;
    out->sessions_closed              = sessions_closed;
    out->active                       = sessions_opened - sessions_closed;
    out->peak_active                  = peak_active;
    out->leak_warnings                = leak_warnings;
    out->total_suspicious_leak_bytes  = total_suspicious_leak;
    out->last_session_heap_delta_bytes = last_session_heap_delta;
    out->max_session_heap_delta_bytes  = max_session_heap_delta;
}

void tcp_shell_server_note_session_open(uint32_t session_id) {
    (void)session_id;
    sessions_opened++;
    uint32_t active = sessions_opened - sessions_closed;
    if (active > peak_active) peak_active = active;
}

void tcp_shell_server_note_session_close(uint32_t session_id,
                                         int32_t  heap_delta_bytes,
                                         const char *pool_attribution) {
    sessions_closed++;

    last_session_heap_delta = heap_delta_bytes;
    if (heap_delta_bytes > max_session_heap_delta) {
        max_session_heap_delta = heap_delta_bytes;
    }

    if (heap_delta_bytes > (int32_t)NET_SHELL_TCP_LEAK_THRESHOLD_BYTES) {
        leak_warnings++;
        total_suspicious_leak += (uint32_t)heap_delta_bytes;
        const char *attr = (pool_attribution && pool_attribution[0])
            ? pool_attribution : "";
        WARN("shell-tcp: session %u closed with +%d bytes still on lwIP heap "
             "(threshold=%u, measured post-tcp_close) — suspect leak%s",
             (unsigned)session_id, (int)heap_delta_bytes,
             (unsigned)NET_SHELL_TCP_LEAK_THRESHOLD_BYTES, attr);
    } else if (pool_attribution && pool_attribution[0]) {
        /* Heap is clean but at least one MEMP pool ended the session
         * with a non-zero delta. Heap-only leak detection misses pool
         * leaks (e.g. PBUF_POOL slots not returned), so surface them
         * here. INFO level — sessions don't always close with all
         * pools at exactly zero in flight (tx unacked may still be
         * in-flight at close-settle measurement time), but a
         * persistent positive PBUF_POOL delta across multiple
         * sessions is the smoking gun for the #581 follow-up
         * pool-leak investigation. */
        /* Use %d (not %+d) — kprintf only supports the `-` and `0`
         * flags (see kprintf.c top-of-file docs). %+d falls into the
         * default case which prints "%+d" literally and does NOT
         * consume the va_arg, so the next %s reads heap_delta_bytes
         * as a char* and prints garbage from wherever that integer
         * happens to point. Negative values still get a leading `-`
         * from %d. */
        INFO("shell-tcp: session %u closed: heap_delta=%d, pool_delta=%s",
             (unsigned)session_id, (int)heap_delta_bytes, pool_attribution);
    }
}

/* -------------------------------------------------------------------------- */
/* Session task body                                                          */
/* -------------------------------------------------------------------------- */

static void session_task_entry(void *arg)
{
    struct shell_session *sess = (struct shell_session *)arg;
    if (!sess) {
        task_exit();
        return;   /* task_exit isn't marked noreturn; make control flow explicit */
    }

    shell_session_bind(task_current(), sess);

    /* Banner — goes over TCP via the bound session's io. */
    shell_puts("\r\n");
    shell_puts("SLM-OS Debug Shell (tcp)\r\n");
    shell_puts("Type 'help' for available commands.\r\n");
    shell_puts("\r\n");

    /* Run the normal REPL. Returns when the peer disconnects (read
     * returns -1) or when the session's io says closed. */
    shell_run();

    /* Teardown. The shell task is now the only user of sess/io; it
     * is safe to mark everything closed and release the pool slots. */
    shell_puts("\r\nbye\r\n");

    struct shell_io *io = sess->io;
    shell_session_unbind(task_current());
    if (io) {
        io->close(io);   /* signals the TCP backend that we are done */
    }

    /* Note: the leak-detector close hook is *not* called here — it
     * fires from `shell_io_tcp_poll` immediately after `tcp_close(pcb)`
     * + `ctx_free`, which is the only point where lwIP has actually
     * released the unacked-TX state and the measurement reflects a
     * true leak (not TIME_WAIT residue). */

    shell_session_free(sess);

    task_exit();
}

/* -------------------------------------------------------------------------- */
/* Accept callback — runs on net_pump                                         */
/* -------------------------------------------------------------------------- */

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    /* Don't let a single slow client monopolize lwIP's listen backlog.
     * Tell lwIP we've accepted the incoming SYN. */
    tcp_backlog_accepted(newpcb);

    struct shell_session *sess = shell_session_alloc();
    if (!sess) {
        /* Pool exhausted — tell the peer and drop. The 19-byte
         * message is short enough to fit in a single segment. */
        const char *msg = "Too many sessions\r\n";
        (void)tcp_write(newpcb, msg, (uint16_t)strlen(msg),
                        TCP_WRITE_FLAG_COPY);
        tcp_output(newpcb);
        tcp_close(newpcb);
        return ERR_MEM;
    }

    struct shell_io *io = shell_io_tcp_create(newpcb);
    if (!io) {
        shell_session_free(sess);
        tcp_close(newpcb);
        return ERR_MEM;
    }
    sess->io = io;
    /* Wire the session back into the TCP backend so telnet
     * subnegotiation can update window_cols / term_type and
     * IAC IP can flip interrupt_requested on this session. */
    shell_io_tcp_attach_session(io, sess);

    /* Spawn the session task. Pin to CPU 0 so it shares a CPU with
     * the net_pump task — the spinlock-based ring buffers are only
     * correct when net_pump's tcp_recv callback and the session
     * task's read_char both IRQ-disable on the same CPU (see
     * shell_io_tcp.c for the discussion). */
    char name[TASK_NAME_LEN];
    uart_snprintf(name, sizeof(name), "shell-tcp%u", sess->id);
    struct task *t = task_create_with_priority(name, session_task_entry,
                                               sess, TASK_PRIORITY_IDLE);
    if (!t) {
        /* Task allocation failed — graceful path mirrors pool
         * exhaustion. */
        shell_io_tcp_destroy(io);
        shell_session_free(sess);
        /* shell_io_tcp_destroy will let the next net_poll close the
         * pcb and free its ctx; don't tcp_close here. */
        return ERR_MEM;
    }
    task_set_affinity(t, 0);

    /* Bump the open counter. The heap-at-open snapshot is owned by
     * shell_io_tcp_create (it lives on tcp_shell_ctx so it can be
     * read at ctx_free time, after tcp_close has actually run). */
    tcp_shell_server_note_session_open(sess->id);

    scheduler_add_task(t);

    accepted_count++;
    return ERR_OK;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/*
 * NOTE: lwIP raw API calls here (tcp_new, tcp_bind, tcp_listen_with_backlog,
 * tcp_accept, tcp_close) run on the shell task, not on net_pump.
 * This follows the existing pattern used by net_init() from cmd_net:
 * the shell and net_pump are both pinned to CPU 0 and both priority
 * IDLE, so under SLM-OS's mostly-cooperative scheduling they do not
 * reach true concurrency on this CPU. A timer preemption mid-call is
 * theoretically possible (and is a known pre-existing risk with
 * net_init too) — Phase 3's `NET_TELNETD_AUTOSTART` path should
 * migrate this onto a net_pump-driven init hook.
 */
int tcp_shell_server_start(uint16_t port)
{
    if (listen_pcb) {
        return (listen_port == port) ? 0 : -1;
    }
    if (port == 0) {
        port = 2323;
    }

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        return -1;
    }

    ip_addr_t any = { 0 };
    IP4_ADDR(&any, 0, 0, 0, 0);  /* bind on all interfaces (QEMU is behind
                                  * host-side port forwarding anyway). */

    err_t err = tcp_bind(pcb, &any, port);
    if (err != ERR_OK) {
        tcp_close(pcb);
        return -2;
    }

    struct tcp_pcb *lpcb = tcp_listen_with_backlog(pcb, MAX_TCP_SHELL_SESSIONS);
    if (!lpcb) {
        tcp_close(pcb);
        return -3;
    }

    tcp_accept(lpcb, on_accept);
    listen_pcb  = lpcb;
    listen_port = port;

    uart_printf("[TCPSH] Listening on 0.0.0.0:%u (unauthenticated — trusted networks only)\r\n",
                (unsigned)port);
    return 0;
}

void tcp_shell_server_stop(void)
{
    if (!listen_pcb) {
        return;
    }
    tcp_close(listen_pcb);
    listen_pcb  = NULL;
    listen_port = 0;
}

bool     tcp_shell_server_running(void)  { return listen_pcb != NULL; }
uint16_t tcp_shell_server_port(void)     { return listen_port; }
uint32_t tcp_shell_server_accepted(void) { return accepted_count; }
