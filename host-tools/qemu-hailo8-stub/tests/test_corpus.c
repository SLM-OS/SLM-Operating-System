/*
 * Unit tests for the hailo8_corpus module.
 *
 * Build + run:
 *   cd host-tools/qemu-hailo8-stub
 *   make -C tests
 */

#include "../src/hailo8_corpus.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail_count;
static int g_test_count;

#define TEST(name)  do { g_test_count++; \
    fprintf(stderr, "[test] %-50s ", #name); \
    if (name()) { fprintf(stderr, "OK\n"); } \
    else        { fprintf(stderr, "FAIL\n"); g_fail_count++; } \
    } while (0)

#define EXPECT(cond) do { if (!(cond)) { \
    fprintf(stderr, "\n    EXPECT failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
    return 0; } } while (0)

#define EXPECT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "\n    EXPECT_EQ failed: %s=%lld vs %s=%lld (%s:%d)\n", \
                #a, _a, #b, _b, __FILE__, __LINE__); \
        return 0; } } while (0)

#define EXPECT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "\n    EXPECT_STR_EQ failed: '%s' vs '%s' (%s:%d)\n", \
                (a), (b), __FILE__, __LINE__); \
        return 0; } } while (0)

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static char tmp_path[256];

static void make_tmp(void)
{
    strcpy(tmp_path, "/tmp/hailo8_corpus_test_XXXXXX");
    int fd = mkstemp(tmp_path);
    assert(fd >= 0);
    close(fd);
}

static void write_file(const char *path, const char *contents)
{
    FILE *fp = fopen(path, "w");
    assert(fp);
    fputs(contents, fp);
    fclose(fp);
}

/* -------------------------------------------------------------------------- */
/* hex round-trip                                                              */
/* -------------------------------------------------------------------------- */

static int test_hex_roundtrip_u32(void)
{
    char buf[16];
    EXPECT_EQ(hailo_value_to_hex(0x40130016, 4, buf, sizeof(buf)), 0);
    EXPECT_STR_EQ(buf, "16001340");
    uint64_t back = 0;
    EXPECT_EQ(hailo_value_from_hex(buf, 4, &back), 0);
    EXPECT_EQ(back, 0x40130016);
    return 1;
}

static int test_hex_roundtrip_u8_u16_u64(void)
{
    char buf[32];
    uint64_t back;

    EXPECT_EQ(hailo_value_to_hex(0xAB, 1, buf, sizeof(buf)), 0);
    EXPECT_STR_EQ(buf, "ab");
    EXPECT_EQ(hailo_value_from_hex(buf, 1, &back), 0);
    EXPECT_EQ(back, 0xAB);

    EXPECT_EQ(hailo_value_to_hex(0x1234, 2, buf, sizeof(buf)), 0);
    EXPECT_STR_EQ(buf, "3412");
    EXPECT_EQ(hailo_value_from_hex(buf, 2, &back), 0);
    EXPECT_EQ(back, 0x1234);

    EXPECT_EQ(hailo_value_to_hex(0x0102030405060708ULL, 8, buf, sizeof(buf)), 0);
    EXPECT_STR_EQ(buf, "0807060504030201");
    EXPECT_EQ(hailo_value_from_hex(buf, 8, &back), 0);
    EXPECT_EQ(back, 0x0102030405060708ULL);
    return 1;
}

static int test_hex_rejects_bad_size(void)
{
    char buf[16];
    EXPECT_EQ(hailo_value_to_hex(0, 3, buf, sizeof(buf)), -1);
    uint64_t v;
    EXPECT_EQ(hailo_value_from_hex("ab", 3, &v), -1);
    EXPECT_EQ(hailo_value_from_hex("abc", 2, &v), -1);   /* wrong length */
    EXPECT_EQ(hailo_value_from_hex("xyzz", 2, &v), -1);  /* bad hex */
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Loader                                                                      */
/* -------------------------------------------------------------------------- */

static int test_load_minimal_corpus(void)
{
    make_tmp();
    write_file(tmp_path,
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\",\"fw_version\":\"4.23.0\",\"capture_host\":\"qemu-x86_64\",\"slmos_base_sha\":\"abc\",\"capture_started_at\":\"2026-05-12T00:00:00Z\"}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":2304,\"size\":4,\"dir\":\"write\",\"value\":\"01000000\",\"source\":\"qemu_capture\",\"validated_at_commit\":null,\"validated_at\":null}\n"
        "{\"type\":\"op\",\"seq\":2,\"bar\":4,\"offset\":2308,\"size\":4,\"dir\":\"read\",\"value\":\"00000000\",\"source\":\"slmos_observed\",\"validated_at_commit\":\"deadbeef\",\"validated_at\":\"2026-05-12T01:00:00Z\"}\n"
        "{\"type\":\"op\",\"seq\":3,\"bar\":4,\"offset\":2308,\"size\":4,\"dir\":\"read\",\"value\":\"01000000\",\"source\":\"slmos_observed\",\"validated_at_commit\":\"deadbeef\",\"validated_at\":\"2026-05-12T01:00:00Z\"}\n");

    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    if (!c) {
        fprintf(stderr, "load failed: %s\n", err);
        return 0;
    }
    EXPECT_EQ(c->n_entries, 3);
    EXPECT_EQ(c->max_seq, 3);
    EXPECT_EQ(c->format_version, 1);
    EXPECT_STR_EQ(c->hailort_version, "4.23.0");

    const hailo_op_entry_t *e1 = hailo_corpus_get(c, 1);
    EXPECT(e1 != NULL);
    EXPECT_EQ(e1->bar, 4);
    EXPECT_EQ(e1->offset, 2304);
    EXPECT_EQ(e1->is_write, true);
    EXPECT_EQ(e1->value, 0x00000001);

    const hailo_op_entry_t *e2 = hailo_corpus_get(c, 2);
    EXPECT(e2 != NULL);
    EXPECT_EQ(e2->is_write, false);
    EXPECT_EQ(e2->value, 0x00000000);

    const hailo_op_entry_t *e3 = hailo_corpus_get(c, 3);
    EXPECT(e3 != NULL);
    EXPECT_EQ(e3->value, 0x00000001);

    /* Polling-loop semantics: same offset, different values per seq. */
    EXPECT_EQ(e2->offset, e3->offset);
    EXPECT(e2->value != e3->value);

    EXPECT(hailo_corpus_get(c, 4) == NULL);
    EXPECT(hailo_corpus_get(c, 0) == NULL);

    hailo_corpus_free(c);
    unlink(tmp_path);
    return 1;
}

static int test_load_rejects_duplicate_seq(void)
{
    make_tmp();
    write_file(tmp_path,
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\",\"fw_version\":\"4.23.0\",\"capture_host\":\"qemu\",\"slmos_base_sha\":\"abc\",\"capture_started_at\":\"x\"}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,\"dir\":\"read\",\"value\":\"00000000\",\"source\":\"slmos_observed\",\"validated_at_commit\":null,\"validated_at\":null}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":4,\"size\":4,\"dir\":\"read\",\"value\":\"00000000\",\"source\":\"slmos_observed\",\"validated_at_commit\":null,\"validated_at\":null}\n");
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    EXPECT(strstr(err, "duplicate") != NULL);
    unlink(tmp_path);
    return 1;
}

static int test_load_rejects_bad_dir(void)
{
    make_tmp();
    write_file(tmp_path,
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\",\"fw_version\":\"4.23.0\",\"capture_host\":\"qemu\",\"slmos_base_sha\":\"abc\",\"capture_started_at\":\"x\"}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,\"dir\":\"sideways\",\"value\":\"00000000\",\"source\":\"slmos_observed\",\"validated_at_commit\":null,\"validated_at\":null}\n");
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    unlink(tmp_path);
    return 1;
}

static int test_load_rejects_bad_value_length(void)
{
    make_tmp();
    write_file(tmp_path,
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"x\",\"fw_version\":\"x\",\"capture_host\":\"x\",\"slmos_base_sha\":\"x\",\"capture_started_at\":\"x\"}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,\"dir\":\"read\",\"value\":\"00\",\"source\":\"slmos_observed\",\"validated_at_commit\":null,\"validated_at\":null}\n");
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    unlink(tmp_path);
    return 1;
}

static int test_load_tolerates_unknown_keys(void)
{
    make_tmp();
    write_file(tmp_path,
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"x\",\"fw_version\":\"x\",\"capture_host\":\"x\",\"slmos_base_sha\":\"x\",\"capture_started_at\":\"x\",\"notes\":\"future fields ok\"}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,\"dir\":\"read\",\"value\":\"00000000\",\"source\":\"slmos_observed\",\"validated_at_commit\":null,\"validated_at\":null,\"experimental_field\":42}\n"
        "{\"type\":\"trailer\",\"ended_at\":\"x\",\"last_seq\":1,\"reason\":\"eof\"}\n");
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c != NULL);
    EXPECT_EQ(c->n_entries, 1);
    hailo_corpus_free(c);
    unlink(tmp_path);
    return 1;
}

static int test_load_picks_up_msi_after(void)
{
    make_tmp();
    write_file(tmp_path,
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"x\",\"fw_version\":\"x\",\"capture_host\":\"x\",\"slmos_base_sha\":\"x\",\"capture_started_at\":\"x\"}\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,\"dir\":\"read\",\"value\":\"01000000\",\"source\":\"slmos_observed\",\"validated_at_commit\":null,\"validated_at\":null,\"msi_after\":2}\n");
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c != NULL);
    const hailo_op_entry_t *e = hailo_corpus_get(c, 1);
    EXPECT(e != NULL);
    EXPECT_EQ(e->msi_after_present, true);
    EXPECT_EQ(e->msi_after_vector, 2);
    hailo_corpus_free(c);
    unlink(tmp_path);
    return 1;
}

static int test_load_tolerates_comments_and_blank_lines(void)
{
    make_tmp();
    write_file(tmp_path,
        "# hand-edited fixture\n"
        "\n"
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"x\",\"fw_version\":\"x\",\"capture_host\":\"x\",\"slmos_base_sha\":\"x\",\"capture_started_at\":\"x\"}\n"
        "\n"
        "{\"type\":\"op\",\"seq\":1,\"bar\":0,\"offset\":0,\"size\":4,\"dir\":\"read\",\"value\":\"deadbeef\",\"source\":\"slmos_observed\",\"validated_at_commit\":null,\"validated_at\":null}\n");
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c != NULL);
    EXPECT_EQ(c->n_entries, 1);
    const hailo_op_entry_t *e = hailo_corpus_get(c, 1);
    EXPECT(e != NULL);
    EXPECT_EQ(e->value, 0xefbeaddeULL);
    hailo_corpus_free(c);
    unlink(tmp_path);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Append + inject                                                             */
/* -------------------------------------------------------------------------- */

static int test_append_write_persists(void)
{
    make_tmp();
    write_file(tmp_path,
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"x\",\"fw_version\":\"x\",\"capture_host\":\"x\",\"slmos_base_sha\":\"x\",\"capture_started_at\":\"x\"}\n");
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, true, err, sizeof(err));
    EXPECT(c != NULL);
    EXPECT_EQ(hailo_corpus_append_write(c, 1, 4, 0x900, 4, 0x12345678, err, sizeof(err)), 0);
    EXPECT_EQ(c->n_entries, 1);
    EXPECT_EQ(c->max_seq, 1);
    const hailo_op_entry_t *e = hailo_corpus_get(c, 1);
    EXPECT(e != NULL);
    EXPECT_EQ(e->is_write, true);
    EXPECT_EQ(e->value, 0x12345678);
    hailo_corpus_free(c);

    /* Re-load to confirm the file actually grew. */
    hailo_corpus_t *c2 = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c2 != NULL);
    EXPECT_EQ(c2->n_entries, 1);
    const hailo_op_entry_t *e2 = hailo_corpus_get(c2, 1);
    EXPECT(e2 != NULL);
    EXPECT_EQ(e2->is_write, true);
    EXPECT_EQ(e2->value, 0x12345678);
    EXPECT_STR_EQ(e2->source, "qemu_capture");
    hailo_corpus_free(c2);
    unlink(tmp_path);
    return 1;
}

static int test_inject_no_file(void)
{
    hailo_corpus_t *c = hailo_corpus_new_memory();
    EXPECT(c != NULL);
    hailo_op_entry_t e = {0};
    e.seq = 7;
    e.bar = 2;
    e.offset = 0xdead;
    e.size = 4;
    e.is_write = false;
    e.value = 0xabad1dea;
    char err[256];
    EXPECT_EQ(hailo_corpus_inject_entry(c, &e, err, sizeof(err)), 0);
    const hailo_op_entry_t *got = hailo_corpus_get(c, 7);
    EXPECT(got != NULL);
    EXPECT_EQ(got->value, 0xabad1dea);
    hailo_corpus_free(c);
    return 1;
}

static int test_load_rejects_seq_above_max(void)
{
    /* HAILO_CORPUS_MAX_SEQ guards against malicious huge seq values that
     * would force tens of GB of index allocation. Use one past the cap. */
    char line[512];
    snprintf(line, sizeof(line),
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"x\",\"fw_version\":\"x\",\"capture_host\":\"x\",\"slmos_base_sha\":\"x\",\"capture_started_at\":\"x\"}\n"
        "{\"type\":\"op\",\"seq\":%u,\"bar\":4,\"offset\":0,\"size\":4,\"dir\":\"read\",\"value\":\"00000000\",\"source\":\"slmos_observed\",\"validated_at_commit\":null,\"validated_at\":null}\n",
        (unsigned)HAILO_CORPUS_MAX_SEQ);
    make_tmp();
    write_file(tmp_path, line);
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    EXPECT(strstr(err, "seq must be in") != NULL);
    unlink(tmp_path);
    return 1;
}

static int test_load_rejects_int_overflow(void)
{
    /* 20-digit positive literal — overflows int64 if the parser doesn't guard. */
    make_tmp();
    write_file(tmp_path,
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"x\",\"fw_version\":\"x\",\"capture_host\":\"x\",\"slmos_base_sha\":\"x\",\"capture_started_at\":\"x\"}\n"
        "{\"type\":\"op\",\"seq\":99999999999999999999,\"bar\":4,\"offset\":0,\"size\":4,\"dir\":\"read\",\"value\":\"00000000\",\"source\":\"slmos_observed\",\"validated_at_commit\":null,\"validated_at\":null}\n");
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    unlink(tmp_path);
    return 1;
}

static int test_load_rejects_op_before_header(void)
{
    /* Spec §File layout requires the header before any op lines. */
    make_tmp();
    write_file(tmp_path,
        "{\"type\":\"op\",\"seq\":1,\"bar\":4,\"offset\":0,\"size\":4,\"dir\":\"read\",\"value\":\"00000000\",\"source\":\"slmos_observed\",\"validated_at_commit\":null,\"validated_at\":null}\n"
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"x\",\"fw_version\":\"x\",\"capture_host\":\"x\",\"slmos_base_sha\":\"x\",\"capture_started_at\":\"x\"}\n");
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    EXPECT(strstr(err, "before header") != NULL);
    unlink(tmp_path);
    return 1;
}

static int test_load_handles_short_line(void)
{
    /* line_type_field's boundary check is `p + 6 <= end`; verify we don't
     * crash on lines shorter than the "\"type\"" marker. */
    make_tmp();
    write_file(tmp_path,
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"x\",\"fw_version\":\"x\",\"capture_host\":\"x\",\"slmos_base_sha\":\"x\",\"capture_started_at\":\"x\"}\n"
        "{}\n");
    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    /* Short non-empty line with no `type` should fail cleanly, not crash. */
    EXPECT(c == NULL);
    EXPECT(strstr(err, "no 'type' field") != NULL);
    unlink(tmp_path);
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Region rule (Phase 4)                                                       */
/* -------------------------------------------------------------------------- */

/* Make a tmp file under /tmp with the supplied bytes; returns the path
 * via static storage. The string is short-lived — copy if needed. */
static char tmp_artifact_path[256];
static void write_tmp_artifact(const uint8_t *bytes, size_t n)
{
    strcpy(tmp_artifact_path, "/tmp/hailo8_region_artifact_XXXXXX");
    int fd = mkstemp(tmp_artifact_path);
    assert(fd >= 0);
    ssize_t w = write(fd, bytes, n);
    assert(w == (ssize_t)n);
    close(fd);
}

static int test_region_load_and_lookup(void)
{
    /* 16-byte artifact: 0x10..0x1F. We map BAR4 [0x100, 0x110) -> first
     * 16 bytes of the file (source_offset=0). */
    uint8_t bytes[16];
    for (size_t i = 0; i < 16; i++) {
        bytes[i] = 0x10 + i;
    }
    write_tmp_artifact(bytes, sizeof(bytes));

    make_tmp();
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\",\"fw_version\":\"4.23.0\",\"capture_host\":\"qemu\",\"slmos_base_sha\":\"abc\",\"capture_started_at\":\"2026-05-14T00:00:00Z\"}\n"
        "{\"type\":\"region\",\"bar\":4,\"start\":256,\"end\":272,"
        "\"source_kind\":\"file\",\"source_path\":\"%s\",\"source_offset\":0,"
        "\"validated_at_commit\":null,\"validated_at\":null}\n",
        tmp_artifact_path);
    write_file(tmp_path, buf);

    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    if (!c) {
        fprintf(stderr, "load failed: %s\n", err);
        return 0;
    }
    EXPECT_EQ(c->n_regions, 1);
    EXPECT_EQ(c->regions[0].bar, 4);
    EXPECT_EQ(c->regions[0].start, 256);
    EXPECT_EQ(c->regions[0].end, 272);
    EXPECT_EQ(c->regions[0].data_size, 16);
    EXPECT_EQ(c->regions[0].data[0], 0x10);
    EXPECT_EQ(c->regions[0].data[15], 0x1F);

    /* In-range lookups. */
    const hailo_region_t *r = hailo_corpus_region_lookup(c, 4, 256, 4);
    EXPECT(r != NULL);
    r = hailo_corpus_region_lookup(c, 4, 268, 4);  /* last 4 bytes */
    EXPECT(r != NULL);

    /* Out-of-range: before, after, wrong BAR, partial-overlap-at-end. */
    EXPECT(hailo_corpus_region_lookup(c, 4, 255, 1) == NULL);
    EXPECT(hailo_corpus_region_lookup(c, 4, 272, 1) == NULL);
    EXPECT(hailo_corpus_region_lookup(c, 2, 256, 4) == NULL);
    EXPECT(hailo_corpus_region_lookup(c, 4, 270, 4) == NULL);  /* spans end */

    /* Value decode (little-endian, matches op value semantics). */
    uint64_t v = 0;
    EXPECT_EQ(hailo_region_get_value(&c->regions[0], 256, 4, &v), 0);
    EXPECT_EQ(v, 0x13121110ULL);
    EXPECT_EQ(hailo_region_get_value(&c->regions[0], 256, 1, &v), 0);
    EXPECT_EQ(v, 0x10ULL);
    EXPECT_EQ(hailo_region_get_value(&c->regions[0], 268, 4, &v), 0);
    EXPECT_EQ(v, 0x1F1E1D1CULL);

    hailo_corpus_free(c);
    unlink(tmp_path);
    unlink(tmp_artifact_path);
    return 1;
}

static int test_region_with_source_offset(void)
{
    /* 32-byte file, region maps the second half to BAR4 [0x200, 0x210). */
    uint8_t bytes[32];
    for (size_t i = 0; i < 32; i++) {
        bytes[i] = (uint8_t)(0x80 + i);
    }
    write_tmp_artifact(bytes, sizeof(bytes));

    make_tmp();
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\",\"fw_version\":\"4.23.0\",\"capture_host\":\"qemu\",\"slmos_base_sha\":\"abc\",\"capture_started_at\":\"2026-05-14T00:00:00Z\"}\n"
        "{\"type\":\"region\",\"bar\":4,\"start\":512,\"end\":528,"
        "\"source_kind\":\"file\",\"source_path\":\"%s\",\"source_offset\":16,"
        "\"validated_at_commit\":null,\"validated_at\":null}\n",
        tmp_artifact_path);
    write_file(tmp_path, buf);

    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    if (!c) {
        fprintf(stderr, "load failed: %s\n", err);
        return 0;
    }
    EXPECT_EQ(c->regions[0].data_size, 16);
    /* data[0] should equal bytes[16] = 0x90. */
    EXPECT_EQ(c->regions[0].data[0], 0x90);

    uint64_t v = 0;
    EXPECT_EQ(hailo_region_get_value(&c->regions[0], 512, 4, &v), 0);
    EXPECT_EQ(v, 0x93929190ULL);

    hailo_corpus_free(c);
    unlink(tmp_path);
    unlink(tmp_artifact_path);
    return 1;
}

static int test_region_rejects_overlap(void)
{
    /* Two regions on BAR4 that overlap should be rejected on load. */
    uint8_t bytes[64];
    memset(bytes, 0xAA, sizeof(bytes));
    write_tmp_artifact(bytes, sizeof(bytes));

    make_tmp();
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\",\"fw_version\":\"4.23.0\",\"capture_host\":\"qemu\",\"slmos_base_sha\":\"abc\",\"capture_started_at\":\"2026-05-14T00:00:00Z\"}\n"
        "{\"type\":\"region\",\"bar\":4,\"start\":0,\"end\":32,"
        "\"source_kind\":\"file\",\"source_path\":\"%s\",\"source_offset\":0,"
        "\"validated_at_commit\":null,\"validated_at\":null}\n"
        "{\"type\":\"region\",\"bar\":4,\"start\":16,\"end\":48,"
        "\"source_kind\":\"file\",\"source_path\":\"%s\",\"source_offset\":0,"
        "\"validated_at_commit\":null,\"validated_at\":null}\n",
        tmp_artifact_path, tmp_artifact_path);
    write_file(tmp_path, buf);

    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    unlink(tmp_path);
    unlink(tmp_artifact_path);
    return 1;
}

static int test_region_rejects_bad_source_kind(void)
{
    /* source_kind != "file" must be rejected — schema reserves "inline"
     * etc. for future use but the loader doesn't implement them yet. */
    uint8_t bytes[16];
    memset(bytes, 0x55, sizeof(bytes));
    write_tmp_artifact(bytes, sizeof(bytes));

    make_tmp();
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\",\"fw_version\":\"4.23.0\",\"capture_host\":\"qemu\",\"slmos_base_sha\":\"abc\",\"capture_started_at\":\"2026-05-14T00:00:00Z\"}\n"
        "{\"type\":\"region\",\"bar\":4,\"start\":0,\"end\":16,"
        "\"source_kind\":\"inline\",\"source_path\":\"%s\",\"source_offset\":0,"
        "\"validated_at_commit\":null,\"validated_at\":null}\n",
        tmp_artifact_path);
    write_file(tmp_path, buf);

    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    EXPECT(strstr(err, "source_kind") != NULL);
    unlink(tmp_path);
    unlink(tmp_artifact_path);
    return 1;
}

static int test_region_rejects_missing_file(void)
{
    /* Region pointing at a non-existent source_path must fail loudly
     * at load time, not silently degrade to empty data. */
    make_tmp();
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\",\"fw_version\":\"4.23.0\",\"capture_host\":\"qemu\",\"slmos_base_sha\":\"abc\",\"capture_started_at\":\"2026-05-14T00:00:00Z\"}\n"
        "{\"type\":\"region\",\"bar\":4,\"start\":0,\"end\":16,"
        "\"source_kind\":\"file\",\"source_path\":\"/nonexistent/path/that/should/not/exist\",\"source_offset\":0,"
        "\"validated_at_commit\":null,\"validated_at\":null}\n");
    write_file(tmp_path, buf);

    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    EXPECT(strstr(err, "open") != NULL);
    unlink(tmp_path);
    return 1;
}

static int test_region_rejects_short_read(void)
{
    /* Region asking for more bytes than the artifact actually contains
     * (source_offset + (end-start) > file_size) must fail at load time. */
    uint8_t bytes[8];
    memset(bytes, 0x77, sizeof(bytes));
    write_tmp_artifact(bytes, sizeof(bytes));  /* 8-byte file */

    make_tmp();
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\",\"fw_version\":\"4.23.0\",\"capture_host\":\"qemu\",\"slmos_base_sha\":\"abc\",\"capture_started_at\":\"2026-05-14T00:00:00Z\"}\n"
        /* Region wants 32 bytes from a file that only has 8. */
        "{\"type\":\"region\",\"bar\":4,\"start\":0,\"end\":32,"
        "\"source_kind\":\"file\",\"source_path\":\"%s\",\"source_offset\":0,"
        "\"validated_at_commit\":null,\"validated_at\":null}\n",
        tmp_artifact_path);
    write_file(tmp_path, buf);

    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    EXPECT(strstr(err, "short read") != NULL);
    unlink(tmp_path);
    unlink(tmp_artifact_path);
    return 1;
}

static int test_region_rejects_negative_fields(void)
{
    /* Negative start/end/source_offset must be rejected with a clear
     * diagnostic — silent uint64_t wraparound would let the load proceed
     * with a garbage (huge unsigned) value. */
    uint8_t bytes[16];
    memset(bytes, 0x33, sizeof(bytes));
    write_tmp_artifact(bytes, sizeof(bytes));

    make_tmp();
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"header\",\"format_version\":1,\"hailort_version\":\"4.23.0\",\"fw_version\":\"4.23.0\",\"capture_host\":\"qemu\",\"slmos_base_sha\":\"abc\",\"capture_started_at\":\"2026-05-14T00:00:00Z\"}\n"
        "{\"type\":\"region\",\"bar\":4,\"start\":-1,\"end\":16,"
        "\"source_kind\":\"file\",\"source_path\":\"%s\",\"source_offset\":0,"
        "\"validated_at_commit\":null,\"validated_at\":null}\n",
        tmp_artifact_path);
    write_file(tmp_path, buf);

    char err[256];
    hailo_corpus_t *c = hailo_corpus_load(tmp_path, false, err, sizeof(err));
    EXPECT(c == NULL);
    EXPECT(strstr(err, "start") != NULL);
    unlink(tmp_path);
    unlink(tmp_artifact_path);
    return 1;
}

static int test_region_free_handles_empty(void)
{
    /* In-memory corpus has n_regions=0; hailo_corpus_free must not
     * dereference regions[].data on the empty path. */
    hailo_corpus_t *c = hailo_corpus_new_memory();
    EXPECT(c != NULL);
    EXPECT_EQ(c->n_regions, 0);
    hailo_corpus_free(c);
    return 1;
}

int main(void)
{
    TEST(test_hex_roundtrip_u32);
    TEST(test_hex_roundtrip_u8_u16_u64);
    TEST(test_hex_rejects_bad_size);
    TEST(test_load_minimal_corpus);
    TEST(test_load_rejects_duplicate_seq);
    TEST(test_load_rejects_bad_dir);
    TEST(test_load_rejects_bad_value_length);
    TEST(test_load_tolerates_unknown_keys);
    TEST(test_load_picks_up_msi_after);
    TEST(test_load_tolerates_comments_and_blank_lines);
    TEST(test_load_rejects_seq_above_max);
    TEST(test_load_rejects_int_overflow);
    TEST(test_load_rejects_op_before_header);
    TEST(test_load_handles_short_line);
    TEST(test_append_write_persists);
    TEST(test_inject_no_file);
    TEST(test_region_load_and_lookup);
    TEST(test_region_with_source_offset);
    TEST(test_region_rejects_overlap);
    TEST(test_region_rejects_bad_source_kind);
    TEST(test_region_rejects_missing_file);
    TEST(test_region_rejects_short_read);
    TEST(test_region_rejects_negative_fields);
    TEST(test_region_free_handles_empty);
    fprintf(stderr, "\n%d / %d tests passed\n", g_test_count - g_fail_count, g_test_count);
    return g_fail_count == 0 ? 0 : 1;
}
