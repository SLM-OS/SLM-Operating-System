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
