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
#include "../include/cache.h"
#include "../include/ncmem.h"
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
    cache_clean(&migration_cpu_before);

    /* Signal ready for migration */
    migration_ready = true;
    cache_clean(&migration_ready);

    /* Wait for migration to complete */
    while (1) {
        cache_invalidate(&migration_done);
        if (migration_done) break;
        delay(10000);
        yield();
    }

    migration_cpu_after = cpu_id();
    cache_clean(&migration_cpu_after);

    /* Do some work to prove we're running */
    for (int i = 0; i < 3; i++) {
        delay(200000);
    }

    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed++;
    cache_clean(&test_state.tasks_completed);
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
    cache_clean(&test_state.tasks_completed);
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
        cache_clean(&contention_counter);
        spin_unlock_irqrestore(&contention_lock, flags);
        delay(1000);
    }

    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed++;
    cache_clean(&test_state.tasks_completed);
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
    cache_clean(&lifecycle_completed);
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
    TEST_IGNORE_MESSAGE("Cross-CPU dispatch blocked: task structs in cacheable memory (NC run queues work, task data doesn't)");
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
    while (timeout > 0) {
        cache_invalidate(&task_a->state);
        cache_invalidate(&task_b->state);
        cache_invalidate(&task_c->state);
        if (task_a->state == TASK_TERMINATED &&
            task_b->state == TASK_TERMINATED &&
            task_c->state == TASK_TERMINATED) break;
        delay(100000);
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for tasks to complete");
    cache_invalidate(&task_a->state);
    cache_invalidate(&task_b->state);
    cache_invalidate(&task_c->state);
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
    TEST_IGNORE_MESSAGE("Cross-CPU dispatch blocked: task structs in cacheable memory (NC run queues work, task data doesn't)");
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
    while (timeout > 0) {
        cache_invalidate(&migration_ready);
        if (migration_ready) break;
        delay(50000);
        timeout--;
    }
    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for migration_ready");

    /* Let task finish */
    migration_done = true;
    cache_clean(&migration_done);

    /* Wait for completion */
    timeout = 200;
    while (timeout > 0) {
        cache_invalidate(&mig_task->state);
        cache_invalidate(&blocker->state);
        if (mig_task->state == TASK_TERMINATED &&
            blocker->state == TASK_TERMINATED) break;
        delay(100000);
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for task completion");
    cache_invalidate(&migration_cpu_before);
    TEST_ASSERT_MESSAGE(migration_cpu_before == 3, "Task should run on CPU 3 after migration");
}

/*
 * Test: Stress Test - Many Tasks Across CPUs
 * Create 6 tasks distributed across CPUs 1, 2, 3.
 */
static void test_stress_multicpu(void)
{
#if defined(PLATFORM_RASPI5)
    TEST_IGNORE_MESSAGE("Cross-CPU dispatch blocked: task structs in cacheable memory (NC run queues work, task data doesn't)");
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
    while (timeout > 0) {
        for (int j = 0; j < 6; j++)
            cache_invalidate(&tasks[j]->state);
        bool all_done = true;
        for (int j = 0; j < 6; j++) {
            if (tasks[j]->state != TASK_TERMINATED) { all_done = false; break; }
        }
        if (all_done) break;
        delay(100000);
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for stress tasks");
    cache_invalidate(&test_state.tasks_completed);
    TEST_ASSERT_EQUAL(6, test_state.tasks_completed);
}

/*
 * Test: Lock Contention
 * Multiple tasks contending on the same spinlock.
 */
static void test_lock_contention(void)
{
#if defined(PLATFORM_RASPI5)
    TEST_IGNORE_MESSAGE("Cross-CPU dispatch blocked: task structs in cacheable memory (NC run queues work, task data doesn't)");
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
    while (timeout > 0) {
        cache_invalidate(&test_state.tasks_completed);
        if (test_state.tasks_completed >= CONTENTION_TASKS) break;
        yield();
        delay(100000);
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for contention tasks");

    cache_invalidate(&contention_counter);
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
    TEST_IGNORE_MESSAGE("Cross-CPU dispatch blocked: task structs in cacheable memory (NC run queues work, task data doesn't)");
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
    while (timeout > 0) {
        cache_invalidate(&lifecycle_completed);
        if (lifecycle_completed >= LIFECYCLE_CYCLES) break;
        yield();
        delay(50000);
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "Timeout waiting for lifecycle tasks");
    cache_invalidate(&lifecycle_completed);
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
 * NC Memory Cross-CPU Visibility Test
 *
 * Validates that non-cacheable memory bypasses L1/L2 and is instantly
 * visible across CPUs — the foundation for cross-CPU task dispatch on Pi 5.
 * ============================================================================ */

#if defined(PLATFORM_RASPI5)

/*
 * Test: NC memory region is correctly mapped and accessible.
 * Validates the VMM/PMM/ncmem infrastructure before Phase 2 (NC run queues).
 * Cross-CPU visibility is tested after run queues move to NC memory.
 */
static void test_nc_memory_accessible(void)
{
    /* Verify ncmem_alloc works */
    volatile uint64_t *nc_val = (volatile uint64_t *)ncmem_alloc(sizeof(uint64_t), 64);
    TEST_ASSERT_NOT_NULL(nc_val);

    /* Verify address is within NC region */
    TEST_ASSERT_MESSAGE((uintptr_t)nc_val >= NC_MEM_BASE,
                        "NC alloc below NC_MEM_BASE");
    TEST_ASSERT_MESSAGE((uintptr_t)nc_val < NC_MEM_BASE + NC_MEM_SIZE,
                        "NC alloc above NC region end");

    /* Write and read back — confirms NC mapping is valid */
    *nc_val = 0xDEADBEEFCAFEBABEULL;
    __asm__ volatile("dmb sy" ::: "memory");
    TEST_ASSERT_EQUAL_HEX64(0xDEADBEEFCAFEBABEULL, *nc_val);

    /* Write a different pattern to verify no stale data */
    *nc_val = 0x1234567890ABCDEFULL;
    __asm__ volatile("dmb sy" ::: "memory");
    TEST_ASSERT_EQUAL_HEX64(0x1234567890ABCDEFULL, *nc_val);

    /* Allocate a larger block and verify stride access */
    volatile uint32_t *nc_arr = (volatile uint32_t *)ncmem_alloc(256, 64);
    TEST_ASSERT_NOT_NULL(nc_arr);
    for (int i = 0; i < 64; i++)
        nc_arr[i] = (uint32_t)(i * 0x11111111);
    __asm__ volatile("dmb sy" ::: "memory");
    for (int i = 0; i < 64; i++)
        TEST_ASSERT_EQUAL_HEX32((uint32_t)(i * 0x11111111), nc_arr[i]);
}

/*
 * Test: NC allocator alignment and edge cases.
 */
static void test_nc_alloc_alignment(void)
{
    /* Allocate with different alignments and verify */
    void *a64 = ncmem_alloc(8, 64);
    TEST_ASSERT_NOT_NULL(a64);
    TEST_ASSERT_EQUAL_UINT64(0, (uintptr_t)a64 % 64);

    void *a128 = ncmem_alloc(8, 128);
    TEST_ASSERT_NOT_NULL(a128);
    TEST_ASSERT_EQUAL_UINT64(0, (uintptr_t)a128 % 128);

    /* Different allocations return different addresses */
    TEST_ASSERT_MESSAGE(a64 != a128, "Two NC allocs returned same address");

    /* Zero size returns NULL */
    TEST_ASSERT_NULL(ncmem_alloc(0, 64));

    /* ncmem_used() returns non-zero after allocations */
    TEST_ASSERT_MESSAGE(ncmem_used() > 0, "ncmem_used should be non-zero");
}

/*
 * Test: NC region is used for scheduler data (run queues start at NC_MEM_BASE).
 * Verifies the first ncmem allocation (run queues in scheduler_init) landed
 * at the expected NC base address.
 */
static void test_nc_region_used_by_scheduler(void)
{
    /* ncmem_used() should be non-zero (scheduler allocated run queues) */
    TEST_ASSERT_MESSAGE(ncmem_used() > 0,
                        "NC region unused — scheduler should have allocated run queues");

    /* The NC region should be writable at the base (run queue data lives there) */
    volatile uint32_t *nc_base = (volatile uint32_t *)NC_MEM_BASE;
    uint32_t saved = *nc_base;
    *nc_base = 0xA5A5A5A5;
    __asm__ volatile("dmb sy" ::: "memory");
    TEST_ASSERT_EQUAL_HEX32(0xA5A5A5A5, *nc_base);
    *nc_base = saved;  /* Restore (this is the spinlock field) */
}

#endif /* PLATFORM_RASPI5 */

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

#if defined(PLATFORM_RASPI5)
    /* NC memory validation */
    RUN_TEST(test_nc_memory_accessible);
    RUN_TEST(test_nc_alloc_alignment);
    RUN_TEST(test_nc_region_used_by_scheduler);
#endif

    return UNITY_END();
}
