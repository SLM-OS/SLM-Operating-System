/*
 * test_slm_prog.c - GPU program for an SLM forward pass (#475, ADR 0003).
 *
 * No GPU here. Layers tested:
 *
 *   ga10b_prog      pushbuffer, QMD and cbuf contents, compared with the
 *                   method encodings the v7 path uses on hardware
 *   slm_prog_encode cbuf offsets and launch shapes, per kernel
 *   slm_prog_*      arena, weights mapping and teardown against fake ops;
 *                   the run loop against a fake channel, including a hang
 *   end to end      the tiny Qwen2 model through the production FFI with
 *                   the fake channel: a program built from a real GGUF,
 *                   and a hung GPU falling back to the CPU with the same
 *                   tokens llama.cpp picks.
 *
 * The plan's dataflow is checked against llama.cpp's logits by the host
 * tests in runtime/src/slm/gpu_program.rs (software GPU).
 */
#include "unity.h"
#include "../gpu/nvidia/ga10b_chan.h"
#include "../gpu/nvidia/ga10b_prog.h"
#include "../gpu/nvidia/ga10b_qmd.h"
#include "../include/oplib_gpu_as.h"
#include "../include/slm_ffi.h"
#include "../include/slm_prog.h"
#include "pmm.h"
#include "slm_kernels_gen.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern const uint8_t slm_golden_tiny_start[], slm_golden_tiny_end[];
extern const uint8_t slm_golden_greedy_start[], slm_golden_greedy_end[];
extern int32_t rust_slm_golden_open_greedy(uint32_t model, uint32_t max_ctx);
extern int32_t rust_slm_golden_record(const uint8_t *rec, size_t rec_len,
                                      const char *name, uint8_t *out, size_t cap);

/* ---- Encoders ---- */

#define PB_DW 512u

static uint32_t g_pb[PB_DW] __attribute__((aligned(256)));
static uint32_t g_qmd[8u * GA10B_QMD_DWORDS] __attribute__((aligned(256)));
static uint8_t g_cb[8u * GA10B_PROG_CBUF_STRIDE] __attribute__((aligned(256)));
static uint8_t g_sm[16u * GA10B_PROG_SEMA_SLOT_BYTES] __attribute__((aligned(16)));

#define PB_VA 0x1000000000ull
#define Q_VA 0x1000010000ull
#define C_VA 0x1000020000ull
#define S_VA 0x1000030000ull

static int prog_init(struct ga10b_prog *p, uint32_t flags, enum ga10b_prog_barrier b)
{
    struct ga10b_prog_mem pb = { (uint8_t *)g_pb, PB_VA, sizeof(g_pb) };
    struct ga10b_prog_mem q = { (uint8_t *)g_qmd, Q_VA, sizeof(g_qmd) };
    struct ga10b_prog_mem c = { g_cb, C_VA, sizeof(g_cb) };
    struct ga10b_prog_mem s = { g_sm, S_VA, sizeof(g_sm) };
    return ga10b_prog_init(p, &pb, &q, &c, &s, 8u, flags, b);
}

static struct ga10b_prog_launch launch(const void *args, uint32_t len)
{
    struct ga10b_prog_launch l;
    memset(&l, 0, sizeof(l));
    l.shader_gpu_va = 0x77000100ull;
    l.register_count_v = 64;
    l.grid_x = 3;
    l.grid_y = 1;
    l.grid_z = 1;
    l.block_x = 32;
    l.block_y = 1;
    l.block_z = 1;
    l.smem_size_bytes = 512;
    l.barrier_count = 1;
    l.args = args;
    l.args_len = len;
    return l;
}

/* Count dwords equal to `v`. */
static unsigned count_dw(uint32_t v, uint32_t n)
{
    unsigned c = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (g_pb[i] == v) c++;
    }
    return c;
}

#define HDR_WFI 0x80002044u     /* immd, subch 1, 0x110 >> 2 */
#define HDR_FLUSH 0x80142087u   /* immd data 0x14, subch 1, 0x21c >> 2 */
#define HDR_PCAS2 0x800a20b0u   /* immd data 0xA, subch 1, 0x2c0 >> 2 */
#define HDR_PCAS_A 0x200120adu  /* inc count 1, subch 1, 0x2b4 >> 2 */

static void test_prog_pushbuffer(void)
{
    static struct ga10b_prog p;
    uint8_t args[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, 0, GA10B_PROG_BARRIER_WFI_FLUSH));
    /* SET_OBJECT AMPERE_COMPUTE_B on subch 1, as in the v7 builders. */
    TEST_ASSERT_EQUAL_HEX32(0x20012000u, g_pb[0]);
    TEST_ASSERT_EQUAL_HEX32(0xC7C0u, g_pb[1]);
    uint32_t pro = p.pb_dwords;

    struct ga10b_prog_launch l = launch(args, sizeof(args));
    TEST_ASSERT_EQUAL_INT(0, ga10b_prog_add_launch(&p, &l));
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, ga10b_prog_add_barrier(&p));
    TEST_ASSERT_EQUAL_INT(1, ga10b_prog_add_launch(&p, &l));
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, ga10b_prog_finish(&p));

    /* Launch: SEND_PCAS_A with QMD VA >> 8, then PCAS2_B. */
    TEST_ASSERT_EQUAL_HEX32(HDR_PCAS_A, g_pb[pro]);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(Q_VA >> 8), g_pb[pro + 1]);
    TEST_ASSERT_EQUAL_HEX32(HDR_PCAS2, g_pb[pro + 2]);
    /* Barrier between the launches: WFI then flush, in that order. */
    TEST_ASSERT_EQUAL_HEX32(HDR_WFI, g_pb[pro + 3]);
    TEST_ASSERT_EQUAL_HEX32(HDR_FLUSH, g_pb[pro + 4]);
    TEST_ASSERT_EQUAL_HEX32(HDR_PCAS_A, g_pb[pro + 5]);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)((Q_VA + GA10B_QMD_SIZE_BYTES) >> 8), g_pb[pro + 6]);
    TEST_ASSERT_EQUAL_UINT32(1u, p.n_barriers);
    /* End semaphore last: incrementing run of 5 from 0x158, four words. A
     * release alone doesn't wait for launched work, so a WFI and flush come
     * first, whatever the barrier kind (hardware: half-computed logits). */
    uint32_t e = p.pb_dwords - 6u;
    TEST_ASSERT_EQUAL_HEX32(HDR_WFI, g_pb[e - 2]);
    TEST_ASSERT_EQUAL_HEX32(HDR_FLUSH, g_pb[e - 1]);
    TEST_ASSERT_EQUAL_HEX32(0x20052056u, g_pb[e]);
    TEST_ASSERT_EQUAL_HEX32(GA10B_PROG_PAYLOAD_END, g_pb[e + 1]);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(S_VA + GA10B_PROG_SEMA_SLOT_BYTES), g_pb[e + 3]);
    TEST_ASSERT_EQUAL_HEX32(0x0u, g_pb[e + 5]);

    /* The GPFIFO entry covers exactly the pushbuffer. */
    uint64_t gp = ga10b_prog_gp_entry(&p);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)PB_VA, (uint32_t)gp);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(PB_VA >> 32) | (p.pb_dwords << 10), (uint32_t)(gp >> 32));

    /* Done means done. */
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_STATE, ga10b_prog_add_launch(&p, &l));
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_STATE, ga10b_prog_add_barrier(&p));
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_STATE, ga10b_prog_finish(&p));
}

static void test_prog_barrier_kinds(void)
{
    static struct ga10b_prog p;
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, 0, GA10B_PROG_BARRIER_WFI));
    uint32_t before = p.pb_dwords;
    (void)ga10b_prog_add_barrier(&p);
    TEST_ASSERT_EQUAL_UINT32(before + 1u, p.pb_dwords);
    TEST_ASSERT_EQUAL_HEX32(HDR_WFI, g_pb[before]);

    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, 0, GA10B_PROG_BARRIER_FLUSH));
    before = p.pb_dwords;
    (void)ga10b_prog_add_barrier(&p);
    TEST_ASSERT_EQUAL_HEX32(HDR_FLUSH, g_pb[before]);
    TEST_ASSERT_EQUAL_UINT32(before + 1u, p.pb_dwords);
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_ARG, prog_init(&p, 0, GA10B_PROG_BARRIER_COUNT));
}

static void test_prog_qmd_and_cbuf(void)
{
    static struct ga10b_prog p;
    uint32_t args[3] = { 0x11111111u, 0x22222222u, 0x33333333u };
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, 0, GA10B_PROG_BARRIER_WFI_FLUSH));
    struct ga10b_prog_launch l = launch(args, sizeof(args));
    (void)ga10b_prog_add_launch(&p, &l);
    TEST_ASSERT_EQUAL_INT(1, ga10b_prog_add_launch(&p, &l));

    /* Second launch: its own cbuf, its args at 0x160, zero below. */
    uint8_t *a = ga10b_prog_cbuf_args(&p, 1);
    TEST_ASSERT_EQUAL_PTR(g_cb + GA10B_PROG_CBUF_STRIDE + 0x160u, a);
    TEST_ASSERT_EQUAL_HEX32(0x22222222u, ((uint32_t *)(void *)a)[1]);
    TEST_ASSERT_EQUAL_HEX8(0, g_cb[GA10B_PROG_CBUF_STRIDE]);

    /* A QMD built here equals what the v7 encoder writes for the same
     * launch, apart from the cbuf address. */
    uint32_t *q = ga10b_prog_qmd(&p, 1);
    uint32_t ref[GA10B_QMD_DWORDS];
    ga10b_qmd_populate(ref, l.shader_gpu_va, C_VA + GA10B_PROG_CBUF_STRIDE, 64, 3, 1, 1, 32, 1, 1);
    ga10b_qmd_set_bits(ref, GA10B_QMD_SHARED_MEMORY_SIZE_HI, GA10B_QMD_SHARED_MEMORY_SIZE_LO, 512);
    ga10b_qmd_set_bits(ref, GA10B_QMD_BARRIER_COUNT_HI, GA10B_QMD_BARRIER_COUNT_LO, 1);
    TEST_ASSERT_EQUAL_MEMORY(ref, q, GA10B_QMD_SIZE_BYTES);

    /* Update rewrites QMD and cbuf in place; the pushbuffer stays. */
    uint32_t dw = p.pb_dwords;
    l.grid_x = 9;
    uint32_t args2[1] = { 0x44444444u };
    l.args = args2;
    l.args_len = sizeof(args2);
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, ga10b_prog_update_launch(&p, 1, &l));
    TEST_ASSERT_EQUAL_UINT32(dw, p.pb_dwords);
    ga10b_qmd_set_bits(ref, GA10B_QMD_CTA_RASTER_WIDTH_HI, GA10B_QMD_CTA_RASTER_WIDTH_LO, 9);
    TEST_ASSERT_EQUAL_MEMORY(ref, q, GA10B_QMD_SIZE_BYTES);
    TEST_ASSERT_EQUAL_HEX32(0x44444444u, *(uint32_t *)(void *)a);
    TEST_ASSERT_EQUAL_HEX32(0, ((uint32_t *)(void *)a)[1]);
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_ARG, ga10b_prog_update_launch(&p, 7, &l));

    /* Lean: no icache, texture or membar work, cbuf invalidate kept. */
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, GA10B_PROG_F_LEAN_QMD,
                                                   GA10B_PROG_BARRIER_WFI_FLUSH));
    l.args = args;
    l.args_len = sizeof(args);
    (void)ga10b_prog_add_launch(&p, &l);
    q = ga10b_prog_qmd(&p, 0);
#define BIT(q_, b) (((q_)[(b) / 32u] >> ((b) % 32u)) & 1u)
    TEST_ASSERT_EQUAL_UINT32(0, BIT(q, GA10B_QMD_INVALIDATE_INSTRUCTION_CACHE_BIT));
    TEST_ASSERT_EQUAL_UINT32(0, BIT(q, GA10B_QMD_CWD_MEMBAR_TYPE_LO_BIT));
    TEST_ASSERT_EQUAL_UINT32(1, BIT(q, GA10B_QMD_INVALIDATE_SHADER_DATA_CACHE_BIT));
    TEST_ASSERT_EQUAL_UINT32(1, BIT(q, GA10B_QMD_CBUF_INVALIDATE_BASE));
}

/* Limits: the sizes for a real program, and refusals that protect memory. */
static void test_prog_limits(void)
{
    uint32_t pb, q, c, s;
    ga10b_prog_sizes(1000u, 0, &pb, &q, &c, &s);
    /* Well under what one GPFIFO entry can address (21-bit dword count). */
    TEST_ASSERT_TRUE(pb < (1u << 23));
    TEST_ASSERT_EQUAL_UINT32(1000u * 256u, q);
    TEST_ASSERT_EQUAL_UINT32(1000u * 512u, c);
    TEST_ASSERT_EQUAL_UINT32(32u, s);
    ga10b_prog_sizes(1000u, GA10B_PROG_F_PROFILE, &pb, NULL, NULL, &s);
    TEST_ASSERT_EQUAL_UINT32((2u + 1000u) * 16u, s);

    static struct ga10b_prog p;
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, 0, GA10B_PROG_BARRIER_WFI));
    struct ga10b_prog_launch l = launch(g_sm, 4);
    for (unsigned i = 0; i < 8u; i++) {
        TEST_ASSERT_EQUAL_INT((int)i, ga10b_prog_add_launch(&p, &l));
    }
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_FULL, ga10b_prog_add_launch(&p, &l));
    (void)ga10b_prog_finish(&p);
    /* Everything fits, with a barrier allowed after every launch. */
    TEST_ASSERT_TRUE(p.pb_dwords <= PB_DW);
    TEST_ASSERT_EQUAL_UINT32(8u, count_dw(HDR_PCAS2, p.pb_dwords));

    /* Regions too small or misaligned for the hardware are refused. */
    struct ga10b_prog_mem pbm = { (uint8_t *)g_pb, PB_VA, sizeof(g_pb) };
    struct ga10b_prog_mem qm = { (uint8_t *)g_qmd, Q_VA + 4u, sizeof(g_qmd) };
    struct ga10b_prog_mem cm = { g_cb, C_VA, sizeof(g_cb) };
    struct ga10b_prog_mem sm = { g_sm, S_VA, sizeof(g_sm) };
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_ARG,
                          ga10b_prog_init(&p, &pbm, &qm, &cm, &sm, 8u, 0, GA10B_PROG_BARRIER_WFI));
    qm.gpu_va = Q_VA;
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_ARG,
                          ga10b_prog_init(&p, &pbm, &qm, &cm, &sm, 9u, 0, GA10B_PROG_BARRIER_WFI));
    /* A launch's args must fit its cbuf; a zero grid never launches. */
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, 0, GA10B_PROG_BARRIER_WFI));
    l.args_len = GA10B_PROG_CBUF_ARGS_MAX + 1u;
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_ARG, ga10b_prog_add_launch(&p, &l));
    l.args_len = 4;
    l.grid_x = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_ARG, ga10b_prog_add_launch(&p, &l));
}

/* `slm gpu inject hang`: an ACQUIRE right before the end semaphore, in the
 * space ga10b_prog_sizes reserves. */
static void test_prog_inject_hang(void)
{
    static struct ga10b_prog p;
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, GA10B_PROG_F_INJECT_HANG, GA10B_PROG_BARRIER_WFI));
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, ga10b_prog_finish(&p));
    uint32_t e = p.pb_dwords - 12u;
    TEST_ASSERT_EQUAL_HEX32(0x20052056u, g_pb[e]);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)S_VA, g_pb[e + 3]);
    TEST_ASSERT_EQUAL_HEX32(1u, g_pb[e + 5]);   /* OPERATION = ACQUIRE */
    TEST_ASSERT_TRUE(g_pb[e + 1] != GA10B_PROG_PAYLOAD_START);
}

static void test_prog_profile_slots(void)
{
    static struct ga10b_prog p;
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, GA10B_PROG_F_PROFILE,
                                                   GA10B_PROG_BARRIER_WFI_FLUSH));
    struct ga10b_prog_launch l = launch(g_sm, 4);
    (void)ga10b_prog_add_launch(&p, &l);
    /* A drained timestamp right after the launch, in its own slot. */
    uint32_t at = p.pb_dwords - 6u;
    TEST_ASSERT_EQUAL_HEX32(HDR_WFI, g_pb[at - 2]);
    TEST_ASSERT_EQUAL_HEX32(GA10B_PROG_PAYLOAD_OP, g_pb[at + 1]);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(S_VA + 2u * GA10B_PROG_SEMA_SLOT_BYTES), g_pb[at + 3]);
    TEST_ASSERT_EQUAL_HEX32(0x0u, g_pb[at + 5]);
    TEST_ASSERT_EQUAL_UINT32(10u * GA10B_PROG_SEMA_SLOT_BYTES, ga10b_prog_sema_bytes(&p));
}

/* The plain pushbuffer is unchanged by the marked copy, which differs only by
 * its timestamps, on a separate GPFIFO entry. This is what makes a step with
 * stats disabled cost nothing. */
static void test_prog_marks(void)
{
    static struct ga10b_prog p, q;
    static uint32_t plain[PB_DW];
    uint8_t args[4] = { 1, 2, 3, 4 };
    struct ga10b_prog_launch l = launch(args, sizeof(args));

    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&q, 0, GA10B_PROG_BARRIER_WFI));
    (void)ga10b_prog_add_launch(&q, &l);
    (void)ga10b_prog_add_barrier(&q);
    (void)ga10b_prog_add_launch(&q, &l);
    (void)ga10b_prog_finish(&q);
    memcpy(plain, g_pb, q.pb_dwords * 4u);
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_ARG, ga10b_prog_add_mark(&q));
    TEST_ASSERT_EQUAL_UINT64(0, ga10b_prog_gp_entry_marked(&q));

    memset(g_pb, 0, sizeof(g_pb));
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, GA10B_PROG_F_MARKS, GA10B_PROG_BARRIER_WFI));
    (void)ga10b_prog_add_launch(&p, &l);
    (void)ga10b_prog_add_barrier(&p);
    TEST_ASSERT_EQUAL_INT(0, ga10b_prog_add_mark(&p));
    (void)ga10b_prog_add_launch(&p, &l);
    (void)ga10b_prog_finish(&p);
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_STATE, ga10b_prog_add_mark(&p));

    TEST_ASSERT_EQUAL_UINT32(q.pb_dwords, p.pb_dwords);
    TEST_ASSERT_EQUAL_MEMORY(plain, g_pb, q.pb_dwords * 4u);
    TEST_ASSERT_EQUAL_UINT64(ga10b_prog_gp_entry(&q), ga10b_prog_gp_entry(&p));
    TEST_ASSERT_EQUAL_UINT32(0u, count_dw(GA10B_PROG_PAYLOAD_MARK, p.pb_dwords));

    /* The copy: plain plus one 6-dword release, right after the barrier's WFI. */
    TEST_ASSERT_EQUAL_UINT32(p.pb_dwords + 6u, p.marks_dwords);
    TEST_ASSERT_TRUE(p.marks_base >= p.pb_dwords);
    const uint32_t *m = g_pb + p.marks_base;
    uint32_t at = 0;
    while (at < p.marks_dwords && m[at] != GA10B_PROG_PAYLOAD_MARK) at++;
    TEST_ASSERT_TRUE(at < p.marks_dwords);
    TEST_ASSERT_EQUAL_HEX32(HDR_WFI, m[at - 2u]);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(S_VA + 2u * GA10B_PROG_SEMA_SLOT_BYTES), m[at + 2u]);
    TEST_ASSERT_EQUAL_HEX32(1u << 5, m[at + 4u]);   /* FLUSH_DISABLE: no L2 flush */
    uint64_t e = ga10b_prog_gp_entry_marked(&p);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(PB_VA + p.marks_base * 4u), (uint32_t)e);
    TEST_ASSERT_EQUAL_UINT32(p.marks_dwords, (uint32_t)(e >> 42));

    /* Plain steps arm the two slots they use, not the marks. */
    TEST_ASSERT_EQUAL_UINT32(2u * GA10B_PROG_SEMA_SLOT_BYTES, ga10b_prog_sema_bytes(&p));
    TEST_ASSERT_EQUAL_UINT32(GA10B_PROG_SEMA_SLOT_BYTES, ga10b_prog_marks_bytes(&p));

    /* Times: up to the mark, then from it to the end. */
    uint64_t ns[2] = { 99, 99 };
    uint32_t *sem = (uint32_t *)(void *)g_sm;
    uint64_t *ts = (uint64_t *)(void *)g_sm;
    TEST_ASSERT_EQUAL_UINT32(0u, ga10b_prog_marks_ns(&p, ns, 1));
    sem[0] = GA10B_PROG_PAYLOAD_START; ts[1] = 1000u;
    sem[4] = GA10B_PROG_PAYLOAD_END;   ts[3] = 1300u;
    sem[8] = GA10B_PROG_PAYLOAD_MARK;  ts[5] = 1100u;
    TEST_ASSERT_EQUAL_UINT32(2u, ga10b_prog_marks_ns(&p, ns, 2));
    TEST_ASSERT_EQUAL_UINT64(100u, ns[0]);
    TEST_ASSERT_EQUAL_UINT64(200u, ns[1]);
    /* A mark that didn't fire, or a clock running backwards: 0, no wild value. */
    sem[8] = 0u;
    (void)ga10b_prog_marks_ns(&p, ns, 2);
    TEST_ASSERT_EQUAL_UINT64(0u, ns[0]);
    TEST_ASSERT_EQUAL_UINT64(0u, ns[1]);
    sem[8] = GA10B_PROG_PAYLOAD_MARK; ts[5] = 500u;
    (void)ga10b_prog_marks_ns(&p, ns, 2);
    TEST_ASSERT_EQUAL_UINT64(0u, ns[0]);
    TEST_ASSERT_EQUAL_UINT64(800u, ns[1]);

    /* A barrier with no idle in it gets one for the timestamp's sake. */
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, GA10B_PROG_F_MARKS, GA10B_PROG_BARRIER_FLUSH));
    (void)ga10b_prog_add_launch(&p, &l);
    (void)ga10b_prog_add_barrier(&p);
    uint32_t before = p.marks_dwords;
    TEST_ASSERT_EQUAL_INT(0, ga10b_prog_add_mark(&p));
    TEST_ASSERT_EQUAL_UINT32(before + 7u, p.marks_dwords);
    TEST_ASSERT_EQUAL_HEX32(HDR_WFI, g_pb[p.marks_base + before]);
}

/* ---- slm_prog_encode: per-kernel ABI ---- */

static uint32_t arg32(const uint8_t *a, uint32_t off)
{
    uint32_t v;
    memcpy(&v, a + off, 4);
    return v;
}

static uint64_t arg64(const uint8_t *a, uint32_t off)
{
    uint64_t v;
    memcpy(&v, a + off, 8);
    return v;
}

static void test_encode_kernels(void)
{
    struct ga10b_prog_launch l;
    uint8_t a[GA10B_PROG_CBUF_ARGS_MAX];

    /* slm_mm_*: offsets checked against the manifest at build time, too. */
    struct slm_prog_op op = {
        .kernel = SLM_PROG_K_MM_Q4K, .rows = 8, .dim = { 1536, 8960 },
        .ptr = { 0x1000, 0x2000, 0x3000, 0x4000 }, .scal = { SLM_PROG_MM_OUT_F16 },
    };
    /* Choice of kernel, as the harness's add_mm. */
    TEST_ASSERT_EQUAL_UINT32(SLM_SASS_MM_Q4K_HMMA_B8, (uint32_t)slm_prog_op_sass(&op));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&op, SLM_SASS_MM_Q4K_HMMA_B8, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_HEX64(0xA000, l.shader_gpu_va);
    TEST_ASSERT_EQUAL_HEX64(0x1000, arg64(a, 0x00));
    TEST_ASSERT_EQUAL_HEX64(0x2000, arg64(a, 0x08));
    TEST_ASSERT_EQUAL_HEX64(0x3000, arg64(a, 0x10));
    TEST_ASSERT_EQUAL_HEX64(0x4000, arg64(a, 0x18));
    TEST_ASSERT_EQUAL_UINT32(1536, arg32(a, 0x20));
    TEST_ASSERT_EQUAL_UINT32(8960, arg32(a, 0x24));
    TEST_ASSERT_EQUAL_UINT32(8, arg32(a, 0x28));
    TEST_ASSERT_EQUAL_UINT32(1, arg32(a, 0x2C));
    TEST_ASSERT_EQUAL_UINT32(598, l.grid_x);    /* ceil(8960 / 15) */
    TEST_ASSERT_EQUAL_UINT32(1, l.grid_y);
    TEST_ASSERT_EQUAL_UINT32(32, l.block_x);
    /* Registers and shared memory come from the manifest. */
    TEST_ASSERT_TRUE(l.register_count_v > 0u);
    TEST_ASSERT_EQUAL_UINT32(0, l.smem_size_bytes % 256u);

    /* B = 1: warps per CTA go in flags and the block. */
    op.rows = 1;
    op.dim[1] = 8960;
    TEST_ASSERT_EQUAL_UINT32(SLM_SASS_MM_Q4K_HMMA_B1, (uint32_t)slm_prog_op_sass(&op));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&op, SLM_SASS_MM_Q4K_HMMA_B1, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_UINT32(598, l.grid_x);
    TEST_ASSERT_EQUAL_UINT32(1, l.block_y);     /* 598 tiles fill the SMs */
    TEST_ASSERT_EQUAL_UINT32(1u | (1u << 8), arg32(a, 0x2C));
    op.dim[1] = 256;                            /* attn_k: 18 tiles, K = 1536 */
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&op, SLM_SASS_MM_Q4K_HMMA_B1, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_UINT32(18, l.grid_x);
    TEST_ASSERT_EQUAL_UINT32(6, l.block_y);     /* min(8, K/256, ceil(200/18)) */
    TEST_ASSERT_EQUAL_UINT32(1u | (6u << 8), arg32(a, 0x2C));
    TEST_ASSERT_TRUE(l.barrier_count >= 1u);    /* __syncthreads between warps */
    TEST_ASSERT_TRUE(l.smem_size_bytes >= 512u);

    /* Prefill, big N: x in shared memory. */
    op.rows = 64;
    op.dim[1] = 8960;
    TEST_ASSERT_EQUAL_UINT32(SLM_SASS_MM_Q4K_HMMA_BS, (uint32_t)slm_prog_op_sass(&op));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&op, SLM_SASS_MM_Q4K_HMMA_BS, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_UINT32(75, l.grid_x);     /* ceil(8960 / 120) */
    TEST_ASSERT_EQUAL_UINT32(8, l.block_y);
    TEST_ASSERT_TRUE(l.register_count_v <= 256u);
    TEST_ASSERT_TRUE(l.smem_size_bytes >= 16384u);
    TEST_ASSERT_EQUAL_UINT32(1, arg32(a, 0x2C));

    /* Prefill, small N: b8, one CTA per (tile, x tile of 8 rows). */
    op.dim[1] = 256;
    op.rows = 9;
    TEST_ASSERT_EQUAL_UINT32(SLM_SASS_MM_Q4K_HMMA_B8, (uint32_t)slm_prog_op_sass(&op));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&op, SLM_SASS_MM_Q4K_HMMA_B8, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_UINT32(18, l.grid_x);
    TEST_ASSERT_EQUAL_UINT32(2, l.grid_y);
    TEST_ASSERT_EQUAL_UINT32(1u | SLM_PROG_MM_SPLIT_XT, arg32(a, 0x2C));
    op.rows = 63;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&op, SLM_SASS_MM_Q4K_HMMA_B8, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_UINT32(8, l.grid_y);
    op.rows = 8;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&op, SLM_SASS_MM_Q4K_HMMA_B8, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_UINT32(1, l.grid_y);
    TEST_ASSERT_EQUAL_UINT32(1u, arg32(a, 0x2C));
    op.rows = 65;
    TEST_ASSERT_EQUAL_INT(-1, slm_prog_encode(&op, SLM_SASS_MM_Q4K_HMMA_B8, 0xA000, &l, a));
    op.dim[1] = 8960;
    op.rows = 8;
    /* The kernels load 16 bytes of x at a time. */
    op.ptr[0] = 0x1008;
    TEST_ASSERT_EQUAL_INT(-1, slm_prog_encode(&op, SLM_SASS_MM_Q4K_HMMA_B8, 0xA000, &l, a));
    op.ptr[0] = 0x1000;

    /* SIMT: one block per (row, batch). Any batch. */
    op.rows = 65;
    op.scal[1] = SLM_PROG_MM_SIMT;
    op.kernel = SLM_PROG_K_MM_Q6K;
    TEST_ASSERT_EQUAL_UINT32(SLM_SASS_MM_Q6K_SIMT, (uint32_t)slm_prog_op_sass(&op));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&op, SLM_SASS_MM_Q6K_SIMT, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_UINT32(8960, l.grid_x);
    TEST_ASSERT_EQUAL_UINT32(65, l.grid_y);
    /* A null weight pointer never launches; a null bias is how none is said. */
    op.ptr[3] = 0;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&op, SLM_SASS_MM_Q6K_SIMT, 0xA000, &l, a));
    op.ptr[1] = 0;
    TEST_ASSERT_EQUAL_INT(-1, slm_prog_encode(&op, SLM_SASS_MM_Q6K_SIMT, 0xA000, &l, a));

    struct slm_prog_op e = {
        .kernel = SLM_PROG_K_EMBED_Q6K, .rows = 4, .dim = { 1536 }, .ptr = { 0x10, 0x20, 0x30 },
    };
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&e, SLM_SASS_EMBED_Q6K, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_HEX64(0x30, arg64(a, 0x10));
    TEST_ASSERT_EQUAL_UINT32(1536, arg32(a, 0x18));
    TEST_ASSERT_EQUAL_UINT32(4, arg32(a, 0x1C));
    TEST_ASSERT_EQUAL_UINT32(4, l.grid_x);

    struct slm_prog_op n = {
        .kernel = SLM_PROG_K_RMSNORM, .rows = 4, .dim = { 1536 },
        .ptr = { 0x10, 0x20, 0x30 }, .scal = { 0x358637BDu },
    };
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&n, SLM_SASS_RMSNORM_W8, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_HEX64(0x10, arg64(a, 0x00));
    TEST_ASSERT_EQUAL_HEX64(0x20, arg64(a, 0x08));
    TEST_ASSERT_EQUAL_UINT32(1536, arg32(a, 0x18));
    TEST_ASSERT_EQUAL_UINT32(4, arg32(a, 0x1C));
    TEST_ASSERT_EQUAL_HEX32(0x358637BDu, arg32(a, 0x20));
    TEST_ASSERT_EQUAL_UINT32(4, l.grid_x);
    TEST_ASSERT_EQUAL_UINT32(8, l.block_y);
    TEST_ASSERT_TRUE(l.barrier_count >= 1u);
    /* The kernel covers whole warps only. */
    n.dim[0] = 1537;
    TEST_ASSERT_EQUAL_INT(-1, slm_prog_encode(&n, SLM_SASS_RMSNORM_W8, 0xA000, &l, a));

    struct slm_prog_op r = {
        .kernel = SLM_PROG_K_ROPE, .rows = 2, .dim = { 12, 128 },
        .ptr = { 0x10, 0x10, 0x30, 0x40 },
    };
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&r, SLM_SASS_ROPE, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_HEX64(0x40, arg64(a, 0x18));
    TEST_ASSERT_EQUAL_UINT32(12, arg32(a, 0x20));
    TEST_ASSERT_EQUAL_UINT32(128, arg32(a, 0x24));
    TEST_ASSERT_EQUAL_UINT32(2, arg32(a, 0x28));
    TEST_ASSERT_EQUAL_UINT32(12, l.grid_x);
    TEST_ASSERT_EQUAL_UINT32(2, l.grid_y);
    r.dim[1] = 100;  /* heads must be whole warps */
    TEST_ASSERT_EQUAL_INT(-1, slm_prog_encode(&r, SLM_SASS_ROPE, 0xA000, &l, a));

    struct slm_prog_op g = {
        .kernel = SLM_PROG_K_GQA, .rows = 5, .dim = { 12, 2, 128 },
        .ptr = { 0x10, 0x20, 0x30, 0x40, 0x50 },
    };
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&g, SLM_SASS_GQA_W8, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_HEX64(0x40, arg64(a, 0x18));
    TEST_ASSERT_EQUAL_HEX64(0x50, arg64(a, 0x20));
    TEST_ASSERT_EQUAL_UINT32(12, arg32(a, 0x28));
    TEST_ASSERT_EQUAL_UINT32(2, arg32(a, 0x2C));
    TEST_ASSERT_EQUAL_UINT32(128, arg32(a, 0x30));
    TEST_ASSERT_EQUAL_UINT32(5, arg32(a, 0x34));
    TEST_ASSERT_EQUAL_UINT32(12, l.grid_x);
    TEST_ASSERT_EQUAL_UINT32(5, l.grid_y);
    TEST_ASSERT_TRUE(l.smem_size_bytes >= 8256u);
    TEST_ASSERT_EQUAL_UINT32(8, l.block_y);
    g.dim[2] = 512;
    TEST_ASSERT_EQUAL_INT(-1, slm_prog_encode(&g, SLM_SASS_GQA_W8, 0xA000, &l, a));

    /* Total elements: rows * width, rounded up to whole warps. */
    struct slm_prog_op s = {
        .kernel = SLM_PROG_K_SWIGLU, .rows = 2, .dim = { 8960 }, .ptr = { 0x10, 0x20, 0x30 },
    };
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&s, SLM_SASS_SWIGLU, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_UINT32(17920, arg32(a, 0x18));
    TEST_ASSERT_EQUAL_UINT32(560, l.grid_x);

    struct slm_prog_op sum = {
        .kernel = SLM_PROG_K_ADD, .rows = 1, .dim = { 1537 }, .ptr = { 0x10, 0x20, 0x30 },
    };
    TEST_ASSERT_EQUAL_INT(0, slm_prog_encode(&sum, SLM_SASS_ADD, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_UINT32(1537, arg32(a, 0x18));
    TEST_ASSERT_EQUAL_UINT32(49, l.grid_x);

    /* A kernel VA of 0 means the lookup failed, and unknown kinds fail. */
    TEST_ASSERT_EQUAL_INT(-1, slm_prog_encode(&sum, SLM_SASS_ADD, 0, &l, a));
    sum.kernel = SLM_PROG_K_COUNT;
    TEST_ASSERT_EQUAL_INT(-1, slm_prog_encode(&sum, SLM_SASS_ADD, 0xA000, &l, a));
    TEST_ASSERT_EQUAL_INT(-1, slm_prog_op_sass(&sum));
}

/* ---- Sessions on a fake channel ---- */

/* Run once from inside the next single-page allocation: a page-table page,
 * when ga10b_vm_map has found an entry empty and not yet written it. Models
 * another task getting the CPU at that point (#1173). */
static void (*g_alloc_hook)(void);

static void *pm_alloc(void *ctx, uint32_t n, uint64_t *phys)
{
    (void)ctx;
    if (n == 1u && g_alloc_hook) {
        void (*hook)(void) = g_alloc_hook;
        g_alloc_hook = NULL;
        hook();
    }
    void *p = pmm_alloc_pages(n);
    if (p) {
        memset(p, 0, (size_t)n * 4096u);
        *phys = (uint64_t)(uintptr_t)p;
    }
    return p;
}

static void pm_free(void *ctx, void *cpu, uint32_t n)
{
    (void)ctx;
    pmm_free_pages(cpu, n);
}

static void *pm_p2c(void *ctx, uint64_t phys)
{
    (void)ctx;
    return (void *)(uintptr_t)phys;
}

/* Cache operations on this address are counted. */
static const void *g_cache_watch;
static unsigned g_cache_watch_hits;

static void pm_cache(void *ctx, const void *p, size_t n)
{
    (void)ctx;
    (void)n;
    if (g_cache_watch && p == g_cache_watch) g_cache_watch_hits++;
}

static uint32_t pm_rd32(void *ctx, uint32_t off)
{
    (void)ctx;
    (void)off;
    return 0x00018000u;     /* MMU FIFO has space and is empty */
}

static void pm_wr32(void *ctx, uint32_t off, uint32_t v)
{
    (void)ctx;
    (void)off;
    (void)v;
}

static void pm_udelay(void *ctx, uint32_t us)
{
    (void)ctx;
    (void)us;
}

static const struct ga10b_gpu_ops g_pm_ops = {
    .rd32 = pm_rd32, .wr32 = pm_wr32, .udelay = pm_udelay,
    .alloc_pages = pm_alloc, .free_pages = pm_free, .phys_to_cpu = pm_p2c,
    .cache_clean = pm_cache, .cache_inval = pm_cache,
};

static struct ga10b_vm g_vm;

/* The fake channel. */
/* FAKE_LATE: the end semaphore is released on wait turn late_turn.
 * FAKE_SLOW_SUBMIT: submit itself takes 4.9 s (the dispatch lock held by
 * another consumer), then as FAKE_LATE. */
enum fake_mode { FAKE_COMPLETE, FAKE_HANG, FAKE_REFUSE, FAKE_LATE, FAKE_SLOW_SUBMIT };
static struct {
    enum fake_mode mode;
    volatile uint32_t *late_slot;
    uint32_t late_payload;
    unsigned late_turn, turns;
    int in_flight;          /* submits minus finishes: never above 1 */
    int max_in_flight;
    int32_t nested_run;     /* run this handle from inside the first wait turn */
    int nested_rc;
    int32_t nested_close;   /* close this handle from inside the first wait turn */
    int32_t nested_clean;   /* clean + invalidate g_cache_watch of this handle there */
    int32_t nested_profile; /* read this handle's profile there */
    uint32_t nested_profile_n;
    uint64_t nested_profile_ns[4];
    bool profile_ts;        /* FAKE_COMPLETE also writes two launches' timestamps */
    bool in_relax;
    uint64_t now;
    unsigned submits, finishes, evicts, tlbs, dumps;
    uint64_t last_gp;
    int evict_rc;
    int tlb_rc;             /* what the TLB invalidate returns */
    bool irqs_off;
    /* recover (#1171): its return code, calls, last `why`, and the call
     * order against dump_fault (a sequence number for each). */
    int recover_rc;
    unsigned recovers;
    uint32_t recover_why;
    unsigned seq, dump_seq, recover_seq;
    bool hang_once;         /* FAKE_HANG for the first submit only */
    bool scattered;
    bool no_q6k;
    bool no_b64;
} g_fk;

static int fk_tlb(void)
{
    g_fk.tlbs++;
    return g_fk.tlb_rc;
}

static int fk_kernel_va(uint32_t sass, uint64_t *va)
{
    if (g_fk.no_q6k && sass == SLM_SASS_MM_Q6K_HMMA_B1) return -1;
    if (g_fk.no_b64 && (sass == SLM_SASS_MM_Q4K_HMMA_BS || sass == SLM_SASS_MM_Q6K_HMMA_BS)) return -1;
    *va = 0x7000000ull + 0x1000u * sass;
    return 0;
}

/* Pretend memory above the page is split, as kbuf memory is. */
static uint64_t fk_v2p(const void *p)
{
    return (uint64_t)(uintptr_t)p;
}

static bool fk_contig(const void *p, uint64_t len)
{
    (void)p;
    (void)len;
    return !g_fk.scattered;
}

static int fk_submit(uint64_t gp, const volatile void *slot, uint32_t payload, uint32_t ms)
{
    (void)ms;
    g_fk.last_gp = gp;
    g_fk.submits++;
    if (g_fk.mode == FAKE_REFUSE) return -1;
    if (++g_fk.in_flight > g_fk.max_in_flight) g_fk.max_in_flight = g_fk.in_flight;
    if (g_fk.mode == FAKE_LATE || g_fk.mode == FAKE_SLOW_SUBMIT) {
        g_fk.late_slot = (volatile uint32_t *)slot;
        g_fk.late_payload = payload;
        if (g_fk.mode == FAKE_SLOW_SUBMIT) g_fk.now += 4900000u;
    }
    if (g_fk.mode == FAKE_HANG && g_fk.hang_once && g_fk.submits > 1u) {
        g_fk.mode = FAKE_COMPLETE;
    }
    if (g_fk.mode == FAKE_COMPLETE) {
        *(volatile uint32_t *)slot = payload;
        /* Timestamps 50 us apart. */
        *((volatile uint64_t *)slot + 1) = 1000050u;
        *((volatile uint64_t *)slot - 1) = 1000000u;
        if (g_fk.profile_ts) {
            /* OP0 and OP1, the slots after END: 10 and 20 us after START. */
            *((volatile uint64_t *)slot + 3) = 1000010u;
            *((volatile uint64_t *)slot + 5) = 1000030u;
        }
    }
    return 0;
}

static void fk_finished(void)
{
    g_fk.finishes++;
    /* As the real flight flag: a recovery that failed already ended it. */
    if (g_fk.in_flight > 0) g_fk.in_flight--;
}

static int fk_evict(void)
{
    g_fk.evicts++;
    return g_fk.evict_rc;
}

static uint64_t fk_now(void)
{
    return g_fk.now;
}

static void fk_relax(void)
{
    /* Each wait turn costs 100 ms, so the 5 s bound doesn't take 5 s. */
    g_fk.now += 100000u;
    if (g_fk.in_relax) return;
    g_fk.in_relax = true;
    g_fk.turns++;
    /* Another task's session, running while this one waits. */
    if (g_fk.turns == 1u && g_fk.nested_run >= 0) {
        struct slm_prog_run_stats st;
        g_fk.nested_rc = slm_prog_run(g_fk.nested_run, 0, &st);
    }
    if (g_fk.turns == 1u && g_fk.nested_close >= 0) slm_prog_close(g_fk.nested_close);
    if (g_fk.turns == 1u && g_fk.nested_clean >= 0) {
        slm_prog_clean(g_fk.nested_clean, g_cache_watch, 64u);
        slm_prog_invalidate(g_fk.nested_clean, g_cache_watch, 64u);
    }
    if (g_fk.turns == 1u && g_fk.nested_profile >= 0) {
        g_fk.nested_profile_n = slm_prog_profile(g_fk.nested_profile, 0, g_fk.nested_profile_ns, 4u);
    }
    if (g_fk.late_slot && g_fk.turns >= g_fk.late_turn) {
        *g_fk.late_slot = g_fk.late_payload;
        g_fk.late_slot = NULL;
    }
    g_fk.in_relax = false;
}

static void fk_dump(const char *why)
{
    (void)why;
    g_fk.dumps++;
    g_fk.dump_seq = ++g_fk.seq;
}

/* The real one (slm_ffi.c) ends the program's flight, success or not. */
static int fk_recover(uint32_t why, uint64_t t_us)
{
    (void)t_us;
    g_fk.recovers++;
    g_fk.recover_why = why;
    g_fk.recover_seq = ++g_fk.seq;
    if (g_fk.in_flight > 0) g_fk.in_flight--;
    return g_fk.recover_rc;
}

static bool fk_irqs(void)
{
    return !g_fk.irqs_off;
}

static const struct slm_prog_backend g_fake = {
    .ops = &g_pm_ops, .vm = &g_vm,
    .tlb_invalidate = fk_tlb, .kernel_va = fk_kernel_va,
    .virt_to_phys = fk_v2p, .is_contiguous = fk_contig,
    .submit = fk_submit, .finished = fk_finished, .l2_evict = fk_evict,
    .now_us = fk_now, .relax = fk_relax, .dump_fault = fk_dump, .irqs_enabled = fk_irqs,
    .recover = fk_recover,
};

static void fake_up(void)
{
    g_alloc_hook = NULL;
    memset(&g_fk, 0, sizeof(g_fk));
    g_fk.recover_rc = -1;       /* as when the channel can't be recovered */
    g_fk.nested_run = -1;
    g_fk.nested_close = -1;
    g_fk.nested_clean = -1;
    g_fk.nested_profile = -1;
    g_cache_watch = NULL;
    g_cache_watch_hits = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_CHAN_OK, ga10b_vm_init(&g_pm_ops, &g_vm));
    slm_prog_backend_set(&g_fake);
}

static void fake_down(void)
{
    slm_prog_backend_set(NULL);
    ga10b_vm_destroy(&g_pm_ops, &g_vm);
}

/* Weights: 2 MiB aligned, as boot models are. */
#define W_BYTES (3u * 1024u * 1024u)

/* A 2 MiB-aligned run: 1024 pages is an order-10 buddy block, aligned to 4 MiB. */
static void *alloc_weights(uint32_t *pages)
{
    *pages = 1024u;
    return pmm_alloc_pages(*pages);
}

static struct slm_prog_open_args open_args(const void *w)
{
    struct slm_prog_open_args a;
    memset(&a, 0, sizeof(a));
    a.weights = w;
    a.weights_bytes = W_BYTES;
    a.data_bytes = 300u * 1024u;
    a.step_bytes = 8192u;
    a.max_launches[0] = 8u;
    a.max_launches[1] = 8u;
    return a;
}

static void test_session_maps_and_frees(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)(uintptr_t)w & (GA10B_VM_PDE0_SPAN - 1u));
    size_t free0 = pmm_get_free_pages();
    struct slm_prog_open_args a = open_args(w);
    struct slm_prog_open_out o;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    TEST_ASSERT_TRUE(o.handle >= 0);
    /* Weights: in place, 64 KB PTEs, cacheable, read-only. */
    TEST_ASSERT_EQUAL_UINT32(1, o.weights_big);
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(uintptr_t)w, ga10b_vm_lookup(&g_pm_ops, &g_vm, o.weights_gpu_va));
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(uintptr_t)w + W_BYTES - 16u,
                            ga10b_vm_lookup(&g_pm_ops, &g_vm, o.weights_gpu_va + W_BYTES - 16u));
    /* Data and control: walks land on the CPU pointers it returned. */
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(uintptr_t)o.data_cpu + 0x12344u,
                            ga10b_vm_lookup(&g_pm_ops, &g_vm, o.data_gpu_va + 0x12344u));
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(uintptr_t)o.step_cpu,
                            ga10b_vm_lookup(&g_pm_ops, &g_vm, o.step_gpu_va));
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.tlbs);
    /* Far fewer table pages than the file has 4 KB pages. */
    TEST_ASSERT_TRUE(o.table_pages_added < 32u);
    /* Weights and data are in separate 2 MB spans from anything small. */
    TEST_ASSERT_EQUAL_UINT64(0, o.weights_gpu_va & (GA10B_VM_PDE0_SPAN - 1u));
    TEST_ASSERT_EQUAL_UINT64(0, o.data_gpu_va & (GA10B_VM_PDE0_SPAN - 1u));

    uint64_t wva = o.weights_gpu_va, dva = o.data_gpu_va;
    slm_prog_close(o.handle);
    /* Close unmaps before it frees, and invalidates between the two. */
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_pm_ops, &g_vm, wva));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_pm_ops, &g_vm, dva));
    TEST_ASSERT_EQUAL_UINT32(2, g_fk.tlbs);

    /* The arena came back; only page-table pages stay with the VM. */
    TEST_ASSERT_TRUE(pmm_get_free_pages() + o.table_pages_added + 4u >= free0);

    /* VA is reused, so reopening doesn't walk the bump allocator. */
    uint64_t next = g_vm.user_next;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    TEST_ASSERT_EQUAL_HEX64(next, g_vm.user_next);
    TEST_ASSERT_EQUAL_HEX64(wva, o.weights_gpu_va);
    slm_prog_close(o.handle);

    /* Bad handle calls are harmless. */
    slm_prog_close(-1);
    slm_prog_close(99);
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_ARG, slm_prog_barrier(0, 0));
    fake_down();
    pmm_free_pages(w, wp);
}

/* A freed 2 MB data range holds big-page tables, so a later session's control
 * region (small pages, same size) must not get it. This failed `slm launch`
 * on the real model after `slm gpu golden` had run on the tiny one. */
static void test_session_va_reuse_keeps_page_size(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_args a = open_args(w);
    struct slm_prog_open_out o;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    uint64_t old_data = o.data_gpu_va, old_control = o.step_gpu_va;
    slm_prog_close(o.handle);

    /* Data no longer fits 2 MB, so only the control region can reuse a range. */
    a.data_bytes = 3u * 1024u * 1024u;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    TEST_ASSERT_EQUAL_HEX64(old_control, o.step_gpu_va);
    TEST_ASSERT_TRUE(o.data_gpu_va != old_data);
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(uintptr_t)o.step_cpu,
                            ga10b_vm_lookup(&g_pm_ops, &g_vm, o.step_gpu_va));
    slm_prog_close(o.handle);
    fake_down();
    pmm_free_pages(w, wp);
}

/* Scattered weights (kbuf) still map, one 4 KB PTE per page. */
static void test_session_scattered_weights(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)(uintptr_t)w & (GA10B_VM_PDE0_SPAN - 1u));
    g_fk.scattered = true;
    struct slm_prog_open_args a = open_args(w);
    struct slm_prog_open_out o;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    TEST_ASSERT_EQUAL_UINT32(0, o.weights_big);
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(uintptr_t)w + 0x7000u,
                            ga10b_vm_lookup(&g_pm_ops, &g_vm, o.weights_gpu_va + 0x7000u));
    /* 768 PTEs of 4 KB need several table pages more than 64 KB would. */
    TEST_ASSERT_TRUE(o.table_pages_added >= 2u);
    slm_prog_close(o.handle);
    fake_down();
    pmm_free_pages(w, wp);
}

/* Failures leave nothing mapped or allocated. */
static void test_session_open_failures(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)(uintptr_t)w & (GA10B_VM_PDE0_SPAN - 1u));
    struct slm_prog_open_args a = open_args(w);
    struct slm_prog_open_out o;
    a.barrier = 99u;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_ARG, slm_prog_open(&a, &o));
    a = open_args(w);
    a.data_bytes = 0;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_ARG, slm_prog_open(&a, &o));
    /* Asking for more memory than exists fails cleanly. */
    size_t free0 = pmm_get_free_pages();
    a = open_args(w);
    a.data_bytes = 0xF0000000ull;
    int rc = slm_prog_open(&a, &o);
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_NOMEM, rc);
    TEST_ASSERT_TRUE(pmm_get_free_pages() + 16u >= free0);

    /* All sessions taken. */
    a = open_args(w);
    int32_t h[SLM_PROG_MAX_SESSIONS];
    for (int i = 0; i < SLM_PROG_MAX_SESSIONS; i++) {
        TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
        h[i] = o.handle;
    }
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_NOMEM, slm_prog_open(&a, &o));
    for (int i = 0; i < SLM_PROG_MAX_SESSIONS; i++) {
        slm_prog_close(h[i]);
    }

    /* No channel at all: the error Rust maps to the per-op path. */
    slm_prog_backend_set(NULL);
    a = open_args(w);
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_NO_BACKEND, slm_prog_open(&a, &o));
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_max_batch(false));
    ga10b_vm_destroy(&g_pm_ops, &g_vm);
    pmm_free_pages(w, wp);
}

static void open_built_flags(void *w, struct slm_prog_open_out *o, uint32_t flags)
{
    struct slm_prog_open_args a = open_args(w);
    a.flags = flags;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, o));
    struct slm_prog_op op = {
        .kernel = SLM_PROG_K_ADD, .rows = 1, .dim = { 256 },
        .ptr = { 0x10, 0x20, 0x30 },
    };
    TEST_ASSERT_EQUAL_INT(0, slm_prog_add(o->handle, 0, &op));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_barrier(o->handle, 0));
    TEST_ASSERT_EQUAL_INT(1, slm_prog_add(o->handle, 0, &op));
    /* Unfinished programs don't run. */
    struct slm_prog_run_stats st;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_ARG, slm_prog_run(o->handle, 0, &st));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_finish(o->handle, 0));
}

static void open_built(void *w, struct slm_prog_open_out *o)
{
    open_built_flags(w, o, 0u);
}

static void test_run_ok_and_patch(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)(uintptr_t)w & (GA10B_VM_PDE0_SPAN - 1u));
    struct slm_prog_open_out o;
    open_built(w, &o);
    int32_t h = o.handle;
    struct slm_prog_run_stats st;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(h, 0, &st));
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.submits);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.finishes);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.evicts);
    TEST_ASSERT_EQUAL_UINT64(50, st.gpu_ns);
    TEST_ASSERT_TRUE(g_fk.last_gp != 0u);

    /* Patching a launch's args doesn't touch anything the GPU saw before. */
    struct slm_prog_op op = {
        .kernel = SLM_PROG_K_ADD, .rows = 1, .dim = { 512 }, .ptr = { 0x10, 0x20, 0x40 },
    };
    TEST_ASSERT_EQUAL_INT(0, slm_prog_patch(h, 0, 1, &op));
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_ARG, slm_prog_patch(h, 0, 2, &op));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_update(h, 0, 1, &op));
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_ARG, slm_prog_update(h, 0, 2, &op));

    /* The semaphore is re-armed per step: the second run needs a new release. */
    g_fk.mode = FAKE_HANG;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_TIMEOUT, slm_prog_run(h, 0, &st));
    slm_prog_close(h);
    fake_down();
    pmm_free_pages(w, wp);
}

/* A hang is bounded, dumps the fault state once, doesn't evict, and kills
 * the session so nothing else is queued behind it. */
/* A UEFI-direct kernel built without SASS booted to the shell with every GPU
 * consumer on the CPU, but `slm launch --gpu on` blamed the boot path. Each
 * cause now names itself, the missing SASS first: with none, the cold
 * bring-up stops at MNIST and the rest can't be reached. */
static void test_unavailable_reason_names_the_cause(void)
{
    TEST_ASSERT_EQUAL_STRING(SLM_PROG_WHY_KEXEC, slm_prog_reason_for(false, true, false, false));
    TEST_ASSERT_EQUAL_STRING(SLM_PROG_WHY_NO_SASS, slm_prog_reason_for(true, true, true, false));
    TEST_ASSERT_EQUAL_STRING(SLM_PROG_WHY_NO_SASS, slm_prog_reason_for(true, true, false, false));
    TEST_ASSERT_NOT_NULL(strstr(SLM_PROG_WHY_NO_SASS, "make oplib-shaders"));
    TEST_ASSERT_EQUAL_STRING(SLM_PROG_WHY_NO_CHANNEL, slm_prog_reason_for(true, false, false, false));
    TEST_ASSERT_EQUAL_STRING(SLM_PROG_WHY_NOT_ATTACHED, slm_prog_reason_for(true, false, true, false));
    TEST_ASSERT_NULL(slm_prog_reason_for(true, false, true, true));
    /* This boot (QEMU, x86) has no cold channel at all. */
    slm_prog_backend_set(NULL);
    TEST_ASSERT_EQUAL_INT(0, slm_prog_available());
    TEST_ASSERT_EQUAL_STRING(SLM_PROG_WHY_KEXEC, slm_prog_unavailable_reason());
}

/* A session's two runs differ in the GPFIFO entry alone. */
static void test_run_marked_uses_the_marked_entry(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_out o;
    open_built(w, &o);
    int32_t h = o.handle;
    struct slm_prog_run_stats st;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(h, 0, &st));
    uint64_t plain = g_fk.last_gp;
    uint64_t ns[8];
    memset(ns, 0xFF, sizeof(ns));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run_marked(h, 0, &st, ns, 8));
    TEST_ASSERT_NOT_EQUAL(plain, g_fk.last_gp);
    TEST_ASSERT_EQUAL_UINT64(50, st.gpu_ns);
    /* No marks in this program: one interval, all of it. */
    TEST_ASSERT_EQUAL_UINT64(50, ns[0]);
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(h, 0, &st));
    TEST_ASSERT_EQUAL_UINT64(plain, g_fk.last_gp);
    /* Too short an array: the run still counts, nothing is written. */
    memset(ns, 0xAB, sizeof(ns));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run_marked(h, 0, &st, ns, 0));
    TEST_ASSERT_EQUAL_HEX64(0xABABABABABABABABull, ns[0]);
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_ARG, slm_prog_run_marked(h, 7, &st, ns, 8));
    slm_prog_close(h);
    fake_down();
    pmm_free_pages(w, wp);
}

static void test_run_timeout_is_bounded(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)(uintptr_t)w & (GA10B_VM_PDE0_SPAN - 1u));
    struct slm_prog_open_out o;
    open_built(w, &o);
    int32_t h = o.handle;
    g_fk.mode = FAKE_HANG;
    struct slm_prog_run_stats st;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_TIMEOUT, slm_prog_run(h, 0, &st));
    TEST_ASSERT_TRUE(g_fk.now >= SLM_PROG_STEP_TIMEOUT_MS * 1000ull);
    TEST_ASSERT_TRUE(g_fk.now < (SLM_PROG_STEP_TIMEOUT_MS + 200u) * 1000ull);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.dumps);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.finishes);
    TEST_ASSERT_EQUAL_UINT32(0, g_fk.evicts);
    unsigned sub = g_fk.submits;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_DEAD, slm_prog_run(h, 0, &st));
    TEST_ASSERT_EQUAL_UINT32(sub, g_fk.submits);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.dumps);
    slm_prog_close(h);
    fake_down();
    pmm_free_pages(w, wp);
}

static void test_run_other_failures(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)(uintptr_t)w & (GA10B_VM_PDE0_SPAN - 1u));
    struct slm_prog_open_out o;
    open_built(w, &o);
    int32_t h = o.handle;
    struct slm_prog_run_stats st;

    /* Waiting with IRQs masked is how #1093 hung the board: refuse it. */
    g_fk.irqs_off = true;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_IRQS_OFF, slm_prog_run(h, 0, &st));
    TEST_ASSERT_EQUAL_UINT32(0, g_fk.submits);
    g_fk.irqs_off = false;

    /* Submit refused: nothing in flight, nothing to report as finished. */
    g_fk.mode = FAKE_REFUSE;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_SUBMIT, slm_prog_run(h, 0, &st));
    TEST_ASSERT_EQUAL_UINT32(0, g_fk.finishes);

    /* A failed evict would hand the CPU stale logits. */
    g_fk.mode = FAKE_COMPLETE;
    g_fk.evict_rc = -1;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_EVICT, slm_prog_run(h, 0, &st));
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_DEAD, slm_prog_run(h, 0, &st));
    slm_prog_close(h);
    fake_down();
    pmm_free_pages(w, wp);
}


/* W1 (#475 review): a timed-out session's arena went back to the PMM with
 * no L2 evict, so the GPU's dirty lines could land on the CPU KV cache the
 * fallback allocates next. A dead session's pages are now leaked, its PTEs
 * are gone, and its VA is never handed out again. */
static void test_dead_session_leaks_arena(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_out o;
    open_built(w, &o);
    uint64_t dva = o.data_gpu_va;
    g_fk.mode = FAKE_HANG;
    struct slm_prog_run_stats st;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_TIMEOUT, slm_prog_run(o.handle, 0, &st));
    size_t before = pmm_get_free_pages();
    slm_prog_close(o.handle);
    /* The weights mapping may give back table pages, the arena may not. */
    TEST_ASSERT_TRUE(pmm_get_free_pages() < before + 300u * 1024u / 4096u);
    TEST_ASSERT_EQUAL_UINT32(1, slm_prog_leaked_sessions());
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_pm_ops, &g_vm, dva));
    /* A later session gets fresh VA, not the range a hung GPU may still use. */
    g_fk.mode = FAKE_COMPLETE;
    struct slm_prog_open_out o2;
    open_built(w, &o2);
    TEST_ASSERT_NOT_EQUAL(dva, o2.data_gpu_va);
    slm_prog_close(o2.handle);
    TEST_ASSERT_EQUAL_UINT32(1, slm_prog_leaked_sessions());

    /* A failed evict is the same: L2 still holds the step's lines. */
    open_built(w, &o);
    g_fk.evict_rc = -1;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_EVICT, slm_prog_run(o.handle, 0, &st));
    slm_prog_close(o.handle);
    TEST_ASSERT_EQUAL_UINT32(2, slm_prog_leaked_sessions());
    fake_down();
    pmm_free_pages(w, wp);
}

/* #1171: a timed-out step whose channel the backend recovers. The failure
 * state is dumped first, then the channel recovered (which ends the
 * flight: finished() is not called, or another dispatcher could queue
 * behind the hung program in between). The session is dead, but nothing
 * of it can run any more, so its arena and VA come back on close, and the
 * next session runs on the same channel. Before #1171 every timeout
 * leaked the arena and the channel stayed wedged until a reboot. */
static void test_timeout_with_recovery_frees_and_runs_again(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_out o;
    open_built(w, &o);
    uint64_t dva = o.data_gpu_va;
    g_fk.mode = FAKE_HANG;
    g_fk.recover_rc = 0;
    struct slm_prog_run_stats st;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_RECOVERED, slm_prog_run(o.handle, 0, &st));
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.dumps);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.recovers);
    TEST_ASSERT_EQUAL_UINT32(SLM_PROG_RECOVER_TIMEOUT, g_fk.recover_why);
    TEST_ASSERT_TRUE(g_fk.dump_seq < g_fk.recover_seq);
    TEST_ASSERT_EQUAL_UINT32(0, g_fk.finishes);
    TEST_ASSERT_EQUAL_INT(0, g_fk.in_flight);
    TEST_ASSERT_EQUAL_UINT32(0, g_fk.evicts);
    /* Dead all the same: its program is half-run. */
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_DEAD, slm_prog_run(o.handle, 0, &st));
    size_t before = pmm_get_free_pages();
    slm_prog_close(o.handle);
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_leaked_sessions());
    TEST_ASSERT_TRUE(pmm_get_free_pages() >= before + 300u * 1024u / 4096u);
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_pm_ops, &g_vm, dva));
    /* The next session gets the arena VA back and runs. */
    g_fk.mode = FAKE_COMPLETE;
    struct slm_prog_open_out o2;
    open_built(w, &o2);
    TEST_ASSERT_EQUAL_HEX64(dva, o2.data_gpu_va);
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(o2.handle, 0, &st));
    slm_prog_close(o2.handle);
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_leaked_sessions());
    fake_down();
    pmm_free_pages(w, wp);
}

/* A failed L2 evict leaves dirty lines of the step in L2: recover too. A
 * recovery that fails keeps the old behaviour: TIMEOUT / EVICT, finished,
 * arena leaked. */
static void test_recovery_after_evict_and_when_it_fails(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_out o;
    struct slm_prog_run_stats st;
    open_built(w, &o);
    g_fk.evict_rc = -1;
    g_fk.recover_rc = 0;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_RECOVERED, slm_prog_run(o.handle, 0, &st));
    TEST_ASSERT_EQUAL_UINT32(SLM_PROG_RECOVER_EVICT, g_fk.recover_why);
    slm_prog_close(o.handle);
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_leaked_sessions());

    g_fk.evict_rc = 0;
    g_fk.recover_rc = -1;
    g_fk.recovers = 0;
    g_fk.finishes = 0;
    open_built(w, &o);
    g_fk.mode = FAKE_HANG;
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_TIMEOUT, slm_prog_run(o.handle, 0, &st));
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.recovers);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.finishes);
    TEST_ASSERT_EQUAL_INT(0, g_fk.in_flight);
    slm_prog_close(o.handle);
    TEST_ASSERT_EQUAL_UINT32(1, slm_prog_leaked_sessions());
    fake_down();
    pmm_free_pages(w, wp);
}

/* W2/W3 (#475 review): the shell and slm-runner each have a session. A step
 * of one started while the other's was in flight, and its finish cleared the
 * other's in-flight flag. Now one lock covers a step from submit to the end
 * of its wait: the second waits, bounded, and never submits under the first. */
static void test_two_sessions_never_overlap(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_out a, b;
    open_built(w, &a);
    open_built(w, &b);
    TEST_ASSERT_NOT_EQUAL(a.handle, b.handle);
    g_fk.mode = FAKE_LATE;
    g_fk.late_turn = 1u;
    g_fk.nested_run = b.handle;
    g_fk.nested_rc = 1;
    struct slm_prog_run_stats st;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(a.handle, 0, &st));
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_BUSY, g_fk.nested_rc);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.submits);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.finishes);
    TEST_ASSERT_EQUAL_INT(1, g_fk.max_in_flight);
    TEST_ASSERT_EQUAL_INT(0, g_fk.in_flight);
    /* With the lock free, b runs. */
    g_fk.nested_run = -1;
    g_fk.mode = FAKE_COMPLETE;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(b.handle, 0, &st));

    /* A close during another session's step is done when that step ends. */
    uint64_t bva = b.data_gpu_va;
    g_fk.mode = FAKE_LATE;
    g_fk.turns = 0;
    g_fk.late_turn = 1u;
    g_fk.nested_close = b.handle;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(a.handle, 0, &st));
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_pm_ops, &g_vm, bva));
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_ARG, slm_prog_barrier(b.handle, 0));
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_leaked_sessions());
    slm_prog_close(a.handle);
    fake_down();
    pmm_free_pages(w, wp);
}

/* #475 W-A: clean and invalidate took the program lock and silently did
 * nothing when it stayed busy (another session's step, a fault dump), so a
 * step could run on a stale step page or the CPU read stale logits. They now
 * take no lock and always reach the cache. */
static void test_cache_ops_never_skipped_while_another_step_runs(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_out a, b;
    open_built(w, &a);
    open_built(w, &b);
    g_cache_watch = b.data_cpu;
    g_fk.mode = FAKE_LATE;
    g_fk.late_turn = 1u;
    g_fk.nested_clean = b.handle;
    struct slm_prog_run_stats st;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(a.handle, 0, &st));
    TEST_ASSERT_EQUAL_UINT32(2, g_cache_watch_hits);
    /* With IRQs masked too: no lock to fail on. */
    g_fk.irqs_off = true;
    slm_prog_clean(b.handle, g_cache_watch, 64u);
    g_fk.irqs_off = false;
    TEST_ASSERT_EQUAL_UINT32(3, g_cache_watch_hits);
    /* A closed handle does nothing. */
    slm_prog_close(b.handle);
    slm_prog_clean(b.handle, g_cache_watch, 64u);
    TEST_ASSERT_EQUAL_UINT32(3, g_cache_watch_hits);
    slm_prog_close(a.handle);
    fake_down();
    pmm_free_pages(w, wp);
}

/* #1164 review W1: `slm gpu profile` read each launch's time under the
 * program lock, so while another session's steps ran it could wait up to a
 * step timeout per launch, about 400 of them, with the Rust session table
 * locked. The read now takes no lock, like clean and invalidate: it returns
 * the owner's last run at once, whoever holds the lock. */
static void test_profile_reads_without_the_lock(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_out a, b;
    open_built_flags(w, &a, SLM_PROG_FLAG_PROFILE);
    open_built(w, &b);
    struct slm_prog_run_stats st;
    g_fk.profile_ts = true;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(a.handle, 0, &st));
    g_fk.profile_ts = false;
    uint64_t ns[4];
    memset(ns, 0xAB, sizeof(ns));
    TEST_ASSERT_EQUAL_UINT32(2, slm_prog_profile(a.handle, 0, ns, 4u));
    TEST_ASSERT_EQUAL_UINT64(10, ns[0]);
    TEST_ASSERT_EQUAL_UINT64(20, ns[1]);
    TEST_ASSERT_EQUAL_HEX64(0xABABABABABABABABull, ns[2]);
    /* A short array gets the first launches only. */
    memset(ns, 0xAB, sizeof(ns));
    TEST_ASSERT_EQUAL_UINT32(1, slm_prog_profile(a.handle, 0, ns, 1u));
    TEST_ASSERT_EQUAL_HEX64(0xABABABABABABABABull, ns[1]);
    /* Not profiled, bad shape, no array: nothing. */
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_profile(b.handle, 0, ns, 4u));
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_profile(a.handle, 7, ns, 4u));
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_profile(a.handle, 0, NULL, 4u));

    /* During b's step, which holds the lock, and with IRQs masked. */
    g_fk.mode = FAKE_LATE;
    g_fk.late_turn = 1u;
    g_fk.nested_profile = a.handle;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(b.handle, 0, &st));
    TEST_ASSERT_EQUAL_UINT32(2, g_fk.nested_profile_n);
    TEST_ASSERT_EQUAL_UINT64(10, g_fk.nested_profile_ns[0]);
    TEST_ASSERT_EQUAL_UINT64(20, g_fk.nested_profile_ns[1]);
    g_fk.irqs_off = true;
    TEST_ASSERT_EQUAL_UINT32(2, slm_prog_profile(a.handle, 0, ns, 4u));
    g_fk.irqs_off = false;

    /* A closed handle reads nothing. */
    slm_prog_close(a.handle);
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_profile(a.handle, 0, ns, 4u));
    slm_prog_close(b.handle);
    fake_down();
    pmm_free_pages(w, wp);
}

/* Review suggestion: the step deadline counted the time submit spent waiting
 * for the dispatch lock, so a step queued behind another GPU consumer could
 * time out (and kill its session) with the GPU never late. */
static void test_deadline_starts_at_the_doorbell(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_out o;
    open_built(w, &o);
    g_fk.mode = FAKE_SLOW_SUBMIT;
    g_fk.late_turn = 3u;
    struct slm_prog_run_stats st;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_run(o.handle, 0, &st));
    TEST_ASSERT_EQUAL_UINT32(0, g_fk.dumps);
    slm_prog_close(o.handle);
    fake_down();
    pmm_free_pages(w, wp);
}

/* Review suggestion: barriers weren't counted, and the pushbuffer is sized
 * for one per launch, so extra barriers wrote past it. */
static void test_prog_barrier_cap(void)
{
    static struct ga10b_prog p;
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, prog_init(&p, 0, GA10B_PROG_BARRIER_WFI_FLUSH));
    for (unsigned i = 0; i < 8u; i++) {
        TEST_ASSERT_EQUAL_INT(GA10B_PROG_OK, ga10b_prog_add_barrier(&p));
    }
    TEST_ASSERT_EQUAL_INT(GA10B_PROG_ERR_FULL, ga10b_prog_add_barrier(&p));
    TEST_ASSERT_EQUAL_UINT32(8u, p.n_barriers);
}

/* The arena works without a model: what `slm gpu smoke` opens. */
static void test_session_without_weights(void)
{
    fake_up();
    struct slm_prog_open_args a = open_args(NULL);
    a.weights_bytes = 0;
    struct slm_prog_open_out o;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    TEST_ASSERT_EQUAL_HEX64(0, o.weights_gpu_va);
    TEST_ASSERT_EQUAL_HEX64((uint64_t)(uintptr_t)o.data_cpu,
                            ga10b_vm_lookup(&g_pm_ops, &g_vm, o.data_gpu_va));
    slm_prog_close(o.handle);
    fake_down();
}

static void test_kernel_lookup_and_batch(void)
{
    fake_up();
    TEST_ASSERT_EQUAL_UINT32(64u, slm_prog_max_batch(false));
    g_fk.no_b64 = true;
    TEST_ASSERT_EQUAL_UINT32(8u, slm_prog_max_batch(false));
    TEST_ASSERT_EQUAL_UINT32(64u, slm_prog_max_batch(true));
    g_fk.no_b64 = false;
    g_fk.no_q6k = true;
    TEST_ASSERT_EQUAL_UINT32(0u, slm_prog_max_batch(false));

    /* A missing kernel fails the add with the code Rust reports. */
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_args a = open_args(w);
    struct slm_prog_open_out o;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    struct slm_prog_op op = {
        .kernel = SLM_PROG_K_MM_Q6K, .rows = 1, .dim = { 256, 256 }, .ptr = { 0x10, 0x20, 0x30 },
    };
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_NO_KERNEL, slm_prog_add(o.handle, 0, &op));
    slm_prog_close(o.handle);
    fake_down();
    pmm_free_pages(w, wp);
}

/* ---- One lock for every writer of the GPU VM (#1173) ----
 *
 * The operator library's cold address space writes the same VM as the
 * program: `nvgpu oplib wpool` during `slm launch --gpu on`. Both used a
 * bump allocator and ga10b_vm_map with no lock in common, so one could
 * overwrite a page-table entry the other had just pointed at its new
 * table, losing every mapping below it. g_alloc_hook runs one side inside
 * the other's page-table write, where that happens. */

#define OPL_CHUNK (64u * 1024u)
#define OPL_CHUNKS 2u

static struct oplib_gpu_as g_oas;
static struct oplib_cold_as_ctx g_octx;
static struct oplib_wpool g_opool;
static int g_opool_rc;
static uint64_t g_opool_wait;
static struct slm_prog_open_args g_nested_args;
static struct slm_prog_open_out g_nested_out;
static int g_nested_open_rc;
static int32_t g_nested_close_h;

/* The operator library's side, attached as on the cold channel. */
static void oplib_side_up(void)
{
    oplib_gpu_as_init_cold(&g_oas, &g_octx, &g_pm_ops, &g_vm, slm_prog_vm_lock,
                           slm_prog_vm_unlock);
    memset(&g_opool, 0, sizeof(g_opool));
    g_opool_rc = 1;
    g_opool_wait = 0;
    g_nested_open_rc = 1;
    g_nested_close_h = -1;
}

static void oplib_side_free(const struct oplib_wpool *w)
{
    for (uint32_t i = 0; i < w->n_chunks; i++) {
        pmm_free_pages(w->chunk[i].cpu, OPL_CHUNK / 4096u);
    }
}

/* `nvgpu oplib wpool`'s build, small. */
static int oplib_wpool(struct oplib_wpool *w)
{
    return oplib_wpool_build(&g_oas, OPL_CHUNKS * OPL_CHUNK, OPL_CHUNK, w);
}

static void hook_oplib_wpool(void)
{
    uint64_t t0 = g_fk.now;
    g_opool_rc = oplib_wpool(&g_opool);
    g_opool_wait = g_fk.now - t0;
}

static void hook_prog_open(void)
{
    g_nested_open_rc = slm_prog_open(&g_nested_args, &g_nested_out);
}

static void hook_prog_close(void)
{
    slm_prog_close(g_nested_close_h);
}

static bool overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

/* Every page of both sides walks to its memory, and their VAs are apart. */
static void check_both_mapped(const struct slm_prog_open_out *o, const void *w,
                              const struct oplib_wpool *pool)
{
    for (uint64_t off = 0; off < W_BYTES; off += 64u * 1024u) {
        TEST_ASSERT_TRUE_MESSAGE((uint64_t)(uintptr_t)w + off ==
                                     ga10b_vm_lookup(&g_pm_ops, &g_vm, o->weights_gpu_va + off),
                                 "program weights page lost");
    }
    TEST_ASSERT_TRUE_MESSAGE((uint64_t)(uintptr_t)o->data_cpu ==
                                 ga10b_vm_lookup(&g_pm_ops, &g_vm, o->data_gpu_va),
                             "program arena page lost");
    TEST_ASSERT_TRUE_MESSAGE((uint64_t)(uintptr_t)o->step_cpu ==
                                 ga10b_vm_lookup(&g_pm_ops, &g_vm, o->step_gpu_va),
                             "program control page lost");
    TEST_ASSERT_EQUAL_UINT32(OPL_CHUNKS, pool->n_chunks);
    for (uint32_t c = 0; c < pool->n_chunks; c++) {
        for (uint32_t off = 0; off < OPL_CHUNK; off += 4096u) {
            uint64_t va = pool->gpu_va + (uint64_t)c * OPL_CHUNK + off;
            TEST_ASSERT_TRUE_MESSAGE(pool->chunk[c].phys + off ==
                                         ga10b_vm_lookup(&g_pm_ops, &g_vm, va),
                                     "operator library pool page lost");
        }
    }
    /* Weights take a 4 MB span, data and control 2 MB each. */
    TEST_ASSERT_FALSE(overlaps(o->weights_gpu_va, 4u << 20, pool->gpu_va, pool->size));
    TEST_ASSERT_FALSE(overlaps(o->data_gpu_va, 2u << 20, pool->gpu_va, pool->size));
    TEST_ASSERT_FALSE(overlaps(o->step_gpu_va, 2u << 20, pool->gpu_va, pool->size));
}

/* `nvgpu oplib wpool` while `slm launch` maps its weights: the pool build
 * stays out until the open is done (here it times out, since the fake clock
 * runs while the open can't), then both are whole. */
static void test_vm_oplib_write_during_program_build(void)
{
    fake_up();
    oplib_side_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_args a = open_args(w);
    struct slm_prog_open_out o;
    g_alloc_hook = hook_oplib_wpool;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    TEST_ASSERT_NULL(g_alloc_hook);
    if (g_opool_rc != OPLIB_AS_OK) {
        TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_BUSY, g_opool_rc);
        TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, oplib_wpool(&g_opool));
    }
    check_both_mapped(&o, w, &g_opool);
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_BUSY, g_opool_rc);
    struct slm_prog_vm_lock_stats ls;
    slm_prog_vm_lock_stats(&ls);
    TEST_ASSERT_EQUAL_UINT32(1, ls.vm_waited_on_map);
    TEST_ASSERT_EQUAL_UINT32(1, ls.vm_busy);
    /* The retry: one reserve and one map per chunk. */
    TEST_ASSERT_EQUAL_UINT32(1u + OPL_CHUNKS, ls.vm_takes);
    slm_prog_close(o.handle);
    oplib_side_free(&g_opool);
    fake_down();
    pmm_free_pages(w, wp);
}

/* The reverse: `slm launch` while the pool build writes a page table. */
static void test_vm_program_build_during_oplib_write(void)
{
    fake_up();
    oplib_side_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    g_nested_args = open_args(w);
    g_alloc_hook = hook_prog_open;
    struct oplib_wpool pool;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, oplib_wpool(&pool));
    TEST_ASSERT_NULL(g_alloc_hook);
    struct slm_prog_open_out o = g_nested_out;
    if (g_nested_open_rc != 0) {
        TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_BUSY, g_nested_open_rc);
        TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&g_nested_args, &o));
    }
    check_both_mapped(&o, w, &pool);
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_BUSY, g_nested_open_rc);
    struct slm_prog_vm_lock_stats ls;
    slm_prog_vm_lock_stats(&ls);
    TEST_ASSERT_EQUAL_UINT32(1, ls.map_waited_on_vm);
    TEST_ASSERT_EQUAL_UINT32(0, ls.vm_busy);
    slm_prog_close(o.handle);
    oplib_side_free(&pool);
    fake_down();
    pmm_free_pages(w, wp);
}

/* With IRQs masked (as under the GPU dispatch lock) a VM writer can't wait
 * for a holder that may need that lock: one try, then BUSY, with no wait
 * turn and nothing mapped. With the lock free it gets it at once. */
static void test_vm_lock_irqs_masked_tries_once(void)
{
    fake_up();
    oplib_side_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_args a = open_args(w);
    struct slm_prog_open_out o;
    g_fk.irqs_off = true;
    g_alloc_hook = hook_oplib_wpool;
    g_opool_wait = 99u;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    g_fk.irqs_off = false;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_ERR_BUSY, g_opool_rc);
    TEST_ASSERT_EQUAL_UINT64(0, g_opool_wait);
    TEST_ASSERT_EQUAL_UINT32(0, g_opool.n_chunks);

    g_fk.irqs_off = true;
    uint64_t t0 = g_fk.now;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_vm_lock());
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_BUSY, slm_prog_vm_lock());
    slm_prog_vm_unlock();
    TEST_ASSERT_EQUAL_UINT64(t0, g_fk.now);
    g_fk.irqs_off = false;
    slm_prog_close(o.handle);
    fake_down();
    pmm_free_pages(w, wp);
}

/* A session closed while another VM writer holds the lock is freed when that
 * writer unlocks, as when slm_prog itself holds it. */
static void test_vm_writer_unlock_finishes_a_close(void)
{
    fake_up();
    oplib_side_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    struct slm_prog_open_args a = open_args(w);
    struct slm_prog_open_out o;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    uint64_t dva = o.data_gpu_va;
    unsigned tlbs = g_fk.tlbs;
    g_nested_close_h = o.handle;
    g_alloc_hook = hook_prog_close;
    struct oplib_wpool pool;
    TEST_ASSERT_EQUAL_INT(OPLIB_AS_OK, oplib_wpool(&pool));
    TEST_ASSERT_NULL(g_alloc_hook);
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_pm_ops, &g_vm, dva));
    TEST_ASSERT_EQUAL_INT(SLM_PROG_ERR_ARG, slm_prog_barrier(o.handle, 0));
    TEST_ASSERT_EQUAL_UINT32(tlbs + 1u, g_fk.tlbs);
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_leaked_sessions());
    oplib_side_free(&pool);
    fake_down();
    pmm_free_pages(w, wp);
}

/* #1159: a close that finds the lock held past its wait (here by another VM
 * writer; on hardware also a step timeout's dump) leaves the session, its
 * weights PTEs included, until the holder's unlock frees it. Its caller has
 * returned and dropped the registry pin by then, so slm_prog_weights_mapped,
 * which the registry's unload asks, must keep counting it until the PTEs
 * are gone. */
static void test_deferred_close_keeps_weights_mapped(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    const uint8_t *wb = (const uint8_t *)w;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_weights_mapped(w, W_BYTES));
    struct slm_prog_open_args a = open_args(w);
    struct slm_prog_open_out o;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    uint64_t wva = o.weights_gpu_va;
    TEST_ASSERT_NOT_EQUAL(0, ga10b_vm_lookup(&g_pm_ops, &g_vm, wva));
    TEST_ASSERT_EQUAL_INT(1, slm_prog_weights_mapped(w, W_BYTES));
    /* Any overlap counts, and only an overlap. */
    TEST_ASSERT_EQUAL_INT(1, slm_prog_weights_mapped(wb + W_BYTES - 1u, 4096u));
    TEST_ASSERT_EQUAL_INT(1, slm_prog_weights_mapped(wb - 4096, 4097u));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_weights_mapped(wb + W_BYTES, 4096u));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_weights_mapped(wb - 4096, 4096u));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_weights_mapped(w, 0u));

    /* Held past the close's 9 s wait. */
    TEST_ASSERT_EQUAL_INT(0, slm_prog_vm_lock());
    uint64_t t0 = g_fk.now;
    unsigned tlbs = g_fk.tlbs;
    slm_prog_close(o.handle);
    TEST_ASSERT_TRUE(g_fk.now - t0 >= ((uint64_t)SLM_PROG_STEP_TIMEOUT_MS + 1000u) * 1000u);
    TEST_ASSERT_NOT_EQUAL(0, ga10b_vm_lookup(&g_pm_ops, &g_vm, wva));
    TEST_ASSERT_EQUAL_UINT32(tlbs, g_fk.tlbs);
    TEST_ASSERT_EQUAL_INT(1, slm_prog_weights_mapped(w, W_BYTES));

    slm_prog_vm_unlock();
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_pm_ops, &g_vm, wva));
    TEST_ASSERT_EQUAL_UINT32(tlbs + 1u, g_fk.tlbs);
    TEST_ASSERT_EQUAL_INT(0, slm_prog_weights_mapped(w, W_BYTES));
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_leaked_sessions());
    fake_down();
    pmm_free_pages(w, wp);
}

/* #1159: a close whose TLB invalidate fails has removed the PTEs, but the
 * GPU's TLB may still translate the weights, so they must stay counted as
 * mapped (for the rest of the boot) and `slm unload` must not free them. */
static void test_tlb_failure_on_close_keeps_weights_mapped(void)
{
    fake_up();
    uint32_t wp;
    void *w = alloc_weights(&wp);
    TEST_ASSERT_NOT_NULL(w);
    const uint8_t *wb = (const uint8_t *)w;
    struct slm_prog_open_args a = open_args(w);
    struct slm_prog_open_out o;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    uint64_t wva = o.weights_gpu_va;
    g_fk.tlb_rc = -1;
    slm_prog_close(o.handle);
    TEST_ASSERT_EQUAL_UINT32(1, slm_prog_leaked_sessions());
    TEST_ASSERT_EQUAL_HEX64(0, ga10b_vm_lookup(&g_pm_ops, &g_vm, wva));
    TEST_ASSERT_EQUAL_INT(1, slm_prog_weights_mapped(w, W_BYTES));
    TEST_ASSERT_EQUAL_INT(1, slm_prog_weights_mapped(wb + W_BYTES - 1u, 1u));
    TEST_ASSERT_EQUAL_INT(0, slm_prog_weights_mapped(wb + W_BYTES, 4096u));
    /* Still counted after later sessions open and close normally. */
    g_fk.tlb_rc = 0;
    TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&a, &o));
    slm_prog_close(o.handle);
    TEST_ASSERT_EQUAL_INT(1, slm_prog_weights_mapped(w, W_BYTES));

    /* More failed closes than the table holds: every range counts. */
    uint8_t *other = (uint8_t *)w + W_BYTES + 4096u;
    for (unsigned i = 0; i < 5u; i++) {
        struct slm_prog_open_args b = open_args(w);
        b.weights_bytes = 4096u * (i + 1u);
        TEST_ASSERT_EQUAL_INT(0, slm_prog_open(&b, &o));
        g_fk.tlb_rc = -1;
        slm_prog_close(o.handle);
        g_fk.tlb_rc = 0;
    }
    TEST_ASSERT_EQUAL_INT(1, slm_prog_weights_mapped(other, 4096u));
    fake_down();
    pmm_free_pages(w, wp);
}

/* ---- End to end: the tiny model through the production FFI ---- */

#define MAX_TOKENS 16u

typedef struct {
    uint32_t ids[MAX_TOKENS];
    uint32_t count;
} collected_t;

static int32_t collect_cb(void *user, uint32_t id, const uint8_t *b, size_t n)
{
    (void)b;
    (void)n;
    collected_t *c = (collected_t *)user;
    if (c->count < MAX_TOKENS) c->ids[c->count] = id;
    c->count++;
    return 1;
}

static size_t greedy_len(void)
{
    return (size_t)(slm_golden_greedy_end - slm_golden_greedy_start);
}

static void tiny_load(int *idx)
{
    rust_slm_test_reset();
    *idx = rust_slm_load((const uint8_t *)"tiny-qwen2", slm_golden_tiny_start,
                         (size_t)(slm_golden_tiny_end - slm_golden_tiny_start));
    TEST_ASSERT_EQUAL_INT(0, *idx);
}

/* Greedy sampler, as the golden run. */
static void open_greedy(int idx, int32_t *out)
{
    *out = rust_slm_golden_open_greedy((uint32_t)idx, 64u);
    TEST_ASSERT_TRUE(*out >= 0);
}

/* --gpu on builds a program with the expected shape from the real GGUF. */
static void test_e2e_build(void)
{
    fake_up();
    int idx = -1;
    tiny_load(&idx);
    int32_t s = -1;
    open_greedy(idx, &s);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_configure((uint32_t)s, SLM_GPU_ON, 4u));
    SlmSessionGpuC g;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_gpu_info((uint32_t)s, &g));
    TEST_ASSERT_EQUAL_UINT32(SLM_PATH_PROGRAM, g.path);
    /* The embedding, two layers of 15, the final norm, the LM head. */
    TEST_ASSERT_EQUAL_UINT32(1u + 2u * 15u + 2u, g.launches);
    TEST_ASSERT_EQUAL_UINT32(64u, g.max_batch);
    /* Layer: 9 barriers inside, 1 before the next one; final norm and head. */
    TEST_ASSERT_TRUE(g.barriers > 15u);
    TEST_ASSERT_TRUE(g.barriers < g.launches);
    /* Model memory came from rust_slm_load: contiguous pages. */
    TEST_ASSERT_EQUAL_UINT32(1u, g.weights_big);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_close((uint32_t)s));
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx));
    rust_slm_test_reset();
    fake_down();
}

/* A hung GPU costs the bounded timeout once. The session then rebuilds its
 * KV cache on the CPU and produces llama.cpp's tokens anyway. */
static void test_e2e_hang_falls_back_to_cpu(void)
{
    uint8_t text[64];
    uint32_t want[MAX_TOKENS];
    int32_t text_len = rust_slm_golden_record(slm_golden_greedy_start, greedy_len(),
                                              "prompt.text", text, sizeof(text));
    TEST_ASSERT_TRUE(text_len > 0);
    int32_t wb = rust_slm_golden_record(slm_golden_greedy_start, greedy_len(),
                                        "greedy.tokens", (uint8_t *)want, sizeof(want));
    TEST_ASSERT_TRUE(wb > 0);
    uint32_t want_count = (uint32_t)wb / 4u;

    fake_up();
    int idx = -1;
    tiny_load(&idx);
    int32_t s = -1;
    open_greedy(idx, &s);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_configure((uint32_t)s, SLM_GPU_ON, 4u));
    g_fk.mode = FAKE_HANG;
    collected_t got = { .count = 0 };
    int32_t rc = rust_slm_prompt_ex((uint32_t)s, text, (size_t)text_len, SLM_PROMPT_RAW,
                                    want_count, collect_cb, NULL, &got);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(want_count, got.count);
    for (uint32_t i = 0; i < want_count; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i], got.ids[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.submits);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.dumps);
    SlmSessionGpuC g;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_gpu_info((uint32_t)s, &g));
    TEST_ASSERT_EQUAL_UINT32(SLM_PATH_CPU, g.path);
    TEST_ASSERT_EQUAL_STRING("GPU step timed out", g.note);
    TEST_ASSERT_EQUAL_UINT64(0, g.gpu_tokens);
    TEST_ASSERT_TRUE(g.cpu_tokens > 0u);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_close((uint32_t)s));
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx));
    rust_slm_test_reset();
    fake_down();
}

/* #1171 through the production FFI: the first step hangs and the backend
 * recovers the channel. The decoder builds the program again and the
 * session stays on the GPU: two opens, the dead one closed without a leak,
 * no CPU tokens. (The fake GPU computes nothing, so the tokens themselves
 * are checked by the Rust test with the software GPU.) */
static void test_e2e_hang_recovers_and_stays_on_the_gpu(void)
{
    uint8_t text[64];
    int32_t text_len = rust_slm_golden_record(slm_golden_greedy_start, greedy_len(),
                                              "prompt.text", text, sizeof(text));
    TEST_ASSERT_TRUE(text_len > 0);
    fake_up();
    int idx = -1;
    tiny_load(&idx);
    int32_t s = -1;
    open_greedy(idx, &s);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_configure((uint32_t)s, SLM_GPU_ON, 4u));
    g_fk.mode = FAKE_HANG;
    g_fk.hang_once = true;
    g_fk.recover_rc = 0;
    collected_t got = { .count = 0 };
    int32_t rc = rust_slm_prompt_ex((uint32_t)s, text, (size_t)text_len, SLM_PROMPT_RAW,
                                    4u, collect_cb, NULL, &got);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.recovers);
    TEST_ASSERT_EQUAL_UINT32(1, g_fk.dumps);
    TEST_ASSERT_TRUE(g_fk.submits >= 2u);
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_leaked_sessions());
    SlmSessionGpuC g;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_gpu_info((uint32_t)s, &g));
    TEST_ASSERT_EQUAL_UINT32(SLM_PATH_PROGRAM, g.path);
    TEST_ASSERT_EQUAL_UINT64(0, g.cpu_tokens);
    TEST_ASSERT_TRUE(g.gpu_tokens > 0u);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_close((uint32_t)s));
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx));
    rust_slm_test_reset();
    fake_down();
}

/* #1159 through the production FFI: an idle GPU session is closed while
 * another VM writer holds the lock past the close's wait. The close is
 * deferred and the program's pin is dropped, but its weights PTEs stay
 * until that writer unlocks. rust_slm_load copied the model into pages the
 * registry owns, which unload frees, so unload must refuse until then. */
static void test_e2e_unload_waits_for_a_deferred_close(void)
{
    fake_up();
    int idx = -1;
    tiny_load(&idx);
    int32_t s = -1;
    open_greedy(idx, &s);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_configure((uint32_t)s, SLM_GPU_ON, 4u));
    SlmSessionGpuC g;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_gpu_info((uint32_t)s, &g));
    TEST_ASSERT_EQUAL_UINT32(SLM_PATH_PROGRAM, g.path);
    TEST_ASSERT_EQUAL_INT(SLM_ERR_BUSY, rust_slm_unload((uint32_t)idx));

    TEST_ASSERT_EQUAL_INT(0, slm_prog_vm_lock());
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_close((uint32_t)s));
    int unload_rc = rust_slm_unload((uint32_t)idx);
    SlmModelInfoC info;
    int still_loaded = rust_slm_get_info((uint32_t)idx, &info);
    slm_prog_vm_unlock();
    TEST_ASSERT_EQUAL_INT(SLM_ERR_BUSY, unload_rc);
    TEST_ASSERT_EQUAL_INT(0, still_loaded);

    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx));
    TEST_ASSERT_EQUAL_UINT32(0, slm_prog_leaked_sessions());
    rust_slm_test_reset();
    fake_down();
}

/* The golden harness `slm gpu golden` runs on hardware: on the fake channel
 * the logits are zeros, so it must say so and where. */
static void test_e2e_golden_reports_divergence(void)
{
    fake_up();
    int idx = -1;
    tiny_load(&idx);
    SlmGpuGoldenReport r;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_gpu_golden((uint32_t)idx, slm_golden_greedy_start,
                                                 greedy_len(), 0u, &r));
    TEST_ASSERT_EQUAL_INT(0, r.first_bad_step);
    TEST_ASSERT_TRUE(r.steps > 0u);
    TEST_ASSERT_TRUE(r.max_err_milli > 1500);
    TEST_ASSERT_TRUE(r.exact_max_err_milli > 100);
    TEST_ASSERT_EQUAL_INT(0, r.exact_first_bad_step);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx));
    rust_slm_test_reset();
    fake_down();
}

int test_suite_slm_prog(void)
{
    UnityBegin("test_slm_prog.c");
    RUN_TEST(test_prog_pushbuffer);
    RUN_TEST(test_prog_barrier_kinds);
    RUN_TEST(test_prog_qmd_and_cbuf);
    RUN_TEST(test_prog_limits);
    RUN_TEST(test_prog_inject_hang);
    RUN_TEST(test_prog_profile_slots);
    RUN_TEST(test_encode_kernels);
    RUN_TEST(test_session_maps_and_frees);
    RUN_TEST(test_session_va_reuse_keeps_page_size);
    RUN_TEST(test_session_scattered_weights);
    RUN_TEST(test_session_open_failures);
    RUN_TEST(test_session_without_weights);
    RUN_TEST(test_run_ok_and_patch);
    RUN_TEST(test_prog_marks);
    RUN_TEST(test_unavailable_reason_names_the_cause);
    RUN_TEST(test_run_marked_uses_the_marked_entry);
    RUN_TEST(test_run_timeout_is_bounded);
    RUN_TEST(test_run_other_failures);
    RUN_TEST(test_dead_session_leaks_arena);
    RUN_TEST(test_timeout_with_recovery_frees_and_runs_again);
    RUN_TEST(test_recovery_after_evict_and_when_it_fails);
    RUN_TEST(test_two_sessions_never_overlap);
    RUN_TEST(test_cache_ops_never_skipped_while_another_step_runs);
    RUN_TEST(test_profile_reads_without_the_lock);
    RUN_TEST(test_deadline_starts_at_the_doorbell);
    RUN_TEST(test_prog_barrier_cap);
    RUN_TEST(test_kernel_lookup_and_batch);
    RUN_TEST(test_vm_oplib_write_during_program_build);
    RUN_TEST(test_vm_program_build_during_oplib_write);
    RUN_TEST(test_vm_lock_irqs_masked_tries_once);
    RUN_TEST(test_vm_writer_unlock_finishes_a_close);
    RUN_TEST(test_deferred_close_keeps_weights_mapped);
    RUN_TEST(test_tlb_failure_on_close_keeps_weights_mapped);
    RUN_TEST(test_e2e_build);
    RUN_TEST(test_e2e_hang_falls_back_to_cpu);
    RUN_TEST(test_e2e_hang_recovers_and_stays_on_the_gpu);
    RUN_TEST(test_e2e_unload_waits_for_a_deferred_close);
    RUN_TEST(test_e2e_golden_reports_divergence);
    return UnityEnd();
}
