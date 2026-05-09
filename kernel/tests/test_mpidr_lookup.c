/*
 * MPIDR -> logical CPU id lookup tests (#647).
 *
 * Validates that the cpu_logical_map[]-based lookup used by both the
 * C-side fold sites (cpu_logical_id, preempt_trampoline_cpu_for_mpidr)
 * and the asm-side ARM64_GET_LOGICAL_CPU macro produces correct results
 * for every platform's MPIDR encoding — including Jetson Orin Nano's
 * dual-cluster A78AE layout where the legacy
 * (mpidr & 0xFF) | ((mpidr >> 8) & 0xFF) fold collided on CPUs 4/5.
 *
 * The asm macro shares its data path (cpu_logical_map[]) with these C
 * helpers, so a C-only test is sufficient to validate the lookup
 * algorithm; the asm prologue/epilogue is exercised at runtime by the
 * resched_trampoline path on hardware.
 */

#include "unity.h"
#include "../include/smp.h"
#include "../include/preempt.h"
#include <stdint.h>

/*
 * Pi 5: Aff1-encoded, four cores. Lookup must agree with the static
 * map in cpu_logical_id().
 */
static void test_pi5_encoding_pi5_only(void)
{
#if defined(PLATFORM_RASPI5)
    static const uint64_t pi5_mpidrs[] = { 0x000, 0x100, 0x200, 0x300 };
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_MESSAGE(cpu_logical_id(pi5_mpidrs[i]) == i,
            "Pi 5 cpu_logical_id mismatch");
        TEST_ASSERT_MESSAGE(
            preempt_trampoline_cpu_for_mpidr(pi5_mpidrs[i]) == (uint32_t)i,
            "Pi 5 preempt_trampoline_cpu_for_mpidr mismatch");
    }
#else
    TEST_IGNORE_MESSAGE("Pi 5-only test (skipped on this platform)");
#endif
}

/*
 * Jetson Orin Nano: dual-cluster A78AE. Cluster 0 holds CPUs 0-3 with
 * the standard Aff1 encoding; cluster 1 holds CPUs 4-5 with a non-zero
 * Aff2 nibble (`0x10000`). The legacy fold (Aff0|Aff1) collapsed Aff2
 * silently and produced 2/3 for CPUs 4/5 — colliding with CPUs 2/3.
 * cpu_logical_id and preempt_trampoline_cpu_for_mpidr must each return
 * the correct logical id for all six.
 */
static void test_jetson_dual_cluster(void)
{
#if defined(PLATFORM_JETSON_ORIN_NANO)
    static const uint64_t jetson_mpidrs[] = {
        0x000, 0x100, 0x200, 0x300,    /* cluster 0 */
        0x10200, 0x10300                /* cluster 1 */
    };

    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_MESSAGE(cpu_logical_id(jetson_mpidrs[i]) == i,
            "Jetson cpu_logical_id mismatch");
        TEST_ASSERT_MESSAGE(
            preempt_trampoline_cpu_for_mpidr(jetson_mpidrs[i]) == (uint32_t)i,
            "Jetson preempt_trampoline_cpu_for_mpidr mismatch — "
            "the legacy (Aff0|Aff1) fold would collide on CPUs 4/5");
    }

    /* Legacy-fold collision proof: CPUs 4 and 5 used to fold to 2/3.
     * The new lookup must NOT produce those values. */
    TEST_ASSERT_MESSAGE(cpu_logical_id(0x10200) != 2,
        "CPU 4 (MPIDR 0x10200) must not collide with CPU 2");
    TEST_ASSERT_MESSAGE(cpu_logical_id(0x10300) != 3,
        "CPU 5 (MPIDR 0x10300) must not collide with CPU 3");
#else
    TEST_IGNORE_MESSAGE("Jetson-only test (skipped on this platform)");
#endif
}

/*
 * QEMU virt: Aff0-encoded. The cpu_logical_id implementation walks
 * cpu_logical_map[0..cpu_count-1] for non-Pi5/non-Jetson platforms.
 * Validates the lookup works on QEMU once smp_init has populated the
 * map.
 */
static void test_qemu_encoding(void)
{
#if defined(PLATFORM_QEMU_VIRT)
    /* cpu_count may be 0 if smp_init hasn't run in the test build —
     * treat that as inconclusive rather than a hard failure. */
    if (cpu_count == 0) {
        TEST_IGNORE_MESSAGE("smp_init not run in this test fixture");
        return;
    }
    for (uint32_t i = 0; i < cpu_count; i++) {
        TEST_ASSERT_MESSAGE(cpu_logical_id(i) == (int)i,
            "QEMU cpu_logical_id mismatch");
    }
#else
    TEST_IGNORE_MESSAGE("QEMU-only test (skipped on this platform)");
#endif
}

/*
 * An MPIDR not in the map returns -1 from cpu_logical_id; the
 * preempt_trampoline wrapper clamps that to 0 so its callers never
 * blow up an array index.
 */
static void test_unknown_mpidr_returns_negative(void)
{
    /* 0xDEAD_BEEF affinity bits are implausible on every supported
     * platform — should never appear in cpu_logical_map[]. */
    uint64_t bogus = 0xDEADBEEFULL;
    TEST_ASSERT_MESSAGE(cpu_logical_id(bogus) < 0,
        "cpu_logical_id must return -1 for an unknown MPIDR");
    TEST_ASSERT_MESSAGE(preempt_trampoline_cpu_for_mpidr(bogus) == 0u,
        "preempt_trampoline_cpu_for_mpidr must clamp -1 to 0");
}

int test_suite_mpidr_lookup(void)
{
    UnityBegin("test_mpidr_lookup.c");
    RUN_TEST(test_pi5_encoding_pi5_only);
    RUN_TEST(test_jetson_dual_cluster);
    RUN_TEST(test_qemu_encoding);
    RUN_TEST(test_unknown_mpidr_returns_negative);
    return UnityEnd();
}
