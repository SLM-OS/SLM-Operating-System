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

/* Discover the currently-bound channel's inst_block by reading
 * NV_PGRAPH_PRI_FECS_CURRENT_CTX (BAR0 + 0x409b00). FECS owns
 * the binding; the register's low 28 bits are `inst_block_phys >> 12`.
 *
 * Used as a fallback when the Linux-side gpu-channel-helper writes
 * `inst_block_phys = 0` into the handoff (the existing
 * `scripts/gpu-channel-helper.c` does this — it leaves the field
 * with a "filled in from FECS_CURRENT_CTX if needed" comment that
 * was never actioned). After `nvgpu inherit + channel`, the
 * channel is bound and FECS_CURRENT_CTX reads back the inst block.
 *
 * Returns 0 on read failure (target field invalid), nonzero phys
 * on success. Reads BAR0 directly; safe post-inherit.
 *
 * On Tegra Orin Nano (gk20a, no `iommus` DT property on the GPU
 * device) the value IS a CPU phys, not an SMMU IOVA. The
 * canonical chain that proves this:
 *
 *   nvgpu_inst_block_addr (common/mm/mm.c)
 *     → nvgpu_mem_get_addr (os/linux/nvgpu_mem.c)
 *       → branches on nvgpu_iommuable(g):
 *         → false on Tegra Orin (no iommu_domain)
 *         → returns gv11b_gpu_phys_addr(NULL, phys) = phys verbatim
 *
 * So the FECS register holds the inst block CPU phys directly.
 * See ~/slmos-ref/nvidia/nvgpu-l4t-r36.4.4-os-linux-nvgpu_mem.c
 * for the source.
 */
uint64_t ga10b_gmmu_discover_inst_block_phys(void);

/* Bounds-check a candidate physical address against the platform's
 * DRAM aperture (and OP-TEE carveout on Jetson). Wraps the
 * file-static `phys_in_dram` helper in ga10b_gmmu.c so other GA10B
 * code (e.g. the wedge-handler inst-block dumper) can validate a
 * phys before dereferencing it as a normal-memory load — a
 * dereference of an MMIO or unmapped region can fault or hang the
 * CPU. Returns true iff [phys, phys+bytes) is fully inside DRAM
 * and does not intersect the OP-TEE carveout. */
bool ga10b_phys_in_dram(uint64_t phys, size_t bytes);

/* Forward decl — full struct is in `ga10b_channel_handoff.h`,
 * pulled in by the rebuild path's translation unit. Declaring here
 * lets callers of `ga10b_gmmu_rebuild_for_handoff` work with an
 * opaque pointer if they only need the function signature. */
struct ga10b_channel_handoff;

/* #788 Stage 2 — rebuild a fresh GMMU page-table tree for a kexec'd
 * channel and write the inst block to point at it.
 *
 * Use case: after Linux kexecs, nvgpu's module .shutdown handler
 * frees the channel's PDB + page tables. The inst-block page
 * survives via the Stage 1 `pmm_user_reserve_add` reservation, but
 * its contents (PDB pointer, ramfc state) are gone. The helper's
 * USER-allocated buffers (pushbuffer, semaphore, SASS pool, etc.)
 * survive intact because the helper's open file descriptors keep
 * Linux from reclaiming them.
 *
 * This function rebuilds:
 *
 *   1. A fresh PDB page allocated from SLM-OS PMM, zeroed so every
 *      PDE3 entry reads invalid until mapped.
 *   2. The inst block at `inst_block_phys` — overwritten with a
 *      valid PDB pointer (sys_mem_ncoh aperture, volatile, ver2
 *      page-table format, 64 KB big-page size) and zeroed
 *      elsewhere.
 *   3. PTEs in the new tree mapping each preserved dmabuf from the
 *      handoff at its original GPU VA: pushbuf, semaphore, SASS
 *      pool (shader_*), cbuf, QMD pool, and the first 4 KB of the
 *      weights pool. USERD and GPFIFO are PBDMA-accessed via phys
 *      and don't need GMMU entries.
 *   4. TLB invalidate against the new PDB so the GPU loads fresh
 *      PTEs on the next walk.
 *
 * **Limitations:**
 *
 *   - Weights pool beyond its first page is NOT mapped. The 1.5 GB
 *     IOVMM allocation is SMMU-stitched from physically-scattered
 *     pages; the handoff publishes only the first page's phys.
 *     `slm xload qwen`'s W3 staging will succeed for the first 4
 *     KB chunk only; further chunks MMU-fault. A future helper
 *     enhancement could enumerate per-page phys via
 *     /proc/self/pagemap pre-kexec.
 *
 *   - GR engine ctxsw save buffer is NOT rebuilt. nvgpu allocates
 *     this lazily on first GR engagement; if FECS tries to save a
 *     context during the rebuilt channel's life, the save will
 *     fault. The MVP doesn't attempt to recreate it; if hardware
 *     testing shows it's needed, a future revision will allocate a
 *     save buffer and patch the inst block's engine_wfi fields.
 *
 *   - The runlist on the GPU still references the OLD inst-block
 *     physical address (because that's where nvgpu wrote it
 *     pre-kexec). The Stage 1 reservation keeps that page intact;
 *     this function fills it with valid contents so the runlist's
 *     reference becomes meaningful again. No runlist edit needed.
 *
 * Returns 0 on success, -1 on PMM exhaustion / invalid inst_phys /
 * mapping failure / TLB-invalidate timeout. All UART-logged so the
 * caller can correlate failures with the boot-time trace. */
int ga10b_gmmu_rebuild_for_handoff(uint64_t inst_block_phys,
                                   const struct ga10b_channel_handoff *h);

/* Discover the inherited channel's inst block by walking DRAM and
 * cross-checking each candidate against a known (gpu_va, leaf_phys)
 * pair from the channel handoff. Used as a fallback when
 * `ga10b_gmmu_discover_inst_block_phys` (FECS_CURRENT_CTX) returns a
 * stale pointer — observed on Jetson when Linux nvgpu replaced the
 * inst-block-pointing memory between the helper's last channel
 * activity and SLM-OS's first GMMU operation.
 *
 * Algorithm: for each 4 KB-aligned candidate in [scan_start..scan_end),
 *   1. Quick filter: read PDB target/ptr at +0x200/+0x204; bail unless
 *      PDB target is non-zero (bits 29:28 of word at +0x200) and the
 *      PDB pointer lands inside DRAM.
 *   2. Full check: walk the candidate for `known_gpu_va`. If the walk
 *      succeeds (status = OK) and the resulting leaf phys matches
 *      `expected_leaf_phys`, this is the channel's inst block.
 *
 * Caller supplies (known_gpu_va, expected_leaf_phys) from a handoff
 * field with a guaranteed mapping — `g_handoff.pushbuf_gpu_va` and
 * `g_handoff.pushbuf_phys` are the obvious pair (the helper's PB is
 * always GMMU-mapped pre-kexec).
 *
 * Returns the matching inst_block_phys, or 0 if no match in range.
 * Pure-logic — no MMIO, only DRAM reads. Bounded cost: scan range /
 * 4 KB candidates, each at most one walk (5 page-table reads).
 */
uint64_t ga10b_gmmu_discover_inst_block_via_walk(uint64_t known_gpu_va,
                                                  uint64_t expected_leaf_phys,
                                                  uint64_t scan_start,
                                                  uint64_t scan_end);

/* Fire a GA10B GMMU TLB invalidate for the given PDB. Mirrors
 * `gm20b_fb_tlb_invalidate` in nvgpu (l4t-r36.4.4 fb_gm20b_fusa.c
 * — see ~/slmos-ref/nvidia/nvgpu-l4t-r36.4.4-fb_gm20b_fusa.c).
 * GA10B inherits the gm20b implementation per `hal_ga10b.c`'s
 * `.tlb_invalidate = gm20b_fb_tlb_invalidate`.
 *
 * Sequence:
 *   1. Poll fb_mmu_ctrl[16:23] (pri_fifo_space) until non-zero —
 *      ensures the MMU PRIv FIFO has room for our invalidate cmd.
 *   2. Write fb_mmu_invalidate_pdb_r =
 *        ((pdb_phys >> 12) << 4) | aperture_sys_mem (=2).
 *   3. Write fb_mmu_invalidate_r =
 *        all_va_true (=1) | trigger (=0x80000000).
 *   4. Poll fb_mmu_ctrl[15] (pri_fifo_empty) until set — invalidate
 *      has completed.
 *
 * `pdb_phys` is the physical address of the page-directory base
 * (the PDB page, not the inst block). Caller obtains it by walking
 * the inst block via `ga10b_gmmu_walk` (the result includes
 * `pdb_phys`) or directly from a SLM-OS-allocated PDB.
 *
 * Returns 0 on success, -1 on FIFO timeout.
 *
 * Single-threaded today — the shell task is the only caller path.
 * Add a spinlock when the GMMU writer becomes accessible to
 * concurrent contexts (e.g. the runtime model loader if it lands a
 * worker thread).
 */
int ga10b_gmmu_tlb_invalidate(uint64_t pdb_phys);

#endif /* GPU_NVIDIA_GA10B_GMMU_H */
