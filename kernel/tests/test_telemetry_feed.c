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

/* ---------- Periodic publishers (tel.cpu / tel.stl / tel.mem) -------- */

static void test_periodic_first_call_is_baseline_only(void)
{
    /* The first pump call after boot/reset establishes the snapshot
     * baseline and must not publish — deltas are meaningless on the
     * first sample. */
    admin_telemetry_periodic_reset_for_tests();

    struct admin_telemetry_periodic_stats st_before, st_after;
    admin_telemetry_get_periodic_stats(&st_before);
    TEST_ASSERT_FALSE(st_before.baseline_set);

    admin_telemetry_periodic_pump(0u);

    admin_telemetry_get_periodic_stats(&st_after);
    TEST_ASSERT_TRUE(st_after.baseline_set);
    TEST_ASSERT_EQUAL_UINT64(0u, st_after.cpu_published);
    TEST_ASSERT_EQUAL_UINT64(0u, st_after.steal_published);
    TEST_ASSERT_EQUAL_UINT64(0u, st_after.memory_published);
}

static void test_periodic_rate_limited_within_interval(void)
{
    /* A second call inside the publish window must be a no-op even
     * though the baseline is set. Rate-limit prevents net_poll's
     * ~100 Hz cadence from drowning subscribers in samples. */
    admin_telemetry_periodic_reset_for_tests();
    admin_telemetry_periodic_pump(1000u);  /* baseline */

    /* Tick forward by less than the interval. */
    admin_telemetry_periodic_pump(1500u);

    struct admin_telemetry_periodic_stats st;
    admin_telemetry_get_periodic_stats(&st);
    TEST_ASSERT_EQUAL_UINT64(0u, st.cpu_published);
    TEST_ASSERT_EQUAL_UINT64(0u, st.steal_published);
    TEST_ASSERT_EQUAL_UINT64(0u, st.memory_published);
}

static void test_periodic_publishes_all_three_topics(void)
{
    /* Past the interval boundary, one call emits one sample on each
     * of the three topics. */
    admin_telemetry_periodic_reset_for_tests();
    admin_telemetry_periodic_pump(0u);  /* baseline */

    admin_telemetry_periodic_pump(TELEMETRY_PERIODIC_INTERVAL_MS);

    struct admin_telemetry_periodic_stats st;
    admin_telemetry_get_periodic_stats(&st);
    TEST_ASSERT_EQUAL_UINT64(1u, st.cpu_published);
    TEST_ASSERT_EQUAL_UINT64(1u, st.steal_published);
    TEST_ASSERT_EQUAL_UINT64(1u, st.memory_published);
    TEST_ASSERT_EQUAL_UINT32(TELEMETRY_PERIODIC_INTERVAL_MS, st.last_pump_ms);
}

static void test_periodic_repeated_publishes_at_interval(void)
{
    /* Three intervals → three samples per topic. */
    admin_telemetry_periodic_reset_for_tests();
    admin_telemetry_periodic_pump(0u);
    admin_telemetry_periodic_pump(1000u);
    admin_telemetry_periodic_pump(2000u);
    admin_telemetry_periodic_pump(3000u);

    struct admin_telemetry_periodic_stats st;
    admin_telemetry_get_periodic_stats(&st);
    TEST_ASSERT_EQUAL_UINT64(3u, st.cpu_published);
    TEST_ASSERT_EQUAL_UINT64(3u, st.steal_published);
    TEST_ASSERT_EQUAL_UINT64(3u, st.memory_published);
}

static void test_periodic_handles_now_ms_wrap(void)
{
    /* sys_now() rolls over every ~49 days. The unsigned subtraction
     * in the elapsed-interval check has to stay correct across that
     * boundary — otherwise telemetry would freeze at the wrap point
     * and only resume after another ~49-day full cycle. */
    admin_telemetry_periodic_reset_for_tests();
    admin_telemetry_periodic_pump(UINT32_MAX - 500u);  /* baseline near wrap */

    /* Tick across the wrap by 600 ms; elapsed = 600 ms via wrap-safe sub. */
    admin_telemetry_periodic_pump(99u);  /* (UINT32_MAX - 500) + 600 mod 2^32 */

    struct admin_telemetry_periodic_stats st;
    admin_telemetry_get_periodic_stats(&st);
    /* 600 ms elapsed but interval is 1000 ms — no publish yet. */
    TEST_ASSERT_EQUAL_UINT64(0u, st.cpu_published);

    /* Now tick another 500 ms past wrap. Total elapsed since baseline:
     * 600 + 500 = 1100 ms ≥ 1000 ms → publish. */
    admin_telemetry_periodic_pump(599u);

    admin_telemetry_get_periodic_stats(&st);
    TEST_ASSERT_EQUAL_UINT64(1u, st.cpu_published);
    TEST_ASSERT_EQUAL_UINT64(1u, st.steal_published);
    TEST_ASSERT_EQUAL_UINT64(1u, st.memory_published);
}

static void test_periodic_topic_names_fit_msg_router(void)
{
    /* msg_router caps topic names at TOPIC_NAME_LEN=16 (incl. nul).
     * All five tel.* topic constants must fit. */
    TEST_ASSERT_TRUE(strlen(TELEMETRY_TOPIC_EVICTION)  < 16);
    TEST_ASSERT_TRUE(strlen(TELEMETRY_TOPIC_INFERENCE) < 16);
    TEST_ASSERT_TRUE(strlen(TELEMETRY_TOPIC_CPU_UTIL)  < 16);
    TEST_ASSERT_TRUE(strlen(TELEMETRY_TOPIC_STEAL)     < 16);
    TEST_ASSERT_TRUE(strlen(TELEMETRY_TOPIC_MEMORY)    < 16);
    /* All share the tel. prefix so a `tel.*` subscription catches
     * every periodic + event-driven topic in one filter. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(TELEMETRY_TOPIC_CPU_UTIL, TELEMETRY_TOPIC_PREFIX, 4));
    TEST_ASSERT_EQUAL_INT(0, memcmp(TELEMETRY_TOPIC_STEAL,    TELEMETRY_TOPIC_PREFIX, 4));
    TEST_ASSERT_EQUAL_INT(0, memcmp(TELEMETRY_TOPIC_MEMORY,   TELEMETRY_TOPIC_PREFIX, 4));
}

static void test_periodic_get_stats_null_safe(void)
{
    admin_telemetry_get_periodic_stats(NULL);
}

/* ---------- Periodic payload-content (functional) -------------------- */

static void test_periodic_steal_payload_format(void)
{
    /* tel.stl payload must contain all five aggregate keys in the
     * documented order: att, ok, stl, emp, fll. Aggregated across
     * CPUs so the payload is a fixed shape regardless of cpu_count. */
    admin_telemetry_periodic_reset_for_tests();
    admin_telemetry_periodic_pump(0u);
    admin_telemetry_periodic_pump(TELEMETRY_PERIODIC_INTERVAL_MS);

    char payload[64] = {0};
    admin_telemetry_get_last_steal_payload_for_tests(payload, sizeof(payload));

    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(payload, "att="),
        "tel.stl must include 'att=' (steal attempts)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(payload, "ok="),
        "tel.stl must include 'ok=' (steal successes)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(payload, "stl="),
        "tel.stl must include 'stl=' (stale-pointer drops)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(payload, "emp="),
        "tel.stl must include 'emp=' (empty-victim count)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(payload, "fll="),
        "tel.stl must include 'fll=' (push-failed-deque-full)");

    /* Documented order: att before ok before stl before emp before fll.
     * If a future refactor reorders the appends, the network-wire
     * compatibility breaks for any consumer that splits on column
     * position. */
    const char *att = strstr(payload, "att=");
    const char *ok  = strstr(payload, "ok=");
    const char *stl = strstr(payload, "stl=");
    const char *emp = strstr(payload, "emp=");
    const char *fll = strstr(payload, "fll=");
    TEST_ASSERT_TRUE(att < ok && ok < stl && stl < emp && emp < fll);
}

static void test_periodic_memory_payload_format(void)
{
    /* tel.mem payload must contain fp, tp, wev, xev — the documented
     * shape. Pin against schema regression. */
    admin_telemetry_periodic_reset_for_tests();
    admin_telemetry_periodic_pump(0u);
    admin_telemetry_periodic_pump(TELEMETRY_PERIODIC_INTERVAL_MS);

    char payload[64] = {0};
    admin_telemetry_get_last_memory_payload_for_tests(payload, sizeof(payload));

    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(payload, "fp="),
        "tel.mem must include 'fp=' (free pages)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(payload, "tp="),
        "tel.mem must include 'tp=' (total pages)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(payload, "wev="),
        "tel.mem must include 'wev=' (weight-pool eviction delta)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(payload, "xev="),
        "tel.mem must include 'xev=' (workspace-pool eviction delta)");

    const char *fp  = strstr(payload, "fp=");
    const char *tp  = strstr(payload, "tp=");
    const char *wev = strstr(payload, "wev=");
    const char *xev = strstr(payload, "xev=");
    TEST_ASSERT_TRUE(fp < tp && tp < wev && wev < xev);
}

static void test_periodic_payload_capped_at_max_msg_len(void)
{
    /* Every published payload must be < MAX_MSG_LEN=60 (msg_router cap)
     * regardless of how many CPUs the publish_cpu_util loop iterated.
     * The append_kv_uint helper enforces this internally; verify the
     * post-publish length is in spec for every topic. */
    admin_telemetry_periodic_reset_for_tests();
    admin_telemetry_periodic_pump(0u);
    admin_telemetry_periodic_pump(TELEMETRY_PERIODIC_INTERVAL_MS);

    char buf[64] = {0};
    admin_telemetry_get_last_cpu_payload_for_tests(buf, sizeof(buf));
    TEST_ASSERT_MESSAGE(strlen(buf) < 60,
        "tel.cpu payload must fit MAX_MSG_LEN=60");
    admin_telemetry_get_last_steal_payload_for_tests(buf, sizeof(buf));
    TEST_ASSERT_MESSAGE(strlen(buf) < 60,
        "tel.stl payload must fit MAX_MSG_LEN=60");
    admin_telemetry_get_last_memory_payload_for_tests(buf, sizeof(buf));
    TEST_ASSERT_MESSAGE(strlen(buf) < 60,
        "tel.mem payload must fit MAX_MSG_LEN=60");
}

static void test_periodic_payload_empty_before_publish(void)
{
    /* Before the first publish (only the baseline call has run), the
     * captured-payload buffers must read as empty. Pins the baseline
     * semantic at the test seam so tests reading content don't see
     * stale data from a previous run. */
    admin_telemetry_periodic_reset_for_tests();
    admin_telemetry_periodic_pump(0u);  /* baseline only — no publish */

    char buf[64];
    memset(buf, 0xAA, sizeof(buf));
    admin_telemetry_get_last_cpu_payload_for_tests(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\0', (uint8_t)buf[0]);
    memset(buf, 0xAA, sizeof(buf));
    admin_telemetry_get_last_steal_payload_for_tests(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\0', (uint8_t)buf[0]);
    memset(buf, 0xAA, sizeof(buf));
    admin_telemetry_get_last_memory_payload_for_tests(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\0', (uint8_t)buf[0]);
}

static void test_periodic_payload_truncates_to_caller_cap(void)
{
    /* Test-seam read-back must respect the caller's `cap`, even when
     * the underlying captured payload is longer. NUL-terminate within
     * the cap. cap=0 / NULL must be no-ops (no crash). */
    admin_telemetry_periodic_reset_for_tests();
    admin_telemetry_periodic_pump(0u);
    admin_telemetry_periodic_pump(TELEMETRY_PERIODIC_INTERVAL_MS);

    char small[8];
    memset(small, 0xAA, sizeof(small));
    admin_telemetry_get_last_steal_payload_for_tests(small, sizeof(small));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\0', (uint8_t)small[sizeof(small) - 1]);

    admin_telemetry_get_last_cpu_payload_for_tests(NULL, 0u);
    admin_telemetry_get_last_cpu_payload_for_tests(small, 0u);
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
    RUN_TEST(test_periodic_first_call_is_baseline_only);
    RUN_TEST(test_periodic_rate_limited_within_interval);
    RUN_TEST(test_periodic_publishes_all_three_topics);
    RUN_TEST(test_periodic_repeated_publishes_at_interval);
    RUN_TEST(test_periodic_handles_now_ms_wrap);
    RUN_TEST(test_periodic_topic_names_fit_msg_router);
    RUN_TEST(test_periodic_get_stats_null_safe);
    RUN_TEST(test_periodic_steal_payload_format);
    RUN_TEST(test_periodic_memory_payload_format);
    RUN_TEST(test_periodic_payload_capped_at_max_msg_len);
    RUN_TEST(test_periodic_payload_empty_before_publish);
    RUN_TEST(test_periodic_payload_truncates_to_caller_cap);
    return UNITY_END();
}
