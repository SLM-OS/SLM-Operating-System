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

/* ---------------------------------------------------------------------
 * Milestone B — single-page allocator + writer
 * --------------------------------------------------------------------- */

/* Bit flags for ga10b_gmmu_alloc_page. */
#define GA10B_GMMU_FLAG_RO          (1u << 0)  /* set PTE read_only bit */
#define GA10B_GMMU_FLAG_PRIV        (1u << 1)  /* set PTE privilege bit */

/* High reserved VA range for SLM-OS allocations. Linux's allocations
 * cluster below ~0x10_0000_0000; 0x40_0000_0000 (256 GB) sits well
 * above any current usage. The cursor never wraps in Milestone B —
 * we have ~256 GB of headroom for 4 KB pages, more than any workload
 * the runtime model loader (#657) will need before free/reuse
 * lands in Milestone C.
 *
 * 4 KB pages: 0x40_0000_0000..0x80_0000_0000 covers ~67M pages,
 * which is room for an entire Qwen2.5-1.5B's worth of weight
 * matrices many times over. */
#define GA10B_GMMU_VA_BASE          0x4000000000ull
#define GA10B_GMMU_VA_LIMIT         0x8000000000ull

/* Allocate one 4 KB page in the inherited channel's GMMU address
 * space.
 *
 *   1. Pick a fresh GPU VA from the bump-cursor in
 *      [GA10B_GMMU_VA_BASE..GA10B_GMMU_VA_LIMIT).
 *   2. Allocate a 4 KB PMM page for the data.
 *   3. Walk the inherited page tables down PDE3 → PDE2 → PDE1 →
 *      PDE0; allocate a fresh 4 KB PMM page for any intermediate
 *      table that doesn't exist (zeroed; entries are all-invalid
 *      until we write the one we care about).
 *   4. Write the leaf PTE at PDE0's small-half-pointed leaf table.
 *   5. cache_clean every page-table byte touched, plus a dsb sy,
 *      so the GPU's GMMU walker reads the new entries from PoC.
 *
 * No TLB invalidate is issued — the VA range is fresh and this
 * milestone never reuses VAs, so there's no stale TLB entry to
 * flush. Free + realloc-same-VA is Milestone C, where the TLB
 * invalidate sequence becomes load-bearing.
 *
 * Returns 0 on success. Out params populated only on success:
 *   *out_gpu_va — 4 KB-aligned GPU VA
 *   *out_cpu_va — kernel-side VA of the same page (identity-mapped
 *                 to *out_phys on Jetson; use for memcpy from CPU)
 *   *out_phys   — physical address (for diag/debug)
 *
 * Returns negative on failure: VA range exhausted, PMM out of
 * pages, or malformed inst-block / page-table chain.
 */
int ga10b_gmmu_alloc_page(uint64_t inst_block_phys,
                          uint32_t flags,
                          uint64_t *out_gpu_va,
                          void    **out_cpu_va,
                          uint64_t *out_phys);

/* Inspector for the allocator's high-water VA (for diag /
 * `nvgpu gmmu alloc-page` output). Returns the VA the next
 * allocation will hand out — i.e. one past the highest alloc'd
 * page. */
uint64_t ga10b_gmmu_va_cursor(void);

/* ---------------------------------------------------------------------
 * Milestone C — multi-page alloc + free
 * --------------------------------------------------------------------- */

/* Maximum number of contiguous-VA pages a single
 * `ga10b_gmmu_alloc` call can hand back. Cap exists to bound the
 * worst-case PMM allocation cost (one PMM page per leaf data page
 * plus on-demand intermediate PT pages). 2 MB at 4 KB granularity =
 * 512 pages — comfortably above the largest single SLM-OS scratch
 * buffer we anticipate (per-layer activation tensors run a few
 * hundred KB); raise if the model loader needs bigger contiguous
 * VA regions. */
#define GA10B_GMMU_MAX_ALLOC_PAGES   512u

/* Number of slots in the free-extent tracker. Each slot records
 * one freed (gpu_va, n_pages) extent so a future TLB-invalidate
 * landing can immediately enable VA reuse without API changes.
 * For Milestone C the table is write-only — the alloc path does
 * NOT reuse freed slots (TLB invalidate is unimplemented). 64
 * slots is room for ~64 outstanding free'd allocations, well past
 * what realistic SLM model load/unload churn would produce. */
#define GA10B_GMMU_FREE_TRACKER_SLOTS 64u

/* Allocate N contiguous 4 KB GPU virtual pages from the bump
 * cursor. Phys pages are allocated INDIVIDUALLY from PMM —
 * non-contiguous in physical memory but contiguous in GPU VA, the
 * shape every consumer (matmul, activation tensors, KV cache)
 * actually wants.
 *
 * Per-page work:
 *   - Allocate a 4 KB PMM page.
 *   - Walk inst_block_phys's GMMU tree for `va_base + i*4096`,
 *     allocating intermediate PDE tables as needed.
 *   - Write the leaf PTE.
 *
 * Crosses PT / PDE0 / PDE1 boundaries automatically — each per-page
 * walk descends from the PDB and reuses any already-populated
 * intermediate tables.
 *
 * Output:
 *   *out_gpu_va_base — VA of the first page (4 KB-aligned)
 *   *out_first_cpu_va — kernel VA of the first page (subsequent
 *                       pages' CPU VAs are NOT contiguous; the
 *                       caller must walk per-page if needed)
 *   *out_first_phys  — phys of the first page (same caveat)
 *
 * On failure: returns negative, partial allocations are NOT rolled
 * back (page-table tree pages stay allocated, leaf PTEs stay set
 * for whatever pages did succeed). Callers should treat alloc
 * failure as a fatal error path. Out params undefined on failure.
 *
 * Requires `n_pages` in [1..GA10B_GMMU_MAX_ALLOC_PAGES].
 *
 * Single-page convenience: `ga10b_gmmu_alloc_page` is equivalent
 * to `ga10b_gmmu_alloc(1, ...)` with the additional convenience of
 * returning the cpu_va as a `void*` instead of a u64.
 */
int ga10b_gmmu_alloc(uint64_t inst_block_phys,
                     uint32_t n_pages,
                     uint32_t flags,
                     uint64_t *out_gpu_va_base,
                     void    **out_first_cpu_va,
                     uint64_t *out_first_phys);

/* Free N contiguous pages previously returned by `ga10b_gmmu_alloc`
 * (or `ga10b_gmmu_alloc_page` with n_pages=1). Per page:
 *   - Walk the existing leaf PTE.
 *   - Clear the PTE (valid=0 + address bits zeroed) so a future
 *     GPU walk would see "unmapped".
 *   - cache_clean the PTE.
 *   - PMM page itself stays allocated to track-via-extent for
 *     future free; tracking removal is the caller's responsibility
 *     today (Milestone C constraint: alloc'd PMM pages leak on
 *     free until a phys-tracker lands in Milestone D).
 *
 * The freed extent is recorded in a small (GA10B_GMMU_FREE_TRACKER_SLOTS)
 * table so a future TLB-invalidate landing can enable VA reuse. The
 * Milestone C alloc path does NOT reuse freed VAs — without TLB
 * invalidate, the GPU's cached translation could still point at the
 * stale phys page after free, making reuse unsafe. The table is
 * write-only for now.
 *
 * Returns 0 on success. Negative on:
 *   - bad gpu_va (not 4 KB aligned, outside the SLM-OS reserved
 *     range, or not currently mapped),
 *   - free-tracker full (drop the limit; new alloc still succeeds
 *     because the bump cursor doesn't depend on the tracker).
 */
int ga10b_gmmu_free(uint64_t inst_block_phys,
                    uint64_t gpu_va,
                    uint32_t n_pages);

/* Inspector for the free-tracker slot count (for diag). Returns
 * the number of currently-recorded freed extents — saturates at
 * GA10B_GMMU_FREE_TRACKER_SLOTS. */
uint32_t ga10b_gmmu_free_tracker_count(void);

#endif /* GPU_NVIDIA_GA10B_GMMU_H */
