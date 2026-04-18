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
#include "telnet.h"
#include "timer.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
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

    /* Telnet parser — state machine between the peer and the RX
     * ring. Fed byte-by-byte from on_recv; emits data bytes into
     * the ring via telnet_inject_rx and sends response IACs via
     * telnet_send_to_peer below. */
    struct telnet_parser telnet;

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

static void tcp_write_buf(struct shell_io *io, const char *buf, size_t len)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;

    size_t written = 0;
    while (written < len) {
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
            /* Yield so net_pump can drain the buffer. */
            sleep_ms(TCP_SHELL_POLL_INTERVAL_MS);
            continue;
        }

        size_t chunk = len - written;
        if (chunk > avail) chunk = avail;

        for (size_t i = 0; i < chunk; i++) {
            ctx->tx_buf[ctx->tx_head & TCP_SHELL_RING_MASK] = (uint8_t)buf[written + i];
            ctx->tx_head = (ctx->tx_head + 1) & TCP_SHELL_RING_MASK;
        }
        spin_unlock_irqrestore(&ctx->tx_lock, flags);
        written += chunk;
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
            c->pcb     = NULL;
            c->rx_lock = (spinlock_t)SPINLOCK_INIT;
            c->tx_lock = (spinlock_t)SPINLOCK_INIT;
            c->rx_head = c->rx_tail = 0;
            c->tx_head = c->tx_tail = 0;
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
    for (uint32_t i = 0; i < MAX_TCP_SHELL_SESSIONS; i++) {
        struct tcp_shell_ctx *ctx = &ctx_pool[i];
        irq_flags_t flags = spin_lock_irqsave(&pool_lock);
        bool match = ctx->in_use && ctx->session_id == session_id;
        spin_unlock_irqrestore(&pool_lock, flags);
        if (!match) continue;

        /* Close the session: shell_read_line will return -1 on its
         * next read once the RX ring drains, the REPL exits, and the
         * normal teardown path takes over. We do NOT set shell_done
         * here — that's the shell task's responsibility. */
        mark_closed(ctx);
        return true;
    }
    return false;
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
            }
        }

        /* Free the slot only once the shell task has released it AND
         * the pcb is fully gone. */
        if (ctx->shell_done && ctx->pcb == NULL) {
            ctx_free(ctx);
        }
    }
}
