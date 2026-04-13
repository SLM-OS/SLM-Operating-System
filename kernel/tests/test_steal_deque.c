/*
 * test_steal_deque.c - Unit tests for the work-stealing deque (#59 Phase A)
 *
 * The deque is the Phase-A data structure for per-CPU work stealing. It
 * is not yet wired into the scheduler, so these tests exercise it in
 * isolation using small sentinel pointers cast to struct task *.
 *
 * Invariants exercised:
 *   - Empty after init; push/pop LIFO on owner side; push/steal FIFO for thieves.
 *   - Capacity is honored (push returns -1 when full).
 *   - Size advances/retreats correctly across interleaved push/pop/steal.
 *   - Modular arithmetic (monotonic indices, index mask) works after wrap.
 */

#include "unity.h"
#include "../include/steal_deque.h"
#include "../include/task.h"

#include <stdint.h>

static steal_deque_t d;

/*
 * The deque stores struct task *. We don't want to drag in the full
 * scheduler just to exercise pointer bookkeeping — cast integers to
 * task pointers and compare for identity.
 */
static struct task *as_task(uintptr_t v)
{
    return (struct task *)v;
}

static void reset_deque(void)
{
    steal_deque_init(&d);
}

/* ---------- Init ---------- */

static void test_init_empty(void)
{
    reset_deque();
    TEST_ASSERT_TRUE(steal_deque_is_empty(&d));
    TEST_ASSERT_EQUAL_UINT32(0, steal_deque_size(&d));
}

static void test_pop_empty_returns_null(void)
{
    reset_deque();
    TEST_ASSERT_NULL(steal_deque_pop(&d));
}

static void test_steal_empty_returns_null(void)
{
    reset_deque();
    TEST_ASSERT_NULL(steal_deque_steal(&d));
}

/* ---------- Owner-side push/pop (LIFO) ---------- */

static void test_push_then_pop_single(void)
{
    reset_deque();
    TEST_ASSERT_EQUAL_INT(0, steal_deque_push(&d, as_task(0xAA)));
    TEST_ASSERT_EQUAL_UINT32(1, steal_deque_size(&d));

    struct task *t = steal_deque_pop(&d);
    TEST_ASSERT_EQUAL_PTR(as_task(0xAA), t);
    TEST_ASSERT_TRUE(steal_deque_is_empty(&d));
}

static void test_push_pop_order_is_lifo(void)
{
    reset_deque();
    steal_deque_push(&d, as_task(0x10));
    steal_deque_push(&d, as_task(0x20));
    steal_deque_push(&d, as_task(0x30));

    TEST_ASSERT_EQUAL_PTR(as_task(0x30), steal_deque_pop(&d));
    TEST_ASSERT_EQUAL_PTR(as_task(0x20), steal_deque_pop(&d));
    TEST_ASSERT_EQUAL_PTR(as_task(0x10), steal_deque_pop(&d));
    TEST_ASSERT_NULL(steal_deque_pop(&d));
}

/* ---------- Thief-side steal (FIFO) ---------- */

static void test_push_then_steal_single(void)
{
    reset_deque();
    steal_deque_push(&d, as_task(0xBB));
    struct task *t = steal_deque_steal(&d);
    TEST_ASSERT_EQUAL_PTR(as_task(0xBB), t);
    TEST_ASSERT_TRUE(steal_deque_is_empty(&d));
}

static void test_steal_order_is_fifo(void)
{
    reset_deque();
    steal_deque_push(&d, as_task(0x10));
    steal_deque_push(&d, as_task(0x20));
    steal_deque_push(&d, as_task(0x30));

    TEST_ASSERT_EQUAL_PTR(as_task(0x10), steal_deque_steal(&d));
    TEST_ASSERT_EQUAL_PTR(as_task(0x20), steal_deque_steal(&d));
    TEST_ASSERT_EQUAL_PTR(as_task(0x30), steal_deque_steal(&d));
    TEST_ASSERT_NULL(steal_deque_steal(&d));
}

/* ---------- Interleaved owner + thief ---------- */

static void test_pop_and_steal_share_queue(void)
{
    reset_deque();
    steal_deque_push(&d, as_task(0x10));
    steal_deque_push(&d, as_task(0x20));
    steal_deque_push(&d, as_task(0x30));
    steal_deque_push(&d, as_task(0x40));

    /* Thief takes oldest */
    TEST_ASSERT_EQUAL_PTR(as_task(0x10), steal_deque_steal(&d));
    /* Owner takes newest */
    TEST_ASSERT_EQUAL_PTR(as_task(0x40), steal_deque_pop(&d));
    /* Thief takes next oldest */
    TEST_ASSERT_EQUAL_PTR(as_task(0x20), steal_deque_steal(&d));
    /* Owner takes remaining */
    TEST_ASSERT_EQUAL_PTR(as_task(0x30), steal_deque_pop(&d));

    TEST_ASSERT_TRUE(steal_deque_is_empty(&d));
}

static void test_empty_after_drain_by_mixed_ops(void)
{
    reset_deque();
    for (int i = 1; i <= 8; i++)
        steal_deque_push(&d, as_task((uintptr_t)i));

    TEST_ASSERT_EQUAL_UINT32(8, steal_deque_size(&d));

    /* Alternate pop and steal until empty */
    int pops = 0, steals = 0;
    while (!steal_deque_is_empty(&d)) {
        if ((pops + steals) & 1) {
            TEST_ASSERT_NOT_NULL(steal_deque_steal(&d));
            steals++;
        } else {
            TEST_ASSERT_NOT_NULL(steal_deque_pop(&d));
            pops++;
        }
    }
    TEST_ASSERT_EQUAL_INT(4, pops);
    TEST_ASSERT_EQUAL_INT(4, steals);
}

/* ---------- Capacity ---------- */

static void test_push_full_returns_error(void)
{
    reset_deque();
    for (uint32_t i = 0; i < STEAL_DEQUE_CAPACITY; i++)
        TEST_ASSERT_EQUAL_INT(0, steal_deque_push(&d, as_task(0x100 + i)));

    TEST_ASSERT_EQUAL_UINT32(STEAL_DEQUE_CAPACITY, steal_deque_size(&d));
    TEST_ASSERT_EQUAL_INT(-1, steal_deque_push(&d, as_task(0xDEAD)));
}

static void test_full_then_pop_reopens_space(void)
{
    reset_deque();
    for (uint32_t i = 0; i < STEAL_DEQUE_CAPACITY; i++)
        steal_deque_push(&d, as_task(0x200 + i));

    /* Full */
    TEST_ASSERT_EQUAL_INT(-1, steal_deque_push(&d, as_task(0xBEEF)));
    /* Pop one */
    steal_deque_pop(&d);
    /* Now has space */
    TEST_ASSERT_EQUAL_INT(0, steal_deque_push(&d, as_task(0xBEEF)));
}

/*
 * Push / steal through more than CAPACITY items total so the monotonic
 * top/bottom indices wrap past the mask boundary. A correctly-masked
 * implementation should still behave identically.
 */
static void test_indices_wrap_correctly(void)
{
    reset_deque();

    /* Drive total ops past STEAL_DEQUE_CAPACITY * 2 */
    uintptr_t counter = 1;
    for (int round = 0; round < 4; round++) {
        /* Fill partway */
        for (int i = 0; i < (int)(STEAL_DEQUE_CAPACITY / 2); i++)
            TEST_ASSERT_EQUAL_INT(0, steal_deque_push(&d, as_task(counter++)));

        /* Drain via steal (FIFO) */
        while (!steal_deque_is_empty(&d))
            TEST_ASSERT_NOT_NULL(steal_deque_steal(&d));
    }

    TEST_ASSERT_TRUE(steal_deque_is_empty(&d));
    /*
     * A fresh push/pop after wrap should still work. bottom and top are
     * now ~64; masked index is still within [0, CAPACITY).
     */
    steal_deque_push(&d, as_task(0xC0DE));
    TEST_ASSERT_EQUAL_PTR(as_task(0xC0DE), steal_deque_pop(&d));
}

/* ---------- Suite ---------- */

int test_suite_steal_deque(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_empty);
    RUN_TEST(test_pop_empty_returns_null);
    RUN_TEST(test_steal_empty_returns_null);
    RUN_TEST(test_push_then_pop_single);
    RUN_TEST(test_push_pop_order_is_lifo);
    RUN_TEST(test_push_then_steal_single);
    RUN_TEST(test_steal_order_is_fifo);
    RUN_TEST(test_pop_and_steal_share_queue);
    RUN_TEST(test_empty_after_drain_by_mixed_ops);
    RUN_TEST(test_push_full_returns_error);
    RUN_TEST(test_full_then_pop_reopens_space);
    RUN_TEST(test_indices_wrap_correctly);

    return UNITY_END();
}
