/*
 * test_percpu_layout.c - Every hot per-CPU element owns its cache line (#1263)
 *
 * rq_lock[] and steal_deque_lock[] used to be aligned as arrays only, so
 * every lock word shared one cache line (one exclusives reservation
 * granule) and a CPU could fail to take its own free lock for milliseconds
 * while another CPU stored to a neighbouring lock. The same packing put
 * preempt_disabled[], the sched_diag counters, current_task[] and the other
 * per-CPU scheduler words of all CPUs into shared lines.
 *
 * These tests take the address of element 0 and element 1 of each such
 * array in the running kernel and fail if the elements are closer than a
 * cache line, are not line-aligned, or do not fill whole lines. A change
 * that puts SPINLOCK_ARRAY or a per-CPU cell back to a packed element
 * fails here (and at the static asserts next to the types).
 *
 * resched_trampoline (vectors.S) indexes preempt_disabled[], orig_elr[]
 * and orig_spsr[] by `cpu << CACHE_LINE_SHIFT` and stores at offset 0; the
 * asserts below pin the C layout to that.
 */

#include "unity.h"
#include "test_harness.h"
#include "../include/cacheline.h"
#include "../include/percpu_layout.h"
#include "../include/sched.h"
#include "../include/sched_diag.h"
#include "../include/spinlock.h"
#include "../include/steal_deque.h"
#include "../include/task.h"
#include "../include/config.h"
#include "../include/uart.h"
#if defined(SECONDARY_PREEMPT) && !defined(PLATFORM_X86_64)
#include "../include/preempt.h"
#endif
#if defined(PLATFORM_X86_64)
#include "../arch/x86_64/x86_smp.h"
#endif
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* What the assembly assumes. */
_Static_assert(sizeof(percpu_int_t) == (1U << CACHE_LINE_SHIFT) &&
               offsetof(percpu_int_t, v) == 0,
               "vectors.S: preempt_disabled[cpu].v is at cpu << CACHE_LINE_SHIFT");
_Static_assert(sizeof(percpu_u64_t) == (1U << CACHE_LINE_SHIFT) &&
               offsetof(percpu_u64_t, v) == 0,
               "vectors.S: orig_elr/orig_spsr[cpu].v is at cpu << CACHE_LINE_SHIFT");
_Static_assert(sizeof(preempt_disabled[0]) == CACHE_LINE_SIZE,
               "preempt_disabled[] must have one cache line per CPU");

/* The lock array element. */
_Static_assert(sizeof(spinlock_padded_t) == CACHE_LINE_SIZE &&
               _Alignof(spinlock_padded_t) == CACHE_LINE_SIZE &&
               offsetof(spinlock_padded_t, lock) == 0,
               "SPINLOCK_ARRAY element must be one cache line");

/* Per-CPU structs: whole lines each. */
CACHE_LINE_ASSERT_PADDED(struct cpu_runqueue);
CACHE_LINE_ASSERT_PADDED(struct sched_diag_cpu);
CACHE_LINE_ASSERT_PADDED(steal_deque_t);

static void check_ent(const struct percpu_layout_ent *e)
{
    uintptr_t a0 = (uintptr_t)e->e0;
    uintptr_t a1 = (uintptr_t)e->e1;

    uart_printf("  [percpu] %-32s base %p stride %u size %u count %u\n",
                e->name, (void *)a0, (unsigned)(a1 - a0),
                (unsigned)e->elem_size, (unsigned)e->count);

    TEST_ASSERT_TRUE_MESSAGE((a0 % CACHE_LINE_SIZE) == (0), e->name);
    TEST_ASSERT_TRUE_MESSAGE((a1 % CACHE_LINE_SIZE) == (0), e->name);
    TEST_ASSERT_TRUE_MESSAGE(a1 - a0 >= CACHE_LINE_SIZE, e->name);
    TEST_ASSERT_TRUE_MESSAGE((a1 - a0) == (e->elem_size), e->name);
    TEST_ASSERT_TRUE_MESSAGE((e->elem_size % CACHE_LINE_SIZE) == (0), e->name);
    TEST_ASSERT_TRUE_MESSAGE((e->count) == (MAX_CPUS), e->name);
}

/* A packed array must fail the checker's conditions: the check is not
 * vacuous. Evaluated by hand so the suite itself stays green. */
static void test_checker_rejects_a_packed_array(void)
{
    static volatile uint32_t packed[MAX_CPUS] __attribute__((aligned(64)));
    uintptr_t a0 = (uintptr_t)&packed[0];
    uintptr_t a1 = (uintptr_t)&packed[1];

    /* This is the old SPINLOCK_ARRAY layout: aligned array, 4-byte stride. */
    TEST_ASSERT_EQUAL_UINT64(0, a0 % CACHE_LINE_SIZE);
    TEST_ASSERT_TRUE(a1 - a0 < CACHE_LINE_SIZE);
    TEST_ASSERT_TRUE(a1 % CACHE_LINE_SIZE != 0);
    /* All eight words sit in the line of element 0. */
    TEST_ASSERT_EQUAL_UINT64(a0 / CACHE_LINE_SIZE,
                             (uintptr_t)&packed[MAX_CPUS - 1] / CACHE_LINE_SIZE);
}

static void test_sched_static_arrays_one_line_per_cpu(void)
{
#if defined(ENABLE_BOOT_TESTS)
    const struct percpu_layout_ent *tab = NULL;
    size_t n = sched_percpu_layout(&tab);

    TEST_ASSERT_NOT_NULL(tab);
    /* rq_lock, run queues, switch_in_waiting, last-tick stamps at least. */
    TEST_ASSERT_TRUE(n >= 4);
    for (size_t i = 0; i < n; i++) {
        check_ent(&tab[i]);
    }
#else
    TEST_IGNORE_MESSAGE("test kernel only");
#endif
}

static void test_rq_lock_is_first_and_padded(void)
{
#if defined(ENABLE_BOOT_TESTS)
    /* The array #1263 is about, by name: one line per lock, and the lock
     * word alone at the start of its line. */
    const struct percpu_layout_ent *tab = NULL;
    size_t n = sched_percpu_layout(&tab);
    bool found_rq = false;
    bool found_steal = false;

    for (size_t i = 0; i < n; i++) {
        bool rq = strcmp(tab[i].name, "rq_lock") == 0;
        bool sd = strcmp(tab[i].name, "steal_deque_lock") == 0;
        if (rq || sd) {
            TEST_ASSERT_EQUAL_UINT64(CACHE_LINE_SIZE, tab[i].elem_size);
            TEST_ASSERT_EQUAL_UINT64(CACHE_LINE_SIZE,
                                     (uintptr_t)tab[i].e1 - (uintptr_t)tab[i].e0);
            found_rq = found_rq || rq;
            found_steal = found_steal || sd;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_rq, "rq_lock missing from the layout table");
#if CONFIG_WORK_STEALING
    TEST_ASSERT_TRUE_MESSAGE(found_steal, "steal_deque_lock missing from the layout table");
#else
    (void)found_steal;
#endif
#else
    TEST_IGNORE_MESSAGE("test kernel only");
#endif
}

static void test_global_arrays_one_line_per_cpu(void)
{
    const struct percpu_layout_ent ents[] = {
        PERCPU_LAYOUT_ENT(preempt_disabled),
        PERCPU_LAYOUT_ENT(sched_diag),
#if defined(PLATFORM_X86_64)
        PERCPU_LAYOUT_ENT(smp_resched_ipi_count),
#endif
    };

    for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        check_ent(&ents[i]);
    }
    /* The value the assembly stores is the first word of the line. */
    TEST_ASSERT_EQUAL_PTR(&preempt_disabled[1], &preempt_disabled[1].v);
}

static void test_current_task_slots_one_line_per_cpu(void)
{
#if defined(ENABLE_BOOT_TESTS)
    for (uint32_t cpu = 0; cpu + 1 < MAX_CPUS; cpu++) {
        uintptr_t a = (uintptr_t)task_current_slot_addr(cpu);
        uintptr_t b = (uintptr_t)task_current_slot_addr(cpu + 1);

        TEST_ASSERT_EQUAL_UINT64(0, a % CACHE_LINE_SIZE);
        TEST_ASSERT_EQUAL_UINT64(CACHE_LINE_SIZE, b - a);
    }
#else
    TEST_IGNORE_MESSAGE("test kernel only");
#endif
}

static void test_preempt_state_one_line_per_cpu(void)
{
#if defined(SECONDARY_PREEMPT) && !defined(PLATFORM_X86_64)
    /* Pointers set by preempt_init(); the trampoline indexes the last two
     * with CACHE_LINE_SHIFT. */
    TEST_ASSERT_NOT_NULL(reschedule_pending);
    TEST_ASSERT_NOT_NULL(orig_elr);
    TEST_ASSERT_NOT_NULL(orig_spsr);

    const struct percpu_layout_ent ents[] = {
        { "reschedule_pending", &reschedule_pending[0], &reschedule_pending[1],
          sizeof(reschedule_pending[0]), MAX_CPUS },
        { "orig_elr", &orig_elr[0], &orig_elr[1], sizeof(orig_elr[0]), MAX_CPUS },
        { "orig_spsr", &orig_spsr[0], &orig_spsr[1], sizeof(orig_spsr[0]), MAX_CPUS },
    };
    for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        check_ent(&ents[i]);
        TEST_ASSERT_EQUAL_UINT64(1U << CACHE_LINE_SHIFT,
                                 (uintptr_t)ents[i].e1 - (uintptr_t)ents[i].e0);
    }
#else
    TEST_IGNORE_MESSAGE("SECONDARY_PREEMPT not built");
#endif
}

static void test_pmu_arrays_one_line_per_cpu(void)
{
#if defined(ENABLE_BOOT_TESTS) && !defined(PLATFORM_X86_64)
    const struct percpu_layout_ent *tab = NULL;
    size_t n = pmu_percpu_layout(&tab);

    TEST_ASSERT_NOT_NULL(tab);
    TEST_ASSERT_EQUAL_UINT64(2, n);
    for (size_t i = 0; i < n; i++) {
        check_ent(&tab[i]);
    }
#else
    TEST_IGNORE_MESSAGE("ARM64 test kernel only");
#endif
}

/* The locks still lock: element i's lock word is the one spin_trylock
 * takes, and taking lock 0 leaves lock 1 free. */
static void test_spinlock_array_elements_are_independent(void)
{
    static SPINLOCK_ARRAY(locks, 2);

    spin_init(SPINLOCK_AT(locks, 0));
    spin_init(SPINLOCK_AT(locks, 1));
    TEST_ASSERT_EQUAL_UINT64(CACHE_LINE_SIZE,
                             (uintptr_t)SPINLOCK_AT(locks, 1) -
                             (uintptr_t)SPINLOCK_AT(locks, 0));
    TEST_ASSERT_EQUAL_UINT64(0, (uintptr_t)SPINLOCK_AT(locks, 0) % CACHE_LINE_SIZE);

    TEST_ASSERT_TRUE(spin_trylock(SPINLOCK_AT(locks, 0)));
    TEST_ASSERT_TRUE(spin_trylock(SPINLOCK_AT(locks, 1)));
    spin_unlock(SPINLOCK_AT(locks, 1));
    spin_unlock(SPINLOCK_AT(locks, 0));
    TEST_ASSERT_EQUAL_UINT32(0, SPINLOCK_AT(locks, 0)->lock);
    TEST_ASSERT_EQUAL_UINT32(0, SPINLOCK_AT(locks, 1)->lock);
}

int test_suite_percpu_layout(void)
{
    UnityBegin("test_percpu_layout.c");
    uart_puts("  [percpu_layout] one cache line per per-CPU element (#1263)\n");
    RUN_TEST(test_checker_rejects_a_packed_array);
    RUN_TEST(test_sched_static_arrays_one_line_per_cpu);
    RUN_TEST(test_rq_lock_is_first_and_padded);
    RUN_TEST(test_global_arrays_one_line_per_cpu);
    RUN_TEST(test_current_task_slots_one_line_per_cpu);
    RUN_TEST(test_preempt_state_one_line_per_cpu);
    RUN_TEST(test_pmu_arrays_one_line_per_cpu);
    RUN_TEST(test_spinlock_array_elements_are_independent);
    return UnityEnd();
}
