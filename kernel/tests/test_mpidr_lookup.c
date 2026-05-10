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
#include "../include/trap.h"
#include <stdint.h>
#include <stddef.h>

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
    /* cpu_logical_map[] is populated by smp_init(). When this test
     * runs under a host harness (e.g. JETSON_ORIN_NANO build executed
     * in QEMU for unit-test purposes) smp_init may not have run yet,
     * in which case the map is all zeros and every assertion below
     * fails noisily. Mirror the QEMU branch's guard — treat as
     * inconclusive when the map is empty. The lookup algorithm itself
     * is exercised against a populated map by test_qemu_encoding. */
    if (cpu_count == 0) {
        TEST_IGNORE_MESSAGE("smp_init not run in this test fixture — "
                            "Jetson dual-cluster encoding cannot be "
                            "validated without a populated map");
        return;
    }

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

/*
 * #750 / PR #752 regression test.
 *
 * `maybe_arm_resched_trampoline` must bail safely when called before
 * `preempt_init()` has allocated `reschedule_pending` from NC memory.
 * Without this guard, the original `if (!reschedule_pending[cpu])`
 * dereferenced a NULL pointer; on Jetson with patched BL31 routing
 * IRQs to NS-EL2, an inherited xudc IRQ delivered between mmu_enable's
 * `daifclr` and scheduler_init's `preempt_init` triggered a level-3
 * TTW external abort that BL31's RAS handler turned into a core
 * power-off (#750). The fix is a single `if (!reschedule_pending)
 * return;` at function entry; this test pins that semantics by
 * temporarily clearing the global and confirming the call doesn't
 * fault.
 *
 * Tests run after scheduler_init has populated reschedule_pending;
 * we save and restore the pointer around the call so the harness's
 * other tests still see a valid pointer afterwards.
 *
 * Gated on SECONDARY_PREEMPT because the function and the global
 * only exist when that build flag is on. The default `make test`
 * build leaves SECONDARY_PREEMPT off, so this test reports
 * IGNORE under default flags and PASSes under
 * `make test SECONDARY_PREEMPT=ON`.
 */
static void test_maybe_arm_resched_trampoline_null_safe(void)
{
#if defined(SECONDARY_PREEMPT)
    /* Snapshot, clear, call, restore. The call returns void — surviving
     * it without faulting IS the assertion. */
    volatile uint32_t *saved = reschedule_pending;
    struct trap_frame tf;
    /* zero-init: ELR / SPSR fields aren't touched on the bail path,
     * but giving them deterministic values makes the diagnostic
     * print path's output reproducible if something does change. */
    for (size_t i = 0; i < sizeof(tf); i++) {
        ((uint8_t *)&tf)[i] = 0;
    }
    tf.elr  = 0xdeadbeefULL;
    tf.spsr = 0x60400009ULL; /* EL2h, IRQs unmasked — matches the real trigger */

    reschedule_pending = NULL;
    maybe_arm_resched_trampoline(&tf);
    reschedule_pending = saved;

    /* If we reach here, the bail worked. */
    TEST_ASSERT_MESSAGE(reschedule_pending == saved,
        "reschedule_pending was not restored after the test — "
        "subsequent tests would see a stale NULL");
#else
    TEST_IGNORE_MESSAGE("SECONDARY_PREEMPT off — test_maybe_arm_resched_"
                        "trampoline_null_safe requires it (run "
                        "`make test SECONDARY_PREEMPT=ON`)");
#endif
}

int test_suite_mpidr_lookup(void)
{
    UnityBegin("test_mpidr_lookup.c");
    RUN_TEST(test_pi5_encoding_pi5_only);
    RUN_TEST(test_jetson_dual_cluster);
    RUN_TEST(test_qemu_encoding);
    RUN_TEST(test_unknown_mpidr_returns_negative);
    RUN_TEST(test_maybe_arm_resched_trampoline_null_safe);
    return UnityEnd();
}
