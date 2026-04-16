/*
 * test_sched_trace.c - Regression tests for the scheduler trace buffer (#195)
 *
 * These exercise the sched_trace API directly without needing to run
 * real workloads. Each assertion pins a specific invariant of the
 * API that the shell command relies on.
 */

#include "unity.h"
#include "../include/sched_trace.h"
#include "../include/task.h"
#include <stdint.h>

/* The hooks take struct task *, but we only ever read ->id. Fake tasks
 * with distinct ids are enough to validate the record path. */
static struct task fake_task(uint32_t id, const char *name)
{
    struct task t = {0};
    t.id = id;
    /* Avoid buffer over-run — task.h says name is fixed-size char[]. */
    for (int i = 0; name[i] && i < (int)(sizeof(t.name) - 1); i++) {
        t.name[i] = name[i];
    }
    return t;
}

/*
 * Baseline: stop + clear leaves tracing off and buffer empty.
 */
static void test_trace_starts_disabled(void)
{
    sched_trace_stop();
    sched_trace_clear();
    TEST_ASSERT_FALSE(sched_trace_is_enabled());
    TEST_ASSERT_EQUAL_UINT64(0, sched_trace_total_events());
}

/*
 * Recording is a no-op when tracing is disabled.
 */
static void test_trace_record_noop_when_disabled(void)
{
    sched_trace_stop();
    sched_trace_clear();
    struct task a = fake_task(1, "a");
    struct task b = fake_task(2, "b");
    sched_trace_record_switch(0, &a, &b);
    sched_trace_record_migrate(&a, 0, 1);
    TEST_ASSERT_EQUAL_UINT64(0, sched_trace_total_events());
}

/*
 * start() flips the flag and clears the buffer.
 */
static void test_trace_start_enables(void)
{
    sched_trace_start();
    TEST_ASSERT_TRUE(sched_trace_is_enabled());
    TEST_ASSERT_EQUAL_UINT64(0, sched_trace_total_events());
    sched_trace_stop();
}

/*
 * Recording captures events and snapshot returns them in order.
 */
static void test_trace_records_events(void)
{
    sched_trace_start();

    struct task a = fake_task(10, "a");
    struct task b = fake_task(20, "b");
    struct task c = fake_task(30, "c");

    sched_trace_record_switch(0, &a, &b);
    sched_trace_record_switch(0, &b, &c);
    sched_trace_record_migrate(&c, 0, 2);

    TEST_ASSERT_EQUAL_UINT64(3, sched_trace_total_events());

    struct sched_trace_record buf[8];
    uint32_t n = sched_trace_snapshot(buf, 8);
    TEST_ASSERT_EQUAL_UINT32(3, n);

    /* Event order preserved. */
    TEST_ASSERT_EQUAL_UINT8(SCHED_TRACE_SCHED, buf[0].event);
    TEST_ASSERT_EQUAL_UINT16(10, buf[0].prev_task_id);
    TEST_ASSERT_EQUAL_UINT16(20, buf[0].next_task_id);

    TEST_ASSERT_EQUAL_UINT8(SCHED_TRACE_SCHED, buf[1].event);
    TEST_ASSERT_EQUAL_UINT16(20, buf[1].prev_task_id);
    TEST_ASSERT_EQUAL_UINT16(30, buf[1].next_task_id);

    TEST_ASSERT_EQUAL_UINT8(SCHED_TRACE_MIGRATE, buf[2].event);
    TEST_ASSERT_EQUAL_UINT8(0, buf[2].prev_cpu);
    TEST_ASSERT_EQUAL_UINT8(2, buf[2].cpu);

    sched_trace_stop();
}

/*
 * NULL prev_task renders as sentinel 0xFFFF.
 */
static void test_trace_null_prev_task(void)
{
    sched_trace_start();
    struct task a = fake_task(7, "a");
    sched_trace_record_switch(0, NULL, &a);
    struct sched_trace_record buf[1];
    TEST_ASSERT_EQUAL_UINT32(1, sched_trace_snapshot(buf, 1));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, buf[0].prev_task_id);
    TEST_ASSERT_EQUAL_UINT16(7, buf[0].next_task_id);
    sched_trace_stop();
}

/*
 * clear() resets the counter and buffer without disabling tracing.
 */
static void test_trace_clear_keeps_enabled(void)
{
    sched_trace_start();
    struct task a = fake_task(1, "a");
    sched_trace_record_switch(0, &a, &a);
    TEST_ASSERT_EQUAL_UINT64(1, sched_trace_total_events());
    sched_trace_clear();
    TEST_ASSERT_TRUE(sched_trace_is_enabled());
    TEST_ASSERT_EQUAL_UINT64(0, sched_trace_total_events());
    struct sched_trace_record buf[1];
    TEST_ASSERT_EQUAL_UINT32(0, sched_trace_snapshot(buf, 1));
    sched_trace_stop();
}

int test_suite_sched_trace(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trace_starts_disabled);
    RUN_TEST(test_trace_record_noop_when_disabled);
    RUN_TEST(test_trace_start_enables);
    RUN_TEST(test_trace_records_events);
    RUN_TEST(test_trace_null_prev_task);
    RUN_TEST(test_trace_clear_keeps_enabled);
    return UNITY_END();
}
