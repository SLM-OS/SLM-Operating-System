/*
 * gpu-qmd-bits.h — Pure-logic bit-range setter for the QMDV03_00
 * struct (Ampere). Lives in its own header — separate from
 * gpu-launch-common.{h,c} — so the host-test harness can include
 * it directly without dragging in /usr/src/nvidia/nvgpu/... .
 *
 * Used by: scripts/gpu-launch-common.c (Linux launchers),
 *          host-tools/gsp-harness/test_ga10b_bringup.c (host tests).
 */
#ifndef GPU_QMD_BITS_H
#define GPU_QMD_BITS_H

#include <stdint.h>

/*
 * Pack `val` into bits [hi:lo] of the 256-byte QMD stored as
 * `qmd[64]` uint32s. Handles fields that span a 32-bit dword
 * boundary. Bits in `val` above (hi - lo + 1) are masked off.
 *
 * `static inline` so the host-test harness can include this header
 * directly without linking against gpu-launch-common.c (which
 * pulls in nvgpu/nvmap headers that only exist on Jetson).
 */
static inline void
gpu_qmd_set_bits(uint32_t *qmd, unsigned hi, unsigned lo, uint64_t val)
{
    unsigned nbits = hi - lo + 1;
    /* The `nbits >= 64` branch isn't dead — it guards the full-width
     * case against `1ULL << 64`, which is undefined behavior per C
     * (shift by ≥ width). All Ampere QMDV03_00 fields used today are
     * ≤ 32 bits, so the guard is precautionary, but cheap and
     * correct, so leave it. */
    uint64_t mask = (nbits >= 64) ? ~0ULL : ((1ULL << nbits) - 1ULL);
    val &= mask;

    unsigned word_lo = lo / 32;
    unsigned word_hi = hi / 32;
    unsigned shift_lo = lo % 32;

    if (word_lo == word_hi) {
        uint32_t wmask = (uint32_t)(mask) << shift_lo;
        qmd[word_lo] = (qmd[word_lo] & ~wmask) |
                       (((uint32_t)val << shift_lo) & wmask);
    } else {
        /* Range spans two 32-bit words: low bits of `val` go to
         * word_lo[shift_lo..31]; high bits to word_hi[0..(hi%32)]. */
        unsigned bits_in_lo = 32 - shift_lo;
        uint32_t lo_mask = (0xFFFFFFFFu << shift_lo);
        qmd[word_lo] = (qmd[word_lo] & ~lo_mask) |
                       (((uint32_t)val << shift_lo) & lo_mask);

        unsigned bits_in_hi = nbits - bits_in_lo;
        uint32_t hi_mask = (bits_in_hi >= 32) ? 0xFFFFFFFFu :
                           ((1u << bits_in_hi) - 1u);
        uint64_t hi_val = val >> bits_in_lo;
        qmd[word_hi] = (qmd[word_hi] & ~hi_mask) |
                       ((uint32_t)hi_val & hi_mask);
    }
}

#endif /* GPU_QMD_BITS_H */
