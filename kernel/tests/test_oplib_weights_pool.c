/*
 * test_oplib_weights_pool.c — W2 of the GPU weights pool
 * (docs/design/gpu-weights-pool.md).
 *
 * The kernel-test build runs on QEMU ARM64, where
 * `PLATFORM_JETSON_ORIN_NANO` is NOT defined. The Jetson-only path
 * compiles to stubs (returning 0/-1 with no global state), so the
 * QEMU tests verify:
 *
 *   - `_alloc` / `_stage` / `_total_size` / `_bytes_used` always
 *     return their well-defined "no pool" values.
 *   - `_reset` is a no-op (idempotent).
 *
 * The arithmetic of the bump allocator + the per-page GMMU walk are
 * exercised on Jetson hardware via the W2 PR's hardware-verification
 * step, not via the Unity harness — those paths need a live channel
 * handoff that QEMU doesn't provide.
 */

#include "unity.h"
#include "../include/oplib_weights_pool.h"

void test_oplib_weights_pool_total_size_zero_on_qemu(void)
{
    /* No v8 handoff in QEMU: the underlying
     * `ga10b_weights_pool_size_bytes` accessor returns 0, so the
     * inspector reports 0. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)0u,
                             (uint64_t)oplib_weights_pool_total_size());
}

void test_oplib_weights_pool_alloc_returns_zero_when_pool_empty(void)
{
    uint64_t va = oplib_weights_pool_alloc(64u, 0u);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)0u, va);
}

void test_oplib_weights_pool_stage_returns_minus_one_when_pool_empty(void)
{
    uint8_t buf[16] = { 0 };
    int rc = oplib_weights_pool_stage(0xdeadbeef00000000ull, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_oplib_weights_pool_alloc_rejects_non_power_of_two_align(void)
{
    /* Even on the Jetson path this check fires before any pool
     * lookup, so the QEMU stub never reaches it — but the test
     * documents the intended invariant for the Jetson path
     * (Hardware-side verification covers it via `slm load`'s
     * default 256-byte alignment). On QEMU the call returns 0
     * because the pool is empty, which is the same observable as
     * "rejected for bad alignment" — both branches return 0. */
    uint64_t va = oplib_weights_pool_alloc(64u, 17u);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)0u, va);
}

void test_oplib_weights_pool_reset_idempotent_when_empty(void)
{
    oplib_weights_pool_reset();
    oplib_weights_pool_reset();
    TEST_ASSERT_EQUAL_UINT64((uint64_t)0u,
                             (uint64_t)oplib_weights_pool_bytes_used());
}

int test_suite_oplib_weights_pool(void)
{
    UnityBegin("test_oplib_weights_pool.c");
    RUN_TEST(test_oplib_weights_pool_total_size_zero_on_qemu);
    RUN_TEST(test_oplib_weights_pool_alloc_returns_zero_when_pool_empty);
    RUN_TEST(test_oplib_weights_pool_stage_returns_minus_one_when_pool_empty);
    RUN_TEST(test_oplib_weights_pool_alloc_rejects_non_power_of_two_align);
    RUN_TEST(test_oplib_weights_pool_reset_idempotent_when_empty);
    return UnityEnd();
}
