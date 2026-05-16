/*
 * passwd.h - User database + password verification for SSH.
 *
 * Backed by /mnt/files/etc/passwd with one line per user. PHC-style
 * KDF-encoded password field:
 *
 *     username:$scrypt$N=2^N,r=8,p=1$<salt-b64>$<hash-b64>:uid:home
 *
 * Read on every SSH auth attempt — small file (≤ MAX_PASSWD_USERS
 * users), well-cached at the littlefs layer; the per-auth cost is
 * dominated by the KDF compute (intentional: makes offline guessing
 * expensive).
 *
 * **scrypt parameters** start at N=2^15 (32 MB), r=8, p=1 — the floor
 * documented in RFC 7914 and OWASP password-storage cheat sheet's
 * "second recommendation". Kept in `kernel/include/config.h` so a
 * platform with tighter RAM can lower N to 2^14 (16 MB) at build time
 * without touching this file's defaults.
 *
 * **Bootstrap gate.** Until at least one user exists (passwd file
 * absent or empty), SSH auth refuses every attempt with an explicit
 * "no users provisioned — run `passwd root` on the console" message.
 * Console (UART) sessions bypass the gate because that's the only
 * way to bootstrap.
 */

#ifndef SLMOS_SSH_PASSWD_H
#define SLMOS_SSH_PASSWD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PASSWD_MAX_USERNAME_LEN  32u
#define PASSWD_MAX_PASSWORD_LEN  128u
#define PASSWD_SALT_LEN          16u
#define PASSWD_HASH_LEN          32u

/* scrypt cost parameters — see comment block at top for derivation. */
#define PASSWD_SCRYPT_LOG2_N     15  /* N = 2^15 = 32768  ~= 32 MB */
#define PASSWD_SCRYPT_R          8
#define PASSWD_SCRYPT_P          1

enum passwd_result {
    PASSWD_OK              = 0,
    PASSWD_E_BAD_ARG       = -1,
    PASSWD_E_NOT_FOUND     = -2,
    PASSWD_E_WRONG         = -3,    /* password mismatch */
    PASSWD_E_IO            = -4,    /* VFS read/write failed */
    PASSWD_E_FULL          = -5,    /* MAX_PASSWD_USERS reached */
    PASSWD_E_EXISTS        = -6,    /* adduser of existing name */
    PASSWD_E_LAST_USER     = -7,    /* deluser of the only remaining user */
    PASSWD_E_FORMAT        = -8,    /* malformed passwd line */
    PASSWD_E_KDF           = -9,    /* scrypt failure */
};

/* True iff at least one user is provisioned in /mnt/files/etc/passwd.
 * Used by the SSH auth callback to enforce the bootstrap gate. */
bool passwd_any_users(void);

/* Verify `password` against the stored hash for `username`. Returns
 * PASSWD_OK on a constant-time match, PASSWD_E_WRONG on mismatch,
 * PASSWD_E_NOT_FOUND if the user doesn't exist. */
int passwd_verify(const char *username, const char *password);

/* Add a user with the given password. Generates a random salt via
 * `rng_get_bytes` and stores the scrypt hash. Returns PASSWD_OK or a
 * negative error. */
int passwd_adduser(const char *username, const char *password);

/* Change an existing user's password. The caller may verify the
 * current password first (passwd_verify) — this function only writes
 * the new hash. */
int passwd_set(const char *username, const char *new_password);

/* Remove a user. Refuses to remove the last remaining user
 * (PASSWD_E_LAST_USER) so the daemon can't lock the operator out. */
int passwd_deluser(const char *username);

#endif /* SLMOS_SSH_PASSWD_H */
