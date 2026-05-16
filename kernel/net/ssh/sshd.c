/*
 * sshd.c - SSH daemon implementation.
 *
 * Listener pattern: lwIP raw API single accept pcb (mirrors
 * tcp_shell_server.c). Each accepted connection allocates a
 * `sshd_conn` slot with a per-connection RX ring buffer; a kernel
 * task is spawned to drive `wolfSSH_accept` to completion. On
 * success the connection is closed immediately — #199c is where the
 * shell channel gets bound to a `shell_session`.
 *
 * IO model:
 *
 *   - net_pump task (lwIP TX/RX) writes inbound bytes into the
 *     per-conn ring buffer from the `tcp_recv` callback under
 *     `conn->lock`.
 *   - The session task calls `wolfSSH_accept`, which in turn calls
 *     our `wolf_io_recv` / `wolf_io_send` callbacks. `wolf_io_recv`
 *     drains the ring buffer and returns `WS_CBIO_ERR_WANT_READ`
 *     when empty (wolfSSH yield-loops on this); `wolf_io_send`
 *     `tcp_write` + `tcp_output` on the pcb.
 *
 * Host key (#199a interim):
 *
 *   - Ephemeral Ed25519 keypair generated at `sshd_start` time via
 *     `wc_ed25519_make_key` seeded by the kernel RNG.
 *   - #199b replaces this with a persisted key in
 *     `/mnt/files/etc/ssh/host_ed25519_key`.
 */

#include "sshd.h"

#include "config.h"
#include "rng.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"
#include "task.h"
#include "timer.h"
#include "uart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/tcp.h"
#include "lwip/pbuf.h"

#include <wolfssh/ssh.h>
#include <wolfssh/error.h>
#include <wolfssh/internal.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/random.h>

#define SSHD_RX_RING_BYTES   8192u

/* ---------------------------------------------------------------- */
/* Per-connection state                                              */
/* ---------------------------------------------------------------- */

struct sshd_conn {
    bool             in_use;
    struct tcp_pcb  *pcb;
    WOLFSSH         *ssh;

    /* RX ring buffer — filled by net_pump tcp_recv callback under
     * `lock`, drained by the session task's wolf_io_recv. */
    spinlock_t       lock;
    uint8_t          rx[SSHD_RX_RING_BYTES];
    uint16_t         rx_head;       /* write index */
    uint16_t         rx_tail;       /* read index */
    bool             peer_closed;   /* FIN received or RST */

    /* Stats */
    uint32_t         session_id;
};

/* ---------------------------------------------------------------- */
/* Module state                                                      */
/* ---------------------------------------------------------------- */

static spinlock_t          g_mod_lock = SPINLOCK_INIT;
static struct tcp_pcb     *g_listen_pcb;
static uint16_t            g_listen_port;
static WOLFSSH_CTX        *g_ctx;
static struct sshd_conn    g_conns[SSHD_MAX_SESSIONS];

static volatile uint32_t   g_accepted;
static volatile uint32_t   g_kex_completed;
static volatile uint32_t   g_kex_failed;
static volatile uint32_t   g_active;

/* ---------------------------------------------------------------- */
/* Ephemeral host key (#199a interim — replaced by #199b)            */
/* ---------------------------------------------------------------- */

/* Ed25519 keypair encoded as the 64-byte "private+public" buffer
 * wolfSSH's UsePrivateKey_buffer expects for `WOLFSSH_FORMAT_RAW`.
 * Filled by `generate_ephemeral_hostkey`. */
static uint8_t  g_hostkey_raw[ED25519_KEY_SIZE + ED25519_PUB_KEY_SIZE];
static uint32_t g_hostkey_raw_len;

static int generate_ephemeral_hostkey(void)
{
    WC_RNG    rng;
    ed25519_key key;
    int rc = wc_InitRng(&rng);
    if (rc != 0) return SSHD_E_NO_HOSTKEY;

    rc = wc_ed25519_init(&key);
    if (rc != 0) { wc_FreeRng(&rng); return SSHD_E_NO_HOSTKEY; }

    rc = wc_ed25519_make_key(&rng, ED25519_KEY_SIZE, &key);
    if (rc != 0) goto out;

    /* Export private + public side-by-side for wolfSSH's raw-format
     * importer. */
    uint32_t priv_len = ED25519_KEY_SIZE;
    uint32_t pub_len  = ED25519_PUB_KEY_SIZE;
    rc = wc_ed25519_export_key(&key,
                               g_hostkey_raw,                          &priv_len,
                               g_hostkey_raw + ED25519_KEY_SIZE,       &pub_len);
    if (rc == 0) {
        g_hostkey_raw_len = priv_len + pub_len;
    }
out:
    wc_ed25519_free(&key);
    wc_FreeRng(&rng);
    return (rc == 0) ? SSHD_OK : SSHD_E_NO_HOSTKEY;
}

/* ---------------------------------------------------------------- */
/* RX ring buffer helpers                                            */
/* ---------------------------------------------------------------- */

static uint16_t ring_used_locked(const struct sshd_conn *c)
{
    return (uint16_t)((c->rx_head - c->rx_tail) & (SSHD_RX_RING_BYTES - 1u));
}

static uint16_t ring_free_locked(const struct sshd_conn *c)
{
    /* One byte reserved to distinguish full from empty. */
    return (uint16_t)(SSHD_RX_RING_BYTES - 1u - ring_used_locked(c));
}

static size_t ring_push(struct sshd_conn *c, const uint8_t *src, size_t n)
{
    irq_flags_t flags = spin_lock_irqsave(&c->lock);
    size_t can = ring_free_locked(c);
    if (n > can) n = can;
    for (size_t i = 0; i < n; i++) {
        c->rx[c->rx_head] = src[i];
        c->rx_head = (uint16_t)((c->rx_head + 1u) & (SSHD_RX_RING_BYTES - 1u));
    }
    spin_unlock_irqrestore(&c->lock, flags);
    return n;
}

static size_t ring_pop(struct sshd_conn *c, uint8_t *dst, size_t n,
                      bool *out_peer_closed)
{
    irq_flags_t flags = spin_lock_irqsave(&c->lock);
    size_t avail = ring_used_locked(c);
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; i++) {
        dst[i] = c->rx[c->rx_tail];
        c->rx_tail = (uint16_t)((c->rx_tail + 1u) & (SSHD_RX_RING_BYTES - 1u));
    }
    bool closed = c->peer_closed && (ring_used_locked(c) == 0u);
    spin_unlock_irqrestore(&c->lock, flags);
    if (out_peer_closed) *out_peer_closed = closed;
    return n;
}

/* ---------------------------------------------------------------- */
/* wolfSSH IO callbacks                                              */
/* ---------------------------------------------------------------- */

static int wolf_io_recv(WOLFSSH *ssh, void *buf, uint32_t sz, void *ctx)
{
    (void)ssh;
    struct sshd_conn *c = (struct sshd_conn *)ctx;
    if (!c) return WS_CBIO_ERR_GENERAL;

    bool peer_closed = false;
    size_t n = ring_pop(c, (uint8_t *)buf, (size_t)sz, &peer_closed);
    if (n > 0) return (int)n;
    if (peer_closed) return WS_CBIO_ERR_CONN_RST;
    return WS_CBIO_ERR_WANT_READ;
}

static int wolf_io_send(WOLFSSH *ssh, void *buf, uint32_t sz, void *ctx)
{
    (void)ssh;
    struct sshd_conn *c = (struct sshd_conn *)ctx;
    if (!c || !c->pcb) return WS_CBIO_ERR_CONN_RST;

    /* tcp_sndbuf reports the snd_buf headroom in the pcb. If it's
     * zero we ask wolfSSH to come back later. */
    uint16_t cap = tcp_sndbuf(c->pcb);
    if (cap == 0u) return WS_CBIO_ERR_WANT_WRITE;

    uint16_t take = (sz < cap) ? (uint16_t)sz : cap;
    err_t e = tcp_write(c->pcb, buf, take, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) return WS_CBIO_ERR_GENERAL;
    e = tcp_output(c->pcb);
    if (e != ERR_OK) return WS_CBIO_ERR_GENERAL;
    return (int)take;
}

/* ---------------------------------------------------------------- */
/* lwIP TCP callbacks                                                */
/* ---------------------------------------------------------------- */

static void conn_close_locked(struct sshd_conn *c)
{
    if (c->ssh) {
        wolfSSH_free(c->ssh);
        c->ssh = NULL;
    }
    if (c->pcb) {
        tcp_recv(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        tcp_arg(c->pcb, NULL);
        tcp_close(c->pcb);
        c->pcb = NULL;
    }
    c->in_use = false;
    if (g_active > 0) g_active--;
}

static err_t on_tcp_recv(void *arg, struct tcp_pcb *pcb,
                         struct pbuf *p, err_t err)
{
    struct sshd_conn *c = (struct sshd_conn *)arg;
    if (!c) {
        if (p) pbuf_free(p);
        return ERR_VAL;
    }
    if (err != ERR_OK || p == NULL) {
        /* Peer FIN or RX error → flag closed; the session task drains
         * what's already in the ring and then sees CBIO_ERR_CONN_RST. */
        if (p) pbuf_free(p);
        irq_flags_t flags = spin_lock_irqsave(&c->lock);
        c->peer_closed = true;
        spin_unlock_irqrestore(&c->lock, flags);
        return ERR_OK;
    }

    struct pbuf *q = p;
    size_t consumed = 0;
    while (q != NULL) {
        size_t pushed = ring_push(c, (const uint8_t *)q->payload,
                                  (size_t)q->len);
        consumed += pushed;
        if (pushed < q->len) break;   /* ring full — peer will retry */
        q = q->next;
    }
    if (consumed > 0) {
        tcp_recved(pcb, (uint16_t)consumed);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void on_tcp_err(void *arg, err_t err)
{
    (void)err;
    struct sshd_conn *c = (struct sshd_conn *)arg;
    if (!c) return;
    /* pcb has been freed by lwIP at this point. */
    irq_flags_t flags = spin_lock_irqsave(&c->lock);
    c->pcb = NULL;
    c->peer_closed = true;
    spin_unlock_irqrestore(&c->lock, flags);
}

/* ---------------------------------------------------------------- */
/* Session task — drives wolfSSH_accept                              */
/* ---------------------------------------------------------------- */

static void sshd_session_task(void *arg)
{
    struct sshd_conn *c = (struct sshd_conn *)arg;
    if (!c || !c->ssh) {
        task_exit();
        return;
    }

    /* Yield-loop on wolfSSH_accept until KEX completes or fatal error. */
    for (;;) {
        int ret = wolfSSH_accept(c->ssh);
        if (ret == WS_SUCCESS) {
            g_kex_completed++;
            uart_printf("[SSHD] conn %u: KEX complete — no shell channel "
                        "(refused; #199c scope)\r\n",
                        (unsigned)c->session_id);
            break;
        }
        int err = wolfSSH_get_error(c->ssh);
        if (err == WS_WANT_READ || err == WS_WANT_WRITE) {
            sleep_ms(10);
            /* If peer hung up while we were waiting, get out. */
            irq_flags_t flags = spin_lock_irqsave(&c->lock);
            bool closed = c->peer_closed && (ring_used_locked(c) == 0u);
            spin_unlock_irqrestore(&c->lock, flags);
            if (closed) {
                g_kex_failed++;
                uart_printf("[SSHD] conn %u: peer disconnected during KEX\r\n",
                            (unsigned)c->session_id);
                break;
            }
            continue;
        }
        /* Any other error is fatal. */
        g_kex_failed++;
        uart_printf("[SSHD] conn %u: wolfSSH_accept fatal err=%d (%s)\r\n",
                    (unsigned)c->session_id,
                    err, wolfSSH_get_error_name(c->ssh));
        break;
    }

    /* Either way, close the connection for #199a. #199c will instead
     * branch on success → shell session, failure → close. */
    irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
    conn_close_locked(c);
    spin_unlock_irqrestore(&g_mod_lock, flags);

    task_exit();
}

/* ---------------------------------------------------------------- */
/* Accept callback — runs on net_pump                                */
/* ---------------------------------------------------------------- */

static struct sshd_conn *alloc_conn_locked(void)
{
    for (size_t i = 0; i < SSHD_MAX_SESSIONS; i++) {
        if (!g_conns[i].in_use) {
            return &g_conns[i];
        }
    }
    return NULL;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;

    tcp_backlog_accepted(newpcb);

    irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
    struct sshd_conn *c = alloc_conn_locked();
    if (!c) {
        spin_unlock_irqrestore(&g_mod_lock, flags);
        const char *msg = "sshd: pool exhausted\r\n";
        (void)tcp_write(newpcb, msg, (uint16_t)strlen(msg),
                        TCP_WRITE_FLAG_COPY);
        tcp_output(newpcb);
        tcp_close(newpcb);
        return ERR_MEM;
    }

    /* Initialize the slot. lock is the per-conn spin; mod-lock was
     * already taken above for the slot allocation; release it before
     * we touch the wolfSSH side (no wolfSSH state escapes this slot). */
    c->in_use      = true;
    c->pcb         = newpcb;
    c->ssh         = NULL;
    c->rx_head     = 0;
    c->rx_tail     = 0;
    c->peer_closed = false;
    c->session_id  = ++g_accepted;
    c->lock        = (spinlock_t)SPINLOCK_INIT;
    g_active++;
    spin_unlock_irqrestore(&g_mod_lock, flags);

    /* Create the wolfSSH connection on top of the shared CTX. */
    c->ssh = wolfSSH_new(g_ctx);
    if (!c->ssh) {
        irq_flags_t f2 = spin_lock_irqsave(&g_mod_lock);
        conn_close_locked(c);
        spin_unlock_irqrestore(&g_mod_lock, f2);
        return ERR_MEM;
    }
    wolfSSH_SetIOReadCtx(c->ssh,  c);
    wolfSSH_SetIOWriteCtx(c->ssh, c);

    /* Wire lwIP-side TCP callbacks now. */
    tcp_arg(newpcb,  c);
    tcp_recv(newpcb, on_tcp_recv);
    tcp_err(newpcb,  on_tcp_err);

    /* Spawn the per-conn task to drive wolfSSH_accept. */
    char name[TASK_NAME_LEN];
    /* Manual integer formatting — uart_snprintf is overkill here. */
    name[0] = 's'; name[1] = 's'; name[2] = 'h'; name[3] = 'd'; name[4] = '-';
    uint32_t id = c->session_id;
    int      ni = 5;
    char     digits[8];
    int      di = 0;
    if (id == 0u) digits[di++] = '0';
    while (id > 0u && di < 8) { digits[di++] = (char)('0' + (id % 10u)); id /= 10u; }
    while (di > 0 && ni < TASK_NAME_LEN - 1) name[ni++] = digits[--di];
    name[ni] = '\0';

    struct task *t = task_create_with_priority(name, sshd_session_task,
                                               c, TASK_PRIORITY_IDLE);
    if (!t) {
        irq_flags_t f2 = spin_lock_irqsave(&g_mod_lock);
        conn_close_locked(c);
        spin_unlock_irqrestore(&g_mod_lock, f2);
        return ERR_MEM;
    }
    task_set_affinity(t, 0);
    scheduler_add_task(t);

    return ERR_OK;
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

int sshd_start(uint16_t port)
{
    if (g_listen_pcb) {
        return (g_listen_port == port) ? SSHD_OK : SSHD_E_ALREADY_UP;
    }
    if (port == 0) port = SSHD_DEFAULT_PORT;

    /* Initialize wolfSSH (idempotent). */
    if (wolfSSH_Init() != WS_SUCCESS) {
        return SSHD_E_WOLF_INIT;
    }

    /* Generate the ephemeral host key (#199b replaces with a
     * persisted key). */
    if (g_hostkey_raw_len == 0u) {
        int rc = generate_ephemeral_hostkey();
        if (rc != SSHD_OK) return rc;
    }

    /* Create the shared SERVER context. */
    if (!g_ctx) {
        g_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
        if (!g_ctx) return SSHD_E_WOLF_INIT;

        wolfSSH_SetIORecv(g_ctx, wolf_io_recv);
        wolfSSH_SetIOSend(g_ctx, wolf_io_send);

        if (wolfSSH_CTX_UsePrivateKey_buffer(g_ctx,
                                             g_hostkey_raw,
                                             g_hostkey_raw_len,
                                             WOLFSSH_FORMAT_RAW) != WS_SUCCESS) {
            wolfSSH_CTX_free(g_ctx);
            g_ctx = NULL;
            return SSHD_E_NO_HOSTKEY;
        }
    }

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return SSHD_E_LISTEN_FAIL;

    ip_addr_t any = {0};
    IP4_ADDR(&any, 0, 0, 0, 0);
    if (tcp_bind(pcb, &any, port) != ERR_OK) {
        tcp_close(pcb);
        return SSHD_E_LISTEN_FAIL;
    }
    struct tcp_pcb *lpcb = tcp_listen_with_backlog(pcb, SSHD_MAX_SESSIONS);
    if (!lpcb) {
        tcp_close(pcb);
        return SSHD_E_LISTEN_FAIL;
    }
    tcp_arg(lpcb, NULL);
    tcp_accept(lpcb, on_accept);

    g_listen_pcb  = lpcb;
    g_listen_port = port;
    uart_printf("[SSHD] listening on port %u\r\n", (unsigned)port);
    return SSHD_OK;
}

int sshd_stop(void)
{
    if (!g_listen_pcb) return SSHD_OK;

    tcp_close(g_listen_pcb);
    g_listen_pcb  = NULL;
    g_listen_port = 0;

    /* Tear down any open sessions (peer will see RST). */
    irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
    for (size_t i = 0; i < SSHD_MAX_SESSIONS; i++) {
        if (g_conns[i].in_use) {
            conn_close_locked(&g_conns[i]);
        }
    }
    spin_unlock_irqrestore(&g_mod_lock, flags);

    uart_printf("[SSHD] stopped\r\n");
    return SSHD_OK;
}

void sshd_get_stats(struct sshd_stats *out)
{
    if (!out) return;
    out->running              = (g_listen_pcb != NULL);
    out->port                 = g_listen_port;
    out->connections_accepted = g_accepted;
    out->kex_completed        = g_kex_completed;
    out->kex_failed           = g_kex_failed;
    out->active               = g_active;
}
