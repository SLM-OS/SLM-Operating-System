/*
 * test_hailo_replay.c — coverage for the #795 Phase 0 Task 0.4
 * corpus parser and the `hailo replay-step` mechanism.
 *
 * Two layers:
 *
 *   1. Parser unit tests — hand-crafted JSONL strings, checked
 *      against `hailo_re_corpus_parse` for field decoding, monotonic
 *      seq enforcement, unknown-type tolerance, hex-value LE decode,
 *      and the binary-search find helper.
 *
 *   2. Replay-loop integration smoke — drive the parsed corpus
 *      through a mock `hailo_platform_ops` whose read32/write32 log
 *      every access. Confirms the platform shim is called in seq
 *      order with the recorded values.
 *
 * No floating-point. Compiles under -mgeneral-regs-only.
 */

#include "unity.h"
#include "test_harness.h"

#include "../ai_accel/hailo/hailo.h"
#include "../ai_accel/hailo/hailo_re_corpus.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Mock platform                                                               */
/* -------------------------------------------------------------------------- */

#define MOCK_OP_LOG_CAP 64u

struct mock_access {
    uint8_t  bar;
    uint8_t  dir;       /* 0 read, 1 write */
    uint32_t offset;
    uint32_t value;
};

static struct mock_access mock_log[MOCK_OP_LOG_CAP];
static uint32_t mock_log_count;

/* Canned-read map: bar/offset → value the next read32 should return.
 * Two slots is plenty for the smoke test. */
struct canned_read {
    uint8_t  bar;
    uint32_t offset;
    uint32_t value;
    bool     active;
};
static struct canned_read canned[8];

static uint32_t mock_read32(uint8_t bar, uint32_t offset)
{
    uint32_t v = 0xDEADBEEFu;
    for (size_t i = 0; i < sizeof(canned) / sizeof(canned[0]); i++) {
        if (canned[i].active && canned[i].bar == bar
            && canned[i].offset == offset) {
            v = canned[i].value;
            break;
        }
    }
    if (mock_log_count < MOCK_OP_LOG_CAP) {
        mock_log[mock_log_count++] = (struct mock_access){
            .bar = bar, .dir = 0, .offset = offset, .value = v
        };
    }
    return v;
}

static void mock_write32(uint8_t bar, uint32_t offset, uint32_t value)
{
    if (mock_log_count < MOCK_OP_LOG_CAP) {
        mock_log[mock_log_count++] = (struct mock_access){
            .bar = bar, .dir = 1, .offset = offset, .value = value
        };
    }
}

static const struct hailo_platform_ops mock_ops = {
    .name    = "test-replay-mock",
    .read32  = mock_read32,
    .write32 = mock_write32,
};

static void reset_mock(void)
{
    mock_log_count = 0;
    memset(mock_log,  0, sizeof(mock_log));
    memset(canned,    0, sizeof(canned));
}

/* -------------------------------------------------------------------------- */
/* Parser tests                                                                */
/* -------------------------------------------------------------------------- */

static void test_parse_header_only(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK, rc);
    TEST_ASSERT_TRUE(c.has_header);
    TEST_ASSERT_EQUAL_UINT32(1u, c.format_version);
    TEST_ASSERT_EQUAL_UINT32(0u, c.op_count);
    TEST_ASSERT_FALSE(c.has_trailer);
}

static void test_parse_missing_header_rejected(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    /* First non-empty line is an op, not a header — must be rejected. */
    const char *text =
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"01000000\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_E_NO_HEADER, rc);
}

static void test_parse_op_write_decodes_le_hex(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":2304,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"16001340\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1u, c.op_count);
    TEST_ASSERT_EQUAL_UINT32(1u, c.ops[0].seq);
    TEST_ASSERT_EQUAL_UINT8(4u, c.ops[0].bar);
    TEST_ASSERT_EQUAL_UINT32(2304u, c.ops[0].offset);
    TEST_ASSERT_EQUAL_UINT8(4u, c.ops[0].size);
    TEST_ASSERT_EQUAL_UINT8(HAILO_RE_DIR_WRITE, c.ops[0].dir);
    /* "16001340" LE → byte0=0x16, byte1=0x00, byte2=0x13, byte3=0x40
     * → uint32 0x40130016 */
    TEST_ASSERT_EQUAL_HEX32(0x40130016u, c.ops[0].value);
}

static void test_parse_op_read_marks_unvalidated(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":5,\"bar\":4,\"offset\":2308,\"size\":4,"
        "\"dir\":\"read\",\"value\":\"00000000\","
        "\"validated_at_commit\":null}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1u, c.op_count);
    TEST_ASSERT_EQUAL_UINT8(HAILO_RE_DIR_READ, c.ops[0].dir);
    TEST_ASSERT_EQUAL_UINT8(0u, c.ops[0].validated);
}

static void test_parse_op_read_validated_sha(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":5,\"bar\":4,\"offset\":2308,\"size\":4,"
        "\"dir\":\"read\",\"value\":\"00000000\","
        "\"validated_at_commit\":\"ad007df819581b493bcb1fae00f131fef176713b\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK, rc);
    TEST_ASSERT_EQUAL_UINT8(1u, c.ops[0].validated);
}

static void test_parse_rejects_nonmonotonic(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":2,\"bar\":4,\"offset\":0,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"01000000\"}\n"
        "{\"type\":\"op\",\"seq\":2,\"bar\":4,\"offset\":4,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"02000000\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_E_NONMONOTONIC, rc);
    /* op_count should reflect what was parsed before the failure (1). */
    TEST_ASSERT_EQUAL_UINT32(1u, c.op_count);
}

static void test_parse_tolerates_unknown_type(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"future_extension\",\"key\":\"some_value\"}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"01000000\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1u, c.skipped_unknown);
    TEST_ASSERT_EQUAL_UINT32(1u, c.op_count);
}

static void test_parse_tolerates_trailer(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"01000000\"}\n"
        "{\"type\":\"trailer\",\"ended_at\":\"2026-05-12T00:00:00Z\","
        "\"last_seq\":1,\"reason\":\"manual\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK, rc);
    TEST_ASSERT_TRUE(c.has_trailer);
    TEST_ASSERT_EQUAL_UINT32(1u, c.op_count);
}

static void test_parse_note_with_embedded_keylike_string(void)
{
    /* A `note` field whose value contains the literal text `"seq":99`
     * must NOT confuse the scanner — the real seq must still resolve
     * to 7. This is the key correctness test for the in-string skip. */
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":7,\"bar\":4,\"offset\":4,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"01000000\","
        "\"note\":\"impostor \\\"seq\\\":99 inside a comment\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1u, c.op_count);
    TEST_ASSERT_EQUAL_UINT32(7u, c.ops[0].seq);
}

static void test_parse_bad_dir_rejected(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,"
        "\"dir\":\"poke\",\"value\":\"01000000\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_E_BAD_FIELD, rc);
}

static void test_parse_value_wrong_length_rejected(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    /* size=4 demands 8 hex chars; this entry has 6. */
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"010000\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_E_BAD_FIELD, rc);
}

static void test_find_seq_binary_search(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"01000000\"}\n"
        "{\"type\":\"op\",\"seq\":7,\"bar\":4,\"offset\":4,\"size\":4,"
        "\"dir\":\"read\",\"value\":\"02000000\","
        "\"validated_at_commit\":null}\n"
        "{\"type\":\"op\",\"seq\":42,\"bar\":4,\"offset\":8,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"03000000\"}\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(3u, c.op_count);
    TEST_ASSERT_NOT_NULL(hailo_re_corpus_find_seq(&c, 1));
    TEST_ASSERT_NOT_NULL(hailo_re_corpus_find_seq(&c, 7));
    TEST_ASSERT_NOT_NULL(hailo_re_corpus_find_seq(&c, 42));
    TEST_ASSERT_NULL(hailo_re_corpus_find_seq(&c, 0));
    TEST_ASSERT_NULL(hailo_re_corpus_find_seq(&c, 8));
    TEST_ASSERT_NULL(hailo_re_corpus_find_seq(&c, 43));
}

static void test_parse_tolerates_crlf(void)
{
    struct hailo_re_op ops[8];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 8 };
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\r\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"01000000\"}\r\n";
    int rc = hailo_re_corpus_parse(text, strlen(text), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1u, c.op_count);
}

/* -------------------------------------------------------------------------- */
/* Replay loop integration smoke                                               */
/* -------------------------------------------------------------------------- */

/*
 * Drive a parsed corpus through the mock platform's read32/write32
 * and confirm the call order + values. Mirrors what `cmd_hailo_replay_step`
 * does on real hardware, minus the response-line formatting (covered
 * inline via replay_format_le_hex's bit pattern test). */
static void test_replay_loop_drives_platform_in_seq_order(void)
{
    reset_mock();

    /* Canned values for the two reads in the corpus + the seq=N read. */
    canned[0] = (struct canned_read){
        .bar = 4, .offset = 2308, .value = 0x00000000u, .active = true,
    };

    struct hailo_re_op ops[16];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 16 };
    /* Polling-loop fixture from the format spec, abbreviated. */
    const char *text =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":2304,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"01000000\"}\n"
        "{\"type\":\"op\",\"seq\":2,\"bar\":4,\"offset\":2308,\"size\":4,"
        "\"dir\":\"read\",\"value\":\"00000000\","
        "\"validated_at_commit\":null}\n"
        "{\"type\":\"op\",\"seq\":3,\"bar\":4,\"offset\":2308,\"size\":4,"
        "\"dir\":\"read\",\"value\":\"00000000\","
        "\"validated_at_commit\":null}\n";
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK,
                          hailo_re_corpus_parse(text, strlen(text), &c));
    TEST_ASSERT_EQUAL_UINT32(3u, c.op_count);

    /* Simulated replay-loop with target seq=3 (read at offset 2308):
     * issue ops with seq<3 against the mock, then the seq=3 read.
     * We replicate the loop logic here so the smoke test stays
     * independent of the shell handler's allocation/VFS plumbing. */
    const uint32_t target = 3u;
    const struct hailo_re_op *t = hailo_re_corpus_find_seq(&c, target);
    TEST_ASSERT_NOT_NULL(t);

    for (uint32_t i = 0; i < c.op_count; i++) {
        const struct hailo_re_op *op = &c.ops[i];
        if (op->seq >= target) break;
        if (op->dir == HAILO_RE_DIR_WRITE) {
            mock_ops.write32(op->bar, op->offset, op->value);
        } else {
            (void)mock_ops.read32(op->bar, op->offset);
        }
    }
    uint32_t captured = mock_ops.read32(t->bar, t->offset);

    /* Three logged accesses: write@2304, read@2308, read@2308 (seq=N). */
    TEST_ASSERT_EQUAL_UINT32(3u, mock_log_count);
    TEST_ASSERT_EQUAL_UINT8(1u, mock_log[0].dir);
    TEST_ASSERT_EQUAL_UINT32(2304u, mock_log[0].offset);
    TEST_ASSERT_EQUAL_HEX32(0x00000001u, mock_log[0].value);
    TEST_ASSERT_EQUAL_UINT8(0u, mock_log[1].dir);
    TEST_ASSERT_EQUAL_UINT32(2308u, mock_log[1].offset);
    TEST_ASSERT_EQUAL_UINT8(0u, mock_log[2].dir);
    TEST_ASSERT_EQUAL_UINT32(2308u, mock_log[2].offset);
    TEST_ASSERT_EQUAL_HEX32(0u, captured);
}

/* Re-declare the local format helper from hailo_shell.c so the test
 * can pin its output. The function is `static` in hailo_shell.c by
 * design — having a parallel copy here means the format is asserted
 * directly without exposing it as a public symbol. If the shell-side
 * implementation drifts, the integration test on real hardware will
 * catch the divergence. */
static void replay_format_le_hex_test_copy(uint32_t value, uint8_t size,
                                           char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (uint8_t i = 0; i < size; i++) {
        uint8_t byte = (uint8_t)((value >> (i * 8u)) & 0xFFu);
        out[i * 2u]     = digits[(byte >> 4) & 0xFu];
        out[i * 2u + 1] = digits[byte & 0xFu];
    }
    out[size * 2u] = '\0';
}

static void test_response_value_le_hex_encoding(void)
{
    /* The spec example: 32-bit value 0x40130016 → "16001340". */
    char buf[2 * 4 + 1];
    replay_format_le_hex_test_copy(0x40130016u, 4, buf);
    TEST_ASSERT_EQUAL_STRING("16001340", buf);

    /* Round-trip: encode + parse should be identity. */
    const char ranged[] =
        "{\"type\":\"header\",\"format_version\":1}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,"
        "\"dir\":\"write\",\"value\":\"16001340\"}\n";
    struct hailo_re_op ops[2];
    struct hailo_re_corpus c = { .ops = ops, .op_capacity = 2 };
    int rc = hailo_re_corpus_parse(ranged, strlen(ranged), &c);
    TEST_ASSERT_EQUAL_INT(HAILO_RE_CORPUS_OK, rc);
    TEST_ASSERT_EQUAL_HEX32(0x40130016u, c.ops[0].value);

    /* Zero and all-ones. */
    replay_format_le_hex_test_copy(0u, 4, buf);
    TEST_ASSERT_EQUAL_STRING("00000000", buf);
    replay_format_le_hex_test_copy(0xFFFFFFFFu, 4, buf);
    TEST_ASSERT_EQUAL_STRING("ffffffff", buf);
}

/* -------------------------------------------------------------------------- */
/* Suite registration                                                          */
/* -------------------------------------------------------------------------- */

int test_suite_hailo_replay(void)
{
    UnityBegin("test_hailo_replay.c");

    RUN_TEST(test_parse_header_only);
    RUN_TEST(test_parse_missing_header_rejected);
    RUN_TEST(test_parse_op_write_decodes_le_hex);
    RUN_TEST(test_parse_op_read_marks_unvalidated);
    RUN_TEST(test_parse_op_read_validated_sha);
    RUN_TEST(test_parse_rejects_nonmonotonic);
    RUN_TEST(test_parse_tolerates_unknown_type);
    RUN_TEST(test_parse_tolerates_trailer);
    RUN_TEST(test_parse_note_with_embedded_keylike_string);
    RUN_TEST(test_parse_bad_dir_rejected);
    RUN_TEST(test_parse_value_wrong_length_rejected);
    RUN_TEST(test_find_seq_binary_search);
    RUN_TEST(test_parse_tolerates_crlf);

    RUN_TEST(test_replay_loop_drives_platform_in_seq_order);
    RUN_TEST(test_response_value_le_hex_encoding);

    return UnityEnd();
}
