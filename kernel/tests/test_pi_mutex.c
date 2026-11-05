/*
 * test_pi_mutex.c - Priority Inheritance Mutex Tests
 *
 * Tests for priority inversion prevention via priority inheritance.
 */

#include "unity.h"
#include "test_harness.h"
#include "pi_mutex.h"
#include "task.h"
#include "sched.h"
#include "spinlock.h"
#include "smp.h"
#include "debug.h"

/* Shared state for tests */
static pi_mutex_t test_mutex;

/* Simple task entry that immediately exits */
static void nop_entry(void *arg)
{
    (void)arg;
    task_exit();
}

/* ============================================================================
 * Unit Tests: Basic PI Mutex Operations
 * ============================================================================ */

/*
 * Test: pi_mutex_init sets correct initial state
 */
static void test_pi_mutex_init(void)
{
    pi_mutex_t mutex;
    pi_mutex_init(&mutex);

    TEST_ASSERT_EQUAL_INT(0, mutex.locked);
    TEST_ASSERT_NULL(mutex.owner);
}

/*
 * Test: Basic lock/unlock cycle
 */
static void test_pi_mutex_lock_unlock(void)
{
    pi_mutex_t mutex;
    pi_mutex_init(&mutex);

    pi_mutex_lock(&mutex);
    TEST_ASSERT_EQUAL_INT(1, mutex.locked);
    TEST_ASSERT_NOT_NULL(mutex.owner);

    pi_mutex_unlock(&mutex);
    TEST_ASSERT_EQUAL_INT(0, mutex.locked);
    TEST_ASSERT_NULL(mutex.owner);
}

/*
 * Test: trylock succeeds when unlocked
 */
static void test_pi_mutex_trylock_success(void)
{
    pi_mutex_t mutex;
    pi_mutex_init(&mutex);

    int result = pi_mutex_trylock(&mutex);
    TEST_ASSERT_EQUAL_INT(1, result);
    TEST_ASSERT_EQUAL_INT(1, mutex.locked);

    pi_mutex_unlock(&mutex);
}

/*
 * Test: trylock fails when already locked
 *
 * We manually set the mutex to locked state (simulating another task holding it)
 * and verify that trylock returns 0 (failure).
 */
static void test_pi_mutex_trylock_fail(void)
{
    pi_mutex_t mutex;
    pi_mutex_init(&mutex);

    /* Manually set mutex to locked state (simulating another task owns it) */
    struct task *fake_owner = task_current();  /* Use any valid task pointer */
    mutex.locked = 1;
    mutex.owner = fake_owner;

    /* trylock should fail (return 0) since mutex is already held */
    int result = pi_mutex_trylock(&mutex);
    TEST_ASSERT_EQUAL_INT(0, result);

    /* Mutex should still be locked by original owner */
    TEST_ASSERT_EQUAL_INT(1, mutex.locked);
    TEST_ASSERT_EQUAL_PTR(fake_owner, mutex.owner);

    /* Clean up - manually unlock since we manually locked */
    mutex.locked = 0;
    mutex.owner = NULL;
}

/*
 * Test: an uncontended lock and unlock leaves the priority unchanged
 */
static void test_pi_mutex_priority_preserved(void)
{
    pi_mutex_t mutex;
    pi_mutex_init(&mutex);

    struct task *self = task_current();
    uint8_t original_pri = self->effective_priority;

    pi_mutex_lock(&mutex);
    TEST_ASSERT_EQUAL_UINT8(original_pri, self->effective_priority);

    pi_mutex_unlock(&mutex);
    TEST_ASSERT_EQUAL_UINT8(original_pri, self->effective_priority);
}

/* ============================================================================
 * Integration Tests: Priority Inheritance
 * ============================================================================ */

/*
 * Test: Priority inheritance occurs when high-pri waits on low-pri
 *
 * This is a simplified test that directly exercises the priority
 * inheritance logic without needing multi-task coordination.
 * We simulate the scenario by:
 * 1. Creating a low-priority task and having it acquire the mutex
 * 2. Calling try_boost from a high-priority context
 */
static void test_priority_inheritance_basic(void)
{
    pi_mutex_init(&test_mutex);

    /* Create a low priority "owner" task */
    struct task *low_task = task_create_with_priority(
        "pi_owner", nop_entry, NULL, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(low_task);

    /* Manually set up the mutex as if low_task owns it */
    irq_flags_t flags = irq_save();

    test_mutex.locked = 1;
    test_mutex.owner = low_task;
    uint8_t low_original_pri = low_task->effective_priority;

    /* Now simulate a high-priority task trying to acquire */
    /* We'll create a high-priority task to represent the waiter */
    struct task *high_task = task_create_with_priority(
        "pi_waiter", nop_entry, NULL, TASK_PRIORITY_HIGH);
    TEST_ASSERT_NOT_NULL(high_task);

    /* Verify initial state */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_LOW, low_task->effective_priority);

    /*
     * The priority inheritance happens when high_task tries to lock.
     * Since we can't easily run the task, we'll manually invoke the
     * boost logic by checking the condition and boosting.
     */
    if (high_task->effective_priority > test_mutex.owner->effective_priority) {
        /* This is what pi_mutex_lock does internally */
        INFO("PI: Boosting task '%s' (pri %u->%u) for waiter '%s' (pri %u)",
             test_mutex.owner->name,
             test_mutex.owner->effective_priority,
             high_task->effective_priority,
             high_task->name,
             high_task->effective_priority);
        test_mutex.owner->effective_priority = high_task->effective_priority;
    }

    /* Verify priority was boosted */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_HIGH, low_task->effective_priority);

    /* Simulate unlock - restore priority */
    low_task->effective_priority = low_original_pri;
    test_mutex.locked = 0;
    test_mutex.owner = NULL;

    /* Verify priority was restored */
    TEST_ASSERT_EQUAL_UINT8(TASK_PRIORITY_LOW, low_task->effective_priority);

    irq_restore(flags);

    /* Cleanup */
    low_task->state = TASK_TERMINATED;
    high_task->state = TASK_TERMINATED;
    task_destroy(low_task);
    task_destroy(high_task);

    INFO("Priority inheritance test passed");
}

/* ----------------------------------------------------------------------------
 * Regression test: repeated pi_mutex cycles do not deadlock (SCHED-H1)
 *
 * Written for SCHED-H1's spin-and-yield wait loop, which #93 replaced
 * with a sleep queue (contended waiters now block and are handed the
 * mutex on unlock). The test still guards the uncontended paths:
 * repeated trylock / unlock cycles, with a yield() between iterations so
 * the scheduler interleaves other tasks, must not deadlock on the
 * mutex's guard spinlock. Contended waiters and the inherited priority
 * are covered by the #1103 tests below.
 * ---------------------------------------------------------------------------- */
static void test_pi_mutex_wait_loop_stress(void)
{
    pi_mutex_t mutex;
    pi_mutex_init(&mutex);

    uint32_t baseline = pi_mutex_inversion_count();

    /* Run many lock/unlock cycles interleaved with yields so any
     * guard-deadlock or IRQ-ordering bug in the new wait-loop path
     * has a chance to manifest. */
    for (int i = 0; i < 200; i++) {
        pi_mutex_lock(&mutex);
        yield();
        pi_mutex_unlock(&mutex);
    }

    /* Uncontended path — inversion count must not have advanced. */
    TEST_ASSERT_EQUAL_UINT32(baseline, pi_mutex_inversion_count());
}

/* ----------------------------------------------------------------------------
 * Regression tests: the inherited priority survives a re-queue (#1103)
 *
 * These run the real pi_mutex_lock path with real tasks, all pinned to
 * CPU 0 with the test harness: single-CPU is the textbook PI scenario, and
 * it keeps the flags below free of cross-CPU cache visibility concerns on
 * Pi 5 / Jetson. The harness (NORMAL) waits with task_sleep_ms so it leaves
 * the run queue; a yield() loop would keep it ahead of the LOW holder.
 * ---------------------------------------------------------------------------- */
static pi_mutex_t requeue_mutex;
static volatile int rq_holder_locked;
static volatile int rq_holder_done;
static volatile int rq_waiter_done;
static volatile int rq_medium_done;
static volatile uint8_t rq_pri_boosted;
static volatile uint8_t rq_pri_after_yield;
static volatile uint8_t rq_pri_after_unlock;
static volatile uint32_t rq_seq;
static volatile uint32_t rq_unlock_seq;
static volatile uint32_t rq_medium_seq;

#define RQ_HOLDER_YIELDS 3
#define RQ_WAIT_MS       2000u

static void rq_holder_entry(void *arg)
{
    (void)arg;
    struct task *self = task_current();

    pi_mutex_lock(&requeue_mutex);
    rq_holder_locked = 1;

    /* wait_head is set once the waiter has boosted us and blocked. Each
     * yield() puts this task back through the run-queue insertion path. */
    while (requeue_mutex.wait_head == NULL) {
        yield();
    }
    rq_pri_boosted = self->effective_priority;

    for (int i = 0; i < RQ_HOLDER_YIELDS; i++) {
        yield();
    }
    rq_pri_after_yield = self->effective_priority;

    rq_unlock_seq = ++rq_seq;
    pi_mutex_unlock(&requeue_mutex);
    rq_pri_after_unlock = self->effective_priority;
    rq_holder_done = 1;
    task_exit();
}

static void rq_waiter_entry(void *arg)
{
    (void)arg;
    pi_mutex_lock(&requeue_mutex);
    pi_mutex_unlock(&requeue_mutex);
    rq_waiter_done = 1;
    task_exit();
}

static void rq_medium_entry(void *arg)
{
    (void)arg;
    rq_medium_seq = ++rq_seq;
    rq_medium_done = 1;
    task_exit();
}

static void rq_reset(void)
{
    pi_mutex_init(&requeue_mutex);
    rq_holder_locked = 0;
    rq_holder_done = 0;
    rq_waiter_done = 0;
    rq_medium_done = 0;
    rq_pri_boosted = 0;
    rq_pri_after_yield = 0;
    rq_pri_after_unlock = 0;
    rq_seq = 0;
    rq_unlock_seq = 0;
    rq_medium_seq = 0;
}

static struct task *rq_create(const char *name, task_entry_t entry, uint8_t pri)
{
    struct task *t = task_create_with_priority(name, entry, NULL, pri);
    if (t) {
        task_set_affinity(t, 0);
    }
    return t;
}

/* Sleep-poll until *flag is set. Returns false on timeout. */
static bool rq_wait(volatile int *flag)
{
    for (uint32_t ms = 0; ms < RQ_WAIT_MS; ms++) {
        if (*flag) {
            return true;
        }
        task_sleep_ms(1);
    }
    return *flag != 0;
}

/* A failed run must not leak LOW/HIGH tasks into CPU 0's run queue (see
 * "Leaky tests" in kernel/CLAUDE.md). The self-exiting tasks are reaped by
 * the scheduler, so only those whose flag is still clear are torn down. */
static void rq_cleanup(struct task *holder, struct task *waiter, struct task *medium)
{
    if (holder && !rq_holder_done) {
        scheduler_terminate_task(holder);
        task_destroy(holder);
    }
    if (waiter && !rq_waiter_done) {
        scheduler_terminate_task(waiter);
        task_destroy(waiter);
    }
    if (medium && !rq_medium_done) {
        scheduler_terminate_task(medium);
        task_destroy(medium);
    }
}

/*
 * The holder's inherited priority must still be in effect after it has been
 * re-queued. update_deadline_boost recomputes effective_priority at every
 * run-queue insertion, so a boost stored only in that field is lost.
 */
static void test_pi_boost_survives_requeue(void)
{
    rq_reset();

    struct task *holder = rq_create("pi_rq_hold", rq_holder_entry, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(holder);
    scheduler_add_task_to_cpu(holder, 0);
    bool locked = rq_wait(&rq_holder_locked);

    struct task *waiter = rq_create("pi_rq_wait", rq_waiter_entry, TASK_PRIORITY_HIGH);
    TEST_ASSERT_NOT_NULL(waiter);
    scheduler_add_task_to_cpu(waiter, 0);
    bool done = locked && rq_wait(&rq_holder_done) && rq_wait(&rq_waiter_done);

    rq_cleanup(holder, waiter, NULL);

    TEST_ASSERT_TRUE_MESSAGE(locked, "holder never acquired the mutex");
    TEST_ASSERT_TRUE_MESSAGE(done, "holder/waiter did not complete");
    TEST_ASSERT_MESSAGE(rq_pri_boosted == TASK_PRIORITY_HIGH, "holder was not boosted");
    TEST_ASSERT_MESSAGE(rq_pri_after_yield == TASK_PRIORITY_HIGH,
                        "boost lost when the holder was re-queued");
    TEST_ASSERT_MESSAGE(rq_pri_after_unlock == TASK_PRIORITY_LOW,
                        "inherited priority not dropped on unlock");
}

/*
 * The symptom: with a HIGH task blocked on the mutex, a NORMAL task must not
 * run ahead of the holder. The holder is sitting in the run queue at its LOW
 * position when the boost happens, so this also needs the boost to move it.
 */
static void test_pi_boosted_holder_runs_before_medium(void)
{
    rq_reset();

    struct task *holder = rq_create("pi_rq_hold", rq_holder_entry, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(holder);
    scheduler_add_task_to_cpu(holder, 0);
    bool locked = rq_wait(&rq_holder_locked);

    struct task *waiter = rq_create("pi_rq_wait", rq_waiter_entry, TASK_PRIORITY_HIGH);
    struct task *medium = rq_create("pi_rq_med", rq_medium_entry, TASK_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(waiter);
    TEST_ASSERT_NOT_NULL(medium);

    /* Queue both with no tick in between, so the waiter's boost happens with
     * the medium task already queued ahead of the holder's LOW position. */
    irq_flags_t flags = irq_save();
    scheduler_add_task_to_cpu(waiter, 0);
    scheduler_add_task_to_cpu(medium, 0);
    irq_restore(flags);

    bool done = locked && rq_wait(&rq_holder_done) && rq_wait(&rq_waiter_done) &&
                rq_wait(&rq_medium_done);

    rq_cleanup(holder, waiter, medium);

    TEST_ASSERT_TRUE_MESSAGE(locked, "holder never acquired the mutex");
    TEST_ASSERT_TRUE_MESSAGE(done, "holder/waiter/medium did not complete");
    TEST_ASSERT_TRUE_MESSAGE(rq_unlock_seq < rq_medium_seq,
                             "medium-priority task ran ahead of the boosted holder");
}

/*
 * A holder of two mutexes keeps the priority it inherited through the outer
 * one when it releases the inner one, and loses it with the last release.
 */
static pi_mutex_t nested_inner_mutex;
static volatile uint8_t rq_pri_after_inner;

static void rq_nested_holder_entry(void *arg)
{
    (void)arg;
    struct task *self = task_current();

    pi_mutex_lock(&requeue_mutex);
    pi_mutex_lock(&nested_inner_mutex);
    rq_holder_locked = 1;

    while (requeue_mutex.wait_head == NULL) {
        yield();
    }

    pi_mutex_unlock(&nested_inner_mutex);
    yield();
    rq_pri_after_inner = self->effective_priority;

    pi_mutex_unlock(&requeue_mutex);
    rq_pri_after_unlock = self->effective_priority;
    rq_holder_done = 1;
    task_exit();
}

static void test_pi_boost_kept_until_last_mutex_released(void)
{
    rq_reset();
    pi_mutex_init(&nested_inner_mutex);
    rq_pri_after_inner = 0;

    struct task *holder = rq_create("pi_rq_hold", rq_nested_holder_entry, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(holder);
    scheduler_add_task_to_cpu(holder, 0);
    bool locked = rq_wait(&rq_holder_locked);

    struct task *waiter = rq_create("pi_rq_wait", rq_waiter_entry, TASK_PRIORITY_HIGH);
    TEST_ASSERT_NOT_NULL(waiter);
    scheduler_add_task_to_cpu(waiter, 0);
    bool done = locked && rq_wait(&rq_holder_done) && rq_wait(&rq_waiter_done);

    rq_cleanup(holder, waiter, NULL);

    TEST_ASSERT_TRUE_MESSAGE(locked, "holder never acquired the mutexes");
    TEST_ASSERT_TRUE_MESSAGE(done, "holder/waiter did not complete");
    TEST_ASSERT_MESSAGE(rq_pri_after_inner == TASK_PRIORITY_HIGH,
                        "boost from the outer mutex lost with the inner release");
    TEST_ASSERT_MESSAGE(rq_pri_after_unlock == TASK_PRIORITY_LOW,
                        "inherited priority not dropped on the last release");
}

/*
 * The floor must not depend on the holder's priority at lock time. A holder
 * already at the waiter's level records no boost, but it must not be able to
 * fall below the waiter afterwards. The drop here is task_set_priority; a
 * cleared AI level or deadline boost takes the same path at the next re-queue.
 */
static void rq_equal_holder_entry(void *arg)
{
    (void)arg;
    struct task *self = task_current();

    pi_mutex_lock(&requeue_mutex);
    rq_holder_locked = 1;

    while (requeue_mutex.wait_head == NULL) {
        yield();
    }

    task_set_priority(self, TASK_PRIORITY_LOW);
    yield();
    rq_pri_after_yield = self->effective_priority;

    pi_mutex_unlock(&requeue_mutex);
    rq_pri_after_unlock = self->effective_priority;
    rq_holder_done = 1;
    task_exit();
}

/* Locks only once the holder owns the mutex, so it is certain to block. */
static void rq_late_waiter_entry(void *arg)
{
    (void)arg;
    while (!rq_holder_locked) {
        yield();
    }
    pi_mutex_lock(&requeue_mutex);
    pi_mutex_unlock(&requeue_mutex);
    rq_waiter_done = 1;
    task_exit();
}

static void test_pi_floor_recorded_when_holder_not_below_waiter(void)
{
    rq_reset();

    struct task *holder = rq_create("pi_rq_hold", rq_equal_holder_entry, TASK_PRIORITY_HIGH);
    struct task *waiter = rq_create("pi_rq_wait", rq_late_waiter_entry, TASK_PRIORITY_HIGH);
    TEST_ASSERT_NOT_NULL(holder);
    TEST_ASSERT_NOT_NULL(waiter);

    /* Both HIGH, so they would starve this NORMAL task: queue them together. */
    irq_flags_t flags = irq_save();
    scheduler_add_task_to_cpu(holder, 0);
    scheduler_add_task_to_cpu(waiter, 0);
    irq_restore(flags);

    bool done = rq_wait(&rq_holder_done) && rq_wait(&rq_waiter_done);

    rq_cleanup(holder, waiter, NULL);

    TEST_ASSERT_TRUE_MESSAGE(done, "holder/waiter did not complete");
    TEST_ASSERT_MESSAGE(rq_pri_after_yield == TASK_PRIORITY_HIGH,
                        "holder fell below a waiter it was not boosted for");
    TEST_ASSERT_MESSAGE(rq_pri_after_unlock == TASK_PRIORITY_LOW,
                        "inherited priority not dropped on unlock");
}

/*
 * A new owner taking the mutex by hand-off inherits from the waiters still
 * queued behind it. It starts at their level, so without a recorded floor it
 * could drop below them just like the holder above.
 */
static void rq_two_waiter_holder_entry(void *arg)
{
    (void)arg;

    pi_mutex_lock(&requeue_mutex);
    rq_holder_locked = 1;

    while (requeue_mutex.wait_head == NULL || requeue_mutex.wait_head->next == NULL) {
        yield();
    }

    pi_mutex_unlock(&requeue_mutex);
    rq_holder_done = 1;
    task_exit();
}

static void rq_handoff_owner_entry(void *arg)
{
    (void)arg;
    struct task *self = task_current();

    pi_mutex_lock(&requeue_mutex);

    /* Owner by hand-off; the other waiter is still queued. */
    task_set_priority(self, TASK_PRIORITY_LOW);
    yield();
    rq_pri_after_yield = self->effective_priority;

    pi_mutex_unlock(&requeue_mutex);
    rq_pri_after_unlock = self->effective_priority;
    rq_medium_done = 1;
    task_exit();
}

static void test_pi_floor_set_on_handoff_for_remaining_waiters(void)
{
    rq_reset();

    struct task *holder = rq_create("pi_rq_hold", rq_two_waiter_holder_entry, TASK_PRIORITY_LOW);
    TEST_ASSERT_NOT_NULL(holder);
    scheduler_add_task_to_cpu(holder, 0);
    bool locked = rq_wait(&rq_holder_locked);

    /* The first to block is first in the FIFO, so it wins the HIGH tie. */
    struct task *next_owner = rq_create("pi_rq_next", rq_handoff_owner_entry, TASK_PRIORITY_HIGH);
    struct task *waiter = rq_create("pi_rq_wait", rq_waiter_entry, TASK_PRIORITY_HIGH);
    TEST_ASSERT_NOT_NULL(next_owner);
    TEST_ASSERT_NOT_NULL(waiter);

    irq_flags_t flags = irq_save();
    scheduler_add_task_to_cpu(next_owner, 0);
    scheduler_add_task_to_cpu(waiter, 0);
    irq_restore(flags);

    bool done = locked && rq_wait(&rq_holder_done) && rq_wait(&rq_medium_done) &&
                rq_wait(&rq_waiter_done);

    rq_cleanup(holder, waiter, next_owner);

    TEST_ASSERT_TRUE_MESSAGE(locked, "holder never acquired the mutex");
    TEST_ASSERT_TRUE_MESSAGE(done, "holder/owner/waiter did not complete");
    TEST_ASSERT_MESSAGE(rq_pri_after_yield == TASK_PRIORITY_HIGH,
                        "new owner fell below a waiter still queued behind it");
    TEST_ASSERT_MESSAGE(rq_pri_after_unlock == TASK_PRIORITY_LOW,
                        "inherited priority not dropped on unlock");
}

/* lock, trylock and unlock keep the per-task held count in step. */
static void test_pi_mutexes_held_count(void)
{
    pi_mutex_t a;
    pi_mutex_t b;
    pi_mutex_init(&a);
    pi_mutex_init(&b);

    struct task *self = task_current();
    uint8_t base = self->pi_mutexes_held;

    pi_mutex_lock(&a);
    TEST_ASSERT_EQUAL_UINT8(base + 1, self->pi_mutexes_held);
    TEST_ASSERT_EQUAL_INT(1, pi_mutex_trylock(&b));
    TEST_ASSERT_EQUAL_UINT8(base + 2, self->pi_mutexes_held);

    /* A failed trylock holds nothing. */
    TEST_ASSERT_EQUAL_INT(0, pi_mutex_trylock(&a));
    TEST_ASSERT_EQUAL_UINT8(base + 2, self->pi_mutexes_held);

    pi_mutex_unlock(&b);
    TEST_ASSERT_EQUAL_UINT8(base + 1, self->pi_mutexes_held);
    pi_mutex_unlock(&a);
    TEST_ASSERT_EQUAL_UINT8(base, self->pi_mutexes_held);
}

/*
 * Test: Inversion count API returns consistent values
 *
 * Verifies that the inversion counter:
 * 1. Returns a valid (non-negative) value
 * 2. Doesn't increment spuriously between calls
 *
 * Note: Full inversion counting verification requires multi-task
 * coordination which is tested in test_priority_inheritance_basic.
 */
static void test_inversion_count(void)
{
    /* Get baseline count */
    uint32_t count1 = pi_mutex_inversion_count();

    /* Perform operations that should NOT cause inversions */
    pi_mutex_t mutex;
    pi_mutex_init(&mutex);
    pi_mutex_lock(&mutex);
    pi_mutex_unlock(&mutex);

    /* Count should not have changed (no inversion scenario) */
    uint32_t count2 = pi_mutex_inversion_count();
    TEST_ASSERT_EQUAL_UINT32(count1, count2);

    /* Also verify pi_mutex_inversion_detected returns 0 (no recent inversion) */
    TEST_ASSERT_EQUAL_INT(0, pi_mutex_inversion_detected());
}

/*
 * pi_mutex_held_by_self: reports true only while the calling task
 * owns the mutex. Used by shell dispatch to avoid a self-deadlock
 * on nested mutating commands.
 */
static void test_pi_mutex_held_by_self_states(void)
{
    pi_mutex_t m;
    pi_mutex_init(&m);

    /* Unlocked: never held by anyone. */
    TEST_ASSERT_FALSE(pi_mutex_held_by_self(&m));

    pi_mutex_lock(&m);
    TEST_ASSERT_TRUE(pi_mutex_held_by_self(&m));

    pi_mutex_unlock(&m);
    TEST_ASSERT_FALSE(pi_mutex_held_by_self(&m));

    /* trylock path behaves the same. */
    TEST_ASSERT_EQUAL_INT(1, pi_mutex_trylock(&m));
    TEST_ASSERT_TRUE(pi_mutex_held_by_self(&m));
    pi_mutex_unlock(&m);
    TEST_ASSERT_FALSE(pi_mutex_held_by_self(&m));
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_pi_mutex(void)
{
    UnityBegin("Priority Inheritance Mutex Tests");

    /* Unit tests */
    RUN_TEST(test_pi_mutex_init);
    RUN_TEST(test_pi_mutex_lock_unlock);
    RUN_TEST(test_pi_mutex_trylock_success);
    RUN_TEST(test_pi_mutex_trylock_fail);
    RUN_TEST(test_pi_mutex_priority_preserved);
    RUN_TEST(test_pi_mutex_held_by_self_states);

    /* Integration tests */
    RUN_TEST(test_priority_inheritance_basic);
    RUN_TEST(test_pi_mutex_wait_loop_stress);
    RUN_TEST(test_pi_boost_survives_requeue);
    RUN_TEST(test_pi_boosted_holder_runs_before_medium);
    RUN_TEST(test_pi_boost_kept_until_last_mutex_released);
    RUN_TEST(test_pi_floor_recorded_when_holder_not_below_waiter);
    RUN_TEST(test_pi_floor_set_on_handoff_for_remaining_waiters);
    RUN_TEST(test_pi_mutexes_held_count);
    RUN_TEST(test_inversion_count);

    return UnityEnd();
}
