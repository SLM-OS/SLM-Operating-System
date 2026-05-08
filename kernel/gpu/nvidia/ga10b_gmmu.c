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
#include "../../include/timer.h"

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

/* Jetson carves OP-TEE out of low DRAM at 0xBE000000-0xC2000000.
 * The VMM doesn't map that 64 MB hole, so a phys_read inside it is
 * a synchronous abort. Reject it here so the walker treats wild PDB
 * pointers landing in the hole as "invalid PDE" instead of crashing.
 * Same applies to the IMX219 frame-buffer carveout at
 * 0xA1000000-0xA1400000 (camrtc.c:CAMRTC_FRAME_BUFFER_PHYS) and the
 * NC-memory page at 0xBDE00000+2 MB the kernel reserves for cross-
 * CPU coherent state. The latter two are platform-narrower than the
 * OP-TEE hole; checking the hole alone covers most failure modes
 * since wild PDB pointers tend to land in the largest unmapped
 * region. Refine further if a candidate's walk faults inside one
 * of the smaller carveouts. */
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

    /* Zero the result struct so partial walks have well-defined
     * fields. Use a small loop because the kernel's freestanding
     * environment doesn't have memset declared in this TU. */
    {
        uint8_t *p = (uint8_t *)result;
        for (size_t i = 0; i < sizeof(*result); i++) p[i] = 0;
    }
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
    } level_bits[5] = {
        {GA10B_PDE3_VA_HI, GA10B_PDE3_VA_LO},
        {GA10B_PDE2_VA_HI, GA10B_PDE2_VA_LO},
        {GA10B_PDE1_VA_HI, GA10B_PDE1_VA_LO},
        {GA10B_PDE0_VA_HI, GA10B_PDE0_VA_LO},
        {GA10B_PTE_VA_HI,  GA10B_PTE_VA_LO},
    };

    uint64_t cur_table_phys = pdb_phys;

    for (int lvl = 0; lvl < 5; lvl++) {
        uint16_t idx = va_index(gpu_va, level_bits[lvl].hi, level_bits[lvl].lo);
        uint64_t entry_phys = cur_table_phys + (uint64_t)idx * GA10B_GMMU_ENTRY_SIZE;
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
            decode_dual_pde_small(entry, &next_phys, &aperture, &valid);
            rec->dual_pde_followed_small = true;
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
static void *alloc_zero_table_page(void)
{
    void *p = pmm_alloc_page();
    if (p == NULL) return NULL;
    volatile uint64_t *q = (volatile uint64_t *)p;
    for (int i = 0; i < 512; i++) {
        q[i] = 0;
    }
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
 * a batch of map_one_page calls so the cost amortizes. */
static int map_one_page(uint64_t pdb_phys,
                        uint64_t gpu_va,
                        uint64_t data_phys,
                        uint32_t flags)
{
    uint16_t i3 = va_index(gpu_va, GA10B_PDE3_VA_HI, GA10B_PDE3_VA_LO);
    uint16_t i2 = va_index(gpu_va, GA10B_PDE2_VA_HI, GA10B_PDE2_VA_LO);
    uint16_t i1 = va_index(gpu_va, GA10B_PDE1_VA_HI, GA10B_PDE1_VA_LO);
    uint16_t i0 = va_index(gpu_va, GA10B_PDE0_VA_HI, GA10B_PDE0_VA_LO);
    uint16_t ip = va_index(gpu_va, GA10B_PTE_VA_HI,  GA10B_PTE_VA_LO);

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
     * how many full walks actually fired. Reading these from the
     * shell helps distinguish "filter too strict" from "lots of
     * walks but no leaf match". */
    extern int uart_printf(const char *fmt, ...);
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
            if (walk_ok <= 4) {
                uart_printf("[gmmu-discover] walk OK at inst=0x%lx "
                            "leaf=0x%lx (want 0x%lx)\n",
                            (unsigned long)cursor,
                            (unsigned long)wr.leaf_phys,
                            (unsigned long)expected_leaf_phys);
            }
            continue;
        }
        uart_printf("[gmmu-discover] match at inst=0x%lx after %u "
                    "candidates, %u walks (%u OK)\n",
                    (unsigned long)cursor, pre_passed, walks, walk_ok);
        return cursor;
    }
    uart_printf("[gmmu-discover] no match after %u candidates "
                "(%u pre-filter pass, %u walks, %u walk-OK)\n",
                (unsigned)((end - scan_start) / 4096u),
                pre_passed, walks, walk_ok);
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

static inline uint32_t bar0_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(uintptr_t)(GA10B_BAR0_BASE + offset);
}

static inline void bar0_write32(uint32_t offset, uint32_t value)
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
             * we already alloc'd leak (no phys-tracker yet,
             * Milestone D). Caller treats this as fatal. */
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
