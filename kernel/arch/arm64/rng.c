/*
 * arch/arm64/rng.c - ARM64 FEAT_RNG (RNDR) probe + reader.
 *
 * FEAT_RNG advertises itself in ID_AA64ISAR0_EL1 bits [63:60]:
 *
 *     RNDR = 0b0001 => FEAT_RNG implemented (RNDR + RNDRRS available)
 *
 * Cortex-A78AE (Jetson Orin Nano) implements FEAT_RNG.
 * Cortex-A76 (Pi 5) does NOT — the jitter pool is the only path there.
 * QEMU TCG does not implement FEAT_RNG either; QEMU KVM inherits the
 * host's setting.
 *
 * On a successful RNDR read, NZCV is set to 0b0000.
 * On a failure (entropy bus stall, transient ill-condition), NZCV is
 * set to 0b0100 — the Z flag is set. We branch on Z to detect failure
 * and retry up to a small per-call budget before reporting failure
 * upwards (the common path falls back to the jitter pool).
 *
 * Per ARM ARM D7.2.97: RNDR reads are blocking — the architectural
 * commitment is that bytes are not returned until the conditioning
 * function has produced new output. There is no upper bound on the
 * blocking time, but in practice a single 64-bit read completes in
 * tens of ns on Cortex-A78AE.
 *
 * Reading RNDR requires PSTATE.RNG access. Available at EL2 (Jetson's
 * runtime EL post-kexec) without HCR_EL2.{RW,E2H} requirements; the
 * Pi 5 path is uninteresting because A76 doesn't have FEAT_RNG.
 */

#include "rng.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool g_rndr_available;

/* Read ID_AA64ISAR0_EL1[63:60] (RNDR field). */
static uint32_t isar0_rndr_field(void)
{
    uint64_t isar0;
    __asm__ volatile("mrs %0, ID_AA64ISAR0_EL1" : "=r"(isar0));
    return (uint32_t)((isar0 >> 60) & 0xFu);
}

/*
 * Attempt one RNDR read. Returns 0 on success and writes 8 bytes to
 * `out`. Returns non-zero on TRNG failure; the per-call retry budget
 * is the caller's concern (we keep this primitive trivial).
 *
 * RNDR sets PSTATE.{N=0, Z=success-ind, C=0, V=0}.
 *
 *     Z = 0 => success, 64-bit random value in RNDR
 *     Z = 1 => entropy unavailable; RNDR reads as 0
 *
 * We capture the flags via `cset` and return them up.
 */
static int rndr_read_one(uint64_t *out)
{
    uint64_t v;
    uint64_t fail;

    __asm__ volatile(
        "mrs %0, s3_3_c2_c4_0\n\t"  /* RNDR: op0=3, op1=3, CRn=2, CRm=4, op2=0 */
        "cset %1, eq\n\t"            /* fail = (Z == 1) */
        : "=r"(v), "=r"(fail)
        :
        : "cc"
    );

    /* Use the system register name spelling that older binutils accept:
     * RNDR is encoded as system register {3, 3, c2, c4, 0}. Newer
     * binutils accept the mnemonic `mrs %0, RNDR` directly; the
     * encoded form is portable across all toolchain versions SLM-OS
     * supports today. */

    if (fail != 0) {
        return -1;
    }
    *out = v;
    return 0;
}

enum rng_source rng_arch_probe(void)
{
    if (isar0_rndr_field() == 0u) {
        g_rndr_available = false;
        return RNG_SOURCE_NONE;
    }

    /* Self-test the read path: pull 4 words. If every one fails, the
     * advertised feature isn't usable — fall back to jitter. */
    uint64_t tmp;
    int success = 0;
    for (int i = 0; i < 4; i++) {
        if (rndr_read_one(&tmp) == 0) {
            success++;
        }
    }
    if (success == 0) {
        g_rndr_available = false;
        return RNG_SOURCE_NONE;
    }

    g_rndr_available = true;
    return RNG_SOURCE_RNDR;
}

int rng_arch_read(uint8_t *buf, size_t len)
{
    if (!g_rndr_available) return -1;
    if (buf == NULL)       return -1;
    if (len == 0u)         return 0;

    /* Per-call retry budget. 16 attempts per 8-byte block, matching
     * the common path's RNG_TRNG_RETRY_BUDGET intent. */
    enum { RETRY_PER_BLOCK = 16 };

    size_t off = 0;
    while (off < len) {
        uint64_t word = 0;
        int      rc   = -1;
        for (int attempt = 0; attempt < RETRY_PER_BLOCK; attempt++) {
            rc = rndr_read_one(&word);
            if (rc == 0) break;
        }
        if (rc != 0) {
            return -1;   /* common path will degrade to jitter */
        }

        size_t take = len - off;
        if (take > sizeof(word)) take = sizeof(word);
        for (size_t i = 0; i < take; i++) {
            buf[off + i] = (uint8_t)(word >> (8u * i));
        }
        off += take;
    }
    return 0;
}
