/*
 * ga10b_gmmu.c — GA10B (Ampere) GMMU page-table walker (read-only).
 *
 * See ga10b_gmmu.h for the full design rationale. Milestone A scope:
 *
 *   1. Read inst_block_phys's PDB at byte offset 512 (= word 128 ×
 *      4 = ram_in_page_dir_base_lo_w() × 4).
 *   2. For each level of the GA10B page table (PDE3 → PDE2 → PDE1 →
 *      PDE0 → PTE), index into the table by the appropriate bits
 *      of `gpu_va`, read the 8-byte entry, decode the next-level
 *      table physical address (or final PTE phys), and descend.
 *   3. Stop on invalid PDE/PTE; populate the per-level record with
 *      raw bytes so a wrong bit decomposition is reverse-
 *      engineerable from the printed output.
 *
 * Identity mapping assumption: Jetson SLM-OS maps all physical RAM
 * (0x80000000 + 8 GB) into the kernel address space at boot
 * (vmm_init produces the "Bytes mapped: 2044 MB" log line). Any
 * physical address can be deref'd directly via
 * `*(volatile uint64_t *)(uintptr_t)phys`.
 */

#include "ga10b_gmmu.h"

#include "../../include/cache.h"
#include "../../include/pmm.h"
#include "../../include/string.h"
#include "../../include/timer.h"
#include "../../include/vmm.h"      /* vmm_map_region, VMM_FLAG_* */

#include <stdint.h>
#include <stddef.h>

/* The inst_block PDB lives at word offset 128 (= byte offset 512)
 * per nvgpu's `ram_in_page_dir_base_lo_w()`. The PDB is encoded as
 * two 32-bit words: lo (bits [31:0] of phys >> 12) + hi (bits
 * [63:32]). The lo word also carries target/vol attribute bits in
 * the bottom 8.
 *
 * Layout (verified against
 * slmos-reference-cache/nvidia/nvgpu-hal-fifo-ramin_ga10b_fusa.c):
 *
 *   word 128 (lo):
 *     bits [0]   : 0 (reserved)
 *     bits [2:1] : target (vid_mem=0, sys_mem_coh=2, sys_mem_ncoh=3)
 *     bits [3]   : volatile flag
 *     bits [11:4]: 0 (reserved — addr starts at bit 12 in the encoded form)
 *     bits [31:12]: phys[31:12]
 *
 * The encoder in nvgpu uses the "_address_lo_f(v) = (v & 0xfffff) << 12"
 * pattern, where v is `phys >> 12`. So:
 *   target/vol = (lo_word >> 0) & 0xf      (low nibble)
 *   phys_lo    = (lo_word & 0xfffff000)    (bits [31:12])
 *   phys_hi    = hi_word                   (bits [63:32] of phys)
 *   pdb_phys   = (uint64_t)phys_hi << 32 | phys_lo
 */
#define INST_BLOCK_PDB_LO_BYTE_OFFSET   (128 * 4)
#define INST_BLOCK_PDB_HI_BYTE_OFFSET   (129 * 4)

/* Jetson Orin Nano DRAM range, per platform.h / vmm_init dump:
 *   0x80000000 .. 0x280000000  (8 GB at 2 GB base)
 *
 * Page-table walk reads must stay inside this — feeding the walker
 * a garbage inst_block (or a corrupt page table) will produce
 * wild "next_phys" values; without a bounds check the descent can
 * page-fault on the next read. Treating out-of-range reads as
 * "invalid PDE" lets the walker abort cleanly with a meaningful
 * status rather than crashing the kernel. */
#define GA10B_GMMU_DRAM_BASE   0x80000000ull
/* Upper bound MUST match the platform's actual VMM map. Jetson's
 * pmm_init declares regions up to 0x240000000 (#580 has the
 * derivation from /proc/iomem); above that the VMM doesn't map
 * anything, so a phys_read here would synchronous-abort on the
 * dereference. The original 0x280000000 (RAM_SIZE) was wrong for
 * exactly this reason — observed on the inst-block walk-discovery
 * path when a candidate PDE3 entry decoded to a 0x27e440000-class
 * address. */
#define GA10B_GMMU_DRAM_TOP    0x240000000ull

/* Jetson VMM has three known unmapped holes in the GA10B GMMU
 * scan range. Only the largest (OP-TEE) is excluded here today;
 * the others are listed for the next person who hits a fault.
 *
 *   1. OP-TEE carveout       0xBE000000 .. 0xC2000000   (64 MB)
 *   2. IMX219 frame buffer   0xA1000000 .. 0xA1400000   ( 4 MB)
 *      camrtc.c:CAMRTC_FRAME_BUFFER_PHYS / _END
 *   3. NC-memory page        0xBDE00000 .. 0xBE000000   ( 2 MB)
 *      the kernel reserves this 2 MB block immediately above the
 *      cacheable region for cross-CPU coherent state — see
 *      ncmem.h. Nominally inside region 1 but the cacheable
 *      heap stops at 0xBDE00000.
 *
 * The walker reaches these regions only when a wild PDB pointer
 * (read from a candidate inst block) decodes to a page-aligned
 * address inside the hole. Empirically wild PDBs cluster in the
 * largest unmapped region — OP-TEE has covered every observed
 * fault to date. If a future scan faults at an address inside
 * (2) or (3), add a matching exclusion below.
 *
 * TODO: fold (2) and (3) here once a real fault from one of
 *       them is observed in the wild. Until then, leaving them
 *       documented-but-unenforced keeps the path narrow. */
#define GA10B_GMMU_OPTEE_BASE  0xBE000000ull
#define GA10B_GMMU_OPTEE_TOP   0xC2000000ull
static inline bool phys_in_dram(uint64_t phys, size_t bytes)
{
    if (phys < GA10B_GMMU_DRAM_BASE) return false;
    uint64_t end = phys + bytes;
    if (end < phys) return false;        /* wrap */
    if (end > GA10B_GMMU_DRAM_TOP) return false;
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Reject any read that intersects the OP-TEE carveout. */
    if (phys < GA10B_GMMU_OPTEE_TOP && end > GA10B_GMMU_OPTEE_BASE) {
        return false;
    }
#endif
    return true;
}

/* Non-inline public wrapper so other GA10B TUs can bounds-check a
 * phys before dereferencing it. Declared in ga10b_gmmu.h. */
bool ga10b_phys_in_dram(uint64_t phys, size_t bytes)
{
    return phys_in_dram(phys, bytes);
}

static inline uint32_t phys_read32(uint64_t phys)
{
    if (!phys_in_dram(phys, 4)) return 0;
    return *(volatile uint32_t *)(uintptr_t)phys;
}

static inline uint64_t phys_read64(uint64_t phys)
{
    /* GA10B PDE/PTE entries are naturally 8-byte-aligned within their
     * 4 KB tables, so a single 64-bit read is fine. */
    if (!phys_in_dram(phys, 8)) return 0;
    return *(volatile uint64_t *)(uintptr_t)phys;
}

/* Bit-extract `gpu_va`'s index for a given page-table level. */
static inline uint16_t va_index(uint64_t gpu_va, int hi, int lo)
{
    uint64_t mask = (1ull << (hi - lo + 1)) - 1;
    return (uint16_t)((gpu_va >> lo) & mask);
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
static void decode_regular_pde(uint64_t entry,
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

/* Decode the small-page half of a dual PDE (PDE0). Big half is
 * ignored for Milestone A — see ga10b_gmmu.h. */
static void decode_dual_pde_small(uint64_t entry,
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
 * This decoder is used when our walker reads the FIRST 8 bytes of a
 * 16-byte dual PDE entry (offset 0..7 in the entry) — that's the
 * big-page child. For 64-KB big-page mappings (the helper's default
 * for pushbuf/sem/qmd_pool/shaders), this is the correct half. */
static void decode_dual_pde_big(uint64_t entry,
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
static void decode_pte(uint64_t entry,
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

int ga10b_gmmu_walk(uint64_t inst_block_phys, uint64_t gpu_va,
                    struct ga10b_gmmu_walk_result *result)
{
    if (result == NULL) {
        return -1;
    }

    /* Zero the result struct so partial walks have well-defined fields. */
    memset(result, 0, sizeof(*result));
    result->inst_block_phys = inst_block_phys;
    result->gpu_va = gpu_va;
    result->status = GA10B_GMMU_WALK_BAD_INST;

    if (inst_block_phys == 0) {
        return 0;
    }

    /* Read PDB from inst block. */
    uint32_t pdb_lo = phys_read32(inst_block_phys + INST_BLOCK_PDB_LO_BYTE_OFFSET);
    uint32_t pdb_hi = phys_read32(inst_block_phys + INST_BLOCK_PDB_HI_BYTE_OFFSET);

    uint8_t target = (uint8_t)((pdb_lo >> 1) & 0x3);
    bool vol = (pdb_lo & (1u << 3)) != 0;
    /* phys_lo bits [31:12] of phys come from lo word at the same bit position */
    uint32_t phys_lo = pdb_lo & 0xfffff000;
    uint64_t pdb_phys = ((uint64_t)pdb_hi << 32) | phys_lo;

    result->pdb_phys = pdb_phys;
    result->pdb_aperture = target;
    result->pdb_volatile = vol;

    if (pdb_phys == 0 || target == 0) {
        return 0;  /* status already BAD_INST */
    }

    /* Walk PDE3 → PDE2 → PDE1 → PDE0 → PTE. */
    struct {
        int hi, lo;
        uint8_t entry_size;
    } level_bits[5] = {
        {GA10B_PDE3_VA_HI, GA10B_PDE3_VA_LO, GA10B_GMMU_ENTRY_SIZE},
        {GA10B_PDE2_VA_HI, GA10B_PDE2_VA_LO, GA10B_GMMU_ENTRY_SIZE},
        {GA10B_PDE1_VA_HI, GA10B_PDE1_VA_LO, GA10B_GMMU_ENTRY_SIZE},
        /* Level 3 entries are dual-PDE0 (16 bytes each — big-page
         * child at offset 0..7, small-page child at 8..15). The
         * walker tries the big-page child first; if its aperture
         * is invalid, falls through to the small-page child at
         * offset +8 (see the lvl==3 block below). The leaf-level
         * index width then follows whichever half was used —
         * tracked via `followed_small_pde0`. */
        {GA10B_PDE0_VA_HI, GA10B_PDE0_VA_LO, GA10B_GMMU_PDE0_SIZE},
        {GA10B_PTE_VA_HI,  GA10B_PTE_VA_LO,  GA10B_GMMU_ENTRY_SIZE},
    };

    uint64_t cur_table_phys = pdb_phys;
    /* Tracks whether the dual-PDE0 fell through to its small-page
     * child. Affects how level 4 indexes the PTE table: big-page
     * mappings use bits [20:16] (5 bits, 32 entries × 64 KB) while
     * small-page mappings use bits [20:12] (9 bits, 512 entries ×
     * 4 KB). Hardware-observed on jetson-nano-2 2026-05-17: the
     * helper's pushbuf is small-page-mapped, so without this
     * fallback the walker stalled at PDE0 with "big-side invalid"
     * even though the small-side child held the right pointer. */
    bool followed_small_pde0 = false;

    for (int lvl = 0; lvl < 5; lvl++) {
        /* L4 index width depends on which PDE0 child we followed.
         * The level_bits[] table holds the big-page width by
         * default; override for the small-side case here. */
        int idx_hi = level_bits[lvl].hi;
        int idx_lo = level_bits[lvl].lo;
        if (lvl == 4 && followed_small_pde0) {
            idx_hi = GA10B_PTE_SMALL_VA_HI;
            idx_lo = GA10B_PTE_SMALL_VA_LO;
        }
        uint16_t idx = va_index(gpu_va, idx_hi, idx_lo);
        uint64_t entry_phys = cur_table_phys +
                              (uint64_t)idx * level_bits[lvl].entry_size;
        uint64_t entry = phys_read64(entry_phys);

        struct ga10b_gmmu_level_record *rec = &result->levels[lvl];
        rec->index = idx;
        rec->table_phys = cur_table_phys;
        rec->entry = entry;
        result->levels_walked = lvl + 1;

        if (lvl == 4) {
            /* PTE level. */
            uint64_t leaf_phys;
            uint8_t aperture;
            bool valid, read_only, privileged;
            decode_pte(entry, &leaf_phys, &aperture, &valid, &read_only, &privileged);
            rec->aperture = aperture;
            rec->valid = valid;
            rec->next_phys = leaf_phys;
            if (!valid) {
                result->status = GA10B_GMMU_WALK_PTE_INVALID;
                return 0;
            }
            result->status = GA10B_GMMU_WALK_OK;
            result->leaf_phys = leaf_phys;
            result->leaf_read_only = read_only;
            result->leaf_privileged = privileged;
            return 0;
        }

        /* PDE level. PDE0 is dual; PDE3/PDE2/PDE1 are regular. */
        uint64_t next_phys;
        uint8_t aperture;
        bool valid;
        if (lvl == 3) {
            /* PDE0 is a dual entry (16 bytes). First 8 bytes are
             * the big-page child; bytes 8..15 are the small-page
             * child. Try big first (cheaper for typical
             * 64-KB-page workloads); if invalid, read the small-
             * page child from offset +8 and try again. The
             * walker's leaf-level indexing follows whichever
             * child was used. */
            decode_dual_pde_big(entry, &next_phys, &aperture, &valid);
            if (!valid) {
                uint64_t small_entry = phys_read64(entry_phys + 8u);
                decode_dual_pde_small(small_entry, &next_phys,
                                       &aperture, &valid);
                if (valid) {
                    /* Update record so post-mortem dumps reflect
                     * which half held the valid pointer. */
                    rec->entry = small_entry;
                    followed_small_pde0 = true;
                    rec->dual_pde_followed_small = true;
                } else {
                    rec->dual_pde_followed_small = false;
                }
            } else {
                rec->dual_pde_followed_small = false;
            }
        } else {
            decode_regular_pde(entry, &next_phys, &aperture, &valid);
        }
        rec->aperture = aperture;
        rec->valid = valid;
        rec->next_phys = next_phys;
        if (!valid) {
            result->status = GA10B_GMMU_WALK_PDE_INVALID;
            return 0;
        }
        cur_table_phys = next_phys;
    }

    /* Unreachable — loop returns from the lvl==4 branch. */
    return 0;
}

/* shell_printf is declared in shell_internal.h but pulling that into
 * a GPU-driver TU adds an awkward dep. Declare locally; resolved at
 * link time. */
extern void shell_printf(const char *fmt, ...);
extern void shell_puts(const char *s);

/* PDB target: 0=vid_mem, 2=sys_mem_coh, 3=sys_mem_ncoh
 * (per ram_in_page_dir_base_target_*_f() in nvgpu's instance-block
 * encoder). Value 0 is meaningful here; on Jetson it never appears
 * because the SoC has no discrete vidmem. */
static const char *pdb_target_str(uint8_t t)
{
    switch (t) {
    case 0: return "vid_mem";
    case 2: return "sys_mem_coh";
    case 3: return "sys_mem_ncoh";
    default: return "unknown";
    }
}

/* PDE aperture: 0=invalid, 2=vid_mem, 4=sys_mem_coh, 6=sys_mem_ncoh
 * (per gmmu_new_pde_aperture_*_f()). The 0=invalid convention is
 * what nvgpu uses to mark unmapped PDE entries. */
static const char *pde_aperture_str(uint8_t a)
{
    switch (a) {
    case 0: return "invalid";
    case 2: return "vid_mem";
    case 4: return "sys_mem_coh";
    case 6: return "sys_mem_ncoh";
    default: return "unknown";
    }
}

/* PTE aperture: 0=vid_mem, 4=sys_mem_coh, 6=sys_mem_ncoh
 * (per gmmu_new_pte_aperture_*_f()). The PTE has a separate "valid"
 * bit (bit 0); aperture 0 here means video memory, NOT unmapped. */
static const char *pte_aperture_str(uint8_t a)
{
    switch (a) {
    case 0: return "vid_mem";
    case 4: return "sys_mem_coh";
    case 6: return "sys_mem_ncoh";
    default: return "unknown";
    }
}

static const char *level_name(int lvl)
{
    static const char *NAMES[5] = {"PDE3", "PDE2", "PDE1", "PDE0", " PTE"};
    if (lvl < 0 || lvl > 4) return "????";
    return NAMES[lvl];
}

void ga10b_gmmu_walk_print(const struct ga10b_gmmu_walk_result *r)
{
    if (r == NULL) {
        shell_puts("ga10b_gmmu_walk_print: NULL result\r\n");
        return;
    }
    shell_printf("GMMU walk for GPU VA 0x%lx (inst block 0x%lx):\r\n",
                 (unsigned long)r->gpu_va, (unsigned long)r->inst_block_phys);
    shell_printf("  PDB: phys=0x%lx target=%s%s\r\n",
                 (unsigned long)r->pdb_phys, pdb_target_str(r->pdb_aperture),
                 r->pdb_volatile ? " volatile" : "");

    for (int lvl = 0; lvl < r->levels_walked; lvl++) {
        const struct ga10b_gmmu_level_record *rec = &r->levels[lvl];
        const char *ap = (lvl == 4) ? pte_aperture_str(rec->aperture)
                                    : pde_aperture_str(rec->aperture);
        shell_printf("  %s[%4u] @ table 0x%lx: entry=0x%016lx valid=%d aperture=%s%s\r\n",
                     level_name(lvl),
                     (unsigned)rec->index,
                     (unsigned long)rec->table_phys,
                     (unsigned long)rec->entry,
                     rec->valid,
                     ap,
                     rec->dual_pde_followed_small ? " (dual: small)" : "");
        shell_printf("           next_phys=0x%lx\r\n",
                     (unsigned long)rec->next_phys);
    }

    switch (r->status) {
    case GA10B_GMMU_WALK_OK:
        shell_printf("  STATUS: OK — leaf phys=0x%lx%s%s\r\n",
                     (unsigned long)r->leaf_phys,
                     r->leaf_read_only ? " RO" : " RW",
                     r->leaf_privileged ? " priv" : "");
        break;
    case GA10B_GMMU_WALK_PDE_INVALID:
        shell_printf("  STATUS: PDE_INVALID at level %d — VA unmapped\r\n",
                     r->levels_walked - 1);
        break;
    case GA10B_GMMU_WALK_PTE_INVALID:
        shell_puts("  STATUS: PTE_INVALID — leaf entry valid bit clear\r\n");
        break;
    case GA10B_GMMU_WALK_BAD_INST:
        shell_puts("  STATUS: BAD_INST — PDB read failed or aperture invalid\r\n");
        break;
    }
}

/* ---------------------------------------------------------------------
 * Milestone B — single-page allocator + writer
 * --------------------------------------------------------------------- */

/* Bump cursor for SLM-OS GMMU allocations. Single-threaded shell
 * task is the only caller path today (matches `g_handoff` access
 * pattern); no spinlock yet. Add one when a second writer (e.g.
 * the runtime model loader's worker task) lands. */
static uint64_t g_va_cursor = GA10B_GMMU_VA_BASE;

uint64_t ga10b_gmmu_va_cursor(void)
{
    return g_va_cursor;
}

/* Encode a regular PDE entry pointing at `next_phys` with sys_mem_coh
 * aperture. Mirrors `gmmu_new_pde_address_sys_f(v) = ((v & 0xffffff) << 8U)`
 * + `gmmu_new_pde_aperture_sys_mem_coh_f() = 0x4`.
 *
 * Layout:
 *   bit  0  : 0 (was: invalid bit; gmmu_new uses aperture==0 instead)
 *   bits 3:1: aperture (0=invalid, 4=sys_mem_coh, 6=sys_mem_ncoh)
 *   bit  3  : volatile
 *   bits 31:8 : phys[35:12]   (24 bits at bit 8 of low word)
 *   bits 55:32: phys[59:36]   (24 bits at bit 0 of high word)
 *
 * For Jetson, all phys < 2 TB so phys[59:36] fits in 5 bits.
 */
static inline uint64_t encode_regular_pde(uint64_t next_phys)
{
    /* sys_mem_coh aperture (4) at bit 1. */
    uint64_t entry = (uint64_t)4 << GA10B_PDE_APERTURE_SHIFT;
    /* phys[35:12] -> low word bits [31:8]. */
    uint64_t addr_lo = (next_phys >> 12) & 0xffffff;
    entry |= addr_lo << 8;
    /* phys[59:36] -> high word bits [23:0]. */
    uint64_t addr_hi = (next_phys >> 36) & 0xffffff;
    entry |= addr_hi << 32;
    return entry;
}

/* Encode the SMALL HALF of a dual PDE (PDE0). Big half left as-is
 * by the caller via read-modify-write — never touch it. */
static inline uint64_t encode_dual_pde_small(uint64_t next_phys)
{
    /* sys_mem_coh aperture for the small half: bits [3:1] of low word. */
    uint64_t entry = (uint64_t)4 << GA10B_DUAL_PDE_SMALL_APERTURE_SHIFT;
    /* phys[35:12] -> low word bits [31:8] (small PT phys >> 12). */
    uint64_t addr_lo = (next_phys >> 12) & 0xffffff;
    entry |= addr_lo << 8;
    /* Small half doesn't carry phys_hi — small PT must fit in
     * phys[35:0] = 64 GB. Jetson DRAM tops at 0x280000000 (10 GB),
     * so any PMM-alloc'd page satisfies this. */
    return entry;
}

/* Encode a leaf PTE pointing at `phys` with sys_mem_coh aperture. */
static inline uint64_t encode_pte(uint64_t phys, uint32_t flags)
{
    uint64_t entry = GA10B_PTE_VALID_BIT;
    /* sys_mem_coh aperture (4). */
    entry |= (uint64_t)4 << GA10B_PTE_APERTURE_SHIFT;
    if (flags & GA10B_GMMU_FLAG_RO)   entry |= GA10B_PTE_READ_ONLY_BIT;
    if (flags & GA10B_GMMU_FLAG_PRIV) entry |= GA10B_PTE_PRIVILEGE_BIT;
    /* phys[35:12] -> low word bits [31:8]. */
    uint64_t addr_lo = (phys >> 12) & 0xffffff;
    entry |= addr_lo << 8;
    /* phys[59:36] -> high word bits [23:0]. */
    uint64_t addr_hi = (phys >> 36) & 0xffffff;
    entry |= addr_hi << 32;
    return entry;
}

/* Allocate a fresh 4 KB PMM page, zero it, cache_clean the whole
 * thing so the GPU sees an all-zero (= all-invalid) table when
 * we point a parent PDE at it. Returns kernel VA (== phys on
 * Jetson via identity map) or NULL on PMM exhaustion. */

/* uart_printf forward decl — file-scope `extern` further down
 * covers later sites; the trace path below needs it earlier. */
extern int uart_printf(const char *fmt, ...);

/* #788 Stage 6: trace every alloc_zero_table_page call when the
 * flag is on. Set by the rebuild path before its mapping loop;
 * cleared at the end. Lets the operator see which physes the
 * rebuild's intermediate PT pages landed on — needed to identify
 * the colliding-with-FW-state phys that breaks `nvgpu submit` in
 * Stage 4 bisect E/F (pushbuf/sem reservation regression). */
static bool g_table_alloc_trace = false;
static int  g_table_alloc_count = 0;

void ga10b_gmmu_table_alloc_trace_set(bool on)
{
    g_table_alloc_trace = on;
    if (on) {
        g_table_alloc_count = 0;
    }
}

static void *alloc_zero_table_page(void)
{
    void *p = pmm_alloc_page();
    if (p == NULL) return NULL;
    if (g_table_alloc_trace) {
        uart_printf("[rebuild-pt] alloc[%d] phys=0x%lx\n",
                    g_table_alloc_count++,
                    (unsigned long)(uintptr_t)p);
    }
    memset(p, 0, 4096);
    cache_clean_range(p, 4096);
    return p;
}

/* Walk a regular PDE level. If the entry is invalid, allocate a
 * fresh next-level table, write the parent PDE, cache_clean.
 * Returns next-level table phys via *out_next_phys.
 *
 * `parent_table_phys` is the table page containing the entry to
 * read/write. `idx` is the entry's index within that page.
 */
static int ensure_regular_pde(uint64_t parent_table_phys,
                              uint16_t idx,
                              uint64_t *out_next_phys)
{
    uint64_t entry_phys = parent_table_phys + (uint64_t)idx * GA10B_GMMU_ENTRY_SIZE;
    uint64_t entry = phys_read64(entry_phys);
    uint64_t next_phys;
    uint8_t aperture;
    bool valid;
    decode_regular_pde(entry, &next_phys, &aperture, &valid);
    if (valid) {
        *out_next_phys = next_phys;
        return 0;
    }
    /* Empty slot — allocate next-level table. */
    void *new_table = alloc_zero_table_page();
    if (new_table == NULL) return -1;
    uint64_t new_phys = (uint64_t)(uintptr_t)new_table;
    uint64_t new_entry = encode_regular_pde(new_phys);
    *(volatile uint64_t *)(uintptr_t)entry_phys = new_entry;
    cache_clean((const volatile void *)(uintptr_t)entry_phys);
    *out_next_phys = new_phys;
    return 0;
}

/* Walk PDE0 (the dual PDE). Reads existing entry; if the small
 * half is unpopulated, allocates a fresh small leaf-PT page and
 * writes the small half via read-modify-write — preserving
 * whatever Linux may have placed in the big half (a 64 KB or
 * 2 MB mapping covering the same 32 MB VA region).
 *
 * `pde0_table_phys` is the PDE0 table page. `idx` is the dual
 * PDE entry within it. Returns small-leaf PT phys via *out.
 */
static int ensure_dual_pde_small_table(uint64_t pde0_table_phys,
                                       uint16_t idx,
                                       uint64_t *out_small_phys)
{
    uint64_t entry_phys = pde0_table_phys + (uint64_t)idx * GA10B_GMMU_ENTRY_SIZE;
    uint64_t entry = phys_read64(entry_phys);
    uint64_t small_phys;
    uint8_t aperture;
    bool valid;
    decode_dual_pde_small(entry, &small_phys, &aperture, &valid);
    if (valid) {
        *out_small_phys = small_phys;
        return 0;
    }
    /* Allocate the small leaf table. Preserve the high 32 bits
     * (potential big-page half) via mask. */
    void *new_pt = alloc_zero_table_page();
    if (new_pt == NULL) return -1;
    uint64_t new_phys = (uint64_t)(uintptr_t)new_pt;
    uint64_t small_bits = encode_dual_pde_small(new_phys);
    /* Read-modify-write: keep upper 32 bits (big-half) intact. */
    uint64_t merged = (entry & 0xffffffff00000000ull) | (small_bits & 0xffffffffull);
    *(volatile uint64_t *)(uintptr_t)entry_phys = merged;
    cache_clean((const volatile void *)(uintptr_t)entry_phys);
    *out_small_phys = new_phys;
    return 0;
}

/* Read PDB physical from inst block. Returns 0 on failure (which
 * the caller treats as "bad inst block"). Mirrors the inline
 * decode in ga10b_gmmu_walk. */
static uint64_t read_pdb_phys(uint64_t inst_block_phys)
{
    if (!phys_in_dram(inst_block_phys, 4096)) return 0;
    uint32_t pdb_lo = phys_read32(inst_block_phys + (128u * 4u));
    uint32_t pdb_hi = phys_read32(inst_block_phys + (129u * 4u));
    uint8_t target = (uint8_t)((pdb_lo >> 1) & 0x3);
    if (target == 0) return 0;  /* vid_mem on a Jetson with no vidmem == bogus */
    uint32_t phys_lo = pdb_lo & 0xfffff000;
    uint64_t pdb_phys = ((uint64_t)pdb_hi << 32) | phys_lo;
    if (!phys_in_dram(pdb_phys, 4096)) return 0;
    return pdb_phys;
}

/* Map one page at `gpu_va` to `data_phys`. Walks PDE3 → PDE0,
 * allocating intermediate tables on demand; writes the leaf PTE.
 * Does NOT issue dsb sy — caller is expected to issue one after
 * a batch of map_one_page calls so the cost amortizes.
 *
 * Leaf index width matches the SMALL-page child of PDE0. The
 * encoder always populates the small-side leaf via
 * `ensure_dual_pde_small_table`, so the index width here MUST
 * match what a small-side walker uses: bits [20:12] (9 bits,
 * 512 entries × 4 KB = 2 MB covered per leaf), NOT bits [20:16]
 * (the big-side 5-bit width). The walker's small-side fallback
 * (PR #969) decodes via [20:12]; using [20:16] here meant 16
 * adjacent 4-KB pages collided on the same PTE slot — the final
 * `map_one_page` won, the others silently disappeared, and any
 * subsequent walk past the first page of a 64-KB region landed
 * on an "invalid PTE" entry. */
static int map_one_page(uint64_t pdb_phys,
                        uint64_t gpu_va,
                        uint64_t data_phys,
                        uint32_t flags)
{
    uint16_t i3 = va_index(gpu_va, GA10B_PDE3_VA_HI, GA10B_PDE3_VA_LO);
    uint16_t i2 = va_index(gpu_va, GA10B_PDE2_VA_HI, GA10B_PDE2_VA_LO);
    uint16_t i1 = va_index(gpu_va, GA10B_PDE1_VA_HI, GA10B_PDE1_VA_LO);
    uint16_t i0 = va_index(gpu_va, GA10B_PDE0_VA_HI, GA10B_PDE0_VA_LO);
    uint16_t ip = va_index(gpu_va, GA10B_PTE_SMALL_VA_HI,
                                    GA10B_PTE_SMALL_VA_LO);

    uint64_t pde2_phys, pde1_phys, pde0_phys, pte_phys;
    if (ensure_regular_pde(pdb_phys, i3, &pde2_phys) < 0) return -1;
    if (ensure_regular_pde(pde2_phys, i2, &pde1_phys) < 0) return -1;
    if (ensure_regular_pde(pde1_phys, i1, &pde0_phys) < 0) return -1;
    if (ensure_dual_pde_small_table(pde0_phys, i0, &pte_phys) < 0) return -1;

    uint64_t pte_entry_phys = pte_phys + (uint64_t)ip * GA10B_GMMU_ENTRY_SIZE;
    uint64_t pte_entry = encode_pte(data_phys, flags);
    *(volatile uint64_t *)(uintptr_t)pte_entry_phys = pte_entry;
    cache_clean((const volatile void *)(uintptr_t)pte_entry_phys);
    return 0;
}

/* Defined alongside ga10b_gmmu_free below; forward decl so the
 * alloc paths can consult the free-extent tracker before touching
 * the bump cursor. */
static uint64_t pop_free_extent(uint32_t n_pages);

int ga10b_gmmu_alloc_page(uint64_t inst_block_phys,
                          uint32_t flags,
                          uint64_t *out_gpu_va,
                          void    **out_cpu_va,
                          uint64_t *out_phys)
{
    if (out_gpu_va == NULL || out_cpu_va == NULL || out_phys == NULL) {
        return -1;
    }
    uint64_t pdb_phys = read_pdb_phys(inst_block_phys);
    if (pdb_phys == 0) return -1;

    /* Reuse a freed VA before bumping the cursor. The free path
     * already TLB-invalidated this VA, so a fresh PTE write here
     * lands on a clean translation slot. */
    uint64_t va = pop_free_extent(1);
    if (va == 0) {
        if (g_va_cursor >= GA10B_GMMU_VA_LIMIT) return -1;
        va = g_va_cursor;
        g_va_cursor += 4096;
    }

    void *data_page = pmm_alloc_page();
    if (data_page == NULL) return -1;
    uint64_t data_phys = (uint64_t)(uintptr_t)data_page;

    if (map_one_page(pdb_phys, va, data_phys, flags) < 0) return -1;

    /* DSB SY: order PT publication relative to anything the caller
     * does next (memcpy via cpu_va, GPU dispatch). Without this the
     * GPU can race the cache_clean and TLB-walk stale entries on
     * the very first lookup. */
    __asm__ volatile("dsb sy" ::: "memory");

    *out_gpu_va = va;
    *out_cpu_va = data_page;
    *out_phys = data_phys;
    return 0;
}

/* ---------------------------------------------------------------------
 * Multi-page alloc + free
 * --------------------------------------------------------------------- */

struct gmmu_free_extent {
    uint64_t gpu_va;
    uint32_t n_pages;
    uint32_t _pad;
};

static struct gmmu_free_extent g_free_extents[GA10B_GMMU_FREE_TRACKER_SLOTS];
static uint32_t g_free_extent_count;

uint32_t ga10b_gmmu_free_tracker_count(void)
{
    return g_free_extent_count;
}

/* GA10B BAR0 base on Jetson Orin Nano (per
 * `kernel/gpu/gpu_nvidia.h`'s comment header — MMIO at 0x17000000).
 * NV_PGRAPH_PRI_FECS_CURRENT_CTX lives at BAR0 + 0x409b00 per
 * `slmos-reference-cache/nvidia/nvgpu-hw-ga10b-hw_gr_ga10b.h`'s
 * `gr_fecs_current_ctx_r() = 0x00409b00U`. Identity-mapped — no
 * extra ioremap needed.
 *
 * Register layout (gr_fecs_current_ctx_*):
 *   bits [27:0]  : ptr   = inst_block_phys >> 12
 *   bits [29:28] : target (0=vid_mem, 2=sys_mem_coh, 3=sys_mem_ncoh)
 */
#define GA10B_BAR0_BASE                  0x17000000ull
#define GA10B_GR_FECS_CURRENT_CTX_OFFSET 0x00409b00u

uint64_t ga10b_gmmu_discover_inst_block_phys(void)
{
    uint32_t reg = *(volatile uint32_t *)(uintptr_t)
        (GA10B_BAR0_BASE + GA10B_GR_FECS_CURRENT_CTX_OFFSET);
    uint8_t target = (uint8_t)((reg >> 28) & 0x3);
    if (target == 0) {
        /* vid_mem on Jetson is bogus (no discrete vidmem); also
         * matches the "register reads as 0xfffffffe / similar
         * garbage when GPU is asleep" failure mode. */
        return 0;
    }
    uint32_t ptr_v = reg & 0x0fffffffu;
    uint64_t inst_block_phys = ((uint64_t)ptr_v) << 12;
    if (!phys_in_dram(inst_block_phys, 4096)) return 0;
    return inst_block_phys;
}

uint64_t ga10b_gmmu_discover_inst_block_via_walk(uint64_t known_gpu_va,
                                                  uint64_t expected_leaf_phys,
                                                  uint64_t scan_start,
                                                  uint64_t scan_end)
{
    if (known_gpu_va == 0 || expected_leaf_phys == 0) return 0;
    if (scan_start >= scan_end) return 0;

    /* 4 KB-align the scan range. Inst blocks are page-aligned per
     * nvgpu's `nvgpu_dma_alloc_flags_sys` call (gv11b/gp10b channel
     * RAM uses NVGPU_DMA_FORCE_CONTIGUOUS | sys-mem alloc, both of
     * which return at least page-aligned phys). */
    uint64_t cursor = (scan_start + 4095u) & ~((uint64_t)4095u);
    uint64_t end    = scan_end & ~((uint64_t)4095u);

    /* Diagnostic counters: how many candidates passed the pre-filter,
     * how many full walks actually fired. Printed via uart_printf
     * only when `gpu debug on` is set (symmetric with the v7
     * dispatch tracing) so a steady-state caller doesn't spam the
     * UART. Errors / final-result lines below are gated the same
     * way — diagnosis is opt-in. */
    extern int uart_printf(const char *fmt, ...);
    extern bool ga10b_dispatch_verbose_get(void);
    bool dbg = ga10b_dispatch_verbose_get();
    uint32_t pre_passed = 0;
    uint32_t walks      = 0;
    uint32_t walk_ok    = 0;

    for (; cursor < end; cursor += 4096u) {
        if (!phys_in_dram(cursor, 4096)) continue;

        /* Cheap filter: peek PDB lo word at +0x200. The aperture
         * field lives in bits [2:1] of the lo word, NOT bits
         * [29:28] — see read_pdb_phys above for the canonical
         * decode. Reject unless aperture is non-zero (vid_mem on
         * Jetson is bogus) and the resulting PDB phys lands in
         * DRAM. This rejects most pages without paying the walker's
         * cost; the walker is what actually validates the rest. */
        uint32_t pdb_lo = phys_read32(cursor + (128u * 4u));
        uint8_t  target = (uint8_t)((pdb_lo >> 1) & 0x3u);
        if (target == 0) continue;
        uint32_t pdb_hi  = phys_read32(cursor + (129u * 4u));
        uint32_t phys_lo = pdb_lo & 0xfffff000u;
        uint64_t pdb_phys = ((uint64_t)pdb_hi << 32) | phys_lo;
        if (!phys_in_dram(pdb_phys, 4096)) continue;
        pre_passed++;

        /* Full check: walk this candidate. The walker silently tolerates
         * a bad PDB by returning status BAD_INST; OK + matching leaf
         * is a positive match. */
        struct ga10b_gmmu_walk_result wr;
        walks++;
        if (ga10b_gmmu_walk(cursor, known_gpu_va, &wr) != 0) continue;
        if (wr.status != GA10B_GMMU_WALK_OK) continue;
        walk_ok++;
        if (wr.leaf_phys != expected_leaf_phys) {
            /* Useful trace: walks that completed but landed at a
             * different leaf (e.g. another channel's inst block
             * mapping the same VA range to a different page). */
            if (dbg && walk_ok <= 4) {
                uart_printf("[gmmu-discover] walk OK at inst=0x%lx "
                            "leaf=0x%lx (want 0x%lx)\n",
                            (unsigned long)cursor,
                            (unsigned long)wr.leaf_phys,
                            (unsigned long)expected_leaf_phys);
            }
            continue;
        }
        if (dbg) {
            uart_printf("[gmmu-discover] match at inst=0x%lx after %u "
                        "candidates, %u walks (%u OK)\n",
                        (unsigned long)cursor,
                        pre_passed, walks, walk_ok);
        }
        return cursor;
    }
    if (dbg) {
        uart_printf("[gmmu-discover] no match after %u candidates "
                    "(%u pre-filter pass, %u walks, %u walk-OK)\n",
                    (unsigned)((end - scan_start) / 4096u),
                    pre_passed, walks, walk_ok);
    }
    return 0;
}

/* GA10B FB MMU register offsets (from
 * ~/slmos-ref/nvidia/nvgpu-l4t-r36.4.4-hw_fb_ga10b.h):
 *   fb_mmu_ctrl_r            = 0x100c80   (status, 32-bit)
 *     bit 15        = pri_fifo_empty  (1 = invalidate done)
 *     bits 16..23   = pri_fifo_space  (non-zero = FIFO has room)
 *   fb_mmu_invalidate_pdb_r  = 0x100cb8   (set target PDB)
 *     bits 4..31    = (pdb_phys >> 12)
 *     bits 0..1     = aperture (sys_mem=2, vid_mem=0)
 *   fb_mmu_invalidate_r      = 0x100cbc   (kick + scope)
 *     bit 0         = all_va
 *     bit 31        = trigger
 */
#define GA10B_FB_MMU_CTRL_OFFSET                  0x00100c80u
#define GA10B_FB_MMU_INVALIDATE_PDB_OFFSET        0x00100cb8u
#define GA10B_FB_MMU_INVALIDATE_OFFSET            0x00100cbcu
#define GA10B_FB_MMU_CTRL_PRI_FIFO_EMPTY_BIT      (1u << 15)
#define GA10B_FB_MMU_INVALIDATE_PDB_APERTURE_SYS  0x2u
#define GA10B_FB_MMU_INVALIDATE_ALL_VA_TRUE       0x1u
#define GA10B_FB_MMU_INVALIDATE_TRIGGER_TRUE      0x80000000u

/* nvgpu uses 1000 retries x 2us = 2ms total. Match that. */
#define GA10B_TLB_POLL_MAX_RETRIES                1000u
#define GA10B_TLB_POLL_INTERVAL_US                2u

/* Offset is uint64_t so callers can pass a discovered register-block
 * base (e.g. `gr_rl_pri_base`, up to 16 MB) plus a small offset
 * without implicit narrowing. Existing call sites that pass a
 * uint32_t literal widen silently. */
static inline uint32_t bar0_read32(uint64_t offset)
{
    return *(volatile uint32_t *)(uintptr_t)(GA10B_BAR0_BASE + offset);
}

static inline void bar0_write32(uint64_t offset, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)(GA10B_BAR0_BASE + offset) = value;
}

int ga10b_gmmu_tlb_invalidate(uint64_t pdb_phys)
{
    if (!phys_in_dram(pdb_phys, 4096)) return -1;

    /* 1. Wait for PRI FIFO room. */
    uint32_t retries = 0;
    while (((bar0_read32(GA10B_FB_MMU_CTRL_OFFSET) >> 16) & 0xffu) == 0u) {
        if (++retries > GA10B_TLB_POLL_MAX_RETRIES) return -1;
        timer_busy_wait_us(GA10B_TLB_POLL_INTERVAL_US);
    }

    /* 2. Write PDB target. Bits [31:4] of the encoded register =
     * pdb_phys[39:12]; bits [1:0] = aperture. */
    uint32_t pdb_lo28 = (uint32_t)((pdb_phys >> 12) & 0x0fffffffu);
    bar0_write32(GA10B_FB_MMU_INVALIDATE_PDB_OFFSET,
                 (pdb_lo28 << 4) | GA10B_FB_MMU_INVALIDATE_PDB_APERTURE_SYS);

    /* 3. Trigger an all-VA invalidate against that PDB. */
    bar0_write32(GA10B_FB_MMU_INVALIDATE_OFFSET,
                 GA10B_FB_MMU_INVALIDATE_ALL_VA_TRUE |
                 GA10B_FB_MMU_INVALIDATE_TRIGGER_TRUE);

    /* 4. Wait for completion (FIFO drains to empty). */
    retries = 0;
    while ((bar0_read32(GA10B_FB_MMU_CTRL_OFFSET) &
            GA10B_FB_MMU_CTRL_PRI_FIFO_EMPTY_BIT) == 0u) {
        if (++retries > GA10B_TLB_POLL_MAX_RETRIES) return -1;
        timer_busy_wait_us(GA10B_TLB_POLL_INTERVAL_US);
    }

    return 0;
}

int ga10b_gmmu_alloc(uint64_t inst_block_phys,
                     uint32_t n_pages,
                     uint32_t flags,
                     uint64_t *out_gpu_va_base,
                     void    **out_first_cpu_va,
                     uint64_t *out_first_phys)
{
    if (out_gpu_va_base == NULL || out_first_cpu_va == NULL ||
        out_first_phys == NULL) {
        return -1;
    }
    if (n_pages == 0 || n_pages > GA10B_GMMU_MAX_ALLOC_PAGES) {
        return -1;
    }

    uint64_t pdb_phys = read_pdb_phys(inst_block_phys);
    if (pdb_phys == 0) return -1;

    /* Try exact-fit reuse from the free-extent tracker first.
     * The free path TLB-invalidated when the extent was returned,
     * so a fresh PTE write here lands on a clean slot. */
    uint64_t va_span = (uint64_t)n_pages * 4096ull;
    uint64_t va_base = pop_free_extent(n_pages);
    if (va_base == 0) {
        /* Reserve the contiguous VA range from the bump cursor.
         * Range exhaustion is checked atomically with the
         * reservation so a partial alloc from the tail of the
         * range doesn't leak VAs (caller can retry with a smaller
         * request, or admit defeat). */
        if (g_va_cursor + va_span > GA10B_GMMU_VA_LIMIT) {
            return -1;
        }
        va_base = g_va_cursor;
        g_va_cursor += va_span;
    }

    /* Per page: alloc PMM page, map it. PMM pages need not be
     * contiguous in phys — every page gets its own PTE entry
     * pointing at its individual phys. */
    void *first_cpu_va = NULL;
    uint64_t first_phys = 0;
    for (uint32_t i = 0; i < n_pages; i++) {
        void *data = pmm_alloc_page();
        if (data == NULL) {
            /* Partial alloc — the pages we mapped before this
             * stay mapped and the PTEs stay set. The PMM pages
             * we already alloc'd leak (no rollback yet; tracked
             * in #825). Caller treats this as fatal. */
            return -1;
        }
        if (i == 0) {
            first_cpu_va = data;
            first_phys = (uint64_t)(uintptr_t)data;
        }
        uint64_t va_i = va_base + (uint64_t)i * 4096ull;
        uint64_t phys_i = (uint64_t)(uintptr_t)data;
        if (map_one_page(pdb_phys, va_i, phys_i, flags) < 0) {
            return -1;
        }
    }

    /* Single dsb sy at the end — amortizes across all N pages. */
    __asm__ volatile("dsb sy" ::: "memory");

    *out_gpu_va_base = va_base;
    *out_first_cpu_va = first_cpu_va;
    *out_first_phys = first_phys;
    return 0;
}

int ga10b_gmmu_map(uint64_t inst_block_phys,
                   uint64_t target_phys,
                   uint32_t n_pages,
                   uint32_t flags,
                   uint64_t *out_gpu_va_base)
{
    if (out_gpu_va_base == NULL) return -1;
    if (n_pages == 0 || n_pages > GA10B_GMMU_MAX_ALLOC_PAGES) {
        return -1;
    }
    /* target_phys must be 4 KB-aligned — the GMMU's smallest PTE
     * granularity. A non-aligned base would cause every per-page
     * PTE write to be off by some fraction of a page, which the
     * GPU would happily fetch from before silently producing
     * garbage. */
    if ((target_phys & 4095ull) != 0u) return -1;

    uint64_t pdb_phys = read_pdb_phys(inst_block_phys);
    if (pdb_phys == 0) return -1;

    /* Reserve a contiguous GPU VA range — same shape as
     * ga10b_gmmu_alloc, but skip the per-page PMM allocation step
     * because the caller already owns the physical buffer. */
    uint64_t va_span = (uint64_t)n_pages * 4096ull;
    uint64_t va_base = pop_free_extent(n_pages);
    if (va_base == 0) {
        if (g_va_cursor + va_span > GA10B_GMMU_VA_LIMIT) {
            return -1;
        }
        va_base = g_va_cursor;
        g_va_cursor += va_span;
    }

    /* Per page: emit a PTE pointing at the caller's contiguous
     * physical buffer. Since target_phys is contiguous, page i's
     * physical address is just target_phys + i*4096 — no walking
     * needed. */
    for (uint32_t i = 0; i < n_pages; i++) {
        uint64_t va_i   = va_base   + (uint64_t)i * 4096ull;
        uint64_t phys_i = target_phys + (uint64_t)i * 4096ull;
        if (map_one_page(pdb_phys, va_i, phys_i, flags) < 0) {
            /* PT-page allocation can fail mid-walk. We have no
             * rollback for the PTEs already written (same gap as
             * ga10b_gmmu_alloc; #825 tracks). Caller treats it
             * as fatal. */
            return -1;
        }
    }

    /* Single dsb sy at the end — amortizes across all N pages,
     * matches ga10b_gmmu_alloc's pattern. */
    __asm__ volatile("dsb sy" ::: "memory");

    *out_gpu_va_base = va_base;
    return 0;
}

int ga10b_gmmu_free(uint64_t inst_block_phys,
                    uint64_t gpu_va,
                    uint32_t n_pages)
{
    if (n_pages == 0 || n_pages > GA10B_GMMU_MAX_ALLOC_PAGES) return -1;
    if ((gpu_va & 0xfff) != 0) return -1;
    if (gpu_va < GA10B_GMMU_VA_BASE ||
        gpu_va + (uint64_t)n_pages * 4096ull > GA10B_GMMU_VA_LIMIT) {
        return -1;
    }

    /* Per page: walk to find the leaf PTE, clear it. */
    for (uint32_t i = 0; i < n_pages; i++) {
        uint64_t va_i = gpu_va + (uint64_t)i * 4096ull;
        struct ga10b_gmmu_walk_result wr;
        if (ga10b_gmmu_walk(inst_block_phys, va_i, &wr) != 0) {
            /* Walker shouldn't return non-zero — it always returns
             * 0 with a status field. Defensive bail. */
            return -1;
        }
        if (wr.status != GA10B_GMMU_WALK_OK) {
            /* Page wasn't mapped to begin with. Skip — caller may
             * have called free twice or with a slightly-off range;
             * the rest of the contiguous range is still freed. */
            continue;
        }
        const struct ga10b_gmmu_level_record *pte_rec = &wr.levels[4];
        uint64_t pte_entry_phys = pte_rec->table_phys +
                                  (uint64_t)pte_rec->index * GA10B_GMMU_ENTRY_SIZE;
        /* Clear the PTE: zero entry = invalid + addr=0 + flags=0. */
        *(volatile uint64_t *)(uintptr_t)pte_entry_phys = 0;
        cache_clean((const volatile void *)(uintptr_t)pte_entry_phys);
    }

    /* DSB SY so the GPU's GMMU walker sees the cleared PTEs from
     * PoC if it re-walks the page table (after the TLB invalidate
     * forces a re-walk on next access). */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Fire the TLB invalidate now — without it, the GPU could serve
     * a cached translation for a freed VA, defeating the point of
     * the free. The PDB phys is read from the inst block (same
     * source as the walker's PDB lookup). */
    uint64_t pdb_phys = read_pdb_phys(inst_block_phys);
    if (pdb_phys != 0) {
        /* Best-effort: a TLB invalidate timeout doesn't roll back
         * the free, since the PTEs are already cleared. The freed
         * VA is at worst served from a stale TLB entry until the
         * next successful invalidate (which the next free will
         * fire). */
        (void)ga10b_gmmu_tlb_invalidate(pdb_phys);
    }

    /* Track the freed extent for VA reuse. ga10b_gmmu_alloc /
     * alloc_page consult this table BEFORE bumping the cursor,
     * pulling the first slot whose n_pages matches the request.
     * Single-page reuse is what the "alloc 1 → free → alloc 1
     * same VA" smoke test exercises; multi-page exact-fit reuse
     * uses the same path. */
    if (g_free_extent_count < GA10B_GMMU_FREE_TRACKER_SLOTS) {
        g_free_extents[g_free_extent_count].gpu_va = gpu_va;
        g_free_extents[g_free_extent_count].n_pages = n_pages;
        g_free_extent_count++;
    }
    return 0;
}

/* ============================================================
 * #788 Stage 2: rebuild GMMU state for an inherited channel.
 *
 * After kexec, Linux's nvgpu has freed the channel's inst block,
 * page-directory base (PDB), and intermediate page tables —
 * verified by `nvgpu instdump` (PR #797) showing inst-block
 * contents as ARM64 kernel code (mid-boot allocations) or random
 * weight bytes (mid-xload). The Stage 1 reservation in
 * `pmm_user_reserve_add` (PR #801) keeps SLM-OS from overwriting
 * the inst-block page itself, but the PDB and page tables stay
 * gone — they were freed before SLM-OS could see them.
 *
 * What survives: the helper's user-allocated dmabufs (USERD,
 * GPFIFO, pushbuffer, semaphore, SASS pool, cbuf, QMD pool,
 * weights pool first-page, handoff dmabuf). These are tied to
 * the helper's open file descriptors, so Linux's reference
 * counting keeps them alive across kexec. The handoff struct
 * publishes (phys, gpu_va, size) for each.
 *
 * Strategy: build a fresh PDB tree in SLM-OS and re-install PTEs
 * mapping every preserved dmabuf at its original GPU VA. Write
 * the inst-block's PDB pointer to point at the new tree. The
 * GPU's runlist still references the old inst-block physical
 * address (because that's what nvgpu wrote pre-kexec), but we're
 * filling THAT physical page with a fresh inst block — so the
 * runlist's pointer becomes meaningful again. TLB-invalidate
 * after the rebuild so any cached PDB-walks from before-kexec
 * are flushed.
 *
 * **Scope:** maps fixed-size buffers from the handoff at their
 * recorded GPU VAs. Skips the weights pool beyond its first
 * page — the 1.5 GB region is SMMU-stitched from scattered
 * physical pages, and the handoff only publishes the first
 * page's phys. A future enhancement (Helper Stage 2.1?) could
 * enumerate per-page phys via /proc/self/pagemap pre-kexec and
 * publish a longer reserve list, enabling full weights-pool
 * mapping. Until then, slm xload's W3 staging will succeed for
 * the first 4 KB chunk only.
 * ============================================================ */

#include "ga10b_channel_handoff.h"
#include "ga10b_bringup.h"          /* ga10b_dump_inst_block_at */
#include "../../include/uart.h"

/* Inst-block PDB-pointer encoding per
 * `~/slmos-ref/nvidia/nvgpu-l4t-r36.4.4-hw_ram_ga10b.h:62-71`.
 * Replicates `ga10b_ramin_init_pdb` from the nvgpu source — keeps
 * the encoding correct without dragging the full HW reg header in. */
#define GA10B_RAM_IN_PDB_TARGET_SYS_NCOH  0x3u
#define GA10B_RAM_IN_PDB_VOL_TRUE         0x4u
#define GA10B_RAM_IN_USE_VER2_PT_FORMAT   0x400u
#define GA10B_RAM_IN_BIG_PAGE_SIZE_64KB   0x800u

/* RAMFC field word offsets per
 * `~/slmos-ref/nvidia/nvgpu-l4t-r36.4.4-hw_ram_ga10b.h:118-140`.
 * RAMFC starts at word 0 of the inst block and ends at word 127;
 * PDB lives at words 128/129. All offsets here are word indices
 * (multiply by 4 for the byte offset into the inst block). */
#define GA10B_RAMFC_W_SIGNATURE            4u
#define GA10B_RAMFC_W_ACQUIRE              12u
#define GA10B_RAMFC_W_GP_BASE              18u
#define GA10B_RAMFC_W_GP_BASE_HI           19u
#define GA10B_RAMFC_W_PB_HEADER            33u
#define GA10B_RAMFC_W_SUBDEVICE            37u
#define GA10B_RAMFC_W_TARGET               43u
#define GA10B_RAMFC_W_CONFIG               61u
#define GA10B_RAMFC_W_INTR_NOTIFY          62u
#define GA10B_RAMFC_W_SET_CHANNEL_INFO     63u
/* engine_fw_magic_value (word 131) must hold 0xcafeca11 for FECS to
 * accept the channel during ctxsw. Per nvgpu's
 * `ga10b_ramin_set_magic_value` (~/slmos-ref/nvidia/
 * nvgpu-hal-fifo-ramin_ga10b_fusa.c). Missing this value was the
 * proximate cause of the 0xbadf1002 GR-register poison observed
 * during the #834 fix-A attempt — FECS rejected the channel because
 * the rebuild zeroed this slot. #839. */
#define GA10B_RAMIN_W_ENGINE_FW_MAGIC      131u
#define GA10B_RAMIN_VAL_ENGINE_FW_MAGIC    0xcafeca11u
/* engine_wfi block (post-PDB). nvgpu's `ram_in_engine_wfi_*` macros
 * lay out a 3-word slot at words 132..134:
 *   132 (target_lo): aperture in low bits + ptr[31:8] in high bits
 *   133 (ptr_hi):    inst-save buffer phys[39:32]
 *   134 (veid):      the subcontext VEID this slot's save belongs to
 * Words 136..137 are `eng_method_buffer_addr` lo/hi — the GR-engine
 * method scratch buffer Linux's nvgpu allocates lazily on first
 * channel engagement (zero on a fresh helper-published inst block). */
#define GA10B_RAMIN_W_ENGINE_WFI_TARGET_LO 132u
#define GA10B_RAMIN_W_ENGINE_WFI_PTR_HI    133u
#define GA10B_RAMIN_W_ENGINE_WFI_VEID      134u
#define GA10B_RAMIN_W_ENG_METHOD_BUF_LO    136u
#define GA10B_RAMIN_W_ENG_METHOD_BUF_HI    137u
/* Subcontext (per-VEID) PDB region — GA10B inst blocks have 64
 * per-VEID PDB slots. GR loads the subcontext indexed by VEID and
 * uses *that* subcontext's PDB pointer when running compute, NOT
 * the main PDB at word 128. Layout per nvgpu's ram_in_sc_* macros
 * in `~/slmos-ref/nvidia/nvgpu-l4t-r36.4.4-hw_ram_ga10b.h`:
 *
 *   - pdb_valid_long bits: 1 bit per VEID, packed 32-per-word.
 *     VEID i's bit lives in word `166 + i/32`, bit position `i % 32`.
 *   - Per-VEID PDB pointer: each subcontext entry is 4 words wide.
 *     VEID i's pdb_lo is at word `168 + 4*i`, pdb_hi at `169 + 4*i`.
 *
 * SLM-OS only uses one subcontext today — GA10B_HELPER_SUBCTX_ID
 * (= 1 = ASYNC veid, matches what the helper publishes via its
 * CREATE_SUBCONTEXT call). Writing only this subcontext's slot
 * leaves the other 63 untouched, which preserves whatever Linux
 * had set up (e.g., subcontext 0 with a different PDB pointer for
 * a Linux-side use case the channel may still need). */
#define GA10B_RAMIN_W_SC_PDB_VALID_LONG_LO 166u
#define GA10B_RAMIN_W_SC_PDB_VALID_LONG_HI 167u
#define GA10B_RAMIN_W_SC_PDB_BASE(veid)    (168u + 4u * (veid))

/* PBDMA-encoded field values for a default Tegra GA10B channel
 * (chid=0, subctx=1 ASYNC veid, engine=GR rleng_id=0, intr_vec=0,
 * privileged-config=no, long acquire timeout saturated). Derived
 * by mechanically following the L4T r36.4.4 nvgpu HAL ops chain:
 *
 *   .signature   = gp10b_pbdma_get_signature
 *                  → litter(GPFIFO_CLASS) = AMPERE_CHANNEL_GPFIFO_B
 *   .pb_header   = gv11b_pbdma_get_fc_pb_header
 *                  → method_zero | subch_zero | level_main |
 *                    first_true | type_inc
 *   .subdevice   = gm20b_pbdma_get_fc_subdevice
 *                  → subdevice_id(1) | status_active | channel_dma_enable
 *   .target      = ga10b_pbdma_get_fc_target
 *                  → engine_f(rleng=0) | eng_ctx_valid_true | ce_ctx_valid_true
 *   .acquire     = gm20b_pbdma_acquire_val (2 s scaled saturates)
 *                  → retry_man_2 | retry_exp_2 |
 *                    timeout_man_max | timeout_exp_max | timeout_en_enable
 *   .intr_notify = ga10b_pbdma_set_intr_notify(0)
 *                  → vector_f(0) | ctrl_cpu_enable | ctrl_gsp_disable
 *   .config      = gv11b_pbdma_config_userd_writeback_enable(0)
 *                  → userd_writeback_enable bit (12)
 *
 * Documented offsets keyed back to the L4T header so a future
 * investigator can re-derive these from the nvgpu source if the
 * Tegra HAL chain ever changes. */
#define GA10B_RAMFC_VAL_SIGNATURE          0x0000C76Fu  /* AMPERE_CHANNEL_GPFIFO_B */
#define GA10B_RAMFC_VAL_PB_HEADER          0x20400000u  /* first | type_inc */
#define GA10B_RAMFC_VAL_SUBDEVICE          0x30000001u  /* id=1 | active | dma_enable */
#define GA10B_RAMFC_VAL_TARGET_GR          0x00030000u  /* rleng=0 | eng+ce_ctx_valid */
#define GA10B_RAMFC_VAL_ACQUIRE_LONG       0xFFFF7902u  /* saturated 2 s acquire */
#define GA10B_RAMFC_VAL_INTR_NOTIFY        0x80000000u  /* vec=0 | cpu_enable */
#define GA10B_RAMFC_VAL_CONFIG_USERD_WB    0x00001000u  /* userd_writeback bit */

/* `set_channel_info` is read-modify-write on a 32-bit value with
 * `chid` at bits[27:16] and `veid` at bits[13:8]. We start from 0
 * (the inst block was just zeroed) and OR in the encoded fields. */
#define GA10B_RAMFC_SET_CHANNEL_INFO(chid, veid) \
    ((((uint32_t)(chid) & 0xfffu) << 16) | (((uint32_t)(veid) & 0x3fu) << 8))

/* The helper's CREATE_SUBCONTEXT call publishes subctx_id=1 (ASYNC
 * veid=1). The handoff doesn't carry this explicitly today
 * (channel_id and tsg_id are u32 fields but veid is implicit);
 * hardcoded here matching the helper's invariant. If a future
 * helper revision uses a different subctx_id, both this constant
 * AND the helper need updating in lock-step — fortunately the
 * runlist + USERD config flow through SLM-OS only handles single-
 * subctx channels today, so the hardcode is the simplest safe
 * choice. Document in `ga10b_channel_handoff.h` if a veid field
 * is ever added to the handoff struct. */
#define GA10B_HELPER_SUBCTX_ID             1u

/* Runlist register block offsets (relative to the engine's
 * `runlist_pri_base`, which we discover via the topology walk in
 * `install_fresh_runlist` below). Constants here factor out the
 * raw magic offsets that previously cluttered the per-write
 * sites. */
#define GA10B_RL_REG_CHANNEL_CONFIG    0x04u  /* CHRAM bar0 offset */
#define GA10B_RL_REG_SUBMIT_BASE_LO    0x80u  /* phys[31:10] | target */
#define GA10B_RL_REG_SUBMIT_BASE_HI    0x84u  /* phys[39:32] in low byte */
#define GA10B_RL_REG_SUBMIT            0x88u  /* (offset<<16) | length */
#define GA10B_RL_REG_SUBMIT_INFO       0x8cu  /* bit 15 = submit/preempt pending */
#define GA10B_RL_REG_SCHED_DISABLE     0x94u  /* 1 = disable scheduling */
#define GA10B_RL_REG_PREEMPT           0x98u  /* 0x200000 = preempt pending */

/* `runlist_submit_info_pending` bit (bit 15) — set while a submit
 * or preempt is in flight, cleared by FECS on completion. */
#define GA10B_RL_SUBMIT_INFO_PENDING_BIT  0x8000u

/* Poll bounds for `runlist_submit_info_pending` to clear after a
 * preempt or submit write. Each iteration is one BAR0 read
 * (~100 ns), so 10000 ≈ 1 ms (preempt — typically <10 µs) and
 * 100000 ≈ 10 ms (submit — typically <100 µs). Caps wall time so
 * a wedged FECS can't hang the shell task indefinitely. Matches
 * the FECS-method poll-iteration idiom in `ga10b_bringup.c`. */
#define GA10B_RL_POLL_PREEMPT_ITERATIONS  10000
#define GA10B_RL_POLL_SUBMIT_ITERATIONS   100000

/* Cached runlist page. The `install_fresh_runlist` path needs a
 * single 4 KB DRAM page to hold the TSG header + channel entry it
 * hands to the GPU via `runlist_submit_base`. The hardware reads
 * from this page indefinitely once the submit lands, so the page
 * must outlive the rebuild call.
 *
 * Cached at file scope so re-running `oplib stage` (which can
 * happen any number of times via the shell verb) reuses the same
 * page instead of leaking one per call. The page contents are
 * fully overwritten on each rebuild, so cross-call coherence is
 * not a concern.
 *
 * Single-writer assumption: the only call site is
 * `install_fresh_runlist_and_chram` from
 * `ga10b_gmmu_rebuild_for_handoff`, which is shell-task-only
 * (`oplib stage` verb). The plain `if (NULL) alloc` initializer
 * has no concurrency protection — if a future caller invokes the
 * rebuild path from a different task or from an ISR, this needs
 * an atomic CAS or a one-shot init guard. */
static void *g_rebuild_runlist_page = NULL;

/* Compute log2 of `n` for n that's a non-zero power of two. Used
 * for the gpfifo `limit2` field in gp_base_hi which encodes
 * log2(num_entries). Returns 0 for n=0 (defensive — caller should
 * have validated). The handoff always carries
 * GPU_LAUNCH_GPFIFO_ENTRIES (currently 512 → log2 = 9), but the
 * computation generalises so a future helper change doesn't bake
 * the magic 9 in. */
static inline uint32_t log2_pow2(uint32_t n)
{
    if (n == 0) return 0;
    return (uint32_t)__builtin_ctz(n);
}

/* Map `n_pages` of contiguous physical memory at the given GPU
 * VA. Helper for the rebuild path — calls into the existing
 * `map_one_page` so it benefits from the auto-allocated
 * intermediate-table code. Returns 0 on success, -1 on PMM
 * exhaustion or invalid range; on partial failure, the pages
 * mapped before the failure stay mapped (the caller treats this
 * as fatal). */
static int rebuild_map_range(uint64_t pdb_phys, uint64_t gpu_va_base,
                             uint64_t phys_base, uint32_t n_pages)
{
    for (uint32_t i = 0; i < n_pages; i++) {
        uint64_t va_i   = gpu_va_base + (uint64_t)i * 4096ull;
        uint64_t phys_i = phys_base   + (uint64_t)i * 4096ull;
        if (map_one_page(pdb_phys, va_i, phys_i, 0) < 0) {
            return -1;
        }
    }
    return 0;
}
/* Discover the GR engine's runlist register base by walking the
 * `top_device_info2` topology table at BAR0+0x22800.
 *
 * Tegra GA10B places the runlist register block at a SoC-specific
 * offset (NOT the dGPU's hardcoded 0xC000), so it must be discovered
 * rather than baked in. Layout per
 * `~/slmos-ref/nvidia/nvgpu-hal-top-top_ga10b_fusa.c:60-114`
 * (`ga10b_top_parse_next_dev`):
 *
 *   - top_device_info_cfg @ BAR0 + 0x224fc
 *       bits[31:20] = num_rows
 *       bits[3:0]   = version (must be 2 = init)
 *   - top_device_info2_r(i) @ BAR0 + 0x22800 + i*4
 *       3 rows per device:
 *         row[0]: type_enum @ [30:24], chain_more @ [31]
 *         row[1]: device_pri_base @ [25:8] << 8, is_engine @ [30]
 *         row[2]: runlist_pri_base @ [25:10] << 10
 *   - Zero rows separate devices (skip those tokens).
 *
 * Returns the GR engine's runlist_pri_base on success, or 0 if the
 * topology table is unparseable / has no GR engine. */
static uint64_t discover_gr_runlist_pri_base(void)
{
    const uint32_t TOP_CFG_OFF        = 0x000224fcu;
    const uint32_t TOP_DEV_INFO2_OFF  = 0x00022800u;
    const uint32_t TYPE_ENUM_GRAPHICS = 0u;

    uint32_t cfg = bar0_read32(TOP_CFG_OFF);
    uint32_t version  = cfg & 0xfu;
    uint32_t num_rows = (cfg >> 20) & 0xfffu;

    if (version != 2u) {
        uart_printf("[rebuild-gmmu] top_device_info_cfg=0x%08x "
                    "version=%u != 2 — can't walk topology; "
                    "CHRAM enable skipped (#844)\n",
                    (unsigned)cfg, (unsigned)version);
        return 0u;
    }

    for (uint32_t i = 0; i + 2u < num_rows; i++) {
        uint32_t row0 = bar0_read32(TOP_DEV_INFO2_OFF + i * 4u);
        if (row0 == 0u) continue;
        uint32_t chain_more = (row0 >> 31) & 0x1u;
        if (chain_more != 1u) continue;
        uint32_t type = (row0 >> 24) & 0x7fu;
        if (type != TYPE_ENUM_GRAPHICS) {
            i += 2;   /* skip device's rows 1 + 2 */
            continue;
        }
        uint32_t row2 = bar0_read32(TOP_DEV_INFO2_OFF + (i + 2u) * 4u);
        uint64_t gr_rl_pri_base =
            (uint64_t)(((row2 >> 10) & 0xffffu)) << 10;
        uart_printf("[rebuild-gmmu] topology: GR at row %u "
                    "row0=0x%08x row2=0x%08x "
                    "runlist_pri_base=0x%lx\n",
                    (unsigned)i, (unsigned)row0,
                    (unsigned)row2,
                    (unsigned long)gr_rl_pri_base);
        return gr_rl_pri_base;
    }
    return 0u;
}

/* Poll `runlist_submit_info` for the pending bit to clear. Returns
 * 1 on completion, 0 on timeout. `max_iter` should be one of the
 * `GA10B_RL_POLL_*_ITERATIONS` constants. */
static int poll_runlist_pending_clear(uint64_t gr_rl_pri_base,
                                      uint32_t max_iter)
{
    uint64_t info_addr =
        GA10B_BAR0_BASE + gr_rl_pri_base + GA10B_RL_REG_SUBMIT_INFO;
    volatile uint32_t *info_reg =
        (volatile uint32_t *)(uintptr_t)info_addr;
    for (uint32_t i = 0; i < max_iter; i++) {
        if ((*info_reg & GA10B_RL_SUBMIT_INFO_PENDING_BIT) == 0u) {
            return 1;
        }
    }
    return 0;
}

/* Dump the first 4 (16-byte) entries of Linux's current runlist for
 * encoding-decode diagnostics. Reads `runlist_submit_base_lo/hi`,
 * adds a kernel mapping for the 2 MB DRAM block holding the runlist
 * (Linux often allocates it in upper-DRAM regions SLM-OS's platform
 * VMM setup doesn't cover), then dumps. Best-effort — failures
 * (out-of-DRAM phys, VMM map error) are logged but ignored. */
static void dump_existing_runlist(uint64_t gr_rl_pri_base)
{
    uint32_t rl_lo = bar0_read32(gr_rl_pri_base +
                                 GA10B_RL_REG_SUBMIT_BASE_LO);
    uint32_t rl_hi = bar0_read32(gr_rl_pri_base +
                                 GA10B_RL_REG_SUBMIT_BASE_HI);
    uint64_t rl_phys =
        ((uint64_t)(rl_lo & 0xfffffc00u)) |
        ((uint64_t)(rl_hi & 0xffu) << 32);
    uart_printf("[rebuild-gmmu] runlist base lo=0x%08x "
                "hi=0x%08x → phys=0x%lx\n",
                (unsigned)rl_lo, (unsigned)rl_hi,
                (unsigned long)rl_phys);

    const uint64_t BLOCK_2M  = 0x200000ull;
    const uint64_t BLOCK_MASK = BLOCK_2M - 1ull;
    uint64_t rl_block = rl_phys & ~BLOCK_MASK;

    /* Defensive bounds: `runlist_submit_base` is Linux-controlled
     * state. If a stale or corrupt register read yields an address
     * outside DRAM (e.g. MMIO range), `vmm_map_region` would install
     * a cacheable Normal-memory mapping over MMIO. */
    if (!phys_in_dram(rl_block, BLOCK_2M)) {
        uart_printf("[rebuild-gmmu] runlist phys=0x%lx outside DRAM "
                    "— refusing to map\n",
                    (unsigned long)rl_phys);
        return;
    }

    int erc = vmm_ensure_kernel_l2_table(rl_block);
    int mrc = -1;
    if (erc == 0) {
        mrc = vmm_map_region(rl_block, rl_block, BLOCK_2M,
                             VMM_FLAG_READ | VMM_FLAG_WRITE);
    }
    uart_printf("[rebuild-gmmu] vmm_ensure_l2(0x%lx) rc=%d, "
                "vmm_map_region(0x%lx, 0x%lx, 2MB) rc=%d\n",
                (unsigned long)rl_block, erc,
                (unsigned long)rl_block,
                (unsigned long)rl_block, mrc);

    if (mrc != 0) return;

    volatile uint32_t *rl =
        (volatile uint32_t *)(uintptr_t)rl_phys;
    uart_printf("[rebuild-gmmu] runlist contents @0x%lx "
                "(first 4 × 16 B entries):\n",
                (unsigned long)rl_phys);
    for (uint32_t e = 0; e < 4u; e++) {
        uart_printf("[rebuild-gmmu]   rl[%u] (offset 0x%x) "
                    "0x%08x 0x%08x 0x%08x 0x%08x\n",
                    (unsigned)e, (unsigned)(e * 16u),
                    (unsigned)rl[e * 4u + 0u],
                    (unsigned)rl[e * 4u + 1u],
                    (unsigned)rl[e * 4u + 2u],
                    (unsigned)rl[e * 4u + 3u]);
    }
}

/* Build a fresh 2-entry runlist (TSG header + one channel entry)
 * pointing at our channel, submit it to the GR engine, and force a
 * preempt so FECS re-reads the new runlist. Returns void — failures
 * are logged; downstream submit will surface its own errors. */
static void build_and_submit_fresh_runlist(uint64_t gr_rl_pri_base,
                                           uint32_t hw_chid,
                                           const struct ga10b_channel_handoff *h)
{
    if (g_rebuild_runlist_page == NULL) {
        g_rebuild_runlist_page = pmm_alloc_page();
    }
    void *new_rl_page = g_rebuild_runlist_page;
    if (new_rl_page == NULL) {
        uart_puts("[rebuild-gmmu] PMM exhausted allocating "
                  "fresh runlist page — #844 work skipped\n");
        return;
    }

    uint64_t new_rl_phys = (uint64_t)(uintptr_t)new_rl_page;
    volatile uint32_t *new_rl =
        (volatile uint32_t *)(uintptr_t)new_rl_phys;
    for (int i = 0; i < 1024; i++) {
        new_rl[i] = 0;
    }

    /* TSG header (#844 entry-decode iterations).
     *
     * Decoded from Linux's runlist that word 0 holds timeslice +
     * length, word 1 is a fixed `0x1` constant (mystery flag —
     * possibly "valid" or "type=tsg"), and word 2 holds the actual
     * tsgid (Linux's two TSGs have word2=0 and word2=1, which
     * contradicts the slmos-ref macros that place tsgid in word 1).
     *
     * Empirical finding: setting word 1 = 0x1 + word 2 =
     * tsg_id_override (matching Linux's format) makes FECS
     * recognise the entry — but the subsequent ctxsw attempt
     * poisons GR (`gr_intr=0xbadf1002` etc.). The poison means
     * FECS *tried* to load our channel but something inside is
     * invalid; before, FECS ignored the entry as malformed and
     * GR stayed clean.
     *
     * Leaving word 1 = 0 + word 2 = tsg_id_override keeps us in
     * the "FECS rejects entry, GR clean" state — a better starting
     * point for the next iteration. The next focus should be on
     * what's invalid inside our channel that crashes GR when FECS
     * loads it: probably engine_wfi or eng_method_buffer fields
     * preserved from Linux pointing at PT pages we may have
     * clobbered, OR an unset subcontext-state field.
     *
     * `TSG_ID_OVERRIDE = 2u` rationale: the runlist DRAM dump just
     * above (`dump_existing_runlist`) shows Linux's existing
     * runlist contains TSGs with IDs 0 and 1. 2 is the next free
     * slot — picking it avoids collision with Linux's active TSGs
     * that may still be referenced by other CHRAM entries we
     * deliberately don't touch. */
    const uint32_t TSG_ID_OVERRIDE = 2u;
    new_rl[0] = (0x80u << 24) | (3u << 16) | (1u << 0);
    new_rl[1] = 0x00000001u;   /* Linux's "valid/type=tsg" flag */
    new_rl[2] = TSG_ID_OVERRIDE & 0xfffu;
    new_rl[3] = 0u;

    /* Channel entry. Type discriminator at bit 11 of word 0:
     * TSG=0, channel=1. Decoded empirically from Linux's runlist
     * (TSG headers have word0 bit 11 = 0; channel entries have
     * bit 11 = 1, e.g. 0x2a925ef0 = chid 0x6f0 + bit 11). This
     * reconciles with `runlist_channel_config_num_channels_log2_-
     * 2k_v() = 11` — CHRAM has 2048 entries, so chid is 11 bits
     * and bit 11 is free for the type discriminator. */
    uint64_t inst_p = h->inst_block_phys;
    uint64_t userd_p = h->userd_phys;
    uint32_t inst_lo20  = (uint32_t)((inst_p >> 12) & 0xfffffu);
    uint32_t inst_hi    = (uint32_t)((inst_p >> 32) & 0xffu);
    uint32_t userd_lo24 = (uint32_t)((userd_p >> 8) & 0xffffffu);
    uint32_t userd_hi   = (uint32_t)(userd_p >> 32);

    new_rl[4] = (hw_chid & 0x7ffu) |
                (1u << 11) |  /* type = channel */
                (inst_lo20 << 12);
    new_rl[5] = inst_hi;
    new_rl[6] = (userd_lo24 << 8) |
                (3u << 6) |   /* userd_target=ncoh */
                (3u << 4) |   /* inst_target=ncoh */
                0xfu;
    new_rl[7] = userd_hi;

    cache_clean_range((void *)(uintptr_t)new_rl_phys, 4096);
    __asm__ volatile("dsb sy" ::: "memory");

    uart_printf("[rebuild-gmmu] fresh runlist @0x%lx:\n"
                "[rebuild-gmmu]   [TSG] 0x%08x 0x%08x "
                "0x%08x 0x%08x\n"
                "[rebuild-gmmu]   [CHN] 0x%08x 0x%08x "
                "0x%08x 0x%08x\n",
                (unsigned long)new_rl_phys,
                (unsigned)new_rl[0], (unsigned)new_rl[1],
                (unsigned)new_rl[2], (unsigned)new_rl[3],
                (unsigned)new_rl[4], (unsigned)new_rl[5],
                (unsigned)new_rl[6], (unsigned)new_rl[7]);

    /* Disable scheduling, update submit_base, re-enable, submit. */
    bar0_write32(gr_rl_pri_base + GA10B_RL_REG_SCHED_DISABLE, 1u);
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t base_lo =
        (uint32_t)((new_rl_phys & 0xfffffc00u)) | 3u;
    uint32_t base_hi =
        (uint32_t)((new_rl_phys >> 32) & 0xffu);
    bar0_write32(gr_rl_pri_base + GA10B_RL_REG_SUBMIT_BASE_LO, base_lo);
    bar0_write32(gr_rl_pri_base + GA10B_RL_REG_SUBMIT_BASE_HI, base_hi);
    __asm__ volatile("dsb sy" ::: "memory");

    bar0_write32(gr_rl_pri_base + GA10B_RL_REG_SCHED_DISABLE, 0u);
    __asm__ volatile("dsb sy" ::: "memory");

    /* Submit: length = 2, offset = 0. */
    bar0_write32(gr_rl_pri_base + GA10B_RL_REG_SUBMIT,
                 (0u << 16) | 2u);
    __asm__ volatile("dsb sy" ::: "memory");

    int submit_done = poll_runlist_pending_clear(
        gr_rl_pri_base, GA10B_RL_POLL_SUBMIT_ITERATIONS);
    uint32_t info_after = bar0_read32(gr_rl_pri_base +
                                      GA10B_RL_REG_SUBMIT_INFO);
    uart_printf("[rebuild-gmmu] fresh runlist submit %s "
                "(submit_base lo=0x%08x hi=0x%08x, info=0x%08x)\n",
                submit_done ? "completed" : "TIMEOUT",
                (unsigned)base_lo, (unsigned)base_hi,
                (unsigned)info_after);

    /* Force a fresh ctxsw to pick our channel now. */
    bar0_write32(gr_rl_pri_base + GA10B_RL_REG_PREEMPT, 0x00200000u);
    __asm__ volatile("dsb sy" ::: "memory");

    int preempt2 = poll_runlist_pending_clear(
        gr_rl_pri_base, GA10B_RL_POLL_PREEMPT_ITERATIONS);
    info_after = bar0_read32(gr_rl_pri_base + GA10B_RL_REG_SUBMIT_INFO);
    uart_printf("[rebuild-gmmu] post-submit preempt %s "
                "(info=0x%08x)\n",
                preempt2 ? "completed" : "TIMEOUT",
                (unsigned)info_after);
}

/* Enable our channel in CHRAM (the per-engine 2048-entry channel
 * descriptor array indexed by hw_chid) and force a context reload
 * so FECS picks up our state. Then preempt the runlist so FECS
 * unloads whatever Linux channel is currently bound. */
static void chram_enable_and_preempt(uint64_t gr_rl_pri_base,
                                     uint32_t hw_chid)
{
    uint32_t channel_config = bar0_read32(gr_rl_pri_base +
                                          GA10B_RL_REG_CHANNEL_CONFIG);
    uint32_t chram_bar0_offset =
        ((channel_config >> 4) & 0x0fffffffu) << 4;
    uint64_t chram_chan_addr =
        GA10B_BAR0_BASE + (uint64_t)chram_bar0_offset +
        (uint64_t)hw_chid * 4ull;
    volatile uint32_t *chram =
        (volatile uint32_t *)(uintptr_t)chram_chan_addr;

    /* enable_channel (0x2) then force_ctx_reload (0x200).
     * The update field accepts one action per write; do two
     * writes in sequence with a dsb between, matching nvgpu's
     * bind sequence. */
    *chram = 0x00000002u;   /* enable_channel */
    __asm__ volatile("dsb sy" ::: "memory");
    *chram = 0x00000200u;   /* force_ctx_reload */
    __asm__ volatile("dsb sy" ::: "memory");
    uart_printf("[rebuild-gmmu] CHRAM[hw_chid=%u]@0x%lx enabled "
                "+ force_ctx_reload (chram_bar0=0x%x from "
                "channel_config=0x%08x)\n",
                (unsigned)hw_chid,
                (unsigned long)chram_chan_addr,
                (unsigned)chram_bar0_offset,
                (unsigned)channel_config);

    /* #844 candidate (3) — REVERTED: clearing all non-ours CHRAM
     * entries (2047 slots × 0xFFFFFFFF = update_clear_channel_v)
     * is too destructive. Linux channels (Xorg display, kworker,
     * etc.) are still actively bound to GR via these CHRAM
     * entries; the clear triggers an immediate GR teardown / power-
     * gate (every register reads 0xbadf1002 poison post-clear).
     * The "lock FECS to only our channel" idea needs a less
     * destructive approach — see #844 for the next candidates. */

    /* Force a runlist preempt so FECS unloads the currently-running
     * Linux channel and re-reads the runlist. Without this, FECS
     * may stay loaded with the Linux channel for the lifetime of
     * SLM-OS — our enable + reload bits become "pending" but never
     * get acted on. Register at gr_rl_pri_base + 0x98; value =
     * runlist_preempt_runlist_preempt_pending_true_f (0x200000) |
     * runlist_preempt_type_runlist_f (0x0). */
    bar0_write32(gr_rl_pri_base + GA10B_RL_REG_PREEMPT, 0x00200000u);
    __asm__ volatile("dsb sy" ::: "memory");
    uart_printf("[rebuild-gmmu] runlist preempt (gr_rl_pri_base=0x%lx + "
                "0x%x) = 0x00200000 (force FECS to re-read runlist)\n",
                (unsigned long)gr_rl_pri_base,
                (unsigned)GA10B_RL_REG_PREEMPT);

    int preempt_done = poll_runlist_pending_clear(
        gr_rl_pri_base, GA10B_RL_POLL_PREEMPT_ITERATIONS);
    uint32_t info_after = bar0_read32(gr_rl_pri_base +
                                      GA10B_RL_REG_SUBMIT_INFO);
    uart_printf("[rebuild-gmmu] preempt %s (info=0x%08x after poll)\n",
                preempt_done ? "completed" : "TIMEOUT",
                (unsigned)info_after);
}

static void install_fresh_runlist_and_chram(
                                const struct ga10b_channel_handoff *h)
{
    uint64_t gr_rl_pri_base = discover_gr_runlist_pri_base();
    if (gr_rl_pri_base == 0u) {
        uart_puts("[rebuild-gmmu] couldn't find GR engine in "
                  "topology table — CHRAM enable skipped (#844)\n");
        return;
    }

    /* hw_chid is the doorbell-namespace channel id. nvgpu's
     * `gv11b_usermode_doorbell_token` formula sets
     *     token = channel_base + chid
     * which IS hw_chid (the CHRAM and runlist-entry chid field both
     * live in the doorbell namespace). So `hw_chid` is just
     * `h->work_submit_token` — adding `h->channel_id` again would
     * double-count it. The prior `token + chid` formula happened to
     * give the right answer only because the helper always
     * publishes chid=0 today. */
    const uint32_t hw_chid = h->work_submit_token;

    chram_enable_and_preempt(gr_rl_pri_base, hw_chid);

    /* Tegra Orin Nano doesn't use the SMMU for the GPU stream (per
     * the comment at ga10b_gmmu.h:359 — no `iommus` DT property on
     * the GA10B node), so `runlist_submit_base` holds a CPU phys
     * verbatim. The dump path adds a kernel mapping for the 2 MB
     * DRAM block holding Linux's runlist (Linux often allocates it
     * in upper-DRAM regions SLM-OS's platform VMM setup doesn't
     * cover). */
    dump_existing_runlist(gr_rl_pri_base);

    /* #844 — write a fresh runlist containing ONLY our channel and
     * submit it. Linux's existing runlist references chids that
     * aren't ours; FECS schedules Linux channels regardless of our
     * CHRAM/preempt writes because our channel was never on its
     * runlist. */
    build_and_submit_fresh_runlist(gr_rl_pri_base, hw_chid, h);
}


int ga10b_gmmu_rebuild_for_handoff(uint64_t inst_block_phys,
                                   const struct ga10b_channel_handoff *h)
{
    if (h == NULL) {
        return -1;
    }
    if (!phys_in_dram(inst_block_phys, 4096)) {
        return -1;
    }

    /* #844 chid override REVERTED. Tried chid=1 with token =
     * channel_base(0x1fc) + 1 = 0x1fd; produced GR-poison state
     * (every register reads 0xbadf1002) consistently across two
     * runs. Reason unclear — chid=1 might collide with a CHRAM
     * slot Linux still depends on, or the
     * `token = channel_base + chid` encoding may have extra bits
     * we haven't decoded. Reverting to chid=0 (helper's value)
     * keeps GR in the "clean but PBDMA-doesn't-see-submits"
     * state, which is a better starting point for the runlist-
     * entry decode work below. */

    /* #788 Stage 6: trace every PMM page the rebuild allocates so
     * the operator can spot which one might be colliding with an
     * unknown FW range. Tail of the function disables tracing
     * once mapping is done. Currently OFF — the 4000+-line trace
     * is overwhelming when not specifically investigating.
     * Toggle on for diagnostic captures. */
    /* ga10b_gmmu_table_alloc_trace_set(true); */

    /* #834 diagnostic — dump the inst block BEFORE we touch it, so
     * the post-rebuild dump (at function exit) can be diffed against
     * Linux's published state. Reveals which fields the rebuild
     * actually changes vs leaves as Linux-published. Especially
     * relevant when GR poisons on ctxsw and we need to identify
     * the LAST inst-block field that's still stale.
     *
     * Gated behind `gpu debug on` (`ga10b_dispatch_verbose_get`)
     * because the dump is 256 lines of serial output — fine for
     * targeted investigation, too noisy for steady-state oplib
     * staging. */
    if (ga10b_dispatch_verbose_get()) {
        ga10b_dump_inst_block_at("REBUILD-BEFORE", inst_block_phys);
    }

    /* 1. Allocate a fresh PDB page from SLM-OS PMM. Zero it so
     *    every PDE3 entry reads as invalid until `map_one_page`
     *    populates the ones it needs. */
    void *pdb_page = alloc_zero_table_page();
    if (pdb_page == NULL) {
        uart_puts("[rebuild-gmmu] PMM exhausted allocating PDB\n");
        return -1;
    }
    uint64_t pdb_phys = (uint64_t)(uintptr_t)pdb_page;
    uart_printf("[rebuild-gmmu] fresh PDB at phys=0x%lx\n",
                (unsigned long)pdb_phys);

    /* 2. PRESERVE the inst block's existing contents — only override
     *    the specific words SLM-OS needs to control. The original
     *    "zero everything then write what we know" approach worked
     *    for SLM-OS-allocated inst blocks (where everything started
     *    at zero anyway), but is fatal when applied to a Linux-nvgpu-
     *    allocated inst block: zeroing wipes out
     *      - engine_fw_magic_value (word 131 = 0xcafeca11)
     *        — FECS rejects the channel without this on ctxsw, which
     *          produces the 0xbadf1002 GR register poison observed
     *          during the #834 fix-A attempt.
     *      - subcontext PDB pointers (words 168+, one per VEID)
     *        — GR uses the subcontext indexed by VEID when running
     *          compute; missing pointer = GR can't find page tables.
     *      - engine_wfi_target / _ptr_hi / eng_method_buffer (132-137)
     *        — Linux sets these per its channel config; we don't
     *          have the values to regenerate them.
     *
     *    The strategy is layered:
     *      - Skip the destructive 1024-word zero-pass (the original
     *        bug — #839 / #842).
     *      - Explicitly zero only the slices we have positive
     *        evidence are stale or unneeded post-kexec: the RAMFC
     *        region (words 0..127, see the next block) and the
     *        non-target subcontext PDB slots (words 168..423 except
     *        the helper's VEID slot, see the subcontext write block
     *        further below).
     *      - Preserve everything else Linux set up.
     *      - Override the fields SLM-OS needs to control
     *        deterministically (PDB pointer at 128/129, engine_fw_
     *        magic at 131, subctx-VEID PDB at 168+4*HELPER_SUBCTX_ID,
     *        valid-long bit at 166, RAMFC fields).
     *
     *    The "explicitly zero only what we know" rule means a future
     *    reader sees both an aggressive RAMFC clear AND a "preserve
     *    Linux's setup" framing here — they're not in conflict: the
     *    cleared slices are bounded and named at their respective
     *    write sites.
     *
     *    For SLM-OS-allocated inst blocks (from `alloc_zero_table_
     *    page`) every field starts at zero, so the rebuild's net
     *    effect is just our explicit writes — same as before for
     *    fields like `eng_method_buffer_addr` (we zero those too,
     *    further below). For Linux-allocated inst blocks the same
     *    rebuild preserves Linux's class binding etc. while
     *    overriding what we need.
     *
     *    #839: the proximate fix for #834 fix-A is the explicit
     *    word-131 magic + subcontext-PDB write below. */
    volatile uint32_t *inst = (volatile uint32_t *)(uintptr_t)inst_block_phys;

    /* #834 experiment: zero ALL of the RAMFC region (words 0..127)
     * before writing our subset.
     *
     * The inst-block diff dump (REBUILD-BEFORE vs REBUILD-AFTER on
     * 2026-05-16) shows Linux leaves several preserved-by-us fields
     * non-zero: pseudorandom-looking values at words 11, 29, 38 and
     * a VA-encoded pair at words 23-24, plus a bit-31-set value at
     * word 27. Some of those plausibly hold stale FECS sequence
     * numbers / checksums / pointers that contribute to the
     * gr_intr=0xbadf1002 poison during ctxsw-in.
     *
     * Strategy: wipe RAMFC, write exactly the fields we know about,
     * leave the rest at zero. Two outcomes both informative:
     *   - GR poison disappears → one of those preserved fields was
     *     the trigger; binary-search to identify which.
     *   - GR poison shifts to a different register pattern → FECS
     *     needed a non-zero value somewhere we just cleared; the
     *     new error code tells us which.
     *   - GR poison identical → RAMFC isn't the source; look at GR-
     *     engine-internal state (gr_ctx, golden image), most likely
     *     fixable only by getting v10 gr_ctx-extents publishing
     *     working on the helper side.
     *
     * Range chosen as 0..127 (i.e. 512 B) which covers all of RAMFC
     * proper (0..63 by GA10B_RAMFC_W_* constants) plus a margin past
     * it. Words 128/129 (PDB lo/hi) and 131 (engine_fw_magic) get
     * re-written immediately below; words 130 + 134-137 + 166-167
     * + 168-423 are all explicitly handled by the existing code. */
    for (uint32_t i = 0; i < 128u; i++) {
        inst[i] = 0u;
    }

    /* Encode PDB-lo word: aperture (sys_mem_ncoh on Tegra),
     * volatile bit, ver2 page-table format, 64 KB big-page size,
     * and phys[31:12] in bits [31:12] (the bottom 12 bits of phys
     * are zero by page alignment). */
    uint32_t pdb_lo_word =
        GA10B_RAM_IN_PDB_TARGET_SYS_NCOH |
        GA10B_RAM_IN_PDB_VOL_TRUE |
        GA10B_RAM_IN_USE_VER2_PT_FORMAT |
        GA10B_RAM_IN_BIG_PAGE_SIZE_64KB |
        (uint32_t)(pdb_phys & 0xfffff000u);
    uint32_t pdb_hi_word = (uint32_t)(pdb_phys >> 32);

    inst[128] = pdb_lo_word;
    inst[129] = pdb_hi_word;

    /* engine_fw_magic_value at word 131 — FECS firmware checks this
     * to accept the channel during ctxsw. Idempotent: when the inst
     * block was Linux-allocated, this is already set to the same
     * value; we re-write defensively for the SLM-OS-allocated case
     * (where the inst block started zero). */
    inst[GA10B_RAMIN_W_ENGINE_FW_MAGIC] = GA10B_RAMIN_VAL_ENGINE_FW_MAGIC;

    /* Subcontext-VEID PDB pointer + valid-long bitmap.
     *
     * GR loads the subcontext indexed by VEID when running compute
     * and dereferences *that* subcontext's pdb pointer (NOT the
     * main pdb). Each VEID 0..63 has a 4-word slot starting at
     * `168 + 4*veid`; the valid-long bitmap (1 bit per veid) lives
     * in words 166 (veids 0..31) and 167 (veids 32..63).
     *
     * #844: previously we ONLY rewrote the slot for our VEID (= 1)
     * and OR-ed in bit 1 of the valid-long lo word, preserving
     * Linux's other subcontext state. That left two stale-pointer
     * hazards in the inst block:
     *
     *   1. Per-VEID PDB slots for veids ≠ 1: Linux may have
     *      configured multiple subcontexts (e.g. veid 0 = SYNC) with
     *      PDB pointers into pages SLM-OS's PMM has since re-used.
     *      When FECS ctxsws to our channel it can iterate the
     *      subcontext valid mask and try to load the secondary
     *      PDBs alongside ours; a dereference of a clobbered page
     *      poisons GR (0xbadf1002).
     *
     *   2. valid-long bits for veids ≠ 1: even if a slot's pointer
     *      is zero, the corresponding valid bit being set tells GR
     *      "this subcontext is configured", and the load happens.
     *
     * Fix: zero ALL 64 subcontext slots (256 words = inst[168..423])
     * and BOTH valid-long words; then write our VEID=1 slot fresh
     * and set just bit 1 of the lo valid-long word. Now only the
     * subcontext we control is visible to GR. */
    for (uint32_t veid = 0; veid < 64u; veid++) {
        uint32_t w = GA10B_RAMIN_W_SC_PDB_BASE(veid);
        inst[w + 0] = 0u;
        inst[w + 1] = 0u;
        inst[w + 2] = 0u;
        inst[w + 3] = 0u;
    }
    inst[GA10B_RAMIN_W_SC_PDB_VALID_LONG_LO] = 0u;
    inst[GA10B_RAMIN_W_SC_PDB_VALID_LONG_HI] = 0u;

    uint32_t sc_base = GA10B_RAMIN_W_SC_PDB_BASE(GA10B_HELPER_SUBCTX_ID);
    inst[sc_base + 0] = pdb_lo_word;
    inst[sc_base + 1] = pdb_hi_word;
    inst[GA10B_RAMIN_W_SC_PDB_VALID_LONG_LO] =
        (1u << GA10B_HELPER_SUBCTX_ID);

    /* 2b. Populate RAMFC fields PBDMA reads when fetching submits
     *    for this channel. Without these, PBDMA finds gpfifo_phys
     *    = 0 in the inst block and silently does nothing — the
     *    Stage 2 PR's known limitation. Mirrors
     *    `ga10b_ramfc_setup` from
     *    `~/slmos-ref/nvidia/nvgpu-hal-fifo-ramfc_ga10b_fusa.c`,
     *    with the PBDMA-HAL-computed values inlined as constants
     *    above (the HAL chain dispatches through 4 chip-version
     *    HAL ops to produce them on Linux; we replicate the
     *    Tegra GA10B output values directly). */

    /* `gp_base_offset_f` shifts the gpfifo phys's bits[31:3] into
     * word bits[31:3]. We just need to mask off the bottom 3 bits;
     * gpfifo phys is page-aligned (4 KB) so the bottom 12 bits are
     * already zero, but the mask keeps the encoding explicit. */
    uint32_t gp_base_lo_val =
        (uint32_t)(h->gpfifo_phys & 0xfffffff8u);
    /* `gp_base_hi` carries phys bits[39:32] in bits[7:0], and the
     * gpfifo size in entries-log2 at bits[20:16]. Tegra GA10B
     * gpfifo phys is 40-bit so the high byte is at most 0xFF. */
    uint32_t gp_base_hi_val =
        ((uint32_t)(h->gpfifo_phys >> 32) & 0xffu) |
        (log2_pow2(h->gpfifo_entries) << 16);

    inst[GA10B_RAMFC_W_GP_BASE]       = gp_base_lo_val;
    inst[GA10B_RAMFC_W_GP_BASE_HI]    = gp_base_hi_val;
    inst[GA10B_RAMFC_W_SIGNATURE]     = GA10B_RAMFC_VAL_SIGNATURE;
    inst[GA10B_RAMFC_W_PB_HEADER]     = GA10B_RAMFC_VAL_PB_HEADER;
    inst[GA10B_RAMFC_W_SUBDEVICE]     = GA10B_RAMFC_VAL_SUBDEVICE;
    inst[GA10B_RAMFC_W_TARGET]        = GA10B_RAMFC_VAL_TARGET_GR;
    inst[GA10B_RAMFC_W_ACQUIRE]       = GA10B_RAMFC_VAL_ACQUIRE_LONG;
    inst[GA10B_RAMFC_W_INTR_NOTIFY]   = GA10B_RAMFC_VAL_INTR_NOTIFY;
    inst[GA10B_RAMFC_W_CONFIG]        = GA10B_RAMFC_VAL_CONFIG_USERD_WB;
    /* #844: encode SET_CHANNEL_INFO with hw_chid (the doorbell-
     * namespace channel id) rather than the per-runlist chid. FECS
     * validates RAMFC's chid against the runlist entry's chid
     * (= hw_chid in our encoding); a mismatch crashes GR during
     * ctxsw with the same 0xbadf1002 poison signature we see when
     * the internal doorbell is rung. `hw_chid = h->work_submit_token`
     * directly per the nvgpu gv11b doorbell-token formula
     * (token = channel_base + chid = hw_chid). */
    const uint32_t ramfc_hw_chid = h->work_submit_token;
    inst[GA10B_RAMFC_W_SET_CHANNEL_INFO] =
        GA10B_RAMFC_SET_CHANNEL_INFO(ramfc_hw_chid,
                                      GA10B_HELPER_SUBCTX_ID);

    /* engine_wfi block (#844 — clear stale Linux pointers).
     * Words 132 (target + ptr_lo) and 133 (ptr_hi) point at the
     * GR ctxsw save buffer Linux nvgpu allocated. Post-kexec
     * SLM-OS's PMM may have re-allocated those pages, so the
     * pointer is now dangling. When FECS tries to ctxsw to our
     * channel, it dereferences `engine_wfi_ptr` to save the
     * outgoing context — that load/store against clobbered
     * memory is the likely cause of the `gr_intr=0xbadf1002`
     * poison we see when word 1 of the TSG header is set to
     * 0x1 (FECS load attempt).
     *
     * Zero target + ptr_hi so nvgpu's "lazy-allocate the
     * ctxsw save buffer on first engagement" path kicks in.
     * Word 134 (veid) keeps the helper's subctx (= 1). */
    inst[GA10B_RAMIN_W_ENGINE_WFI_TARGET_LO] = 0u;
    inst[GA10B_RAMIN_W_ENGINE_WFI_PTR_HI]    = 0u;
    inst[GA10B_RAMIN_W_ENGINE_WFI_VEID]      = GA10B_HELPER_SUBCTX_ID;
    /* Also clear eng_method_buffer_addr. Same staleness story as
     * engine_wfi: Linux pointed these at a method-buffer DMA
     * allocation that SLM-OS PMM may have re-used. nvgpu lazy-
     * allocates when zero. */
    inst[GA10B_RAMIN_W_ENG_METHOD_BUF_LO] = 0u;
    inst[GA10B_RAMIN_W_ENG_METHOD_BUF_HI] = 0u;

    cache_clean_range((void *)(uintptr_t)inst_block_phys, 4096);

    uart_printf("[rebuild-gmmu] inst_block@0x%lx written "
                "(non-target subcontexts zeroed; only VEID=%u active):\n",
                (unsigned long)inst_block_phys,
                (unsigned)GA10B_HELPER_SUBCTX_ID);
    uart_printf("[rebuild-gmmu]   PDB_lo=0x%08x PDB_hi=0x%08x "
                "(main + subcontext VEID=%u at word %u)\n",
                (unsigned)pdb_lo_word, (unsigned)pdb_hi_word,
                (unsigned)GA10B_HELPER_SUBCTX_ID, (unsigned)sc_base);
    uart_printf("[rebuild-gmmu]   engine_fw_magic=0x%08x "
                "sc_valid_long=lo:0x%08x/hi:0x%08x\n",
                (unsigned)inst[GA10B_RAMIN_W_ENGINE_FW_MAGIC],
                (unsigned)inst[GA10B_RAMIN_W_SC_PDB_VALID_LONG_LO],
                (unsigned)inst[GA10B_RAMIN_W_SC_PDB_VALID_LONG_HI]);
    uart_printf("[rebuild-gmmu]   gp_base=0x%08x gp_base_hi=0x%08x "
                "(entries=%u → log2=%u)\n",
                (unsigned)gp_base_lo_val, (unsigned)gp_base_hi_val,
                (unsigned)h->gpfifo_entries,
                (unsigned)log2_pow2(h->gpfifo_entries));
    uart_printf("[rebuild-gmmu]   signature=0x%08x pb_header=0x%08x "
                "subdevice=0x%08x target=0x%08x\n",
                GA10B_RAMFC_VAL_SIGNATURE, GA10B_RAMFC_VAL_PB_HEADER,
                GA10B_RAMFC_VAL_SUBDEVICE, GA10B_RAMFC_VAL_TARGET_GR);
    uart_printf("[rebuild-gmmu]   acquire=0x%08x intr_notify=0x%08x "
                "config=0x%08x set_channel_info=0x%08x veid=%u\n",
                GA10B_RAMFC_VAL_ACQUIRE_LONG,
                GA10B_RAMFC_VAL_INTR_NOTIFY,
                GA10B_RAMFC_VAL_CONFIG_USERD_WB,
                GA10B_RAMFC_SET_CHANNEL_INFO(ramfc_hw_chid,
                                              GA10B_HELPER_SUBCTX_ID),
                (unsigned)GA10B_HELPER_SUBCTX_ID);

    /* 3. Map each preserved buffer at its original GPU VA. The
     *    handoff publishes (phys, gpu_va, size) for every dmabuf
     *    the helper allocated; we replay those mappings into the
     *    fresh PDB tree.
     *
     *    USERD and GPFIFO are accessed by PBDMA via physical
     *    addresses (not GPU VAs), so they don't need GMMU
     *    mappings — skip them.
     *
     *    Buffers that DO need GMMU mappings (Pushbuffer +
     *    Semaphore + SASS pool + cbuf + QMD pool + weights pool
     *    first-page) are mapped below. Anything with size=0 or
     *    gpu_va=0 is skipped — that means the helper didn't
     *    allocate that buffer for this channel. */
    struct rebuild_buf {
        const char *tag;
        uint64_t phys;
        uint64_t gpu_va;
        uint64_t size_bytes;
    };

    struct rebuild_buf bufs[] = {
        { "pushbuf", h->pushbuf_phys, h->pushbuf_gpu_va,
          (uint64_t)h->pushbuf_size },
        { "sem", h->semaphore_phys, h->semaphore_gpu_va, 4096 },
        { "shader", h->shader_phys, h->shader_gpu_va,
          (uint64_t)h->shader_size },
        { "cbuf", h->cbuf_phys, h->cbuf_gpu_va,
          (uint64_t)h->cbuf_size },
        { "qmd_pool", h->qmd_pool_phys, h->qmd_pool_gpu_va,
          (uint64_t)h->qmd_pool_size_bytes },
        /* Weights pool: when the helper publishes a v9 extents
         * list, we walk it below (after this fixed-list loop)
         * and map every extent. The single-page first-run entry
         * stays here as a fallback for v8 helpers — SLM-OS sees
         * `weights_n_extents == 0` and maps only this 4 KB run,
         * mirroring the Stage 3 behaviour. v9 helpers leave
         * `weights_pool_phys` populated (matches the first
         * extent's phys) so the entry is harmlessly redundant
         * with extent[0] — the GMMU's PTE write is idempotent
         * since both writes encode the same `phys → gpu_va`
         * mapping for the first page. */
        { "weights_first", h->weights_pool_phys, h->weights_pool_gpu_va,
          (h->weights_pool_size_bytes != 0) ? 4096ull : 0ull },
    };

    int mapped = 0;
    for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
        const struct rebuild_buf *b = &bufs[i];
        if (b->size_bytes == 0 || b->phys == 0 || b->gpu_va == 0) {
            uart_printf("[rebuild-gmmu]  skip %-14s (not allocated)\n",
                        b->tag);
            continue;
        }
        uint32_t n_pages = (uint32_t)((b->size_bytes + 4095u) / 4096u);
        if (rebuild_map_range(pdb_phys, b->gpu_va, b->phys,
                               n_pages) < 0) {
            uart_printf("[rebuild-gmmu]  FAIL %-14s gpu_va=0x%lx "
                        "phys=0x%lx pages=%u\n",
                        b->tag, (unsigned long)b->gpu_va,
                        (unsigned long)b->phys, (unsigned)n_pages);
            return -1;
        }
        uart_printf("[rebuild-gmmu]  ok   %-14s gpu_va=0x%lx "
                    "phys=0x%lx pages=%u\n",
                    b->tag, (unsigned long)b->gpu_va,
                    (unsigned long)b->phys, (unsigned)n_pages);
        mapped++;
    }

    /* v9 weights-pool extents — the helper walks
     * /proc/self/pagemap over the 1.5 GB IOVMM-stitched pool
     * pre-kexec and publishes a list of (phys, n_pages) extents
     * in a separate dmabuf. Walk them and install PTEs covering
     * the entire pool. Without this, only the first 4 KB above
     * is mapped and CE memcpy writes past that fault.
     *
     * The cursor advances by `n_pages * 4 KB` per extent because
     * the pool is IO-virtually contiguous at `weights_pool_gpu_va`
     * — the SMMU stitches scattered phys into one IOVA range —
     * but the underlying physical pages aren't contiguous. Each
     * extent maps a contiguous physical run to its corresponding
     * stretch of GPU VA. */
    if (h->version >= 9u && h->weights_n_extents > 0u &&
        h->weights_extents_phys != 0u &&
        h->weights_pool_gpu_va != 0u) {
        if (!phys_in_dram(h->weights_extents_phys,
                          (size_t)h->weights_n_extents *
                          sizeof(struct ga10b_phys_extent))) {
            uart_printf("[rebuild-gmmu] weights_extents_phys=0x%lx "
                        "(n=%u) outside DRAM — skipping extent map\n",
                        (unsigned long)h->weights_extents_phys,
                        (unsigned)h->weights_n_extents);
        } else {
            const struct ga10b_phys_extent *exts =
                (const struct ga10b_phys_extent *)
                (uintptr_t)h->weights_extents_phys;
            uint64_t cursor_va = h->weights_pool_gpu_va;
            uint32_t total_pages_mapped = 0;
            uint32_t failed_at = 0;
            bool extent_fail = false;
            for (uint32_t i = 0; i < h->weights_n_extents; i++) {
                if (exts[i].n_pages == 0u) {
                    continue;
                }
                if (rebuild_map_range(pdb_phys, cursor_va,
                                       exts[i].phys,
                                       exts[i].n_pages) < 0) {
                    failed_at = i;
                    extent_fail = true;
                    break;
                }
                cursor_va += (uint64_t)exts[i].n_pages * 4096ull;
                total_pages_mapped += exts[i].n_pages;
            }
            if (extent_fail) {
                uart_printf("[rebuild-gmmu] weights extents FAIL at "
                            "index %u of %u (mapped %u pages before "
                            "failure)\n",
                            (unsigned)failed_at,
                            (unsigned)h->weights_n_extents,
                            (unsigned)total_pages_mapped);
                return -1;
            }
            uart_printf("[rebuild-gmmu]  ok   weights_extents  "
                        "n=%u total_pages=%u (~%u MB) end_va=0x%lx\n",
                        (unsigned)h->weights_n_extents,
                        (unsigned)total_pages_mapped,
                        (unsigned)(total_pages_mapped / 256u),
                        (unsigned long)cursor_va);
            mapped++;
        }
    }

    /* Amortized dsb sy so all PTE writes are visible to the GPU
     * before TLB invalidate fires (matches the `dsb sy` at the
     * tail of `ga10b_gmmu_alloc` for the same reason). */
    __asm__ volatile("dsb sy" ::: "memory");

    /* 4. Flush any TLB entries the GPU might have cached against
     *    the old PDB-tree state. `ga10b_gmmu_tlb_invalidate`
     *    targets the new PDB phys so the next walk loads fresh
     *    PTEs from the tree we just built. */
    int rc = ga10b_gmmu_tlb_invalidate(pdb_phys);
    uart_printf("[rebuild-gmmu] TLB invalidate rc=%d, %d buffer(s) "
                "mapped (%d PT pages allocated)\n",
                rc, mapped, g_table_alloc_count);

    install_fresh_runlist_and_chram(h);

    ga10b_gmmu_table_alloc_trace_set(false);

    /* #834 diagnostic — dump the rebuilt inst block AFTER all writes
     * (PDB pointers + RAMFC + engine_wfi/eng_method_buffer clears +
     * subcontext zero-and-rewrite + CHRAM/runlist housekeeping), so a
     * post-mortem can compare against Linux's pre-rebuild state. The
     * GR-poison-on-ctxsw hypothesis is that *some* field FECS reads
     * during context-switch-IN is still pointing at clobbered Linux
     * pages. With the dump in hand, anything that looks like a
     * pointer (non-zero high bits, page-aligned, in DRAM range) can
     * be walked separately to see whether it's still valid.
     *
     * Same `gpu debug on` gate as the BEFORE dump. */
    if (ga10b_dispatch_verbose_get()) {
        ga10b_dump_inst_block_at("REBUILD-AFTER", inst_block_phys);
    }

    return (rc == 0) ? 0 : -1;
}

/* Try to satisfy an allocation of `n_pages` from the free-extent
 * tracker (exact-fit). Returns the freed VA on success, removing
 * its slot; returns 0 on miss (caller falls through to the bump
 * cursor). */
static uint64_t pop_free_extent(uint32_t n_pages)
{
    for (uint32_t i = 0; i < g_free_extent_count; i++) {
        if (g_free_extents[i].n_pages == n_pages) {
            uint64_t va = g_free_extents[i].gpu_va;
            /* Remove slot by swapping with the tail. Order in the
             * tracker is otherwise irrelevant. */
            g_free_extent_count--;
            if (i != g_free_extent_count) {
                g_free_extents[i] = g_free_extents[g_free_extent_count];
            }
            return va;
        }
    }
    return 0;
}
