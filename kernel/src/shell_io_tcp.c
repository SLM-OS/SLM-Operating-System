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
#include "timer.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"

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

    /* Copy payload into the ring buffer under rx_lock. */
    uint16_t consumed = 0;
    struct pbuf *q = p;
    while (q) {
        const uint8_t *data = (const uint8_t *)q->payload;
        uint16_t len = q->len;

        irq_flags_t flags = spin_lock_irqsave(&ctx->rx_lock);
        uint32_t avail = ring_free(ctx->rx_head, ctx->rx_tail);
        uint16_t to_copy = (len < avail) ? len : (uint16_t)avail;
        for (uint16_t i = 0; i < to_copy; i++) {
            ctx->rx_buf[ctx->rx_head & TCP_SHELL_RING_MASK] = data[i];
            ctx->rx_head = (ctx->rx_head + 1) & TCP_SHELL_RING_MASK;
        }
        spin_unlock_irqrestore(&ctx->rx_lock, flags);

        consumed += to_copy;
        if (to_copy < len) {
            /* Ring is full — drop the rest. This throttles a
             * misbehaving peer; a polite peer will slow down once
             * we stop advancing the TCP window. */
            break;
        }
        q = q->next;
    }

    if (consumed > 0) {
        tcp_recved(pcb, consumed);
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

    ctx->pcb = pcb;

    ctx->io.read_char     = tcp_read_char;
    ctx->io.try_read_char = tcp_try_read_char;
    ctx->io.write         = tcp_write_buf;
    ctx->io.flush         = tcp_flush;
    ctx->io.close         = tcp_close_io;
    ctx->io.is_open       = tcp_is_open;
    ctx->io.ctx           = ctx;

    /* Wire lwIP callbacks. `arg` is the ctx so callbacks can find it. */
    tcp_arg(pcb, ctx);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    tcp_sent(pcb, on_sent);

    return &ctx->io;
}

void shell_io_tcp_destroy(struct shell_io *io)
{
    if (!io) return;
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;
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
