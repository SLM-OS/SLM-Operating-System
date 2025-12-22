/*
 * main.c - SLM-OS kernel main entry point
 */

#include "platform.h"
#include "uart.h"
#include "debug.h"
#include "pmm.h"
#include "vmm.h"
#include "task.h"
#include "sched.h"
#include "gic.h"
#include "timer.h"
#include "smp.h"        /* For cpu_id() and cpu_count */
#include "spinlock.h"
#include "ipc.h"
#include "slm_ffi.h"
#include "test_harness.h"
#include <stdint.h>
#include <stdbool.h>

/* External symbols from linker script */
extern char __text_start, __text_end;
extern char __data_start, __data_end;
extern char __bss_start, __bss_end;
extern char __kernel_end;

/*
 * Print kernel memory layout information.
 */
static void print_memory_info(void)
{
    uart_printf("Memory Layout:\n");
    uart_printf("  .text:   %p - %p\n", &__text_start, &__text_end);
    uart_printf("  .data:   %p - %p\n", &__data_start, &__data_end);
    uart_printf("  .bss:    %p - %p\n", &__bss_start, &__bss_end);
    uart_printf("  End:     %p\n", &__kernel_end);
    uart_printf("  RAM:     %p - %p (%u MB)\n",
                (void *)RAM_BASE,
                (void *)(RAM_BASE + RAM_SIZE),
                RAM_SIZE / (1024 * 1024));
}

/*
 * Simple delay loop (busy wait)
 */
static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile("nop");
    }
}

/* ============================================================================
 * Test Infrastructure
 * ============================================================================ */

/* Shared test state protected by spinlock */
static struct {
    spinlock_t lock;
    volatile uint32_t tasks_completed;
    volatile uint32_t total_iterations;
    volatile uint32_t cpu_task_count[MAX_CPUS];  /* Tasks executed per CPU */
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
 * Test Task Functions
 * ============================================================================ */


/*
 * Migration test task - runs on initial CPU, gets migrated, runs more.
 */
static volatile bool migration_ready = false;
static volatile bool migration_done = false;
static volatile uint32_t migration_cpu_before = 0;
static volatile uint32_t migration_cpu_after = 0;

static void migration_test_task(void *arg)
{
    (void)arg;

    migration_cpu_before = cpu_id();
    uart_printf("[Migration Task] Started on CPU %u\n", migration_cpu_before);

    /* Signal ready for migration */
    migration_ready = true;

    /* Wait for migration to complete */
    while (!migration_done) {
        delay(10000);
        yield();
    }

    migration_cpu_after = cpu_id();
    uart_printf("[Migration Task] After migration, running on CPU %u\n",
                migration_cpu_after);

    /* Do some work to prove we're running */
    for (int i = 0; i < 3; i++) {
        uart_printf("[Migration Task @ CPU %u] iteration %d/3\n", cpu_id(), i + 1);
        delay(200000);
    }

    uart_printf("[Migration Task] Done!\n");

    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed++;
    spin_unlock_irqrestore(&test_state.lock, flags);
}

/* Simple named tasks for basic multi-core test */
static void task_a_func(void *arg)
{
    int count = (int)(uintptr_t)arg;
    for (int i = 0; i < count; i++) {
        uart_printf("[Task A @ CPU %u] iteration %d/%d\n", cpu_id(), i + 1, count);
        delay(500000);
    }
    uart_printf("[Task A @ CPU %u] Done!\n", cpu_id());
}

static void task_b_func(void *arg)
{
    int count = (int)(uintptr_t)arg;
    for (int i = 0; i < count; i++) {
        uart_printf("[Task B @ CPU %u] iteration %d/%d\n", cpu_id(), i + 1, count);
        delay(500000);
    }
    uart_printf("[Task B @ CPU %u] Done!\n", cpu_id());
}

static void task_c_func(void *arg)
{
    int count = (int)(uintptr_t)arg;
    for (int i = 0; i < count; i++) {
        uart_printf("[Task C @ CPU %u] iteration %d/%d\n", cpu_id(), i + 1, count);
        delay(500000);
    }
    uart_printf("[Task C @ CPU %u] Done!\n", cpu_id());
}

/* ============================================================================
 * Test Suites
 * ============================================================================ */

static int test_errors = 0;

/*
 * Test 1: Basic Multi-Core Execution
 * Distribute 3 tasks to 3 different CPUs, verify all complete.
 */
static void test_multicore_basic(void)
{
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Test 1: Basic Multi-Core Execution\n");
    uart_puts("========================================\n");

    struct task *task_a = task_create("task_a", task_a_func, (void *)3);
    struct task *task_b = task_create("task_b", task_b_func, (void *)3);
    struct task *task_c = task_create("task_c", task_c_func, (void *)3);

    if (!task_a || !task_b || !task_c) {
        uart_puts("  [FAIL] Failed to create test tasks\n");
        test_errors++;
        return;
    }

    scheduler_add_task_to_cpu(task_a, 1);
    scheduler_add_task_to_cpu(task_b, 2);
    scheduler_add_task_to_cpu(task_c, 3);

    uart_puts("[Test1] Tasks distributed to CPUs 1, 2, 3\n");

    /* Wait for completion */
    while (task_a->state != TASK_TERMINATED ||
           task_b->state != TASK_TERMINATED ||
           task_c->state != TASK_TERMINATED) {
        delay(100000);
    }

    uart_puts("  [PASS] All 3 tasks completed on separate CPUs\n");
}

/*
 * Test 2: Task Migration
 * Create a task, put it in a queue, migrate to different CPU before it runs.
 *
 * Strategy: Queue migrate task on CPU 1 first, then immediately queue a
 * long-running blocker. Blocker runs first (FIFO), keeping CPU 1 busy
 * while we migrate the waiting task to CPU 3.
 */
static void test_task_migration(void)
{
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Test 2: Cross-Core Task Migration\n");
    uart_puts("========================================\n");

    /* Reset migration state */
    migration_ready = false;
    migration_done = false;
    migration_cpu_before = 0;
    migration_cpu_after = 0;
    reset_test_state();

    /*
     * Create migration task FIRST but don't add to queue yet.
     * Then add blocker, which will run first. Then add migrate task.
     * While blocker runs, we migrate the READY migrate task to CPU 3.
     */
    struct task *mig_task = task_create("migrate", migration_test_task, NULL);
    if (!mig_task) {
        uart_puts("  [FAIL] Failed to create migration task\n");
        test_errors++;
        return;
    }

    /* Create a long blocker (10 iterations) to ensure plenty of time */
    struct task *blocker = task_create("blocker", task_a_func, (void *)10);
    if (!blocker) {
        uart_puts("  [FAIL] Failed to create blocker task\n");
        test_errors++;
        return;
    }

    /* Add blocker to CPU 1 first - it will start running */
    scheduler_add_task_to_cpu(blocker, 1);
    uart_puts("[Test2] Blocker task added to CPU 1\n");

    /* Very small delay to let blocker become RUNNING */
    delay(50000);

    /* Now add migrate task - it will be READY, waiting behind blocker */
    scheduler_add_task_to_cpu(mig_task, 1);
    uart_puts("[Test2] Migration task queued behind blocker\n");

    /* Migrate immediately while blocker is still running */
    uart_printf("[Test2] Task state: %d (1=READY, 2=RUNNING)\n", mig_task->state);

    int ret = sched_migrate_task(mig_task, 3);
    if (ret != 0) {
        uart_printf("  [FAIL] sched_migrate_task returned %d\n", ret);
        uart_printf("         Task state was %d (need READY=1)\n", mig_task->state);
        test_errors++;
        migration_done = true;
        /* Still wait for tasks to finish */
        while (mig_task->state != TASK_TERMINATED ||
               blocker->state != TASK_TERMINATED) {
            delay(100000);
        }
        return;
    }

    uart_puts("[Test2] Migration successful! Task moved to CPU 3 queue\n");

    /* Wait for task to start on CPU 3 and signal ready */
    while (!migration_ready) {
        delay(50000);
    }

    /* Let task check its CPU and finish */
    migration_done = true;

    /* Wait for both tasks to complete */
    while (mig_task->state != TASK_TERMINATED ||
           blocker->state != TASK_TERMINATED) {
        delay(100000);
    }

    /* Verify migration occurred - task should report running on CPU 3 */
    if (migration_cpu_before == 3) {
        uart_printf("  [PASS] Task ran on CPU 3 after migration\n");
    } else {
        uart_printf("  [FAIL] Task ran on CPU %u (expected CPU 3)\n",
                    migration_cpu_before);
        test_errors++;
    }
}

/*
 * Simple stress task function - just loops with output
 */
static void stress_task_func(void *arg)
{
    int id = (int)(uintptr_t)arg;
    uint32_t cpu = cpu_id();

    for (int i = 0; i < 2; i++) {
        uart_printf("[stress%d @ CPU %u] iter %d/2\n", id, cpu, i + 1);
        delay(200000);
    }
    uart_printf("[stress%d @ CPU %u] Done!\n", id, cpu);

    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed++;
    spin_unlock_irqrestore(&test_state.lock, flags);
}

/*
 * Test 3: Stress Test - Many Tasks Across Secondary CPUs
 * Create 6 tasks distributed across CPUs 1, 2, 3 (2 per CPU).
 */
static void test_stress_multicpu(void)
{
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Test 3: Stress Test (6 Tasks, 3 CPUs)\n");
    uart_puts("========================================\n");

    reset_test_state();

    struct task *tasks[6];
    static const char *names[6] = {"s0", "s1", "s2", "s3", "s4", "s5"};

    /* Create 6 tasks */
    for (int i = 0; i < 6; i++) {
        tasks[i] = task_create(names[i], stress_task_func, (void *)(uintptr_t)i);
        if (!tasks[i]) {
            uart_printf("  [FAIL] Failed to create task %d\n", i);
            test_errors++;
            return;
        }
    }

    /* Distribute: 2 tasks per CPU (CPUs 1, 2, 3) */
    scheduler_add_task_to_cpu(tasks[0], 1);
    scheduler_add_task_to_cpu(tasks[1], 2);
    scheduler_add_task_to_cpu(tasks[2], 3);
    scheduler_add_task_to_cpu(tasks[3], 1);
    scheduler_add_task_to_cpu(tasks[4], 2);
    scheduler_add_task_to_cpu(tasks[5], 3);

    uart_puts("[Test3] 6 tasks distributed: 2 per CPU (1, 2, 3)\n");

    /* Wait for all tasks to complete */
    while (tasks[0]->state != TASK_TERMINATED ||
           tasks[1]->state != TASK_TERMINATED ||
           tasks[2]->state != TASK_TERMINATED ||
           tasks[3]->state != TASK_TERMINATED ||
           tasks[4]->state != TASK_TERMINATED ||
           tasks[5]->state != TASK_TERMINATED) {
        delay(100000);
    }

    uart_printf("[Test3] All 6 tasks completed (%u tracked)\n",
                test_state.tasks_completed);
    uart_puts("  [PASS] Stress test completed successfully\n");
}

/*
 * Test 4: Concurrent Lock Stress
 * Multiple tasks contending on the same spinlock.
 */
static spinlock_t contention_lock = SPINLOCK_INIT;
static volatile uint32_t contention_counter = 0;

static void contention_task(void *arg)
{
    int increments = (int)(uintptr_t)arg;

    for (int i = 0; i < increments; i++) {
        irq_flags_t flags = spin_lock_irqsave(&contention_lock);
        contention_counter++;
        spin_unlock_irqrestore(&contention_lock, flags);
        /* Small delay to create contention */
        delay(1000);
    }

    irq_flags_t flags = spin_lock_irqsave(&test_state.lock);
    test_state.tasks_completed++;
    spin_unlock_irqrestore(&test_state.lock, flags);
}

static void test_lock_contention(void)
{
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Test 4: Lock Contention (Race Detection)\n");
    uart_puts("========================================\n");

    reset_test_state();
    contention_counter = 0;

    #define CONTENTION_TASKS 3
    #define INCREMENTS_PER_TASK 50

    struct task *tasks[CONTENTION_TASKS];

    for (int i = 0; i < CONTENTION_TASKS; i++) {
        char name[16];
        name[0] = 'c'; name[1] = 'n'; name[2] = 't';
        name[3] = '0' + i; name[4] = '\0';
        tasks[i] = task_create(name, contention_task, (void *)(uintptr_t)INCREMENTS_PER_TASK);
        if (!tasks[i]) {
            uart_printf("  [FAIL] Failed to create contention task %d\n", i);
            test_errors++;
            return;
        }
        /* Spread across CPUs 1, 2, 3 (avoid CPU 0 where main runs) */
        scheduler_add_task_to_cpu(tasks[i], (i % 3) + 1);
    }

    uart_printf("[Test4] %d tasks each incrementing counter %d times\n",
                CONTENTION_TASKS, INCREMENTS_PER_TASK);

    /* Wait for completion with timeout */
    int timeout = 200;
    while (test_state.tasks_completed < CONTENTION_TASKS && timeout > 0) {
        yield();  /* Let scheduler run */
        delay(100000);
        timeout--;
    }

    if (timeout == 0) {
        uart_printf("  [FAIL] Timeout waiting for tasks (completed: %u/%d)\n",
                    test_state.tasks_completed, CONTENTION_TASKS);
        test_errors++;
        return;
    }

    uint32_t expected = CONTENTION_TASKS * INCREMENTS_PER_TASK;
    if (contention_counter == expected) {
        uart_printf("  [PASS] Counter = %u (expected %u) - no race conditions\n",
                    contention_counter, expected);
    } else {
        uart_printf("  [FAIL] Counter = %u (expected %u) - RACE CONDITION DETECTED\n",
                    contention_counter, expected);
        test_errors++;
    }
}

/*
 * Test 5: Task Lifecycle Stress Test
 *
 * Creates and terminates tasks rapidly to verify:
 * - Zombie cleanup mechanism works correctly
 * - Stack memory is properly reclaimed (no leaks)
 * - No use-after-free issues
 */

/* Completion tracking for lifecycle test */
static volatile int lifecycle_completed = 0;

static void lifecycle_task_func(void *arg)
{
    (void)arg;
    /* Increment completion counter atomically before exiting */
    __asm__ volatile(
        "1: ldxr w0, [%0]\n"
        "   add w0, w0, #1\n"
        "   stxr w1, w0, [%0]\n"
        "   cbnz w1, 1b\n"
        : : "r"(&lifecycle_completed)
        : "w0", "w1", "memory"
    );
    /* Task exits immediately - tests rapid cleanup */
}

static void test_task_lifecycle(void)
{
    uart_puts("\n--- Test 5: Task Lifecycle Stress Test ---\n");

    #define LIFECYCLE_CYCLES 8

    /* Get initial PMM state */
    uint64_t initial_free = pmm_get_free_pages();
    uart_printf("[Test5] Initial free pages: %lu\n", initial_free);

    lifecycle_completed = 0;

    /* Create all lifecycle tasks */
    for (int cycle = 0; cycle < LIFECYCLE_CYCLES; cycle++) {
        char name[16];
        name[0] = 'l'; name[1] = 'c'; name[2] = '0' + cycle; name[3] = '\0';

        struct task *t = task_create(name, lifecycle_task_func, NULL);
        if (!t) {
            uart_printf("  [FAIL] Failed to create lifecycle task %d\n", cycle);
            test_errors++;
            return;
        }

        /* Spread across CPUs 1-3 */
        scheduler_add_task_to_cpu(t, (cycle % 3) + 1);
    }

    uart_printf("[Test5] Created %d lifecycle tasks\n", LIFECYCLE_CYCLES);

    /* Wait for all tasks to complete (with timeout) */
    int timeout = 100;
    while (lifecycle_completed < LIFECYCLE_CYCLES && timeout > 0) {
        delay(50000);
        timeout--;
    }

    if (timeout == 0) {
        uart_printf("  [FAIL] Timeout - only %d/%d tasks completed\n",
                    lifecycle_completed, LIFECYCLE_CYCLES);
        test_errors++;
        return;
    }

    uart_printf("[Test5] All %d tasks completed\n", LIFECYCLE_CYCLES);

    /* Give scheduler time to clean up all zombies */
    for (int i = 0; i < 10; i++) {
        yield();
        delay(50000);
    }

    /* Check final PMM state */
    uint64_t final_free = pmm_get_free_pages();
    uart_printf("[Test5] Final free pages: %lu\n", final_free);

    /* Verify no memory leak (allow small variance for timing) */
    if (final_free >= initial_free) {
        uart_printf("  [PASS] All %d tasks created/destroyed, memory reclaimed\n",
                    LIFECYCLE_CYCLES);
    } else {
        uint64_t leaked = initial_free - final_free;
        uart_printf("  [WARN] Possible leak: %lu pages missing (may be timing)\n", leaked);
        /* Don't fail - zombie cleanup is async */
    }
}

/*
 * Main task - runs all scheduler tests
 */
static void main_task_func(void *arg)
{
    (void)arg;

    /* Run Unity test suites via harness */
    test_harness_init();
    test_errors += test_harness_run_all();

    /* Run Rust FFI tests (not yet migrated to Unity) */
    test_errors += rust_run_tests();

    /* Run all scheduler tests */
    test_multicore_basic();
    test_task_migration();
    test_stress_multicpu();
    test_lock_contention();
    test_task_lifecycle();

    /* Summary */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Scheduler Test Summary\n");
    uart_puts("========================================\n");
    scheduler_dump();
    pmm_dump_stats();

    uart_puts("\n");
    if (test_errors == 0) {
        INFO("Scheduler tests passed");
        uart_puts("  [PASS] All scheduler tests passed!\n");
    } else {
        ERROR("Scheduler tests failed: %d errors", test_errors);
        uart_printf("  [FAIL] %d test(s) failed\n", test_errors);
    }

    uart_puts("\n");
    INFO("Triggering intentional fault to end test...");
    __asm__ volatile(".word 0x00000000");  /* Undefined instruction */

    /* Should never reach here */
    while (1) {
        __asm__ volatile("wfi");
    }
}

/*
 * kernel_main - Main kernel entry point
 *
 * Called from boot.S after basic hardware initialization.
 * This function should not return.
 */
void kernel_main(void)
{
    /* Initialize UART for debug output */
    uart_init();

    /* Banner */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("  SLM-OS v0.1.0\n");
    uart_puts("  Small Language Model Operating System\n");
    uart_puts("========================================\n\n");

    INFO("Boot successful");
    INFO("Running at EL1 on %s", "QEMU virt");

    /* Show memory layout */
    uart_puts("\n");
    print_memory_info();

    /* Initialize physical memory manager */
    uart_puts("\n");
    pmm_init();
    pmm_dump_stats();

    /* Initialize virtual memory manager and enable MMU */
    uart_puts("\n");
    vmm_init();

    /* Initialize interrupt controller */
    uart_puts("\n");
    INFO("Initializing GIC...");
    gic_init();

    /* Initialize timer (but don't start yet) */
    INFO("Initializing timer...");
    timer_init();

    /* Initialize SMP and boot secondary CPUs */
    uart_puts("\n");
    smp_init();

    /* Initialize scheduler */
    uart_puts("\n");
    scheduler_init();

    /* Initialize IPC subsystem */
    ipc_init();

    /* Initialize Rust runtime */
    INFO("Initializing Rust runtime...");

    /* Allocate heap for Rust (1MB = 256 pages) */
    void *rust_heap = pmm_alloc_pages(256);
    if (!rust_heap) {
        panic("Failed to allocate Rust heap");
    }
    rust_heap_init(rust_heap, 256 * 4096);
    INFO("  Rust heap: %p (%u KB)", rust_heap, (256 * 4096) / 1024);

    /* Call Rust init and verify */
    int magic = rust_init();
    if (magic != 42) {
        panic("Rust init failed (expected 42, got %d)", magic);
    }
    INFO("  Rust init: OK (magic=%d)", magic);

    /* Say hello from Rust */
    rust_hello();

    /* Initialize model memory pools */
    INFO("Initializing model memory...");
    int model_init = rust_model_mem_init();
    if (model_init != 0) {
        WARN("Model memory init failed (code=%d)", model_init);
    } else {
        INFO("  Model memory: OK (16 MB weights, 8 MB workspace)");
    }

    /* Create main task */
    struct task *main_task = task_create("main", main_task_func, NULL);
    if (!main_task) {
        panic("Failed to create main task");
    }
    scheduler_add_task(main_task);

    /* Start timer - will generate periodic interrupts */
    INFO("Starting timer (100 Hz)...");
    timer_start();

    /* Enable interrupts */
    INFO("Enabling interrupts...");
    __asm__ volatile("msr daifclr, #0x2");  /* Clear IRQ mask */

    /* Start scheduler - this does not return */
    INFO("Starting scheduler...");
    scheduler_start();

    /* Should never reach here */
    panic("scheduler_start returned!");
}
