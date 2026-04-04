/*
 * test_integration.c - Multi-core scheduler integration tests
 *
 * These tests exercise the scheduler with actual tasks running across
 * multiple CPUs. Unlike unit tests, they require the full scheduler
 * infrastructure to be running.
 *
 * Tests:
 *   - Multi-core task execution
 *   - Cross-core task migration
 *   - Stress testing with many concurrent tasks
 *   - Lock contention across CPUs
 *   - Task lifecycle (create/destroy cycles)
 */

#include "unity.h"
#include "../include/uart.h"
#include "../include/task.h"
#include "../include/sched.h"
#include "../include/smp.h"
#include "../include/spinlock.h"
#include "../include/pmm.h"
#include "../include/platform.h"
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Test Infrastructure
 * ============================================================================ */

/*
 * Simple delay loop (busy wait)
 */
static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile("nop");
    }
}

/* Shared test state protected by spinlock */
static struct {
    spinlock_t lock;
    volatile uint32_t tasks_completed;
    volatile uint32_t total_iterations;
    volatile uint32_t cpu_task_count[MAX_CPUS];
} test_state = {
    .lock = SPINLOCK_INIT,
    .tasks_completed = 0,
    .total_iterations = 0,
    .cpu_task_count = {0}
};

static void reset_test_state(void)
{
    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed = 0;
    test_state.total_iterations = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        test_state.cpu_task_count[i] = 0;
    }
    spin_unlock_irqrestore(&test_state.lock, flags);
}

/* ============================================================================
 * Task Functions for Tests
 * ============================================================================ */

/* Migration test state */
static volatile bool migration_ready = false;
static volatile bool migration_done = false;
static volatile uint32_t migration_cpu_before = 0;
static volatile uint32_t migration_cpu_after = 0;

static void migration_test_task(void *arg)
{
    (void)arg;

    migration_cpu_before = cpu_id();

    /* Signal ready for migration */
    migration_ready = true;

    /* Wait for migration to complete */
    while (!migration_done) {
        delay(10000);
        yield();
    }

    migration_cpu_after = cpu_id();

    /* Do some work to prove we're running */
    for (int i = 0; i < 3; i++) {
        delay(200000);
    }

    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed++;
    spin_unlock_irqrestore(&test_state.lock, flags);
}

/* Simple named tasks for basic multi-core test */
static void task_a_func(void *arg)
{
    int count = (int)(uintptr_t)arg;
    for (int i = 0; i < count; i++) {
        delay(500000);
    }
}

static void task_b_func(void *arg)
{
    int count = (int)(uintptr_t)arg;
    for (int i = 0; i < count; i++) {
        delay(500000);
    }
}

static void task_c_func(void *arg)
{
    int count = (int)(uintptr_t)arg;
    for (int i = 0; i < count; i++) {
        delay(500000);
    }
}

/* Stress task function */
static void stress_task_func(void *arg)
{
    (void)arg;

    for (int i = 0; i < 2; i++) {
        delay(200000);
    }

    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed++;
    spin_unlock_irqrestore(&test_state.lock, flags);
}

/* Lock contention task */
static spinlock_t contention_lock = SPINLOCK_INIT;
static volatile uint32_t contention_counter = 0;

static void contention_task(void *arg)
{
    int increments = (int)(uintptr_t)arg;

    for (int i = 0; i < increments; i++) {
        irq_flags_t flags = spin_lock_irqsave(&contention_lock);
        contention_counter++;
        spin_unlock_irqrestore(&contention_lock, flags);
        delay(1000);
    }

    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed++;
    spin_unlock_irqrestore(&test_state.lock, flags);
}

/* Lifecycle task - completes immediately */
static volatile uint32_t lifecycle_completed = 0;

static void lifecycle_task_func(void *arg)
{
    (void)arg;

    /* Atomic increment using spinlock for reliability */
    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    lifecycle_completed++;
    spin_unlock_irqrestore(&test_state.lock, flags);
}

/* ============================================================================
 * Integration Tests
 * ============================================================================ */

/*
 * Test: Basic Multi-Core Execution
 * Distribute 3 tasks to 3 different CPUs, verify all complete.
 */
static void test_multicore_basic(void)
{
#if defined(PLATFORM_RASPI5)
    TEST_IGNORE_MESSAGE("Cross-CPU dispatch requires SMPEN (not set by TF-A on Pi 5)");
#endif
    struct task *task_a = task_create("task_a", task_a_func, (void *)3);
    struct task *task_b = task_create("task_b", task_b_func, (void *)3);
    struct task *task_c = task_create("task_c", task_c_func, (void *)3);

    TEST_ASSERT_NOT_NULL_MESSAGE(task_a, "Failed to create task_a");
    TEST_ASSERT_NOT_NULL_MESSAGE(task_b, "Failed to create task_b");
    TEST_ASSERT_NOT_NULL_MESSAGE(task_c, "Failed to create task_c");

    scheduler_add_task_to_cpu(task_a, 1);
    scheduler_add_task_to_cpu(task_b, 2);
    scheduler_add_task_to_cpu(task_c, 3);

    /* Wait for completion with timeout */
    int timeout = 500;
    while ((task_a->state != TASK_TERMINATED ||
            task_b->state != TASK_TERMINATED ||
            task_c->state != TASK_TERMINATED) && timeout > 0) {
        delay(100000);
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for tasks to complete");
    TEST_ASSERT_EQUAL(TASK_TERMINATED, task_a->state);
    TEST_ASSERT_EQUAL(TASK_TERMINATED, task_b->state);
    TEST_ASSERT_EQUAL(TASK_TERMINATED, task_c->state);
}

/*
 * Test: Task Migration
 * Create a task on CPU 1, migrate to CPU 3 before it runs.
 */
static void test_task_migration(void)
{
#if defined(PLATFORM_RASPI5)
    TEST_IGNORE_MESSAGE("Cross-CPU dispatch requires SMPEN (not set by TF-A on Pi 5)");
#endif
    migration_ready = false;
    migration_done = false;
    migration_cpu_before = 0;
    migration_cpu_after = 0;
    reset_test_state();

    struct task *mig_task = task_create("migrate", migration_test_task, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(mig_task, "Failed to create migration task");

    /* Create blocker to occupy CPU 1 */
    struct task *blocker = task_create("blocker", task_a_func, (void *)10);
    TEST_ASSERT_NOT_NULL_MESSAGE(blocker, "Failed to create blocker task");

    /* Add blocker first, then migration task */
    scheduler_add_task_to_cpu(blocker, 1);
    delay(50000);
    scheduler_add_task_to_cpu(mig_task, 1);

    /* Migrate while blocker is running */
    int ret = sched_migrate_task(mig_task, 3);
    TEST_ASSERT_MESSAGE(ret == 0, "sched_migrate_task failed");

    /* Wait for task to start and signal ready */
    int timeout = 200;
    while (!migration_ready && timeout > 0) {
        delay(50000);
        timeout--;
    }
    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for migration_ready");

    /* Let task finish */
    migration_done = true;

    /* Wait for completion */
    timeout = 200;
    while ((mig_task->state != TASK_TERMINATED ||
            blocker->state != TASK_TERMINATED) && timeout > 0) {
        delay(100000);
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for task completion");
    TEST_ASSERT_MESSAGE(migration_cpu_before == 3, "Task should run on CPU 3 after migration");
}

/*
 * Test: Stress Test - Many Tasks Across CPUs
 * Create 6 tasks distributed across CPUs 1, 2, 3.
 */
static void test_stress_multicpu(void)
{
#if defined(PLATFORM_RASPI5)
    TEST_IGNORE_MESSAGE("Cross-CPU dispatch requires SMPEN (not set by TF-A on Pi 5)");
#endif
    reset_test_state();

    struct task *tasks[6];
    static const char *names[6] = {"s0", "s1", "s2", "s3", "s4", "s5"};

    for (int i = 0; i < 6; i++) {
        tasks[i] = task_create(names[i], stress_task_func, (void *)(uintptr_t)i);
        TEST_ASSERT_NOT_NULL(tasks[i]);
    }

    /* Distribute: 2 tasks per CPU */
    scheduler_add_task_to_cpu(tasks[0], 1);
    scheduler_add_task_to_cpu(tasks[1], 2);
    scheduler_add_task_to_cpu(tasks[2], 3);
    scheduler_add_task_to_cpu(tasks[3], 1);
    scheduler_add_task_to_cpu(tasks[4], 2);
    scheduler_add_task_to_cpu(tasks[5], 3);

    /* Wait for completion */
    int timeout = 500;
    while ((tasks[0]->state != TASK_TERMINATED ||
            tasks[1]->state != TASK_TERMINATED ||
            tasks[2]->state != TASK_TERMINATED ||
            tasks[3]->state != TASK_TERMINATED ||
            tasks[4]->state != TASK_TERMINATED ||
            tasks[5]->state != TASK_TERMINATED) && timeout > 0) {
        delay(100000);
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for stress tasks");
    TEST_ASSERT_EQUAL(6, test_state.tasks_completed);
}

/*
 * Test: Lock Contention
 * Multiple tasks contending on the same spinlock.
 */
static void test_lock_contention(void)
{
#if defined(PLATFORM_RASPI5)
    TEST_IGNORE_MESSAGE("Cross-CPU dispatch requires SMPEN (not set by TF-A on Pi 5)");
#endif
    reset_test_state();
    contention_counter = 0;

    #define CONTENTION_TASKS 3
    #define INCREMENTS_PER_TASK 50

    struct task *tasks[CONTENTION_TASKS];

    for (int i = 0; i < CONTENTION_TASKS; i++) {
        char name[8];
        name[0] = 'c'; name[1] = 'n'; name[2] = 't';
        name[3] = '0' + (char)i; name[4] = '\0';
        tasks[i] = task_create(name, contention_task, (void *)(uintptr_t)INCREMENTS_PER_TASK);
        TEST_ASSERT_NOT_NULL(tasks[i]);
        scheduler_add_task_to_cpu(tasks[i], (uint32_t)(i % 3) + 1);
    }

    /* Wait for completion */
    int timeout = 300;
    while (test_state.tasks_completed < CONTENTION_TASKS && timeout > 0) {
        yield();
        delay(100000);
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for contention tasks");

    uint32_t expected = CONTENTION_TASKS * INCREMENTS_PER_TASK;
    TEST_ASSERT_MESSAGE(contention_counter == expected, "Race condition detected");
}

/*
 * Test: Task Lifecycle
 * Rapidly create and destroy tasks to verify cleanup.
 */
static void test_task_lifecycle(void)
{
#if defined(PLATFORM_RASPI5)
    TEST_IGNORE_MESSAGE("Cross-CPU dispatch requires SMPEN (not set by TF-A on Pi 5)");
#endif
    #define LIFECYCLE_CYCLES 8

    uint64_t initial_free = pmm_get_free_pages();
    lifecycle_completed = 0;

    for (int cycle = 0; cycle < LIFECYCLE_CYCLES; cycle++) {
        char name[8];
        name[0] = 'l'; name[1] = 'c'; name[2] = '0' + (char)cycle; name[3] = '\0';

        struct task *t = task_create(name, lifecycle_task_func, NULL);
        TEST_ASSERT_NOT_NULL(t);
        scheduler_add_task_to_cpu(t, (uint32_t)(cycle % 3) + 1);
    }

    /* Wait for completion */
    int timeout = 300;  /* Increased timeout for lifecycle tasks */
    while (lifecycle_completed < LIFECYCLE_CYCLES && timeout > 0) {
        yield();
        delay(50000);
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for lifecycle tasks");
    TEST_ASSERT_MESSAGE(lifecycle_completed == LIFECYCLE_CYCLES, "Not all lifecycle tasks completed");

    /* Give scheduler time to clean up zombies */
    for (int i = 0; i < 10; i++) {
        yield();
        delay(50000);
    }

    /* Verify no major memory leak */
    uint64_t final_free = pmm_get_free_pages();
    TEST_ASSERT_MESSAGE(final_free >= initial_free - 4, "Memory leak detected");
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_integration(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_multicore_basic);
    RUN_TEST(test_task_migration);
    RUN_TEST(test_stress_multicpu);
    RUN_TEST(test_lock_contention);
    RUN_TEST(test_task_lifecycle);

    return UNITY_END();
}
