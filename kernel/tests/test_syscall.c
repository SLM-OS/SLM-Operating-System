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

/* Test: SYS_INFER rejects in_len that would overflow `in_len * sizeof(float)`
 * before the validate_user_ptr bounds check. Regression for IPC-C1. */
static void test_syscall_infer_overflow_in_len(void)
{
    struct trap_frame frame;
    /* A small scratch buffer — the input pointer must be non-NULL so that
     * validate_user_ptr only fails due to the overflow guard, not a NULL
     * pointer. */
    uint32_t scratch[4] = {0};

    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_INFER;
    frame.x0 = 0;                      /* model idx */
    frame.x1 = (uint64_t)scratch;      /* input */
    frame.x2 = 0x40000001;             /* in_len: triggers in_len > UINT32_MAX/4 */
    frame.x3 = (uint64_t)scratch;      /* output */
    frame.x4 = 1;                      /* out_len */

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)frame.x0);
}

/* Test: SYS_INFER rejects out_len overflow symmetrically. Regression for IPC-C1. */
static void test_syscall_infer_overflow_out_len(void)
{
    struct trap_frame frame;
    uint32_t scratch[4] = {0};

    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_INFER;
    frame.x0 = 0;
    frame.x1 = (uint64_t)scratch;
    frame.x2 = 1;
    frame.x3 = (uint64_t)scratch;
    frame.x4 = 0x40000001;             /* out_len triggers the overflow guard */

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)frame.x0);
}

/* Test: SYS_RECV with a NULL topic_out fails validation. Regression for IPC-M2. */
static void test_syscall_recv_null_topic_out(void)
{
    struct trap_frame frame;
    char data_buf[64];
    memset(&frame, 0, sizeof(frame));
    memset(data_buf, 0, sizeof(data_buf));

    frame.x8 = SYS_RECV;
    frame.x0 = 0;                      /* topic_out = NULL */
    frame.x1 = (uint64_t)data_buf;
    frame.x2 = sizeof(data_buf);
    frame.x3 = 0;

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)frame.x0);
}

/* Test: SYS_SEND rejects data that is not NUL-terminated within `len`.
 * Regression for IPC-H2. */
static void test_syscall_send_missing_nul(void)
{
    struct trap_frame frame;
    char topic[] = "t\0xxxxxxxxxxxxx";  /* NUL within MSG_ROUTER_TOPIC_LEN */
    /* Data buffer intentionally lacks a NUL in the last byte. */
    char data[4] = {'a', 'b', 'c', 'd'};

    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_SEND;
    frame.x0 = (uint64_t)topic;
    frame.x1 = (uint64_t)data;
    frame.x2 = sizeof(data);           /* last byte is 'd', not '\0' */

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)frame.x0);
}

/* Test: SYS_SEND rejects len == 0. Regression for IPC-H2. */
static void test_syscall_send_zero_len(void)
{
    struct trap_frame frame;
    char topic[] = "t\0xxxxxxxxxxxxx";
    char data = 0;

    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_SEND;
    frame.x0 = (uint64_t)topic;
    frame.x1 = (uint64_t)&data;
    frame.x2 = 0;

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)frame.x0);
}

/* Test: SYS_SEND rejects a topic buffer with no NUL terminator in the first
 * MSG_ROUTER_TOPIC_LEN bytes. Regression for IPC-H2. */
static void test_syscall_send_topic_no_nul(void)
{
    struct trap_frame frame;
    /* 16 bytes, no NUL byte anywhere. */
    char topic[16];
    memset(topic, 'x', sizeof(topic));
    char data[] = "hi";

    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_SEND;
    frame.x0 = (uint64_t)topic;
    frame.x1 = (uint64_t)data;
    frame.x2 = sizeof(data);

    syscall_dispatch(&frame);

    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)frame.x0);
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
    TEST_ASSERT_EQUAL_INT(7, SYS_TOUCH_BLOCK);
    TEST_ASSERT_EQUAL_INT(8, SYS_MAX);
}

/* ============================================================================
 * SYS_TOUCH_BLOCK Tests (#123)
 * ============================================================================ */

/*
 * Test: SYS_TOUCH_BLOCK with an invalid handle returns -1.
 * Uses a fabricated trap_frame with block_index=0xFFFF (the null
 * sentinel) to verify the handler rejects bad handles.
 */
static void test_syscall_touch_block_invalid_handle(void)
{
    struct trap_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.x8 = SYS_TOUCH_BLOCK;
    frame.x0 = 0xFFFF;  /* null block_index sentinel */
    frame.x1 = 0xFF;    /* null pool_id sentinel */
    frame.x2 = 0;       /* generation */

    syscall_dispatch(&frame);

    /* rust_model_touch rejects the null handle → -1 */
    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)frame.x0);
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

#if !defined(PLATFORM_X86_64)
/*
 * Test (#683 PR-5): the lower-EL AArch64 sync slot at offset 0x400
 * inside the ARM64 vector table dispatches to `el0_sync`.
 *
 * Why this matters: with the kernel at EL2h+TGE (Pi 5 after #683
 * PR-2), EL0 SVC exceptions are taken to VBAR_EL2 + 0x400. The
 * vector table is shared between QEMU (EL1h) and Pi 5/Jetson (EL2h)
 * because writes to VBAR_EL1 redirect to VBAR_EL2 under VHE; the
 * same single block of code at `exception_vectors` services both ELs.
 *
 * This test decodes the AArch64 unconditional-branch instruction at
 * the slot and asserts it targets `el0_sync` — proving the SVC path
 * lands at the C handler from any kernel EL.
 *
 * AArch64 B encoding (ARM ARM C6.2.34): bits[31:26] = 000101,
 * bits[25:0] = signed imm26 (offset-by-4 in word units).
 */
/* Declared as char[] symbols so we can take their address as a data
 * pointer without converting a function pointer to `void *` (ISO C
 * forbids that and the kernel build is `-Wpedantic -Werror`). The
 * actual definitions are in vectors.S — labels, no type info. */
extern char exception_vectors[];
extern char el0_sync[];

static void test_lower_el_sync_vector_dispatches_to_el0_sync(void)
{
    /* Vector table requires 2 KB alignment. */
    TEST_ASSERT_EQUAL_UINT64(0,
        (uint64_t)(uintptr_t)exception_vectors & 0x7FF);

    uint32_t *slot = (uint32_t *)((uintptr_t)exception_vectors + 0x400);
    uint32_t insn = *slot;

    /* `B <label>` opcode top-6 bits = 0b000101 → 0x14000000. */
    TEST_ASSERT_EQUAL_HEX32(0x14000000, insn & 0xFC000000);

    /* Sign-extend the 26-bit immediate, scale by 4, add to slot PC. */
    int32_t imm26 = (int32_t)(insn << 6) >> 6;          /* sign-extend */
    uintptr_t target = (uintptr_t)slot + ((int64_t)imm26 << 2);

    TEST_ASSERT_EQUAL_UINT64((uint64_t)(uintptr_t)el0_sync,
                             (uint64_t)target);
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
    RUN_TEST(test_syscall_infer_overflow_in_len);
    RUN_TEST(test_syscall_infer_overflow_out_len);
    RUN_TEST(test_syscall_recv_null_topic_out);
    RUN_TEST(test_syscall_send_missing_nul);
    RUN_TEST(test_syscall_send_zero_len);
    RUN_TEST(test_syscall_send_topic_no_nul);
    RUN_TEST(test_syscall_recv_no_message);

    /* SYS_TOUCH_BLOCK (#123) */
    RUN_TEST(test_syscall_touch_block_invalid_handle);

    /* Syscall number constants */
    RUN_TEST(test_syscall_numbers_contiguous);

#if !defined(PLATFORM_X86_64)
    /* User task creation */
    RUN_TEST(test_task_create_user_sets_flag);

    /* #683 PR-5: vector layout for EL0 → EL2 SVC */
    RUN_TEST(test_lower_el_sync_vector_dispatches_to_el0_sync);
#endif

    return UnityEnd();
}
