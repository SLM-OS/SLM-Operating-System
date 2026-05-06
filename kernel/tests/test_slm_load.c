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
#include "../include/pmm.h"
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

    /* All three null/zero args produce -1. The first two cases don't
     * read fixture_buf at all (it's just a non-null pointer to satisfy
     * the load signature), and the third short-circuits on the zero
     * length before any data deref. Keeping these assertions before
     * build_fixture means a hypothetical fixture-build failure can't
     * silently skip them via Unity's `return;`-on-failure macros. */
    TEST_ASSERT_EQUAL_INT(-1,
        rust_slm_load(NULL, fixture_buf, sizeof(fixture_buf)));
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
 * `rust_slm_load_take_pages` ownership-transfer FFI tests
 * ----------------------------------------------------------------------------
 * The streaming GGUF load path (`slm xload` shell verb) hands a
 * pre-allocated PMM buffer to the registry instead of letting the
 * registry allocate + copy. Each test below pins one half of the
 * ownership contract:
 *   - Success: registry owns the buffer; `unload` frees it. We don't
 *     have a heap-leak detector in-tree, so we re-load into the same
 *     slot afterward to prove the slot is actually free (not just
 *     vacated by `unload` while the buffer leaked).
 *   - Failure: registry leaves the buffer with the caller; we have
 *     to call `pmm_free_pages` ourselves. We exercise this by
 *     submitting non-GGUF bytes.
 *   - Argument guards: null/zero rejection mirrors `rust_slm_load`.
 * ========================================================================= */

/* Round-trip: load via take_pages, get_info, unload. After unload the
 * slot should be reusable — proven by a second `rust_slm_load` into
 * the same slot succeeding. If the take_pages success path failed to
 * register the buffer with the registry, this second load would
 * collide; if `unload` failed to release the take_pages allocation,
 * we'd notice on cumulative test runs (no leak detector but the
 * round-trip doesn't accumulate state). */
static void test_take_pages_round_trip_and_unload(void)
{
    rust_slm_test_reset();
    size_t n = 0; build_fixture(64, &n);

    /* Allocate PMM-backed pages and copy the fixture into them.
     * Mirrors what `slm xload` does after streaming bytes off the
     * shell session. */
    size_t pages = (n + 4095u) / 4096u;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages);
    TEST_ASSERT_NOT_NULL(buf);
    memcpy(buf, fixture_buf, n);

    int idx = rust_slm_load_take_pages(
        (const uint8_t *)"qwen-stream", buf, pages, n);
    TEST_ASSERT_TRUE(idx >= 0);
    /* DO NOT call pmm_free_pages(buf, pages) here — registry owns. */

    SlmModelInfoC info;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_get_info((uint32_t)idx, &info));
    TEST_ASSERT_EQUAL_INT(0, memcmp(info.architecture, "qwen2", 5));
    TEST_ASSERT_EQUAL_INT(0, memcmp(info.name, "qwen-stream", 11));
    TEST_ASSERT_EQUAL_UINT32(64u, info.vocab_size);

    /* Unload returns the slot — registry frees its take_pages buffer. */
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx));

    /* Re-load via the copying path into the same slot: succeeds only
     * if the take_pages buffer was actually released and the slot is
     * back in the free pool. */
    int idx_b = rust_slm_load((const uint8_t *)"reuse", fixture_buf, n);
    TEST_ASSERT_EQUAL_INT(idx, idx_b);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)idx_b));
}

/* Failure path: caller still owns the buffer when take_pages returns
 * -1. We hand in non-GGUF bytes so parse rejects, then free the PMM
 * pages ourselves. If the registry had erroneously freed them on the
 * error path (mismatched-double-free contract), this `pmm_free_pages`
 * call would corrupt the buddy free list and the next allocation
 * would either fail or hand back the same address — both detectable
 * by a follow-up alloc round trip. */
static void test_take_pages_caller_frees_on_failure(void)
{
    rust_slm_test_reset();

    size_t pages = 1;  /* 4 KB is plenty for a junk header. */
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages);
    TEST_ASSERT_NOT_NULL(buf);
    memset(buf, 0, 4096);
    memcpy(buf, "NOTAGGUF", 8);

    int idx = rust_slm_load_take_pages(
        (const uint8_t *)"junk", buf, pages, 64);
    TEST_ASSERT_EQUAL_INT(-1, idx);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());

    /* Caller MUST free on -1. If the contract were inverted, this
     * would double-free. */
    pmm_free_pages(buf, pages);

    /* Buddy sanity: a fresh alloc + free of the same size shouldn't
     * fail. Catches the classic post-double-free wedge where the free
     * list is left holding a stale pointer. */
    uint8_t *probe = (uint8_t *)pmm_alloc_pages(pages);
    TEST_ASSERT_NOT_NULL(probe);
    pmm_free_pages(probe, pages);
}

/* Argument guards: null name / null data / zero pages / zero data_len
 * all return -1 without touching the registry or freeing anything.
 * The caller in each case retains ownership of whatever buffer it
 * passed (if any) — matching the documented contract. */
static void test_take_pages_handles_null_zero_args(void)
{
    rust_slm_test_reset();

    /* A non-null page so the data check is exercised in isolation
     * for the null-name and null-data cases. */
    size_t pages = 1;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages);
    TEST_ASSERT_NOT_NULL(buf);

    TEST_ASSERT_EQUAL_INT(-1,
        rust_slm_load_take_pages(NULL, buf, pages, 16));
    TEST_ASSERT_EQUAL_INT(-1,
        rust_slm_load_take_pages((const uint8_t *)"x", NULL, pages, 16));
    TEST_ASSERT_EQUAL_INT(-1,
        rust_slm_load_take_pages((const uint8_t *)"x", buf, 0, 16));
    TEST_ASSERT_EQUAL_INT(-1,
        rust_slm_load_take_pages((const uint8_t *)"x", buf, pages, 0));

    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());

    /* Caller still owns buf in every branch above. */
    pmm_free_pages(buf, pages);
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
    RUN_TEST(test_take_pages_round_trip_and_unload);
    RUN_TEST(test_take_pages_caller_frees_on_failure);
    RUN_TEST(test_take_pages_handles_null_zero_args);

    return (int)UnityEnd();
}
