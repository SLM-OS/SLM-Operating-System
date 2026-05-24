/*
 * wolf_os.c - SLM-OS-side glue for the vendored wolfSSL + wolfSSH.
 *
 * Provides:
 *
 *   - `slm_wolfcrypt_seed` — entropy hook wired to the kernel's
 *     crypto-quality RNG (`kernel/include/rng.h`) via the
 *     `CUSTOM_RAND_GENERATE_SEED` macro in `user_settings.h`.
 *     wolfSSL's HASHDRBG draws from this; SLM-OS provides the
 *     entropy floor (RNDR / RDRAND / SHA-256-mixed jitter pool).
 *
 *   - `XTIME` / monotonic seconds — wolfSSL uses `time_t` for
 *     certificate validity checks (we disable certs via `NO_CERTS`)
 *     and DRBG re-seed scheduling. A CNTPCT-derived monotonic second
 *     counter is correct semantics for the latter — the absolute
 *     epoch doesn't matter as long as the counter advances.
 *
 * Compiled only when `NET_SSHD=ON` (gated by CMake). The XMALLOC /
 * XFREE / XREALLOC redirection lives in `user_settings.h` and points
 * at the kernel's existing byte-level allocator (`lua_stubs.c`'s
 * malloc/free/realloc shims).
 */

#include <stddef.h>
#include <stdint.h>

#include "rng.h"
#include "timer.h"

/* ---------------------------------------------------------------- */
/* Heap hooks (XMALLOC_USER)                                         */
/* ---------------------------------------------------------------- */

/* Dedicated 48 MB wolfssl heap (kernel/net/ssh/wolf_heap.c). The
 * scrypt KDF at the RFC 7914 floor (N=2^15, r=8, p=1) needs a
 * single 32 MB working buffer per derivation, which the shared
 * lua_stubs heap (1 MB) can't satisfy — discovered during the
 * #199e hardware-validation pass on pi-5-2. */
extern void *wolf_heap_alloc(size_t size);
extern void  wolf_heap_free(void *p);
extern void *wolf_heap_realloc(void *p, size_t size);

void *XMALLOC(size_t n, void *heap, int type)
{
    (void)heap;
    (void)type;
    return wolf_heap_alloc(n);
}

void XFREE(void *p, void *heap, int type)
{
    (void)heap;
    (void)type;
    wolf_heap_free(p);
}

void *XREALLOC(void *p, size_t n, void *heap, int type)
{
    (void)heap;
    (void)type;
    return wolf_heap_realloc(p, n);
}

/* ---------------------------------------------------------------- */
/* Entropy hook                                                      */
/* ---------------------------------------------------------------- */

/*
 * wolfSSL invokes this every time its HASHDRBG needs to (re-)seed.
 * Returns 0 on success per wolfSSL convention. The `byte` /
 * `word32` types come from wolfssl/wolfcrypt/types.h; we deliberately
 * avoid pulling that header into this TU and treat the buffer as
 * raw bytes — the macro signature is
 * `int slm_wolfcrypt_seed(byte *output, word32 sz)`, but `byte` is
 * just `unsigned char` and `word32` is just `unsigned int` in every
 * config wolfSSL ships.
 */
int slm_wolfcrypt_seed(unsigned char *output, unsigned int sz)
{
    if (rng_get_bytes((uint8_t *)output, (size_t)sz) != 0) {
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Freestanding-toolchain string ops needed by wolfSSH               */
/* ---------------------------------------------------------------- */

/* The bare-metal aarch64-none-elf toolchain's newlib subset declares
 * these as standard names but doesn't ship implementations linkable
 * in -nostdlib builds. wolfssh references them from internal.c and
 * ssh.c — provide minimal in-kernel implementations.
 *
 * `strncasecmp` is byte-wise case-insensitive compare, ASCII only.
 * `strtok_r` is the reentrant string tokeniser. Both implementations
 * are straight RFC/POSIX semantics, no locale awareness. */

static int slm_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n-- != 0) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        int la = slm_tolower(ca);
        int lb = slm_tolower(cb);
        if (la != lb) return la - lb;
        if (ca == 0)  return 0;
    }
    return 0;
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    if (saveptr == NULL || delim == NULL) return NULL;
    char *s = (str != NULL) ? str : *saveptr;
    if (s == NULL) return NULL;

    /* Skip leading delimiters. */
    while (*s != '\0') {
        const char *d = delim;
        while (*d != '\0' && *s != *d) {
            d++;
        }
        if (*d == '\0') break;
        s++;
    }
    if (*s == '\0') { *saveptr = s; return NULL; }

    char *tok = s;
    /* Find end of token. */
    while (*s != '\0') {
        const char *d = delim;
        while (*d != '\0' && *s != *d) {
            d++;
        }
        if (*d != '\0') { *s++ = '\0'; *saveptr = s; return tok; }
        s++;
    }
    *saveptr = s;
    return tok;
}

/* ---------------------------------------------------------------- */
/* Unused wolfSSL function stubs                                     */
/* ---------------------------------------------------------------- */

/* wolfSSH's DoPemKey calls wc_KeyPemToDer unconditionally — but the
 * PEM key format isn't reachable in our SLM-OS code (we use the
 * RAW format for the in-memory Ed25519 key, OPENSSH for keys on
 * disk). Returning a negative value signals the parse error path,
 * which wolfSSH handles cleanly. */
int wc_KeyPemToDer(const unsigned char *pem, int pemSz,
                   unsigned char *buff, int buffSz,
                   const char *pass)
{
    (void)pem; (void)pemSz; (void)buff; (void)buffSz; (void)pass;
    return -1;
}

/* ---------------------------------------------------------------- */
/* Time hook — provided by lua_stubs.c                               */
/* ---------------------------------------------------------------- */
/*
 * wolfSSL's USER_TIME mode calls a `time(time_t *)` function. The
 * kernel already exposes `time()` from `kernel/src/lua_stubs.c:703`
 * for Lua's `os.time()` — both consumers want monotonic seconds and
 * the lua_stubs implementation does exactly that. We deliberately do
 * NOT redefine `time()` here to avoid a multiple-definition link
 * error; the wolfSSL build picks up the lua_stubs symbol via the
 * common linker namespace. */
