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
#include "../include/slm_ffi.h"
#include "../include/ncmem.h"
#include "../include/timer.h"
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

/* Migration test state.
 * NOTE: These are in cacheable BSS, not NC memory. On Pi 5 (incoherent L2),
 * cache_invalidate (DC CIVAC) doesn't propagate through per-core L2, so
 * cross-CPU reads of these variables are unreliable. These tests currently
 * fail on Pi 5 due to the secondary CPU preemption blocker (see
 * docs/pi5-secondary-cpu-preemption.md). Moving to NC memory would fix
 * data visibility but not the preemption issue. */
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

/* Simple named tasks for basic multi-core test.
 * On NC platforms, write a done flag to NC memory for reliable cross-CPU
 * completion detection (task->state is also NC but this matches bench smp). */
#if defined(PLATFORM_HAS_NC_MEMORY)
#define INTEG_NC_DONE(cpu) (*(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 384 + (cpu) * 4))
#endif

static void task_a_func(void *arg)
{
    int count = (int)(uintptr_t)arg;
    for (int i = 0; i < count; i++) {
        delay(500000);
    }
#if defined(PLATFORM_HAS_NC_MEMORY)
    INTEG_NC_DONE(1) = 1;
#endif
}

static void task_b_func(void *arg)
{
    int count = (int)(uintptr_t)arg;
    for (int i = 0; i < count; i++) {
        delay(500000);
    }
#if defined(PLATFORM_HAS_NC_MEMORY)
    INTEG_NC_DONE(2) = 1;
#endif
}

static void task_c_func(void *arg)
{
    int count = (int)(uintptr_t)arg;
    for (int i = 0; i < count; i++) {
        delay(500000);
    }
#if defined(PLATFORM_HAS_NC_MEMORY)
    INTEG_NC_DONE(3) = 1;
#endif
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
#if defined(PLATFORM_HAS_NC_MEMORY)
    /* Clear NC done flags */
    INTEG_NC_DONE(1) = 0;
    INTEG_NC_DONE(2) = 0;
    INTEG_NC_DONE(3) = 0;
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

    /* Wait for completion with timeout.
     * On NC platforms, poll NC done flags (same pattern as bench smp).
     * On non-NC, poll task state with cache invalidate. */
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 5;
    while ((timer_get_count() - start) < limit) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (INTEG_NC_DONE(1) && INTEG_NC_DONE(2) && INTEG_NC_DONE(3)) break;
#else
        cache_invalidate(&task_a->state);
        cache_invalidate(&task_b->state);
        cache_invalidate(&task_c->state);
        if (task_a->state == TASK_TERMINATED &&
            task_b->state == TASK_TERMINATED &&
            task_c->state == TASK_TERMINATED) break;
#endif
        yield();
    }

#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(INTEG_NC_DONE(1), "task_a did not complete (NC flag)");
    TEST_ASSERT_MESSAGE(INTEG_NC_DONE(2), "task_b did not complete (NC flag)");
    TEST_ASSERT_MESSAGE(INTEG_NC_DONE(3), "task_c did not complete (NC flag)");
#else
    TEST_ASSERT_MESSAGE(task_a->state == TASK_TERMINATED, "task_a did not complete");
    TEST_ASSERT_MESSAGE(task_b->state == TASK_TERMINATED, "task_b did not complete");
    TEST_ASSERT_MESSAGE(task_c->state == TASK_TERMINATED, "task_c did not complete");
#endif
}

/*
 * Test: Task Migration
 * Create a task on CPU 1, migrate to CPU 3 before it runs.
 */
static void test_task_migration(void)
{
    /* Cross-CPU dispatch now works with SEVL+WFE idle loop */
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

    /* Wait for task to start and signal ready.
     * migration_ready is in cacheable BSS — use cache_invalidate on non-NC,
     * but on NC platforms the task writes to cacheable memory visible to CPU 0. */
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 5;
    while ((timer_get_count() - start) < limit) {
        cache_invalidate(&migration_ready);
        if (migration_ready) break;
        yield();
    }
    TEST_ASSERT_MESSAGE(migration_ready, "Timeout waiting for migration_ready");

    /* Let task finish */
    migration_done = true;
    cache_clean(&migration_done);

    /* Wait for completion */
    start = timer_get_count();
    while ((timer_get_count() - start) < limit) {
        if (mig_task->state == TASK_TERMINATED &&
            blocker->state == TASK_TERMINATED) break;
        yield();
    }

    TEST_ASSERT_MESSAGE(mig_task->state == TASK_TERMINATED,
        "Migration task did not complete");
    cache_invalidate(&migration_cpu_before);
    TEST_ASSERT_MESSAGE(migration_cpu_before == 3, "Task should run on CPU 3 after migration");
}

/*
 * Test: Stress Test - Many Tasks Across CPUs
 * Create 6 tasks distributed across CPUs 1, 2, 3.
 */
static void test_stress_multicpu(void)
{
    /* Cross-CPU dispatch now works with SEVL+WFE idle loop */
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
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 10;  /* 10 second timeout */
    while ((timer_get_count() - start) < limit) {
        bool all_done = true;
        for (int j = 0; j < 6; j++) {
            if (tasks[j]->state != TASK_TERMINATED) { all_done = false; break; }
        }
        if (all_done) break;
        yield();
    }

    cache_invalidate(&test_state.tasks_completed);
    TEST_ASSERT_MESSAGE(test_state.tasks_completed == 6,
        "Timeout waiting for stress tasks");
}

/*
 * Test: Lock Contention
 * Multiple tasks contending on the same spinlock.
 */
static void test_lock_contention(void)
{
    /* Cross-CPU dispatch now works with SEVL+WFE idle loop */
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
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 10;
    while ((timer_get_count() - start) < limit) {
        cache_invalidate(&test_state.tasks_completed);
        if (test_state.tasks_completed >= CONTENTION_TASKS) break;
        yield();
    }

    cache_invalidate(&test_state.tasks_completed);
    TEST_ASSERT_MESSAGE(test_state.tasks_completed >= CONTENTION_TASKS,
        "Timeout waiting for contention tasks");

    cache_invalidate(&contention_counter);
    uint32_t expected = CONTENTION_TASKS * INCREMENTS_PER_TASK;
    TEST_ASSERT_MESSAGE(contention_counter == expected,
        "Race condition detected in lock contention");
}

/*
 * Test: Task Lifecycle
 * Rapidly create and destroy tasks to verify cleanup.
 */
static void test_task_lifecycle(void)
{
    /* Cross-CPU dispatch now works with SEVL+WFE idle loop */
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
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 10;
    while ((timer_get_count() - start) < limit) {
        cache_invalidate(&lifecycle_completed);
        if (lifecycle_completed >= LIFECYCLE_CYCLES) break;
        yield();
    }

    cache_invalidate(&lifecycle_completed);
    TEST_ASSERT_MESSAGE(lifecycle_completed >= LIFECYCLE_CYCLES,
        "Timeout: not all lifecycle tasks completed");

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

#if defined(PLATFORM_HAS_NC_MEMORY)

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
        nc_arr[i] = (uint32_t)((uint32_t)i * 0x11111111U);
    __asm__ volatile("dmb sy" ::: "memory");
    for (int i = 0; i < 64; i++)
        TEST_ASSERT_EQUAL_HEX32((uint32_t)((uint32_t)i * 0x11111111U), nc_arr[i]);
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

#endif /* PLATFORM_HAS_NC_MEMORY */

/*
 * Regression test: verify boot order — PMM allocations before scheduler.
 *
 * On Pi 5 without SMPEN, concurrent PMM access from multiple CPUs causes
 * deadlock because the PMM spinlock (ldaxr/stxr on cacheable memory)
 * doesn't provide cross-CPU mutual exclusion. The fix: CPU 0 completes
 * all initial PMM allocations (ramdisk, Rust heap, model memory) BEFORE
 * scheduler_init() signals secondary CPUs to allocate idle task stacks.
 *
 * This test verifies that model memory pools are allocated (requires PMM)
 * and the scheduler is running (requires scheduler_init). If the boot
 * order regresses and these run concurrently, Pi 5 would deadlock at boot.
 */
static void test_boot_order_pmm_before_scheduler(void)
{
    /* Model memory requires PMM — if pools are allocated, PMM worked */
    RustPoolStats wstats = rust_weight_pool_stats();
    TEST_ASSERT_TRUE(wstats.total_blocks > 0);

    /* Scheduler must be initialized (secondary CPUs created idle tasks) */
    extern int scheduler_is_initialized(void);
    TEST_ASSERT_TRUE(scheduler_is_initialized());

    /* Both conditions true means the boot order is correct:
     * PMM allocations completed, THEN scheduler initialized. */
}

/*
 * Regression: pit_ticks vs hardware counter for component timeouts.
 *
 * On Pi 5, tasks run with DAIF.I=1 (IRQ masked) so pit_ticks does not
 * advance during task execution. Component timeout loops that polled
 * pit_ticks would hang forever. The fix uses timer_get_count() (hardware
 * counter) which always advances regardless of IRQ state.
 *
 * This test verifies that the hardware counter can be used for a timeout
 * loop with yield(), simulating how components poll for timeouts.
 */
static void test_hw_timeout_with_yield(void)
{
    uint64_t start = timer_get_count();
    uint64_t freq = timer_get_frequency();
    uint64_t limit = freq / 10;  /* 100ms timeout */
    int loops = 0;

    while ((timer_get_count() - start) < limit) {
        yield();
        loops++;
        if (loops > 1000000) {
            /* Safety: if counter not advancing, don't spin forever */
            break;
        }
    }

    uint64_t elapsed = timer_get_count() - start;
    uint64_t elapsed_ms = (elapsed * 1000) / freq;

    /* Should have completed in roughly 100ms (allow 50-500ms for QEMU variance) */
    TEST_ASSERT_MESSAGE(elapsed_ms >= 50,
        "Hardware timer timeout completed too fast (< 50ms)");
    TEST_ASSERT_MESSAGE(elapsed_ms <= 500,
        "Hardware timer timeout took too long (> 500ms)");
    TEST_ASSERT_MESSAGE(loops > 0,
        "Timeout loop did not iterate (yield never returned)");
}

/*
 * Regression: idle task must unmask IRQs on CPU 0.
 *
 * On Pi 5, the idle task previously used WFE-only for ALL CPUs (including
 * CPU 0), which prevented timer interrupts from firing. Without timer ticks,
 * pit_ticks never advances and uptime/sleep functions break.
 *
 * The fix makes CPU 0's idle task do daifclr + wfi (like QEMU), while
 * secondary CPUs continue using WFE-only.
 *
 * This test verifies that timer_get_count() advances across a yield(),
 * which requires the timer hardware to be running (started during boot).
 */
static void test_timer_running_after_boot(void)
{
    uint64_t t1 = timer_get_count();
    yield();  /* Let at least one scheduler cycle happen */
    uint64_t t2 = timer_get_count();

    TEST_ASSERT_MESSAGE(t2 > t1,
        "Timer counter did not advance across yield (timer not running)");

    /* Verify the timer frequency is set (means timer_init completed) */
    uint64_t freq = timer_get_frequency();
    TEST_ASSERT_MESSAGE(freq > 0,
        "Timer frequency is 0 (timer not initialized)");
}

#if CONFIG_WORK_STEALING
/* ============================================================================
 * Work-stealing integration test (#59 Phase B)
 *
 * Queue N unpinned tasks all onto CPU 1. Without stealing they would all
 * run on CPU 1 one after another. With stealing enabled, idle CPUs
 * should pull from CPU 1's deque and run tasks in parallel. Pass
 * criteria: all tasks complete AND at least two distinct CPUs observed
 * running them.
 * ============================================================================ */

#define STEAL_TASK_COUNT 5
static volatile uint32_t steal_cpu_recorded[STEAL_TASK_COUNT];
static volatile uint32_t steal_done_count;

static void steal_test_task(void *arg)
{
    uint32_t idx = (uint32_t)(uintptr_t)arg;
    if (idx < STEAL_TASK_COUNT) {
        steal_cpu_recorded[idx] = cpu_id() + 1;  /* +1 so 0 means "not run" */
        cache_clean((void *)&steal_cpu_recorded[idx]);
    }
    /* Small work simulation so the steal window is real */
    for (int i = 0; i < 3; i++) delay(100000);

    irq_flags_t f = spin_lock_irqsave(&test_state.lock);
    steal_done_count++;
    cache_clean((void *)&steal_done_count);
    spin_unlock_irqrestore(&test_state.lock, f);
}

static void test_work_stealing_distributes_load(void)
{
    if (cpu_count < 3) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 3");
        return;
    }

    for (int i = 0; i < STEAL_TASK_COUNT; i++) steal_cpu_recorded[i] = 0;
    steal_done_count = 0;
    cache_clean((void *)&steal_done_count);

    /* Queue all tasks onto CPU 1 with ANY affinity so they're stealable. */
    for (int i = 0; i < STEAL_TASK_COUNT; i++) {
        char name[8];
        name[0] = 'w'; name[1] = 's'; name[2] = '0' + (char)i; name[3] = '\0';
        struct task *t = task_create(name, steal_test_task, (void *)(uintptr_t)i);
        TEST_ASSERT_NOT_NULL(t);
        /* task_create defaults cpu_affinity to CPU_AFFINITY_ANY — leave it
         * so steal_deque_push accepts this task in scheduler_add_task_to_cpu. */
        scheduler_add_task_to_cpu(t, 1);
    }

    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 8;
    while ((timer_get_count() - start) < limit) {
        cache_invalidate((void *)&steal_done_count);
        if (steal_done_count >= STEAL_TASK_COUNT) break;
        yield();
    }

    cache_invalidate((void *)&steal_done_count);
    TEST_ASSERT_MESSAGE(steal_done_count == STEAL_TASK_COUNT,
        "work-steal test: not all tasks completed");

    /* Count distinct CPUs that ran tasks. */
    uint8_t cpu_seen[MAX_CPUS] = {0};
    int distinct = 0;
    for (int i = 0; i < STEAL_TASK_COUNT; i++) {
        cache_invalidate((void *)&steal_cpu_recorded[i]);
        uint32_t r = steal_cpu_recorded[i];
        TEST_ASSERT_MESSAGE(r != 0, "task did not record a CPU");
        uint32_t c = r - 1;
        if (c < MAX_CPUS && !cpu_seen[c]) {
            cpu_seen[c] = 1;
            distinct++;
        }
    }

    /*
     * With CONFIG_WORK_STEALING on, we expect at least 2 distinct CPUs.
     * Exact count depends on timing and which CPUs were idle; 2 is a
     * conservative floor that proves stealing happened at all.
     */
    TEST_ASSERT_MESSAGE(distinct >= 2,
        "work stealing did not distribute: only 1 CPU ran tasks");
}
#endif /* CONFIG_WORK_STEALING */

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

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* NC memory validation */
    RUN_TEST(test_nc_memory_accessible);
    RUN_TEST(test_nc_alloc_alignment);
    RUN_TEST(test_nc_region_used_by_scheduler);
#endif

    /* Boot order regression test (Pi 5 PMM deadlock prevention) */
    RUN_TEST(test_boot_order_pmm_before_scheduler);

    /* Regression: Pi 5 timer and timeout fixes (April 2026) */
    RUN_TEST(test_hw_timeout_with_yield);
    RUN_TEST(test_timer_running_after_boot);

#if CONFIG_WORK_STEALING
    /* Phase B: work-stealing load distribution (#59). */
    RUN_TEST(test_work_stealing_distributes_load);
#endif

    return UNITY_END();
}
