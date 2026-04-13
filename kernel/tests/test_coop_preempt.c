/*
 * Cooperative-preemption tests (issue #99 resolution).
 *
 * Validates that pit_ticks and timer_handler_count advance over real
 * wall-clock time regardless of IRQ-delivery path:
 *
 *   - On QEMU / Jetson: hardware timer ISR increments the counters.
 *   - On Pi 5 with PI5_COOP_PREEMPT: schedule() synthesizes
 *     scheduler_tick() from CNTPCT_EL0 every 10 ms. The test's yield
 *     loop drives schedule(), which fires coop_preempt_maybe_tick,
 *     which advances the counters.
 *
 * The test passes on every platform the kernel builds for, and
 * regresses if:
 *   - QEMU timer ISR stops firing
 *   - PI5_COOP_PREEMPT is accidentally disabled
 *   - coop_preempt_maybe_tick() computes the period wrong
 *   - schedule() stops calling the coop tick helper
 */

#include "unity.h"
#include "../include/timer.h"
#include "../include/sched.h"
#include <stdint.h>

extern void yield(void);
extern volatile uint32_t timer_handler_count;
extern volatile uint64_t pit_ticks;

/*
 * Verify that yielding in a loop for ~50 ms of wall-clock time causes
 * pit_ticks to advance at least 4 times (10 ms nominal tick period,
 * minus conservative slack for scheduler overhead and CNTPCT
 * granularity).
 */
static void test_pit_ticks_advances_over_time(void)
{
    uint64_t freq = timer_get_frequency();
    TEST_ASSERT_MESSAGE(freq > 0, "timer_get_frequency returned 0");

    /* Wait 50 ms of wall-clock time, yielding in the loop so coop
     * preempt has a chance to fire on platforms that need it. */
    uint64_t target_cycles = freq / 20;   /* 50 ms */
    uint64_t start_cycles = timer_get_count();
    uint64_t start_ticks  = pit_ticks;

    while ((timer_get_count() - start_cycles) < target_cycles) {
        yield();
    }

    uint64_t end_ticks = pit_ticks;
    uint64_t delta = end_ticks - start_ticks;

    /* Expect at least 4 ticks in 50 ms. 10 ms nominal period means 5
     * ticks ideally; allow for one missed tick at the boundary. */
    TEST_ASSERT_MESSAGE(delta >= 4,
        "pit_ticks did not advance across yield loop — "
        "coop preempt / timer ISR may be broken");

    /* Upper bound: should not have advanced wildly faster than real
     * time. More than 20 ticks in 50 ms would mean the period math is
     * wrong. */
    TEST_ASSERT_MESSAGE(delta <= 20,
        "pit_ticks advanced too fast — period math may be wrong");
}

/*
 * timer_handler_count tracks ISR entries (hardware IRQ) or synthetic
 * ticks (coop preempt). Same wall-clock bound as pit_ticks.
 */
static void test_timer_handler_count_advances(void)
{
    uint64_t freq = timer_get_frequency();
    TEST_ASSERT_MESSAGE(freq > 0, "timer_get_frequency returned 0");

    uint64_t target_cycles = freq / 20;   /* 50 ms */
    uint64_t start_cycles = timer_get_count();
    uint32_t start_count  = timer_handler_count;

    while ((timer_get_count() - start_cycles) < target_cycles) {
        yield();
    }

    uint32_t delta = timer_handler_count - start_count;
    TEST_ASSERT_MESSAGE(delta >= 4,
        "timer_handler_count did not advance across yield loop");
    TEST_ASSERT_MESSAGE(delta <= 20,
        "timer_handler_count advanced too fast");
}

/*
 * CNTPCT_EL0 sanity — should monotonically advance regardless of any
 * scheduler state. Guards against regressions that clobber the
 * read_cntpct helper.
 */
static void test_cntpct_monotonic(void)
{
    uint64_t a = timer_get_count();
    for (volatile int i = 0; i < 1000; i++) {
        __asm__ volatile("nop");
    }
    uint64_t b = timer_get_count();
    TEST_ASSERT_MESSAGE(b > a,
        "timer_get_count (CNTPCT_EL0) did not advance");
}

int test_suite_coop_preempt(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cntpct_monotonic);
    RUN_TEST(test_pit_ticks_advances_over_time);
    RUN_TEST(test_timer_handler_count_advances);
    return UNITY_END();
}
