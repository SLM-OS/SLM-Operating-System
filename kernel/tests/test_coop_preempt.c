/*
 * Cooperative-preemption tests (issue #99 resolution).
 *
 * Validates that pit_ticks and timer_handler_count advance over real
 * wall-clock time regardless of IRQ-delivery path:
 *
 *   - On QEMU / x86-64: hardware timer ISR increments the counters.
 *   - On Pi 5 / Jetson with COOP_PREEMPT: schedule() synthesizes
 *     scheduler_tick() from CNTPCT_EL0 every 10 ms. The test's yield
 *     loop drives schedule(), which fires coop_preempt_maybe_tick,
 *     which advances the counters.
 *
 * The test passes on every platform the kernel builds for, and
 * regresses if:
 *   - QEMU timer ISR stops firing
 *   - COOP_PREEMPT is accidentally disabled on Pi 5 or Jetson
 *   - coop_preempt_maybe_tick() computes the period wrong
 *   - schedule() stops calling the coop tick helper
 */

#include "unity.h"
#include "../include/timer.h"
#include "../include/sched.h"
#include "../include/smp.h"
#include "../include/preempt_point.h"
#include <stdint.h>

extern void yield(void);
/* timer_handler_count and pit_ticks come from timer.h above. */

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

    /* Upper bound: pit_ticks is a GLOBAL counter that every CPU
     * increments. On QEMU with a hardware timer IRQ, only CPU 0
     * ticks → ~5 ticks / 50 ms. On COOP_PREEMPT platforms (Pi 5,
     * Jetson) every CPU runs its own coop tick off yield() → up to
     * cpu_count × 5 ticks / 50 ms. Bound scales with cpu_count
     * with a 2× safety margin for yield-loop bursts and the
     * occasional boundary overshoot at the 10 ms period edge. */
    uint32_t upper = cpu_count * 10;
    if (upper < 20) upper = 20;
    TEST_ASSERT_MESSAGE(delta <= upper,
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
    /* Same multi-CPU rationale as test_pit_ticks_advances_over_time. */
    uint32_t upper = cpu_count * 10;
    if (upper < 20) upper = 20;
    TEST_ASSERT_MESSAGE(delta <= upper,
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

/*
 * Smoke test: slm_preempt_point() macro must be callable on every
 * platform without crashing or hanging. On platforms without
 * COOP_PREEMPT defined the macro expands to ((void)0); on Pi 5 and
 * Jetson it expands to slm_preempt_check_and_yield(). Either way,
 * 1024 back-to-back calls must complete and the function must return
 * normally each time.
 */
static void test_slm_preempt_point_callable(void)
{
    for (int i = 0; i < 1024; i++) {
        slm_preempt_point();
    }
    /* Reaching here without faulting is the assertion. */
    TEST_ASSERT_TRUE(true);
}

#if defined(COOP_PREEMPT)
/*
 * On COOP_PREEMPT platforms (Pi 5, Jetson) a tight loop calling
 * slm_preempt_point() should observe schedule() entries via the slow
 * path at roughly the TIMER_HZ rate. Verifies the Track A contract:
 * an in-tree CPU-bound loop that calls the macro cooperates with
 * the scheduler instead of monopolizing the CPU.
 *
 * Bound the loop by wall-clock (PREEMPT_POINT_TEST_WINDOW_MS) and
 * assert the slow-path counter advanced by at least
 * PREEMPT_POINT_TEST_MIN_SCHED — one less than the integer quantum
 * count over the window, allowing one missed boundary (same slack
 * as the existing test_pit_ticks_advances_over_time check).
 */
#define PREEMPT_POINT_TEST_WINDOW_MS  50UL
#define PREEMPT_POINT_TEST_MIN_SCHED  ((PREEMPT_POINT_TEST_WINDOW_MS / \
                                       (1000UL / TIMER_HZ)) - 1)

/* Catch future TIMER_HZ / WINDOW_MS combinations where the window
 * doesn't span at least two quanta — without this guard the integer
 * subtraction in MIN_SCHED would underflow a UL and the assertion
 * would silently demand ~1.8e19 schedule entries. */
_Static_assert((PREEMPT_POINT_TEST_WINDOW_MS / (1000UL / TIMER_HZ)) >= 2,
    "PREEMPT_POINT_TEST_WINDOW_MS must span >= 2 TIMER_HZ quanta");

static void test_slm_preempt_point_drives_schedule(void)
{
    uint64_t freq = timer_get_frequency();
    TEST_ASSERT_MESSAGE(freq > 0, "timer_get_frequency returned 0");

    uint64_t target_cycles = (freq * PREEMPT_POINT_TEST_WINDOW_MS) / 1000UL;
    uint64_t start_cycles  = timer_get_count();
    uint64_t start_calls   = slm_preempt_point_calls;
    uint64_t start_sched   = slm_preempt_point_schedule_calls;

    while ((timer_get_count() - start_cycles) < target_cycles) {
        slm_preempt_point();
    }

    uint64_t total_calls = slm_preempt_point_calls - start_calls;
    uint64_t slow_path   = slm_preempt_point_schedule_calls - start_sched;

    /* Fast-path call count should dominate — at minimum we expect
     * thousands of macro invocations in 50 ms even on a slow CPU. */
    TEST_ASSERT_MESSAGE(total_calls >= 1000,
        "slm_preempt_point_calls did not advance — macro may not be "
        "calling slm_preempt_check_and_yield()");

    /* Slow path: one schedule() entry per (1000/TIMER_HZ) ms quantum;
     * over PREEMPT_POINT_TEST_WINDOW_MS we expect at least
     * PREEMPT_POINT_TEST_MIN_SCHED entries (one less than the integer
     * quantum count, allowing a single missed boundary). */
    TEST_ASSERT_MESSAGE(slow_path >= PREEMPT_POINT_TEST_MIN_SCHED,
        "slm_preempt_point() did not drive schedule() — quantum check "
        "may be wrong, or COOP_PREEMPT is not actually engaged");
}
#endif  /* COOP_PREEMPT */

int test_suite_coop_preempt(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cntpct_monotonic);
    RUN_TEST(test_pit_ticks_advances_over_time);
    RUN_TEST(test_timer_handler_count_advances);
    RUN_TEST(test_slm_preempt_point_callable);
#if defined(COOP_PREEMPT)
    RUN_TEST(test_slm_preempt_point_drives_schedule);
#endif
    return UNITY_END();
}
