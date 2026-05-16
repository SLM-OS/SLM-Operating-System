/*
 * sshd.h - SSH daemon for SLM-OS (Phase 3 / #199).
 *
 * Single-port lwIP listener that hands each accepted connection to
 * wolfSSH for the SSH-2 protocol. The shell-channel routing into
 * `shell_session` is #199c's scope; #199a's acceptance criterion is
 * "OpenSSH client completes KEX and gets a clean 'no shell channel'
 * disconnect."
 *
 * Built only when `NET_SSHD=ON` (CMake gate). Independent of
 * `NET_SSHD_AUTOSTART` — the autostart driver (added in #199c) calls
 * `sshd_start` at boot when its config says so.
 */

#ifndef SSHD_H
#define SSHD_H

#include <stdbool.h>
#include <stdint.h>

#define SSHD_DEFAULT_PORT  2222     /* QEMU-friendly default; 22 once #199e
                                       lands the bootstrap gate */
#define SSHD_MAX_SESSIONS  4        /* per-instance cap; raises with the
                                       shell_session pool in #199c */

enum sshd_status_code {
    SSHD_OK             = 0,
    SSHD_E_NOT_INIT     = -1,
    SSHD_E_ALREADY_UP   = -2,
    SSHD_E_NO_HOSTKEY   = -3,
    SSHD_E_LISTEN_FAIL  = -4,
    SSHD_E_WOLF_INIT    = -5,
};

struct sshd_stats {
    bool     running;
    uint16_t port;
    uint32_t connections_accepted;
    uint32_t kex_completed;          /* connections that reached WS_SUCCESS */
    uint32_t kex_failed;             /* connections that errored during accept */
    uint32_t active;                 /* currently open sessions */
};

/* Start the daemon listening on `port`. Returns SSHD_OK on success.
 * Idempotent if called twice with the same port; returns
 * SSHD_E_ALREADY_UP if a different port is requested while running. */
int sshd_start(uint16_t port);

/* Stop accepting new connections + tear down any open sessions. */
int sshd_stop(void);

/* Fill `out` with current stats. Always succeeds when `out != NULL`. */
void sshd_get_stats(struct sshd_stats *out);

#endif /* SSHD_H */
