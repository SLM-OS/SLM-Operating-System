/*
 * test_oplib_probe.c - Unit tests for the boot-time op-tier probe
 * (#714, B.2).
 *
 * Covers:
 *
 *   1. The probe walks the registered op_kinds and reports a
 *      non-zero "Simt count" for the registered set today
 *      (RMSNORM, ROPE, EMBEDDING, Q4K_DOT, GQA_ATTN, SWIGLU).
 *   2. Unregistered op_kinds (Q4K_GEMM, LM_HEAD) stay at the boot
 *      default Cpu — i.e. don't bump the Simt count above the
 *      registered count.
 *
 * The Rust-side TIER_TABLE update can't be observed from this test
 * (the runtime crate isn't linked into the kernel-test image; the
 * `slm_runtime_set_tier_simt` extern resolves to a stub that
 * always returns 0). What we verify here is the C-side walk: the
 * dispatcher accepts every fixture, the probe reports the right
 * count, and the UART summary fires without panicking.
 *
 * Pure-logic. No GPU, no MMIO. Runs on QEMU.
 */

#include "unity.h"
#include "../include/oplib_probe.h"
#include "../include/operator_dispatch.h"
#include "../include/gpu_handoff.h"

#include <stdint.h>

void test_oplib_probe_run_reports_registered_ops(void)
{
    /* Today's registered set: RMSNORM, ROPE, EMBEDDING, Q4K_DOT,
     * GQA_ATTN, SWIGLU = 6 ops. Q4K_GEMM and LM_HEAD have no
     * dispatcher metadata so they should NOT bump the count. */
    int n = oplib_probe_run();
    TEST_ASSERT_EQUAL_INT(6, n);
}

void test_oplib_probe_run_is_idempotent(void)
{
    /* Calling probe twice in a row should land the same count
     * (the static probe has no side effects on the dispatcher). */
    int first  = oplib_probe_run();
    int second = oplib_probe_run();
    TEST_ASSERT_EQUAL_INT(first, second);
}

int test_suite_oplib_probe(void)
{
    UnityBegin("test_oplib_probe.c");
    RUN_TEST(test_oplib_probe_run_reports_registered_ops);
    RUN_TEST(test_oplib_probe_run_is_idempotent);
    return UnityEnd();
}
