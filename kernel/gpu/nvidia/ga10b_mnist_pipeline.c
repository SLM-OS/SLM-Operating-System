/*
 * ga10b_mnist_pipeline.c - MNIST GPU pipeline on an SLM-OS-made channel
 * (#1088). See ga10b_mnist_pipeline.h.
 *
 * Line references are to scripts/gpu-kernel-mnist.c unless another file
 * is named.
 */
#include "ga10b_mnist_pipeline.h"
#include "ga10b_channel_handoff.h"
#include "ga10b_qmd.h"

#include "string.h"

/* ------------------------------------------------------------------ */
/* Shape tables                                                        */
/* ------------------------------------------------------------------ */

const struct ga10b_mnist_weight_desc ga10b_mnist_weights[GA10B_MNIST_N_WEIGHTS] = {
    /* scripts/mnist-extract-weights.py WEIGHT_MAP; buffer sizes :298-303 */
    [GA10B_MNIST_W_CONV1] = { "Parameter5",     8u * 1u * 5u * 5u * 4u,  4096u },
    [GA10B_MNIST_B_CONV1] = { "Parameter6",     8u * 4u,                 4096u },
    [GA10B_MNIST_W_CONV2] = { "Parameter87",   16u * 8u * 5u * 5u * 4u, 16384u },
    [GA10B_MNIST_B_CONV2] = { "Parameter88",   16u * 4u,                 4096u },
    /* [16,4,4,10] in the model; read as row-major [256,10], same bytes */
    [GA10B_MNIST_W_FC]    = { "Parameter193", 256u * 10u * 4u,          12288u },
    [GA10B_MNIST_B_FC]    = { "Parameter194",  10u * 4u,                 4096u },
};

#define IN       GA10B_MNIST_REF_INPUT
#define W(x)     GA10B_MNIST_REF_WEIGHT(GA10B_MNIST_##x)
#define OUT(n)   GA10B_MNIST_REF_OUTPUT(n)

/* Grid x for a 256-thread 1-D raster over an HxW plane, as the helper
 * computes it: (H*W + 255) / 256. */
#define GX(hw)   (((hw) + 255u) / 256u)

const struct ga10b_mnist_op_desc ga10b_mnist_ops[GA10B_MNIST_N_OPS] = {
    /* Op 0 Conv1 (:396-433): N, C_in, H, W, C_out, kH, kW, pad_h, pad_w */
    { "00_conv1", GA10B_MNIST_SH_CONV, 3, 9, { IN, W(W_CONV1), OUT(0) },
      { 1, 1, 28, 28, 8, 5, 5, 2, 2 },
      { GX(28u * 28u), 8, 1 }, { 256, 1, 1 }, 1u * 8u * 28u * 28u * 4u, 0 },
    /* Op 1 Add+ReLU1 (:435-466): N, C, H, W, apply_relu */
    { "01_addrelu1", GA10B_MNIST_SH_ADDRELU, 3, 5, { OUT(0), W(B_CONV1), OUT(1) },
      { 1, 8, 28, 28, 1 },
      { GX(28u * 28u), 8, 1 }, { 256, 1, 1 }, 1u * 8u * 28u * 28u * 4u, 2 },
    /* Op 2 MaxPool1 (:468-503): N, C, H_in, W_in, kH, kW, sH, sW, H_out, W_out */
    { "02_pool1", GA10B_MNIST_SH_POOL, 2, 10, { OUT(1), OUT(2) },
      { 1, 8, 28, 28, 2, 2, 2, 2, 14, 14 },
      { GX(14u * 14u), 8, 1 }, { 256, 1, 1 }, 1u * 8u * 14u * 14u * 4u, 0 },
    /* Op 3 Conv2 (:505-540) */
    { "03_conv2", GA10B_MNIST_SH_CONV, 3, 9, { OUT(2), W(W_CONV2), OUT(3) },
      { 1, 8, 14, 14, 16, 5, 5, 2, 2 },
      { GX(14u * 14u), 16, 1 }, { 256, 1, 1 }, 1u * 16u * 14u * 14u * 4u, 0 },
    /* Op 4 Add+ReLU2 (:542-573) */
    { "04_addrelu2", GA10B_MNIST_SH_ADDRELU, 3, 5, { OUT(3), W(B_CONV2), OUT(4) },
      { 1, 16, 14, 14, 1 },
      { GX(14u * 14u), 16, 1 }, { 256, 1, 1 }, 1u * 16u * 14u * 14u * 4u, 57 },
    /* Op 5 MaxPool2 (:575-610) */
    { "05_pool2", GA10B_MNIST_SH_POOL, 2, 10, { OUT(4), OUT(5) },
      { 1, 16, 14, 14, 3, 3, 3, 3, 4, 4 },
      { GX(4u * 4u), 16, 1 }, { 256, 1, 1 }, 1u * 16u * 4u * 4u * 4u, 4 },
    /* Op 6 GEMM, SIMT tier (:612-685): M, K, N; block 16x16, grid
     * ceil(N/16) x ceil(M/16) */
    { "06_matmul", GA10B_MNIST_SH_GEMM, 3, 3, { OUT(5), W(W_FC), OUT(6) },
      { 1, 256, 10 },
      { (10u + 15u) / 16u, (1u + 15u) / 16u, 1 }, { 16, 16, 1 }, 1u * 10u * 4u, 0 },
    /* Op 7 AddBias on logits (:687-718): N=1 C=10 H=1 W=1, apply_relu=0 */
    { "07_addbias", GA10B_MNIST_SH_ADDRELU, 3, 5, { OUT(6), W(B_FC), OUT(7) },
      { 1, 10, 1, 1, 0 },
      { 1, 10, 1 }, { 256, 1, 1 }, 1u * 10u * 4u, 0 },
};

#undef IN
#undef W
#undef OUT
#undef GX

uint32_t ga10b_mnist_round_page(uint32_t bytes)
{
    if (bytes == 0u) return 4096u;
    if (bytes > 0xfffff000u) return 0u;
    return (bytes + 4095u) & ~4095u;
}

uint64_t ga10b_mnist_ref_va(const struct ga10b_mnist_vas *va, uint8_t ref,
                            uint32_t op)
{
    if (!va) return 0;
    if (ref == GA10B_MNIST_REF_INPUT) return va->input;
    if (ref >= 0x10u && ref < 0x10u + GA10B_MNIST_N_WEIGHTS) {
        return va->weight[ref - 0x10u];
    }
    if (ref >= 0x20u && ref < 0x20u + GA10B_MNIST_N_OPS) {
        uint32_t n = ref - 0x20u;
        /* An op may write its own output (n == op) but only read
         * earlier ones. */
        return n <= op ? va->output[n] : 0;
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

int ga10b_mnist_fill_cbuf(uint32_t op, const struct ga10b_mnist_vas *va,
                          uint8_t *cbuf, size_t cap)
{
    if (op >= GA10B_MNIST_N_OPS || !va || !cbuf || cap < 512u) return -1;
    const struct ga10b_mnist_op_desc *d = &ga10b_mnist_ops[op];
    memset(cbuf, 0, cap);

    /* Built-in dims: blockDim xyz at 0x00..0x08, gridDim at 0x0c..0x14
     * (gpu-launch-common.c:671 gpu_write_builtin_dims). */
    for (unsigned i = 0; i < 3u; i++) {
        put32(cbuf + 4u * i, d->block[i]);
        put32(cbuf + 12u + 4u * i, d->grid[i]);
    }

    uint32_t off = GA10B_MNIST_CBUF_PARAM_BASE;
    for (unsigned i = 0; i < d->n_ptr; i++) {
        uint64_t v = ga10b_mnist_ref_va(va, d->ptr[i], op);
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
/* ONNX initializer lookup                                             */
/* ------------------------------------------------------------------ */

/* Protobuf wire types. */
#define PB_VARINT 0u
#define PB_I64    1u
#define PB_LEN    2u
#define PB_I32    5u

struct pb {
    const uint8_t *p;
    const uint8_t *end;
};

static bool pb_varint(struct pb *b, uint64_t *v)
{
    uint64_t r = 0;
    for (unsigned shift = 0; shift < 64u; shift += 7u) {
        if (b->p >= b->end) return false;
        uint8_t c = *b->p++;
        r |= (uint64_t)(c & 0x7fu) << shift;
        if (!(c & 0x80u)) {
            *v = r;
            return true;
        }
    }
    return false;
}

/* Read one field: key into *field / *wt, its payload bytes into *sub. */
static bool pb_field(struct pb *b, uint32_t *field, uint32_t *wt, struct pb *sub)
{
    uint64_t key;
    if (!pb_varint(b, &key)) return false;
    *field = (uint32_t)(key >> 3);
    *wt = (uint32_t)(key & 7u);
    sub->p = sub->end = NULL;
    switch (*wt) {
    case PB_VARINT: {
        uint64_t dummy;
        const uint8_t *start = b->p;
        if (!pb_varint(b, &dummy)) return false;
        sub->p = start;
        sub->end = b->p;
        return true;
    }
    case PB_I64:
        if ((size_t)(b->end - b->p) < 8u) return false;
        sub->p = b->p;
        b->p += 8;
        sub->end = b->p;
        return true;
    case PB_I32:
        if ((size_t)(b->end - b->p) < 4u) return false;
        sub->p = b->p;
        b->p += 4;
        sub->end = b->p;
        return true;
    case PB_LEN: {
        uint64_t n;
        if (!pb_varint(b, &n)) return false;
        if (n > (uint64_t)(b->end - b->p)) return false;
        sub->p = b->p;
        b->p += (size_t)n;
        sub->end = b->p;
        return true;
    }
    default:
        return false;   /* groups (3/4) and reserved types */
    }
}

static bool name_eq(const struct pb *s, const char *name)
{
    size_t n = strlen(name);
    return (size_t)(s->end - s->p) == n && memcmp(s->p, name, n) == 0;
}

/* Parse a TensorProto whose name matched. */
static int parse_tensor(struct pb t, struct ga10b_onnx_tensor *out)
{
    memset(out, 0, sizeof(*out));
    bool have_raw = false, have_float = false;
    struct pb raw = { NULL, NULL }, flt = { NULL, NULL };
    while (t.p < t.end) {
        uint32_t f, wt;
        struct pb s;
        if (!pb_field(&t, &f, &wt, &s)) return GA10B_ONNX_ERR_FORMAT;
        if (f == 1u) {                          /* dims, int64 */
            if (wt == PB_VARINT) {
                uint64_t v;
                if (out->ndims >= GA10B_ONNX_MAX_DIMS || !pb_varint(&s, &v)) {
                    return GA10B_ONNX_ERR_FORMAT;
                }
                out->dims[out->ndims++] = (int64_t)v;
            } else if (wt == PB_LEN) {          /* packed */
                while (s.p < s.end) {
                    uint64_t v;
                    if (out->ndims >= GA10B_ONNX_MAX_DIMS || !pb_varint(&s, &v)) {
                        return GA10B_ONNX_ERR_FORMAT;
                    }
                    out->dims[out->ndims++] = (int64_t)v;
                }
            } else {
                return GA10B_ONNX_ERR_FORMAT;
            }
        } else if (f == 2u && wt == PB_VARINT) { /* data_type */
            uint64_t v;
            if (!pb_varint(&s, &v)) return GA10B_ONNX_ERR_FORMAT;
            out->data_type = (uint32_t)v;
        } else if (f == 4u) {                   /* float_data */
            if (wt != PB_LEN) return GA10B_ONNX_ERR_ENCODING;  /* unpacked */
            if (have_float) return GA10B_ONNX_ERR_ENCODING;    /* split */
            flt = s;
            have_float = true;
        } else if (f == 9u && wt == PB_LEN) {   /* raw_data */
            raw = s;
            have_raw = true;
        } else if (f == 13u || f == 14u) {      /* external_data / location */
            if (f == 14u && wt == PB_VARINT) {
                uint64_t v;
                if (!pb_varint(&s, &v)) return GA10B_ONNX_ERR_FORMAT;
                if (v != 0u) return GA10B_ONNX_ERR_ENCODING;
            } else if (f == 13u) {
                return GA10B_ONNX_ERR_ENCODING;
            }
        }
    }
    if (out->data_type != GA10B_ONNX_FLOAT) return GA10B_ONNX_ERR_ENCODING;
    if (have_raw && (raw.end - raw.p) > 0) {
        out->data = raw.p;
        out->bytes = (size_t)(raw.end - raw.p);
    } else if (have_float) {
        out->data = flt.p;
        out->bytes = (size_t)(flt.end - flt.p);
    } else {
        return GA10B_ONNX_ERR_ENCODING;
    }
    uint64_t n = 1;
    for (uint32_t i = 0; i < out->ndims; i++) {
        if (out->dims[i] <= 0 || (uint64_t)out->dims[i] > (1ull << 32)) {
            return GA10B_ONNX_ERR_FORMAT;
        }
        n *= (uint64_t)out->dims[i];
        if (n > (1ull << 32)) return GA10B_ONNX_ERR_FORMAT;
    }
    if (n * 4u != (uint64_t)out->bytes) return GA10B_ONNX_ERR_FORMAT;
    return GA10B_ONNX_OK;
}

int ga10b_onnx_find_initializer(const uint8_t *model, size_t len,
                                const char *name,
                                struct ga10b_onnx_tensor *out)
{
    if (!model || !name || !out || len == 0u) return GA10B_ONNX_ERR_ARG;
    struct pb m = { model, model + len };
    bool saw_graph = false;
    while (m.p < m.end) {
        uint32_t f, wt;
        struct pb g;
        if (!pb_field(&m, &f, &wt, &g)) return GA10B_ONNX_ERR_FORMAT;
        if (f != 7u || wt != PB_LEN) continue;          /* ModelProto.graph */
        saw_graph = true;
        while (g.p < g.end) {
            struct pb t;
            if (!pb_field(&g, &f, &wt, &t)) return GA10B_ONNX_ERR_FORMAT;
            if (f != 5u || wt != PB_LEN) continue;      /* GraphProto.initializer */
            /* Scan the TensorProto for its name (field 8) first. */
            struct pb scan = t;
            bool match = false;
            while (scan.p < scan.end) {
                uint32_t tf, twt;
                struct pb s;
                if (!pb_field(&scan, &tf, &twt, &s)) return GA10B_ONNX_ERR_FORMAT;
                if (tf == 8u && twt == PB_LEN) {
                    match = name_eq(&s, name);
                    break;
                }
            }
            if (match) return parse_tensor(t, out);
        }
    }
    return saw_graph ? GA10B_ONNX_ERR_NOT_FOUND : GA10B_ONNX_ERR_FORMAT;
}

int ga10b_mnist_weights_from_onnx(const uint8_t *model, size_t len,
                                  struct ga10b_mnist_sources *src)
{
    if (!src) return GA10B_ONNX_ERR_ARG;
    for (unsigned w = 0; w < GA10B_MNIST_N_WEIGHTS; w++) {
        struct ga10b_onnx_tensor t;
        int rc = ga10b_onnx_find_initializer(model, len,
                                             ga10b_mnist_weights[w].onnx_name, &t);
        if (rc != GA10B_ONNX_OK) return rc;
        if (t.bytes != ga10b_mnist_weights[w].bytes) return GA10B_ONNX_ERR_ENCODING;
        src->weight[w] = t.data;
    }
    return GA10B_ONNX_OK;
}

/* ------------------------------------------------------------------ */
/* Building                                                            */
/* ------------------------------------------------------------------ */

static int alloc_map(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                     uint32_t bytes, struct ga10b_dma *d, uint64_t *va)
{
    if (ga10b_dma_alloc(ops, bytes, d) != GA10B_CHAN_OK) return GA10B_MNIST_ERR_NOMEM;
    if (!va) return GA10B_MNIST_OK;
    /* MAP_BUFFER_EX without the CACHEABLE flag (gpu-launch-common.c:513):
     * not GPU-L2 cacheable. */
    if (ga10b_vm_map_dma(ops, vm, d, 0u, false, va) != GA10B_CHAN_OK) {
        return GA10B_MNIST_ERR_MAP;
    }
    return GA10B_MNIST_OK;
}

void ga10b_mnist_pipeline_free(const struct ga10b_gpu_ops *ops,
                               struct ga10b_mnist_pipeline *p)
{
    if (!ops || !p) return;
    for (unsigned i = 0; i < GA10B_MNIST_N_SHADERS; i++) {
        ga10b_dma_free(ops, &p->shader[i]);
    }
    for (unsigned i = 0; i < GA10B_MNIST_N_WEIGHTS; i++) {
        ga10b_dma_free(ops, &p->weight[i]);
    }
    ga10b_dma_free(ops, &p->input);
    for (unsigned i = 0; i < GA10B_MNIST_N_OPS; i++) {
        ga10b_dma_free(ops, &p->qmd[i]);
        ga10b_dma_free(ops, &p->cbuf[i]);
        ga10b_dma_free(ops, &p->out[i]);
    }
    ga10b_dma_free(ops, &p->qmd_pool);
    ga10b_dma_free(ops, &p->ops);
    p->built = false;
}

void ga10b_mnist_fill_op_v7(const struct ga10b_mnist_pipeline *p, uint32_t op,
                            struct ga10b_pipeline_op_v7 *o)
{
    memset(o, 0, sizeof(*o));
    if (!p || op >= GA10B_MNIST_N_OPS) return;
    const struct ga10b_mnist_op_desc *d = &ga10b_mnist_ops[op];
    unsigned sh = d->shader;
    /* :897-920 */
    o->qmd_gpu_va = p->qmd_va[op];
    o->output_phys = p->out[op].phys + (uint64_t)d->sentinel_cell * 4u;
    o->expected_payload = 0u;            /* any-nonzero mode */
    o->flags = 0u;
    o->shader_gpu_va = p->shader_va[sh];
    o->cbuf_gpu_va = p->cbuf_va[op];
    o->register_count_v = GA10B_MNIST_REGISTER_COUNT_V;
    o->grid_x = d->grid[0];
    o->grid_y = d->grid[1];
    o->grid_z = d->grid[2];
    o->block_x = d->block[0];
    o->block_y = d->block[1];
    o->block_z = d->block[2];
    o->smem_size_bytes = 0u;             /* SIMT: encoder defaults (:676-684) */
    o->slm_size_bytes = 0u;
    o->barrier_count = 0u;
    /* v7.1: shader phys + SASS size rounded to a page (:282-289). */
    o->shader_phys = p->shader[sh].phys;
    o->shader_size_bytes = ga10b_mnist_round_page(p->sass_bytes[sh]);
}

int ga10b_mnist_pipeline_build(const struct ga10b_gpu_ops *ops,
                               struct ga10b_vm *vm,
                               const struct ga10b_mnist_sources *src,
                               struct ga10b_mnist_pipeline *p)
{
    if (!ops || !vm || !src || !p) return GA10B_MNIST_ERR_ARG;
    for (unsigned i = 0; i < GA10B_MNIST_N_SHADERS; i++) {
        if (!src->sass[i] || src->sass_bytes[i] == 0u ||
            src->sass_bytes[i] > GA10B_MNIST_SHADER_BUF_BYTES) {
            return GA10B_MNIST_ERR_ARG;
        }
    }
    for (unsigned i = 0; i < GA10B_MNIST_N_WEIGHTS; i++) {
        if (!src->weight[i]) return GA10B_MNIST_ERR_ARG;
    }

    memset(p, 0, sizeof(*p));
    int rc;

    /* Shaders: 64 KB each, zero tail (gpu_load_shader_buffer :557-560;
     * the conv shader lives in ctx's 64 KB shader dmabuf, :397/:470). */
    for (unsigned i = 0; i < GA10B_MNIST_N_SHADERS; i++) {
        if ((rc = alloc_map(ops, vm, GA10B_MNIST_SHADER_BUF_BYTES, &p->shader[i],
                            &p->shader_va[i])) != GA10B_MNIST_OK) {
            goto fail;
        }
        memcpy(p->shader[i].cpu, src->sass[i], src->sass_bytes[i]);
        p->sass_bytes[i] = src->sass_bytes[i];
        ga10b_dma_clean(ops, &p->shader[i]);
    }

    /* Weights + input (:298-329). */
    for (unsigned i = 0; i < GA10B_MNIST_N_WEIGHTS; i++) {
        if ((rc = alloc_map(ops, vm, ga10b_mnist_weights[i].buf_bytes, &p->weight[i],
                            &p->va.weight[i])) != GA10B_MNIST_OK) {
            goto fail;
        }
        memcpy(p->weight[i].cpu, src->weight[i], ga10b_mnist_weights[i].bytes);
        ga10b_dma_clean(ops, &p->weight[i]);
    }
    if ((rc = alloc_map(ops, vm, 4096u, &p->input, &p->va.input)) != GA10B_MNIST_OK) {
        goto fail;
    }
    if (src->input) memcpy(p->input.cpu, src->input, GA10B_MNIST_INPUT_BYTES);
    ga10b_dma_clean(ops, &p->input);

    /* Per op: QMD page, cbuf page, output buffer, all allocated before
     * the cbufs are written (:375-385). Outputs start zeroed (:720-725,
     * :938-941); ops->alloc_pages hands out zeroed, cleaned pages. */
    for (unsigned i = 0; i < GA10B_MNIST_N_OPS; i++) {
        if ((rc = alloc_map(ops, vm, GA10B_MNIST_QMD_PAGE_BYTES, &p->qmd[i],
                            &p->qmd_va[i])) != GA10B_MNIST_OK ||
            (rc = alloc_map(ops, vm, GA10B_MNIST_CBUF_PAGE_BYTES, &p->cbuf[i],
                            &p->cbuf_va[i])) != GA10B_MNIST_OK ||
            (rc = alloc_map(ops, vm, ga10b_mnist_round_page(ga10b_mnist_ops[i].output_bytes),
                            &p->out[i], &p->va.output[i])) != GA10B_MNIST_OK) {
            goto fail;
        }
    }

    for (unsigned i = 0; i < GA10B_MNIST_N_OPS; i++) {
        const struct ga10b_mnist_op_desc *d = &ga10b_mnist_ops[i];
        if (ga10b_mnist_fill_cbuf(i, &p->va, (uint8_t *)p->cbuf[i].cpu,
                                  p->cbuf[i].size) != 0) {
            rc = GA10B_MNIST_ERR_ARG;
            goto fail;
        }
        ga10b_dma_clean(ops, &p->cbuf[i]);
        /* Helper-baked QMD (gpu_populate_qmd_at + dim overrides). The v7
         * dispatch authors its own per launch from the ops array; this
         * one only backs qmd_gpu_va, which the dispatch requires non-zero
         * and keeps for byte-compare. */
        ga10b_qmd_populate((uint32_t *)p->qmd[i].cpu, p->shader_va[d->shader],
                           p->cbuf_va[i], GA10B_MNIST_REGISTER_COUNT_V,
                           d->grid[0], d->grid[1], d->grid[2],
                           d->block[0], d->block[1], d->block[2]);
        ga10b_dma_clean(ops, &p->qmd[i]);
    }

    /* QMD pool (gpu_alloc_qmd_pool, gpu-launch-common.c:1092): zeroed. */
    if ((rc = alloc_map(ops, vm, GA10B_MNIST_QMD_POOL_SLOTS * GA10B_MNIST_QMD_SLOT_BYTES,
                        &p->qmd_pool, &p->qmd_pool_va)) != GA10B_MNIST_OK) {
        goto fail;
    }

    /* v7 ops array (:890-922). Only the CPU reads it (by physical
     * address), so it is not mapped into the GPU VM. */
    if ((rc = alloc_map(ops, vm, 4096u, &p->ops, NULL)) != GA10B_MNIST_OK) goto fail;
    static_assert(GA10B_MNIST_N_OPS * sizeof(struct ga10b_pipeline_op_v7) <= 4096u,
                  "v7 ops array fits one page");
    struct ga10b_pipeline_op_v7 *arr = (struct ga10b_pipeline_op_v7 *)p->ops.cpu;
    for (unsigned i = 0; i < GA10B_MNIST_N_OPS; i++) {
        ga10b_mnist_fill_op_v7(p, i, &arr[i]);
    }
    ga10b_dma_clean(ops, &p->ops);

    /* The channel has already run (submit tests), so the GMMU may hold
     * translations for this VM: invalidate after adding the mappings. */
    if (ga10b_vm_tlb_invalidate(ops, vm) != GA10B_CHAN_OK) {
        rc = GA10B_MNIST_ERR_MAP;
        goto fail;
    }
    p->built = true;
    return GA10B_MNIST_OK;

fail:
    /* Pages already mapped into `vm` (the live channel's address space)
     * are not returned to the PMM: freeing them would leave GPU PTEs
     * pointing at reused memory. A failed build is rare (allocation or
     * TLB-invalidate failure), so leaking it is the safe choice. */
    return rc;
}

void ga10b_mnist_pipeline_fill_handoff(const struct ga10b_mnist_pipeline *p,
                                       const struct ga10b_chan *ch,
                                       struct ga10b_channel_handoff *h)
{
    ga10b_chan_fill_handoff(ch, h);
    if (!p || !p->built) return;
    /* gpu_write_handoff_v7 (gpu-launch-common.c:1196-1244). The v3
     * single-shot fields stay zero so nothing falls back to a one-QMD
     * launch. */
    h->version = 7;
    h->expected_payload = GA10B_MNIST_OP7_SENTINEL_BITS;
    h->pipeline_n_ops = GA10B_MNIST_N_OPS;
    h->pipeline_ops_phys = p->ops.phys;
    h->input_buf_phys = p->input.phys;
    h->input_buf_size = GA10B_MNIST_INPUT_BYTES;
    h->pipeline_kind = GA10B_PIPELINE_KIND_MNIST;
    h->qmd_pool_phys = p->qmd_pool.phys;
    h->qmd_pool_gpu_va = p->qmd_pool_va;
    h->qmd_pool_size_bytes = p->qmd_pool.size;
    h->qmd_pool_n_slots = p->qmd_pool.size / GA10B_MNIST_QMD_SLOT_BYTES;
}
