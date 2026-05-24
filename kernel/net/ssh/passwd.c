/*
 * passwd.c - User database + scrypt password verification.
 *
 * Backed by /mnt/files/etc/passwd. PHC-style line format:
 *
 *   username:$scrypt$N=N,r=R,p=P$<salt-b64-no-padding>$<hash-b64-no-padding>:uid:home
 *
 * Fields after the hash (uid, home) are reserved for #199e or later
 * usage — currently parsed but not enforced (SLM-OS has no
 * permission model).
 *
 * scrypt cost: N=2^PASSWD_SCRYPT_LOG2_N (32 MB), r=8, p=1. Single
 * derivation per auth attempt takes ~100 ms on Pi 5 (acceptable for
 * SSH-handshake frequency, prohibitive for offline guessing).
 *
 * Concurrency: the daemon takes one auth per session-task at a time;
 * we don't lock the file across calls. The race window between
 * `passwd_verify` and a concurrent `passwd_set` is bounded by the
 * littlefs atomic-rename semantics (open snapshot is consistent
 * even if a parallel writer rotates the file underneath).
 */

#include "passwd.h"

#include "littlefs_slm.h"
#include "rng.h"
#include "string.h"
#include "uart.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wolfssl/wolfcrypt/pwdbased.h>

/* ---------------------------------------------------------------- */
/* On-disk layout                                                    */
/* ---------------------------------------------------------------- */

#define PASSWD_PATH_PREFIX     "/mnt/files"
#define PASSWD_LFS             "/etc/passwd"
#define PASSWD_TMP_LFS         "/etc/passwd.tmp"

#define MAX_PASSWD_USERS       8u
#define MAX_LINE_LEN           (PASSWD_MAX_USERNAME_LEN + 256u)

/* Buffer the entire file in a single read — keeps the parser simple
 * and lets passwd_set rewrite atomically via .tmp + rename. */
#define PASSWD_FILE_MAX_BYTES  (MAX_PASSWD_USERS * MAX_LINE_LEN)

/* ---------------------------------------------------------------- */
/* Base64 (no padding) helpers — reused from host_key.c semantics    */
/* ---------------------------------------------------------------- */

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    size_t oi = 0;
    size_t i  = 0;
    while (i + 3u <= len) {
        if (oi + 4u > cap) return 0;
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8)
                     | (uint32_t)in[i + 2];
        out[oi++] = k_b64[(v >> 18) & 0x3Fu];
        out[oi++] = k_b64[(v >> 12) & 0x3Fu];
        out[oi++] = k_b64[(v >> 6)  & 0x3Fu];
        out[oi++] = k_b64[v & 0x3Fu];
        i += 3u;
    }
    size_t tail = len - i;
    if (tail == 1u) {
        if (oi + 2u > cap) return 0;
        uint32_t v = (uint32_t)in[i] << 16;
        out[oi++] = k_b64[(v >> 18) & 0x3Fu];
        out[oi++] = k_b64[(v >> 12) & 0x3Fu];
    } else if (tail == 2u) {
        if (oi + 3u > cap) return 0;
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[oi++] = k_b64[(v >> 18) & 0x3Fu];
        out[oi++] = k_b64[(v >> 12) & 0x3Fu];
        out[oi++] = k_b64[(v >> 6)  & 0x3Fu];
    }
    return oi;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, size_t len, uint8_t *out, size_t cap)
{
    size_t oi = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64_value(in[i]);
        if (v < 0) return 0;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (oi >= cap) return 0;
            out[oi++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    return oi;
}

/* ---------------------------------------------------------------- */
/* VFS helpers                                                       */
/* ---------------------------------------------------------------- */

static struct lfs_mount *resolve_mount(void)
{
    const char *subpath = NULL;
    return (struct lfs_mount *)vfs_get_mount_ctx(PASSWD_PATH_PREFIX, &subpath);
}

static int read_file_all(struct lfs_mount *mnt, char *buf, size_t cap)
{
    int fd = littlefs_file_open(mnt, PASSWD_LFS, LFS_O_RDONLY);
    if (fd < 0) return -1;
    int n = littlefs_file_read(mnt, fd, buf, cap - 1u);
    littlefs_file_close(mnt, fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

static int write_file_atomic(struct lfs_mount *mnt, const char *buf, size_t len)
{
    (void)littlefs_mkdir(mnt, "/etc");
    int fd = littlefs_file_open(mnt, PASSWD_TMP_LFS,
                                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fd < 0) return -1;
    int n = littlefs_file_write(mnt, fd, buf, len);
    (void)littlefs_file_sync(mnt, fd);
    littlefs_file_close(mnt, fd);
    if (n != (int)len) {
        (void)littlefs_remove(mnt, PASSWD_TMP_LFS);
        return -1;
    }
    (void)littlefs_remove(mnt, PASSWD_LFS);
    if (littlefs_rename(mnt, PASSWD_TMP_LFS, PASSWD_LFS) != 0) {
        (void)littlefs_remove(mnt, PASSWD_TMP_LFS);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Line parser / formatter                                           */
/* ---------------------------------------------------------------- */

struct passwd_entry {
    char     username[PASSWD_MAX_USERNAME_LEN + 1u];
    uint8_t  salt[PASSWD_SALT_LEN];
    uint8_t  hash[PASSWD_HASH_LEN];
    int      log2_n;
    int      r;
    int      p;
};

/*
 * Parse one PHC-style password line in place. The line buffer is
 * mutated (NULs inserted). Returns 0 on success.
 */
static int parse_line(char *line, struct passwd_entry *out)
{
    char *p1 = strchr(line, ':');
    if (!p1) return PASSWD_E_FORMAT;
    *p1++ = '\0';

    size_t un_len = strlen(line);
    if (un_len == 0u || un_len > PASSWD_MAX_USERNAME_LEN) return PASSWD_E_FORMAT;
    memcpy(out->username, line, un_len + 1u);

    /* p1 now points at "$scrypt$N=N,r=R,p=P$<salt>$<hash>". */
    if (strncmp(p1, "$scrypt$", 8) != 0) return PASSWD_E_FORMAT;
    p1 += 8;

    /* Parse "N=<num>,r=<num>,p=<num>". */
    int log2_n = 0, r = 0, pp = 0;
    while (*p1 && *p1 != '$') {
        if (strncmp(p1, "N=", 2) == 0) {
            p1 += 2;
            while (*p1 >= '0' && *p1 <= '9') {
                log2_n = log2_n * 10 + (*p1 - '0'); p1++;
            }
        } else if (strncmp(p1, "r=", 2) == 0) {
            p1 += 2;
            while (*p1 >= '0' && *p1 <= '9') { r = r * 10 + (*p1 - '0'); p1++; }
        } else if (strncmp(p1, "p=", 2) == 0) {
            p1 += 2;
            while (*p1 >= '0' && *p1 <= '9') { pp = pp * 10 + (*p1 - '0'); p1++; }
        } else if (*p1 == ',') {
            p1++;
        } else {
            return PASSWD_E_FORMAT;
        }
    }
    if (*p1 != '$') return PASSWD_E_FORMAT;
    p1++;

    out->log2_n = log2_n;
    out->r      = r;
    out->p      = pp;

    /* Salt — base64-decode up to the next '$'. */
    char *salt_end = strchr(p1, '$');
    if (!salt_end) return PASSWD_E_FORMAT;
    size_t slen = b64_decode(p1, (size_t)(salt_end - p1),
                             out->salt, PASSWD_SALT_LEN);
    if (slen != PASSWD_SALT_LEN) return PASSWD_E_FORMAT;

    /* Hash — base64-decode up to the next ':' or '\0'. */
    p1 = salt_end + 1;
    char *hash_end = p1;
    while (*hash_end && *hash_end != ':' && *hash_end != '\n') {
        hash_end++;
    }
    size_t hlen = b64_decode(p1, (size_t)(hash_end - p1),
                             out->hash, PASSWD_HASH_LEN);
    if (hlen != PASSWD_HASH_LEN) return PASSWD_E_FORMAT;

    return PASSWD_OK;
}

static size_t format_line(const struct passwd_entry *e, char *out, size_t cap)
{
    /* "username:$scrypt$N=N,r=R,p=P$<salt>$<hash>:0:/\n"
     * The 0 and / placeholders are uid and home; we don't use them. */
    char salt_b64[32];
    char hash_b64[48];
    size_t sl = b64_encode(e->salt, PASSWD_SALT_LEN, salt_b64, sizeof(salt_b64));
    size_t hl = b64_encode(e->hash, PASSWD_HASH_LEN, hash_b64, sizeof(hash_b64));

    /* Manual format — uart_snprintf would work but the field set is
     * fixed and small. */
    size_t pos = 0;
    for (size_t i = 0; e->username[i] != '\0'; i++) {
        if (pos >= cap) return 0;
        out[pos++] = e->username[i];
    }
    static const char k_pfx[] = ":$scrypt$N=";
    for (size_t i = 0; i < sizeof(k_pfx) - 1u; i++) {
        if (pos >= cap) return 0;
        out[pos++] = k_pfx[i];
    }
    /* log2_n as decimal */
    char num[8];
    int  ni = 0;
    int  n  = e->log2_n;
    if (n == 0) num[ni++] = '0';
    while (n > 0 && ni < 8) { num[ni++] = '0' + (n % 10); n /= 10; }
    while (ni > 0) { if (pos >= cap) return 0; out[pos++] = num[--ni]; }

    static const char k_r[] = ",r=";
    for (size_t i = 0; i < sizeof(k_r) - 1u; i++) {
        if (pos >= cap) return 0;
        out[pos++] = k_r[i];
    }
    n = e->r; ni = 0;
    if (n == 0) num[ni++] = '0';
    while (n > 0 && ni < 8) { num[ni++] = '0' + (n % 10); n /= 10; }
    while (ni > 0) { if (pos >= cap) return 0; out[pos++] = num[--ni]; }

    static const char k_p[] = ",p=";
    for (size_t i = 0; i < sizeof(k_p) - 1u; i++) {
        if (pos >= cap) return 0;
        out[pos++] = k_p[i];
    }
    n = e->p; ni = 0;
    if (n == 0) num[ni++] = '0';
    while (n > 0 && ni < 8) { num[ni++] = '0' + (n % 10); n /= 10; }
    while (ni > 0) { if (pos >= cap) return 0; out[pos++] = num[--ni]; }

    if (pos >= cap) return 0;
    out[pos++] = '$';
    for (size_t i = 0; i < sl; i++) {
        if (pos >= cap) return 0;
        out[pos++] = salt_b64[i];
    }
    if (pos >= cap) return 0;
    out[pos++] = '$';
    for (size_t i = 0; i < hl; i++) {
        if (pos >= cap) return 0;
        out[pos++] = hash_b64[i];
    }

    static const char k_suffix[] = ":0:/\n";
    for (size_t i = 0; i < sizeof(k_suffix) - 1u; i++) {
        if (pos >= cap) return 0;
        out[pos++] = k_suffix[i];
    }
    return pos;
}

/* ---------------------------------------------------------------- */
/* scrypt wrapper                                                    */
/* ---------------------------------------------------------------- */

static int derive_hash(const char *password,
                       const uint8_t salt[PASSWD_SALT_LEN],
                       int log2_n, int r, int p,
                       uint8_t out[PASSWD_HASH_LEN])
{
    /* Cap at our configured floor — a stored entry that claims a
     * higher log2_n than we ever produce is either corruption or an
     * import from foreign tooling we don't support. wc_scrypt would
     * fail allocation long before log2_n > 20 (~1 GB) on any SLM-OS
     * platform anyway. */
    if (log2_n <= 0 || log2_n > PASSWD_SCRYPT_LOG2_N) return PASSWD_E_KDF;
    int rc = wc_scrypt(out,
                       (const uint8_t *)password,
                       (int)strlen(password),
                       salt, (int)PASSWD_SALT_LEN,
                       log2_n, r, p,
                       (int)PASSWD_HASH_LEN);
    if (rc != 0) {
        /* Log the wolfcrypt error code (e.g. -125 MEMORY_E, -173
         * BAD_FUNC_ARG) so operators can tell scrypt-failed-due-to-
         * heap-exhaustion apart from scrypt-failed-due-to-bad-args
         * without rebuilding. */
        uart_printf("[PASSWD] wc_scrypt failed: rc=%d (log2_n=%d r=%d p=%d)\r\n",
                    rc, log2_n, r, p);
        return PASSWD_E_KDF;
    }
    return PASSWD_OK;
}

static int constant_time_compare(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (a[i] ^ b[i]);
    }
    return (diff == 0) ? 0 : 1;
}

/* ---------------------------------------------------------------- */
/* File-level operations                                             */
/* ---------------------------------------------------------------- */

/*
 * Walk every line in the file buffer. For each line invokes `cb`
 * which may return non-zero to stop iteration. Returns the number
 * of entries successfully parsed.
 */
static int for_each_entry(char *buf, size_t len,
                          int (*cb)(const struct passwd_entry *e,
                                    char *raw_line, void *ctx),
                          void *ctx)
{
    size_t i = 0;
    int    count = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && buf[i] != '\n' && buf[i] != '\0') {
            i++;
        }
        size_t end = i;
        if (i < len) {
            buf[i++] = '\0';
        }
        char *line = buf + start;
        if (end == start) continue;   /* empty line */
        if (line[0] == '#') continue; /* comment */

        struct passwd_entry e;
        if (parse_line(line, &e) != PASSWD_OK) {
            /* Surface corruption (truncated write, manual edit gone
             * wrong) so the operator sees the cause instead of just
             * a generic "user not found" downstream. */
            uart_printf("[passwd] WARN: skipping malformed line: %s\r\n",
                        line);
            continue;
        }
        if (cb && cb(&e, line, ctx) != 0) return count + 1;
        count++;
    }
    return count;
}

struct find_ctx { const char *username; struct passwd_entry *out; int found; };

static int find_cb(const struct passwd_entry *e, char *raw_line, void *ctx)
{
    struct find_ctx *fc = (struct find_ctx *)ctx;
    (void)raw_line;
    if (strcmp(e->username, fc->username) == 0) {
        *fc->out = *e;
        fc->found = 1;
        return 1;   /* stop */
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

bool passwd_any_users(void)
{
    struct lfs_mount *mnt = resolve_mount();
    if (!mnt) return false;

    /* Stack-local: the buffer is 2.3 KB on a 256 KB task stack. Was
     * `static` — that raced under concurrent SSH auth attempts since
     * passwd_verify ran on per-connection session tasks and shared
     * the same buffer with this function. */
    char buf[PASSWD_FILE_MAX_BYTES];
    int n = read_file_all(mnt, buf, sizeof(buf));
    if (n <= 0) return false;

    /* Any non-comment, non-blank line counts. */
    int users = for_each_entry(buf, (size_t)n, NULL, NULL);
    return users > 0;
}

int passwd_verify(const char *username, const char *password)
{
    if (!username || !password) return PASSWD_E_BAD_ARG;

    struct lfs_mount *mnt = resolve_mount();
    if (!mnt) return PASSWD_E_IO;

    /* Stack-local — was `static` and raced across session tasks. */
    char buf[PASSWD_FILE_MAX_BYTES];
    int n = read_file_all(mnt, buf, sizeof(buf));
    if (n <= 0) return PASSWD_E_NOT_FOUND;

    struct passwd_entry entry;
    struct find_ctx fc = { .username = username, .out = &entry, .found = 0 };
    for_each_entry(buf, (size_t)n, find_cb, &fc);
    if (!fc.found) return PASSWD_E_NOT_FOUND;

    uint8_t derived[PASSWD_HASH_LEN];
    int rc = derive_hash(password, entry.salt,
                         entry.log2_n, entry.r, entry.p, derived);
    if (rc != PASSWD_OK) return rc;
    int match = constant_time_compare(derived, entry.hash, PASSWD_HASH_LEN);
    /* Wipe the derived hash from the stack — secure_zero defeats the
     * dead-store elimination the optimiser otherwise applies. */
    secure_zero(derived, sizeof(derived));
    return (match == 0) ? PASSWD_OK : PASSWD_E_WRONG;
}

/* Rebuild the file with one entry replaced / added / removed. The
 * `mode` controls semantics:
 *   MODE_SET: replace if username present, else append.
 *   MODE_ADD: refuse if username present, else append.
 *   MODE_DEL: remove if present; refuse if it's the last user.
 */
enum modify_mode { MODE_SET, MODE_ADD, MODE_DEL };

struct rebuild_ctx {
    const char *target;
    const struct passwd_entry *new_entry;
    enum modify_mode mode;
    char  *out_buf;
    size_t out_cap;
    size_t out_pos;
    int    replaced;
    int    kept;
    int    skipped;
};

static int rebuild_cb(const struct passwd_entry *e, char *raw_line, void *ctx)
{
    (void)raw_line;
    struct rebuild_ctx *rc = (struct rebuild_ctx *)ctx;

    if (strcmp(e->username, rc->target) == 0) {
        rc->skipped++;
        if (rc->mode == MODE_DEL) {
            return 0;   /* drop this line */
        }
        /* SET or ADD: replace with new_entry. */
        if (rc->mode == MODE_ADD) {
            /* Caller should have detected the collision before
             * reaching here; treat as no-op (preserve existing). */
            size_t w = format_line(e, rc->out_buf + rc->out_pos,
                                   rc->out_cap - rc->out_pos);
            if (w == 0u) return 1;
            rc->out_pos += w;
            rc->kept++;
        } else {
            size_t w = format_line(rc->new_entry, rc->out_buf + rc->out_pos,
                                   rc->out_cap - rc->out_pos);
            if (w == 0u) return 1;
            rc->out_pos += w;
            rc->replaced = 1;
        }
        return 0;
    }

    size_t w = format_line(e, rc->out_buf + rc->out_pos,
                           rc->out_cap - rc->out_pos);
    if (w == 0u) return 1;
    rc->out_pos += w;
    rc->kept++;
    return 0;
}

static int rebuild_and_persist(const char *target,
                               const struct passwd_entry *new_entry,
                               enum modify_mode mode)
{
    struct lfs_mount *mnt = resolve_mount();
    if (!mnt) return PASSWD_E_IO;

    /* Stack-local — see passwd_verify comment for the rationale. The
     * 4.6 KB combined budget (two PASSWD_FILE_MAX_BYTES buffers) is
     * comfortable on a 256 KB task stack. */
    char in_buf[PASSWD_FILE_MAX_BYTES];
    char out_buf[PASSWD_FILE_MAX_BYTES];
    int n = read_file_all(mnt, in_buf, sizeof(in_buf));
    if (n < 0) n = 0;

    struct rebuild_ctx rc = {
        .target    = target,
        .new_entry = new_entry,
        .mode      = mode,
        .out_buf   = out_buf,
        .out_cap   = sizeof(out_buf),
        .out_pos   = 0,
        .replaced  = 0,
        .kept      = 0,
        .skipped   = 0,
    };
    for_each_entry(in_buf, (size_t)n, rebuild_cb, &rc);

    /* Append for ADD/SET when the target wasn't present. */
    if ((mode == MODE_ADD || mode == MODE_SET) && !rc.replaced && rc.skipped == 0) {
        size_t w = format_line(new_entry, out_buf + rc.out_pos,
                               sizeof(out_buf) - rc.out_pos);
        if (w == 0u) return PASSWD_E_FULL;
        rc.out_pos += w;
    }

    if (mode == MODE_DEL) {
        if (rc.skipped == 0)   return PASSWD_E_NOT_FOUND;
        if (rc.kept    == 0)   return PASSWD_E_LAST_USER;
    }
    if (mode == MODE_ADD && rc.replaced == 0 && rc.skipped > 0) {
        return PASSWD_E_EXISTS;
    }

    if (write_file_atomic(mnt, out_buf, rc.out_pos) != 0) return PASSWD_E_IO;
    return PASSWD_OK;
}

int passwd_adduser(const char *username, const char *password)
{
    if (!username || !password) return PASSWD_E_BAD_ARG;
    size_t un_len = strlen(username);
    if (un_len > PASSWD_MAX_USERNAME_LEN) return PASSWD_E_BAD_ARG;
    if (strlen(password) > PASSWD_MAX_PASSWORD_LEN) return PASSWD_E_BAD_ARG;

    struct passwd_entry e;
    memcpy(e.username, username, un_len + 1u);
    if (rng_get_bytes(e.salt, PASSWD_SALT_LEN) != 0) return PASSWD_E_KDF;
    e.log2_n = PASSWD_SCRYPT_LOG2_N;
    e.r      = PASSWD_SCRYPT_R;
    e.p      = PASSWD_SCRYPT_P;
    int rc = derive_hash(password, e.salt, e.log2_n, e.r, e.p, e.hash);
    if (rc != PASSWD_OK) return rc;

    return rebuild_and_persist(username, &e, MODE_ADD);
}

int passwd_set(const char *username, const char *new_password)
{
    if (!username || !new_password) return PASSWD_E_BAD_ARG;
    /* Same bounds as passwd_adduser — without these `cmd_passwd` would
     * let a long argv overflow `e.username[33]` and corrupt kernel
     * stack. Reachable from any authenticated shell user (SSH /
     * console / telnet). */
    size_t un_len = strlen(username);
    size_t pw_len = strlen(new_password);
    if (un_len > PASSWD_MAX_USERNAME_LEN) return PASSWD_E_BAD_ARG;
    if (pw_len > PASSWD_MAX_PASSWORD_LEN) return PASSWD_E_BAD_ARG;

    struct passwd_entry e;
    memcpy(e.username, username, un_len + 1u);
    if (rng_get_bytes(e.salt, PASSWD_SALT_LEN) != 0) return PASSWD_E_KDF;
    e.log2_n = PASSWD_SCRYPT_LOG2_N;
    e.r      = PASSWD_SCRYPT_R;
    e.p      = PASSWD_SCRYPT_P;
    int rc = derive_hash(new_password, e.salt, e.log2_n, e.r, e.p, e.hash);
    if (rc != PASSWD_OK) return rc;

    return rebuild_and_persist(username, &e, MODE_SET);
}

int passwd_deluser(const char *username)
{
    if (!username) return PASSWD_E_BAD_ARG;
    return rebuild_and_persist(username, NULL, MODE_DEL);
}
