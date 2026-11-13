/*
 * Fault-dump lock tests (kernel/include/fault_dump.h).
 *
 * The lock primitive is tested on private lock instances, so these run in
 * every boot-test kernel without touching the real dump state:
 *   - a free lock is taken, a held one is reported as nested for its owner;
 *   - a holder that prints nothing makes a waiter give up after the stall
 *     bound, measured on the hardware counter (about 50 ms), not never;
 *   - a holder that keeps printing holds a waiter off until the total cap;
 *   - a counter that stops moving cannot turn the wait into a hang.
 *
 * FAULT_DUMP_SELFTEST=ON (default OFF; nothing below the #if is built
 * without it) adds test_fault_dump_two_cpus_concurrent, which faults
 * CPU 1 (data abort) and CPU 2 (BRK, panic path) at the same instant and
 * waits for both dumps. The harness then runs only that test, because
 * CPUs 1 and 2 stay halted. The run log is the evidence: two complete
 * dumps, one after the other, every line prefixed with its CPU.
 */

#include "unity.h"
#include "../include/fault_dump.h"
#include "../include/timer.h"
#include "../include/smp.h"
#include <stdbool.h>
#include <stdint.h>

static uint64_t ms_to_ticks(uint64_t ms)
{
    return timer_get_frequency() * ms / 1000u;
}

static uint64_t real_now(void)
{
    return timer_get_count();
}

static void test_fault_dump_lock_free_is_acquired_and_released(void)
{
    struct fault_dump_lock l = { 0 };
    TEST_ASSERT_EQUAL_INT(FAULT_DUMP_ACQUIRED,
                          fault_dump_lock_acquire(&l, 3, ms_to_ticks(50),
                                                  ms_to_ticks(5000), real_now));
    TEST_ASSERT_EQUAL_UINT32(4, l.owner);
    /* Another CPU's release does nothing. */
    fault_dump_lock_release(&l, 1);
    TEST_ASSERT_EQUAL_UINT32(4, l.owner);
    fault_dump_lock_release(&l, 3);
    TEST_ASSERT_EQUAL_UINT32(0, l.owner);
}

/* A fault inside a dump must not wait on its own CPU. */
static void test_fault_dump_lock_owner_reenters_as_nested(void)
{
    struct fault_dump_lock l = { .owner = 2 + 1, .progress = 0 };
    uint64_t t0 = timer_get_count();
    TEST_ASSERT_EQUAL_INT(FAULT_DUMP_NESTED,
                          fault_dump_lock_acquire(&l, 2, ms_to_ticks(50),
                                                  ms_to_ticks(5000), real_now));
    TEST_ASSERT_TRUE(timer_get_count() - t0 < ms_to_ticks(5));
    TEST_ASSERT_EQUAL_UINT32(3, l.owner);
}

/* A silent holder (faulted or hung mid-dump) delays a waiter by the stall
 * bound, measured on the counter, then the waiter dumps anyway. */
static void test_fault_dump_lock_silent_holder_times_out_at_stall(void)
{
    struct fault_dump_lock l = { .owner = 5 + 1, .progress = 0 };
    uint64_t t0 = timer_get_count();
    fault_dump_entry_t e = fault_dump_lock_acquire(&l, 0, ms_to_ticks(50),
                                                   ms_to_ticks(5000), real_now);
    uint64_t dt = timer_get_count() - t0;
    TEST_ASSERT_EQUAL_INT(FAULT_DUMP_STALLED, e);
    TEST_ASSERT_TRUE(dt >= ms_to_ticks(50));
    TEST_ASSERT_TRUE(dt < ms_to_ticks(1000));   /* bounded, with room for host load */
    TEST_ASSERT_EQUAL_UINT32(6, l.owner);    /* the holder keeps it */
}

/* Fake counter: advances 1 ms per read; every 10th read the "holder"
 * prints a line (progress changes), so the stall bound never expires and
 * the waiter stops at the total cap instead. */
static struct fault_dump_lock *busy_lock;
static uint64_t fake_ticks;
static uint32_t fake_reads;

static uint64_t fake_now_busy_holder(void)
{
    fake_ticks += ms_to_ticks(1);
    if (++fake_reads % 10u == 0) {
        busy_lock->progress = fake_ticks;
    }
    return fake_ticks;
}

static void test_fault_dump_lock_printing_holder_holds_until_cap(void)
{
    struct fault_dump_lock l = { .owner = 1 + 1, .progress = 0 };
    busy_lock = &l;
    fake_ticks = 0;
    fake_reads = 0;
    fault_dump_entry_t e = fault_dump_lock_acquire(&l, 0, ms_to_ticks(50),
                                                   ms_to_ticks(5000),
                                                   fake_now_busy_holder);
    TEST_ASSERT_EQUAL_INT(FAULT_DUMP_CAPPED, e);
    TEST_ASSERT_TRUE(fake_ticks >= ms_to_ticks(5000));
    TEST_ASSERT_TRUE(fake_ticks < ms_to_ticks(5100));
}

/* A holder that releases is followed by the waiter taking the lock. */
static struct fault_dump_lock *release_lock;
static uint32_t release_reads;

static uint64_t fake_now_holder_releases(void)
{
    if (++release_reads == 20u) {
        release_lock->owner = 0;
    }
    return timer_get_count();
}

static void test_fault_dump_lock_waiter_gets_lock_after_release(void)
{
    struct fault_dump_lock l = { .owner = 4 + 1, .progress = 0 };
    release_lock = &l;
    release_reads = 0;
    TEST_ASSERT_EQUAL_INT(FAULT_DUMP_ACQUIRED,
                          fault_dump_lock_acquire(&l, 1, ms_to_ticks(50),
                                                  ms_to_ticks(5000),
                                                  fake_now_holder_releases));
    TEST_ASSERT_EQUAL_UINT32(2, l.owner);
}

/* A frozen counter (x86-64 PIT fallback with IRQs off) ends the wait
 * after a bounded number of polls instead of spinning forever. */
static uint64_t frozen_now(void)
{
    return 12345u;
}

static void test_fault_dump_lock_frozen_counter_cannot_hang(void)
{
    struct fault_dump_lock l = { .owner = 7 + 1, .progress = 0 };
    TEST_ASSERT_EQUAL_INT(FAULT_DUMP_CAPPED,
                          fault_dump_lock_acquire(&l, 0, ms_to_ticks(50),
                                                  ms_to_ticks(5000), frozen_now));
}

/* Outside a dump nothing is prefixed and the UART lock is used normally. */
static void test_fault_dump_idle_state(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, fault_dump_cpus);
    TEST_ASSERT_FALSE(fault_dump_this_cpu());
}

#if defined(FAULT_DUMP_SELFTEST) && !defined(PLATFORM_X86_64)
#include "../include/task.h"
#include "../include/sched.h"
#include "../include/uart.h"

static volatile uint32_t fd_ready[2];
static volatile uint32_t fd_cpu[2];
static volatile uint32_t fd_go;

static void fd_fault_task(void *arg)
{
    uint32_t which = (uint32_t)(uintptr_t)arg;
    __asm__ volatile("msr daifset, #0x2" ::: "memory");   /* no preemption */
    fd_cpu[which] = cpu_id();
    __atomic_store_n(&fd_ready[which], 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&fd_go, __ATOMIC_ACQUIRE) == 0) {
    }
    if (which == 0) {
        /* Data abort: page-fault dump. */
        volatile uint64_t *bad = (volatile uint64_t *)(uintptr_t)0x00007F0000000000ULL;
        (void)*bad;
    } else {
        /* BRK: panic() dump. */
        __asm__ volatile("brk #0x7");
    }
    for (;;) {
    }
}

static void test_fault_dump_two_cpus_concurrent(void)
{
    if (cpu_count < 3) {
        TEST_IGNORE_MESSAGE("needs CPUs 1 and 2");
        return;
    }
    uint32_t before = fault_dump_completed();
    struct task *a = task_create("fd_cpu1", fd_fault_task, (void *)(uintptr_t)0);
    struct task *b = task_create("fd_cpu2", fd_fault_task, (void *)(uintptr_t)1);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    scheduler_add_task_to_cpu(a, 1);
    scheduler_add_task_to_cpu(b, 2);

    uint64_t f = timer_get_frequency();
    uint64_t t0 = timer_get_count();
    while (!(fd_ready[0] && fd_ready[1]) && timer_get_count() - t0 < 2 * f) {
        yield();
    }
    TEST_ASSERT_TRUE(fd_ready[0] && fd_ready[1]);
    TEST_ASSERT_TRUE(fd_cpu[0] != fd_cpu[1]);
    uart_printf("[fault_dump selftest] faulting cpu%u (data abort) and cpu%u (brk) now\n",
                fd_cpu[0], fd_cpu[1]);

    __atomic_store_n(&fd_go, 1u, __ATOMIC_RELEASE);

    t0 = timer_get_count();
    while (fault_dump_completed() - before < 2 && timer_get_count() - t0 < 20 * f) {
    }
    uart_printf("[fault_dump selftest] dumps completed: %u of 2\n",
                fault_dump_completed() - before);
    TEST_ASSERT_EQUAL_UINT32(2, fault_dump_completed() - before);
}
#endif

int test_suite_fault_dump(void)
{
    UnityBegin("Fault Dump Lock Tests");

    RUN_TEST(test_fault_dump_lock_free_is_acquired_and_released);
    RUN_TEST(test_fault_dump_lock_owner_reenters_as_nested);
    RUN_TEST(test_fault_dump_lock_silent_holder_times_out_at_stall);
    RUN_TEST(test_fault_dump_lock_printing_holder_holds_until_cap);
    RUN_TEST(test_fault_dump_lock_waiter_gets_lock_after_release);
    RUN_TEST(test_fault_dump_lock_frozen_counter_cannot_hang);
    RUN_TEST(test_fault_dump_idle_state);
#if defined(FAULT_DUMP_SELFTEST) && !defined(PLATFORM_X86_64)
    RUN_TEST(test_fault_dump_two_cpus_concurrent);
#endif

    return UnityEnd();
}
