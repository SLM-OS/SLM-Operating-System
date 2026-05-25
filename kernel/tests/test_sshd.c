/*
 * test_sshd.c — sshd public-API + RX ring regression coverage (#199a).
 *
 * Targets:
 *
 *   - sshd_get_stats fills a sane initial state when the daemon has
 *     never been started in this test run.
 *   - sshd_get_stats tolerates NULL out without crashing.
 *   - Ring push/pop round-trip for sub-capacity, exact-capacity, and
 *     over-capacity writes — would have caught the data-loss path
 *     where the lwIP callback freed a pbuf after only partially
 *     pushing it into the ring.
 *   - Ring wrap-around: head/tail wrap independently.
 *   - Slot allocation respects the SSHD_MAX_SESSIONS pool cap.
 *
 * Out of scope: end-to-end KEX over a real TCP connection. Those
 * paths are validated by the hardware bring-up checklist (see
 * docs/security.md).
 */

#include "sshd.h"
#include "sshd_test.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

static void test_initial_stats_are_stopped(void)
{
    struct sshd_stats st;
    /* Pre-seed to catch a sshd_get_stats that no-ops or partially fills. */
    memset(&st, 0xCC, sizeof(st));
    sshd_get_stats(&st);
    TEST_ASSERT_FALSE(st.running);
    TEST_ASSERT_EQUAL_UINT16(0, st.port);
    /* Counters are best-effort but never decrement once a session
     * has been seen. In a fresh test run they're zero; if the daemon
     * was started in an earlier test, they're whatever value those
     * sessions left behind. We assert "structurally sane" only. */
    TEST_ASSERT_TRUE(st.kex_failed <= st.connections_accepted);
}

static void test_get_stats_tolerates_null(void)
{
    /* No assertions — the requirement is "doesn't crash". A failing
     * NULL dereference would fault here and the test harness would
     * surface a panic. */
    sshd_get_stats(NULL);
    TEST_PASS();
}

static void test_ring_push_pop_roundtrip(void)
{
    sshd_test_release_all_slots();
    void *c = sshd_test_take_slot();
    TEST_ASSERT_NOT_NULL(c);

    uint8_t in[64];
    for (size_t i = 0; i < sizeof(in); i++) in[i] = (uint8_t)(i * 17u + 3u);

    size_t pushed = sshd_test_ring_push(c, in, sizeof(in));
    TEST_ASSERT_EQUAL_UINT(sizeof(in), pushed);

    uint8_t out[64];
    memset(out, 0, sizeof(out));
    size_t popped = sshd_test_ring_pop(c, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(sizeof(in), popped);
    TEST_ASSERT_EQUAL_MEMORY(in, out, sizeof(in));

    sshd_test_release_slot(c);
}

static void test_ring_push_to_full_caps_at_capacity(void)
{
    sshd_test_release_all_slots();
    void *c = sshd_test_take_slot();
    TEST_ASSERT_NOT_NULL(c);

    size_t cap = sshd_test_ring_capacity();
    TEST_ASSERT_TRUE(cap > 0);

    /* Fill the ring with a known pattern. */
    uint8_t byte = 0xA5;
    size_t  filled = 0;
    while (filled < cap) {
        size_t pushed = sshd_test_ring_push(c, &byte, 1);
        TEST_ASSERT_EQUAL_UINT(1, pushed);
        filled++;
    }
    TEST_ASSERT_EQUAL_UINT(0, sshd_test_ring_free(c));

    /* One more byte must NOT fit — this is the invariant the Critical
     * on_tcp_recv finding relied on. */
    size_t over = sshd_test_ring_push(c, &byte, 1);
    TEST_ASSERT_EQUAL_UINT(0, over);

    /* Drain everything; ring goes back to empty. */
    uint8_t scratch[256];
    size_t drained = 0;
    while (drained < cap) {
        size_t got = sshd_test_ring_pop(c, scratch,
                                        (sizeof(scratch) < cap - drained)
                                            ? sizeof(scratch)
                                            : (cap - drained));
        TEST_ASSERT_TRUE(got > 0);
        drained += got;
    }
    TEST_ASSERT_EQUAL_UINT(cap, sshd_test_ring_free(c));

    sshd_test_release_slot(c);
}

static void test_ring_partial_push_returns_pushed_count(void)
{
    sshd_test_release_all_slots();
    void *c = sshd_test_take_slot();
    TEST_ASSERT_NOT_NULL(c);

    size_t cap = sshd_test_ring_capacity();
    /* Push 10 bytes shy of full. */
    uint8_t filler[256];
    memset(filler, 0x5A, sizeof(filler));
    size_t headroom = cap - 10u;
    size_t pushed_total = 0;
    while (pushed_total < headroom) {
        size_t want = headroom - pushed_total;
        if (want > sizeof(filler)) want = sizeof(filler);
        size_t got = sshd_test_ring_push(c, filler, want);
        TEST_ASSERT_EQUAL_UINT(want, got);
        pushed_total += got;
    }
    TEST_ASSERT_EQUAL_UINT(10, sshd_test_ring_free(c));

    /* Ask to push 30 bytes — only 10 should fit; return value must
     * reflect that. The lwIP callback uses this return value to
     * decide whether to ERR_MEM the pbuf. A buggy implementation that
     * returned the full request size would silently lose the 20
     * uncopied bytes. */
    uint8_t extra[30];
    memset(extra, 0x99, sizeof(extra));
    size_t partial = sshd_test_ring_push(c, extra, sizeof(extra));
    TEST_ASSERT_EQUAL_UINT(10, partial);
    TEST_ASSERT_EQUAL_UINT(0, sshd_test_ring_free(c));

    sshd_test_release_slot(c);
}

static void test_ring_wraps_around(void)
{
    sshd_test_release_all_slots();
    void *c = sshd_test_take_slot();
    TEST_ASSERT_NOT_NULL(c);

    size_t cap = sshd_test_ring_capacity();
    /* Walk head + tail past the buffer end twice. Use a chunk size
     * that isn't a clean divisor of capacity so the wrap lands at a
     * non-trivial offset. */
    uint8_t chunk_in[97];
    uint8_t chunk_out[97];
    for (size_t i = 0; i < sizeof(chunk_in); i++) chunk_in[i] = (uint8_t)(i + 1);

    size_t iterations = (cap * 2u + sizeof(chunk_in) - 1u) / sizeof(chunk_in);
    for (size_t k = 0; k < iterations; k++) {
        size_t pushed = sshd_test_ring_push(c, chunk_in, sizeof(chunk_in));
        TEST_ASSERT_EQUAL_UINT(sizeof(chunk_in), pushed);
        memset(chunk_out, 0, sizeof(chunk_out));
        size_t popped = sshd_test_ring_pop(c, chunk_out, sizeof(chunk_out));
        TEST_ASSERT_EQUAL_UINT(sizeof(chunk_in), popped);
        TEST_ASSERT_EQUAL_MEMORY(chunk_in, chunk_out, sizeof(chunk_in));
    }

    sshd_test_release_slot(c);
}

static void test_slot_pool_respects_max_sessions(void)
{
    sshd_test_release_all_slots();
    /* Take all available slots, confirm the next one is NULL, then
     * release them. SSHD_MAX_SESSIONS is the cap; the test stops
     * at SSHD_MAX_SESSIONS + 1 to confirm the failure mode. */
    void *slots[SSHD_MAX_SESSIONS + 1];
    for (size_t i = 0; i < SSHD_MAX_SESSIONS; i++) {
        slots[i] = sshd_test_take_slot();
        TEST_ASSERT_NOT_NULL(slots[i]);
    }
    slots[SSHD_MAX_SESSIONS] = sshd_test_take_slot();
    TEST_ASSERT_NULL(slots[SSHD_MAX_SESSIONS]);

    for (size_t i = 0; i < SSHD_MAX_SESSIONS; i++) {
        sshd_test_release_slot(slots[i]);
    }
    /* After release, the pool must accept new takes again. */
    void *recheck = sshd_test_take_slot();
    TEST_ASSERT_NOT_NULL(recheck);
    sshd_test_release_slot(recheck);
}

int test_suite_sshd(void)
{
    UnityBegin("sshd (public API + RX ring)");

    RUN_TEST(test_initial_stats_are_stopped);
    RUN_TEST(test_get_stats_tolerates_null);
    RUN_TEST(test_ring_push_pop_roundtrip);
    RUN_TEST(test_ring_push_to_full_caps_at_capacity);
    RUN_TEST(test_ring_partial_push_returns_pushed_count);
    RUN_TEST(test_ring_wraps_around);
    RUN_TEST(test_slot_pool_respects_max_sessions);

    return UnityEnd();
}
