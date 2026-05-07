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
#define GA10B_GMMU_DRAM_TOP    0x280000000ull

static inline bool phys_in_dram(uint64_t phys, size_t bytes)
{
    if (phys < GA10B_GMMU_DRAM_BASE) return false;
    uint64_t end = phys + bytes;
    if (end < phys) return false;        /* wrap */
    if (end > GA10B_GMMU_DRAM_TOP) return false;
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
    for (int i = 0; i < 512; i++) q[i] = 0;
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

    if (g_va_cursor >= GA10B_GMMU_VA_LIMIT) return -1;
    uint64_t va = g_va_cursor;
    g_va_cursor += 4096;

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

    /* Reserve the contiguous VA range from the bump cursor.
     * Range exhaustion is checked atomically with the reservation
     * so a partial alloc from the tail of the range doesn't leak
     * VAs (caller can retry with a smaller request, or admit
     * defeat). */
    uint64_t va_span = (uint64_t)n_pages * 4096ull;
    if (g_va_cursor + va_span > GA10B_GMMU_VA_LIMIT) {
        return -1;
    }
    uint64_t va_base = g_va_cursor;
    g_va_cursor += va_span;

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

    /* DSB SY so any subsequent GPU dispatch sees the cleared PTEs.
     * Note: this does NOT invalidate the GPU's TLB. If the GPU has
     * a cached translation for any of these VAs from a prior walk,
     * a subsequent access would still serve the cached (now stale)
     * mapping. The Milestone D TLB invalidate sequence + safe-VA-
     * reuse is the fix; until then, callers must NOT touch a freed
     * VA from the GPU side. */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Track the freed extent for future TLB-invalidate-enabled
     * reuse. Drop on the floor if the table is full — alloc still
     * works because the bump cursor is independent. */
    if (g_free_extent_count < GA10B_GMMU_FREE_TRACKER_SLOTS) {
        g_free_extents[g_free_extent_count].gpu_va = gpu_va;
        g_free_extents[g_free_extent_count].n_pages = n_pages;
        g_free_extent_count++;
    }
    return 0;
}
