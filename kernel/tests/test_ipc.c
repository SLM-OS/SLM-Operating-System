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

    /* Stress test */
    RUN_TEST(test_queue_stress);

    return UnityEnd();
}
