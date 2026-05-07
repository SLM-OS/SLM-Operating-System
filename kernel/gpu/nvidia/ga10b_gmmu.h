/*
 * ga10b_gmmu.h — GA10B (Ampere) GMMU page-table walker.
 *
 * Milestone A of #666 (GPU runtime loader prereq A): a READ-ONLY
 * walker that opens an inherited channel's instance block, reads the
 * page-directory-base (PDB), and walks the GMMU page tables for a
 * given GPU virtual address. Reports per-level entry values and the
 * final physical translation if the walk completes.
 *
 * No mutation. No allocation. No TLB touches. The intent is to
 * prove that SLM-OS can correctly read the inherited Linux page
 * tables before adding a writer in Milestone B.
 *
 * Validation: walk `g_handoff.pushbuf_gpu_va` and confirm the leaf
 * PTE physical address matches `g_handoff.pushbuf_phys`.
 */

#ifndef GPU_NVIDIA_GA10B_GMMU_H
#define GPU_NVIDIA_GA10B_GMMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* GA10B page-table levels. Default Ampere config is 5-level:
 *   PDE3 (top)  → PDE2 → PDE1 → PDE0 (dual PDE, leaf) → PTE
 *
 * GPU VA bit decomposition (best-guess for 49-bit VAs; may need
 * tuning on hardware — see ga10b_gmmu.c). The high two levels are
 * unequal in nvgpu's modern config; PDE3 covers the top 9 bits and
 * PDE2 the next 10. */
#define GA10B_PDE3_VA_HI    48
#define GA10B_PDE3_VA_LO    40   /* PDE3 index = bits [48:40], 9 bits */
#define GA10B_PDE2_VA_HI    39
#define GA10B_PDE2_VA_LO    30   /* PDE2 index = bits [39:30], 10 bits */
#define GA10B_PDE1_VA_HI    29
#define GA10B_PDE1_VA_LO    21   /* PDE1 index = bits [29:21], 9 bits */
#define GA10B_PDE0_VA_HI    20
#define GA10B_PDE0_VA_LO    16   /* PDE0 index = bits [20:16], 5 bits — covers 64 KB */
#define GA10B_PTE_VA_HI     15
#define GA10B_PTE_VA_LO     12   /* PTE index = bits [15:12], 4 bits — 16 entries × 4 KB = 64 KB region */
#define GA10B_PAGE_SHIFT    12

/* Each PDE/PTE entry is 8 bytes. */
#define GA10B_GMMU_ENTRY_SIZE   8

/* PDB target (aperture) — from nvgpu's
 * `ram_in_page_dir_base_target_*_f()` constants (instance-block layout). */
#define GA10B_PDB_TARGET_VID_MEM        0x0
#define GA10B_PDB_TARGET_SYS_MEM_COH    0x2
#define GA10B_PDB_TARGET_SYS_MEM_NCOH   0x3

/* PTE/PDE field decoders — derived from
 * `nvgpu-hw-ga10b-hw_gmmu_ga10b.h`'s gmmu_new_* encoders. The PTE
 * stores `phys_addr >> 12` in the lower 32 bits at bit positions
 * [31:8]; the upper 32 bits carry additional phys bits for systems
 * with > 36-bit physical addresses (Jetson Orin's 8 GB DRAM at
 * 0x80000000 needs only 33 phys bits, all of which fit in the low
 * word). */
#define GA10B_PTE_VALID_BIT             (1ull << 0)
#define GA10B_PTE_VOL_BIT               (1ull << 3)
#define GA10B_PTE_APERTURE_MASK         (0x7ull << 1)
#define GA10B_PTE_APERTURE_SHIFT        1
#define GA10B_PTE_PRIVILEGE_BIT         (1ull << 5)
#define GA10B_PTE_READ_ONLY_BIT         (1ull << 6)
#define GA10B_PTE_KIND_MASK             (0xffull << 24)
#define GA10B_PTE_KIND_SHIFT            24

/* The encoded address occupies bits [31:8] of the entry's low word
 * and bits [23:0] of the high word — together 56 bits at shift 12,
 * giving 68-bit physical address support (way more than Jetson
 * needs). Decode by masking and combining. */
#define GA10B_PTE_ADDR_LO_MASK          (0xffffff00ull)         /* bits [31:8] */
#define GA10B_PTE_ADDR_HI_MASK          (0x00ffffffull << 32)   /* bits [55:32] */

/* PDE entry layout — same shape as PTE for the regular (non-dual)
 * variants. Used by PDE3/PDE2/PDE1. */
#define GA10B_PDE_APERTURE_MASK         (0x7ull << 1)
#define GA10B_PDE_APERTURE_SHIFT        1
#define GA10B_PDE_VOL_BIT               (1ull << 3)
#define GA10B_PDE_ADDR_LO_MASK          (0xffffff00ull)
#define GA10B_PDE_ADDR_HI_MASK          (0x00ffffffull << 32)

/* Dual PDE (PDE0) — encodes pointers to BOTH a small-page leaf table
 * (covering 4 KB pages within a 2 MB VA region — but on Ampere with
 * the modern bit decomposition, covers 64 KB of VA with 16 × 4 KB
 * pages) AND a big-page leaf table. Entry is 8 bytes split into two
 * 4-byte halves: small (low) + big (high).
 *
 * For Milestone A's read-only walker, we only follow the SMALL half
 * (4 KB pages). Big-page mappings are ignored — they won't show up
 * in handoff buffers like pushbuf which Linux maps with 4 KB
 * granularity by default for small DMA buffers. */
#define GA10B_DUAL_PDE_SMALL_APERTURE_MASK   (0x7ull << 1)
#define GA10B_DUAL_PDE_SMALL_APERTURE_SHIFT  1
#define GA10B_DUAL_PDE_SMALL_VOL_BIT         (1ull << 3)
#define GA10B_DUAL_PDE_SMALL_ADDR_MASK       (0xffffffull << 8)  /* low word [31:8] = small PT phys >> 12 */
#define GA10B_DUAL_PDE_BIG_APERTURE_MASK     (0x7ull << (32 + 1))
#define GA10B_DUAL_PDE_BIG_APERTURE_SHIFT    33
#define GA10B_DUAL_PDE_BIG_VOL_BIT           (1ull << (32 + 3))
#define GA10B_DUAL_PDE_BIG_ADDR_MASK         (0xfffffffull << (32 + 4)) /* high word [31:4] = big PT phys >> 8 */

/* Per-level walk record — populated by `ga10b_gmmu_walk` for every
 * level it visits (including the level that aborts the walk). */
struct ga10b_gmmu_level_record {
    uint16_t index;             /* index used at this level */
    uint64_t table_phys;        /* physical address of the table walked into */
    uint64_t entry;             /* raw 8-byte entry value */
    uint64_t next_phys;         /* next-level table physical (or leaf PTE phys at PTE level) */
    uint8_t aperture;           /* decoded aperture field */
    bool valid;                 /* entry valid bit set */
    bool dual_pde_followed_small; /* PDE0 only: true if we descended via the small-page half */
};

enum ga10b_gmmu_walk_status {
    GA10B_GMMU_WALK_OK,         /* reached PTE, leaf valid */
    GA10B_GMMU_WALK_PDE_INVALID,/* hit an invalid PDE — VA is unmapped */
    GA10B_GMMU_WALK_PTE_INVALID,/* reached PTE level but PTE invalid */
    GA10B_GMMU_WALK_BAD_INST,   /* inst block PDB invalid */
};

struct ga10b_gmmu_walk_result {
    uint64_t inst_block_phys;
    uint64_t gpu_va;
    uint64_t pdb_phys;          /* page-directory base from inst block */
    uint8_t pdb_aperture;
    bool pdb_volatile;

    /* Levels: [0]=PDE3, [1]=PDE2, [2]=PDE1, [3]=PDE0, [4]=PTE.
     * Only entries [0..levels_walked-1] are valid. */
    struct ga10b_gmmu_level_record levels[5];
    int levels_walked;

    enum ga10b_gmmu_walk_status status;
    /* When status == OK: leaf physical address the GPU VA maps to,
     * and the PTE attribute bits. */
    uint64_t leaf_phys;
    bool leaf_read_only;
    bool leaf_privileged;
};

/* Walk `inst_block_phys`'s GMMU page tables for `gpu_va`. Always
 * returns 0 — the result struct's `status` field carries the walk
 * outcome. Reads only; never modifies any DRAM byte.
 *
 * `inst_block_phys` is the physical address of a 4 KB instance
 * block — typically `g_handoff.inst_block_phys` from the inherited
 * channel handoff. Caller is responsible for ensuring the address
 * is identity-mapped (true on Jetson, where vmm_init covers all
 * physical RAM in TTBR0/TTBR1).
 */
int ga10b_gmmu_walk(uint64_t inst_block_phys, uint64_t gpu_va,
                    struct ga10b_gmmu_walk_result *result);

/* Pretty-print a walk result via the kernel's shell_printf. Includes
 * raw 8-byte entry hex at every level so a wrong bit decomposition
 * (the high-risk part of Milestone A) is reverse-engineerable from
 * the output. */
void ga10b_gmmu_walk_print(const struct ga10b_gmmu_walk_result *r);

#endif /* GPU_NVIDIA_GA10B_GMMU_H */
