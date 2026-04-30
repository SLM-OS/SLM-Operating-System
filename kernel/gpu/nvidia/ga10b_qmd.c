/*
 * ga10b_qmd.c — Ampere QMDV03_00 descriptor encoder.
 *
 * Mirror of scripts/gpu-launch-common.c:gpu_populate_qmd_at, ported
 * for SLM-OS. Same bit layout, same defaults; the only difference
 * is that the Linux-side helper calls msync(MS_SYNC) after writing
 * while SLM-OS callers invoke gsp_platform->cache_clean separately
 * — keeping this file pure-logic lets host tests exercise the
 * encoder with no platform vtable.
 *
 * Phase 1 of docs/gpu-qmd-per-dispatch-plan.md (issue #558).
 */

#include "ga10b_qmd.h"

/* Local memcpy/memset replacement: we want this file freestanding-
 * safe (no <string.h>) so it compiles cleanly under -ffreestanding. */
static inline void ga10b_qmd_zero(uint32_t *qmd)
{
    for (size_t i = 0; i < GA10B_QMD_DWORDS; i++) {
        qmd[i] = 0u;
    }
}

void ga10b_qmd_set_bits(uint32_t *qmd, unsigned hi, unsigned lo,
                        uint64_t val)
{
    /* Defensive: all QMDV03_00 macros in the header have HI ≥ LO,
     * but a typo in a future addition (or an out-of-range caller)
     * would underflow `hi - lo + 1u` on the next line and write to
     * far-past-end words via the two-word path. Bail out cleanly
     * instead. Same safety-vs-cost trade as the `nbits >= 64u`
     * guard below. */
    if (hi < lo) return;

    unsigned nbits = hi - lo + 1u;
    /* Guard against `1ULL << 64` (UB). All QMDV03_00 fields used today
     * are ≤ 32 bits, so this branch is precautionary. */
    uint64_t mask = (nbits >= 64u) ? ~0ULL : ((1ULL << nbits) - 1ULL);
    val &= mask;

    unsigned word_lo  = lo / 32u;
    unsigned word_hi  = hi / 32u;
    unsigned shift_lo = lo % 32u;

    if (word_lo == word_hi) {
        uint32_t wmask = (uint32_t)mask << shift_lo;
        qmd[word_lo] = (qmd[word_lo] & ~wmask)
                     | (((uint32_t)val << shift_lo) & wmask);
    } else {
        /* Range spans two 32-bit words: low bits go to
         * word_lo[shift_lo..31]; high bits to word_hi[0..(hi%32)]. */
        unsigned bits_in_lo = 32u - shift_lo;
        uint32_t lo_mask    = 0xFFFFFFFFu << shift_lo;
        qmd[word_lo] = (qmd[word_lo] & ~lo_mask)
                     | (((uint32_t)val << shift_lo) & lo_mask);

        unsigned bits_in_hi = nbits - bits_in_lo;
        uint32_t hi_mask    = (bits_in_hi >= 32u) ? 0xFFFFFFFFu
                                                  : ((1u << bits_in_hi) - 1u);
        uint64_t hi_val     = val >> bits_in_lo;
        qmd[word_hi] = (qmd[word_hi] & ~hi_mask)
                     | ((uint32_t)hi_val & hi_mask);
    }
}

void ga10b_qmd_populate(uint32_t *qmd,
                        uint64_t shader_gpu_va,
                        uint64_t cbuf_gpu_va,
                        uint32_t register_count_v,
                        uint32_t grid_x, uint32_t grid_y, uint32_t grid_z,
                        uint32_t block_x, uint32_t block_y, uint32_t block_z)
{
    /* Defensive: symmetric with ga10b_qmd_pool_prepare's null guards.
     * A buggy upstream caller passing NULL would otherwise deref via
     * ga10b_qmd_zero on the next line. */
    if (qmd == NULL) return;

    ga10b_qmd_zero(qmd);

    /* Version + enum defaults (matches NVK's qmd_init!). */
    ga10b_qmd_set_bits(qmd, GA10B_QMD_MAJOR_VERSION_HI,
                       GA10B_QMD_MAJOR_VERSION_LO, 3u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_VERSION_HI,
                       GA10B_QMD_VERSION_LO, 0u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_API_VISIBLE_CALL_LIMIT_BIT,
                       GA10B_QMD_API_VISIBLE_CALL_LIMIT_BIT, 1u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_SAMPLER_INDEX_BIT,
                       GA10B_QMD_SAMPLER_INDEX_BIT, 0u);

    /* Grid (CTA raster) dimensions. */
    ga10b_qmd_set_bits(qmd, GA10B_QMD_CTA_RASTER_WIDTH_HI,
                       GA10B_QMD_CTA_RASTER_WIDTH_LO, grid_x);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_CTA_RASTER_HEIGHT_HI,
                       GA10B_QMD_CTA_RASTER_HEIGHT_LO, grid_y);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_CTA_RASTER_DEPTH_HI,
                       GA10B_QMD_CTA_RASTER_DEPTH_LO, grid_z);

    /* Block (CTA thread dim) dimensions. */
    ga10b_qmd_set_bits(qmd, GA10B_QMD_CTA_THREAD_DIM0_HI,
                       GA10B_QMD_CTA_THREAD_DIM0_LO, block_x);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_CTA_THREAD_DIM1_HI,
                       GA10B_QMD_CTA_THREAD_DIM1_LO, block_y);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_CTA_THREAD_DIM2_HI,
                       GA10B_QMD_CTA_THREAD_DIM2_LO, block_z);

    /* Shader program address (Ampere: absolute, no shift). */
    ga10b_qmd_set_bits(qmd, GA10B_QMD_PROGRAM_ADDRESS_LOWER_HI,
                       GA10B_QMD_PROGRAM_ADDRESS_LOWER_LO,
                       shader_gpu_va & 0xFFFFFFFFu);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_PROGRAM_ADDRESS_UPPER_HI,
                       GA10B_QMD_PROGRAM_ADDRESS_UPPER_LO,
                       (shader_gpu_va >> 32) & 0x1FFFFu);

    /* Registers / shmem / SLM / barriers (defaults; caller may
     * override via ga10b_qmd_set_bits). */
    ga10b_qmd_set_bits(qmd, GA10B_QMD_REGISTER_COUNT_V_HI,
                       GA10B_QMD_REGISTER_COUNT_V_LO,
                       register_count_v);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_SHARED_MEMORY_SIZE_HI,
                       GA10B_QMD_SHARED_MEMORY_SIZE_LO, 0u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_SHADER_LOCAL_MEM_LOW_SIZE_HI,
                       GA10B_QMD_SHADER_LOCAL_MEM_LOW_SIZE_LO, 0u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_SHADER_LOCAL_MEM_HIGH_SIZE_HI,
                       GA10B_QMD_SHADER_LOCAL_MEM_HIGH_SIZE_LO, 0u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_BARRIER_COUNT_HI,
                       GA10B_QMD_BARRIER_COUNT_LO, 0u);

    /* Global caching + cache invalidate all. Safer on first dispatch
     * after channel setup; cheap to set per QMD. */
    ga10b_qmd_set_bits(qmd, GA10B_QMD_SM_GLOBAL_CACHING_ENABLE_BIT,
                       GA10B_QMD_SM_GLOBAL_CACHING_ENABLE_BIT, 1u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_INVALIDATE_TEXTURE_HEADER_CACHE_BIT,
                       GA10B_QMD_INVALIDATE_TEXTURE_HEADER_CACHE_BIT, 1u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_INVALIDATE_TEXTURE_SAMPLER_CACHE_BIT,
                       GA10B_QMD_INVALIDATE_TEXTURE_SAMPLER_CACHE_BIT, 1u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_INVALIDATE_TEXTURE_DATA_CACHE_BIT,
                       GA10B_QMD_INVALIDATE_TEXTURE_DATA_CACHE_BIT, 1u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_INVALIDATE_SHADER_DATA_CACHE_BIT,
                       GA10B_QMD_INVALIDATE_SHADER_DATA_CACHE_BIT, 1u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_INVALIDATE_INSTRUCTION_CACHE_BIT,
                       GA10B_QMD_INVALIDATE_INSTRUCTION_CACHE_BIT, 1u);
    ga10b_qmd_set_bits(qmd, GA10B_QMD_INVALIDATE_SHADER_CONSTANT_CACHE_BIT,
                       GA10B_QMD_INVALIDATE_SHADER_CONSTANT_CACHE_BIT, 1u);

    /* cbuf[0]: CUDA-compatible param area at cbuf_gpu_va. The valid
     * bit goes on, size encodes shifted-by-4 (so the field is 16-byte
     * granular; matches NVK's qmd_impl_set_cbuf!(NONE, SHIFTED4)). */
    const unsigned cbuf_idx = 0u;
    const uint32_t cbuf_size_B = GA10B_QMD_CBUF0_SIZE_BYTES;
    ga10b_qmd_set_bits(qmd,
                       GA10B_QMD_CBUF_ADDR_LO_BASE + cbuf_idx * 64u + 31u,
                       GA10B_QMD_CBUF_ADDR_LO_BASE + cbuf_idx * 64u,
                       cbuf_gpu_va & 0xFFFFFFFFu);
    ga10b_qmd_set_bits(qmd,
                       GA10B_QMD_CBUF_ADDR_HI_BASE + cbuf_idx * 64u + 16u,
                       GA10B_QMD_CBUF_ADDR_HI_BASE + cbuf_idx * 64u,
                       (cbuf_gpu_va >> 32) & 0x1FFFFu);
    ga10b_qmd_set_bits(qmd,
                       GA10B_QMD_CBUF_SIZE_SHIFTED4_BASE + cbuf_idx * 64u + 12u,
                       GA10B_QMD_CBUF_SIZE_SHIFTED4_BASE + cbuf_idx * 64u,
                       cbuf_size_B >> 4);
    ga10b_qmd_set_bits(qmd,
                       GA10B_QMD_CBUF_VALID_BASE + cbuf_idx,
                       GA10B_QMD_CBUF_VALID_BASE + cbuf_idx, 1u);
}

struct ga10b_qmd_pool_slot
ga10b_qmd_pool_prepare(uint8_t *pool_va,
                       uint64_t pool_gpu_va,
                       uint32_t pool_n_slots,
                       uint32_t *slot_inout,
                       const struct ga10b_pipeline_op_v7 *op)
{
    struct ga10b_qmd_pool_slot result = { 0, NULL, 0u };

    /* Defensive: caller is supposed to validate, but guarding the
     * modulo against pool_n_slots==0 here costs nothing and prevents
     * a divide-by-zero crash from an upstream bug. */
    if (pool_n_slots == 0u || pool_va == NULL ||
        slot_inout == NULL || op == NULL) {
        return result;
    }

    uint32_t slot = *slot_inout % pool_n_slots;
    uint8_t *slot_va = pool_va + (size_t)slot * GA10B_QMD_SIZE_BYTES;

    /* Cast slot to uint32_t* for the encoder. The 256-byte alignment
     * is the caller's responsibility (the helper allocates the pool
     * page-aligned and slots are 256-byte multiples, so this falls
     * out as long as pool_va is at least 4-byte aligned). */
    ga10b_qmd_populate((uint32_t *)slot_va,
                       op->shader_gpu_va,
                       op->cbuf_gpu_va,
                       op->register_count_v,
                       op->grid_x, op->grid_y, op->grid_z,
                       op->block_x, op->block_y, op->block_z);

    *slot_inout = (slot + 1u) % pool_n_slots;

    result.gpu_va = pool_gpu_va + (uint64_t)slot * GA10B_QMD_SIZE_BYTES;
    result.cpu_va = slot_va;
    result.index  = slot;
    return result;
}
