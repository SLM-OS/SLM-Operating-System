/*
 * test_coherency.c - Cache-coherency litmus suite (#1193)
 *
 * Runs P1-P6 from docs/archive/coherency-gauntlet/litmus-spec.md through
 * the same driver as the `coherency litmus` shell verb, at a reduced scale,
 * and asserts zero forbidden outcomes and zero timeouts on every row. S1
 * and the negative controls are report-only: QEMU TCG models no caches, so
 * it cannot make them fail.
 *
 * The suite can fail: the self-checks feed the forbidden-outcome
 * predicates and the real pattern readers synthetic forbidden observations
 * and assert they are counted, and check that a row with any forbidden
 * outcome or timeout fails the board.
 */

#include "unity.h"
#include "test_harness.h"
#include "../include/coherency.h"
#include "../include/coherency_litmus.h"
#include "../include/smp.h"
#include "../include/uart.h"
#include "../include/timer.h"
#include "../include/vmm.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Divisor on the spec counts for `make test`: P1/P2/P4/S1 run 10^5 per
 * pair, P3/P5 10^4 rounds per pair, P6 10^5 per CPU; the negative
 * controls run 10^4 per pair.
 */
#define TEST_COH_SCALE      10u

/* Scratch shared state for the single-CPU self-checks. */
static struct coh_vars test_vars;

static struct coh_env test_env(void)
{
    struct coh_env env = {
        .abort = &test_vars.abort.v,
        .timeout_ticks = coh_cntfrq() / 20u,    /* 50 ms */
    };
    return env;
}

/* ------------------------------------------------------------------------
 * Self-checks: the checker can see a forbidden outcome
 * ------------------------------------------------------------------------ */

static void test_predicates_flag_forbidden_outcomes(void)
{
    TEST_ASSERT_TRUE(coh_mp_is_forbidden(5, 4));        /* d < f */
    TEST_ASSERT_FALSE(coh_mp_is_forbidden(5, 5));
    TEST_ASSERT_FALSE(coh_mp_is_forbidden(4, 5));

    TEST_ASSERT_TRUE(coh_sb_is_forbidden(7, 6, 6));     /* both stale */
    TEST_ASSERT_FALSE(coh_sb_is_forbidden(7, 7, 6));
    TEST_ASSERT_FALSE(coh_sb_is_forbidden(7, 6, 7));

    TEST_ASSERT_TRUE(coh_corr_is_forbidden(9, 8));      /* second < first */
    TEST_ASSERT_FALSE(coh_corr_is_forbidden(8, 9));
    TEST_ASSERT_FALSE(coh_corr_is_forbidden(8, 8));

    TEST_ASSERT_TRUE(coh_final_is_forbidden(199, 200));
    TEST_ASSERT_FALSE(coh_final_is_forbidden(200, 200));
}

/* A synthetic stale observation through the real P1, P2 and S1 readers:
 * flag already at N while data still holds an older value. */
static void test_mp_reader_counts_synthetic_forbidden(void)
{
    static const enum coh_mp_kind kinds[] = { COH_MP_REL_ACQ, COH_MP_DMB, COH_MP_NONE };
    for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        test_vars.abort.v = 0;
        test_vars.flag.v = 100;
        test_vars.data.v = 37;
        struct coh_env env = test_env();
        struct coh_result res = {0};
        coh_mp_reader(kinds[i], &test_vars.data.v, &test_vars.flag.v, 100, &env, &res);
        TEST_ASSERT_EQUAL_UINT64(1, res.forbidden);
        TEST_ASSERT_EQUAL_UINT64(0, res.timeouts);
    }
}

/* A flag that never moves is a timeout, and it raises the abort flag. */
static void test_mp_reader_times_out_without_progress(void)
{
    test_vars.abort.v = 0;
    test_vars.flag.v = 3;
    test_vars.data.v = 3;
    struct coh_env env = test_env();
    struct coh_result res = {0};
    coh_mp_reader(COH_MP_DMB, &test_vars.data.v, &test_vars.flag.v, 100, &env, &res);
    TEST_ASSERT_EQUAL_UINT64(0, res.forbidden);
    TEST_ASSERT_EQUAL_UINT64(1, res.timeouts);
    TEST_ASSERT_EQUAL_UINT64(1, test_vars.abort.v);

    /* With abort already raised, a second reader leaves without counting. */
    struct coh_result res2 = {0};
    coh_mp_reader(COH_MP_DMB, &test_vars.data.v, &test_vars.flag.v, 100, &env, &res2);
    TEST_ASSERT_EQUAL_UINT64(0, res2.timeouts);
    test_vars.abort.v = 0;
}

/* CoRR reader on a value already at N: no forbidden outcome, no timeout. */
static void test_corr_reader_finishes_at_n(void)
{
    test_vars.abort.v = 0;
    test_vars.x.v = 50;
    struct coh_env env = test_env();
    struct coh_result res = {0};
    coh_corr_reader(&test_vars.x.v, 50, &env, &res);
    TEST_ASSERT_EQUAL_UINT64(0, res.forbidden);
    TEST_ASSERT_EQUAL_UINT64(0, res.timeouts);
}

/* P5: an even value ahead of what CPU A expects is a lost step. */
static void test_pingpong_counts_lost_step(void)
{
    test_vars.abort.v = 0;
    test_vars.ctr.v = 4;            /* A expects 0 first */
    struct coh_env env = test_env();
    struct coh_result res = {0};
    coh_pingpong(0, &test_vars.ctr.v, 3, &env, &res);   /* A acts on 0, 2, 4 */
    TEST_ASSERT_EQUAL_UINT64(1, res.forbidden);
    /* A resynced on 4 and wrote 5; nobody answers, so it stops on 5 or
     * leaves after its last value. */
    TEST_ASSERT_EQUAL_UINT64(5, test_vars.ctr.v);
    test_vars.abort.v = 0;
}

/* The single-participant barrier passes straight through, twice. */
static void test_barrier_single_participant(void)
{
    test_vars.abort.v = 0;
    test_vars.bar_count.v = 0;
    test_vars.bar_sense.v = 0;
    struct coh_env env = test_env();
    struct coh_result res = {0};
    uint64_t sense = 0;
    TEST_ASSERT_EQUAL_INT(COH_WAIT_OK, coh_barrier_wait(&test_vars.bar_count.v,
                          &test_vars.bar_sense.v, 1, &sense, &env, &res));
    TEST_ASSERT_EQUAL_INT(COH_WAIT_OK, coh_barrier_wait(&test_vars.bar_count.v,
                          &test_vars.bar_sense.v, 1, &sense, &env, &res));
    TEST_ASSERT_EQUAL_UINT64(0, test_vars.bar_count.v);
    TEST_ASSERT_EQUAL_UINT64(0, test_vars.bar_sense.v);
    TEST_ASSERT_EQUAL_UINT64(0, res.timeouts);
}

/* A waiter whose partner never arrives times out instead of hanging. */
static void test_barrier_times_out_alone(void)
{
    test_vars.abort.v = 0;
    test_vars.bar_count.v = 0;
    test_vars.bar_sense.v = 0;
    struct coh_env env = test_env();
    struct coh_result res = {0};
    uint64_t sense = 0;
    TEST_ASSERT_EQUAL_INT(COH_WAIT_TIMEOUT, coh_barrier_wait(&test_vars.bar_count.v,
                          &test_vars.bar_sense.v, 2, &sense, &env, &res));
    TEST_ASSERT_EQUAL_UINT64(1, res.timeouts);
    test_vars.abort.v = 0;
}

static void test_row_judgement(void)
{
    TEST_ASSERT_FALSE(coherency_row_fails(COH_P1, 0, 0));
    TEST_ASSERT_TRUE(coherency_row_fails(COH_P1, 1, 0));
    TEST_ASSERT_TRUE(coherency_row_fails(COH_P5, 0, 1));
    TEST_ASSERT_TRUE(coherency_row_fails(COH_P6, 2, 3));
    /* S1 is sensitivity only. */
    TEST_ASSERT_FALSE(coherency_row_fails(COH_S1, 12345, 0));
    TEST_ASSERT_FALSE(coherency_row_fails(COH_S1, 0, 7));
}

static void test_iteration_counts(void)
{
    TEST_ASSERT_EQUAL_UINT64(1000000, coherency_iters(COH_P1, 1));
    TEST_ASSERT_EQUAL_UINT64(1000000, coherency_iters(COH_S1, 1));
    TEST_ASSERT_EQUAL_UINT64(100000, coherency_iters(COH_P3, 1));
    TEST_ASSERT_EQUAL_UINT64(100000, coherency_iters(COH_P5, 1));
    TEST_ASSERT_EQUAL_UINT64(1000000, coherency_iters(COH_P6, 1));
    TEST_ASSERT_EQUAL_UINT64(10000, coherency_iters(COH_P2, 100));
    TEST_ASSERT_EQUAL_UINT64(COH_MIN_ITERS, coherency_iters(COH_P3, COH_MAX_SCALE));
}

/* ------------------------------------------------------------------------
 * Multi-CPU runs
 * ------------------------------------------------------------------------ */

static void test_regs_every_cpu_reports(void)
{
    static struct coh_cpu_regs regs[MAX_CPUS];
    int n = coherency_regs_collect(regs, false);
    TEST_ASSERT_EQUAL_INT((int)cpu_count, n);
    coherency_regs_print(uart_printf, regs, cpu_count);

    uint64_t midr;
    uint64_t el;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    for (uint32_t c = 0; c < cpu_count; c++) {
        TEST_ASSERT_TRUE(regs[c].valid);
        TEST_ASSERT_EQUAL_UINT32(c, regs[c].ran_on);
        TEST_ASSERT_EQUAL_HEX64(midr, regs[c].midr);
        TEST_ASSERT_EQUAL_UINT64((el >> 2) & 3u, regs[c].current_el);
        TEST_ASSERT_TRUE(regs[c].sctlr_el1 != 0 || regs[c].current_el == 2);
        TEST_ASSERT_TRUE(regs[c].ncache >= 1);
#if defined(PLATFORM_QEMU_VIRT)
        /* The IMPLEMENTATION DEFINED reads are compiled out for QEMU. */
        TEST_ASSERT_FALSE(regs[c].impdef_read);
#endif
        for (uint32_t d = 0; d < c; d++) {
            TEST_ASSERT_TRUE((regs[c].mpidr & 0xFFFFFFull) != (regs[d].mpidr & 0xFFFFFFull));
        }
    }
}

static void test_litmus_patterns_pass(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("needs 2+ CPUs");
    }
    struct coh_summary sum;
    uint64_t t0 = timer_get_count();
    int rc = coherency_litmus_run(TEST_COH_SCALE, uart_printf, &sum);
    uint64_t ms = (timer_get_count() - t0) * 1000u / timer_get_frequency();
    uart_printf("[INFO] coherency litmus scale %u: %u rows in %llu ms\r\n",
                (unsigned)TEST_COH_SCALE, (unsigned)sum.rows, (unsigned long long)ms);

    uint32_t pairs = cpu_count * (cpu_count - 1);
    /* P1 P2 P3 P5 S1 per pair, P4 per writer, one P6. */
    TEST_ASSERT_EQUAL_UINT32(5u * pairs + cpu_count + 1u, sum.rows);
    for (int p = COH_P1; p <= COH_P6; p++) {
        TEST_ASSERT_MESSAGE(sum.forbidden[p] == 0, coherency_pattern_name(p));
        TEST_ASSERT_MESSAGE(sum.timeouts[p] == 0, coherency_pattern_name(p));
    }
    TEST_ASSERT_EQUAL_UINT32(0, sum.failed_rows);
    TEST_ASSERT_EQUAL_INT(0, rc);
    /* S1: reported only. */
    uart_printf("[INFO] coherency S1 relaxed outcomes: %llu, timeouts %llu (not judged)\r\n",
                (unsigned long long)sum.forbidden[COH_S1],
                (unsigned long long)sum.timeouts[COH_S1]);
}

static void test_negative_controls_report(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("needs 2+ CPUs");
    }
    struct coh_neg_summary sum;
    uint64_t t0 = timer_get_count();
    int rc = coherency_neg_run(TEST_COH_SCALE, uart_printf, &sum);
    uart_printf("[INFO] coherency neg scale %u: %llu ms\r\n", (unsigned)TEST_COH_SCALE,
                (unsigned long long)((timer_get_count() - t0) * 1000u / timer_get_frequency()));
    TEST_ASSERT_EQUAL_INT(0, rc);
    for (int c = 0; c < COH_NEG_COUNT; c++) {
        TEST_ASSERT_TRUE(sum.ran[c]);
    }
    for (int c = 0; c < COH_NEG_DCACHE_OFF; c++) {
        uart_printf("[INFO] coherency neg %c: P2 forbidden %llu timeouts %llu, "
                    "P4 forbidden %llu timeouts %llu (report only)\r\n", 'a' + c,
                    (unsigned long long)sum.forbidden[c][0],
                    (unsigned long long)sum.timeouts[c][0],
                    (unsigned long long)sum.forbidden[c][1],
                    (unsigned long long)sum.timeouts[c][1]);
    }
    uart_printf("[INFO] coherency neg c: loads/phase %llu, phase 1 stale %llu fresh %llu, "
                "phase 2 stale %llu fresh %llu, timeouts %llu (report only)\r\n",
                (unsigned long long)sum.dcoff_loads, (unsigned long long)sum.dcoff_p1_stale,
                (unsigned long long)sum.dcoff_p1_fresh, (unsigned long long)sum.dcoff_p2_stale,
                (unsigned long long)sum.dcoff_p2_fresh, (unsigned long long)sum.dcoff_timeouts);
    /* Mechanism check, not an outcome check: every C=0 load returned P0
     * or P1 (the line holds nothing else once phase 1 starts). */
    if (sum.dcoff_timeouts == 0) {
        TEST_ASSERT_EQUAL_UINT64(sum.dcoff_loads, sum.dcoff_p1_stale + sum.dcoff_p1_fresh);
        TEST_ASSERT_EQUAL_UINT64(sum.dcoff_loads, sum.dcoff_p2_stale + sum.dcoff_p2_fresh);
    }
    TEST_ASSERT_EQUAL_UINT64((uint64_t)cpu_count * (cpu_count - 1) * COH_DCOFF_LOADS,
                             sum.dcoff_loads);
    /* The aliases were installed with the requested attributes. */
    TEST_ASSERT_EQUAL_HEX64(0UL << 8, sum.alias_pte[COH_NEG_NONSHARE] & (3UL << 8));
    TEST_ASSERT_EQUAL_HEX64(3UL << 2, sum.alias_pte[COH_NEG_NONSHARE] & (7UL << 2));
    TEST_ASSERT_EQUAL_HEX64(2UL << 2, sum.alias_pte[COH_NEG_NONCACHE] & (7UL << 2));
    /* ...and torn down afterwards. */
    TEST_ASSERT_FALSE(vmm_is_mapped(COH_ALIAS_VA));
}

/*
 * A second `coherency neg` in the same boot takes new slots (rows never
 * reuse a line an earlier alias read touched) and still runs every control.
 */
static void test_negative_controls_rerun_uses_fresh_slots(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("needs 2+ CPUs");
    }
    struct coh_neg_summary sum;
    TEST_ASSERT_EQUAL_INT(0, coherency_neg_run(1000u, NULL, &sum));
    for (int c = 0; c < COH_NEG_COUNT; c++) {
        TEST_ASSERT_TRUE(sum.ran[c]);
    }
    TEST_ASSERT_FALSE(vmm_is_mapped(COH_ALIAS_VA));
}

int test_suite_coherency(void)
{
    UnityBegin("test_coherency.c");
    RUN_TEST(test_predicates_flag_forbidden_outcomes);
    RUN_TEST(test_mp_reader_counts_synthetic_forbidden);
    RUN_TEST(test_mp_reader_times_out_without_progress);
    RUN_TEST(test_corr_reader_finishes_at_n);
    RUN_TEST(test_pingpong_counts_lost_step);
    RUN_TEST(test_barrier_single_participant);
    RUN_TEST(test_barrier_times_out_alone);
    RUN_TEST(test_row_judgement);
    RUN_TEST(test_iteration_counts);
    RUN_TEST(test_regs_every_cpu_reports);
    RUN_TEST(test_litmus_patterns_pass);
    RUN_TEST(test_negative_controls_report);
    RUN_TEST(test_negative_controls_rerun_uses_fresh_slots);
    return UnityEnd();
}
