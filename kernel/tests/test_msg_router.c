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
extern int msg_router_publish_nowait(const uint8_t *topic_name,
                                     const uint8_t *data);
extern const uint8_t *msg_router_receive(int component_idx, uint8_t *topic_out);
extern void msg_router_ack(int component_idx);

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

/*
 * Regression for #69: subscribe must reject topic names that exceed
 * TOPIC_NAME_LEN - 1 (15 bytes) rather than silently truncate. Prior
 * behavior coerced "/sensors/temperature" (20 chars) to "/sensors/temper"
 * and would alias with any other long name that shared the 15-byte prefix.
 */
static void test_subscribe_rejects_long_topic(void)
{
    msg_router_init();

    /* Exactly 15 chars (fits with NUL) — must succeed. */
    int ok = msg_router_subscribe("/test/abcdefghi", 0);
    TEST_ASSERT_EQUAL_INT(0, ok);
    msg_router_unsubscribe_all(0);

    /* 16 chars — would truncate; must fail. */
    int r16 = msg_router_subscribe("/test/abcdefghij", 1);
    TEST_ASSERT_EQUAL_INT(-1, r16);

    /* 20 chars — clearly oversized; must fail. */
    int r20 = msg_router_subscribe("/sensors/temperature", 2);
    TEST_ASSERT_EQUAL_INT(-1, r20);
}

/*
 * Regression for #69: publish must also reject (return 0) oversized
 * topic names so a caller cannot sneak a silently-truncated publish
 * past a legitimate subscribe.
 */
static void test_publish_rejects_long_topic(void)
{
    msg_router_init();

    int delivered = msg_router_publish(
        (const uint8_t *)"/sensors/temperature",
        (const uint8_t *)"data");
    TEST_ASSERT_EQUAL_INT(0, delivered);
}

/*
 * Regression for #68: wildcard pattern matching must not read past the
 * NUL terminator of a short topic name. A subscription to "/sensors/" followed by '*'
 * against a short topic like "/s" must return "no match" without
 * dereferencing past the NUL. Exercises is_wildcard_pattern +
 * wildcard_matches for a bounded scan.
 */
static void test_wildcard_short_topic_bounds(void)
{
    msg_router_init();

    /* Wildcard subscription for "/sensors/" followed by '*' (10 bytes, fits). */
    int sub = msg_router_subscribe("/sensors/*", 0);
    TEST_ASSERT_EQUAL_INT(0, sub);

    /* Publish to a topic shorter than the wildcard prefix. The router
     * must not match and must not time out on ack — no mailbox was
     * delivered to. */
    uint64_t start = timer_get_count();
    int delivered = msg_router_publish((const uint8_t *)"/s",
                                       (const uint8_t *)"data");
    uint64_t elapsed = timer_get_count() - start;

    TEST_ASSERT_EQUAL_INT(0, delivered);

    /* Must complete quickly: no subscriber was actually targeted. */
    uint64_t freq = timer_get_frequency();
    TEST_ASSERT_LESS_THAN(freq, elapsed);

    msg_router_unsubscribe_all(0);
}

/*
 * Regression for #320: msg_router_subscribe must be idempotent by
 * (component_idx, topic). Previously, repeated subscribe calls
 * consumed fresh subscriber slots, and a component like sensor_monitor
 * that re-subscribed during hot-swap would end up in two slots — every
 * published message was delivered twice to the same component ("double
 * alert" on value 88 in the demo).
 *
 * MAX_SUBSCRIBERS is 4. Subscribing the same (idx, topic) pair 5 times
 * used to exhaust the topic's slots and fail on the 5th call; with
 * dedup it succeeds every time.
 */
static void test_subscribe_idempotent_exact(void)
{
    msg_router_init();

    /* 5 subscribes of the same (idx, topic) — with dedup, all return 0. */
    for (int i = 0; i < 5; i++) {
        int rc = msg_router_subscribe("/test/idem", 7);
        TEST_ASSERT_EQUAL_INT(0, rc);
    }

    /* Different component_idx on the same topic still takes a new slot. */
    int rc2 = msg_router_subscribe("/test/idem", 8);
    TEST_ASSERT_EQUAL_INT(0, rc2);

    msg_router_unsubscribe_all(7);
    msg_router_unsubscribe_all(8);
}

static void test_subscribe_idempotent_wildcard(void)
{
    msg_router_init();

    /* Same idempotency rule for wildcard subscriptions. */
    for (int i = 0; i < 5; i++) {
        int rc = msg_router_subscribe("/wild/*", 7);
        TEST_ASSERT_EQUAL_INT(0, rc);
    }

    msg_router_unsubscribe_all(7);
}

static void test_subscribe_max_topics_sixteen(void)
{
    msg_router_init();

    for (int i = 0; i < 16; i++) {
        char topic[8];
        topic[0] = 't';
        topic[1] = (char)('0' + (i / 10));
        topic[2] = (char)('0' + (i % 10));
        topic[3] = '\0';
        TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(topic, i));
    }

    TEST_ASSERT_EQUAL_INT(-1, msg_router_subscribe("t16", 16));
}

/*
 * Regression for #863 (#67b): wildcard+exact overlap delivers twice.
 *
 * A component subscribed via both an exact topic name and a matching
 * wildcard pattern receives the message TWICE — once per subscription.
 * Each subscription owns its own independent mailbox, and publish_internal
 * fans out across every matching mailbox. This is the per-subscription
 * delivery contract the project documents in docs/ipc.md and the message
 * router rs source comments; it matches DDS / ROS 2 / ZeroMQ / nanomsg /
 * Linux notifier chains. Applications that want exactly-once delivery
 * across overlapping patterns dedupe at the application layer.
 *
 * This test pins the contract via `msg_router_publish_nowait`, which
 * returns the number of mailboxes the message was dropped into without
 * ack-wait choreography. Using the no-wait variant lets the test run
 * single-threaded — the full ack-wait publish path is exercised by the
 * cross-CPU concurrency tests in test_integration.c. The fan-out count
 * is the contract being asserted; the ack-wait loop is orthogonal.
 *
 * Topic budget reminder: TOPIC_NAME_LEN = 16 (NUL-terminated). The names
 * below are all ≤ 15 bytes.
 */
static void test_wildcard_exact_overlap_delivers_twice(void)
{
    msg_router_init();

    /* Subscribe one component to BOTH an exact topic and a matching
     * wildcard pattern. The wildcard prefix is "/sensors/" so it matches
     * "/sensors/data" as well. */
    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe("/sensors/data", 0));
    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe("/sensors/*",    0));

    /* Publish to the exact name. Both the exact mailbox and the wildcard
     * mailbox match — fan-out should be 2. */
    int delivered = msg_router_publish_nowait(
        (const uint8_t *)"/sensors/data", (const uint8_t *)"d1");
    TEST_ASSERT_EQUAL_INT(2, delivered);

    /* Drain both mailboxes via two receive+ack cycles. Each receive
     * returns one mailbox; receive's lock-held scan picks one (priority
     * tie-break order between exact + wildcard at equal priority is
     * unspecified, so we don't assert WHICH mailbox arrives first — only
     * that two distinct mailboxes yielded the published message). Both
     * mailboxes carry the PUBLISHED topic name ("/sensors/data"), since
     * deliver() copies the publisher's name into the mailbox slot. */
    uint8_t topic_buf[16];
    const uint8_t *m1 = msg_router_receive(0, topic_buf);
    TEST_ASSERT_NOT_NULL(m1);
    TEST_ASSERT_EQUAL_STRING("/sensors/data", (const char *)topic_buf);
    TEST_ASSERT_EQUAL_STRING("d1", (const char *)m1);
    msg_router_ack(0);

    const uint8_t *m2 = msg_router_receive(0, topic_buf);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_STRING("/sensors/data", (const char *)topic_buf);
    TEST_ASSERT_EQUAL_STRING("d1", (const char *)m2);
    msg_router_ack(0);

    /* After two acks both mailboxes are cleared — a third receive
     * returns NULL. Confirms there was no third hidden mailbox and that
     * ack correctly clears the mailbox it targeted via LAST_RECEIVED. */
    const uint8_t *m3 = msg_router_receive(0, topic_buf);
    TEST_ASSERT_NULL(m3);

    msg_router_unsubscribe_all(0);
}

/*
 * Regression for #863 (#67b): wildcard-only matches deliver once.
 *
 * Companion to the exact+wildcard overlap test above. When a published
 * topic name matches ONLY the wildcard pattern (not the exact name),
 * fan-out is 1 — only the wildcard mailbox receives the message.
 */
static void test_wildcard_only_delivers_once(void)
{
    msg_router_init();

    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe("/sensors/data", 0));
    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe("/sensors/*",    0));

    /* /sensors/temp matches only the wildcard, not the exact subscription. */
    int delivered = msg_router_publish_nowait(
        (const uint8_t *)"/sensors/temp", (const uint8_t *)"t1");
    TEST_ASSERT_EQUAL_INT(1, delivered);

    uint8_t topic_buf[16];
    const uint8_t *m1 = msg_router_receive(0, topic_buf);
    TEST_ASSERT_NOT_NULL(m1);
    TEST_ASSERT_EQUAL_STRING("/sensors/temp", (const char *)topic_buf);
    TEST_ASSERT_EQUAL_STRING("t1", (const char *)m1);
    msg_router_ack(0);

    /* No second mailbox to drain. */
    const uint8_t *m2 = msg_router_receive(0, topic_buf);
    TEST_ASSERT_NULL(m2);

    msg_router_unsubscribe_all(0);
}

int test_suite_msg_router(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_publish_no_subscribers);
    RUN_TEST(test_subscribe_rejects_long_topic);
    RUN_TEST(test_publish_rejects_long_topic);
    RUN_TEST(test_wildcard_short_topic_bounds);
    RUN_TEST(test_subscribe_idempotent_exact);
    RUN_TEST(test_subscribe_idempotent_wildcard);
    RUN_TEST(test_subscribe_max_topics_sixteen);
    RUN_TEST(test_wildcard_exact_overlap_delivers_twice);
    RUN_TEST(test_wildcard_only_delivers_once);
    RUN_TEST(test_publish_times_out_without_ack);
    return UNITY_END();
}
