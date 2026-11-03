/*
 * test_ga10b_ctxsw_ucode.c - FECS/GPCCS ctxsw ucode surface (#1088).
 *
 * Synthetic firmware: a 16-byte nvgpu_ctxsw_bootloader_desc + boot
 * image and small inst/data lists whose sizes are deliberately not
 * multiples of 256 (and, for data, not of 4) so the 256-byte segment
 * alignment, the netlist count round-up and the zero padding are all
 * visible. Expected layout (gr_falcon.c:248-274):
 *
 *   FECS  boot 0x000/0x040  code 0x100/0x104  data 0x300/0x008
 *   GPCCS boot 0x400/0x040  code 0x500/0x104  data 0x700/0x008
 *   surface 0x800
 */
#include "unity.h"
#include "../gpu/nvidia/ga10b_ctxsw_ucode.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint8_t fw_fecs[16 + 0x40];
static uint8_t fw_gpccs[16 + 0x40];
static uint8_t inst_buf[0x104];
static uint8_t data_buf[6];
static uint8_t surf[0x900];

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void make_fw(uint8_t *fw, uint32_t boot_size, uint32_t imem, uint32_t entry,
                    uint8_t fill)
{
    put32(fw, 0);
    put32(fw + 4, boot_size);
    put32(fw + 8, imem);
    put32(fw + 12, entry);
    for (uint32_t i = 0; i < 0x40; i++)
        fw[16 + i] = (uint8_t)(fill + i);
}

static void make_inputs(struct ga10b_ctxsw_ucode_inputs *in)
{
    make_fw(fw_fecs, 0x40, 0x9e00, 0x9e04, 0x10);
    make_fw(fw_gpccs, 0x40, 0x3800, 0x3804, 0x80);
    for (uint32_t i = 0; i < sizeof(inst_buf); i++)
        inst_buf[i] = (uint8_t)(0xA0 ^ i);
    for (uint32_t i = 0; i < sizeof(data_buf); i++)
        data_buf[i] = (uint8_t)(0x50 + i);
    memset(in, 0, sizeof(*in));
    in->fecs_fw = fw_fecs;   in->fecs_fw_size = sizeof(fw_fecs);
    in->gpccs_fw = fw_gpccs; in->gpccs_fw_size = sizeof(fw_gpccs);
    in->fecs_inst = inst_buf;  in->fecs_inst_size = sizeof(inst_buf);
    in->fecs_data = data_buf;  in->fecs_data_size = sizeof(data_buf);
    in->gpccs_inst = inst_buf; in->gpccs_inst_size = sizeof(inst_buf);
    in->gpccs_data = data_buf; in->gpccs_data_size = sizeof(data_buf);
}

static void test_layout_segment_table(void)
{
    struct ga10b_ctxsw_ucode_inputs in;
    make_inputs(&in);
    struct ga10b_ctxsw_ucode_info info;
    TEST_ASSERT_EQUAL_INT(GA10B_CTXSW_OK, ga10b_ctxsw_ucode_layout(&in, &info));

    TEST_ASSERT_EQUAL_HEX32(0x9e04, info.fecs.boot_entry);
    TEST_ASSERT_EQUAL_HEX32(0x9e00, info.fecs.boot_imem_offset);
    TEST_ASSERT_EQUAL_HEX32(0x000, info.fecs.boot.offset);
    TEST_ASSERT_EQUAL_HEX32(0x040, info.fecs.boot.size);
    TEST_ASSERT_EQUAL_HEX32(0x100, info.fecs.code.offset);
    TEST_ASSERT_EQUAL_HEX32(0x104, info.fecs.code.size);
    TEST_ASSERT_EQUAL_HEX32(0x300, info.fecs.data.offset);
    TEST_ASSERT_EQUAL_HEX32(0x008, info.fecs.data.size);  /* 6 -> count 2 -> 8 */

    TEST_ASSERT_EQUAL_HEX32(0x3804, info.gpccs.boot_entry);
    TEST_ASSERT_EQUAL_HEX32(0x400, info.gpccs.boot.offset);
    TEST_ASSERT_EQUAL_HEX32(0x500, info.gpccs.code.offset);
    TEST_ASSERT_EQUAL_HEX32(0x700, info.gpccs.data.offset);
    TEST_ASSERT_EQUAL_HEX32(0x800, info.surface_size);
}

static void test_boot_signature_is_word_sum(void)
{
    struct ga10b_ctxsw_ucode_inputs in;
    make_inputs(&in);
    struct ga10b_ctxsw_ucode_info info;
    ga10b_ctxsw_ucode_layout(&in, &info);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 0x40; i += 4)
        sum += (uint32_t)fw_fecs[16 + i] | ((uint32_t)fw_fecs[17 + i] << 8) |
               ((uint32_t)fw_fecs[18 + i] << 16) | ((uint32_t)fw_fecs[19 + i] << 24);
    TEST_ASSERT_EQUAL_HEX32(sum, info.fecs.boot_signature);
}

static void test_build_writes_segments_and_zero_padding(void)
{
    struct ga10b_ctxsw_ucode_inputs in;
    make_inputs(&in);
    struct ga10b_ctxsw_ucode_info info;
    memset(surf, 0xEE, sizeof(surf));
    TEST_ASSERT_EQUAL_INT(GA10B_CTXSW_OK,
        ga10b_ctxsw_ucode_build(&in, surf, sizeof(surf), &info));

    TEST_ASSERT_EQUAL_MEMORY(fw_fecs + 16, surf + 0x000, 0x40);
    TEST_ASSERT_EQUAL_MEMORY(inst_buf, surf + 0x100, sizeof(inst_buf));
    TEST_ASSERT_EQUAL_MEMORY(data_buf, surf + 0x300, sizeof(data_buf));
    TEST_ASSERT_EQUAL_MEMORY(fw_gpccs + 16, surf + 0x400, 0x40);
    TEST_ASSERT_EQUAL_MEMORY(inst_buf, surf + 0x500, sizeof(inst_buf));
    /* Padding: after FECS boot, after FECS code, the partial data word. */
    TEST_ASSERT_EQUAL_UINT8(0, surf[0x40]);
    TEST_ASSERT_EQUAL_UINT8(0, surf[0xFF]);
    TEST_ASSERT_EQUAL_UINT8(0, surf[0x204]);
    TEST_ASSERT_EQUAL_UINT8(0, surf[0x306]);
    TEST_ASSERT_EQUAL_UINT8(0, surf[0x7FF]);
    /* Bytes past surface_size are not touched. */
    TEST_ASSERT_EQUAL_UINT8(0xEE, surf[0x800]);
}

static void test_build_rejects_small_buffer(void)
{
    struct ga10b_ctxsw_ucode_inputs in;
    make_inputs(&in);
    struct ga10b_ctxsw_ucode_info info;
    TEST_ASSERT_EQUAL_INT(GA10B_CTXSW_ERR_BUF_SMALL,
        ga10b_ctxsw_ucode_build(&in, surf, 0x7FF, &info));
    TEST_ASSERT_EQUAL_HEX32(0x800, info.surface_size);  /* size query still filled */
}

static void test_unaligned_boot_size_rounds_to_word(void)
{
    struct ga10b_ctxsw_ucode_inputs in;
    make_inputs(&in);
    put32(fw_fecs + 4, 0x3E);  /* ALIGN(0x3E, 4) = 0x40 still fits */
    struct ga10b_ctxsw_ucode_info info;
    TEST_ASSERT_EQUAL_INT(GA10B_CTXSW_OK, ga10b_ctxsw_ucode_layout(&in, &info));
    TEST_ASSERT_EQUAL_HEX32(0x40, info.fecs.boot.size);
}

static void test_rejects_short_firmware(void)
{
    struct ga10b_ctxsw_ucode_inputs in;
    make_inputs(&in);
    struct ga10b_ctxsw_ucode_info info;
    put32(fw_gpccs + 4, 0x41);  /* needs 0x44 boot bytes, file has 0x40 */
    TEST_ASSERT_EQUAL_INT(GA10B_CTXSW_ERR_FW_SIZE, ga10b_ctxsw_ucode_layout(&in, &info));
    TEST_ASSERT_EQUAL_UINT32(0, info.surface_size);
    make_inputs(&in);
    in.fecs_fw_size = 15;       /* shorter than the descriptor */
    TEST_ASSERT_EQUAL_INT(GA10B_CTXSW_ERR_FW_SIZE, ga10b_ctxsw_ucode_layout(&in, &info));
    make_inputs(&in);
    in.fecs_data = nullptr;
    TEST_ASSERT_EQUAL_INT(GA10B_CTXSW_ERR_ARG, ga10b_ctxsw_ucode_layout(&in, &info));
}

int test_suite_ga10b_ctxsw_ucode(void)
{
    UnityBegin("test_ga10b_ctxsw_ucode.c");
    RUN_TEST(test_layout_segment_table);
    RUN_TEST(test_boot_signature_is_word_sum);
    RUN_TEST(test_build_writes_segments_and_zero_padding);
    RUN_TEST(test_build_rejects_small_buffer);
    RUN_TEST(test_unaligned_boot_size_rounds_to_word);
    RUN_TEST(test_rejects_short_firmware);
    return UnityEnd();
}
