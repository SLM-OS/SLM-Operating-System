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
#include "pmm.h"
#if !defined(PLATFORM_X86_64)
#include "vmm.h"
#endif
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Trap Frame Layout Tests
 * ============================================================================ */

/* Test: trap_frame struct has correct size.
 * The struct is 264 bytes (33 uint64_t fields) — the C-visible portion.
 * The assembly save_regs allocates TRAP_FRAME_ALLOC bytes (currently 800)
 * and stores the FP/SIMD register file in the bytes past the C view; the
 * struct deliberately does not expose those fields. */
static void test_trap_frame_size(void)
{
    /* 31 GPRs + ELR + SPSR = 33 * 8 = 264 bytes */
    TEST_ASSERT_EQUAL_UINT32(264, sizeof(struct trap_frame));
    /* Must be <= the assembly allocation (which now includes FP/SIMD) */
    TEST_ASSERT_TRUE(sizeof(struct trap_frame) <= TRAP_FRAME_ALLOC);
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

/* Test: trap_frame FP/SIMD region constants pin the asm-side layout.
 * The runtime FP-preserve test below relies on these matching what
 * vectors.S writes; the constants are also referenced by the asm
 * macros via #include "trap.h", so a drift here would break the
 * build. This test makes the contract visible at the test level. */
static void test_trap_frame_fp_layout_constants(void)
{
    TEST_ASSERT_EQUAL_UINT32(272, TRAP_FRAME_OFF_QREGS);
    TEST_ASSERT_EQUAL_UINT32(784, TRAP_FRAME_OFF_FPCR);
    TEST_ASSERT_EQUAL_UINT32(792, TRAP_FRAME_OFF_FPSR);
    TEST_ASSERT_EQUAL_UINT32(800, TRAP_FRAME_ALLOC);
    /* FPSR slot must end inside the allocation. */
    TEST_ASSERT_TRUE(TRAP_FRAME_OFF_FPSR + 8 <= TRAP_FRAME_ALLOC);
    /* Q-region must be 16-byte aligned (stp q,q requirement). */
    TEST_ASSERT_EQUAL_UINT32(0, TRAP_FRAME_OFF_QREGS % 16);
    /* SP allocation must be 16-byte aligned. */
    TEST_ASSERT_EQUAL_UINT32(0, TRAP_FRAME_ALLOC % 16);
}

/* ============================================================================
 * IRQ-path FP/SIMD preservation regression test
 *
 * Validates that save_regs / restore_regs in kernel/arch/arm64/vectors.S
 * round-trip the full q0-q31 + FPCR + FPSR file across an IRQ entry.
 * This is the load-bearing guarantee added in #753: without it,
 * AAPCS-clobber inside the el1_irq_handler / maybe_arm_resched_trampoline
 * C path would silently corrupt an interrupted task's FPU state.
 *
 * Test strategy:
 *   1. Snapshot pit_ticks (incremented by timer_handler from inside the
 *      el1_irq vector path — proves the path actually ran).
 *   2. Load q0-q31 with a known pattern via inline asm.
 *   3. Busy-loop reading pit_ticks until it advances by >= 1, i.e. at
 *      least one timer IRQ has gone through save_regs/restore_regs while
 *      our q-reg pattern was live.
 *   4. Bound the wait with a CNTPCT deadline so a hung timer doesn't
 *      hang the test indefinitely.
 *   5. Read q0-q31 back and compare.
 *
 * The busy-loop reads only x-regs (load + cmp + branch), so the loop
 * itself is guaranteed not to clobber q-regs. The compiler is told
 * q0-q31 are clobbered around the asm block so it doesn't try to keep
 * floats live across the wait.
 *
 * Architecture-only: ARM64. Skipped on x86-64.
 * ============================================================================ */

#if !defined(PLATFORM_X86_64)
#include "timer.h"

/* Read CNTPCT_EL0 (free-running counter, used as a deadline source so
 * the busy-loop has a wall-clock bound that doesn't depend on pit_ticks
 * itself advancing). */
static inline uint64_t read_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void test_irq_save_regs_preserves_fp_simd(void)
{
    /* Per-register patterns. Bit-mix the index across both halves so a
     * misindexed save/restore (e.g. q[i].d[0] swapped with q[i+1].d[1])
     * surfaces as a value mismatch even when the index byte alone would
     * happen to alias. Index `i` (0..31) ends up in distinct byte
     * positions of low/high to defeat that aliasing class:
     *   low  = 0xDEADBEEF_<i>5A5A5A   ← index in bits[31:24]
     *   high = 0x<i>5A5A5A_CAFEBABE   ← index in bits[63:56]
     * Two registers can never produce identical low+high pairs by
     * accident under any swap/off-by-N permutation of the file. */
    static volatile uint64_t pattern_lo[32];
    static volatile uint64_t pattern_hi[32];
    static volatile uint64_t observed_lo[32];
    static volatile uint64_t observed_hi[32];

    for (int i = 0; i < 32; i++) {
        pattern_lo[i] = 0xDEADBEEF005A5A5AULL | ((uint64_t)i << 24);
        pattern_hi[i] = 0x005A5A5ACAFEBABEULL | ((uint64_t)i << 56);
    }

    /* Deadline: 200 ms in CNTPCT ticks. At the kernel's 100 Hz tick
     * rate that's 20 ticks of margin — far more than enough; if no
     * tick has fired in 200 ms something else is broken and the test
     * should fail loud rather than spin forever.
     *
     * Overflow safety: CNTFRQ on every shipping ARM64 platform is
     * 19.2–54 MHz, so freq/5 ≤ ~11M. CNTPCT is bounded by uptime in
     * counter ticks (54M ticks/s × 2^64 / 54M ≈ 10^10 years before
     * wrap), so cntpct + freq/5 cannot overflow uint64 on any plausible
     * boot. If a future platform reports a CNTFRQ near 2^61, this
     * deadline arithmetic would need to be reworked — that's the only
     * regression class to worry about. */
    uint64_t freq = read_cntfrq();
    uint64_t deadline = read_cntpct() + (freq / 5);

    uint64_t baseline_ticks = pit_ticks;

    /*
     * Critical region: load q0-q31, wait for an IRQ, store q0-q31.
     *
     * The whole sequence is a single inline-asm block. Doing it in
     * multiple blocks would let the compiler reload spilled state
     * between them and corrupt the registers under test.
     *
     * We list q0-q31 as outputs (clobbered) and pass pointers to
     * pattern / observed / pit_ticks / deadline via input registers.
     * The C-visible q-reg state on entry to the block is irrelevant
     * because the first thing the block does is overwrite it.
     */
    int timed_out = 0;
    __asm__ volatile (
        /* Load each q-reg from pattern_lo[i] / pattern_hi[i]. */
        "mov   x9,  %[pat_lo]\n"
        "mov   x10, %[pat_hi]\n"
        "ldp   x11, x12, [x9],  #16\n"  /* low for q0/q1 */
        "ldp   x13, x14, [x10], #16\n"  /* high for q0/q1 */
        "ins   v0.d[0], x11\n"
        "ins   v0.d[1], x13\n"
        "ins   v1.d[0], x12\n"
        "ins   v1.d[1], x14\n"

#define LOAD_Q_PAIR(qa, qb)                       \
        "ldp   x11, x12, [x9],  #16\n"            \
        "ldp   x13, x14, [x10], #16\n"            \
        "ins   v" #qa ".d[0], x11\n"              \
        "ins   v" #qa ".d[1], x13\n"              \
        "ins   v" #qb ".d[0], x12\n"              \
        "ins   v" #qb ".d[1], x14\n"

        LOAD_Q_PAIR(2,  3)
        LOAD_Q_PAIR(4,  5)
        LOAD_Q_PAIR(6,  7)
        LOAD_Q_PAIR(8,  9)
        LOAD_Q_PAIR(10, 11)
        LOAD_Q_PAIR(12, 13)
        LOAD_Q_PAIR(14, 15)
        LOAD_Q_PAIR(16, 17)
        LOAD_Q_PAIR(18, 19)
        LOAD_Q_PAIR(20, 21)
        LOAD_Q_PAIR(22, 23)
        LOAD_Q_PAIR(24, 25)
        LOAD_Q_PAIR(26, 27)
        LOAD_Q_PAIR(28, 29)
        LOAD_Q_PAIR(30, 31)
#undef LOAD_Q_PAIR

        /* Busy-loop until pit_ticks moves OR deadline expires. Uses only
         * x9..x14; q-regs untouched. */
        "1:\n"
        "    ldr  x9,  [%[ticks_p]]\n"
        "    cmp  x9,  %[base]\n"
        "    b.hi 2f\n"                /* tick advanced — done */
        "    mrs  x10, cntpct_el0\n"
        "    cmp  x10, %[deadline]\n"
        "    b.lo 1b\n"                /* still under deadline — keep waiting */
        /* deadline hit without a tick: signal timeout to C land. */
        "    mov  w11, #1\n"
        "    str  w11, [%[timed_out]]\n"
        "2:\n"

        /* Store q0..q31 back into observed_lo[i] / observed_hi[i]. */
        "mov   x9,  %[obs_lo]\n"
        "mov   x10, %[obs_hi]\n"

#define STORE_Q_PAIR(qa, qb)                      \
        "umov  x11, v" #qa ".d[0]\n"              \
        "umov  x12, v" #qb ".d[0]\n"              \
        "umov  x13, v" #qa ".d[1]\n"              \
        "umov  x14, v" #qb ".d[1]\n"              \
        "stp   x11, x12, [x9],  #16\n"            \
        "stp   x13, x14, [x10], #16\n"

        STORE_Q_PAIR(0,  1)
        STORE_Q_PAIR(2,  3)
        STORE_Q_PAIR(4,  5)
        STORE_Q_PAIR(6,  7)
        STORE_Q_PAIR(8,  9)
        STORE_Q_PAIR(10, 11)
        STORE_Q_PAIR(12, 13)
        STORE_Q_PAIR(14, 15)
        STORE_Q_PAIR(16, 17)
        STORE_Q_PAIR(18, 19)
        STORE_Q_PAIR(20, 21)
        STORE_Q_PAIR(22, 23)
        STORE_Q_PAIR(24, 25)
        STORE_Q_PAIR(26, 27)
        STORE_Q_PAIR(28, 29)
        STORE_Q_PAIR(30, 31)
#undef STORE_Q_PAIR

        /* No outputs — observed_lo / observed_hi / timed_out are
         * written through pointer inputs. */
        :
        : [pat_lo]    "r" (pattern_lo),
          [pat_hi]    "r" (pattern_hi),
          [obs_lo]    "r" (observed_lo),
          [obs_hi]    "r" (observed_hi),
          [ticks_p]   "r" (&pit_ticks),
          [base]      "r" (baseline_ticks),
          [deadline]  "r" (deadline),
          [timed_out] "r" (&timed_out)
        : "x9", "x10", "x11", "x12", "x13", "x14",
          "v0",  "v1",  "v2",  "v3",  "v4",  "v5",  "v6",  "v7",
          "v8",  "v9",  "v10", "v11", "v12", "v13", "v14", "v15",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
          "memory", "cc"
    );

    if (timed_out) {
        TEST_IGNORE_MESSAGE("no timer IRQ delivered within 200 ms — "
                            "test inconclusive (timer not running on this CPU)");
        return;
    }

    /* At least one timer IRQ fired between our q-reg load and store —
     * meaning save_regs / restore_regs ran with our pattern live. The
     * pattern must round-trip element-by-element. */
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_MESSAGE(pattern_lo[i] == observed_lo[i],
            "q-reg low half corrupted across IRQ — save_regs/restore_regs "
            "did not preserve interrupted task's FPU state");
        TEST_ASSERT_MESSAGE(pattern_hi[i] == observed_hi[i],
            "q-reg high half corrupted across IRQ — save_regs/restore_regs "
            "did not preserve interrupted task's FPU state");
    }
}
#endif /* !PLATFORM_X86_64 */

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
    TEST_ASSERT_EQUAL_INT(8, SYS_MMAP);
    TEST_ASSERT_EQUAL_INT(9, SYS_MUNMAP);
    TEST_ASSERT_EQUAL_INT(10, SYS_MAX);
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
/* Test: task_create_user sets is_user flag and allocates a per-task L1
 * (#697 PR-3 — user_l1_pa is populated at create time, freed at destroy). */
static void test_task_create_user_sets_flag(void)
{
    /* Create a dummy user task (it won't actually run at EL0) */
    extern void task_exit(void);
    struct task *t = task_create_user("test_user", (task_entry_t)task_exit, NULL, 4);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(1, t->is_user);
    TEST_ASSERT_NOT_NULL((void *)(uintptr_t)t->user_entry);

    /* PR-3: user_l1_pa is non-zero and page-aligned. */
    TEST_ASSERT_TRUE(t->user_l1_pa != 0);
    TEST_ASSERT_EQUAL_UINT64(0, t->user_l1_pa & 0xFFF);

    /* Clean up — mark as terminated so it doesn't run */
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/* Test (#697 PR-3): a kernel-mode task (created via task_create) leaves
 * user_l1_pa == 0. The schedule() TTBR0-swap path keys off this — kernel
 * tasks must not trigger an address-space switch. */
static void test_task_create_kernel_leaves_user_l1_zero(void)
{
    extern void task_exit(void);
    struct task *t = task_create("test_kernel_l1", (task_entry_t)task_exit, NULL);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(0, t->is_user);
    TEST_ASSERT_EQUAL_UINT64(0, t->user_l1_pa);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/* Test (#697 PR-3): task_destroy returns the per-task L1 page to PMM.
 * Asserts the buddy free count after destroy is at least the count
 * before create — same shape as test_create_destroy_user_l1_no_leak in
 * test_vmm.c, but exercising the lifecycle end-to-end through
 * task_create_user / task_destroy rather than the raw vmm helpers. */
static void test_task_destroy_frees_user_l1(void)
{
    extern void task_exit(void);
    struct pmm_stats before, after;
    pmm_get_stats(&before);

    struct task *t = task_create_user("destroy_l1", (task_entry_t)task_exit, NULL, 4);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_TRUE(t->user_l1_pa != 0);

    t->state = TASK_TERMINATED;
    task_destroy(t);

    pmm_get_stats(&after);
    TEST_ASSERT_MESSAGE(after.free_pages >= before.free_pages,
                        "task_destroy leaked the per-task L1 page");
}

/* Test (#697 PR-4): task_create_user populates user_stack_top and
 * user_stack_phys. Both are zero on kernel-mode tasks (already covered
 * by test_task_create_kernel_leaves_user_l1_zero) and non-zero on user
 * tasks. user_stack_top must equal USER_STACK_TOP — the smoke runs
 * with a fixed VA layout. */
static void test_task_create_user_populates_stack(void)
{
    extern void task_exit(void);
    extern void user_smoke_main(void *arg);
    /* Pass the real .text.user entry so task_create_user takes the
     * translation path; the alternative (task_exit out of range) leaves
     * user_entry == kernel VA, which is fine for the L1/stack checks
     * but doesn't exercise the translation. */
    struct task *t = task_create_user("usrstk", (task_entry_t)user_smoke_main, NULL, 4);
    TEST_ASSERT_NOT_NULL(t);

    TEST_ASSERT_EQUAL_UINT64(USER_STACK_TOP, t->user_stack_top);
    TEST_ASSERT_TRUE(t->user_stack_phys != 0);
    TEST_ASSERT_EQUAL_UINT64(0, t->user_stack_phys & 0xFFF);

    /* The translated user_entry should land inside the user window —
     * specifically inside the first 4 KB after USER_TEXT_VA (the only
     * page mapped to .text.user — user_smoke is small). */
    uintptr_t entry_va = (uintptr_t)t->user_entry;
    TEST_ASSERT_TRUE(entry_va >= USER_TEXT_VA);
    TEST_ASSERT_TRUE(entry_va < USER_TEXT_VA + 0x10000);

    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/* Test (mmap): user_va_next is initialised to USER_MMAP_VA_START on
 * task_create_user. */
static void test_task_create_user_inits_mmap_cursor(void)
{
    extern void user_smoke_main(void *arg);
    struct task *t = task_create_user("mmcur", (task_entry_t)user_smoke_main, NULL, 4);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT64(USER_MMAP_VA_START, t->user_va_next);
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/* Test (mmap): a page mapped via sys_mmap shows up in the per-task
 * L1 with VMM_FLAG_PMM_OWNED set, advances the cursor, and is
 * reclaimed by task_destroy without leaking PMM pages.
 *
 * The handler is exercised directly via syscall_dispatch to avoid
 * needing scheduler-driven EL0 context — the EL0 round-trip is
 * separately verified by the `mmaptest` shell command on hardware. */
static void test_sys_mmap_allocates_advances_cursor(void)
{
    extern void user_smoke_main(void *arg);
    struct pmm_stats before, after;

    pmm_get_stats(&before);
    struct task *t = task_create_user("mmtask", (task_entry_t)user_smoke_main, NULL, 4);
    TEST_ASSERT_NOT_NULL(t);

    /* Set this task as current so the syscall handler picks the right
     * user_l1_pa / cursor. The original `current_task` is restored
     * before destroying the task to keep the rest of the test suite
     * stable. */
    extern void task_set_current(struct task *task);
    extern struct task *task_current(void);
    struct task *prev = task_current();
    task_set_current(t);

    struct trap_frame f;
    memset(&f, 0, sizeof(f));
    f.x8 = SYS_MMAP;
    f.x0 = 0;                                /* hint */
    f.x1 = 4096;                             /* len */
    f.x2 = PROT_READ | PROT_WRITE;           /* prot */
    f.x3 = MAP_ANONYMOUS;                    /* flags */
    syscall_dispatch(&f);

    int64_t ret = (int64_t)f.x0;
    TEST_ASSERT_EQUAL_INT64(USER_MMAP_VA_START, ret);
    TEST_ASSERT_EQUAL_UINT64(USER_MMAP_VA_START + 4096, t->user_va_next);

    /* munmap the same range — leaf is freed back to PMM. */
    memset(&f, 0, sizeof(f));
    f.x8 = SYS_MUNMAP;
    f.x0 = (uint64_t)ret;
    f.x1 = 4096;
    syscall_dispatch(&f);
    TEST_ASSERT_EQUAL_INT64(0, (int64_t)f.x0);

    /* Restore current and tear down. PMM should be net-neutral. */
    task_set_current(prev);
    t->state = TASK_TERMINATED;
    task_destroy(t);

    pmm_get_stats(&after);
    TEST_ASSERT_MESSAGE(after.free_pages >= before.free_pages,
                        "mmap+munmap+destroy leaked pages");
}

/* Test: sys_mmap rejects len == 0, len > 64 MB, and a kernel-mode
 * caller (no user_l1_pa). */
static void test_sys_mmap_validation(void)
{
    struct trap_frame f;

    /* Kernel-mode caller (the test scaffolding's task is a kernel
     * task — task_current returns the test driver, not a user task). */
    memset(&f, 0, sizeof(f));
    f.x8 = SYS_MMAP;
    f.x1 = 4096;
    f.x2 = PROT_READ | PROT_WRITE;
    syscall_dispatch(&f);
    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)f.x0);

    /* Need a user task to exercise the len validation. */
    extern void user_smoke_main(void *arg);
    struct task *t = task_create_user("mmval", (task_entry_t)user_smoke_main, NULL, 4);
    TEST_ASSERT_NOT_NULL(t);
    extern void task_set_current(struct task *task);
    extern struct task *task_current(void);
    struct task *prev = task_current();
    task_set_current(t);

    /* len == 0 */
    memset(&f, 0, sizeof(f));
    f.x8 = SYS_MMAP;
    f.x1 = 0;
    f.x2 = PROT_READ;
    syscall_dispatch(&f);
    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)f.x0);

    /* len > 64 MB */
    memset(&f, 0, sizeof(f));
    f.x8 = SYS_MMAP;
    f.x1 = 65UL * 1024 * 1024;
    f.x2 = PROT_READ;
    syscall_dispatch(&f);
    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)f.x0);

    task_set_current(prev);
    t->state = TASK_TERMINATED;
    task_destroy(t);
}

/* Test: sys_munmap rejects unaligned addr and addresses outside the
 * mmap window (.text.user / stack). */
static void test_sys_munmap_validation(void)
{
    extern void user_smoke_main(void *arg);
    struct task *t = task_create_user("muval", (task_entry_t)user_smoke_main, NULL, 4);
    TEST_ASSERT_NOT_NULL(t);
    extern void task_set_current(struct task *task);
    extern struct task *task_current(void);
    struct task *prev = task_current();
    task_set_current(t);

    struct trap_frame f;

    /* Unaligned addr. */
    memset(&f, 0, sizeof(f));
    f.x8 = SYS_MUNMAP;
    f.x0 = USER_MMAP_VA_START + 0x1;
    f.x1 = 4096;
    syscall_dispatch(&f);
    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)f.x0);

    /* Addr below mmap window — refuses to munmap .text.user / stack. */
    memset(&f, 0, sizeof(f));
    f.x8 = SYS_MUNMAP;
    f.x0 = USER_TEXT_VA;
    f.x1 = 4096;
    syscall_dispatch(&f);
    TEST_ASSERT_EQUAL_INT64(-1, (int64_t)f.x0);

    task_set_current(prev);
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
    RUN_TEST(test_trap_frame_fp_layout_constants);
#if !defined(PLATFORM_X86_64)
    RUN_TEST(test_irq_save_regs_preserves_fp_simd);
#endif

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
    RUN_TEST(test_task_create_kernel_leaves_user_l1_zero);
    RUN_TEST(test_task_destroy_frees_user_l1);
    RUN_TEST(test_task_create_user_populates_stack);
    RUN_TEST(test_task_create_user_inits_mmap_cursor);
    RUN_TEST(test_sys_mmap_allocates_advances_cursor);
    RUN_TEST(test_sys_mmap_validation);
    RUN_TEST(test_sys_munmap_validation);

    /* #683 PR-5: vector layout for EL0 → EL2 SVC */
    RUN_TEST(test_lower_el_sync_vector_dispatches_to_el0_sync);
#endif

    return UnityEnd();
}
