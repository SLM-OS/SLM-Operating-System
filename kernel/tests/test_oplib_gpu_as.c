/*
 * test_oplib_gpu_as.c - Operator-library address space on an
 * SLM-OS-built channel (#1092).
 *
 * Runs the cold-VM backend and the staging helpers against a real
 * struct ga10b_vm whose pages come from a fake allocator (physical
 * addresses above 4 GB, as in test_ga10b_chan.c), and checks every
 * mapping by walking the page tables with ga10b_vm_lookup: the SASS
 * pool and its scratch slots, the cbuf, the QMD pool a channel-only
 * handoff gets, and a chunked weights pool whose chunks are not
 * physically adjacent. A second, scripted backend checks the
 * free-after-failed-map rule.
 */
#include "unity.h"
#include "../include/oplib_gpu_as.h"
#include "../include/oplib_pool.h"
#include "../gpu/nvidia/ga10b_chan.h"
#include "../gpu/nvidia/ga10b_channel_handoff.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define POOL_PAGES 400u
#define POOL_PHYS  0x234500000ull

static alignas(4096) uint8_t g_pool[POOL_PAGES][4096];
static bool g_used[POOL_PAGES];
static unsigned g_allocs, g_frees;
/* Multi-page allocations left before failing (<0 = never). Single
 * pages (page-table pages) are never failed or counted, so the
 * injection hits pool / chunk allocations only. */
static int g_fail_after;

static void *f_alloc(void *ctx, uint32_t n, uint64_t *phys)
{
    (void)ctx;
    if (n > 1 && g_fail_after == 0) return NULL;
    for (uint32_t i = 0; i + n <= POOL_PAGES; i++) {
        uint32_t k;
        for (k = 0; k < n && !g_used[i + k]; k++) { }
        if (k == n) {
            for (k = 0; k < n; k++) {
                g_used[i + k] = true;
            }
            memset(g_pool[i], 0xA5, (size_t)n * 4096u);   /* NOT zeroed */
            *phys = POOL_PHYS + (uint64_t)i * 4096u;
            g_allocs++;
            if (n > 1 && g_fail_after > 0) g_fail_after--;
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

static unsigned g_cleans;
static void f_clean(void *ctx, const void *p, size_t n) { (void)ctx; (void)p; (void)n; g_cleans++; }
static void f_inval(void *ctx, const void *p, size_t n) { (void)ctx; (void)p; (void)n; }
/* MMU_CTRL: PRI FIFO space non-zero and FIFO empty, so the TLB
 * invalidate completes at once (same value as test_ga10b_chan.c). */
static uint32_t f_rd32(void *ctx, uint32_t off) { (void)ctx; (void)off; return 0x00018000u; }
static unsigned g_wr32s;
static void f_wr32(void *ctx, uint32_t off, uint32_t v) { (void)ctx; (void)off; (void)v; g_wr32s++; }
static void f_udelay(void *ctx, uint32_t us) { (void)ctx; (void)us; }

static const struct ga10b_gpu_ops g_ops = {
    .rd32 = f_rd32, .wr32 = f_wr32, .udelay = f_udelay,
    .alloc_pages = f_alloc, .free_pages = f_free, .phys_to_cpu = f_p2c,
    .cache_clean = f_clean, .cache_inval = f_inval,
};

static struct ga10b_vm g_vm;
static struct oplib_cold_as_ctx g_ctx;
static struct oplib_gpu_as g_as;

static void reset(void)
{
    memset(g_used, 0, sizeof(g_used));
    g_allocs = g_frees = g_cleans = g_wr32s = 0;
    g_fail_after = -1;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_ops, &g_vm));
    oplib_gpu_as_init_cold(&g_as, &g_ctx, &g_ops, &g_vm);
}

/* Every page of [va, va + bytes) translates to phys + offset. */
static bool maps_linear(uint64_t va, uint64_t phys, uint64_t bytes)
{
    for (uint64_t off = 0; off < bytes; off += 4096u) {
        if (ga10b_vm_lookup(&g_ops, &g_vm, va + off) != phys + off) return false;
    }
    return true;
}

static void test_cold_backend_maps_into_vm(void)
{
    reset();
    TEST_ASSERT_EQUAL_STRING("cold-vm", g_as.name);
    TEST_ASSERT_NULL(g_as.free);    /* never free after a failed map */
    TEST_ASSERT_NOT_NULL(g_as.reserve);
    TEST_ASSERT_NOT_NULL(g_as.map_at);
    TEST_ASSERT_NOT_NULL(g_as.tlb_invalidate);

    uint64_t pa = 0;
    void *p = g_as.alloc(g_as.ctx, 3, &pa);
    TEST_ASSERT_NOT_NULL(p);
    uint64_t va = 0;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, g_as.map(g_as.ctx, pa, 3u * 4096u, &va));
    TEST_ASSERT_TRUE(va >= GA10B_VM_USER_BASE && va < GA10B_VM_USER_LIMIT);
    TEST_ASSERT_TRUE(maps_linear(va, pa, 3u * 4096u));
    /* Next mapping lands after it (no overlap). */
    uint64_t va2 = 0;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, g_as.map(g_as.ctx, pa, 4096u, &va2));
    TEST_ASSERT_TRUE(va2 >= va + 3u * 4096u);
    /* Unaligned physical address is rejected by ga10b_vm_map. */
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_MAP, g_as.map(g_as.ctx, pa + 8u, 4096u, &va2));

    unsigned before = g_wr32s;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, g_as.tlb_invalidate(g_as.ctx));
    TEST_ASSERT_EQUAL_UINT32(before + 2u, g_wr32s);   /* PDB + trigger */
}

static void test_stage_pool_layout(void)
{
    reset();
    static uint8_t sass[5000];
    for (size_t i = 0; i < sizeof(sass); i++) {
        sass[i] = (uint8_t)(i * 7u + 1u);
    }
    struct oplib_staged_pool sp;
    memset(&sp, 0, sizeof(sp));
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK,
                          oplib_gpu_as_stage_pool(&g_as, sass, sizeof(sass), &sp));
    const uint8_t *pool = sp.pool_cpu;
    TEST_ASSERT_EQUAL_MEMORY(sass, pool + OPLIB_POOL_OFF_SASS, sizeof(sass));
    /* Allocator handed out 0xA5 garbage: everything past the SASS is
     * zeroed, including the scratch slots. */
    TEST_ASSERT_EQUAL_HEX8(0, pool[sizeof(sass)]);
    TEST_ASSERT_EQUAL_HEX8(0, pool[OPLIB_POOL_SLOT_BYTES - 1u]);
    TEST_ASSERT_EQUAL_HEX8(0, pool[OPLIB_POOL_OFF_SCRATCH2 + 17u]);
    TEST_ASSERT_EQUAL_HEX8(0, pool[OPLIB_POOL_MIN_BYTES - 1u]);
    /* One contiguous VA range over the whole 256 KB pool, so
     * base + OPLIB_POOL_OFF_SCRATCH<N> is valid on both CPU and GPU. */
    TEST_ASSERT_TRUE(maps_linear(sp.pool_gpu_va, sp.pool_phys, OPLIB_POOL_MIN_BYTES));
    TEST_ASSERT_EQUAL_HEX64(sp.pool_phys + OPLIB_POOL_OFF_SCRATCH1 + 0x123u,
                            ga10b_vm_lookup(&g_ops, &g_vm,
                                            sp.pool_gpu_va + OPLIB_POOL_OFF_SCRATCH1 + 0x123u));

    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, oplib_gpu_as_stage_cbuf(&g_as, &sp));
    TEST_ASSERT_NOT_EQUAL(0, sp.cbuf_gpu_va);
    TEST_ASSERT_TRUE(sp.cbuf_gpu_va >= sp.pool_gpu_va + OPLIB_POOL_MIN_BYTES);
    TEST_ASSERT_TRUE(maps_linear(sp.cbuf_gpu_va, sp.cbuf_phys, 4096u));
    TEST_ASSERT_EQUAL_HEX8(0, ((const uint8_t *)sp.cbuf_cpu)[0]);
    TEST_ASSERT_EQUAL_HEX8(0, ((const uint8_t *)sp.cbuf_cpu)[4095]);
}

static void test_stage_pool_limits(void)
{
    reset();
    static uint8_t big[OPLIB_POOL_SLOT_BYTES + 1u];
    struct oplib_staged_pool sp;
    memset(&sp, 0, sizeof(sp));
    unsigned before = g_allocs;
    /* Rejected before the buffer is read, so `big` can be short. */
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_TOO_BIG,
                          oplib_gpu_as_stage_pool(&g_as, big,
                                                  OPLIB_POOL_SASS_MAX_BYTES + 1u, &sp));
    TEST_ASSERT_EQUAL_UINT32(before, g_allocs);       /* nothing allocated */
    /* Exactly one slot gives the 256 KB helper layout. */
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK,
                          oplib_gpu_as_stage_pool(&g_as, big, OPLIB_POOL_SLOT_BYTES, &sp));
    TEST_ASSERT_EQUAL_HEX64(OPLIB_POOL_MIN_BYTES, sp.pool_bytes);
    TEST_ASSERT_EQUAL_HEX64(0, sp.scratch_off);
    /* Empty SASS is fine; NULL SASS with a length is not. */
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, oplib_gpu_as_stage_pool(&g_as, NULL, 0, &sp));
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_ARG, oplib_gpu_as_stage_pool(&g_as, NULL, 4, &sp));
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_ARG, oplib_gpu_as_stage_pool(NULL, big, 4, &sp));
    /* Out of memory. */
    g_fail_after = 0;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_NOMEM, oplib_gpu_as_stage_pool(&g_as, big, 4, &sp));
}

/* A library past one slot (all SLM ops packed: ~88 KB, #1092) spills
 * into a second SASS slot; the scratch slots move up behind it so
 * scratch writes cannot overwrite SASS, and the scratch base keeps
 * `base + OPLIB_POOL_OFF_SCRATCH<N>` addressing. */
static void test_stage_pool_multi_slot_sass(void)
{
    reset();
    static uint8_t sass[88192];
    for (size_t i = 0; i < sizeof(sass); i++) {
        sass[i] = (uint8_t)(i * 13u + 5u);
    }
    TEST_ASSERT_EQUAL_UINT32(1u, oplib_pool_sass_slots(0));
    TEST_ASSERT_EQUAL_UINT32(1u, oplib_pool_sass_slots(OPLIB_POOL_SLOT_BYTES));
    TEST_ASSERT_EQUAL_UINT32(2u, oplib_pool_sass_slots(OPLIB_POOL_SLOT_BYTES + 1u));
    TEST_ASSERT_EQUAL_UINT32(2u, oplib_pool_sass_slots(sizeof(sass)));

    struct oplib_staged_pool sp;
    memset(&sp, 0, sizeof(sp));
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK,
                          oplib_gpu_as_stage_pool(&g_as, sass, sizeof(sass), &sp));
    TEST_ASSERT_EQUAL_HEX64(5u * OPLIB_POOL_SLOT_BYTES, sp.pool_bytes);
    TEST_ASSERT_EQUAL_HEX64(OPLIB_POOL_SLOT_BYTES, sp.scratch_off);
    const uint8_t *pool = sp.pool_cpu;
    TEST_ASSERT_EQUAL_MEMORY(sass, pool, sizeof(sass));
    /* First scratch slot starts after the last SASS byte, and the
     * last scratch slot ends at the end of the pool. */
    uint64_t s0 = sp.scratch_off + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t s2_end = sp.scratch_off + OPLIB_POOL_OFF_SCRATCH2 + OPLIB_POOL_SLOT_BYTES;
    TEST_ASSERT_TRUE(s0 >= sizeof(sass));
    TEST_ASSERT_EQUAL_HEX64(sp.pool_bytes, s2_end);
    TEST_ASSERT_EQUAL_HEX8(0, pool[sizeof(sass)]);
    TEST_ASSERT_EQUAL_HEX8(0, pool[sp.pool_bytes - 1u]);
    TEST_ASSERT_TRUE(maps_linear(sp.pool_gpu_va, sp.pool_phys, sp.pool_bytes));
}

/* oplib_pool_stage_with_as + oplib_pool_scratch_base with the embedded
 * library in this address space: the scratch base it reports is the
 * pool plus oplib_pool_scratch_off(SASS length), so scratch never
 * overlaps SASS, and a failed staging is retried from scratch. With the
 * stub library (no SASS extracted) there is nothing to stage. */
static void test_pool_stage_with_as_scratch_base(void)
{
    reset();
    TEST_ASSERT_EQUAL_INT(0, oplib_pool_init());
    size_t len = oplib_pool_sass_region_len();
    oplib_pool_unstage();
    uint64_t phys = 0, va = 0;

    if (len == 0) {
        TEST_ASSERT_EQUAL_INT(0, oplib_pool_stage_with_as(&g_as));
        TEST_ASSERT_EQUAL_INT(-1, oplib_pool_scratch_base(&phys, &va));
        oplib_pool_unstage();
        return;
    }

    /* First attempt: the pool allocation fails. */
    g_fail_after = 0;
    TEST_ASSERT_TRUE(oplib_pool_stage_with_as(&g_as) < 0);
    TEST_ASSERT_EQUAL_INT(-1, oplib_pool_scratch_base(&phys, &va));
    /* Retry succeeds. */
    g_fail_after = -1;
    unsigned wr_before = g_wr32s;
    TEST_ASSERT_EQUAL_INT(0, oplib_pool_stage_with_as(&g_as));
    TEST_ASSERT_TRUE(g_wr32s > wr_before);            /* TLB invalidated */
    TEST_ASSERT_EQUAL_INT(0, oplib_pool_scratch_base(&phys, &va));
    uint64_t off = oplib_pool_scratch_off(len);
    TEST_ASSERT_EQUAL_HEX64(oplib_pool_base_phys() + off, phys);
    TEST_ASSERT_EQUAL_HEX64(oplib_pool_gpu_va_base() + off, va);
    TEST_ASSERT_TRUE(va + OPLIB_POOL_OFF_SCRATCH0 >= oplib_pool_gpu_va_base() + len);
    TEST_ASSERT_TRUE(maps_linear(va + OPLIB_POOL_OFF_SCRATCH0, phys + OPLIB_POOL_OFF_SCRATCH0,
                                 3u * OPLIB_POOL_SLOT_BYTES));
    oplib_pool_unstage();
}

/* ---- Scripted backend: map always fails ---- */

static void *s_alloc(void *ctx, uint32_t n, uint64_t *phys) { return f_alloc(ctx, n, phys); }
static int s_map_fail(void *ctx, uint64_t phys, uint64_t bytes, uint64_t *va)
{
    (void)ctx; (void)phys; (void)bytes; (void)va;
    return OPLIB_AS_ERR_MAP;
}
static void s_clean(void *ctx, const void *p, size_t n) { f_clean(ctx, p, n); }

static void test_failed_map_frees_only_when_safe(void)
{
    reset();
    struct oplib_gpu_as as = {
        .name = "fake", .alloc = s_alloc, .free = f_free,
        .map = s_map_fail, .clean = s_clean,
    };
    struct oplib_staged_pool sp;
    memset(&sp, 0, sizeof(sp));
    uint8_t sass[16] = { 1 };
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_MAP, oplib_gpu_as_stage_pool(&as, sass, 16, &sp));
    TEST_ASSERT_EQUAL_UINT32(1, g_frees);
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_MAP, oplib_gpu_as_stage_cbuf(&as, &sp));
    TEST_ASSERT_EQUAL_UINT32(2, g_frees);
    TEST_ASSERT_EQUAL_HEX64(0, sp.pool_gpu_va);       /* nothing published */
    TEST_ASSERT_EQUAL_HEX64(0, sp.cbuf_gpu_va);

    /* free == NULL (cold backend semantics): pages are leaked. */
    as.free = NULL;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_MAP, oplib_gpu_as_stage_pool(&as, sass, 16, &sp));
    TEST_ASSERT_EQUAL_UINT32(2, g_frees);

    /* The weights pool needs the split reserve/map_at form. */
    struct oplib_wpool w;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_UNSUPP, oplib_wpool_build(&as, 4096u, 4096u, &w));
}

static void test_qmd_pool_for_channel_only_handoff(void)
{
    reset();
    struct ga10b_channel_handoff h;
    memset(&h, 0, sizeof(h));
    h.version = 2;
    TEST_ASSERT_EQUAL_INT(1, oplib_gpu_as_ensure_qmd_pool(&g_as, &h));
    TEST_ASSERT_EQUAL_UINT32(OPLIB_QMD_POOL_SLOTS, h.qmd_pool_n_slots);
    TEST_ASSERT_EQUAL_UINT32(OPLIB_QMD_POOL_SLOTS * OPLIB_QMD_POOL_SLOT_BYTES,
                             h.qmd_pool_size_bytes);
    /* ga10b_dispatch_v7_pipeline's size check: size >= slots * 256. */
    TEST_ASSERT_TRUE(h.qmd_pool_size_bytes >= h.qmd_pool_n_slots * 256u);
    TEST_ASSERT_TRUE(maps_linear(h.qmd_pool_gpu_va, h.qmd_pool_phys, h.qmd_pool_size_bytes));
    TEST_ASSERT_TRUE(h.qmd_pool_gpu_va < (1ull << 40));
    const uint8_t *q = f_p2c(NULL, h.qmd_pool_phys);
    TEST_ASSERT_EQUAL_HEX8(0, q[0]);
    TEST_ASSERT_EQUAL_HEX8(0, q[h.qmd_pool_size_bytes - 1u]);
    TEST_ASSERT_EQUAL_UINT32(2, h.version);           /* version untouched */

    /* A handoff that already has one (MNIST v7) is left alone. */
    struct ga10b_channel_handoff before = h;
    unsigned allocs = g_allocs;
    TEST_ASSERT_EQUAL_INT(0, oplib_gpu_as_ensure_qmd_pool(&g_as, &h));
    TEST_ASSERT_EQUAL_MEMORY(&before, &h, sizeof(h));
    TEST_ASSERT_EQUAL_UINT32(allocs, g_allocs);
}

/* Cold backend whose allocator leaves a one-page hole after every
 * allocation, so weights-pool chunks are never physically adjacent. */
static void *gap_alloc(void *ctx, uint32_t n, uint64_t *phys)
{
    void *p = f_alloc(ctx, n, phys);
    uint64_t gap;
    if (p) (void)f_alloc(ctx, 1, &gap);
    return p;
}

static void test_wpool_chunks_and_write(void)
{
    reset();
    struct oplib_gpu_as as = g_as;
    as.alloc = gap_alloc;
    const uint64_t chunk = 8u * 4096u;

    struct oplib_wpool w;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, oplib_wpool_build(&as, 3u * chunk + 100u, chunk, &w));
    TEST_ASSERT_EQUAL_UINT32(4, w.n_chunks);           /* rounded up */
    TEST_ASSERT_EQUAL_HEX64(4u * chunk, w.size);
    TEST_ASSERT_TRUE(w.gpu_va >= GA10B_VM_USER_BASE);
    TEST_ASSERT_EQUAL_HEX64(0, w.gpu_va % chunk);      /* chunk-aligned */
    for (uint32_t c = 0; c < w.n_chunks; c++) {
        TEST_ASSERT_TRUE(maps_linear(w.gpu_va + c * chunk, w.chunk[c].phys, chunk));
        if (c) TEST_ASSERT_NOT_EQUAL(w.chunk[c - 1].phys + chunk, w.chunk[c].phys);
    }

    /* Write straddling chunk 0 -> 1 -> 2 and check it through the GPU
     * page tables (VA -> phys -> CPU) rather than the pool's own map. */
    static uint8_t src[2u * 8u * 4096u + 300u];
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)(i ^ (i >> 8));
    }
    uint64_t dst = w.gpu_va + chunk - 1000u;
    unsigned cleans = g_cleans;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, oplib_wpool_write(&w, &as, dst, src, sizeof(src)));
    TEST_ASSERT_EQUAL_UINT32(cleans + 3u, g_cleans);   /* one per chunk touched */
    for (size_t i = 0; i < sizeof(src); i += 997u) {
        uint64_t pa = ga10b_vm_lookup(&g_ops, &g_vm, dst + i);
        TEST_ASSERT_NOT_EQUAL(0, pa);
        TEST_ASSERT_EQUAL_HEX8(src[i], *(const uint8_t *)f_p2c(NULL, pa));
    }
    uint64_t last = dst + sizeof(src) - 1u;
    TEST_ASSERT_EQUAL_HEX8(src[sizeof(src) - 1u],
                           *(const uint8_t *)f_p2c(NULL, ga10b_vm_lookup(&g_ops, &g_vm, last)));

    /* translate */
    uint64_t run = 0;
    TEST_ASSERT_EQUAL_PTR((uint8_t *)w.chunk[1].cpu + 5u,
                          oplib_wpool_translate(&w, w.gpu_va + chunk + 5u, &run));
    TEST_ASSERT_EQUAL_HEX64(chunk - 5u, run);
    TEST_ASSERT_NULL(oplib_wpool_translate(&w, w.gpu_va - 1u, &run));
    TEST_ASSERT_NULL(oplib_wpool_translate(&w, w.gpu_va + w.size, &run));

    /* Range checks: last byte ok, one past the end rejected, before base
     * rejected, length overflow rejected. */
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, oplib_wpool_write(&w, &as, w.gpu_va + w.size - 1u, src, 1));
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_ARG, oplib_wpool_write(&w, &as, w.gpu_va + w.size - 1u, src, 2));
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_ARG, oplib_wpool_write(&w, &as, w.gpu_va - 4096u, src, 1));
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_ARG, oplib_wpool_write(&w, &as, w.gpu_va + w.size + 4096u, src, 1));
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_ARG, oplib_wpool_write(&w, &as, w.gpu_va, src, 0));
}

static void test_wpool_partial_and_capped(void)
{
    reset();
    const uint64_t chunk = 4u * 4096u;
    struct oplib_wpool w;

    /* Allocator runs dry after two chunks: a smaller pool, no error. */
    g_fail_after = 2;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, oplib_wpool_build(&g_as, 5u * chunk, chunk, &w));
    TEST_ASSERT_EQUAL_UINT32(2, w.n_chunks);
    TEST_ASSERT_EQUAL_HEX64(2u * chunk, w.size);
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_ops, &g_vm, w.gpu_va + 2u * chunk));

    /* None at all. */
    g_fail_after = 0;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_NOMEM, oplib_wpool_build(&g_as, chunk, chunk, &w));

    /* Request beyond OPLIB_WPOOL_MAX_CHUNKS is capped. */
    g_fail_after = -1;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK,
                          oplib_wpool_build(&g_as, 1000u * 2u * 4096u, 2u * 4096u, &w));
    TEST_ASSERT_EQUAL_UINT32(OPLIB_WPOOL_MAX_CHUNKS, w.n_chunks);
    TEST_ASSERT_EQUAL_HEX64(OPLIB_WPOOL_MAX_CHUNKS * 2ull * 4096u, w.size);

    /* Bad arguments. */
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_ARG, oplib_wpool_build(&g_as, 0, chunk, &w));
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_ARG, oplib_wpool_build(&g_as, chunk, 100u, &w));
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_ARG, oplib_wpool_build(NULL, chunk, chunk, &w));
}

int test_suite_oplib_gpu_as(void)
{
    UnityBegin("test_oplib_gpu_as.c");
    RUN_TEST(test_cold_backend_maps_into_vm);
    RUN_TEST(test_stage_pool_layout);
    RUN_TEST(test_stage_pool_limits);
    RUN_TEST(test_stage_pool_multi_slot_sass);
    RUN_TEST(test_pool_stage_with_as_scratch_base);
    RUN_TEST(test_failed_map_frees_only_when_safe);
    RUN_TEST(test_qmd_pool_for_channel_only_handoff);
    RUN_TEST(test_wpool_chunks_and_write);
    RUN_TEST(test_wpool_partial_and_capped);
    return UnityEnd();
}
