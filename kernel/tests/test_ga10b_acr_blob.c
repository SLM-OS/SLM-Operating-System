/*
 * test_ga10b_acr_blob.c - GA10B non-WPR LS blob + flcn2_acr_desc
 * (#1088).
 *
 * Uses a synthetic ctxsw surface (both falcons: boot 0x40, code
 * 0x104, data 0x6 bytes; same layout as test_ga10b_ctxsw_ucode.c) and
 * synthetic 2248-byte PKC signature wrappers. Expected values are
 * derived by hand from acr_blob_construct.c (L4T R36.4.x):
 *
 *   wpr headers: 3 x 24 B (GPCCS, FECS, terminator)
 *   GPCCS lsb 0x0100, ucode 0x1000 (0x400 B), bl_data 0x1400
 *   FECS  lsb 0x1500, ucode 0x2000 (0x400 B), bl_data 0x2400
 *   wpr_size 0x2500
 *
 * With the LS PMU (include_pmu, the L4T configuration) a synthetic
 * next-core descriptor (bootloader 0x100+0xf0, params 0x10, ELF
 * 0x300) and a 0x1000-byte image give:
 *
 *   wpr headers: 4 x 24 B (PMU_NEXT_CORE, GPCCS, FECS, terminator)
 *   PMU   lsb 0x0100, ucode 0x1000 (0x1000 B), bl_data 0x2000,
 *         +0x8000 reserved (PMU_NVRISCV_WPR_RSVD_BYTES)
 *   GPCCS lsb 0xa100, ucode 0xb000, bl_data 0xb400
 *   FECS  lsb 0xb500, ucode 0xc000, bl_data 0xc400
 *   wpr_size 0xc500
 *
 * The real firmware is checked by `make test-ga10b-acr`.
 */
#include "unity.h"
#include "../gpu/nvidia/ga10b_acr_blob.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint8_t a_fw_fecs[16 + 0x40];
static uint8_t a_fw_gpccs[16 + 0x40];
static uint8_t a_inst[0x104];
static uint8_t a_data[6];
static uint8_t a_surface[0x800];
static uint8_t a_sig_fecs[2248];
static uint8_t a_sig_gpccs[2248];
static alignas(8) uint8_t a_blob[0x3000];
static alignas(8) uint8_t a_pblob[0xd000];
static uint8_t a_pmu_img[0x1000];
static uint8_t a_pmu_desc[76];
static uint8_t a_sig_pmu[2248];
static struct ga10b_ctxsw_ucode_info a_info;
static struct ga10b_acr_blob_layout a_lay;

#define WPR_BASE 0x27d000000ull

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void make_sig(uint8_t *s, uint32_t file_falcon_id, uint32_t version, uint8_t fill)
{
    for (size_t i = 0; i < 2248; i++)
        s[i] = (uint8_t)(fill + i);
    s[0] = 1; s[1] = 0; s[2] = 2; s[3] = 0;   /* identifier 1, version 2 */
    put32(s + 4, 2248);
    put32(s + 8, file_falcon_id);
    put32(s + 8 + 2168, version);              /* ls_ucode_version */
}

static void make_inputs(struct ga10b_acr_blob_inputs *in)
{
    struct ga10b_ctxsw_ucode_inputs u;
    memset(&u, 0, sizeof(u));
    put32(a_fw_fecs + 4, 0x40);  put32(a_fw_fecs + 8, 0x9e00);  put32(a_fw_fecs + 12, 0x9e00);
    put32(a_fw_gpccs + 4, 0x40); put32(a_fw_gpccs + 8, 0x3800); put32(a_fw_gpccs + 12, 0x3800);
    for (size_t i = 0; i < 0x40; i++) {
        a_fw_fecs[16 + i] = (uint8_t)(0x10 + i);
        a_fw_gpccs[16 + i] = (uint8_t)(0x90 + i);
    }
    for (size_t i = 0; i < sizeof(a_inst); i++)
        a_inst[i] = (uint8_t)(0xC0 ^ i);
    for (size_t i = 0; i < sizeof(a_data); i++)
        a_data[i] = (uint8_t)(0x30 + i);
    u.fecs_fw = a_fw_fecs;   u.fecs_fw_size = sizeof(a_fw_fecs);
    u.gpccs_fw = a_fw_gpccs; u.gpccs_fw_size = sizeof(a_fw_gpccs);
    u.fecs_inst = a_inst;  u.fecs_inst_size = sizeof(a_inst);
    u.fecs_data = a_data;  u.fecs_data_size = sizeof(a_data);
    u.gpccs_inst = a_inst; u.gpccs_inst_size = sizeof(a_inst);
    u.gpccs_data = a_data; u.gpccs_data_size = sizeof(a_data);
    TEST_ASSERT_EQUAL_INT(GA10B_CTXSW_OK,
        ga10b_ctxsw_ucode_build(&u, a_surface, sizeof(a_surface), &a_info));

    /* The sig files carry falcon_id already; nvgpu overwrites it anyway. */
    make_sig(a_sig_fecs, 0x77, 5, 0x01);
    make_sig(a_sig_gpccs, 0x66, 9, 0x41);

    memset(in, 0, sizeof(*in));
    in->surface = a_surface;
    in->surface_size = a_info.surface_size;
    in->ucode = &a_info;
    in->fecs_sig = a_sig_fecs;   in->fecs_sig_size = sizeof(a_sig_fecs);
    in->gpccs_sig = a_sig_gpccs; in->gpccs_sig_size = sizeof(a_sig_gpccs);
    in->wpr_base = WPR_BASE;
}

static void make_pmu_inputs(struct ga10b_acr_blob_inputs *in)
{
    make_inputs(in);
    memset(a_pmu_desc, 0, sizeof(a_pmu_desc));
    put32(a_pmu_desc + 0, 4);          /* version */
    put32(a_pmu_desc + 4, 0x100);      /* bootloader_offset */
    put32(a_pmu_desc + 8, 0xf0);       /* bootloader_size */
    put32(a_pmu_desc + 12, 0x1f0);     /* bootloader_param_offset */
    put32(a_pmu_desc + 16, 0x10);      /* bootloader_param_size */
    put32(a_pmu_desc + 20, 0x200);     /* next_core_elf_offset */
    put32(a_pmu_desc + 24, 0x300);     /* next_core_elf_size */
    put32(a_pmu_desc + 28, 0x01f774cc);/* app_version */
    put32(a_pmu_desc + 36, 0x80);      /* manifest_size */
    a_pmu_desc[56] = 1;                /* is_monitor_enabled */
    for (size_t i = 60; i < sizeof(a_pmu_desc); i++)
        a_pmu_desc[i] = 0xD5;          /* beyond the struct: never copied */
    for (size_t i = 0; i < sizeof(a_pmu_img); i++)
        a_pmu_img[i] = (uint8_t)(0x5A ^ (i * 7));
    make_sig(a_sig_pmu, 0x0d, 1, 0x81);
    in->include_pmu = true;
    in->pmu_image = a_pmu_img;  in->pmu_image_size = sizeof(a_pmu_img);
    in->pmu_desc = a_pmu_desc;  in->pmu_desc_size = sizeof(a_pmu_desc);
    in->pmu_sig = a_sig_pmu;    in->pmu_sig_size = sizeof(a_sig_pmu);
}

static void test_wpr_info_from_nano2_regs(void)
{
    struct ga10b_wpr_info w;
    TEST_ASSERT_TRUE(ga10b_acr_wpr_info_from_regs(0x027d0000u, 0x027d7e00u, &w));
    TEST_ASSERT_EQUAL_HEX64(0x27d000000ull, w.wpr_base);
    TEST_ASSERT_EQUAL_HEX64(0x7e0000ull, w.size);
    /* Low nibble is not part of the address field. */
    TEST_ASSERT_TRUE(ga10b_acr_wpr_info_from_regs(0x0000001fu, 0x0000002fu, &w));
    TEST_ASSERT_EQUAL_HEX64(0x1000ull, w.wpr_base);
    TEST_ASSERT_EQUAL_HEX64(0x1000ull, w.size);
    TEST_ASSERT_FALSE(ga10b_acr_wpr_info_from_regs(0x20u, 0x10u, &w));
    TEST_ASSERT_EQUAL_HEX64(0, w.wpr_base);
}

static void test_layout_order_and_offsets(void)
{
    struct ga10b_acr_blob_inputs in;
    make_inputs(&in);
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_OK, ga10b_acr_blob_layout(&in, &a_lay));
    TEST_ASSERT_EQUAL_UINT32(2, a_lay.managed_flcn_cnt);
    TEST_ASSERT_EQUAL_UINT32(GA10B_FALCON_ID_GPCCS, a_lay.flcn[0].falcon_id);
    TEST_ASSERT_EQUAL_UINT32(GA10B_FALCON_ID_FECS, a_lay.flcn[1].falcon_id);
    TEST_ASSERT_EQUAL_HEX32(0x30, a_lay.terminator_offset);

    const struct ga10b_acr_blob_falcon *g = &a_lay.flcn[0];
    const struct ga10b_acr_blob_falcon *f = &a_lay.flcn[1];
    TEST_ASSERT_EQUAL_HEX32(0x0100, g->wpr_header.lsb_offset);
    TEST_ASSERT_EQUAL_HEX32(0x1000, g->lsb.ucode_off);
    TEST_ASSERT_EQUAL_HEX32(0x1400, g->lsb.bl_data_off);
    TEST_ASSERT_EQUAL_HEX32(0x0100, g->lsb.bl_data_size);
    TEST_ASSERT_EQUAL_HEX32(0x1500, f->wpr_header.lsb_offset);
    TEST_ASSERT_EQUAL_HEX32(0x2000, f->lsb.ucode_off);
    TEST_ASSERT_EQUAL_HEX32(0x2400, f->lsb.bl_data_off);
    TEST_ASSERT_EQUAL_HEX32(0x2500, a_lay.wpr_size);
    TEST_ASSERT_EQUAL_HEX32(0x500, f->full_ucode_size);
}

static void test_ucode_desc_fecs_vs_gpccs(void)
{
    struct ga10b_acr_blob_inputs in;
    make_inputs(&in);
    ga10b_acr_blob_layout(&in, &a_lay);
    const struct ga10b_acr_blob_falcon *g = &a_lay.flcn[0];
    const struct ga10b_acr_blob_falcon *f = &a_lay.flcn[1];

    /* FECS: unaligned resident sizes, data offset from the surface. */
    TEST_ASSERT_EQUAL_HEX32(0x400, f->desc.image_size);
    TEST_ASSERT_EQUAL_HEX32(0x300, f->desc.app_size);
    TEST_ASSERT_EQUAL_HEX32(0x100, f->desc.app_start_offset);
    TEST_ASSERT_EQUAL_HEX32(0x104, f->desc.app_resident_code_size);
    TEST_ASSERT_EQUAL_HEX32(0x200, f->desc.app_resident_data_offset);
    TEST_ASSERT_EQUAL_HEX32(0x008, f->desc.app_resident_data_size);
    TEST_ASSERT_EQUAL_HEX32(0x000, f->src_offset);
    TEST_ASSERT_EQUAL_HEX32(0x400, f->data_size);

    /* GPCCS: everything 256-aligned, image starts at its boot segment. */
    TEST_ASSERT_EQUAL_HEX32(0x100, g->desc.app_start_offset);
    TEST_ASSERT_EQUAL_HEX32(0x200, g->desc.app_resident_code_size);
    TEST_ASSERT_EQUAL_HEX32(0x200, g->desc.app_resident_data_offset);
    TEST_ASSERT_EQUAL_HEX32(0x100, g->desc.app_resident_data_size);
    TEST_ASSERT_EQUAL_HEX32(0x400, g->src_offset);
    TEST_ASSERT_EQUAL_HEX32(0x400, g->data_size);
    TEST_ASSERT_EQUAL_HEX32(0x3800, g->desc.bootloader_imem_offset);
}

static void test_lsb_fields_and_flags(void)
{
    struct ga10b_acr_blob_inputs in;
    make_inputs(&in);
    ga10b_acr_blob_layout(&in, &a_lay);
    const struct ga10b_acr_blob_falcon *g = &a_lay.flcn[0];
    const struct ga10b_acr_blob_falcon *f = &a_lay.flcn[1];

    TEST_ASSERT_EQUAL_HEX32(0x100, f->lsb.bl_code_size);
    TEST_ASSERT_EQUAL_HEX32(0x300, f->lsb.ucode_size);
    TEST_ASSERT_EQUAL_HEX32(0x100, f->lsb.data_size);
    TEST_ASSERT_EQUAL_HEX32(0x100, f->lsb.app_code_off);
    TEST_ASSERT_EQUAL_HEX32(0x200, f->lsb.app_code_size);
    TEST_ASSERT_EQUAL_HEX32(0x300, f->lsb.app_data_off);
    TEST_ASSERT_EQUAL_HEX32(0x100, f->lsb.app_data_size);
    TEST_ASSERT_EQUAL_HEX32(0x9e00, f->lsb.bl_imem_off);
    TEST_ASSERT_EQUAL_HEX32(0, f->lsb.flags);                 /* FECS: no priv load */
    TEST_ASSERT_EQUAL_HEX32(GA10B_NV_FLCN_ACR_LSF_FLAG_FORCE_PRIV_LOAD_TRUE, g->lsb.flags);

    TEST_ASSERT_EQUAL_UINT32(GA10B_FALCON_ID_GSPLITE, f->wpr_header.bootstrap_owner);
    TEST_ASSERT_EQUAL_UINT32(0, f->wpr_header.lazy_bootstrap);
    TEST_ASSERT_EQUAL_UINT32(GA10B_LSF_IMAGE_STATUS_COPY, f->wpr_header.status);
    TEST_ASSERT_EQUAL_UINT32(5, f->wpr_header.bin_version);
    TEST_ASSERT_EQUAL_UINT32(9, g->wpr_header.bin_version);
}

static void test_bl_dmem_desc_uses_wpr_addresses(void)
{
    struct ga10b_acr_blob_inputs in;
    make_inputs(&in);
    ga10b_acr_blob_layout(&in, &a_lay);
    const struct ga10b_flcn_bl_dmem_desc *fb = &a_lay.flcn[1].bl_desc;
    const struct ga10b_flcn_bl_dmem_desc *gb = &a_lay.flcn[0].bl_desc;

    uint64_t fcode = WPR_BASE + 0x2000 + 0x100;
    TEST_ASSERT_EQUAL_HEX32((uint32_t)fcode, fb->code_dma_base.lo);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(fcode >> 32), fb->code_dma_base.hi);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(fcode + 0x200), fb->data_dma_base.lo);
    TEST_ASSERT_EQUAL_HEX32(0x104, fb->non_sec_code_size);
    TEST_ASSERT_EQUAL_HEX32(0x008, fb->data_size);
    TEST_ASSERT_EQUAL_UINT32(1, fb->argc);
    TEST_ASSERT_EQUAL_UINT32(0, fb->argv);
    TEST_ASSERT_EQUAL_UINT32(0, fb->ctx_dma);

    uint64_t gcode = WPR_BASE + 0x1000 + 0x100;
    TEST_ASSERT_EQUAL_HEX32((uint32_t)gcode, gb->code_dma_base.lo);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(gcode + 0x200), gb->data_dma_base.lo);
    TEST_ASSERT_EQUAL_HEX32(0x200, gb->non_sec_code_size);
    TEST_ASSERT_EQUAL_HEX32(0x100, gb->data_size);
}

static void test_build_blob_bytes(void)
{
    struct ga10b_acr_blob_inputs in;
    make_inputs(&in);
    memset(a_blob, 0xEE, sizeof(a_blob));
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_OK,
        ga10b_acr_blob_build(&in, a_blob, sizeof(a_blob), &a_lay));

    /* WPR header array + terminator. */
    TEST_ASSERT_EQUAL_HEX32(GA10B_FALCON_ID_GPCCS, get32(a_blob + 0));
    TEST_ASSERT_EQUAL_HEX32(0x100, get32(a_blob + 4));
    TEST_ASSERT_EQUAL_HEX32(GA10B_FALCON_ID_FECS, get32(a_blob + 24));
    TEST_ASSERT_EQUAL_HEX32(0x1500, get32(a_blob + 28));
    TEST_ASSERT_EQUAL_HEX32(GA10B_FALCON_ID_INVALID, get32(a_blob + 48));
    for (uint32_t i = 52; i < 72; i += 4)
        TEST_ASSERT_EQUAL_HEX32(0, get32(a_blob + i));
    TEST_ASSERT_EQUAL_UINT8(0, a_blob[72]);            /* zero gap */

    /* FECS LSB: generic header copied, falcon_id overwritten, rest of
     * the signature copied verbatim, fields after it. */
    const uint8_t *lsb = a_blob + 0x1500;
    TEST_ASSERT_EQUAL_MEMORY(a_sig_fecs, lsb, 8);
    TEST_ASSERT_EQUAL_HEX32(GA10B_FALCON_ID_FECS, get32(lsb + 8));
    TEST_ASSERT_EQUAL_MEMORY(a_sig_fecs + 12, lsb + 12, 2248 - 12);
    TEST_ASSERT_EQUAL_HEX32(0x2000, get32(lsb + 2248));       /* ucode_off */
    TEST_ASSERT_EQUAL_HEX32(0x2400, get32(lsb + 2268));       /* bl_data_off */
    TEST_ASSERT_EQUAL_HEX32(0, get32(lsb + 2292));            /* flags */
    TEST_ASSERT_EQUAL_HEX32(GA10B_FALCON_ID_GPCCS, get32(a_blob + 0x100 + 8));
    TEST_ASSERT_EQUAL_HEX32(8, get32(a_blob + 0x100 + 2292));

    /* Ucode images. */
    TEST_ASSERT_EQUAL_MEMORY(a_surface + 0x000, a_blob + 0x2000, 0x400);
    TEST_ASSERT_EQUAL_MEMORY(a_surface + 0x400, a_blob + 0x1000, 0x400);

    /* bl desc: 84 bytes written, the rest of its 256 B slot zero. */
    TEST_ASSERT_EQUAL_MEMORY(&a_lay.flcn[1].bl_desc, a_blob + 0x2400, 84);
    TEST_ASSERT_EQUAL_UINT8(0, a_blob[0x2400 + 84]);
    TEST_ASSERT_EQUAL_UINT8(0, a_blob[0x24FF]);
    TEST_ASSERT_EQUAL_UINT8(0xEE, a_blob[0x2500]);     /* past wpr_size untouched */
}

static void test_short_sig_is_zero_padded(void)
{
    struct ga10b_acr_blob_inputs in;
    make_inputs(&in);
    in.fecs_sig_size = 100;  /* nvgpu copies min(size, sizeof wrapper) */
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_OK,
        ga10b_acr_blob_build(&in, a_blob, sizeof(a_blob), &a_lay));
    TEST_ASSERT_EQUAL_MEMORY(a_sig_fecs + 12, a_blob + 0x1500 + 12, 100 - 12);
    TEST_ASSERT_EQUAL_UINT8(0, a_blob[0x1500 + 100]);
    TEST_ASSERT_EQUAL_UINT32(0, a_lay.flcn[1].wpr_header.bin_version);
}

static void test_build_rejects_bad_input(void)
{
    struct ga10b_acr_blob_inputs in;
    make_inputs(&in);
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_ERR_BUF_SMALL,
        ga10b_acr_blob_build(&in, a_blob, 0x24FF, &a_lay));
    TEST_ASSERT_EQUAL_HEX32(0x2500, a_lay.wpr_size);
    in.surface_size = 0x7FF;  /* GPCCS image reads to 0x800 */
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_ERR_SURFACE, ga10b_acr_blob_layout(&in, &a_lay));
    make_inputs(&in);
    in.gpccs_sig = nullptr;
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_ERR_ARG, ga10b_acr_blob_layout(&in, &a_lay));
}

static void test_flcn2_acr_desc(void)
{
    struct ga10b_flcn2_acr_desc d;
    memset(&d, 0xA5, sizeof(d));
    ga10b_acr_build_flcn2_desc(&d, 0x123456000ull, 0x12100u);
    TEST_ASSERT_EQUAL_UINT32(0, d.wpr_region_id);
    TEST_ASSERT_EQUAL_UINT32(0, d.wpr_offset);
    TEST_ASSERT_EQUAL_UINT32(1, d.regions.no_regions);
    TEST_ASSERT_EQUAL_UINT32(0, d.regions.region_props[0].start_addr);
    TEST_ASSERT_EQUAL_UINT32(0, d.regions.region_props[1].client_mask);
    TEST_ASSERT_EQUAL_HEX32(0x12100, d.nonwpr_ucode_blob_size);
    TEST_ASSERT_EQUAL_HEX64(0x123456000ull, d.nonwpr_ucode_blob_start);
    TEST_ASSERT_EQUAL_HEX64(0, d.ls_pmu_desc);
    TEST_ASSERT_EQUAL_HEX32(0, d.gpu_mode);
    const uint8_t *raw = (const uint8_t *)&d;
    TEST_ASSERT_EQUAL_HEX32(0x12100, get32(raw + 68));
    TEST_ASSERT_EQUAL_HEX32(0x23456000, get32(raw + 72));
    TEST_ASSERT_EQUAL_HEX32(0x1, get32(raw + 76));
    TEST_ASSERT_EQUAL_HEX32(0, get32(raw + 92));    /* tail padding zeroed */
}

static void test_pmu_layout_order_and_offsets(void)
{
    struct ga10b_acr_blob_inputs in;
    make_pmu_inputs(&in);
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_OK, ga10b_acr_blob_layout(&in, &a_lay));
    TEST_ASSERT_EQUAL_UINT32(3, a_lay.managed_flcn_cnt);
    TEST_ASSERT_EQUAL_UINT32(GA10B_FALCON_ID_PMU_NEXT_CORE, a_lay.flcn[0].falcon_id);
    TEST_ASSERT_EQUAL_UINT32(GA10B_FALCON_ID_GPCCS, a_lay.flcn[1].falcon_id);
    TEST_ASSERT_EQUAL_UINT32(GA10B_FALCON_ID_FECS, a_lay.flcn[2].falcon_id);
    TEST_ASSERT_EQUAL_HEX32(0x48, a_lay.terminator_offset);

    const struct ga10b_acr_blob_falcon *p = &a_lay.flcn[0];
    TEST_ASSERT_TRUE(p->is_next_core);
    TEST_ASSERT_EQUAL_HEX32(0x0100, p->wpr_header.lsb_offset);
    TEST_ASSERT_EQUAL_HEX32(0x1000, p->lsb.ucode_off);
    TEST_ASSERT_EQUAL_HEX32(0x1000, p->data_size);       /* whole image file */
    TEST_ASSERT_EQUAL_HEX32(0x2000, p->lsb.bl_data_off);
    TEST_ASSERT_EQUAL_HEX32(0x0100, p->lsb.bl_data_size);
    TEST_ASSERT_EQUAL_HEX32(0x1100, p->full_ucode_size);  /* reserve not counted */
    TEST_ASSERT_EQUAL_HEX32(0xa100, a_lay.flcn[1].wpr_header.lsb_offset);
    TEST_ASSERT_EQUAL_HEX32(0xb000, a_lay.flcn[1].lsb.ucode_off);
    TEST_ASSERT_EQUAL_HEX32(0xb500, a_lay.flcn[2].wpr_header.lsb_offset);
    TEST_ASSERT_EQUAL_HEX32(0xc000, a_lay.flcn[2].lsb.ucode_off);
    TEST_ASSERT_EQUAL_HEX32(0xc500, a_lay.wpr_size);
}

static void test_pmu_lsb_fields_and_lazy_bits(void)
{
    struct ga10b_acr_blob_inputs in;
    make_pmu_inputs(&in);
    ga10b_acr_blob_layout(&in, &a_lay);
    const struct ga10b_acr_blob_falcon *p = &a_lay.flcn[0];

    /* ucode_size = bl_off + bl_size + param_size; data_size = elf size
     * padded so ucode_size + data_size is 4 KB aligned. */
    TEST_ASSERT_EQUAL_HEX32(0x200, p->lsb.ucode_size);
    TEST_ASSERT_EQUAL_HEX32(0xe00, p->lsb.data_size);
    TEST_ASSERT_EQUAL_HEX32(0, p->lsb.bl_code_size);
    TEST_ASSERT_EQUAL_HEX32(0, p->lsb.bl_imem_off);
    TEST_ASSERT_EQUAL_HEX32(0, p->lsb.app_code_off);
    TEST_ASSERT_EQUAL_HEX32(0, p->lsb.app_code_size);
    TEST_ASSERT_EQUAL_HEX32(0, p->lsb.app_data_off);
    TEST_ASSERT_EQUAL_HEX32(0, p->lsb.app_data_size);
    TEST_ASSERT_EQUAL_HEX32(0, p->lsb.flags);
    TEST_ASSERT_EQUAL_UINT32(0, p->wpr_header.lazy_bootstrap);
    TEST_ASSERT_EQUAL_UINT32(GA10B_FALCON_ID_GSPLITE, p->wpr_header.bootstrap_owner);
    TEST_ASSERT_EQUAL_UINT32(1, p->wpr_header.bin_version);
    TEST_ASSERT_EQUAL_HEX32(0x01f774cc, p->ndesc.app_version);
    TEST_ASSERT_EQUAL_UINT8(1, p->ndesc.is_monitor_enabled);

    /* With the LS PMU, FECS and GPCCS are lazily bootstrapped. */
    TEST_ASSERT_EQUAL_UINT32(1, a_lay.flcn[1].wpr_header.lazy_bootstrap);
    TEST_ASSERT_EQUAL_UINT32(1, a_lay.flcn[2].wpr_header.lazy_bootstrap);
    TEST_ASSERT_EQUAL_HEX32(GA10B_NV_FLCN_ACR_LSF_FLAG_FORCE_PRIV_LOAD_TRUE,
                            a_lay.flcn[1].lsb.flags);
    TEST_ASSERT_EQUAL_HEX32(0, a_lay.flcn[2].lsb.flags);

    /* PMU has no BL desc; the ctxsw falcons' descs follow their new
     * WPR offsets. */
    TEST_ASSERT_EQUAL_HEX32(0, p->bl_desc.argc);
    TEST_ASSERT_EQUAL_HEX32(0, p->bl_desc.code_dma_base.lo);
    uint64_t gcode = WPR_BASE + 0xb000 + 0x100;
    TEST_ASSERT_EQUAL_HEX32((uint32_t)gcode, a_lay.flcn[1].bl_desc.code_dma_base.lo);
    uint64_t fcode = WPR_BASE + 0xc000 + 0x100;
    TEST_ASSERT_EQUAL_HEX32((uint32_t)fcode, a_lay.flcn[2].bl_desc.code_dma_base.lo);
}

static void test_pmu_blob_bytes(void)
{
    struct ga10b_acr_blob_inputs in;
    make_pmu_inputs(&in);
    memset(a_pblob, 0xEE, sizeof(a_pblob));
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_OK,
        ga10b_acr_blob_build(&in, a_pblob, sizeof(a_pblob), &a_lay));

    TEST_ASSERT_EQUAL_HEX32(GA10B_FALCON_ID_PMU_NEXT_CORE, get32(a_pblob + 0));
    TEST_ASSERT_EQUAL_HEX32(0x100, get32(a_pblob + 4));
    TEST_ASSERT_EQUAL_HEX32(0, get32(a_pblob + 12));            /* lazy */
    TEST_ASSERT_EQUAL_HEX32(GA10B_FALCON_ID_GPCCS, get32(a_pblob + 24));
    TEST_ASSERT_EQUAL_HEX32(1, get32(a_pblob + 24 + 12));       /* lazy */
    TEST_ASSERT_EQUAL_HEX32(GA10B_FALCON_ID_FECS, get32(a_pblob + 48));
    TEST_ASSERT_EQUAL_HEX32(GA10B_FALCON_ID_INVALID, get32(a_pblob + 72));

    const uint8_t *lsb = a_pblob + 0x100;
    TEST_ASSERT_EQUAL_MEMORY(a_sig_pmu, lsb, 8);
    TEST_ASSERT_EQUAL_HEX32(GA10B_FALCON_ID_PMU_NEXT_CORE, get32(lsb + 8));
    TEST_ASSERT_EQUAL_MEMORY(a_sig_pmu + 12, lsb + 12, 2248 - 12);
    TEST_ASSERT_EQUAL_HEX32(0x1000, get32(lsb + 2248));        /* ucode_off */
    TEST_ASSERT_EQUAL_HEX32(0x200, get32(lsb + 2252));         /* ucode_size */
    TEST_ASSERT_EQUAL_HEX32(0xe00, get32(lsb + 2256));         /* data_size */
    TEST_ASSERT_EQUAL_HEX32(0x2000, get32(lsb + 2268));        /* bl_data_off */
    TEST_ASSERT_EQUAL_HEX32(0x100, get32(lsb + 2272));         /* bl_data_size */

    TEST_ASSERT_EQUAL_MEMORY(a_pmu_img, a_pblob + 0x1000, sizeof(a_pmu_img));
    /* No BL desc for the PMU and the reserve stays zero. */
    for (uint32_t i = 0x2000; i < 0xa100; i += 0x100)
        TEST_ASSERT_EQUAL_UINT8(0, a_pblob[i]);
    TEST_ASSERT_EQUAL_MEMORY(a_surface + 0x400, a_pblob + 0xb000, 0x400);
    TEST_ASSERT_EQUAL_MEMORY(a_surface + 0x000, a_pblob + 0xc000, 0x400);
    TEST_ASSERT_EQUAL_MEMORY(&a_lay.flcn[2].bl_desc, a_pblob + 0xc400, 84);
    TEST_ASSERT_EQUAL_UINT8(0xEE, a_pblob[0xc500]);
}

static void test_pmu_rejects_bad_input(void)
{
    struct ga10b_acr_blob_inputs in;
    make_pmu_inputs(&in);
    in.pmu_desc_size = 59;
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_ERR_ARG, ga10b_acr_blob_layout(&in, &a_lay));
    make_pmu_inputs(&in);
    in.pmu_image = nullptr;
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_ERR_ARG, ga10b_acr_blob_layout(&in, &a_lay));
    make_pmu_inputs(&in);
    in.pmu_sig_size = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_ERR_ARG, ga10b_acr_blob_layout(&in, &a_lay));
    /* include_pmu = false ignores the PMU fields entirely. */
    make_pmu_inputs(&in);
    in.include_pmu = false;
    in.pmu_image = nullptr;
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_OK, ga10b_acr_blob_layout(&in, &a_lay));
    TEST_ASSERT_EQUAL_UINT32(2, a_lay.managed_flcn_cnt);
    TEST_ASSERT_EQUAL_UINT32(0, a_lay.flcn[0].wpr_header.lazy_bootstrap);
}

static void test_flcn2_acr_desc_ls_pmu(void)
{
    struct ga10b_flcn2_acr_desc d;
    memset(&d, 0xA5, sizeof(d));
    ga10b_acr_build_flcn2_desc_ls_pmu(&d, 0x123456000ull, 0xc500u, 0x10761e000ull);
    TEST_ASSERT_EQUAL_HEX64(0x10761e000ull, d.ls_pmu_desc);
    TEST_ASSERT_EQUAL_HEX64(0x123456000ull, d.nonwpr_ucode_blob_start);
    TEST_ASSERT_EQUAL_HEX32(0xc500, d.nonwpr_ucode_blob_size);
    TEST_ASSERT_EQUAL_UINT32(1, d.regions.no_regions);
    TEST_ASSERT_EQUAL_HEX32(0, d.gpu_mode);
    const uint8_t *raw = (const uint8_t *)&d;
    TEST_ASSERT_EQUAL_HEX32(0x0761e000, get32(raw + 80));
    TEST_ASSERT_EQUAL_HEX32(0x1, get32(raw + 84));

    uint8_t copy[64];
    memset(copy, 0x11, sizeof(copy));
    struct ga10b_acr_blob_inputs in;
    make_pmu_inputs(&in);
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_OK,
        ga10b_acr_copy_ls_pmu_desc(copy, sizeof(copy), a_pmu_desc, sizeof(a_pmu_desc)));
    TEST_ASSERT_EQUAL_MEMORY(a_pmu_desc, copy, 60);
    TEST_ASSERT_EQUAL_UINT8(0x11, copy[60]);                    /* only 60 bytes */
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_ERR_ARG,
        ga10b_acr_copy_ls_pmu_desc(copy, 59, a_pmu_desc, sizeof(a_pmu_desc)));
    TEST_ASSERT_EQUAL_INT(GA10B_ACR_ERR_ARG,
        ga10b_acr_copy_ls_pmu_desc(copy, sizeof(copy), a_pmu_desc, 59));
}

int test_suite_ga10b_acr_blob(void)
{
    UnityBegin("test_ga10b_acr_blob.c");
    RUN_TEST(test_wpr_info_from_nano2_regs);
    RUN_TEST(test_layout_order_and_offsets);
    RUN_TEST(test_ucode_desc_fecs_vs_gpccs);
    RUN_TEST(test_lsb_fields_and_flags);
    RUN_TEST(test_bl_dmem_desc_uses_wpr_addresses);
    RUN_TEST(test_build_blob_bytes);
    RUN_TEST(test_short_sig_is_zero_padded);
    RUN_TEST(test_build_rejects_bad_input);
    RUN_TEST(test_flcn2_acr_desc);
    RUN_TEST(test_pmu_layout_order_and_offsets);
    RUN_TEST(test_pmu_lsb_fields_and_lazy_bits);
    RUN_TEST(test_pmu_blob_bytes);
    RUN_TEST(test_pmu_rejects_bad_input);
    RUN_TEST(test_flcn2_acr_desc_ls_pmu);
    return UnityEnd();
}
