/*
 * test_pmu.c - PMU primitive smoke tests (#874).
 *
 * These are plumbing checks, not value checks. QEMU TCG fabricates
 * PMU counter values that do not correspond to real cache misses or
 * retired instructions — under TCG the counters tick on emulated
 * cycle boundaries, not on architectural events. The tests therefore
 * verify only "the counter advances when we run code" and "reset
 * returns to a known state." Authoritative values come from hardware
 * (Pi 5 in #874's acceptance, Jetson in #875).
 *
 * On PLATFORM_X86_64 every test reports IGNORE — the x86 RDPMC
 * equivalent is tracked separately under #870.
 */

#include "unity.h"

#if !defined(PLATFORM_X86_64)
#include "../include/pmu.h"
#include "../include/smp.h"
#include <stdint.h>

/* A small busy loop that we can confidently say retires
 * instructions and accesses memory. Marked `volatile` so the
 * compiler doesn't fold the whole body away on -O2. */
static volatile uint32_t pmu_test_sink;

static void busy_work(uint32_t iters)
{
    for (uint32_t i = 0; i < iters; i++) {
        pmu_test_sink = pmu_test_sink + i;
    }
}

static void test_pmu_enable_returns_true(void)
{
    /* pmu_enable_self has already been called from kernel_main (primary
     * CPU) and secondary_init (each secondary). Calling it again is
     * safe — it re-resets the counters and re-enables them. The return
     * value confirms PMCR_EL0.E stuck, which is the early-trip wire if
     * EL3 firmware has trapped PMU writes. */
    TEST_ASSERT_MESSAGE(pmu_enable_self(),
        "pmu_enable_self() returned false — PMCR_EL0.E did not stick. "
        "Likely EL3 trap (MDCR_EL3.TPM / TPMCR set). On Pi 5 this should "
        "always pass; on Jetson #875 verifies.");
}

static void test_pmu_is_ready_after_enable(void)
{
    /* pmu_is_ready reads the per-CPU readiness flag set by
     * pmu_enable_self. After explicitly calling enable above (or even
     * just from the boot path), this CPU must read ready=true. */
    TEST_ASSERT_MESSAGE(pmu_is_ready(),
        "pmu_is_ready() returned false after pmu_enable_self()");
}

static void test_pmu_cycle_counter_advances(void)
{
    pmu_reset();
    uint64_t before = pmu_read_cycles();
    busy_work(10000);
    uint64_t after = pmu_read_cycles();

    /* QEMU TCG and real hardware both advance PMCCNTR monotonically
     * when the counter is enabled. We don't assert a magnitude — TCG
     * values are not architecturally meaningful — only that the
     * counter moved forward. */
    TEST_ASSERT_MESSAGE(after > before,
        "PMCCNTR_EL0 did not advance after a busy loop. PMU may not be "
        "enabled on this CPU.");
}

static void test_pmu_event_counter_advances(void)
{
    pmu_reset();
    busy_work(10000);

    /* Read INST_RETIRED (counter index 2 in the preset table). On real
     * hardware this should be roughly proportional to the loop body.
     *
     * QEMU TCG does not emulate architectural event counters — the
     * cycle counter (PMCCNTR_EL0) ticks but PMEVCNTR<x>_EL0 stays at
     * zero regardless of selected event. Under QEMU virt this test
     * verifies only that the read does not fault. Hardware
     * verification (Pi 5 / Jetson) is the authoritative check.
     */
    uint32_t inst = pmu_read_event(2);
#if defined(PLATFORM_QEMU_VIRT)
    (void)inst;  /* TCG event counters are not modeled — read-no-fault is the test. */
#else
    TEST_ASSERT_MESSAGE(inst > 0,
        "PMEVCNTR2_EL0 (INST_RETIRED) stayed at 0 after 10000-iter loop. "
        "PMU event selection or enable bit is wrong.");
#endif
}

static void test_pmu_reset_clears_counters(void)
{
    /* Do some work to make sure the counters are non-zero, then reset
     * and verify they returned to ~0. Cycle counter may tick a few
     * ticks between reset and read — accept anything below a generous
     * upper bound. */
    busy_work(1000);
    pmu_reset();
    uint64_t cycles_after_reset = pmu_read_cycles();
    /* Bound is generous to absorb slow QEMU TCG hosts under CI load.
     * What we're proving is "reset worked, not no-op" — a hot
     * busy_work followed by un-reset PMCCNTR would have accumulated
     * far more than a million cycles. */
    TEST_ASSERT_MESSAGE(cycles_after_reset < 1000000ULL,
        "PMCCNTR_EL0 not reset by pmu_reset() — read returned > 1M "
        "cycles immediately after reset");

    uint32_t evt_after_reset = pmu_read_event(2);
    TEST_ASSERT_MESSAGE(evt_after_reset < 100000,
        "PMEVCNTR2_EL0 not reset by pmu_reset()");
}

static void test_pmu_read_all_snapshot_consistent(void)
{
    pmu_reset();
    busy_work(5000);

    struct pmu_snapshot snap;
    pmu_read_all(&snap);

    /* The snapshot's `cycles` should match what pmu_read_cycles
     * returns within a small delta (a few cycles for the second
     * read). */
    uint64_t cycles_direct = pmu_read_cycles();
    TEST_ASSERT_MESSAGE(cycles_direct >= snap.cycles,
        "Direct PMCCNTR read smaller than the snapshot read taken before "
        "it — counter went backwards");

    /* `events[2]` is INST_RETIRED per the preset; should be non-zero
     * after busy_work on real hardware. QEMU TCG doesn't emulate event
     * counters (see test_pmu_event_counter_advances for the same
     * relaxation), so under QEMU we only confirm the snapshot read
     * doesn't fault. */
#if !defined(PLATFORM_QEMU_VIRT)
    TEST_ASSERT_MESSAGE(snap.events[2] > 0,
        "pmu_read_all snapshot has events[2] (INST_RETIRED) == 0 after "
        "busy_work — preset table or enable mask is wrong");
#endif
}

static void test_pmu_read_event_out_of_range_safe(void)
{
    /* Out-of-range index must return 0, not fault. The defensive
     * guard in pmu_read_event protects against caller bugs. */
    TEST_ASSERT_MESSAGE(pmu_read_event(PMU_NUM_EVENT_COUNTERS) == 0,
        "pmu_read_event with idx == PMU_NUM_EVENT_COUNTERS must "
        "return 0");
    TEST_ASSERT_MESSAGE(pmu_read_event(99) == 0,
        "pmu_read_event with absurd idx must return 0");
}

/*
 * #872 — pmu_sample_cache_pressure and pmu_get_cache_pressure_q16
 * coverage. The sampler is called from scheduler_tick on every CPU,
 * feeding the per-core slot the AI scheduler reads via
 * pmu_get_cache_pressure_q16(cpu). These tests confirm the plumbing
 * works (no fault, sensible return values, out-of-range bound check)
 * without depending on a specific cache-miss rate — QEMU TCG returns
 * zero L1D refills regardless, so the EWMA stays at 0 under emulation.
 */
static void test_pmu_get_cache_pressure_q16_out_of_range(void)
{
    /* Out-of-range CPU id must return 0, not fault. ai_state.c
     * iterates over AI_STATE_NUM_CORES (6) which can exceed cpu_count
     * on Pi 5 (4 cores); the guard ensures the resulting slot read
     * never crashes regardless of how the caller bounds its iteration. */
    TEST_ASSERT_MESSAGE(pmu_get_cache_pressure_q16(MAX_CPUS) == 0,
        "pmu_get_cache_pressure_q16(MAX_CPUS) must return 0");
    TEST_ASSERT_MESSAGE(pmu_get_cache_pressure_q16(UINT32_MAX) == 0,
        "pmu_get_cache_pressure_q16(UINT32_MAX) must return 0");
}

static void test_pmu_sample_cache_pressure_no_fault(void)
{
    /* The sampler is called from scheduler_tick on every CPU. It
     * must complete without faulting even when:
     *   - The PMU is already enabled (the common path).
     *   - It's called multiple times in quick succession (prime →
     *     normal-update → normal-update).
     * QEMU TCG returns zero L1D refills, so the EWMA stays at 0,
     * but the underlying state machine (prime + delta tracking +
     * EWMA blend) still exercises every code path.
     *
     * Surviving the calls IS the assertion — there's no PMCR_EL0
     * trap or null-deref to surface a regression. */
    pmu_sample_cache_pressure();  /* first call: prime the per-CPU state */
    pmu_sample_cache_pressure();  /* second call: takes the delta path */
    pmu_sample_cache_pressure();  /* third call: exercises EWMA blend */
    /* Surviving all three calls IS the pass condition — no Unity assert
     * is needed. A fault would crash the kernel before reaching here. */
}

static void test_pmu_get_cache_pressure_q16_bounded(void)
{
    /* After the sampler runs, the per-CPU cache returns a Q16.16
     * fixed-point value in [0, PMU_Q16_ONE]. PMU_Q16_ONE is the
     * sentinel for 1.0 (full saturation). Under QEMU TCG the value
     * stays at 0 because L1D refills don't tick; on hardware it can
     * be anywhere in the range. Either way, the value must never
     * exceed the saturation cap — a regression in the EWMA clamp
     * would surface here as a value > 0x10000. */
    pmu_sample_cache_pressure();
    uint32_t cp = pmu_get_cache_pressure_q16(cpu_id());
    TEST_ASSERT_MESSAGE(cp <= PMU_Q16_ONE,
        "pmu_get_cache_pressure_q16 returned a value > 1.0 in Q16.16 — "
        "the EWMA saturation clamp regressed");
}

int test_suite_pmu(void)
{
    UnityBegin("test_pmu.c");
    RUN_TEST(test_pmu_enable_returns_true);
    RUN_TEST(test_pmu_is_ready_after_enable);
    RUN_TEST(test_pmu_cycle_counter_advances);
    RUN_TEST(test_pmu_event_counter_advances);
    RUN_TEST(test_pmu_reset_clears_counters);
    RUN_TEST(test_pmu_read_all_snapshot_consistent);
    RUN_TEST(test_pmu_read_event_out_of_range_safe);
    /* #872 — cache_pressure sampler + getter coverage. */
    RUN_TEST(test_pmu_get_cache_pressure_q16_out_of_range);
    RUN_TEST(test_pmu_sample_cache_pressure_no_fault);
    RUN_TEST(test_pmu_get_cache_pressure_q16_bounded);
    return UnityEnd();
}

#else /* PLATFORM_X86_64 */

int test_suite_pmu(void)
{
    UnityBegin("test_pmu.c");
    /* PMU primitives are ARM-only (#874 scope); x86-64 RDPMC is #870. */
    return UnityEnd();
}

#endif /* !PLATFORM_X86_64 */
