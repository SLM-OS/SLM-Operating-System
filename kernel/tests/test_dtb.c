/*
 * test_dtb.c - DTB parser regression tests
 *
 * Covers:
 *   - dtb_validate() bounds handling against crafted headers
 *     (BOOT-H1 in docs/code-review-2026-04-12.md)
 *   - /memreserve/ parsing — both standard FDT-header reserve map and
 *     non-standard root-node `memreserve` property (Pi convention)
 *   - /chosen entropy + bootloader metadata parsing
 *
 * DTBs are hand-rolled in-memory; no `dtc` dependency.
 */

#include "unity.h"
#include "../include/dtb.h"
#include <stdint.h>
#include <string.h>

/* Build a DTB header in-place using big-endian stores (DTB is always BE). */
static inline uint32_t cpu_to_be32(uint32_t v)
{
    return ((v & 0xFFU) << 24) |
           ((v & 0xFF00U) << 8) |
           ((v & 0xFF0000U) >> 8) |
           ((v & 0xFF000000U) >> 24);
}

static void fill_header(struct fdt_header *hdr,
                        uint32_t magic,
                        uint32_t version,
                        uint32_t off_dt_struct,
                        uint32_t size_dt_struct,
                        uint32_t totalsize)
{
    hdr->magic             = cpu_to_be32(magic);
    hdr->totalsize         = cpu_to_be32(totalsize);
    hdr->off_dt_struct     = cpu_to_be32(off_dt_struct);
    hdr->off_dt_strings    = cpu_to_be32(0);
    hdr->off_mem_rsvmap    = cpu_to_be32(0);
    hdr->version           = cpu_to_be32(version);
    hdr->last_comp_version = cpu_to_be32(version);
    hdr->boot_cpuid_phys   = cpu_to_be32(0);
    hdr->size_dt_strings   = cpu_to_be32(0);
    hdr->size_dt_struct    = cpu_to_be32(size_dt_struct);
}

static void test_dtb_validate_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(FDT_ERR_BADPTR, dtb_validate(NULL));
}

static void test_dtb_validate_rejects_bad_magic(void)
{
    struct fdt_header hdr;
    fill_header(&hdr, 0xDEADBEEFU, FDT_VERSION, 64, 16, 1024);
    TEST_ASSERT_EQUAL_INT(FDT_ERR_BADMAGIC, dtb_validate(&hdr));
}

static void test_dtb_validate_rejects_old_version(void)
{
    struct fdt_header hdr;
    fill_header(&hdr, FDT_MAGIC, FDT_VERSION - 1, 64, 16, 1024);
    TEST_ASSERT_EQUAL_INT(FDT_ERR_BADVERSION, dtb_validate(&hdr));
}

static void test_dtb_validate_accepts_well_formed(void)
{
    struct fdt_header hdr;
    fill_header(&hdr, FDT_MAGIC, FDT_VERSION, 64, 128, 512);
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_validate(&hdr));
}

/* BOOT-H1 regression: struct block larger than totalsize must be rejected. */
static void test_dtb_validate_rejects_struct_past_total(void)
{
    struct fdt_header hdr;
    fill_header(&hdr, FDT_MAGIC, FDT_VERSION,
                /*off*/ 64,
                /*size*/ 1024,
                /*total*/ 512);
    TEST_ASSERT_EQUAL_INT(FDT_ERR_BADSTRUCT, dtb_validate(&hdr));
}

/* BOOT-H1 regression: off + size overflow must be rejected. */
static void test_dtb_validate_rejects_overflow(void)
{
    struct fdt_header hdr;
    fill_header(&hdr, FDT_MAGIC, FDT_VERSION,
                /*off*/ 0xFFFFFFF0U,
                /*size*/ 0x00000100U,
                /*total*/ 0xFFFFFFFFU);
    TEST_ASSERT_EQUAL_INT(FDT_ERR_BADSTRUCT, dtb_validate(&hdr));
}

/* ============================================================================
 * /memreserve/ + /chosen parsing — exercises parse_memreserves_from_header,
 * parse_memreserves_from_root_property, and parse_chosen (all static in
 * dtb.c, called via dtb_parse and verified through the public getters).
 * ============================================================================ */

struct test_dtb_spec {
    /* /memreserve/ header reserve map (each = u64 addr, u64 size) */
    int      n_rsvmap_entries;
    uint64_t rsvmap[4][2];

    /* memreserve property on root node (each = u32 addr, u32 size) */
    int      n_root_memreserve_pairs;
    uint32_t root_memreserve[4][2];

    /* /chosen entropy */
    const uint8_t *chosen_rng_seed;
    uint32_t       chosen_rng_seed_len;
    const uint8_t *chosen_kaslr_seed;
    uint32_t       chosen_kaslr_seed_len;

    /* /chosen/bootloader */
    const char *chosen_bootloader_version;
    uint32_t    chosen_bootloader_caps;
    uint32_t    chosen_bootloader_build_ts;
    uint32_t    chosen_bootloader_update_ts;
};

static void emit_u32_be(uint8_t *buf, uint32_t *off, uint32_t v)
{
    buf[*off + 0] = (v >> 24) & 0xFF;
    buf[*off + 1] = (v >> 16) & 0xFF;
    buf[*off + 2] = (v >>  8) & 0xFF;
    buf[*off + 3] = (v >>  0) & 0xFF;
    *off += 4;
}

static void emit_u64_be(uint8_t *buf, uint32_t *off, uint64_t v)
{
    emit_u32_be(buf, off, (uint32_t)(v >> 32));
    emit_u32_be(buf, off, (uint32_t) v);
}

static void emit_str_aligned4(uint8_t *buf, uint32_t *off, const char *s)
{
    size_t len = strlen(s) + 1;
    memcpy(buf + *off, s, len);
    *off += len;
    while (*off % 4) buf[(*off)++] = 0;
}

static void emit_bytes_aligned4(uint8_t *buf, uint32_t *off,
                                const uint8_t *data, uint32_t len)
{
    memcpy(buf + *off, data, len);
    *off += len;
    while (*off % 4) buf[(*off)++] = 0;
}

static uint32_t intern_string(uint8_t *buf, uint32_t strings_base,
                              uint32_t *strings_off, const char *s)
{
    uint32_t name_off = *strings_off - strings_base;
    size_t len = strlen(s) + 1;
    memcpy(buf + *strings_off, s, len);
    *strings_off += len;
    return name_off;
}

static void emit_prop(uint8_t *buf, uint32_t *struct_off,
                      uint32_t name_off,
                      const uint8_t *data, uint32_t data_len)
{
    emit_u32_be(buf, struct_off, FDT_PROP);
    emit_u32_be(buf, struct_off, data_len);
    emit_u32_be(buf, struct_off, name_off);
    emit_bytes_aligned4(buf, struct_off, data, data_len);
}

static uint32_t make_test_dtb(uint8_t *buf, uint32_t buf_size,
                              const struct test_dtb_spec *spec)
{
    memset(buf, 0, buf_size);

    const uint32_t off_rsvmap  = 64;
    const uint32_t off_strings = off_rsvmap + (8 + 1) * 16;
    const uint32_t off_struct  = off_strings + 256;
    uint32_t strings_off = off_strings;
    uint32_t struct_off  = off_struct;

    /* Reserve map */
    {
        uint32_t o = off_rsvmap;
        for (int i = 0; i < spec->n_rsvmap_entries; i++) {
            emit_u64_be(buf, &o, spec->rsvmap[i][0]);
            emit_u64_be(buf, &o, spec->rsvmap[i][1]);
        }
        emit_u64_be(buf, &o, 0);   /* terminator */
        emit_u64_be(buf, &o, 0);
    }

    /* Pre-intern string offsets */
    uint32_t s_memreserve = 0, s_rng_seed = 0, s_kaslr_seed = 0;
    uint32_t s_version = 0, s_caps = 0, s_build_ts = 0, s_update_ts = 0;
    if (spec->n_root_memreserve_pairs > 0)
        s_memreserve = intern_string(buf, off_strings, &strings_off, "memreserve");
    if (spec->chosen_rng_seed_len > 0)
        s_rng_seed   = intern_string(buf, off_strings, &strings_off, "rng-seed");
    if (spec->chosen_kaslr_seed_len > 0)
        s_kaslr_seed = intern_string(buf, off_strings, &strings_off, "kaslr-seed");
    if (spec->chosen_bootloader_version) {
        s_version    = intern_string(buf, off_strings, &strings_off, "version");
        s_caps       = intern_string(buf, off_strings, &strings_off, "capabilities");
        s_build_ts   = intern_string(buf, off_strings, &strings_off, "build-timestamp");
        s_update_ts  = intern_string(buf, off_strings, &strings_off, "update-timestamp");
    }

    /* / { ... } */
    emit_u32_be(buf, &struct_off, FDT_BEGIN_NODE);
    emit_str_aligned4(buf, &struct_off, "");

    /* Root memreserve property (Pi convention, u32 cells) */
    if (spec->n_root_memreserve_pairs > 0) {
        uint32_t plen = (uint32_t)spec->n_root_memreserve_pairs * 8;
        emit_u32_be(buf, &struct_off, FDT_PROP);
        emit_u32_be(buf, &struct_off, plen);
        emit_u32_be(buf, &struct_off, s_memreserve);
        for (int i = 0; i < spec->n_root_memreserve_pairs; i++) {
            emit_u32_be(buf, &struct_off, spec->root_memreserve[i][0]);
            emit_u32_be(buf, &struct_off, spec->root_memreserve[i][1]);
        }
    }

    bool emit_chosen = (spec->chosen_rng_seed_len > 0)    ||
                       (spec->chosen_kaslr_seed_len > 0)  ||
                       (spec->chosen_bootloader_version);
    if (emit_chosen) {
        emit_u32_be(buf, &struct_off, FDT_BEGIN_NODE);
        emit_str_aligned4(buf, &struct_off, "chosen");

        if (spec->chosen_rng_seed_len > 0)
            emit_prop(buf, &struct_off, s_rng_seed,
                      spec->chosen_rng_seed, spec->chosen_rng_seed_len);
        if (spec->chosen_kaslr_seed_len > 0)
            emit_prop(buf, &struct_off, s_kaslr_seed,
                      spec->chosen_kaslr_seed, spec->chosen_kaslr_seed_len);

        if (spec->chosen_bootloader_version) {
            emit_u32_be(buf, &struct_off, FDT_BEGIN_NODE);
            emit_str_aligned4(buf, &struct_off, "bootloader");

            const char *v = spec->chosen_bootloader_version;
            emit_prop(buf, &struct_off, s_version,
                      (const uint8_t *)v, (uint32_t)strlen(v) + 1);

            uint8_t buf32[4];
            #define PUT_U32_BE(out, val) do { \
                (out)[0] = ((val) >> 24) & 0xFF; (out)[1] = ((val) >> 16) & 0xFF; \
                (out)[2] = ((val) >>  8) & 0xFF; (out)[3] = ((val) >>  0) & 0xFF; \
            } while (0)
            PUT_U32_BE(buf32, spec->chosen_bootloader_caps);
            emit_prop(buf, &struct_off, s_caps, buf32, 4);
            PUT_U32_BE(buf32, spec->chosen_bootloader_build_ts);
            emit_prop(buf, &struct_off, s_build_ts, buf32, 4);
            PUT_U32_BE(buf32, spec->chosen_bootloader_update_ts);
            emit_prop(buf, &struct_off, s_update_ts, buf32, 4);
            #undef PUT_U32_BE

            emit_u32_be(buf, &struct_off, FDT_END_NODE);  /* /chosen/bootloader */
        }

        emit_u32_be(buf, &struct_off, FDT_END_NODE);      /* /chosen */
    }

    emit_u32_be(buf, &struct_off, FDT_END_NODE);          /* / */
    emit_u32_be(buf, &struct_off, FDT_END);

    uint32_t struct_size  = struct_off  - off_struct;
    uint32_t strings_size = strings_off - off_strings;
    uint32_t total_size   = struct_off;

    struct fdt_header *hdr = (struct fdt_header *)buf;
    hdr->magic             = cpu_to_be32(FDT_MAGIC);
    hdr->totalsize         = cpu_to_be32(total_size);
    hdr->off_dt_struct     = cpu_to_be32(off_struct);
    hdr->off_dt_strings    = cpu_to_be32(off_strings);
    hdr->off_mem_rsvmap    = cpu_to_be32(off_rsvmap);
    hdr->version           = cpu_to_be32(FDT_VERSION);
    hdr->last_comp_version = cpu_to_be32(FDT_VERSION);
    hdr->boot_cpuid_phys   = cpu_to_be32(0);
    hdr->size_dt_strings   = cpu_to_be32(strings_size);
    hdr->size_dt_struct    = cpu_to_be32(struct_size);

    return total_size;
}

static uint8_t g_test_dtb[2048];

static void test_memreserve_header_single_entry(void)
{
    struct test_dtb_spec spec = {0};
    spec.n_rsvmap_entries = 1;
    spec.rsvmap[0][0] = 0x3fc00000;
    spec.rsvmap[0][1] = 0x00400000;
    make_test_dtb(g_test_dtb, sizeof(g_test_dtb), &spec);

    fdt_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_parse(g_test_dtb, &info));

    dtb_memreserve_t out[4];
    int n = dtb_get_memreserves(out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT64(0x3fc00000, out[0].addr);
    TEST_ASSERT_EQUAL_UINT64(0x00400000, out[0].size);
}

static void test_memreserve_header_multiple_entries(void)
{
    struct test_dtb_spec spec = {0};
    spec.n_rsvmap_entries = 3;
    spec.rsvmap[0][0] = 0x10000;     spec.rsvmap[0][1] = 0x1000;
    spec.rsvmap[1][0] = 0x100000;    spec.rsvmap[1][1] = 0x4000;
    spec.rsvmap[2][0] = 0x40000000;  spec.rsvmap[2][1] = 0x800000;
    make_test_dtb(g_test_dtb, sizeof(g_test_dtb), &spec);

    fdt_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_parse(g_test_dtb, &info));

    dtb_memreserve_t out[4];
    int n = dtb_get_memreserves(out, 4);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_UINT64(0x10000,    out[0].addr);
    TEST_ASSERT_EQUAL_UINT64(0x100000,   out[1].addr);
    TEST_ASSERT_EQUAL_UINT64(0x40000000, out[2].addr);
    TEST_ASSERT_EQUAL_UINT64(0x800000,   out[2].size);
}

static void test_memreserve_root_property_pi_convention(void)
{
    struct test_dtb_spec spec = {0};
    spec.n_root_memreserve_pairs = 1;
    spec.root_memreserve[0][0] = 0x3fc00000;
    spec.root_memreserve[0][1] = 0x00400000;
    make_test_dtb(g_test_dtb, sizeof(g_test_dtb), &spec);

    fdt_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_parse(g_test_dtb, &info));

    dtb_memreserve_t out[4];
    int n = dtb_get_memreserves(out, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT64(0x3fc00000, out[0].addr);
    TEST_ASSERT_EQUAL_UINT64(0x00400000, out[0].size);
}

static void test_memreserve_both_encodings_concatenated(void)
{
    struct test_dtb_spec spec = {0};
    spec.n_rsvmap_entries = 1;
    spec.rsvmap[0][0] = 0x10000;
    spec.rsvmap[0][1] = 0x1000;
    spec.n_root_memreserve_pairs = 1;
    spec.root_memreserve[0][0] = 0x3fc00000;
    spec.root_memreserve[0][1] = 0x400000;
    make_test_dtb(g_test_dtb, sizeof(g_test_dtb), &spec);

    fdt_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_parse(g_test_dtb, &info));

    dtb_memreserve_t out[4];
    int n = dtb_get_memreserves(out, 4);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_UINT64(0x10000,    out[0].addr);
    TEST_ASSERT_EQUAL_UINT64(0x3fc00000, out[1].addr);
}

static void test_memreserve_absent_returns_zero(void)
{
    struct test_dtb_spec spec = {0};
    make_test_dtb(g_test_dtb, sizeof(g_test_dtb), &spec);

    fdt_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_parse(g_test_dtb, &info));

    dtb_memreserve_t out[4];
    int n = dtb_get_memreserves(out, 4);
    TEST_ASSERT_EQUAL_INT(0, n);
}

static void test_chosen_rng_and_kaslr_seed(void)
{
    static const uint8_t rng[8]   = { 1,2,3,4,5,6,7,8 };
    static const uint8_t kaslr[16]= { 9,10,11,12,13,14,15,16,
                                      17,18,19,20,21,22,23,24 };
    struct test_dtb_spec spec = {0};
    spec.chosen_rng_seed     = rng;
    spec.chosen_rng_seed_len = sizeof(rng);
    spec.chosen_kaslr_seed     = kaslr;
    spec.chosen_kaslr_seed_len = sizeof(kaslr);
    make_test_dtb(g_test_dtb, sizeof(g_test_dtb), &spec);

    fdt_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_parse(g_test_dtb, &info));

    const dtb_chosen_t *ch = dtb_get_chosen();
    TEST_ASSERT_EQUAL_UINT32(8,  ch->rng_seed_len);
    TEST_ASSERT_EQUAL_UINT32(16, ch->kaslr_seed_len);
    TEST_ASSERT_EQUAL_UINT8(0x01, ch->rng_seed[0]);
    TEST_ASSERT_EQUAL_UINT8(0x08, ch->rng_seed[7]);
    TEST_ASSERT_EQUAL_UINT8(0x09, ch->kaslr_seed[0]);
    TEST_ASSERT_EQUAL_UINT8(0x18, ch->kaslr_seed[15]);
    TEST_ASSERT_EQUAL_STRING("", ch->bootloader_version);
}

static void test_chosen_bootloader_metadata(void)
{
    struct test_dtb_spec spec = {0};
    spec.chosen_bootloader_version    = "2682625908f5585e5f4832bb82e74e7d757ec48f";
    spec.chosen_bootloader_caps       = 0x7f;
    spec.chosen_bootloader_build_ts   = 0x66f16700;
    spec.chosen_bootloader_update_ts  = 0x69e94a26;
    make_test_dtb(g_test_dtb, sizeof(g_test_dtb), &spec);

    fdt_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_parse(g_test_dtb, &info));

    const dtb_chosen_t *ch = dtb_get_chosen();
    TEST_ASSERT_EQUAL_STRING("2682625908f5585e5f4832bb82e74e7d757ec48f",
                             ch->bootloader_version);
    TEST_ASSERT_EQUAL_UINT32(0x7fU,        ch->bootloader_capabilities);
    TEST_ASSERT_EQUAL_UINT32(0x66f16700U,  ch->bootloader_build_timestamp);
    TEST_ASSERT_EQUAL_UINT32(0x69e94a26U,  ch->bootloader_update_timestamp);
}

static void test_chosen_absent_returns_zero(void)
{
    struct test_dtb_spec spec = {0};
    make_test_dtb(g_test_dtb, sizeof(g_test_dtb), &spec);

    fdt_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_parse(g_test_dtb, &info));

    const dtb_chosen_t *ch = dtb_get_chosen();
    TEST_ASSERT_EQUAL_UINT32(0, ch->rng_seed_len);
    TEST_ASSERT_EQUAL_UINT32(0, ch->kaslr_seed_len);
    TEST_ASSERT_EQUAL_STRING("", ch->bootloader_version);
    TEST_ASSERT_EQUAL_UINT32(0, ch->bootloader_capabilities);
}

static void test_chosen_long_bootloader_version_truncated(void)
{
    char long_version[200];
    for (int i = 0; i < 199; i++) long_version[i] = 'a' + (i % 26);
    long_version[199] = '\0';

    struct test_dtb_spec spec = {0};
    spec.chosen_bootloader_version = long_version;
    make_test_dtb(g_test_dtb, sizeof(g_test_dtb), &spec);

    fdt_info_t info = {0};
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_parse(g_test_dtb, &info));

    const dtb_chosen_t *ch = dtb_get_chosen();
    size_t out_len = strlen(ch->bootloader_version);
    TEST_ASSERT_TRUE(out_len < 80);
    TEST_ASSERT_TRUE(out_len > 0);
}

int test_suite_dtb(void)
{
    UnityBegin("DTB Parser Tests");

    RUN_TEST(test_dtb_validate_rejects_null);
    RUN_TEST(test_dtb_validate_rejects_bad_magic);
    RUN_TEST(test_dtb_validate_rejects_old_version);
    RUN_TEST(test_dtb_validate_accepts_well_formed);
    RUN_TEST(test_dtb_validate_rejects_struct_past_total);
    RUN_TEST(test_dtb_validate_rejects_overflow);

    RUN_TEST(test_memreserve_header_single_entry);
    RUN_TEST(test_memreserve_header_multiple_entries);
    RUN_TEST(test_memreserve_root_property_pi_convention);
    RUN_TEST(test_memreserve_both_encodings_concatenated);
    RUN_TEST(test_memreserve_absent_returns_zero);

    RUN_TEST(test_chosen_rng_and_kaslr_seed);
    RUN_TEST(test_chosen_bootloader_metadata);
    RUN_TEST(test_chosen_absent_returns_zero);
    RUN_TEST(test_chosen_long_bootloader_version_truncated);

    return UnityEnd();
}
