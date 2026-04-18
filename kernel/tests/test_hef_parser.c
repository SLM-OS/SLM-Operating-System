/*
 * test_hef_parser.c — tests for the nanopb-driven `.hef` proto decode
 * in kernel/ai_accel/hailo/hef_parser.c.
 *
 * Tests build synthetic protobuf wire-format blobs directly. Using
 * hand-crafted bytes (rather than a real HEF fixture) keeps the
 * test kernel small and makes each test's intent explicit — the
 * wire bytes encode exactly one scenario and nothing more. The
 * real yolov5s.hef fixture is exercised on hardware by the
 * `hailo load /mnt/files/yolov5s.hef` shell command.
 *
 * Wire format cheat-sheet (proto3, as used here):
 *   tag byte = (field_number << 3) | wire_type
 *   wire_type 0 = varint        (uint32, uint64, enum, bool)
 *   wire_type 2 = length-prefix (string, bytes, sub-message, packed repeated)
 *
 * Numbers: varint-encoded little-endian, 7 bits per byte, MSB=1 means
 * "more bytes follow". Small values (< 128) fit in one byte.
 */

#include "unity.h"
#include "../ai_accel/hailo/hef_parser.h"
#include "test_harness.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Wire-format builders                                                        */
/* -------------------------------------------------------------------------- */

/* Emit a varint into buf at offset *off; advance *off. Caller ensures
 * buf has at least 5 bytes of room (uint32 max is 5 varint bytes). */
static void emit_varint(uint8_t *buf, size_t *off, uint64_t val)
{
    while (val >= 0x80) {
        buf[(*off)++] = (uint8_t)((val & 0x7F) | 0x80);
        val >>= 7;
    }
    buf[(*off)++] = (uint8_t)val;
}

static void emit_tag(uint8_t *buf, size_t *off,
                     uint32_t field_no, uint32_t wire_type)
{
    emit_varint(buf, off, ((uint64_t)field_no << 3) | wire_type);
}

/* Emit a length-delimited sub-message or string. */
static void emit_lenprefix(uint8_t *buf, size_t *off,
                           uint32_t field_no,
                           const uint8_t *payload, size_t n)
{
    emit_tag(buf, off, field_no, 2);  /* wire type 2 = length-delimited */
    emit_varint(buf, off, (uint64_t)n);
    memcpy(buf + *off, payload, n);
    *off += n;
}

/* Emit a varint field (uint32/uint64/enum/bool). */
static void emit_varint_field(uint8_t *buf, size_t *off,
                              uint32_t field_no, uint64_t val)
{
    emit_tag(buf, off, field_no, 0);
    emit_varint(buf, off, val);
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                       */
/* -------------------------------------------------------------------------- */

/*
 * Decode a ProtoHEFHef that carries a ProtoHEFHeader with both
 * hw_arch and sdk_version_str set.
 */
static void test_decode_header_fields(void)
{
    /* ProtoHEFHeader body: hw_arch=1 (HAILO8L), sdk_version_str="v4.21.0" */
    uint8_t inner[32];
    size_t ilen = 0;
    emit_varint_field(inner, &ilen, /*field 1 hw_arch*/ 1, HEF_HW_ARCH_HAILO8L);
    emit_lenprefix(inner, &ilen, /*field 3 sdk_version_str*/ 3,
                   (const uint8_t *)"v4.21.0", 7);

    /* ProtoHEFHef: field 1 is ProtoHEFHeader sub-message. */
    uint8_t outer[64];
    size_t olen = 0;
    emit_lenprefix(outer, &olen, 1, inner, ilen);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(outer, olen, &info));
    TEST_ASSERT_TRUE(info.hw_arch_known);
    TEST_ASSERT_EQUAL_UINT32(HEF_HW_ARCH_HAILO8L, info.hw_arch);
    TEST_ASSERT_EQUAL_STRING("v4.21.0", info.sdk_version);
    TEST_ASSERT_EQUAL_UINT32(0, info.network_group_count);
    TEST_ASSERT_FALSE(info.string_truncated);
}

/*
 * Missing header: hef_parse_body still succeeds, but leaves
 * hw_arch_known=false and sdk_version empty.
 */
static void test_decode_empty_proto(void)
{
    /* Zero-length blob is a valid "empty" protobuf (no fields). All
     * output stays at its default zero state. */
    uint8_t stub = 0;
    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(&stub, 0, &info));
    TEST_ASSERT_FALSE(info.hw_arch_known);
    TEST_ASSERT_EQUAL_UINT32(0, info.network_group_count);
    TEST_ASSERT_EQUAL_STRING("", info.sdk_version);
}

static void test_decode_proto_without_header(void)
{
    /* Just a (truncated) network_groups entry — no header. */
    uint8_t ng_inner[8];
    size_t  ng_ilen = 0;
    emit_lenprefix(ng_inner, &ng_ilen, /*field 10 name*/ 10,
                   (const uint8_t *)"net", 3);

    uint8_t outer[32];
    size_t  olen = 0;
    emit_lenprefix(outer, &olen, /*field 2 network_groups*/ 2,
                   ng_inner, ng_ilen);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(outer, olen, &info));
    TEST_ASSERT_FALSE(info.hw_arch_known);
    TEST_ASSERT_EQUAL_STRING("", info.sdk_version);
    TEST_ASSERT_EQUAL_UINT32(1, info.network_group_count);
    TEST_ASSERT_EQUAL_STRING("net", info.first_network_group);
}

/*
 * Three network groups — count must be 3, first name captured,
 * subsequent names not overwriting the first.
 */
static void test_decode_multiple_network_groups(void)
{
    uint8_t outer[96];
    size_t  olen = 0;

    const char *names[] = { "alpha", "beta", "gamma" };
    for (int i = 0; i < 3; i++) {
        uint8_t inner[16];
        size_t  ilen = 0;
        emit_lenprefix(inner, &ilen, 10,
                       (const uint8_t *)names[i], strlen(names[i]));
        emit_lenprefix(outer, &olen, 2, inner, ilen);
    }

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(outer, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(3, info.network_group_count);
    TEST_ASSERT_EQUAL_STRING("alpha", info.first_network_group);
}

/*
 * String truncation: a sdk_version_str longer than HEF_PARSER_MAX_STR-1
 * must be cut to cap-1 bytes, NUL-terminated, and the truncated flag
 * set. Rest of the decode proceeds normally.
 */
static void test_decode_string_truncation(void)
{
    /* Build a 200-byte sdk_version_str (much longer than MAX_STR=64). */
    uint8_t long_str[200];
    memset(long_str, 'X', sizeof(long_str));

    uint8_t inner[256];
    size_t  ilen = 0;
    emit_lenprefix(inner, &ilen, 3, long_str, sizeof(long_str));

    uint8_t outer[300];
    size_t  olen = 0;
    emit_lenprefix(outer, &olen, 1, inner, ilen);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(outer, olen, &info));
    TEST_ASSERT_TRUE(info.string_truncated);
    /* sdk_version should be cap-1 X's, then NUL. */
    for (int i = 0; i < HEF_PARSER_MAX_STR - 1; i++) {
        TEST_ASSERT_EQUAL_HEX8('X', (uint8_t)info.sdk_version[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(0, (uint8_t)info.sdk_version[HEF_PARSER_MAX_STR - 1]);
}

/*
 * Garbage bytes in the middle of a varint → nanopb rejects.
 * Build a proto with a field tag but then a never-terminating varint
 * (all bytes with MSB=1), truncated mid-varint by the buffer end.
 */
static void test_decode_rejects_malformed(void)
{
    /* Tag says field 1, wire type 0 (varint); then 10 bytes all MSB=1
     * with nothing to terminate. nanopb must report decode failure. */
    uint8_t bad[] = { 0x08,
                      0x80, 0x80, 0x80, 0x80, 0x80,
                      0x80, 0x80, 0x80, 0x80, 0x80 };

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_ERR_DECODE,
                          hef_parse_body(bad, sizeof(bad), &info));
}

/*
 * Null-arg and zero-size handling.
 */
static void test_decode_rejects_null_args(void)
{
    struct hef_info info;
    uint8_t b[1] = {0};
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_ERR_INVAL,
                          hef_parse_body(NULL, 64, &info));
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_ERR_INVAL,
                          hef_parse_body(b, sizeof(b), NULL));
}

/* -------------------------------------------------------------------------- */
/* Suite entry                                                                 */
/* -------------------------------------------------------------------------- */

int test_suite_hef_parser(void)
{
    UnityBegin("HEF protobuf parser");

    RUN_TEST(test_decode_header_fields);
    RUN_TEST(test_decode_empty_proto);
    RUN_TEST(test_decode_proto_without_header);
    RUN_TEST(test_decode_multiple_network_groups);
    RUN_TEST(test_decode_string_truncation);
    RUN_TEST(test_decode_rejects_malformed);
    RUN_TEST(test_decode_rejects_null_args);

    return UnityEnd();
}
