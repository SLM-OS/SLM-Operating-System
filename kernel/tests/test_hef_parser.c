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
/* Tensor-metadata tests (Phase 5.1)                                           */
/*                                                                             */
/* Wire structure built by the helpers below:                                   */
/*   ProtoHEFHef                                                                */
/*     network_groups[i]  (field 2)                                             */
/*       ops[j]           (field 8)                                             */
/*         input_pads[k]  (field 2)                                             */
/*           tensor_shape (field 6 inside shape_info oneof)                     */
/*         output_pads[l] (field 3)                                             */
/* -------------------------------------------------------------------------- */

/* Emit a ProtoHEFTensorShape body: 6 uint32 fields (tags 1..6). Only
 * non-zero dims are emitted — proto3 default-skips zeros. */
static size_t emit_tensor_shape(uint8_t *buf,
                                uint32_t h, uint32_t ph,
                                uint32_t w, uint32_t pw,
                                uint32_t f, uint32_t pf)
{
    size_t off = 0;
    if (h)  emit_varint_field(buf, &off, 1, h);
    if (ph) emit_varint_field(buf, &off, 2, ph);
    if (w)  emit_varint_field(buf, &off, 3, w);
    if (pw) emit_varint_field(buf, &off, 4, pw);
    if (f)  emit_varint_field(buf, &off, 5, f);
    if (pf) emit_varint_field(buf, &off, 6, pf);
    return off;
}

/* Emit one ProtoHEFPad with optional tensor_shape. Returns bytes written. */
static size_t emit_pad(uint8_t *buf, uint32_t index, const char *name,
                       uint32_t h, uint32_t ph, uint32_t w, uint32_t pw,
                       uint32_t f, uint32_t pf)
{
    size_t off = 0;
    emit_varint_field(buf, &off, /*1=index*/ 1, index);
    if (name) {
        emit_lenprefix(buf, &off, /*2=name*/ 2,
                       (const uint8_t *)name, strlen(name));
    }
    if (h || ph || w || pw || f || pf) {
        uint8_t shape[48];
        size_t  slen = emit_tensor_shape(shape, h, ph, w, pw, f, pf);
        emit_lenprefix(buf, &off, /*6=tensor_shape*/ 6, shape, slen);
    }
    return off;
}

/* Emit one ProtoHEFPad that carries an NMS shape instead of a
 * tensor shape (tag 7 in the oneof). Body is any bytes; the parser
 * should default-skip them and leave has_tensor_shape = false. */
static size_t emit_pad_nms(uint8_t *buf, uint32_t index)
{
    size_t off = 0;
    emit_varint_field(buf, &off, 1, index);
    /* nms_shape body: one arbitrary uint32 field so the sub-message
     * is non-empty. Tag 1 inside NmsShape is number_of_classes. */
    uint8_t nms[8];
    size_t  nlen = 0;
    emit_varint_field(nms, &nlen, 1, /*number_of_classes*/ 80);
    emit_lenprefix(buf, &off, /*7=nms_shape*/ 7, nms, nlen);
    return off;
}

/*
 * Build a full ProtoHEFHef wire blob with ONE network group that
 * contains ONE op with the given number of input and output pads.
 * All pads share the same 224x224x3 shape so the test can spot-
 * check the dims were decoded correctly. Returns total blob size.
 */
static size_t build_ng_with_pads(uint8_t *out, size_t cap,
                                 uint32_t num_in_pads,
                                 uint32_t num_out_pads)
{
    /* Op body: name="op0", then input_pads repeated, output_pads repeated. */
    uint8_t op_buf[1024];
    size_t  op_len = 0;
    emit_lenprefix(op_buf, &op_len, 1, (const uint8_t *)"op0", 3);
    for (uint32_t i = 0; i < num_in_pads; i++) {
        uint8_t pad_buf[64];
        size_t  pad_len = emit_pad(pad_buf, i, "in", 224, 224, 224, 224, 3, 4);
        emit_lenprefix(op_buf, &op_len, 2, pad_buf, pad_len);
    }
    for (uint32_t i = 0; i < num_out_pads; i++) {
        uint8_t pad_buf[64];
        size_t  pad_len = emit_pad(pad_buf, 100 + i, "out",
                                   7, 7, 7, 7, 1000, 1000);
        emit_lenprefix(op_buf, &op_len, 3, pad_buf, pad_len);
    }

    /* NG body: name="ng0", then ops (single op). */
    uint8_t ng_buf[2048];
    size_t  ng_len = 0;
    emit_lenprefix(ng_buf, &ng_len, 10, (const uint8_t *)"ng0", 3);
    emit_lenprefix(ng_buf, &ng_len, 8, op_buf, op_len);

    /* Root: one network_groups. */
    size_t olen = 0;
    (void)cap;
    emit_lenprefix(out, &olen, 2, ng_buf, ng_len);
    return olen;
}

static void test_decode_pad_with_tensor_shape(void)
{
    uint8_t blob[512];
    size_t  n = build_ng_with_pads(blob, sizeof(blob), /*in*/ 1, /*out*/ 1);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, n, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.network_group_count);
    TEST_ASSERT_EQUAL_UINT32(1, info.op_count);
    TEST_ASSERT_EQUAL_UINT32(2, info.pad_count);
    TEST_ASSERT_FALSE(info.pads_truncated);

    /* Input pad first (decode order), then output. */
    TEST_ASSERT_TRUE(info.pads[0].is_input);
    TEST_ASSERT_TRUE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(0, info.pads[0].index);
    TEST_ASSERT_EQUAL_STRING("in", info.pads[0].name);
    TEST_ASSERT_EQUAL_UINT32(224, info.pads[0].height);
    TEST_ASSERT_EQUAL_UINT32(224, info.pads[0].width);
    TEST_ASSERT_EQUAL_UINT32(3,   info.pads[0].features);
    TEST_ASSERT_EQUAL_UINT32(4,   info.pads[0].padded_features);

    TEST_ASSERT_FALSE(info.pads[1].is_input);
    TEST_ASSERT_TRUE(info.pads[1].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(100, info.pads[1].index);
    TEST_ASSERT_EQUAL_STRING("out", info.pads[1].name);
    TEST_ASSERT_EQUAL_UINT32(7,    info.pads[1].height);
    TEST_ASSERT_EQUAL_UINT32(1000, info.pads[1].features);
}

static void test_decode_multi_pad_network_group(void)
{
    /* 2 input + 3 output → pad_count must be 5, order preserved. */
    uint8_t blob[1024];
    size_t  n = build_ng_with_pads(blob, sizeof(blob), 2, 3);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, n, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.op_count);
    TEST_ASSERT_EQUAL_UINT32(5, info.pad_count);

    /* Two in pads first, then three out pads. */
    for (uint32_t i = 0; i < 2; i++) {
        TEST_ASSERT_TRUE(info.pads[i].is_input);
    }
    for (uint32_t i = 2; i < 5; i++) {
        TEST_ASSERT_FALSE(info.pads[i].is_input);
    }
}

static void test_decode_pads_truncated(void)
{
    /* Force > HEF_PARSER_MAX_PADS pads → pads_truncated must be set
     * and pad_count must saturate at MAX_PADS. */
    uint8_t blob[4096];
    size_t  n = build_ng_with_pads(blob, sizeof(blob),
                                   HEF_PARSER_MAX_PADS + 1, 0);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, n, &info));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)HEF_PARSER_MAX_PADS, info.pad_count);
    TEST_ASSERT_TRUE(info.pads_truncated);
}

static void test_decode_pad_without_tensor_shape(void)
{
    /* Pad that carries no shape at all — has_tensor_shape stays false
     * and dims remain zero. */
    uint8_t pad[32];
    size_t  pad_len = emit_pad(pad, 5, "empty", 0, 0, 0, 0, 0, 0);

    uint8_t op[64];
    size_t  op_len = 0;
    emit_lenprefix(op, &op_len, 1, (const uint8_t *)"op0", 3);
    emit_lenprefix(op, &op_len, 2, pad, pad_len);

    uint8_t ng[128];
    size_t  ng_len = 0;
    emit_lenprefix(ng, &ng_len, 8, op, op_len);

    uint8_t blob[256];
    size_t  olen = 0;
    emit_lenprefix(blob, &olen, 2, ng, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_FALSE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(0, info.pads[0].height);
    TEST_ASSERT_EQUAL_STRING("empty", info.pads[0].name);
}

static void test_decode_pad_with_nms_shape_leaves_tensor_fields_empty(void)
{
    /* A pad with nms_shape (tag 7) instead of tensor_shape (tag 6).
     * Because both branches share the oneof callback slot, our
     * callback fires with field->tag==7 and must not populate
     * tensor-shape dims. */
    uint8_t pad[32];
    size_t  pad_len = emit_pad_nms(pad, 99);

    uint8_t op[64];
    size_t  op_len = 0;
    emit_lenprefix(op, &op_len, 3, pad, pad_len);  /* output_pads */

    uint8_t ng[128];
    size_t  ng_len = 0;
    emit_lenprefix(ng, &ng_len, 8, op, op_len);

    uint8_t blob[256];
    size_t  olen = 0;
    emit_lenprefix(blob, &olen, 2, ng, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_FALSE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_FALSE(info.pads[0].is_input);
    TEST_ASSERT_EQUAL_UINT32(99, info.pads[0].index);
}

static void test_decode_second_network_group_pads_ignored(void)
{
    /* Two network groups — the first has ops; the second also has
     * ops but its pad_count contribution must be zero. op_count
     * likewise reflects only NG 0. */
    uint8_t blob[512];
    size_t  n = build_ng_with_pads(blob, sizeof(blob), 1, 1);
    /* Append a second NG identical to the first. */
    size_t second_len = build_ng_with_pads(blob + n, sizeof(blob) - n, 1, 1);
    size_t total = n + second_len;
    (void)second_len;

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, total, &info));
    TEST_ASSERT_EQUAL_UINT32(2, info.network_group_count);
    TEST_ASSERT_EQUAL_UINT32(1, info.op_count);      /* only NG 0 counted */
    TEST_ASSERT_EQUAL_UINT32(2, info.pad_count);     /* only NG 0 pads */
}

static void test_decode_partial_tensor_shape(void)
{
    /* Only height + width set; features and all padded variants
     * default to zero. has_tensor_shape must be true because the
     * tensor_shape sub-message was present on the wire. */
    uint8_t pad[32];
    size_t  pad_len = emit_pad(pad, 2, "p", /*h*/ 32, /*ph*/ 0,
                               /*w*/ 32, /*pw*/ 0, /*f*/ 0, /*pf*/ 0);

    uint8_t op[64];
    size_t  op_len = 0;
    emit_lenprefix(op, &op_len, 2, pad, pad_len);

    uint8_t ng[128];
    size_t  ng_len = 0;
    emit_lenprefix(ng, &ng_len, 8, op, op_len);

    uint8_t blob[256];
    size_t  olen = 0;
    emit_lenprefix(blob, &olen, 2, ng, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_TRUE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(32, info.pads[0].height);
    TEST_ASSERT_EQUAL_UINT32(32, info.pads[0].width);
    TEST_ASSERT_EQUAL_UINT32(0,  info.pads[0].features);
    TEST_ASSERT_EQUAL_UINT32(0,  info.pads[0].padded_height);
    TEST_ASSERT_EQUAL_UINT32(0,  info.pads[0].padded_width);
    TEST_ASSERT_EQUAL_UINT32(0,  info.pads[0].padded_features);
}

static void test_decode_empty_tensor_shape(void)
{
    /* Pad carries an empty tensor_shape sub-message (zero body bytes).
     * Wire: pad { index=3; tensor_shape: <empty length-prefix> }. */
    uint8_t pad[16];
    size_t  pad_len = 0;
    emit_varint_field(pad, &pad_len, 1, 3);
    emit_lenprefix(pad, &pad_len, /*6=tensor_shape*/ 6, NULL, 0);

    uint8_t op[32];
    size_t  op_len = 0;
    emit_lenprefix(op, &op_len, 3, pad, pad_len);  /* output_pads */

    uint8_t ng[64];
    size_t  ng_len = 0;
    emit_lenprefix(ng, &ng_len, 8, op, op_len);

    uint8_t blob[128];
    size_t  olen = 0;
    emit_lenprefix(blob, &olen, 2, ng, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    /* has_tensor_shape flips true the moment the sub-message is seen,
     * regardless of whether it carries any dims. All dims stay zero. */
    TEST_ASSERT_TRUE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(0, info.pads[0].height);
    TEST_ASSERT_EQUAL_UINT32(0, info.pads[0].width);
    TEST_ASSERT_EQUAL_UINT32(0, info.pads[0].features);
}

static void test_decode_op_without_pads(void)
{
    /* Op with no input_pads and no output_pads — op_count bumps to 1
     * but pad_count stays 0. Guards against a regression where the
     * op walker mistakenly initialized a pad slot from a field it
     * wasn't actually given. */
    uint8_t op[32];
    size_t  op_len = 0;
    emit_lenprefix(op, &op_len, 1, (const uint8_t *)"bare", 4);

    uint8_t ng[64];
    size_t  ng_len = 0;
    emit_lenprefix(ng, &ng_len, 8, op, op_len);

    uint8_t blob[128];
    size_t  olen = 0;
    emit_lenprefix(blob, &olen, 2, ng, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.op_count);
    TEST_ASSERT_EQUAL_UINT32(0, info.pad_count);
    TEST_ASSERT_FALSE(info.pads_truncated);
}

static void test_decode_tensor_shape_rejects_oversize_dim(void)
{
    /* A shape dim > UINT32_MAX must fail the decode (decode_tensor_
     * shape_cb's v > UINT32_MAX guard). Emit height as a 10-byte
     * varint with the high bit set so it exceeds 32-bit range. */
    uint8_t shape[16];
    size_t  slen = 0;
    /* Tag for field 1, wire-type 0 = 0x08 */
    shape[slen++] = 0x08;
    /* Varint encoding of 0x1_0000_0000 (2^32): needs 5 bytes:
     *   byte0 = (bits 0..6)  | 0x80 = 0x80
     *   byte1 = (bits 7..13) | 0x80 = 0x80
     *   byte2 = (bits 14..20)| 0x80 = 0x80
     *   byte3 = (bits 21..27)| 0x80 = 0x80
     *   byte4 = (bits 28..34)         = 0x10
     * 0x10 = decimal 16 = bit 32 set = 2^32. */
    shape[slen++] = 0x80;
    shape[slen++] = 0x80;
    shape[slen++] = 0x80;
    shape[slen++] = 0x80;
    shape[slen++] = 0x10;

    uint8_t pad[32];
    size_t  pad_len = 0;
    emit_varint_field(pad, &pad_len, 1, 0);
    emit_lenprefix(pad, &pad_len, 6, shape, slen);

    uint8_t op[64];
    size_t  op_len = 0;
    emit_lenprefix(op, &op_len, 2, pad, pad_len);

    uint8_t ng[128];
    size_t  ng_len = 0;
    emit_lenprefix(ng, &ng_len, 8, op, op_len);

    uint8_t blob[256];
    size_t  olen = 0;
    emit_lenprefix(blob, &olen, 2, ng, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_ERR_DECODE,
                          hef_parse_body(blob, olen, &info));
}

static void test_decode_tensor_shape_ignores_unknown_field(void)
{
    /* A future Hailo schema could add field 7 (or higher) to
     * ProtoHEFTensorShape. Our decoder must skip it gracefully so
     * older SLM-OS kernels parse future HEFs. Field 7 as varint, any
     * value, followed by a known field (height) proves the walker
     * resumed correctly after the unknown. */
    uint8_t shape[16];
    size_t  slen = 0;
    emit_varint_field(shape, &slen, 7, 0xABCD);   /* unknown future field */
    emit_varint_field(shape, &slen, 1, 128);      /* height */

    uint8_t pad[32];
    size_t  pad_len = 0;
    emit_varint_field(pad, &pad_len, 1, 42);
    emit_lenprefix(pad, &pad_len, 6, shape, slen);

    uint8_t op[64];
    size_t  op_len = 0;
    emit_lenprefix(op, &op_len, 2, pad, pad_len);

    uint8_t ng[128];
    size_t  ng_len = 0;
    emit_lenprefix(ng, &ng_len, 8, op, op_len);

    uint8_t blob[256];
    size_t  olen = 0;
    emit_lenprefix(blob, &olen, 2, ng, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_TRUE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(128, info.pads[0].height);
    TEST_ASSERT_EQUAL_UINT32(42, info.pads[0].index);
}

/*
 * Build a ProtoHEFHef blob whose single tensor_shape sub-message has
 * `shape_bytes` as its body, wrapped in the NG/Op/Pad chain. Shared
 * harness for the three unknown-wire-type skip tests below. Callers
 * pass a buffer sized >= 256 B — the intermediate `ng[256]` bound
 * is the effective ceiling on the blob we emit.
 */
static size_t wrap_tensor_shape_body(uint8_t *out,
                                     const uint8_t *shape_bytes,
                                     size_t shape_len)
{
    uint8_t pad[64];
    size_t  pad_len = 0;
    emit_varint_field(pad, &pad_len, 1, 1);  /* pad index */
    emit_lenprefix(pad, &pad_len, 6, shape_bytes, shape_len);

    uint8_t op[128];
    size_t  op_len = 0;
    emit_lenprefix(op, &op_len, 2, pad, pad_len);  /* input_pads */

    uint8_t ng[256];
    size_t  ng_len = 0;
    emit_lenprefix(ng, &ng_len, 8, op, op_len);    /* ops */

    size_t olen = 0;
    emit_lenprefix(out, &olen, 2, ng, ng_len);     /* network_groups */
    return olen;
}

static void test_decode_tensor_shape_skips_fixed64_unknown_field(void)
{
    /* Future tag=7 field with wire_type=1 (fixed64, 8 B payload)
     * sandwiched between known height and features. Both knowns
     * must survive the skip. Payload bytes are all 0xAA so a casual
     * reader doesn't mistake any one of them for a valid proto tag. */
    uint8_t shape[32];
    size_t  slen = 0;
    emit_varint_field(shape, &slen, 1, 64);            /* height */
    emit_tag(shape, &slen, /*field*/ 7, /*wire_type*/ 1);
    for (int i = 0; i < 8; i++) shape[slen++] = 0xAA;
    emit_varint_field(shape, &slen, 5, 3);             /* features */

    uint8_t blob[256];
    size_t  olen = wrap_tensor_shape_body(blob, shape, slen);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_TRUE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(64, info.pads[0].height);
    TEST_ASSERT_EQUAL_UINT32(3,  info.pads[0].features);
}

static void test_decode_tensor_shape_skips_fixed32_unknown_field(void)
{
    /* Future tag=7 field with wire_type=5 (fixed32, 4 B payload)
     * sandwiched between known width and padded_features. */
    uint8_t shape[32];
    size_t  slen = 0;
    emit_varint_field(shape, &slen, 3, 224);           /* width */
    emit_tag(shape, &slen, /*field*/ 7, /*wire_type*/ 5);
    /* 4 bytes of arbitrary payload. */
    shape[slen++] = 0xDE;
    shape[slen++] = 0xAD;
    shape[slen++] = 0xBE;
    shape[slen++] = 0xEF;
    emit_varint_field(shape, &slen, 6, 8);             /* padded_features */

    uint8_t blob[256];
    size_t  olen = wrap_tensor_shape_body(blob, shape, slen);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_TRUE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(224, info.pads[0].width);
    TEST_ASSERT_EQUAL_UINT32(8,   info.pads[0].padded_features);
}

static void test_decode_tensor_shape_rejects_group_wire_type(void)
{
    /* Wire types 3 and 4 are the deprecated proto2 "start group" /
     * "end group" markers. No modern Hailo HEF should use either;
     * the decoder must reject the message rather than try to
     * interpret groups. Test BOTH to guard the whole reject arm. */
    struct hef_info info;

    /* Wire type 3 (start group). */
    {
        uint8_t shape[16];
        size_t  slen = 0;
        emit_varint_field(shape, &slen, 1, 42);        /* height */
        emit_tag(shape, &slen, /*field*/ 7, /*wire_type*/ 3);
        uint8_t blob[256];
        size_t  olen = wrap_tensor_shape_body(blob, shape, slen);
        TEST_ASSERT_EQUAL_INT(HEF_PARSER_ERR_DECODE,
                              hef_parse_body(blob, olen, &info));
    }

    /* Wire type 4 (end group). */
    {
        uint8_t shape[16];
        size_t  slen = 0;
        emit_varint_field(shape, &slen, 1, 42);
        emit_tag(shape, &slen, 7, 4);
        uint8_t blob[256];
        size_t  olen = wrap_tensor_shape_body(blob, shape, slen);
        TEST_ASSERT_EQUAL_INT(HEF_PARSER_ERR_DECODE,
                              hef_parse_body(blob, olen, &info));
    }
}

static void test_decode_tensor_shape_skips_non_varint_unknown_field(void)
{
    /* Companion to the varint-unknown test: a future non-varint
     * field inside TensorShape (e.g. a length-delimited bytes field
     * at tag 7) must be skipped per-field, NOT cause the walker to
     * abandon the rest of the sub-message. The test places the
     * unknown length-delimited field BETWEEN two known varint dims
     * (height, then unknown_lenprefix, then features). Before the
     * fix this test would have captured height but dropped features. */
    uint8_t shape[32];
    size_t  slen = 0;
    emit_varint_field(shape, &slen, 1, 64);                  /* height */
    /* Emit tag=7, wire_type=2 (length-delimited) with a 3-byte payload. */
    const uint8_t payload[3] = { 0xAA, 0xBB, 0xCC };
    emit_lenprefix(shape, &slen, /*field 7*/ 7, payload, sizeof(payload));
    emit_varint_field(shape, &slen, 5, 3);                   /* features */

    uint8_t pad[32];
    size_t  pad_len = 0;
    emit_varint_field(pad, &pad_len, 1, 1);
    emit_lenprefix(pad, &pad_len, 6, shape, slen);

    uint8_t op[64];
    size_t  op_len = 0;
    emit_lenprefix(op, &op_len, 2, pad, pad_len);

    uint8_t ng[128];
    size_t  ng_len = 0;
    emit_lenprefix(ng, &ng_len, 8, op, op_len);

    uint8_t blob[256];
    size_t  olen = 0;
    emit_lenprefix(blob, &olen, 2, ng, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_TRUE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(64, info.pads[0].height);
    TEST_ASSERT_EQUAL_UINT32(3,  info.pads[0].features);
}

static void test_decode_pad_name_truncation(void)
{
    /* A pad name longer than HEF_PARSER_MAX_PAD_NAME-1 bytes must
     * be truncated with NUL terminator and set string_truncated. */
    char long_name[HEF_PARSER_MAX_PAD_NAME + 16];
    memset(long_name, 'P', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    uint8_t pad[128];
    size_t  pad_len = 0;
    emit_varint_field(pad, &pad_len, 1, 1);  /* index */
    emit_lenprefix(pad, &pad_len, 2,
                   (const uint8_t *)long_name, strlen(long_name));

    uint8_t op[256];
    size_t  op_len = 0;
    emit_lenprefix(op, &op_len, 2, pad, pad_len);  /* input_pads */

    uint8_t ng[512];
    size_t  ng_len = 0;
    emit_lenprefix(ng, &ng_len, 8, op, op_len);

    uint8_t blob[1024];
    size_t  olen = 0;
    emit_lenprefix(blob, &olen, 2, ng, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK, hef_parse_body(blob, olen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_TRUE(info.string_truncated);
    TEST_ASSERT_EQUAL_HEX8(0,
        (uint8_t)info.pads[0].name[HEF_PARSER_MAX_PAD_NAME - 1]);
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

    /* Phase 5.1 tensor-metadata tests */
    RUN_TEST(test_decode_pad_with_tensor_shape);
    RUN_TEST(test_decode_multi_pad_network_group);
    RUN_TEST(test_decode_pads_truncated);
    RUN_TEST(test_decode_pad_without_tensor_shape);
    RUN_TEST(test_decode_pad_with_nms_shape_leaves_tensor_fields_empty);
    RUN_TEST(test_decode_second_network_group_pads_ignored);
    RUN_TEST(test_decode_partial_tensor_shape);
    RUN_TEST(test_decode_empty_tensor_shape);
    RUN_TEST(test_decode_op_without_pads);
    RUN_TEST(test_decode_tensor_shape_rejects_oversize_dim);
    RUN_TEST(test_decode_tensor_shape_ignores_unknown_field);
    RUN_TEST(test_decode_tensor_shape_skips_non_varint_unknown_field);
    RUN_TEST(test_decode_tensor_shape_skips_fixed64_unknown_field);
    RUN_TEST(test_decode_tensor_shape_skips_fixed32_unknown_field);
    RUN_TEST(test_decode_tensor_shape_rejects_group_wire_type);
    RUN_TEST(test_decode_pad_name_truncation);

    return UnityEnd();
}
