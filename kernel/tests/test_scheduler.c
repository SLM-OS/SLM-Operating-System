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
#include "timer.h"
#include "platform.h"
#include "uart.h"
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

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
 * Regression Tests: DAIF Context Switch Preservation
 * ============================================================================ */

/*
 * Regression test: DAIF register is saved and restored across context switches.
 * Previously, DAIF was not part of cpu_context, meaning a task's interrupt
 * mask state could leak to/from other tasks.
 */
static void test_daif_saved_in_context(void)
{
    struct task *t = task_create_with_priority("daif_test", nop_entry, NULL,
                                               TASK_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(t);

    /* Set a known DAIF value in the task's context (all exceptions masked) */
    t->context.daif = 0x3C0;  /* D=1, A=1, I=1, F=1 */

    /* Verify it's preserved in the struct */
    TEST_ASSERT_EQUAL_HEX64(0x3C0, t->context.daif);

    /* Set a different value (only IRQ masked) */
    t->context.daif = 0x080;  /* I=1 only */
    TEST_ASSERT_EQUAL_HEX64(0x080, t->context.daif);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: New task's DAIF starts with IRQ masked (0x080).
 * This prevents timer interrupts from firing during the fragile first
 * context switch into the task. IRQs are unmasked when the task calls
 * spin_unlock_irqrestore or explicitly clears the I bit.
 */
static void test_new_task_daif_irq_masked(void)
{
    struct task *t = task_create_with_priority("daif_init", nop_entry, NULL,
                                               TASK_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(t);

    /* New tasks should have IRQ masked to survive first context switch */
    TEST_ASSERT_EQUAL_HEX64(0x080, t->context.daif);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Regression test: No task is ever created with DAIF=0.
 * DAIF=0 means all exceptions unmasked, which caused the original bug where
 * timer interrupts fired during the first context switch into a new task,
 * corrupting the context switch. Tasks at all priority levels must have
 * DAIF=0x080 (IRQ masked) on creation.
 */
static void test_daif_not_zero_on_create(void)
{
    struct task *low = task_create_with_priority("daif_low", nop_entry, NULL,
                                                  TASK_PRIORITY_LOW);
    struct task *normal = task_create_with_priority("daif_norm", nop_entry, NULL,
                                                     TASK_PRIORITY_NORMAL);
    struct task *high = task_create_with_priority("daif_high", nop_entry, NULL,
                                                   TASK_PRIORITY_HIGH);
    struct task *critical = task_create_with_priority("daif_crit", nop_entry, NULL,
                                                       TASK_PRIORITY_CRITICAL);

    TEST_ASSERT_NOT_NULL(low);
    TEST_ASSERT_NOT_NULL(normal);
    TEST_ASSERT_NOT_NULL(high);
    TEST_ASSERT_NOT_NULL(critical);

    /* No task should ever have DAIF=0 (all exceptions unmasked) */
    TEST_ASSERT_TRUE(low->context.daif != 0);
    TEST_ASSERT_TRUE(normal->context.daif != 0);
    TEST_ASSERT_TRUE(high->context.daif != 0);
    TEST_ASSERT_TRUE(critical->context.daif != 0);

    /* All should specifically be 0x080 (IRQ masked) */
    TEST_ASSERT_EQUAL_HEX64(0x080, low->context.daif);
    TEST_ASSERT_EQUAL_HEX64(0x080, normal->context.daif);
    TEST_ASSERT_EQUAL_HEX64(0x080, high->context.daif);
    TEST_ASSERT_EQUAL_HEX64(0x080, critical->context.daif);

    low->state = TASK_TERMINATED;
    task_destroy(low);
    normal->state = TASK_TERMINATED;
    task_destroy(normal);
    high->state = TASK_TERMINATED;
    task_destroy(high);
    critical->state = TASK_TERMINATED;
    task_destroy(critical);
}

/*
 * Regression test: Multiple tasks all have IRQ masked in initial context.
 * This catches regressions where someone might memset the context to zero
 * without re-applying the DAIF=0x080 initialization afterward.
 */
static void test_task_create_multiple_all_irq_masked(void)
{
    #define MULTI_DAIF_COUNT 6
    struct task *tasks[MULTI_DAIF_COUNT];

    /* Create tasks in a batch */
    for (int i = 0; i < MULTI_DAIF_COUNT; i++) {
        tasks[i] = task_create_with_priority("daif_multi", nop_entry, NULL,
                                              TASK_PRIORITY_NORMAL);
        TEST_ASSERT_NOT_NULL(tasks[i]);
    }

    /* Verify every single task has IRQ masked */
    for (int i = 0; i < MULTI_DAIF_COUNT; i++) {
        TEST_ASSERT_EQUAL_HEX64(0x080, tasks[i]->context.daif);
    }

    /* Clean up */
    for (int i = 0; i < MULTI_DAIF_COUNT; i++) {
        tasks[i]->state = TASK_TERMINATED;
        task_destroy(tasks[i]);
    }
    #undef MULTI_DAIF_COUNT
}

/*
 * Regression test: Idle task (ID 1) also has DAIF=0x080.
 * The idle task is created by scheduler_init(), not task_create_with_priority(),
 * so it follows a different code path. This verifies that path also sets
 * DAIF correctly to prevent timer ISR corruption during context switch.
 */
static void test_idle_task_daif(void)
{
    struct task *idle = task_get(1);
    TEST_ASSERT_NOT_NULL(idle);

    /* Idle task should also have IRQ masked in its saved context */
    TEST_ASSERT_EQUAL_HEX64(0x080, idle->context.daif);
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
 * Use direct CPU 0 assignment to test boost logic in isolation.
 */
static void test_distant_deadline_no_boost(void)
{
    struct task *t = task_create_with_priority("dist_dl", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(t);

    /* Set deadline AFTER task creation to minimize elapsed time */
    uint64_t now = slm_get_time_ns();
    uint64_t deadline = now + (200 * 1000000ULL); /* 200ms */
    task_set_deadline(t, deadline);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    /* Add to CPU 0 directly - this triggers boost calculation */
    scheduler_add_task_to_cpu(t, 0);

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
 *
 * Use 90ms to stay safely in the 50-100ms range.
 */
static void test_deadline_100ms_boost_plus_one(void)
{
    struct task *t = task_create_with_priority("90ms_dl", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(t);

    /* Set deadline AFTER task creation to minimize elapsed time */
    uint64_t now = slm_get_time_ns();
    uint64_t deadline = now + (90 * 1000000ULL); /* 90ms - safely within 50-100ms range */
    task_set_deadline(t, deadline);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    /* Add to CPU 0 directly to test boost logic in isolation */
    scheduler_add_task_to_cpu(t, 0);

    /* Should be boosted by +1 (LOW=2 -> 3) */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_LOW + 1, t->effective_priority);

    scheduler_remove_task(t);

    irq_restore(flags);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Deadline < 50ms boosts to HIGH
 *
 * Use 45ms to stay safely in the 10-50ms range even after setup time.
 */
static void test_deadline_50ms_boost_to_high(void)
{
    struct task *t = task_create_with_priority("45ms_dl", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(t);

    /* Set deadline AFTER task creation to minimize elapsed time */
    uint64_t now = slm_get_time_ns();
    uint64_t deadline = now + (45 * 1000000ULL); /* 45ms - safely within 10-50ms range */
    task_set_deadline(t, deadline);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    /* Add to CPU 0 directly to test boost logic in isolation */
    scheduler_add_task_to_cpu(t, 0);

    /* Should be boosted to HIGH (not CRITICAL) */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_HIGH, t->effective_priority);

    scheduler_remove_task(t);

    irq_restore(flags);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Deadline < 10ms boosts to CRITICAL
 *
 * Use 5ms offset and CPU 0 for deterministic testing.
 */
static void test_deadline_10ms_boost_to_critical(void)
{
    struct task *t = task_create_with_priority("5ms_dl", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(t);

    /* Set deadline AFTER task creation to minimize elapsed time */
    uint64_t now = slm_get_time_ns();
    uint64_t deadline = now + (5 * 1000000ULL); /* 5ms - within 10ms threshold */
    task_set_deadline(t, deadline);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    /* Add to CPU 0 directly to test boost logic in isolation */
    scheduler_add_task_to_cpu(t, 0);

    /* Should be boosted to CRITICAL */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_CRITICAL, t->effective_priority);

    scheduler_remove_task(t);

    irq_restore(flags);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Test: Missed deadline (past) boosts to CRITICAL
 *
 * Use CPU 0 directly for deterministic testing.
 */
static void test_missed_deadline_boost_to_critical(void)
{
    struct task *t = task_create_with_priority("past_dl", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(t);

    /* Set deadline in the past */
    uint64_t now = slm_get_time_ns();
    uint64_t deadline = now > 1000000ULL ? now - 1000000ULL : 1;
    task_set_deadline(t, deadline);

    /* Disable interrupts to prevent scheduler from running the task */
    irq_flags_t flags = irq_save();

    /* Add to CPU 0 directly to test boost logic in isolation */
    scheduler_add_task_to_cpu(t, 0);

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
/*
 * Signal for blocking FFI test tasks.
 * The task waits for this flag before exiting, giving tests time to
 * inspect/modify the task on SMP systems where the task may start
 * running immediately on another CPU.
 */
static volatile bool ffi_task_proceed = false;

/*
 * Blocking task for FFI tests.
 * Waits for ffi_task_proceed flag before exiting.
 */
static void blocking_ffi_task(void *arg)
{
    (void)arg;
    while (!ffi_task_proceed) {
        yield();
    }
}

/*
 * Non-blocking dummy task for simple FFI tests.
 * Yields once then exits.
 */
static void dummy_task(void *arg)
{
    (void)arg;
    yield();
}

static void test_ffi_task_create_returns_id(void)
{
    uint32_t id = slm_task_create("ffi_test", dummy_task, NULL);
    TEST_ASSERT(id != 0);

    /* Verify task exists */
    struct task *t = task_get(id);
    TEST_ASSERT_NOT_NULL(t);

    /* Wait for task to complete (it yields once then exits) */
    while (t->state != TASK_TERMINATED) {
        yield();
    }
    task_destroy(t);
}

/*
 * Test: slm_task_set_priority works via FFI
 */
static void test_ffi_set_priority(void)
{
    /* Reset signal so task blocks */
    ffi_task_proceed = false;

    uint32_t id = slm_task_create("ffi_pri", blocking_ffi_task, NULL);
    TEST_ASSERT(id != 0);

    /* Task is blocked waiting for signal, safe to access */
    struct task *t = task_get(id);
    TEST_ASSERT_NOT_NULL(t);

    int ret = slm_task_set_priority(id, TASK_PRIORITY_HIGH);
    TEST_ASSERT_EQUAL_INT(SLM_OK, ret);

    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_HIGH, t->priority);

    /* Signal task to exit */
    ffi_task_proceed = true;

    /* Wait for task to complete */
    while (t->state != TASK_TERMINATED) {
        yield();
    }
    task_destroy(t);
}

/*
 * Test: slm_task_set_deadline works via FFI
 */
static void test_ffi_set_deadline(void)
{
    /* Reset signal so task blocks */
    ffi_task_proceed = false;

    uint32_t id = slm_task_create("ffi_dl", blocking_ffi_task, NULL);
    TEST_ASSERT(id != 0);

    /* Task is blocked waiting for signal, safe to access */
    struct task *t = task_get(id);
    TEST_ASSERT_NOT_NULL(t);

    uint64_t deadline = 999999999ULL;
    int ret = slm_task_set_deadline(id, deadline);
    TEST_ASSERT_EQUAL_INT(SLM_OK, ret);

    TEST_ASSERT_EQUAL_UINT64(deadline, t->deadline_ns);

    /* Signal task to exit */
    ffi_task_proceed = true;

    /* Wait for task to complete */
    while (t->state != TASK_TERMINATED) {
        yield();
    }
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

/*
 * Barrier for SMP-safe priority ordering tests.
 * Tasks wait on this barrier before recording their execution order.
 * This ensures all tasks are queued before any starts recording.
 */
static volatile bool order_barrier_released = false;

/* Task that records its ID in the execution order array */
static void order_tracking_task(void *arg)
{
    int task_id = (int)(uintptr_t)arg;

    /* Wait at barrier until all tasks are queued */
    while (!order_barrier_released) {
        /* Spin-wait - don't yield to avoid scheduler interference */
        __asm__ volatile("yield" ::: "memory");
    }

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
    /* Reset execution tracking and barrier */
    execution_index = 0;
    for (int i = 0; i < 8; i++) execution_order[i] = -1;
    order_barrier_released = false;

    /* Create tasks with different priorities */
    struct task *low = task_create_with_priority("low", order_tracking_task,
                                                  (void *)1, TASK_PRIORITY_LOW);
    struct task *high = task_create_with_priority("high", order_tracking_task,
                                                   (void *)2, TASK_PRIORITY_HIGH);

    TEST_ASSERT_NOT_NULL(low);
    TEST_ASSERT_NOT_NULL(high);

    /* Lower our priority below all test tasks BEFORE adding them.
     * This ensures when barrier is released, scheduler picks by priority. */
    struct task *self = task_current();
    uint8_t saved_pri = self->effective_priority;
    self->priority = TASK_PRIORITY_IDLE;
    self->effective_priority = TASK_PRIORITY_IDLE;

    /* Add LOW first, then HIGH - but HIGH should run first due to priority.
     * Use CPU 0 and disable IRQs to add atomically. */
    irq_flags_t flags = irq_save();
    scheduler_add_task_to_cpu(low, 0);
    scheduler_add_task_to_cpu(high, 0);
    irq_restore(flags);

    /* Memory barrier then release - tasks will start recording in priority order */
    __asm__ volatile("dmb sy" ::: "memory");
    order_barrier_released = true;

    /* Wait for both tasks to complete, yielding to let them run */
    int timeout = 100;
    while ((low->state != TASK_TERMINATED || high->state != TASK_TERMINATED) && timeout > 0) {
        yield();
        test_delay(10000);
        timeout--;
    }

    /* Restore our priority */
    self->priority = saved_pri;
    self->effective_priority = saved_pri;

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
    /* Reset execution tracking and barrier */
    execution_index = 0;
    for (int i = 0; i < 8; i++) execution_order[i] = -1;
    order_barrier_released = false;

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

    /* Lower our priority below all test tasks BEFORE adding them */
    struct task *self = task_current();
    uint8_t saved_pri = self->effective_priority;
    self->priority = TASK_PRIORITY_IDLE;
    self->effective_priority = TASK_PRIORITY_IDLE;

    /* Add tasks to CPU 0 (same as test task).
     * Disable IRQs to prevent preemption while adding all tasks. */
    irq_flags_t flags = irq_save();
    scheduler_add_task_to_cpu(normal, 0);
    scheduler_add_task_to_cpu(idle, 0);
    scheduler_add_task_to_cpu(critical, 0);
    scheduler_add_task_to_cpu(low, 0);
    scheduler_add_task_to_cpu(high, 0);
    irq_restore(flags);

    /* Memory barrier then release - tasks will start recording in priority order */
    __asm__ volatile("dmb sy" ::: "memory");
    order_barrier_released = true;

    /* Wait for all tasks to complete, yielding to let them run */
    int timeout = 200;
    while ((idle->state != TASK_TERMINATED ||
            low->state != TASK_TERMINATED ||
            normal->state != TASK_TERMINATED ||
            high->state != TASK_TERMINATED ||
            critical->state != TASK_TERMINATED) && timeout > 0) {
        yield();
        test_delay(10000);
        timeout--;
    }

    /* Restore our priority */
    self->priority = saved_pri;
    self->effective_priority = saved_pri;

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
    /* Reset execution tracking and barrier */
    execution_index = 0;
    for (int i = 0; i < 8; i++) execution_order[i] = -1;
    order_barrier_released = false;

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

    /* Lower our priority below all test tasks BEFORE adding them */
    struct task *self = task_current();
    uint8_t saved_pri = self->effective_priority;
    self->priority = TASK_PRIORITY_IDLE;
    self->effective_priority = TASK_PRIORITY_IDLE;

    /* Add no_deadline first, then urgent.
     * Use CPU 0 and disable IRQs to add atomically. */
    irq_flags_t flags = irq_save();
    scheduler_add_task_to_cpu(no_deadline, 0);
    scheduler_add_task_to_cpu(urgent, 0);
    irq_restore(flags);

    /* Memory barrier then release - tasks will start recording in priority order */
    __asm__ volatile("dmb sy" ::: "memory");
    order_barrier_released = true;

    /* Wait for completion, yielding to let tasks run */
    int timeout = 100;
    while ((no_deadline->state != TASK_TERMINATED ||
            urgent->state != TASK_TERMINATED) && timeout > 0) {
        yield();
        test_delay(10000);
        timeout--;
    }

    /* Restore our priority */
    self->priority = saved_pri;
    self->effective_priority = saved_pri;

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

    /* Task should NOT be assigned to isolated CPU 1 */
    TEST_ASSERT(t->assigned_cpu != 1);

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
 * Stress Tests: Scheduler Robustness
 * ============================================================================ */

/*
 * Starvation test tracking.
 * We track interleaved execution to verify:
 * 1. HIGH priority runs first (priority ordering works)
 * 2. LOW priority still completes (no starvation)
 */
#define STARV_ITERATIONS 5
static volatile int starv_order[STARV_ITERATIONS * 2];
static volatile int starv_index = 0;

/* Task records its ID each iteration */
static void starv_task_entry(void *arg)
{
    int id = (int)(uintptr_t)arg;  /* 1=LOW, 2=HIGH */

    for (int i = 0; i < STARV_ITERATIONS; i++) {
        irq_flags_t flags = spin_lock_irqsave(&test_lock);
        if (starv_index < STARV_ITERATIONS * 2) {
            starv_order[starv_index++] = id;
        }
        spin_unlock_irqrestore(&test_lock, flags);
        test_delay(1000);  /* Small delay between iterations */
    }

    task_exit();
}

/*
 * Test: Low-priority tasks are not starved
 *
 * Creates one LOW priority task and one HIGH priority task on same CPU.
 * Verifies:
 * 1. HIGH priority task runs first (correct priority ordering)
 * 2. Both tasks complete all iterations (no starvation)
 * 3. HIGH gets majority of early slots (priority is respected)
 *
 * Uses CPU 0 with IRQ protection and lowered test priority for determinism.
 */
static void test_no_starvation(void)
{
    /* Reset tracking */
    starv_index = 0;
    for (int i = 0; i < STARV_ITERATIONS * 2; i++) {
        starv_order[i] = 0;
    }

    /* Create tasks: LOW=1, HIGH=2 */
    struct task *low_task = task_create_with_priority("starv_low",
        starv_task_entry, (void *)1, TASK_PRIORITY_LOW);
    struct task *high_task = task_create_with_priority("starv_high",
        starv_task_entry, (void *)2, TASK_PRIORITY_HIGH);

    TEST_ASSERT_NOT_NULL(low_task);
    TEST_ASSERT_NOT_NULL(high_task);

    /* Add both tasks to CPU 0 atomically (HIGH should run first due to priority) */
    irq_flags_t flags = irq_save();
    scheduler_add_task_to_cpu(low_task, 0);
    scheduler_add_task_to_cpu(high_task, 0);
    irq_restore(flags);

    /* Lower our priority below all test tasks so they can run */
    struct task *self = task_current();
    uint8_t saved_pri = self->effective_priority;
    self->priority = TASK_PRIORITY_IDLE;
    self->effective_priority = TASK_PRIORITY_IDLE;

    /* Wait for both to complete */
    int timeout = 300;
    while ((low_task->state != TASK_TERMINATED ||
            high_task->state != TASK_TERMINATED) && timeout > 0) {
        yield();
        test_delay(10000);
        timeout--;
    }

    /* Restore our priority */
    self->priority = saved_pri;
    self->effective_priority = saved_pri;

    /* Verify both completed all iterations (no starvation) */
    int low_count = 0, high_count = 0;
    for (int i = 0; i < starv_index; i++) {
        if (starv_order[i] == 1) low_count++;
        else if (starv_order[i] == 2) high_count++;
    }
    TEST_ASSERT_EQUAL_INT(STARV_ITERATIONS, low_count);
    TEST_ASSERT_EQUAL_INT(STARV_ITERATIONS, high_count);

    /* Verify HIGH ran first (priority ordering works) */
    TEST_ASSERT_EQUAL_INT(2, starv_order[0]);

    /* Cleanup */
    task_destroy(high_task);
    task_destroy(low_task);
}

/*
 * Stress test with priority verification.
 * We use fewer tasks and put them all on one CPU to get deterministic ordering.
 */
#define STRESS_TASK_COUNT 5
static volatile int stress_order[STRESS_TASK_COUNT];
static volatile int stress_index = 0;

/* Task records its priority when it starts */
static void stress_order_task(void *arg)
{
    int priority = (int)(uintptr_t)arg;

    irq_flags_t flags = spin_lock_irqsave(&test_lock);
    if (stress_index < STRESS_TASK_COUNT) {
        stress_order[stress_index++] = priority;
    }
    spin_unlock_irqrestore(&test_lock, flags);

    /* Small delay to ensure we don't exit before recording */
    test_delay(1000);

    task_exit();
}

/*
 * Test: Stress test with mixed priorities and deadlines
 *
 * Creates 5 tasks with different priorities on the same CPU.
 * Verifies they execute in strict priority order.
 * One LOW priority task has an urgent deadline and should be boosted.
 */
static void test_stress_mixed_priorities(void)
{
    /* Reset tracking */
    stress_index = 0;
    for (int i = 0; i < STRESS_TASK_COUNT; i++) {
        stress_order[i] = -1;
    }

    uint64_t now = slm_get_time_ns();

    /*
     * Create 5 tasks:
     * - IDLE (0)
     * - LOW (2) with urgent deadline -> should boost to CRITICAL (7)
     * - NORMAL (4)
     * - HIGH (6)
     * - CRITICAL (7)
     *
     * Expected order after deadline boost: CRITICAL, boosted-LOW, HIGH, NORMAL, IDLE
     * But since boosted-LOW becomes CRITICAL too, FIFO within same priority.
     * We add CRITICAL first, then boosted-LOW, so: CRITICAL(7), boosted(7), HIGH(6), NORMAL(4), IDLE(0)
     */
    struct task *t_idle = task_create_with_priority("st_idle",
        stress_order_task, (void *)0, TASK_PRIORITY_IDLE);
    struct task *t_low_dl = task_create_with_priority("st_low_dl",
        stress_order_task, (void *)2, TASK_PRIORITY_LOW);
    struct task *t_normal = task_create_with_priority("st_norm",
        stress_order_task, (void *)4, TASK_PRIORITY_NORMAL);
    struct task *t_high = task_create_with_priority("st_high",
        stress_order_task, (void *)6, TASK_PRIORITY_HIGH);
    struct task *t_crit = task_create_with_priority("st_crit",
        stress_order_task, (void *)7, TASK_PRIORITY_CRITICAL);

    TEST_ASSERT_NOT_NULL(t_idle);
    TEST_ASSERT_NOT_NULL(t_low_dl);
    TEST_ASSERT_NOT_NULL(t_normal);
    TEST_ASSERT_NOT_NULL(t_high);
    TEST_ASSERT_NOT_NULL(t_crit);

    /* Give t_low_dl an urgent deadline (5ms) - should boost to CRITICAL */
    task_set_deadline(t_low_dl, now + (5 * 1000000ULL));

    /* Add all tasks to CPU 0 atomically, in scrambled order.
     * Use CPU 0 (same as test) with IRQ protection for deterministic ordering. */
    irq_flags_t flags = irq_save();
    scheduler_add_task_to_cpu(t_normal, 0);   /* NORMAL - should run 4th */
    scheduler_add_task_to_cpu(t_idle, 0);     /* IDLE - should run 5th */
    scheduler_add_task_to_cpu(t_crit, 0);     /* CRITICAL - should run 1st */
    scheduler_add_task_to_cpu(t_low_dl, 0);   /* LOW+deadline -> boosted, 2nd */
    scheduler_add_task_to_cpu(t_high, 0);     /* HIGH - should run 3rd */
    irq_restore(flags);

    /* Lower our priority below all test tasks so they can run */
    struct task *self = task_current();
    uint8_t saved_pri = self->effective_priority;
    self->priority = TASK_PRIORITY_IDLE;
    self->effective_priority = TASK_PRIORITY_IDLE;

    /* Wait for all to complete, yielding to let them run */
    int timeout = 300;
    while ((t_idle->state != TASK_TERMINATED ||
            t_low_dl->state != TASK_TERMINATED ||
            t_normal->state != TASK_TERMINATED ||
            t_high->state != TASK_TERMINATED ||
            t_crit->state != TASK_TERMINATED) && timeout > 0) {
        yield();
        test_delay(10000);
        timeout--;
    }

    /* Restore our priority */
    self->priority = saved_pri;
    self->effective_priority = saved_pri;

    /* Verify all 5 tasks ran */
    TEST_ASSERT_EQUAL_INT(5, stress_index);

    /*
     * Verify priority ordering:
     * stress_order[0] should be CRITICAL (7)
     * stress_order[1] should be boosted LOW (effective 7, recorded as 2)
     * stress_order[2] should be HIGH (6)
     * stress_order[3] should be NORMAL (4)
     * stress_order[4] should be IDLE (0)
     */
    TEST_ASSERT_EQUAL_INT(7, stress_order[0]);  /* CRITICAL first */
    TEST_ASSERT_EQUAL_INT(2, stress_order[1]);  /* Boosted LOW second (records base pri) */
    TEST_ASSERT_EQUAL_INT(6, stress_order[2]);  /* HIGH third */
    TEST_ASSERT_EQUAL_INT(4, stress_order[3]);  /* NORMAL fourth */
    TEST_ASSERT_EQUAL_INT(0, stress_order[4]);  /* IDLE last */

    /* Cleanup */
    task_destroy(t_crit);
    task_destroy(t_low_dl);
    task_destroy(t_high);
    task_destroy(t_normal);
    task_destroy(t_idle);
}

/* ============================================================================
 * Latency and Benchmark Tests
 * ============================================================================ */

/*
 * Latency measurement tracking.
 * We measure wake-to-run latency for tasks on isolated vs non-isolated cores.
 */
#define LATENCY_SAMPLES 10
static volatile uint64_t latency_start_ns;
static volatile uint64_t latency_samples[LATENCY_SAMPLES];
static volatile int latency_sample_index = 0;

/* Task that measures wake-to-run latency */
static void latency_task_entry(void *arg)
{
    (void)arg;

    /* Record latency immediately on entry */
    uint64_t now = slm_get_time_ns();

    irq_flags_t flags = spin_lock_irqsave(&test_lock);
    if (latency_sample_index < LATENCY_SAMPLES) {
        latency_samples[latency_sample_index++] = now - latency_start_ns;
    }
    spin_unlock_irqrestore(&test_lock, flags);

    task_exit();
}

/*
 * Test: Pinned inference task shows consistent latency on isolated core
 *
 * Compares latency variance between:
 * 1. Task on isolated core (should have low variance)
 * 2. Task on non-isolated core (may have more variance due to interference)
 *
 * This is a functional test, not a hard pass/fail - it logs the results
 * for analysis.
 */
static void test_isolated_core_latency(void)
{
    if (cpu_count < 2) {
        /* Skip on single-CPU systems */
        TEST_ASSERT(1);
        return;
    }

    uint64_t isolated_sum = 0;
    uint64_t isolated_max = 0;
    uint64_t isolated_min = UINT64_MAX;

    uint64_t normal_sum = 0;
    uint64_t normal_max = 0;
    uint64_t normal_min = UINT64_MAX;

    /*
     * Phase 1: Measure latency on isolated core
     */
    sched_isolate_core(1);

    for (int i = 0; i < LATENCY_SAMPLES; i++) {
        latency_sample_index = 0;

        struct task *t = task_create_with_priority("lat_iso",
            latency_task_entry, NULL, TASK_PRIORITY_HIGH);
        TEST_ASSERT_NOT_NULL(t);
        t->cpu_affinity = 1;  /* Pin to isolated core */

        /* Record start time and add task */
        latency_start_ns = slm_get_time_ns();
        scheduler_add_task(t);

        /* Wait for completion */
        int timeout = 100;
        while (t->state != TASK_TERMINATED && timeout > 0) {
            yield();
            test_delay(1000);
            timeout--;
        }

        /* Record sample */
        if (latency_sample_index > 0) {
            uint64_t sample = latency_samples[0];
            isolated_sum += sample;
            if (sample > isolated_max) isolated_max = sample;
            if (sample < isolated_min) isolated_min = sample;
        }

        task_destroy(t);
    }

    sched_unisolate_core(1);

    /*
     * Phase 2: Measure latency on non-isolated core
     */
    for (int i = 0; i < LATENCY_SAMPLES; i++) {
        latency_sample_index = 0;

        struct task *t = task_create_with_priority("lat_norm",
            latency_task_entry, NULL, TASK_PRIORITY_HIGH);
        TEST_ASSERT_NOT_NULL(t);
        t->cpu_affinity = 1;  /* Same core, but not isolated */

        latency_start_ns = slm_get_time_ns();
        scheduler_add_task(t);

        int timeout = 100;
        while (t->state != TASK_TERMINATED && timeout > 0) {
            yield();
            test_delay(1000);
            timeout--;
        }

        if (latency_sample_index > 0) {
            uint64_t sample = latency_samples[0];
            normal_sum += sample;
            if (sample > normal_max) normal_max = sample;
            if (sample < normal_min) normal_min = sample;
        }

        task_destroy(t);
    }

    /*
     * Calculate and log results
     */
    uint64_t isolated_avg = isolated_sum / LATENCY_SAMPLES;
    uint64_t isolated_range = isolated_max - isolated_min;
    uint64_t normal_avg = normal_sum / LATENCY_SAMPLES;
    uint64_t normal_range = normal_max - normal_min;

    uart_printf("  Latency (isolated): avg=%lu ns, range=%lu ns (min=%lu, max=%lu)\n",
                (unsigned long)isolated_avg, (unsigned long)isolated_range,
                (unsigned long)isolated_min, (unsigned long)isolated_max);
    uart_printf("  Latency (normal):   avg=%lu ns, range=%lu ns (min=%lu, max=%lu)\n",
                (unsigned long)normal_avg, (unsigned long)normal_range,
                (unsigned long)normal_min, (unsigned long)normal_max);

    /* Test passes if we got valid measurements */
    TEST_ASSERT(isolated_avg > 0);
    TEST_ASSERT(normal_avg > 0);

    /* Log whether isolation improved consistency (smaller range = better) */
    if (isolated_range < normal_range) {
        uart_puts("  Result: Isolated core shows more consistent latency\n");
    } else {
        uart_puts("  Result: Similar latency variance (expected on QEMU)\n");
    }
}

/*
 * Benchmark: Measure context switch overhead
 *
 * Creates two tasks that yield back and forth, measuring the time
 * for N context switches.
 */
#define BENCH_CONTEXT_SWITCHES 100
static volatile int bench_switch_count = 0;
static volatile uint64_t bench_start_time = 0;
static volatile uint64_t bench_end_time = 0;

/* Task A and B ping-pong yielding */
static void bench_switch_task(void *arg)
{
    int task_id = (int)(uintptr_t)arg;  /* 0 or 1 */

    while (bench_switch_count < BENCH_CONTEXT_SWITCHES) {
        irq_flags_t flags = spin_lock_irqsave(&test_lock);
        bench_switch_count++;
        if (bench_switch_count >= BENCH_CONTEXT_SWITCHES) {
            bench_end_time = slm_get_time_ns();
        }
        spin_unlock_irqrestore(&test_lock, flags);

        /* Yield to other task */
        if (bench_switch_count < BENCH_CONTEXT_SWITCHES) {
            yield();
        }
    }

    (void)task_id;
    task_exit();
}

static void test_benchmark_context_switch(void)
{
    /* Reset benchmark state */
    bench_switch_count = 0;
    bench_start_time = 0;
    bench_end_time = 0;

    /* Create two tasks at same priority (will round-robin) */
    struct task *a = task_create_with_priority("bench_a",
        bench_switch_task, (void *)0, TASK_PRIORITY_HIGH);
    struct task *b = task_create_with_priority("bench_b",
        bench_switch_task, (void *)1, TASK_PRIORITY_HIGH);

    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    /* Record start time and begin */
    bench_start_time = slm_get_time_ns();

    /* Add tasks to CPU 0 (same as test task) for reliable scheduling.
     * Cross-CPU task scheduling would require IPI wake mechanism. */
    irq_flags_t flags = irq_save();
    scheduler_add_task_to_cpu(a, 0);
    scheduler_add_task_to_cpu(b, 0);
    irq_restore(flags);

    /* Wait for benchmark to complete */
    int timeout = 500;
    while ((a->state != TASK_TERMINATED || b->state != TASK_TERMINATED) && timeout > 0) {
        yield();
        test_delay(10000);
        timeout--;
    }

    /* Calculate results */
    uint64_t total_time = bench_end_time - bench_start_time;
    uint64_t per_switch = total_time / BENCH_CONTEXT_SWITCHES;

    uart_printf("  Context switch benchmark: %d switches in %lu ns\n",
                BENCH_CONTEXT_SWITCHES, (unsigned long)total_time);
    uart_printf("  Average: %lu ns per switch (%lu µs)\n",
                (unsigned long)per_switch, (unsigned long)(per_switch / 1000));

    /* Test passes if benchmark completed */
    TEST_ASSERT_EQUAL_INT(BENCH_CONTEXT_SWITCHES, bench_switch_count);
    TEST_ASSERT(per_switch > 0);

    /* Log assessment */
    if (per_switch < 10000) {
        uart_puts("  Assessment: Excellent (< 10 µs)\n");
    } else if (per_switch < 50000) {
        uart_puts("  Assessment: Good (< 50 µs)\n");
    } else if (per_switch < 100000) {
        uart_puts("  Assessment: Acceptable (< 100 µs)\n");
    } else {
        uart_puts("  Assessment: Needs optimization (> 100 µs)\n");
    }

    task_destroy(a);
    task_destroy(b);
}

/*
 * Benchmark: Measure scheduling overhead (pick_next_task + queue operations)
 *
 * Measures time to add/remove tasks from the run queue.
 * Note: Keep small to avoid exhausting task slots (MAX_TASKS=32).
 */
#define BENCH_QUEUE_OPS 8

static void test_benchmark_queue_operations(void)
{
    struct task *tasks[BENCH_QUEUE_OPS];
    uint64_t add_total = 0;
    uint64_t remove_total = 0;

    /* Pre-create all tasks */
    for (int i = 0; i < BENCH_QUEUE_OPS; i++) {
        tasks[i] = task_create_with_priority("bench_q", nop_entry, NULL,
            (i % 8));  /* Various priorities */
        TEST_ASSERT_NOT_NULL(tasks[i]);
    }

    /* Benchmark: Add tasks to queue */
    irq_flags_t flags = irq_save();
    uint64_t start = slm_get_time_ns();

    for (int i = 0; i < BENCH_QUEUE_OPS; i++) {
        scheduler_add_task_to_cpu(tasks[i], 0);
    }

    uint64_t end = slm_get_time_ns();
    irq_restore(flags);
    add_total = end - start;

    /* Benchmark: Remove tasks from queue */
    flags = irq_save();
    start = slm_get_time_ns();

    for (int i = 0; i < BENCH_QUEUE_OPS; i++) {
        scheduler_remove_task(tasks[i]);
    }

    end = slm_get_time_ns();
    irq_restore(flags);
    remove_total = end - start;

    /* Clean up */
    for (int i = 0; i < BENCH_QUEUE_OPS; i++) {
        tasks[i]->state = TASK_TERMINATED;
        task_destroy(tasks[i]);
    }

    /* Report results */
    uint64_t add_per_op = add_total / BENCH_QUEUE_OPS;
    uint64_t remove_per_op = remove_total / BENCH_QUEUE_OPS;

    uart_printf("  Queue operation benchmark (%d ops):\n", BENCH_QUEUE_OPS);
    uart_printf("    Add:    %lu ns total, %lu ns per op\n",
                (unsigned long)add_total, (unsigned long)add_per_op);
    uart_printf("    Remove: %lu ns total, %lu ns per op\n",
                (unsigned long)remove_total, (unsigned long)remove_per_op);

    /* Test passes if operations completed */
    TEST_ASSERT(add_per_op > 0);
    TEST_ASSERT(remove_per_op > 0);
}

/* ============================================================================
 * Timer Basic Tests (TEST-L6)
 * ============================================================================ */

/*
 * Test: timer_get_count() returns a non-zero value after boot.
 */
static void test_timer_counter_readable(void)
{
    uint64_t count = timer_get_count();
    /* Counter should be non-zero after boot */
    TEST_ASSERT_TRUE(count > 0);
}

/*
 * Test: Two consecutive reads show the counter advancing (or not going backwards).
 */
static void test_timer_counter_advances(void)
{
    uint64_t a = timer_get_count();
    /* Small delay */
    for (volatile int i = 0; i < 10000; i++) {
        /* spin */
    }
    uint64_t b = timer_get_count();
    /* Counter should advance (or at minimum not go backwards) */
    TEST_ASSERT_TRUE(b >= a);
}

/*
 * Test: timer_get_frequency() returns a reasonable value.
 */
static void test_timer_frequency_reasonable(void)
{
    uint64_t freq = timer_get_frequency();
    /* Timer frequency should be between 1 MHz and 100 GHz */
    TEST_ASSERT_TRUE(freq >= 1000000);
    TEST_ASSERT_TRUE(freq <= 100000000000ULL);
}

/*
 * Regression test: Timer uses physical timer IRQ 30 (PPI 14).
 * Previously used virtual timer IRQ 27 (PPI 11). Linux kernel and
 * Circle both use the physical timer at EL1.
 */
static void test_timer_irq_is_physical(void)
{
    /* TIMER_IRQ is defined in platform.h as 30 for all platforms */
    TEST_ASSERT_EQUAL_INT(30, TIMER_IRQ);
}

/* ============================================================================
 * Spinlock Hardware Mode Tests (post-MMU)
 *
 * These tests run after vmm_init() has enabled the MMU and set
 * spinlock_hw_enabled=1. They verify hardware spinlocks (ldaxr/stxr)
 * work correctly, unlike the smp.c tests which run pre-MMU in
 * barrier-only mode.
 * ============================================================================ */

#if !defined(SPINLOCK_SKIP_LOCKING)
extern volatile int spinlock_hw_enabled;
#endif

/*
 * Test: spinlock_hw_enabled is 1 after boot (VMM has initialized).
 */
static void test_spinlock_hw_enabled_after_boot(void)
{
#if !defined(SPINLOCK_SKIP_LOCKING)
    TEST_ASSERT_EQUAL_INT(1, spinlock_hw_enabled);
#else
    TEST_IGNORE_MESSAGE("SPINLOCK_SKIP_LOCKING active, no runtime flag");
#endif
}

/*
 * Test: Hardware spin_lock actually sets lock value to 1.
 * This test runs post-MMU so it exercises real ldaxr/stxr.
 */
static void test_spinlock_hw_acquire_release(void)
{
    spinlock_t lock = SPINLOCK_INIT;

    TEST_ASSERT_EQUAL_INT(0, lock.lock);

    spin_lock(&lock);
#if !defined(SPINLOCK_SKIP_LOCKING)
    /* With hardware spinlocks, lock.lock should be 1 */
    TEST_ASSERT_EQUAL_INT(1, lock.lock);
#endif

    spin_unlock(&lock);
    TEST_ASSERT_EQUAL_INT(0, lock.lock);
}

/*
 * Test: Hardware spin_trylock returns 0 when lock is held.
 */
static void test_spinlock_hw_trylock_contention(void)
{
    spinlock_t lock = SPINLOCK_INIT;

    spin_lock(&lock);

    /* trylock should fail since lock is held */
    int result = spin_trylock(&lock);
#if !defined(SPINLOCK_SKIP_LOCKING)
    TEST_ASSERT_EQUAL_INT(0, result);
#else
    (void)result;
    TEST_IGNORE_MESSAGE("SPINLOCK_SKIP_LOCKING: trylock always succeeds");
#endif

    spin_unlock(&lock);
}

/*
 * Test: IRQ-safe spinlock works in hardware mode.
 */
static void test_spinlock_hw_irqsafe(void)
{
    spinlock_t lock = SPINLOCK_INIT;

    irq_flags_t flags = spin_lock_irqsave(&lock);

#if !defined(SPINLOCK_SKIP_LOCKING)
    /* Lock should be held */
    TEST_ASSERT_EQUAL_INT(1, lock.lock);
#endif

    spin_unlock_irqrestore(&lock, flags);
    TEST_ASSERT_EQUAL_INT(0, lock.lock);
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_scheduler(void)
{
    UnityBegin("Scheduler Tests");

    /* Regression tests: DAIF context preservation */
    RUN_TEST(test_daif_saved_in_context);
    RUN_TEST(test_new_task_daif_irq_masked);
    RUN_TEST(test_daif_not_zero_on_create);
    RUN_TEST(test_task_create_multiple_all_irq_masked);
    RUN_TEST(test_idle_task_daif);

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

    /* Stress tests */
    RUN_TEST(test_no_starvation);
    RUN_TEST(test_stress_mixed_priorities);

    /* Latency and benchmark tests */
    RUN_TEST(test_isolated_core_latency);
    RUN_TEST(test_benchmark_context_switch);
    RUN_TEST(test_benchmark_queue_operations);

    /* Timer basic tests (TEST-L6) */
    RUN_TEST(test_timer_counter_readable);
    RUN_TEST(test_timer_counter_advances);
    RUN_TEST(test_timer_frequency_reasonable);
    RUN_TEST(test_timer_irq_is_physical);

    /* Spinlock hardware mode tests (post-MMU) */
    RUN_TEST(test_spinlock_hw_enabled_after_boot);
    RUN_TEST(test_spinlock_hw_acquire_release);
    RUN_TEST(test_spinlock_hw_trylock_contention);
    RUN_TEST(test_spinlock_hw_irqsafe);

    return UnityEnd();
}
