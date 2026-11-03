/*
 * test_ga10b_mnist_pipeline.c - MNIST GPU pipeline built on an
 * SLM-OS-made channel (#1088).
 *
 * Expected values are written out from scripts/gpu-kernel-mnist.c
 * (cbuf layouts :396-718, buffer sizes :298-385, v7 ops :890-922) and
 * gpu-launch-common.c (gpu_write_handoff_v7 :1155), independently of
 * the tables in ga10b_mnist_pipeline.c. The weight checksums are FNV-1a
 * of the W_*.bin / B_*.bin files the board's helper loaded (produced
 * by scripts/mnist-extract-weights.py from models/test/mnist.onnx), so
 * the in-kernel ONNX walk is pinned to the exact bytes Linux used.
 *
 * The builder test runs ga10b_mnist_pipeline_build against fake ops
 * whose "physical" pages sit above 4 GB.
 */
#include "unity.h"
#include "../gpu/nvidia/ga10b_mnist_pipeline.h"
#include "../gpu/nvidia/ga10b_channel_handoff.h"
#include "../gpu/nvidia/ga10b_qmd.h"
#include "operator_library.h"   /* operator_library_checksum32 = FNV-1a */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern const uint8_t mnist_onnx_start[];
extern const uint8_t mnist_onnx_end[];
extern const uint8_t mnist_input_synth_start[];
extern const uint8_t mnist_input_synth_end[];

/* ---- fake GPU memory ---- */

#define POOL_PAGES 400u
#define POOL_PHYS  0x2a0000000ull

static alignas(4096) uint8_t g_pool[POOL_PAGES][4096];
static bool g_used[POOL_PAGES];
static unsigned g_allocs, g_frees, g_wr32s, g_cleans;

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
        i += k;   /* skip past the used page */
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

static void f_clean(void *ctx, const void *p, size_t n) { (void)ctx; (void)p; (void)n; g_cleans++; }
static void f_inval(void *ctx, const void *p, size_t n) { (void)ctx; (void)p; (void)n; }
/* MMU_CTRL: PRI FIFO has space and is empty (same value as test_ga10b_chan). */
static uint32_t f_rd32(void *ctx, uint32_t off) { (void)ctx; (void)off; return 0x00018000u; }
static void f_wr32(void *ctx, uint32_t off, uint32_t v) { (void)ctx; (void)off; (void)v; g_wr32s++; }
static void f_udelay(void *ctx, uint32_t us) { (void)ctx; (void)us; }

static const struct ga10b_gpu_ops g_ops = {
    .rd32 = f_rd32, .wr32 = f_wr32, .udelay = f_udelay,
    .alloc_pages = f_alloc, .free_pages = f_free, .phys_to_cpu = f_p2c,
    .cache_clean = f_clean, .cache_inval = f_inval,
};

static void pool_reset(void)
{
    memset(g_used, 0, sizeof(g_used));
    g_allocs = g_frees = g_wr32s = g_cleans = 0;
}

static unsigned pool_pages_used(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < POOL_PAGES; i++) {
        n += g_used[i] ? 1u : 0u;
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

/* ---- shape tables ---- */

static void test_round_page(void)
{
    TEST_ASSERT_EQUAL_UINT32(4096, ga10b_mnist_round_page(0));
    TEST_ASSERT_EQUAL_UINT32(4096, ga10b_mnist_round_page(1));
    TEST_ASSERT_EQUAL_UINT32(4096, ga10b_mnist_round_page(4096));
    TEST_ASSERT_EQUAL_UINT32(28672, ga10b_mnist_round_page(25088));
    TEST_ASSERT_EQUAL_UINT32(8192, ga10b_mnist_round_page(6272));
    TEST_ASSERT_EQUAL_UINT32(16384, ga10b_mnist_round_page(12544));
    TEST_ASSERT_EQUAL_UINT32(0, ga10b_mnist_round_page(0xfffff001u));
}

static void test_op_table(void)
{
    /* grid / block / output bytes / sentinel cell per op. */
    static const uint32_t want[8][9] = {
        /* gx gy gz  bx  by bz  out    sentinel shader */
        { 4, 8, 1, 256, 1, 1, 25088,  0, GA10B_MNIST_SH_CONV },
        { 4, 8, 1, 256, 1, 1, 25088,  2, GA10B_MNIST_SH_ADDRELU },
        { 1, 8, 1, 256, 1, 1,  6272,  0, GA10B_MNIST_SH_POOL },
        { 1, 16, 1, 256, 1, 1, 12544, 0, GA10B_MNIST_SH_CONV },
        { 1, 16, 1, 256, 1, 1, 12544, 57, GA10B_MNIST_SH_ADDRELU },
        { 1, 16, 1, 256, 1, 1,  1024,  4, GA10B_MNIST_SH_POOL },
        { 1, 1, 1, 16, 16, 1,     40,  0, GA10B_MNIST_SH_GEMM },
        { 1, 10, 1, 256, 1, 1,    40,  0, GA10B_MNIST_SH_ADDRELU },
    };
    for (unsigned i = 0; i < 8; i++) {
        const struct ga10b_mnist_op_desc *d = &ga10b_mnist_ops[i];
        TEST_ASSERT_EQUAL_UINT32(want[i][0], d->grid[0]);
        TEST_ASSERT_EQUAL_UINT32(want[i][1], d->grid[1]);
        TEST_ASSERT_EQUAL_UINT32(want[i][2], d->grid[2]);
        TEST_ASSERT_EQUAL_UINT32(want[i][3], d->block[0]);
        TEST_ASSERT_EQUAL_UINT32(want[i][4], d->block[1]);
        TEST_ASSERT_EQUAL_UINT32(want[i][5], d->block[2]);
        TEST_ASSERT_EQUAL_UINT32(want[i][6], d->output_bytes);
        TEST_ASSERT_EQUAL_UINT32(want[i][7], d->sentinel_cell);
        TEST_ASSERT_EQUAL_UINT32(want[i][8], d->shader);
        TEST_ASSERT_TRUE(d->sentinel_cell * 4u < d->output_bytes);
    }
    /* Each op reads the previous op's output (the chain). */
    for (unsigned i = 1; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(GA10B_MNIST_REF_OUTPUT(i - 1), ga10b_mnist_ops[i].ptr[0]);
    }
    TEST_ASSERT_EQUAL_UINT8(GA10B_MNIST_REF_INPUT, ga10b_mnist_ops[0].ptr[0]);
    /* Op 7's output (logits) is read from cell 0 by
     * ga10b_bringup_read_pipeline_output via output_phys. */
    TEST_ASSERT_EQUAL_UINT32(0, ga10b_mnist_ops[7].sentinel_cell);

    static const uint32_t wbytes[6] = { 800, 32, 12800, 64, 10240, 40 };
    static const uint32_t wbuf[6] = { 4096, 4096, 16384, 4096, 12288, 4096 };
    for (unsigned w = 0; w < 6; w++) {
        TEST_ASSERT_EQUAL_UINT32(wbytes[w], ga10b_mnist_weights[w].bytes);
        TEST_ASSERT_EQUAL_UINT32(wbuf[w], ga10b_mnist_weights[w].buf_bytes);
    }
    TEST_ASSERT_EQUAL_STRING("Parameter193", ga10b_mnist_weights[GA10B_MNIST_W_FC].onnx_name);
}

/* ---- cbuf layouts ---- */

static void fake_vas(struct ga10b_mnist_vas *va)
{
    va->input = 0x4100000ull;
    for (unsigned w = 0; w < 6; w++) {
        va->weight[w] = 0x4200000ull + 0x10000ull * w;
    }
    for (unsigned o = 0; o < 8; o++) {
        va->output[o] = 0x5000000ull + 0x100000ull * o;
    }
}

static void check_dims(const uint8_t *c, uint32_t bx, uint32_t by, uint32_t gx,
                       uint32_t gy)
{
    TEST_ASSERT_EQUAL_UINT32(bx, rd32le(c + 0x00));
    TEST_ASSERT_EQUAL_UINT32(by, rd32le(c + 0x04));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(c + 0x08));
    TEST_ASSERT_EQUAL_UINT32(gx, rd32le(c + 0x0c));
    TEST_ASSERT_EQUAL_UINT32(gy, rd32le(c + 0x10));
    TEST_ASSERT_EQUAL_UINT32(1, rd32le(c + 0x14));
}

static void check_ints(const uint8_t *c, uint32_t off, const int32_t *v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT32(v[i], (int32_t)rd32le(c + off + 4u * i));
    }
    /* Nothing written past the last argument. */
    for (uint32_t o = off + 4u * n; o < 512u; o++) {
        TEST_ASSERT_EQUAL_UINT8(0, c[o]);
    }
}

static uint8_t g_cbuf[4096];

static void test_cbuf_layouts(void)
{
    struct ga10b_mnist_vas va;
    fake_vas(&va);
    uint8_t *c = g_cbuf;

    /* Op 0 conv1 (:400-415) */
    memset(c, 0xa5, sizeof(g_cbuf));
    TEST_ASSERT_EQUAL_INT(0, ga10b_mnist_fill_cbuf(0, &va, c, sizeof(g_cbuf)));
    check_dims(c, 256, 1, 4, 8);
    TEST_ASSERT_EQUAL_HEX64(va.input, rd64le(c + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.weight[GA10B_MNIST_W_CONV1], rd64le(c + 0x168));
    TEST_ASSERT_EQUAL_HEX64(va.output[0], rd64le(c + 0x170));
    static const int32_t conv1[] = { 1, 1, 28, 28, 8, 5, 5, 2, 2 };
    check_ints(c, 0x178, conv1, 9);
    for (uint32_t o = 0x18; o < 0x160; o++) {
        TEST_ASSERT_EQUAL_UINT8(0, c[o]);
    }
    TEST_ASSERT_EQUAL_UINT8(0, c[4095]);    /* whole page zeroed */

    /* Op 1 addrelu1 (:441-450) */
    TEST_ASSERT_EQUAL_INT(0, ga10b_mnist_fill_cbuf(1, &va, c, sizeof(g_cbuf)));
    check_dims(c, 256, 1, 4, 8);
    TEST_ASSERT_EQUAL_HEX64(va.output[0], rd64le(c + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.weight[GA10B_MNIST_B_CONV1], rd64le(c + 0x168));
    TEST_ASSERT_EQUAL_HEX64(va.output[1], rd64le(c + 0x170));
    static const int32_t ar1[] = { 1, 8, 28, 28, 1 };
    check_ints(c, 0x178, ar1, 5);

    /* Op 2 pool1 (:474-487): two pointers, ints from 0x170 */
    TEST_ASSERT_EQUAL_INT(0, ga10b_mnist_fill_cbuf(2, &va, c, sizeof(g_cbuf)));
    check_dims(c, 256, 1, 1, 8);
    TEST_ASSERT_EQUAL_HEX64(va.output[1], rd64le(c + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.output[2], rd64le(c + 0x168));
    static const int32_t p1[] = { 1, 8, 28, 28, 2, 2, 2, 2, 14, 14 };
    check_ints(c, 0x170, p1, 10);

    /* Op 3 conv2 (:511-524) */
    TEST_ASSERT_EQUAL_INT(0, ga10b_mnist_fill_cbuf(3, &va, c, sizeof(g_cbuf)));
    check_dims(c, 256, 1, 1, 16);
    TEST_ASSERT_EQUAL_HEX64(va.output[2], rd64le(c + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.weight[GA10B_MNIST_W_CONV2], rd64le(c + 0x168));
    TEST_ASSERT_EQUAL_HEX64(va.output[3], rd64le(c + 0x170));
    static const int32_t conv2[] = { 1, 8, 14, 14, 16, 5, 5, 2, 2 };
    check_ints(c, 0x178, conv2, 9);

    /* Op 4 addrelu2 (:548-557) */
    TEST_ASSERT_EQUAL_INT(0, ga10b_mnist_fill_cbuf(4, &va, c, sizeof(g_cbuf)));
    check_dims(c, 256, 1, 1, 16);
    TEST_ASSERT_EQUAL_HEX64(va.output[3], rd64le(c + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.weight[GA10B_MNIST_B_CONV2], rd64le(c + 0x168));
    TEST_ASSERT_EQUAL_HEX64(va.output[4], rd64le(c + 0x170));
    static const int32_t ar2[] = { 1, 16, 14, 14, 1 };
    check_ints(c, 0x178, ar2, 5);

    /* Op 5 pool2 (:581-594) */
    TEST_ASSERT_EQUAL_INT(0, ga10b_mnist_fill_cbuf(5, &va, c, sizeof(g_cbuf)));
    check_dims(c, 256, 1, 1, 16);
    TEST_ASSERT_EQUAL_HEX64(va.output[4], rd64le(c + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.output[5], rd64le(c + 0x168));
    static const int32_t p2[] = { 1, 16, 14, 14, 3, 3, 3, 3, 4, 4 };
    check_ints(c, 0x170, p2, 10);

    /* Op 6 GEMM SIMT (:633-640) */
    TEST_ASSERT_EQUAL_INT(0, ga10b_mnist_fill_cbuf(6, &va, c, sizeof(g_cbuf)));
    check_dims(c, 16, 16, 1, 1);
    TEST_ASSERT_EQUAL_HEX64(va.output[5], rd64le(c + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.weight[GA10B_MNIST_W_FC], rd64le(c + 0x168));
    TEST_ASSERT_EQUAL_HEX64(va.output[6], rd64le(c + 0x170));
    static const int32_t mkn[] = { 1, 256, 10 };
    check_ints(c, 0x178, mkn, 3);

    /* Op 7 addbias (:693-702) */
    TEST_ASSERT_EQUAL_INT(0, ga10b_mnist_fill_cbuf(7, &va, c, sizeof(g_cbuf)));
    check_dims(c, 256, 1, 1, 10);
    TEST_ASSERT_EQUAL_HEX64(va.output[6], rd64le(c + 0x160));
    TEST_ASSERT_EQUAL_HEX64(va.weight[GA10B_MNIST_B_FC], rd64le(c + 0x168));
    TEST_ASSERT_EQUAL_HEX64(va.output[7], rd64le(c + 0x170));
    static const int32_t ab[] = { 1, 10, 1, 1, 0 };
    check_ints(c, 0x178, ab, 5);
}

static void test_cbuf_errors(void)
{
    struct ga10b_mnist_vas va;
    fake_vas(&va);
    TEST_ASSERT_EQUAL_INT(-1, ga10b_mnist_fill_cbuf(8, &va, g_cbuf, sizeof(g_cbuf)));
    TEST_ASSERT_EQUAL_INT(-1, ga10b_mnist_fill_cbuf(0, NULL, g_cbuf, sizeof(g_cbuf)));
    TEST_ASSERT_EQUAL_INT(-1, ga10b_mnist_fill_cbuf(0, &va, g_cbuf, 511));
    va.weight[GA10B_MNIST_W_CONV1] = 0;   /* unmapped argument */
    TEST_ASSERT_EQUAL_INT(-1, ga10b_mnist_fill_cbuf(0, &va, g_cbuf, sizeof(g_cbuf)));

    fake_vas(&va);
    /* Forward references and junk refs resolve to 0. */
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_mnist_ref_va(&va, GA10B_MNIST_REF_OUTPUT(3), 2));
    TEST_ASSERT_EQUAL_HEX64(va.output[2], ga10b_mnist_ref_va(&va, GA10B_MNIST_REF_OUTPUT(2), 2));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_mnist_ref_va(&va, 0x16u, 0));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_mnist_ref_va(&va, 0x28u, 7));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_mnist_ref_va(NULL, 0, 0));
}

/* ---- ONNX walk ---- */

/* Minimal protobuf writer for synthetic models. */
static size_t pb_key(uint8_t *b, uint32_t field, uint32_t wt)
{
    b[0] = (uint8_t)((field << 3) | wt);
    return 1;
}

static size_t pb_vint(uint8_t *b, uint64_t v)
{
    size_t n = 0;
    do {
        uint8_t c = (uint8_t)(v & 0x7fu);
        v >>= 7;
        b[n++] = c | (v ? 0x80u : 0u);
    } while (v);
    return n;
}

static size_t pb_bytes(uint8_t *b, uint32_t field, const void *p, size_t len)
{
    size_t n = pb_key(b, field, 2);
    n += pb_vint(b + n, len);
    memcpy(b + n, p, len);
    return n + len;
}

/* TensorProto: name, dims (packed or not), data_type, payload. */
static size_t make_tensor(uint8_t *b, const char *name, const int64_t *dims,
                          unsigned nd, bool packed_dims, uint32_t dt,
                          uint32_t payload_field, const uint8_t *data, size_t len)
{
    size_t n = 0;
    if (packed_dims) {
        uint8_t tmp[32];
        size_t t = 0;
        for (unsigned i = 0; i < nd; i++) {
            t += pb_vint(tmp + t, (uint64_t)dims[i]);
        }
        n += pb_bytes(b + n, 1, tmp, t);
    } else {
        for (unsigned i = 0; i < nd; i++) {
            n += pb_key(b + n, 1, 0);
            n += pb_vint(b + n, (uint64_t)dims[i]);
        }
    }
    n += pb_key(b + n, 2, 0);
    n += pb_vint(b + n, dt);
    if (data) n += pb_bytes(b + n, payload_field, data, len);
    n += pb_bytes(b + n, 8, name, strlen(name));
    return n;
}

static uint8_t g_model[512];

static size_t make_model(const uint8_t *t1, size_t l1, const uint8_t *t2, size_t l2)
{
    uint8_t graph[400];
    size_t g = 0;
    g += pb_bytes(graph + g, 2, "g", 1);            /* GraphProto.name */
    g += pb_bytes(graph + g, 5, t1, l1);
    if (t2) g += pb_bytes(graph + g, 5, t2, l2);
    size_t n = 0;
    n += pb_key(g_model + n, 1, 0);                 /* ir_version */
    n += pb_vint(g_model + n, 3);
    n += pb_bytes(g_model + n, 2, "x", 1);          /* producer_name */
    n += pb_bytes(g_model + n, 7, graph, g);
    return n;
}

static void test_onnx_synthetic(void)
{
    static const uint8_t data[24] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                      13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
    static const int64_t d23[] = { 2, 3 };
    static const int64_t d6[] = { 6 };
    uint8_t ta[96], tb[96];
    size_t la = make_tensor(ta, "A", d23, 2, false, 1, 9, data, 24);    /* raw_data */
    size_t lb = make_tensor(tb, "Bee", d6, 1, true, 1, 4, data, 24);    /* float_data */
    size_t len = make_model(ta, la, tb, lb);

    struct ga10b_onnx_tensor t;
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_OK, ga10b_onnx_find_initializer(g_model, len, "A", &t));
    TEST_ASSERT_EQUAL_UINT32(2, t.ndims);
    TEST_ASSERT_EQUAL_INT64(2, t.dims[0]);
    TEST_ASSERT_EQUAL_INT64(3, t.dims[1]);
    TEST_ASSERT_EQUAL_UINT64(24, t.bytes);
    TEST_ASSERT_EQUAL_MEMORY(data, t.data, 24);
    TEST_ASSERT_TRUE(t.data > g_model && t.data + 24 <= g_model + len);

    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_OK, ga10b_onnx_find_initializer(g_model, len, "Bee", &t));
    TEST_ASSERT_EQUAL_UINT32(1, t.ndims);
    TEST_ASSERT_EQUAL_INT64(6, t.dims[0]);
    TEST_ASSERT_EQUAL_MEMORY(data, t.data, 24);

    /* Name match is exact, not a prefix. */
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_NOT_FOUND, ga10b_onnx_find_initializer(g_model, len, "Be", &t));
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_NOT_FOUND, ga10b_onnx_find_initializer(g_model, len, "C", &t));
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_ARG, ga10b_onnx_find_initializer(NULL, len, "A", &t));
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_ARG, ga10b_onnx_find_initializer(g_model, 0, "A", &t));

    /* Truncation anywhere inside the graph is a format error, never an
     * out-of-bounds read (ASan / UBSan builds check the latter). */
    for (size_t cut = 1; cut < len; cut++) {
        int rc = ga10b_onnx_find_initializer(g_model, cut, "Bee", &t);
        TEST_ASSERT_TRUE(rc == GA10B_ONNX_ERR_FORMAT || rc == GA10B_ONNX_ERR_NOT_FOUND);
    }

    /* Dims product disagrees with the payload. */
    static const int64_t d5[] = { 5 };
    la = make_tensor(ta, "A", d5, 1, false, 1, 9, data, 24);
    len = make_model(ta, la, NULL, 0);
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_FORMAT, ga10b_onnx_find_initializer(g_model, len, "A", &t));

    /* Not FLOAT. */
    la = make_tensor(ta, "A", d6, 1, false, 7, 9, data, 24);
    len = make_model(ta, la, NULL, 0);
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_ENCODING, ga10b_onnx_find_initializer(g_model, len, "A", &t));

    /* No payload. */
    la = make_tensor(ta, "A", d6, 1, false, 1, 9, NULL, 0);
    len = make_model(ta, la, NULL, 0);
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_ENCODING, ga10b_onnx_find_initializer(g_model, len, "A", &t));

    /* Unpacked float_data (wire type 5 per element) is not contiguous. */
    size_t n = 0;
    n += pb_key(ta + n, 1, 0); n += pb_vint(ta + n, 1);
    n += pb_key(ta + n, 2, 0); n += pb_vint(ta + n, 1);
    n += pb_key(ta + n, 4, 5); memcpy(ta + n, data, 4); n += 4;
    n += pb_bytes(ta + n, 8, "A", 1);
    len = make_model(ta, n, NULL, 0);
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_ENCODING, ga10b_onnx_find_initializer(g_model, len, "A", &t));

    /* A buffer with no graph at all. */
    uint8_t junk[3] = { 0x08, 0x01, 0x00 };
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_FORMAT, ga10b_onnx_find_initializer(junk, 2, "A", &t));
    /* Group wire type. */
    uint8_t grp[2] = { 0x0b, 0x00 };
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_FORMAT, ga10b_onnx_find_initializer(grp, 2, "A", &t));
}

/* FNV-1a, first and last little-endian word of each file the board's
 * helper loaded (scripts/mnist-weights/, extraction script output). */
static const struct { uint32_t fnv, first, last; } g_weight_ref[6] = {
    { 0xa1891254u, 0xbc11e916u, 0xbe42eb19u },   /* W_conv1.bin */
    { 0x20f2003fu, 0xbe256aabu, 0xbdf808a4u },   /* B_conv1.bin */
    { 0xc314b674u, 0xbd46e30au, 0xbde94118u },   /* W_conv2.bin */
    { 0xd88d53d8u, 0xbda87212u, 0xbd1e55a3u },   /* B_conv2.bin */
    { 0x29b6b1cdu, 0x3dbbaa05u, 0x3df9e79eu },   /* W_fc.bin    */
    { 0x1cf9a518u, 0xbd37baf4u, 0xbd5f65c5u },   /* B_fc.bin    */
};

static void test_onnx_embedded_weights(void)
{
    size_t len = (size_t)(mnist_onnx_end - mnist_onnx_start);
    TEST_ASSERT_EQUAL_UINT64(26143, len);

    struct ga10b_mnist_sources src;
    memset(&src, 0, sizeof(src));
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_OK, ga10b_mnist_weights_from_onnx(mnist_onnx_start, len, &src));
    for (unsigned w = 0; w < 6; w++) {
        uint32_t n = ga10b_mnist_weights[w].bytes;
        TEST_ASSERT_NOT_NULL(src.weight[w]);
        TEST_ASSERT_EQUAL_HEX32(g_weight_ref[w].fnv, operator_library_checksum32(src.weight[w], n));
        TEST_ASSERT_EQUAL_HEX32(g_weight_ref[w].first, rd32le(src.weight[w]));
        TEST_ASSERT_EQUAL_HEX32(g_weight_ref[w].last, rd32le(src.weight[w] + n - 4u));
    }

    struct ga10b_onnx_tensor t;
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_OK,
        ga10b_onnx_find_initializer(mnist_onnx_start, len, "Parameter193", &t));
    TEST_ASSERT_EQUAL_UINT32(4, t.ndims);
    TEST_ASSERT_EQUAL_INT64(16, t.dims[0]);
    TEST_ASSERT_EQUAL_INT64(10, t.dims[3]);
    /* An int64 shape initializer is in the model too: not FLOAT. */
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_ERR_ENCODING,
        ga10b_onnx_find_initializer(mnist_onnx_start, len, "Parameter193_reshape1_shape", &t));
}

static void test_embedded_input_fixture(void)
{
    size_t len = (size_t)(mnist_input_synth_end - mnist_input_synth_start);
    TEST_ASSERT_EQUAL_UINT64(GA10B_MNIST_INPUT_BYTES, len);
    /* input_synth.bin from the extraction script (default_rng(42)). */
    TEST_ASSERT_EQUAL_HEX32(0xcf889c9fu, operator_library_checksum32(mnist_input_synth_start, len));
}

/* ---- builder ---- */

static uint8_t g_sass[4][64];
static struct ga10b_mnist_pipeline g_pipe;

static void make_sources(struct ga10b_mnist_sources *src)
{
    memset(src, 0, sizeof(*src));
    static const uint32_t sizes[4] = { 64, 48, 40, 32 };
    for (unsigned i = 0; i < 4; i++) {
        memset(g_sass[i], 0xc0 + (int)i, sizeof(g_sass[i]));
        src->sass[i] = g_sass[i];
        src->sass_bytes[i] = sizes[i];
    }
    size_t len = (size_t)(mnist_onnx_end - mnist_onnx_start);
    TEST_ASSERT_EQUAL_INT(GA10B_ONNX_OK, ga10b_mnist_weights_from_onnx(mnist_onnx_start, len, src));
    src->input = mnist_input_synth_start;
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

static void test_build_and_handoff(void)
{
    pool_reset();
    struct ga10b_vm vm;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_ops, &vm));
    struct ga10b_mnist_sources src;
    make_sources(&src);

    struct ga10b_mnist_pipeline *p = &g_pipe;
    unsigned wr_before = g_wr32s;
    TEST_ASSERT_EQUAL_INT(GA10B_MNIST_OK, ga10b_mnist_pipeline_build(&g_ops, &vm, &src, p));
    TEST_ASSERT_TRUE(p->built);
    TEST_ASSERT_TRUE(g_wr32s >= wr_before + 2u);   /* TLB invalidate issued */

    /* Sizes. */
    for (unsigned i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(65536, p->shader[i].size);
    }
    for (unsigned w = 0; w < 6; w++) {
        TEST_ASSERT_EQUAL_UINT32(ga10b_mnist_weights[w].buf_bytes, p->weight[w].size);
    }
    static const uint32_t out_sz[8] = { 28672, 28672, 8192, 16384, 16384, 4096, 4096, 4096 };
    for (unsigned i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT32(out_sz[i], p->out[i].size);
        TEST_ASSERT_EQUAL_UINT32(4096, p->cbuf[i].size);
        TEST_ASSERT_EQUAL_UINT32(4096, p->qmd[i].size);
    }
    TEST_ASSERT_EQUAL_UINT32(262144, p->qmd_pool.size);
    TEST_ASSERT_EQUAL_UINT32(4096, p->input.size);

    /* Every GPU-visible buffer is mapped where the VM says (all pages). */
    struct { const struct ga10b_dma *d; uint64_t va; } maps[40];
    unsigned nm = 0;
    for (unsigned i = 0; i < 4; i++) { maps[nm].d = &p->shader[i]; maps[nm++].va = p->shader_va[i]; }
    for (unsigned w = 0; w < 6; w++) { maps[nm].d = &p->weight[w]; maps[nm++].va = p->va.weight[w]; }
    maps[nm].d = &p->input; maps[nm++].va = p->va.input;
    for (unsigned i = 0; i < 8; i++) {
        maps[nm].d = &p->qmd[i]; maps[nm++].va = p->qmd_va[i];
        maps[nm].d = &p->cbuf[i]; maps[nm++].va = p->cbuf_va[i];
        maps[nm].d = &p->out[i]; maps[nm++].va = p->va.output[i];
    }
    maps[nm].d = &p->qmd_pool; maps[nm++].va = p->qmd_pool_va;
    for (unsigned m = 0; m < nm; m++) {
        TEST_ASSERT_TRUE(maps[m].va >= GA10B_VM_USER_BASE && maps[m].va < (1ull << 40));
        TEST_ASSERT_EQUAL_HEX64(0, maps[m].va & 0xfffu);
        for (uint32_t off = 0; off < maps[m].d->size; off += 4096u) {
            TEST_ASSERT_EQUAL_HEX64(maps[m].d->phys + off,
                                    ga10b_vm_lookup(&g_ops, &vm, maps[m].va + off));
        }
        for (unsigned k = 0; k < m; k++) {        /* VA ranges disjoint */
            TEST_ASSERT_TRUE(maps[m].va >= maps[k].va + maps[k].d->size ||
                             maps[k].va >= maps[m].va + maps[m].d->size);
        }
    }
    /* The ops array is CPU-only. */
    TEST_ASSERT_NOT_NULL(p->ops.cpu);

    /* Contents: shader bytes + zero tail, weights, input, outputs zero. */
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t *s = (const uint8_t *)p->shader[i].cpu;
        TEST_ASSERT_EQUAL_MEMORY(g_sass[i], s, src.sass_bytes[i]);
        for (uint32_t o = src.sass_bytes[i]; o < 65536u; o++) {
            TEST_ASSERT_EQUAL_UINT8(0, s[o]);
        }
    }
    for (unsigned w = 0; w < 6; w++) {
        TEST_ASSERT_EQUAL_HEX32(g_weight_ref[w].fnv,
            operator_library_checksum32((const uint8_t *)p->weight[w].cpu,
                                        ga10b_mnist_weights[w].bytes));
    }
    TEST_ASSERT_EQUAL_MEMORY(mnist_input_synth_start, p->input.cpu, GA10B_MNIST_INPUT_BYTES);
    for (unsigned i = 0; i < 8; i++) {
        const uint8_t *o = (const uint8_t *)p->out[i].cpu;
        for (uint32_t k = 0; k < p->out[i].size; k++) {
            TEST_ASSERT_EQUAL_UINT8(0, o[k]);
        }
    }

    /* cbuf pointers resolve to the buffers they name. */
    const uint8_t *c0 = (const uint8_t *)p->cbuf[0].cpu;
    TEST_ASSERT_EQUAL_HEX64(p->input.phys, ga10b_vm_lookup(&g_ops, &vm, rd64le(c0 + 0x160)));
    TEST_ASSERT_EQUAL_HEX64(p->weight[0].phys, ga10b_vm_lookup(&g_ops, &vm, rd64le(c0 + 0x168)));
    TEST_ASSERT_EQUAL_HEX64(p->out[0].phys, ga10b_vm_lookup(&g_ops, &vm, rd64le(c0 + 0x170)));
    const uint8_t *c6 = (const uint8_t *)p->cbuf[6].cpu;
    TEST_ASSERT_EQUAL_HEX64(p->out[5].phys, ga10b_vm_lookup(&g_ops, &vm, rd64le(c6 + 0x160)));
    TEST_ASSERT_EQUAL_HEX64(p->weight[GA10B_MNIST_W_FC].phys,
                            ga10b_vm_lookup(&g_ops, &vm, rd64le(c6 + 0x168)));

    /* v7 ops array (:897-920). */
    const struct ga10b_pipeline_op_v7 *ops = (const struct ga10b_pipeline_op_v7 *)p->ops.cpu;
    static const uint8_t shader_of[8] = { 0, 1, 2, 0, 1, 2, 3, 1 };
    static const uint32_t sentinel[8] = { 0, 2, 0, 0, 57, 4, 0, 0 };
    uint8_t pool_qmd[256];
    for (unsigned i = 0; i < 8; i++) {
        const struct ga10b_pipeline_op_v7 *o = &ops[i];
        TEST_ASSERT_TRUE(ga10b_pipeline_op_is_valid((const struct ga10b_pipeline_op *)o));
        TEST_ASSERT_EQUAL_HEX64(p->qmd_va[i], o->qmd_gpu_va);
        TEST_ASSERT_EQUAL_HEX64(p->out[i].phys + sentinel[i] * 4u, o->output_phys);
        TEST_ASSERT_EQUAL_UINT32(0, o->expected_payload);
        TEST_ASSERT_EQUAL_UINT32(0, o->flags);
        TEST_ASSERT_EQUAL_HEX64(p->shader_va[shader_of[i]], o->shader_gpu_va);
        TEST_ASSERT_EQUAL_HEX64(p->cbuf_va[i], o->cbuf_gpu_va);
        TEST_ASSERT_EQUAL_UINT32(128, o->register_count_v);
        TEST_ASSERT_EQUAL_UINT32(ga10b_mnist_ops[i].grid[0], o->grid_x);
        TEST_ASSERT_EQUAL_UINT32(ga10b_mnist_ops[i].grid[1], o->grid_y);
        TEST_ASSERT_EQUAL_UINT32(1, o->grid_z);
        TEST_ASSERT_EQUAL_UINT32(ga10b_mnist_ops[i].block[0], o->block_x);
        TEST_ASSERT_EQUAL_UINT32(ga10b_mnist_ops[i].block[1], o->block_y);
        TEST_ASSERT_EQUAL_UINT32(1, o->block_z);
        TEST_ASSERT_EQUAL_UINT32(0, o->smem_size_bytes);
        TEST_ASSERT_EQUAL_UINT32(0, o->slm_size_bytes);
        TEST_ASSERT_EQUAL_UINT32(0, o->barrier_count);
        TEST_ASSERT_EQUAL_HEX64(p->shader[shader_of[i]].phys, o->shader_phys);
        TEST_ASSERT_EQUAL_UINT32(4096, o->shader_size_bytes);   /* SASS <= 4 KB here */

        /* The QMD the v7 dispatch authors per launch equals the baked
         * one (SIMT ops leave smem/barriers at the encoder defaults). */
        uint32_t slot = i;
        memset(pool_qmd, 0, sizeof(pool_qmd));
        struct ga10b_qmd_pool_slot s = ga10b_qmd_pool_prepare(pool_qmd, 0x9000000ull, 1u,
                                                              &slot, o);
        TEST_ASSERT_NOT_NULL(s.cpu_va);
        TEST_ASSERT_EQUAL_MEMORY(p->qmd[i].cpu, pool_qmd, 256);
    }

    /* Handoff (gpu_write_handoff_v7). */
    struct ga10b_chan ch;
    make_chan(&vm, &ch);
    struct ga10b_channel_handoff h;
    memset(&h, 0xee, sizeof(h));
    ga10b_mnist_pipeline_fill_handoff(p, &ch, &h);
    TEST_ASSERT_EQUAL_HEX32(GA10B_CHANNEL_HANDOFF_MAGIC, h.magic);
    TEST_ASSERT_EQUAL_UINT32(7, h.version);
    TEST_ASSERT_EQUAL_UINT32(8, h.pipeline_n_ops);
    TEST_ASSERT_EQUAL_HEX64(p->ops.phys, h.pipeline_ops_phys);
    TEST_ASSERT_EQUAL_HEX64(p->input.phys, h.input_buf_phys);
    TEST_ASSERT_EQUAL_UINT32(3136, h.input_buf_size);
    TEST_ASSERT_EQUAL_UINT32(GA10B_PIPELINE_KIND_MNIST, h.pipeline_kind);
    TEST_ASSERT_EQUAL_HEX64(p->qmd_pool.phys, h.qmd_pool_phys);
    TEST_ASSERT_EQUAL_HEX64(p->qmd_pool_va, h.qmd_pool_gpu_va);
    TEST_ASSERT_EQUAL_UINT32(262144, h.qmd_pool_size_bytes);
    TEST_ASSERT_EQUAL_UINT32(1024, h.qmd_pool_n_slots);
    TEST_ASSERT_EQUAL_HEX32(0xc003b6c9u, h.expected_payload);
    /* v3 single-shot fields and the v8+ tails stay zero. */
    TEST_ASSERT_EQUAL_HEX64(0, h.qmd_gpu_va);
    TEST_ASSERT_EQUAL_HEX64(0, h.shader_phys);
    TEST_ASSERT_EQUAL_HEX64(0, h.output_phys);
    TEST_ASSERT_EQUAL_HEX64(0, h.weights_pool_size_bytes);
    TEST_ASSERT_EQUAL_HEX64(0, h.gr_ctx_extents_phys);
    /* Channel fields from ga10b_chan_fill_handoff; semaphore on the
     * channel's own page. */
    TEST_ASSERT_EQUAL_HEX64(ch.userd.phys, h.userd_phys);
    TEST_ASSERT_EQUAL_HEX64(ch.sema.phys, h.semaphore_phys);
    TEST_ASSERT_EQUAL_HEX64(ch.inst.phys, h.inst_block_phys);
    TEST_ASSERT_EQUAL_UINT32(ch.work_submit_token, h.work_submit_token);
    /* What the dispatch path checks: ga10b_validate_handoff's rules,
     * then the v7 gates. */
    TEST_ASSERT_TRUE(h.userd_phys && h.gpfifo_phys && h.pushbuf_phys && h.semaphore_phys);
    TEST_ASSERT_TRUE(h.work_submit_token != 0 && h.inst_block_phys != 0);
    TEST_ASSERT_TRUE(h.gpfifo_entries != 0 && (h.gpfifo_entries & (h.gpfifo_entries - 1u)) == 0);
    TEST_ASSERT_TRUE(h.pushbuf_gpu_va < (1ull << 40) && h.semaphore_gpu_va < (1ull << 40));
    TEST_ASSERT_TRUE(ga10b_handoff_is_v7(&h));
    TEST_ASSERT_EQUAL_INT(GA10B_V7_OK, ga10b_v7_validate_handoff(&h));
    /* Capacity checks of ga10b_dispatch_v7_pipeline_inline. */
    TEST_ASSERT_TRUE(h.pipeline_n_ops + 2u <= h.gpfifo_entries / 2u);
    TEST_ASSERT_TRUE(8u * 56u + 2u * 48u <= h.pushbuf_size);

    unsigned used = pool_pages_used();
    ga10b_mnist_pipeline_free(&g_ops, p);
    TEST_ASSERT_FALSE(p->built);
    /* Only the VM's page-table pages remain. */
    TEST_ASSERT_EQUAL_UINT32(vm.table_pages, pool_pages_used());
    TEST_ASSERT_TRUE(used > vm.table_pages + 180u);
    ga10b_vm_destroy(&g_ops, &vm);
    TEST_ASSERT_EQUAL_UINT32(0, pool_pages_used());
}

static void test_build_errors(void)
{
    pool_reset();
    struct ga10b_vm vm;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_ops, &vm));
    struct ga10b_mnist_sources src;
    make_sources(&src);
    struct ga10b_mnist_pipeline *p = &g_pipe;

    TEST_ASSERT_EQUAL_INT(GA10B_MNIST_ERR_ARG, ga10b_mnist_pipeline_build(NULL, &vm, &src, p));
    src.sass_bytes[2] = 65537;
    TEST_ASSERT_EQUAL_INT(GA10B_MNIST_ERR_ARG, ga10b_mnist_pipeline_build(&g_ops, &vm, &src, p));
    make_sources(&src);
    src.sass[3] = NULL;
    TEST_ASSERT_EQUAL_INT(GA10B_MNIST_ERR_ARG, ga10b_mnist_pipeline_build(&g_ops, &vm, &src, p));
    make_sources(&src);
    src.weight[GA10B_MNIST_B_FC] = NULL;
    TEST_ASSERT_EQUAL_INT(GA10B_MNIST_ERR_ARG, ga10b_mnist_pipeline_build(&g_ops, &vm, &src, p));

    /* Out of memory part-way: the pages allocated so far stay allocated
     * (some are already mapped in the VM, so freeing them would leave
     * PTEs to reused memory), and the pipeline is not marked built. */
    make_sources(&src);
    /* ~190 pages needed, ~150 left: fails in the middle. */
    for (unsigned i = POOL_PAGES - 250u; i < POOL_PAGES; i++) {
        g_used[i] = true;
    }
    int rc = ga10b_mnist_pipeline_build(&g_ops, &vm, &src, p);
    TEST_ASSERT_TRUE(rc == GA10B_MNIST_ERR_NOMEM || rc == GA10B_MNIST_ERR_MAP);
    TEST_ASSERT_FALSE(p->built);
    for (unsigned i = POOL_PAGES - 250u; i < POOL_PAGES; i++) {
        g_used[i] = false;
    }
    TEST_ASSERT_TRUE(pool_pages_used() > vm.table_pages);

    /* Fresh pool and VM for the remaining cases. */
    pool_reset();
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_ops, &vm));

    /* Input NULL: buffer starts zeroed. */
    make_sources(&src);
    src.input = NULL;
    TEST_ASSERT_EQUAL_INT(GA10B_MNIST_OK, ga10b_mnist_pipeline_build(&g_ops, &vm, &src, p));
    const uint8_t *in = (const uint8_t *)p->input.cpu;
    for (unsigned k = 0; k < 4096u; k++) {
        TEST_ASSERT_EQUAL_UINT8(0, in[k]);
    }
    ga10b_mnist_pipeline_free(&g_ops, p);
    ga10b_vm_destroy(&g_ops, &vm);

    /* An unbuilt pipeline adds nothing beyond the channel handoff. */
    struct ga10b_vm vm2;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_ops, &vm2));
    struct ga10b_chan ch;
    make_chan(&vm2, &ch);
    struct ga10b_channel_handoff h;
    ga10b_mnist_pipeline_fill_handoff(p, &ch, &h);
    TEST_ASSERT_EQUAL_UINT32(2, h.version);
    TEST_ASSERT_EQUAL_UINT32(0, h.pipeline_n_ops);
    TEST_ASSERT_FALSE(ga10b_handoff_is_v7(&h));
    ga10b_vm_destroy(&g_ops, &vm2);
}

int test_suite_ga10b_mnist_pipeline(void)
{
    UnityBegin("test_ga10b_mnist_pipeline.c");
    RUN_TEST(test_round_page);
    RUN_TEST(test_op_table);
    RUN_TEST(test_cbuf_layouts);
    RUN_TEST(test_cbuf_errors);
    RUN_TEST(test_onnx_synthetic);
    RUN_TEST(test_onnx_embedded_weights);
    RUN_TEST(test_embedded_input_fixture);
    RUN_TEST(test_build_and_handoff);
    RUN_TEST(test_build_errors);
    return UnityEnd();
}
