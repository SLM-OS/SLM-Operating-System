/*
 * test_syscall.c - Tests for the syscall dispatch infrastructure
 *
 * Tests the syscall dispatch table, trap frame layout, and handler
 * functions by constructing mock trap frames and calling dispatch
 * directly from EL1.
 *
 * Note: Full EL0 execution tests are deferred pending VMM_FLAG_USER
 * investigation on QEMU. These tests validate the kernel-side syscall
 * infrastructure independently.
 */

#include "test_harness.h"
#include "unity.h"
#include "trap.h"
#include "syscall.h"
#include "task.h"
#include "string.h"
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Trap Frame Layout Tests
 * ============================================================================ */

/* Test: trap_frame struct has correct size.
 * The struct is 264 bytes (33 uint64_t fields). The assembly save_regs
 * allocates 272 bytes (264 + 8 padding for 16-byte SP alignment). */
static void test_trap_frame_size(void)
{
    /* 31 GPRs + ELR + SPSR = 33 * 8 = 264 bytes */
    TEST_ASSERT_EQUAL_UINT32(264, sizeof(struct trap_frame));
    /* Must be <= the assembly allocation */
    TEST_ASSERT_TRUE(sizeof(struct trap_frame) <= 272);
}

/* Test: trap_frame fields are at correct offsets */
static void test_trap_frame_offsets(void)
{
    /* x0 at offset 0 */
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)__builtin_offsetof(struct trap_frame, x0));
    /* x8 at offset 64 (8 * 8) */
    TEST_ASSERT_EQUAL_UINT32(64, (uint32_t)__builtin_offsetof(struct trap_frame, x8));
    /* x30 at offset 240 (30 * 8) */
    TEST_ASSERT_EQUAL_UINT32(240, (uint32_t)__builtin_offsetof(struct trap_frame, x30));
    /* elr at offset 248 */
    TEST_ASSERT_EQUAL_UINT32(248, (uint32_t)__builtin_offsetof(struct trap_frame, elr));
    /* spsr at offset 256 */
    TEST_ASSERT_EQUAL_UINT32(256, (uint32_t)__builtin_offsetof(struct trap_frame, spsr));
}

/* ============================================================================
 * Syscall Dispatch Tests
 * ============================================================================ */

/* Test: syscall_dispatch with SYS_YIELD returns 0 */
static void test_syscall_yield_returns_zero(void)
{
    struct trap_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_YIELD;

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(0, (int64_t)frame.x0);
}

/* Test: syscall_dispatch with invalid syscall number returns -1 */
static void test_syscall_invalid_number(void)
{
    struct trap_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.x8 = 999;  /* Way beyond SYS_MAX */

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)frame.x0);
}

/* Test: syscall_dispatch with SYS_MAX returns -1 (boundary) */
static void test_syscall_boundary(void)
{
    struct trap_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_MAX;  /* Exactly at boundary */

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)frame.x0);
}

/* Test: SYS_LOG writes to UART without crash */
static void test_syscall_log(void)
{
    struct trap_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_LOG;
    frame.x0 = (uint64_t)"[test] syscall log\r\n";
    frame.x1 = 20;

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(0, (int64_t)frame.x0);
}

/* Test: SYS_SLEEP with 0ms completes immediately */
static void test_syscall_sleep_zero(void)
{
    struct trap_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_SLEEP;
    frame.x0 = 0;  /* 0 ms */

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(0, (int64_t)frame.x0);
}

/* Test: SYS_INFER with invalid model index returns error */
static void test_syscall_infer_invalid_model(void)
{
    struct trap_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_INFER;
    frame.x0 = 99;   /* Invalid model index */
    frame.x1 = 0;    /* NULL input */
    frame.x2 = 0;
    frame.x3 = 0;    /* NULL output */
    frame.x4 = 0;

    syscall_dispatch(&frame);

    /* Should return negative (validation failure or inference error) */
    TEST_ASSERT_TRUE((int64_t)frame.x0 < 0);
}

/* Test: SYS_RECV with no pending messages returns -1 */
static void test_syscall_recv_no_message(void)
{
    struct trap_frame frame;
    char topic_buf[16];
    char data_buf[64];
    memset(&frame, 0, sizeof(frame));
    memset(topic_buf, 0, sizeof(topic_buf));
    memset(data_buf, 0, sizeof(data_buf));

    frame.x8 = SYS_RECV;
    frame.x0 = (uint64_t)topic_buf;
    frame.x1 = (uint64_t)data_buf;
    frame.x2 = sizeof(data_buf);
    frame.x3 = 0;  /* No timeout */

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)frame.x0);
}

/* ============================================================================
 * Syscall Number Constants Tests
 * ============================================================================ */

/* Test: syscall numbers are contiguous starting from 0 */
static void test_syscall_numbers_contiguous(void)
{
    TEST_ASSERT_EQUAL_INT(0, SYS_EXIT);
    TEST_ASSERT_EQUAL_INT(1, SYS_YIELD);
    TEST_ASSERT_EQUAL_INT(2, SYS_SEND);
    TEST_ASSERT_EQUAL_INT(3, SYS_RECV);
    TEST_ASSERT_EQUAL_INT(4, SYS_INFER);
    TEST_ASSERT_EQUAL_INT(5, SYS_SLEEP);
    TEST_ASSERT_EQUAL_INT(6, SYS_LOG);
    TEST_ASSERT_EQUAL_INT(7, SYS_MAX);
}

/* ============================================================================
 * Task User Mode Tests
 * ============================================================================ */

#if !defined(PLATFORM_X86_64)
/* Test: task_create_user sets is_user flag */
static void test_task_create_user_sets_flag(void)
{
    /* Create a dummy user task (it won't actually run at EL0) */
    extern void task_exit(void);
    struct task *t = task_create_user("test_user", (task_entry_t)task_exit, NULL, 4);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(1, t->is_user);
    TEST_ASSERT_NOT_NULL((void *)(uintptr_t)t->user_entry);

    /* Clean up — mark as terminated so it doesn't run */
    t->state = TASK_TERMINATED;
    task_destroy(t);
}
#endif

/* ============================================================================
 * Test Suite Runner
 * ============================================================================ */

int test_suite_syscall(void)
{
    UnityBegin("Syscall Infrastructure Tests");

    /* Trap frame layout */
    RUN_TEST(test_trap_frame_size);
    RUN_TEST(test_trap_frame_offsets);

    /* Syscall dispatch */
    RUN_TEST(test_syscall_yield_returns_zero);
    RUN_TEST(test_syscall_invalid_number);
    RUN_TEST(test_syscall_boundary);
    RUN_TEST(test_syscall_log);
    RUN_TEST(test_syscall_sleep_zero);
    RUN_TEST(test_syscall_infer_invalid_model);
    RUN_TEST(test_syscall_recv_no_message);

    /* Syscall number constants */
    RUN_TEST(test_syscall_numbers_contiguous);

#if !defined(PLATFORM_X86_64)
    /* User task creation */
    RUN_TEST(test_task_create_user_sets_flag);
#endif

    return UnityEnd();
}
