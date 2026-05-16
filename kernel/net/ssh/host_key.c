/*
 * host_key.c - Ed25519 host-key generation + persistence.
 *
 * Sits between sshd.c and the littlefs VFS mounted at /mnt/files.
 * Mirrors the read/write pattern in blob_autoload.c (atomic .tmp +
 * rename). See `host_key.h` for the API contract.
 */

#include "host_key.h"

#include "littlefs_slm.h"
#include "sha256.h"
#include "string.h"
#include "uart.h"
#include "vfs.h"

#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/random.h>

/* All host-key state lives under /mnt/files/etc/ssh/.
 *
 * The VFS prefix "/mnt/files" is stripped by vfs_get_mount_ctx — the
 * paths we hand to littlefs_* are mount-relative (e.g. "/etc/ssh/...").
 */
#define HOSTKEY_MNT_PREFIX        "/mnt/files"
#define HOSTKEY_DIR_LFS           "/etc"
#define HOSTKEY_SUBDIR_LFS        "/etc/ssh"
#define HOSTKEY_FILE_LFS          "/etc/ssh/host_ed25519_key"
#define HOSTKEY_TMP_LFS           "/etc/ssh/host_ed25519_key.tmp"
#define HOSTKEY_PUB_LFS           "/etc/ssh/host_ed25519_key.pub"
#define HOSTKEY_PUB_TMP_LFS       "/etc/ssh/host_ed25519_key.pub.tmp"

/* ---------------------------------------------------------------- */
/* Base64 encoder (no padding, URL-safe variant disabled)            */
/* ---------------------------------------------------------------- */

static const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode `len` bytes from `in` into `out`. Returns the number of
 * output chars written (NOT including a trailing NUL). The caller
 * must size `out` to at least 4 * ((len + 2) / 3) + padding bytes.
 *
 * `pad` controls whether the output is padded with '=' to a 4-byte
 * boundary (true = OpenSSH .pub format, false = SHA256 fingerprint
 * format which strips '=' per OpenSSH convention).
 */
static size_t b64_encode(const uint8_t *in, size_t len, char *out, bool pad)
{
    size_t oi = 0;
    size_t i  = 0;
    while (i + 3u <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8)
                     | (uint32_t)in[i + 2];
        out[oi++] = kBase64Alphabet[(v >> 18) & 0x3Fu];
        out[oi++] = kBase64Alphabet[(v >> 12) & 0x3Fu];
        out[oi++] = kBase64Alphabet[(v >> 6) & 0x3Fu];
        out[oi++] = kBase64Alphabet[v & 0x3Fu];
        i += 3u;
    }
    size_t tail = len - i;
    if (tail == 1u) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[oi++] = kBase64Alphabet[(v >> 18) & 0x3Fu];
        out[oi++] = kBase64Alphabet[(v >> 12) & 0x3Fu];
        if (pad) { out[oi++] = '='; out[oi++] = '='; }
    } else if (tail == 2u) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[oi++] = kBase64Alphabet[(v >> 18) & 0x3Fu];
        out[oi++] = kBase64Alphabet[(v >> 12) & 0x3Fu];
        out[oi++] = kBase64Alphabet[(v >> 6) & 0x3Fu];
        if (pad) { out[oi++] = '='; }
    }
    return oi;
}

/* ---------------------------------------------------------------- */
/* RFC 4253 "ssh-ed25519" blob construction                          */
/* ---------------------------------------------------------------- */

/*
 * Pack the public key into the RFC 4253 ssh-ed25519 wire format:
 *
 *   uint32 BE length=11; "ssh-ed25519"
 *   uint32 BE length=32; <32 bytes pubkey>
 *
 * Total = 4 + 11 + 4 + 32 = 51 bytes.
 */
#define HOSTKEY_RFC4253_BLOB_LEN  51u

static void pack_rfc4253(const uint8_t pub[HOST_KEY_PUB_LEN],
                         uint8_t out[HOSTKEY_RFC4253_BLOB_LEN])
{
    static const char prefix[] = "ssh-ed25519";
    size_t plen = sizeof(prefix) - 1u;   /* 11 */
    size_t pos  = 0;
    out[pos++] = 0; out[pos++] = 0; out[pos++] = 0; out[pos++] = (uint8_t)plen;
    for (size_t i = 0; i < plen; i++) out[pos++] = (uint8_t)prefix[i];
    out[pos++] = 0; out[pos++] = 0; out[pos++] = 0; out[pos++] = (uint8_t)HOST_KEY_PUB_LEN;
    for (size_t i = 0; i < HOST_KEY_PUB_LEN; i++) out[pos++] = pub[i];
}

/* ---------------------------------------------------------------- */
/* VFS helpers                                                       */
/* ---------------------------------------------------------------- */

static struct lfs_mount *resolve_mount(void)
{
    const char *subpath = NULL;
    return (struct lfs_mount *)vfs_get_mount_ctx(HOSTKEY_MNT_PREFIX, &subpath);
}

static int ensure_dirs(struct lfs_mount *mnt)
{
    /* Create /etc and /etc/ssh — ignore errors if they already exist
     * (littlefs returns LFS_ERR_EXIST in that case; littlefs_mkdir
     * returns 0 on existing too on this codebase). */
    (void)littlefs_mkdir(mnt, HOSTKEY_DIR_LFS);
    (void)littlefs_mkdir(mnt, HOSTKEY_SUBDIR_LFS);
    return 0;
}

static int read_full(struct lfs_mount *mnt, const char *path,
                     uint8_t *buf, size_t want)
{
    int fd = littlefs_file_open(mnt, path, LFS_O_RDONLY);
    if (fd < 0) return -1;
    int n = littlefs_file_read(mnt, fd, buf, want);
    littlefs_file_close(mnt, fd);
    return (n == (int)want) ? 0 : -1;
}

static int write_atomic(struct lfs_mount *mnt,
                        const char *final_path, const char *tmp_path,
                        const uint8_t *buf, size_t len)
{
    int fd = littlefs_file_open(mnt, tmp_path,
                                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fd < 0) return -1;
    int n = littlefs_file_write(mnt, fd, buf, len);
    (void)littlefs_file_sync(mnt, fd);
    littlefs_file_close(mnt, fd);
    if (n != (int)len) {
        (void)littlefs_remove(mnt, tmp_path);
        return -1;
    }
    /* Remove the existing final (if any) before rename — littlefs
     * doesn't atomic-replace on rename. */
    (void)littlefs_remove(mnt, final_path);
    if (littlefs_rename(mnt, tmp_path, final_path) != 0) {
        (void)littlefs_remove(mnt, tmp_path);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Generation                                                        */
/* ---------------------------------------------------------------- */

/*
 * Generate a fresh keypair and pack it into the 64-byte SLM-OS
 * persistence layout: 32-byte seed (private) || 32-byte pub.
 *
 * wolfSSL's `wc_ed25519_export_key` returns a 64-byte private side
 * (the seed CONCATENATED with the public key — RFC 8032 §5.1.5
 * "expanded private key"). We don't want that layout on disk —
 * OpenSSH and every other tool stores the 32-byte seed alone — so
 * we use `wc_ed25519_export_private_only` + `wc_ed25519_export_public`
 * to get the two halves explicitly.
 */
static int generate_keypair(uint8_t out[HOST_KEY_RAW_BUF_LEN])
{
    WC_RNG      rng;
    ed25519_key key;

    int rc = wc_InitRng(&rng);
    if (rc != 0) return -1;
    rc = wc_ed25519_init(&key);
    if (rc != 0) { wc_FreeRng(&rng); return -1; }

    rc = wc_ed25519_make_key(&rng, ED25519_KEY_SIZE, &key);
    if (rc != 0) goto out;

    uint32_t plen = HOST_KEY_PRIV_LEN;
    rc = wc_ed25519_export_private_only(&key, out, &plen);
    if (rc != 0 || plen != HOST_KEY_PRIV_LEN) { rc = -1; goto out; }

    uint32_t ulen = HOST_KEY_PUB_LEN;
    rc = wc_ed25519_export_public(&key, out + HOST_KEY_PRIV_LEN, &ulen);
    if (rc != 0 || ulen != HOST_KEY_PUB_LEN) { rc = -1; goto out; }

    rc = 0;
out:
    wc_ed25519_free(&key);
    wc_FreeRng(&rng);
    return rc;
}

/* ---------------------------------------------------------------- */
/* Public-key file formatting                                        */
/* ---------------------------------------------------------------- */

/*
 * Build the .pub line "ssh-ed25519 <base64> SLM-OS\n".
 * Returns the byte length (no NUL).
 */
static size_t format_pub_line(const uint8_t pub[HOST_KEY_PUB_LEN],
                              char *out, size_t cap)
{
    static const char k_prefix[]  = "ssh-ed25519 ";
    static const char k_comment[] = " SLM-OS\n";

    size_t pos = 0;
    for (size_t i = 0; i < sizeof(k_prefix) - 1u; i++) {
        if (pos >= cap) return 0;
        out[pos++] = k_prefix[i];
    }

    uint8_t blob[HOSTKEY_RFC4253_BLOB_LEN];
    pack_rfc4253(pub, blob);

    /* Worst-case base64 length: 4 * ((51 + 2) / 3) = 72 chars + 3 pad. */
    char b64[80];
    size_t b64len = b64_encode(blob, sizeof(blob), b64, /* pad */ true);
    if (pos + b64len > cap) return 0;
    for (size_t i = 0; i < b64len; i++) out[pos++] = b64[i];

    for (size_t i = 0; i < sizeof(k_comment) - 1u; i++) {
        if (pos >= cap) return 0;
        out[pos++] = k_comment[i];
    }
    return pos;
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

static int persist_pair(struct lfs_mount *mnt,
                        const uint8_t priv_pub[HOST_KEY_RAW_BUF_LEN])
{
    if (ensure_dirs(mnt) != 0) return HOST_KEY_E_PERSIST;

    /* Private key first (the load-time check looks for this). */
    if (write_atomic(mnt, HOSTKEY_FILE_LFS, HOSTKEY_TMP_LFS,
                     priv_pub, HOST_KEY_RAW_BUF_LEN) != 0) {
        return HOST_KEY_E_PERSIST;
    }

    /* .pub line. */
    char pub_line[160];
    size_t pub_len = format_pub_line(priv_pub + HOST_KEY_PRIV_LEN,
                                      pub_line, sizeof(pub_line));
    if (pub_len == 0u) return HOST_KEY_E_PERSIST;

    if (write_atomic(mnt, HOSTKEY_PUB_LFS, HOSTKEY_PUB_TMP_LFS,
                     (const uint8_t *)pub_line, pub_len) != 0) {
        return HOST_KEY_E_PERSIST;
    }
    return HOST_KEY_OK;
}

int host_key_load_or_generate(uint8_t priv_pub_out[HOST_KEY_RAW_BUF_LEN])
{
    if (!priv_pub_out) return HOST_KEY_E_BAD_ARG;

    struct lfs_mount *mnt = resolve_mount();
    if (!mnt) {
        /* No /mnt/files. Fall back to ephemeral (in-memory) key —
         * the caller (sshd_start) gets a valid keypair but it won't
         * survive a reboot. Returning OK here is the contract: a
         * boot without a persistent FS still gets a working sshd. */
        if (generate_keypair(priv_pub_out) != 0) return HOST_KEY_E_GENERATE;
        uart_printf("[SSHD] host key: ephemeral (no /mnt/files mount)\r\n");
        return HOST_KEY_OK;
    }

    if (read_full(mnt, HOSTKEY_FILE_LFS, priv_pub_out,
                  HOST_KEY_RAW_BUF_LEN) == 0) {
        uart_printf("[SSHD] host key: loaded from %s%s\r\n",
                    HOSTKEY_MNT_PREFIX, HOSTKEY_FILE_LFS);
        return HOST_KEY_OK;
    }

    /* Not present — first boot. Generate + persist. */
    if (generate_keypair(priv_pub_out) != 0) return HOST_KEY_E_GENERATE;
    int rc = persist_pair(mnt, priv_pub_out);
    if (rc == HOST_KEY_OK) {
        uart_printf("[SSHD] host key: generated + persisted to %s%s\r\n",
                    HOSTKEY_MNT_PREFIX, HOSTKEY_FILE_LFS);
    } else {
        uart_printf("[SSHD] host key: persist failed (%d) — using ephemeral\r\n", rc);
        /* Don't fail the boot; the in-memory key is still usable. */
        rc = HOST_KEY_OK;
    }
    return rc;
}

int host_key_regenerate(uint8_t priv_pub_out[HOST_KEY_RAW_BUF_LEN])
{
    if (!priv_pub_out) return HOST_KEY_E_BAD_ARG;

    struct lfs_mount *mnt = resolve_mount();
    if (mnt) {
        (void)littlefs_remove(mnt, HOSTKEY_FILE_LFS);
        (void)littlefs_remove(mnt, HOSTKEY_PUB_LFS);
    }
    /* Re-run the "no file → generate" path. */
    return host_key_load_or_generate(priv_pub_out);
}

int host_key_fingerprint(const uint8_t pub[HOST_KEY_PUB_LEN],
                         char *out, size_t cap)
{
    if (!pub || !out || cap < HOST_KEY_FINGERPRINT_MAX) return HOST_KEY_E_BAD_ARG;

    uint8_t blob[HOSTKEY_RFC4253_BLOB_LEN];
    pack_rfc4253(pub, blob);

    uint8_t digest[SHA256_DIGEST_LEN];
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, blob, sizeof(blob));
    sha256_final(&ctx, digest);

    /* "SHA256:" prefix + base64-without-padding of the 32-byte digest. */
    static const char k_pfx[] = "SHA256:";
    size_t pos = 0;
    for (size_t i = 0; i < sizeof(k_pfx) - 1u && pos < cap - 1u; i++) {
        out[pos++] = k_pfx[i];
    }
    pos += b64_encode(digest, sizeof(digest), out + pos, /* pad */ false);
    if (pos >= cap) return HOST_KEY_E_BAD_ARG;
    out[pos] = '\0';
    return HOST_KEY_OK;
}
