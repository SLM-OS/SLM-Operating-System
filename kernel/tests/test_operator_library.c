/*
 * test_operator_library.c - Unit tests for the operator library
 * parser (#663).
 *
 * Synthesizes library blobs in memory using the documented wire
 * format. Real CUDA SASS isn't needed — the parser cares only about
 * sizes and offsets, so the SASS region is just a sequence of
 * recognisable byte patterns we can assert against post-lookup.
 *
 * Coverage:
 *
 *   build_minimal_library:       happy path with one entry
 *   build_three_entries:         multiple entries, lookup hits all
 *   open_rejects_null:           NULL data + NULL out
 *   open_rejects_truncated:      shorter than outer header / payload
 *   open_rejects_bad_magic:      wrong magic
 *   open_rejects_bad_version:    version != 1
 *   open_rejects_bad_schema:     schema_version != 1
 *   open_rejects_reserved:       any reserved field non-zero
 *   open_rejects_bad_checksum:   payload mutated post-checksum
 *   open_rejects_bad_layout:     entry sass_offset+size > region
 *   lookup_misses_unknown:       (op,tier,dtype) not in library
 *   lookup_handles_first_match:  duplicates resolve to first entry
 */

#include "unity.h"
#include "../include/operator_library.h"
#include "../include/gpu_handoff.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- Builders --- */

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u64_le(uint8_t *p, uint64_t v)
{
    put_u32_le(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_u32_le(p + 4, (uint32_t)(v >> 32));
}

/* Build one entry's 32-byte record into `dst`. */
static void build_entry(uint8_t *dst,
                        uint32_t op_kind, uint32_t tier, uint32_t dtype,
                        uint32_t flags,
                        uint64_t sass_offset, uint64_t sass_size)
{
    put_u32_le(dst,      op_kind);
    put_u32_le(dst + 4,  tier);
    put_u32_le(dst + 8,  dtype);
    put_u32_le(dst + 12, flags);
    put_u64_le(dst + 16, sass_offset);
    put_u64_le(dst + 24, sass_size);
}

/* Test-only spec for an entry (named so it can be passed by pointer
 * to build_blob without anonymous-struct-in-parameter-list issues
 * under -Wpedantic). */
struct test_entry_spec {
    uint32_t op_kind;
    uint32_t tier;
    uint32_t dtype;
    size_t   sass_size;
};

/* Lay out a complete library blob into `dst` (size MUST be large
 * enough). Returns total written length. The SASS bytes are simple
 * byte-fills (each entry's region filled with `'A' + entry_idx`). */
static size_t build_blob(uint8_t *dst,
                         const struct test_entry_spec *entries,
                         uint32_t op_count)
{
    /* Lay out outer header (24) + inner header (8) + entries
     * (32 × N) + sass region (sum of sizes). */
    size_t entries_bytes = (size_t)op_count * OPERATOR_LIBRARY_ENTRY_LEN;
    size_t header_total  = OPERATOR_LIBRARY_INNER_HEADER_LEN + entries_bytes;
    size_t sass_total = 0;
    for (uint32_t i = 0; i < op_count; i++) sass_total += entries[i].sass_size;

    size_t payload_len = header_total + sass_total;
    size_t total       = OPERATOR_LIBRARY_OUTER_HEADER_LEN + payload_len;

    /* Outer header — defer checksum until payload is filled. */
    memcpy(dst, "OPLB", 4);
    put_u16_le(dst + 4,  OPERATOR_LIBRARY_VERSION_V1);
    put_u16_le(dst + 6,  0);                          /* kind_id reserved */
    put_u16_le(dst + 8,  OPERATOR_LIBRARY_SCHEMA_V1);
    put_u16_le(dst + 10, 0);                          /* reserved        */
    put_u32_le(dst + 12, (uint32_t)payload_len);
    put_u32_le(dst + 16, 0);                          /* checksum filled below */
    put_u32_le(dst + 20, 0);                          /* reserved        */

    uint8_t *payload = dst + OPERATOR_LIBRARY_OUTER_HEADER_LEN;

    /* Inner header. */
    put_u32_le(payload,     op_count);
    put_u32_le(payload + 4, 0);

    /* Entries + interleaved SASS region tracking. */
    uint8_t *e   = payload + OPERATOR_LIBRARY_INNER_HEADER_LEN;
    uint8_t *ssr = e + entries_bytes;
    uint64_t cursor = 0;
    for (uint32_t i = 0; i < op_count; i++) {
        build_entry(e + (size_t)i * OPERATOR_LIBRARY_ENTRY_LEN,
                    entries[i].op_kind, entries[i].tier, entries[i].dtype,
                    0,
                    cursor, (uint64_t)entries[i].sass_size);
        memset(ssr + (size_t)cursor,
               (int)('A' + i),
               entries[i].sass_size);
        cursor += entries[i].sass_size;
    }

    /* Compute checksum over the populated payload. */
    uint32_t cks = operator_library_checksum32(payload, payload_len);
    put_u32_le(dst + 16, cks);

    return total;
}

/* --- Tests --- */

static void test_open_minimal_library(void)
{
    static uint8_t buf[256];
    const struct test_entry_spec entries[] = {
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 16 },
    };
    size_t total = build_blob(buf, entries, 1);

    struct operator_library lib;
    int rc = operator_library_open(&lib, buf, total);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, lib.op_count);
    TEST_ASSERT_EQUAL_UINT(16, lib.sass_region_len);
}

static void test_open_three_entries_lookup_each(void)
{
    static uint8_t buf[1024];
    const struct test_entry_spec entries[] = {
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 32 },
        { SLM_GPU_OP_CONV2D,       SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 48 },
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_HMMA, SLM_GPU_DTYPE_FP16, 64 },
    };
    size_t total = build_blob(buf, entries, 3);

    struct operator_library lib;
    TEST_ASSERT_EQUAL_INT(0, operator_library_open(&lib, buf, total));
    TEST_ASSERT_EQUAL_UINT32(3, lib.op_count);

    const uint8_t *sass = NULL;
    size_t size = 0;

    /* Hit entry 0 (SIMT GEMM FP32). Region was filled with 'A'. */
    TEST_ASSERT_EQUAL_INT(0,
        operator_library_lookup(&lib,
            SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT,
            SLM_GPU_DTYPE_FP32, &sass, &size));
    TEST_ASSERT_EQUAL_UINT(32, size);
    TEST_ASSERT_EQUAL_HEX8('A', sass[0]);
    TEST_ASSERT_EQUAL_HEX8('A', sass[31]);

    /* Hit entry 1 (Conv2D SIMT FP32). Region was filled with 'B'. */
    TEST_ASSERT_EQUAL_INT(0,
        operator_library_lookup(&lib,
            SLM_GPU_OP_CONV2D, SLM_GPU_TIER_SIMT,
            SLM_GPU_DTYPE_FP32, &sass, &size));
    TEST_ASSERT_EQUAL_UINT(48, size);
    TEST_ASSERT_EQUAL_HEX8('B', sass[0]);
    TEST_ASSERT_EQUAL_HEX8('B', sass[47]);

    /* Hit entry 2 (HMMA GEMM FP16). Region was filled with 'C'. */
    TEST_ASSERT_EQUAL_INT(0,
        operator_library_lookup(&lib,
            SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_HMMA,
            SLM_GPU_DTYPE_FP16, &sass, &size));
    TEST_ASSERT_EQUAL_UINT(64, size);
    TEST_ASSERT_EQUAL_HEX8('C', sass[0]);
    TEST_ASSERT_EQUAL_HEX8('C', sass[63]);
}

static void test_lookup_misses_unknown_combo(void)
{
    static uint8_t buf[256];
    const struct test_entry_spec entries[] = {
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 16 },
    };
    size_t total = build_blob(buf, entries, 1);
    struct operator_library lib;
    TEST_ASSERT_EQUAL_INT(0, operator_library_open(&lib, buf, total));

    /* Wrong op_kind. */
    TEST_ASSERT_EQUAL_INT(-1,
        operator_library_lookup(&lib,
            SLM_GPU_OP_CONV2D, SLM_GPU_TIER_SIMT,
            SLM_GPU_DTYPE_FP32, NULL, NULL));

    /* Wrong tier. */
    TEST_ASSERT_EQUAL_INT(-1,
        operator_library_lookup(&lib,
            SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_HMMA,
            SLM_GPU_DTYPE_FP32, NULL, NULL));

    /* Wrong dtype. */
    TEST_ASSERT_EQUAL_INT(-1,
        operator_library_lookup(&lib,
            SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT,
            SLM_GPU_DTYPE_FP16, NULL, NULL));
}

static void test_open_rejects_null(void)
{
    struct operator_library lib;
    static uint8_t dummy[1] = { 0 };
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_NULL,
        operator_library_open(NULL, dummy, sizeof(dummy)));
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_NULL,
        operator_library_open(&lib, NULL, 0));
}

static void test_open_rejects_truncated(void)
{
    static uint8_t buf[1] = { 'O' };
    struct operator_library lib;
    /* Shorter than outer header. */
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_TRUNC,
        operator_library_open(&lib, buf, sizeof(buf)));

    /* Outer header present but len doesn't match payload_len. */
    static uint8_t big[256];
    const struct test_entry_spec entries[] = {
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 16 },
    };
    size_t total = build_blob(big, entries, 1);
    /* Pass a length that's one byte short of declared total. */
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_TRUNC,
        operator_library_open(&lib, big, total - 1));
}

static void test_open_rejects_bad_magic(void)
{
    static uint8_t buf[256];
    const struct test_entry_spec entries[] = {
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 16 },
    };
    size_t total = build_blob(buf, entries, 1);
    buf[0] = 'X';   /* corrupt magic */
    struct operator_library lib;
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_MAGIC,
        operator_library_open(&lib, buf, total));
}

static void test_open_rejects_bad_version(void)
{
    static uint8_t buf[256];
    const struct test_entry_spec entries[] = {
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 16 },
    };
    size_t total = build_blob(buf, entries, 1);
    /* version field at bytes 4..5. Set to 99. */
    put_u16_le(buf + 4, 99);
    struct operator_library lib;
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_VERSION,
        operator_library_open(&lib, buf, total));

    /* schema_version at bytes 8..9. */
    total = build_blob(buf, entries, 1);
    put_u16_le(buf + 8, 99);
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_VERSION,
        operator_library_open(&lib, buf, total));
}

static void test_open_rejects_reserved_set(void)
{
    static uint8_t buf[256];
    const struct test_entry_spec entries[] = {
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 16 },
    };
    size_t total = build_blob(buf, entries, 1);
    /* kind_id at bytes 6..7 — reserved must be 0. */
    put_u16_le(buf + 6, 1);
    struct operator_library lib;
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_RESERVED,
        operator_library_open(&lib, buf, total));

    /* Reset, corrupt the trailing reserved field at bytes 20..23. */
    total = build_blob(buf, entries, 1);
    put_u32_le(buf + 20, 0xDEADBEEFu);
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_RESERVED,
        operator_library_open(&lib, buf, total));
}

static void test_open_rejects_bad_checksum(void)
{
    static uint8_t buf[256];
    const struct test_entry_spec entries[] = {
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 16 },
    };
    size_t total = build_blob(buf, entries, 1);
    /* Flip a bit deep in the SASS region after the checksum was
     * computed. The parser must catch the mutation. */
    buf[total - 1] ^= 0xFF;
    struct operator_library lib;
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_CHECKSUM,
        operator_library_open(&lib, buf, total));
}

static void test_open_rejects_bad_layout(void)
{
    static uint8_t buf[256];
    const struct test_entry_spec entries[] = {
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 16 },
    };
    size_t total = build_blob(buf, entries, 1);

    /* Corrupt the only entry's sass_size to claim a region 64 KB
     * long when only 16 B is actually available. */
    uint8_t *entry = buf + OPERATOR_LIBRARY_OUTER_HEADER_LEN
                       + OPERATOR_LIBRARY_INNER_HEADER_LEN;
    put_u64_le(entry + 24, 65536);

    /* Recompute the checksum so the parser reaches the layout check
     * instead of the checksum check. */
    uint8_t *payload = buf + OPERATOR_LIBRARY_OUTER_HEADER_LEN;
    size_t payload_len = total - OPERATOR_LIBRARY_OUTER_HEADER_LEN;
    uint32_t cks = operator_library_checksum32(payload, payload_len);
    put_u32_le(buf + 16, cks);

    struct operator_library lib;
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_LAYOUT,
        operator_library_open(&lib, buf, total));
}

static void test_lookup_returns_first_match_on_duplicate(void)
{
    static uint8_t buf[1024];
    /* Two entries with identical (op,tier,dtype). The parser
     * doesn't reject duplicates — it leaves dedup to the builder
     * — but lookup should be deterministic: first match wins. */
    const struct test_entry_spec entries[] = {
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 16 },
        { SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT, SLM_GPU_DTYPE_FP32, 32 },
    };
    size_t total = build_blob(buf, entries, 2);
    struct operator_library lib;
    TEST_ASSERT_EQUAL_INT(0, operator_library_open(&lib, buf, total));

    const uint8_t *sass = NULL;
    size_t size = 0;
    TEST_ASSERT_EQUAL_INT(0,
        operator_library_lookup(&lib,
            SLM_GPU_OP_GEMM_GENERIC, SLM_GPU_TIER_SIMT,
            SLM_GPU_DTYPE_FP32, &sass, &size));
    /* First entry was 16 bytes, fill 'A'. */
    TEST_ASSERT_EQUAL_UINT(16, size);
    TEST_ASSERT_EQUAL_HEX8('A', sass[0]);
}

int test_suite_operator_library(void)
{
    UnityBegin("test_operator_library.c");
    RUN_TEST(test_open_minimal_library);
    RUN_TEST(test_open_three_entries_lookup_each);
    RUN_TEST(test_lookup_misses_unknown_combo);
    RUN_TEST(test_open_rejects_null);
    RUN_TEST(test_open_rejects_truncated);
    RUN_TEST(test_open_rejects_bad_magic);
    RUN_TEST(test_open_rejects_bad_version);
    RUN_TEST(test_open_rejects_reserved_set);
    RUN_TEST(test_open_rejects_bad_checksum);
    RUN_TEST(test_open_rejects_bad_layout);
    RUN_TEST(test_lookup_returns_first_match_on_duplicate);
    return UnityEnd();
}
