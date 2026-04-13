/*
 * Message Router Tests - Unity Framework
 *
 * Regression tests for the Rust message router (runtime/src/msg_router.rs).
 * The router is only available on ARM64 builds (Rust runtime is ARM64-only).
 */

#include "unity.h"
#include "../include/timer.h"
#include <stdint.h>

/* Rust FFI declarations (msg_router_publish is in slm_ffi.h, others are not) */
extern void msg_router_init(void);
extern int msg_router_subscribe(const char *topic_name, int component_idx);
extern void msg_router_unsubscribe_all(int component_idx);
extern int msg_router_publish(const uint8_t *topic_name, const uint8_t *data);

/*
 * Regression for GitHub issue #80: msg_router_publish hangs on Pi 5 when
 * no subscriber acks.
 *
 * On Pi 5, tasks run with DAIF.I=1 so the tick counter (pit_ticks) does
 * not advance while a task is executing. The old publish loop used
 * pit_ticks for its ack timeout, so the loop spun forever. The fix uses
 * the ARM generic timer (CNTPCT_EL0) which always advances.
 *
 * This test subscribes a fake component (index 0) to a topic and then
 * publishes. No task ever calls msg_router_ack(), so publish must time
 * out and return 0 instead of hanging. The test also asserts the
 * timeout completes in a reasonable wall-clock window to catch a
 * regression that makes the timeout run forever or return instantly.
 */
static void test_publish_times_out_without_ack(void)
{
    msg_router_init();

    int sub_ret = msg_router_subscribe("/test/timeout", 0);
    TEST_ASSERT_EQUAL_INT(0, sub_ret);

    uint64_t start = timer_get_count();
    int delivered = msg_router_publish((const uint8_t *)"/test/timeout",
                                       (const uint8_t *)"data");
    uint64_t elapsed = timer_get_count() - start;

    /* No subscriber task runs, so nothing acks -> publish returns 0. */
    TEST_ASSERT_EQUAL_INT(0, delivered);

    /*
     * Timeout is 5 seconds in the router. Allow 3-8 seconds to account
     * for timer read overhead and any yield-loop pacing.
     */
    uint64_t freq = timer_get_frequency();
    uint64_t elapsed_sec = elapsed / freq;
    TEST_ASSERT_GREATER_OR_EQUAL(3, elapsed_sec);
    TEST_ASSERT_LESS_OR_EQUAL(8, elapsed_sec);

    msg_router_unsubscribe_all(0);
}

/*
 * Publishing to a topic with no subscribers must return 0 immediately
 * (no mailboxes to deliver to, no ack wait). Cheap sanity check.
 */
static void test_publish_no_subscribers(void)
{
    msg_router_init();

    uint64_t start = timer_get_count();
    int delivered = msg_router_publish((const uint8_t *)"/test/nosub",
                                       (const uint8_t *)"data");
    uint64_t elapsed = timer_get_count() - start;

    TEST_ASSERT_EQUAL_INT(0, delivered);

    /* Should complete in well under a second. */
    uint64_t freq = timer_get_frequency();
    TEST_ASSERT_LESS_THAN(freq, elapsed);
}

int test_suite_msg_router(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_publish_no_subscribers);
    RUN_TEST(test_publish_times_out_without_ack);
    return UNITY_END();
}
