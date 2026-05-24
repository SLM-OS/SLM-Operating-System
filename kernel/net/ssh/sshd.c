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
 * Host key:
 *
 *   - Ed25519 keypair persisted under
 *     `/mnt/files/etc/ssh/host_ed25519_key` — see
 *     kernel/net/ssh/host_key.c for the read/generate/persist flow.
 *   - `sshd_start` calls `host_key_load_or_generate` once, caches the
 *     [seed||pub] layout in `g_hostkey_raw`, and re-materialises a
 *     PKCS#8 DER form for `wolfSSH_CTX_UsePrivateKey_buffer` on each
 *     start (wolfSSH copies it internally). The cached buffer is
 *     wiped via `sshd_invalidate_hostkey` so the next start re-loads.
 */

#include "sshd.h"

#include "config.h"
#include "host_key.h"
#include "rng.h"
#include "sched.h"
#include "shell.h"
#include "shell_io.h"
#include "shell_io_ssh.h"
#include "shell_session.h"
#include "smp.h"
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
#include <wolfssl/wolfcrypt/asn_public.h>
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

/* All four counters mutate under g_mod_lock. Reads (via
 * sshd_get_stats) are best-effort snapshots that don't lock; the
 * documented contract is that the values are advisory diagnostics, not
 * fenced observations. `volatile` keeps the compiler from CSE-ing the
 * unlocked reads across statements. */
static volatile uint32_t   g_accepted;
static volatile uint32_t   g_kex_completed;
static volatile uint32_t   g_kex_failed;
static volatile uint32_t   g_active;
static volatile int        g_last_kex_err;
static volatile uint32_t   g_last_kex_cpu;

/* ---------------------------------------------------------------- */
/* User authentication callback                                      */
/* ---------------------------------------------------------------- */

#include "passwd.h"

/*
 * Two user-auth callbacks live side-by-side, selected at compile time
 * by `NET_SSHD_DEMO_ALLOW_ALL`:
 *
 *   ON  → `sshd_userauth_allow_all` accepts every login; matches the
 *         #199c demo path. `sshd_autostart` prints a loud WARNING on
 *         every boot while this is wired.
 *
 *   OFF → `sshd_userauth_passwd` verifies (username, password) against
 *         `/mnt/files/etc/passwd` via the scrypt-based `passwd_verify`
 *         from #199d. Bootstrap gate: until `passwd_any_users()`
 *         returns true (operator has run `passwd <name> <pw>` on the
 *         console at least once), every auth attempt fails with the
 *         standard FAILURE response. Peer sees "Permission denied
 *         (password)." — uninformative to an attacker, operator
 *         diagnoses from the boot banner.
 *
 * Rate limiting + per-IP backoff are deferred to #199e. Public-key
 * auth is out of scope per #199's non-goals.
 *
 * The CMake default for `NET_SSHD_DEMO_ALLOW_ALL` flipped from ON to
 * OFF in this PR so real auth is the default once #199d landed; an
 * operator who wants the demo bypass can rebuild with
 * `-DNET_SSHD_DEMO_ALLOW_ALL=ON`.
 */
#if defined(NET_SSHD_DEMO_ALLOW_ALL) && NET_SSHD_DEMO_ALLOW_ALL
static int sshd_userauth_allow_all(uint8_t auth_type,
                                   WS_UserAuthData *data,
                                   void *ctx)
#else
static int sshd_userauth_passwd(uint8_t auth_type,
                                WS_UserAuthData *data,
                                void *ctx)
#endif
{
#if defined(NET_SSHD_DEMO_ALLOW_ALL) && NET_SSHD_DEMO_ALLOW_ALL
    (void)auth_type;
    (void)data;
    (void)ctx;
    return WOLFSSH_USERAUTH_SUCCESS;
#else
    (void)ctx;
    if (!passwd_any_users()) {
        return WOLFSSH_USERAUTH_FAILURE;
    }
    if (auth_type != WOLFSSH_USERAUTH_PASSWORD || data == NULL) {
        return WOLFSSH_USERAUTH_FAILURE;
    }
    if (data->usernameSz == 0u || data->usernameSz > PASSWD_MAX_USERNAME_LEN) {
        return WOLFSSH_USERAUTH_FAILURE;
    }
    if (data->sf.password.passwordSz == 0u ||
        data->sf.password.passwordSz > PASSWD_MAX_PASSWORD_LEN) {
        return WOLFSSH_USERAUTH_FAILURE;
    }

    char username[PASSWD_MAX_USERNAME_LEN + 1u];
    char password[PASSWD_MAX_PASSWORD_LEN + 1u];
    /* Lengths are already bounded by the early-return checks above. */
    memcpy(username, data->username, data->usernameSz);
    username[data->usernameSz] = '\0';
    memcpy(password, data->sf.password.password, data->sf.password.passwordSz);
    password[data->sf.password.passwordSz] = '\0';

    int rc = passwd_verify(username, password);

    /* Wipe the plaintext password from the stack before returning.
     * secure_zero is the standard helper (kernel/include/string.h)
     * — defeats dead-store-elim on the stack copy. */
    secure_zero(password, sizeof(password));

    return (rc == PASSWD_OK) ? WOLFSSH_USERAUTH_SUCCESS
                             : WOLFSSH_USERAUTH_FAILURE;
#endif
}

/* ---------------------------------------------------------------- */
/* Host key — VFS-persisted via kernel/net/ssh/host_key.c            */
/* ---------------------------------------------------------------- */

/* Cached keypair held resident for the daemon's lifetime. Loaded
 * from `/mnt/files/etc/ssh/host_ed25519_key` (or generated +
 * persisted on first boot). Layout is [32-byte private seed][32-byte
 * public key] — matches wolfSSH's WOLFSSH_FORMAT_RAW expectation. */
static uint8_t  g_hostkey_raw[HOST_KEY_RAW_BUF_LEN];
static uint32_t g_hostkey_raw_len;

const uint8_t *sshd_internal_public_key(void)
{
    if (g_hostkey_raw_len == 0u) return NULL;
    return g_hostkey_raw + HOST_KEY_PRIV_LEN;
}

void sshd_invalidate_hostkey(void)
{
    /* Wipe the cached private+public buffer so the next sshd_start
     * re-runs the load path. Also tears down the cached CTX so its
     * cached PrivateKey is dropped. */
    irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
    secure_zero(g_hostkey_raw, sizeof(g_hostkey_raw));
    g_hostkey_raw_len = 0u;
    if (g_ctx) {
        wolfSSH_CTX_free(g_ctx);
        g_ctx = NULL;
    }
    spin_unlock_irqrestore(&g_mod_lock, flags);
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
    /* Idempotent: tolerate a double-call. Reachable when sshd_stop's
     * pool walk and the session task's own task_exit close race on
     * g_mod_lock — both end up calling this on the same slot. Without
     * the early-return, g_active would decrement twice for a single
     * connection (the > 0 guard keeps the counter from underflowing
     * but the count still drifts low). */
    if (!c->in_use) return;
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

    /* Atomically check the ring has room for the entire pbuf chain
     * before taking ownership. Partial consumption is unsafe: lwIP
     * advances its receive sequence whenever a pbuf reaches the recv
     * callback (the wire bytes have been off-loaded), and `tcp_recved`
     * only controls the sliding window. If we pbuf_free a pbuf
     * containing N bytes after pushing only M < N of them, the
     * unpushed (N - M) bytes are silently lost — TCP won't
     * retransmit them because they were never marked unacked. The
     * peer keeps sending under the (now-collapsed) window until it
     * stalls, with our application missing a chunk of the byte
     * stream.
     *
     * The correct lwIP pattern: return ERR_MEM without pbuf_free.
     * lwIP retains the pbuf in its recv queue and re-delivers via
     * this callback the next time the window opens (driven by the
     * `tcp_recved` call below — only invoked after a successful
     * push, so the window only advances by bytes actually accepted
     * into the ring). */
    irq_flags_t flags = spin_lock_irqsave(&c->lock);
    uint16_t free_now = ring_free_locked(c);
    spin_unlock_irqrestore(&c->lock, flags);

    size_t total = (size_t)p->tot_len;
    if (total > (size_t)free_now) {
        /* Don't pbuf_free — lwIP keeps it and retries. */
        return ERR_MEM;
    }

    size_t consumed = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        size_t pushed = ring_push(c, (const uint8_t *)q->payload,
                                  (size_t)q->len);
        consumed += pushed;
        /* By construction (we pre-checked tot_len <= free_now) every
         * push within this chain succeeds in full. The strict-equal
         * check is a defensive invariant; failure here would mean
         * something else is mutating the ring concurrently outside
         * the lock, which violates the documented model. */
        if (pushed != q->len) break;
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
    bool kex_ok = false;
    for (;;) {
        int ret = wolfSSH_accept(c->ssh);
        if (ret == WS_SUCCESS) {
            irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
            g_kex_completed++;
            spin_unlock_irqrestore(&g_mod_lock, flags);
            kex_ok = true;
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
                irq_flags_t mflags = spin_lock_irqsave(&g_mod_lock);
                g_kex_failed++;
                g_last_kex_err = err;
                g_last_kex_cpu = cpu_id();
                spin_unlock_irqrestore(&g_mod_lock, mflags);
                uart_printf("[SSHD] conn %u: peer disconnected during KEX\r\n",
                            (unsigned)c->session_id);
                break;
            }
            continue;
        }
        /* Any other error is fatal. */
        irq_flags_t mflags = spin_lock_irqsave(&g_mod_lock);
        g_kex_failed++;
        g_last_kex_err = err;
        g_last_kex_cpu = cpu_id();
        spin_unlock_irqrestore(&g_mod_lock, mflags);
        uart_printf("[SSHD] conn %u: wolfSSH_accept fatal err=%d (%s)\r\n",
                    (unsigned)c->session_id,
                    err, wolfSSH_get_error_name(c->ssh));
        break;
    }

    if (!kex_ok) {
        irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
        conn_close_locked(c);
        spin_unlock_irqrestore(&g_mod_lock, flags);
        task_exit();
        return;
    }

    /* KEX done — bind the wolfSSH stream to a shell_session and run
     * the normal REPL. The user-auth callback gate (wired in #199d)
     * gets driven from inside wolfSSH_accept above, so reaching here
     * means the peer has either authenticated successfully or
     * wolfSSH's default policy (refuse everything in the current
     * config) let nothing pass — for #199c we accept all auth so the
     * demo works; #199d adds the real check.
     *
     * #199e flips the default-on flip behind the bootstrap gate after
     * #199d. */
    struct shell_session *sess = shell_session_alloc();
    if (!sess) {
        const char *msg = "sshd: session pool exhausted\r\n";
        (void)wolfSSH_stream_send(c->ssh, (uint8_t *)msg,
                                  (uint32_t)strlen(msg));
        irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
        conn_close_locked(c);
        spin_unlock_irqrestore(&g_mod_lock, flags);
        task_exit();
        return;
    }

    struct shell_io *io = shell_io_ssh_create(c->ssh);
    if (!io) {
        shell_session_free(sess);
        irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
        conn_close_locked(c);
        spin_unlock_irqrestore(&g_mod_lock, flags);
        task_exit();
        return;
    }
    sess->io = io;
    shell_session_bind(task_current(), sess);

    /* Banner — straight to the SSH stream. */
    shell_puts("\r\n");
    shell_puts("SLM-OS Debug Shell (ssh)\r\n");
    shell_puts("Type 'help' for available commands.\r\n");
    shell_puts("\r\n");

    /* Run the normal REPL. Returns when the peer disconnects (the
     * shell_io_ssh `is_open` flips false) or the user types `exit`. */
    shell_run();

    shell_puts("\r\nbye\r\n");

    /* Teardown. */
    shell_session_unbind(task_current());
    shell_io_ssh_destroy(io);
    shell_session_free(sess);

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
    uart_snprintf(name, sizeof(name), "sshd-%u", (unsigned)c->session_id);

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

    /* Load (or first-boot-generate-and-persist) the Ed25519 host
     * keypair under /mnt/files/etc/ssh/. See kernel/net/ssh/host_key.c.
     * Re-loaded across sshd_stop/start cycles in case the operator
     * regenerated the key. */
    if (g_hostkey_raw_len == 0u) {
        int rc = host_key_load_or_generate(g_hostkey_raw);
        if (rc != HOST_KEY_OK) return SSHD_E_NO_HOSTKEY;
        g_hostkey_raw_len = HOST_KEY_RAW_BUF_LEN;
    }

    /* Create the shared SERVER context. */
    if (!g_ctx) {
        g_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
        if (!g_ctx) return SSHD_E_WOLF_INIT;

        wolfSSH_SetIORecv(g_ctx, wolf_io_recv);
        wolfSSH_SetIOSend(g_ctx, wolf_io_send);
#if defined(NET_SSHD_DEMO_ALLOW_ALL) && NET_SSHD_DEMO_ALLOW_ALL
        wolfSSH_SetUserAuth(g_ctx, sshd_userauth_allow_all);
#else
        wolfSSH_SetUserAuth(g_ctx, sshd_userauth_passwd);
#endif

        /* Convert the SLM-OS-native (seed || pub) layout into the
         * PKCS#8 DER form wolfSSH's WOLFSSH_FORMAT_RAW importer
         * actually expects (it requires bytes starting with 0x30,
         * the ASN.1 SEQUENCE tag — "RAW" in wolfSSH terminology is
         * "DER without PEM wrapper"). We materialise the DER each
         * sshd_start so the cached host-key buffer remains the
         * compact 64-byte seed||pub format on disk. */
        ed25519_key tmp_key;
        if (wc_ed25519_init(&tmp_key) != 0) {
            wolfSSH_CTX_free(g_ctx); g_ctx = NULL; return SSHD_E_NO_HOSTKEY;
        }
        if (wc_ed25519_import_private_key(g_hostkey_raw, HOST_KEY_PRIV_LEN,
                                          g_hostkey_raw + HOST_KEY_PRIV_LEN,
                                          HOST_KEY_PUB_LEN,
                                          &tmp_key) != 0) {
            wc_ed25519_free(&tmp_key);
            wolfSSH_CTX_free(g_ctx); g_ctx = NULL; return SSHD_E_NO_HOSTKEY;
        }

        /* DER form for Ed25519 PKCS#8 is at most ~85 bytes; 128 gives
         * a safe margin. wc_Ed25519PrivateKeyToDer returns the length
         * actually written. The buffer carries the full private seed
         * on the stack — wipe it on every exit path (secure_zero
         * defeats dead-store-elim; the stack frame is reclaimed by
         * the scheduler when sshd_start's caller task returns). */
        uint8_t der[128];
        int der_len = wc_Ed25519PrivateKeyToDer(&tmp_key, der, (uint32_t)sizeof(der));
        wc_ed25519_free(&tmp_key);
        if (der_len <= 0) {
            secure_zero(der, sizeof(der));
            wolfSSH_CTX_free(g_ctx); g_ctx = NULL; return SSHD_E_NO_HOSTKEY;
        }

        int use_rc = wolfSSH_CTX_UsePrivateKey_buffer(g_ctx,
                                                     der,
                                                     (uint32_t)der_len,
                                                     WOLFSSH_FORMAT_RAW);
        secure_zero(der, sizeof(der));
        if (use_rc != WS_SUCCESS) {
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
    out->last_kex_err         = g_last_kex_err;
    out->last_kex_cpu         = g_last_kex_cpu;
}

/* ---------------------------------------------------------------- */
/* Test hooks                                                        */
/* ---------------------------------------------------------------- */
/*
 * Declared in `sshd_test.h` (kernel/net/ssh/sshd_test.h) and consumed
 * only by `kernel/tests/test_sshd.c`. Kept always-on rather than
 * #ifdef-gated because the SSH module itself is already
 * `NET_SSHD`-gated at the CMake level — these few hundred bytes of
 * code only exist in builds that ship the daemon, and `--gc-sections`
 * removes them from production kernels that don't link the test
 * harness. The hooks rent a slot from `g_conns[]` (the same pool the
 * real listener uses), so tests must `sshd_test_release_slot` before
 * the real listener starts, or vice versa.
 */

void *sshd_test_take_slot(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
    struct sshd_conn *c = alloc_conn_locked();
    if (c) {
        c->in_use      = true;
        c->pcb         = NULL;
        c->ssh         = NULL;
        c->rx_head     = 0;
        c->rx_tail     = 0;
        c->peer_closed = false;
        c->session_id  = 0;
        c->lock        = (spinlock_t)SPINLOCK_INIT;
    }
    spin_unlock_irqrestore(&g_mod_lock, flags);
    return c;
}

void sshd_test_release_slot(void *handle)
{
    struct sshd_conn *c = (struct sshd_conn *)handle;
    if (!c) return;
    irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
    c->in_use = false;
    spin_unlock_irqrestore(&g_mod_lock, flags);
}

/* Defensive reset: drop the `in_use` flag on every slot. Tests call
 * this at their start so a prior test that longjmp'd out of Unity's
 * assertion path (leaking slots) doesn't cascade into "no slots
 * available" failures on the next test. The real listener must NOT
 * be running when this is called — caller responsibility. */
void sshd_test_release_all_slots(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_mod_lock);
    for (size_t i = 0; i < SSHD_MAX_SESSIONS; i++) {
        g_conns[i].in_use = false;
    }
    spin_unlock_irqrestore(&g_mod_lock, flags);
}

size_t sshd_test_ring_push(void *handle, const void *src, size_t n)
{
    if (!handle || !src) return 0;
    return ring_push((struct sshd_conn *)handle, (const uint8_t *)src, n);
}

size_t sshd_test_ring_pop(void *handle, void *dst, size_t n)
{
    if (!handle || !dst) return 0;
    return ring_pop((struct sshd_conn *)handle, (uint8_t *)dst, n, NULL);
}

uint16_t sshd_test_ring_free(void *handle)
{
    struct sshd_conn *c = (struct sshd_conn *)handle;
    if (!c) return 0;
    irq_flags_t flags = spin_lock_irqsave(&c->lock);
    uint16_t f = ring_free_locked(c);
    spin_unlock_irqrestore(&c->lock, flags);
    return f;
}

size_t sshd_test_ring_capacity(void)
{
    /* One byte reserved to distinguish full from empty (see ring_free_locked). */
    return SSHD_RX_RING_BYTES - 1u;
}
