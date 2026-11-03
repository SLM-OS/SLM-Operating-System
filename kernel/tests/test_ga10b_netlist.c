/*
 * test_ga10b_netlist.c - GA10B NET?_img container parser (#1088).
 *
 * Synthetic images only; the real NETA..NETD files are exercised by
 * the host test (make test-ga10b-acr). Covers the header/region-table
 * layout, nvgpu's count rules (u32 lists round up, av/av64/aiv lists
 * truncate), last-occurrence-wins lookup, alignment and bounds
 * rejection, and the silicon image-selection rule.
 */
#include "unity.h"
#include "../gpu/nvidia/ga10b_netlist.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static alignas(8) uint8_t nl_buf[512];

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

struct reg_spec { uint32_t id, size, offset; };

/* Header + region table at `base`; returns bytes used by the table. */
static size_t put_table(uint8_t *base, const struct reg_spec *r, uint32_t n)
{
    put32(base, 0);      /* version */
    put32(base + 4, n);  /* regions */
    for (uint32_t i = 0; i < n; i++) {
        put32(base + 8 + i * 12, r[i].id);
        put32(base + 8 + i * 12 + 4, r[i].size);
        put32(base + 8 + i * 12 + 8, r[i].offset);
    }
    return 8 + (size_t)n * 12;
}

/* Image with: FECS_UCODE_INST (6 bytes), SW_BUNDLE_INIT (20 bytes =
 * 2.5 av), MAJORV (4), SW_CTX_LOAD (24 bytes = 2 aiv),
 * SW_BUNDLE64_INIT (12 = 1 av64). Data starts at 0x80. */
static size_t build_basic(void)
{
    memset(nl_buf, 0, sizeof(nl_buf));
    const struct reg_spec r[] = {
        { GA10B_NETLIST_REGIONID_FECS_UCODE_INST, 6, 0x80 },
        { GA10B_NETLIST_REGIONID_SW_BUNDLE_INIT, 20, 0x88 },
        { GA10B_NETLIST_REGIONID_MAJORV, 4, 0xA0 },
        { GA10B_NETLIST_REGIONID_SW_CTX_LOAD, 24, 0xA8 },
        { GA10B_NETLIST_REGIONID_SW_BUNDLE64_INIT, 12, 0xC0 },
    };
    put_table(nl_buf, r, 5);
    put32(nl_buf + 0x80, 0x11223344);
    nl_buf[0x84] = 0x55; nl_buf[0x85] = 0x66;
    put32(nl_buf + 0x88, 0x1000); put32(nl_buf + 0x8C, 2);
    put32(nl_buf + 0x90, 0x2000); put32(nl_buf + 0x94, 3);
    put32(nl_buf + 0xA0, 0xC0FFEE01);
    put32(nl_buf + 0xA8, 0x404014); put32(nl_buf + 0xAC, 7); put32(nl_buf + 0xB0, 9);
    put32(nl_buf + 0xC0, 0x5000); put32(nl_buf + 0xC4, 0xAAAA); put32(nl_buf + 0xC8, 0xBBBB);
    return 0xD0;
}

static void test_open_reads_header_and_table(void)
{
    size_t n = build_basic();
    struct ga10b_netlist nl;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK, ga10b_netlist_open(&nl, nl_buf, n));
    TEST_ASSERT_EQUAL_UINT32(0, nl.version);
    TEST_ASSERT_EQUAL_UINT32(5, nl.region_count);

    struct ga10b_netlist_region r;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK, ga10b_netlist_region_at(&nl, 2, &r));
    TEST_ASSERT_EQUAL_UINT32(GA10B_NETLIST_REGIONID_MAJORV, r.region_id);
    TEST_ASSERT_EQUAL_UINT32(4, r.data_size);
    TEST_ASSERT_EQUAL_HEX32(0xA0, r.data_offset);
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_ARG, ga10b_netlist_region_at(&nl, 5, &r));
}

static void test_find_points_into_blob(void)
{
    size_t n = build_basic();
    struct ga10b_netlist nl;
    ga10b_netlist_open(&nl, nl_buf, n);
    struct ga10b_netlist_blob_view v;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK,
        ga10b_netlist_find(&nl, GA10B_NETLIST_REGIONID_SW_BUNDLE_INIT, &v));
    TEST_ASSERT_EQUAL_PTR(nl_buf + 0x88, v.data);
    TEST_ASSERT_EQUAL_UINT32(20, v.size);
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_NOT_FOUND,
        ga10b_netlist_find(&nl, GA10B_NETLIST_REGIONID_GPCCS_UCODE_INST, &v));
}

static void test_u32_list_count_rounds_up(void)
{
    size_t n = build_basic();
    struct ga10b_netlist nl;
    ga10b_netlist_open(&nl, nl_buf, n);
    struct ga10b_netlist_u32_view u;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK,
        ga10b_netlist_get_u32_list(&nl, GA10B_NETLIST_REGIONID_FECS_UCODE_INST, &u));
    TEST_ASSERT_EQUAL_UINT32(2, u.count);   /* (6 + 3) / 4, netlist.c:85-87 */
    TEST_ASSERT_EQUAL_UINT32(6, u.size);
    TEST_ASSERT_EQUAL_HEX32(0x11223344, u.l[0]);
}

static void test_av_aiv_av64_counts_truncate(void)
{
    size_t n = build_basic();
    struct ga10b_netlist nl;
    ga10b_netlist_open(&nl, nl_buf, n);

    struct ga10b_netlist_av_view av;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK,
        ga10b_netlist_get_av_list(&nl, GA10B_NETLIST_REGIONID_SW_BUNDLE_INIT, &av));
    TEST_ASSERT_EQUAL_UINT32(2, av.count);  /* 20 / 8 */
    TEST_ASSERT_EQUAL_HEX32(0x2000, av.l[1].addr);
    TEST_ASSERT_EQUAL_UINT32(3, av.l[1].value);

    struct ga10b_netlist_aiv_view aiv;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK,
        ga10b_netlist_get_aiv_list(&nl, GA10B_NETLIST_REGIONID_SW_CTX_LOAD, &aiv));
    TEST_ASSERT_EQUAL_UINT32(2, aiv.count);
    TEST_ASSERT_EQUAL_HEX32(0x404014, aiv.l[0].addr);
    TEST_ASSERT_EQUAL_UINT32(7, aiv.l[0].index);
    TEST_ASSERT_EQUAL_UINT32(9, aiv.l[0].value);

    struct ga10b_netlist_av64_view av64;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK,
        ga10b_netlist_get_av64_list(&nl, GA10B_NETLIST_REGIONID_SW_BUNDLE64_INIT, &av64));
    TEST_ASSERT_EQUAL_UINT32(1, av64.count);
    TEST_ASSERT_EQUAL_HEX32(0xAAAA, av64.l[0].value_lo);
    TEST_ASSERT_EQUAL_HEX32(0xBBBB, av64.l[0].value_hi);
}

static void test_scalar_region(void)
{
    size_t n = build_basic();
    struct ga10b_netlist nl;
    ga10b_netlist_open(&nl, nl_buf, n);
    uint32_t v = 0;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK,
        ga10b_netlist_get_u32(&nl, GA10B_NETLIST_REGIONID_MAJORV, &v));
    TEST_ASSERT_EQUAL_HEX32(0xC0FFEE01, v);
    /* FECS_UCODE_INST is 6 bytes: reads the first word fine. A 2-byte
     * region is rejected (nvgpu would read past it). */
    put32(nl_buf + 8 + 2 * 12 + 4, 2);
    ga10b_netlist_open(&nl, nl_buf, n);
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_SIZE,
        ga10b_netlist_get_u32(&nl, GA10B_NETLIST_REGIONID_MAJORV, &v));
}

static void test_duplicate_id_last_wins(void)
{
    memset(nl_buf, 0, sizeof(nl_buf));
    const struct reg_spec r[] = {
        { GA10B_NETLIST_REGIONID_BUFFER_SIZE, 4, 0x40 },
        { GA10B_NETLIST_REGIONID_BUFFER_SIZE, 4, 0x44 },
    };
    put_table(nl_buf, r, 2);
    put32(nl_buf + 0x40, 1);
    put32(nl_buf + 0x44, 2);
    struct ga10b_netlist nl;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK, ga10b_netlist_open(&nl, nl_buf, 0x48));
    uint32_t v = 0;
    ga10b_netlist_get_u32(&nl, GA10B_NETLIST_REGIONID_BUFFER_SIZE, &v);
    TEST_ASSERT_EQUAL_UINT32(2, v);
}

static void test_open_rejects_bad_input(void)
{
    struct ga10b_netlist nl;
    size_t n = build_basic();
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_ARG, ga10b_netlist_open(nullptr, nl_buf, n));
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_ARG, ga10b_netlist_open(&nl, nullptr, n));
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_TRUNCATED, ga10b_netlist_open(&nl, nl_buf, 7));
    /* Table (8 + 5*12 = 68 bytes) does not fit in 60. */
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_TRUNCATED, ga10b_netlist_open(&nl, nl_buf, 60));
    TEST_ASSERT_NULL(nl.data);
    /* Last region ends at 0xCC: one byte short. */
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_BOUNDS, ga10b_netlist_open(&nl, nl_buf, 0xCB));
    /* Offset + size wrapping 32 bits must not pass. */
    put32(nl_buf + 8 + 8, 0xFFFFFFF0u);
    put32(nl_buf + 8 + 4, 0x20u);
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_BOUNDS, ga10b_netlist_open(&nl, nl_buf, n));
    /* A huge region count must not overflow the table-size check. */
    put32(nl_buf + 4, 0xFFFFFFFFu);
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_TRUNCATED, ga10b_netlist_open(&nl, nl_buf, n));
}

static void test_typed_view_rejects_misaligned(void)
{
    memset(nl_buf, 0, sizeof(nl_buf));
    const struct reg_spec r[] = {
        { GA10B_NETLIST_REGIONID_SW_METHOD_INIT, 8, 0x22 },
    };
    put_table(nl_buf, r, 1);
    struct ga10b_netlist nl;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK, ga10b_netlist_open(&nl, nl_buf, 0x40));
    struct ga10b_netlist_av_view av;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_ERR_ALIGN,
        ga10b_netlist_get_av_list(&nl, GA10B_NETLIST_REGIONID_SW_METHOD_INIT, &av));
    TEST_ASSERT_EQUAL_UINT32(0, av.count);
    /* The raw view still works. */
    struct ga10b_netlist_blob_view v;
    TEST_ASSERT_EQUAL_INT(GA10B_NETLIST_OK,
        ga10b_netlist_find(&nl, GA10B_NETLIST_REGIONID_SW_METHOD_INIT, &v));
}

static void test_silicon_selection_rule(void)
{
    /* NETLIST_FINAL (what GA10B silicon uses) skips the MAJORV check;
     * slots must match the FECS major revision. */
    TEST_ASSERT_TRUE(ga10b_netlist_is_valid(GA10B_NETLIST_FINAL, 0, 5));
    TEST_ASSERT_FALSE(ga10b_netlist_is_valid(GA10B_NETLIST_SLOT_C, 0, 5));
    TEST_ASSERT_TRUE(ga10b_netlist_is_valid(GA10B_NETLIST_SLOT_A, 5, 5));
    TEST_ASSERT_EQUAL_STRING("NETC_img_prod_encrypted.bin",
                             ga10b_netlist_silicon_image_name(false));
    TEST_ASSERT_EQUAL_STRING("NETC_img_debug_encrypted.bin",
                             ga10b_netlist_silicon_image_name(true));
    TEST_ASSERT_EQUAL_STRING("MAJORV", ga10b_netlist_region_name(15));
}

int test_suite_ga10b_netlist(void)
{
    UnityBegin("test_ga10b_netlist.c");
    RUN_TEST(test_open_reads_header_and_table);
    RUN_TEST(test_find_points_into_blob);
    RUN_TEST(test_u32_list_count_rounds_up);
    RUN_TEST(test_av_aiv_av64_counts_truncate);
    RUN_TEST(test_scalar_region);
    RUN_TEST(test_duplicate_id_last_wins);
    RUN_TEST(test_open_rejects_bad_input);
    RUN_TEST(test_typed_view_rejects_misaligned);
    RUN_TEST(test_silicon_selection_rule);
    return UnityEnd();
}
