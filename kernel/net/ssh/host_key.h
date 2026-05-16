/*
 * host_key.h - Ed25519 host-key generation + persistence.
 *
 * On first boot of an `NET_SSHD=ON` kernel, generates an Ed25519
 * keypair and persists it under `/mnt/files/etc/ssh/`. On subsequent
 * boots, loads the existing keypair so the SSH host-key fingerprint
 * is stable across reboots.
 *
 * File layout under `/mnt/files/etc/ssh/`:
 *
 *   host_ed25519_key      32-byte raw Ed25519 seed (private)
 *   host_ed25519_key.pub  "ssh-ed25519 <base64-of-rfc4253-blob> SLM-OS\n"
 *
 * Atomic persist: write to `host_ed25519_key.tmp`, then `rename` to
 * the final path. A power loss mid-write therefore leaves either the
 * pre-existing key or the new key — never a torn half.
 *
 * The private-key format is intentionally a 32-byte raw seed and NOT
 * OpenSSH's PEM-wrapped `BEGIN OPENSSH PRIVATE KEY` envelope. SLM-OS
 * doesn't need PEM compatibility — only the kernel reads this file.
 * The public-key file IS OpenSSH-compatible so an operator can
 * `cat host_ed25519_key.pub >> ~/.ssh/known_hosts` directly.
 *
 * #199b (this sub-ticket). #199c will replace the ephemeral key in
 * sshd_start with a call to host_key_load_or_generate.
 */

#ifndef SLMOS_HOST_KEY_H
#define SLMOS_HOST_KEY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define HOST_KEY_PRIV_LEN          32u   /* Ed25519 seed */
#define HOST_KEY_PUB_LEN           32u   /* Ed25519 public key */
#define HOST_KEY_RAW_BUF_LEN       (HOST_KEY_PRIV_LEN + HOST_KEY_PUB_LEN)

/* SHA-256 fingerprint, base64-encoded without padding, plus the
 * "SHA256:" prefix. 32 raw bytes → 43 base64 chars. */
#define HOST_KEY_FINGERPRINT_MAX   64u

enum host_key_result {
    HOST_KEY_OK              = 0,
    HOST_KEY_E_NO_MOUNT      = -1,   /* /mnt/files not available */
    HOST_KEY_E_GENERATE      = -2,   /* wc_ed25519_make_key failed */
    HOST_KEY_E_PERSIST       = -3,   /* file write / rename failed */
    HOST_KEY_E_LOAD          = -4,   /* file present but malformed */
    HOST_KEY_E_BAD_ARG       = -5,
};

/*
 * Load the host key from `/mnt/files/etc/ssh/host_ed25519_key`. If
 * absent, generates a new keypair and persists it before returning.
 *
 * `priv_pub_out` must be at least HOST_KEY_RAW_BUF_LEN bytes; on
 * success it's populated with [32-byte private seed][32-byte public].
 * Returns 0 on success.
 *
 * Concurrency: callers must serialize among themselves; the function
 * does not take a mount-level lock. In practice sshd_start is the
 * only caller and runs once per boot.
 */
int host_key_load_or_generate(uint8_t priv_pub_out[HOST_KEY_RAW_BUF_LEN]);

/*
 * Force regeneration: removes any existing key files and generates a
 * new pair. Useful for compromise recovery / testing via the
 * `sshd regenerate-host-key` shell verb.
 */
int host_key_regenerate(uint8_t priv_pub_out[HOST_KEY_RAW_BUF_LEN]);

/*
 * Compute the OpenSSH-compatible SHA-256 fingerprint of the current
 * (or supplied) public key. Format: "SHA256:<base64-without-padding>".
 *
 * `out` must be at least HOST_KEY_FINGERPRINT_MAX bytes. On success
 * the buffer is NUL-terminated.
 */
int host_key_fingerprint(const uint8_t pub[HOST_KEY_PUB_LEN],
                         char *out, size_t cap);

#endif /* SLMOS_HOST_KEY_H */
