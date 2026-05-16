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

/* Kernel-side byte allocator. Provided by lua_stubs.c — same heap
 * the rest of the kernel uses (Rust-backed bump+free allocator). */
extern void *malloc(size_t size);
extern void  free(void *ptr);
extern void *realloc(void *ptr, size_t size);

/* The wolfssl heap and type arguments are advisory — we discard
 * them. Forward to the kernel allocator. */
void *XMALLOC(size_t n, void *heap, int type)
{
    (void)heap;
    (void)type;
    return malloc(n);
}

void XFREE(void *p, void *heap, int type)
{
    (void)heap;
    (void)type;
    free(p);
}

void *XREALLOC(void *p, size_t n, void *heap, int type)
{
    (void)heap;
    (void)type;
    return realloc(p, n);
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
/* Time hook                                                         */
/* ---------------------------------------------------------------- */

/*
 * USER_TIME pulls our `time` / `XTIME` into wolfSSL. The actual
 * macro substitution happens inside wolfcrypt: `XTIME(t)` ends up
 * calling `time(t)` (a function name, not the standard libc symbol —
 * wolfSSL uses the name without a header so we control the
 * declaration). Return value: monotonic seconds since boot.
 *
 * `time_t` here is the toolchain's freestanding header definition.
 * We pull it in via <time.h>; the bare-metal aarch64 toolchain
 * provides this header with `time_t = long` and no library symbol,
 * so the declaration here is fine.
 */
#include <time.h>

time_t time(time_t *out)
{
    /* CNTPCT-based monotonic seconds. timer_get_frequency() returns
     * the counter Hz; integer division gives whole seconds. */
    uint64_t cnt  = timer_get_count();
    uint64_t freq = timer_get_frequency();
    time_t   secs = (freq != 0) ? (time_t)(cnt / freq) : (time_t)0;
    if (out != NULL) {
        *out = secs;
    }
    return secs;
}
