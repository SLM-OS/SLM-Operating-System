/*
 * Scheduler Tests
 *
 * Tests for priority-based scheduling and deadline boost functionality.
 */

#include "unity.h"
#include "task.h"
#include "sched.h"
#include "slm_ffi.h"
#include "spinlock.h"
#include "smp.h"
#include "uart.h"
#include <stdint.h>
#include <stdbool.h>

/* Test state for tracking task execution order */
static spinlock_t test_lock = SPINLOCK_INIT;
static volatile int execution_order[8];
static volatile int execution_index = 0;

/* Simple delay loop */
static void test_delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile("nop");
    }
}

/*
 * No-op entry function for unit tests.
 * Tasks with this entry will just exit immediately if scheduled.
 * This prevents crashes when testing tasks that get added to the scheduler.
 */
static void nop_entry(void *arg)
{
    (void)arg;
    task_exit();
}

/* ============================================================================
 * Unit Tests: Deadline Boost Logic
 * ============================================================================ */

/*
 * Test: Task with no deadline has effective_priority == base priority
 */
static void test_no_deadline_no_boost(void)
{
    struct task *t = task_create_with_priority("test_no_dl", NULL, NULL, TASK_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(t);

    /* No deadline set - effective should equal base */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_NORMAL, t->priority);
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_NORMAL, t->effective_priority);
    TEST_ASSERT_EQUAL_UINT64(0, t->deadline_ns);

    /* Clean up - mark as terminated so destroy works */
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Setting deadline updates task field
 */
static void test_deadline_field_set(void)
{
    struct task *t = task_create_with_priority("test_dl", NULL, NULL, TASK_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(t);

    uint64_t deadline = 1000000000ULL; /* 1 second */
    task_set_deadline(t, deadline);

    TEST_ASSERT_EQUAL_UINT64(deadline, t->deadline_ns);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Distant deadline (> 100ms) does not boost priority
 *
 * This test requires adding task to queue to trigger boost calculation.
 * The task runs on CPU 0 (same as test) so we can check immediately.
 */
static void test_distant_deadline_no_boost(void)
{
    /* Get current time and set deadline 200ms in future */
    uint64_t now = slm_get_time_ns();
    uint64_t deadline = now + (200 * 1000000ULL); /* 200ms */

    struct task *t = task_create_with_priority("dist_dl", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(t);

    task_set_deadline(t, deadline);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    /* Add to scheduler - this triggers boost calculation */
    scheduler_add_task(t);

    /* With 200ms remaining, no boost should be applied */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_LOW, t->effective_priority);

    /* Remove before re-enabling interrupts */
    scheduler_remove_task(t);

    irq_restore(flags);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Deadline < 100ms gets +1 boost
 */
static void test_deadline_100ms_boost_plus_one(void)
{
    uint64_t now = slm_get_time_ns();
    uint64_t deadline = now + (80 * 1000000ULL); /* 80ms - within 100ms threshold */

    struct task *t = task_create_with_priority("80ms_dl", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(t);

    task_set_deadline(t, deadline);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    scheduler_add_task(t);

    /* Should be boosted by +1 (LOW=2 -> 3) */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_LOW + 1, t->effective_priority);

    scheduler_remove_task(t);

    irq_restore(flags);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Deadline < 50ms boosts to HIGH
 */
static void test_deadline_50ms_boost_to_high(void)
{
    uint64_t now = slm_get_time_ns();
    uint64_t deadline = now + (30 * 1000000ULL); /* 30ms - within 50ms threshold */

    struct task *t = task_create_with_priority("30ms_dl", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(t);

    task_set_deadline(t, deadline);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    scheduler_add_task(t);

    /* Should be boosted to HIGH */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_HIGH, t->effective_priority);

    scheduler_remove_task(t);

    irq_restore(flags);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Deadline < 10ms boosts to CRITICAL
 */
static void test_deadline_10ms_boost_to_critical(void)
{
    uint64_t now = slm_get_time_ns();
    uint64_t deadline = now + (5 * 1000000ULL); /* 5ms - within 10ms threshold */

    struct task *t = task_create_with_priority("5ms_dl", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(t);

    task_set_deadline(t, deadline);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    scheduler_add_task(t);

    /* Should be boosted to CRITICAL */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_CRITICAL, t->effective_priority);

    scheduler_remove_task(t);

    irq_restore(flags);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Missed deadline (past) boosts to CRITICAL
 */
static void test_missed_deadline_boost_to_critical(void)
{
    uint64_t now = slm_get_time_ns();
    /* Set deadline in the past */
    uint64_t deadline = now > 1000000ULL ? now - 1000000ULL : 1;

    struct task *t = task_create_with_priority("past_dl", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(t);

    task_set_deadline(t, deadline);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    scheduler_add_task(t);

    /* Missed deadline should be CRITICAL */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_CRITICAL, t->effective_priority);

    scheduler_remove_task(t);

    irq_restore(flags);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/* ============================================================================
 * Unit Tests: Priority Setting
 * ============================================================================ */

/*
 * Test: task_set_priority updates priority field
 */
static void test_set_priority_updates_field(void)
{
    struct task *t = task_create("set_pri", NULL, NULL);
    TEST_ASSERT_NOT_NULL(t);

    /* Default is NORMAL */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_NORMAL, t->priority);

    /* Set to HIGH */
    task_set_priority(t, TASK_PRIORITY_HIGH);
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_HIGH, t->priority);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Priority clamped to valid range
 */
static void test_priority_clamped_to_max(void)
{
    struct task *t = task_create("clamp", NULL, NULL);
    TEST_ASSERT_NOT_NULL(t);

    /* Try to set priority above MAX (7) */
    task_set_priority(t, 100);
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_MAX, t->priority);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/* ============================================================================
 * Unit Tests: FFI Task API
 * ============================================================================ */

/*
 * Test: slm_task_create returns valid task ID
 */
static void dummy_task(void *arg) { (void)arg; }

static void test_ffi_task_create_returns_id(void)
{
    uint32_t id = slm_task_create("ffi_test", dummy_task, NULL);
    TEST_ASSERT(id != 0);

    /* Verify task exists */
    struct task *t = task_get(id);
    TEST_ASSERT_NOT_NULL(t);

    /* Clean up */
    scheduler_remove_task(t);
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: slm_task_set_priority works via FFI
 */
static void test_ffi_set_priority(void)
{
    uint32_t id = slm_task_create("ffi_pri", dummy_task, NULL);
    TEST_ASSERT(id != 0);

    int ret = slm_task_set_priority(id, TASK_PRIORITY_HIGH);
    TEST_ASSERT_EQUAL_INT(SLM_OK, ret);

    struct task *t = task_get(id);
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_HIGH, t->priority);

    scheduler_remove_task(t);
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: slm_task_set_deadline works via FFI
 */
static void test_ffi_set_deadline(void)
{
    uint32_t id = slm_task_create("ffi_dl", dummy_task, NULL);
    TEST_ASSERT(id != 0);

    uint64_t deadline = 999999999ULL;
    int ret = slm_task_set_deadline(id, deadline);
    TEST_ASSERT_EQUAL_INT(SLM_OK, ret);

    struct task *t = task_get(id);
    TEST_ASSERT_EQUAL_UINT64(deadline, t->deadline_ns);

    scheduler_remove_task(t);
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: FFI functions return error for invalid task ID
 */
static void test_ffi_invalid_task_id(void)
{
    int ret = slm_task_set_priority(99999, TASK_PRIORITY_HIGH);
    TEST_ASSERT_EQUAL_INT(SLM_ERR_INVALID, ret);

    ret = slm_task_set_deadline(99999, 1000);
    TEST_ASSERT_EQUAL_INT(SLM_ERR_INVALID, ret);
}

/* ============================================================================
 * Integration Tests: Priority Ordering
 * ============================================================================
 *
 * These tests verify that tasks actually run in priority order.
 * They create tasks on a secondary CPU and track execution order.
 */

/* Task that records its ID in the execution order array */
static void order_tracking_task(void *arg)
{
    int task_id = (int)(uintptr_t)arg;

    irq_flags_t flags = spin_lock_irqsave(&test_lock);
    if (execution_index < 8) {
        execution_order[execution_index++] = task_id;
    }
    spin_unlock_irqrestore(&test_lock, flags);

    /* Small delay to ensure tasks don't all run simultaneously */
    test_delay(10000);
}

/*
 * Test: High priority task runs before low priority task
 *
 * This test queues a LOW priority task, then a HIGH priority task.
 * Due to priority ordering, HIGH should run first.
 */
static void test_high_priority_runs_first(void)
{
    /* Reset execution tracking */
    execution_index = 0;
    for (int i = 0; i < 8; i++) execution_order[i] = -1;

    /* Create tasks with different priorities */
    struct task *low = task_create_with_priority("low", order_tracking_task,
                                                  (void *)1, TASK_PRIORITY_LOW);
    struct task *high = task_create_with_priority("high", order_tracking_task,
                                                   (void *)2, TASK_PRIORITY_HIGH);

    TEST_ASSERT_NOT_NULL(low);
    TEST_ASSERT_NOT_NULL(high);

    /* Add LOW first, then HIGH - but HIGH should run first due to priority */
    scheduler_add_task_to_cpu(low, 1);
    scheduler_add_task_to_cpu(high, 1);

    /* Wait for both tasks to complete */
    int timeout = 100;
    while ((low->state != TASK_TERMINATED || high->state != TASK_TERMINATED) && timeout > 0) {
        test_delay(50000);
        timeout--;
    }

    /* Verify execution order: HIGH (2) should run before LOW (1) */
    TEST_ASSERT_EQUAL_INT(2, execution_order[0]); /* HIGH ran first */
    TEST_ASSERT_EQUAL_INT(1, execution_order[1]); /* LOW ran second */

    task_destroy(high);
    task_destroy(low);
}

/*
 * Test: Multiple priority levels execute in correct order
 *
 * Queues tasks at IDLE, LOW, NORMAL, HIGH, CRITICAL priorities.
 * Verifies they execute in descending priority order.
 */
static void test_priority_ordering_multiple_levels(void)
{
    /* Reset execution tracking */
    execution_index = 0;
    for (int i = 0; i < 8; i++) execution_order[i] = -1;

    /* Create tasks at different priority levels */
    struct task *idle = task_create_with_priority("p_idle", order_tracking_task,
                                                   (void *)0, TASK_PRIORITY_IDLE);
    struct task *low = task_create_with_priority("p_low", order_tracking_task,
                                                  (void *)2, TASK_PRIORITY_LOW);
    struct task *normal = task_create_with_priority("p_norm", order_tracking_task,
                                                     (void *)4, TASK_PRIORITY_NORMAL);
    struct task *high = task_create_with_priority("p_high", order_tracking_task,
                                                   (void *)6, TASK_PRIORITY_HIGH);
    struct task *critical = task_create_with_priority("p_crit", order_tracking_task,
                                                       (void *)7, TASK_PRIORITY_CRITICAL);

    TEST_ASSERT_NOT_NULL(idle);
    TEST_ASSERT_NOT_NULL(low);
    TEST_ASSERT_NOT_NULL(normal);
    TEST_ASSERT_NOT_NULL(high);
    TEST_ASSERT_NOT_NULL(critical);

    /* Add in random order - should still execute by priority */
    scheduler_add_task_to_cpu(normal, 1);
    scheduler_add_task_to_cpu(idle, 1);
    scheduler_add_task_to_cpu(critical, 1);
    scheduler_add_task_to_cpu(low, 1);
    scheduler_add_task_to_cpu(high, 1);

    /* Wait for all tasks to complete */
    int timeout = 200;
    while ((idle->state != TASK_TERMINATED ||
            low->state != TASK_TERMINATED ||
            normal->state != TASK_TERMINATED ||
            high->state != TASK_TERMINATED ||
            critical->state != TASK_TERMINATED) && timeout > 0) {
        test_delay(50000);
        timeout--;
    }

    /* Verify execution order: CRITICAL(7), HIGH(6), NORMAL(4), LOW(2), IDLE(0) */
    TEST_ASSERT_EQUAL_INT(7, execution_order[0]); /* CRITICAL first */
    TEST_ASSERT_EQUAL_INT(6, execution_order[1]); /* HIGH second */
    TEST_ASSERT_EQUAL_INT(4, execution_order[2]); /* NORMAL third */
    TEST_ASSERT_EQUAL_INT(2, execution_order[3]); /* LOW fourth */
    TEST_ASSERT_EQUAL_INT(0, execution_order[4]); /* IDLE last */

    task_destroy(critical);
    task_destroy(high);
    task_destroy(normal);
    task_destroy(low);
    task_destroy(idle);
}

/*
 * Test: Deadline boost affects execution order
 *
 * Two LOW priority tasks, but one has an urgent deadline.
 * The urgent one should run first due to deadline boost.
 */
static void test_deadline_boost_affects_order(void)
{
    /* Reset execution tracking */
    execution_index = 0;
    for (int i = 0; i < 8; i++) execution_order[i] = -1;

    uint64_t now = slm_get_time_ns();

    /* Create two LOW priority tasks */
    struct task *no_deadline = task_create_with_priority("no_dl", order_tracking_task,
                                                          (void *)1, TASK_PRIORITY_LOW);
    struct task *urgent = task_create_with_priority("urgent", order_tracking_task,
                                                     (void *)2, TASK_PRIORITY_LOW);

    TEST_ASSERT_NOT_NULL(no_deadline);
    TEST_ASSERT_NOT_NULL(urgent);

    /* Set urgent deadline (5ms) - will boost to CRITICAL */
    task_set_deadline(urgent, now + (5 * 1000000ULL));

    /* Add no_deadline first, then urgent */
    scheduler_add_task_to_cpu(no_deadline, 1);
    scheduler_add_task_to_cpu(urgent, 1);

    /* Wait for completion */
    int timeout = 100;
    while ((no_deadline->state != TASK_TERMINATED ||
            urgent->state != TASK_TERMINATED) && timeout > 0) {
        test_delay(50000);
        timeout--;
    }

    /* Urgent (2) should run first despite being added second */
    TEST_ASSERT_EQUAL_INT(2, execution_order[0]); /* Urgent ran first */
    TEST_ASSERT_EQUAL_INT(1, execution_order[1]); /* No deadline ran second */

    task_destroy(urgent);
    task_destroy(no_deadline);
}

/* ============================================================================
 * Unit Tests: Core Isolation
 * ============================================================================ */

/*
 * Test: sched_isolate_core marks core as isolated
 */
static void test_isolate_core_marks_isolated(void)
{
    /* CPU 1 should not be isolated initially */
    TEST_ASSERT_EQUAL_INT(0, sched_is_core_isolated(1));
    TEST_ASSERT_EQUAL_UINT32(0, sched_get_isolated_cores());

    /* Isolate CPU 1 */
    int ret = sched_isolate_core(1);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify it's now isolated */
    TEST_ASSERT_EQUAL_INT(1, sched_is_core_isolated(1));
    TEST_ASSERT_EQUAL_UINT32(0x2, sched_get_isolated_cores()); /* bit 1 set */

    /* Clean up */
    sched_unisolate_core(1);
    TEST_ASSERT_EQUAL_INT(0, sched_is_core_isolated(1));
}

/*
 * Test: Cannot isolate CPU 0 (boot CPU)
 */
static void test_cannot_isolate_cpu0(void)
{
    int ret = sched_isolate_core(0);
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_INT(0, sched_is_core_isolated(0));
}

/*
 * Test: Invalid CPU returns error
 */
static void test_isolate_invalid_cpu(void)
{
    int ret = sched_isolate_core(99);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: Task with CPU_AFFINITY_ANY avoids isolated cores
 *
 * Isolate CPU 1, create a task with CPU_AFFINITY_ANY.
 * The task should be placed on CPU 0 (not the isolated CPU 1).
 */
static void test_affinity_any_avoids_isolated(void)
{
    /* Isolate CPU 1 */
    sched_isolate_core(1);

    /* Create task with no affinity (CPU_AFFINITY_ANY) */
    struct task *t = task_create_with_priority("any_aff", nop_entry, NULL, TASK_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT32(CPU_AFFINITY_ANY, t->cpu_affinity);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    scheduler_add_task(t);

    /* Task should be assigned to CPU 0 (not isolated CPU 1) */
    TEST_ASSERT_EQUAL_UINT32(0, t->assigned_cpu);

    scheduler_remove_task(t);
    irq_restore(flags);

    /* Clean up */
    sched_unisolate_core(1);
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Pinned task runs on isolated core
 *
 * Isolate CPU 1, create a task pinned to CPU 1.
 * The task should still be placed on CPU 1 despite isolation.
 */
static void test_pinned_task_runs_on_isolated(void)
{
    /* Isolate CPU 1 */
    sched_isolate_core(1);

    /* Create task pinned to CPU 1 */
    struct task *t = task_create_with_priority("pinned", nop_entry, NULL, TASK_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(t);

    /* Set affinity to CPU 1 before adding to scheduler */
    t->cpu_affinity = 1;

    /* Disable interrupts */
    irq_flags_t flags = irq_save();

    scheduler_add_task(t);

    /* Task should be on isolated CPU 1 (pinned overrides isolation) */
    TEST_ASSERT_EQUAL_UINT32(1, t->assigned_cpu);

    scheduler_remove_task(t);
    irq_restore(flags);

    /* Clean up */
    sched_unisolate_core(1);
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: sched_set_task_affinity updates affinity
 */
static void test_set_affinity_updates_field(void)
{
    struct task *t = task_create("aff_test", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);

    /* Default is CPU_AFFINITY_ANY */
    TEST_ASSERT_EQUAL_UINT32(CPU_AFFINITY_ANY, t->cpu_affinity);

    /* Set to CPU 1 */
    int ret = sched_set_task_affinity(t, 1);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1, t->cpu_affinity);

    /* Set back to ANY */
    ret = sched_set_task_affinity(t, CPU_AFFINITY_ANY);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(CPU_AFFINITY_ANY, t->cpu_affinity);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: sched_set_task_affinity rejects invalid CPU
 */
static void test_set_affinity_invalid_cpu(void)
{
    struct task *t = task_create("aff_inv", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);

    int ret = sched_set_task_affinity(t, 99);
    TEST_ASSERT_EQUAL_INT(-1, ret);

    /* Affinity should be unchanged */
    TEST_ASSERT_EQUAL_UINT32(CPU_AFFINITY_ANY, t->cpu_affinity);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Multiple cores can be isolated
 */
static void test_multiple_cores_isolated(void)
{
    /* Isolate CPUs 1, 2, 3 (if available) */
    if (cpu_count >= 2) sched_isolate_core(1);
    if (cpu_count >= 3) sched_isolate_core(2);
    if (cpu_count >= 4) sched_isolate_core(3);

    uint32_t expected = 0;
    if (cpu_count >= 2) expected |= 0x2;
    if (cpu_count >= 3) expected |= 0x4;
    if (cpu_count >= 4) expected |= 0x8;

    TEST_ASSERT_EQUAL_UINT32(expected, sched_get_isolated_cores());

    /* Task with CPU_AFFINITY_ANY should go to CPU 0 (only non-isolated) */
    struct task *t = task_create_with_priority("multi", nop_entry, NULL, TASK_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(t);

    irq_flags_t flags = irq_save();
    scheduler_add_task(t);
    TEST_ASSERT_EQUAL_UINT32(0, t->assigned_cpu);
    scheduler_remove_task(t);
    irq_restore(flags);

    /* Clean up */
    if (cpu_count >= 2) sched_unisolate_core(1);
    if (cpu_count >= 3) sched_unisolate_core(2);
    if (cpu_count >= 4) sched_unisolate_core(3);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_scheduler(void)
{
    UnityBegin("Scheduler Tests");

    /* Unit tests: Deadline boost logic */
    RUN_TEST(test_no_deadline_no_boost);
    RUN_TEST(test_deadline_field_set);
    RUN_TEST(test_distant_deadline_no_boost);
    RUN_TEST(test_deadline_100ms_boost_plus_one);
    RUN_TEST(test_deadline_50ms_boost_to_high);
    RUN_TEST(test_deadline_10ms_boost_to_critical);
    RUN_TEST(test_missed_deadline_boost_to_critical);

    /* Unit tests: Priority setting */
    RUN_TEST(test_set_priority_updates_field);
    RUN_TEST(test_priority_clamped_to_max);

    /* Unit tests: FFI task API */
    RUN_TEST(test_ffi_task_create_returns_id);
    RUN_TEST(test_ffi_set_priority);
    RUN_TEST(test_ffi_set_deadline);
    RUN_TEST(test_ffi_invalid_task_id);

    /* Unit tests: Core isolation */
    RUN_TEST(test_isolate_core_marks_isolated);
    RUN_TEST(test_cannot_isolate_cpu0);
    RUN_TEST(test_isolate_invalid_cpu);
    RUN_TEST(test_affinity_any_avoids_isolated);
    RUN_TEST(test_pinned_task_runs_on_isolated);
    RUN_TEST(test_set_affinity_updates_field);
    RUN_TEST(test_set_affinity_invalid_cpu);
    RUN_TEST(test_multiple_cores_isolated);

    /* Integration tests: Priority ordering */
    RUN_TEST(test_high_priority_runs_first);
    RUN_TEST(test_priority_ordering_multiple_levels);
    RUN_TEST(test_deadline_boost_affects_order);

    return UnityEnd();
}
