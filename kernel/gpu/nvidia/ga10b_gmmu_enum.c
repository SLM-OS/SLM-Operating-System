/*
 * ga10b_gmmu_enum.c — enumerate every page of a GA10B channel VM.
 *
 * `ga10b_gmmu_walk` resolves one GPU VA. The boot reserve needs the
 * opposite: every page a kexec-inherited channel depends on — its
 * page tables and every page they map (shaders, cbufs, QMDs, input /
 * weight / output buffers, GR global ctx buffers) — so it can pin
 * them before `pmm_init` turns the rest of DRAM into free blocks.
 * Without that pin, PMM writes a `{next, prev}` free-list link into
 * the first 16 bytes of each free block's head page; on jetson-nano-2
 * (#1029, 2026-10-30) that link landed on a PDE2 table entry and on
 * the first SASS instruction of an inherited shader.
 *
 * DRAM access goes through the caller's `read64` so the host test
 * harness can drive this against in-memory tables.
 */

#include "ga10b_gmmu.h"
#include "ga10b_gmmu_decode.h"

#include <stdbool.h>
#include <stdint.h>

struct enum_state {
    ga10b_gmmu_read64_fn read64;
    ga10b_gmmu_page_fn   visit;
    void                *ctx;
    uint32_t             tables;
    int                  visits;
    bool                 over_budget;
};

/* Regular-PDE levels: [0]=PDE3, [1]=PDE2, [2]=PDE1. */
static const struct {
    uint32_t entries;
    int      va_lo;
} k_regular_levels[3] = {
    { 1u << (GA10B_PDE3_VA_HI - GA10B_PDE3_VA_LO + 1), GA10B_PDE3_VA_LO },
    { 1u << (GA10B_PDE2_VA_HI - GA10B_PDE2_VA_LO + 1), GA10B_PDE2_VA_LO },
    { 1u << (GA10B_PDE1_VA_HI - GA10B_PDE1_VA_LO + 1), GA10B_PDE1_VA_LO },
};

#define PDE0_ENTRIES    (1u << (GA10B_PDE0_VA_HI - GA10B_PDE0_VA_LO + 1))
#define BIG_PTE_ENTRIES (1u << (GA10B_PTE_VA_HI - GA10B_PTE_VA_LO + 1))
#define SMALL_PTE_ENTRIES \
    (1u << (GA10B_PTE_SMALL_VA_HI - GA10B_PTE_SMALL_VA_LO + 1))

/* Report a table; false once the table budget is spent. */
static bool visit_table(struct enum_state *st, uint64_t phys,
                        uint64_t bytes)
{
    if (st->tables >= GA10B_GMMU_ENUM_MAX_TABLES) {
        st->over_budget = true;
        return false;
    }
    st->tables++;
    st->visit(st->ctx, GA10B_GMMU_PAGE_TABLE, 0, phys, bytes);
    st->visits++;
    return true;
}

static void enum_pte_table(struct enum_state *st, uint64_t table,
                           uint64_t va_base, bool small)
{
    uint32_t n     = small ? SMALL_PTE_ENTRIES : BIG_PTE_ENTRIES;
    int      va_lo = small ? GA10B_PTE_SMALL_VA_LO : GA10B_PTE_VA_LO;
    uint64_t page  = 1ull << va_lo;
    if (!visit_table(st, table, (uint64_t)n * GA10B_GMMU_ENTRY_SIZE)) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint64_t entry = st->read64(table + (uint64_t)i * GA10B_GMMU_ENTRY_SIZE);
        uint64_t phys;
        uint8_t aperture;
        bool valid, read_only, privileged;
        decode_pte(entry, &phys, &aperture, &valid, &read_only, &privileged);
        if (!valid) {
            continue;
        }
        st->visit(st->ctx, GA10B_GMMU_PAGE_LEAF,
                  va_base + (uint64_t)i * page, phys, page);
        st->visits++;
    }
}

/* PDE0 entries are dual (16 B): big-page child in bytes 0..7, small-
 * page child in 8..15. Unlike the single-VA walker, which takes the
 * first valid half, both halves are followed here — a 2 MB span can
 * hold big- and small-page mappings at once. */
static void enum_pde0_table(struct enum_state *st, uint64_t table,
                            uint64_t va_base)
{
    if (!visit_table(st, table,
                     (uint64_t)PDE0_ENTRIES * GA10B_GMMU_PDE0_SIZE)) {
        return;
    }
    for (uint32_t i = 0; i < PDE0_ENTRIES && !st->over_budget; i++) {
        uint64_t entry_phys = table + (uint64_t)i * GA10B_GMMU_PDE0_SIZE;
        uint64_t va = va_base + ((uint64_t)i << GA10B_PDE0_VA_LO);
        uint64_t next;
        uint8_t aperture;
        bool valid;

        decode_dual_pde_big(st->read64(entry_phys), &next, &aperture, &valid);
        if (valid) {
            enum_pte_table(st, next, va, false);
        }
        decode_dual_pde_small(st->read64(entry_phys + 8u), &next,
                              &aperture, &valid);
        if (valid) {
            enum_pte_table(st, next, va, true);
        }
    }
}

static void enum_regular_table(struct enum_state *st, int lvl,
                               uint64_t table, uint64_t va_base)
{
    uint32_t n = k_regular_levels[lvl].entries;
    if (!visit_table(st, table, (uint64_t)n * GA10B_GMMU_ENTRY_SIZE)) {
        return;
    }
    for (uint32_t i = 0; i < n && !st->over_budget; i++) {
        uint64_t entry = st->read64(table + (uint64_t)i * GA10B_GMMU_ENTRY_SIZE);
        uint64_t next;
        uint8_t aperture;
        bool valid;
        decode_regular_pde(entry, &next, &aperture, &valid);
        if (!valid) {
            continue;
        }
        uint64_t va = va_base + ((uint64_t)i << k_regular_levels[lvl].va_lo);
        if (lvl < 2) {
            enum_regular_table(st, lvl + 1, next, va);
        } else {
            enum_pde0_table(st, next, va);
        }
    }
}

int ga10b_gmmu_enum_pages(uint64_t inst_block_phys,
                          ga10b_gmmu_read64_fn read64,
                          ga10b_gmmu_page_fn visit, void *ctx)
{
    if (inst_block_phys == 0 || read64 == NULL || visit == NULL) {
        return -1;
    }
    /* PDB lo/hi are adjacent 32-bit words (512, 516): one 8-byte read.
     * Decoded exactly as ga10b_gmmu_walk does. */
    uint64_t pdb = read64(inst_block_phys + INST_BLOCK_PDB_LO_BYTE_OFFSET);
    uint32_t pdb_lo = (uint32_t)pdb;
    uint32_t pdb_hi = (uint32_t)(pdb >> 32);
    uint8_t target = (uint8_t)((pdb_lo >> 1) & 0x3);
    uint64_t pdb_phys = ((uint64_t)pdb_hi << 32) | (pdb_lo & 0xfffff000u);
    if (pdb_phys == 0 || target == 0) {
        return -1;
    }

    struct enum_state st = {
        .read64 = read64, .visit = visit, .ctx = ctx,
        .tables = 0, .visits = 0, .over_budget = false,
    };
    enum_regular_table(&st, 0, pdb_phys, 0);
    return st.over_budget ? -2 : st.visits;
}
