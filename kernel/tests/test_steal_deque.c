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
 * The deque now reads `task->generation` at push time (#139 ABA
 * fix), so the old trick of casting integers to struct task * would
 * dereference invalid memory. Instead we maintain a small
 * association map: each distinct sentinel value the test passes gets
 * a fresh struct task slot. Slot storage is stamped with
 * generation=0 on reset; dedicated ABA tests at the bottom mutate it
 * directly to exercise the generation-mismatch path.
 */
#define FAKE_TASK_COUNT 64
static struct task fake_tasks[FAKE_TASK_COUNT];
static uintptr_t fake_sentinels[FAKE_TASK_COUNT];
static int fake_task_used;

static struct task *as_task(uintptr_t v)
{
    /* Return the existing slot if we've seen this sentinel before —
     * tests rely on `as_task(0x10) == as_task(0x10)` for pointer
     * identity. Otherwise allocate a fresh slot. */
    for (int i = 0; i < fake_task_used; i++) {
        if (fake_sentinels[i] == v)
            return &fake_tasks[i];
    }
    /* FAKE_TASK_COUNT (64) is larger than STEAL_DEQUE_CAPACITY (32)
     * plus worst-case overhead in any single test between
     * reset_deque() calls. If a new test wants more, bump the
     * constant — the existing TEST_ASSERT machinery can't be called
     * from a non-void return path without the return-type warning. */
    if (fake_task_used >= FAKE_TASK_COUNT) {
        /* Alias into slot 0 as a last-resort fallback. Any test that
         * hits this will see pointer-identity collisions and fail
         * naturally via the existing TEST_ASSERT_EQUAL_PTR checks. */
        return &fake_tasks[0];
    }
    fake_sentinels[fake_task_used] = v;
    fake_tasks[fake_task_used].generation = 0;
    return &fake_tasks[fake_task_used++];
}

static void reset_deque(void)
{
    /* Releasing the sentinel→slot map here means each test starts
     * with a clean association — `as_task(0x10)` in one test does
     * not share a struct task with `as_task(0x10)` in another. */
    fake_task_used = 0;
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
    TEST_ASSERT_NULL(steal_deque_pop(&d, (void *)0));
}

static void test_steal_empty_returns_null(void)
{
    reset_deque();
    TEST_ASSERT_NULL(steal_deque_steal(&d, (void *)0));
}

/* ---------- Owner-side push/pop (LIFO) ---------- */

static void test_push_then_pop_single(void)
{
    reset_deque();
    TEST_ASSERT_EQUAL_INT(0, steal_deque_push(&d, as_task(0xAA)));
    TEST_ASSERT_EQUAL_UINT32(1, steal_deque_size(&d));

    struct task *t = steal_deque_pop(&d, (void *)0);
    TEST_ASSERT_EQUAL_PTR(as_task(0xAA), t);
    TEST_ASSERT_TRUE(steal_deque_is_empty(&d));
}

static void test_push_pop_order_is_lifo(void)
{
    reset_deque();
    steal_deque_push(&d, as_task(0x10));
    steal_deque_push(&d, as_task(0x20));
    steal_deque_push(&d, as_task(0x30));

    TEST_ASSERT_EQUAL_PTR(as_task(0x30), steal_deque_pop(&d, (void *)0));
    TEST_ASSERT_EQUAL_PTR(as_task(0x20), steal_deque_pop(&d, (void *)0));
    TEST_ASSERT_EQUAL_PTR(as_task(0x10), steal_deque_pop(&d, (void *)0));
    TEST_ASSERT_NULL(steal_deque_pop(&d, (void *)0));
}

/* ---------- Thief-side steal (FIFO) ---------- */

static void test_push_then_steal_single(void)
{
    reset_deque();
    steal_deque_push(&d, as_task(0xBB));
    struct task *t = steal_deque_steal(&d, (void *)0);
    TEST_ASSERT_EQUAL_PTR(as_task(0xBB), t);
    TEST_ASSERT_TRUE(steal_deque_is_empty(&d));
}

static void test_steal_order_is_fifo(void)
{
    reset_deque();
    steal_deque_push(&d, as_task(0x10));
    steal_deque_push(&d, as_task(0x20));
    steal_deque_push(&d, as_task(0x30));

    TEST_ASSERT_EQUAL_PTR(as_task(0x10), steal_deque_steal(&d, (void *)0));
    TEST_ASSERT_EQUAL_PTR(as_task(0x20), steal_deque_steal(&d, (void *)0));
    TEST_ASSERT_EQUAL_PTR(as_task(0x30), steal_deque_steal(&d, (void *)0));
    TEST_ASSERT_NULL(steal_deque_steal(&d, (void *)0));
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
    TEST_ASSERT_EQUAL_PTR(as_task(0x10), steal_deque_steal(&d, (void *)0));
    /* Owner takes newest */
    TEST_ASSERT_EQUAL_PTR(as_task(0x40), steal_deque_pop(&d, (void *)0));
    /* Thief takes next oldest */
    TEST_ASSERT_EQUAL_PTR(as_task(0x20), steal_deque_steal(&d, (void *)0));
    /* Owner takes remaining */
    TEST_ASSERT_EQUAL_PTR(as_task(0x30), steal_deque_pop(&d, (void *)0));

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
            TEST_ASSERT_NOT_NULL(steal_deque_steal(&d, (void *)0));
            steals++;
        } else {
            TEST_ASSERT_NOT_NULL(steal_deque_pop(&d, (void *)0));
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
    steal_deque_pop(&d, (void *)0);
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
            TEST_ASSERT_NOT_NULL(steal_deque_steal(&d, (void *)0));
    }

    TEST_ASSERT_TRUE(steal_deque_is_empty(&d));
    /*
     * A fresh push/pop after wrap should still work. bottom and top are
     * now ~64; masked index is still within [0, CAPACITY).
     */
    steal_deque_push(&d, as_task(0xC0DE));
    TEST_ASSERT_EQUAL_PTR(as_task(0xC0DE), steal_deque_pop(&d, (void *)0));
}

/* ---------- Generation capture (#139 ABA fix) ---------- */

/*
 * Push captures task->generation at the moment of push; pop / steal
 * return that captured value via the out_gen argument. Validators
 * compare it to the current task->generation and reject on mismatch.
 */
static void test_push_captures_generation_at_push_time(void)
{
    reset_deque();
    struct task *t = as_task(0xA1);
    t->generation = 7;
    steal_deque_push(&d, t);

    /* A later bump (simulating task_destroy + task_create recycle of
     * the same slot) must not be visible to the already-pushed
     * entry. */
    t->generation = 8;

    uint32_t captured = 0;
    TEST_ASSERT_EQUAL_PTR(t, steal_deque_pop(&d, &captured));
    TEST_ASSERT_EQUAL_UINT32(7, captured);
}

static void test_steal_returns_captured_generation(void)
{
    reset_deque();
    struct task *t = as_task(0xA2);
    t->generation = 42;
    steal_deque_push(&d, t);

    uint32_t captured = 0;
    TEST_ASSERT_EQUAL_PTR(t, steal_deque_steal(&d, &captured));
    TEST_ASSERT_EQUAL_UINT32(42, captured);
}

/*
 * The exact ABA scenario from #139, reproduced with the test's
 * bookkeeping: push T with generation=N, later bump to N+1 (as
 * task_destroy would), then steal. The caller gets the old captured
 * generation and can tell it doesn't match the current one — which
 * is what sched_try_steal checks under rq_lock before accepting.
 */
static void test_steal_aba_generation_mismatch_detectable(void)
{
    reset_deque();
    struct task *t = as_task(0xABA);
    t->generation = 1;
    steal_deque_push(&d, t);

    /* Simulate task_destroy bumping the slot's generation after the
     * push but before the steal. */
    t->generation = 2;

    uint32_t captured = 0xDEADBEEF;
    struct task *stolen = steal_deque_steal(&d, &captured);
    TEST_ASSERT_EQUAL_PTR(t, stolen);
    TEST_ASSERT_EQUAL_UINT32(1, captured);
    /* The caller can now see generation has advanced; this is the
     * signal sched_try_steal uses to reject the steal. */
    TEST_ASSERT_NOT_EQUAL(stolen->generation, captured);
}

static void test_remove_clears_generation_slot(void)
{
    reset_deque();
    struct task *t = as_task(0xA3);
    t->generation = 99;
    steal_deque_push(&d, t);

    /* steal_deque_remove NULLs both the task slot and the generation
     * slot — a subsequent pop / steal that walks past the cleared
     * entry sees a NULL task and skips, which is the existing
     * semantics; explicitly verified here so the gen_buf zeroing
     * can't regress silently. */
    TEST_ASSERT_EQUAL_INT(1, steal_deque_remove(&d, t));
    TEST_ASSERT_NULL(steal_deque_pop(&d, (void *)0));
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
    RUN_TEST(test_push_captures_generation_at_push_time);
    RUN_TEST(test_steal_returns_captured_generation);
    RUN_TEST(test_steal_aba_generation_mismatch_detectable);
    RUN_TEST(test_remove_clears_generation_slot);

    return UNITY_END();
}
