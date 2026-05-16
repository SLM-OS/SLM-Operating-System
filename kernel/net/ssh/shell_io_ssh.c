/*
 * shell_io_ssh.c - shell_io backend wrapping a wolfSSH stream.
 *
 * After wolfSSH_accept completes successfully, sshd_session_task
 * creates one of these and binds it to a shell_session — the same
 * REPL that drives UART/telnet then runs over the encrypted SSH
 * stream without any modifications.
 *
 * Read path:  shell REPL → read_char → wolfSSH_stream_read →
 *             (internally) our wolf_io_recv → ring buffer drained
 *             by net_pump's tcp_recv callback.
 *
 * Write path: shell command output → write → wolfSSH_stream_send →
 *             (internally) our wolf_io_send → tcp_write → lwIP.
 *
 * Blocking semantics: wolfSSH_stream_read returns `WS_WANT_READ`
 * when no plaintext is available; we yield-loop on that with
 * `sleep_ms(10)` until either data arrives, the peer disconnects,
 * or the shell task is being torn down. `try_read_char` translates
 * the same condition to "no data" without blocking.
 */

#include "shell_io.h"

#include "kbuf.h"
#include "rng.h"
#include "shell_session.h"
#include "spinlock.h"
#include "string.h"
#include "task.h"
#include "timer.h"
#include "uart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wolfssh/ssh.h>
#include <wolfssh/error.h>

struct ssh_io_ctx {
    WOLFSSH         *ssh;        /* borrowed — owner is sshd_conn */
    spinlock_t       lock;       /* serialises stream_send writes */
    volatile bool    closed;     /* set by close() or peer disconnect */
    bool             binary_mode;
    char             read_holdover;
    bool             have_holdover;
};

/* ---------------------------------------------------------------- */
/* Vtable ops                                                        */
/* ---------------------------------------------------------------- */

static int io_read_buf(struct shell_io *io, char *dst, int max_len)
{
    struct ssh_io_ctx *c = (struct ssh_io_ctx *)io->ctx;
    if (!c || c->closed) return -1;
    if (max_len <= 0)    return 0;

    /* Drain the one-byte holdover from try_read_char first. */
    int wrote = 0;
    if (c->have_holdover) {
        dst[wrote++] = c->read_holdover;
        c->have_holdover = false;
        if (wrote == max_len) return wrote;
    }

    for (;;) {
        int rc = wolfSSH_stream_read(c->ssh,
                                     (uint8_t *)dst + wrote,
                                     (uint32_t)(max_len - wrote));
        if (rc > 0) return wrote + rc;

        int err = wolfSSH_get_error(c->ssh);
        if (err == WS_WANT_READ || err == WS_WANT_WRITE) {
            if (wrote > 0) return wrote;
            sleep_ms(10);
            if (c->closed) return (wrote > 0) ? wrote : -1;
            continue;
        }
        /* Anything else is a disconnect / fatal. */
        c->closed = true;
        return (wrote > 0) ? wrote : -1;
    }
}

static int io_read_char(struct shell_io *io)
{
    char b;
    int n = io_read_buf(io, &b, 1);
    if (n <= 0) return -1;
    return (int)(uint8_t)b;
}

static int io_try_read_char(struct shell_io *io)
{
    struct ssh_io_ctx *c = (struct ssh_io_ctx *)io->ctx;
    if (!c || c->closed) return -1;

    if (c->have_holdover) {
        int b = (int)(uint8_t)c->read_holdover;
        c->have_holdover = false;
        return b;
    }

    char b;
    int rc = wolfSSH_stream_read(c->ssh, (uint8_t *)&b, 1u);
    if (rc == 1) return (int)(uint8_t)b;
    if (rc > 0)  return (int)(uint8_t)b;   /* defensive — shouldn't happen */

    int err = wolfSSH_get_error(c->ssh);
    if (err == WS_WANT_READ || err == WS_WANT_WRITE) return -1;
    c->closed = true;
    return -1;
}

static void io_write(struct shell_io *io, const char *buf, size_t len)
{
    struct ssh_io_ctx *c = (struct ssh_io_ctx *)io->ctx;
    if (!c || c->closed || len == 0u) return;

    /* Serialise concurrent writers on the same session. The shell
     * task is the only writer in production today, but commands that
     * publish from background tasks (`top` watchers, telemetry feeds)
     * could trip a tear during stream_send's internal MAC append. */
    irq_flags_t flags = spin_lock_irqsave(&c->lock);

    size_t off = 0;
    while (off < len) {
        uint32_t chunk = (uint32_t)((len - off) < 1024u ? (len - off) : 1024u);
        int rc = wolfSSH_stream_send(c->ssh,
                                     (uint8_t *)(buf + off),
                                     chunk);
        if (rc > 0) {
            off += (size_t)rc;
            continue;
        }
        int err = wolfSSH_get_error(c->ssh);
        if (err == WS_WANT_WRITE || err == WS_WANT_READ) {
            /* The wolf_io_send callback hit pcb->snd_buf == 0. Yield
             * briefly and retry. */
            spin_unlock_irqrestore(&c->lock, flags);
            sleep_ms(5);
            if (c->closed) return;
            flags = spin_lock_irqsave(&c->lock);
            continue;
        }
        c->closed = true;
        break;
    }

    spin_unlock_irqrestore(&c->lock, flags);
}

static void io_flush(struct shell_io *io)
{
    (void)io;
    /* wolfSSH_stream_send flushes inside wolfssh; nothing to do here. */
}

static void io_close(struct shell_io *io)
{
    struct ssh_io_ctx *c = (struct ssh_io_ctx *)io->ctx;
    if (!c) return;
    c->closed = true;
}

static bool io_is_open(struct shell_io *io)
{
    struct ssh_io_ctx *c = (struct ssh_io_ctx *)io->ctx;
    return (c != NULL) && !c->closed;
}

static bool io_echo_enabled(struct shell_io *io)
{
    (void)io;
    /* OpenSSH client does its own line editing in cooked mode —
     * but in this build we run cooked-line on the kernel side too
     * (no PTY emulation per #199c's documented scope). Echoing
     * locally matches what the operator expects from a familiar
     * shell. */
    return true;
}

static void io_set_binary_mode(struct shell_io *io, bool on)
{
    struct ssh_io_ctx *c = (struct ssh_io_ctx *)io->ctx;
    if (!c) return;
    c->binary_mode = on;
    /* SSH is already a binary-clean stream — flag is informational
     * for shell-side decisions (e.g. xput-bin disabling CRLF
     * normalisation). */
}

/* ---------------------------------------------------------------- */
/* Constructor / destructor                                          */
/* ---------------------------------------------------------------- */

struct shell_io *shell_io_ssh_create(WOLFSSH *ssh)
{
    if (!ssh) return NULL;

    /* Pull the shell_io + ctx from the byte-level kernel allocator so
     * the per-session pair survives an out-of-pool condition cleanly. */
    struct shell_io   *io = kbuf_alloc(sizeof(*io));
    struct ssh_io_ctx *c  = kbuf_alloc(sizeof(*c));
    if (!io || !c) {
        if (io) kbuf_free(io);
        if (c)  kbuf_free(c);
        return NULL;
    }

    c->ssh          = ssh;
    c->lock         = (spinlock_t)SPINLOCK_INIT;
    c->closed       = false;
    c->binary_mode  = false;
    c->have_holdover = false;
    c->read_holdover = 0;

    io->read_char       = io_read_char;
    io->try_read_char   = io_try_read_char;
    io->write           = io_write;
    io->flush           = io_flush;
    io->close           = io_close;
    io->is_open         = io_is_open;
    io->echo_enabled    = io_echo_enabled;
    io->read_buf        = io_read_buf;
    io->set_binary_mode = io_set_binary_mode;
    io->ctx             = c;
    return io;
}

void shell_io_ssh_destroy(struct shell_io *io)
{
    if (!io) return;
    if (io->ctx) kbuf_free(io->ctx);
    kbuf_free(io);
}

/* Signal that the underlying WOLFSSH stream is gone (peer disconnect,
 * connection RST). The shell task notices via read_char returning -1
 * on the next iteration. */
void shell_io_ssh_mark_closed(struct shell_io *io)
{
    struct ssh_io_ctx *c = (io != NULL) ? (struct ssh_io_ctx *)io->ctx : NULL;
    if (c) c->closed = true;
}
