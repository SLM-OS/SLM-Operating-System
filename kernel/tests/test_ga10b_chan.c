/*
 * test_ga10b_chan.c - GA10B address space, instance block, RAMFC and
 * runlist encoders (#1088).
 *
 * Values are worked out from nvgpu L4T R36.4.x (gmmu_ga10b_fusa.c,
 * ramin_ga10b/gv11b_fusa.c, ramfc_ga10b_fusa.c, runlist_ram_gv11b_fusa.c)
 * and, where the MMIO traces show them, checked against nvgpu on
 * jetson-nano-2: runlist base 0x24a200000 -> 0x4a200003/0x2 (mnist
 * trace line 2), TLB invalidate PDB 0x1080ad000 -> 0x01080ad2 (cold
 * trace 17474), CHRAM base from channel_config 0x00c2000b (cold trace
 * 248). The channel entry is also checked against a Linux runlist word
 * (0x2a925ef0) that the old SLM-OS decode misread.
 *
 * The VM tests run ga10b_vm_map against fake ops whose "physical"
 * pages sit above 4 GB, so every high-word field is exercised.
 */
#include "unity.h"
#include "../gpu/nvidia/ga10b_chan.h"
#include "../gpu/nvidia/ga10b_grctx.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define POOL_PAGES 32u
#define POOL_PHYS  0x123400000ull

static alignas(4096) uint8_t g_pool[POOL_PAGES][4096];
static bool g_used[POOL_PAGES];
static unsigned g_allocs, g_frees;

static void *f_alloc(void *ctx, uint32_t n, uint64_t *phys)
{
    (void)ctx;
    for (uint32_t i = 0; i + n <= POOL_PAGES; i++) {
        uint32_t k;
        for (k = 0; k < n && !g_used[i + k]; k++) { }
        if (k == n) {
            for (k = 0; k < n; k++) {
                g_used[i + k] = true;
            }
            memset(g_pool[i], 0, (size_t)n * 4096u);
            *phys = POOL_PHYS + (uint64_t)i * 4096u;
            g_allocs++;
            return g_pool[i];
        }
    }
    return NULL;
}

static void f_free(void *ctx, void *cpu, uint32_t n)
{
    (void)ctx;
    size_t i = (size_t)((uint8_t *)cpu - &g_pool[0][0]) / 4096u;
    for (uint32_t k = 0; k < n; k++) {
        g_used[i + k] = false;
    }
    g_frees++;
}

static void *f_p2c(void *ctx, uint64_t phys)
{
    (void)ctx;
    if (phys < POOL_PHYS || phys >= POOL_PHYS + POOL_PAGES * 4096ull) return NULL;
    return &g_pool[0][0] + (phys - POOL_PHYS);
}

static void f_cache(void *ctx, const void *p, size_t n) { (void)ctx; (void)p; (void)n; }
static uint32_t f_rd32(void *ctx, uint32_t off) { (void)ctx; (void)off; return 0x00018000u; }
static void f_wr32(void *ctx, uint32_t off, uint32_t v) { (void)ctx; (void)off; (void)v; }
static void f_udelay(void *ctx, uint32_t us) { (void)ctx; (void)us; }

static const struct ga10b_gpu_ops g_ops = {
    .rd32 = f_rd32, .wr32 = f_wr32, .udelay = f_udelay,
    .alloc_pages = f_alloc, .free_pages = f_free, .phys_to_cpu = f_p2c,
    .cache_clean = f_cache, .cache_inval = f_cache,
};

static void pool_reset(void)
{
    memset(g_used, 0, sizeof(g_used));
    g_allocs = g_frees = 0;
}

static void test_gmmu_encoders(void)
{
    uint32_t w[4];
    ga10b_pde_encode(0x1080ad000ull, w);
    /* aperture ncoh 0x6 | vol 0x8 | (phys >> 12)[23:0] << 8 */
    TEST_ASSERT_EQUAL_HEX32(0x1080ad0eu, w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x0u, w[1]);
    ga10b_pde0_small_encode(0x1080ad000ull, w);
    TEST_ASSERT_EQUAL_HEX32(0, w[0]);              /* big half invalid */
    TEST_ASSERT_EQUAL_HEX32(0, w[1]);
    TEST_ASSERT_EQUAL_HEX32(0x1080ad0eu, w[2]);
    TEST_ASSERT_EQUAL_HEX32(0x0u, w[3]);
    ga10b_pde_encode(0x1234567000000ull, w);            /* bits above 2^36 */
    TEST_ASSERT_EQUAL_HEX32(0x5670000eu, w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x1234u, w[1]);
    ga10b_pte_encode(0x24a201000ull, 0, w);
    TEST_ASSERT_EQUAL_HEX32(0x24a2010fu, w[0]);    /* valid | ncoh | vol */
    TEST_ASSERT_EQUAL_HEX32(0x0u, w[1]);           /* kind 0 */
    ga10b_pte_encode(0x24a201000ull, GA10B_MAP_CACHEABLE | GA10B_MAP_PRIV | GA10B_MAP_RO, w);
    TEST_ASSERT_EQUAL_HEX32(0x24a20167u, w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x01080ad2u, ga10b_tlb_invalidate_pdb_val(0x1080ad000ull));
    TEST_ASSERT_EQUAL_UINT32(0, ga10b_vm_idx_pde1(0x4000000ull));
    TEST_ASSERT_EQUAL_UINT32(256, ga10b_vm_idx_pde1(0x2000000000ull));
    TEST_ASSERT_EQUAL_UINT32(32, ga10b_vm_idx_pde0(0x4000000ull));
    TEST_ASSERT_EQUAL_UINT32(0x1ff, ga10b_vm_idx_pte(0x41ff000ull));
}

static void test_vm_map_walk(void)
{
    pool_reset();
    struct ga10b_vm vm;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_ops, &vm));
    TEST_ASSERT_EQUAL_UINT32(1, vm.table_pages);

    uint64_t pa = POOL_PHYS + 20u * 4096u;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK,
        ga10b_vm_map(&g_ops, &vm, GA10B_VM_USER_BASE, pa, 2u * 4096u, 0));
    /* odd PDE0 index too: 0x4200000 -> PDE0 33 */
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK,
        ga10b_vm_map(&g_ops, &vm, 0x4200000ull, pa, 4096u, 0));
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK,
        ga10b_vm_map(&g_ops, &vm, GA10B_VM_KERNEL_BASE, pa + 4096u, 4096u, GA10B_MAP_PRIV));
    /* PDB + PDE2 + PDE1 + 2 PDE0 (user / kernel PDE1 slots) + 3 PTs. */
    TEST_ASSERT_EQUAL_UINT32(8, vm.table_pages);

    TEST_ASSERT_EQUAL_HEX64(pa, ga10b_vm_lookup(&g_ops, &vm, GA10B_VM_USER_BASE));
    TEST_ASSERT_EQUAL_HEX64(pa + 4096u + 0x10u, ga10b_vm_lookup(&g_ops, &vm, GA10B_VM_USER_BASE + 0x1010u));
    TEST_ASSERT_EQUAL_HEX64(pa, ga10b_vm_lookup(&g_ops, &vm, 0x4200000ull));
    TEST_ASSERT_EQUAL_HEX64(pa + 4096u, ga10b_vm_lookup(&g_ops, &vm, GA10B_VM_KERNEL_BASE));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_ops, &vm, GA10B_VM_USER_BASE + 0x2000u));

    /* Hardware layout: walk the user VA by hand with nvgpu's strides. */
    const uint32_t *t = (const uint32_t *)vm.pdb.cpu;
    TEST_ASSERT_EQUAL_HEX32(0xe, t[0] & 0xfu);        /* PDE3[0] ncoh+vol */
    const uint32_t *l2 = (const uint32_t *)f_p2c(NULL, ((uint64_t)t[1] << 32 | (t[0] & ~0xffu)) >> 8 << 12);
    TEST_ASSERT_NOT_NULL(l2);
    const uint32_t *l1 = (const uint32_t *)f_p2c(NULL, (uint64_t)(l2[0] >> 8) << 12 | (uint64_t)l2[1] << 36);
    TEST_ASSERT_NOT_NULL(l1);
    const uint32_t *l0 = (const uint32_t *)f_p2c(NULL, (uint64_t)(l1[0] >> 8) << 12 | (uint64_t)l1[1] << 36);
    TEST_ASSERT_NOT_NULL(l0);
    /* PDE0 index 32 at byte 32 * 16, small half in words 2/3. */
    const uint32_t *e = l0 + 32u * 4u;
    TEST_ASSERT_EQUAL_HEX32(0, e[0]);
    TEST_ASSERT_EQUAL_HEX32(0, e[1]);
    TEST_ASSERT_EQUAL_HEX32(0xe, e[2] & 0xfu);
    TEST_ASSERT_EQUAL_HEX32(0xe, l0[33u * 4u + 2u] & 0xfu);
    TEST_ASSERT_EQUAL_HEX32(0, l0[33u * 4u]);          /* index 33 big half */
    TEST_ASSERT_EQUAL_HEX32(0, l0[31u * 4u + 2u]);     /* neighbours untouched */

    /* Misaligned / out-of-range requests. */
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_VA, ga10b_vm_map(&g_ops, &vm, 0x4000800ull, pa, 4096u, 0));
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_VA, ga10b_vm_map(&g_ops, &vm, 1ull << 49, pa, 4096u, 0));

    unsigned frees_before = g_frees;
    ga10b_vm_destroy(&g_ops, &vm);
    TEST_ASSERT_EQUAL_UINT32(frees_before + 8u, g_frees);
    for (unsigned i = 0; i < POOL_PAGES; i++) {
        TEST_ASSERT_FALSE(g_used[i]);
    }
}

/* 64 KB mappings: encoding from nvgpu's ga10b_update_gmmu_pde0_locked,
 * walk, exclusivity of a 2 MB span, unmap, and teardown of slab pages. */
static void test_vm_map_big(void)
{
    uint32_t w[4];
    ga10b_pde0_big_encode(0x1080ad100ull, w);
    /* aperture ncoh 0x6 | vol 0x8 | (phys >> 8) << 4 */
    TEST_ASSERT_EQUAL_HEX32((0x1080ad1u << 4) | 0xeu, w[0]);
    TEST_ASSERT_EQUAL_HEX32(0, w[1]);
    TEST_ASSERT_EQUAL_HEX32(0, w[2]);
    TEST_ASSERT_EQUAL_HEX32(0, w[3]);
    ga10b_pde0_big_encode(0x1234500000100ull, w);       /* above bit 36 */
    TEST_ASSERT_EQUAL_HEX32(0x5000001eu, w[0]);
    TEST_ASSERT_EQUAL_HEX32(0x1234u, w[1]);

    pool_reset();
    struct ga10b_vm vm;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_ops, &vm));
    uint64_t va = 0x40000000ull;      /* 2 MB aligned */
    uint64_t pa = 0x200000000ull;     /* mapped memory need not be in the pool */

    /* Misalignment is refused for all three values. */
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_VA,
        ga10b_vm_map_big(&g_ops, &vm, va + 0x1000u, pa, 0x10000u, 0));
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_VA,
        ga10b_vm_map_big(&g_ops, &vm, va, pa + 0x1000u, 0x10000u, 0));
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_VA,
        ga10b_vm_map_big(&g_ops, &vm, va, pa, 0x11000u, 0));

    /* 5 MB across three PDE0 spans: 3 tables from one slab page. */
    uint32_t before = vm.table_pages;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK,
        ga10b_vm_map_big(&g_ops, &vm, va, pa, 0x500000u, GA10B_MAP_CACHEABLE | GA10B_MAP_RO));
    /* PDE2 + PDE1 + PDE0 + one slab page. */
    TEST_ASSERT_EQUAL_UINT32(before + 4u, vm.table_pages);
    TEST_ASSERT_EQUAL_UINT32(3u, vm.big_slab_used);
    TEST_ASSERT_EQUAL_HEX64(pa, ga10b_vm_lookup(&g_ops, &vm, va));
    TEST_ASSERT_EQUAL_HEX64(pa + 0x1234u, ga10b_vm_lookup(&g_ops, &vm, va + 0x1234u));
    TEST_ASSERT_EQUAL_HEX64(pa + 0x4fffffu, ga10b_vm_lookup(&g_ops, &vm, va + 0x4fffffu));
    /* Past the last big PTE, inside the mapped span's table. */
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_ops, &vm, va + 0x500000u));

    /* No 2 MB span holds both page sizes, either way round. */
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_VA,
        ga10b_vm_map(&g_ops, &vm, va + 0x400000u, POOL_PHYS, 4096u, 0));
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK,
        ga10b_vm_map(&g_ops, &vm, va + 0x600000u, POOL_PHYS, 4096u, 0));
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_VA,
        ga10b_vm_map_big(&g_ops, &vm, va + 0x600000u + 0x10000u, pa, 0x10000u, 0));

    /* Unmap drops only the range; small and big both. */
    ga10b_vm_unmap(&g_ops, &vm, va + 0x10000u, 0x200000u);
    TEST_ASSERT_EQUAL_HEX64(pa, ga10b_vm_lookup(&g_ops, &vm, va));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_ops, &vm, va + 0x10000u));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_ops, &vm, va + 0x200000u));
    TEST_ASSERT_EQUAL_HEX64(pa + 0x210000u, ga10b_vm_lookup(&g_ops, &vm, va + 0x210000u));
    ga10b_vm_unmap(&g_ops, &vm, va + 0x600000u, 4096u);
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_ops, &vm, va + 0x600000u));
    /* Re-mapping the same VA works. */
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK,
        ga10b_vm_map_big(&g_ops, &vm, va + 0x10000u, pa, 0x10000u, 0));
    TEST_ASSERT_EQUAL_HEX64(pa, ga10b_vm_lookup(&g_ops, &vm, va + 0x10000u));

    /* A 17th table starts a second slab page; destroy frees every page once. */
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK,
        ga10b_vm_map_big(&g_ops, &vm, va + 0x800000u, pa, 16u * 0x200000u, 0));
    ga10b_vm_destroy(&g_ops, &vm);
    for (unsigned i = 0; i < POOL_PAGES; i++) {
        TEST_ASSERT_FALSE(g_used[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(g_allocs, g_frees);
}

static void test_vm_reserve(void)
{
    struct ga10b_vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.user_next = GA10B_VM_USER_BASE;
    vm.kernel_next = GA10B_VM_KERNEL_BASE;
    TEST_ASSERT_EQUAL_HEX64(0x4000000, ga10b_vm_reserve(&vm, 100, 4096, false));
    TEST_ASSERT_EQUAL_HEX64(0x4010000, ga10b_vm_reserve(&vm, 0x10000, 0x10000, false));
    TEST_ASSERT_EQUAL_HEX64(0x4020000, vm.user_next);
    TEST_ASSERT_EQUAL_HEX64(0x2000000000, ga10b_vm_reserve(&vm, 0x7d500, 4096, true));
    TEST_ASSERT_EQUAL_HEX64(0x200007e000, vm.kernel_next);
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_reserve(&vm, 4096, 3000, true));   /* bad align */
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_reserve(&vm, GA10B_VM_KERNEL_LIMIT, 4096, true));
}

static void test_inst_block_fields(void)
{
    static uint32_t w[1024];
    memset(w, 0, sizeof(w));
    ga10b_inst_set_magic(w);
    ga10b_inst_init_pdb(w, 0x1080ad000ull);
    TEST_ASSERT_EQUAL_HEX32(0xcafeca11u, w[131]);
    /* ncoh 3 | vol 4 | ver2 0x400 | 64K 0x800 | pdb[31:12] */
    TEST_ASSERT_EQUAL_HEX32(0x080adc07u, w[128]);
    TEST_ASSERT_EQUAL_HEX32(0x1u, w[129]);

    TEST_ASSERT_EQUAL_HEX32(0x080adc07u, ga10b_subctx_pdb_format_word(0x1080ad000ull, false));
    TEST_ASSERT_EQUAL_HEX32(0x080adc37u, ga10b_subctx_pdb_format_word(0x1080ad000ull, true));
    ga10b_inst_set_subctxs(w, (1ull << 1) | (1ull << 40), 0x1080ad000ull, false);
    TEST_ASSERT_EQUAL_HEX32(0x1u, w[168]);            /* VEID 0 invalid */
    TEST_ASSERT_EQUAL_HEX32(0x0u, w[169]);
    TEST_ASSERT_EQUAL_HEX32(0x080adc07u, w[172]);     /* VEID 1 */
    TEST_ASSERT_EQUAL_HEX32(0x1u, w[173]);
    TEST_ASSERT_EQUAL_HEX32(0x080adc07u, w[168 + 160]);   /* VEID 40 */
    TEST_ASSERT_EQUAL_HEX32(0x2u, w[166]);
    TEST_ASSERT_EQUAL_HEX32(0x100u, w[167]);
    TEST_ASSERT_EQUAL_HEX32(0x1u, w[168 + 63 * 4]);   /* last entry */
    TEST_ASSERT_EQUAL_HEX32(0x0u, w[168 + 64 * 4]);   /* nothing past it */

    ga10b_inst_set_gr_ptr(w, 0x2000081000ull);
    TEST_ASSERT_EQUAL_HEX32(0x00081004u, w[132]);     /* virtual wfi mode */
    TEST_ASSERT_EQUAL_HEX32(0x20u, w[133]);
    ga10b_inst_set_eng_method_buffer(w, 0x123456789000ull);
    TEST_ASSERT_EQUAL_HEX32(0x56789000u, w[136]);
    TEST_ASSERT_EQUAL_HEX32(0x1234u, w[137]);
}

static void test_ramfc_helper_channel(void)
{
    static uint32_t w[1024];
    for (unsigned i = 0; i < 1024; i++) {
        w[i] = 0xdeadbeefu;
    }
    struct ga10b_ramfc_params p = {
        .gpfifo_va = 0x4001000ull, .gpfifo_entries = 512, .chid = 508, .veid = 1,
    };
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_ramfc_setup(w, &p));
    TEST_ASSERT_EQUAL_HEX32(0x04001000u, w[18]);
    TEST_ASSERT_EQUAL_HEX32(0x00090000u, w[19]);      /* limit2 = log2(512) */
    TEST_ASSERT_EQUAL_HEX32(0xc76fu, w[4]);
    TEST_ASSERT_EQUAL_HEX32(0x20400000u, w[33]);
    TEST_ASSERT_EQUAL_HEX32(0x30000001u, w[37]);
    TEST_ASSERT_EQUAL_HEX32(0x00030000u, w[43]);
    TEST_ASSERT_EQUAL_HEX32(0x00000102u, w[12]);      /* WDT off */
    TEST_ASSERT_EQUAL_HEX32(0x01fc0100u, w[63]);
    TEST_ASSERT_EQUAL_HEX32(0x80000000u, w[62]);
    TEST_ASSERT_EQUAL_HEX32(0x00001000u, w[61]);
    TEST_ASSERT_EQUAL_HEX32(0, w[57]);                /* not privileged */
    TEST_ASSERT_EQUAL_HEX32(0, w[0]);                 /* RAMFC zeroed */
    TEST_ASSERT_EQUAL_HEX32(0, w[127]);
    TEST_ASSERT_EQUAL_HEX32(0x1u, w[134]);            /* engine_wfi_veid */
    TEST_ASSERT_EQUAL_HEX32(0xdeadbeefu, w[128]);     /* RAMIN untouched */
    TEST_ASSERT_EQUAL_HEX32(0xdeadbeefu, w[133]);

    p.gpfifo_entries = 500;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_ARG, ga10b_ramfc_setup(w, &p));
    p.gpfifo_entries = 512;
    p.veid = 64;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_ARG, ga10b_ramfc_setup(w, &p));

    /* 3000 ms watchdog: 2343750 x 1024 ns -> mantissa 36621, exponent 6. */
    TEST_ASSERT_EQUAL_HEX32(0xc786b102u, ga10b_pbdma_acquire_val(3000));
    TEST_ASSERT_EQUAL_HEX32(0x102u, ga10b_pbdma_acquire_val(0));
}

static void test_userd_and_gpfifo(void)
{
    uint32_t u[128];
    for (unsigned i = 0; i < 128; i++) {
        u[i] = 0xffffffffu;
    }
    ga10b_userd_init(u);
    TEST_ASSERT_EQUAL_HEX32(0, u[34]);
    TEST_ASSERT_EQUAL_HEX32(0, u[35]);
    TEST_ASSERT_EQUAL_HEX32(0, u[16]);
    TEST_ASSERT_EQUAL_HEX32(0, u[24]);
    TEST_ASSERT_EQUAL_HEX32(0xffffffffu, u[20]);
    uint32_t e[2];
    ga10b_gpfifo_entry(0x4011000ull, 14, e);
    TEST_ASSERT_EQUAL_HEX32(0x04011000u, e[0]);
    TEST_ASSERT_EQUAL_HEX32(14u << 10, e[1]);
}

static void test_runlist_entries(void)
{
    uint32_t w[4];
    ga10b_runlist_tsg_entry(w, 3, 1, GA10B_TSG_TIMESLICE_DEFAULT_US);
    /* 1024 -> timeout 128, scale 3 */
    TEST_ASSERT_EQUAL_HEX32(0x80030001u, w[0]);
    TEST_ASSERT_EQUAL_HEX32(1u, w[1]);
    TEST_ASSERT_EQUAL_HEX32(3u, w[2]);
    TEST_ASSERT_EQUAL_HEX32(0u, w[3]);
    ga10b_runlist_tsg_entry(w, 0, 2, 100);
    TEST_ASSERT_EQUAL_HEX32(0x64000001u, w[0]);

    ga10b_runlist_chan_entry(w, 508, 0x106b6c000ull, 0x2a925e00ull, 0);
    TEST_ASSERT_EQUAL_HEX32(0x2a925ef0u, w[0]);       /* the Linux runlist word */
    TEST_ASSERT_EQUAL_HEX32(0u, w[1]);
    TEST_ASSERT_EQUAL_HEX32(0x06b6c1fcu, w[2]);
    TEST_ASSERT_EQUAL_HEX32(1u, w[3]);

    TEST_ASSERT_EQUAL_HEX32(0x4a200003u, ga10b_runlist_base_lo(0x24a200000ull));
    TEST_ASSERT_EQUAL_HEX32(0x2u, ga10b_runlist_base_hi(0x24a200000ull));
    TEST_ASSERT_EQUAL_HEX32(0x8u, ga10b_runlist_submit_val(0, 8));
    TEST_ASSERT_EQUAL_HEX32(0x00c20000u, ga10b_chram_base_from_config(0x00c2000bu));
    TEST_ASSERT_EQUAL_HEX32(0x1fcu, ga10b_doorbell_token(508, 0));
    TEST_ASSERT_EQUAL_HEX32(0x201fcu, ga10b_doorbell_token(508, 2));
}

static void test_fifo_init_table(void)
{
    TEST_ASSERT_EQUAL_UINT32(82, ga10b_chan_fifo_init_writes_count);
    TEST_ASSERT_EQUAL_HEX32(0xc00300u, ga10b_chan_fifo_init_writes[0].offset);
    TEST_ASSERT_EQUAL_HEX32(0x800007ffu, ga10b_chan_fifo_init_writes[0].value);
    TEST_ASSERT_EQUAL_HEX32(0xc00828u, ga10b_chan_fifo_init_writes[81].offset);
    TEST_ASSERT_EQUAL_HEX32(0x64u, ga10b_chan_fifo_init_writes[81].value);
}

/* ga10b_chan_create rejects a GPFIFO larger than
 * GA10B_CHAN_MAX_GPFIFO_ENTRIES before touching any memory (the ring
 * size is entries * 8 in 32 bits). */
static void test_chan_create_bounds_gpfifo(void)
{
    static struct ga10b_grctx grctx;
    static struct ga10b_gr_chan_ctx gr;
    static struct ga10b_chan ch;
    struct ga10b_chan_params p;
    ga10b_chan_default_params(&p);
    unsigned before = g_allocs;
    p.gpfifo_entries = GA10B_CHAN_MAX_GPFIFO_ENTRIES << 1;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_ARG, ga10b_chan_create(&g_ops, &grctx, &p, &gr, &ch));
    p.gpfifo_entries = 1u << 31;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_ARG, ga10b_chan_create(&g_ops, &grctx, &p, &gr, &ch));
    TEST_ASSERT_EQUAL_UINT(before, g_allocs);
}

/* #1171: ga10b_chan_reset, the channel half of recovery. Recording ops:
 * every register write in order, reads 0 (polls pass at once). */
static struct { uint32_t off, val; } g_log[64];
static unsigned g_nlog;
static uint32_t r_rd32(void *ctx, uint32_t off) { (void)ctx; (void)off; return 0u; }
static void r_wr32(void *ctx, uint32_t off, uint32_t v)
{
    (void)ctx;
    if (g_nlog < 64u) {
        g_log[g_nlog].off = off;
        g_log[g_nlog].val = v;
    }
    g_nlog++;
}

static const struct ga10b_gpu_ops g_rec_ops = {
    .rd32 = r_rd32, .wr32 = r_wr32, .udelay = f_udelay,
    .alloc_pages = f_alloc, .free_pages = f_free, .phys_to_cpu = f_p2c,
    .cache_clean = f_cache, .cache_inval = f_cache,
};

static int log_find(uint32_t off, uint32_t val, unsigned from)
{
    for (unsigned i = from; i < g_nlog && i < 64u; i++) {
        if (g_log[i].off == off && g_log[i].val == val) return (int)i;
    }
    return -1;
}

static void test_chan_reset_rebinds_a_clean_channel(void)
{
    pool_reset();
    static uint8_t golden[0x1000], img[0x1000], patch[0x10000], hdr[GA10B_GR_FECS_HEADER_BYTES];
    struct ga10b_grctx g;
    memset(&g, 0, sizeof(g));
    ga10b_gr_config_default(&g.cfg);
    g.golden.cpu = golden;
    g.golden.size = sizeof(golden);
    g.golden_size = sizeof(golden);
    g.golden_ready = true;
    struct ga10b_gr_chan_ctx gr;
    memset(&gr, 0, sizeof(gr));
    gr.gr_ctx.cpu = img;
    gr.gr_ctx.size = sizeof(img);
    gr.patch.cpu = patch;
    gr.patch.size = sizeof(patch);
    gr.header.cpu = hdr;
    gr.header.size = sizeof(hdr);

    struct ga10b_chan ch;
    memset(&ch, 0, sizeof(ch));
    ga10b_chan_default_params(&ch.p);
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_dma_alloc(&g_rec_ops, GA10B_INST_BLOCK_BYTES, &ch.inst));
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_dma_alloc(&g_rec_ops, 4096u, &ch.userd));
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_dma_alloc(&g_rec_ops, 4096u, &ch.runlist));
    ch.gpfifo_va = 0x40000000ull;
    ch.chram_base = 0xc20000u;
    ch.gr = &gr;
    uint32_t *iw = (uint32_t *)ch.inst.cpu;
    uint32_t *uw = (uint32_t *)ch.userd.cpu;
    /* RAMIN as the channel was built; RAMFC and USERD as a hang left them:
     * GP_GET mid-ring, a pending acquire, GP_PUT ahead. */
    iw[GA10B_RAMIN_W_PAGE_DIR_BASE_LO] = 0x12345678u;
    iw[GA10B_RAMIN_W_PAGE_DIR_BASE_LO + 1u] = 0x9u;
    for (unsigned i = 0; i < GA10B_RAMFC_BYTES / 4u; i++) {
        iw[i] = 0xdead0000u | i;
    }
    uw[GA10B_USERD_W_GP_GET] = 17u;
    uw[GA10B_USERD_W_GP_PUT] = 19u;

    g_nlog = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_chan_reset(&g_rec_ops, &g, &ch, 0u));

    /* RAMFC is what SETUP_BIND programs: compare with a fresh setup. */
    static uint32_t fresh[GA10B_INST_BLOCK_BYTES / 4u];
    memset(fresh, 0, sizeof(fresh));
    struct ga10b_ramfc_params rp = {
        .gpfifo_va = ch.gpfifo_va, .gpfifo_entries = ch.p.gpfifo_entries,
        .chid = ch.p.chid, .veid = ch.p.veid,
    };
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_ramfc_setup(fresh, &rp));
    TEST_ASSERT_EQUAL_MEMORY(fresh, iw, GA10B_RAMFC_BYTES);
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, iw[GA10B_RAMIN_W_PAGE_DIR_BASE_LO]);
    TEST_ASSERT_EQUAL_HEX32(0x9u, iw[GA10B_RAMIN_W_PAGE_DIR_BASE_LO + 1u]);
    TEST_ASSERT_EQUAL_UINT32(0, uw[GA10B_USERD_W_GP_GET]);
    TEST_ASSERT_EQUAL_UINT32(0, uw[GA10B_USERD_W_GP_PUT]);
    /* The golden image is back in the context. */
    TEST_ASSERT_EQUAL_MEMORY(golden + 0x100, img + 0x100, 0x100);

    /* Order: CHRAM cleared, the runlist submitted, scheduling back on,
     * then a forced reload and the channel enabled, last. */
    uint32_t chram = ch.chram_base + ch.p.chid * 4u;
    uint32_t rl = ch.p.runlist_pri_base;
    int clear = log_find(chram, GA10B_CHRAM_CLEAR_CHANNEL, 0);
    int submit = log_find(rl + GA10B_RL_SUBMIT, ga10b_runlist_submit_val(0u, 2u), 0);
    int sched = log_find(rl + GA10B_RL_SCHED_DISABLE, 0u, 0);
    int reload = log_find(chram, GA10B_CHRAM_FORCE_CTX_RELOAD, 0);
    int enable = log_find(chram, GA10B_CHRAM_ENABLE_CHANNEL, 0);
    TEST_ASSERT_EQUAL_INT(0, clear);
    TEST_ASSERT_TRUE(submit > clear);
    TEST_ASSERT_TRUE(sched > submit);
    TEST_ASSERT_TRUE(reload > sched);
    TEST_ASSERT_EQUAL_INT((int)g_nlog - 1, enable);
    TEST_ASSERT_TRUE(ch.enabled);

    /* GA10B_CHAN_RESET_NO_RELOAD, the recovery's default (#1171): the
     * same order with no forced context reload at all. A reload still
     * pending when the GR engine is reset left the channel DEAD on
     * hardware. */
    g_nlog = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK,
                          ga10b_chan_reset(&g_rec_ops, &g, &ch, GA10B_CHAN_RESET_NO_RELOAD));
    clear = log_find(chram, GA10B_CHRAM_CLEAR_CHANNEL, 0);
    submit = log_find(rl + GA10B_RL_SUBMIT, ga10b_runlist_submit_val(0u, 2u), 0);
    sched = log_find(rl + GA10B_RL_SCHED_DISABLE, 0u, 0);
    enable = log_find(chram, GA10B_CHRAM_ENABLE_CHANNEL, 0);
    TEST_ASSERT_EQUAL_INT(-1, log_find(chram, GA10B_CHRAM_FORCE_CTX_RELOAD, 0));
    TEST_ASSERT_EQUAL_INT(0, clear);
    TEST_ASSERT_TRUE(submit > clear);
    TEST_ASSERT_TRUE(sched > submit);
    TEST_ASSERT_EQUAL_INT((int)g_nlog - 1, enable);
    TEST_ASSERT_TRUE(enable > sched);

    /* Without a GR context to reload: refused before touching anything. */
    ch.gr = NULL;
    g_nlog = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_ERR_ARG, ga10b_chan_reset(&g_rec_ops, &g, &ch, 0u));
    TEST_ASSERT_EQUAL_UINT32(0, g_nlog);
    ga10b_dma_free(&g_rec_ops, &ch.inst);
    ga10b_dma_free(&g_rec_ops, &ch.userd);
    ga10b_dma_free(&g_rec_ops, &ch.runlist);
}

/* Runlist-wide preempt for recovery: type RUNLIST, then CHRAM polled. */
static void test_preempt_runlist(void)
{
    struct ga10b_chan ch;
    memset(&ch, 0, sizeof(ch));
    ga10b_chan_default_params(&ch.p);
    ch.chram_base = 0xc20000u;
    g_nlog = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_chan_preempt_runlist(&g_rec_ops, &ch, 100u));
    TEST_ASSERT_EQUAL_UINT32(1, g_nlog);
    TEST_ASSERT_EQUAL_HEX32(ch.p.runlist_pri_base + GA10B_RL_PREEMPT, g_log[0].off);
    TEST_ASSERT_EQUAL_HEX32(GA10B_RL_PREEMPT_TYPE_RUNLIST, g_log[0].val);
    /* f_rd32 reads 0x18000: ON_PBDMA clear, TSG PENDING clear. */
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_chan_preempt_tsg_within(&g_ops, &ch, 100u));
}

int test_suite_ga10b_chan(void)
{
    UnityBegin("test_ga10b_chan.c");
    RUN_TEST(test_gmmu_encoders);
    RUN_TEST(test_vm_map_walk);
    RUN_TEST(test_vm_map_big);
    RUN_TEST(test_vm_reserve);
    RUN_TEST(test_inst_block_fields);
    RUN_TEST(test_ramfc_helper_channel);
    RUN_TEST(test_userd_and_gpfifo);
    RUN_TEST(test_chan_create_bounds_gpfifo);
    RUN_TEST(test_runlist_entries);
    RUN_TEST(test_fifo_init_table);
    RUN_TEST(test_chan_reset_rebinds_a_clean_channel);
    RUN_TEST(test_preempt_runlist);
    return UnityEnd();
}
