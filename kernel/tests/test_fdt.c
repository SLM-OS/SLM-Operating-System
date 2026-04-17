/*
 * test_fdt.c - General-purpose FDT reader regression tests.
 *
 * Exercises kernel/lib/fdt/fdt.c end to end by building a tiny
 * flattened device tree in memory and hitting every public API
 * entry point. DTB layout is deliberately hand-rolled (no
 * dependency on `dtc`) so the tests remain self-contained.
 *
 * Tree built by make_test_fdt():
 *
 *   / {
 *       prop_at_root = [ "slmos-root\0" ];
 *       cpus {
 *           #address-cells = <1>;
 *       };
 *       axi {
 *           eth@100000 {
 *               local-mac-address = [ 2c cf 67 ca a0 b5 ];
 *               compatible = [ "cdns,macb\0" ];
 *               speed_mbps = <1000>;
 *           };
 *       };
 *   };
 */

#include "unity.h"
#include "../include/fdt.h"
#include "../include/dtb.h"       /* FDT_MAGIC, FDT_BEGIN_NODE, etc. */
#include <stdint.h>
#include <string.h>

/* ============================================================================
 * Helpers
 * ============================================================================ */

static inline uint32_t cpu_to_be32(uint32_t v)
{
    return ((v & 0xFFU)       << 24) |
           ((v & 0xFF00U)     <<  8) |
           ((v & 0xFF0000U)   >>  8) |
           ((v & 0xFF000000U) >> 24);
}

/* Emit one big-endian u32 into buf at *off, advancing *off. */
static void emit_u32(uint8_t *buf, uint32_t *off, uint32_t val)
{
    uint32_t be = cpu_to_be32(val);
    memcpy(buf + *off, &be, 4);
    *off += 4;
}

/* Emit a null-terminated string, then pad to 4-byte alignment. */
static void emit_str_aligned(uint8_t *buf, uint32_t *off, const char *s)
{
    size_t len = strlen(s) + 1;
    memcpy(buf + *off, s, len);
    *off += len;
    while (*off % 4) {
        buf[(*off)++] = 0;
    }
}

/* Emit raw bytes (property value), then pad to 4-byte alignment. */
static void emit_bytes_aligned(uint8_t *buf, uint32_t *off,
                               const uint8_t *data, uint32_t len)
{
    memcpy(buf + *off, data, len);
    *off += len;
    while (*off % 4) {
        buf[(*off)++] = 0;
    }
}

/*
 * Build the test FDT blob in `buf` (caller provides >= 512 bytes).
 * Returns total size written.
 *
 * String block and struct block are placed in fixed regions of the
 * buffer so offsets are easy to compute.
 */
#define STRINGS_OFFSET   64      /* room for the header */
#define STRINGS_SIZE     128
#define STRUCT_OFFSET    (STRINGS_OFFSET + STRINGS_SIZE)

/* Fixed string-table offsets — baked into the emitted tokens. Each
 * comment shows the null-terminated byte length; offsets are the
 * running sum, chosen so strings don't overlap. */
#define STR_PROP_AT_ROOT       0   /* "prop_at_root\0"      = 13 bytes */
#define STR_ADDRESS_CELLS     13   /* "#address-cells\0"    = 15 bytes */
#define STR_LOCAL_MAC_ADDRESS 28   /* "local-mac-address\0" = 18 bytes */
#define STR_COMPATIBLE        46   /* "compatible\0"        = 11 bytes */
#define STR_SPEED_MBPS        57   /* "speed_mbps\0"        = 11 bytes */

static void fill_strings(uint8_t *buf)
{
    uint32_t off = STRINGS_OFFSET;
    memcpy(buf + off + STR_PROP_AT_ROOT,       "prop_at_root",       13);
    memcpy(buf + off + STR_ADDRESS_CELLS,      "#address-cells",     15);
    memcpy(buf + off + STR_LOCAL_MAC_ADDRESS,  "local-mac-address",  18);
    memcpy(buf + off + STR_COMPATIBLE,         "compatible",         11);
    memcpy(buf + off + STR_SPEED_MBPS,         "speed_mbps",         11);
}

static uint32_t make_test_fdt(uint8_t *buf)
{
    /* Zero-fill the buffer so residual bytes don't confuse alignment logic. */
    memset(buf, 0, 512);

    /* ---- Strings block ---- */
    fill_strings(buf);

    /* ---- Struct block ---- */
    uint32_t o = STRUCT_OFFSET;

    /* BEGIN root ("") */
    emit_u32(buf, &o, FDT_BEGIN_NODE);
    emit_str_aligned(buf, &o, "");

    /* Root property: prop_at_root = "slmos-root" */
    emit_u32(buf, &o, FDT_PROP);
    emit_u32(buf, &o, 11);                    /* len of "slmos-root\0" */
    emit_u32(buf, &o, STR_PROP_AT_ROOT);
    emit_bytes_aligned(buf, &o, (const uint8_t *)"slmos-root", 11);

    /* BEGIN cpus */
    emit_u32(buf, &o, FDT_BEGIN_NODE);
    emit_str_aligned(buf, &o, "cpus");
    /*   cpus.#address-cells = <1> */
    emit_u32(buf, &o, FDT_PROP);
    emit_u32(buf, &o, 4);
    emit_u32(buf, &o, STR_ADDRESS_CELLS);
    emit_u32(buf, &o, 1);
    emit_u32(buf, &o, FDT_END_NODE);          /* /cpus */

    /* BEGIN axi */
    emit_u32(buf, &o, FDT_BEGIN_NODE);
    emit_str_aligned(buf, &o, "axi");

    /*   BEGIN eth@100000 */
    emit_u32(buf, &o, FDT_BEGIN_NODE);
    emit_str_aligned(buf, &o, "eth@100000");

    /*     local-mac-address = [ 2c cf 67 ca a0 b5 ] */
    static const uint8_t mac[6] = { 0x2c, 0xcf, 0x67, 0xca, 0xa0, 0xb5 };
    emit_u32(buf, &o, FDT_PROP);
    emit_u32(buf, &o, 6);
    emit_u32(buf, &o, STR_LOCAL_MAC_ADDRESS);
    emit_bytes_aligned(buf, &o, mac, 6);

    /*     compatible = "cdns,macb" */
    emit_u32(buf, &o, FDT_PROP);
    emit_u32(buf, &o, 10);                    /* "cdns,macb\0" */
    emit_u32(buf, &o, STR_COMPATIBLE);
    emit_bytes_aligned(buf, &o, (const uint8_t *)"cdns,macb", 10);

    /*     speed_mbps = <1000> */
    emit_u32(buf, &o, FDT_PROP);
    emit_u32(buf, &o, 4);
    emit_u32(buf, &o, STR_SPEED_MBPS);
    emit_u32(buf, &o, 1000);

    emit_u32(buf, &o, FDT_END_NODE);          /* /axi/eth@100000 */
    emit_u32(buf, &o, FDT_END_NODE);          /* /axi */
    emit_u32(buf, &o, FDT_END_NODE);          /* / */
    emit_u32(buf, &o, FDT_END);               /* end of structure */

    uint32_t struct_size = o - STRUCT_OFFSET;
    uint32_t total_size  = o;

    /* ---- Header ---- */
    struct fdt_header *hdr = (struct fdt_header *)buf;
    hdr->magic             = cpu_to_be32(FDT_MAGIC);
    hdr->totalsize         = cpu_to_be32(total_size);
    hdr->off_dt_struct     = cpu_to_be32(STRUCT_OFFSET);
    hdr->off_dt_strings    = cpu_to_be32(STRINGS_OFFSET);
    hdr->off_mem_rsvmap    = cpu_to_be32(0);
    hdr->version           = cpu_to_be32(FDT_VERSION);
    hdr->last_comp_version = cpu_to_be32(FDT_VERSION);
    hdr->boot_cpuid_phys   = cpu_to_be32(0);
    hdr->size_dt_strings   = cpu_to_be32(STRINGS_SIZE);
    hdr->size_dt_struct    = cpu_to_be32(struct_size);

    return total_size;
}

/* ============================================================================
 * fdt_init
 * ============================================================================ */

static uint8_t g_buf[512];
static struct fdt_handle g_h;

static void test_fdt_init_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_INVALID, fdt_init(NULL, g_buf));
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_INVALID, fdt_init(&g_h, NULL));
}

static void test_fdt_init_rejects_bad_magic(void)
{
    (void)make_test_fdt(g_buf);
    struct fdt_header *hdr = (struct fdt_header *)g_buf;
    hdr->magic = cpu_to_be32(0xDEADBEEFU);
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_BADMAGIC, fdt_init(&g_h, g_buf));
}

static void test_fdt_init_rejects_old_version(void)
{
    (void)make_test_fdt(g_buf);
    struct fdt_header *hdr = (struct fdt_header *)g_buf;
    hdr->version = cpu_to_be32(FDT_VERSION - 1);
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_BADVERSION, fdt_init(&g_h, g_buf));
}

static void test_fdt_init_rejects_struct_past_total(void)
{
    (void)make_test_fdt(g_buf);
    struct fdt_header *hdr = (struct fdt_header *)g_buf;
    hdr->size_dt_struct = cpu_to_be32(0xFFFFu);   /* bigger than totalsize */
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_BADSTRUCT, fdt_init(&g_h, g_buf));
}

static void test_fdt_init_rejects_overflow(void)
{
    (void)make_test_fdt(g_buf);
    struct fdt_header *hdr = (struct fdt_header *)g_buf;
    hdr->off_dt_struct  = cpu_to_be32(0xFFFFFFF0U);
    hdr->size_dt_struct = cpu_to_be32(0x00000100U);
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_BADSTRUCT, fdt_init(&g_h, g_buf));
}

static void test_fdt_init_accepts_well_formed(void)
{
    uint32_t total = make_test_fdt(g_buf);
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK, fdt_init(&g_h, g_buf));
    TEST_ASSERT_EQUAL_UINT32(total, g_h.totalsize);
    TEST_ASSERT_EQUAL_UINT32(STRUCT_OFFSET,  g_h.off_struct);
    TEST_ASSERT_EQUAL_UINT32(STRINGS_OFFSET, g_h.off_strings);
    TEST_ASSERT_EQUAL_UINT32(STRINGS_SIZE,   g_h.size_strings);
}

/* ============================================================================
 * fdt_find_node_by_path
 * ============================================================================ */

static void test_find_rejects_relative_path(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);
    uint32_t off;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_INVALID,
                          fdt_find_node_by_path(&g_h, "axi", &off));
}

static void test_find_returns_root_for_slash(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);
    uint32_t off = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_find_node_by_path(&g_h, "/", &off));
    TEST_ASSERT_EQUAL_INT(0, off);
}

static void test_find_nested_node(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);
    uint32_t off = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_find_node_by_path(&g_h,
                                                "/axi/eth@100000", &off));
    TEST_ASSERT_TRUE(off > 0);
}

static void test_find_missing_leaf(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);
    uint32_t off = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_NOTFOUND,
                          fdt_find_node_by_path(&g_h,
                                                "/axi/does-not-exist", &off));
}

/* Partial matches must NOT count — "/cpus" should match the cpus node,
 * but "/cpu" should return NOTFOUND. Catches a classic off-by-one in
 * name comparison. */
static void test_find_partial_name_rejected(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);
    uint32_t off = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_NOTFOUND,
                          fdt_find_node_by_path(&g_h, "/cpu", &off));
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_find_node_by_path(&g_h, "/cpus", &off));
}

/* Unit address matching: ethernet@100000 requires the full name in
 * the path. "/axi/eth" must fail; "/axi/eth@100000" must succeed. */
static void test_find_requires_unit_address(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);
    uint32_t off = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_NOTFOUND,
                          fdt_find_node_by_path(&g_h, "/axi/eth", &off));
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_find_node_by_path(&g_h,
                                                "/axi/eth@100000", &off));
}

/* ============================================================================
 * fdt_get_property
 * ============================================================================ */

static void test_get_property_root_string(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);
    uint32_t off = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_find_node_by_path(&g_h, "/", &off));

    const void *data = NULL;
    uint32_t len = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_get_property(&g_h, off, "prop_at_root",
                                           &data, &len));
    TEST_ASSERT_EQUAL_UINT32(11, len);
    TEST_ASSERT_EQUAL_STRING("slmos-root", (const char *)data);
}

static void test_get_property_on_nested_node(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);
    uint32_t off = 0;
    fdt_find_node_by_path(&g_h, "/axi/eth@100000", &off);

    const void *data = NULL;
    uint32_t len = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_get_property(&g_h, off, "local-mac-address",
                                           &data, &len));
    TEST_ASSERT_EQUAL_UINT32(6, len);
    const uint8_t *mac = data;
    TEST_ASSERT_EQUAL_UINT8(0x2c, mac[0]);
    TEST_ASSERT_EQUAL_UINT8(0xcf, mac[1]);
    TEST_ASSERT_EQUAL_UINT8(0x67, mac[2]);
    TEST_ASSERT_EQUAL_UINT8(0xca, mac[3]);
    TEST_ASSERT_EQUAL_UINT8(0xa0, mac[4]);
    TEST_ASSERT_EQUAL_UINT8(0xb5, mac[5]);
}

static void test_get_property_missing(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);
    uint32_t off = 0;
    fdt_find_node_by_path(&g_h, "/axi/eth@100000", &off);

    const void *data = NULL;
    uint32_t len = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_NOTFOUND,
                          fdt_get_property(&g_h, off,
                                           "this-property-does-not-exist",
                                           &data, &len));
}

/* Multi-prop node: both properties must be independently reachable.
 * Also proves the scanner doesn't walk into child nodes or trip over
 * END_NODE tokens. */
static void test_get_property_after_sibling_property(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);
    uint32_t off = 0;
    fdt_find_node_by_path(&g_h, "/axi/eth@100000", &off);

    const void *data = NULL;
    uint32_t len = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_get_property(&g_h, off, "compatible",
                                           &data, &len));
    TEST_ASSERT_EQUAL_UINT32(10, len);
    TEST_ASSERT_EQUAL_STRING("cdns,macb", (const char *)data);

    /* speed_mbps comes AFTER compatible — tests scanning past a prop. */
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_get_property(&g_h, off, "speed_mbps",
                                           &data, &len));
    TEST_ASSERT_EQUAL_UINT32(4, len);
}

/* ============================================================================
 * fdt_get_property_by_path + fdt_get_u32
 * ============================================================================ */

static void test_get_property_by_path_convenience(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);

    const void *data = NULL;
    uint32_t len = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_get_property_by_path(&g_h,
                                                   "/axi/eth@100000",
                                                   "local-mac-address",
                                                   &data, &len));
    TEST_ASSERT_EQUAL_UINT32(6, len);
}

static void test_get_property_by_path_missing_node(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);

    const void *data = NULL;
    uint32_t len = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_NOTFOUND,
                          fdt_get_property_by_path(&g_h,
                                                   "/axi/does-not-exist",
                                                   "local-mac-address",
                                                   &data, &len));
}

static void test_get_u32(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);

    uint32_t v = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK,
                          fdt_get_u32(&g_h, "/axi/eth@100000",
                                      "speed_mbps", &v));
    TEST_ASSERT_EQUAL_UINT32(1000, v);
}

/* Wrong-sized property must be rejected even if present — local-mac-address
 * is 6 bytes, which is NOT a valid u32. */
static void test_get_u32_rejects_wrong_size(void)
{
    make_test_fdt(g_buf);
    fdt_init(&g_h, g_buf);

    uint32_t v = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_BADSTRUCT,
                          fdt_get_u32(&g_h, "/axi/eth@100000",
                                      "local-mac-address", &v));
}

/* ============================================================================
 * Bounds hardening (review Warnings 1 & 2)
 *
 * These tests intentionally craft malformed DTBs where a node name or
 * property-value length would cause an unbounded walker to read past
 * the struct block. The library must reject them with BADSTRUCT
 * rather than walking off the end of the buffer.
 * ============================================================================ */

/* Build a DTB whose root BEGIN_NODE is followed by a name that has
 * no null terminator before the struct block ends. An unbounded
 * strlen would read past size_dt_struct. */
static void test_find_rejects_unterminated_node_name(void)
{
    /* Minimal header + struct block with a dangling name. */
    memset(g_buf, 0, 512);
    /* Strings block empty but present so fdt_init bounds-checks pass. */
    struct fdt_header *hdr = (struct fdt_header *)g_buf;
    const uint32_t struct_off = 64;
    const uint8_t  struct_bytes[8] = {
        0, 0, 0, 1,                 /* FDT_BEGIN_NODE           */
        'a', 'a', 'a', 'a',         /* name — no null terminator */
    };
    memcpy(g_buf + struct_off, struct_bytes, sizeof struct_bytes);

    hdr->magic             = cpu_to_be32(FDT_MAGIC);
    hdr->totalsize         = cpu_to_be32(struct_off + sizeof struct_bytes + 4);
    hdr->off_dt_struct     = cpu_to_be32(struct_off);
    hdr->size_dt_struct    = cpu_to_be32((uint32_t)sizeof struct_bytes);
    hdr->off_dt_strings    = cpu_to_be32(struct_off + sizeof struct_bytes);
    hdr->size_dt_strings   = cpu_to_be32(4);
    hdr->version           = cpu_to_be32(FDT_VERSION);
    hdr->last_comp_version = cpu_to_be32(FDT_VERSION);

    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK, fdt_init(&g_h, g_buf));
    uint32_t off = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_BADSTRUCT,
                          fdt_find_node_by_path(&g_h, "/a", &off));
}

/* Build a DTB whose FDT_PROP declares a length so large that
 * align4(len) would either overflow to 0 or push the walker past
 * size_dt_struct. Without the length guard this would cause an
 * unbounded walk. */
static void test_find_rejects_huge_prop_length(void)
{
    memset(g_buf, 0, 512);
    struct fdt_header *hdr = (struct fdt_header *)g_buf;
    const uint32_t struct_off = 64;
    uint8_t *s = g_buf + struct_off;
    uint32_t o = 0;

    /* BEGIN_NODE root ("") */
    uint32_t tok = cpu_to_be32(FDT_BEGIN_NODE); memcpy(s + o, &tok, 4); o += 4;
    s[o++] = 0; o += 3;                        /* empty name + pad */

    /* FDT_PROP with huge len that would overflow align4. */
    tok = cpu_to_be32(FDT_PROP); memcpy(s + o, &tok, 4); o += 4;
    uint32_t huge = cpu_to_be32(0xFFFFFFFDu);  /* align4(0xFFFFFFFD)=0 */
    memcpy(s + o, &huge, 4); o += 4;
    uint32_t zero = 0;
    memcpy(s + o, &zero, 4); o += 4;           /* nameoff = 0 */

    hdr->magic             = cpu_to_be32(FDT_MAGIC);
    hdr->totalsize         = cpu_to_be32(struct_off + o + 4);
    hdr->off_dt_struct     = cpu_to_be32(struct_off);
    hdr->size_dt_struct    = cpu_to_be32(o);
    hdr->off_dt_strings    = cpu_to_be32(struct_off + o);
    hdr->size_dt_strings   = cpu_to_be32(4);
    hdr->version           = cpu_to_be32(FDT_VERSION);
    hdr->last_comp_version = cpu_to_be32(FDT_VERSION);

    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK, fdt_init(&g_h, g_buf));
    uint32_t off = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_BADSTRUCT,
                          fdt_find_node_by_path(&g_h, "/any", &off));
}

/* fdt_get_property: a property's nameoff pointing outside the strings
 * block must be rejected rather than producing an unbounded strcmp. */
static void test_get_property_rejects_bad_nameoff(void)
{
    make_test_fdt(g_buf);

    /* Corrupt the nameoff of the first property (prop_at_root) to
     * point beyond the strings block. The strings block is declared
     * at STRINGS_SIZE = 128; setting nameoff to 0x10000 makes it
     * certainly out of range. */
    struct fdt_header *hdr = (struct fdt_header *)g_buf;
    uint32_t off_struct = STRUCT_OFFSET;
    /* BEGIN_NODE (4) + empty name padded to 4 + FDT_PROP (4) + len (4)
     * = 16 bytes in, then nameoff. */
    uint32_t nameoff_pos = off_struct + 16;
    uint32_t bad_nameoff = cpu_to_be32(0x10000u);
    memcpy(g_buf + nameoff_pos, &bad_nameoff, 4);
    (void)hdr;

    TEST_ASSERT_EQUAL_INT(FDT_LIB_OK, fdt_init(&g_h, g_buf));
    uint32_t off = 0;
    fdt_find_node_by_path(&g_h, "/", &off);
    const void *data = NULL;
    uint32_t len = 0;
    TEST_ASSERT_EQUAL_INT(FDT_LIB_E_BADSTRUCT,
                          fdt_get_property(&g_h, off, "prop_at_root",
                                           &data, &len));
}

/* ============================================================================
 * Suite
 * ============================================================================ */

int test_suite_fdt(void)
{
    UnityBegin("FDT Reader Tests");

    /* fdt_init */
    RUN_TEST(test_fdt_init_rejects_null);
    RUN_TEST(test_fdt_init_rejects_bad_magic);
    RUN_TEST(test_fdt_init_rejects_old_version);
    RUN_TEST(test_fdt_init_rejects_struct_past_total);
    RUN_TEST(test_fdt_init_rejects_overflow);
    RUN_TEST(test_fdt_init_accepts_well_formed);

    /* fdt_find_node_by_path */
    RUN_TEST(test_find_rejects_relative_path);
    RUN_TEST(test_find_returns_root_for_slash);
    RUN_TEST(test_find_nested_node);
    RUN_TEST(test_find_missing_leaf);
    RUN_TEST(test_find_partial_name_rejected);
    RUN_TEST(test_find_requires_unit_address);

    /* fdt_get_property */
    RUN_TEST(test_get_property_root_string);
    RUN_TEST(test_get_property_on_nested_node);
    RUN_TEST(test_get_property_missing);
    RUN_TEST(test_get_property_after_sibling_property);

    /* fdt_get_property_by_path + fdt_get_u32 */
    RUN_TEST(test_get_property_by_path_convenience);
    RUN_TEST(test_get_property_by_path_missing_node);
    RUN_TEST(test_get_u32);
    RUN_TEST(test_get_u32_rejects_wrong_size);

    /* Bounds hardening */
    RUN_TEST(test_find_rejects_unterminated_node_name);
    RUN_TEST(test_find_rejects_huge_prop_length);
    RUN_TEST(test_get_property_rejects_bad_nameoff);

    return UnityEnd();
}
