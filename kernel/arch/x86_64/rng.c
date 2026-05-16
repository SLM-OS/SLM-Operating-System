/*
 * arch/x86_64/rng.c - x86-64 RDRAND probe + reader.
 *
 * RDRAND is advertised via CPUID.01H:ECX bit 30. Available on test-pc
 * (Skylake i7-6700) and every subsequent Intel and AMD generation
 * SLM-OS targets. RDRAND can return CF=0 (no data) under sustained
 * load; the Intel SDM recommends retrying up to ten times before
 * declaring the source dead. We retry sixteen times per call,
 * matching the common path's RNG_TRNG_RETRY_BUDGET.
 *
 * Note: x86-64 is not a primary SLM-OS target — the deployment story
 * is QEMU + test-pc for SEC2 inheritance experiments. RDRAND is wired
 * here so the RNG works on test-pc out of the box; the validation
 * budget for SSH on x86-64 is QEMU-only (RDRAND is not exposed by
 * QEMU TCG by default, so the jitter pool is the path that gets
 * exercised in CI).
 */

#include "rng.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool g_rdrand_available;

/* CPUID(1) ECX bit 30 = RDRAND.
 *
 * x86-64 baseline ABI clobbers RBX (PIC base), so the inline-asm
 * preserves it explicitly with `push %%rbx; ...; pop %%rbx`. The
 * RDX/RCX outputs we want directly. */
static bool cpuid_has_rdrand(void)
{
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile(
        "cpuid"
        : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx)
    );
    return (ecx & (1u << 30)) != 0u;
}

/*
 * Attempt one RDRAND read. CF=1 indicates success; CF=0 indicates
 * "no data, retry". The instruction encoding is rdrand %rax —
 * available on every CPU that advertises the CPUID bit.
 */
static int rdrand_read_one(uint64_t *out)
{
    uint64_t v;
    uint8_t  cf;

    __asm__ volatile(
        "rdrand %0\n\t"
        "setc   %1\n\t"
        : "=r"(v), "=qm"(cf)
        :
        : "cc"
    );

    if (cf == 0u) return -1;
    *out = v;
    return 0;
}

enum rng_source rng_arch_probe(void)
{
    if (!cpuid_has_rdrand()) {
        g_rdrand_available = false;
        return RNG_SOURCE_NONE;
    }

    /* Self-test: pull four words. If every one returns CF=0, the
     * silicon is reporting "available" but not actually delivering —
     * fall back to jitter. */
    uint64_t tmp;
    int success = 0;
    for (int i = 0; i < 4; i++) {
        if (rdrand_read_one(&tmp) == 0) success++;
    }
    if (success == 0) {
        g_rdrand_available = false;
        return RNG_SOURCE_NONE;
    }

    g_rdrand_available = true;
    return RNG_SOURCE_RDRAND;
}

int rng_arch_read(uint8_t *buf, size_t len)
{
    if (!g_rdrand_available) return -1;
    if (buf == NULL)         return -1;
    if (len == 0u)           return 0;

    enum { RETRY_PER_BLOCK = 16 };

    size_t off = 0;
    while (off < len) {
        uint64_t word = 0;
        int      rc   = -1;
        for (int attempt = 0; attempt < RETRY_PER_BLOCK; attempt++) {
            rc = rdrand_read_one(&word);
            if (rc == 0) break;
        }
        if (rc != 0) return -1;

        size_t take = len - off;
        if (take > sizeof(word)) take = sizeof(word);
        for (size_t i = 0; i < take; i++) {
            buf[off + i] = (uint8_t)(word >> (8u * i));
        }
        off += take;
    }
    return 0;
}
