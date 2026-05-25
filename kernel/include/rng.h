/*
 * rng.h - Crypto-quality random number generator for SLM-OS.
 *
 * Distinct from `lwip_rand_slm` (kernel/include/net.h:390-420), which
 * is a per-boot-seeded LCG used for TCP ISN / ephemeral-port choices
 * and is explicitly documented as NOT cryptographically secure.
 *
 * The API here is the entropy source the SSH server (Phase 3, #199)
 * builds on. It is also the entry point future crypto callers (host
 * key generation, password salts, session-key derivation) should use.
 *
 * Source ordering — `rng_init` probes in this order and locks the
 * result in for the lifetime of the boot:
 *
 *   1. Architectural TRNG (ARM64 FEAT_RNG `RNDR`, x86-64 `RDRAND`).
 *      Used directly when present and self-test passes.
 *   2. Timer-jitter PRNG. SHA-256-mixed entropy pool seeded from
 *      DTB `/chosen/{rng-seed,kaslr-seed}` (firmware-supplied bytes),
 *      `CNTPCT_EL0` deltas across short busy loops, and a Knuth-style
 *      counter. Used when no hardware TRNG is available — Cortex-A76
 *      (Pi 5) lacks FEAT_RNG, and QEMU TCG does not implement RNDR.
 *
 * Entropy quality of the jitter source is best-effort; #199e (#895)
 * will run a NIST SP800-90B / `ent` battery against it and document
 * the result. The TRNG path delegates the entropy guarantee to the
 * CPU vendor — see the per-arch implementations for self-test details.
 *
 * Calls block (busy-poll for hardware TRNG; spin through the SHA-256
 * compress for the jitter path). Neither path is on a hot scheduler
 * path — wolfSSH consumes a few KB per session at handshake time and
 * none thereafter. Adding a coarser-grained yield point inside the
 * jitter loop is fine if profiles show a need.
 */

#ifndef RNG_H
#define RNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum rng_source {
    RNG_SOURCE_NONE   = 0,  /* unprobed / no entropy available (treat as error) */
    RNG_SOURCE_RNDR   = 1,  /* ARM64 FEAT_RNG, available on Cortex-A78AE / Jetson */
    RNG_SOURCE_RDRAND = 2,  /* x86-64 RDRAND, available on test-pc Skylake */
    RNG_SOURCE_JITTER = 3,  /* SHA-256-mixed timer-jitter pool fallback */
};

/*
 * Initialize the RNG. Probes for an architectural TRNG and falls
 * back to the jitter pool if absent. Mixes in the DTB `/chosen`
 * firmware seed (`rng-seed` + `kaslr-seed`) so the jitter pool is
 * pre-seeded with bytes the bootloader provided.
 *
 * Safe to call multiple times; only the first call has effect.
 * Returns 0 on success. The jitter path is always available as the
 * floor; a non-zero return indicates the boot-time mixing produced
 * insufficient entropy (DTB seed missing AND CNTPCT delta below
 * sanity threshold). The pool is still seeded with something — the
 * call signals "operator should treat this boot as low-entropy" and
 * the boot banner will reflect it.
 */
int rng_init(void);

/*
 * Fill `len` bytes of `buf` with crypto-quality random bytes.
 *
 * Returns 0 on success. On failure (rng_init never ran successfully,
 * NULL buf, len == 0) returns negative and `buf` is unmodified.
 *
 * The TRNG path retries internally up to a budget (RDRAND has been
 * observed to return zero ten times in a row on stressed Skylake);
 * if the budget is exhausted, the call degrades to the jitter pool
 * and continues, recording the degradation event for the boot banner.
 */
int rng_get_bytes(uint8_t *buf, size_t len);

/*
 * Convenience: one 32-bit word. Same semantics as rng_get_bytes for
 * a 4-byte buffer. Returns 0 on failure (callers that care must use
 * rng_get_bytes directly — but every caller's failure path is "abort
 * the session" so the silent-zero is acceptable for the typical
 * SSH-side use sites).
 */
uint32_t rng_get_u32(void);

/*
 * Which source is the RNG currently using? Stable after rng_init
 * returns. Reads are atomic on every supported arch (it's a 32-bit
 * enum stored in a single word).
 */
enum rng_source rng_active_source(void);

/* Stable string for a source. Never NULL. */
const char *rng_source_name(enum rng_source s);

/* True iff a hardware TRNG was probed successfully and is in use. */
bool rng_has_trng(void);

/*
 * Statistics surfaced to the `rng` shell command + diagnostics. Each
 * counter is a `volatile uint64_t` updated under `g_rng_lock`; reads
 * are atomic per-field but the struct as a whole is a best-effort
 * snapshot — concurrent rng_get_bytes calls may interleave between
 * field reads, leaving the snapshot internally inconsistent. Counters
 * are monotonic since boot; resetting requires a reboot.
 *
 * Latency: rng_get_bytes holds `g_rng_lock` (IRQ-disable) across the
 * jitter SHA-256 squeeze. A 256-byte request is ~80 µs of disabled
 * IRQs on Pi 5. Safe for SSH-handshake frequency; not safe for an
 * inner hot loop — re-shape the API to a per-task DRBG if a hot-path
 * crypto consumer appears. */
struct rng_stats {
    uint64_t bytes_served;           /* total returned to callers */
    uint64_t hw_calls;               /* underlying TRNG-instruction invocations */
    uint64_t hw_failures;            /* CF/Z indicated failure on a TRNG call */
    uint64_t hw_degrade_events;      /* full-call retries exhausted, fell back */
    uint64_t jitter_reseeds;         /* jitter pool reseeded from CNTPCT */
};

void rng_get_stats(struct rng_stats *out);

/*
 * Per-architecture hook surface. Implementations live under
 * kernel/arch/{arm64,x86_64}/rng.c. The common path in
 * kernel/src/rng.c never calls these directly outside of rng_init's
 * probe step + rng_get_bytes's hot path; tests can call them directly
 * to drive the probe / read for coverage.
 *
 * rng_arch_probe must run before rng_arch_read. Returns the detected
 * source (RNDR / RDRAND / NONE) and stamps internal arch state so
 * subsequent rng_arch_read calls work. Idempotent.
 *
 * rng_arch_read returns 0 on success. The arch is expected to retry
 * internally up to a per-call budget; a non-zero return means the
 * budget was exhausted and the caller should degrade to the jitter
 * pool. `len` may be any value up to a few KB.
 */
enum rng_source rng_arch_probe(void);
int             rng_arch_read(uint8_t *buf, size_t len);

/*
 * Per-call TRNG retry budget shared by both arch implementations and
 * the common path. RDRAND has been observed to return zero ten times
 * in a row on stressed Skylake; ARM RNDR retries are cheap. 16 leaves
 * headroom for both. Bump after the #199e entropy audit if any
 * platform shows a tail in its failure-rate histogram. The single
 * definition here keeps the two arch impls from drifting.
 */
#define RNG_TRNG_RETRY_BUDGET   16u

/*
 * Test hook: force the next rng_get_bytes call(s) to use a specific
 * source. Useful for coverage tests of the jitter path even on hosts
 * that probed RNDR/RDRAND successfully. `src == RNG_SOURCE_NONE`
 * reverts to the auto-probed source. Returns the previous force-source.
 */
enum rng_source rng_test_force_source(enum rng_source src);

/*
 * Test hook: inject a synthetic chunk of "TRNG output" so the arch
 * path can be exercised in a self-contained unit test. Returns the
 * number of bytes accepted. Cleared on the next rng_init or on an
 * explicit pass of NULL/0.
 *
 * **Lifetime:** the buffer is stored by reference, not copied. The
 * caller MUST ensure `bytes` remains live (not freed, not stack-
 * popped, not overwritten with unrelated data) until either the
 * injection drains naturally via rng_get_bytes calls or the caller
 * explicitly clears the injection with `rng_test_inject_bytes(NULL, 0)`.
 *
 * In-tree callers (`kernel/tests/test_rng.c`) pass `static const`
 * buffers, which satisfy the invariant trivially. */
size_t rng_test_inject_bytes(const uint8_t *bytes, size_t len);

/*
 * Run a basic self-test on the active source. Pulls 4 KB and applies
 * a chi-squared sanity check over the byte histogram; pass threshold
 * is generous (any sane source clears it comfortably, the goal is
 * to catch "RDRAND returned all zeros" / stuck-state failures, not
 * to make a statistical claim).
 *
 * On success returns 0. On failure returns negative and writes a
 * short reason into `reason` (caller-supplied, at least 64 bytes).
 *
 * The shell command `rng selftest` is a thin wrapper over this.
 */
int rng_selftest(char *reason, size_t reason_cap);

#endif /* RNG_H */
