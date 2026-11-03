/*
 * test_ga10b_grctx.c - GA10B GR context encoders (#1088).
 *
 * Expected register values come from the MMIO trace of nvgpu's first
 * power-on on jetson-nano-2 (~/slmos-ref/derivatives/nvgpu-traces/,
 * cold power-on trace): the golden-context commit of the global buffers
 * (lines 18154-18187) with nvgpu's VAs bundle CB 0x2000004000, pagepool
 * 0x2000020000, attribute CB 0x2000100000, RTV CB 0x2000200000, and the
 * FECS BIND_POINTER data for inst block 0x106b6c000 (line 17523).
 */
#include "unity.h"
#include "../gpu/nvidia/ga10b_grctx.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct reg_rec { uint32_t off, val; };

static struct reg_rec g_wr[64];
static unsigned g_nwr;

static uint32_t f_rd32(void *ctx, uint32_t off) { (void)ctx; (void)off; return 0; }
static void f_wr32(void *ctx, uint32_t off, uint32_t val)
{
    (void)ctx;
    if (g_nwr < sizeof(g_wr) / sizeof(g_wr[0])) {
        g_wr[g_nwr].off = off;
        g_wr[g_nwr].val = val;
    }
    g_nwr++;
}

static const struct ga10b_gpu_ops g_ops = { .rd32 = f_rd32, .wr32 = f_wr32 };

static const struct reg_rec trace_commit[GA10B_GR_GLOBAL_COMMIT_WRITES] = {
    { 0x40800cu, 0x20000200u }, { 0x408010u, 0x80000000u }, { 0x419004u, 0x20000200u },
    { 0x419008u, 0x00000000u }, { 0x408004u, 0x20000040u }, { 0x408008u, 0x80000030u },
    { 0x418e24u, 0x20000040u }, { 0x418e28u, 0x80000030u }, { 0x4064c8u, 0x01800480u },
    { 0x418810u, 0x82000100u }, { 0x419848u, 0x12000100u }, { 0x419c2cu, 0x12000100u },
    { 0x419c2cu, 0x12000100u }, { 0x419e00u, 0x02000100u }, { 0x419e04u, 0x80002000u },
    { 0x405830u, 0x00000800u }, { 0x40585cu, 0x00000800u }, { 0x4064c4u, 0x0200ffffu },
    { 0x5030c0u, 0x00001000u }, { 0x5030f4u, 0x00002000u }, { 0x5030f0u, 0x00001000u },
    { 0x5030e4u, 0x00001000u }, { 0x5030f8u, 0x00000000u }, { 0x418ea0u, 0x00001000u },
    { 0x5032c0u, 0x00001000u }, { 0x5032f4u, 0x00003000u }, { 0x5032f0u, 0x00001000u },
    { 0x5032e4u, 0x00001000u }, { 0x5032f8u, 0x00001000u }, { 0x418ea4u, 0x00001000u },
    { 0x408070u, 0x20002000u }, { 0x408074u, 0x00000800u }, { 0x419034u, 0x20002000u },
    { 0x408078u, 0x00000000u },
};

static void trace_vas(struct ga10b_gr_global_vas *v)
{
    memset(v, 0, sizeof(*v));
    v->va[GA10B_GR_BUF_BUNDLE_CB] = 0x2000004000ull;
    v->va[GA10B_GR_BUF_PAGEPOOL] = 0x2000020000ull;
    v->va[GA10B_GR_BUF_ATTRIB_CB] = 0x2000100000ull;
    v->va[GA10B_GR_BUF_RTV_CB] = 0x2000200000ull;
    v->va[GA10B_GR_BUF_PRIV_ACCESS_MAP] = 0x2000300000ull;
}

static void test_buffer_sizes(void)
{
    struct ga10b_gr_config c;
    ga10b_gr_config_default(&c);
    TEST_ASSERT_EQUAL_UINT32(1, c.gpc_count);
    TEST_ASSERT_EQUAL_UINT32(4, c.tpc_count);
    TEST_ASSERT_EQUAL_UINT32(8, c.max_tpc_count);
    TEST_ASSERT_EQUAL_HEX32(0x3000, ga10b_gr_global_buf_bytes(&c, GA10B_GR_BUF_BUNDLE_CB));
    TEST_ASSERT_EQUAL_HEX32(0x20000, ga10b_gr_global_buf_bytes(&c, GA10B_GR_BUF_PAGEPOOL));
    /* tex_rm_cb_1 0x2000 x 128 B (trace 18168). */
    TEST_ASSERT_EQUAL_HEX32(0x100000, ga10b_gr_global_buf_bytes(&c, GA10B_GR_BUF_ATTRIB_CB));
    TEST_ASSERT_EQUAL_HEX32(0x80000, ga10b_gr_global_buf_bytes(&c, GA10B_GR_BUF_RTV_CB));
    TEST_ASSERT_EQUAL_HEX32(0x80000, ga10b_gr_global_buf_bytes(&c, GA10B_GR_BUF_PRIV_ACCESS_MAP));
    TEST_ASSERT_EQUAL_HEX32(0, ga10b_gr_global_buf_bytes(&c, GA10B_GR_BUF_COUNT));
    /* (3 slots x 64 VEIDs + 2 + 512) -> align 512 = 1024, + 1024 = 2048
     * slots x 2 = 4096 bytes. */
    TEST_ASSERT_EQUAL_UINT32(4096, ga10b_gr_patch_ctx_bytes(&c));
    TEST_ASSERT_EQUAL_HEX32(0x300, GA10B_GR_FECS_HEADER_BYTES);
}

static void test_commit_global_matches_trace(void)
{
    struct ga10b_gr_config c;
    struct ga10b_gr_global_vas v;
    ga10b_gr_config_default(&c);
    trace_vas(&v);
    g_nwr = 0;
    struct ga10b_ctx_writer w = { .ops = &g_ops };
    ga10b_gr_commit_global_ctx_buffers(&w, &c, &v);
    TEST_ASSERT_EQUAL_UINT32(GA10B_GR_GLOBAL_COMMIT_WRITES, g_nwr);
    for (unsigned i = 0; i < GA10B_GR_GLOBAL_COMMIT_WRITES; i++) {
        TEST_ASSERT_EQUAL_HEX32(trace_commit[i].off, g_wr[i].off);
        TEST_ASSERT_EQUAL_HEX32(trace_commit[i].val, g_wr[i].val);
    }
}

static void test_commit_global_patch_mode(void)
{
    struct ga10b_gr_config c;
    struct ga10b_gr_global_vas v;
    ga10b_gr_config_default(&c);
    trace_vas(&v);
    static uint32_t patch[1024];
    memset(patch, 0, sizeof(patch));
    g_nwr = 0;
    struct ga10b_ctx_writer w = { .ops = &g_ops, .patch = patch, .patch_words = 1024 };
    ga10b_gr_commit_global_ctx_buffers(&w, &c, &v);
    TEST_ASSERT_EQUAL_UINT32(0, g_nwr);                 /* nothing reached BAR0 */
    TEST_ASSERT_EQUAL_UINT32(GA10B_GR_GLOBAL_COMMIT_WRITES, w.count);
    TEST_ASSERT_FALSE(w.overflow);
    for (unsigned i = 0; i < GA10B_GR_GLOBAL_COMMIT_WRITES; i++) {
        TEST_ASSERT_EQUAL_HEX32(trace_commit[i].off, patch[2 * i]);
        TEST_ASSERT_EQUAL_HEX32(trace_commit[i].val, patch[2 * i + 1]);
    }
    /* CB manager re-patch (SET_PREEMPTION_MODE) appends 15 entries. */
    ga10b_gr_commit_cb_manager(&w, &c);
    TEST_ASSERT_EQUAL_UINT32(GA10B_GR_GLOBAL_COMMIT_WRITES + GA10B_GR_CB_MANAGER_WRITES, w.count);
    TEST_ASSERT_EQUAL_HEX32(0x405830, patch[2 * 34]);
    TEST_ASSERT_EQUAL_HEX32(0x418ea4, patch[2 * 48]);
}

static void test_patch_overflow(void)
{
    /* nvgpu: slot = count * 2 must be <= words - 2. */
    uint32_t patch[6] = { 0 };
    struct ga10b_ctx_writer w = { .ops = &g_ops, .patch = patch, .patch_words = 6 };
    ga10b_ctx_write(&w, 1, 2);
    ga10b_ctx_write(&w, 3, 4);
    ga10b_ctx_write(&w, 5, 6);
    TEST_ASSERT_FALSE(w.overflow);
    ga10b_ctx_write(&w, 7, 8);
    TEST_ASSERT_TRUE(w.overflow);
    TEST_ASSERT_EQUAL_UINT32(3, w.count);
    TEST_ASSERT_EQUAL_HEX32(5, patch[4]);
}

static void test_priv_access_map(void)
{
    static uint8_t map[GA10B_GR_PRIV_ACCESS_MAP_BYTES];
    memset(map, 0xff, sizeof(map));
    ga10b_gr_build_priv_access_map(map, sizeof(map));
    unsigned bits = 0;
    for (size_t i = 0; i < sizeof(map); i++) {
        for (unsigned b = 0; b < 8; b++) bits += (map[i] >> b) & 1u;
    }
    TEST_ASSERT_EQUAL_UINT32(35, ga10b_gr_priv_access_regs_count);
    TEST_ASSERT_EQUAL_UINT32(35, bits);
    /* 0x418380 >> 2 = 0x1060e0: byte 0x20c1c, bit 0. */
    TEST_ASSERT_EQUAL_HEX8(0x01, map[0x20c1c]);
    /* 0x419eac >> 2 = 0x1067ab: byte 0x20cf5, bit 3; 0x419ea8 -> bit 2. */
    TEST_ASSERT_EQUAL_HEX8(0x0c, map[0x20cf5]);
}

static void test_ctx_header_layout(void)
{
    static uint8_t hdr[GA10B_GR_FECS_HEADER_BYTES];
    memset(hdr, 0xee, sizeof(hdr));
    ga10b_ctx_header_load(hdr, 0x2000300000ull, 0x2000081000ull, 0x2000400000ull);
    TEST_ASSERT_EQUAL_HEX32(0x21, ga10b_ctx_rd(hdr, 0x0c));
    TEST_ASSERT_EQUAL_HEX32(0x00081000, ga10b_ctx_rd(hdr, 0x14));
    TEST_ASSERT_EQUAL_HEX32(0x20, ga10b_ctx_rd(hdr, 0x18));
    TEST_ASSERT_EQUAL_HEX32(0, ga10b_ctx_rd(hdr, 0x20));
    TEST_ASSERT_EQUAL_HEX32(0, ga10b_ctx_rd(hdr, 0x60));
    TEST_ASSERT_EQUAL_HEX32(0x20, ga10b_ctx_rd(hdr, 0x78));
    TEST_ASSERT_EQUAL_HEX32(0x00400000, ga10b_ctx_rd(hdr, 0x7c));
    TEST_ASSERT_EQUAL_HEX32(0x00300000, ga10b_ctx_rd(hdr, 0xa4));
    TEST_ASSERT_EQUAL_HEX32(0x20, ga10b_ctx_rd(hdr, 0xa8));
    TEST_ASSERT_EQUAL_HEX32(0, ga10b_ctx_rd(hdr, 0x2c));
    TEST_ASSERT_EQUAL_HEX32(0, ga10b_ctx_rd(hdr, 0x94));
    TEST_ASSERT_EQUAL_HEX32(0xeeeeeeee, ga10b_ctx_rd(hdr, 0x10));   /* untouched */
}

static void test_golden_load_fields(void)
{
    static uint8_t img[0x200];
    memset(img, 0xff, sizeof(img));
    ga10b_ctx_image_after_golden_load(img, 0x2000300000ull, 51, 0x2000081000ull);
    TEST_ASSERT_EQUAL_UINT32(51, ga10b_ctx_rd(img, 0x10));
    TEST_ASSERT_EQUAL_HEX32(0x00081000, ga10b_ctx_rd(img, 0x14));
    TEST_ASSERT_EQUAL_HEX32(0x20, ga10b_ctx_rd(img, 0x18));
    TEST_ASSERT_EQUAL_HEX32(0x2, ga10b_ctx_rd(img, 0xa0));
    TEST_ASSERT_EQUAL_HEX32(0x00300000, ga10b_ctx_rd(img, 0xa4));
    TEST_ASSERT_EQUAL_HEX32(0xfffffff7, ga10b_ctx_rd(img, 0x3c));   /* verif off */
    TEST_ASSERT_EQUAL_HEX32(0xfffffff8, ga10b_ctx_rd(img, 0x28));   /* pm no_ctxsw */
    TEST_ASSERT_EQUAL_HEX32(0, ga10b_ctx_rd(img, 0x2c));
    for (uint32_t o = 0xd0; o <= 0xdc; o += 4) {
        TEST_ASSERT_EQUAL_HEX32(0, ga10b_ctx_rd(img, o));
    }
    TEST_ASSERT_EQUAL_HEX32(0, ga10b_ctx_rd(img, 0xf4));
    TEST_ASSERT_EQUAL_HEX32(0, ga10b_ctx_rd(img, 0xf8));
    TEST_ASSERT_EQUAL_HEX32(0xffffffff, ga10b_ctx_rd(img, 0x84));

    ga10b_ctx_image_set_compute_preempt(img, GA10B_PREEMPT_WFI);
    TEST_ASSERT_EQUAL_HEX32(0xffffffff, ga10b_ctx_rd(img, 0x84));   /* WFI writes nothing */
    ga10b_ctx_image_set_compute_preempt(img, GA10B_PREEMPT_CILP);
    TEST_ASSERT_EQUAL_HEX32(0x2, ga10b_ctx_rd(img, 0x84));
    ga10b_ctx_image_set_compute_preempt(img, GA10B_PREEMPT_CTA);
    TEST_ASSERT_EQUAL_HEX32(0x1, ga10b_ctx_rd(img, 0x84));
}

static void test_fecs_ctx_data_and_tables(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xb0106b6c, ga10b_fecs_current_ctx_data(0x106b6c000ull));
    TEST_ASSERT_EQUAL_UINT32(68, ga10b_grctx_fs_state_writes_count);
    TEST_ASSERT_EQUAL_HEX32(0x419bd8, ga10b_grctx_fs_state_writes[0].offset);
    TEST_ASSERT_EQUAL_HEX32(0x405bdc, ga10b_grctx_fs_state_writes[67].offset);
}

int test_suite_ga10b_grctx(void)
{
    UnityBegin("test_ga10b_grctx.c");
    RUN_TEST(test_buffer_sizes);
    RUN_TEST(test_commit_global_matches_trace);
    RUN_TEST(test_commit_global_patch_mode);
    RUN_TEST(test_patch_overflow);
    RUN_TEST(test_priv_access_map);
    RUN_TEST(test_ctx_header_layout);
    RUN_TEST(test_golden_load_fields);
    RUN_TEST(test_fecs_ctx_data_and_tables);
    return UnityEnd();
}
