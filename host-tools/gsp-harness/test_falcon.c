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
    uint32_t halt_after;     /* CPUCTL-reads until auto-halt; 0 = never */
    uint32_t last_bootvec;
    uint32_t dma_steps;      /* decrements each DMATRFCMD poll; 0 → IDLE */
    /* Capture of DMA commands the driver issued — tests assert on these. */
    uint32_t dma_issued;
    uint64_t last_dma_base;
    uint32_t last_dma_moffs;
    uint32_t last_dma_fboffs;
    uint32_t last_dma_cmd;

    /* CPUCTL.ALIAS_EN simulation. When true, reads of CPUCTL set
     * the ALIAS_EN bit (mocking the BROM-handed-off state). The
     * driver must then route STARTCPU through CPUCTL_ALIAS rather
     * than CPUCTL — we capture which path was actually used. */
    bool     alias_en;
    uint32_t startcpu_via_cpuctl;       /* count of writes to 0x100 */
    uint32_t startcpu_via_cpuctl_alias; /* count of writes to 0x130 */

    /* PRI-arbiter priv-lock simulation. When true, CPUCTL reads
     * return NVIDIA's 0xbadfXXXX poison pattern instead of real
     * state — exactly what the GA107 SEC2 engine does under VFIO
     * after BSI re-applies DEVINIT. Lets tests exercise
     * falcon_is_priv_locked() and the wait_halted early-bail. */
    bool     priv_locked;

    /* PIO IMEM/DMEM upload capture — a few hundred 4-byte slots so
     * tests can read back what the driver streamed through the data
     * port. The mock auto-increments because IMEMC/DMEMC AINCW=1. */
    uint32_t imem_pio_ctrl;
    uint32_t imem_pio_tag;
    uint32_t imem_pio_words[1024];
    uint32_t imem_pio_count;
    uint32_t dmem_pio_ctrl;
    uint32_t dmem_pio_words[1024];
    uint32_t dmem_pio_count;

    /* Pre-PIO setup capture: the driver should mask-set 0x624 bit 7
     * and clear DMACTL (0x10c). */
    uint32_t r0x624_after_setup;
    bool     dmactl_cleared_in_pre_pio;
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
        case FALCON_CPUCTL: {
            if (e->priv_locked) return 0xbadf5620u;
            if (e->cpu_running && e->halt_after > 0) {
                e->halt_after--;
                if (e->halt_after == 0) e->cpu_running = false;
            }
            uint32_t v = e->cpu_running ? 0 : FALCON_CPUCTL_HALTED;
            if (e->alias_en) v |= FALCON_CPUCTL_ALIAS_EN;
            return v;
        }
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
            if (v & FALCON_CPUCTL_STARTCPU) {
                e->cpu_running = true;
                e->startcpu_via_cpuctl++;
            }
            break;
        case FALCON_CPUCTL_ALIAS:
            if (v & FALCON_CPUCTL_STARTCPU) {
                e->cpu_running = true;
                e->startcpu_via_cpuctl_alias++;
            }
            break;
        case FALCON_DMACTL:
            if (v == 0) e->dmactl_cleared_in_pre_pio = true;
            break;
        /* Pre-PIO/DMA setup capture: 0x624 mask-set bit 7. */
        case 0x624:
            e->r0x624_after_setup = v;
            break;
        /* PIO IMEM port 0 — three registers (IMEMC/IMEMT/IMEMD)
         * followed by a stream of u32 writes to IMEMD. AINCW=1 auto-
         * increments, so we just append each IMEMD write. */
        case FALCON_IMEMC(0):
            e->imem_pio_ctrl = v;
            break;
        case FALCON_IMEMT(0):
            e->imem_pio_tag = v;
            break;
        case FALCON_IMEMD(0):
            if (e->imem_pio_count <
                sizeof(e->imem_pio_words) / sizeof(e->imem_pio_words[0]))
                e->imem_pio_words[e->imem_pio_count++] = v;
            break;
        /* PIO DMEM port 0. */
        case FALCON_DMEMC(0):
            e->dmem_pio_ctrl = v;
            break;
        case FALCON_DMEMD(0):
            if (e->dmem_pio_count <
                sizeof(e->dmem_pio_words) / sizeof(e->dmem_pio_words[0]))
                e->dmem_pio_words[e->dmem_pio_count++] = v;
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

static void test_hs_boot_programs_brom_and_starts(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);
    /* Simulate ucode execution — halts after a few CPUCTL polls. */
    e->halt_after = 5;

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    REQUIRE(falcon_hs_boot(&f, NV_PSEC2_BROM_BASE,
                           /*dmem_sign*/ 0x200,
                           /*engine_id*/ 0x01,
                           /*ucode_id*/  3,
                           /*boot_vec*/  0x100,
                           /*timeout*/   10000) == 0);

    /* BROM register writes landed on the absolute BAR0 addresses for SEC2. */
    REQUIRE(g_bar0[(NV_PSEC2_BROM_BASE + FALCON_BROM_PARAADDR0) / 4] == 0x200);
    REQUIRE(g_bar0[(NV_PSEC2_BROM_BASE + FALCON_BROM_ENGIDMASK) / 4] == 0x01);
    REQUIRE(g_bar0[(NV_PSEC2_BROM_BASE + FALCON_BROM_UCODE_ID) / 4]  == 3);
    REQUIRE(g_bar0[(NV_PSEC2_BROM_BASE + FALCON_BROM_MOD_SEL) / 4]
            == FALCON_BROM_MOD_SEL_RSA3K);
    REQUIRE(e->last_bootvec == 0x100);
    /* CPU has halted after the simulated execution. */
    REQUIRE(!e->cpu_running);
}

static void test_hs_boot_times_out_when_never_halts(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);
    e->halt_after = 0;    /* never halts */
    (void)e;

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    REQUIRE(falcon_hs_boot(&f, NV_PSEC2_BROM_BASE,
                           0x200, 0x01, 3, 0x100,
                           /*timeout*/ 200) < 0);
}

static void test_hs_boot_rejects_non_idle(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);
    e->cpu_running = true;   /* engine in use — hs_boot must refuse */

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    REQUIRE(falcon_hs_boot(&f, NV_PSEC2_BROM_BASE, 0, 0, 0, 0, 100) < 0);
}

/* ---- ALIAS_EN routing test (regression for the BROM-handoff bug) ----
 *
 * After the BROM finishes verifying an HS ucode, CPUCTL.ALIAS_EN
 * (bit 6) is set and STARTCPU writes to CPUCTL (0x100) are gated.
 * The driver must route through CPUCTL_ALIAS (0x130) instead.
 * This is the second of two bugs found in the FWSEC audit.
 */
static void test_start_uses_cpuctl_alias_when_en_set(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);
    e->alias_en = true;     /* mock the BROM-handed-off state */

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    falcon_start(&f, 0x42);
    /* Driver must have written STARTCPU to CPUCTL_ALIAS, not CPUCTL. */
    REQUIRE(e->startcpu_via_cpuctl_alias == 1);
    REQUIRE(e->startcpu_via_cpuctl == 0);
    REQUIRE(e->last_bootvec == 0x42);
}

static void test_start_uses_cpuctl_when_alias_clear(void)
{
    /* Existing path: ALIAS_EN clear → write CPUCTL as before. The
     * stock test_start_writes_bootvec_and_starts already covers
     * this, but we re-assert here to make the contract symmetric. */
    reset_mock();
    struct mock_engine *e = add_engine(NV_PGSP_BASE, 0x10000, 0x10000, true);
    /* alias_en defaults false. */

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PGSP_BASE) == 0);
    falcon_start(&f, 0x100);
    REQUIRE(e->startcpu_via_cpuctl == 1);
    REQUIRE(e->startcpu_via_cpuctl_alias == 0);
}

/* ---- pre_pio_setup test ---- */

static void test_pre_pio_setup_writes_correct_regs(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    /* Pre-seed 0x624 with bit 0 set; pre_pio_setup must mask-set
     * bit 7 without clearing other bits. */
    g_bar0[(NV_PSEC2_BASE + 0x624) / 4] = 0x01;
    falcon_pre_pio_setup(&f);
    REQUIRE(e->r0x624_after_setup == 0x81);     /* 0x01 | 0x80 */
    REQUIRE(e->dmactl_cleared_in_pre_pio);
}

/* ---- PIO upload tests ----
 *
 * Cover alignment + bounds rejection, the IMEMC/IMEMT/IMEMD register
 * sequence, the SECURE bit, and that bytes stream through in the
 * declared little-endian-u32 order.
 */

static void test_pio_imem_rejects_misaligned_offset(void)
{
    reset_mock();
    add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    uint8_t src[16] = { 0 };
    /* Specific INVAL code so debugging "PIO upload failed" is one
     * register away from "bad caller-side alignment" vs other modes. */
    REQUIRE(falcon_pio_upload_imem(&f, src, sizeof(src), 0x123, false)
            == GSP_ERR_INVAL);
}

static void test_pio_imem_rejects_misaligned_length(void)
{
    reset_mock();
    add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    uint8_t src[10] = { 0 };
    REQUIRE(falcon_pio_upload_imem(&f, src, 10, 0, false) == GSP_ERR_INVAL);
}

static void test_pio_imem_rejects_overflow(void)
{
    reset_mock();
    add_engine(NV_PSEC2_BASE, 0x1000, 0x1000, false);   /* 4 KB IMEM */

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    uint8_t src[16] = { 0 };
    /* Tail past IMEM end. */
    REQUIRE(falcon_pio_upload_imem(&f, src, sizeof(src), 0xFF8, false)
            == GSP_ERR_INVAL);
}

static void test_pio_imem_writes_words_in_order(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    /* 12 bytes = 3 words, distinct LE patterns so we can confirm
     * the assembly order. */
    uint8_t src[12] = { 0x01, 0x02, 0x03, 0x04,
                         0xAA, 0xBB, 0xCC, 0xDD,
                         0xFF, 0x00, 0x55, 0xAA };
    REQUIRE(falcon_pio_upload_imem(&f, src, sizeof(src),
                                   /*falcon_off*/ 0x100,
                                   /*is_secure*/ false) == GSP_OK);
    REQUIRE(e->imem_pio_count == 3);
    REQUIRE(e->imem_pio_words[0] == 0x04030201u);
    REQUIRE(e->imem_pio_words[1] == 0xDDCCBBAAu);
    REQUIRE(e->imem_pio_words[2] == 0xAA5500FFu);
    /* IMEMC must carry AINCW + the target offset in the low 24 bits. */
    REQUIRE((e->imem_pio_ctrl & (1u << 24)) != 0);
    REQUIRE((e->imem_pio_ctrl & 0x00FFFFFFu) == 0x100);
    /* SECURE bit 28 clear because we passed false. */
    REQUIRE((e->imem_pio_ctrl & (1u << 28)) == 0);
    /* IMEMT == falcon_off >> 8 (page tag). */
    REQUIRE(e->imem_pio_tag == (0x100u >> 8));
}

static void test_pio_imem_secure_sets_bit28(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    uint8_t src[4] = { 0 };
    REQUIRE(falcon_pio_upload_imem(&f, src, 4, 0x200, /*is_secure*/ true) == GSP_OK);
    REQUIRE((e->imem_pio_ctrl & (1u << 28)) != 0);
}

static void test_pio_dmem_writes_words_in_order(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    uint8_t src[8] = { 0xDE, 0xAD, 0xBE, 0xEF,
                        0xCA, 0xFE, 0xBA, 0xBE };
    REQUIRE(falcon_pio_upload_dmem(&f, src, sizeof(src), 0x80) == GSP_OK);
    REQUIRE(e->dmem_pio_count == 2);
    REQUIRE(e->dmem_pio_words[0] == 0xEFBEADDEu);
    REQUIRE(e->dmem_pio_words[1] == 0xBEBAFECAu);
    REQUIRE((e->dmem_pio_ctrl & (1u << 24)) != 0);
    REQUIRE((e->dmem_pio_ctrl & 0x00FFFFFFu) == 0x80);
}

static void test_pio_dmem_rejects_misaligned(void)
{
    reset_mock();
    add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    uint8_t src[10] = { 0 };
    REQUIRE(falcon_pio_upload_dmem(&f, src, 10, 0) == GSP_ERR_INVAL);
    REQUIRE(falcon_pio_upload_dmem(&f, src, 8, 0x123) == GSP_ERR_INVAL);
}

static void test_pio_zero_len_succeeds(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    uint8_t src[4] = { 0 };
    REQUIRE(falcon_pio_upload_imem(&f, src, 0, 0, false) == GSP_OK);
    REQUIRE(falcon_pio_upload_dmem(&f, src, 0, 0) == GSP_OK);
    /* No port writes for zero-length uploads. */
    REQUIRE(e->imem_pio_count == 0);
    REQUIRE(e->dmem_pio_count == 0);
}

static void test_pio_uninitialized_rejects(void)
{
    reset_mock();
    struct falcon f = { 0 };    /* not probed */
    uint8_t src[4] = { 0 };
    REQUIRE(falcon_pio_upload_imem(&f, src, 4, 0, false) == GSP_ERR_INVAL);
    REQUIRE(falcon_pio_upload_dmem(&f, src, 4, 0) == GSP_ERR_INVAL);
    /* pre_pio_setup is void — must no-op cleanly. */
    falcon_pre_pio_setup(&f);
}

/* ---- falcon_hs_kick tests (BROM program + STARTCPU split) ---- */

static void test_hs_kick_programs_brom_and_starts(void)
{
    /* hs_kick is the same setup as hs_boot minus the wait_halted step;
     * verify BROM registers and STARTCPU fire without observing halt. */
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);
    e->halt_after = 0;    /* kick returns before any halt check */

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    REQUIRE(falcon_hs_kick(&f, NV_PSEC2_BROM_BASE,
                           /*dmem_sign*/ 0x300,
                           /*engine_id*/ 0x01,
                           /*ucode_id*/  5,
                           /*boot_vec*/  0x400) == 0);

    REQUIRE(g_bar0[(NV_PSEC2_BROM_BASE + FALCON_BROM_PARAADDR0) / 4] == 0x300);
    REQUIRE(g_bar0[(NV_PSEC2_BROM_BASE + FALCON_BROM_ENGIDMASK) / 4] == 0x01);
    REQUIRE(g_bar0[(NV_PSEC2_BROM_BASE + FALCON_BROM_UCODE_ID) / 4]  == 5);
    REQUIRE(g_bar0[(NV_PSEC2_BROM_BASE + FALCON_BROM_MOD_SEL) / 4]
            == FALCON_BROM_MOD_SEL_RSA3K);
    REQUIRE(e->last_bootvec == 0x400);
    /* Unlike hs_boot, kick doesn't wait — engine is running, not halted. */
    REQUIRE(e->cpu_running);
}

static void test_hs_kick_rejects_non_idle(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);
    e->cpu_running = true;  /* not idle */
    (void)e;

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    REQUIRE(falcon_hs_kick(&f, NV_PSEC2_BROM_BASE, 0, 0, 0, 0) < 0);
}

static void test_hs_kick_rejects_uninitialized(void)
{
    reset_mock();
    struct falcon f = { 0 };    /* not probed */
    REQUIRE(falcon_hs_kick(&f, NV_PSEC2_BROM_BASE, 0, 0, 0, 0) < 0);
}

/* ---- falcon_is_priv_locked tests ---- */

static void test_priv_locked_detects_poison_pattern(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);
    e->priv_locked = true;

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    REQUIRE(falcon_is_priv_locked(&f));
    /* is_idle() must also return false — HALTED isn't set in 0xbadfXXXX. */
    REQUIRE(!falcon_is_idle(&f));
}

static void test_priv_locked_clean_engine_not_flagged(void)
{
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);
    (void)e;  /* default: priv_locked = false */

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    REQUIRE(!falcon_is_priv_locked(&f));
    REQUIRE(falcon_is_idle(&f));
}

static void test_priv_locked_rejects_uninitialized(void)
{
    struct falcon f = { 0 };    /* not probed */
    REQUIRE(!falcon_is_priv_locked(&f));
}

/* ---- falcon_wait_halted priv-lock early-bail test ---- */

static void test_wait_halted_bails_on_priv_lock(void)
{
    /* With a priv-locked CPUCTL, wait_halted cannot observe HALTED
     * and would otherwise spin its full budget. The bail path should
     * return -1 within one poll iteration. Pick a huge timeout so a
     * non-bailing implementation would clearly exceed reasonable
     * wall time. */
    reset_mock();
    struct mock_engine *e = add_engine(NV_PSEC2_BASE, 0x10000, 0x10000, false);
    e->priv_locked = true;

    struct falcon f;
    REQUIRE(falcon_probe(&f, NV_PSEC2_BASE) == 0);
    /* 1 hour nominal timeout — should return almost immediately. */
    REQUIRE(falcon_wait_halted(&f, 3600u * 1000u * 1000u) < 0);
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
    test_hs_boot_programs_brom_and_starts();
    test_hs_boot_rejects_non_idle();
    test_hs_boot_times_out_when_never_halts();
    test_start_uses_cpuctl_alias_when_en_set();
    test_start_uses_cpuctl_when_alias_clear();
    test_pre_pio_setup_writes_correct_regs();
    test_pio_imem_rejects_misaligned_offset();
    test_pio_imem_rejects_misaligned_length();
    test_pio_imem_rejects_overflow();
    test_pio_imem_writes_words_in_order();
    test_pio_imem_secure_sets_bit28();
    test_pio_dmem_writes_words_in_order();
    test_pio_dmem_rejects_misaligned();
    test_pio_zero_len_succeeds();
    test_pio_uninitialized_rejects();
    test_hs_kick_programs_brom_and_starts();
    test_hs_kick_rejects_non_idle();
    test_hs_kick_rejects_uninitialized();
    test_priv_locked_detects_poison_pattern();
    test_priv_locked_clean_engine_not_flagged();
    test_priv_locked_rejects_uninitialized();
    test_wait_halted_bails_on_priv_lock();

    if (failures == 0) {
        printf("test_falcon: all tests PASS\n");
        return 0;
    }
    printf("test_falcon: %d FAIL\n", failures);
    return 1;
}
