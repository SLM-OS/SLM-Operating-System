/*
 * `bench stealing --sustain --assert-shares` decision tests (#1216).
 *
 * steal_shares_check() (kernel/sched/steal_trace.h) decides whether a
 * sustained work-stealing run passes: every thief (every online CPU
 * but the owner) must execute at least half as many tasks as the best
 * thief, and the run must not have timed out. On jetson-nano-2 the
 * #1216 stall left CPUs 2 and 3 at 0.40 to 0.83 of the best thief's
 * median share, and at 0 tasks in some runs; this check is what turns
 * that into a FAIL line and a nonzero shell status. Pure function, so
 * it runs on every platform.
 */

#include "unity.h"
#include "../sched/steal_trace.h"
#include <stdint.h>

#define OWNER 1u

/* Even shares, six CPUs (the cell B shape): pass. */
static void test_even_shares_pass(void)
{
    const uint32_t exec[6] = { 130, 128, 131, 132, 132, 130 };
    struct steal_shares_verdict v = steal_shares_check(exec, 6, OWNER, 0);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_PASS, v.result);
    TEST_ASSERT_EQUAL_UINT32(0, v.below_mask);
    TEST_ASSERT_EQUAL_UINT32(3, v.best_cpu);    /* ties: lowest CPU */
    TEST_ASSERT_EQUAL_UINT32(0, v.worst_cpu);   /* ties: lowest CPU */
    TEST_ASSERT_EQUAL_UINT32(5, v.nthieves);
    TEST_ASSERT_EQUAL_UINT32(783, v.total);
}

/* A thief at 49% of the best thief fails and is named. */
static void test_thief_at_49_percent_fails(void)
{
    const uint32_t exec[6] = { 200, 150, 98, 190, 200, 199 };
    struct steal_shares_verdict v = steal_shares_check(exec, 6, OWNER, 0);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_LOW, v.result);
    TEST_ASSERT_EQUAL_UINT32(2, v.worst_cpu);
    TEST_ASSERT_EQUAL_UINT32(0, v.best_cpu);    /* 200 on CPUs 0 and 4 */
    TEST_ASSERT_EQUAL_UINT32(1u << 2, v.below_mask);
}

/* Exactly half of the best passes ("below half" fails, half does not). */
static void test_exactly_half_passes(void)
{
    const uint32_t exec[6] = { 200, 150, 100, 190, 200, 199 };
    struct steal_shares_verdict v = steal_shares_check(exec, 6, OWNER, 0);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_PASS, v.result);
    TEST_ASSERT_EQUAL_UINT32(2, v.worst_cpu);
}

/* Odd best: 2 * 50 = 100 < 101 fails; 2 * 51 = 102 passes. */
static void test_odd_best_rounding(void)
{
    const uint32_t lo[3] = { 101, 0, 50 };
    const uint32_t ok[3] = { 101, 0, 51 };
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_LOW,
                             steal_shares_check(lo, 3, OWNER, 0).result);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_PASS,
                             steal_shares_check(ok, 3, OWNER, 0).result);
}

/* The owner is never compared: an owner with 0 tasks (or far more
 * than any thief) does not decide the verdict, and is never best. */
static void test_owner_excluded(void)
{
    const uint32_t owner_zero[6] = { 100, 0, 100, 100, 100, 100 };
    const uint32_t owner_big[6]  = { 100, 900, 100, 100, 100, 100 };
    struct steal_shares_verdict a =
        steal_shares_check(owner_zero, 6, OWNER, 0);
    struct steal_shares_verdict b =
        steal_shares_check(owner_big, 6, OWNER, 0);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_PASS, a.result);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_PASS, b.result);
    TEST_ASSERT_TRUE(a.best_cpu != OWNER && a.worst_cpu != OWNER);
    TEST_ASSERT_TRUE(b.best_cpu != OWNER && b.worst_cpu != OWNER);
    TEST_ASSERT_EQUAL_UINT32(0, b.below_mask & (1u << OWNER));
    TEST_ASSERT_EQUAL_UINT32(1400, b.total);    /* owner counts in total */
}

/* All thieves tied: pass, best and worst are the lowest thief. */
static void test_ties(void)
{
    const uint32_t exec[6] = { 77, 5, 77, 77, 77, 77 };
    struct steal_shares_verdict v = steal_shares_check(exec, 6, OWNER, 0);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_PASS, v.result);
    TEST_ASSERT_EQUAL_UINT32(0, v.best_cpu);
    TEST_ASSERT_EQUAL_UINT32(0, v.worst_cpu);
}

/* A thief at 0 tasks (run 2's run 16, CPU 3) fails; two low thieves
 * are both in the mask and the lower one is named. */
static void test_cpu_at_zero_fails(void)
{
    const uint32_t exec[6] = { 175, 163, 73, 0, 180, 178 };
    struct steal_shares_verdict v = steal_shares_check(exec, 6, OWNER, 0);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_LOW, v.result);
    TEST_ASSERT_EQUAL_UINT32(3, v.worst_cpu);
    TEST_ASSERT_EQUAL_UINT32(4, v.best_cpu);
    TEST_ASSERT_EQUAL_UINT32((1u << 2) | (1u << 3), v.below_mask);
}

/* A timed-out run fails even with even shares, and timeout wins over
 * a low share. */
static void test_timeout_fails(void)
{
    const uint32_t even[6] = { 10, 10, 10, 10, 10, 10 };
    const uint32_t low[6]  = { 10, 10, 1, 10, 10, 10 };
    struct steal_shares_verdict a = steal_shares_check(even, 6, OWNER, 1);
    struct steal_shares_verdict b = steal_shares_check(low, 6, OWNER, 1);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_TIMEOUT, a.result);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_TIMEOUT, b.result);
    TEST_ASSERT_EQUAL_UINT32(1u << 2, b.below_mask);
    TEST_ASSERT_EQUAL_UINT32(2, b.worst_cpu);
}

/* No thief ran a task, or there is no thief at all: fail. */
static void test_no_thief_fails(void)
{
    const uint32_t none_stole[4] = { 0, 40, 0, 0 };
    const uint32_t only_owner[2] = { 0, 40 };
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_NO_THIEF,
        steal_shares_check(none_stole, 4, OWNER, 0).result);
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_NO_THIEF,
        steal_shares_check(only_owner, 1, 0, 0).result);
    /* Two CPUs: one thief is its own best and passes. */
    TEST_ASSERT_EQUAL_UINT32(STEAL_SHARES_PASS,
        steal_shares_check(only_owner, 2, 0, 0).result);
}

int test_suite_steal_shares(void)
{
    UnityBegin("test_steal_shares.c");
    RUN_TEST(test_even_shares_pass);
    RUN_TEST(test_thief_at_49_percent_fails);
    RUN_TEST(test_exactly_half_passes);
    RUN_TEST(test_odd_best_rounding);
    RUN_TEST(test_owner_excluded);
    RUN_TEST(test_ties);
    RUN_TEST(test_cpu_at_zero_fails);
    RUN_TEST(test_timeout_fails);
    RUN_TEST(test_no_thief_fails);
    return UnityEnd();
}
