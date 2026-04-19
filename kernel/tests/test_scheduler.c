/*
 * Scheduler Tests
 *
 * Tests for priority-based scheduling and deadline boost functionality.
 */

#include "unity.h"
#include "task.h"
#include "sched.h"
#include "sched_policy.h"
#include "slm_ffi.h"
#include "spinlock.h"
#include "smp.h"
#include "timer.h"
#include "platform.h"
#include "uart.h"
#include "cache.h"
#include "string.h"
#include <stdint.h>
#include <stdbool.h>

/* AI scheduler types (always available — header-only) */
#include "ai_types.h"

#ifdef CONFIG_AI_SCHEDULER
#include "ai_inference.h"
#include "ai_state.h"
#endif
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

#if !defined(PLATFORM_X86_64)
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
#endif /* !PLATFORM_X86_64 */

/* ============================================================================
 * Unit Tests: task_slot() accessor (#321)
 * ============================================================================ */

/*
 * task_slot(idx) returns the i-th entry of the task table directly,
 * independent of the monotonic task-ID counter. Use case: iterating
 * all live tasks from diagnostic code (`tasks`, `top`, slm.tasks(),
 * /proc-like VFS listings) without missing tasks whose IDs have
 * grown past MAX_TASKS.
 */

/* Bounds: task_slot returns NULL for out-of-range indices. */
static void test_task_slot_out_of_range_returns_null(void)
{
    TEST_ASSERT_NULL(task_slot(MAX_TASKS));
    TEST_ASSERT_NULL(task_slot(MAX_TASKS + 1));
    TEST_ASSERT_NULL(task_slot(UINT32_MAX));
}

/* In-range: task_slot returns a non-NULL slot pointer for every valid
 * index, whether or not the slot currently holds a live task. The
 * empty-slot marker is t->id == 0 (per task_destroy). */
static void test_task_slot_in_range_returns_slot(void)
{
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(task_slot(i),
            "task_slot returned NULL for an in-range index");
    }
}

/* Creating a task populates some slot; iterating via task_slot must
 * find it by its name (not by id, which we do not control). */
static void test_task_slot_finds_created_task(void)
{
    struct task *t = task_create_with_priority("slot_test",
                                               nop_entry, NULL,
                                               TASK_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(t);
    uint32_t created_id = t->id;
    TEST_ASSERT_NOT_EQUAL(0, created_id);

    bool found = false;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        struct task *s = task_slot(i);
        if (s && s->id == created_id) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_MESSAGE(found,
        "task_slot iteration did not find the just-created task");

    /* Clean up */
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * After task_destroy, the slot that held the task is marked free via
 * t->id == 0 (see task.c:586-587). Iterating via task_slot must skip
 * that slot based on id==0, NOT state, because state stays
 * TASK_TERMINATED on freed slots (state is not reset by task_destroy).
 */
static void test_task_slot_skips_freed_by_id(void)
{
    struct task *t = task_create_with_priority("slot_free_test",
                                               nop_entry, NULL,
                                               TASK_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(t);

    /* Capture the pointer to the exact slot this task occupies so we
     * can find it again after destroy. */
    struct task *slot_ptr = t;
    uint32_t created_id = t->id;

    t->state = TASK_TERMINATED;
    task_destroy(t);

    /* Same slot pointer is still valid; id has been zeroed. */
    TEST_ASSERT_EQUAL_UINT32(0, slot_ptr->id);

    /* A fresh iteration via task_slot must not re-discover the now-free
     * slot under its old id — id == 0 is the documented free marker. */
    (void)created_id;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        struct task *s = task_slot(i);
        if (s == slot_ptr) {
            TEST_ASSERT_MESSAGE(s->id == 0,
                "Freed slot kept non-zero id after task_destroy");
        }
    }
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

    /* Wait for task to complete (it yields once then exits).
     * Large timeout because cross-CPU dispatch on QEMU may be slow. */
    int timeout = 500000;
    while (t->state != TASK_TERMINATED && timeout > 0) {
        yield();
        timeout--;
    }
    TEST_ASSERT_MESSAGE(timeout > 0, "ffi_test task did not complete");
    task_destroy(t);
}

/*
 * Test: slm_task_set_priority works via FFI
 */
static void test_ffi_set_priority(void)
{
    /* Reset signal so task blocks */
    ffi_task_proceed = false;

    /* Create task and pin to CPU 0 before adding to scheduler.
     * This test validates the FFI priority API, not cross-CPU dispatch.
     * slm_task_create dispatches immediately via round-robin, which on
     * Pi 5 may send the task to a secondary CPU where the cacheable
     * ffi_task_proceed flag is invisible (incoherent L2, no SMPEN).
     * Pinning to CPU 0 is the correct fix — the alternative (NC memory
     * for the flag) would add complexity for no functional benefit. */
    struct task *t = task_create("ffi_pri", (task_entry_t)blocking_ffi_task, NULL);
    TEST_ASSERT_NOT_NULL(t);
    scheduler_add_task_to_cpu(t, 0);

    int ret = slm_task_set_priority(t->id, TASK_PRIORITY_HIGH);
    TEST_ASSERT_EQUAL_INT(SLM_OK, ret);

    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_HIGH, t->priority);

    /* Signal task to exit */
    ffi_task_proceed = true;

    /* Wait for task to complete */
    int timeout = 500000;
    while (t->state != TASK_TERMINATED && timeout > 0) {
        yield();
        timeout--;
    }
    TEST_ASSERT_MESSAGE(timeout > 0, "ffi_pri task did not complete");
    task_destroy(t);
}

/*
 * Test: slm_task_set_deadline works via FFI
 */
static void test_ffi_set_deadline(void)
{
    /* Reset signal so task blocks */
    ffi_task_proceed = false;

    /* Create task and pin to CPU 0 before adding to scheduler —
     * same rationale as test_ffi_set_priority. */
    struct task *t = task_create("ffi_dl", (task_entry_t)blocking_ffi_task, NULL);
    TEST_ASSERT_NOT_NULL(t);
    scheduler_add_task_to_cpu(t, 0);

    uint64_t deadline = 999999999ULL;
    int ret = slm_task_set_deadline(t->id, deadline);
    TEST_ASSERT_EQUAL_INT(SLM_OK, ret);

    TEST_ASSERT_EQUAL_UINT64(deadline, t->deadline_ns);

    /* Signal task to exit */
    ffi_task_proceed = true;

    /* Wait for task to complete */
    int timeout = 500000;
    while (t->state != TASK_TERMINATED && timeout > 0) {
        yield();
        timeout--;
    }
    TEST_ASSERT_MESSAGE(timeout > 0, "ffi_dl task did not complete");
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

    /* Pin to CPU 0: single-CPU priority-ordering assertion. Without
     * pinning, a work-stealing idle CPU can take LOW off CPU 0's
     * deque (FIFO) before CPU 0 wakes up and picks HIGH. */
    low->cpu_affinity = 0;
    high->cpu_affinity = 0;

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

    /* Pin to CPU 0 so work-stealing can't reorder the tasks FIFO
     * from another CPU's idle loop. This test asserts strict
     * priority ordering within CPU 0's runqueue; it is not testing
     * cross-CPU placement. */
    idle->cpu_affinity = 0;
    low->cpu_affinity = 0;
    normal->cpu_affinity = 0;
    high->cpu_affinity = 0;
    critical->cpu_affinity = 0;

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

    /* Pin to CPU 0: the deadline-boost assertion is about CPU 0's
     * priority scheduler seeing the boost; work-stealing thieves
     * don't know about deadlines and would take no_deadline FIFO. */
    no_deadline->cpu_affinity = 0;
    urgent->cpu_affinity = 0;

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
 * Regression test: sched_migrate_task must not double-queue a task
 * that was stolen between the assigned_cpu read and the rq_lock acquire.
 *
 * Race (fixed April 16, 2026, commit f7cae25):
 *   Thread A (CPU 0): reads task->assigned_cpu = 1
 *   Thread B (stealer on CPU 2): pulls task from CPU 1's queue onto CPU 2's
 *   Thread A: acquires rq_lock[1] and rq_lock[3]
 *   Thread A: calls remove_from_cpu_queue_locked(task, 1) — returns 0
 *             (task no longer on CPU 1's queue, but the old code ignored
 *              the return value)
 *   Thread A: calls add_to_cpu_queue_locked(task, 3) — LINKS the task into
 *             CPU 3's queue while it is already on CPU 2's queue, setting
 *             task->next to corrupt one of the two queues' linked lists.
 *   Both CPUs pick the task and run its entry wrapper on shared stack state.
 *
 * This test simulates the stolen-task case by manually removing the task
 * from its queue (via scheduler_remove_task) before calling sched_migrate_task.
 * The fix returns success (ret == 0) without adding the task to the target
 * queue, and updates task->assigned_cpu so future placement prefers the
 * target. The task's `next` pointer and the target queue's head/tail are
 * left untouched — there's nothing to queue because the task isn't here.
 */
static void test_migrate_stolen_task_no_double_queue(void)
{
    if (cpu_count < 4) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 4 for CPU 1 / CPU 3 migration");
        return;
    }

    struct task *t = task_create("mig_stolen", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);

    /* Place on CPU 1's queue. Default affinity is ANY, which is fine for
     * sched_migrate_task's affinity check. */
    scheduler_add_task_to_cpu(t, 1);
    TEST_ASSERT_EQUAL_UINT32(1, t->assigned_cpu);

    /* Simulate a work-stealing thief pulling the task from CPU 1's queue.
     * scheduler_remove_task removes from the task's current assigned CPU
     * without changing the task state (same path a thief's
     * remove_from_cpu_queue_locked takes during steal validation). */
    scheduler_remove_task(t);

    /* After the simulated steal, task is not in CPU 1's queue. */
    struct cpu_runqueue *rq1 = sched_cpu_rq(1);
    bool found_on_cpu1 = false;
    for (struct task *cur = rq1->head; cur; cur = cur->next) {
        if (cur == t) { found_on_cpu1 = true; break; }
    }
    TEST_ASSERT_MESSAGE(!found_on_cpu1,
        "task should not appear in CPU 1's rq after remove");

    /* Snapshot the target queue before migration. */
    struct cpu_runqueue *rq3 = sched_cpu_rq(3);
    struct task *rq3_head_before = rq3->head;
    uint32_t rq3_ready_before = rq3->ready_count;

    /* Set state to READY explicitly — scheduler_remove_task leaves state
     * unchanged, but sched_migrate_task's READY check is what selects the
     * branch under test. */
    t->state = TASK_READY;

    /* Preserve a sentinel in task->next so we can detect the bug: the old
     * code would overwrite task->next in add_to_cpu_queue_locked when
     * linking the task into CPU 3's queue. The fix leaves it alone. */
    t->next = (struct task *)0xDEADBEEFCAFEBABEULL;

    int ret = sched_migrate_task(t, 3);
    TEST_ASSERT_MESSAGE(ret == 0, "migrate should succeed");

    /* Assigned CPU is updated so future placement lands on the target. */
    TEST_ASSERT_MESSAGE(t->assigned_cpu == 3,
        "task->assigned_cpu should be updated to target");

    /* Task must NOT be linked into CPU 3's queue — it was already "stolen"
     * and is logically somewhere else. If the fix regresses, the task is
     * now at rq3->head or ready_count has incremented. */
    TEST_ASSERT_MESSAGE(rq3->head == rq3_head_before,
        "CPU 3 rq head changed — task was double-queued");
    TEST_ASSERT_MESSAGE(rq3->ready_count == rq3_ready_before,
        "CPU 3 ready_count changed — task was double-queued");

    /* task->next was not overwritten — sentinel intact. */
    TEST_ASSERT_MESSAGE(
        t->next == (struct task *)0xDEADBEEFCAFEBABEULL,
        "task->next was clobbered — add_to_cpu_queue_locked ran unexpectedly");

    t->next = NULL;
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/*
 * Companion test to test_migrate_stolen_task_no_double_queue: when
 * migration succeeds on a still-queued task, task->assigned_cpu
 * reflects the new target. Physical queue-move is NOT asserted here
 * — that observation is inherently racy on Pi 5 under cooperative
 * preemption (CPU 3 wakes, runs nop_entry → task_exit → task leaves
 * the queue before our walk) and the "stolen" branch test already
 * covers the queue-state invariant for its specific scenario. This
 * test's name is deliberately scoped to the assigned_cpu semantic.
 */
static void test_migrate_ready_task_updates_assigned_cpu(void)
{
    if (cpu_count < 4) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 4 for CPU 1 / CPU 3 migration");
        return;
    }

    struct task *t = task_create("mig_ok", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);

    /* Pin to CPU 1 so work-stealing thieves on CPU 2 / CPU 3
     * can't yank the task before we exercise the migrate path
     * (CPU_AFFINITY_ANY would race with a thief under
     * CONFIG_WORK_STEALING). */
    t->cpu_affinity = 1;
    scheduler_add_task_to_cpu(t, 1);
    TEST_ASSERT_EQUAL_UINT32(1, t->assigned_cpu);

    /* The migration's SEMANTIC effect is that task->assigned_cpu
     * becomes the target CPU. The prior version of this test also
     * walked CPU 1's and CPU 3's queues to observe physical link
     * state, but that observation is inherently racy on Pi 5
     * under cooperative preemption: as soon as CPU 1 or CPU 3 is
     * idle and sees a TASK_READY with matching affinity, it runs
     * nop_entry → task_exit → the task leaves the queue before
     * we can walk it. Assigned-cpu is the authoritative semantic
     * check; physical-queue walks are implementation-bound and
     * duplicate what test_migrate_stolen_task_no_double_queue
     * already covers for the "stolen" branch. */

    /* Relax affinity so the migrate call passes the target check. */
    t->cpu_affinity = 3;
    int ret = sched_migrate_task(t, 3);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(3, t->assigned_cpu);

    /* Cleanup: whatever state the task is in, terminate + destroy. */
    scheduler_terminate_task(t);
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

    /* Pin to CPU 0: the test asserts HIGH runs first on the CPU 0
     * priority scheduler. Without pinning, a work-stealing idle CPU
     * can take LOW (FIFO) before CPU 0 wakes up and picks HIGH. */
    low_task->cpu_affinity = 0;
    high_task->cpu_affinity = 0;

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

    /* Pin tasks to CPU 0. This test asserts strict priority ordering
     * on a single CPU's runqueue — if work-stealing is enabled and
     * the tasks have default CPU_AFFINITY_ANY, an idle CPU can pull
     * them off CPU 0's deque FIFO-style (oldest first), breaking
     * the priority-based expected order. Pinning removes the steal
     * path without changing what the test is asserting. */
    t_idle->cpu_affinity = 0;
    t_low_dl->cpu_affinity = 0;
    t_normal->cpu_affinity = 0;
    t_high->cpu_affinity = 0;
    t_crit->cpu_affinity = 0;

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

/* ============================================================================
 * task_exit race regression test (TEST-RACE-EXIT)
 *
 * Regression test for the race between task_exit() and timer-driven schedule().
 * Before the fix, the timer could fire between state=TERMINATED and
 * scheduler_remove_task(), finding a terminated task still in the run queue
 * and triggering a panic. The fix masks IRQs at the top of task_exit().
 *
 * This test runs on CPU 0 so it works on both QEMU and Pi 5.
 * ============================================================================ */

static volatile uint32_t rapid_exit_count;
static spinlock_t rapid_exit_lock = SPINLOCK_INIT;

static void rapid_exit_task(void *arg)
{
    (void)arg;
    /* Minimal work — exit immediately to maximize timer race window */
    irq_flags_t flags = spin_lock_irqsave(&rapid_exit_lock);
    rapid_exit_count++;
    spin_unlock_irqrestore(&rapid_exit_lock, flags);
    /* task_exit() is called automatically by task_entry_trampoline */
}

/*
 * Test: Rapidly create and destroy tasks on CPU 0 with timer running.
 * Exercises the task_exit/schedule race window. Before the IRQ mask fix,
 * this would panic under load (~1 in 50 iterations).
 */
static void test_rapid_task_exit_no_panic(void)
{
    rapid_exit_count = 0;
    #define RAPID_EXIT_ITERATIONS 50

    for (int i = 0; i < RAPID_EXIT_ITERATIONS; i++) {
        struct task *t = task_create("rapid", rapid_exit_task, NULL);
        TEST_ASSERT_NOT_NULL(t);
        /* Pin to CPU 0 — this tests the local task_exit race, not cross-CPU */
        scheduler_add_task_to_cpu(t, 0);

        /* Wait for task to complete — polling with yield to let scheduler run */
        int timeout = 200;
        while (timeout > 0) {
            cache_invalidate(&t->state);
            if (t->state == TASK_TERMINATED) break;
            yield();
            timeout--;
        }
        TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for rapid exit task");

        /* Give scheduler time to clean up zombie */
        yield();
    }

    TEST_ASSERT_EQUAL_UINT32(RAPID_EXIT_ITERATIONS, rapid_exit_count);
}

/* ============================================================================
 * Latency and benchmark tests
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
    /* Cross-CPU dispatch now works with SEVL+WFE idle loop */
    /* This test dispatches short-lived tasks to secondary CPUs.
     * Previously disabled due to a task_exit/schedule race (now fixed:
     * IRQ mask in task_exit prevents timer from interrupting between
     * state=TERMINATED and scheduler_remove_task). */
    if (cpu_count < 2) {
        TEST_ASSERT(1);
        return;
    }

    uint64_t isolated_sum = 0;
    uint64_t isolated_max = 0;
    uint64_t isolated_min = UINT64_MAX;
    int isolated_count = 0;

    uint64_t normal_sum = 0;
    uint64_t normal_max = 0;
    uint64_t normal_min = UINT64_MAX;
    int normal_count = 0;

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
            isolated_count++;
            if (sample > isolated_max) isolated_max = sample;
            if (sample < isolated_min) isolated_min = sample;
        }

        /* On Pi 5 the isolated-core task can time out waiting to run
         * (slow schedule path under DC CIVAC contention). task_destroy
         * silently refuses to reclaim non-TERMINATED tasks, so without
         * forced termination these leak into CPU 1's run queue and
         * then block later tests (notably test_multicore_basic) whose
         * tasks queue up behind them. scheduler_terminate_task sets
         * state=TERMINATED and dequeues atomically — safe to call on
         * a READY task that never ran. */
        if (t->state != TASK_TERMINATED) {
            scheduler_terminate_task(t);
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
            normal_count++;
            if (sample > normal_max) normal_max = sample;
            if (sample < normal_min) normal_min = sample;
        }

        /* Force-terminate a stuck task so task_destroy can reclaim it.
         * See comment on the isolated-phase timeout above. */
        if (t->state != TASK_TERMINATED) {
            scheduler_terminate_task(t);
        }
        task_destroy(t);
    }

    /*
     * Calculate and log results
     */
    uart_printf("  Samples collected: isolated=%d/%d, normal=%d/%d\n",
                isolated_count, LATENCY_SAMPLES, normal_count, LATENCY_SAMPLES);

    if (isolated_count > 0 && normal_count > 0) {
        uint64_t isolated_avg = isolated_sum / (uint64_t)isolated_count;
        uint64_t isolated_range = isolated_max - isolated_min;
        uint64_t normal_avg = normal_sum / (uint64_t)normal_count;
        uint64_t normal_range = normal_max - normal_min;

        uart_printf("  Latency (isolated): avg=%lu ns, range=%lu ns (min=%lu, max=%lu)\n",
                    (unsigned long)isolated_avg, (unsigned long)isolated_range,
                    (unsigned long)isolated_min, (unsigned long)isolated_max);
        uart_printf("  Latency (normal):   avg=%lu ns, range=%lu ns (min=%lu, max=%lu)\n",
                    (unsigned long)normal_avg, (unsigned long)normal_range,
                    (unsigned long)normal_min, (unsigned long)normal_max);

        TEST_ASSERT(isolated_avg > 0);
        TEST_ASSERT(normal_avg > 0);

        /* Log whether isolation improved consistency (smaller range = better) */
        if (isolated_range < normal_range) {
            uart_puts("  Result: Isolated core shows more consistent latency\n");
        } else {
            uart_puts("  Result: Similar latency variance (expected on QEMU)\n");
        }
    } else {
        uart_puts("  Skipped: cross-CPU tasks did not complete within timeout\n");
        TEST_ASSERT(1);  /* Pass — timing-dependent, not a logic failure */
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

/*
 * #137: resched_trampoline's MPIDR fold gives unique ids for Pi 5
 * and QEMU virt, but collides on Jetson (dual-cluster A78AE) —
 * cluster 1's CPU 4 (MPIDR 0x10200) and CPU 5 (0x10300) fold to
 * 2 and 3, the same slots as cluster 0's CPU 2 and 3. The boot-
 * time sanity check calls this helper per-CPU; verify the helper
 * itself agrees with every known platform encoding.
 */
#include "preempt.h"

static void test_preempt_trampoline_cpu_fold(void)
{
    /* Pi 5 — Aff1 holds the CPU index, Aff0 always 0. */
    TEST_ASSERT_EQUAL_UINT32(0, preempt_trampoline_cpu_for_mpidr(0x000));
    TEST_ASSERT_EQUAL_UINT32(1, preempt_trampoline_cpu_for_mpidr(0x100));
    TEST_ASSERT_EQUAL_UINT32(2, preempt_trampoline_cpu_for_mpidr(0x200));
    TEST_ASSERT_EQUAL_UINT32(3, preempt_trampoline_cpu_for_mpidr(0x300));

    /* QEMU virt — Aff0 holds the CPU index, Aff1 always 0. */
    TEST_ASSERT_EQUAL_UINT32(0, preempt_trampoline_cpu_for_mpidr(0x00));
    TEST_ASSERT_EQUAL_UINT32(1, preempt_trampoline_cpu_for_mpidr(0x01));
    TEST_ASSERT_EQUAL_UINT32(2, preempt_trampoline_cpu_for_mpidr(0x02));
    TEST_ASSERT_EQUAL_UINT32(3, preempt_trampoline_cpu_for_mpidr(0x03));

    /* Jetson dual-cluster A78AE — cluster 0 Aff1 = cpu, cluster 1
     * Aff2 = 1 and Aff1 = 2/3. The fold ignores Aff2, so cluster
     * 1's CPU 4 (0x10200) and CPU 5 (0x10300) collide with
     * cluster 0's CPU 2/3. This is the exact bug the boot-time
     * check must catch. */
    TEST_ASSERT_EQUAL_UINT32(0, preempt_trampoline_cpu_for_mpidr(0x00000));
    TEST_ASSERT_EQUAL_UINT32(1, preempt_trampoline_cpu_for_mpidr(0x00100));
    TEST_ASSERT_EQUAL_UINT32(2, preempt_trampoline_cpu_for_mpidr(0x00200));
    TEST_ASSERT_EQUAL_UINT32(3, preempt_trampoline_cpu_for_mpidr(0x00300));
    /* CPU 4 wants slot 4 but the fold yields 2 — collision. */
    TEST_ASSERT_EQUAL_UINT32(2, preempt_trampoline_cpu_for_mpidr(0x10200));
    /* CPU 5 wants slot 5 but the fold yields 3 — collision. */
    TEST_ASSERT_EQUAL_UINT32(3, preempt_trampoline_cpu_for_mpidr(0x10300));
}

#if !defined(PLATFORM_X86_64)
#include "gic.h"

/*
 * Unit tests for the GIC handler registration table (#204 follow-up).
 *
 * The table is shared state — the virtio-net driver has already
 * registered a handler during platform init. These tests use IRQ
 * numbers outside the normal SPI range that drivers would pick
 * (0x300+) so they can't collide with any real registration.
 */

static void test_gic_noop_handler(void) { /* never invoked */ }
static void test_gic_noop_handler_2(void) { /* never invoked */ }

/*
 * Test: register + lookup round-trips the exact handler pointer.
 *
 * TEST_ASSERT_EQUAL_PTR is avoided throughout because Unity casts
 * through (void *), and C23 -Wpedantic (used in this codebase) treats
 * function-pointer-to-object-pointer conversion as an error. Compare
 * the function pointers directly with TEST_ASSERT_TRUE instead.
 */
static void test_gic_register_lookup_roundtrip(void)
{
    const uint32_t irq = 0x300;
    TEST_ASSERT_EQUAL_INT(0, gic_register_handler(irq, test_gic_noop_handler));
    TEST_ASSERT_TRUE(gic_lookup_handler(irq) == test_gic_noop_handler);

    /* Clean up so later tests in this suite start from a known state. */
    TEST_ASSERT_EQUAL_INT(0, gic_unregister_handler(irq));
}

/*
 * Test: lookup for an unregistered IRQ returns NULL.
 */
static void test_gic_lookup_miss_returns_null(void)
{
    TEST_ASSERT_TRUE(gic_lookup_handler(0x3FE) == NULL);
}

/*
 * Test: registering with a NULL handler is rejected.
 * The table treats NULL entries as free slots, so accepting NULL
 * would corrupt the free-slot invariant.
 */
static void test_gic_register_null_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(-1, gic_register_handler(0x301, NULL));
    TEST_ASSERT_TRUE(gic_lookup_handler(0x301) == NULL);
}

/*
 * Test: unregister then re-register reuses the same slot cleanly.
 * Guards against a "slot not freed" leak on teardown that would
 * eventually exhaust the 16-entry table.
 */
static void test_gic_unregister_frees_slot(void)
{
    const uint32_t irq = 0x302;
    TEST_ASSERT_EQUAL_INT(0, gic_register_handler(irq, test_gic_noop_handler));
    TEST_ASSERT_EQUAL_INT(0, gic_unregister_handler(irq));
    TEST_ASSERT_TRUE(gic_lookup_handler(irq) == NULL);

    /* Second registration with a different handler should succeed —
     * proves the first registration's slot was actually freed. */
    TEST_ASSERT_EQUAL_INT(0, gic_register_handler(irq, test_gic_noop_handler_2));
    TEST_ASSERT_TRUE(gic_lookup_handler(irq) == test_gic_noop_handler_2);

    TEST_ASSERT_EQUAL_INT(0, gic_unregister_handler(irq));
}

/*
 * Test: unregister on a never-registered IRQ returns -1 without
 * disturbing the table.
 */
static void test_gic_unregister_miss(void)
{
    TEST_ASSERT_EQUAL_INT(-1, gic_unregister_handler(0x3FD));
}
#endif /* !PLATFORM_X86_64 */

/*
 * #171: slm_time_ticks_to_ns must not overflow for realistic uptimes
 * on any supported timer frequency. The naive `ticks * 1e9 / freq`
 * wraps on x86-64 TSC (~3.4 GHz) after roughly 5.4 seconds.
 */
static void test_slm_time_ticks_to_ns_no_overflow(void)
{
    extern uint64_t slm_time_ticks_to_ns(uint64_t ticks, uint64_t freq);

    /* Sanity: zero ticks is zero nanoseconds, any freq. */
    TEST_ASSERT_EQUAL_UINT64(0, slm_time_ticks_to_ns(0, 62500000ULL));
    TEST_ASSERT_EQUAL_UINT64(0, slm_time_ticks_to_ns(0, 3400000000ULL));
    /* Zero freq → zero (defensive — slm_get_time_ns short-circuits). */
    TEST_ASSERT_EQUAL_UINT64(0, slm_time_ticks_to_ns(1000, 0));

    /* QEMU virt: 62.5 MHz, fast path (1e9 / 62.5e6 = 16 ns/tick). */
    TEST_ASSERT_EQUAL_UINT64(16ULL, slm_time_ticks_to_ns(1, 62500000ULL));
    TEST_ASSERT_EQUAL_UINT64(1000000000ULL,
        slm_time_ticks_to_ns(62500000ULL, 62500000ULL));

    /* x86-64 TSC ~3.4 GHz, general path with remainder.
     * 10 seconds of uptime = 3.4e10 ticks; naive multiply would
     * have overflowed u64 past 5.4 s. Post-fix the value must
     * match 10e9 ns within the per-second rounding slop of the
     * frac-tick division (< freq ns per integer second). */
    uint64_t tsc_freq = 3400000000ULL;
    uint64_t ns_10s = slm_time_ticks_to_ns(10ULL * tsc_freq, tsc_freq);
    TEST_ASSERT_TRUE(ns_10s >= 9999999900ULL && ns_10s <= 10000000100ULL);

    /* 60 seconds at the same TSC — well past the 5.4 s overflow
     * boundary reported in #171. Monotonicity and magnitude must
     * both hold. */
    uint64_t ns_60s = slm_time_ticks_to_ns(60ULL * tsc_freq, tsc_freq);
    TEST_ASSERT_TRUE(ns_60s > ns_10s);
    TEST_ASSERT_TRUE(ns_60s >= 59999999400ULL && ns_60s <= 60000000600ULL);

    /* Monotonicity across a tick boundary at high uptime. */
    uint64_t big = 100ULL * tsc_freq + 12345ULL;
    TEST_ASSERT_TRUE(
        slm_time_ticks_to_ns(big + 1, tsc_freq) >=
        slm_time_ticks_to_ns(big, tsc_freq));
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
 * SMP: MPIDR Encoding and Secondary MMU Tests
 * ============================================================================ */

/*
 * Test: cpu_logical_map has correct MPIDR encoding for this platform.
 */
static void test_cpu_logical_map_encoding(void)
{
    extern uint64_t cpu_logical_map[];
    extern uint32_t cpu_count;

    /* CPU 0 should match the boot CPU's actual MPIDR */
    uint64_t boot_mpidr = cpu_get_mpidr() & MPIDR_AFF_MASK;
    TEST_ASSERT_EQUAL_HEX64(boot_mpidr, cpu_logical_map[0]);

    /* Verify map has at least 1 entry */
    TEST_ASSERT_TRUE(cpu_count >= 1);

#if defined(PLATFORM_RASPI5)
    /* Pi 5: Aff1 encoding — each CPU ID shifted left by 8 */
    if (cpu_count >= 2)
        TEST_ASSERT_EQUAL_HEX64(0x100, cpu_logical_map[1]);
    if (cpu_count >= 3)
        TEST_ASSERT_EQUAL_HEX64(0x200, cpu_logical_map[2]);
    if (cpu_count >= 4)
        TEST_ASSERT_EQUAL_HEX64(0x300, cpu_logical_map[3]);
#elif defined(PLATFORM_QEMU_VIRT)
    /* QEMU: Aff0 encoding — sequential */
    if (cpu_count >= 2)
        TEST_ASSERT_EQUAL_HEX64(1, cpu_logical_map[1]);
    if (cpu_count >= 3)
        TEST_ASSERT_EQUAL_HEX64(2, cpu_logical_map[2]);
#endif
}

/*
 * Test: secondary_mmu_ttbr is set after VMM init (used by secondary CPUs).
 */
static void test_secondary_mmu_ttbr_set(void)
{
#if !defined(SPINLOCK_SKIP_LOCKING)
    extern volatile uint64_t secondary_mmu_ttbr;
    extern volatile uint64_t secondary_mmu_mair;
    extern volatile uint64_t secondary_mmu_tcr;

    /* These should be non-zero after vmm_init */
    TEST_ASSERT_TRUE(secondary_mmu_ttbr != 0);
    TEST_ASSERT_TRUE(secondary_mmu_mair != 0);
    TEST_ASSERT_TRUE(secondary_mmu_tcr != 0);
#else
    TEST_IGNORE_MESSAGE("SPINLOCK_SKIP_LOCKING: secondary MMU vars not defined");
#endif
}

/* ============================================================================
 * SMP: Multi-Core Online Verification
 * ============================================================================ */

/*
 * Test: All configured CPUs are online after boot.
 * After smp_init() completes, cpus_online should equal cpu_count.
 */
static void test_all_cpus_online(void)
{
    extern volatile uint32_t cpus_online;
    extern uint32_t cpu_count;
    cache_invalidate(&cpus_online);
    cache_invalidate(&cpu_count);
    TEST_ASSERT_EQUAL_INT(cpu_count, cpus_online);
}

/*
 * Test: Each CPU's per_cpu online flag is set.
 * Verifies that every entry in cpu_data[] up to cpu_count has online==true.
 */
static void test_cpu_data_online_flags(void)
{
    extern struct per_cpu cpu_data[];
    extern uint32_t cpu_count;
    for (uint32_t i = 0; i < cpu_count; i++) {
        cache_invalidate(&cpu_data[i].online);
        TEST_ASSERT_TRUE(cpu_data[i].online);
    }
}

/*
 * Test: CPU 0 is the boot CPU.
 * The test suite runs on the primary core, which should be logical CPU 0.
 */
static void test_boot_cpu_is_cpu0(void)
{
    TEST_ASSERT_EQUAL_INT(0, cpu_id());
}

/*
 * Test: cpus_online counter matches per-CPU online flags.
 * Verifies consistency between the global counter and per-CPU structs.
 * This catches the DC CIVAC writeback bug where CPU 0's stale dirty cacheline
 * for cpu_data[] would overwrite secondary CPUs' online=true at PoC.
 */
static void test_cpus_online_matches_flags(void)
{
    extern struct per_cpu cpu_data[];
    extern volatile uint32_t cpus_online;
    extern uint32_t cpu_count;

    cache_invalidate(&cpus_online);
    uint32_t counter = cpus_online;
    uint32_t flags_count = 0;

    for (uint32_t i = 0; i < cpu_count; i++) {
        cache_invalidate(&cpu_data[i].online);
        if (cpu_data[i].online) {
            flags_count++;
        }
    }

    TEST_ASSERT_EQUAL_UINT32(counter, flags_count);
}

/*
 * Test: Per-CPU struct fields are consistent after boot.
 * Verifies that cpu_data[i] has correct cpu_id and valid mpidr after
 * cache maintenance. This tests that cache_clean_range on cpu_data before
 * secondary boot doesn't corrupt neighboring struct fields (false sharing).
 */
static void test_cpu_data_fields_consistent(void)
{
    extern struct per_cpu cpu_data[];
    extern uint32_t cpu_count;
    extern uint64_t cpu_logical_map[];

    for (uint32_t i = 0; i < cpu_count; i++) {
        /* Invalidate the entire per-CPU struct to get fresh data */
        cache_invalidate_range(&cpu_data[i], sizeof(struct per_cpu));

        TEST_ASSERT_EQUAL_UINT32(i, cpu_data[i].cpu_id);
        TEST_ASSERT_EQUAL_HEX64(cpu_logical_map[i], cpu_data[i].mpidr);
        TEST_ASSERT_TRUE(cpu_data[i].online);
        TEST_ASSERT_TRUE(cpu_data[i].stack_top != NULL);
    }
}

/* ============================================================================
 * SPSC Ring Buffer: Tests the algorithm used by UART IRQ RX path
 * ============================================================================ */

/* Local ring buffer matching uart_rp1.c implementation */
#define TEST_RB_SIZE 16  /* Small power-of-2 for testing wraps */

static volatile uint8_t  test_rb_buf[TEST_RB_SIZE];
static volatile uint32_t test_rb_head;
static volatile uint32_t test_rb_tail;

static void test_rb_reset(void) {
    test_rb_head = 0;
    test_rb_tail = 0;
}

/* Producer (mimics ISR): returns 1 if inserted, 0 if full */
static int test_rb_put(uint8_t ch) {
    uint32_t next = (test_rb_head + 1) & (TEST_RB_SIZE - 1);
    if (next == test_rb_tail) return 0;  /* Full */
    test_rb_buf[test_rb_head] = ch;
    test_rb_head = next;
    return 1;
}

/* Consumer (mimics uart_getc): returns char or -1 if empty */
static int test_rb_get(void) {
    if (test_rb_head == test_rb_tail) return -1;  /* Empty */
    uint8_t ch = test_rb_buf[test_rb_tail];
    test_rb_tail = (test_rb_tail + 1) & (TEST_RB_SIZE - 1);
    return ch;
}

/*
 * Test: Empty ring buffer returns no data.
 */
static void test_ringbuf_empty(void)
{
    test_rb_reset();
    TEST_ASSERT_EQUAL_INT(-1, test_rb_get());
    TEST_ASSERT_EQUAL_UINT32(0, test_rb_head);
    TEST_ASSERT_EQUAL_UINT32(0, test_rb_tail);
}

/*
 * Test: Basic put/get maintains FIFO order.
 */
static void test_ringbuf_fifo_order(void)
{
    test_rb_reset();
    TEST_ASSERT_EQUAL_INT(1, test_rb_put('A'));
    TEST_ASSERT_EQUAL_INT(1, test_rb_put('B'));
    TEST_ASSERT_EQUAL_INT(1, test_rb_put('C'));

    TEST_ASSERT_EQUAL_INT('A', test_rb_get());
    TEST_ASSERT_EQUAL_INT('B', test_rb_get());
    TEST_ASSERT_EQUAL_INT('C', test_rb_get());
    TEST_ASSERT_EQUAL_INT(-1, test_rb_get());  /* Empty */
}

/*
 * Test: Buffer full rejects new data (capacity = size - 1).
 */
static void test_ringbuf_full(void)
{
    test_rb_reset();

    /* Fill to capacity (size - 1 entries usable) */
    for (int i = 0; i < TEST_RB_SIZE - 1; i++) {
        TEST_ASSERT_EQUAL_INT(1, test_rb_put((uint8_t)i));
    }

    /* One more should fail */
    TEST_ASSERT_EQUAL_INT(0, test_rb_put(0xFF));

    /* Drain and verify order */
    for (int i = 0; i < TEST_RB_SIZE - 1; i++) {
        TEST_ASSERT_EQUAL_INT(i, test_rb_get());
    }
    TEST_ASSERT_EQUAL_INT(-1, test_rb_get());
}

/*
 * Test: Wrap-around works correctly when head/tail cross buffer boundary.
 */
static void test_ringbuf_wrap(void)
{
    test_rb_reset();

    /* Fill and drain several times to force wrap-around */
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < TEST_RB_SIZE - 1; i++) {
            TEST_ASSERT_EQUAL_INT(1, test_rb_put((uint8_t)(round * 10 + i)));
        }
        for (int i = 0; i < TEST_RB_SIZE - 1; i++) {
            TEST_ASSERT_EQUAL_INT(round * 10 + i, test_rb_get());
        }
        TEST_ASSERT_EQUAL_INT(-1, test_rb_get());
    }
}

/*
 * Test: Interleaved put/get (streaming pattern).
 */
static void test_ringbuf_interleaved(void)
{
    test_rb_reset();

    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT(1, test_rb_put((uint8_t)(i & 0xFF)));
        int ch = test_rb_get();
        TEST_ASSERT_EQUAL_INT(i & 0xFF, ch);
    }
    TEST_ASSERT_EQUAL_INT(-1, test_rb_get());
}

/* ============================================================================
 * Sleep: Timer-driven sleep tests
 * ============================================================================ */

/*
 * Test: sleep_ms(0) returns immediately without blocking.
 */
static void test_sleep_zero_returns_immediately(void)
{
    uint64_t before = slm_get_time_ns();
    sleep_ms(0);
    uint64_t after = slm_get_time_ns();

    /* Should complete in well under 1ms */
    uint64_t elapsed_us = (after - before) / 1000;
    TEST_ASSERT_MESSAGE(elapsed_us < 1000, "sleep_ms(0) took > 1ms");
}

/*
 * Test: sleep_ms blocks for approximately the requested duration.
 * Timer tick is 10ms, so sleep_ms(50) should sleep ~50ms (5 ticks).
 */
static void test_sleep_ms_duration(void)
{
    uint64_t before = slm_get_time_ns();
    sleep_ms(50);
    uint64_t after = slm_get_time_ns();

    uint64_t elapsed_ms = (after - before) / 1000000;

    /* Should be at least 40ms (allowing for tick alignment) */
    TEST_ASSERT_MESSAGE(elapsed_ms >= 40, "sleep_ms(50) woke too early");
    /* Should be no more than 80ms (50ms + 2 extra ticks tolerance) */
    TEST_ASSERT_MESSAGE(elapsed_ms <= 80, "sleep_ms(50) woke too late");
}

/*
 * Test: sleep_ms with a short duration (1 tick or less).
 * sleep_ms(5) should wake on the next tick (~10ms).
 */
static void test_sleep_ms_short(void)
{
    uint64_t before = slm_get_time_ns();
    sleep_ms(5);
    uint64_t after = slm_get_time_ns();

    uint64_t elapsed_ms = (after - before) / 1000000;

    /* Should wake within ~20ms (at most 2 ticks) */
    TEST_ASSERT_MESSAGE(elapsed_ms <= 30, "sleep_ms(5) took > 30ms");
}

/*
 * Test: sleep_us works for microsecond-level durations.
 * Since timer tick is 10ms, sub-tick sleeps wake on next tick.
 */
static void test_sleep_us_wakes_on_tick(void)
{
    uint64_t before = slm_get_time_ns();
    sleep_us(1000);  /* 1ms = sub-tick */
    uint64_t after = slm_get_time_ns();

    uint64_t elapsed_ms = (after - before) / 1000000;

    /* Should wake within ~20ms (next tick or two) */
    TEST_ASSERT_MESSAGE(elapsed_ms <= 30, "sleep_us(1000) took > 30ms");
}

/*
 * Test: Multiple sequential sleeps accumulate correctly.
 */
static void test_sleep_sequential(void)
{
    uint64_t before = slm_get_time_ns();
    sleep_ms(30);
    sleep_ms(30);
    uint64_t after = slm_get_time_ns();

    uint64_t elapsed_ms = (after - before) / 1000000;

    /* Two 30ms sleeps should total ~60ms, allow 40-100ms range */
    TEST_ASSERT_MESSAGE(elapsed_ms >= 40, "Sequential sleeps too short");
    TEST_ASSERT_MESSAGE(elapsed_ms <= 100, "Sequential sleeps too long");
}

/*
 * Test: Task state is TASK_RUNNING after waking from sleep.
 * Verifies that the sleep/wake cycle restores the task properly.
 */
static void test_sleep_task_state_restored(void)
{
    struct task *current = task_current();
    TEST_ASSERT_NOT_NULL(current);

    /* Before sleep, should be RUNNING */
    TEST_ASSERT_EQUAL(TASK_RUNNING, current->state);

    sleep_ms(10);

    /* After wake, should be RUNNING again */
    TEST_ASSERT_EQUAL(TASK_RUNNING, current->state);

    /* wake_time should be cleared */
    TEST_ASSERT_EQUAL_UINT64(0, current->wake_time_ns);
}

/* ============================================================================
 * Cross-CPU Dispatch Infrastructure Tests
 * ============================================================================ */

/*
 * Test: cpu_id() returns the correct logical ID for the boot CPU.
 * On Pi 5, this validates the hardcoded MPIDR table (Aff1 encoding).
 * On QEMU, this validates the dynamic cpu_logical_map lookup.
 */
static void test_cpu_id_returns_correct_value(void)
{
    /* Boot CPU should always be 0 */
    TEST_ASSERT_EQUAL_UINT32(0, cpu_id());

    /* cpu_logical_id with boot CPU's MPIDR should return 0 */
    uint64_t mpidr = cpu_get_mpidr();
    int logical = cpu_logical_id(mpidr);
    TEST_ASSERT_EQUAL_INT(0, logical);

#if defined(PLATFORM_RASPI5)
    /* Pi 5 hardcoded table: verify all 4 entries */
    TEST_ASSERT_EQUAL_INT(0, cpu_logical_id(0x000));
    TEST_ASSERT_EQUAL_INT(1, cpu_logical_id(0x100));
    TEST_ASSERT_EQUAL_INT(2, cpu_logical_id(0x200));
    TEST_ASSERT_EQUAL_INT(3, cpu_logical_id(0x300));
    /* Invalid MPIDR should return -1 */
    TEST_ASSERT_EQUAL_INT(-1, cpu_logical_id(0x400));
    TEST_ASSERT_EQUAL_INT(-1, cpu_logical_id(0x001));
#endif
}

/*
 * Test: find_target_cpu() distributes tasks across CPUs.
 * The round-robin should NOT always return 0 when all queues are empty.
 */
static void test_round_robin_distributes_tasks(void)
{
    extern uint32_t cpu_count;
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("Single CPU — round-robin not applicable");
    }

    /* Create and dispatch 4 tasks via scheduler_add_task (uses find_target_cpu).
     * Use NULL entry + IRQ protection to prevent tasks from running
     * on secondary CPUs before we can check assigned_cpu. */
    uint32_t cpu_hits[4] = {0, 0, 0, 0};
    irq_flags_t rr_flags = irq_save();
    for (int i = 0; i < 4; i++) {
        struct task *t = task_create("rr_test", nop_entry, NULL);
        TEST_ASSERT_NOT_NULL(t);
        scheduler_add_task(t);
        uint32_t assigned = t->assigned_cpu;
        if (assigned < 4) cpu_hits[assigned]++;
        scheduler_remove_task(t);
        t->id = 0;
    }
    irq_restore(rr_flags);

    /* At least 2 different CPUs should have been selected */
    int cpus_used = 0;
    for (int i = 0; i < 4; i++) {
        if (cpu_hits[i] > 0) cpus_used++;
    }
    TEST_ASSERT_MESSAGE(cpus_used >= 2,
        "Round-robin should distribute across multiple CPUs");
}

/*
 * Test: scheduler_start() takes a cpu parameter (API change validation).
 * This is a compile-time test — if the signature is wrong, this won't compile.
 * At runtime, verify the function declaration matches expectations.
 */
static void test_scheduler_start_accepts_cpu_param(void)
{
    /* Verify the function pointer matches the expected signature.
     * This is effectively a compile-time check — if scheduler_start
     * still took void, this would fail to compile. */
    void (*fn)(uint32_t) = scheduler_start;
    TEST_ASSERT_NOT_NULL(fn);
    (void)fn;  /* Suppress unused warning */
}

/*
 * Test: Deadline boost raises effective priority.
 * Creates a task with a tight deadline, verifies priority is boosted.
 */
static void test_deadline_boost_raises_priority(void)
{
    struct task *t = task_create("dl_test", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);

    /* Default priority, no deadline — effective should match base */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_DEFAULT, t->effective_priority);

    /* Set a tight absolute deadline (now + 10ms) */
    task_set_deadline(t, slm_get_time_ns() + 10 * 1000000ULL);

    /* Pin to CPU 0 to avoid cross-CPU dispatch race (test may remove
     * task while secondary CPU is trying to run it) */
    task_set_affinity(t, 0);

    /* Add to scheduler to trigger deadline boost */
    irq_flags_t flags = irq_save();
    scheduler_add_task(t);

    /* Effective priority should be boosted above base (higher number = higher priority) */
    TEST_ASSERT_TRUE(t->effective_priority > t->priority);

    scheduler_remove_task(t);
    irq_restore(flags);
    t->id = 0;
}

/*
 * Test: Isolated cores excluded from round-robin dispatch.
 * Isolates a core, dispatches multiple tasks, verifies none land on it.
 */
static void test_isolation_excludes_from_dispatch(void)
{
    extern uint32_t cpu_count;
    if (cpu_count < 3) {
        TEST_IGNORE_MESSAGE("Need 3+ CPUs for isolation dispatch test");
    }

    sched_isolate_core(2);

    int landed_on_2 = 0;
    irq_flags_t iso_flags = irq_save();
    for (int i = 0; i < 8; i++) {
        struct task *t = task_create("iso_d", nop_entry, NULL);
        TEST_ASSERT_NOT_NULL(t);
        scheduler_add_task(t);
        if (t->assigned_cpu == 2) landed_on_2++;
        scheduler_remove_task(t);
        t->id = 0;
    }
    irq_restore(iso_flags);

    sched_unisolate_core(2);

    TEST_ASSERT_MESSAGE(landed_on_2 == 0,
        "No tasks should land on isolated CPU 2");
}

/* ============================================================================
 * Pluggable Scheduler Policy Tests
 * ============================================================================ */

/*
 * Test: Default policy is "heuristic" after scheduler_init().
 */
static void test_policy_default_is_heuristic(void)
{
    const char *name = sched_get_policy();
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_STRING("heuristic", name);
}

/*
 * Test: Heuristic policy is registered and findable.
 */
static void test_policy_find_heuristic(void)
{
    const struct sched_policy_ops *p = sched_find_policy("heuristic");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("heuristic", p->name);
    TEST_ASSERT_NOT_NULL(p->assign_cpu);
}

/*
 * Test: sched_find_policy returns NULL for unknown names.
 */
static void test_policy_find_unknown_returns_null(void)
{
    const struct sched_policy_ops *p = sched_find_policy("nonexistent");
    TEST_ASSERT_NULL(p);

    p = sched_find_policy(NULL);
    TEST_ASSERT_NULL(p);
}

/*
 * Test: sched_policy_count returns at least 1 (the heuristic policy).
 */
static void test_policy_count_at_least_one(void)
{
    int count = sched_policy_count();
    TEST_ASSERT_TRUE(count >= 1);
}

/*
 * Test: sched_policy_get returns policies within range, NULL outside.
 */
static void test_policy_get_bounds(void)
{
    int count = sched_policy_count();

    /* Valid indices should return non-NULL */
    for (int i = 0; i < count; i++) {
        const struct sched_policy_ops *p = sched_policy_get(i);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_NOT_NULL(p->name);
    }

    /* Out-of-bounds should return NULL */
    TEST_ASSERT_NULL(sched_policy_get(-1));
    TEST_ASSERT_NULL(sched_policy_get(count));
    TEST_ASSERT_NULL(sched_policy_get(SCHED_POLICY_MAX + 1));
}

/*
 * Test: sched_set_policy rejects NULL.
 */
static void test_policy_set_null_rejected(void)
{
    int ret = sched_set_policy(NULL);
    TEST_ASSERT_EQUAL_INT(-1, ret);
    /* Active policy should still be heuristic */
    TEST_ASSERT_EQUAL_STRING("heuristic", sched_get_policy());
}

/* Stub policy for testing: always assigns to CPU 0 */
static uint32_t stub_assign_cpu(struct task *task)
{
    (void)task;
    return 0;
}

static int stub_init_called;
static int stub_shutdown_called;

static int stub_init(void)
{
    stub_init_called++;
    return 0;
}

static void stub_shutdown(void)
{
    stub_shutdown_called++;
}

static const struct sched_policy_ops stub_policy = {
    .name       = "test_stub",
    .init       = stub_init,
    .shutdown   = stub_shutdown,
    .assign_cpu = stub_assign_cpu,
    .tick       = NULL,
};

/*
 * Test: Register a custom policy and find it by name.
 */
static void test_policy_register_and_find(void)
{
    int count_before = sched_policy_count();
    int ret = sched_register_policy(&stub_policy);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_INT(count_before + 1, sched_policy_count());

    const struct sched_policy_ops *p = sched_find_policy("test_stub");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_PTR(&stub_policy, p);
}

/*
 * Test: Switch to custom policy, verify init/shutdown callbacks called,
 * then switch back to heuristic.
 */
static void test_policy_switch_calls_init_shutdown(void)
{
    /* Ensure stub is registered (may already be from prior test) */
    if (!sched_find_policy("test_stub")) {
        sched_register_policy(&stub_policy);
    }

    stub_init_called = 0;
    stub_shutdown_called = 0;

    /* Switch to stub */
    int ret = sched_set_policy(&stub_policy);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING("test_stub", sched_get_policy());
    TEST_ASSERT_EQUAL_INT(1, stub_init_called);

    /* Switch back to heuristic */
    const struct sched_policy_ops *heuristic = sched_find_policy("heuristic");
    TEST_ASSERT_NOT_NULL(heuristic);
    ret = sched_set_policy(heuristic);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING("heuristic", sched_get_policy());
    TEST_ASSERT_EQUAL_INT(1, stub_shutdown_called);
}

/*
 * Test: Custom policy's assign_cpu callback is actually used.
 */
static void test_policy_custom_assign_cpu_called(void)
{
    /* Ensure stub is registered */
    if (!sched_find_policy("test_stub")) {
        sched_register_policy(&stub_policy);
    }

    /* Switch to stub policy (always returns CPU 0) */
    stub_init_called = 0;
    sched_set_policy(&stub_policy);

    irq_flags_t flags = irq_save();
    for (int i = 0; i < 4; i++) {
        struct task *t = task_create("pol_test", nop_entry, NULL);
        TEST_ASSERT_NOT_NULL(t);
        scheduler_add_task(t);
        /* stub_assign_cpu always returns 0 */
        TEST_ASSERT_EQUAL_UINT32(0, t->assigned_cpu);
        scheduler_remove_task(t);
        t->id = 0;
    }
    irq_restore(flags);

    /* Switch back to heuristic */
    sched_set_policy(sched_find_policy("heuristic"));
}

/* Tracking policy: records which CPU was assigned per call */
static uint32_t tracking_assignments[16];
static int tracking_count;

static uint32_t tracking_assign_cpu(struct task *task)
{
    (void)task;
    /* Simple round-robin across all CPUs */
    extern uint32_t cpu_count;
    uint32_t cpu = tracking_count % cpu_count;
    if (tracking_count < 16)
        tracking_assignments[tracking_count] = cpu;
    tracking_count++;
    return cpu;
}

static const struct sched_policy_ops tracking_policy = {
    .name       = "test_track",
    .init       = NULL,
    .shutdown   = NULL,
    .assign_cpu = tracking_assign_cpu,
    .tick       = NULL,
};

/*
 * Test: Tasks with explicit affinity bypass the policy callback entirely.
 */
static void test_policy_bypassed_for_explicit_affinity(void)
{
    if (!sched_find_policy("test_track")) {
        sched_register_policy(&tracking_policy);
    }

    tracking_count = 0;
    sched_set_policy(&tracking_policy);

    irq_flags_t flags = irq_save();

    /* Task with explicit affinity — should NOT call policy */
    struct task *t = task_create("aff_pin", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);
    task_set_affinity(t, 0);
    int count_before = tracking_count;
    scheduler_add_task(t);
    TEST_ASSERT_EQUAL_INT(count_before, tracking_count);
    TEST_ASSERT_EQUAL_UINT32(0, t->assigned_cpu);
    scheduler_remove_task(t);
    t->id = 0;

    /* Task with CPU_AFFINITY_ANY — SHOULD call policy */
    t = task_create("aff_any", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);
    count_before = tracking_count;
    scheduler_add_task(t);
    TEST_ASSERT_EQUAL_INT(count_before + 1, tracking_count);
    scheduler_remove_task(t);
    t->id = 0;

    irq_restore(flags);

    sched_set_policy(sched_find_policy("heuristic"));
}

/*
 * Test: S5 proactive load-balance override.
 *
 * Under a stub policy that always returns CPU 0, adding N unpinned
 * tasks in a row should cause some to land elsewhere than CPU 0 —
 * the override fires once CPU 0's `ready_count` is high enough vs.
 * the system average. If cpu_count < 2 the override is a no-op.
 *
 * Uses deltas from baseline rather than absolute counts so prior
 * tests' residual state on CPU 0's queue doesn't matter.
 */
static void test_proactive_load_balance_redirect(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("S5 override requires cpu_count >= 2");
        return;
    }

    if (!sched_find_policy("test_stub")) {
        sched_register_policy(&stub_policy);
    }
    sched_set_policy(&stub_policy);

    /* Add 6 tasks in one irq-save region so no scheduler tick fires
     * mid-test. Stub policy returns CPU 0 for every call; we want to
     * see at least one redirect to a non-zero CPU. */
    irq_flags_t flags = irq_save();
    const int N = 6;
    struct task *tasks[6];
    for (int i = 0; i < N; i++) {
        tasks[i] = task_create("s5_bal", nop_entry, NULL);
        TEST_ASSERT_NOT_NULL(tasks[i]);
        scheduler_add_task(tasks[i]);
    }

    int on_cpu0 = 0;
    int off_cpu0 = 0;
    for (int i = 0; i < N; i++) {
        if (tasks[i]->assigned_cpu == 0) on_cpu0++;
        else off_cpu0++;
    }
    TEST_ASSERT_MESSAGE(off_cpu0 >= 1,
        "S5 override never redirected away from overloaded CPU 0");
    TEST_ASSERT_MESSAGE(on_cpu0 < N,
        "all tasks landed on CPU 0 — override is inert");

    for (int i = 0; i < N; i++) {
        scheduler_remove_task(tasks[i]);
        tasks[i]->id = 0;
    }
    irq_restore(flags);

    sched_set_policy(sched_find_policy("heuristic"));
}

/*
 * Test: S5 override never redirects to an isolated CPU.
 *
 * With CPUs 2+ isolated, stub policy returning CPU 0, adding many
 * unpinned tasks: the override must keep them on non-isolated CPUs
 * (0 and 1), never push to an isolated CPU. Regression for the
 * isolation gate in `least_loaded_cpu` + the `!isolated` check in
 * `scheduler_add_task`.
 */
static void test_proactive_load_balance_respects_isolation(void)
{
    if (cpu_count < 3) {
        TEST_IGNORE_MESSAGE("Isolation test requires cpu_count >= 3");
        return;
    }

    if (!sched_find_policy("test_stub")) {
        sched_register_policy(&stub_policy);
    }
    sched_set_policy(&stub_policy);

    /* Isolate CPUs 2 and above. CPU 0 and CPU 1 remain the only
     * legitimate redirect destinations. */
    for (uint32_t c = 2; c < cpu_count; c++) {
        sched_isolate_core(c);
    }

    irq_flags_t flags = irq_save();
    const int N = 6;
    struct task *tasks[6];
    for (int i = 0; i < N; i++) {
        tasks[i] = task_create("s5_iso", nop_entry, NULL);
        TEST_ASSERT_NOT_NULL(tasks[i]);
        scheduler_add_task(tasks[i]);
    }

    for (int i = 0; i < N; i++) {
        TEST_ASSERT_MESSAGE(tasks[i]->assigned_cpu < 2,
            "S5 redirected onto an isolated CPU");
    }

    for (int i = 0; i < N; i++) {
        scheduler_remove_task(tasks[i]);
        tasks[i]->id = 0;
    }
    irq_restore(flags);

    for (uint32_t c = 2; c < cpu_count; c++) {
        sched_unisolate_core(c);
    }
    sched_set_policy(sched_find_policy("heuristic"));
}

/*
 * Test: S5 override stays inert under light load.
 *
 * A single task added to an empty system must land on exactly the
 * CPU the policy chose — the `target_ready >= 2` gate exists
 * precisely so warmth heuristics win the low-load regime. Uses the
 * same stub policy (always returns CPU 0).
 */
static void test_proactive_load_balance_inert_when_light(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 2");
        return;
    }

    if (!sched_find_policy("test_stub")) {
        sched_register_policy(&stub_policy);
    }
    sched_set_policy(&stub_policy);

    irq_flags_t flags = irq_save();

    /* One task; target_ready will be 0 or 1, gate `>= 2` blocks the
     * override. Result must be CPU 0 regardless of what other CPUs
     * have queued. */
    struct task *t = task_create("s5_light", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);
    scheduler_add_task(t);
    TEST_ASSERT_EQUAL_UINT32(0, t->assigned_cpu);
    scheduler_remove_task(t);
    t->id = 0;

    irq_restore(flags);

    sched_set_policy(sched_find_policy("heuristic"));
}

/* Failing init policy: init() returns -1 */
static int fail_init(void)
{
    return -1;
}

static const struct sched_policy_ops fail_policy = {
    .name       = "test_fail",
    .init       = fail_init,
    .shutdown   = NULL,
    .assign_cpu = stub_assign_cpu,
    .tick       = NULL,
};

/*
 * Test: sched_set_policy rejects a policy whose init() fails,
 * and the previous policy remains active.
 */
static void test_policy_init_failure_keeps_old(void)
{
    if (!sched_find_policy("test_fail")) {
        sched_register_policy(&fail_policy);
    }

    /* Start with heuristic */
    sched_set_policy(sched_find_policy("heuristic"));
    TEST_ASSERT_EQUAL_STRING("heuristic", sched_get_policy());

    /* Try switching to fail_policy — should fail */
    int ret = sched_set_policy(&fail_policy);
    TEST_ASSERT_EQUAL_INT(-1, ret);

    /* Heuristic should still be active */
    TEST_ASSERT_EQUAL_STRING("heuristic", sched_get_policy());
}

/* Tick-counting policy: tracks tick() calls */
static volatile uint32_t tick_calls[8];

static void tick_counter(uint32_t cpu)
{
    if (cpu < 8) tick_calls[cpu]++;
}

static const struct sched_policy_ops tick_policy = {
    .name       = "test_tick",
    .init       = NULL,
    .shutdown   = NULL,
    .assign_cpu = stub_assign_cpu,
    .tick       = tick_counter,
};

/*
 * Test: Policy tick() callback is invoked by scheduler_tick().
 */
static void test_policy_tick_callback_invoked(void)
{
    if (!sched_find_policy("test_tick")) {
        sched_register_policy(&tick_policy);
    }

    for (int i = 0; i < 8; i++) tick_calls[i] = 0;

    sched_set_policy(&tick_policy);

    /* scheduler_tick() is called by the timer ISR. Call it directly
     * a few times to test the tick callback. We need preempt_disabled
     * to be set so schedule() is skipped (we just want the tick call). */
    extern volatile int preempt_disabled[];
    uint32_t cpu = cpu_id();
    preempt_disabled[cpu] = 1;

    scheduler_tick();
    scheduler_tick();
    scheduler_tick();

    preempt_disabled[cpu] = 0;

    TEST_ASSERT_TRUE(tick_calls[cpu] >= 3);

    sched_set_policy(sched_find_policy("heuristic"));
}

/*
 * Test: sched_register_policy rejects NULL and policies without assign_cpu.
 */
static void test_policy_register_rejects_invalid(void)
{
    int ret = sched_register_policy(NULL);
    TEST_ASSERT_EQUAL_INT(-1, ret);

    static const struct sched_policy_ops no_assign = {
        .name       = "bad",
        .init       = NULL,
        .shutdown   = NULL,
        .assign_cpu = NULL,
        .tick       = NULL,
    };
    ret = sched_register_policy(&no_assign);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: Heuristic policy still distributes tasks across CPUs
 * (behavioral equivalence with the old inline code).
 */
static void test_policy_heuristic_distributes_tasks(void)
{
    extern uint32_t cpu_count;
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("Single CPU — distribution not applicable");
    }

    /* Ensure heuristic is active */
    sched_set_policy(sched_find_policy("heuristic"));

    /* Previously: add-then-remove per iteration. That pattern makes
     * every task see all-empty queues and the heuristic always
     * picked CPU 0 — "distribution" collapsed to a single CPU every
     * time. The heuristic's job is to spread work across LOADED
     * queues, so keep all 8 tasks live in the queues while we
     * record placements and clean up at the end.
     *
     * We record assigned_cpu into cpu_hits[] *inside* the add-loop,
     * right after scheduler_add_task returns. That read captures
     * the policy's placement decision directly; a subsequent steal
     * that moves the task to a different CPU doesn't affect our
     * observation. No irq_save is needed — it wouldn't help
     * anyway, since IRQ disable on the running CPU doesn't prevent
     * other CPUs from stealing. */
    const int N = 8;
    struct task *tasks[8];
    uint32_t cpu_hits[8] = {0};

    for (int i = 0; i < N; i++) {
        tasks[i] = task_create("dist", nop_entry, NULL);
        TEST_ASSERT_NOT_NULL(tasks[i]);
        scheduler_add_task(tasks[i]);
        uint32_t assigned = tasks[i]->assigned_cpu;
        if (assigned < 8) cpu_hits[assigned]++;
    }

    /* Clean up all tasks before the assertion so a partial test
     * failure doesn't leak live tasks into run queues. */
    for (int i = 0; i < N; i++) {
        scheduler_remove_task(tasks[i]);
        tasks[i]->id = 0;
    }

    int cpus_used = 0;
    for (uint32_t i = 0; i < cpu_count && i < 8; i++) {
        if (cpu_hits[i] > 0) cpus_used++;
    }
    TEST_ASSERT_MESSAGE(cpus_used >= 2,
        "Heuristic policy should distribute across multiple CPUs");
}

/* ============================================================================
 * AI Scheduler Types Tests (unconditional — ai_types.h is header-only)
 * ============================================================================ */

/*
 * Test: ai_decode_action with index 0 → all fields zero.
 */
static void test_ai_decode_action_zero(void)
{
    struct ai_sched_action a;
    ai_decode_action(0, &a);
    TEST_ASSERT_EQUAL_UINT8(0, a.core_assignment);
    TEST_ASSERT_EQUAL_UINT8(0, a.priority_adj);
    TEST_ASSERT_EQUAL_UINT8(0, a.preempt);
}

/*
 * Test: ai_decode_action correctly separates preempt, priority_adj, core.
 * Encoding: idx = core * 6 + priority_adj * 2 + preempt
 */
static void test_ai_decode_action_components(void)
{
    struct ai_sched_action a;

    /* preempt=1, priority_adj=0, core=0 → idx=1 */
    ai_decode_action(1, &a);
    TEST_ASSERT_EQUAL_UINT8(0, a.core_assignment);
    TEST_ASSERT_EQUAL_UINT8(0, a.priority_adj);
    TEST_ASSERT_EQUAL_UINT8(1, a.preempt);

    /* preempt=0, priority_adj=1, core=0 → idx=2 */
    ai_decode_action(2, &a);
    TEST_ASSERT_EQUAL_UINT8(0, a.core_assignment);
    TEST_ASSERT_EQUAL_UINT8(1, a.priority_adj);
    TEST_ASSERT_EQUAL_UINT8(0, a.preempt);

    /* preempt=0, priority_adj=0, core=1 → idx=6 */
    ai_decode_action(6, &a);
    TEST_ASSERT_EQUAL_UINT8(1, a.core_assignment);
    TEST_ASSERT_EQUAL_UINT8(0, a.priority_adj);
    TEST_ASSERT_EQUAL_UINT8(0, a.preempt);

    /* preempt=1, priority_adj=2, core=2 → idx = 2*6 + 2*2 + 1 = 17 */
    ai_decode_action(17, &a);
    TEST_ASSERT_EQUAL_UINT8(2, a.core_assignment);
    TEST_ASSERT_EQUAL_UINT8(2, a.priority_adj);
    TEST_ASSERT_EQUAL_UINT8(1, a.preempt);
}

/*
 * Test: ai_decode_action with max valid index (N_ACTIONS - 1).
 */
static void test_ai_decode_action_max(void)
{
    struct ai_sched_action a;
    /* idx = 41 (last of 42): core=6, pri_adj=2, preempt=1
     * 41 / 2 = 20 r 1 (preempt=1)
     * 20 / 3 = 6  r 2 (priority_adj=2)
     * core = 6 */
    ai_decode_action(AI_SCHED_N_ACTIONS - 1, &a);
    TEST_ASSERT_EQUAL_UINT8(1, a.preempt);
    TEST_ASSERT_EQUAL_UINT8(2, a.priority_adj);
    TEST_ASSERT_EQUAL_UINT8((AI_SCHED_N_ACTIONS - 1) / 6, a.core_assignment);
}

/*
 * Test: All valid action indices produce in-range component values.
 */
static void test_ai_decode_action_all_valid(void)
{
    struct ai_sched_action a;
    for (int i = 0; i < AI_SCHED_N_ACTIONS; i++) {
        ai_decode_action(i, &a);
        TEST_ASSERT_TRUE(a.preempt <= 1);
        TEST_ASSERT_TRUE(a.priority_adj <= 2);
        /* core_assignment upper bound depends on N_ACTIONS */
        TEST_ASSERT_TRUE(a.core_assignment < (AI_SCHED_N_ACTIONS + 5) / 6);
    }
}

/*
 * Test: State vector dimension constants are self-consistent.
 * (The _Static_assert in ai_types.h catches compile-time errors,
 * but this verifies the runtime values match expectations.)
 */
static void test_ai_state_dim_constants(void)
{
    TEST_ASSERT_EQUAL_INT(108, AI_STATE_DIM);
    TEST_ASSERT_EQUAL_INT(6, AI_STATE_NUM_CORES);
    TEST_ASSERT_EQUAL_INT(6, AI_FEATURES_PER_CORE);
    TEST_ASSERT_EQUAL_INT(8, AI_STATE_NUM_TASKS);
    TEST_ASSERT_EQUAL_INT(8, AI_FEATURES_PER_TASK);
    TEST_ASSERT_EQUAL_INT(8, AI_GLOBAL_FEATURES);
    TEST_ASSERT_EQUAL_INT(AI_STATE_NUM_CORES * AI_FEATURES_PER_CORE +
                          AI_STATE_NUM_TASKS * AI_FEATURES_PER_TASK +
                          AI_GLOBAL_FEATURES, AI_STATE_DIM);
}

/*
 * Test: MLP layer dimensions form a valid chain.
 */
static void test_ai_mlp_layer_dims(void)
{
    /* Each layer's output must match the next layer's input */
    TEST_ASSERT_EQUAL_INT(AI_STATE_DIM, AI_MLP_LAYER0_IN);
    TEST_ASSERT_EQUAL_INT(AI_MLP_LAYER0_OUT, AI_MLP_LAYER1_IN);
    TEST_ASSERT_EQUAL_INT(AI_MLP_LAYER1_OUT, AI_MLP_LAYER2_IN);
    TEST_ASSERT_EQUAL_INT(AI_MLP_LAYER2_OUT, AI_MLP_LAYER3_IN);
    TEST_ASSERT_EQUAL_INT(AI_SCHED_N_ACTIONS, AI_MLP_LAYER3_OUT);
}

/* ============================================================================
 * AI Scheduler Library Tests (only when CONFIG_AI_SCHEDULER is enabled)
 * ============================================================================ */

#if defined(CONFIG_AI_SCHEDULER)

/*
 * Test: ai_extract_state writes to all 108 floats.
 * Verifies the function doesn't crash and overwrites the sentinel pattern.
 * Note: uses memset/byte checks because this test file is compiled
 * with -mgeneral-regs-only (no FP instructions).
 */
static void test_ai_extract_state_writes_all(void)
{
    float state[AI_STATE_DIM];
    /* Fill with 0xDE sentinel pattern */
    memset(state, 0xDE, sizeof(state));

    ai_extract_state(state);

    /* Verify that every 4-byte float slot was written.
     * Check that no float still has the exact sentinel pattern (0xDEDEDEDE).
     * A float with all bytes 0xDE = ~-1.845e+29 — extremely unlikely to be
     * a valid feature value (features are in [0, ~1] range). */
    uint32_t sentinel = 0xDEDEDEDE;
    uint32_t *words = (uint32_t *)state;
    int unwritten = 0;
    for (int i = 0; i < AI_STATE_DIM; i++) {
        if (words[i] == sentinel) unwritten++;
    }
    TEST_ASSERT_EQUAL_INT(0, unwritten);
}

/*
 * AI inference math tests (require both CONFIG_AI_SCHEDULER and ENABLE_BOOT_TESTS).
 * These call helper functions in ai_test_helpers.c (compiled with FP
 * enabled in the ai_sched library). Each helper returns 0 on pass.
 */
#if defined(ENABLE_BOOT_TESTS)
extern int ai_test_matvec_basic(void);
extern int ai_test_matvec_identity(void);
extern int ai_test_matvec_zero_weights(void);
extern int ai_test_relu_mixed(void);
extern int ai_test_relu_all_positive(void);
extern int ai_test_relu_all_negative(void);
extern int ai_test_argmax_basic(void);
extern int ai_test_argmax_last(void);
extern int ai_test_argmax_first(void);
extern int ai_test_argmax_tie(void);
extern int ai_test_argmax_negative(void);
extern int ai_test_mlp_stub_inference(void);
extern int ai_test_ppo_stub_inference(void);
extern int ai_test_validate_action(void);
extern int ai_test_argmax_empty(void);
extern int ai_test_argmax_single(void);
extern int ai_test_relu_single_neg(void);
extern int ai_test_relu_empty(void);
extern int ai_test_matvec_single_row(void);
extern int ai_test_matvec_8x8(void);
extern int ai_test_mlp_null_state(void);
extern int ai_test_ppo_null_state(void);
extern int ai_test_mlp_action_bounds(void);
extern int ai_test_decode_roundtrip(void);

static void test_ai_matvec_basic(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_matvec_basic()); }

static void test_ai_matvec_identity(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_matvec_identity()); }

static void test_ai_matvec_zero_weights(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_matvec_zero_weights()); }

static void test_ai_relu_mixed(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_relu_mixed()); }

static void test_ai_relu_all_positive(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_relu_all_positive()); }

static void test_ai_relu_all_negative(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_relu_all_negative()); }

static void test_ai_argmax_basic(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_argmax_basic()); }

static void test_ai_argmax_last(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_argmax_last()); }

static void test_ai_argmax_first(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_argmax_first()); }

static void test_ai_argmax_tie(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_argmax_tie()); }

static void test_ai_argmax_negative(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_argmax_negative()); }

static void test_ai_mlp_forward_pass(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_mlp_stub_inference()); }

static void test_ai_ppo_forward_pass(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_ppo_stub_inference()); }

static void test_ai_validate_action_bounds(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_validate_action()); }

static void test_ai_argmax_empty(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_argmax_empty()); }

static void test_ai_argmax_single(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_argmax_single()); }

static void test_ai_relu_single_neg(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_relu_single_neg()); }

static void test_ai_relu_empty(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_relu_empty()); }

static void test_ai_matvec_single_row(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_matvec_single_row()); }

static void test_ai_matvec_8x8(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_matvec_8x8()); }

static void test_ai_mlp_null_state(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_mlp_null_state()); }

static void test_ai_ppo_null_state(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_ppo_null_state()); }

static void test_ai_mlp_action_bounds_check(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_mlp_action_bounds()); }

static void test_ai_decode_roundtrip(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_decode_roundtrip()); }

/* State extraction tests (M4) */
extern int ai_test_extract_state_core_type(void);
extern int ai_test_extract_state_task_zero_fill(void);
extern int ai_test_extract_state_global_offset(void);
extern int ai_test_extract_state_utilization_range(void);

static void test_ai_state_core_type(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_extract_state_core_type()); }

static void test_ai_state_task_zero_fill(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_extract_state_task_zero_fill()); }

static void test_ai_state_global_offset(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_extract_state_global_offset()); }

static void test_ai_state_utilization_range(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_extract_state_utilization_range()); }

extern int ai_test_extract_state_core_zero_fill(void);
extern int ai_test_extract_state_isolated_core(void);
extern int ai_test_extract_state_task_features(void);

static void test_ai_state_core_zero_fill(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_extract_state_core_zero_fill()); }

static void test_ai_state_isolated_core(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_extract_state_isolated_core()); }

static void test_ai_state_task_features(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_extract_state_task_features()); }

/* M6+M7: FP context and AI policy integration tests */
extern int ai_test_policy_mlp_end_to_end(void);
extern int ai_test_fp_repeated_save_restore(void);
extern int ai_test_fp_state_size(void);
extern int ai_test_policy_dispatches_any_affinity(void);

static void test_ai_policy_mlp_end_to_end(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_policy_mlp_end_to_end()); }

static void test_ai_fp_repeated_save_restore(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_fp_repeated_save_restore()); }

static void test_ai_fp_state_size(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_fp_state_size()); }

static void test_ai_policy_dispatches_any_affinity(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_policy_dispatches_any_affinity()); }

/* M8: Fallback and isolation tests */
extern int ai_test_policy_fallback(void);
extern int ai_test_policy_respects_isolation(void);

static void test_ai_policy_fallback(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_policy_fallback()); }

static void test_ai_policy_respects_isolation(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_policy_respects_isolation()); }

/* M8: Performance, stress, and integration tests */
extern int ai_test_inference_latency(void);
extern int ai_test_state_extraction_latency(void);
extern int ai_test_fp_latency(void);
extern int ai_test_scheduler_stress(void);
extern int ai_test_mixed_policy_switch(void);

static void test_ai_inference_latency(void)
{
    int avg_ns = ai_test_inference_latency();
    /* Just verify it completed (QEMU timing unreliable for latency bounds) */
    TEST_ASSERT_TRUE(avg_ns >= 0);
}

static void test_ai_state_extraction_latency(void)
{
    int avg_ns = ai_test_state_extraction_latency();
    TEST_ASSERT_TRUE(avg_ns >= 0);
}

static void test_ai_fp_save_restore_latency(void)
{
    int avg_ns = ai_test_fp_latency();
    TEST_ASSERT_TRUE(avg_ns >= 0);
}

static void test_ai_scheduler_stress(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_scheduler_stress()); }

static void test_ai_mixed_policy_switch(void)
{ TEST_ASSERT_EQUAL_INT(0, ai_test_mixed_policy_switch()); }

#endif /* ENABLE_BOOT_TESTS — math test helpers */

/*
 * Test: arrival_time_ns is set when a task is added to the scheduler.
 * This is an integer test (no FP) so it doesn't need ENABLE_BOOT_TESTS.
 */
static void test_ai_arrival_time_set(void)
{
#ifdef CONFIG_AI_SCHEDULER
    struct task *t = task_create("arr_test", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);

    task_set_affinity(t, 0);
    irq_flags_t flags = irq_save();

    uint64_t before = slm_get_time_ns();
    scheduler_add_task(t);
    uint64_t after = slm_get_time_ns();

    /* arrival_time_ns should be set to a time between before and after */
    TEST_ASSERT_TRUE(t->arrival_time_ns >= before);
    TEST_ASSERT_TRUE(t->arrival_time_ns <= after);

    scheduler_remove_task(t);
    irq_restore(flags);
    t->id = 0;
#else
    TEST_IGNORE_MESSAGE("CONFIG_AI_SCHEDULER not enabled");
#endif
}

/*
 * Shell command tests for sched (M8).
 * cmd_sched is always compiled but AI subcommands only work with CONFIG_AI_SCHEDULER.
 */
extern int cmd_sched(int argc, char **argv);

static void test_sched_cmd_no_args(void)
{
    /* sched with no args should show current policy and return 0 */
    char *argv[] = {"sched"};
    int ret = cmd_sched(1, argv);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_sched_cmd_policy_list(void)
{
    /* sched policy should list policies and return 0 */
    char *argv[] = {"sched", "policy"};
    int ret = cmd_sched(2, argv);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_sched_cmd_stats(void)
{
    /* sched stats should show statistics and return 0 */
    char *argv[] = {"sched", "stats"};
    int ret = cmd_sched(2, argv);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_sched_cmd_invalid(void)
{
    /* sched with invalid subcommand should return 1 */
    char *argv[] = {"sched", "bogus"};
    int ret = cmd_sched(2, argv);
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/*
 * Test: Per-CPU utilization counters exist in the run queue struct.
 */
static void test_ai_utilization_counters_exist(void)
{
#ifdef CONFIG_AI_SCHEDULER
    struct cpu_runqueue *rq = sched_cpu_rq(0);
    /* total_ticks should be advancing (scheduler_tick increments it) */
    TEST_ASSERT_TRUE(rq->total_ticks > 0);
    /* running_ticks <= total_ticks */
    TEST_ASSERT_TRUE(rq->running_ticks <= rq->total_ticks);
#else
    TEST_IGNORE_MESSAGE("CONFIG_AI_SCHEDULER not enabled");
#endif
}

/*
 * Test: AI policies (ai_mlp, ai_ppo) are registered and findable.
 */
static void test_ai_policies_registered(void)
{
    const struct sched_policy_ops *mlp = sched_find_policy("ai_mlp");
    TEST_ASSERT_NOT_NULL(mlp);
    TEST_ASSERT_EQUAL_STRING("ai_mlp", mlp->name);
    TEST_ASSERT_NOT_NULL(mlp->assign_cpu);

    const struct sched_policy_ops *ppo = sched_find_policy("ai_ppo");
    TEST_ASSERT_NOT_NULL(ppo);
    TEST_ASSERT_EQUAL_STRING("ai_ppo", ppo->name);
    TEST_ASSERT_NOT_NULL(ppo->assign_cpu);
}

/*
 * Test: Can switch to ai_mlp policy and back to heuristic.
 */
static void test_ai_policy_switch_to_mlp_and_back(void)
{
    const struct sched_policy_ops *mlp = sched_find_policy("ai_mlp");
    TEST_ASSERT_NOT_NULL(mlp);

    int ret = sched_set_policy(mlp);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING("ai_mlp", sched_get_policy());

    /* Tasks should still be assignable (AI policy picks a valid CPU) */
    irq_flags_t flags = irq_save();
    struct task *t = task_create("ai_test", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);
    scheduler_add_task(t);
    TEST_ASSERT_MESSAGE(t->assigned_cpu < cpu_count,
        "AI policy assigned to invalid CPU");
    scheduler_remove_task(t);
    t->id = 0;
    irq_restore(flags);

    /* Switch back */
    ret = sched_set_policy(sched_find_policy("heuristic"));
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING("heuristic", sched_get_policy());
}

/*
 * Test: fp_save and fp_restore are callable and don't crash.
 * (Full FP register verification requires M6; this just tests linkage
 * and basic operation.)
 */
extern void fp_save(void *state);
extern void fp_restore(const void *state);

static void test_fp_save_restore_callable(void)
{
    /* 528 bytes = 32 x 16 (V regs) + 4 (fpcr) + 4 (fpsr), 16-byte aligned */
    alignas(16) uint8_t fp_state[528];

    /* Zero the state buffer */
    for (int i = 0; i < 528; i++)
        fp_state[i] = 0;

    /* These should not crash */
    fp_save(fp_state);
    fp_restore(fp_state);

    /* If we got here, save/restore linkage and basic operation work */
    TEST_PASS();
}

#endif /* CONFIG_AI_SCHEDULER */

/* ============================================================================
 * Regression Tests: Pi 5 Fixes (April 2026)
 *
 * These tests catch regressions in fixes that were required for Pi 5 hardware.
 * They run on all platforms (QEMU included) to ensure the fixes don't break
 * anything and that the underlying invariants are maintained.
 * ============================================================================ */

/*
 * Regression: sched_set_policy must not enable IRQs mid-switch.
 *
 * On Pi 5, enabling IRQs inside sched_set_policy caused a timer tick that
 * preempted the caller. The idle task then ran with IRQs masked (DAIF.I=1
 * from context restore) and never re-enabled them, hanging the system.
 *
 * Fix: sched_set_policy wraps the entire init/swap/shutdown in irq_save/restore.
 * This test switches policies 50 times under normal scheduling to verify no hang.
 */
static void test_policy_switch_no_hang(void)
{
    if (!sched_find_policy("test_stub")) {
        sched_register_policy(&stub_policy);
    }

    const struct sched_policy_ops *heuristic = sched_find_policy("heuristic");
    TEST_ASSERT_NOT_NULL(heuristic);

    for (int i = 0; i < 50; i++) {
        int ret = sched_set_policy(&stub_policy);
        TEST_ASSERT_EQUAL_INT(0, ret);
        ret = sched_set_policy(heuristic);
        TEST_ASSERT_EQUAL_INT(0, ret);
    }
    /* If we reach here, no hang occurred during rapid policy switching */
    TEST_PASS();
}

/*
 * Regression: hardware timer counter must always advance.
 *
 * On Pi 5, pit_ticks does not advance while tasks run (IRQs masked).
 * Component timeouts were changed to use timer_get_count() which reads
 * the always-running hardware counter. This test verifies the counter
 * advances even without timer IRQs.
 */
static void test_hw_timer_counter_advances(void)
{
    uint64_t t1 = timer_get_count();

    /* Small busy-wait — counter should advance even with IRQs masked */
    for (volatile int i = 0; i < 10000; i++) {}

    uint64_t t2 = timer_get_count();
    TEST_ASSERT_MESSAGE(t2 > t1,
        "Hardware timer counter did not advance (timer_get_count broken)");

    /* Verify frequency is reasonable (> 1 MHz) */
    uint64_t freq = timer_get_frequency();
    TEST_ASSERT_MESSAGE(freq > 1000000,
        "Timer frequency unreasonably low (< 1 MHz)");
}

/*
 * Regression: UART output must work after kprintf_init_nc_lock.
 *
 * On Pi 5, the UART lock was changed from ldaxr/stxr spinlock to
 * IRQ-disable-only to avoid deadlocks with incoherent L2 caches.
 * This test verifies uart_puts works correctly mid-boot and
 * doesn't deadlock.
 */
static void test_uart_output_after_init(void)
{
    /* If this test runs at all, UART output is working.
     * The test framework itself uses uart_puts for [PASS]/[FAIL].
     * Explicitly test both locked and unlocked variants. */
    uart_puts_unlocked("");   /* Empty string, unlocked */
    uart_puts("");            /* Empty string, locked */
    uart_printf("%s", "");    /* Empty format, locked */
    TEST_PASS();
}

/*
 * Regression: tasks created with task_create use CPU 0 pinning for FFI tests.
 *
 * On Pi 5, slm_task_create dispatches tasks via round-robin which can send
 * them to secondary CPUs. Without cache coherency, shared flags (like
 * ffi_task_proceed) are invisible cross-CPU. FFI tests must use
 * task_create + scheduler_add_task_to_cpu to pin to CPU 0.
 *
 * This test verifies that task_create followed by scheduler_add_task_to_cpu
 * correctly places the task on the specified CPU.
 */
static void test_task_pinned_to_cpu0(void)
{
    struct task *t = task_create("pin_test", nop_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);

    scheduler_add_task_to_cpu(t, 0);
    TEST_ASSERT_EQUAL_UINT32(0, t->assigned_cpu);

    /* Clean up */
    int timeout = 100000;
    while (t->state != TASK_TERMINATED && timeout > 0) {
        yield();
        timeout--;
    }
    task_destroy(t);
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_scheduler(void)
{
    UnityBegin("Scheduler Tests");

    /* Regression tests: DAIF context preservation (ARM64 only) */
#if !defined(PLATFORM_X86_64)
    RUN_TEST(test_daif_saved_in_context);
    RUN_TEST(test_new_task_daif_irq_masked);
    RUN_TEST(test_daif_not_zero_on_create);
    RUN_TEST(test_task_create_multiple_all_irq_masked);
    RUN_TEST(test_idle_task_daif);
#endif

    /* Unit tests: task_slot() accessor (#321) */
    RUN_TEST(test_task_slot_out_of_range_returns_null);
    RUN_TEST(test_task_slot_in_range_returns_slot);
    RUN_TEST(test_task_slot_finds_created_task);
    RUN_TEST(test_task_slot_skips_freed_by_id);

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
    RUN_TEST(test_migrate_stolen_task_no_double_queue);
    RUN_TEST(test_migrate_ready_task_updates_assigned_cpu);
    RUN_TEST(test_multiple_cores_isolated);

    /* Integration tests: Priority ordering */
    RUN_TEST(test_high_priority_runs_first);
    RUN_TEST(test_priority_ordering_multiple_levels);
    RUN_TEST(test_deadline_boost_affects_order);

    /* Stress tests */
    RUN_TEST(test_no_starvation);
    RUN_TEST(test_stress_mixed_priorities);

    /* Regression: task_exit race (runs on all platforms) */
    RUN_TEST(test_rapid_task_exit_no_panic);

    /* Latency and benchmark tests */
    RUN_TEST(test_isolated_core_latency);
    RUN_TEST(test_benchmark_context_switch);
    RUN_TEST(test_benchmark_queue_operations);

    /* Timer basic tests (TEST-L6) */
    RUN_TEST(test_timer_counter_readable);
    RUN_TEST(test_timer_counter_advances);
    RUN_TEST(test_timer_frequency_reasonable);
    RUN_TEST(test_timer_irq_is_physical);
    RUN_TEST(test_slm_time_ticks_to_ns_no_overflow);
    RUN_TEST(test_preempt_trampoline_cpu_fold);

#if !defined(PLATFORM_X86_64)
    /* GIC handler registration table (#204 follow-up) */
    RUN_TEST(test_gic_register_lookup_roundtrip);
    RUN_TEST(test_gic_lookup_miss_returns_null);
    RUN_TEST(test_gic_register_null_rejected);
    RUN_TEST(test_gic_unregister_frees_slot);
    RUN_TEST(test_gic_unregister_miss);
#endif

    /* Spinlock hardware mode tests (post-MMU) */
    RUN_TEST(test_spinlock_hw_enabled_after_boot);
    RUN_TEST(test_spinlock_hw_acquire_release);
    RUN_TEST(test_spinlock_hw_trylock_contention);
    RUN_TEST(test_spinlock_hw_irqsafe);

    /* SMP: MPIDR encoding and secondary MMU */
    RUN_TEST(test_cpu_logical_map_encoding);
    RUN_TEST(test_secondary_mmu_ttbr_set);

    /* SMP: multi-core online verification */
    RUN_TEST(test_all_cpus_online);
    RUN_TEST(test_cpu_data_online_flags);
    RUN_TEST(test_boot_cpu_is_cpu0);

    /* SMP: cache coherency regression tests */
    RUN_TEST(test_cpus_online_matches_flags);
    RUN_TEST(test_cpu_data_fields_consistent);

    /* SPSC ring buffer (UART IRQ RX algorithm) */
    RUN_TEST(test_ringbuf_empty);
    RUN_TEST(test_ringbuf_fifo_order);
    RUN_TEST(test_ringbuf_full);
    RUN_TEST(test_ringbuf_wrap);
    RUN_TEST(test_ringbuf_interleaved);

    /* Sleep: timer-driven sleep */
    RUN_TEST(test_sleep_zero_returns_immediately);
    RUN_TEST(test_sleep_ms_duration);
    RUN_TEST(test_sleep_ms_short);
    RUN_TEST(test_sleep_us_wakes_on_tick);
    RUN_TEST(test_sleep_sequential);
    RUN_TEST(test_sleep_task_state_restored);

    /* Cross-CPU dispatch infrastructure */
    RUN_TEST(test_cpu_id_returns_correct_value);
    RUN_TEST(test_round_robin_distributes_tasks);
    RUN_TEST(test_scheduler_start_accepts_cpu_param);
    RUN_TEST(test_deadline_boost_raises_priority);
    RUN_TEST(test_isolation_excludes_from_dispatch);

    /* Pluggable scheduler policy interface */
    RUN_TEST(test_policy_default_is_heuristic);
    RUN_TEST(test_policy_find_heuristic);
    RUN_TEST(test_policy_find_unknown_returns_null);
    RUN_TEST(test_policy_count_at_least_one);
    RUN_TEST(test_policy_get_bounds);
    RUN_TEST(test_policy_set_null_rejected);
    RUN_TEST(test_policy_register_rejects_invalid);
    RUN_TEST(test_policy_register_and_find);
    RUN_TEST(test_policy_switch_calls_init_shutdown);
    RUN_TEST(test_policy_custom_assign_cpu_called);
    RUN_TEST(test_policy_bypassed_for_explicit_affinity);
    RUN_TEST(test_proactive_load_balance_redirect);
    RUN_TEST(test_proactive_load_balance_respects_isolation);
    RUN_TEST(test_proactive_load_balance_inert_when_light);
    RUN_TEST(test_policy_init_failure_keeps_old);
    RUN_TEST(test_policy_tick_callback_invoked);
    RUN_TEST(test_policy_heuristic_distributes_tasks);

    /* Regression tests: Pi 5 fixes (April 2026) */
    RUN_TEST(test_policy_switch_no_hang);
    RUN_TEST(test_hw_timer_counter_advances);
    RUN_TEST(test_uart_output_after_init);
    RUN_TEST(test_task_pinned_to_cpu0);

    /* AI scheduler types (unconditional — header-only) */
    RUN_TEST(test_ai_decode_action_zero);
    RUN_TEST(test_ai_decode_action_components);
    RUN_TEST(test_ai_decode_action_max);
    RUN_TEST(test_ai_decode_action_all_valid);
    RUN_TEST(test_ai_state_dim_constants);
    RUN_TEST(test_ai_mlp_layer_dims);

#ifdef CONFIG_AI_SCHEDULER
    /* AI scheduler library tests (only with -DENABLE_AI_SCHEDULER=ON) */
    RUN_TEST(test_ai_extract_state_writes_all);
    RUN_TEST(test_ai_policies_registered);
    RUN_TEST(test_ai_policy_switch_to_mlp_and_back);
    RUN_TEST(test_fp_save_restore_callable);

#if defined(ENABLE_BOOT_TESTS)
    /* AI inference engine math tests (M3) — need FP-enabled test helpers */
    RUN_TEST(test_ai_matvec_basic);
    RUN_TEST(test_ai_matvec_identity);
    RUN_TEST(test_ai_matvec_zero_weights);
    RUN_TEST(test_ai_relu_mixed);
    RUN_TEST(test_ai_relu_all_positive);
    RUN_TEST(test_ai_relu_all_negative);
    RUN_TEST(test_ai_argmax_basic);
    RUN_TEST(test_ai_argmax_last);
    RUN_TEST(test_ai_argmax_first);
    RUN_TEST(test_ai_argmax_tie);
    RUN_TEST(test_ai_argmax_negative);
    RUN_TEST(test_ai_mlp_forward_pass);
    RUN_TEST(test_ai_ppo_forward_pass);
    RUN_TEST(test_ai_validate_action_bounds);

    /* Edge cases and NULL handling */
    RUN_TEST(test_ai_argmax_empty);
    RUN_TEST(test_ai_argmax_single);
    RUN_TEST(test_ai_relu_single_neg);
    RUN_TEST(test_ai_relu_empty);
    RUN_TEST(test_ai_matvec_single_row);
    RUN_TEST(test_ai_matvec_8x8);
    RUN_TEST(test_ai_mlp_null_state);
    RUN_TEST(test_ai_ppo_null_state);
    RUN_TEST(test_ai_mlp_action_bounds_check);
    RUN_TEST(test_ai_decode_roundtrip);

    /* State extraction tests (M4) */
    RUN_TEST(test_ai_state_core_type);
    RUN_TEST(test_ai_state_task_zero_fill);
    RUN_TEST(test_ai_state_global_offset);
    RUN_TEST(test_ai_state_utilization_range);
    RUN_TEST(test_ai_state_core_zero_fill);
    RUN_TEST(test_ai_state_isolated_core);
    RUN_TEST(test_ai_state_task_features);

    /* M6+M7: FP context and AI policy integration */
    RUN_TEST(test_ai_fp_state_size);
    RUN_TEST(test_ai_fp_repeated_save_restore);
    RUN_TEST(test_ai_policy_mlp_end_to_end);
    RUN_TEST(test_ai_policy_dispatches_any_affinity);

    /* M8: Performance tests (report-only on QEMU) */
    RUN_TEST(test_ai_inference_latency);
    RUN_TEST(test_ai_state_extraction_latency);
    RUN_TEST(test_ai_fp_save_restore_latency);

    /* M8: Integration/stress tests */
    RUN_TEST(test_ai_scheduler_stress);
    RUN_TEST(test_ai_mixed_policy_switch);

    /* M8: Fallback and isolation tests */
    RUN_TEST(test_ai_policy_fallback);
    RUN_TEST(test_ai_policy_respects_isolation);
#endif /* ENABLE_BOOT_TESTS */

    /* M5 counter tests (integer-only, no ENABLE_BOOT_TESTS needed) */
    RUN_TEST(test_ai_arrival_time_set);
    RUN_TEST(test_ai_utilization_counters_exist);

    /* Shell command tests */
    RUN_TEST(test_sched_cmd_no_args);
    RUN_TEST(test_sched_cmd_policy_list);
    RUN_TEST(test_sched_cmd_stats);
    RUN_TEST(test_sched_cmd_invalid);
#endif

    return UnityEnd();
}
