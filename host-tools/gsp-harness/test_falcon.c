/*
 * test_falcon.c — regression tests for the shared Falcon driver
 * (kernel/gpu/nvidia/falcon.c).
 *
 * Mocks the `gsp_platform` vtable with a 16 MB byte buffer that
 * simulates BAR0. All Falcon ops route reads/writes through the
 * mock — letting us exercise the full protocol (reset, scrub-poll,
 * DMA register sequence, halt detection) without a real GPU.
 *
 * Wired into `make test-falcon` for CI.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/gpu/nvidia/falcon.h"
#include "../../kernel/gpu/nvidia/gsp.h"

/* ---- Mock BAR0 ----
 *
 * 16 MB backing buffer exposed through the gsp_platform vtable.
 * Most offsets are plain RAM — read-your-writes. A tiny list of
 * "smart" offsets have side-effects to model hardware bits like
 * ENGINE.RESET (self-clearing), CPUCTL.STARTCPU (sticky → HALTED).
 *
 * Mocks only cover the subset of Falcon behavior the tests need.
 * When they don't cover something, reads return 0 and writes are
 * silently dropped.
 */

#define MOCK_BAR0_SIZE  (16u * 1024u * 1024u)
static uint32_t g_bar0[MOCK_BAR0_SIZE / 4];

/* Per-engine mock state. Indexed by engine base so one global
 * BAR0 can host multiple Falcons simultaneously. */
struct mock_engine {
    uint32_t base;           /* engine base */
    bool     present;        /* false → reads return 0xFFFFFFFF */
    bool     scrubbing;      /* HWCFG2.MEM_SCRUBBING emulation */
    uint32_t scrub_steps;    /* decrements each HWCFG2 read; 0 → cleared */
    uint32_t imem_size;
    uint32_t dmem_size;
    bool     has_riscv;
    bool     cpu_running;    /* false = halted */
    uint32_t last_bootvec;
    uint32_t dma_steps;      /* decrements each DMATRFCMD poll; 0 → IDLE */
    /* Capture of DMA commands the driver issued — tests assert on these. */
    uint32_t dma_issued;
    uint64_t last_dma_base;
    uint32_t last_dma_moffs;
    uint32_t last_dma_fboffs;
    uint32_t last_dma_cmd;
};

static struct mock_engine g_engines[4];
static uint32_t g_num_engines;

static struct mock_engine *engine_for(uint32_t full_addr)
{
    for (uint32_t i = 0; i < g_num_engines; i++) {
        uint32_t base = g_engines[i].base;
        if (full_addr >= base && full_addr < base + 0x1000u)
            return &g_engines[i];
    }
    return NULL;
}

static uint32_t mock_read32(uint32_t addr)
{
    struct mock_engine *e = engine_for(addr);
    if (e && !e->present) return 0xFFFFFFFFu;

    if (e) {
        uint32_t off = addr - e->base;
        switch (off) {
        case FALCON_CPUCTL:
            return e->cpu_running ? 0 : FALCON_CPUCTL_HALTED;
        case FALCON_HWCFG: {
            uint32_t imem_blk = e->imem_size / FALCON_DMA_CHUNK;
            uint32_t dmem_blk = e->dmem_size / FALCON_DMA_CHUNK;
            return (imem_blk & FALCON_HWCFG_IMEM_SIZE_MASK) |
                   ((dmem_blk << FALCON_HWCFG_DMEM_SIZE_SHIFT)
                    & FALCON_HWCFG_DMEM_SIZE_MASK);
        }
        case FALCON_HWCFG2: {
            /* Scrub countdown: each read decrements scrub_steps;
             * clears scrubbing when it hits zero. */
            if (e->scrubbing) {
                if (e->scrub_steps > 0) e->scrub_steps--;
                if (e->scrub_steps == 0) e->scrubbing = false;
            }
            uint32_t v = 0;
            if (e->has_riscv)  v |= FALCON_HWCFG2_RISCV_ENABLE;
            if (e->scrubbing)  v |= FALCON_HWCFG2_MEM_SCRUBBING;
            return v;
        }
        case FALCON_DMATRFCMD: {
            if (e->dma_steps > 0) {
                e->dma_steps--;
                return 0;    /* not idle yet */
            }
            return FALCON_DMATRFCMD_IDLE;
        }
        case FALCON_DMACTL:
            return 0;
        }
    }
    return g_bar0[addr / 4];
}

static void mock_write32(uint32_t addr, uint32_t v)
{
    struct mock_engine *e = engine_for(addr);
    g_bar0[addr / 4] = v;

    if (e) {
        uint32_t off = addr - e->base;
        switch (off) {
        case FALCON_ENGINE:
            if (v & FALCON_ENGINE_RESET) {
                e->scrubbing = true;
                e->scrub_steps = 3;
                e->cpu_running = false;
                e->dma_steps = 0;
            }
            break;
        case FALCON_CPUCTL:
            if (v & FALCON_CPUCTL_STARTCPU) e->cpu_running = true;
            break;
        case FALCON_BOOTVEC:
            e->last_bootvec = v;
            break;
        case FALCON_DMATRFBASE:
            e->last_dma_base = (e->last_dma_base & 0xFFFFFF0000000000ull)
                             | ((uint64_t)v << 8);
            break;
        case FALCON_DMATRFBASE1:
            e->last_dma_base = (e->last_dma_base & 0x000000FFFFFFFFFFull)
                             | ((uint64_t)v << 40);
            break;
        case FALCON_DMATRFMOFFS:
            e->last_dma_moffs = v;
            break;
        case FALCON_DMATRFFBOFFS:
            e->last_dma_fboffs = v;
            break;
        case FALCON_DMATRFCMD:
            e->last_dma_cmd = v;
            e->dma_issued++;
            /* Complete after a couple of polls — tests that expect a
             * specific iteration count can tweak dma_steps beforehand. */
            e->dma_steps = 2;
            break;
        }
    }
}

static void mock_bar1_read(uint32_t off, void *dst, size_t n)
{
    (void)off; (void)dst; (void)n;
}
static void mock_bar1_write(uint32_t off, const void *src, size_t n)
{
    (void)off; (void)src; (void)n;
}
static void *mock_dma_alloc(size_t s, size_t a, uint64_t *p)
{
    (void)s; (void)a; if (p) *p = 0; return NULL;
}
static void mock_dma_free(void *p, size_t s) { (void)p; (void)s; }
static void mock_cache_clean(const void *a, size_t s) { (void)a; (void)s; }
static void mock_cache_inval(void *a, size_t s) { (void)a; (void)s; }
static void mock_mb(void) { /* no-op */ }
static void mock_firmware_get(enum gsp_firmware_kind k,
                              struct gsp_firmware_blob *out)
{
    (void)k;
    if (out) { out->data = NULL; out->size = 0; out->version = NULL; }
}
static int mock_vbios_get_fwsec(const void **d, size_t *s)
{
    if (d) *d = NULL;
    if (s) *s = 0;
    return -1;
}

static const struct gsp_platform_ops mock_ops = {
    .read32           = mock_read32,
    .write32          = mock_write32,
    .bar1_read        = mock_bar1_read,
    .bar1_write       = mock_bar1_write,
    .dma_alloc        = mock_dma_alloc,
    .dma_free         = mock_dma_free,
    .cache_clean      = mock_cache_clean,
    .cache_invalidate = mock_cache_inval,
    .mb               = mock_mb,
    .firmware_get     = mock_firmware_get,
    .vbios_get_fwsec  = mock_vbios_get_fwsec,
};

extern const struct gsp_platform_ops *gsp_platform;

static struct mock_engine *add_engine(uint32_t base, uint32_t imem, uint32_t dmem,
                                      bool riscv)
{
    struct mock_engine *e = &g_engines[g_num_engines++];
    memset(e, 0, sizeof(*e));
    e->base = base;
    e->present = true;
    e->imem_size = imem;
    e->dmem_size = dmem;
    e->has_riscv = riscv;
    e->cpu_running = false;
    return e;
}

static void reset_mock(void)
{
    memset(g_bar0, 0, sizeof(g_bar0));
    memset(g_engines, 0, sizeof(g_engines));
    g_num_engines = 0;
    gsp_platform = &mock_ops;
}

/* ---- Tests ---- */

static int failures;

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void test_probe_gsp_falcon(void)
{
    reset_mock();
    add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    REQUIRE(f.initialized);
    REQUIRE(f.imem_size == 0x10000);
    REQUIRE(f.dmem_size == 0x10000);
    REQUIRE(f.has_riscv);
}

static void test_probe_sec2_falcon(void)
{
    reset_mock();
    add_engine(NV_PSEC2_BASE, 0xC000, 0x8000, false);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    REQUIRE(f.initialized);
    REQUIRE(f.imem_size == 0xC000);
    REQUIRE(f.dmem_size == 0x8000);
    REQUIRE(!f.has_riscv);     /* SEC2 has BROM, not full RISC-V aperture */
}

static void test_probe_rejects_offdie(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);
    e->present = false;         /* reads return 0xFFFFFFFF */

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) < 0);
    REQUIRE(!f.initialized);
}

static void test_probe_rejects_zero_hwcfg(void)
{
    reset_mock();
    add_engine(NV_PGSP_BASE, 0, 0, false);   /* imem_size=0 → hwcfg = 0 */

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) < 0);
}

static void test_reset_completes_after_scrub(void)
{
    reset_mock();
    add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    REQUIRE(falcon_reset(&f) == 0);
}

static void test_wait_halted_succeeds_when_halted(void)
{
    reset_mock();
    add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    /* Default state: cpu_running = false → reads HALTED. */
    REQUIRE(falcon_wait_halted(&f, 1000) == 0);
}

static void test_wait_halted_times_out(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);
    e->cpu_running = true;       /* CPU always running → never halts */

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    REQUIRE(falcon_wait_halted(&f, 100) < 0);
}

static void test_start_writes_bootvec_and_starts(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    falcon_start(&f, 0xDEADBEEFu);
    REQUIRE(e->last_bootvec == 0xDEADBEEFu);
    REQUIRE(e->cpu_running);
}

static void test_dma_upload_imem_shape(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    /* 1 KB / 256-byte chunks = 4 chunks. */
    uint64_t src = 0x0000000080000000ull;
    REQUIRE(falcon_dma_upload(&f, src, 0x200, 0x400, true) == 0);
    REQUIRE(e->dma_issued == 4);
    REQUIRE(e->last_dma_base == src);
    REQUIRE(e->last_dma_cmd & FALCON_DMATRFCMD_IMEM);
    REQUIRE(e->last_dma_cmd & FALCON_DMATRFCMD_WRITE);
}

static void test_dma_upload_dmem_shape(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    REQUIRE(falcon_dma_upload(&f, 0x80000000ull, 0, 0x100, false) == 0);
    REQUIRE((e->last_dma_cmd & FALCON_DMATRFCMD_IMEM) == 0);
    REQUIRE(e->last_dma_cmd & FALCON_DMATRFCMD_WRITE);
}

static void test_dma_rejects_misaligned(void)
{
    reset_mock();
    add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    /* offset not multiple of 256 */
    REQUIRE(falcon_dma_upload(&f, 0x80000000ull, 0x123, 0x100, true) < 0);
    /* length not multiple of 256 */
    REQUIRE(falcon_dma_upload(&f, 0x80000000ull, 0x100, 0x123, true) < 0);
}

static void test_dma_rejects_overflow(void)
{
    reset_mock();
    add_engine(NV_PGSP_BASE, 0x1000, 0x1000, true);   /* 4 KB IMEM */

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    /* Upload that extends past IMEM end. */
    REQUIRE(falcon_dma_upload(&f, 0x80000000ull, 0xE00, 0x400, true) < 0);
}

static void test_dma_handles_40bit_addr(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    /* 40-bit IOVA uses DMATRFBASE1 for bits 39:32. Source at
     * 0x0000009000000000 should split:
     *   DMATRFBASE  = (0x9000000000 >> 8) & 0xFFFFFFFF = 0x90000000
     *   DMATRFBASE1 = (0x9000000000 >> 40)             = 0 on 40-bit
     * For a 40-bit IOVA 0x0000FF_80000000:
     *   >> 8  = 0xFF_8000_0000 → u32 truncates to 0x80000000, hi to 0xFF */
    uint64_t iova = 0x000000FF80000000ull;
    REQUIRE(falcon_dma_upload(&f, iova, 0, 0x100, true) == 0);
    REQUIRE(e->last_dma_base == iova);
}

static void test_is_idle_after_probe(void)
{
    reset_mock();
    add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    REQUIRE(falcon_is_idle(&f));
}

static void test_is_idle_rejects_cpu_running(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);
    e->cpu_running = true;

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    REQUIRE(!falcon_is_idle(&f));
}

static void test_uninitialized_rejects(void)
{
    reset_mock();
    struct falcon f = { 0 };    /* not probed */
    REQUIRE(falcon_reset(&f) < 0);
    REQUIRE(falcon_wait_halted(&f, 100) < 0);
    REQUIRE(falcon_dma_upload(&f, 0, 0, 0x100, true) < 0);
    REQUIRE(!falcon_is_idle(&f));
    /* start() is declared void but should no-op on uninitialized. */
    falcon_start(&f, 0);
}

int main(void)
{
    test_probe_gsp_falcon();
    test_probe_sec2_falcon();
    test_probe_rejects_offdie();
    test_probe_rejects_zero_hwcfg();
    test_reset_completes_after_scrub();
    test_wait_halted_succeeds_when_halted();
    test_wait_halted_times_out();
    test_start_writes_bootvec_and_starts();
    test_dma_upload_imem_shape();
    test_dma_upload_dmem_shape();
    test_dma_rejects_misaligned();
    test_dma_rejects_overflow();
    test_dma_handles_40bit_addr();
    test_is_idle_after_probe();
    test_is_idle_rejects_cpu_running();
    test_uninitialized_rejects();

    if (failures == 0) {
        printf("test_falcon: all tests PASS\n");
        return 0;
    }
    printf("test_falcon: %d FAIL\n", failures);
    return 1;
}
