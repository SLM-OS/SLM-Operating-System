/*
 * ga10b_cold_pipelines.c - Scheduler-MLP and eviction-MLP GPU pipelines
 * on an SLM-OS-made channel, and the per-kind handoff store (#1092).
 * See ga10b_cold_pipelines.h.
 *
 * "sched.c:N" is scripts/gpu-kernel-sched-mlp.c, "evict.c:N" is
 * scripts/gpu-kernel-eviction-qnet.c, "common.c:N" is
 * scripts/gpu-launch-common.c.
 */
#include "ga10b_cold_pipelines.h"
#include "ga10b_mnist_pipeline.h"   /* ga10b_mnist_round_page */
#include "ga10b_qmd.h"

#include "gpu_handoff.h"            /* SLM_GPU_OP_* */
#include "string.h"

/* ------------------------------------------------------------------ */
/* Specs                                                               */
/* ------------------------------------------------------------------ */

#define IN       GA10B_CP_REF_INPUT
#define WREF(i)  GA10B_CP_REF_WEIGHT(i)
#define OUT(n)   GA10B_CP_REF_OUTPUT(n)
#define CEIL16(x) (((x) + 15u) / 16u)

/* Weights in the helpers' allocation order W0, b0, W1, b1, ...
 * (sched.c:175-196, evict.c:282-306). Source layout is the CPU model's
 * [OUT][IN]; the GPU gets [IN][OUT] (sched-extract-weights.py /
 * eviction-extract-weights.py transpose_weight). */
static const struct ga10b_cp_weight_desc sched_weights[8] = {
    { "W0.bin", 108u * 256u * 4u, 256u, 108u },
    { "b0.bin", 256u * 4u, 0, 0 },
    { "W1.bin", 256u * 256u * 4u, 256u, 256u },
    { "b1.bin", 256u * 4u, 0, 0 },
    { "W2.bin", 256u * 128u * 4u, 128u, 256u },
    { "b2.bin", 128u * 4u, 0, 0 },
    { "W3.bin", 128u * 42u * 4u, 42u, 128u },
    { "b3.bin", 42u * 4u, 0, 0 },
};

/* GEMM (sched.c:274-312): block 16x16, grid ceil(N/16) x ceil(M/16),
 * args a, W, out, M, K, N. Add+ReLU (sched.c:314-349, #1040): block
 * relu_threads (default 32, sched.c:126) x 1 x 1, grid 1 x N x 1, args
 * x, bias, out, N=1, C=N, H=1, W=1, apply_relu. Sentinels:
 * sched-weights/pipeline_sentinels.bin. */
#define SCHED_GEMM(nm, op, a, w, k, n, cell, bits) \
    { (nm), GA10B_CP_SH_GEMM, 3, 3, { (a), WREF(w), OUT(op) }, \
      { 1, (k), (n) }, { CEIL16(n), CEIL16(1u), 1 }, { 16, 16, 1 }, (n) * 4u, (cell), (bits) }
#define SCHED_ADD(nm, op, w, n, relu, cell, bits) \
    { (nm), GA10B_CP_SH_ADDRELU, 3, 5, { OUT((op) - 1), WREF(w), OUT(op) }, \
      { 1, (n), 1, 1, (relu) }, { 1, (n), 1 }, { 32, 1, 1 }, (n) * 4u, (cell), (bits) }

/* Op names: sched.c:231-236. */
static const struct ga10b_cp_op_desc sched_ops[8] = {
    SCHED_GEMM("00_gemm0", 0, IN,     0, 108u, 256u, 0, 0x3e31b556u),
    SCHED_ADD ("01_addrelu0", 1,         1,       256u, 1, 0, 0x3df18dcfu),
    SCHED_GEMM("02_gemm1", 2, OUT(1), 2, 256u, 256u, 0, 0x432f0851u),
    SCHED_ADD ("03_addrelu1", 3,         3,       256u, 1, 0, 0x43328e2bu),
    SCHED_GEMM("04_gemm2", 4, OUT(3), 4, 256u, 128u, 0, 0xc39ccb53u),
    SCHED_ADD ("05_addrelu2", 5,         5,       128u, 1, 2, 0x43112a8cu),
    SCHED_GEMM("06_gemm3", 6, OUT(5), 6, 128u,  42u, 0, 0xc295575fu),
    SCHED_ADD ("07_addbias3", 7,         7,        42u, 0, 0, 0xc2b52a0eu),
};

const struct ga10b_cp_spec ga10b_cp_sched_spec = {
    .kind = GA10B_PIPELINE_KIND_SCHED_MLP,
    .name = "sched-mlp",
    .n_shaders = 2,
    .shader_op_kind = { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_OP_ADD_BIAS },
    .shader_name = { "gemm_fp32", "add_bias_relu_fp32" },
    .n_weights = 8,
    .weights = sched_weights,
    /* SCHED_INPUT_DIM * 4 (sched.c:200-205, handoff :482); the buffer is
     * one page. */
    .input_bytes = GA10B_CP_SCHED_STATE_DIM * 4u,
    .n_ops = 8,
    .ops = sched_ops,
    .result_bytes = GA10B_CP_SCHED_ACTIONS * 4u,   /* slm_gpu_run_sched_inference: 168 */
};

static const struct ga10b_cp_weight_desc evict_weights[8] = {
    { "W_L1.bin", 27u * 64u * 4u, 64u, 27u },
    { "B_L1.bin", 64u * 4u, 0, 0 },
    { "W_L2.bin", 64u * 32u * 4u, 32u, 64u },
    { "B_L2.bin", 32u * 4u, 0, 0 },
    { "W_L3.bin", 32u * 16u * 4u, 16u, 32u },
    { "B_L3.bin", 16u * 4u, 0, 0 },
    { "W_OUT.bin", 16u * 1u * 4u, 1u, 16u },
    { "B_OUT.bin", 1u * 4u, 0, 0 },
};

#define EB GA10B_CP_EVICT_BATCH

/* GEMM (evict.c:410-458): M = batch, grid ceil(N/16) x ceil(M/16).
 * Add+ReLU (evict.c:460-503, #1036): block 32 x 1 x 1, grid 1 x N x M,
 * args x, bias, out, N=M, C=N, H=1, W=1, apply_relu. Output bytes
 * B * LAYER_OUT * 4 (evict.c:373-383). */
#define EVICT_GEMM(nm, op, a, w, k, n, cell, bits) \
    { (nm), GA10B_CP_SH_GEMM, 3, 3, { (a), WREF(w), OUT(op) }, \
      { (int32_t)EB, (k), (n) }, { CEIL16(n), CEIL16(EB), 1 }, { 16, 16, 1 }, \
      EB * (n) * 4u, (cell), (bits) }
#define EVICT_ADD(nm, op, w, n, relu, cell, bits) \
    { (nm), GA10B_CP_SH_ADDRELU, 3, 5, { OUT((op) - 1), WREF(w), OUT(op) }, \
      { (int32_t)EB, (n), 1, 1, (relu) }, { 1, (n), EB }, { 32, 1, 1 }, \
      EB * (n) * 4u, (cell), (bits) }

/* Op names: evict.c:365-371. */
static const struct ga10b_cp_op_desc evict_ops[9] = {
    EVICT_GEMM("00_gemmL1", 0, IN,     0, 27u, 64u, 0, 0x3e17cedeu),
    EVICT_ADD ("01_addreluL1", 1,         1,      64u, 1, 0, 0x3ee5f918u),
    EVICT_GEMM("02_gemmL2", 2, OUT(1), 2, 64u, 32u, 0, 0xbfa691d7u),
    EVICT_ADD ("03_addreluL2", 3,         3,      32u, 1, 1, 0x3e9ba7bfu),
    EVICT_GEMM("04_gemmL3", 4, OUT(3), 4, 32u, 16u, 0, 0xbee9cdd6u),
    EVICT_ADD ("05_addreluL3", 5,         5,      16u, 1, 0, 0x3e935946u),
    EVICT_GEMM("06_gemmOUT", 6, OUT(5), 6, 16u,  1u, 0, 0x3f65241fu),
    EVICT_ADD ("07_addbiasOUT", 7,         7,       1u, 0, 0, 0x3f39837au),
    /* Sigmoid (evict.c:505-533): block 256, grid 1, args x, out, N = B. */
    { "08_sigmoid", GA10B_CP_SH_SIGMOID, 2, 1, { OUT(7), OUT(8) },
      { (int32_t)EB }, { 1, 1, 1 }, { 256, 1, 1 }, EB * 4u, 0, 0x3f2c7334u },
};

const struct ga10b_cp_spec ga10b_cp_evict_spec = {
    .kind = GA10B_PIPELINE_KIND_EVICTION_QNET,
    .name = "eviction-qnet",
    .n_shaders = 3,
    .shader_op_kind = { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_OP_ADD_BIAS, SLM_GPU_OP_SIGMOID },
    .shader_name = { "gemm_fp32", "add_bias_relu_fp32", "sigmoid_fp32" },
    .n_weights = 8,
    .weights = evict_weights,
    /* B rows x 27 features (evict.c:312, handoff :734). */
    .input_bytes = EB * GA10B_CP_EVICT_FEATURES * 4u,
    .n_ops = 9,
    .ops = evict_ops,
    .result_bytes = EB * 4u,
};

#undef IN
#undef WREF
#undef OUT
#undef CEIL16
#undef EB

static_assert(GA10B_CP_MAX_OPS * sizeof(struct ga10b_pipeline_op_v7) <= GA10B_CP_PAGE_BYTES,
              "v7 ops array fits one page");
static_assert(GA10B_CP_MAX_OPS <= GA10B_PIPELINE_V7_MAX_OPS, "v7 op cap");

const struct ga10b_cp_spec *ga10b_cp_spec_for_kind(uint32_t kind)
{
    switch (kind) {
    case GA10B_PIPELINE_KIND_SCHED_MLP:     return &ga10b_cp_sched_spec;
    case GA10B_PIPELINE_KIND_EVICTION_QNET: return &ga10b_cp_evict_spec;
    default:                                return NULL;
    }
}

/* scripts/sched-extract-weights.py: in_vec = [(i / 53.5) - 1.0]. */
const uint32_t ga10b_cp_sched_input_synth[GA10B_CP_SCHED_STATE_DIM] = {
    0xbf800000u, 0xbf7b3707u, 0xbf766e0eu, 0xbf71a516u, 0xbf6cdc1du, 0xbf681324u,
    0xbf634a2bu, 0xbf5e8132u, 0xbf59b839u, 0xbf54ef41u, 0xbf502648u, 0xbf4b5d4fu,
    0xbf469456u, 0xbf41cb5du, 0xbf3d0264u, 0xbf38396cu, 0xbf337073u, 0xbf2ea77au,
    0xbf29de81u, 0xbf251588u, 0xbf204c90u, 0xbf1b8397u, 0xbf16ba9eu, 0xbf11f1a5u,
    0xbf0d28acu, 0xbf085fb3u, 0xbf0396bbu, 0xbefd9b84u, 0xbef40992u, 0xbeea77a0u,
    0xbee0e5afu, 0xbed753bdu, 0xbecdc1cbu, 0xbec42fdau, 0xbeba9de8u, 0xbeb10bf6u,
    0xbea77a05u, 0xbe9de813u, 0xbe945621u, 0xbe8ac430u, 0xbe81323eu, 0xbe6f4099u,
    0xbe5c1cb6u, 0xbe48f8d3u, 0xbe35d4efu, 0xbe22b10cu, 0xbe0f8d29u, 0xbdf8d28bu,
    0xbdd28ac4u, 0xbdac42feu, 0xbd85fb37u, 0xbd3f66e1u, 0xbce5aea7u, 0xbc191f1au,
    0x3c191f1au, 0x3ce5aea7u, 0x3d3f66e1u, 0x3d85fb37u, 0x3dac42feu, 0x3dd28ac4u,
    0x3df8d28bu, 0x3e0f8d29u, 0x3e22b10cu, 0x3e35d4efu, 0x3e48f8d3u, 0x3e5c1cb6u,
    0x3e6f4099u, 0x3e81323eu, 0x3e8ac430u, 0x3e945621u, 0x3e9de813u, 0x3ea77a05u,
    0x3eb10bf6u, 0x3eba9de8u, 0x3ec42fdau, 0x3ecdc1cbu, 0x3ed753bdu, 0x3ee0e5afu,
    0x3eea77a0u, 0x3ef40992u, 0x3efd9b84u, 0x3f0396bbu, 0x3f085fb3u, 0x3f0d28acu,
    0x3f11f1a5u, 0x3f16ba9eu, 0x3f1b8397u, 0x3f204c90u, 0x3f251588u, 0x3f29de81u,
    0x3f2ea77au, 0x3f337073u, 0x3f38396cu, 0x3f3d0264u, 0x3f41cb5du, 0x3f469456u,
    0x3f4b5d4fu, 0x3f502648u, 0x3f54ef41u, 0x3f59b839u, 0x3f5e8132u, 0x3f634a2bu,
    0x3f681324u, 0x3f6cdc1du, 0x3f71a516u, 0x3f766e0eu, 0x3f7b3707u, 0x3f800000u,
};

/* scripts/eviction-extract-weights.py: in_vec = [(i / 130.0) - 0.1]. */
const uint32_t ga10b_cp_evict_input_synth[GA10B_CP_EVICT_FEATURES] = {
    0xbdcccccdu, 0xbdbd0bd1u, 0xbdad4ad5u, 0xbd9d89d9u, 0xbd8dc8ddu, 0xbd7c0fc1u,
    0xbd5c8dc9u, 0xbd3d0bd1u, 0xbd1d89d9u, 0xbcfc0fc1u, 0xbcbd0bd1u, 0xbc7c0fc1u,
    0xbbfc0fc1u, 0x00000000u, 0x3bfc0fc1u, 0x3c7c0fc1u, 0x3cbd0bd1u, 0x3cfc0fc1u,
    0x3d1d89d9u, 0x3d3d0bd1u, 0x3d5c8dc9u, 0x3d7c0fc1u, 0x3d8dc8ddu, 0x3d9d89d9u,
    0x3dad4ad5u, 0x3dbd0bd1u, 0x3dcccccdu,
};

/* sched-weights/expected_logits.bin (synthetic ai_weights_mlp.c). */
const uint32_t ga10b_cp_sched_expected_logits[GA10B_CP_SCHED_ACTIONS] = {
    0xc2b52a0eu, 0xc2ace013u, 0x4325dd80u, 0xc28228f6u, 0xc34ac68fu, 0xc2fc24dbu,
    0xc2de767eu, 0xc2d76474u, 0x43e0bc98u, 0xc32696e7u, 0x42997989u, 0x4157d974u,
    0xc2a44292u, 0xc26e54c5u, 0x423c8056u, 0xc2e41d49u, 0xc3894948u, 0xc341308cu,
    0xc28dc868u, 0xc2df534fu, 0x4307e212u, 0xc3be855fu, 0xc3691eb7u, 0xc2f0ac30u,
    0xc2bd61c2u, 0xc2f21346u, 0x429f7ee8u, 0xc264768fu, 0xc3846f75u, 0xc2d945c7u,
    0xc2c1158eu, 0xc2de53c7u, 0x410eb19cu, 0xc2e421a4u, 0xc38066e4u, 0xc307fdbeu,
    0xc29850cbu, 0xc2c62c53u, 0xc2d5fb3fu, 0xc2f928f9u, 0xc2b1b6d8u, 0xc2d0659cu,
};

/* ------------------------------------------------------------------ */
/* Pure helpers                                                        */
/* ------------------------------------------------------------------ */

/* fp32 bits -> fixed point in units of 2^-20, without FP registers
 * (the kernel builds with -mgeneral-regs-only). False for NaN/Inf and
 * for |x| >= 2^20, which no pipeline output reaches. */
static bool cp_f32_to_fx(uint32_t bits, int64_t *out)
{
    uint32_t exp = (bits >> 23) & 0xFFu;
    if (exp == 0xFFu) return false;
    if (exp >= 127u + 20u) return false;
    int64_t v = 0;
    if (exp != 0u) {                       /* denormals round to 0 */
        uint64_t m = (bits & 0x7FFFFFu) | 0x800000u;
        int shift = (int)exp - 130;        /* exp - 127 - 23 + 20 */
        if (shift >= 0) {
            v = (int64_t)(m << shift);
        } else if (shift > -64) {
            v = (int64_t)(m >> -shift);
        }
    }
    *out = (bits & 0x80000000u) ? -v : v;
    return true;
}

uint32_t ga10b_cp_count_mismatch(const uint32_t *got, const uint32_t *want,
                                 uint32_t n, uint32_t tol_ppm)
{
    if (got == NULL || want == NULL) return n;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        int64_t g, w;
        if (!cp_f32_to_fx(got[i], &g) || !cp_f32_to_fx(want[i], &w)) {
            bad++;
            continue;
        }
        int64_t d = g > w ? g - w : w - g;           /* < 2^41 */
        int64_t scale = w < 0 ? -w : w;
        if (scale < (1LL << 20)) scale = 1LL << 20;  /* max(1, |want|) */
        if ((uint64_t)d * 1000000u > (uint64_t)tol_ppm * (uint64_t)scale) bad++;
    }
    return bad;
}

/* Monotonic unsigned key for fp32 ordering (NaN sorts past +Inf). */
static uint32_t cp_f32_key(uint32_t bits)
{
    return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
}

uint32_t ga10b_cp_argmax(const uint32_t *v, uint32_t n)
{
    uint32_t best = 0;
    for (uint32_t i = 1; v != NULL && i < n; i++) {
        if (cp_f32_key(v[i]) > cp_f32_key(v[best])) best = i;
    }
    return best;
}

void ga10b_cp_transpose32(uint8_t *dst, const uint8_t *src, uint32_t rows,
                          uint32_t cols)
{
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            memcpy(dst + ((size_t)c * rows + r) * 4u,
                   src + ((size_t)r * cols + c) * 4u, 4u);
        }
    }
}

int ga10b_cp_weight_fill(const struct ga10b_cp_weight_desc *w, uint8_t *dst,
                         const uint8_t *src)
{
    if (!w || !dst || !src) return -1;
    if (w->rows == 0u) {
        memcpy(dst, src, w->bytes);
        return 0;
    }
    if ((uint64_t)w->rows * w->cols * 4u != w->bytes) return -1;
    ga10b_cp_transpose32(dst, src, w->rows, w->cols);
    return 0;
}

uint64_t ga10b_cp_ref_va(const struct ga10b_cp_spec *s, const struct ga10b_cp_vas *va,
                         uint8_t ref, uint32_t op)
{
    if (!s || !va) return 0;
    if (ref == GA10B_CP_REF_INPUT) return va->input;
    if (ref >= 0x10u && ref < 0x10u + s->n_weights) return va->weight[ref - 0x10u];
    if (ref >= 0x20u && ref < 0x20u + s->n_ops) {
        uint32_t n = ref - 0x20u;
        return n <= op ? va->output[n] : 0;   /* no forward references */
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

int ga10b_cp_fill_cbuf(const struct ga10b_cp_spec *s, uint32_t op,
                       const struct ga10b_cp_vas *va, uint8_t *cbuf, size_t cap)
{
    if (!s || op >= s->n_ops || !va || !cbuf || cap < GA10B_CP_CBUF_MIN_BYTES) return -1;
    const struct ga10b_cp_op_desc *d = &s->ops[op];
    if (d->n_ptr > GA10B_CP_MAX_PTR_ARGS || d->n_int > GA10B_CP_MAX_INT_ARGS) return -1;
    memset(cbuf, 0, cap);
    /* cbuf[0..2] = blockDim, cbuf[3..5] = gridDim (sched.c:280-281). */
    for (unsigned i = 0; i < 3u; i++) {
        put32(cbuf + 4u * i, d->block[i]);
        put32(cbuf + 12u + 4u * i, d->grid[i]);
    }
    uint32_t off = GA10B_CP_CBUF_PARAM_BASE;
    for (unsigned i = 0; i < d->n_ptr; i++) {
        uint64_t v = ga10b_cp_ref_va(s, va, d->ptr[i], op);
        if (v == 0) return -1;
        put64(cbuf + off, v);
        off += 8u;
    }
    for (unsigned i = 0; i < d->n_int; i++) {
        put32(cbuf + off, (uint32_t)d->ints[i]);
        off += 4u;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Building                                                            */
/* ------------------------------------------------------------------ */

static int alloc_map(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                     uint32_t bytes, struct ga10b_dma *d, uint64_t *va)
{
    if (bytes == 0u) return GA10B_CP_ERR_ARG;
    if (ga10b_dma_alloc(ops, bytes, d) != GA10B_CHAN_OK) return GA10B_CP_ERR_NOMEM;
    if (!va) return GA10B_CP_OK;
    /* MAP_BUFFER_EX without CACHEABLE (common.c:490-540). */
    if (ga10b_vm_map_dma(ops, vm, d, 0u, false, va) != GA10B_CHAN_OK) {
        return GA10B_CP_ERR_MAP;
    }
    return GA10B_CP_OK;
}

void ga10b_cp_free(const struct ga10b_gpu_ops *ops, struct ga10b_cp_pipeline *p)
{
    if (!ops || !p) return;
    for (unsigned i = 0; i < GA10B_CP_MAX_SHADERS; i++) ga10b_dma_free(ops, &p->shader[i]);
    for (unsigned i = 0; i < GA10B_CP_MAX_WEIGHTS; i++) ga10b_dma_free(ops, &p->weight[i]);
    ga10b_dma_free(ops, &p->input);
    for (unsigned i = 0; i < GA10B_CP_MAX_OPS; i++) {
        ga10b_dma_free(ops, &p->qmd[i]);
        ga10b_dma_free(ops, &p->cbuf[i]);
        ga10b_dma_free(ops, &p->out[i]);
    }
    ga10b_dma_free(ops, &p->qmd_pool);
    ga10b_dma_free(ops, &p->ops);
    p->built = false;
}

void ga10b_cp_fill_op_v7(const struct ga10b_cp_pipeline *p, uint32_t op,
                         struct ga10b_pipeline_op_v7 *o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
    if (!p || !p->spec || op >= p->spec->n_ops) return;
    const struct ga10b_cp_spec *s = p->spec;
    const struct ga10b_cp_op_desc *d = &s->ops[op];
    unsigned sh = d->shader;
    /* v6 prefix as the helpers write it (sched.c:455-463,
     * evict.c:703-717): QMD page, sentinel cell (buffer start for the
     * last op), any-non-zero, no flags. */
    o->qmd_gpu_va = p->qmd_va[op];
    o->output_phys = p->out[op].phys +
                     (op + 1u == s->n_ops ? 0u : (uint64_t)d->sentinel_cell * 4u);
    o->expected_payload = 0u;
    o->flags = 0u;
    /* QMD inputs: what gpu_populate_qmd_at + the CTA dim overrides
     * encode (sched.c:299-311 / 336-348, evict.c:445-457 / 490-502 /
     * 523-532). SIMT kernels: smem / slm / barriers at encoder
     * defaults (0), as for the cold MNIST SIMT ops. */
    o->shader_gpu_va = p->shader_va[sh];
    o->cbuf_gpu_va = p->cbuf_va[op];
    o->register_count_v = GA10B_CP_REGISTER_COUNT_V;
    o->grid_x = d->grid[0];
    o->grid_y = d->grid[1];
    o->grid_z = d->grid[2];
    o->block_x = d->block[0];
    o->block_y = d->block[1];
    o->block_z = d->block[2];
    o->shader_phys = p->shader[sh].phys;
    o->shader_size_bytes = ga10b_mnist_round_page(p->sass_bytes[sh]);
}

int ga10b_cp_build(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                   const struct ga10b_cp_spec *spec,
                   const struct ga10b_cp_sources *src, struct ga10b_cp_pipeline *p)
{
    if (!ops || !vm || !spec || !src || !p) return GA10B_CP_ERR_ARG;
    if (spec->n_shaders == 0u || spec->n_shaders > GA10B_CP_MAX_SHADERS ||
        spec->n_weights > GA10B_CP_MAX_WEIGHTS || spec->n_ops == 0u ||
        spec->n_ops > GA10B_CP_MAX_OPS || spec->input_bytes == 0u) {
        return GA10B_CP_ERR_ARG;
    }
    for (unsigned i = 0; i < spec->n_shaders; i++) {
        if (!src->sass[i] || src->sass_bytes[i] == 0u ||
            src->sass_bytes[i] > GA10B_CP_SHADER_BUF_BYTES) {
            return GA10B_CP_ERR_ARG;
        }
    }
    for (unsigned i = 0; i < spec->n_weights; i++) {
        const struct ga10b_cp_weight_desc *w = &spec->weights[i];
        if (!src->weight[i] || w->bytes == 0u ||
            (w->rows != 0u && (uint64_t)w->rows * w->cols * 4u != w->bytes)) {
            return GA10B_CP_ERR_ARG;
        }
    }
    for (unsigned i = 0; i < spec->n_ops; i++) {
        if (spec->ops[i].shader >= spec->n_shaders) return GA10B_CP_ERR_ARG;
    }
    if (src->input && src->input_bytes > spec->input_bytes) return GA10B_CP_ERR_ARG;

    memset(p, 0, sizeof(*p));
    p->spec = spec;
    int rc;

    /* Shaders: 64 KB, zero tail (common.c:542-568). */
    for (unsigned i = 0; i < spec->n_shaders; i++) {
        if ((rc = alloc_map(ops, vm, GA10B_CP_SHADER_BUF_BYTES, &p->shader[i],
                            &p->shader_va[i])) != GA10B_CP_OK) {
            return rc;
        }
        memcpy(p->shader[i].cpu, src->sass[i], src->sass_bytes[i]);
        p->sass_bytes[i] = src->sass_bytes[i];
        ga10b_dma_clean(ops, &p->shader[i]);
    }

    /* Weights at round_up_page(bytes) (sched.c:191-192, evict.c:295-296),
     * then the input buffer (sched.c:200, evict.c:313-314). */
    for (unsigned i = 0; i < spec->n_weights; i++) {
        const struct ga10b_cp_weight_desc *w = &spec->weights[i];
        if ((rc = alloc_map(ops, vm, ga10b_mnist_round_page(w->bytes), &p->weight[i],
                            &p->va.weight[i])) != GA10B_CP_OK) {
            return rc;
        }
        if (ga10b_cp_weight_fill(w, (uint8_t *)p->weight[i].cpu, src->weight[i]) != 0) {
            return GA10B_CP_ERR_ARG;
        }
        ga10b_dma_clean(ops, &p->weight[i]);
    }
    if ((rc = alloc_map(ops, vm, ga10b_mnist_round_page(spec->input_bytes), &p->input,
                        &p->va.input)) != GA10B_CP_OK) {
        return rc;
    }
    if (src->input) memcpy(p->input.cpu, src->input, src->input_bytes);
    ga10b_dma_clean(ops, &p->input);

    /* Per op: QMD page, cbuf page, output buffer (sched.c:244-248,
     * evict.c:385-389). alloc_pages hands out zeroed pages, which is
     * the helpers' pre-zeroing (sched.c:351-356). */
    for (unsigned i = 0; i < spec->n_ops; i++) {
        if ((rc = alloc_map(ops, vm, GA10B_CP_PAGE_BYTES, &p->qmd[i],
                            &p->qmd_va[i])) != GA10B_CP_OK ||
            (rc = alloc_map(ops, vm, GA10B_CP_PAGE_BYTES, &p->cbuf[i],
                            &p->cbuf_va[i])) != GA10B_CP_OK ||
            (rc = alloc_map(ops, vm, ga10b_mnist_round_page(spec->ops[i].output_bytes),
                            &p->out[i], &p->va.output[i])) != GA10B_CP_OK) {
            return rc;
        }
    }

    for (unsigned i = 0; i < spec->n_ops; i++) {
        const struct ga10b_cp_op_desc *d = &spec->ops[i];
        if (ga10b_cp_fill_cbuf(spec, i, &p->va, (uint8_t *)p->cbuf[i].cpu,
                               p->cbuf[i].size) != 0) {
            return GA10B_CP_ERR_ARG;
        }
        ga10b_dma_clean(ops, &p->cbuf[i]);
        /* The helper-baked QMD; the v7 dispatch authors its own per
         * launch but requires qmd_gpu_va to be backed. */
        ga10b_qmd_populate((uint32_t *)p->qmd[i].cpu, p->shader_va[d->shader],
                           p->cbuf_va[i], GA10B_CP_REGISTER_COUNT_V,
                           d->grid[0], d->grid[1], d->grid[2],
                           d->block[0], d->block[1], d->block[2]);
        ga10b_dma_clean(ops, &p->qmd[i]);
    }

    /* QMD pool (v7 addition; gpu_alloc_qmd_pool common.c:1092). */
    if ((rc = alloc_map(ops, vm, GA10B_CP_QMD_POOL_SLOTS * GA10B_CP_QMD_SLOT_BYTES,
                        &p->qmd_pool, &p->qmd_pool_va)) != GA10B_CP_OK) {
        return rc;
    }

    /* v7 ops array: read by the CPU only, so not GPU-mapped. */
    if ((rc = alloc_map(ops, vm, GA10B_CP_PAGE_BYTES, &p->ops, NULL)) != GA10B_CP_OK) {
        return rc;
    }
    struct ga10b_pipeline_op_v7 *arr = (struct ga10b_pipeline_op_v7 *)p->ops.cpu;
    for (unsigned i = 0; i < spec->n_ops; i++) {
        ga10b_cp_fill_op_v7(p, i, &arr[i]);
    }
    ga10b_dma_clean(ops, &p->ops);

    /* The channel has run already: drop stale GMMU translations. */
    if (ga10b_vm_tlb_invalidate(ops, vm) != GA10B_CHAN_OK) return GA10B_CP_ERR_MAP;
    p->built = true;
    return GA10B_CP_OK;
}

void ga10b_cp_fill_handoff(const struct ga10b_cp_pipeline *p, const struct ga10b_chan *ch,
                           struct ga10b_channel_handoff *h)
{
    ga10b_chan_fill_handoff(ch, h);
    if (!p || !p->built || !p->spec) return;
    const struct ga10b_cp_spec *s = p->spec;
    /* gpu_write_handoff_v6 pipeline fields (common.c:998-1066; called
     * at sched.c:477, evict.c:730) plus the v7 pool (common.c:1236-1239).
     * The semaphore stays the channel's own page (the helper aliases the
     * last op's output page, :1036); the dispatch writes it at +0x800. */
    h->version = 7;
    h->expected_payload = s->ops[s->n_ops - 1u].sentinel_bits;
    h->pipeline_n_ops = s->n_ops;
    h->pipeline_ops_phys = p->ops.phys;
    h->input_buf_phys = p->input.phys;
    h->input_buf_size = s->input_bytes;
    h->pipeline_kind = s->kind;
    h->qmd_pool_phys = p->qmd_pool.phys;
    h->qmd_pool_gpu_va = p->qmd_pool_va;
    h->qmd_pool_size_bytes = p->qmd_pool.size;
    h->qmd_pool_n_slots = p->qmd_pool.size / GA10B_CP_QMD_SLOT_BYTES;
}

/* ------------------------------------------------------------------ */
/* Per-kind handoff store                                              */
/* ------------------------------------------------------------------ */

int ga10b_cp_store_put(struct ga10b_cp_store *st, const struct ga10b_channel_handoff *h)
{
    if (!st || !h || h->magic != GA10B_CHANNEL_HANDOFF_MAGIC || !ga10b_handoff_is_v7(h) ||
        ga10b_v7_validate_handoff(h) != GA10B_V7_OK) {
        return GA10B_CP_STORE_ERR_ARG;
    }
    if (h->pipeline_kind >= GA10B_PIPELINE_KIND_COUNT) return GA10B_CP_STORE_ERR_KIND;
    st->h[h->pipeline_kind] = *h;
    st->have[h->pipeline_kind] = true;
    return GA10B_CP_STORE_OK;
}

const struct ga10b_channel_handoff *ga10b_cp_store_get(const struct ga10b_cp_store *st,
                                                       uint32_t kind)
{
    if (!st || kind >= GA10B_PIPELINE_KIND_COUNT || !st->have[kind]) return NULL;
    return &st->h[kind];
}

static bool same_channel(const struct ga10b_channel_handoff *a,
                         const struct ga10b_channel_handoff *b)
{
    return a->userd_phys == b->userd_phys && a->gpfifo_phys == b->gpfifo_phys &&
           a->gpfifo_entries == b->gpfifo_entries && a->pushbuf_phys == b->pushbuf_phys &&
           a->work_submit_token == b->work_submit_token;
}

int ga10b_cp_switch_handoff(const struct ga10b_cp_store *st, uint32_t kind,
                            const struct ga10b_channel_handoff *active,
                            struct ga10b_channel_handoff *out)
{
    if (!st || !out) return GA10B_CP_STORE_ERR_ARG;
    if (kind >= GA10B_PIPELINE_KIND_COUNT) return GA10B_CP_STORE_ERR_KIND;
    const struct ga10b_channel_handoff *s = ga10b_cp_store_get(st, kind);
    if (!s) return GA10B_CP_STORE_ERR_NONE;
    if (active && active->magic == GA10B_CHANNEL_HANDOFF_MAGIC) {
        if (!same_channel(s, active)) return GA10B_CP_STORE_ERR_CHAN;
        *out = *s;
        out->initial_gp_put = active->initial_gp_put;
        out->initial_gp_get = active->initial_gp_get;
        return GA10B_CP_STORE_OK;
    }
    *out = *s;
    return GA10B_CP_STORE_OK;
}

static struct ga10b_cp_store g_cp_store;

struct ga10b_cp_store *ga10b_cp_store_global(void)
{
    return &g_cp_store;
}
