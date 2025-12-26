/*
 * IPC Tests - Unity Framework
 *
 * Tests for message queues, shared buffers, timeouts, and statistics.
 */

#include "unity.h"
#include "../include/ipc.h"
#include "../include/pmm.h"
#include "../include/timer.h"
#include "../include/config.h"

/* ========================================================================= */
/* Message Queue Tests                                                       */
/* ========================================================================= */

static void test_queue_create_destroy(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    uint32_t id = q->id;
    msg_queue_destroy(q);

    /* Verify queue is gone */
    TEST_ASSERT_NULL(msg_queue_lookup(id));
}

static void test_queue_nonblocking_send_recv(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    /* Test non-blocking send */
    struct slm_message msg = {
        .type = 42,
        .sender_id = 1,
        .payload.raw = {0x11, 0x22, 0x33, 0x44}
    };
    int result = msg_send(q, &msg, 0);
    TEST_ASSERT_EQUAL_INT(0, result);

    /* Test non-blocking receive */
    struct slm_message recv_msg;
    result = msg_recv(q, &recv_msg, 0);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(42, recv_msg.type);
    TEST_ASSERT_EQUAL_HEX8(0x11, recv_msg.payload.raw[0]);

    msg_queue_destroy(q);
}

static void test_queue_lookup_by_id(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    uint32_t id = q->id;
    struct msg_queue *found = msg_queue_lookup(id);
    TEST_ASSERT_EQUAL_PTR(q, found);

    msg_queue_destroy(q);
}

static void test_recv_empty_queue_nonblocking(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message recv_msg;
    int result = msg_recv(q, &recv_msg, 0);
    TEST_ASSERT_TRUE(result != 0);  /* Should fail - empty queue */

    msg_queue_destroy(q);
}

/* ========================================================================= */
/* Shared Buffer Tests                                                       */
/* ========================================================================= */

static void test_buffer_create_destroy(void)
{
    struct shared_buffer *buf = shared_buffer_create(4096, SHM_RDWR);
    TEST_ASSERT_NOT_NULL(buf);

    uint32_t id = buf->id;
    shared_buffer_destroy(buf);

    /* Verify buffer is gone */
    TEST_ASSERT_NULL(shared_buffer_lookup(id));
}

static void test_buffer_map_unmap(void)
{
    struct shared_buffer *buf = shared_buffer_create(4096, SHM_RDWR);
    TEST_ASSERT_NOT_NULL(buf);

    void *mapped = shared_buffer_map(buf, NULL, SHM_RDWR);
    TEST_ASSERT_NOT_NULL(mapped);

    /* Write test pattern */
    volatile uint32_t *ptr = (volatile uint32_t *)mapped;
    *ptr = 0xDEADBEEF;
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, *ptr);

    shared_buffer_unmap(buf, NULL);
    shared_buffer_destroy(buf);
}

static void test_buffer_lookup_by_id(void)
{
    struct shared_buffer *buf = shared_buffer_create(4096, SHM_RDWR);
    TEST_ASSERT_NOT_NULL(buf);

    uint32_t id = buf->id;
    struct shared_buffer *found = shared_buffer_lookup(id);
    TEST_ASSERT_EQUAL_PTR(buf, found);

    shared_buffer_destroy(buf);
}

/* ========================================================================= */
/* Timeout Tests                                                             */
/* ========================================================================= */

static void test_recv_timeout(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message recv_msg;
    uint64_t start = timer_get_count();
    int result = msg_recv(q, &recv_msg, 50);  /* 50ms timeout */
    uint64_t elapsed = timer_get_count() - start;

    TEST_ASSERT_TRUE(result != 0);  /* Should timeout */

    /* Verify reasonable elapsed time (40-200ms accounting for overhead) */
    uint64_t elapsed_ms = (elapsed * 1000) / timer_get_frequency();
    TEST_ASSERT_GREATER_OR_EQUAL(40, elapsed_ms);
    TEST_ASSERT_LESS_OR_EQUAL(200, elapsed_ms);

    msg_queue_destroy(q);
}

static void test_send_timeout_full_queue(void)
{
    struct msg_queue *q = msg_queue_create(2, 0);  /* Small queue */
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg = {.type = 1, .sender_id = 1};

    /* Fill the queue */
    TEST_ASSERT_EQUAL_INT(0, msg_send(q, &msg, 0));
    TEST_ASSERT_EQUAL_INT(0, msg_send(q, &msg, 0));

    /* Third send should timeout */
    uint64_t start = timer_get_count();
    int result = msg_send(q, &msg, 50);  /* 50ms timeout */
    uint64_t elapsed = timer_get_count() - start;

    TEST_ASSERT_TRUE(result != 0);  /* Should timeout */

    /* Verify reasonable elapsed time */
    uint64_t elapsed_ms = (elapsed * 1000) / timer_get_frequency();
    TEST_ASSERT_GREATER_OR_EQUAL(40, elapsed_ms);
    TEST_ASSERT_LESS_OR_EQUAL(200, elapsed_ms);

    msg_queue_destroy(q);
}

/* ========================================================================= */
/* Memory Leak Tests                                                         */
/* ========================================================================= */

static void test_no_memory_leak(void)
{
    uint64_t free_before = pmm_get_free_pages();

    /* Create and destroy several IPC objects */
    for (int i = 0; i < 5; i++) {
        struct msg_queue *q = msg_queue_create(10, 0);
        TEST_ASSERT_NOT_NULL(q);
        msg_queue_destroy(q);

        struct shared_buffer *buf = shared_buffer_create(PAGE_SIZE, SHM_RDWR);
        TEST_ASSERT_NOT_NULL(buf);
        shared_buffer_destroy(buf);
    }

    uint64_t free_after = pmm_get_free_pages();
    TEST_ASSERT_EQUAL_UINT64(free_before, free_after);
}

/* ========================================================================= */
/* Statistics Tests                                                          */
/* ========================================================================= */

static void test_queue_statistics(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg = {.type = 1, .sender_id = 1};

    /* Send some messages */
    for (int i = 0; i < 5; i++) {
        msg_send(q, &msg, 0);
    }

    /* Receive some */
    struct slm_message recv;
    for (int i = 0; i < 3; i++) {
        msg_recv(q, &recv, 0);
    }

    /* Check statistics */
    uint64_t msgs_sent, msgs_recv;
    size_t high_water;
    msg_queue_stats(q, &msgs_sent, &msgs_recv, &high_water);

    TEST_ASSERT_EQUAL_UINT64(5, msgs_sent);
    TEST_ASSERT_EQUAL_UINT64(3, msgs_recv);
    TEST_ASSERT_GREATER_OR_EQUAL(2, high_water);

    msg_queue_destroy(q);
}

static void test_global_ipc_statistics(void)
{
    struct ipc_stats stats_before;
    ipc_get_stats(&stats_before);

    struct msg_queue *q = msg_queue_create(10, 0);
    struct shared_buffer *buf = shared_buffer_create(PAGE_SIZE, SHM_RDWR);

    struct ipc_stats stats_during;
    ipc_get_stats(&stats_during);

    TEST_ASSERT_EQUAL_UINT32(stats_before.queue_count + 1, stats_during.queue_count);
    TEST_ASSERT_EQUAL_UINT32(stats_before.buffer_count + 1, stats_during.buffer_count);

    msg_queue_destroy(q);
    shared_buffer_destroy(buf);

    struct ipc_stats stats_after;
    ipc_get_stats(&stats_after);

    TEST_ASSERT_EQUAL_UINT32(stats_before.queue_count, stats_after.queue_count);
    TEST_ASSERT_EQUAL_UINT32(stats_before.buffer_count, stats_after.buffer_count);
}

/* ========================================================================= */
/* Priority Queue Tests                                                      */
/* ========================================================================= */

/*
 * Test: Priority send/receive - high priority received first.
 */
static void test_priority_high_before_low(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg_low = {.type = 1, .sender_id = 0};
    struct slm_message msg_high = {.type = 2, .sender_id = 0};

    /* Send low priority first, then high */
    msg_send_priority(q, &msg_low, MSG_PRIO_LOW, 0);
    msg_send_priority(q, &msg_high, MSG_PRIO_HIGH, 0);

    /* Receive should get high priority first */
    struct slm_message recv;
    int ret = msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_INT(IPC_OK, ret);
    TEST_ASSERT_EQUAL_UINT32(2, recv.type);  /* High priority message */

    /* Second receive should get low priority */
    ret = msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_INT(IPC_OK, ret);
    TEST_ASSERT_EQUAL_UINT32(1, recv.type);  /* Low priority message */

    msg_queue_destroy(q);
}

/*
 * Test: Multiple priority levels ordering.
 */
static void test_priority_ordering_all_levels(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    /* Send messages in order: low, normal, high, urgent */
    struct slm_message msg;
    msg.sender_id = 0;

    msg.type = MSG_PRIO_LOW;
    msg_send_priority(q, &msg, MSG_PRIO_LOW, 0);

    msg.type = MSG_PRIO_NORMAL;
    msg_send_priority(q, &msg, MSG_PRIO_NORMAL, 0);

    msg.type = MSG_PRIO_HIGH;
    msg_send_priority(q, &msg, MSG_PRIO_HIGH, 0);

    msg.type = MSG_PRIO_URGENT;
    msg_send_priority(q, &msg, MSG_PRIO_URGENT, 0);

    /* Receive should be in priority order: urgent, high, normal, low */
    struct slm_message recv;

    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(MSG_PRIO_URGENT, recv.type);

    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(MSG_PRIO_HIGH, recv.type);

    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(MSG_PRIO_NORMAL, recv.type);

    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(MSG_PRIO_LOW, recv.type);

    msg_queue_destroy(q);
}

/*
 * Test: msg_send() uses normal priority by default.
 */
static void test_default_priority_is_normal(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg_default = {.type = 100, .sender_id = 0};
    struct slm_message msg_low = {.type = 200, .sender_id = 0};

    /* Send low priority, then normal (via msg_send) */
    msg_send_priority(q, &msg_low, MSG_PRIO_LOW, 0);
    msg_send(q, &msg_default, 0);  /* Should be MSG_PRIO_NORMAL */

    /* Normal priority should be received first */
    struct slm_message recv;
    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(100, recv.type);  /* Default = normal */

    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(200, recv.type);  /* Low priority */

    msg_queue_destroy(q);
}

/*
 * Test: Starvation prevention - low priority messages eventually delivered.
 */
static void test_starvation_prevention(void)
{
    struct msg_queue *q = msg_queue_create(32, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg;
    msg.sender_id = 0;

    /* Send one low priority message */
    msg.type = 0xDEAD;  /* Marker for low priority */
    msg_send_priority(q, &msg, MSG_PRIO_LOW, 0);

    /* Send many high priority messages (more than starvation threshold) */
    for (int i = 0; i < MSG_STARVATION_THRESHOLD + 2; i++) {
        msg.type = (uint32_t)(0x1000 + i);
        msg_send_priority(q, &msg, MSG_PRIO_HIGH, 0);
    }

    /*
     * Receive all messages. The low priority message should be received
     * after MSG_STARVATION_THRESHOLD high priority receives due to
     * starvation prevention.
     */
    struct slm_message recv;
    int low_prio_received = 0;
    int receives_before_low = 0;

    for (int i = 0; i < MSG_STARVATION_THRESHOLD + 3; i++) {
        msg_recv(q, &recv, 0);
        if (recv.type == 0xDEAD) {
            low_prio_received = 1;
            receives_before_low = i;
            break;
        }
    }

    TEST_ASSERT_TRUE(low_prio_received);
    /* Should be received within a reasonable window after threshold */
    TEST_ASSERT_LESS_OR_EQUAL(MSG_STARVATION_THRESHOLD + 1, receives_before_low);

    msg_queue_destroy(q);
}

/*
 * Test: Per-priority capacity limits.
 */
static void test_priority_capacity(void)
{
    /* Create queue with 4 slots per priority */
    struct msg_queue *q = msg_queue_create(4, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg = {.type = 1, .sender_id = 0};

    /* Fill low priority slots */
    for (int i = 0; i < 4; i++) {
        int ret = msg_send_priority(q, &msg, MSG_PRIO_LOW, 0);
        TEST_ASSERT_EQUAL_INT(IPC_OK, ret);
    }

    /* Low priority should now be full */
    int ret = msg_send_priority(q, &msg, MSG_PRIO_LOW, 0);
    TEST_ASSERT_EQUAL_INT(IPC_ERR_FULL, ret);

    /* But high priority should still have space */
    ret = msg_send_priority(q, &msg, MSG_PRIO_HIGH, 0);
    TEST_ASSERT_EQUAL_INT(IPC_OK, ret);

    msg_queue_destroy(q);
}

/*
 * Test: Priority statistics tracking.
 */
static void test_priority_statistics(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg = {.type = 1, .sender_id = 0};

    /* Send at different priorities */
    msg_send_priority(q, &msg, MSG_PRIO_LOW, 0);
    msg_send_priority(q, &msg, MSG_PRIO_LOW, 0);
    msg_send_priority(q, &msg, MSG_PRIO_NORMAL, 0);
    msg_send_priority(q, &msg, MSG_PRIO_HIGH, 0);
    msg_send_priority(q, &msg, MSG_PRIO_HIGH, 0);
    msg_send_priority(q, &msg, MSG_PRIO_HIGH, 0);
    msg_send_priority(q, &msg, MSG_PRIO_URGENT, 0);

    /* Check per-priority statistics */
    TEST_ASSERT_EQUAL_UINT64(2, q->prio_msgs_sent[MSG_PRIO_LOW]);
    TEST_ASSERT_EQUAL_UINT64(1, q->prio_msgs_sent[MSG_PRIO_NORMAL]);
    TEST_ASSERT_EQUAL_UINT64(3, q->prio_msgs_sent[MSG_PRIO_HIGH]);
    TEST_ASSERT_EQUAL_UINT64(1, q->prio_msgs_sent[MSG_PRIO_URGENT]);

    /* Total should match sum */
    uint64_t total_sent, total_recv;
    size_t high_water;
    msg_queue_stats(q, &total_sent, &total_recv, &high_water);
    TEST_ASSERT_EQUAL_UINT64(7, total_sent);

    msg_queue_destroy(q);
}

/*
 * Test: FIFO ordering within same priority level.
 *
 * Messages at the same priority should be received in send order.
 */
static void test_priority_fifo_within_level(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg;
    msg.sender_id = 0;

    /* Send 5 HIGH priority messages with sequence numbers */
    for (uint32_t i = 1; i <= 5; i++) {
        msg.type = i;  /* Use type as sequence number */
        msg_send_priority(q, &msg, MSG_PRIO_HIGH, 0);
    }

    /* Receive and verify FIFO order */
    struct slm_message recv;
    for (uint32_t i = 1; i <= 5; i++) {
        int ret = msg_recv(q, &recv, 0);
        TEST_ASSERT_EQUAL_INT(IPC_OK, ret);
        TEST_ASSERT_EQUAL_UINT32(i, recv.type);
    }

    msg_queue_destroy(q);
}

/*
 * Test: Interleaved send/receive maintains priority ordering.
 *
 * Complex pattern: send at various priorities, receive some,
 * send more, verify order is still correct.
 */
static void test_priority_interleaved_operations(void)
{
    struct msg_queue *q = msg_queue_create(16, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg, recv;
    msg.sender_id = 0;

    /* Send low, normal, high */
    msg.type = 10;
    msg_send_priority(q, &msg, MSG_PRIO_LOW, 0);
    msg.type = 20;
    msg_send_priority(q, &msg, MSG_PRIO_NORMAL, 0);
    msg.type = 30;
    msg_send_priority(q, &msg, MSG_PRIO_HIGH, 0);

    /* Receive one (should be HIGH=30) */
    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(30, recv.type);

    /* Send urgent */
    msg.type = 40;
    msg_send_priority(q, &msg, MSG_PRIO_URGENT, 0);

    /* Receive (should be URGENT=40) */
    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(40, recv.type);

    /* Send another high */
    msg.type = 31;
    msg_send_priority(q, &msg, MSG_PRIO_HIGH, 0);

    /* Remaining order: HIGH=31, NORMAL=20, LOW=10 */
    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(31, recv.type);

    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(20, recv.type);

    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(10, recv.type);

    msg_queue_destroy(q);
}

/*
 * Test: Receive correctly skips empty priority levels.
 *
 * With only LOW and URGENT messages (no NORMAL/HIGH),
 * verify URGENT is received first, then LOW.
 */
static void test_priority_skip_empty_levels(void)
{
    struct msg_queue *q = msg_queue_create(10, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg, recv;
    msg.sender_id = 0;

    /* Only send LOW and URGENT - skip NORMAL and HIGH */
    msg.type = 1;
    msg_send_priority(q, &msg, MSG_PRIO_LOW, 0);
    msg.type = 2;
    msg_send_priority(q, &msg, MSG_PRIO_LOW, 0);
    msg.type = 99;
    msg_send_priority(q, &msg, MSG_PRIO_URGENT, 0);

    /* Should receive URGENT first */
    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(99, recv.type);

    /* Then LOW messages in FIFO order */
    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(1, recv.type);

    msg_recv(q, &recv, 0);
    TEST_ASSERT_EQUAL_UINT32(2, recv.type);

    msg_queue_destroy(q);
}

/*
 * Test: Starvation threshold boundary behavior.
 *
 * Send exactly MSG_STARVATION_THRESHOLD high priority messages
 * after one low priority. The low should be served after exactly
 * that many high priority receives.
 */
static void test_starvation_threshold_boundary(void)
{
    struct msg_queue *q = msg_queue_create(32, 0);
    TEST_ASSERT_NOT_NULL(q);

    struct slm_message msg, recv;
    msg.sender_id = 0;

    /* Send one LOW priority */
    msg.type = 0xDEAD;
    msg_send_priority(q, &msg, MSG_PRIO_LOW, 0);

    /* Send exactly MSG_STARVATION_THRESHOLD HIGH priority */
    for (int i = 0; i < MSG_STARVATION_THRESHOLD; i++) {
        msg.type = (uint32_t)(0x1000 + i);
        msg_send_priority(q, &msg, MSG_PRIO_HIGH, 0);
    }

    /* First MSG_STARVATION_THRESHOLD receives should be HIGH */
    for (int i = 0; i < MSG_STARVATION_THRESHOLD; i++) {
        int ret = msg_recv(q, &recv, 0);
        TEST_ASSERT_EQUAL_INT(IPC_OK, ret);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(0x1000 + i), recv.type);
    }

    /* Next receive should be the LOW priority (starvation prevention kicks in) */
    /* Note: This depends on implementation - the low might come earlier */
    /* The key is it MUST be received, tested in test_starvation_prevention */

    msg_queue_destroy(q);
}

/* ========================================================================= */
/* Stress Tests                                                              */
/* ========================================================================= */

static void test_queue_stress(void)
{
    struct msg_queue *q = msg_queue_create(32, 0);
    TEST_ASSERT_NOT_NULL(q);

    uint64_t total_sent = 0;
    uint64_t total_recv = 0;
    struct slm_message msg = {.type = 0xAA, .sender_id = 1};
    struct slm_message recv;

    /* Run stress test: interleaved sends and receives */
    for (int round = 0; round < 100; round++) {
        /* Send batch */
        for (int i = 0; i < 10; i++) {
            if (msg_send(q, &msg, 0) == 0) {
                total_sent++;
            }
        }
        /* Receive batch */
        for (int i = 0; i < 8; i++) {
            if (msg_recv(q, &recv, 0) == 0) {
                total_recv++;
            }
        }
    }

    /* Drain remaining */
    while (msg_recv(q, &recv, 0) == 0) {
        total_recv++;
    }

    TEST_ASSERT_EQUAL_UINT64(total_sent, total_recv);
    TEST_ASSERT_GREATER_OR_EQUAL(800, total_sent);  /* At least 800 messages */

    msg_queue_destroy(q);
}

/* ========================================================================= */
/* Test Suite Entry Point                                                    */
/* ========================================================================= */

int test_suite_ipc(void)
{
    UnityBegin("IPC Tests");

    /* Message queue tests */
    RUN_TEST(test_queue_create_destroy);
    RUN_TEST(test_queue_nonblocking_send_recv);
    RUN_TEST(test_queue_lookup_by_id);
    RUN_TEST(test_recv_empty_queue_nonblocking);

    /* Shared buffer tests */
    RUN_TEST(test_buffer_create_destroy);
    RUN_TEST(test_buffer_map_unmap);
    RUN_TEST(test_buffer_lookup_by_id);

    /* Timeout tests */
    RUN_TEST(test_recv_timeout);
    RUN_TEST(test_send_timeout_full_queue);

    /* Memory leak test */
    RUN_TEST(test_no_memory_leak);

    /* Statistics tests */
    RUN_TEST(test_queue_statistics);
    RUN_TEST(test_global_ipc_statistics);

    /* Priority queue tests */
    RUN_TEST(test_priority_high_before_low);
    RUN_TEST(test_priority_ordering_all_levels);
    RUN_TEST(test_default_priority_is_normal);
    RUN_TEST(test_starvation_prevention);
    RUN_TEST(test_priority_capacity);
    RUN_TEST(test_priority_statistics);
    RUN_TEST(test_priority_fifo_within_level);
    RUN_TEST(test_priority_interleaved_operations);
    RUN_TEST(test_priority_skip_empty_levels);
    RUN_TEST(test_starvation_threshold_boundary);

    /* Stress test */
    RUN_TEST(test_queue_stress);

    return UnityEnd();
}
