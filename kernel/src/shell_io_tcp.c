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
#include "sched.h"               /* yield() for #597 Option B fast read loop */
#include "task.h"
#include "tcp_shell_server.h"   /* tcp_shell_server_note_session_close */
#include "telnet.h"
#include "timer.h"
#include "uart.h"               /* uart_printf for the timeout WARN (#536) */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/memp.h"         /* MEMP_MAX for per-pool delta snapshots (#537) */
#include "lwip/pbuf.h"
#include "lwip/stats.h"        /* lwip_stats.mem.used for the heap snapshot */
#include "arch/sys_arch.h"   /* sys_now() for connected_at timestamp */

/* Per-direction shell ring sizes. Both must be powers of two.
 *
 * TX ring (16 KB): originally bumped 4 KB → 16 KB in #581 so a
 * SHELL_MAX_LINE-sized response (8 KB) plus IAC headroom fits even
 * when the shell task is briefly behind on draining. Bounded by
 * tcp_write's uint16_t length argument; static_assert below.
 *
 * RX ring (256 KB): sized to absorb a full TCP_WND burst plus the
 * end-of-stream tail of an `xput-bin` upload without dropping bytes
 * (telnet_inject_rx silently drops on overflow; on_recv tcp_recveds
 * the consumed pbuf bytes regardless, so any drop is a permanent
 * data loss for the session). TCP_WND = 32 * MSS ≈ 46 KB, but on
 * stream-end the peer can have a window of acked-but-not-yet-
 * consumed bytes plus IAC-doubled 0xFF stuffing in flight that
 * collectively exceed a 64 KB ring. 256 KB gives ample margin —
 * the dropped-tail symptom on 100 MB uploads went away once it
 * landed.
 *
 * BSS cost: (16 KB tx + 256 KB rx) × MAX_TCP_SHELL_SESSIONS (16)
 * ≈ 4.3 MB. Trivial on Pi 5 / Jetson (4-8 GB RAM); roughly 0.4%
 * of QEMU's default 1 GB guest RAM, also fine. */
#define TCP_SHELL_TX_RING_SIZE 16384
#define TCP_SHELL_TX_RING_MASK (TCP_SHELL_TX_RING_SIZE - 1)
#define TCP_SHELL_RX_RING_SIZE 262144
#define TCP_SHELL_RX_RING_MASK (TCP_SHELL_RX_RING_SIZE - 1)

/* Backwards-compat alias used in places that don't care which ring
 * — kept for code clarity until existing call sites are audited.
 * Existing rx-side accesses have already been migrated to
 * TCP_SHELL_RX_RING_*; tx-side accesses use TCP_SHELL_TX_RING_*. */
#define TCP_SHELL_RING_SIZE TCP_SHELL_TX_RING_SIZE
#define TCP_SHELL_RING_MASK TCP_SHELL_TX_RING_MASK

_Static_assert((TCP_SHELL_TX_RING_SIZE & TCP_SHELL_TX_RING_MASK) == 0,
               "TCP_SHELL_TX_RING_SIZE must be a power of two");
_Static_assert((TCP_SHELL_RX_RING_SIZE & TCP_SHELL_RX_RING_MASK) == 0,
               "TCP_SHELL_RX_RING_SIZE must be a power of two");
/* TX drain passes a u16 length to tcp_write; clamp checked at call
 * site, but pin via assert so a future bump can't silently truncate. */
_Static_assert(TCP_SHELL_TX_RING_SIZE <= 0xFFFF,
               "TCP_SHELL_TX_RING_SIZE must fit in uint16_t for tcp_write()");

/* The per-session memp snapshot stores `lwip_stats.memp[i]->used`
 * losslessly so close-time deltas are accurate (#537). lwIP's
 * `mem_size_t` is configurable: u16_t for builds with
 * MEM_SIZE <= 64000, u32_t otherwise. Our build defaults to
 * MEM_SIZE = 131072 (PR #440), so `mem_size_t == u32_t`. The
 * snapshot array was uint16_t in the original commit; that
 * truncated the counter on our build (no observed harm in
 * practice — pool used-counts are tens at most — but a latent
 * bug). The assert pins the snapshot width to mem_size_t so any
 * future lwipopts.h change either compiles cleanly or breaks
 * loudly here. */
#if MEMP_STATS
_Static_assert(sizeof(((struct stats_mem *)0)->used) <= sizeof(uint32_t),
               "memp_used_at_open[] storage assumes mem_size_t fits "
               "in uint32_t; widen the field in struct tcp_shell_ctx "
               "to match a wider mem_size_t");
#endif

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

    /* First close path that fired, captured for the close-log INFO line.
     * String literal — no allocation, no free. NULL until the first
     * close path runs. Subsequent close paths leave the first reason
     * intact so we report root cause, not the cleanup that ran
     * immediately after.
     *
     * Two writers reach this field via different locks: `mark_closed`
     * holds `rx_lock`, while `shell_io_tcp_kick` holds `pool_lock`.
     * In practice they never race because every close path runs on
     * CPU 0 under cooperative scheduling. The `volatile` qualifier +
     * first-NULL-wins check keeps a future multi-CPU regression to
     * "wrong reason logged for one line" rather than corruption. The
     * inconsistent lock holders mirror the pre-existing model for
     * `closed`, which has the same two writers and same single-CPU
     * justification — see kick's pool_lock comment for why kick can't
     * drop pool_lock to acquire rx_lock without re-validating the
     * slot. */
    volatile const char *close_reason;

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
    uint8_t           rx_buf[TCP_SHELL_RX_RING_SIZE];
    uint32_t          rx_head;
    uint32_t          rx_tail;

    /* RX flow control (#621 follow-up): pbuf chain holding bytes
     * that didn't fit in the rx ring at on_recv time. Drained from
     * on_recv (next pbuf arrival) and from shell_io_tcp_poll (every
     * net_pump tick). lwIP's recv window only advances when we
     * tcp_recved, which we do only for bytes that actually injected
     * into the ring — so a slow shell task back-pressures the peer
     * naturally instead of the ring silently dropping. Both fields
     * are touched only from net_pump context (on_recv +
     * shell_io_tcp_poll); shell-task drains the ring without
     * touching them. */
    struct pbuf      *pending_pbuf;
    uint16_t          pending_offset;

    /* TX ring: bytes from shell task waiting to go out via tcp_write. */
    spinlock_t        tx_lock;
    uint8_t           tx_buf[TCP_SHELL_TX_RING_SIZE];
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

    /* Per-MEMP-pool snapshot at session open (#537 attribution).
     * Diffed at close to attribute any residual heap delta to the
     * specific lwIP allocator (PBUF_POOL, TCP_SEG, etc.) — the
     * heap-byte total alone doesn't tell us which allocation site
     * is leaking. Only meaningful when MEMP_STATS is enabled in
     * lwipopts.h; otherwise the array is unused.
     *
     * uint32_t to fit `mem_size_t` losslessly across both lwIP
     * configurations (u16 below MEM_SIZE=64000, u32 above). See
     * the static_assert above the struct. */
#if MEMP_STATS
    uint32_t          memp_used_at_open[MEMP_MAX];
#endif

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

static inline uint32_t ring_used_mask(uint32_t head, uint32_t tail, uint32_t mask)
{
    return (head - tail) & mask;
}

static inline uint32_t ring_free_mask(uint32_t head, uint32_t tail, uint32_t mask)
{
    return mask - ring_used_mask(head, tail, mask);
}

/* TX-only convenience wrappers — historical API used by the TX
 * drain in poll. Kept for now; RX paths use the *_mask form
 * directly with TCP_SHELL_RX_RING_MASK. */
static inline uint32_t ring_used(uint32_t head, uint32_t tail)
{
    return ring_used_mask(head, tail, TCP_SHELL_TX_RING_MASK);
}

static inline uint32_t ring_free(uint32_t head, uint32_t tail)
{
    return ring_free_mask(head, tail, TCP_SHELL_TX_RING_MASK);
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
    if (ring_free_mask(ctx->rx_head, ctx->rx_tail,
                       TCP_SHELL_RX_RING_MASK) > 0) {
        ctx->rx_buf[ctx->rx_head & TCP_SHELL_RX_RING_MASK] = byte;
        ctx->rx_head = (ctx->rx_head + 1) & TCP_SHELL_RX_RING_MASK;
        spin_unlock_irqrestore(&ctx->rx_lock, flags);
        return;
    }
    spin_unlock_irqrestore(&ctx->rx_lock, flags);

    /* Ring full at the moment of inject. With the on_recv
     * flow-control path (drain_pending_pbuf below), this should be
     * unreachable: the pre-check refuses to feed a wire byte to
     * telnet_rx_byte unless ring_free >= 1, so no inject can ever
     * find the ring full. WARN if we somehow get here — it means a
     * caller bypassed the pre-check (bug) or the telnet state
     * machine produced more than 1 inject from a single wire byte
     * (also a bug). */
    uart_printf("[WARN] shell-tcp: rx ring full — flow-control "
                "invariant violated; dropping byte 0x%02x\n", byte);
}

/*
 * Drain bytes from ctx->pending_pbuf into the rx ring (via the
 * telnet state machine), stopping as soon as the ring would have
 * to drop. Returns the count of pbuf wire bytes consumed —
 * caller passes that to tcp_recved so lwIP only slides its recv
 * window for bytes we actually accepted.
 *
 * Pre-check rule: process a wire byte only when ring_free >= 1.
 * telnet_rx_byte injects 0 or 1 ring bytes per wire byte (regular
 * data is 1:1; IAC sequences absorb 2-3 wire bytes for 0 or 1
 * inject). One free slot is therefore sufficient for the worst
 * case. The pre-check is conservative across IAC subnegotiations
 * (might idle at ring_free=0 even though the next several wire
 * bytes are IAC scaffolding that wouldn't inject), but the
 * pessimization is bounded by the IAC option length (~tens of
 * bytes) and never causes drops.
 *
 * Locking: the pre-check takes rx_lock briefly to read head/tail
 * coherently, then releases. telnet_rx_byte may then call
 * telnet_inject_rx which takes rx_lock again. Between the
 * pre-check and the inject, ring_free can only INCREASE (shell
 * task drains the ring; nobody else writes head/tail) — so a
 * pre-check that observed free >= 1 guarantees the inject sees
 * free >= 1. No silent drops.
 *
 * Runs in net_pump context. The shell task never calls this; it
 * only drains the ring (which makes more space, which the next
 * net_pump tick of shell_io_tcp_poll will turn into more
 * tcp_recved).
 */
static uint32_t drain_pending_pbuf(struct tcp_shell_ctx *ctx)
{
    uint32_t consumed = 0;

    while (ctx->pending_pbuf != NULL) {
        struct pbuf *head = ctx->pending_pbuf;
        const uint8_t *data = (const uint8_t *)head->payload;

        while (ctx->pending_offset < head->len) {
            irq_flags_t f = spin_lock_irqsave(&ctx->rx_lock);
            uint32_t free = ring_free_mask(ctx->rx_head, ctx->rx_tail,
                                           TCP_SHELL_RX_RING_MASK);
            spin_unlock_irqrestore(&ctx->rx_lock, f);
            if (free == 0) {
                return consumed;
            }

            telnet_rx_byte(&ctx->telnet, data[ctx->pending_offset]);
            ctx->pending_offset++;
            consumed++;
        }

        /* Head segment fully drained — advance to the next link in
         * the chain. pbuf_ref(next) before pbuf_free(head) so the
         * recursive free walk inside pbuf_free decrements next's
         * refcount back to 1 instead of all the way to 0. */
        struct pbuf *next = head->next;
        if (next) {
            pbuf_ref(next);
        }
        pbuf_free(head);
        ctx->pending_pbuf = next;
        ctx->pending_offset = 0;
    }

    return consumed;
}

/*
 * Drain pending bytes into the ring and then advance the recv
 * window for what fit. tcp_recved takes a u16_t length, so chunk
 * the call if the drain produced > 64 KB (rare under normal
 * TCP_WND <= 64 KB but possible if a cat'd chain was bigger).
 * Followed by tcp_output to flush any telnet responses the parser
 * queued during the drain — skipped when the drain consumed 0
 * bytes, since telnet_rx_byte never ran and could not have queued
 * anything new.
 */
static void recved_pending(struct tcp_shell_ctx *ctx, struct tcp_pcb *pcb)
{
    uint32_t consumed = drain_pending_pbuf(ctx);
    if (consumed == 0) {
        return;
    }
    while (consumed > 0) {
        uint16_t step = (consumed > UINT16_MAX) ? UINT16_MAX
                                                : (uint16_t)consumed;
        tcp_recved(pcb, step);
        consumed -= step;
    }
    if (ctx->pcb) {
        tcp_output(ctx->pcb);
    }
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
            uint8_t c = ctx->rx_buf[ctx->rx_tail & TCP_SHELL_RX_RING_MASK];
            ctx->rx_tail = (ctx->rx_tail + 1) & TCP_SHELL_RX_RING_MASK;
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
    uint8_t c = ctx->rx_buf[ctx->rx_tail & TCP_SHELL_RX_RING_MASK];
    ctx->rx_tail = (ctx->rx_tail + 1) & TCP_SHELL_RX_RING_MASK;
    spin_unlock_irqrestore(&ctx->rx_lock, flags);
    return (int)c;
}

/*
 * Batched RX: drain up to `max_len` bytes from the ring into `dst`
 * in a single lock cycle. Blocks (sleeps) waiting for the first byte
 * the same way `tcp_read_char` does, then returns whatever the ring
 * currently holds (could be 1, could be max_len). Returns -1 if the
 * session closes with the ring empty.
 *
 * Per #597: a 32 KB hex `xput chunk` line via the per-char
 * `tcp_read_char` path costs ~32K spin_lock_irqsave + memcpy(1) +
 * spin_unlock cycles. With this batched path and a caller-side
 * prefetch buffer (see `shell_read_command`), the same line costs
 * ~32 lock cycles (one per ~1024-byte refill), with the per-char
 * line-edit work happening against a local memory buffer.
 */
static int tcp_read_buf(struct shell_io *io, char *dst, int max_len)
{
    if (max_len <= 0) {
        return 0;
    }
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;

    for (;;) {
        irq_flags_t flags = spin_lock_irqsave(&ctx->rx_lock);
        uint32_t used = ring_used_mask(ctx->rx_head, ctx->rx_tail,
                                       TCP_SHELL_RX_RING_MASK);
        if (used > 0) {
            uint32_t want = (used < (uint32_t)max_len)
                          ? used
                          : (uint32_t)max_len;

            /* Two-step copy to handle ring wrap. tail_idx is the
             * masked-into-buffer position; `contiguous` is how many
             * bytes we can copy before hitting the wrap point. */
            uint32_t tail_idx = ctx->rx_tail & TCP_SHELL_RX_RING_MASK;
            uint32_t contiguous = TCP_SHELL_RX_RING_SIZE - tail_idx;
            uint32_t first = (want < contiguous) ? want : contiguous;

            for (uint32_t i = 0; i < first; i++) {
                dst[i] = (char)ctx->rx_buf[tail_idx + i];
            }
            uint32_t second = want - first;
            for (uint32_t i = 0; i < second; i++) {
                dst[first + i] = (char)ctx->rx_buf[i];
            }
            ctx->rx_tail = (ctx->rx_tail + want) & TCP_SHELL_RX_RING_MASK;

            spin_unlock_irqrestore(&ctx->rx_lock, flags);
            return (int)want;
        }

        bool closed = ctx->closed;
        spin_unlock_irqrestore(&ctx->rx_lock, flags);

        if (closed) {
            return -1;
        }
        /* Cooperative yield instead of sleep_ms(10) — net_pump task
         * runs on the same CPU as the shell task today, so we just
         * need to give it a turn, not wait a full timer tick. The
         * 10 ms sleep was the dominant per-block cost on `xput-bin`
         * uploads (#597 Option B) — LFS write was 1 ms but each
         * block needed multiple ring-empty wait cycles, each
         * costing 10 ms scheduler latency. yield() returns within
         * microseconds after net_pump processes pending pbufs.
         *
         * If shell tasks ever migrate off CPU 0 while net_pump
         * stays pinned, this loop could starve when net_pump
         * doesn't get scheduled. Today the shell task runs on
         * CPU 0 with net_pump alongside it (TASK_PRIORITY_IDLE),
         * so a yield reliably hands control back. */
        yield();
    }
}

/* Forward decl — defined in the lwIP-callback section below. The
 * tcp_write_buf timeout path calls it to mark the session closed
 * when the drain has stalled past the cap. The `reason` is a static
 * string literal recorded for the close-log INFO line. */
static void mark_closed(struct tcp_shell_ctx *ctx, const char *reason);

/* Cached timer-counter frequency. The timer is set up once at boot
 * and the frequency is constant thereafter, but every tcp_write_buf
 * call and every poll-cycle close-settling check needs to convert
 * ticks to milliseconds. Reading the frequency once and caching it
 * avoids a per-call register read (mrs CNTFRQ_EL0 on ARM64) that
 * adds up under heavy shell traffic.
 *
 * Lazy init: zero means "not yet cached"; the read is racy across
 * concurrent first callers but since the underlying frequency is
 * a fixed constant, every CPU computes the same value, so the
 * worst case is two redundant initialisations writing the same
 * uint64_t. That's benign for our usage. */
static uint64_t g_cached_timer_freq = 0;

static inline uint64_t shell_io_tcp_timer_freq(void)
{
    if (g_cached_timer_freq == 0) {
        g_cached_timer_freq = timer_get_frequency();
    }
    return g_cached_timer_freq;
}

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

/* Common bail path for tcp_write_buf when the configured wall-clock
 * cap (#536) has elapsed without ring-space progress. Marks the
 * session degraded so subsequent writes short-circuit, marks it
 * closed so the REPL unwinds, logs a one-shot WARN naming the
 * dropped byte count, and returns to the caller. Two separate
 * timeout-trigger sites in tcp_write_buf both delegate here — keeps
 * the warning text and the degrade/close ordering single-source. */
static void tcp_write_timeout_bail(struct tcp_shell_ctx *ctx, size_t dropped)
{
    ctx->write_degraded = true;
    uart_printf("[WARN] shell-tcp: session %u write timeout "
                "(>=%u ms drain stall, dropping %u bytes) "
                "— marking degraded, closing\n",
                (unsigned)ctx->session_id,
                (unsigned)g_write_timeout_ms,
                (unsigned)dropped);
    mark_closed(ctx, "write_timeout");
}

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
    const uint64_t freq = shell_io_tcp_timer_freq();
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
                tcp_write_timeout_bail(ctx, len - src);
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
                tcp_write_timeout_bail(ctx, len - src);
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

/* Echo decision driven by telnet IAC negotiation. The kernel offers
 * `WILL ECHO` on connect (telnet.c:282); peers that want server-side
 * echo reply `DO ECHO` (the flag stays set), peers that want local /
 * line-mode echo reply `DONT ECHO` (the flag clears via
 * handle_opt_dont in telnet.c). slm-put.py and any other line-
 * buffering tool always reply DONT, so this returns false for them
 * and the line-edit loop skips its per-char tcp_write_buf call —
 * which was capping bulk-upload throughput at ~30 KB/s through the
 * cumulative spinlock + tx-ring overhead. Interactive telnet clients
 * that *do* want server echo (some old terminals, or a peer that
 * negotiated DO ECHO explicitly) keep the flag set and continue to
 * see their input echoed. The check is a single bit read on the
 * per-session telnet parser — cheap to call once per input char. */
static bool tcp_echo_enabled(struct shell_io *io)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;
    return (ctx->telnet.negotiated_flags & TELNET_F_WILL_ECHO) != 0;
}

/*
 * Forward to the telnet parser's binary_mode flag. Atomic store
 * paired with the atomic load in telnet_rx_byte — both use
 * RELAXED ordering because the flag stands alone (no other data
 * is published with it) and on the SLM-OS shell-task / net_pump
 * cooperative-on-CPU-0 model the natural sequencing already
 * guarantees the writer's effect is visible before the next byte
 * arrives. The atomic primitives are kept for parity with
 * `xput_bin_active` in cmd_xput_bin (same single-flag pattern)
 * and so a future preemptive or multi-CPU shell-task layout
 * doesn't silently regress.
 */
static void tcp_set_binary_mode(struct shell_io *io, bool on)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)io->ctx;
    __atomic_store_n(&ctx->telnet.binary_mode, on, __ATOMIC_RELAXED);
}

/* -------------------------------------------------------------------------- */
/* lwIP callbacks — net_pump context                                          */
/* -------------------------------------------------------------------------- */

static void mark_closed(struct tcp_shell_ctx *ctx, const char *reason)
{
    irq_flags_t flags = spin_lock_irqsave(&ctx->rx_lock);
    if (!ctx->close_reason) {
        ctx->close_reason = reason;
    }
    ctx->closed = true;
    spin_unlock_irqrestore(&ctx->rx_lock, flags);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct tcp_shell_ctx *ctx = (struct tcp_shell_ctx *)arg;

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        mark_closed(ctx, "rx_err");
        return ERR_OK;
    }

    if (p == NULL) {
        /* Peer FIN. Drain any pending bytes one last time so the
         * shell task can read them before observing closed=true.
         * Whatever still doesn't fit gets freed in ctx_free; that's
         * the bound on stream-end byte loss (≤ ring size) and only
         * happens if the shell task was truly wedged. */
        if (ctx->pending_pbuf) {
            recved_pending(ctx, pcb);
        }
        mark_closed(ctx, "peer_fin");
        return ERR_OK;
    }

    /* Hand the new pbuf to the flow-control queue. If a previous
     * on_recv left bytes pending (ring was full), pbuf_cat appends
     * the new chain so byte order is preserved. Ownership transfers
     * to ctx — we don't pbuf_free p here (drain_pending_pbuf does
     * that as it advances through the chain). */
    if (ctx->pending_pbuf) {
        pbuf_cat(ctx->pending_pbuf, p);
    } else {
        ctx->pending_pbuf = p;
        ctx->pending_offset = 0;
    }

    /* Drain as much as fits. tcp_recved advances the recv window
     * only for the bytes that actually injected — anything still in
     * pending_pbuf stays out of the window slide, which is what
     * back-pressures the peer. */
    recved_pending(ctx, pcb);
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
    mark_closed(ctx, "peer_rst");
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
            c->close_reason = NULL;
            c->pcb     = NULL;
            c->rx_lock = (spinlock_t)SPINLOCK_INIT;
            c->tx_lock = (spinlock_t)SPINLOCK_INIT;
            c->rx_head = c->rx_tail = 0;
            c->pending_pbuf = NULL;
            c->pending_offset = 0;
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
    /* Free any flow-control pbuf chain still queued. Bytes in here
     * never reached the ring; the session is going away so they're
     * legitimately dropped. */
    if (ctx->pending_pbuf) {
        pbuf_free(ctx->pending_pbuf);
        ctx->pending_pbuf = NULL;
        ctx->pending_offset = 0;
    }
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
     * is about the timeout, not about what got queued.
     *
     * `static` rather than stack-local: TCP_SHELL_RING_SIZE is
     * now 16 KB (post-#581 throughput bump) so this would otherwise
     * consume ~16.4 KB in one local — well under the 64 KB
     * STACK_SIZE budget but wasteful when only the timeout path
     * is being exercised. The test runs single-threaded within the
     * kernel-test main task, so the static is safe and keeps stack
     * pressure low for any future test that happens to nest inside
     * this one. */
    static char buf[TCP_SHELL_RING_SIZE + 256];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 'x';

    uint64_t freq = shell_io_tcp_timer_freq();
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

    int rc;

    /* Scenario 1: settling timer just started. Poll must NOT free. */
    uint64_t now = timer_get_count();
    ctx->close_completed_ticks = now ? now : 1;
    shell_io_tcp_poll();
    if (!ctx->in_use) {
        /* Poll prematurely freed the slot — already gone, just
         * report and skip the second scenario. */
        rc = -2;
        goto out;
    }

    /* Scenario 2: rewind close_completed_ticks far enough that the
     * settle window has elapsed. Poll must free. */
    uint64_t freq = shell_io_tcp_timer_freq();
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
    rc = ctx->in_use ? -3 : 0;

out:
    /* `ctx_free` is idempotent (just clears `in_use` under
     * pool_lock), so calling it on an already-freed slot is a
     * harmless no-op. Single cleanup point keeps the test path
     * symmetric across success and both failure modes. */
    ctx_free(ctx);
    return rc;
}

/*
 * Test-only driver for the #537 leak-regression test.
 *
 * Drives one full close cycle with the heap-baseline snapshot
 * captured at "open time" so the post-settle delta measurement
 * is meaningful. (`shell_io_tcp_test_run_close_settling` above
 * pins `heap_used_at_open_bytes = 0`, which makes its measured
 * delta equal to whatever the lwIP heap holds at the moment —
 * fine for testing slot-release timing, useless for testing
 * leak detection because the baseline isn't real.)
 *
 * Cycle:
 *   1. Allocate a ctx.
 *   2. Snapshot `lwip_stats.mem.used` into `heap_used_at_open_bytes`
 *      and the per-MEMP-pool `used` counts into `memp_used_at_open[]`,
 *      mirroring what `shell_io_tcp_create` does for a real session.
 *   3. Call `tcp_shell_server_note_session_open` to balance the
 *      eventual close-counter bump (keeps the opened/closed invariant
 *      `closed <= opened` holding for any subsequent test that
 *      observes the stats struct).
 *   4. Mark closed + shell_done with a rewound settle timer.
 *   5. Drive `shell_io_tcp_poll` once. The poll path measures the
 *      close-time heap delta and routes through
 *      `tcp_shell_server_note_session_close`, which increments
 *      `leak_warnings` if the delta exceeds NET_SHELL_TCP_LEAK_THRESHOLD_BYTES.
 *
 * Returns 0 if the slot was freed (cycle completed), -1 if no
 * slot could be allocated, -2 if poll didn't free the slot.
 *
 * Caller (test_net.c) wraps this in a 50-cycle loop and asserts
 * `leak_warnings` doesn't increment. The cycle never touches a real
 * pcb / never calls `tcp_write`, so it can't detect leaks in
 * lwIP-side allocations — but it pins the kernel-side bookkeeping
 * (heap snapshot diff, MEMP attribution arithmetic, settle
 * release ordering) which is where the late-April leak in #537
 * lived.
 */
/*
 * Test-only driver for #597 / `tcp_read_buf`.
 *
 * Allocates a ctx, primes the RX ring with a known pattern, and
 * verifies tcp_read_buf drains all the bytes in a SINGLE call (not
 * one byte per call as `tcp_read_char` would). Returns 0 on
 * success, -1 if no slot could be allocated, -2 if the byte count
 * or content didn't match.
 */
int shell_io_tcp_test_run_read_buf_drains_ring(void)
{
    struct tcp_shell_ctx *ctx = ctx_alloc();
    if (!ctx) {
        return -1;
    }

    /* Prime the ring with a known pattern at the start of the
     * buffer (tail_idx == 0). Exercises the non-wrap path of
     * tcp_read_buf's two-step copy — `first` covers everything,
     * `second` is zero. The wrap path has its own test below. */
    static const char src[] =
        "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUV";
    const uint32_t n = (uint32_t)sizeof(src) - 1;     /* drop NUL */
    for (uint32_t i = 0; i < n; i++) {
        ctx->rx_buf[i] = (uint8_t)src[i];
    }
    ctx->rx_head = n;
    ctx->rx_tail = 0;
    ctx->closed = false;

    char dst[128];
    int got = tcp_read_buf(&ctx->io, dst, (int)sizeof(dst));

    int rc = (got == (int)n && memcmp(dst, src, n) == 0) ? 0 : -2;

    ctx_free(ctx);
    return rc;
}

/*
 * Test-only driver for the wrap branch of `tcp_read_buf` (#597
 * review follow-up).
 *
 * The non-wrap test above stays below TCP_SHELL_RING_SIZE with
 * tail_idx==0 so `first` covers the whole copy. This variant
 * positions tail_idx near the END of the ring buffer and writes
 * data that crosses the wrap boundary, forcing the two-step copy:
 * `first` from `tail_idx..ring_end`, then `second` from `ring_start
 * ..(want - first)`. Prior to tcp_read_buf this code path was
 * unreachable (per-char reads always touched one slot at a time),
 * so a bug in either copy length or the wrap-around tail update
 * would silently corrupt the assembled buffer. Returns 0 on
 * success, -1 if no slot, -2 on count or content mismatch.
 */
int shell_io_tcp_test_run_read_buf_wrap(void)
{
    struct tcp_shell_ctx *ctx = ctx_alloc();
    if (!ctx) {
        return -1;
    }

    /* Position tail_idx 16 bytes from ring end. Total payload 32
     * bytes splits as 16 (pre-wrap) + 16 (post-wrap). Both halves
     * non-trivial so an off-by-one in either step shows up
     * immediately as a content miscompare. */
    const uint32_t span_first  = 16;
    const uint32_t span_second = 16;
    const uint32_t total       = span_first + span_second;

    static uint8_t pattern[32];
    for (uint32_t i = 0; i < total; i++) {
        pattern[i] = (uint8_t)(0xC0 + i);   /* deterministic, non-zero, distinct */
    }

    /* Place `pattern` so the first 16 bytes land at the end of the
     * ring buffer and the next 16 land at the start. tail_idx then
     * points at the first byte of the payload. */
    const uint32_t tail_idx = TCP_SHELL_RX_RING_SIZE - span_first;
    for (uint32_t i = 0; i < span_first; i++) {
        ctx->rx_buf[tail_idx + i] = pattern[i];
    }
    for (uint32_t i = 0; i < span_second; i++) {
        ctx->rx_buf[i] = pattern[span_first + i];
    }
    ctx->rx_tail = tail_idx;
    ctx->rx_head = tail_idx + total;        /* pre-mask wraps via & MASK on read */
    ctx->closed  = false;

    uint8_t dst[64];
    int got = tcp_read_buf(&ctx->io, (char *)dst, (int)sizeof(dst));

    int rc = (got == (int)total && memcmp(dst, pattern, total) == 0) ? 0 : -2;

    /* Verify the post-read tail landed in the post-wrap region —
     * confirms the `(rx_tail + want) & MASK` advance handled the
     * wrap, not just the copy. */
    if (rc == 0 && ctx->rx_tail != span_second) {
        rc = -2;
    }

    ctx_free(ctx);
    return rc;
}

int shell_io_tcp_test_run_clean_close_cycle(void)
{
    struct tcp_shell_ctx *ctx = ctx_alloc();
    if (!ctx) {
        return -1;
    }

    /* Snapshot the heap baseline as a real session would. */
#if MEM_STATS
    ctx->heap_used_at_open_bytes = (uint32_t)lwip_stats.mem.used;
#else
    ctx->heap_used_at_open_bytes = 0;
#endif
#if MEMP_STATS
    for (int i = 0; i < MEMP_MAX; i++) {
        ctx->memp_used_at_open[i] = lwip_stats.memp[i]
            ? (uint32_t)lwip_stats.memp[i]->used
            : 0u;
    }
#endif

    ctx->shell_done = true;
    ctx->pcb = NULL;
    ctx->session_id = 0xC10C1057u;   /* literal: "CLOC1057" — close-test */

    /* Pair the close-counter increment that the poll path will fire
     * (via tcp_shell_server_note_session_close) with a matching
     * sessions_opened bump, so the loop caller doesn't underflow
     * `active = opened - closed` on the stats struct. The
     * `stats_invariants` test in test_net.c asserts opened >=
     * closed; this keeps that holding even after N cycles. */
    tcp_shell_server_note_session_open(ctx->session_id);

    /* Rewind close_completed_ticks past the settle window so the
     * poll path's measure-and-free branch fires on the first call. */
    uint64_t freq = shell_io_tcp_timer_freq();
    uint64_t settle_ticks =
        (freq * (uint64_t)(TCP_SHELL_CLOSE_SETTLE_MS + 100)) / 1000ULL;
    uint64_t cur = timer_get_count();
    ctx->close_completed_ticks = (cur > settle_ticks)
        ? (cur - settle_ticks)
        : 1ULL;

    shell_io_tcp_poll();

    int rc = ctx->in_use ? -2 : 0;

    /* Idempotent — see close-settling test for the rationale. */
    ctx_free(ctx);
    return rc;
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

    ctx->io.read_char       = tcp_read_char;
    ctx->io.try_read_char   = tcp_try_read_char;
    ctx->io.write           = tcp_write_buf;
    ctx->io.flush           = tcp_flush;
    ctx->io.close           = tcp_close_io;
    ctx->io.is_open         = tcp_is_open;
    ctx->io.echo_enabled    = tcp_echo_enabled;
    ctx->io.read_buf        = tcp_read_buf;
    ctx->io.set_binary_mode = tcp_set_binary_mode;
    ctx->io.ctx             = ctx;

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

    /* Snapshot per-MEMP-pool used counts so the close path can
     * attribute any heap-delta to a specific pool (#537). Stored
     * as uint32_t so the snapshot is lossless regardless of
     * whether `mem_size_t` is u16_t or u32_t on this build. */
#if MEMP_STATS
    for (int i = 0; i < MEMP_MAX; i++) {
        ctx->memp_used_at_open[i] = lwip_stats.memp[i]
            ? (uint32_t)lwip_stats.memp[i]->used
            : 0u;
    }
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
    mark_closed(ctx, "shell_exit");
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
            if (!ctx->close_reason) {
                ctx->close_reason = "kicked";
            }
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

        /* RX flow control: pick up any bytes that didn't fit in the
         * ring last time on_recv ran. The shell task may have drained
         * the ring since then, so there's room now. Done before the
         * TX drain so the recv-window slide goes out in the same
         * tcp_output cycle as any TX bytes the shell-task queued. */
        if (ctx->pcb && ctx->pending_pbuf) {
            recved_pending(ctx, ctx->pcb);
        }

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
            uint64_t freq = shell_io_tcp_timer_freq();
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

            /* Compute per-MEMP-pool deltas to attribute the leak to
             * a specific allocator (#537 follow-up). Build a short
             * string like " PBUF_POOL+2 TCP_SEG+1 PBUF+0" — only
             * pools with non-zero deltas appear, capped to fit the
             * fixed buffer. */
            char pool_attribution[160];
            pool_attribution[0] = '\0';
#if MEMP_STATS
            size_t attr_off = 0;
            for (int p = 0;
                 p < MEMP_MAX && attr_off + 32 < sizeof(pool_attribution);
                 p++) {
                if (!lwip_stats.memp[p]) continue;
                int32_t pd = (int32_t)lwip_stats.memp[p]->used
                           - (int32_t)ctx->memp_used_at_open[p];
                if (pd == 0) continue;
                const char *name = lwip_stats.memp[p]->name
                    ? lwip_stats.memp[p]->name : "?";
                /* kprintf only supports `-` and `0` flags (see
                 * kprintf.c). `%+d` is silently broken — it prints
                 * "%+d" literally, doesn't consume the int arg, and
                 * the integer leaks into the next %s read of the
                 * outer caller. Print the `+` manually for non-
                 * negative values; %d already prefixes negatives
                 * with `-`. (pd == 0 is filtered above.) */
                const char *sign = (pd > 0) ? "+" : "";
                int n = uart_snprintf(pool_attribution + attr_off,
                                      sizeof(pool_attribution) - attr_off,
                                      " %s%s%d", name, sign, (int)pd);
                if (n <= 0) break;
                attr_off += (size_t)n;
            }
#endif
            tcp_shell_server_note_session_close(ctx->session_id, delta,
                                                pool_attribution[0]
                                                    ? pool_attribution
                                                    : NULL,
                                                (const char *)
                                                    ctx->close_reason);
            ctx_free(ctx);
        }
    }
}
