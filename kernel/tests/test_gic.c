/*
 * test_gic.c — GIC driver regression tests.
 *
 * Focused coverage for the GICv3 SPI affinity helpers (#909). The bug
 * fixed in PR <this PR> was that `gic_set_affinity` and the
 * exclude/include pair hardcoded `affinity = cpu` (logical CPU number)
 * into GICD_IROUTER, assuming a single-cluster MPIDR layout. On
 * Jetson Orin Nano (dual-cluster, every CPU has Aff0=0 with the
 * cluster differentiator in Aff1/Aff2) those writes targeted no
 * actual CPU and the gic_include_cpu_in_spis(cpu) path corrupted
 * SPI routing enough to crash the kernel during
 * `test_suite_scheduler`'s isolation block.
 *
 * The new GICv3 path resolves through cpu_logical_map[] (same canonical
 * source the trampoline uses, PR #647) and applies TF-A's
 * MPIDR_AFFINITY_MASK formula. Tests below verify the round-trip is
 * stable on whichever cluster layout the platform exposes — QEMU
 * (single-cluster) covers the regression generically; Jetson hardware
 * verification covers the dual-cluster case specifically.
 *
 * Pi 5 + x86-64 use the GICv2 / x86 affinity paths and aren't exercised
 * here.
 */

#include "unity.h"
#include "../include/gic.h"
#include "../include/smp.h"
#include "../include/platform.h"
#include <stdint.h>

#if !defined(PLATFORM_X86_64)
/* QEMU virt, Raspberry Pi 5, and Jetson Orin Nano all expose the ARM
 * GIC API surface (`gic_set_affinity`, `gic_get_affinity`,
 * `gic_exclude_cpu_from_spis`, `gic_include_cpu_in_spis`). QEMU + Pi 5
 * use GICv2; Jetson uses GICv3. x86-64 uses LAPIC and is excluded.
 *
 * The round-trip tests below run on every ARM target; the strict
 * GICv3-specific assertions (input validation in set_affinity, the
 * single-PE re-route in exclude/include) are gated on Jetson where
 * GIC_VERSION == 3 is set in platform.h. */
#define TEST_GIC_ANY_ARM 1
#endif

#if defined(PLATFORM_JETSON_ORIN_NANO)
/* Only Jetson exercises the GICv3 affinity helpers — the fix in #909
 * lives in that code path. */
#define TEST_GIC_HAS_GICV3 1
#endif

#if defined(TEST_GIC_ANY_ARM)

/*
 * Pick an SPI that's unlikely to be in active use by anything else.
 * SPI 200 is well above the typical interrupt range claimed by virtio
 * devices on QEMU virt (32..40 for the few real devices) and inside
 * any plausible Jetson SPI window. The test snapshots the prior
 * IROUTER state and restores it after, so even if it IS in use we
 * don't leak side effects.
 */
#define TEST_GIC_SPI            200

/*
 * Test: gic_set_affinity → gic_get_affinity round-trip on every logical
 * CPU produces the right bitmask.
 *
 * On QEMU virt (single-cluster, cpu_logical_map[i] == MPIDR with
 * Aff0=i), the prior single-cluster shortcut would also pass this
 * test. The point of the test is that the NEW code resolves through
 * cpu_logical_map[] and TF-A's affinity mask, so a future regression
 * to the single-cluster shortcut would still pass on QEMU — but the
 * verification on Jetson hardware (where Aff0 is always 0) is the
 * primary defense. This test catches the QEMU half of the regression.
 *
 * Round-trip on every CPU: gic_get_affinity must return (1u << i)
 * exactly when set to CPU i.
 */
static void test_gic_set_get_affinity_round_trip(void)
{
    /* Snapshot original IROUTER so the test is reversible. */
    uint32_t saved = gic_get_affinity(TEST_GIC_SPI);

    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        int rc = gic_set_affinity(TEST_GIC_SPI, 1u << cpu);
        TEST_ASSERT_EQUAL_INT(0, rc);

        uint32_t got = gic_get_affinity(TEST_GIC_SPI);
        /* The contract: get returns the bitmask for the SAME logical
         * CPU that set targeted. The new code resolves through
         * cpu_logical_map[]; the prior code returned (1u << Aff0)
         * which collides on dual-cluster Jetson. */
        TEST_ASSERT_EQUAL_UINT32(1u << cpu, got);
    }

    /* Best-effort restore. If the prior value didn't match any logical
     * CPU (e.g. boot firmware left an unusual affinity), saved is 0;
     * in that case we leave the SPI on CPU 0 — same effective state
     * as exclude/include would produce. */
    if (saved != 0) {
        uint32_t cpu = 0;
        while (cpu < 32 && !(saved & (1u << cpu))) {
            cpu++;
        }
        if (cpu < cpu_count) {
            (void)gic_set_affinity(TEST_GIC_SPI, saved);
        }
    }
}

/*
 * Test: gic_set_affinity rejects non-SPI IRQ ids.
 *
 * SGIs (0..15) and PPIs (16..31) are per-CPU on GICv3 and don't have
 * IROUTER entries. Rejecting them at the API boundary prevents an
 * out-of-bounds GICD_IROUTER MMIO write that would land in some other
 * register window.
 */
static void test_gic_set_affinity_rejects_non_spi(void)
{
    TEST_ASSERT_EQUAL_INT(-1, gic_set_affinity(0,  1u));   /* SGI 0 */
    TEST_ASSERT_EQUAL_INT(-1, gic_set_affinity(15, 1u));   /* SGI 15 */
    TEST_ASSERT_EQUAL_INT(-1, gic_set_affinity(16, 1u));   /* PPI 16 */
    TEST_ASSERT_EQUAL_INT(-1, gic_set_affinity(26, 1u));   /* PPI 26 (CNTHP) */
    TEST_ASSERT_EQUAL_INT(-1, gic_set_affinity(31, 1u));   /* PPI 31 */
}

/*
 * Test: gic_set_affinity rejects out-of-range CPU.
 *
 * Defensive — earlier code wrote literally the cpu number into
 * IROUTER without bound-checking against cpu_count. Confirms the new
 * path's `cpu >= cpu_count` guard fires.
 */
static void test_gic_set_affinity_rejects_invalid_cpu(void)
{
#if !defined(TEST_GIC_HAS_GICV3)
    TEST_IGNORE_MESSAGE("Strict cpu_mask validation is a GICv3 contract; "
                        "GICv2 ITARGETSR accepts any mask (Pi 5 / QEMU)");
    return;
#else
    /* Empty mask. */
    TEST_ASSERT_EQUAL_INT(-1, gic_set_affinity(TEST_GIC_SPI, 0u));
    /* CPU index past cpu_count. Use 31 (top of representable range)
     * which exceeds every platform's CPU count. */
    TEST_ASSERT_EQUAL_INT(-1, gic_set_affinity(TEST_GIC_SPI, 1u << 31));
#endif
}

/*
 * Test: gic_exclude_cpu_from_spis / gic_include_cpu_in_spis is a
 * proper save/restore pair.
 *
 * The prior implementation's include path didn't restore — it wrote
 * routing for half the SPIs based on a `count % (cpu+1) == cpu`
 * formula. The new path saves which SPIs were on `cpu` at exclude
 * time and restores exactly those at include time.
 *
 * Protocol:
 *   1. Pin TEST_GIC_SPI to CPU 1.
 *   2. Exclude CPU 1. SPI should now NOT target CPU 1.
 *   3. Include CPU 1. SPI should be back on CPU 1.
 *
 * Pi 5 and x86-64 are unaffected (different code paths); this test is
 * compile-gated to GICv3 platforms.
 */
static void test_gic_exclude_include_round_trip(void)
{
#if !defined(TEST_GIC_HAS_GICV3)
    /* The GICv2 exclude/include path uses ITARGETSR bit masks with
     * different semantics (exclude just clears the cpu bit; the SPI
     * remains addressable by any other still-set CPU). The single-PE
     * "re-route to CPU 0" contract this test asserts is GICv3-specific.
     * Pi 5 (#909's regression-safe baseline) exercises the GICv2 path
     * via test_isolate_core_marks_isolated in test_scheduler.c. */
    TEST_IGNORE_MESSAGE("exclude/include re-route contract is GICv3-only");
    return;
#else
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 2");
        return;
    }

    uint32_t saved = gic_get_affinity(TEST_GIC_SPI);

    /* Pin the SPI to CPU 1. */
    int rc = gic_set_affinity(TEST_GIC_SPI, 1u << 1);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1u << 1, gic_get_affinity(TEST_GIC_SPI));

    /* Exclude CPU 1: re-routes any SPI targeting CPU 1 to CPU 0. */
    gic_exclude_cpu_from_spis(1);
    TEST_ASSERT_EQUAL_UINT32(1u << 0, gic_get_affinity(TEST_GIC_SPI));

    /* Include CPU 1: restores exactly those SPIs that were on CPU 1. */
    gic_include_cpu_in_spis(1);
    TEST_ASSERT_EQUAL_UINT32(1u << 1, gic_get_affinity(TEST_GIC_SPI));

    /* Best-effort restore. */
    if (saved != 0) {
        uint32_t cpu = 0;
        while (cpu < 32 && !(saved & (1u << cpu))) {
            cpu++;
        }
        if (cpu < cpu_count) {
            (void)gic_set_affinity(TEST_GIC_SPI, saved);
        }
    }
#endif
}

/*
 * Test: exclude on an out-of-range CPU is a no-op.
 *
 * The old code would walk the SPI table and write GICD_IROUTER(irq) = 0
 * for any SPI whose Aff0 matched the bogus cpu — i.e. it could wipe
 * CPU 0's routing if cpu_count was 4 and the caller passed cpu=99
 * (because (anything & 0xFF) == 0x63, which never matches anyway,
 * but it's still 992 unnecessary MMIO reads). The new code's
 * `cpu >= cpu_count` guard exits early.
 */
static void test_gic_exclude_out_of_range_is_noop(void)
{
    uint32_t saved = gic_get_affinity(TEST_GIC_SPI);
    gic_exclude_cpu_from_spis(99);
    gic_include_cpu_in_spis(99);
    /* IROUTER should be untouched. */
    TEST_ASSERT_EQUAL_UINT32(saved, gic_get_affinity(TEST_GIC_SPI));
}

/*
 * Test: post-set_affinity read returns the just-written value, never a
 * stale prior value.
 *
 * On GICv3 hardware the distributor latches IROUTER writes asynchronously;
 * GICD_CTLR.RWP indicates the write is still pending. Before #942 the
 * driver did not poll RWP, so a back-to-back `set_affinity(A) ->
 * get_affinity()` sequence could observe the pre-write value on a busy
 * distributor (Jetson PCIe / GIC contention). After #942, `set_affinity`
 * waits for RWP to clear before returning.
 *
 * This test is monotonic — never times-out-races on QEMU's instant
 * writes — and matches the contract that "after set_affinity returns 0,
 * get_affinity returns what was set."
 */
static void test_gic_set_affinity_post_write_is_observed(void)
{
    uint32_t saved = gic_get_affinity(TEST_GIC_SPI);

    /* Walk every logical CPU. After each set, get must return exactly
     * the mask we wrote. With pending RWP, on hardware this could
     * previously return the prior write's value. */
    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        int rc = gic_set_affinity(TEST_GIC_SPI, 1u << cpu);
        TEST_ASSERT_EQUAL_INT(0, rc);

        /* Immediately read — no intervening operation. */
        uint32_t got = gic_get_affinity(TEST_GIC_SPI);
        TEST_ASSERT_EQUAL_UINT32(1u << cpu, got);
    }

    /* Best-effort restore. */
    if (saved != 0) {
        uint32_t cpu = 0;
        while (cpu < 32 && !(saved & (1u << cpu))) {
            cpu++;
        }
        if (cpu < cpu_count) {
            (void)gic_set_affinity(TEST_GIC_SPI, saved);
        }
    }
}

#endif /* TEST_GIC_ANY_ARM */

int test_suite_gic(void)
{
    UNITY_BEGIN();
#if defined(TEST_GIC_ANY_ARM)
    RUN_TEST(test_gic_set_get_affinity_round_trip);
    RUN_TEST(test_gic_set_affinity_rejects_non_spi);
    RUN_TEST(test_gic_set_affinity_rejects_invalid_cpu);
    RUN_TEST(test_gic_exclude_include_round_trip);
    RUN_TEST(test_gic_exclude_out_of_range_is_noop);
    RUN_TEST(test_gic_set_affinity_post_write_is_observed);
#endif
    /* On non-ARM platforms (x86-64) the suite is empty — UNITY_END
     * reports zero tests, which is the cleanest "skip" signal. */
    return UNITY_END();
}
