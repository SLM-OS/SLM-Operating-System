/*
 * SLM Loader FFI Tests (Phase SLM, M1.5)
 *
 * Exercises the rust_slm_load family end-to-end against a synthetic
 * Qwen2.5-shaped GGUF built by rust_slm_test_build_qwen_fixture.
 * The fixture matches every metadata key validate_for_inference
 * reads, so this is the closest unit-level proof that the M1
 * critical path (parse → validate → register → query) works.
 *
 * Hardware-side acceptance ("model load /mnt/files/qwen2.5-1.5b-…",
 * "model info <h> prints arch=qwen2 …") is deferred to M7 once the
 * shell verb is wired.
 */

#include "unity.h"
#include "../include/slm_ffi.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Fixture buffer — sized for a Qwen2 metadata GGUF with up to 128
 * vocab entries (the registry tests use 8 / 64 / 152 064; we keep
 * the C side compact). */
#define TEST_FIXTURE_CAP    (4u * 1024u)
static uint8_t fixture_buf[TEST_FIXTURE_CAP];

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Unity assertion macros expand to `return;` on failure, which is
 * incompatible with a `size_t`-returning helper under -Werror=return-type.
 * Use a void function with an out-parameter instead. */
static void build_fixture(uint32_t vocab_size, size_t *out_size)
{
    *out_size = 0;
    int rc = rust_slm_test_build_qwen_fixture(
        vocab_size, fixture_buf, sizeof(fixture_buf), out_size);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(*out_size > 0 && *out_size <= sizeof(fixture_buf));
}

/* ============================================================================
 * Tests
 * ============================================================================ */

static void test_load_and_get_info_round_trip(void)
{
    rust_slm_test_reset();
    size_t n = 0; build_fixture(64, &n);

    int idx = rust_slm_load((const uint8_t *)"qwen2.5-1.5b", fixture_buf, n);
    TEST_ASSERT_EQUAL_INT(0, idx);

    SlmModelInfoC info;
    int rc = rust_slm_get_info((uint32_t)idx, &info);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Architecture and name are null-padded ASCII; compare prefix. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(info.architecture, "qwen2", 5));
    TEST_ASSERT_EQUAL_INT(0, info.architecture[5]); /* null-padded */
    TEST_ASSERT_EQUAL_INT(0, memcmp(info.name, "qwen2.5-1.5b", 12));

    /* Architecture-specific dimensions match Qwen2.5-1.5B-Instruct. */
    TEST_ASSERT_EQUAL_UINT32(28u, info.block_count);
    TEST_ASSERT_EQUAL_UINT32(1536u, info.embedding_length);
    TEST_ASSERT_EQUAL_UINT32(12u, info.head_count);
    TEST_ASSERT_EQUAL_UINT32(2u, info.head_count_kv);
    TEST_ASSERT_EQUAL_UINT32(128u, info.head_dim);
    TEST_ASSERT_EQUAL_UINT32(8960u, info.feed_forward_length);
    TEST_ASSERT_EQUAL_UINT32(32768u, info.context_length);
    TEST_ASSERT_EQUAL_UINT32(64u, info.vocab_size);
    /* The kernel build is `-mgeneral-regs-only` (no FP in C), so
     * compare rope_freq_base by bit pattern. 1_000_000.0f is
     * 0x49742400 in IEEE-754 single precision. */
    {
        uint32_t actual_bits;
        memcpy(&actual_bits, &info.rope_freq_base, sizeof(uint32_t));
        TEST_ASSERT_EQUAL_HEX32(0x49742400u, actual_bits);
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, info.source_bytes);

    /* Cleanup: unload the slot so the next test starts clean. */
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx));
}

static void test_unload_releases_slot_for_reuse(void)
{
    rust_slm_test_reset();
    size_t n = 0; build_fixture(8, &n);

    int idx_a = rust_slm_load((const uint8_t *)"a", fixture_buf, n);
    TEST_ASSERT_EQUAL_INT(0, idx_a);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx_a));

    /* Slot 0 should be reusable. */
    int idx_b = rust_slm_load((const uint8_t *)"b", fixture_buf, n);
    TEST_ASSERT_EQUAL_INT(0, idx_b);

    /* And get_info on the freshly-vacated index returns -1 once slot
     * is empty (we already reused it; verify by unloading + reading). */
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx_b));
    SlmModelInfoC info;
    TEST_ASSERT_EQUAL_INT(-1, rust_slm_get_info((uint32_t)idx_b, &info));
}

static void test_count_tracks_active_slots(void)
{
    rust_slm_test_reset();
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());

    size_t n = 0; build_fixture(8, &n);

    int idx_a = rust_slm_load((const uint8_t *)"a", fixture_buf, n);
    TEST_ASSERT_EQUAL_INT(0, idx_a);
    TEST_ASSERT_EQUAL_UINT32(1u, rust_slm_count());

    int idx_b = rust_slm_load((const uint8_t *)"b", fixture_buf, n);
    TEST_ASSERT_EQUAL_INT(1, idx_b);
    TEST_ASSERT_EQUAL_UINT32(2u, rust_slm_count());

    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx_a));
    TEST_ASSERT_EQUAL_UINT32(1u, rust_slm_count());

    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx_b));
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

static void test_load_rejects_non_gguf_bytes(void)
{
    rust_slm_test_reset();

    /* "NOTAGGUF" + zeros — not a valid GGUF header. */
    uint8_t junk[64];
    memset(junk, 0, sizeof(junk));
    memcpy(junk, "NOTAGGUF", 8);

    int idx = rust_slm_load((const uint8_t *)"junk", junk, sizeof(junk));
    TEST_ASSERT_EQUAL_INT(-1, idx);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

static void test_load_handles_null_args(void)
{
    rust_slm_test_reset();
    size_t n = 0;
    build_fixture(8, &n);

    /* All three null/zero args produce -1. */
    TEST_ASSERT_EQUAL_INT(-1,
        rust_slm_load(NULL, fixture_buf, n));
    TEST_ASSERT_EQUAL_INT(-1,
        rust_slm_load((const uint8_t *)"x", NULL, 16));
    TEST_ASSERT_EQUAL_INT(-1,
        rust_slm_load((const uint8_t *)"x", fixture_buf, 0));
}

static void test_get_info_rejects_empty_slot(void)
{
    rust_slm_test_reset();
    SlmModelInfoC info;
    /* No slots loaded; index 0 is empty. */
    TEST_ASSERT_EQUAL_INT(-1, rust_slm_get_info(0, &info));
    /* Out-of-range index also returns -1. */
    TEST_ASSERT_EQUAL_INT(-1, rust_slm_get_info(99, &info));
    /* Null info pointer returns -1. */
    TEST_ASSERT_EQUAL_INT(-1, rust_slm_get_info(0, NULL));
}

/* ============================================================================
 * Suite Runner
 * ============================================================================ */

int test_suite_slm_load(void)
{
    UnityBegin("SLM Loader FFI Tests");

    RUN_TEST(test_load_and_get_info_round_trip);
    RUN_TEST(test_unload_releases_slot_for_reuse);
    RUN_TEST(test_count_tracks_active_slots);
    RUN_TEST(test_load_rejects_non_gguf_bytes);
    RUN_TEST(test_load_handles_null_args);
    RUN_TEST(test_get_info_rejects_empty_slot);

    return (int)UnityEnd();
}
