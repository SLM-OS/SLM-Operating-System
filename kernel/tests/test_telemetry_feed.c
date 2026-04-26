/*
 * test_telemetry_feed.c - Unit tests for the telemetry feed (M4).
 *
 * Exercises:
 *   - Topic emission counter increments on each record_*() call
 *   - Topic name strings round-trip through the feed_stats accessor
 *   - Reset paths leave counters intact (telemetry counters are
 *     monotonic; only the per-consumer hist/rate counters reset)
 *
 * Does NOT exercise the msg_router subscriber path here — that's
 * covered by `test_msg_router.c` (existing) and the new bindings
 * are pure aliases of those, so a separate Lua-binding test would
 * largely duplicate existing coverage.
 */

#include "unity.h"
#include "../include/admin_telemetry.h"

#include <stdint.h>
#include <string.h>

/* ---------- feed_stats: shape ---------------------------------------- */

static void test_feed_stats_topic_strings(void)
{
    struct admin_telemetry_feed_stats st;
    /* Pre-fill with 0xAA so we'd see uninitialised reads. */
    memset(&st, 0xAA, sizeof(st));
    admin_telemetry_get_feed_stats(&st);

    TEST_ASSERT_NOT_NULL(st.eviction_topic);
    TEST_ASSERT_NOT_NULL(st.inference_topic);
    /* Topic names match the constants the docstring promises. */
    TEST_ASSERT_EQUAL_STRING("tel.evi", st.eviction_topic);
    TEST_ASSERT_EQUAL_STRING("tel.inf", st.inference_topic);
    /* Both fit in msg_router's TOPIC_NAME_LEN=16. */
    TEST_ASSERT_TRUE(strlen(st.eviction_topic) < 16);
    TEST_ASSERT_TRUE(strlen(st.inference_topic) < 16);
}

static void test_feed_stats_null_safe(void)
{
    /* NULL-out-arg must not crash. */
    admin_telemetry_get_feed_stats(NULL);
}

/* ---------- Counter increments ---------------------------------------- */

static void test_eviction_publish_increments_counter(void)
{
    struct admin_telemetry_feed_stats before, after;
    admin_telemetry_get_feed_stats(&before);

    admin_telemetry_record_eviction_decision(1234u);

    admin_telemetry_get_feed_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.eviction_published + 1u,
                             after.eviction_published);
}

static void test_eviction_fallback_publishes_separately(void)
{
    struct admin_telemetry_feed_stats before, after;
    admin_telemetry_get_feed_stats(&before);

    admin_telemetry_record_eviction_fallback();

    admin_telemetry_get_feed_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.eviction_published + 1u,
                             after.eviction_published);
}

static void test_inference_publish_increments_counter(void)
{
    struct admin_telemetry_feed_stats before, after;
    admin_telemetry_get_feed_stats(&before);

    admin_telemetry_record_inference(5678u, true);

    admin_telemetry_get_feed_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.inference_published + 1u,
                             after.inference_published);
}

static void test_inference_failed_call_still_publishes(void)
{
    /* ok=0 case — error_rate ticks but the publish path is the same. */
    struct admin_telemetry_feed_stats before, after;
    admin_telemetry_get_feed_stats(&before);

    admin_telemetry_record_inference(99u, false);

    admin_telemetry_get_feed_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.inference_published + 1u,
                             after.inference_published);
}

/* ---------- Reset semantics ------------------------------------------ */

static void test_reset_does_not_zero_telemetry_counters(void)
{
    /* The per-consumer reset paths zero decisions / fallbacks /
     * latency_hist for that consumer, but they intentionally leave
     * the cumulative `published` counters alone — those are wired
     * to the msg_router emission path and represent total events
     * the system has ever broadcast, not "this measurement window".
     *
     * Pin that contract so a future edit to admin_telemetry_reset_*
     * doesn't accidentally drop the published count and confuse the
     * `telemetry stats` shell command. */
    admin_telemetry_record_eviction_decision(100u);
    admin_telemetry_record_inference(100u, true);

    struct admin_telemetry_feed_stats before;
    admin_telemetry_get_feed_stats(&before);

    admin_telemetry_reset_eviction();
    admin_telemetry_reset_inference();

    struct admin_telemetry_feed_stats after;
    admin_telemetry_get_feed_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.eviction_published,  after.eviction_published);
    TEST_ASSERT_EQUAL_UINT64(before.inference_published, after.inference_published);
}

/* ---------- Suite registration --------------------------------------- */

int test_suite_telemetry_feed(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_feed_stats_topic_strings);
    RUN_TEST(test_feed_stats_null_safe);
    RUN_TEST(test_eviction_publish_increments_counter);
    RUN_TEST(test_eviction_fallback_publishes_separately);
    RUN_TEST(test_inference_publish_increments_counter);
    RUN_TEST(test_inference_failed_call_still_publishes);
    RUN_TEST(test_reset_does_not_zero_telemetry_counters);
    return UNITY_END();
}
