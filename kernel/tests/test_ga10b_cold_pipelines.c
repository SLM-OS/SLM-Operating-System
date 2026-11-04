/*
 * test_ga10b_cold_pipelines.c - Scheduler-MLP and eviction-MLP GPU
 * pipelines on an SLM-OS-made channel, and the per-kind handoff store
 * (#1092).
 *
 * Expected values are written out from scripts/gpu-kernel-sched-mlp.c
 * and scripts/gpu-kernel-eviction-qnet.c (buffer sizes, cbuf layouts,
 * CTA dims, v6 op entries, handoff fields), independently of the spec
 * tables in ga10b_cold_pipelines.c. The checksums are FNV-1a of the
 * files the board's helpers load (sched-weights/, eviction-weights/,
 * produced by scripts/sched-extract-weights.py and
 * scripts/eviction-extract-weights.py), so the kernel's weight sources
 * and transposes are pinned to the exact bytes Linux used.
 *
 * The builder tests run ga10b_cp_build against fake ops whose
 * "physical" pages sit above 4 GB.
 */
#include "unity.h"
#include "../gpu/nvidia/ga10b_cold_pipelines.h"
#include "../gpu/nvidia/ga10b_channel_handoff.h"
#include "../gpu/nvidia/ga10b_mnist_pipeline.h"
#include "../gpu/nvidia/ga10b_qmd.h"
#include "gpu_handoff.h"
#include "operator_library.h"   /* operator_library_checksum32 = FNV-1a */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(CONFIG_AI_SCHEDULER)
extern const float ai_mlp_w0[], ai_mlp_b0[], ai_mlp_w1[], ai_mlp_b1[];
extern const float ai_mlp_w2[], ai_mlp_b2[], ai_mlp_w3[], ai_mlp_b3[];
#endif

/* ---- fake GPU memory ---- */

#define CP_POOL_PAGES 320u
#define CP_POOL_PHYS  0x2c0000000ull

static alignas(4096) uint8_t g_cp_pool[CP_POOL_PAGES][4096];
static bool g_cp_used[CP_POOL_PAGES];
static unsigned g_cp_fail_after;   /* fail the Nth allocation (0 = never) */
static unsigned g_cp_allocs;

static void *cp_alloc(void *ctx, uint32_t n, uint64_t *phys)
{
    (void)ctx;
    g_cp_allocs++;
    if (g_cp_fail_after != 0u && g_cp_allocs >= g_cp_fail_after) return NULL;
    for (uint32_t i = 0; i + n <= CP_POOL_PAGES; i++) {
        uint32_t k;
        for (k = 0; k < n && !g_cp_used[i + k]; k++) { }
        if (k == n) {
            for (k = 0; k < n; k++) {
                g_cp_used[i + k] = true;
            }
            memset(g_cp_pool[i], 0, (size_t)n * 4096u);
            *phys = CP_POOL_PHYS + (uint64_t)i * 4096u;
            return g_cp_pool[i];
        }
        i += k;
    }
    return NULL;
}

static void cp_free(void *ctx, void *cpu, uint32_t n)
{
    (void)ctx;
    size_t i = (size_t)((uint8_t *)cpu - &g_cp_pool[0][0]) / 4096u;
    for (uint32_t k = 0; k < n; k++) {
        g_cp_used[i + k] = false;
    }
}

static void *cp_p2c(void *ctx, uint64_t phys)
{
    (void)ctx;
    if (phys < CP_POOL_PHYS || phys >= CP_POOL_PHYS + CP_POOL_PAGES * 4096ull) return NULL;
    return &g_cp_pool[0][0] + (phys - CP_POOL_PHYS);
}

static void cp_clean(void *ctx, const void *p, size_t n) { (void)ctx; (void)p; (void)n; }
static void cp_inval(void *ctx, const void *p, size_t n) { (void)ctx; (void)p; (void)n; }
static uint32_t cp_rd32(void *ctx, uint32_t off) { (void)ctx; (void)off; return 0x00018000u; }
static void cp_wr32(void *ctx, uint32_t off, uint32_t v) { (void)ctx; (void)off; (void)v; }
static void cp_udelay(void *ctx, uint32_t us) { (void)ctx; (void)us; }

static const struct ga10b_gpu_ops g_cp_ops = {
    .rd32 = cp_rd32, .wr32 = cp_wr32, .udelay = cp_udelay,
    .alloc_pages = cp_alloc, .free_pages = cp_free, .phys_to_cpu = cp_p2c,
    .cache_clean = cp_clean, .cache_inval = cp_inval,
};

static void cp_pool_reset(void)
{
    memset(g_cp_used, 0, sizeof(g_cp_used));
    g_cp_allocs = 0;
    g_cp_fail_after = 0;
}

static unsigned cp_pages_used(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < CP_POOL_PAGES; i++) {
        n += g_cp_used[i] ? 1u : 0u;
    }
    return n;
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const uint8_t *p)
{
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

/* ---- spec tables ---- */

static void check_ops(const struct ga10b_cp_spec *s, const uint32_t (*want)[10], unsigned n)
{
    TEST_ASSERT_EQUAL_UINT32(n, s->n_ops);
    for (unsigned i = 0; i < n; i++) {
        const struct ga10b_cp_op_desc *d = &s->ops[i];
        TEST_ASSERT_EQUAL_UINT32(want[i][0], d->grid[0]);
        TEST_ASSERT_EQUAL_UINT32(want[i][1], d->grid[1]);
        TEST_ASSERT_EQUAL_UINT32(want[i][2], d->grid[2]);
        TEST_ASSERT_EQUAL_UINT32(want[i][3], d->block[0]);
        TEST_ASSERT_EQUAL_UINT32(want[i][4], d->block[1]);
        TEST_ASSERT_EQUAL_UINT32(want[i][5], d->block[2]);
        TEST_ASSERT_EQUAL_UINT32(want[i][6], d->output_bytes);
        TEST_ASSERT_EQUAL_UINT32(want[i][7], d->sentinel_cell);
        TEST_ASSERT_EQUAL_UINT32(want[i][8], d->shader);
        TEST_ASSERT_EQUAL_HEX32(want[i][9], d->sentinel_bits);
    }
}

static void test_spec_tables(void)
{
    /* gpu-kernel-sched-mlp.c: GEMM 16x16 / ceil(N/16) x 1, Add+ReLU 32
     * threads / 1 x N; outputs LAYER_OUT * 4; sentinels from
     * sched-weights/pipeline_sentinels.bin. */
    static const uint32_t sched[8][10] = {
        /* gx gy gz  bx  by bz  out  cell shader bits */
        { 16,   1, 1, 16, 16, 1, 1024, 0, 0, 0x3e31b556u },
        {  1, 256, 1, 32,  1, 1, 1024, 0, 1, 0x3df18dcfu },
        { 16,   1, 1, 16, 16, 1, 1024, 0, 0, 0x432f0851u },
        {  1, 256, 1, 32,  1, 1, 1024, 0, 1, 0x43328e2bu },
        {  8,   1, 1, 16, 16, 1,  512, 0, 0, 0xc39ccb53u },
        {  1, 128, 1, 32,  1, 1,  512, 2, 1, 0x43112a8cu },
        {  3,   1, 1, 16, 16, 1,  168, 0, 0, 0xc295575fu },
        {  1,  42, 1, 32,  1, 1,  168, 0, 1, 0xc2b52a0eu },
    };
    check_ops(&ga10b_cp_sched_spec, sched, 8);
    TEST_ASSERT_EQUAL_UINT32(GA10B_PIPELINE_KIND_SCHED_MLP, ga10b_cp_sched_spec.kind);
    TEST_ASSERT_EQUAL_UINT32(432, ga10b_cp_sched_spec.input_bytes);
    TEST_ASSERT_EQUAL_UINT32(168, ga10b_cp_sched_spec.result_bytes);
    TEST_ASSERT_EQUAL_UINT32(2, ga10b_cp_sched_spec.n_shaders);
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_OP_GEMM_GENERIC, ga10b_cp_sched_spec.shader_op_kind[0]);
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_OP_ADD_BIAS, ga10b_cp_sched_spec.shader_op_kind[1]);
    TEST_ASSERT_EQUAL_STRING("00_gemm0", ga10b_cp_sched_spec.ops[0].name);
    TEST_ASSERT_EQUAL_STRING("07_addbias3", ga10b_cp_sched_spec.ops[7].name);

    /* gpu-kernel-eviction-qnet.c at --batch 64: GEMM grid ceil(N/16) x 4,
     * Add+ReLU 32 threads / 1 x N x 64, sigmoid 256 threads / 1;
     * outputs 64 * LAYER_OUT * 4. */
    static const uint32_t evict[9][10] = {
        { 4,  4,  1,  16, 16, 1, 16384, 0, 0, 0x3e17cedeu },
        { 1, 64, 64,  32,  1, 1, 16384, 0, 1, 0x3ee5f918u },
        { 2,  4,  1,  16, 16, 1,  8192, 0, 0, 0xbfa691d7u },
        { 1, 32, 64,  32,  1, 1,  8192, 1, 1, 0x3e9ba7bfu },
        { 1,  4,  1,  16, 16, 1,  4096, 0, 0, 0xbee9cdd6u },
        { 1, 16, 64,  32,  1, 1,  4096, 0, 1, 0x3e935946u },
        { 1,  4,  1,  16, 16, 1,   256, 0, 0, 0x3f65241fu },
        { 1,  1, 64,  32,  1, 1,   256, 0, 1, 0x3f39837au },
        { 1,  1,  1, 256,  1, 1,   256, 0, 2, 0x3f2c7334u },
    };
    for (unsigned i = 0; i < 4; i++) {
        /* evict ops 0..7 grid y for GEMMs is ceil(64/16) = 4. */
        TEST_ASSERT_EQUAL_UINT32(4, ga10b_cp_evict_spec.ops[2u * i].grid[1]);
    }
    check_ops(&ga10b_cp_evict_spec, evict, 9);
    TEST_ASSERT_EQUAL_UINT32(GA10B_PIPELINE_KIND_EVICTION_QNET, ga10b_cp_evict_spec.kind);
    TEST_ASSERT_EQUAL_UINT32(6912, ga10b_cp_evict_spec.input_bytes);
    TEST_ASSERT_EQUAL_UINT32(256, ga10b_cp_evict_spec.result_bytes);
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_OP_SIGMOID, ga10b_cp_evict_spec.shader_op_kind[2]);
    TEST_ASSERT_EQUAL_UINT32(13, SLM_GPU_OP_SIGMOID);
    TEST_ASSERT_EQUAL_STRING("08_sigmoid", ga10b_cp_evict_spec.ops[8].name);
    TEST_ASSERT_EQUAL_HEX32(GA10B_CP_EVICT_EXPECTED_SCORE, ga10b_cp_evict_spec.ops[8].sentinel_bits);

    TEST_ASSERT_TRUE(ga10b_cp_spec_for_kind(GA10B_PIPELINE_KIND_SCHED_MLP) == &ga10b_cp_sched_spec);
    TEST_ASSERT_TRUE(ga10b_cp_spec_for_kind(GA10B_PIPELINE_KIND_EVICTION_QNET) == &ga10b_cp_evict_spec);
    TEST_ASSERT_NULL(ga10b_cp_spec_for_kind(GA10B_PIPELINE_KIND_MNIST));
    TEST_ASSERT_NULL(ga10b_cp_spec_for_kind(GA10B_PIPELINE_KIND_COUNT));

    /* Weight sizes (sched.c:175-188, evict.c:282-294). */
    static const uint32_t sw[8] = { 110592, 1024, 262144, 1024, 131072, 512, 21504, 168 };
    static const uint32_t ew[8] = { 6912, 256, 8192, 128, 2048, 64, 64, 4 };
    for (unsigned i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT32(sw[i], ga10b_cp_sched_spec.weights[i].bytes);
        TEST_ASSERT_EQUAL_UINT32(ew[i], ga10b_cp_evict_spec.weights[i].bytes);
    }
}

/* ---- cbufs ---- */

static void fake_vas(struct ga10b_cp_vas *va)
{
    memset(va, 0, sizeof(*va));
    va->input = 0x4100000ull;
    for (unsigned i = 0; i < GA10B_CP_MAX_WEIGHTS; i++) va->weight[i] = 0x4200000ull + i * 0x10000ull;
    for (unsigned i = 0; i < GA10B_CP_MAX_OPS; i++) va->output[i] = 0x4800000ull + i * 0x10000ull;
}

static void test_cbuf_layouts(void)
{
    struct ga10b_cp_vas va;
    fake_vas(&va);
    static uint8_t cb[4096];

    /* sched op 0 GEMM (sched.c:280-293). */
    TEST_ASSERT_EQUAL_INT(0, ga10b_cp_fill_cbuf(&ga10b_cp_sched_spec, 0, &va, cb, sizeof(cb)));
    TEST_ASSERT_EQUAL_UINT32(16, rd32le(cb + 0x00));
    TEST_ASSERT_EQUAL_UINT32(16, rd32le(cb + 0x04));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x08));
    TEST_ASSERT_EQUAL_UINT32(16, rd32le(cb + 0x0c));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x10));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x14));
    TEST_ASSERT_EQUAL_HEX64(va.input, rd64le(cb + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.weight[0], rd64le(cb + 0x168));
    TEST_ASSERT_EQUAL_HEX64(va.output[0], rd64le(cb + 0x170));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x178));
    TEST_ASSERT_EQUAL_UINT32(108, rd32le(cb + 0x17c));
    TEST_ASSERT_EQUAL_UINT32(256, rd32le(cb + 0x180));
    TEST_ASSERT_EQUAL_UINT32(0, rd32le(cb + 0x184));

    /* sched op 6 GEMM reads op 5's output (sched.c:287-288). */
    TEST_ASSERT_EQUAL_INT(0, ga10b_cp_fill_cbuf(&ga10b_cp_sched_spec, 6, &va, cb, sizeof(cb)));
    TEST_ASSERT_EQUAL_HEX64(va.output[5], rd64le(cb + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.weight[6], rd64le(cb + 0x168));
    TEST_ASSERT_EQUAL_UINT32(3, rd32le(cb + 0x0c));
    TEST_ASSERT_EQUAL_UINT32(128, rd32le(cb + 0x17c));
    TEST_ASSERT_EQUAL_UINT32(42, rd32le(cb + 0x180));

    /* sched op 7 AddBias (sched.c:322-331). */
    TEST_ASSERT_EQUAL_INT(0, ga10b_cp_fill_cbuf(&ga10b_cp_sched_spec, 7, &va, cb, sizeof(cb)));
    TEST_ASSERT_EQUAL_UINT32(32, rd32le(cb + 0x00));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x04));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x0c));
    TEST_ASSERT_EQUAL_UINT32(42, rd32le(cb + 0x10));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x14));
    TEST_ASSERT_EQUAL_HEX64(va.output[6], rd64le(cb + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.weight[7], rd64le(cb + 0x168));
    TEST_ASSERT_EQUAL_HEX64(va.output[7], rd64le(cb + 0x170));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x178));
    TEST_ASSERT_EQUAL_UINT32(42, rd32le(cb + 0x17c));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x180));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x184));
    TEST_ASSERT_EQUAL_UINT32(0, rd32le(cb + 0x188));
    /* sched op 1 has apply_relu = 1. */
    TEST_ASSERT_EQUAL_INT(0, ga10b_cp_fill_cbuf(&ga10b_cp_sched_spec, 1, &va, cb, sizeof(cb)));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x188));
    TEST_ASSERT_EQUAL_UINT32(256, rd32le(cb + 0x17c));

    /* evict op 0 GEMM with M = 64 (evict.c:426-443). */
    TEST_ASSERT_EQUAL_INT(0, ga10b_cp_fill_cbuf(&ga10b_cp_evict_spec, 0, &va, cb, sizeof(cb)));
    TEST_ASSERT_EQUAL_UINT32(4, rd32le(cb + 0x0c));
    TEST_ASSERT_EQUAL_UINT32(4, rd32le(cb + 0x10));
    TEST_ASSERT_EQUAL_HEX64(va.input, rd64le(cb + 0x160));
    TEST_ASSERT_EQUAL_UINT32(64, rd32le(cb + 0x178));
    TEST_ASSERT_EQUAL_UINT32(27, rd32le(cb + 0x17c));
    TEST_ASSERT_EQUAL_UINT32(64, rd32le(cb + 0x180));

    /* evict op 3 Add+ReLU: grid 1 x 32 x 64, N = 64 rows (evict.c:477-488). */
    TEST_ASSERT_EQUAL_INT(0, ga10b_cp_fill_cbuf(&ga10b_cp_evict_spec, 3, &va, cb, sizeof(cb)));
    TEST_ASSERT_EQUAL_UINT32(32, rd32le(cb + 0x00));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x0c));
    TEST_ASSERT_EQUAL_UINT32(32, rd32le(cb + 0x10));
    TEST_ASSERT_EQUAL_UINT32(64, rd32le(cb + 0x14));
    TEST_ASSERT_EQUAL_HEX64(va.output[2], rd64le(cb + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.weight[3], rd64le(cb + 0x168));
    TEST_ASSERT_EQUAL_HEX64(va.output[3], rd64le(cb + 0x170));
    TEST_ASSERT_EQUAL_UINT32(64, rd32le(cb + 0x178));
    TEST_ASSERT_EQUAL_UINT32(32, rd32le(cb + 0x17c));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x180));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x184));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x188));

    /* evict op 8 sigmoid (evict.c:516-520). */
    TEST_ASSERT_EQUAL_INT(0, ga10b_cp_fill_cbuf(&ga10b_cp_evict_spec, 8, &va, cb, sizeof(cb)));
    TEST_ASSERT_EQUAL_UINT32(256, rd32le(cb + 0x00));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x0c));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x10));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(cb + 0x14));
    TEST_ASSERT_EQUAL_HEX64(va.output[7], rd64le(cb + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.output[8], rd64le(cb + 0x168));
    TEST_ASSERT_EQUAL_UINT32(64, rd32le(cb + 0x170));
    TEST_ASSERT_EQUAL_UINT32(0, rd32le(cb + 0x174));

    /* Errors. */
    TEST_ASSERT_EQUAL_INT(-1, ga10b_cp_fill_cbuf(&ga10b_cp_sched_spec, 8, &va, cb, sizeof(cb)));
    TEST_ASSERT_EQUAL_INT(-1, ga10b_cp_fill_cbuf(&ga10b_cp_sched_spec, 0, &va, cb, 511));
    TEST_ASSERT_EQUAL_INT(-1, ga10b_cp_fill_cbuf(NULL, 0, &va, cb, sizeof(cb)));
    TEST_ASSERT_EQUAL_INT(-1, ga10b_cp_fill_cbuf(&ga10b_cp_sched_spec, 0, NULL, cb, sizeof(cb)));
    va.input = 0;   /* an unmapped reference */
    TEST_ASSERT_EQUAL_INT(-1, ga10b_cp_fill_cbuf(&ga10b_cp_sched_spec, 0, &va, cb, sizeof(cb)));

    /* References. */
    fake_vas(&va);
    TEST_ASSERT_EQUAL_HEX64(va.output[3],
                            ga10b_cp_ref_va(&ga10b_cp_sched_spec, &va, GA10B_CP_REF_OUTPUT(3), 3));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_cp_ref_va(&ga10b_cp_sched_spec, &va, GA10B_CP_REF_OUTPUT(4), 3));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_cp_ref_va(&ga10b_cp_sched_spec, &va, GA10B_CP_REF_OUTPUT(8), 9));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_cp_ref_va(&ga10b_cp_sched_spec, &va, GA10B_CP_REF_WEIGHT(8), 0));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_cp_ref_va(&ga10b_cp_sched_spec, &va, 0x05, 0));
    TEST_ASSERT_EQUAL_HEX64(va.output[8],
                            ga10b_cp_ref_va(&ga10b_cp_evict_spec, &va, GA10B_CP_REF_OUTPUT(8), 8));
}

/* ---- transposes + reference data ---- */

static void test_transpose(void)
{
    /* [2][3] -> [3][2] */
    uint32_t src[6] = { 1, 2, 3, 4, 5, 6 }, dst[6] = { 0 };
    ga10b_cp_transpose32((uint8_t *)dst, (const uint8_t *)src, 2, 3);
    static const uint32_t want[6] = { 1, 4, 2, 5, 3, 6 };
    TEST_ASSERT_EQUAL_MEMORY(want, dst, sizeof(want));

    struct ga10b_cp_weight_desc w = { "x", 24, 2, 3 };
    memset(dst, 0, sizeof(dst));
    TEST_ASSERT_EQUAL_INT(0, ga10b_cp_weight_fill(&w, (uint8_t *)dst, (const uint8_t *)src));
    TEST_ASSERT_EQUAL_MEMORY(want, dst, sizeof(want));
    struct ga10b_cp_weight_desc b = { "b", 12, 0, 0 };
    memset(dst, 0, sizeof(dst));
    TEST_ASSERT_EQUAL_INT(0, ga10b_cp_weight_fill(&b, (uint8_t *)dst, (const uint8_t *)src));
    TEST_ASSERT_EQUAL_MEMORY(src, dst, 12);
    TEST_ASSERT_EQUAL_UINT32(0, dst[3]);
    struct ga10b_cp_weight_desc bad = { "bad", 20, 2, 3 };
    TEST_ASSERT_EQUAL_INT(-1, ga10b_cp_weight_fill(&bad, (uint8_t *)dst, (const uint8_t *)src));
    TEST_ASSERT_EQUAL_INT(-1, ga10b_cp_weight_fill(NULL, (uint8_t *)dst, (const uint8_t *)src));
}

/* FNV-1a of the transposed weight bytes, i.e. of the helper's files. */
static uint32_t fnv_of_weight(const struct ga10b_cp_weight_desc *w, const void *src)
{
    static uint8_t buf[262144];
    /* 0 never matches a reference checksum, so a failure here fails the
     * caller's assertion. */
    if (w->bytes > sizeof(buf) || ga10b_cp_weight_fill(w, buf, (const uint8_t *)src) != 0) {
        return 0;
    }
    return operator_library_checksum32(buf, w->bytes);
}

static void test_eviction_weights_match_helper(void)
{
    /* eviction-weights/{W,B}_*.bin from mlp_policy_f32.rs. */
    static const uint32_t ref[8] = {
        0x6ff9d0bbu, 0xfc99cb86u, 0x329aad51u, 0x2faaaf1cu,
        0xbc334b66u, 0xa032a6b5u, 0x851ed466u, 0xeb860a02u,
    };
    const void *src[8] = {
        ga10b_evict_mlp_w_l1, ga10b_evict_mlp_b_l1, ga10b_evict_mlp_w_l2, ga10b_evict_mlp_b_l2,
        ga10b_evict_mlp_w_l3, ga10b_evict_mlp_b_l3, ga10b_evict_mlp_w_out, ga10b_evict_mlp_b_out,
    };
    for (unsigned i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX32(ref[i], fnv_of_weight(&ga10b_cp_evict_spec.weights[i], src[i]));
    }
    /* First Rust literal 0.0747476 and B_OUT -0.17042 as fp32 bits. */
    uint32_t bits;
    memcpy(&bits, ga10b_evict_mlp_w_l1, 4);
    TEST_ASSERT_EQUAL_HEX32(0x3d991545u, bits);
    memcpy(&bits, ga10b_evict_mlp_b_out, 4);
    TEST_ASSERT_EQUAL_HEX32(0xbe2e8295u, bits);
}

static void test_sched_weights_match_helper(void)
{
#if defined(CONFIG_AI_SCHEDULER) && !defined(SLMOS_AI_WEIGHTS_REAL)
    /* sched-weights/{W,b}*.bin from the synthetic ai_weights_mlp.c. */
    static const uint32_t ref[8] = {
        0x07e21d1du, 0xf4009907u, 0x82d6a8e1u, 0xb8bba1d5u,
        0x76d329c7u, 0xdcd3af28u, 0xdaf5ec71u, 0x2ed30e8fu,
    };
    const void *src[8] = { ai_mlp_w0, ai_mlp_b0, ai_mlp_w1, ai_mlp_b1,
                           ai_mlp_w2, ai_mlp_b2, ai_mlp_w3, ai_mlp_b3 };
    for (unsigned i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX32(ref[i], fnv_of_weight(&ga10b_cp_sched_spec.weights[i], src[i]));
    }
#else
    TEST_IGNORE_MESSAGE("AI_SCHED=OFF or AI_WEIGHTS=real: synthetic sched weights not linked");
#endif
}

/* Monotonic integer key for fp32 bits (no FP in the kernel). */
static uint32_t f32_key(uint32_t b)
{
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

static void test_reference_vectors(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xbcf6e4a9u,
        operator_library_checksum32((const uint8_t *)ga10b_cp_sched_input_synth, 432));
    TEST_ASSERT_EQUAL_HEX32(0xc7f9e525u,
        operator_library_checksum32((const uint8_t *)ga10b_cp_evict_input_synth, 108));
    TEST_ASSERT_EQUAL_HEX32(0x7364974eu,
        operator_library_checksum32((const uint8_t *)ga10b_cp_sched_expected_logits, 168));
    /* in_vec endpoints: -1.0 / 1.0 and -0.1 / 0.0 / 0.1. */
    TEST_ASSERT_EQUAL_HEX32(0xbf800000u, ga10b_cp_sched_input_synth[0]);
    TEST_ASSERT_EQUAL_HEX32(0x3f800000u, ga10b_cp_sched_input_synth[107]);
    TEST_ASSERT_EQUAL_HEX32(0xbdcccccdu, ga10b_cp_evict_input_synth[0]);
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, ga10b_cp_evict_input_synth[13]);
    /* The extraction script printed argmax(expected) = 8; op 7's
     * sentinel is logit 0. */
    unsigned best = 0;
    for (unsigned i = 1; i < GA10B_CP_SCHED_ACTIONS; i++) {
        if (f32_key(ga10b_cp_sched_expected_logits[i]) >
            f32_key(ga10b_cp_sched_expected_logits[best])) {
            best = i;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(GA10B_CP_SCHED_EXPECTED_ARGMAX, best);
    TEST_ASSERT_EQUAL_HEX32(ga10b_cp_sched_spec.ops[7].sentinel_bits,
                            ga10b_cp_sched_expected_logits[0]);
}

/* ---- building ---- */

static uint8_t g_sass[3][3000];

static void make_sources(const struct ga10b_cp_spec *s, struct ga10b_cp_sources *src,
                         uint8_t *wbuf)
{
    memset(src, 0, sizeof(*src));
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned k = 0; k < sizeof(g_sass[i]); k++) g_sass[i][k] = (uint8_t)(0x40u * i + k);
    }
    static const uint32_t sizes[3] = { 2048, 2560, 2048 };
    for (unsigned i = 0; i < s->n_shaders; i++) {
        src->sass[i] = g_sass[i];
        src->sass_bytes[i] = sizes[i];
    }
    /* Weight i word j = (i << 24) | j, in source layout. */
    size_t off = 0;
    for (unsigned i = 0; i < s->n_weights; i++) {
        uint32_t n = s->weights[i].bytes / 4u;
        for (uint32_t j = 0; j < n; j++) {
            uint32_t v = ((uint32_t)i << 24) | j;
            memcpy(wbuf + off + 4u * j, &v, 4);
        }
        src->weight[i] = wbuf + off;
        off += s->weights[i].bytes;
    }
}

static void make_chan(const struct ga10b_vm *vm, struct ga10b_chan *ch)
{
    memset(ch, 0, sizeof(*ch));
    ga10b_chan_default_params(&ch->p);
    ch->vm = *vm;
    ch->userd.phys = 0x1000000ull;
    ch->gpfifo.phys = 0x1001000ull;
    ch->pushbuf.phys = 0x1002000ull;
    ch->pushbuf.size = 65536u;
    ch->sema.phys = 0x1012000ull;
    ch->inst.phys = 0x1013000ull;
    ch->gpfifo_va = 0x4000000ull;
    ch->pushbuf_va = 0x4001000ull;
    ch->sema_va = 0x4011000ull;
    ch->work_submit_token = ga10b_doorbell_token(ch->p.chid, 0);
    ch->enabled = true;
}

static uint8_t g_wbuf[600000];

static void check_built(const struct ga10b_cp_spec *s, const struct ga10b_cp_pipeline *p,
                        const struct ga10b_cp_sources *src, struct ga10b_vm *vm)
{
    /* Shaders: 64 KB, SASS then zeros, mapped. */
    for (unsigned i = 0; i < s->n_shaders; i++) {
        TEST_ASSERT_EQUAL_UINT32(65536, p->shader[i].size);
        TEST_ASSERT_EQUAL_MEMORY(src->sass[i], p->shader[i].cpu, src->sass_bytes[i]);
        TEST_ASSERT_EQUAL_UINT8(0, ((const uint8_t *)p->shader[i].cpu)[src->sass_bytes[i]]);
        TEST_ASSERT_EQUAL_HEX64(p->shader[i].phys, ga10b_vm_lookup(&g_cp_ops, vm, p->shader_va[i]));
    }
    /* Weights: page-rounded, transposed: GPU [col][row] = source [row][col]. */
    for (unsigned i = 0; i < s->n_weights; i++) {
        const struct ga10b_cp_weight_desc *w = &s->weights[i];
        TEST_ASSERT_EQUAL_UINT32(ga10b_mnist_round_page(w->bytes), p->weight[i].size);
        TEST_ASSERT_EQUAL_HEX64(p->weight[i].phys, ga10b_vm_lookup(&g_cp_ops, vm, p->va.weight[i]));
        const uint8_t *g = (const uint8_t *)p->weight[i].cpu;
        if (w->rows == 0u) {
            TEST_ASSERT_EQUAL_MEMORY(src->weight[i], g, w->bytes);
        } else {
            uint32_t r = w->rows - 1u, c = w->cols - 1u;
            TEST_ASSERT_EQUAL_HEX32(((uint32_t)i << 24) | (r * w->cols + c),
                                    rd32le(g + 4u * ((size_t)c * w->rows + r)));
            /* GPU word 1 = source [1][0] = word cols. */
            TEST_ASSERT_EQUAL_HEX32(((uint32_t)i << 24) | (w->rows > 1u ? w->cols : 1u),
                                    rd32le(g + 4u));
        }
    }
    /* Input. */
    TEST_ASSERT_EQUAL_UINT32(ga10b_mnist_round_page(s->input_bytes), p->input.size);
    TEST_ASSERT_EQUAL_MEMORY(src->input, p->input.cpu, src->input_bytes);
    for (uint32_t k = src->input_bytes; k < p->input.size; k++) {
        TEST_ASSERT_EQUAL_UINT8(0, ((const uint8_t *)p->input.cpu)[k]);
    }

    const struct ga10b_pipeline_op_v7 *arr = (const struct ga10b_pipeline_op_v7 *)p->ops.cpu;
    for (unsigned i = 0; i < s->n_ops; i++) {
        const struct ga10b_cp_op_desc *d = &s->ops[i];
        TEST_ASSERT_EQUAL_UINT32(ga10b_mnist_round_page(d->output_bytes), p->out[i].size);
        TEST_ASSERT_EQUAL_UINT32(4096, p->cbuf[i].size);
        TEST_ASSERT_EQUAL_UINT32(4096, p->qmd[i].size);
        for (uint32_t k = 0; k < p->out[i].size; k++) {
            TEST_ASSERT_EQUAL_UINT8(0, ((const uint8_t *)p->out[i].cpu)[k]);
        }
        /* cbuf matches the pure filler on the real VAs. */
        static uint8_t want[4096];
        TEST_ASSERT_EQUAL_INT(0, ga10b_cp_fill_cbuf(s, i, &p->va, want, sizeof(want)));
        TEST_ASSERT_EQUAL_MEMORY(want, p->cbuf[i].cpu, 4096);
        /* Baked QMD = ga10b_qmd_populate. */
        uint32_t q[64];
        ga10b_qmd_populate(q, p->shader_va[d->shader], p->cbuf_va[i], 128,
                           d->grid[0], d->grid[1], d->grid[2],
                           d->block[0], d->block[1], d->block[2]);
        TEST_ASSERT_EQUAL_MEMORY(q, p->qmd[i].cpu, sizeof(q));
        /* v7 op entry. */
        const struct ga10b_pipeline_op_v7 *o = &arr[i];
        TEST_ASSERT_EQUAL_HEX64(p->qmd_va[i], o->qmd_gpu_va);
        uint64_t off = (i + 1u == s->n_ops) ? 0u : (uint64_t)d->sentinel_cell * 4u;
        TEST_ASSERT_EQUAL_HEX64(p->out[i].phys + off, o->output_phys);
        TEST_ASSERT_EQUAL_UINT32(0, o->expected_payload);
        TEST_ASSERT_EQUAL_UINT32(0, o->flags);
        TEST_ASSERT_EQUAL_HEX64(p->shader_va[d->shader], o->shader_gpu_va);
        TEST_ASSERT_EQUAL_HEX64(p->cbuf_va[i], o->cbuf_gpu_va);
        TEST_ASSERT_EQUAL_UINT32(128, o->register_count_v);
        TEST_ASSERT_EQUAL_UINT32(d->grid[0], o->grid_x);
        TEST_ASSERT_EQUAL_UINT32(d->grid[1], o->grid_y);
        TEST_ASSERT_EQUAL_UINT32(d->grid[2], o->grid_z);
        TEST_ASSERT_EQUAL_UINT32(d->block[0], o->block_x);
        TEST_ASSERT_EQUAL_UINT32(d->block[1], o->block_y);
        TEST_ASSERT_EQUAL_UINT32(d->block[2], o->block_z);
        TEST_ASSERT_EQUAL_UINT32(0, o->smem_size_bytes);
        TEST_ASSERT_EQUAL_UINT32(0, o->slm_size_bytes);
        TEST_ASSERT_EQUAL_UINT32(0, o->barrier_count);
        TEST_ASSERT_EQUAL_HEX64(p->shader[d->shader].phys, o->shader_phys);
        TEST_ASSERT_EQUAL_UINT32(4096, o->shader_size_bytes);
        TEST_ASSERT_EQUAL_HEX64(p->out[i].phys, ga10b_vm_lookup(&g_cp_ops, vm, p->va.output[i]));
        TEST_ASSERT_EQUAL_HEX64(p->cbuf[i].phys, ga10b_vm_lookup(&g_cp_ops, vm, p->cbuf_va[i]));
        TEST_ASSERT_EQUAL_HEX64(p->qmd[i].phys, ga10b_vm_lookup(&g_cp_ops, vm, p->qmd_va[i]));
    }
    /* Entries past n_ops stay zero. */
    TEST_ASSERT_EQUAL_HEX64(0, arr[s->n_ops].qmd_gpu_va);
    /* Pool: 1024 x 256 B, mapped; ops page unmapped. */
    TEST_ASSERT_EQUAL_UINT32(262144, p->qmd_pool.size);
    TEST_ASSERT_EQUAL_HEX64(p->qmd_pool.phys, ga10b_vm_lookup(&g_cp_ops, vm, p->qmd_pool_va));
    TEST_ASSERT_TRUE(p->built);
}

static void check_handoff(const struct ga10b_cp_spec *s, const struct ga10b_cp_pipeline *p,
                          const struct ga10b_chan *ch, const struct ga10b_channel_handoff *h)
{
    TEST_ASSERT_EQUAL_HEX32(GA10B_CHANNEL_HANDOFF_MAGIC, h->magic);
    TEST_ASSERT_EQUAL_UINT32(7, h->version);
    TEST_ASSERT_EQUAL_HEX64(ch->userd.phys, h->userd_phys);
    TEST_ASSERT_EQUAL_HEX64(ch->sema.phys, h->semaphore_phys);
    TEST_ASSERT_EQUAL_UINT32(ch->work_submit_token, h->work_submit_token);
    TEST_ASSERT_EQUAL_HEX32(s->ops[s->n_ops - 1u].sentinel_bits, h->expected_payload);
    TEST_ASSERT_EQUAL_UINT32(s->n_ops, h->pipeline_n_ops);
    TEST_ASSERT_EQUAL_HEX64(p->ops.phys, h->pipeline_ops_phys);
    TEST_ASSERT_EQUAL_HEX64(p->input.phys, h->input_buf_phys);
    TEST_ASSERT_EQUAL_UINT32(s->input_bytes, h->input_buf_size);
    TEST_ASSERT_EQUAL_UINT32(s->kind, h->pipeline_kind);
    TEST_ASSERT_EQUAL_HEX64(p->qmd_pool.phys, h->qmd_pool_phys);
    TEST_ASSERT_EQUAL_HEX64(p->qmd_pool_va, h->qmd_pool_gpu_va);
    TEST_ASSERT_EQUAL_UINT32(262144, h->qmd_pool_size_bytes);
    TEST_ASSERT_EQUAL_UINT32(1024, h->qmd_pool_n_slots);
    /* v3 single-shot fields zero (gpu_write_handoff_v6/v7). */
    TEST_ASSERT_EQUAL_HEX64(0, h->qmd_gpu_va);
    TEST_ASSERT_EQUAL_HEX64(0, h->output_phys);
    TEST_ASSERT_EQUAL_HEX64(0, h->shader_phys);
    TEST_ASSERT_TRUE(ga10b_handoff_is_v7(h));
    TEST_ASSERT_EQUAL_INT(GA10B_V7_OK, ga10b_v7_validate_handoff(h));
    /* What ga10b_bringup_read_pipeline_output reads: the last op's
     * output_phys is the result buffer's start. */
    const struct ga10b_pipeline_op_v7 *arr = (const struct ga10b_pipeline_op_v7 *)p->ops.cpu;
    TEST_ASSERT_EQUAL_HEX64(p->out[s->n_ops - 1u].phys, arr[h->pipeline_n_ops - 1u].output_phys);
    TEST_ASSERT_TRUE(s->result_bytes <= p->out[s->n_ops - 1u].size);
}

static void build_one(const struct ga10b_cp_spec *s, const void *input, uint32_t input_bytes)
{
    cp_pool_reset();
    struct ga10b_vm vm;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_cp_ops, &vm));
    struct ga10b_cp_sources src;
    make_sources(s, &src, g_wbuf);
    src.input = (const uint8_t *)input;
    src.input_bytes = input_bytes;
    static struct ga10b_cp_pipeline p;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_OK, ga10b_cp_build(&g_cp_ops, &vm, s, &src, &p));
    check_built(s, &p, &src, &vm);

    struct ga10b_chan ch;
    make_chan(&vm, &ch);
    struct ga10b_channel_handoff h;
    memset(&h, 0xee, sizeof(h));
    ga10b_cp_fill_handoff(&p, &ch, &h);
    check_handoff(s, &p, &ch, &h);

    ga10b_cp_free(&g_cp_ops, &p);
    TEST_ASSERT_FALSE(p.built);
    ga10b_vm_destroy(&g_cp_ops, &vm);
    TEST_ASSERT_EQUAL_UINT32(0, cp_pages_used());
}

static void test_build_sched(void)
{
    build_one(&ga10b_cp_sched_spec, ga10b_cp_sched_input_synth, 432);
}

static void test_build_evict(void)
{
    /* Row 0 only; rows 1..63 stay zero. */
    build_one(&ga10b_cp_evict_spec, ga10b_cp_evict_input_synth, 108);
}

static void test_build_errors(void)
{
    cp_pool_reset();
    struct ga10b_vm vm;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_cp_ops, &vm));
    const struct ga10b_cp_spec *s = &ga10b_cp_evict_spec;
    struct ga10b_cp_sources src;
    static struct ga10b_cp_pipeline p;

    g_cp_allocs = 0;   /* ga10b_vm_init allocated the PDB */
    make_sources(s, &src, g_wbuf);
    TEST_ASSERT_EQUAL_INT(GA10B_CP_ERR_ARG, ga10b_cp_build(NULL, &vm, s, &src, &p));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_ERR_ARG, ga10b_cp_build(&g_cp_ops, &vm, NULL, &src, &p));
    src.sass[2] = NULL;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_ERR_ARG, ga10b_cp_build(&g_cp_ops, &vm, s, &src, &p));
    make_sources(s, &src, g_wbuf);
    src.sass_bytes[0] = 65537;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_ERR_ARG, ga10b_cp_build(&g_cp_ops, &vm, s, &src, &p));
    make_sources(s, &src, g_wbuf);
    src.weight[7] = NULL;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_ERR_ARG, ga10b_cp_build(&g_cp_ops, &vm, s, &src, &p));
    make_sources(s, &src, g_wbuf);
    src.input = g_wbuf;
    src.input_bytes = 6913;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_ERR_ARG, ga10b_cp_build(&g_cp_ops, &vm, s, &src, &p));
    TEST_ASSERT_EQUAL_UINT32(0, g_cp_allocs);   /* argument errors allocate nothing */

    /* Allocation failure part-way: an error, pages leaked by design. */
    make_sources(s, &src, g_wbuf);
    g_cp_fail_after = 10;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_ERR_NOMEM, ga10b_cp_build(&g_cp_ops, &vm, s, &src, &p));
    TEST_ASSERT_FALSE(p.built);

    /* Unbuilt pipeline: only the v2 channel handoff. */
    struct ga10b_chan ch;
    make_chan(&vm, &ch);
    struct ga10b_channel_handoff h;
    memset(&p, 0, sizeof(p));
    ga10b_cp_fill_handoff(&p, &ch, &h);
    TEST_ASSERT_EQUAL_UINT32(2, h.version);
    TEST_ASSERT_FALSE(ga10b_handoff_is_v7(&h));
}

/* ---- per-kind store ---- */

static void fake_v7(struct ga10b_channel_handoff *h, uint32_t kind, uint64_t ops_phys)
{
    memset(h, 0, sizeof(*h));
    h->magic = GA10B_CHANNEL_HANDOFF_MAGIC;
    h->version = 7;
    h->userd_phys = 0x1000000ull;
    h->gpfifo_phys = 0x1001000ull;
    h->gpfifo_entries = 512;
    h->pushbuf_phys = 0x1002000ull;
    h->work_submit_token = 0x77;
    h->pipeline_n_ops = 8;
    h->pipeline_ops_phys = ops_phys;
    h->pipeline_kind = kind;
    h->qmd_pool_phys = 0x3000000ull;
    h->qmd_pool_n_slots = 1024;
    h->qmd_pool_size_bytes = 262144;
    h->initial_gp_put = 0;
    h->initial_gp_get = 0;
}

static void test_store_and_switch(void)
{
    static struct ga10b_cp_store st;
    memset(&st, 0, sizeof(st));
    struct ga10b_channel_handoff m, s, e, out;
    fake_v7(&m, GA10B_PIPELINE_KIND_MNIST, 0x5000000ull);
    fake_v7(&s, GA10B_PIPELINE_KIND_SCHED_MLP, 0x5001000ull);
    fake_v7(&e, GA10B_PIPELINE_KIND_EVICTION_QNET, 0x5002000ull);

    TEST_ASSERT_NULL(ga10b_cp_store_get(&st, GA10B_PIPELINE_KIND_SCHED_MLP));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_NONE,
                          ga10b_cp_switch_handoff(&st, GA10B_PIPELINE_KIND_SCHED_MLP, NULL, &out));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_OK, ga10b_cp_store_put(&st, &m));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_OK, ga10b_cp_store_put(&st, &s));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_OK, ga10b_cp_store_put(&st, &e));
    TEST_ASSERT_EQUAL_HEX64(0x5001000ull,
                            ga10b_cp_store_get(&st, GA10B_PIPELINE_KIND_SCHED_MLP)->pipeline_ops_phys);
    TEST_ASSERT_EQUAL_HEX64(0x5002000ull,
                            ga10b_cp_store_get(&st, GA10B_PIPELINE_KIND_EVICTION_QNET)->pipeline_ops_phys);
    TEST_ASSERT_NULL(ga10b_cp_store_get(&st, GA10B_PIPELINE_KIND_COUNT));

    /* Rejections: v2, bad magic, bad pool, bad kind. */
    struct ga10b_channel_handoff bad = s;
    bad.version = 2;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_ARG, ga10b_cp_store_put(&st, &bad));
    bad = s;
    bad.magic = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_ARG, ga10b_cp_store_put(&st, &bad));
    bad = s;
    bad.qmd_pool_size_bytes = 100;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_ARG, ga10b_cp_store_put(&st, &bad));
    bad = s;
    bad.pipeline_kind = GA10B_PIPELINE_KIND_COUNT;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_KIND, ga10b_cp_store_put(&st, &bad));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_ARG, ga10b_cp_store_put(NULL, &s));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_ARG, ga10b_cp_store_put(&st, NULL));

    /* Switch from MNIST (which has run: cursor at 37/37) to sched: the
     * stored sched copy says 0/0, the installed one must say 37/37. */
    struct ga10b_channel_handoff active = m;
    active.initial_gp_put = 37;
    active.initial_gp_get = 37;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_OK,
                          ga10b_cp_switch_handoff(&st, GA10B_PIPELINE_KIND_SCHED_MLP, &active, &out));
    TEST_ASSERT_EQUAL_UINT32(GA10B_PIPELINE_KIND_SCHED_MLP, out.pipeline_kind);
    TEST_ASSERT_EQUAL_HEX64(0x5001000ull, out.pipeline_ops_phys);
    TEST_ASSERT_EQUAL_UINT32(37, out.initial_gp_put);
    TEST_ASSERT_EQUAL_UINT32(37, out.initial_gp_get);
    /* The store itself keeps the build-time cursor. */
    TEST_ASSERT_EQUAL_UINT32(0, ga10b_cp_store_get(&st, GA10B_PIPELINE_KIND_SCHED_MLP)->initial_gp_put);

    /* Wrapped cursor carries verbatim. */
    active.initial_gp_put = 3;
    active.initial_gp_get = 3;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_OK,
                          ga10b_cp_switch_handoff(&st, GA10B_PIPELINE_KIND_EVICTION_QNET, &active, &out));
    TEST_ASSERT_EQUAL_UINT32(3, out.initial_gp_put);

    /* No active handoff: stored cursor. */
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_OK,
                          ga10b_cp_switch_handoff(&st, GA10B_PIPELINE_KIND_SCHED_MLP, NULL, &out));
    TEST_ASSERT_EQUAL_UINT32(0, out.initial_gp_put);
    struct ga10b_channel_handoff empty;
    memset(&empty, 0, sizeof(empty));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_OK,
                          ga10b_cp_switch_handoff(&st, GA10B_PIPELINE_KIND_SCHED_MLP, &empty, &out));

    /* An active handoff on another channel is refused. */
    active = m;
    active.userd_phys = 0x9000000ull;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_CHAN,
                          ga10b_cp_switch_handoff(&st, GA10B_PIPELINE_KIND_SCHED_MLP, &active, &out));
    active = m;
    active.work_submit_token = 0x78;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_CHAN,
                          ga10b_cp_switch_handoff(&st, GA10B_PIPELINE_KIND_SCHED_MLP, &active, &out));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_KIND,
                          ga10b_cp_switch_handoff(&st, GA10B_PIPELINE_KIND_COUNT, NULL, &out));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_ERR_ARG,
                          ga10b_cp_switch_handoff(&st, GA10B_PIPELINE_KIND_SCHED_MLP, NULL, NULL));

    /* The kernel-wide store is a single instance. */
    TEST_ASSERT_TRUE(ga10b_cp_store_global() == ga10b_cp_store_global());
}

/* A handoff from the real builder goes into the store and switches. */
static void test_store_real_handoffs(void)
{
    cp_pool_reset();
    struct ga10b_vm vm;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_cp_ops, &vm));
    struct ga10b_chan ch;
    make_chan(&vm, &ch);
    struct ga10b_cp_sources src;
    make_sources(&ga10b_cp_evict_spec, &src, g_wbuf);
    static struct ga10b_cp_pipeline p;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_OK, ga10b_cp_build(&g_cp_ops, &vm, &ga10b_cp_evict_spec, &src, &p));
    struct ga10b_channel_handoff h, out;
    ga10b_cp_fill_handoff(&p, &ch, &h);
    static struct ga10b_cp_store st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_OK, ga10b_cp_store_put(&st, &h));
    struct ga10b_channel_handoff active;
    ga10b_chan_fill_handoff(&ch, &active);   /* the v2 channel handoff, cursor advanced */
    active.initial_gp_put = 12;
    active.initial_gp_get = 12;
    TEST_ASSERT_EQUAL_INT(GA10B_CP_STORE_OK,
                          ga10b_cp_switch_handoff(&st, GA10B_PIPELINE_KIND_EVICTION_QNET, &active, &out));
    TEST_ASSERT_EQUAL_UINT32(12, out.initial_gp_put);
    TEST_ASSERT_EQUAL_HEX64(h.pipeline_ops_phys, out.pipeline_ops_phys);
    ga10b_cp_free(&g_cp_ops, &p);
    ga10b_vm_destroy(&g_cp_ops, &vm);
}

/* `nvgpu cold check` compares a GPU run with the CPU reference. fp32
 * values are written as bit patterns: kernel tests build with
 * -mgeneral-regs-only. */
static void test_check_helpers(void)
{
    /* The reference compares equal to itself and has the documented
     * argmax. */
    TEST_ASSERT_EQUAL_UINT32(0, ga10b_cp_count_mismatch(
        ga10b_cp_sched_expected_logits, ga10b_cp_sched_expected_logits,
        GA10B_CP_SCHED_ACTIONS, 1000u));
#if !defined(SLMOS_AI_WEIGHTS_REAL)
    TEST_ASSERT_EQUAL_UINT32(GA10B_CP_SCHED_EXPECTED_ARGMAX,
                             ga10b_cp_argmax(ga10b_cp_sched_expected_logits,
                                             GA10B_CP_SCHED_ACTIONS));
#endif
    /* 100.0, -200.0, 0.5, 0.0 */
    const uint32_t want[4] = { 0x42C80000u, 0xC3480000u, 0x3F000000u, 0x00000000u };
    uint32_t got[4];
    memcpy(got, want, sizeof(got));
    got[0] = 0x42C8199Au;   /* 100.05: 5e-4 relative, within 1e-3 */
    got[2] = 0x3F003AFBu;   /* 0.5009: 9e-4 absolute (|want| < 1) */
    TEST_ASSERT_EQUAL_UINT32(0, ga10b_cp_count_mismatch(got, want, 4, 1000u));
    TEST_ASSERT_EQUAL_UINT32(2, ga10b_cp_count_mismatch(got, want, 4, 100u));
    got[1] = 0xC3484CCDu;   /* -200.3: 1.5e-3 relative */
    TEST_ASSERT_EQUAL_UINT32(1, ga10b_cp_count_mismatch(got, want, 4, 1000u));
    got[3] = 0x7FC00000u;  /* NaN never matches */
    TEST_ASSERT_EQUAL_UINT32(2, ga10b_cp_count_mismatch(got, want, 4, 1000u));
    got[3] = 0x7F800000u;  /* +Inf */
    TEST_ASSERT_EQUAL_UINT32(2, ga10b_cp_count_mismatch(got, want, 4, 1000u));
    TEST_ASSERT_EQUAL_UINT32(4, ga10b_cp_count_mismatch(NULL, want, 4, 1000u));
    /* The eviction reference against itself and a neighbour 1 ULP off. */
    const uint32_t ev = GA10B_CP_EVICT_EXPECTED_SCORE;
    const uint32_t ev1 = GA10B_CP_EVICT_EXPECTED_SCORE + 1u;
    TEST_ASSERT_EQUAL_UINT32(0, ga10b_cp_count_mismatch(&ev1, &ev, 1, 100u));
    /* First of equal maxima wins; negatives order correctly. */
    const uint32_t v[4] = { 0x3F800000u, 0x40400000u, 0x40400000u, 0xC1100000u };
    TEST_ASSERT_EQUAL_UINT32(1, ga10b_cp_argmax(v, 4));
    const uint32_t neg[3] = { 0xC1100000u, 0xC3480000u, 0x00000000u };
    TEST_ASSERT_EQUAL_UINT32(2, ga10b_cp_argmax(neg, 3));
    TEST_ASSERT_EQUAL_UINT32(0, ga10b_cp_argmax(NULL, 4));
}

int test_suite_ga10b_cold_pipelines(void)
{
    UnityBegin("test_ga10b_cold_pipelines.c");
    RUN_TEST(test_spec_tables);
    RUN_TEST(test_cbuf_layouts);
    RUN_TEST(test_transpose);
    RUN_TEST(test_eviction_weights_match_helper);
    RUN_TEST(test_sched_weights_match_helper);
    RUN_TEST(test_reference_vectors);
    RUN_TEST(test_build_sched);
    RUN_TEST(test_build_evict);
    RUN_TEST(test_build_errors);
    RUN_TEST(test_store_and_switch);
    RUN_TEST(test_store_real_handoffs);
    RUN_TEST(test_check_helpers);
    return UnityEnd();
}
