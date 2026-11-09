/*
 * test_coherency.c - Cache-coherency litmus suite (#1193)
 *
 * Runs P1-P6, S1 and S2 from docs/archive/coherency-gauntlet/litmus-spec.md
 * (revision 2) through the same driver as the `coherency litmus` shell
 * verb, at a reduced scale, and asserts zero forbidden outcomes, zero
 * timeouts and P4 coverage on every P row. S1, S2 and the negative
 * controls are report-only: QEMU TCG models no caches, so it cannot make
 * them fail.
 *
 * The suite can fail: the self-checks feed the forbidden-outcome
 * predicates and the real pattern readers synthetic forbidden
 * observations, and the fault-injection runs replace a role with a fake
 * (P3 stale partner, P4 descending writer, P4 writer with no writes) or
 * corrupt the start state (P6 sum offset). Each injected run must count
 * the fault in the right row and end in `coherency: FAIL (n rows)` with
 * return code 1, so the path from a failing row to the verdict is covered.
 */

#include "unity.h"
#include "test_harness.h"
#include "../include/coherency.h"
#include "../include/coherency_litmus.h"
#include "../include/smp.h"
#include "../include/uart.h"
#include "../include/timer.h"
#include "../include/vmm.h"
#include "../include/string.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Divisor on the spec counts for `make test`: P1/P2/S1 run 10^5 per pair,
 * P3/P5/S2 10^4 rounds per pair, P6 10^5 per CPU, P4 at least 10^5 writes
 * and on until each reader has 10^4 straddles (or 2 s).
 */
#define TEST_COH_SCALE      10u

/*
 * The negative controls are report-only on QEMU (TCG models no caches),
 * so `make test` runs them at a larger divisor: P2 10^3 per pair, P4 at
 * least 10^4 writes and 10^3 straddles per reader. Their time-budgeted P4
 * rows on the alias mappings are the slowest part of the suite.
 */
#define TEST_COH_NEG_SCALE  100u

/* Fault-injection runs: the largest scale (16 iterations, P4 straddle
 * target 1) keeps each one short. */
#define TEST_COH_INJECT_SCALE   COH_MAX_SCALE

/* ------------------------------------------------------------------------
 * Output capture: forwards to the UART and counts the lines the checks
 * need (the driver prints each row with one out() call).
 * ------------------------------------------------------------------------ */

static struct {
    uint32_t sens;              /* SENS lines */
    uint32_t sens_bad;          /* SENS lines that do not parse as expected */
    uint32_t s2_rows;
    uint32_t p4_rows;           /* rows carrying coverage= */
    uint32_t p4_weak;           /* ... with coverage=weak */
    uint32_t pass_lines;        /* "coherency: PASS" */
    uint32_t fail_lines;        /* "coherency: FAIL (...)" */
    char verdict[48];           /* last verdict line, without CR/LF */
} cap;

static void cap_reset(void)
{
    memset(&cap, 0, sizeof(cap));
}

static int cap_out(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = uart_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uart_puts(buf);

    if (strncmp(buf, "SENS cpus=", 10) == 0) {
        cap.sens++;
        if (strstr(buf, " S1=") == NULL || strstr(buf, " S2=") == NULL ||
            strstr(buf, " P2_sensitive=") == NULL || strstr(buf, " P3_sensitive=") == NULL) {
            cap.sens_bad++;
        }
    } else if (strncmp(buf, "S2 ", 3) == 0) {
        cap.s2_rows++;
    } else if (strncmp(buf, "P4 ", 3) == 0 && strstr(buf, " pairs=") != NULL &&
               strstr(buf, " straddles=") != NULL) {
        if (strstr(buf, " coverage=weak") != NULL) {
            cap.p4_rows++;
            cap.p4_weak++;
        } else if (strstr(buf, " coverage=ok") != NULL) {
            cap.p4_rows++;
        }
    } else if (strncmp(buf, "coherency: ", 11) == 0) {
        if (strncmp(buf, "coherency: PASS", 15) == 0) {
            cap.pass_lines++;
        } else if (strncmp(buf, "coherency: FAIL (", 17) == 0) {
            cap.fail_lines++;
        }
        size_t k = 0;
        while (buf[k] != '\0' && buf[k] != '\r' && buf[k] != '\n' &&
               k + 1 < sizeof(cap.verdict)) {
            cap.verdict[k] = buf[k];
            k++;
        }
        cap.verdict[k] = '\0';
    }
    return n;
}

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

/* CoRR reader with the writer already done: it stops at its first done
 * check, with pairs counted, no straddle and the count published. */
static void test_corr_reader_stops_on_done(void)
{
    test_vars.abort.v = 0;
    test_vars.x.v = 50;
    test_vars.p4_done.v = 1;
    test_vars.strad[0].v = 77;
    struct coh_env env = test_env();
    struct coh_result res = {0};
    coh_corr_reader(&test_vars.x.v, &test_vars.p4_done.v, &test_vars.strad[0].v, &env, &res);
    TEST_ASSERT_EQUAL_UINT64(0, res.forbidden);
    TEST_ASSERT_EQUAL_UINT64(0, res.timeouts);
    TEST_ASSERT_EQUAL_UINT64(COH_CORR_DONE_MASK + 1u, res.pairs);
    TEST_ASSERT_EQUAL_UINT64(0, res.straddles);
    TEST_ASSERT_EQUAL_UINT64(0, test_vars.strad[0].v);
    test_vars.p4_done.v = 0;
}

/* CoRR reader whose writer never finishes and never writes: a timeout. */
static void test_corr_reader_times_out_without_writes(void)
{
    test_vars.abort.v = 0;
    test_vars.x.v = 9;
    test_vars.p4_done.v = 0;
    struct coh_env env = test_env();
    struct coh_result res = {0};
    coh_corr_reader(&test_vars.x.v, &test_vars.p4_done.v, &test_vars.strad[0].v, &env, &res);
    TEST_ASSERT_EQUAL_UINT64(1, res.timeouts);
    TEST_ASSERT_EQUAL_UINT64(0, res.straddles);
    TEST_ASSERT_TRUE(res.pairs > 0);
    test_vars.abort.v = 0;
}

/*
 * CoRR writer stop rule: at least n writes (rounded up to the stop-check
 * period), then on until every reader's published straddles reach the
 * target or the budget runs out, then done. The descending variant (the
 * self-test's fake writer) counts down from its start value.
 */
static void test_corr_writer_stop_rule(void)
{
    struct coh_env env = test_env();
    const uint64_t period = COH_WRITER_ABORT_MASK + 1u;

    /* Readers already at the target: stops at the first check >= n. */
    test_vars.abort.v = 0;
    test_vars.x.v = 0;
    test_vars.p4_done.v = 0;
    test_vars.strad[0].v = 10;
    test_vars.strad[1].v = 10;
    uint64_t w = coh_corr_writer(&test_vars.x.v, &test_vars.p4_done.v, test_vars.strad, 2,
                                 period + 1u, 10, coh_cntfrq(), &env);
    TEST_ASSERT_EQUAL_UINT64(2u * period, w);
    TEST_ASSERT_EQUAL_UINT64(2u * period, test_vars.x.v);
    TEST_ASSERT_EQUAL_UINT64(1, test_vars.p4_done.v);

    /* One reader short of the target: runs until the budget (20 ms). */
    test_vars.x.v = 0;
    test_vars.p4_done.v = 0;
    test_vars.strad[1].v = 9;
    uint64_t budget = coh_cntfrq() / 50u;
    uint64_t t0 = coh_cntpct();
    w = coh_corr_writer(&test_vars.x.v, &test_vars.p4_done.v, test_vars.strad, 2,
                        16, 10, budget, &env);
    uint64_t took = coh_cntpct() - t0;
    TEST_ASSERT_TRUE(took >= budget);
    TEST_ASSERT_TRUE(w >= period);
    TEST_ASSERT_EQUAL_UINT64(0, w % period);
    TEST_ASSERT_EQUAL_UINT64(w, test_vars.x.v);
    TEST_ASSERT_EQUAL_UINT64(1, test_vars.p4_done.v);

    /* No writes and no budget (the weak-row fault): done at once. */
    test_vars.x.v = 5;
    test_vars.p4_done.v = 0;
    w = coh_corr_writer(&test_vars.x.v, &test_vars.p4_done.v, test_vars.strad, 2,
                        0, 10, 0, &env);
    TEST_ASSERT_EQUAL_UINT64(0, w);
    TEST_ASSERT_EQUAL_UINT64(5, test_vars.x.v);
    TEST_ASSERT_EQUAL_UINT64(1, test_vars.p4_done.v);

    /* Descending: values go down from the start value. */
    test_vars.x.v = 0;
    test_vars.p4_done.v = 0;
    w = coh_corr_writer_ex(&test_vars.x.v, &test_vars.p4_done.v, test_vars.strad, 0,
                           1, 1, coh_cntfrq(), 1000000, true, &env);
    TEST_ASSERT_EQUAL_UINT64(period, w);
    TEST_ASSERT_EQUAL_UINT64(1000000 - period, test_vars.x.v);

    /* Abort raised: stops at once and still sets done. */
    test_vars.abort.v = 1;
    test_vars.x.v = 0;
    test_vars.p4_done.v = 0;
    w = coh_corr_writer(&test_vars.x.v, &test_vars.p4_done.v, test_vars.strad, 2,
                        1000, 10, coh_cntfrq(), &env);
    TEST_ASSERT_EQUAL_UINT64(0, w);
    TEST_ASSERT_EQUAL_UINT64(1, test_vars.p4_done.v);
    test_vars.abort.v = 0;
    test_vars.p4_done.v = 0;
    test_vars.strad[0].v = 0;
    test_vars.strad[1].v = 0;
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
    TEST_ASSERT_FALSE(coherency_row_fails(COH_P1, 0, 0, false));
    TEST_ASSERT_TRUE(coherency_row_fails(COH_P1, 1, 0, false));
    TEST_ASSERT_TRUE(coherency_row_fails(COH_P3, 1, 0, false));
    TEST_ASSERT_TRUE(coherency_row_fails(COH_P5, 0, 1, false));
    TEST_ASSERT_TRUE(coherency_row_fails(COH_P6, 2, 3, false));
    /* A weak P4 row fails even when clean; weak means nothing elsewhere. */
    TEST_ASSERT_FALSE(coherency_row_fails(COH_P4, 0, 0, false));
    TEST_ASSERT_TRUE(coherency_row_fails(COH_P4, 0, 0, true));
    TEST_ASSERT_TRUE(coherency_row_fails(COH_P4, 1, 0, false));
    TEST_ASSERT_FALSE(coherency_row_fails(COH_P2, 0, 0, true));
    /* S1 and S2 are sensitivity only. */
    TEST_ASSERT_FALSE(coherency_row_fails(COH_S1, 12345, 0, false));
    TEST_ASSERT_FALSE(coherency_row_fails(COH_S1, 0, 7, false));
    TEST_ASSERT_FALSE(coherency_row_fails(COH_S2, 999, 0, false));
    TEST_ASSERT_FALSE(coherency_row_fails(COH_S2, 0, 3, true));
}

static void test_iteration_counts(void)
{
    TEST_ASSERT_EQUAL_UINT64(1000000, coherency_iters(COH_P1, 1));
    TEST_ASSERT_EQUAL_UINT64(1000000, coherency_iters(COH_S1, 1));
    TEST_ASSERT_EQUAL_UINT64(100000, coherency_iters(COH_P3, 1));
    TEST_ASSERT_EQUAL_UINT64(100000, coherency_iters(COH_P5, 1));
    TEST_ASSERT_EQUAL_UINT64(1000000, coherency_iters(COH_P6, 1));
    TEST_ASSERT_EQUAL_UINT64(100000, coherency_iters(COH_S2, 1));
    TEST_ASSERT_EQUAL_UINT64(1000000, coherency_iters(COH_P4, 1));
    TEST_ASSERT_EQUAL_UINT64(100000, coherency_p4_target(1));
    TEST_ASSERT_EQUAL_UINT64(10000, coherency_p4_target(10));
    TEST_ASSERT_EQUAL_UINT64(1, coherency_p4_target(COH_MAX_SCALE));
    TEST_ASSERT_EQUAL_STRING("S2", coherency_pattern_name(COH_S2));
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
    static struct coh_summary sum;
    cap_reset();
    uint64_t t0 = timer_get_count();
    int rc = coherency_litmus_run(TEST_COH_SCALE, cap_out, &sum);
    uint64_t ms = (timer_get_count() - t0) * 1000u / timer_get_frequency();
    uart_printf("[INFO] coherency litmus scale %u: %u rows in %llu ms\r\n",
                (unsigned)TEST_COH_SCALE, (unsigned)sum.rows, (unsigned long long)ms);

    uint32_t pairs = cpu_count * (cpu_count - 1);
    /* P1 P2 P3 P5 S1 S2 per pair, P4 per writer, one P6. */
    TEST_ASSERT_EQUAL_UINT32(6u * pairs + cpu_count + 1u, sum.rows);
    for (int p = COH_P1; p <= COH_P6; p++) {
        TEST_ASSERT_MESSAGE(sum.forbidden[p] == 0, coherency_pattern_name(p));
        TEST_ASSERT_MESSAGE(sum.timeouts[p] == 0, coherency_pattern_name(p));
    }
    /* P4 coverage: every reader of every row saw writes land. */
    TEST_ASSERT_EQUAL_UINT32(0, sum.p4_weak_rows);
    TEST_ASSERT_TRUE(sum.p4_min_straddles > 0);
    TEST_ASSERT_EQUAL_UINT64(coherency_p4_target(TEST_COH_SCALE), sum.p4_target);
    TEST_ASSERT_EQUAL_UINT32(0, sum.failed_rows);
    TEST_ASSERT_EQUAL_INT(0, rc);
    /* Output shape shared with the C1 harness. */
    TEST_ASSERT_EQUAL_UINT32(pairs, cap.s2_rows);
    TEST_ASSERT_EQUAL_UINT32(cpu_count, cap.p4_rows);
    TEST_ASSERT_EQUAL_UINT32(0, cap.p4_weak);
    TEST_ASSERT_EQUAL_UINT32(pairs, cap.sens);
    TEST_ASSERT_EQUAL_UINT32(0, cap.sens_bad);
    TEST_ASSERT_EQUAL_UINT32(1, cap.pass_lines);
    TEST_ASSERT_EQUAL_UINT32(0, cap.fail_lines);
    /* S1 / S2 per-pair counts add up to the pattern totals. */
    uint64_t s1 = 0;
    uint64_t s2 = 0;
    for (uint32_t a = 0; a < cpu_count; a++) {
        for (uint32_t b = 0; b < cpu_count; b++) {
            s1 += sum.s1_pair[a][b];
            s2 += sum.s2_pair[a][b];
        }
    }
    TEST_ASSERT_EQUAL_UINT64(sum.forbidden[COH_S1], s1);
    TEST_ASSERT_EQUAL_UINT64(sum.forbidden[COH_S2], s2);
    /* S1 and S2: reported only. */
    uart_printf("[INFO] coherency S1 relaxed outcomes: %llu, timeouts %llu; "
                "S2 relaxed outcomes: %llu, timeouts %llu (not judged)\r\n",
                (unsigned long long)sum.forbidden[COH_S1],
                (unsigned long long)sum.timeouts[COH_S1],
                (unsigned long long)sum.forbidden[COH_S2],
                (unsigned long long)sum.timeouts[COH_S2]);
}

/* ------------------------------------------------------------------------
 * Fault injection: a failing row reaches the FAIL verdict
 * ------------------------------------------------------------------------ */

/* Runs @patterns with @inject and checks the shared end of every injected
 * run: return code 1, @failed_rows failed rows and the matching verdict. */
static void run_injected(uint32_t patterns, enum coh_inject inject, uint32_t failed_rows,
                         struct coh_summary *sum)
{
    struct coh_run_opts opts = {
        .scale = TEST_COH_INJECT_SCALE,
        .patterns = patterns,
        .inject = inject,
    };
    cap_reset();
    uint64_t t0 = timer_get_count();
    int rc = coherency_litmus_run_opts(&opts, cap_out, sum);
    uart_printf("[INFO] coherency inject %d: %u rows, %u failed, %llu ms\r\n", (int)inject,
                (unsigned)sum->rows, (unsigned)sum->failed_rows,
                (unsigned long long)((timer_get_count() - t0) * 1000u / timer_get_frequency()));

    char want[48];
    uart_snprintf(want, sizeof(want), "coherency: FAIL (%u rows)", (unsigned)failed_rows);
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_EQUAL_UINT32(failed_rows, sum->failed_rows);
    TEST_ASSERT_EQUAL_UINT32(1, cap.fail_lines);
    TEST_ASSERT_EQUAL_UINT32(0, cap.pass_lines);
    TEST_ASSERT_EQUAL_STRING(want, cap.verdict);
    TEST_ASSERT_EQUAL_UINT32(0, cap.sens);      /* S1/S2 did not run */
}

/* P3 predicate through the real T0: a partner that never stores y and
 * reports x as r - 1 makes every round forbidden on every pair. */
static void test_inject_p3_stale_partner_fails(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("needs 2+ CPUs");
    }
    static struct coh_summary sum;
    uint32_t pairs = cpu_count * (cpu_count - 1);
    run_injected(COH_PAT_BIT(COH_P3), COH_INJECT_P3_STALE_PARTNER, pairs, &sum);
    TEST_ASSERT_EQUAL_UINT32(pairs, sum.rows);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)pairs * coherency_iters(COH_P3, TEST_COH_INJECT_SCALE),
                             sum.forbidden[COH_P3]);
    TEST_ASSERT_EQUAL_UINT64(0, sum.timeouts[COH_P3]);
}

/* P4 b < a predicate through the real readers: a descending writer. */
static void test_inject_p4_descending_writer_fails(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("needs 2+ CPUs");
    }
    static struct coh_summary sum;
    run_injected(COH_PAT_BIT(COH_P4), COH_INJECT_P4_DESCENDING, cpu_count, &sum);
    TEST_ASSERT_EQUAL_UINT32(cpu_count, sum.rows);
    /* Every reader straddled the target, and with values going down every
     * straddle is b < a. */
    TEST_ASSERT_EQUAL_UINT32(0, sum.p4_weak_rows);
    TEST_ASSERT_TRUE(sum.forbidden[COH_P4] >= (uint64_t)cpu_count * (cpu_count - 1u));
    TEST_ASSERT_EQUAL_UINT64(0, sum.timeouts[COH_P4]);
    TEST_ASSERT_EQUAL_UINT32(cpu_count, cap.p4_rows);
}

/* A P4 row with a reader that saw no straddle is weak, and weak fails. */
static void test_inject_p4_weak_row_fails(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("needs 2+ CPUs");
    }
    static struct coh_summary sum;
    run_injected(COH_PAT_BIT(COH_P4), COH_INJECT_P4_NO_WRITES, cpu_count, &sum);
    TEST_ASSERT_EQUAL_UINT32(cpu_count, sum.rows);
    TEST_ASSERT_EQUAL_UINT32(cpu_count, sum.p4_weak_rows);
    TEST_ASSERT_EQUAL_UINT64(0, sum.p4_min_straddles);
    /* Weak alone fails the row: nothing forbidden, nothing timed out. */
    TEST_ASSERT_EQUAL_UINT64(0, sum.forbidden[COH_P4]);
    TEST_ASSERT_EQUAL_UINT64(0, sum.timeouts[COH_P4]);
    TEST_ASSERT_EQUAL_UINT32(cpu_count, cap.p4_rows);
    TEST_ASSERT_EQUAL_UINT32(cpu_count, cap.p4_weak);
}

/* P6 final-sum check: a sum that starts one too high is one forbidden. */
static void test_inject_p6_sum_offset_fails(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("needs 2+ CPUs");
    }
    static struct coh_summary sum;
    run_injected(COH_PAT_BIT(COH_P6), COH_INJECT_P6_SUM_OFFSET, 1, &sum);
    TEST_ASSERT_EQUAL_UINT32(1, sum.rows);
    TEST_ASSERT_EQUAL_UINT64(1, sum.forbidden[COH_P6]);
    TEST_ASSERT_EQUAL_UINT64(0, sum.timeouts[COH_P6]);
}

/* Control for the injection runs: the same subsets with no fault pass. */
static void test_subset_without_injection_passes(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("needs 2+ CPUs");
    }
    static struct coh_summary sum;
    struct coh_run_opts opts = {
        .scale = TEST_COH_INJECT_SCALE,
        .patterns = COH_PAT_BIT(COH_P3) | COH_PAT_BIT(COH_P4) | COH_PAT_BIT(COH_P6),
        .inject = COH_INJECT_NONE,
    };
    cap_reset();
    int rc = coherency_litmus_run_opts(&opts, cap_out, &sum);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(0, sum.failed_rows);
    TEST_ASSERT_EQUAL_UINT32(cpu_count * (cpu_count - 1u) + cpu_count + 1u, sum.rows);
    TEST_ASSERT_EQUAL_UINT32(1, cap.pass_lines);
    TEST_ASSERT_EQUAL_STRING("coherency: PASS", cap.verdict);
}

static void test_negative_controls_report(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("needs 2+ CPUs");
    }
    struct coh_neg_summary sum;
    uint64_t t0 = timer_get_count();
    int rc = coherency_neg_run(TEST_COH_NEG_SCALE, uart_printf, &sum);
    uart_printf("[INFO] coherency neg scale %u: %llu ms\r\n", (unsigned)TEST_COH_NEG_SCALE,
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

int test_suite_coherency(void)
{
    uint64_t t0 = timer_get_count();
    UnityBegin("test_coherency.c");
    RUN_TEST(test_predicates_flag_forbidden_outcomes);
    RUN_TEST(test_mp_reader_counts_synthetic_forbidden);
    RUN_TEST(test_mp_reader_times_out_without_progress);
    RUN_TEST(test_corr_reader_stops_on_done);
    RUN_TEST(test_corr_reader_times_out_without_writes);
    RUN_TEST(test_corr_writer_stop_rule);
    RUN_TEST(test_pingpong_counts_lost_step);
    RUN_TEST(test_barrier_single_participant);
    RUN_TEST(test_barrier_times_out_alone);
    RUN_TEST(test_row_judgement);
    RUN_TEST(test_iteration_counts);
    RUN_TEST(test_regs_every_cpu_reports);
    RUN_TEST(test_litmus_patterns_pass);
    RUN_TEST(test_subset_without_injection_passes);
    RUN_TEST(test_inject_p3_stale_partner_fails);
    RUN_TEST(test_inject_p4_descending_writer_fails);
    RUN_TEST(test_inject_p4_weak_row_fails);
    RUN_TEST(test_inject_p6_sum_offset_fails);
    RUN_TEST(test_negative_controls_report);
    uart_printf("[INFO] test_coherency.c: %llu ms\r\n",
                (unsigned long long)((timer_get_count() - t0) * 1000u / timer_get_frequency()));
    return UnityEnd();
}
