/*
 * rng.c - Crypto-quality RNG for SLM-OS (common path).
 *
 * See `kernel/include/rng.h` for the API contract.
 *
 * Design at a glance:
 *
 *   - rng_init probes for an architectural TRNG (rng_arch_probe).
 *     On success, the TRNG is the primary source. On failure, the
 *     jitter pool is the primary source.
 *
 *   - The jitter pool is a SHA-256-mixed entropy accumulator:
 *
 *         pool = SHA-256(pool || material)
 *
 *     where `material` at boot time is DTB /chosen entropy + a
 *     short CNTPCT-jitter capture, and at runtime is a CNTPCT
 *     sample folded in on every output call.
 *
 *   - Output bytes come from a SHA-256 squeeze:
 *
 *         block = SHA-256(pool || counter); counter += 1
 *
 *     Counter never wraps in any realistic boot (64-bit) and is
 *     re-seeded with fresh CNTPCT delta every JITTER_RESEED_PERIOD
 *     bytes to bound state compromise after a long boot.
 *
 *   - The TRNG path delegates to rng_arch_read which retries the
 *     underlying instruction up to a per-call budget (#199e audit
 *     uses this to catch RDRAND stuck-state). On exhaustion, the
 *     common path degrades to the jitter pool for that call AND
 *     increments hw_degrade_events so the operator can correlate
 *     during the entropy audit.
 *
 *   - All state lives in this TU. No PMM allocation, no locks beyond
 *     IRQ-disable on the multi-byte fields. SSH handshake consumes
 *     ~600 bytes per session and is single-threaded inside the
 *     wolfSSH callback, so the IRQ-disable interval is microseconds.
 */

#include "rng.h"

#include "dtb.h"
#include "sha256.h"
#include "spinlock.h"
#include "string.h"
#include "timer.h"
#include "uart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Mix a fresh CNTPCT sample into the jitter pool every N output bytes.
 * 4 KB picks up at least 32 timer ticks of jitter even at the slowest
 * supported counter rate (Pi 5 ~54 MHz, Jetson ~31.25 MHz). Tuned to
 * be cheap (~one SHA-256 compress per 4 KB) while still guaranteeing
 * forward-secrecy improves on every page-sized request. */
#define RNG_JITTER_RESEED_BYTES   4096u

/* RNG_TRNG_RETRY_BUDGET lives in <rng.h> so both arch impls share the
 * same constant. */

static spinlock_t            g_rng_lock = SPINLOCK_INIT;
static enum rng_source       g_active   = RNG_SOURCE_NONE;
static enum rng_source       g_forced   = RNG_SOURCE_NONE;
static bool                  g_inited;

/* Jitter pool. Updated under g_rng_lock. */
static uint8_t               g_pool[SHA256_DIGEST_LEN];
static uint64_t              g_counter;
static uint64_t              g_bytes_since_reseed;

/* Test injection buffer — when non-empty, rng_get_bytes pulls from
 * this *for the next call only*, regardless of active source. Used
 * by the unit tests to drive deterministic vectors through the
 * shared output path. */
static const uint8_t        *g_inject_buf;
static size_t                g_inject_len;
static size_t                g_inject_pos;

/* Counters surfaced via rng_get_stats. */
static volatile uint64_t     g_bytes_served;
static volatile uint64_t     g_hw_calls;
static volatile uint64_t     g_hw_failures;
static volatile uint64_t     g_hw_degrade_events;
static volatile uint64_t     g_jitter_reseeds;

/* ---------------------------------------------------------------- */
/* Jitter pool primitives                                            */
/* ---------------------------------------------------------------- */

/*
 * Absorb `len` bytes of new material into the pool:
 *
 *     pool = SHA-256(pool || material)
 *
 * Caller holds g_rng_lock.
 */
static void pool_absorb(const void *material, size_t len)
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, g_pool, sizeof(g_pool));
    sha256_update(&ctx, material, len);
    sha256_final(&ctx, g_pool);
}

/*
 * Squeeze one 32-byte block out of the pool:
 *
 *     block = SHA-256(pool || counter); counter += 1
 *
 * The pool itself is NOT updated (that would consume one entropy
 * step per output block — instead we re-seed periodically via
 * pool_reseed_from_jitter). Caller holds g_rng_lock.
 */
static void pool_squeeze_block(uint8_t out[SHA256_DIGEST_LEN])
{
    struct sha256_ctx ctx;
    uint8_t           counter_be[8];

    /* counter as big-endian eight bytes — matches OpenSSL's HMAC-DRBG
     * convention and is easier to inspect in hex dumps. */
    for (size_t i = 0; i < 8u; i++) {
        counter_be[i] = (uint8_t)(g_counter >> (56 - (8u * i)));
    }

    sha256_init(&ctx);
    sha256_update(&ctx, g_pool, sizeof(g_pool));
    sha256_update(&ctx, counter_be, sizeof(counter_be));
    sha256_final(&ctx, out);

    g_counter++;
}

/*
 * Pull a fresh CNTPCT timestamp delta and absorb it into the pool.
 * Done at init time (multi-sample bootstrap) and periodically during
 * runtime output. Caller holds g_rng_lock.
 */
static void pool_reseed_from_jitter(void)
{
    /* Two back-to-back reads — the low bits of the delta carry the
     * jitter, which is the entropy we actually want from this source.
     * Higher bits are predictable from boot uptime. */
    uint64_t a = timer_get_count();
    /* A short busy wait keeps the two samples far enough apart on
     * platforms with very fine-grained counters that two consecutive
     * reads would return the same value (Jetson CNTPCT @ 31.25 MHz =
     * 32 ns per tick — fine on QEMU TCG too). */
    for (volatile int i = 0; i < 64; i++) {
        __asm__ volatile("");
    }
    uint64_t b = timer_get_count();

    uint64_t sample[2] = { a, b };
    pool_absorb(sample, sizeof(sample));
    g_jitter_reseeds++;
}

/*
 * Output `len` bytes from the jitter source.
 */
static int jitter_read_locked(uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        if (g_bytes_since_reseed >= RNG_JITTER_RESEED_BYTES) {
            pool_reseed_from_jitter();
            g_bytes_since_reseed = 0;
        }

        uint8_t block[SHA256_DIGEST_LEN];
        pool_squeeze_block(block);

        size_t take = len - off;
        if (take > sizeof(block)) take = sizeof(block);

        for (size_t i = 0; i < take; i++) {
            buf[off + i] = block[i];
        }
        off += take;
        g_bytes_since_reseed += take;

        /* Wipe the block from the stack frame before the next iter.
         * secure_zero uses a volatile pointer so the optimiser cannot
         * elide the writes — a plain `memset` or hand loop on
         * about-to-go-out-of-scope storage is dead-store-eliminated
         * under -O2. */
        secure_zero(block, sizeof(block));
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Init + DTB seed mix                                               */
/* ---------------------------------------------------------------- */

/*
 * Boot-time entropy mix:
 *
 *   - DTB /chosen rng-seed (firmware-supplied, up to 64 B on Pi 5
 *     and Jetson; QEMU passes through 64 B by default).
 *   - DTB /chosen kaslr-seed (16 B).
 *   - 8 CNTPCT delta samples captured back-to-back.
 *   - A boot constant so two boots with identical timing still
 *     differ in pool state.
 *
 * Reports per-source contribution byte counts via the out-params so
 * the caller can surface them in the boot banner — easier to diagnose
 * firmware regressions (e.g. Pi 5 EEPROM updates have been observed
 * to drop rng-seed while keeping kaslr-seed). `out_rng_seed_bytes`
 * and `out_kaslr_seed_bytes` may be NULL.
 *
 * The jitter samples are absorbed unconditionally regardless of
 * firmware contribution.
 */
static void mix_boot_entropy_locked(size_t *out_rng_seed_bytes,
                                    size_t *out_kaslr_seed_bytes)
{
    size_t rng_seed_bytes   = 0;
    size_t kaslr_seed_bytes = 0;

    const dtb_chosen_t *ch = dtb_get_chosen();
    if (ch != NULL) {
        if (ch->rng_seed_len > 0u && ch->rng_seed_len <= sizeof(ch->rng_seed)) {
            pool_absorb(ch->rng_seed, ch->rng_seed_len);
            rng_seed_bytes = ch->rng_seed_len;
        }
        if (ch->kaslr_seed_len > 0u && ch->kaslr_seed_len <= sizeof(ch->kaslr_seed)) {
            pool_absorb(ch->kaslr_seed, ch->kaslr_seed_len);
            kaslr_seed_bytes = ch->kaslr_seed_len;
        }
    }

    /* Capture some CNTPCT-jitter samples regardless of firmware
     * contribution. On QEMU TCG these are quasi-deterministic but
     * the timing of the busy-wait loop on the host machine still
     * varies boot-to-boot enough to matter. */
    for (int i = 0; i < 8; i++) {
        pool_reseed_from_jitter();
    }

    /* Boot tag — keeps two consecutive identical boots distinguishable
     * in the pool. Eight bytes of "I was here". */
    const uint8_t tag[] = { 'S', 'L', 'M', 'O', 'S', '/', 'r', 'n' };
    pool_absorb(tag, sizeof(tag));

    if (out_rng_seed_bytes)   *out_rng_seed_bytes   = rng_seed_bytes;
    if (out_kaslr_seed_bytes) *out_kaslr_seed_bytes = kaslr_seed_bytes;
}

int rng_init(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_rng_lock);

    if (g_inited) {
        spin_unlock_irqrestore(&g_rng_lock, flags);
        return 0;
    }

    /* Bootstrap the jitter pool unconditionally — it's the floor source
     * even when TRNG is available, used both as fallback and as
     * additional entropy mixed into the boot banner. */
    size_t rng_seed_bytes = 0;
    size_t kaslr_seed_bytes = 0;
    mix_boot_entropy_locked(&rng_seed_bytes, &kaslr_seed_bytes);

    /* Probe for hardware TRNG. */
    enum rng_source probed = rng_arch_probe();
    g_active = (probed == RNG_SOURCE_NONE) ? RNG_SOURCE_JITTER : probed;

    g_inited = true;
    spin_unlock_irqrestore(&g_rng_lock, flags);

    uart_printf("[RNG] init: source=%s rng_seed=%uB kaslr_seed=%uB\r\n",
                rng_source_name(g_active),
                (unsigned)rng_seed_bytes,
                (unsigned)kaslr_seed_bytes);
    /* rng_init's documented contract: 0 on success; negative when
     * boot-time mixing produced insufficient entropy (no firmware seed
     * AND no jitter — but jitter is always available, so the only
     * remaining signal is the firmware-seed presence). Return 0 if at
     * least one firmware seed contributed; -1 otherwise so the operator
     * can see a "low-entropy boot" signal in the banner. */
    return (rng_seed_bytes > 0u || kaslr_seed_bytes > 0u) ? 0 : -1;
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

int rng_get_bytes(uint8_t *buf, size_t len)
{
    if (buf == NULL) return -1;
    if (len == 0u)   return -1;

    irq_flags_t flags = spin_lock_irqsave(&g_rng_lock);

    if (!g_inited) {
        spin_unlock_irqrestore(&g_rng_lock, flags);
        return -1;
    }

    /* Test injection takes precedence so unit tests can drive
     * deterministic vectors through the output path without having
     * to stub out the arch hooks individually. Drains the injection
     * buffer and falls through to the live source for any tail. */
    if (g_inject_buf != NULL && g_inject_pos < g_inject_len) {
        size_t avail = g_inject_len - g_inject_pos;
        size_t take = (avail < len) ? avail : len;
        for (size_t i = 0; i < take; i++) {
            buf[i] = g_inject_buf[g_inject_pos + i];
        }
        g_inject_pos += take;
        g_bytes_served += take;
        if (take == len) {
            spin_unlock_irqrestore(&g_rng_lock, flags);
            return 0;
        }
        buf += take;
        len -= take;
    }

    enum rng_source use = (g_forced == RNG_SOURCE_NONE) ? g_active : g_forced;

    int rc = -1;
    if (use == RNG_SOURCE_RNDR || use == RNG_SOURCE_RDRAND) {
        g_hw_calls++;
        rc = rng_arch_read(buf, len);
        if (rc != 0) {
            g_hw_failures++;
            g_hw_degrade_events++;
            /* Fall back to jitter on TRNG failure. Pool is always
             * available as the floor source. */
            rc = jitter_read_locked(buf, len);
        }
    } else {
        rc = jitter_read_locked(buf, len);
    }

    if (rc == 0) {
        g_bytes_served += len;
    }

    spin_unlock_irqrestore(&g_rng_lock, flags);
    return rc;
}

uint32_t rng_get_u32(void)
{
    uint8_t bytes[4];
    if (rng_get_bytes(bytes, sizeof(bytes)) != 0) {
        return 0u;
    }
    return ((uint32_t)bytes[0])       |
           ((uint32_t)bytes[1] << 8)  |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

enum rng_source rng_active_source(void)
{
    return g_active;
}

const char *rng_source_name(enum rng_source s)
{
    switch (s) {
    case RNG_SOURCE_RNDR:   return "RNDR";
    case RNG_SOURCE_RDRAND: return "RDRAND";
    case RNG_SOURCE_JITTER: return "jitter";
    case RNG_SOURCE_NONE:
    default:                return "none";
    }
}

bool rng_has_trng(void)
{
    return g_active == RNG_SOURCE_RNDR || g_active == RNG_SOURCE_RDRAND;
}

void rng_get_stats(struct rng_stats *out)
{
    if (out == NULL) return;
    out->bytes_served      = g_bytes_served;
    out->hw_calls          = g_hw_calls;
    out->hw_failures       = g_hw_failures;
    out->hw_degrade_events = g_hw_degrade_events;
    out->jitter_reseeds    = g_jitter_reseeds;
}

enum rng_source rng_test_force_source(enum rng_source src)
{
    irq_flags_t flags = spin_lock_irqsave(&g_rng_lock);
    enum rng_source prev = g_forced;
    g_forced = src;
    spin_unlock_irqrestore(&g_rng_lock, flags);
    return prev;
}

size_t rng_test_inject_bytes(const uint8_t *bytes, size_t len)
{
    irq_flags_t flags = spin_lock_irqsave(&g_rng_lock);
    g_inject_buf = bytes;
    g_inject_len = (bytes == NULL) ? 0u : len;
    g_inject_pos = 0u;
    size_t accepted = g_inject_len;
    spin_unlock_irqrestore(&g_rng_lock, flags);
    return accepted;
}

/* ---------------------------------------------------------------- */
/* Self-test                                                          */
/* ---------------------------------------------------------------- */

/*
 * Pull 4 KB and apply a chi-squared sanity check over the byte
 * histogram. The pass threshold is intentionally generous (the goal
 * is to catch failure modes like "all bytes zero" / "stuck state",
 * not to make a fine-grained entropy claim). #199e (#895) adds the
 * NIST SP800-90B battery against this same source.
 *
 * Critical value: with 4096 samples and 255 degrees of freedom, the
 * 99.9th-percentile chi-square is ~380 (uniform null hypothesis).
 * Our pass threshold is 600 — a sane RNG clears it by a wide margin,
 * a stuck RNG fails it spectacularly. The integer arithmetic here
 * keeps the math in fixed-point to avoid pulling soft-float; we
 * compute `sum_sq * 256` and compare against `(expected * 4096 * 256)
 * + threshold * (expected * 4096)`. Since `expected = 16` and we're
 * scaling everything by 256, the comparison is over integers up to
 * roughly 268 M — comfortably within uint64_t.
 */
int rng_selftest(char *reason, size_t reason_cap)
{
    enum {
        SAMPLES      = 4096,
        BINS         = 256,
        EXPECTED     = SAMPLES / BINS,         /* 16 */
        SCALE        = 256,                    /* fixed-point factor */
        FAIL_THRESH  = 600,                    /* chi-squared cutoff */
    };

    uint8_t  buf[SAMPLES];
    uint32_t hist[BINS];

    for (size_t i = 0; i < BINS; i++) {
        hist[i] = 0;
    }

    if (rng_get_bytes(buf, sizeof(buf)) != 0) {
        if (reason != NULL && reason_cap > 0u) {
            const char *m = "rng_get_bytes failed";
            size_t mn = 0;
            while (m[mn] != '\0' && (mn + 1u) < reason_cap) {
                reason[mn] = m[mn];
                mn++;
            }
            reason[mn] = '\0';
        }
        return -1;
    }

    for (size_t i = 0; i < SAMPLES; i++) {
        hist[buf[i]]++;
    }

    /* chi^2 = sum ((observed - expected)^2 / expected).
     *
     * To stay in integer math: compute SUM = sum (observed - expected)^2.
     * The chi^2 value is SUM / expected. Threshold check is therefore:
     *
     *      SUM > FAIL_THRESH * expected   ?
     *
     * No floats, no fixed-point gymnastics.
     */
    uint64_t sum_sq_dev = 0;
    for (size_t i = 0; i < BINS; i++) {
        int32_t  dev = (int32_t)hist[i] - (int32_t)EXPECTED;
        int64_t  sq  = (int64_t)dev * (int64_t)dev;
        sum_sq_dev += (uint64_t)sq;
    }

    uint64_t threshold = (uint64_t)FAIL_THRESH * (uint64_t)EXPECTED;
    if (sum_sq_dev > threshold) {
        if (reason != NULL && reason_cap > 0u) {
            /* "chi^2 too high: X > Y (source=Z)" — we can't depend on
             * snprintf here so the message is a short fixed prefix
             * and the operator can re-run with the diagnostic command
             * to get the numbers. */
            const char *m = "chi-squared above pass threshold";
            size_t mn = 0;
            while (m[mn] != '\0' && (mn + 1u) < reason_cap) {
                reason[mn] = m[mn];
                mn++;
            }
            reason[mn] = '\0';
        }
        return -2;
    }

    if (reason != NULL && reason_cap > 0u) reason[0] = '\0';
    return 0;
}
