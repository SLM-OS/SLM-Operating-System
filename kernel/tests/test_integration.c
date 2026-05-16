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
#include "../include/preempt_point.h"
#include "../include/cpu_supervisor.h"
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Test Infrastructure
 * ============================================================================ */

/*
 * Busy-wait for multi-core integration tests.
 *
 * Calls slm_preempt_point() on every iteration:
 *   - On COOP_PREEMPT platforms (Pi 5, Jetson) the macro checks
 *     CNTPCT_EL0 and reschedules only when the per-CPU 10 ms quantum
 *     has elapsed. Lets the scheduler migrate, priority-boost, or
 *     observe progress on another CPU.
 *   - On platforms with hardware timer IRQs (QEMU, x86-64) the macro
 *     expands to a no-op so the original timing behavior is unchanged.
 *
 * Compared to the previous fixed 1024-iteration yield, the macro
 * doesn't churn unnecessarily inside short delay() calls — one
 * reschedule per quantum, not per loop budget. See
 * kernel/include/preempt_point.h.
 */
static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile("nop");
        slm_preempt_point();
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

/*
 * NC test sync slots: cross-CPU signaling via non-cacheable memory.
 *
 * On Pi 5 (no SMPEN), cache_invalidate (DC CIVAC) doesn't propagate through
 * per-core L2. Cacheable BSS variables written by one CPU are invisible to
 * others. NC memory bypasses L1/L2 entirely — writes are instantly visible.
 *
 * NC_SYNC region runs from NC_MEM_SIZE - 768 up to INTEG_NC_DONE at
 * NC_MEM_SIZE - 384, so 384 bytes / 4 = 96 uint32_t slots are available
 * (slot 0..95). Slots 0..30 are currently in use (high-water mark below;
 * update when adding new slot families). Each test zeroes its slots
 * before use; tasks write, CPU 0 polls.
 */
#if defined(PLATFORM_HAS_NC_MEMORY)
#define NC_SYNC_BASE    (NC_MEM_BASE + NC_MEM_SIZE - 768)
#define NC_SYNC(n)      (*(volatile uint32_t *)(NC_SYNC_BASE + (n) * 4))

/* Slot assignments */
#define NC_TASKS_COMPLETED    0
#define NC_CONTENTION_COUNTER 1
#define NC_LIFECYCLE_DONE     2
#define NC_MIG_READY          3
#define NC_MIG_DONE           4
#define NC_MIG_CPU_BEFORE     5
#define NC_MIG_CPU_AFTER      6
#define NC_MSG_RECEIVED       7
#define NC_MSG_PUBLISHED      8
#define NC_MSG_SUB_DONE       9
#define NC_MSG_PUB_DONE      10
#define NC_NOTIFY_SENTINEL   11
#define NC_STEAL_DONE        12
#define NC_STEAL_CPU_BASE    13  /* 13-17: steal_cpu_recorded[0-4] */
/* msg_router #67a: multi-subscriber + priority ordering stress tests. */
#define NC_MSGMS_A_RECEIVED  18
#define NC_MSGMS_B_RECEIVED  19
#define NC_MSGMS_PUB_DONE    20
#define NC_MSGMS_SUB_A_DONE  21
#define NC_MSGMS_SUB_B_DONE  22
#define NC_MSGMS_DELIVERED   23  /* sum of every publish's delivered count */
#define NC_MSGPR_HI_RECEIVED 24
#define NC_MSGPR_LO_RECEIVED 25
#define NC_MSGPR_PUB_HI_DONE 26
#define NC_MSGPR_PUB_LO_DONE 27
#define NC_MSGPR_SUB_DONE    28
#define NC_MSGPR_HI_DELIVERED 29
#define NC_MSGPR_LO_DELIVERED 30
/* msg_router #67c: cross-CPU LAST_RECEIVED ack-targeting test. */
#define NC_MSGAR_TRIGGER     31  /* main → helper: publish /b now */
#define NC_MSGAR_HELPER_DONE 32  /* helper → main: publish complete */
/* msg_router #869: same-mailbox concurrent-publisher zero-loss test. */
#define NC_MSGSM_A_DELIVERED 33  /* pub_a's total delivered count */
#define NC_MSGSM_B_DELIVERED 34  /* pub_b's total delivered count */
#define NC_MSGSM_SUB_A_SEEN  35  /* subscriber's count of A-payloads */
#define NC_MSGSM_SUB_B_SEEN  36  /* subscriber's count of B-payloads */
#define NC_MSGSM_PUB_A_DONE  37
#define NC_MSGSM_PUB_B_DONE  38
#define NC_MSGSM_SUB_DONE    39

static void nc_sync_clear(uint32_t start, uint32_t count)
{
    for (uint32_t i = start; i < start + count; i++)
        NC_SYNC(i) = 0;
}
#endif

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
 * Per-test idle-loop snapshots (#216 dormancy investigation, Pi 5 only)
 *
 * Hooks into Unity's setUp / tearDown to capture sched_diag_idle_loops[]
 * before and after every test in this suite. A secondary CPU whose
 * delta is zero across a whole test is dormant during that test — and
 * since tests run sequentially, the FIRST test whose tearDown reports
 * a zero delta is the test that wedged the CPU. This is what Option A
 * of the #216 investigation plan needs: pin the culprit test.
 *
 * setUp / tearDown are weak in Unity; defining them here overrides the
 * weak defaults for the whole kernel-test binary. The output is quiet
 * by default and only prints when a secondary is actually dormant, so
 * other test suites (ipc, vmm, net, etc. — none of which spawn cross-
 * CPU work) are unaffected.
 * ============================================================================ */

#if defined(PLATFORM_RASPI5)
extern volatile uint32_t *sched_diag_idle_loops;

/* A "dormant" CPU is one whose idle_loops counter hasn't moved
 * across THIS many consecutive tearDowns. Single tests can finish
 * in <100us (far below one idle loop period on a WFE'd secondary),
 * so treating a 1-sample stall as dormancy false-positives on fast
 * tests. Three consecutive stalled samples (covering ~3x the
 * average test duration) is the sweet spot — rare enough that an
 * alive CPU won't trip it, frequent enough to pin the culprit
 * test within a window of ~3 tests. */
#define DORMANCY_STALL_THRESHOLD 3

static uint32_t last_idle_abs[MAX_CPUS];
static uint32_t stall_streak[MAX_CPUS];
static bool     dormancy_reported[MAX_CPUS];
#endif

/* Unity's weak default setUp is fine — all sampling happens in
 * tearDown, so we don't need to override the entry hook. */

/* Forward decl: defined in test_lua.c. Closes any lua_State left in
 * test_lua's per-test registry (issue #374). On non-Lua suites the
 * registry is always empty, so this is a no-op. */
extern void test_lua_teardown_reclaim(void);

void tearDown(void)
{
    /* Reclaim any lua_State that escaped its test (#374). Runs on
     * every platform — Lua suite is platform-neutral. */
    test_lua_teardown_reclaim();

#if defined(PLATFORM_RASPI5)
    if (!Unity.current_test) return;

    uint32_t n = cpu_count < MAX_CPUS ? cpu_count : MAX_CPUS;

    /* Snapshot every secondary's absolute idle counter once. */
    uint32_t cur[MAX_CPUS] = {0};
    for (uint32_t c = 1; c < n; c++) {
        cur[c] = sched_diag_idle_loops[c];
    }

    /* Update per-CPU stall streak; emit a [dormancy-enter] line the
     * moment a CPU first crosses the threshold. This pinpoints the
     * test that wedged the CPU (or at least the test window inside
     * which the wedge happened). A follow-up [dormancy-recover]
     * line is emitted if the CPU later resumes. */
    for (uint32_t c = 1; c < n; c++) {
        if (cur[c] == last_idle_abs[c]) {
            stall_streak[c]++;
        } else {
            if (dormancy_reported[c] && stall_streak[c] >= DORMANCY_STALL_THRESHOLD) {
                uart_printf("  [dormancy-recover] CPU %lu resumed at %s "
                            "(abs %lu after %lu stalled samples)\n",
                            (unsigned long)c, Unity.current_test,
                            (unsigned long)cur[c],
                            (unsigned long)stall_streak[c]);
                dormancy_reported[c] = false;
            }
            stall_streak[c] = 0;
        }
        last_idle_abs[c] = cur[c];

        if (!dormancy_reported[c] &&
            stall_streak[c] == DORMANCY_STALL_THRESHOLD) {
            uart_printf("  [dormancy-enter] CPU %lu dormant at %s "
                        "(idle_abs frozen at %lu for %u consecutive tests)\n",
                        (unsigned long)c, Unity.current_test,
                        (unsigned long)cur[c],
                        DORMANCY_STALL_THRESHOLD);
            dormancy_reported[c] = true;

            /* #216 Tier 3: drive recovery from the test harness.
             *
             * The Tier 2 supervisor task (kernel/sched/cpu_supervisor.c)
             * is gated on !ENABLE_BOOT_TESTS so its CPU 0 background
             * task doesn't perturb scheduler-test ready-counts. That
             * gating means the auto-recovery path is OFF in the test
             * kernel — exactly when dormancy is most likely.
             *
             * Compromise: the test harness invokes
             * cpu_supervisor_resurrect() inline at the dormancy-enter
             * threshold. The recovery path runs on CPU 0 (same as the
             * supervisor task would) and lets the next test see a
             * working CPU. Tasks already queued on the dormant CPU
             * are leaked — the same trade-off the Tier 2 supervisor
             * makes — and the diagnostic uart_printf above pins the
             * culprit test before recovery scrubs the queue. */
            int rc = cpu_supervisor_resurrect(c);
            uart_printf("  [dormancy-recover-attempt] CPU %lu rc=%d\n",
                        (unsigned long)c, rc);
        }
    }
#endif
}

/* ============================================================================
 * Task Functions for Tests
 * ============================================================================ */

/* Migration test state.
 * On NC platforms, use NC sync slots for cross-CPU visibility.
 * On coherent platforms, use cacheable BSS with cache_clean/invalidate. */
static volatile bool migration_ready = false;
static volatile bool migration_done = false;
static volatile uint32_t migration_cpu_before = 0;
static volatile uint32_t migration_cpu_after = 0;

static void migration_test_task(void *arg)
{
    (void)arg;

    uint32_t my_cpu = cpu_id();
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MIG_CPU_BEFORE) = my_cpu;
    NC_SYNC(NC_MIG_READY) = 1;
#else
    migration_cpu_before = my_cpu;
    cache_clean(&migration_cpu_before);
    migration_ready = true;
    cache_clean(&migration_ready);
#endif

    /* Wait for migration to complete */
    while (1) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_MIG_DONE)) break;
#else
        cache_invalidate(&migration_done);
        if (migration_done) break;
#endif
        delay(10000);
        yield();
    }

#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MIG_CPU_AFTER) = cpu_id();
#else
    migration_cpu_after = cpu_id();
    cache_clean(&migration_cpu_after);
#endif

    /* Do some work to prove we're running */
    for (int i = 0; i < 3; i++) {
        delay(200000);
    }

    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed++;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_TASKS_COMPLETED) = test_state.tasks_completed;
#else
    cache_clean(&test_state.tasks_completed);
#endif
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
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_TASKS_COMPLETED) = test_state.tasks_completed;
#else
    cache_clean(&test_state.tasks_completed);
#endif
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
#if defined(PLATFORM_HAS_NC_MEMORY)
        NC_SYNC(NC_CONTENTION_COUNTER) = contention_counter;
#else
        cache_clean(&contention_counter);
#endif
        spin_unlock_irqrestore(&contention_lock, flags);
        delay(1000);
    }

    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed++;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_TASKS_COMPLETED) = test_state.tasks_completed;
#else
    cache_clean(&test_state.tasks_completed);
#endif
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
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_LIFECYCLE_DONE) = lifecycle_completed;
#else
    cache_clean(&lifecycle_completed);
#endif
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
#if defined(PLATFORM_HAS_NC_MEMORY)
    nc_sync_clear(NC_MIG_READY, 4);  /* Clear MIG_READY through MIG_CPU_AFTER */
    nc_sync_clear(NC_TASKS_COMPLETED, 1);
    INTEG_NC_DONE(1) = 0;
    INTEG_NC_DONE(3) = 0;
#endif

    struct task *mig_task = task_create("migrate", migration_test_task, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(mig_task, "Failed to create migration task");

    /* Create blocker to occupy CPU 1.
     *
     * Pin blocker to CPU 1 (affinity=1, not ANY) so work-stealing does
     * NOT pull it to another CPU. Without this, bench-stealing-style
     * aggressive stealing would relocate the blocker to CPU 2 or 3
     * during the delay(50000) below, and mig_task would be picked up
     * by CPU 1 (now free) and start running before sched_migrate_task
     * could inspect it — sched_migrate_task would reject the migration
     * with "task is running" and the test would fail on the ret==0
     * assert. Pinning keeps the test scenario intact: blocker owns
     * CPU 1, mig_task queues behind it, migration moves mig_task to
     * CPU 3. */
    struct task *blocker = task_create("blocker", task_a_func, (void *)10);
    TEST_ASSERT_NOT_NULL_MESSAGE(blocker, "Failed to create blocker task");
    blocker->cpu_affinity = 1;

    /* Add blocker first, then migration task */
    scheduler_add_task_to_cpu(blocker, 1);
    delay(50000);
    scheduler_add_task_to_cpu(mig_task, 1);

    /* Migrate while blocker is running */
    int ret = sched_migrate_task(mig_task, 3);
    TEST_ASSERT_MESSAGE(ret == 0, "sched_migrate_task failed");

    /* Wait for task to start and signal ready.
     *
     * 15-second timeout on Pi 5 because the counter component from
     * the Lua test suite keeps running for ~5 seconds into integration
     * tests, and if it lands on target_cpu (3) the migrated task has
     * to wait behind it. The shorter 5-second budget raced with the
     * counter's lifetime and flaked. */
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 15;
    while ((timer_get_count() - start) < limit) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_MIG_READY)) break;
#else
        cache_invalidate(&migration_ready);
        if (migration_ready) break;
#endif
        yield();
    }
#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MIG_READY), "Timeout waiting for migration_ready");
#else
    TEST_ASSERT_MESSAGE(migration_ready, "Timeout waiting for migration_ready");
#endif

    /* Let task finish */
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MIG_DONE) = 1;
#else
    migration_done = true;
    cache_clean(&migration_done);
#endif

    /* Wait for completion — use NC task state (task_table is NC on Pi 5) */
    start = timer_get_count();
    while ((timer_get_count() - start) < limit) {
        if (mig_task->state == TASK_TERMINATED &&
            blocker->state == TASK_TERMINATED) break;
        yield();
    }

    TEST_ASSERT_MESSAGE(mig_task->state == TASK_TERMINATED,
        "Migration task did not complete");
#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MIG_CPU_BEFORE) == 3,
        "Task should run on CPU 3 after migration");
#else
    cache_invalidate(&migration_cpu_before);
    TEST_ASSERT_MESSAGE(migration_cpu_before == 3, "Task should run on CPU 3 after migration");
#endif
}

/*
 * Test: Stress Test - Many Tasks Across CPUs
 * Create 6 tasks distributed across CPUs 1, 2, 3.
 */
static void test_stress_multicpu(void)
{
    /* Cross-CPU dispatch now works with SEVL+WFE idle loop */
    reset_test_state();
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_TASKS_COMPLETED) = 0;
#endif

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
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_TASKS_COMPLETED) >= 6) break;
#else
        bool all_done = true;
        for (int j = 0; j < 6; j++) {
            if (tasks[j]->state != TASK_TERMINATED) { all_done = false; break; }
        }
        if (all_done) break;
#endif
        yield();
    }

#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_TASKS_COMPLETED) >= 6,
        "Timeout waiting for stress tasks");
#else
    cache_invalidate(&test_state.tasks_completed);
    TEST_ASSERT_MESSAGE(test_state.tasks_completed == 6,
        "Timeout waiting for stress tasks");
#endif
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
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_TASKS_COMPLETED) = 0;
    NC_SYNC(NC_CONTENTION_COUNTER) = 0;
#endif

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
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_TASKS_COMPLETED) >= CONTENTION_TASKS) break;
#else
        cache_invalidate(&test_state.tasks_completed);
        if (test_state.tasks_completed >= CONTENTION_TASKS) break;
#endif
        yield();
    }

#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_TASKS_COMPLETED) >= CONTENTION_TASKS,
        "Timeout waiting for contention tasks");
    uint32_t expected = CONTENTION_TASKS * INCREMENTS_PER_TASK;
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_CONTENTION_COUNTER) == expected,
        "Race condition detected in lock contention");
#else
    cache_invalidate(&test_state.tasks_completed);
    TEST_ASSERT_MESSAGE(test_state.tasks_completed >= CONTENTION_TASKS,
        "Timeout waiting for contention tasks");
    cache_invalidate(&contention_counter);
    uint32_t expected = CONTENTION_TASKS * INCREMENTS_PER_TASK;
    TEST_ASSERT_MESSAGE(contention_counter == expected,
        "Race condition detected in lock contention");
#endif
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
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_LIFECYCLE_DONE) = 0;
#endif

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
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_LIFECYCLE_DONE) >= LIFECYCLE_CYCLES) break;
#else
        cache_invalidate(&lifecycle_completed);
        if (lifecycle_completed >= LIFECYCLE_CYCLES) break;
#endif
        yield();
    }

#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_LIFECYCLE_DONE) >= LIFECYCLE_CYCLES,
        "Timeout: not all lifecycle tasks completed");
#else
    cache_invalidate(&lifecycle_completed);
    TEST_ASSERT_MESSAGE(lifecycle_completed >= LIFECYCLE_CYCLES,
        "Timeout: not all lifecycle tasks completed");
#endif

    /*
     * Give scheduler time to clean up zombies. Under
     * CONFIG_WORK_STEALING=ON, lifecycle tasks get distributed
     * across CPUs 1-3 (via the dispatch above) and each CPU's
     * `schedule()` cycle is what actually frees its zombies. A
     * short sleep with only CPU 0 yielding can leave stacks
     * allocated while the owning CPUs are idle. Yield in a long
     * enough loop for every CPU to run schedule() several times
     * after the last task terminates.
     */
    for (int i = 0; i < 50; i++) {
        yield();
        delay(50000);
    }

    /*
     * Verify no major memory leak. Threshold is generous enough to
     * tolerate one full task stack (4 pages) lingering after the
     * cleanup window because its owning CPU has not run
     * schedule() again — this can happen when a secondary CPU
     * parks in WFE right after its test task exits and doesn't
     * wake until a timer tick. 16 pages (~64 KB) covers the
     * worst case of one stack per stealable CPU without hiding a
     * real per-task leak. Every `LIFECYCLE_CYCLES` tasks would
     * leak 32 pages, which this threshold would still catch.
     */
    uint64_t final_free = pmm_get_free_pages();
    TEST_ASSERT_MESSAGE(final_free >= initial_free - 16, "Memory leak detected");
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
#if defined(PLATFORM_HAS_NC_MEMORY)
        NC_SYNC(NC_STEAL_CPU_BASE + idx) = cpu_id() + 1;
#else
        cache_clean((void *)&steal_cpu_recorded[idx]);
#endif
    }
    /*
     * CPU-bound arithmetic — mirrors bench stealing's workload.
     *
     * The previous implementation used delay(100000)*3 which yields
     * under COOP_PREEMPT. Each yield calls schedule(), briefly
     * re-queuing and re-picking the task on CPU 1, so the task is
     * effectively "sticky" to whichever CPU started it and stealers
     * pick up later tasks from the deque instead. That's fine for
     * stealing in theory, but when the whole task only does ~0.3 ms
     * of work it finishes before stealers on Pi 5 (whose yield path
     * is slowed by the DC CIVAC spinlock work) can claim their share.
     *
     * A pure CPU-bound loop holds the task on its owner for long
     * enough that stealers reliably claim the other deque entries.
     */
    volatile uint64_t x = 1;
    for (uint64_t i = 1; i < 300000; i++) {
        x = x * 1103515245 + 12345;
    }
    (void)x;

    irq_flags_t f = spin_lock_irqsave(&test_state.lock);
    steal_done_count++;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_STEAL_DONE) = steal_done_count;
#else
    cache_clean((void *)&steal_done_count);
#endif
    spin_unlock_irqrestore(&test_state.lock, f);
}

static void test_work_stealing_distributes_load(void)
{
    if (cpu_count < 3) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 3");
        return;
    }

    /*
     * Success criterion: "at least one task ran on a CPU that is NOT
     * the owner (CPU 1)". That's the semantic meaning of "work
     * stealing distributes load" — stealing moved work off the
     * overloaded CPU. An earlier version required `distinct >= 2`
     * (at least two distinct CPUs ran tasks), which turned out to
     * be a weak proxy: on Pi 5, secondary CPUs are sometimes
     * dormant after boot (ws-diag shows sched_diag_schedule[c] == 0
     * for one or more CPUs during the entire test window — see
     * issue #216). When only one non-owner CPU is alive as a
     * stealer it grabs all the tasks before the other stealers
     * wake, giving distinct == 1 — but stealing still worked.
     * The new criterion is the regression we actually care about:
     * if tasks never left the owner, stealing is broken.
     *
     * Still run 8 attempts and require 2 successes to tolerate
     * one-off hiccups; we still count `distinct` as a secondary
     * diagnostic in the Pi 5 ws-diag dump.
     */
    const int ATTEMPTS = 8;
    const int REQUIRED_SUCCESSES = 2;
    const uint32_t OWNER_CPU = 1;
    int success_count = 0;

    /* #175: snapshot the per-CPU push-failure counter. STEAL_TASK_COUNT (5)
     * << STEAL_DEQUE_CAPACITY (32), so this workload should never fill
     * the deque; any delta indicates `scheduler_add_task_to_cpu` thinks
     * it couldn't push and either a workload regression (e.g. capacity
     * shrunk) or a counter-bookkeeping bug. */
#if defined(PLATFORM_HAS_NC_MEMORY)
    extern volatile uint32_t *sched_diag_steal_push_full;
#else
    extern volatile uint32_t sched_diag_steal_push_full[];
#endif
#if defined(PLATFORM_RASPI5)
    /* Per-attempt ws-diag dump only references these on Pi 5. */
    extern volatile uint32_t *sched_diag_steal_attempts;
    extern volatile uint32_t *sched_diag_steal_successes;
    extern volatile uint32_t *sched_diag_steal_stale;
    extern volatile uint32_t *sched_diag_schedule;
    extern volatile uint32_t *sched_diag_picked;
    extern volatile uint32_t *sched_diag_idle_loops;
#endif
    uint32_t pre_push_full[MAX_CPUS] = {0};
    for (uint32_t c = 0; c < cpu_count; c++)
        pre_push_full[c] = sched_diag_steal_push_full[c];

    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        for (int i = 0; i < STEAL_TASK_COUNT; i++) steal_cpu_recorded[i] = 0;
        steal_done_count = 0;
#if defined(PLATFORM_HAS_NC_MEMORY)
        NC_SYNC(NC_STEAL_DONE) = 0;
        for (int i = 0; i < STEAL_TASK_COUNT; i++)
            NC_SYNC(NC_STEAL_CPU_BASE + i) = 0;
#else
        cache_clean((void *)&steal_done_count);
#endif

        /* Flakiness diagnostic: per-attempt snapshot of steal
         * attempts/successes/stale and schedule()/picked so we
         * can tell whether stealers never tried (CPU 1 finished
         * first), tried but rejected (stale/affinity/state race),
         * or succeeded but the recording mis-counted distinct CPUs.
         * Gated to PLATFORM_RASPI5 — on QEMU the cross-CPU dispatch
         * story is well-behaved and the output would just pollute
         * `make test` logs. */
#if defined(PLATFORM_RASPI5)
        uint32_t pre_attempts[MAX_CPUS] = {0};
        uint32_t pre_successes[MAX_CPUS] = {0};
        uint32_t pre_stale[MAX_CPUS] = {0};
        uint32_t pre_schedule[MAX_CPUS] = {0};
        uint32_t pre_picked[MAX_CPUS] = {0};
        uint32_t pre_idle[MAX_CPUS] = {0};
        for (uint32_t c = 0; c < cpu_count; c++) {
            pre_attempts[c] = sched_diag_steal_attempts[c];
            pre_successes[c] = sched_diag_steal_successes[c];
            pre_stale[c] = sched_diag_steal_stale[c];
            pre_schedule[c] = sched_diag_schedule[c];
            pre_picked[c] = sched_diag_picked[c];
            pre_idle[c] = sched_diag_idle_loops[c];
        }
        uint64_t t_queued = timer_get_count();
#endif

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
        uint64_t limit = timer_get_frequency() * 4;
        while ((timer_get_count() - start) < limit) {
#if defined(PLATFORM_HAS_NC_MEMORY)
            if (NC_SYNC(NC_STEAL_DONE) >= STEAL_TASK_COUNT) break;
#else
            cache_invalidate((void *)&steal_done_count);
            if (steal_done_count >= STEAL_TASK_COUNT) break;
#endif
            yield();
        }
#if defined(PLATFORM_RASPI5)
        uint64_t t_done = timer_get_count();
#endif

#if defined(PLATFORM_HAS_NC_MEMORY)
        TEST_ASSERT_MESSAGE(NC_SYNC(NC_STEAL_DONE) == STEAL_TASK_COUNT,
            "work-steal test: not all tasks completed");
#else
        cache_invalidate((void *)&steal_done_count);
        TEST_ASSERT_MESSAGE(steal_done_count == STEAL_TASK_COUNT,
            "work-steal test: not all tasks completed");
#endif

        /* Tally per-attempt stats: distinct CPUs (diagnostic only;
         * kept in the Pi 5 ws-diag output) and whether any task
         * ran off the owner (the real success criterion). */
        uint8_t cpu_seen[MAX_CPUS] = {0};
#if defined(PLATFORM_RASPI5)
        uint32_t recorded[STEAL_TASK_COUNT];
        int distinct = 0;
#endif
        int ran_off_owner = 0;
        for (int i = 0; i < STEAL_TASK_COUNT; i++) {
#if defined(PLATFORM_HAS_NC_MEMORY)
            uint32_t r = NC_SYNC(NC_STEAL_CPU_BASE + i);
#else
            cache_invalidate((void *)&steal_cpu_recorded[i]);
            uint32_t r = steal_cpu_recorded[i];
#endif
            TEST_ASSERT_MESSAGE(r != 0, "task did not record a CPU");
#if defined(PLATFORM_RASPI5)
            recorded[i] = r;
#endif
            uint32_t c = r - 1;
            if (c < MAX_CPUS && !cpu_seen[c]) {
                cpu_seen[c] = 1;
#if defined(PLATFORM_RASPI5)
                distinct++;
#endif
            }
            if (c != OWNER_CPU) {
                ran_off_owner = 1;
            }
        }
        if (ran_off_owner) {
            success_count++;
        }

#if defined(PLATFORM_RASPI5)
        /* Dump per-attempt diagnostics on Pi 5 so boot-to-boot
         * comparisons can identify which bucket the failure falls
         * into (no attempts / all-rejected / all-succeeded-on-owner). */
        uint64_t elapsed_ticks = t_done - t_queued;
        uint64_t freq = timer_get_frequency();
        uint64_t elapsed_us = (freq > 0)
            ? (elapsed_ticks * 1000000ULL) / freq
            : 0;
        uart_printf("  ws-diag attempt=%d distinct=%d elapsed_us=%lu cpus=[",
                    attempt, distinct, (unsigned long)elapsed_us);
        for (int i = 0; i < STEAL_TASK_COUNT; i++) {
            uart_printf("%lu%s",
                        (unsigned long)(recorded[i] - 1),
                        (i + 1 < STEAL_TASK_COUNT) ? "," : "");
        }
        uart_printf("] per-cpu(att/succ/stale/sched/picked/idle)=");
        for (uint32_t c = 0; c < cpu_count; c++) {
            uint32_t da = sched_diag_steal_attempts[c] - pre_attempts[c];
            uint32_t ds = sched_diag_steal_successes[c] - pre_successes[c];
            uint32_t dst = sched_diag_steal_stale[c] - pre_stale[c];
            uint32_t dsc = sched_diag_schedule[c] - pre_schedule[c];
            uint32_t dp = sched_diag_picked[c] - pre_picked[c];
            uint32_t di = sched_diag_idle_loops[c] - pre_idle[c];
            uart_printf("%s%lu/%lu/%lu/%lu/%lu/%lu",
                        (c == 0) ? "" : ",",
                        (unsigned long)da, (unsigned long)ds,
                        (unsigned long)dst,
                        (unsigned long)dsc, (unsigned long)dp,
                        (unsigned long)di);
        }
        uart_printf("\n");
        /* Absolute idle_loops snapshot separates "CPU never woke
         * since boot" from "CPU was alive earlier but is dormant
         * now". Zero here = CPU never executed its idle task. */
        uart_printf("  ws-diag idle_abs=[");
        for (uint32_t c = 0; c < cpu_count; c++) {
            uart_printf("%s%lu",
                        (c == 0) ? "" : ",",
                        (unsigned long)sched_diag_idle_loops[c]);
        }
        uart_printf("]\n");
#endif
    }

    TEST_ASSERT_MESSAGE(success_count >= REQUIRED_SUCCESSES,
        "work stealing never moved tasks off the owner CPU in 8 attempts");

    /* #175: push-full counter must not advance for this workload. */
    for (uint32_t c = 0; c < cpu_count; c++) {
        uint32_t delta = sched_diag_steal_push_full[c] - pre_push_full[c];
        TEST_ASSERT_MESSAGE(delta == 0,
            "steal_deque push overflowed during distributes-load test");
    }
}
#endif /* CONFIG_WORK_STEALING */

/* ============================================================================
 * Regression: msg_router cross-CPU publish/receive/ack (#66)
 *
 * Runs a publisher on one CPU and a subscriber on another. Verifies that
 * N publish/receive/ack round-trips complete correctly under concurrent
 * access. Also exercises the IRQ-safe SpinGuard in runtime/src/msg_router.rs
 * — previous guard did not mask IRQs, allowing a timer ISR to reenter the
 * router on a CPU that already held MSG_ROUTER_LOCK.
 * ============================================================================ */

extern void msg_router_init(void);
extern int msg_router_subscribe(const uint8_t *topic_name, int component_idx);
extern int msg_router_publish(const uint8_t *topic_name, const uint8_t *data);
extern int msg_router_publish_priority(const uint8_t *topic_name,
                                       const uint8_t *data, uint8_t priority);
extern const uint8_t *msg_router_receive(int component_idx, uint8_t *topic_out);
extern void msg_router_ack(int component_idx);
extern void msg_router_unsubscribe_all(int component_idx);

#define MSG_X_COMPONENT 17
#define MSG_X_MESSAGES  5
#define MSG_X_TOPIC     ((const uint8_t *)"/regress/cpu")

static volatile uint32_t msg_x_received = 0;
static volatile uint32_t msg_x_published = 0;
static volatile uint32_t msg_x_subscriber_done = 0;
static volatile uint32_t msg_x_publisher_done = 0;

static void msg_x_subscriber(void *arg)
{
    (void)arg;
    uint8_t topic_buf[16];
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 5;

    while (msg_x_received < MSG_X_MESSAGES) {
        if ((timer_get_count() - start) >= limit) break;
        const uint8_t *data = msg_router_receive(MSG_X_COMPONENT, topic_buf);
        if (data) {
            msg_x_received++;
#if defined(PLATFORM_HAS_NC_MEMORY)
            NC_SYNC(NC_MSG_RECEIVED) = msg_x_received;
#else
            cache_clean((void *)&msg_x_received);
#endif
            msg_router_ack(MSG_X_COMPONENT);
        } else {
            yield();
        }
    }

    msg_x_subscriber_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSG_SUB_DONE) = 1;
#else
    cache_clean((void *)&msg_x_subscriber_done);
#endif
}

static void msg_x_publisher(void *arg)
{
    (void)arg;
    /* Small delay so subscriber is scheduled first. */
    delay(50000);

    for (uint32_t i = 0; i < MSG_X_MESSAGES; i++) {
        uint8_t payload[4] = { (uint8_t)('a' + i), 0, 0, 0 };
        int delivered = msg_router_publish(MSG_X_TOPIC, payload);
        if (delivered == 1) {
            msg_x_published++;
#if defined(PLATFORM_HAS_NC_MEMORY)
            NC_SYNC(NC_MSG_PUBLISHED) = msg_x_published;
#else
            cache_clean((void *)&msg_x_published);
#endif
        }
    }

    msg_x_publisher_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSG_PUB_DONE) = 1;
#else
    cache_clean((void *)&msg_x_publisher_done);
#endif
}

static void test_msg_router_cross_cpu(void)
{
    /* Skip if we don't have at least 3 CPUs (shell on 0, pub on 1, sub on 2). */
    if (cpu_count < 3) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 3");
        return;
    }

    msg_router_init();
    msg_x_received = 0;
    msg_x_published = 0;
    msg_x_subscriber_done = 0;
    msg_x_publisher_done = 0;
#if defined(PLATFORM_HAS_NC_MEMORY)
    nc_sync_clear(NC_MSG_RECEIVED, 4);  /* Clear MSG slots */
#else
    cache_clean((void *)&msg_x_received);
    cache_clean((void *)&msg_x_published);
    cache_clean((void *)&msg_x_subscriber_done);
    cache_clean((void *)&msg_x_publisher_done);
#endif

    int ret = msg_router_subscribe(MSG_X_TOPIC, MSG_X_COMPONENT);
    TEST_ASSERT_MESSAGE(ret == 0, "subscribe failed");

    struct task *sub = task_create("msg_sub", msg_x_subscriber, NULL);
    struct task *pub = task_create("msg_pub", msg_x_publisher, NULL);
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_NOT_NULL(pub);

    scheduler_add_task_to_cpu(sub, 2);
    scheduler_add_task_to_cpu(pub, 1);

    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 8;
    while ((timer_get_count() - start) < limit) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_MSG_SUB_DONE) && NC_SYNC(NC_MSG_PUB_DONE)) break;
#else
        cache_invalidate((void *)&msg_x_subscriber_done);
        cache_invalidate((void *)&msg_x_publisher_done);
        if (msg_x_subscriber_done && msg_x_publisher_done) break;
#endif
        yield();
    }

    msg_router_unsubscribe_all(MSG_X_COMPONENT);

#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSG_PUBLISHED) == MSG_X_MESSAGES,
        "publisher did not publish all messages");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSG_RECEIVED) == MSG_X_MESSAGES,
        "subscriber did not receive all messages");
#else
    cache_invalidate((void *)&msg_x_received);
    cache_invalidate((void *)&msg_x_published);
    TEST_ASSERT_MESSAGE(msg_x_published == MSG_X_MESSAGES,
        "publisher did not publish all messages");
    TEST_ASSERT_MESSAGE(msg_x_received == MSG_X_MESSAGES,
        "subscriber did not receive all messages");
#endif
}

/* ============================================================================
 * Regression: msg_router multi-subscriber concurrent delivery (#67a / #864)
 *
 * Two components (MS_A, MS_B) subscribe to the same topic. A publisher on
 * CPU 1 emits N messages. MS_A's receive/ack loop runs on CPU 2; MS_B's on
 * CPU 3. Assertion: both subscribers see all N messages, every publish
 * returns delivered == 2, no task deadlocks.
 *
 * Why this is interesting: publish_internal serializes per-target ack waits
 * (gather targets under lock, then deliver + wait-ack per target before
 * moving to the next). Two subscribers on two CPUs exercise that loop with
 * genuinely concurrent ack arrivals — the lock-release-before-yield
 * discipline in runtime/src/msg_router.rs is what prevents the ack-side
 * (msg_router_ack, which re-takes MSG_ROUTER_LOCK) from deadlocking the
 * publisher.
 *
 * Pattern follows test_msg_router_cross_cpu: NC-sync slots for cross-CPU
 * counters, CNTPCT-based wall-clock timeout, TEST_IGNORE on cpu_count < 4
 * (shell + pub + 2 subs).
 * ============================================================================ */

#define MS_A_COMPONENT 20
#define MS_B_COMPONENT 21
#define MS_MESSAGES    5
#define MS_TOPIC       ((const uint8_t *)"/ms/x")

static volatile uint32_t ms_a_received = 0;
static volatile uint32_t ms_b_received = 0;
static volatile uint32_t ms_pub_done = 0;
static volatile uint32_t ms_sub_a_done = 0;
static volatile uint32_t ms_sub_b_done = 0;
static volatile uint32_t ms_delivered_total = 0;

static void ms_subscriber(void *arg)
{
    uintptr_t which = (uintptr_t)arg;  /* 0 = A, 1 = B */
    int component = (which == 0) ? MS_A_COMPONENT : MS_B_COMPONENT;
    uint8_t topic_buf[16];
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 10;
    uint32_t count = 0;

    while (count < MS_MESSAGES) {
        if ((timer_get_count() - start) >= limit) break;
        const uint8_t *data = msg_router_receive(component, topic_buf);
        if (data) {
            count++;
            if (which == 0) {
                ms_a_received = count;
#if defined(PLATFORM_HAS_NC_MEMORY)
                NC_SYNC(NC_MSGMS_A_RECEIVED) = count;
#else
                cache_clean((void *)&ms_a_received);
#endif
            } else {
                ms_b_received = count;
#if defined(PLATFORM_HAS_NC_MEMORY)
                NC_SYNC(NC_MSGMS_B_RECEIVED) = count;
#else
                cache_clean((void *)&ms_b_received);
#endif
            }
            msg_router_ack(component);
        } else {
            yield();
        }
    }

    if (which == 0) {
        ms_sub_a_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
        NC_SYNC(NC_MSGMS_SUB_A_DONE) = 1;
#else
        cache_clean((void *)&ms_sub_a_done);
#endif
    } else {
        ms_sub_b_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
        NC_SYNC(NC_MSGMS_SUB_B_DONE) = 1;
#else
        cache_clean((void *)&ms_sub_b_done);
#endif
    }
}

static void ms_publisher(void *arg)
{
    (void)arg;
    /* Small delay so both subscribers are scheduled and ready to drain. */
    delay(50000);

    uint32_t total = 0;
    for (uint32_t i = 0; i < MS_MESSAGES; i++) {
        uint8_t payload[4] = { (uint8_t)('a' + i), 0, 0, 0 };
        int delivered = msg_router_publish(MS_TOPIC, payload);
        if (delivered > 0) {
            total += (uint32_t)delivered;
        }
    }
    ms_delivered_total = total;
    ms_pub_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSGMS_DELIVERED) = total;
    NC_SYNC(NC_MSGMS_PUB_DONE) = 1;
#else
    cache_clean((void *)&ms_delivered_total);
    cache_clean((void *)&ms_pub_done);
#endif
}

static void test_msg_router_multi_subscriber(void)
{
    /* shell on 0, pub on 1, sub_a on 2, sub_b on 3 */
    if (cpu_count < 4) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 4");
        return;
    }

    msg_router_init();
    ms_a_received = 0;
    ms_b_received = 0;
    ms_pub_done = 0;
    ms_sub_a_done = 0;
    ms_sub_b_done = 0;
    ms_delivered_total = 0;
#if defined(PLATFORM_HAS_NC_MEMORY)
    nc_sync_clear(NC_MSGMS_A_RECEIVED, 6);
#else
    cache_clean((void *)&ms_a_received);
    cache_clean((void *)&ms_b_received);
    cache_clean((void *)&ms_pub_done);
    cache_clean((void *)&ms_sub_a_done);
    cache_clean((void *)&ms_sub_b_done);
    cache_clean((void *)&ms_delivered_total);
#endif

    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(MS_TOPIC, MS_A_COMPONENT));
    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(MS_TOPIC, MS_B_COMPONENT));

    struct task *sub_a = task_create("ms_sub_a", ms_subscriber, (void *)0);
    struct task *sub_b = task_create("ms_sub_b", ms_subscriber, (void *)1);
    struct task *pub = task_create("ms_pub", ms_publisher, NULL);
    TEST_ASSERT_NOT_NULL(sub_a);
    TEST_ASSERT_NOT_NULL(sub_b);
    TEST_ASSERT_NOT_NULL(pub);

    scheduler_add_task_to_cpu(sub_a, 2);
    scheduler_add_task_to_cpu(sub_b, 3);
    scheduler_add_task_to_cpu(pub, 1);

    /*
     * Generous timeout: Pi 5 ships cooperative preempt by default so
     * ack-wait yield-loops pace at the scheduler tick rate (10 ms).
     * 5 messages × 2 subscribers × per-ack handoff costs ~hundreds of
     * milliseconds in the worst case; 15 s leaves abundant headroom.
     */
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 15;
    while ((timer_get_count() - start) < limit) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_MSGMS_PUB_DONE) &&
            NC_SYNC(NC_MSGMS_SUB_A_DONE) &&
            NC_SYNC(NC_MSGMS_SUB_B_DONE)) break;
#else
        cache_invalidate((void *)&ms_pub_done);
        cache_invalidate((void *)&ms_sub_a_done);
        cache_invalidate((void *)&ms_sub_b_done);
        if (ms_pub_done && ms_sub_a_done && ms_sub_b_done) break;
#endif
        yield();
    }

    msg_router_unsubscribe_all(MS_A_COMPONENT);
    msg_router_unsubscribe_all(MS_B_COMPONENT);

#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGMS_PUB_DONE),
        "publisher did not finish all publishes");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGMS_SUB_A_DONE),
        "subscriber A did not finish");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGMS_SUB_B_DONE),
        "subscriber B did not finish");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGMS_A_RECEIVED) == MS_MESSAGES,
        "subscriber A did not receive all messages");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGMS_B_RECEIVED) == MS_MESSAGES,
        "subscriber B did not receive all messages");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGMS_DELIVERED) == MS_MESSAGES * 2u,
        "publisher: not every message reached both subscribers (delivered != 2*N)");
#else
    cache_invalidate((void *)&ms_a_received);
    cache_invalidate((void *)&ms_b_received);
    cache_invalidate((void *)&ms_pub_done);
    cache_invalidate((void *)&ms_sub_a_done);
    cache_invalidate((void *)&ms_sub_b_done);
    cache_invalidate((void *)&ms_delivered_total);
    TEST_ASSERT_MESSAGE(ms_pub_done, "publisher did not finish all publishes");
    TEST_ASSERT_MESSAGE(ms_sub_a_done, "subscriber A did not finish");
    TEST_ASSERT_MESSAGE(ms_sub_b_done, "subscriber B did not finish");
    TEST_ASSERT_MESSAGE(ms_a_received == MS_MESSAGES,
        "subscriber A did not receive all messages");
    TEST_ASSERT_MESSAGE(ms_b_received == MS_MESSAGES,
        "subscriber B did not receive all messages");
    TEST_ASSERT_MESSAGE(ms_delivered_total == MS_MESSAGES * 2u,
        "publisher: not every message reached both subscribers (delivered != 2*N)");
#endif
}

/* ============================================================================
 * Regression: msg_router priority ordering under concurrent load (#67a / #864)
 *
 * Component C subscribes to /pr/lo and /pr/hi. Two publishers run in parallel:
 *   - pub_lo (CPU 1): publish_priority(/pr/lo, ..., 0)
 *   - pub_hi (CPU 2): publish_priority(/pr/hi, ..., 5)
 * C drains on CPU 3.
 *
 * What this pins: msg_router_receive's lock-held priority scan
 * (runtime/src/msg_router.rs:840-917). When both mailboxes are ready,
 * receive returns /pr/hi first. Each receive call therefore prefers /pr/hi
 * whenever it's available — /pr/lo only flows through during windows where
 * pub_hi is between publishes (ack-waiting or briefly between iterations).
 * Under sustained load every iteration of pub_hi forces this scan to compare
 * priorities with a ready /pr/lo present.
 *
 * Caveat: mailboxes are single-slot. Because each publish_internal call
 * blocks on ack before returning, this test never has two unacked messages
 * in the SAME mailbox at once, so the single-slot overwrite issue isn't
 * exercised here. That case (back-to-back delivery to one mailbox with no
 * intervening ack) is a real limitation of the current design and is filed
 * as #869 for post-capstone follow-up — explicitly NOT in scope for this
 * test. The assertion is about CROSS-MAILBOX priority comparison at receive
 * time, not within-mailbox queueing.
 *
 * Acceptance: both publishers complete all N publishes with every message
 * acked (no starvation deadlock), subscriber receives exactly N hi + N lo,
 * total receive count matches.
 * ============================================================================ */

#define PR_C_COMPONENT 22
#define PR_MESSAGES    3
#define PR_TOPIC_HI    ((const uint8_t *)"/pr/hi")
#define PR_TOPIC_LO    ((const uint8_t *)"/pr/lo")
#define PR_PRIO_HI     5
#define PR_PRIO_LO     0

static volatile uint32_t pr_hi_received = 0;
static volatile uint32_t pr_lo_received = 0;
static volatile uint32_t pr_pub_hi_done = 0;
static volatile uint32_t pr_pub_lo_done = 0;
static volatile uint32_t pr_sub_done = 0;
static volatile uint32_t pr_hi_delivered = 0;
static volatile uint32_t pr_lo_delivered = 0;

static void pr_publisher_hi(void *arg)
{
    (void)arg;
    delay(50000);
    uint32_t total = 0;
    for (uint32_t i = 0; i < PR_MESSAGES; i++) {
        uint8_t payload[4] = { 'H', (uint8_t)('0' + i), 0, 0 };
        int delivered = msg_router_publish_priority(PR_TOPIC_HI, payload, PR_PRIO_HI);
        if (delivered > 0) total += (uint32_t)delivered;
    }
    pr_hi_delivered = total;
    pr_pub_hi_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSGPR_HI_DELIVERED) = total;
    NC_SYNC(NC_MSGPR_PUB_HI_DONE) = 1;
#else
    cache_clean((void *)&pr_hi_delivered);
    cache_clean((void *)&pr_pub_hi_done);
#endif
}

static void pr_publisher_lo(void *arg)
{
    (void)arg;
    delay(50000);
    uint32_t total = 0;
    for (uint32_t i = 0; i < PR_MESSAGES; i++) {
        uint8_t payload[4] = { 'L', (uint8_t)('0' + i), 0, 0 };
        int delivered = msg_router_publish_priority(PR_TOPIC_LO, payload, PR_PRIO_LO);
        if (delivered > 0) total += (uint32_t)delivered;
    }
    pr_lo_delivered = total;
    pr_pub_lo_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSGPR_LO_DELIVERED) = total;
    NC_SYNC(NC_MSGPR_PUB_LO_DONE) = 1;
#else
    cache_clean((void *)&pr_lo_delivered);
    cache_clean((void *)&pr_pub_lo_done);
#endif
}

static void pr_subscriber(void *arg)
{
    (void)arg;
    uint8_t topic_buf[16];
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 10;
    uint32_t hi = 0;
    uint32_t lo = 0;

    while (hi + lo < PR_MESSAGES * 2u) {
        if ((timer_get_count() - start) >= limit) break;
        const uint8_t *data = msg_router_receive(PR_C_COMPONENT, topic_buf);
        if (data) {
            /* Distinguish via the payload's first byte rather than re-walking
             * the topic_buf — both are valid signals; payload is cheaper. */
            if (data[0] == 'H') {
                hi++;
#if defined(PLATFORM_HAS_NC_MEMORY)
                NC_SYNC(NC_MSGPR_HI_RECEIVED) = hi;
#endif
            } else if (data[0] == 'L') {
                lo++;
#if defined(PLATFORM_HAS_NC_MEMORY)
                NC_SYNC(NC_MSGPR_LO_RECEIVED) = lo;
#endif
            }
            pr_hi_received = hi;
            pr_lo_received = lo;
#if !defined(PLATFORM_HAS_NC_MEMORY)
            cache_clean((void *)&pr_hi_received);
            cache_clean((void *)&pr_lo_received);
#endif
            msg_router_ack(PR_C_COMPONENT);
        } else {
            yield();
        }
    }

    pr_sub_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSGPR_SUB_DONE) = 1;
#else
    cache_clean((void *)&pr_sub_done);
#endif
}

static void test_msg_router_priority_concurrent(void)
{
    /* shell on 0, pub_lo on 1, pub_hi on 2, sub on 3 */
    if (cpu_count < 4) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 4");
        return;
    }

    msg_router_init();
    pr_hi_received = 0;
    pr_lo_received = 0;
    pr_pub_hi_done = 0;
    pr_pub_lo_done = 0;
    pr_sub_done = 0;
    pr_hi_delivered = 0;
    pr_lo_delivered = 0;
#if defined(PLATFORM_HAS_NC_MEMORY)
    nc_sync_clear(NC_MSGPR_HI_RECEIVED, 7);
#else
    cache_clean((void *)&pr_hi_received);
    cache_clean((void *)&pr_lo_received);
    cache_clean((void *)&pr_pub_hi_done);
    cache_clean((void *)&pr_pub_lo_done);
    cache_clean((void *)&pr_sub_done);
    cache_clean((void *)&pr_hi_delivered);
    cache_clean((void *)&pr_lo_delivered);
#endif

    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(PR_TOPIC_HI, PR_C_COMPONENT));
    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(PR_TOPIC_LO, PR_C_COMPONENT));

    struct task *sub = task_create("pr_sub", pr_subscriber, NULL);
    struct task *pub_lo = task_create("pr_pub_lo", pr_publisher_lo, NULL);
    struct task *pub_hi = task_create("pr_pub_hi", pr_publisher_hi, NULL);
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_NOT_NULL(pub_lo);
    TEST_ASSERT_NOT_NULL(pub_hi);

    scheduler_add_task_to_cpu(pub_lo, 1);
    scheduler_add_task_to_cpu(pub_hi, 2);
    scheduler_add_task_to_cpu(sub, 3);

    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 15;
    while ((timer_get_count() - start) < limit) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_MSGPR_PUB_HI_DONE) &&
            NC_SYNC(NC_MSGPR_PUB_LO_DONE) &&
            NC_SYNC(NC_MSGPR_SUB_DONE)) break;
#else
        cache_invalidate((void *)&pr_pub_hi_done);
        cache_invalidate((void *)&pr_pub_lo_done);
        cache_invalidate((void *)&pr_sub_done);
        if (pr_pub_hi_done && pr_pub_lo_done && pr_sub_done) break;
#endif
        yield();
    }

    msg_router_unsubscribe_all(PR_C_COMPONENT);

#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGPR_PUB_HI_DONE), "hi publisher hung");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGPR_PUB_LO_DONE), "lo publisher hung");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGPR_SUB_DONE), "subscriber hung");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGPR_HI_DELIVERED) == PR_MESSAGES,
        "hi publisher: not every publish was acked");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGPR_LO_DELIVERED) == PR_MESSAGES,
        "lo publisher: not every publish was acked (priority starvation?)");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGPR_HI_RECEIVED) == PR_MESSAGES,
        "subscriber: missed at least one /pr/hi message");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGPR_LO_RECEIVED) == PR_MESSAGES,
        "subscriber: missed at least one /pr/lo message");
#else
    cache_invalidate((void *)&pr_hi_received);
    cache_invalidate((void *)&pr_lo_received);
    cache_invalidate((void *)&pr_pub_hi_done);
    cache_invalidate((void *)&pr_pub_lo_done);
    cache_invalidate((void *)&pr_sub_done);
    cache_invalidate((void *)&pr_hi_delivered);
    cache_invalidate((void *)&pr_lo_delivered);
    TEST_ASSERT_MESSAGE(pr_pub_hi_done, "hi publisher hung");
    TEST_ASSERT_MESSAGE(pr_pub_lo_done, "lo publisher hung");
    TEST_ASSERT_MESSAGE(pr_sub_done, "subscriber hung");
    TEST_ASSERT_MESSAGE(pr_hi_delivered == PR_MESSAGES,
        "hi publisher: not every publish was acked");
    TEST_ASSERT_MESSAGE(pr_lo_delivered == PR_MESSAGES,
        "lo publisher: not every publish was acked (priority starvation?)");
    TEST_ASSERT_MESSAGE(pr_hi_received == PR_MESSAGES,
        "subscriber: missed at least one /pr/hi message");
    TEST_ASSERT_MESSAGE(pr_lo_received == PR_MESSAGES,
        "subscriber: missed at least one /pr/lo message");
#endif
}

/* ============================================================================
 * Regression: msg_router LAST_RECEIVED ack-targeting under cross-CPU
 * activity (#67c / #867)
 *
 * Cross-CPU variant of test_msg_router_ack_targets_last_received in
 * test_msg_router.c. The test thread runs on the shell's CPU (CPU 0);
 * a helper task pinned to CPU 1 publishes /ar/b after a barrier so
 * the publish lands AFTER the test's receive() but BEFORE its ack().
 *
 * Sequence (with cross-CPU helper):
 *   T0: test (CPU 0): subscribe /ar/a + /ar/b
 *   T0: test (CPU 0): publish_nowait /ar/a (prio 5) — MA.ready=1
 *   T0: test (CPU 0): receive — returns /ar/a, sets LAST_RECEIVED[C]
 *   T0: test (CPU 0): NC_SYNC(TRIGGER) = 1 (signal helper)
 *   T1: helper (CPU 1): wakes, publish_nowait /ar/b (prio 5) — MB.ready=1
 *   T1: helper (CPU 1): NC_SYNC(HELPER_DONE) = 1
 *   T0: test (CPU 0): spin on HELPER_DONE
 *   T0: test (CPU 0): ack() — clears MA, NOT MB
 *   T0: test (CPU 0): receive — returns /ar/b
 *   T0: test (CPU 0): ack(), receive → NULL
 *
 * Demonstrates the ack-targeting contract under genuinely concurrent
 * activity: the publishing CPU is serializing through MSG_ROUTER_LOCK
 * (SpinGuard) alongside the receive/ack calls. Because publish_internal
 * (and publish_internal_nowait) releases the lock between gather and
 * the BSS-static ready/ack atomics, ack() can interleave with helper's
 * publish without deadlock.
 *
 * Requires cpu_count >= 2 (shell + helper). The receive/ack work runs
 * on the same CPU as the shell, so we don't need a third CPU.
 * ============================================================================ */

#define AR_COMPONENT 23
#define AR_TOPIC_A   ((const uint8_t *)"/ar/a")
#define AR_TOPIC_B   ((const uint8_t *)"/ar/b")

extern int msg_router_publish_nowait(const uint8_t *topic_name,
                                     const uint8_t *data);

static volatile uint32_t ar_trigger;
static volatile uint32_t ar_helper_done;

static void ar_helper(void *arg)
{
    (void)arg;
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 5;
    while ((timer_get_count() - start) < limit) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_MSGAR_TRIGGER)) break;
#else
        cache_invalidate((void *)&ar_trigger);
        if (ar_trigger) break;
#endif
        yield();
    }

    /* Helper publishes /ar/b at the same priority as the earlier /ar/a
     * publish. The equal-priority tie-break in receive picks /ar/a
     * first (subscribed earlier → lower TOPICS[] index), so after ack
     * clears /ar/a the next receive picks /ar/b. */
    (void)msg_router_publish_nowait(AR_TOPIC_B, (const uint8_t *)"B1");

    ar_helper_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSGAR_HELPER_DONE) = 1;
#else
    cache_clean((void *)&ar_helper_done);
#endif
}

static void test_msg_router_ack_targets_last_received_cross_cpu(void)
{
    /* shell on 0, helper on 1 */
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 2");
        return;
    }

    msg_router_init();
    ar_trigger = 0;
    ar_helper_done = 0;
#if defined(PLATFORM_HAS_NC_MEMORY)
    nc_sync_clear(NC_MSGAR_TRIGGER, 2);
#else
    cache_clean((void *)&ar_trigger);
    cache_clean((void *)&ar_helper_done);
#endif

    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(AR_TOPIC_A, AR_COMPONENT));
    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(AR_TOPIC_B, AR_COMPONENT));

    /* Spawn helper that will publish /ar/b on CPU 1 when triggered. */
    struct task *helper = task_create("ar_helper", ar_helper, NULL);
    TEST_ASSERT_NOT_NULL(helper);
    scheduler_add_task_to_cpu(helper, 1);

    /* Step 1: publish /ar/a on the test thread (CPU 0). MA.ready=1.
     * publish_nowait is used so the test thread can continue to
     * receive/ack itself — the contract being tested is ack-target
     * routing across CPUs, not the ack-wait loop. */
    int d_a = msg_router_publish_nowait(AR_TOPIC_A, (const uint8_t *)"A1");
    TEST_ASSERT_EQUAL_INT(1, d_a);

    /* Step 2: receive returns /ar/a; LAST_RECEIVED[AR_COMPONENT] := MA. */
    uint8_t topic_buf[16];
    const uint8_t *m1 = msg_router_receive(AR_COMPONENT, topic_buf);
    TEST_ASSERT_NOT_NULL(m1);
    TEST_ASSERT_EQUAL_STRING("/ar/a", (const char *)topic_buf);

    /* Step 3: trigger helper to publish /ar/b from CPU 1, and wait for
     * confirmation that the publish has landed. After HELPER_DONE we
     * know MB.ready=1. */
    ar_trigger = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSGAR_TRIGGER) = 1;
#else
    cache_clean((void *)&ar_trigger);
#endif
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 5;
    while ((timer_get_count() - start) < limit) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_MSGAR_HELPER_DONE)) break;
#else
        cache_invalidate((void *)&ar_helper_done);
        if (ar_helper_done) break;
#endif
        yield();
    }
#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGAR_HELPER_DONE),
        "helper task did not complete /ar/b publish within timeout");
#else
    cache_invalidate((void *)&ar_helper_done);
    TEST_ASSERT_MESSAGE(ar_helper_done,
        "helper task did not complete /ar/b publish within timeout");
#endif

    /* Step 4: ack — targets MA (the receive's source), NOT MB. */
    msg_router_ack(AR_COMPONENT);

    /* Step 5: receive must return /ar/b. If ack had wrongly cleared MB,
     * receive's lock-held scan would still pick /ar/a (subscribed
     * earlier, equal priority). */
    const uint8_t *m2 = msg_router_receive(AR_COMPONENT, topic_buf);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_STRING("/ar/b", (const char *)topic_buf);

    msg_router_ack(AR_COMPONENT);

    /* Both mailboxes drained. */
    const uint8_t *m3 = msg_router_receive(AR_COMPONENT, topic_buf);
    TEST_ASSERT_NULL(m3);

    msg_router_unsubscribe_all(AR_COMPONENT);
}

/* ============================================================================
 * Regression: msg_router same-mailbox zero-loss under concurrent
 * publishers (#869)
 *
 * Two publishers on different CPUs target the SAME topic — and therefore
 * the SAME subscriber mailbox — while a subscriber on a third CPU drains
 * messages and acks each. Pre-#869 the mailbox was single-slot:
 * publisher A's deliver() set ready=1; publisher B's concurrent deliver()
 * overwrote the data in place while ready was still 1. The subscriber
 * saw only one of the two messages, but BOTH publishers observed the
 * subscriber's ack and reported delivered=1 — silent data loss with no
 * observable error.
 *
 * After #869, deliver() refuses to overwrite an unacked mailbox; the
 * publisher waits for ack and retries. Both messages reach the subscriber.
 *
 * Test invariant:
 *   subscriber A-payload count + B-payload count
 *     == pub_a_delivered + pub_b_delivered
 *
 * On main pre-fix this invariant fails — the publisher counts can
 * exceed what the subscriber actually saw. Post-fix they match exactly.
 *
 * Requires cpu_count >= 4 (publisher A on CPU 1, publisher B on CPU 2,
 * subscriber on CPU 3, plus shell on CPU 0).
 *
 * IMPORTANT — QEMU vs hardware: this test passes on QEMU virt regardless
 * of whether the fix is present, because QEMU's serial TCG scheduler
 * interleaves the publishers and subscriber deterministically and never
 * opens the race window in the first place. The test's primary value is
 * on real Pi 5 / Jetson SMP hardware where publishers genuinely run
 * concurrently. A green QEMU run is structural only — do NOT conclude
 * from it that the underlying mailbox semantics are correct without a
 * hardware run. The matching same-CPU contract canary in
 * test_msg_router.c (`test_publish_nowait_basic_contract`) DOES fire on
 * QEMU and is the day-to-day regression guard.
 * ============================================================================ */

#define SM_C_COMPONENT 24
#define SM_TOPIC       ((const uint8_t *)"/sm")
#define SM_MESSAGES    5
#define SM_PRIO        0

static volatile uint32_t sm_a_delivered = 0;
static volatile uint32_t sm_b_delivered = 0;
static volatile uint32_t sm_sub_a_seen  = 0;
static volatile uint32_t sm_sub_b_seen  = 0;
static volatile uint32_t sm_pub_a_done  = 0;
static volatile uint32_t sm_pub_b_done  = 0;
static volatile uint32_t sm_sub_done    = 0;

static void sm_publisher_a(void *arg)
{
    (void)arg;
    delay(50000);
    uint32_t total = 0;
    for (uint32_t i = 0; i < SM_MESSAGES; i++) {
        uint8_t payload[4] = { 'A', (uint8_t)('0' + i), 0, 0 };
        int delivered = msg_router_publish_priority(SM_TOPIC, payload, SM_PRIO);
        if (delivered > 0) total += (uint32_t)delivered;
    }
    sm_a_delivered = total;
    sm_pub_a_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSGSM_A_DELIVERED) = total;
    NC_SYNC(NC_MSGSM_PUB_A_DONE) = 1;
#else
    cache_clean((void *)&sm_a_delivered);
    cache_clean((void *)&sm_pub_a_done);
#endif
}

static void sm_publisher_b(void *arg)
{
    (void)arg;
    delay(50000);
    uint32_t total = 0;
    for (uint32_t i = 0; i < SM_MESSAGES; i++) {
        uint8_t payload[4] = { 'B', (uint8_t)('0' + i), 0, 0 };
        int delivered = msg_router_publish_priority(SM_TOPIC, payload, SM_PRIO);
        if (delivered > 0) total += (uint32_t)delivered;
    }
    sm_b_delivered = total;
    sm_pub_b_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSGSM_B_DELIVERED) = total;
    NC_SYNC(NC_MSGSM_PUB_B_DONE) = 1;
#else
    cache_clean((void *)&sm_b_delivered);
    cache_clean((void *)&sm_pub_b_done);
#endif
}

static void sm_subscriber(void *arg)
{
    (void)arg;
    uint8_t topic_buf[16];
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 10;
    uint32_t a = 0;
    uint32_t b = 0;

    while (a + b < SM_MESSAGES * 2u) {
        if ((timer_get_count() - start) >= limit) break;
        const uint8_t *data = msg_router_receive(SM_C_COMPONENT, topic_buf);
        if (data) {
            if (data[0] == 'A') {
                a++;
#if defined(PLATFORM_HAS_NC_MEMORY)
                NC_SYNC(NC_MSGSM_SUB_A_SEEN) = a;
#endif
            } else if (data[0] == 'B') {
                b++;
#if defined(PLATFORM_HAS_NC_MEMORY)
                NC_SYNC(NC_MSGSM_SUB_B_SEEN) = b;
#endif
            }
            sm_sub_a_seen = a;
            sm_sub_b_seen = b;
#if !defined(PLATFORM_HAS_NC_MEMORY)
            cache_clean((void *)&sm_sub_a_seen);
            cache_clean((void *)&sm_sub_b_seen);
#endif
            msg_router_ack(SM_C_COMPONENT);
        } else {
            yield();
        }
    }

    sm_sub_done = 1;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_MSGSM_SUB_DONE) = 1;
#else
    cache_clean((void *)&sm_sub_done);
#endif
}

static void test_msg_router_same_mailbox_zero_loss(void)
{
    /* shell on 0, pub_a on 1, pub_b on 2, sub on 3 */
    if (cpu_count < 4) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 4");
        return;
    }

    msg_router_init();
    sm_a_delivered = 0;
    sm_b_delivered = 0;
    sm_sub_a_seen = 0;
    sm_sub_b_seen = 0;
    sm_pub_a_done = 0;
    sm_pub_b_done = 0;
    sm_sub_done = 0;
#if defined(PLATFORM_HAS_NC_MEMORY)
    nc_sync_clear(NC_MSGSM_A_DELIVERED, 7);
#else
    cache_clean((void *)&sm_a_delivered);
    cache_clean((void *)&sm_b_delivered);
    cache_clean((void *)&sm_sub_a_seen);
    cache_clean((void *)&sm_sub_b_seen);
    cache_clean((void *)&sm_pub_a_done);
    cache_clean((void *)&sm_pub_b_done);
    cache_clean((void *)&sm_sub_done);
#endif

    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(SM_TOPIC, SM_C_COMPONENT));

    struct task *sub   = task_create("sm_sub",   sm_subscriber, NULL);
    struct task *pub_a = task_create("sm_pub_a", sm_publisher_a, NULL);
    struct task *pub_b = task_create("sm_pub_b", sm_publisher_b, NULL);
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_NOT_NULL(pub_a);
    TEST_ASSERT_NOT_NULL(pub_b);

    scheduler_add_task_to_cpu(pub_a, 1);
    scheduler_add_task_to_cpu(pub_b, 2);
    scheduler_add_task_to_cpu(sub, 3);

    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 15;
    while ((timer_get_count() - start) < limit) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_MSGSM_PUB_A_DONE) &&
            NC_SYNC(NC_MSGSM_PUB_B_DONE) &&
            NC_SYNC(NC_MSGSM_SUB_DONE)) break;
#else
        cache_invalidate((void *)&sm_pub_a_done);
        cache_invalidate((void *)&sm_pub_b_done);
        cache_invalidate((void *)&sm_sub_done);
        if (sm_pub_a_done && sm_pub_b_done && sm_sub_done) break;
#endif
        yield();
    }

    msg_router_unsubscribe_all(SM_C_COMPONENT);

#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGSM_PUB_A_DONE), "pub_a hung");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGSM_PUB_B_DONE), "pub_b hung");
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_MSGSM_SUB_DONE),   "subscriber hung");
    uint32_t a_seen = NC_SYNC(NC_MSGSM_SUB_A_SEEN);
    uint32_t b_seen = NC_SYNC(NC_MSGSM_SUB_B_SEEN);
    uint32_t a_del  = NC_SYNC(NC_MSGSM_A_DELIVERED);
    uint32_t b_del  = NC_SYNC(NC_MSGSM_B_DELIVERED);
#else
    cache_invalidate((void *)&sm_pub_a_done);
    cache_invalidate((void *)&sm_pub_b_done);
    cache_invalidate((void *)&sm_sub_done);
    cache_invalidate((void *)&sm_sub_a_seen);
    cache_invalidate((void *)&sm_sub_b_seen);
    cache_invalidate((void *)&sm_a_delivered);
    cache_invalidate((void *)&sm_b_delivered);
    TEST_ASSERT_MESSAGE(sm_pub_a_done, "pub_a hung");
    TEST_ASSERT_MESSAGE(sm_pub_b_done, "pub_b hung");
    TEST_ASSERT_MESSAGE(sm_sub_done,   "subscriber hung");
    uint32_t a_seen = sm_sub_a_seen;
    uint32_t b_seen = sm_sub_b_seen;
    uint32_t a_del  = sm_a_delivered;
    uint32_t b_del  = sm_b_delivered;
#endif

    /* Invariant: every message a publisher counted as delivered must
     * have been seen by the subscriber. With single-slot semantics the
     * publisher counts can exceed the subscriber-seen counts because
     * two deliveries collapse into one acked mailbox state. */
    TEST_ASSERT_MESSAGE(a_del == SM_MESSAGES,
        "pub_a: not every publish returned delivered>0");
    TEST_ASSERT_MESSAGE(b_del == SM_MESSAGES,
        "pub_b: not every publish returned delivered>0");
    TEST_ASSERT_MESSAGE(a_seen == SM_MESSAGES,
        "subscriber lost A-payloads (mailbox overwrite bug)");
    TEST_ASSERT_MESSAGE(b_seen == SM_MESSAGES,
        "subscriber lost B-payloads (mailbox overwrite bug)");
}

/* ============================================================================
 * smp_notify_cpu (Prereq #3) — direct functional tests
 *
 * `scheduler_add_task_to_cpu()` invokes `smp_notify_cpu(cpu)` after
 * queuing a task. The abstraction replaces the old
 * `#if !defined(PLATFORM_X86_64) __asm__ volatile("sev") #endif` block.
 * These tests verify:
 *   1. The API is safe to call for every valid CPU id (compile-time
 *      linkage + runtime no-crash).
 *   2. It actually wakes a secondary CPU that is idle in WFE — the
 *      property the scheduler depends on.
 * ============================================================================ */

static void test_smp_notify_cpu_safe(void)
{
    /* Fire smp_notify_cpu for every known CPU id. On ARM64 each call
     * issues an SEV (broadcast); on x86-64 it is a no-op. If the symbol
     * failed to link, this test would not compile. If the call hung or
     * faulted, the test would time out / crash. */
    for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
        smp_notify_cpu(cpu);
    }
    /* Also tolerate out-of-range ids without crashing. The API does not
     * promise anything for invalid cpus, but the implementations must
     * not dereference cpu-indexed tables unsafely. */
    smp_notify_cpu(MAX_CPUS);
    TEST_ASSERT_TRUE(1);  /* Reached — no crash. */
}

/*
 * Verify that smp_notify_cpu wakes a secondary CPU from its idle WFE.
 *
 * Approach: we queue a task that writes a sentinel to an NC slot.
 * scheduler_add_task_to_cpu() in turn calls smp_notify_cpu(). Without
 * the wake, the secondary CPU could be stuck in WFE and the sentinel
 * would never be written. A timeout proves the negative.
 *
 * This is not a new scenario — `test_multicore_basic` already proves
 * cross-CPU dispatch works end-to-end. What this test adds is a
 * focused assertion on the notification path specifically, catching
 * a regression where `smp_notify_cpu` would become a no-op on ARM64.
 */
#define NOTIFY_TARGET_CPU 1
#define NOTIFY_SENTINEL   0xD00DFACE
static volatile uint32_t smp_notify_sentinel;

static void smp_notify_probe_task(void *arg)
{
    (void)arg;
    smp_notify_sentinel = NOTIFY_SENTINEL;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_NOTIFY_SENTINEL) = NOTIFY_SENTINEL;
#else
    cache_clean((void *)&smp_notify_sentinel);
#endif
}

static void test_smp_notify_cpu_wakes_secondary(void)
{
    if (cpu_count < 2) {
        TEST_IGNORE_MESSAGE("Requires cpu_count >= 2");
        return;
    }

    smp_notify_sentinel = 0;
#if defined(PLATFORM_HAS_NC_MEMORY)
    NC_SYNC(NC_NOTIFY_SENTINEL) = 0;
#else
    cache_clean((void *)&smp_notify_sentinel);
#endif

    struct task *probe = task_create("smp_notify_probe",
                                     smp_notify_probe_task, NULL);
    TEST_ASSERT_NOT_NULL(probe);

    /* scheduler_add_task_to_cpu publishes the task AND calls
     * smp_notify_cpu(NOTIFY_TARGET_CPU) — exactly the path we want to
     * exercise. */
    scheduler_add_task_to_cpu(probe, NOTIFY_TARGET_CPU);

    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * 3;  /* 3 s */
    while ((timer_get_count() - start) < limit) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        if (NC_SYNC(NC_NOTIFY_SENTINEL) == NOTIFY_SENTINEL) break;
#else
        cache_invalidate((void *)&smp_notify_sentinel);
        if (smp_notify_sentinel == NOTIFY_SENTINEL) break;
#endif
        yield();
    }

#if defined(PLATFORM_HAS_NC_MEMORY)
    TEST_ASSERT_MESSAGE(NC_SYNC(NC_NOTIFY_SENTINEL) == NOTIFY_SENTINEL,
        "smp_notify_cpu: secondary CPU did not wake and run probe task within timeout");
#else
    cache_invalidate((void *)&smp_notify_sentinel);
    TEST_ASSERT_MESSAGE(smp_notify_sentinel == NOTIFY_SENTINEL,
        "smp_notify_cpu: secondary CPU did not wake and run probe task within timeout");
#endif
}

/* ============================================================================
 * SECONDARY_PREEMPT rename (Prereq #2) — compile-time regression test
 * ============================================================================
 * The rename is entirely cfg-guarded, so the "runtime behavior test" for
 * it is: does the binary still link and behave identically? The full
 * test suite running green under both `SECONDARY_PREEMPT=ON` and the
 * legacy `PI5_SECONDARY_PREEMPT=ON` aliases is that test.
 *
 * We also assert the macro spelling here at compile time. If a future
 * refactor accidentally reverts the rename (e.g. re-introduces a
 * `PI5_SECONDARY_PREEMPT`-only guard in shared source), and the user
 * builds with `SECONDARY_PREEMPT=ON` expecting the trampoline, this
 * assertion fires before link-time mysteries surface.
 */
#if defined(SECONDARY_PREEMPT)
_Static_assert(SECONDARY_PREEMPT == 1,
    "SECONDARY_PREEMPT is defined but not 1 — CMake plumbing broken");
#endif
#if defined(PI5_SECONDARY_PREEMPT) && !defined(SECONDARY_PREEMPT)
_Static_assert(0,
    "Legacy PI5_SECONDARY_PREEMPT is set but SECONDARY_PREEMPT is not — "
    "CMake alias wiring broken; see docs/smp.md §Secondary-CPU Preemption");
#endif

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

    /* Regression: msg_router cross-CPU publish/receive/ack (#66) */
    RUN_TEST(test_msg_router_cross_cpu);

    /* Regression: msg_router concurrency stress (#67a / #864) */
    RUN_TEST(test_msg_router_multi_subscriber);
    RUN_TEST(test_msg_router_priority_concurrent);

    /* Regression: msg_router LAST_RECEIVED ack-targeting (#67c / #867) */
    RUN_TEST(test_msg_router_ack_targets_last_received_cross_cpu);

    /* Regression: msg_router same-mailbox zero-loss (#869) */
    RUN_TEST(test_msg_router_same_mailbox_zero_loss);

    /* Prereq #3: smp_notify_cpu abstraction */
    RUN_TEST(test_smp_notify_cpu_safe);
    RUN_TEST(test_smp_notify_cpu_wakes_secondary);

#if CONFIG_WORK_STEALING
    /* Phase B: work-stealing load distribution (#59). */
    RUN_TEST(test_work_stealing_distributes_load);
#endif

    return UNITY_END();
}
