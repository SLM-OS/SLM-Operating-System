/*
 * test_hef.c — Unit tests for the .hef outer-header validator and
 * the nanopb runtime (freestanding integration smoke test).
 *
 * Covered:
 *   - hef_parse_outer_header accepts a hand-built v0 / v1 header
 *     and rejects bad magic / bad version / truncated body / oversize
 *     proto_size.
 *   - nanopb pb_encode_varint / pb_decode_varint round-trip through
 *     a pb_ostream_from_buffer / pb_istream_from_buffer pair,
 *     proving the library links correctly against our freestanding
 *     glue in pb_syshdr.h.
 *
 * Out of scope (lands with real .hef parsing in later work):
 *   - Decoding the full ProtoHEFHef message — needs hef.pb.{c,h}
 *     generated from docs/reference/hailo-hef.proto.
 *   - MD5 / CRC verification — deferred until a consumer actually
 *     needs it.
 */

#include "unity.h"
#include "../ai_accel/hailo/hef_header.h"
#include "../include/uart.h"
#include "pb.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "test_harness.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Helpers — build a minimal valid `.hef` blob in memory.                     */
/* -------------------------------------------------------------------------- */

static void put_be_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}

static void put_be_u64(uint8_t *p, uint64_t v)
{
    put_be_u32(p + 0, (uint32_t)(v >> 32));
    put_be_u32(p + 4, (uint32_t)(v & 0xFFFFFFFFu));
}

/* Build a v0 header into `buf`. Returns total bytes written
 * (header + fake proto body). Caller ensures `buf` is large enough. */
static size_t build_v0_blob(uint8_t *buf, uint32_t proto_size)
{
    /* 12-byte common header + 20-byte v0 trailer = 32 bytes header. */
    put_be_u32(buf + 0,  HEF_MAGIC);
    put_be_u32(buf + 4,  HEF_VERSION_V0);
    put_be_u32(buf + 8,  proto_size);
    /* Reserved (4 bytes) + MD5 (16 bytes) — leave zero. */
    memset(buf + 12, 0, 20);
    /* Proto body — zeros are fine for header-validator tests. */
    memset(buf + 32, 0, proto_size);
    return 32 + proto_size;
}

static size_t build_v1_blob(uint8_t *buf, uint32_t proto_size,
                            uint64_t ccws_size)
{
    /* 12-byte common + 16-byte v1 trailer = 28 bytes header. */
    put_be_u32(buf + 0,  HEF_MAGIC);
    put_be_u32(buf + 4,  HEF_VERSION_V1);
    put_be_u32(buf + 8,  proto_size);
    put_be_u32(buf + 12, 0xDEADBEEFu);        /* CRC placeholder */
    put_be_u64(buf + 16, ccws_size);
    put_be_u32(buf + 24, 0);                   /* reserved */
    /* Proto body + CCWS — zeros. */
    size_t n = 28 + proto_size + ccws_size;
    memset(buf + 28, 0, proto_size + ccws_size);
    return n;
}

/* -------------------------------------------------------------------------- */
/* HEF header validator tests                                                 */
/* -------------------------------------------------------------------------- */

static void test_hef_rejects_short_blob(void)
{
    uint8_t tiny[8] = {0};
    struct hef_outer_header hdr;
    TEST_ASSERT_EQUAL_INT(HEF_ERR_SHORT,
                          hef_parse_outer_header(tiny, sizeof(tiny), &hdr));
}

static void test_hef_rejects_bad_magic(void)
{
    uint8_t buf[64] = {0};
    build_v0_blob(buf, 16);
    put_be_u32(buf, 0xCAFEBABEu);   /* clobber magic */
    struct hef_outer_header hdr;
    TEST_ASSERT_EQUAL_INT(HEF_ERR_BAD_MAGIC,
                          hef_parse_outer_header(buf, sizeof(buf), &hdr));
}

static void test_hef_rejects_bad_version(void)
{
    uint8_t buf[64] = {0};
    build_v0_blob(buf, 16);
    put_be_u32(buf + 4, 99);     /* unknown version */
    struct hef_outer_header hdr;
    TEST_ASSERT_EQUAL_INT(HEF_ERR_BAD_VERSION,
                          hef_parse_outer_header(buf, sizeof(buf), &hdr));
}

static void test_hef_rejects_zero_proto_size(void)
{
    uint8_t buf[64] = {0};
    build_v0_blob(buf, 0);
    struct hef_outer_header hdr;
    TEST_ASSERT_EQUAL_INT(HEF_ERR_BAD_SIZE,
                          hef_parse_outer_header(buf, sizeof(buf), &hdr));
}

static void test_hef_rejects_truncated_proto(void)
{
    uint8_t buf[64] = {0};
    build_v0_blob(buf, 200);     /* declare 200 bytes of proto... */
    struct hef_outer_header hdr;
    /* ...but pass only 64 bytes — body doesn't fit. */
    TEST_ASSERT_EQUAL_INT(HEF_ERR_TRUNCATED,
                          hef_parse_outer_header(buf, 64, &hdr));
}

static void test_hef_accepts_v0(void)
{
    uint8_t buf[128] = {0};
    size_t total = build_v0_blob(buf, 64);
    struct hef_outer_header hdr;
    int rc = hef_parse_outer_header(buf, total, &hdr);
    TEST_ASSERT_EQUAL_INT(HEF_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(HEF_VERSION_V0, hdr.version);
    TEST_ASSERT_EQUAL_UINT32(64, hdr.proto_size);
    TEST_ASSERT_EQUAL_UINT32(32, hdr.proto_offset);
    TEST_ASSERT_EQUAL_UINT64(0, hdr.ccws_size);
}

static void test_hef_accepts_v1_with_ccws(void)
{
    uint8_t buf[256] = {0};
    size_t total = build_v1_blob(buf, 64, 128);
    struct hef_outer_header hdr;
    int rc = hef_parse_outer_header(buf, total, &hdr);
    TEST_ASSERT_EQUAL_INT(HEF_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(HEF_VERSION_V1, hdr.version);
    TEST_ASSERT_EQUAL_UINT32(64, hdr.proto_size);
    TEST_ASSERT_EQUAL_UINT32(28, hdr.proto_offset);
    TEST_ASSERT_EQUAL_UINT64(128, hdr.ccws_size);
    TEST_ASSERT_EQUAL_UINT64(28 + 64, hdr.ccws_offset);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, hdr.crc);
}

static void test_hef_rejects_truncated_ccws(void)
{
    uint8_t buf[256] = {0};
    /* Declare 128 bytes of CCWS but keep only 64 bytes past the proto. */
    size_t total = build_v1_blob(buf, 64, 128);
    struct hef_outer_header hdr;
    int rc = hef_parse_outer_header(buf, total - 64, &hdr);
    TEST_ASSERT_EQUAL_INT(HEF_ERR_TRUNCATED, rc);
}

static void test_hef_rejects_null_blob(void)
{
    struct hef_outer_header hdr;
    TEST_ASSERT_EQUAL_INT(HEF_ERR_SHORT,
                          hef_parse_outer_header(NULL, 64, &hdr));
}

static void test_hef_rejects_oversize_proto(void)
{
    /* proto_size > 256 MB upper bound — defense against a non-.hef
     * file that happens to pass the magic test. */
    uint8_t buf[64] = {0};
    put_be_u32(buf +  0, HEF_MAGIC);
    put_be_u32(buf +  4, HEF_VERSION_V0);
    put_be_u32(buf +  8, 0x20000000u);   /* 512 MB — too big */
    struct hef_outer_header hdr;
    TEST_ASSERT_EQUAL_INT(HEF_ERR_BAD_SIZE,
                          hef_parse_outer_header(buf, sizeof(buf), &hdr));
}

static void test_hef_accepts_v0_with_md5(void)
{
    uint8_t buf[128] = {0};
    build_v0_blob(buf, 32);
    /* Seed MD5 bytes in the v0 trailer (after the 12-byte common
     * header + 4-byte reserved). Round-trip through the parser. */
    for (int i = 0; i < 16; i++) buf[16 + i] = (uint8_t)(0xA0 + i);
    struct hef_outer_header hdr;
    int rc = hef_parse_outer_header(buf, 32 + 32, &hdr);
    TEST_ASSERT_EQUAL_INT(HEF_OK, rc);
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(0xA0 + i), hdr.md5[i]);
    }
}

/*
 * Regression for the overflow check on CCWS size: an attacker-
 * controlled v1 blob that declares a near-UINT64_MAX ccws_size
 * previously slipped through (offset + size wrapped to a small
 * value). The fixed check is `ccws_size > size - proto_end`,
 * which rejects it.
 */
static void test_hef_v1_ccws_size_overflow_rejected(void)
{
    uint8_t buf[256] = {0};
    /* Build a v1 header with proto_size=64 and a poisoned
     * ccws_size = 0xFFFFFFFFFFFFFFFF. */
    put_be_u32(buf + 0,  HEF_MAGIC);
    put_be_u32(buf + 4,  HEF_VERSION_V1);
    put_be_u32(buf + 8,  64);                         /* proto_size */
    put_be_u32(buf + 12, 0);                          /* crc */
    put_be_u32(buf + 16, 0xFFFFFFFFu);                /* ccws_size high */
    put_be_u32(buf + 20, 0xFFFFFFFFu);                /* ccws_size low */
    put_be_u32(buf + 24, 0);                          /* reserved */
    /* Proto body fills remaining space — parser needs header (28)
     * + proto_size (64) = 92 bytes to pass the earlier truncation
     * check, which our 256-byte buf satisfies. */
    struct hef_outer_header hdr;
    int rc = hef_parse_outer_header(buf, sizeof(buf), &hdr);
    TEST_ASSERT_EQUAL_INT(HEF_ERR_TRUNCATED, rc);
}

/* Boundary: ccws_size exactly fits the remaining blob. Must accept. */
static void test_hef_v1_ccws_exact_fit_accepted(void)
{
    uint8_t buf[256] = {0};
    /* header (28) + proto (64) + ccws (164) = 256 bytes exactly. */
    size_t total = build_v1_blob(buf, 64, 164);
    TEST_ASSERT_EQUAL_UINT64(256, total);
    struct hef_outer_header hdr;
    int rc = hef_parse_outer_header(buf, total, &hdr);
    TEST_ASSERT_EQUAL_INT(HEF_OK, rc);
    TEST_ASSERT_EQUAL_UINT64(164, hdr.ccws_size);
}

/* -------------------------------------------------------------------------- */
/* nanopb freestanding smoke test                                             */
/* -------------------------------------------------------------------------- */

static void test_nanopb_varint_roundtrip(void)
{
    /* Encode three varints, decode them back — proves the nanopb
     * runtime links and runs against our pb_syshdr.h. Uses the
     * low-level API so no generated .pb.c is needed. */
    uint8_t buf[32];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));

    TEST_ASSERT_TRUE(pb_encode_varint(&os, 42u));
    TEST_ASSERT_TRUE(pb_encode_varint(&os, 300u));
    TEST_ASSERT_TRUE(pb_encode_varint(&os, 0xDEADBEEFu));

    size_t written = os.bytes_written;
    TEST_ASSERT_TRUE(written > 0);

    pb_istream_t is = pb_istream_from_buffer(buf, written);
    uint64_t a = 0, b = 0, c = 0;
    TEST_ASSERT_TRUE(pb_decode_varint(&is, &a));
    TEST_ASSERT_TRUE(pb_decode_varint(&is, &b));
    TEST_ASSERT_TRUE(pb_decode_varint(&is, &c));

    TEST_ASSERT_EQUAL_UINT64(42u,         a);
    TEST_ASSERT_EQUAL_UINT64(300u,        b);
    TEST_ASSERT_EQUAL_UINT64(0xDEADBEEFu, c);
}

static void test_nanopb_ostream_overflow(void)
{
    /* Zero-byte buffer — every encode must fail cleanly. */
    uint8_t buf[1];
    pb_ostream_t os = pb_ostream_from_buffer(buf, 0);
    TEST_ASSERT_FALSE(pb_encode_varint(&os, 1u));
}

/* -------------------------------------------------------------------------- */

int test_suite_hef(void)
{
    UnityBegin(".hef outer header + nanopb");

    RUN_TEST(test_hef_rejects_short_blob);
    RUN_TEST(test_hef_rejects_bad_magic);
    RUN_TEST(test_hef_rejects_bad_version);
    RUN_TEST(test_hef_rejects_zero_proto_size);
    RUN_TEST(test_hef_rejects_truncated_proto);
    RUN_TEST(test_hef_accepts_v0);
    RUN_TEST(test_hef_accepts_v1_with_ccws);
    RUN_TEST(test_hef_rejects_truncated_ccws);
    RUN_TEST(test_hef_rejects_null_blob);
    RUN_TEST(test_hef_rejects_oversize_proto);
    RUN_TEST(test_hef_accepts_v0_with_md5);
    RUN_TEST(test_hef_v1_ccws_size_overflow_rejected);
    RUN_TEST(test_hef_v1_ccws_exact_fit_accepted);

    RUN_TEST(test_nanopb_varint_roundtrip);
    RUN_TEST(test_nanopb_ostream_overflow);

    return UnityEnd();
}
