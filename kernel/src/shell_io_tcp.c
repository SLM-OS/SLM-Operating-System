/*
 * shell_io_tcp.c - TCP backend for shell_io
 *
 * Implements the design described in shell_io_tcp.h. Each session
 * owns a tcp_shell_ctx holding the pcb, two ring buffers (RX from
 * peer, TX to peer), and flags. The shell_io vtable is embedded in
 * the ctx and io->ctx points back at it.
 *
 * Lock discipline:
 *   - ctx->rx_lock protects rx_buf + rx_head/rx_tail + closed flag
 *     (writers: tcp_recv cb on net_pump; readers: shell task).
 *   - ctx->tx_lock protects tx_buf + tx_head/tx_tail
 *     (writers: shell task; readers: shell_io_tcp_poll on net_pump).
 *   - The pcb pointer is only touched from net_pump context (accept,
 *     tcp_recv / tcp_err / tcp_sent callbacks, shell_io_tcp_poll).
 *     The shell task never touches the pcb directly — it only
 *     interacts with the rings and the closed flag.
 *
 * Ring buffer: simple power-of-two sized circular buffer with head
 * (next write index) and tail (next read index). Full when
 * ((head + 1) & mask) == tail. Empty when head == tail.
 */

#include "shell_io_tcp.h"
#include "shell_session.h"
#include "spinlock.h"
#include "string.h"
#include "task.h"
#include "tcp_shell_server.h"   /* tcp_shell_server_note_session_close */
#include "telnet.h"
#include "timer.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"        /* lwip_stats.mem.used for the heap snapshot */
#include "arch/sys_arch.h"   /* sys_now() for connected_at timestamp */

/* Per-direction buffer size. Must be a power of two. 4 KB matches
 * Plan §1.7's resource-limit budget. */
#define TCP_SHELL_RING_SIZE 4096
#define TCP_SHELL_RING_MASK (TCP_SHELL_RING_SIZE - 1)

_Static_assert((TCP_SHELL_RING_SIZE & TCP_SHELL_RING_MASK) == 0,
               "TCP_SHELL_RING_SIZE must be a power of two");
/* The drain path passes `chunk` (clamped to ring size) into tcp_write
 * which takes a uint16_t length — guard against a future bump above
 * 64 K silently truncating. */
_Static_assert(TCP_SHELL_RING_SIZE <= 0xFFFF,
               "TCP_SHELL_RING_SIZE must fit in uint16_t for tcp_write()");

/* How long read_char sleeps between empty-ring checks. 10 ms matches
 * the net_pump cadence so we don't sleep longer than one pump cycle. */
#define TCP_SHELL_POLL_INTERVAL_MS 10

/*
 * Settling delay between `tcp_close(pcb)` and the post-close heap
 * measurement (#537). The measurement was previously taken in the
 * same poll cycle as tcp_close, before lwIP had a chance to ACK
 * outbound data and free the corresponding pbufs. That falsely
 * accounted for in-flight TCP_WRITE_FLAG_COPY bytes as a "leak"
 * (typically several KB depending on session output volume).
 *
 * After tcp_close we hold the ctx slot in a "settling" state for
 * this many milliseconds. The slot stays unusable to new sessions,
 * but pcb is already NULL on our side and lwIP is free to drain
 * TX. When the timer expires we measure `lwip_stats.mem.used`
 * against the open-time baseline — a positive delta now reflects
 * a real leak, not unacked TX residue.
 *
 * 1500 ms is comfortably above the lwIP retransmit timeout (~1 s)
 * so even one retransmit cycle has time to clear, but well below
 * the 16-slot pool's exhaustion point at typical close cadences.
 */
#define TCP_SHELL_CLOSE_SETTLE_MS 1500

/*
 * Total wall-clock cap on `tcp_write_buf` waiting for ring space (#536).
 *
 * When the lwIP heap is exhausted (#537 leak path or a transient burst),
 * tcp_write returns ERR_MEM forever, the drain in shell_io_tcp_poll
 * can't free TX-ring slots, and tcp_write_buf would otherwise loop
 * indefinitely in sleep_ms. The user-visible symptom is any multi-line
 * shell command (model stats, model gpu, tasks, mem, ...) freezing
 * the telnet session.
 *
 * Once this cap is exceeded we mark the session degraded — subsequent
 * writes short-circuit, the session is closed so the user sees a clean
 * disconnect, and a one-shot warning is logged. The shell command
 * returns to its caller; the caller may print a short error and the
 * REPL exits cleanly.
 *
 * 2000 ms is comfortably above any plausible legitimate stall (one
 * lwIP retransmit timeout is ~1 s) but well below the user's
 * patience threshold for "is the box dead?".
 */
#define TCP_SHELL_WRITE_TIMEOUT_MS 2000

/*
 * Lifecycle:
 *
 *   shell_io_tcp_create sets in_use=true, closed=false, shell_done=false
 *   and pcb=peer_pcb. Both "sides" (the shell task, via the shell_io
 *   pointer, and lwIP, via the pcb) share the ctx.
 *
 *   Close can originate from either side:
 *     - Peer:  on_recv(p=NULL) or on_err -> closed=true.
 *     - Shell: shell_run returns, the session task calls
 *              shell_io_tcp_destroy() which sets both closed=true and
 *              shell_done=true. After this, the shell task MUST NOT
 *              touch the io/ctx.
 *
 *   Net_pump (shell_io_tcp_poll) does all lwIP work:
 *     - Drain TX on every tick while pcb is valid.
 *     - Once closed && tx empty && pcb still held -> tcp_close(pcb),
 *       set pcb=NULL. lwIP frees the pcb itself.
 *     - on_err nulls pcb for us and sets closed=true.
 *     - Once shell_done && pcb==NULL, ctx_free the slot.
 *
 *   Only net_pump frees ctx. This avoids the use-after-free that
 *   would arise if the shell task freed the slot while a TCP recv
 *   callback was still in flight.
 */
struct tcp_shell_ctx {
    bool              in_use;
    volatile bool     closed;        /* set on any close path */
    volatile bool     shell_done;    /* set by shell_io_tcp_destroy */
    /* Set by tcp_write_buf when the wall-clock cap is exceeded (#536).
     * Subsequent writes short-circuit instead of busy-waiting on a
     * drain that lwIP can't service. */
    volatile bool     write_degraded;

    /* lwIP pcb — net_pump context only. NULL after close. */
    struct tcp_pcb   *pcb;

    /* Peer + timing info captured in shell_io_tcp_create. Read-only
     * after that. Used by telnetd sessions / kick for diagnostics. */
    uint32_t          peer_ip;        /* network byte order */
    uint16_t          peer_port;
    uint32_t          connected_at;   /* sys_now() at accept time */
    uint32_t          session_id;     /* mirror of sess->id for iteration without deref */

    /* Associated shell_session — populated after shell_io_tcp_create
     * by the tcp_shell_server accept path. Lets the telnet parser
     * update window_cols / term_type / interrupt_requested directly
     * on the session. May be NULL briefly between create and attach
     * if the caller hasn't set it yet; telnet callbacks tolerate NULL. */
    struct shell_session *session;

    /* RX ring: bytes from peer waiting for the shell task to read. */
    spinlock_t        rx_lock;
    uint8_t           rx_buf[TCP_SHELL_RING_SIZE];
    uint32_t          rx_head;
    uint32_t          rx_tail;

    /* TX ring: bytes from shell task waiting to go out via tcp_write. */
    spinlock_t        tx_lock;
    uint8_t           tx_buf[TCP_SHELL_RING_SIZE];
    uint32_t          tx_head;
    uint32_t          tx_tail;
    bool              tx_prev_was_cr;   /* preserve CRLF state across write calls */

    /* Telnet parser — state machine between the peer and the RX
     * ring. Fed byte-by-byte from on_recv; emits data bytes into
     * the ring via telnet_inject_rx and sends response IACs via
     * telnet_send_to_peer below. */
    struct telnet_parser telnet;

    /* lwIP heap usage at the moment this session's lwIP wiring went
     * live (end of shell_io_tcp_create, after the initial telnet
     * negotiation has been queued). The close path measures
     * `lwip_stats.mem.used - heap_used_at_open_bytes` *after*
     * tcp_close + ctx_free have run, which is the only window where
     * a sustained positive delta indicates a real leak rather than
     * unacked-TX residue lwIP will free during TIME_WAIT. */
    uint32_t          heap_used_at_open_bytes;

    /* Timer-counter value captured immediately after tcp_close (#537).
     * Zero when the close hasn't happened yet. The measurement +
     * ctx_free is deferred until at least TCP_SHELL_CLOSE_SETTLE_MS
     * after this point so unacked TX bytes have a chance to drain. */
    uint64_t          close_completed_ticks;

    /* io vtable embedded so we don't heap-allocate. io.ctx == this. */
    struct shell_io   io;
};

static struct tcp_shell_ctx ctx_pool[MAX_TCP_SHELL_SESSIONS];
static spinlock_t            pool_lock = SPINLOCK_INIT;

/* -------------------------------------------------------------------------- */
/* Ring helpers — all callers already hold the relevant lock.                 */
/* -------------------------------------------------------------------------- */

static inline uint32_t ring_used(uint32_t head, uint32_t tail)
{
    return (head - tail) & TCP_SHELL_RING_MASK;
}

static inline uint32_t ring_free(uint32_t head, uint32_t tail)
{
    return TCP_SHELL_RING_MASK - ring_used(head, tail);
}

static size_t normalize_output_bytes(uint8_t *dst,
                                     size_t avail,
                                     const char *src,
                                     size_t src_len,
                                     bool *prev_was_cr)
{
    size_t out = 0;
    for (size_t i = 0; i < src_len && out < avail; i++) {
        char c = src[i];
        if (c == '\n' && !*prev_was_cr) {
            if (out + 2 > avail) {
                break;
            }
            dst[out++] = '\r';
        }
        dst[out++] = (uint8_t)c;
        *prev_was_cr = (c == '\r');
    }
    return out;
}

/* -------------------------------------------------------------------------- */
/* Telnet parser callbacks                                                    */
/*                                                                            */
/* These run in net_pump context (invoked from on_recv or from                */
/* shell_io_tcp_create). Data bytes go into the RX ring (same lock as         */
/* tcp_recv's direct writes in the raw-TCP Phase 1 path). Response bytes      */
/* go straight out via tcp_write — it's safe from net_pump context and        */
/* avoids contending with the shell task's outbound TX ring writes.           */
/* -------------------------------------------------------------------------- */

static void telnet_inject_rx(void *opaque, uint8_t byte)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)opaque;
    irq_flags_t flags = spin_lock_irqsave(&ctx->rx_lock);
    if (ring_free(ctx->rx_head, ctx->rx_tail) > 0) {
        ctx->rx_buf[ctx->rx_head & TCP_SHELL_RING_MASK] = byte;
        ctx->rx_head = (ctx->rx_head + 1) & TCP_SHELL_RING_MASK;
    }
    /* Ring full — drop. A polite peer stops once the TCP window
     * stops advancing; see on_recv for the window-slide logic. */
    spin_unlock_irqrestore(&ctx->rx_lock, flags);
}

static void telnet_send_to_peer(void *opaque, const uint8_t *data, size_t len)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)opaque;
    if (!ctx->pcb || len == 0) {
        return;
    }
    if (len > 0xFFFF) len = 0xFFFF;
    /* TCP_WRITE_FLAG_COPY because `data` may live on the parser's
     * stack (e.g. a 3-byte IAC response) which goes out of scope
     * before lwIP actually transmits. */
    (void)tcp_write(ctx->pcb, data, (uint16_t)len, TCP_WRITE_FLAG_COPY);
    /* tcp_output is called once at the end of on_recv; individual
     * response writes don't need to flush. */
}

static void telnet_on_naws(void *opaque, uint16_t cols, uint16_t rows)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)opaque;
    if (ctx->session) {
        ctx->session->window_cols = cols;
        ctx->session->window_rows = rows;
    }
}

static void telnet_on_term_type(void *opaque, const char *term)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)opaque;
    if (ctx->session && term) {
        size_t n = strlen(term);
        if (n >= sizeof(ctx->session->term_type)) {
            n = sizeof(ctx->session->term_type) - 1;
        }
        for (size_t i = 0; i < n; i++) {
            ctx->session->term_type[i] = term[i];
        }
        ctx->session->term_type[n] = '\0';
    }
}

static void telnet_on_interrupt(void *opaque)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)opaque;
    if (ctx->session) {
        ctx->session->interrupt_requested = true;
    }
    /* Also inject 0x03 so a blocking shell_read_line sees ^C and
     * cancels the current line. */
    telnet_inject_rx(ctx, 0x03);
}

static const struct telnet_ops telnet_ops_template = {
    .inject_rx     = telnet_inject_rx,
    .send_to_peer  = telnet_send_to_peer,
    .on_naws       = telnet_on_naws,
    .on_term_type  = telnet_on_term_type,
    .on_interrupt  = telnet_on_interrupt,
    .ctx           = NULL,   /* overwritten per-ctx in shell_io_tcp_create */
};

/* -------------------------------------------------------------------------- */
/* shell_io vtable                                                            */
/* -------------------------------------------------------------------------- */

static int tcp_read_char(struct shell_io *io)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;

    for (;;) {
        irq_flags_t flags = spin_lock_irqsave(&ctx->rx_lock);
        if (ctx->rx_head != ctx->rx_tail) {
            uint8_t c = ctx->rx_buf[ctx->rx_tail & TCP_SHELL_RING_MASK];
            ctx->rx_tail = (ctx->rx_tail + 1) & TCP_SHELL_RING_MASK;
            spin_unlock_irqrestore(&ctx->rx_lock, flags);

            /* Acknowledge one byte so lwIP can slide its recv window.
             * Deferring this to shell_io_tcp_poll would require a
             * per-session "bytes-to-ack" counter; calling tcp_recved
             * from shell-task context is safe per lwIP docs because
             * the raw API permits it from any task as long as the
             * call is not concurrent with net_poll — and we cooperate
             * with net_pump via the ring locks. The shell task and
             * net_pump both run on CPU 0, so no real concurrency. */
            /* Intentionally acked in poll() instead to keep all lwIP
             * calls on net_pump — simpler invariant, slightly higher
             * latency. */

            return (int)c;
        }
        bool closed = ctx->closed;
        spin_unlock_irqrestore(&ctx->rx_lock, flags);

        if (closed) {
            return -1;
        }
        sleep_ms(TCP_SHELL_POLL_INTERVAL_MS);
    }
}

static int tcp_try_read_char(struct shell_io *io)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;

    irq_flags_t flags = spin_lock_irqsave(&ctx->rx_lock);
    if (ctx->rx_head == ctx->rx_tail) {
        spin_unlock_irqrestore(&ctx->rx_lock, flags);
        return -1;    /* no data, regardless of whether closed */
    }
    uint8_t c = ctx->rx_buf[ctx->rx_tail & TCP_SHELL_RING_MASK];
    ctx->rx_tail = (ctx->rx_tail + 1) & TCP_SHELL_RING_MASK;
    spin_unlock_irqrestore(&ctx->rx_lock, flags);
    return (int)c;
}

/* Forward decl — defined in the lwIP-callback section below. The
 * tcp_write_buf timeout path calls it to mark the session closed
 * when the drain has stalled past the cap. */
static void mark_closed(struct tcp_shell_ctx *ctx);

/* Test hook — overridable via shell_io_tcp_test_set_write_timeout_ms.
 * Defaults to TCP_SHELL_WRITE_TIMEOUT_MS in production. The
 * regression test in test_net.c drives the timeout path with a
 * tiny override (e.g. 50 ms) so it doesn't have to wait 2 seconds
 * to assert the bound. */
static uint32_t g_write_timeout_ms = TCP_SHELL_WRITE_TIMEOUT_MS;

void shell_io_tcp_test_set_write_timeout_ms(uint32_t ms)
{
    g_write_timeout_ms = ms ? ms : TCP_SHELL_WRITE_TIMEOUT_MS;
}

#include "uart.h"

static void tcp_write_buf(struct shell_io *io, const char *buf, size_t len)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;

    /* Once the session has hit the write-timeout (#536) all further
     * writes drop on the floor — the lwIP path is wedged and trying
     * again would just hang the next command too. The session is
     * already marked closed by the timeout path so the REPL will
     * unwind and the slot will be freed by the poll path. */
    if (ctx->write_degraded || ctx->closed) {
        return;
    }

    /* Bound the total wall-clock time spent waiting for ring space.
     * Per-iteration sleep_ms gives net_pump a chance to drain; if the
     * lwIP heap is exhausted we'd otherwise loop forever (#536). */
    const uint64_t freq = timer_get_frequency();
    const uint64_t timeout_ticks = (freq * (uint64_t)g_write_timeout_ms) / 1000ULL;
    const uint64_t deadline = timer_get_count() + timeout_ticks;

    /* Match the UART backend's line discipline: emit CRLF on output so
     * telnet clients don't render bare LF as "move down but stay in the
     * same column". Preserve standalone '\r' state across write calls so
     * callers that emit '\r' and '\n' separately still produce one CRLF. */
    size_t src = 0;
    while (src < len) {
        /* Fast unlocked check — `closed` is volatile bool, so single-
         * byte reads are atomic on both targets. If it's true, the
         * session is gone and any bytes we'd queue will be dropped
         * on the next drain anyway. */
        if (ctx->closed) {
            return;
        }

        irq_flags_t flags = spin_lock_irqsave(&ctx->tx_lock);
        uint32_t avail = ring_free(ctx->tx_head, ctx->tx_tail);

        if (avail == 0) {
            spin_unlock_irqrestore(&ctx->tx_lock, flags);
            if (timer_get_count() >= deadline) {
                /* Drain hasn't progressed — lwIP is wedged. Mark the
                 * session degraded so subsequent shell_printf calls
                 * from the same command bail immediately, then close
                 * the session so the user sees a disconnect rather
                 * than a black hole. The shell command returns to
                 * its REPL caller, which sees `closed` and exits. */
                ctx->write_degraded = true;
                size_t dropped = len - src;
                uart_printf("[WARN] shell-tcp: session %u write timeout "
                            "(>=%u ms drain stall, dropping %u bytes) "
                            "— marking degraded, closing\n",
                            (unsigned)ctx->session_id,
                            (unsigned)g_write_timeout_ms,
                            (unsigned)dropped);
                mark_closed(ctx);
                return;
            }
            /* Yield so net_pump can drain the buffer. */
            sleep_ms(TCP_SHELL_POLL_INTERVAL_MS);
            continue;
        }

        size_t queued = 0;
        while (src < len && queued < avail) {
            char c = buf[src];
            if (c == '\n' && !ctx->tx_prev_was_cr) {
                if (queued + 2 > avail) {
                    break;
                }
                ctx->tx_buf[ctx->tx_head & TCP_SHELL_RING_MASK] = '\r';
                ctx->tx_head = (ctx->tx_head + 1) & TCP_SHELL_RING_MASK;
                queued++;
            }
            ctx->tx_buf[ctx->tx_head & TCP_SHELL_RING_MASK] = (uint8_t)c;
            ctx->tx_head = (ctx->tx_head + 1) & TCP_SHELL_RING_MASK;
            ctx->tx_prev_was_cr = (c == '\r');
            queued++;
            src++;
        }
        spin_unlock_irqrestore(&ctx->tx_lock, flags);
        if (queued == 0) {
            /* A bare '\n' needs two bytes of ring space. If only one
             * slot is free, we must yield here rather than immediately
             * retrying and spinning forever on CPU 0. The same wall-
             * clock cap applies — if the ring stays one-byte-free
             * forever we still bail. */
            if (timer_get_count() >= deadline) {
                ctx->write_degraded = true;
                size_t dropped = len - src;
                uart_printf("[WARN] shell-tcp: session %u write timeout "
                            "(>=%u ms drain stall, dropping %u bytes) "
                            "— marking degraded, closing\n",
                            (unsigned)ctx->session_id,
                            (unsigned)g_write_timeout_ms,
                            (unsigned)dropped);
                mark_closed(ctx);
                return;
            }
            sleep_ms(TCP_SHELL_POLL_INTERVAL_MS);
        }
    }
}

static void tcp_flush(struct shell_io *io)
{
    (void)io;
    /* Nothing to do — shell_io_tcp_poll() drains every ~10 ms. */
}

static void tcp_close_io(struct shell_io *io)
{
    /* close via the io->close vtable path is also the "shell task is
     * done" signal. Equivalent to shell_io_tcp_destroy. */
    shell_io_tcp_destroy(io);
}

static bool tcp_is_open(struct shell_io *io)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;
    /* `closed` is volatile bool — single-byte atomic load. */
    return !ctx->closed;
}

/* -------------------------------------------------------------------------- */
/* lwIP callbacks — net_pump context                                          */
/* -------------------------------------------------------------------------- */

static void mark_closed(struct tcp_shell_ctx *ctx)
{
    irq_flags_t flags = spin_lock_irqsave(&ctx->rx_lock);
    ctx->closed = true;
    spin_unlock_irqrestore(&ctx->rx_lock, flags);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)arg;

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        mark_closed(ctx);
        return ERR_OK;
    }

    if (p == NULL) {
        /* Peer FIN — mark closed; shell task will exit its REPL and
         * the poll path will complete the tcp_close. */
        mark_closed(ctx);
        return ERR_OK;
    }

    /* Feed every byte through the telnet parser. The parser pushes
     * decoded data bytes into the RX ring via telnet_inject_rx and
     * emits option-negotiation replies via telnet_send_to_peer. Ring
     * overflow is still possible (telnet_inject_rx drops on a full
     * ring) but is rare in practice because (a) IAC sequences shrink
     * the byte stream on average, and (b) a polite peer slows down
     * once tcp_recved stops advancing the window. */
    uint16_t consumed = 0;
    struct pbuf *q = p;
    while (q) {
        const uint8_t *data = (const uint8_t *)q->payload;
        telnet_rx(&ctx->telnet, data, q->len);
        consumed += q->len;
        q = q->next;
    }

    if (consumed > 0) {
        tcp_recved(pcb, consumed);
        /* Flush any telnet responses the parser queued on the pcb. */
        if (ctx->pcb) {
            tcp_output(ctx->pcb);
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)err;
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)arg;
    /* lwIP has already freed the pcb by the time tcp_err fires. */
    ctx->pcb = NULL;
    /* Start the post-close settling timer (#537). The poll path
     * normally sets this immediately after its own tcp_close, but
     * on_err is the abrupt-close path (peer RST, ABRT) — the pcb
     * is already gone by the time we get here, so the poll path's
     * close branch will never fire. Set the timestamp here so the
     * slot isn't held indefinitely. */
    if (ctx->close_completed_ticks == 0) {
        uint64_t now = timer_get_count();
        ctx->close_completed_ticks = now ? now : 1;
    }
    mark_closed(ctx);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, uint16_t len)
{
    (void)arg;
    (void)pcb;
    (void)len;
    /* Nothing to do — shell_io_tcp_poll will push more TX on the next
     * tick when the TX ring has bytes. Kept for diagnostic hooks. */
    return ERR_OK;
}

/* -------------------------------------------------------------------------- */
/* Pool allocation                                                            */
/* -------------------------------------------------------------------------- */

static struct tcp_shell_ctx *ctx_alloc(void)
{
    irq_flags_t flags = spin_lock_irqsave(&pool_lock);
    for (uint32_t i = 0; i < MAX_TCP_SHELL_SESSIONS; i++) {
        struct tcp_shell_ctx *c = &ctx_pool[i];
        if (!c->in_use) {
            c->in_use  = true;
            c->closed  = false;
            c->shell_done = false;
            c->write_degraded = false;
            c->pcb     = NULL;
            c->rx_lock = (spinlock_t)SPINLOCK_INIT;
            c->tx_lock = (spinlock_t)SPINLOCK_INIT;
            c->rx_head = c->rx_tail = 0;
            c->tx_head = c->tx_tail = 0;
            c->tx_prev_was_cr = false;
            c->close_completed_ticks = 0;
            spin_unlock_irqrestore(&pool_lock, flags);
            return c;
        }
    }
    spin_unlock_irqrestore(&pool_lock, flags);
    return NULL;
}

static void ctx_free(struct tcp_shell_ctx *ctx)
{
    irq_flags_t flags = spin_lock_irqsave(&pool_lock);
    ctx->in_use = false;
    spin_unlock_irqrestore(&pool_lock, flags);
}

/*
 * Test-only driver for the #536 timeout path.
 *
 * Allocates a slot from the pool, primes it as if a session were
 * active (no pcb — net_pump's drain therefore won't run, simulating
 * the lwIP-wedged state), then issues a tcp_write_buf large enough
 * to overflow the TX ring. The function is expected to return after
 * roughly `timeout_override_ms` (one `sleep_ms` step of slop) with
 * `write_degraded` set on the ctx and `closed` true.
 *
 * Returns 0 on success (the bound was respected), -1 if no slot
 * could be allocated, -2 if the timeout was not honored.
 */
int shell_io_tcp_test_run_write_timeout(uint32_t timeout_override_ms)
{
    struct tcp_shell_ctx *ctx = ctx_alloc();
    if (!ctx) {
        return -1;
    }

    /* Wire just enough vtable to call write — the rest of the
     * tcp_shell_ctx fields are zero-initialised by ctx_alloc. */
    ctx->io.write = tcp_write_buf;
    ctx->io.ctx   = ctx;
    ctx->session_id = 0xDEADBEEF;   /* visible in the warning log */

    shell_io_tcp_test_set_write_timeout_ms(timeout_override_ms);

    /* Buffer must exceed TCP_SHELL_RING_SIZE so the second pass
     * stalls on a full ring. The contents don't matter — the test
     * is about the timeout, not about what got queued. */
    char buf[TCP_SHELL_RING_SIZE + 256];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 'x';

    uint64_t freq = timer_get_frequency();
    uint64_t t0 = timer_get_count();
    tcp_write_buf(&ctx->io, buf, sizeof(buf));
    uint64_t elapsed_ms = ((timer_get_count() - t0) * 1000ULL) / freq;

    /* Restore the production default before any other tests run. */
    shell_io_tcp_test_set_write_timeout_ms(0);

    bool ok = ctx->write_degraded && ctx->closed
              && elapsed_ms >= timeout_override_ms
              && elapsed_ms < timeout_override_ms + 1000ULL;

    ctx_free(ctx);
    return ok ? 0 : -2;
}

/*
 * Test-only driver for the #537 close-settling path.
 *
 * Drives two controlled scenarios:
 *   1. close_completed_ticks set to "now" — poll must NOT free the
 *      slot (settling delay hasn't elapsed).
 *   2. close_completed_ticks rewound past TCP_SHELL_CLOSE_SETTLE_MS —
 *      poll must free the slot on the next call.
 *
 * Returns 0 on success, -1 if no slot could be allocated, -2 if
 * the "hold" assertion failed, -3 if the "release" assertion failed.
 */
int shell_io_tcp_test_run_close_settling(void)
{
    struct tcp_shell_ctx *ctx = ctx_alloc();
    if (!ctx) {
        return -1;
    }

    /* Mimic the post-tcp_close state: pcb already NULL, shell task
     * has signaled done, baseline heap snapshot in place. */
    ctx->shell_done = true;
    ctx->pcb = NULL;
    ctx->session_id = 0xCAFEBABE;
    ctx->heap_used_at_open_bytes = 0;

    /* Scenario 1: settling timer just started. Poll must NOT free. */
    uint64_t now = timer_get_count();
    ctx->close_completed_ticks = now ? now : 1;
    shell_io_tcp_poll();
    if (!ctx->in_use) {
        /* Poll prematurely freed the slot — but we have no slot to
         * release back since it's already gone. Return the failure. */
        return -2;
    }

    /* Scenario 2: rewind close_completed_ticks far enough that the
     * settle window has elapsed. Poll must free. */
    uint64_t freq = timer_get_frequency();
    uint64_t settle_ticks =
        (freq * (uint64_t)(TCP_SHELL_CLOSE_SETTLE_MS + 100)) / 1000ULL;
    /* Saturating subtract — older firmware on slow boards has tiny
     * tick counters at this point in boot; just clamp to 1 (the
     * "set" sentinel) if we'd underflow. */
    uint64_t cur = timer_get_count();
    ctx->close_completed_ticks = (cur > settle_ticks)
        ? (cur - settle_ticks)
        : 1ULL;
    shell_io_tcp_poll();
    if (ctx->in_use) {
        /* Poll didn't free. Release manually so the test cleanup
         * doesn't leak the slot. */
        ctx_free(ctx);
        return -3;
    }

    return 0;
}

size_t shell_io_tcp_test_normalize_output(const char *first,
                                          const char *second,
                                          char *out,
                                          size_t out_len)
{
    size_t total = 0;
    bool prev_was_cr = false;

    if (!out || out_len == 0) {
        return 0;
    }
    if (first) {
        total += normalize_output_bytes((uint8_t *)out + total,
                                        out_len - total,
                                        first,
                                        strlen(first),
                                        &prev_was_cr);
    }
    if (second && total < out_len) {
        total += normalize_output_bytes((uint8_t *)out + total,
                                        out_len - total,
                                        second,
                                        strlen(second),
                                        &prev_was_cr);
    }
    return total;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

struct shell_io *shell_io_tcp_create(struct tcp_pcb *pcb)
{
    struct tcp_shell_ctx *ctx = ctx_alloc();
    if (!ctx) {
        return NULL;
    }

    ctx->pcb          = pcb;
    ctx->session      = NULL;   /* Populated later by shell_io_tcp_attach_session. */
    ctx->peer_ip      = pcb->remote_ip.addr;
    ctx->peer_port    = pcb->remote_port;
    ctx->connected_at = sys_now();
    ctx->session_id   = 0;      /* Populated by shell_io_tcp_attach_session. */

    ctx->io.read_char     = tcp_read_char;
    ctx->io.try_read_char = tcp_try_read_char;
    ctx->io.write         = tcp_write_buf;
    ctx->io.flush         = tcp_flush;
    ctx->io.close         = tcp_close_io;
    ctx->io.is_open       = tcp_is_open;
    ctx->io.ctx           = ctx;

    /* Initialize the telnet parser with our callback set. ctx->ctx
     * is the opaque pointer the parser hands back to every callback. */
    struct telnet_ops ops = telnet_ops_template;
    ops.ctx = ctx;
    telnet_init(&ctx->telnet, &ops);

    /* Wire lwIP callbacks. `arg` is the ctx so callbacks can find it. */
    tcp_arg(pcb, ctx);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    tcp_sent(pcb, on_sent);

    /* Send the initial option-negotiation burst. Raw-nc clients just
     * see 12 bytes of 0xFF-prefixed noise; telnet clients transition
     * into character-at-a-time server-echoed mode. tcp_output flushes. */
    telnet_send_initial_negotiation(&ctx->telnet);
    tcp_output(pcb);

    /* Snapshot the lwIP heap *after* the initial negotiation has
     * been queued. Anything still on the heap at ctx_free time
     * above this baseline is the per-session leak the detector
     * is hunting for. MEM_STATS guards the field but lwip_stats
     * is always defined; falls back to 0 when stats are off. */
#if MEM_STATS
    ctx->heap_used_at_open_bytes = (uint32_t)lwip_stats.mem.used;
#else
    ctx->heap_used_at_open_bytes = 0;
#endif

    return &ctx->io;
}

void shell_io_tcp_attach_session(struct shell_io *io, struct shell_session *s)
{
    if (!io) return;
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;
    ctx->session    = s;
    ctx->session_id = s ? s->id : 0;
}

void shell_io_tcp_destroy(struct shell_io *io)
{
    if (!io) return;
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;
    /* Drop the session pointer before the shell task returns from
     * this call and then releases `sess` via shell_session_free. A
     * late on_recv on net_pump (e.g. peer sent data+FIN together,
     * data portion delivered after the shell task exited its REPL)
     * would otherwise dereference this stale pointer — worse, the
     * pool could have re-allocated the slot to a new connection by
     * then, so telnet_on_naws / on_term_type / on_interrupt would
     * corrupt the new connection's session state. Both sides run on
     * CPU 0, so the scheduler switch orders this NULL store ahead of
     * any subsequent net_pump read — no barrier needed. */
    ctx->session = NULL;
    mark_closed(ctx);
    ctx->shell_done = true;
    /* After this returns, the caller MUST NOT touch the io again —
     * the poll path may tcp_close and free the slot on the next
     * tick. */
}

uint32_t shell_io_tcp_active_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_TCP_SHELL_SESSIONS; i++) {
        if (ctx_pool[i].in_use) n++;
    }
    return n;
}

void shell_io_tcp_foreach(tcp_session_visitor_t visitor, void *user_ctx)
{
    if (!visitor) return;
    for (uint32_t i = 0; i < MAX_TCP_SHELL_SESSIONS; i++) {
        struct tcp_shell_ctx *ctx = &ctx_pool[i];
        /* Snapshot under pool_lock so a concurrent ctx_alloc / ctx_free
         * can't flip in_use mid-read. The snapshot fields are all
         * scalars, so a consistent set comes out of the locked region. */
        irq_flags_t flags = spin_lock_irqsave(&pool_lock);
        bool active = ctx->in_use && !ctx->closed;
        struct tcp_session_info info = {0};
        if (active) {
            info.session_id   = ctx->session_id;
            info.peer_ip      = ctx->peer_ip;
            info.peer_port    = ctx->peer_port;
            info.connected_at = ctx->connected_at;
        }
        spin_unlock_irqrestore(&pool_lock, flags);

        if (active) {
            if (!visitor(&info, user_ctx)) {
                return;
            }
        }
    }
}

bool shell_io_tcp_kick(uint32_t session_id)
{
    /* Hold pool_lock across the whole scan AND the `closed` store so
     * an intervening ctx_free + ctx_alloc can't re-assign the slot to
     * a different session between our match check and the store —
     * otherwise we'd disconnect an innocent session that happened to
     * land in this slot during the kick's context switch. Pool_lock
     * precedes rx_lock in the lock ordering (no path holds rx_lock
     * then grabs pool_lock), so setting `closed` directly under
     * pool_lock is safe. `closed` is volatile, so the unlocked
     * reader in tcp_read_char will observe the new value on its
     * next iteration; we don't need rx_lock for the store itself
     * (it's there to guard head/tail on the recv path, not the flag). */
    bool kicked = false;
    irq_flags_t flags = spin_lock_irqsave(&pool_lock);
    for (uint32_t i = 0; i < MAX_TCP_SHELL_SESSIONS; i++) {
        struct tcp_shell_ctx *ctx = &ctx_pool[i];
        if (ctx->in_use && ctx->session_id == session_id) {
            ctx->closed = true;
            kicked = true;
            break;
        }
    }
    spin_unlock_irqrestore(&pool_lock, flags);
    return kicked;
}

void shell_io_tcp_poll(void)
{
    for (uint32_t i = 0; i < MAX_TCP_SHELL_SESSIONS; i++) {
        struct tcp_shell_ctx *ctx = &ctx_pool[i];
        if (!ctx->in_use) continue;

        /* Drain the TX ring into lwIP. */
        if (ctx->pcb) {
            for (;;) {
                irq_flags_t flags = spin_lock_irqsave(&ctx->tx_lock);
                uint32_t used = ring_used(ctx->tx_head, ctx->tx_tail);
                if (used == 0) {
                    spin_unlock_irqrestore(&ctx->tx_lock, flags);
                    break;
                }
                /* lwIP's send buffer is bounded by tcp_sndbuf(). Send
                 * no more than sndbuf in one call. */
                uint16_t sndbuf = tcp_sndbuf(ctx->pcb);
                if (sndbuf == 0) {
                    spin_unlock_irqrestore(&ctx->tx_lock, flags);
                    break;
                }
                uint32_t chunk = (used < sndbuf) ? used : sndbuf;

                /* Wrap-around: copy the pre-wrap portion only. */
                uint32_t tail_idx = ctx->tx_tail & TCP_SHELL_RING_MASK;
                uint32_t contiguous = TCP_SHELL_RING_SIZE - tail_idx;
                if (chunk > contiguous) chunk = contiguous;

                /* tcp_write can fail with ERR_MEM — retry next tick. */
                err_t werr = tcp_write(ctx->pcb, &ctx->tx_buf[tail_idx],
                                       (uint16_t)chunk, TCP_WRITE_FLAG_COPY);
                if (werr == ERR_OK) {
                    ctx->tx_tail = (ctx->tx_tail + chunk) & TCP_SHELL_RING_MASK;
                }
                spin_unlock_irqrestore(&ctx->tx_lock, flags);

                if (werr != ERR_OK) {
                    break;   /* try again next poll */
                }
            }

            /* Kick lwIP to actually send queued segments. */
            tcp_output(ctx->pcb);
        }

        /* Close the pcb once our side is closed AND TX has drained.
         * lwIP owns the pcb memory after tcp_close. */
        if (ctx->closed && ctx->pcb) {
            irq_flags_t flags = spin_lock_irqsave(&ctx->tx_lock);
            bool tx_empty = (ctx->tx_head == ctx->tx_tail);
            spin_unlock_irqrestore(&ctx->tx_lock, flags);

            if (tx_empty) {
                tcp_arg(ctx->pcb, NULL);
                tcp_recv(ctx->pcb, NULL);
                tcp_err(ctx->pcb, NULL);
                tcp_sent(ctx->pcb, NULL);
                (void)tcp_close(ctx->pcb);
                ctx->pcb = NULL;
                /* Start the post-close settling timer (#537). The
                 * measurement + ctx_free below is gated on
                 * TCP_SHELL_CLOSE_SETTLE_MS elapsing so any unacked
                 * TX bytes still on the lwIP heap have a chance to
                 * be ACKed and freed first — otherwise they're
                 * incorrectly counted as a leak. timer_get_count()
                 * never returns 0 in normal operation, but be
                 * defensive: bias to 1 so the "0 means unset" check
                 * below is unambiguous. */
                uint64_t now = timer_get_count();
                ctx->close_completed_ticks = now ? now : 1;
            }
        }

        /* Free the slot only once the shell task has released it AND
         * the pcb is fully gone AND the post-close settling delay
         * (#537) has elapsed. The settling delay gives lwIP time to
         * receive the FIN-ACK and release any unacked TX pbufs that
         * were allocated by tcp_write(... TCP_WRITE_FLAG_COPY). */
        if (ctx->shell_done && ctx->pcb == NULL &&
            ctx->close_completed_ticks != 0) {
            uint64_t freq = timer_get_frequency();
            uint64_t elapsed_ticks =
                timer_get_count() - ctx->close_completed_ticks;
            uint64_t elapsed_ms = (elapsed_ticks * 1000ULL) / freq;
            if (elapsed_ms < TCP_SHELL_CLOSE_SETTLE_MS) {
                continue;   /* not yet — let lwIP drain */
            }

            /* Settling complete. Compute and report the post-close
             * heap delta. After TCP_SHELL_CLOSE_SETTLE_MS, any
             * remaining positive delta against `heap_used_at_open_bytes`
             * is a real leak — TIME_WAIT pcbs live in their own
             * memp pool, not the heap, and unacked TX retransmits
             * have completed by now. */
#if MEM_STATS
            uint32_t close_used = (uint32_t)lwip_stats.mem.used;
#else
            uint32_t close_used = 0;
#endif
            int32_t delta = (int32_t)(close_used - ctx->heap_used_at_open_bytes);
            tcp_shell_server_note_session_close(ctx->session_id, delta);
            ctx_free(ctx);
        }
    }
}
