/*
 * test_wait.c - wall-clock bounded waits for kernel tests (see test_wait.h)
 */

#include "test_wait.h"
#include "../include/sched.h"
#include "../include/task.h"
#include "../include/timer.h"
#include "../include/uart.h"

/* Re-poll marks after a timeout, in microseconds after the bound. */
#define TEST_WAIT_REPOLL_N 4
static const long long repoll_marks_us[TEST_WAIT_REPOLL_N] = {
    1000, 10000, 100000, 1000000
};

long long test_wait_us_between(uint64_t a, uint64_t b)
{
    uint64_t f = timer_get_frequency();
    if (f == 0 || b < a) {
        return 0;
    }
    return (long long)((b - a) * 1000000ull / f);
}

static uint64_t ticks_for_us(uint64_t us)
{
    uint64_t f = timer_get_frequency();
    /* Round up so a bound is never shorter than asked. */
    return (us * f + 999999ull) / 1000000ull;
}

/* After a timeout: yield for 1 s, recording the condition (and value) at
 * each mark and when the condition first held. Then log one line. */
static void repoll_and_log(const char *what, test_wait_cond_t cond,
                           test_wait_val_t val, void *ctx,
                           uint64_t t_bound, uint64_t timeout_us)
{
    int held[TEST_WAIT_REPOLL_N];
    long long v_at[TEST_WAIT_REPOLL_N];
    long long v_bound = val ? val(ctx) : 0;
    long long first_true_us = -1;
    int next = 0;

    for (;;) {
        long long dt = test_wait_us_between(t_bound, timer_get_count());
        bool c = cond(ctx);
        if (c && first_true_us < 0) {
            first_true_us = dt;
        }
        while (next < TEST_WAIT_REPOLL_N && dt >= repoll_marks_us[next]) {
            held[next] = c ? 1 : 0;
            v_at[next] = val ? val(ctx) : 0;
            next++;
        }
        if (next >= TEST_WAIT_REPOLL_N) {
            break;
        }
        yield();
    }

    uart_printf("[test_wait] %s: TIMEOUT after %llu us; after the bound "
                "+1ms=%d +10ms=%d +100ms=%d +1000ms=%d; first held %lld us "
                "after the bound (-1 = never)\n",
                what, (unsigned long long)timeout_us, held[0], held[1],
                held[2], held[3], first_true_us);
    if (val) {
        uart_printf("[test_wait] %s: value at bound %lld, +1ms %lld, "
                    "+10ms %lld, +100ms %lld, +1000ms %lld\n",
                    what, v_bound, v_at[0], v_at[1], v_at[2], v_at[3]);
    }
}

long long test_wait_until_v(const char *what, test_wait_cond_t cond,
                            test_wait_val_t val, void *ctx,
                            uint64_t timeout_us)
{
    uint64_t start = timer_get_count();
    uint64_t limit = ticks_for_us(timeout_us);

    for (;;) {
        if (cond(ctx)) {
            long long el = test_wait_us_between(start, timer_get_count());
            if ((uint64_t)el > timeout_us / 4u) {
                uart_printf("[test_wait] %s: met after %lld us, over a "
                            "quarter of the %llu us bound\n",
                            what, el, (unsigned long long)timeout_us);
            }
            return el;
        }
        uint64_t now = timer_get_count();
        if (now - start >= limit) {
            repoll_and_log(what, cond, val, ctx, now, timeout_us);
            return -1;
        }
        yield();
    }
}

long long test_wait_until(const char *what, test_wait_cond_t cond, void *ctx,
                          uint64_t timeout_us)
{
    return test_wait_until_v(what, cond, NULL, ctx, timeout_us);
}

static bool cond_flag(void *ctx)
{
    return __atomic_load_n((volatile bool *)ctx, __ATOMIC_ACQUIRE);
}

long long test_wait_flag(const char *what, volatile bool *flag,
                         uint64_t timeout_us)
{
    return test_wait_until(what, cond_flag, (void *)flag, timeout_us);
}

struct int_target {
    volatile int *v;
    int target;
};

static bool cond_int_at_least(void *ctx)
{
    struct int_target *it = ctx;
    return __atomic_load_n(it->v, __ATOMIC_ACQUIRE) >= it->target;
}

static long long val_int(void *ctx)
{
    struct int_target *it = ctx;
    return __atomic_load_n(it->v, __ATOMIC_ACQUIRE);
}

long long test_wait_int_at_least(const char *what, volatile int *v, int target,
                                 uint64_t timeout_us)
{
    struct int_target it = { v, target };
    return test_wait_until_v(what, cond_int_at_least, val_int, &it, timeout_us);
}

static bool cond_u32_nonzero(void *ctx)
{
    return __atomic_load_n((volatile uint32_t *)ctx, __ATOMIC_ACQUIRE) != 0;
}

long long test_wait_u32_nonzero(const char *what, volatile uint32_t *v,
                                uint64_t timeout_us)
{
    return test_wait_until(what, cond_u32_nonzero, (void *)v, timeout_us);
}

static bool cond_task_terminated(void *ctx)
{
    struct task *t = ctx;
    return __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TASK_TERMINATED;
}

static long long val_task_state(void *ctx)
{
    struct task *t = ctx;
    return (long long)__atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
}

long long test_wait_task_terminated(const char *what, struct task *t,
                                    uint64_t timeout_us)
{
    return test_wait_until_v(what, cond_task_terminated, val_task_state, t,
                             timeout_us);
}
