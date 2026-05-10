/*
 * test_oplib_pool.c - In-kernel handle for the embedded operator-
 * library blob (#714).
 *
 * The embedded blob is whatever CMake selected at build time — the
 * default is the 32-byte stub from
 * `scripts/build-operator-library-stub.py` (op_count=0). Tests assert
 * the parser-wrapper API behaves correctly on that stub: init
 * succeeds, blob size is sane, every lookup misses, and init is
 * idempotent.
 *
 * Real-blob coverage (op_count > 0, lookups hit) lives in the
 * operator-library parser tests (test_operator_library.c) which
 * synthesize blobs in memory. This file exists to pin the wrapper
 * + the embedded-symbol plumbing.
 */

#include "unity.h"
#include "../include/oplib_pool.h"
#include "../include/operator_library.h"

void test_oplib_pool_init_succeeds(void)
{
    int rc = oplib_pool_init();
    TEST_ASSERT_EQUAL_INT(0, rc);
}

void test_oplib_pool_blob_size_is_at_least_outer_header(void)
{
    /* The embedded blob is at minimum a 24-byte outer header + 8-byte
     * inner header (the stub). Real blobs add entries + SASS. */
    size_t sz = oplib_pool_blob_size();
    TEST_ASSERT_TRUE(sz >= (OPERATOR_LIBRARY_OUTER_HEADER_LEN +
                            OPERATOR_LIBRARY_INNER_HEADER_LEN));
}

void test_oplib_pool_init_is_idempotent(void)
{
    int rc1 = oplib_pool_init();
    int rc2 = oplib_pool_init();
    int rc3 = oplib_pool_init();
    TEST_ASSERT_EQUAL_INT(rc1, rc2);
    TEST_ASSERT_EQUAL_INT(rc2, rc3);
}

void test_oplib_pool_lookup_after_init_returns_well_defined(void)
{
    /* On the default stub blob, every lookup should return
     * NOT_FOUND (op_count=0, no entries to match). On a real-blob
     * build this test still passes for an arbitrary unknown triple
     * since (op_kind=0xFFFFFFFF, tier=0xFF, dtype=0xFF) doesn't
     * appear in any production manifest. */
    (void)oplib_pool_init();
    const uint8_t *sass = NULL;
    size_t size = 0;
    int rc = oplib_pool_lookup(0xFFFFFFFFu, 0xFFu, 0xFFu, &sass, &size);
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_NOT_FOUND, rc);
    /* Per the parser contract, *out_sass / *out_size are unchanged
     * on miss — they were initialized to NULL/0 above. */
    TEST_ASSERT_NULL(sass);
    TEST_ASSERT_TRUE(size == 0);
}

/* GPU-VA staging tests (#718, A.1.5). The stage_to_gpu() call needs
 * a real GA10B GMMU and only exists on Jetson; on every other platform
 * (QEMU virt, x86, Pi 5) it's a stub that returns -1 without touching
 * any state. The lookup `oplib_pool_get_sass_gpu_va` then always
 * returns NULL because the SASS pool isn't staged. */

void test_oplib_pool_get_sass_gpu_va_unstaged_returns_null(void)
{
    (void)oplib_pool_init();
    uint64_t gpu_va = 0xdeadbeefdeadbeefull;
    size_t size = 0xdead;
    int rc = oplib_pool_get_sass_gpu_va(0, 0, 0, &gpu_va, &size);
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_NULL, rc);
}

void test_oplib_pool_get_sass_gpu_va_rejects_null_out_params(void)
{
    (void)oplib_pool_init();
    uint64_t gpu_va = 0;
    size_t size = 0;
    /* NULL out_gpu_va. */
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_NULL,
        oplib_pool_get_sass_gpu_va(0, 0, 0, NULL, &size));
    /* NULL out_size. */
    TEST_ASSERT_EQUAL_INT(OPERATOR_LIBRARY_ERR_NULL,
        oplib_pool_get_sass_gpu_va(0, 0, 0, &gpu_va, NULL));
}

void test_oplib_pool_gpu_va_base_zero_when_unstaged(void)
{
    /* Pre-stage, the cached GPU VA is zero. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)0, oplib_pool_gpu_va_base());
}

#if !defined(PLATFORM_JETSON_ORIN_NANO)
void test_oplib_pool_stage_to_gpu_non_jetson_returns_minus_one(void)
{
    /* On non-Jetson platforms the stub returns -1 (no GMMU available)
     * and the lookup stays NULL-returning. */
    int rc = oplib_pool_stage_to_gpu(0xdeadbeefull);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)0, oplib_pool_gpu_va_base());
}
#endif

int test_suite_oplib_pool(void)
{
    UnityBegin("test_oplib_pool.c");
    RUN_TEST(test_oplib_pool_init_succeeds);
    RUN_TEST(test_oplib_pool_blob_size_is_at_least_outer_header);
    RUN_TEST(test_oplib_pool_init_is_idempotent);
    RUN_TEST(test_oplib_pool_lookup_after_init_returns_well_defined);
    RUN_TEST(test_oplib_pool_get_sass_gpu_va_unstaged_returns_null);
    RUN_TEST(test_oplib_pool_get_sass_gpu_va_rejects_null_out_params);
    RUN_TEST(test_oplib_pool_gpu_va_base_zero_when_unstaged);
#if !defined(PLATFORM_JETSON_ORIN_NANO)
    RUN_TEST(test_oplib_pool_stage_to_gpu_non_jetson_returns_minus_one);
#endif
    return UnityEnd();
}
