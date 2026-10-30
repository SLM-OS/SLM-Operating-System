/*
 * ga10b_gmmu_decode.h — pure GA10B GMMU entry decoders.
 *
 * Shared by the read-only walker in ga10b_gmmu.c and the whole-tree
 * enumerator in ga10b_gmmu_enum.c. No DRAM access here: every helper
 * takes an already-read 64-bit entry, so the enumerator can be driven
 * against in-memory tables by the host test harness.
 */
#ifndef GA10B_GMMU_DECODE_H
#define GA10B_GMMU_DECODE_H

#include "ga10b_gmmu.h"

#include <stdbool.h>
#include <stdint.h>

/* Inst-block PDB words (ram_in_page_dir_base_{lo,hi}_w() = 128/129).
 * Decoding is documented above the walker in ga10b_gmmu.c. */
#define INST_BLOCK_PDB_LO_BYTE_OFFSET   (128 * 4)
#define INST_BLOCK_PDB_HI_BYTE_OFFSET   (129 * 4)

/* Decode the inst block's PDB from its two adjacent 32-bit words, read
 * as one 8-byte value at INST_BLOCK_PDB_LO_BYTE_OFFSET. `out_target`
 * is the aperture (0 = invalid). */
static inline void decode_inst_block_pdb(uint64_t pdb_words,
                                         uint64_t *out_pdb_phys,
                                         uint8_t *out_target)
{
    uint32_t pdb_lo = (uint32_t)pdb_words;
    uint32_t pdb_hi = (uint32_t)(pdb_words >> 32);
    *out_target   = (uint8_t)((pdb_lo >> 1) & 0x3);
    *out_pdb_phys = ((uint64_t)pdb_hi << 32) | (pdb_lo & 0xfffff000u);
}

/* Decode a regular PDE entry (used for PDE3, PDE2, PDE1).
 *   - lower 32 bits: aperture(2:1) | volatile(3) | next_phys[31:12] (24 bits at [31:8] encoded as phys >> 12)
 *   - upper 32 bits: next_phys[55:32] (24 bits at [23:0])
 *
 * Per nvgpu's `gmmu_new_pde_address_sys_f(v) = ((v & 0xffffff) << 8U)`,
 * the 24-bit lo half packs `phys[35:12]` shifted to bit position 8,
 * giving `(entry_lo & 0xffffff00) >> 8 << 12 = entry_lo & 0xfffff000`
 * — i.e. `phys[35:12]` lives at the same bit position in the entry
 * word as it does in the physical address (high 24 bits of low 32).
 */
static inline void decode_regular_pde(uint64_t entry,
                                      uint64_t *out_next_phys,
                                      uint8_t *out_aperture,
                                      bool *out_valid)
{
    uint8_t aperture = (uint8_t)((entry >> GA10B_PDE_APERTURE_SHIFT) & 0x7);
    *out_aperture = aperture;
    /* Aperture 0 = invalid (matches gmmu_new_pde_aperture_invalid_f
     * convention even though the explicit "_invalid_" macro isn't in
     * the cached header). */
    *out_valid = (aperture != 0);

    uint64_t addr_lo = (entry & GA10B_PDE_ADDR_LO_MASK) >> 8;   /* phys[35:12] */
    uint64_t addr_hi = (entry & GA10B_PDE_ADDR_HI_MASK) >> 32;  /* phys[59:36] */
    *out_next_phys = ((addr_hi << 24) | addr_lo) << 12;
}

/* Decode the small-page half of a dual PDE (PDE0) — bytes 8..15 of
 * the 16-byte entry. */
static inline void decode_dual_pde_small(uint64_t entry,
                                         uint64_t *out_next_phys,
                                         uint8_t *out_aperture,
                                         bool *out_valid)
{
    uint8_t aperture = (uint8_t)((entry >> GA10B_DUAL_PDE_SMALL_APERTURE_SHIFT) & 0x7);
    *out_aperture = aperture;
    *out_valid = (aperture != 0);
    /* Small PT phys >> 12 in low word [31:8]. */
    uint64_t lo = (entry >> 8) & 0xffffff;
    *out_next_phys = lo << 12;
}

/* Decode the big-page half of a dual PDE (PDE0). Layout per nvgpu's
 * `gmmu_new_dual_pde_*_big_*_f()` encoders:
 *   bits[2:0]   = aperture (0=invalid, 2=vid, 4=sys_coh, 6=sys_ncoh)
 *   bit 3       = volatile
 *   bits[31:4]  = big_addr = (phys_of_big_PT >> 8) — 28 bits
 *   pde_v[1][7:0] = high byte of big_addr (extends to ~36 bits of phys)
 *
 * Phys reconstruction: phys = ((hi_byte << 28) | low_28_bits) << 8.
 *
 * Takes the FIRST 8 bytes of the 16-byte dual PDE entry (offset 0..7),
 * which point at the big-page (64 KB) PTE table. */
static inline void decode_dual_pde_big(uint64_t entry,
                                       uint64_t *out_next_phys,
                                       uint8_t *out_aperture,
                                       bool *out_valid)
{
    uint8_t aperture = (uint8_t)(entry & 0x7u);
    *out_aperture = aperture;
    *out_valid = (aperture != 0);
    uint64_t lo28 = (entry >> 4) & 0x0fffffffULL;   /* bits [31:4]   */
    uint64_t hi8  = (entry >> 32) & 0xffULL;        /* pde_v[1][7:0] */
    *out_next_phys = ((hi8 << 28) | lo28) << 8;
}

/* Decode a leaf PTE. */
static inline void decode_pte(uint64_t entry,
                              uint64_t *out_phys,
                              uint8_t *out_aperture,
                              bool *out_valid,
                              bool *out_read_only,
                              bool *out_privileged)
{
    *out_valid = (entry & GA10B_PTE_VALID_BIT) != 0;
    *out_aperture = (uint8_t)((entry >> GA10B_PTE_APERTURE_SHIFT) & 0x7);
    *out_read_only = (entry & GA10B_PTE_READ_ONLY_BIT) != 0;
    *out_privileged = (entry & GA10B_PTE_PRIVILEGE_BIT) != 0;
    uint64_t addr_lo = (entry & GA10B_PTE_ADDR_LO_MASK) >> 8;   /* phys[35:12] */
    uint64_t addr_hi = (entry & GA10B_PTE_ADDR_HI_MASK) >> 32;  /* phys[59:36] */
    *out_phys = ((addr_hi << 24) | addr_lo) << 12;
}

#endif /* GA10B_GMMU_DECODE_H */
