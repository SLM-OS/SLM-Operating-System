/*
 * test_gpu_chan_state.c - the cold channel's dispatch gate (#1171,
 * kernel/gpu/gpu_chan_state.c).
 *
 * Before #1171 a program past its deadline let the next dispatcher onto
 * the channel: its work queued behind the hung program and timed out
 * too, 2 s with IRQs masked each, until the breaker tripped. Now a
 * dispatcher is let in only while the channel is up and no program owns
 * it, and is turned away (CPU fallback) while a program is past its
 * deadline, during a recovery, and after one failed.
 */
#include "unity.h"
#include "gpu_chan_state.h"

#include <stdint.h>

static void test_idle_channel_is_go(void)
{
    struct gpu_chan_state c;
    gpu_chan_init(&c);
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_GO, gpu_chan_gate(&c, false, 0));
    TEST_ASSERT_EQUAL_UINT32(0, c.epoch);
}

static void test_flight_waits_then_retires_on_its_semaphore(void)
{
    struct gpu_chan_state c;
    gpu_chan_init(&c);
    gpu_chan_flight_start(&c, 5000u);
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_WAIT, gpu_chan_gate(&c, false, 1000u));
    /* Any CPU that sees the end semaphore retires the program. */
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_GO, gpu_chan_gate(&c, true, 1000u));
    TEST_ASSERT_FALSE(c.flight);
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_GO, gpu_chan_gate(&c, false, 2000u));
}

/* The #1171 wedge: before, an expired flight was "free". */
static void test_expired_flight_is_down_not_free(void)
{
    struct gpu_chan_state c;
    gpu_chan_init(&c);
    gpu_chan_flight_start(&c, 5000u);
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_DOWN, gpu_chan_gate(&c, false, 5000u));
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_DOWN, gpu_chan_gate(&c, false, 9000u));
    TEST_ASSERT_TRUE(c.flight);
    /* A program that ends late after all is retired as usual. */
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_GO, gpu_chan_gate(&c, true, 9000u));
    /* Or its owner finishes it. */
    gpu_chan_flight_start(&c, 10000u);
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_DOWN, gpu_chan_gate(&c, false, 10001u));
    gpu_chan_flight_end(&c);
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_GO, gpu_chan_gate(&c, false, 10002u));
}

static void test_recovery_turns_everyone_away_then_opens_a_new_epoch(void)
{
    struct gpu_chan_state c;
    gpu_chan_init(&c);
    gpu_chan_flight_start(&c, 5000u);
    TEST_ASSERT_TRUE(gpu_chan_recover_begin(&c));
    TEST_ASSERT_FALSE(c.flight);
    TEST_ASSERT_EQUAL_UINT32(GPU_CHAN_RECOVERING, c.mode);
    /* Even a "finished" end semaphore doesn't let anyone in now. */
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_DOWN, gpu_chan_gate(&c, true, 1u));
    /* One recovery at a time. */
    TEST_ASSERT_FALSE(gpu_chan_recover_begin(&c));
    gpu_chan_recover_end(&c, true);
    TEST_ASSERT_EQUAL_UINT32(GPU_CHAN_UP, c.mode);
    TEST_ASSERT_EQUAL_UINT32(1, c.epoch);
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_GO, gpu_chan_gate(&c, false, 2u));
}

static void test_failed_recovery_is_dead_and_can_be_retried(void)
{
    struct gpu_chan_state c;
    gpu_chan_init(&c);
    TEST_ASSERT_TRUE(gpu_chan_recover_begin(&c));
    gpu_chan_recover_end(&c, false);
    TEST_ASSERT_EQUAL_UINT32(GPU_CHAN_DEAD, c.mode);
    TEST_ASSERT_EQUAL_UINT32(0, c.epoch);
    TEST_ASSERT_EQUAL_UINT32(1, c.failed);
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_DOWN, gpu_chan_gate(&c, false, 0));
    /* `nvgpu cold recover` tries again from DEAD. */
    TEST_ASSERT_TRUE(gpu_chan_recover_begin(&c));
    gpu_chan_recover_end(&c, true);
    TEST_ASSERT_EQUAL_INT(GPU_CHAN_GO, gpu_chan_gate(&c, false, 0));
    TEST_ASSERT_EQUAL_UINT32(1, c.epoch);
}

int test_suite_gpu_chan_state(void)
{
    UnityBegin("test_gpu_chan_state.c");
    RUN_TEST(test_idle_channel_is_go);
    RUN_TEST(test_flight_waits_then_retires_on_its_semaphore);
    RUN_TEST(test_expired_flight_is_down_not_free);
    RUN_TEST(test_recovery_turns_everyone_away_then_opens_a_new_epoch);
    RUN_TEST(test_failed_recovery_is_dead_and_can_be_retried);
    return UnityEnd();
}
