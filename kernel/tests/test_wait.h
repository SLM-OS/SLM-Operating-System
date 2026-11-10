/*
 * test_wait.h - wall-clock bounded waits for kernel tests
 *
 * A test that waits for another task to set a flag must bound the wait by
 * time, not by a count of yield() calls or loop iterations. A yield that
 * finds nothing else to run costs about 2.3 us on Pi 5 with the NC axis and
 * about 0.1 us without it (#1193), so the same count expires 20 times sooner
 * in wall time once a context switch gets faster, while the work the test
 * waits for takes as long as before (#1202, #1205). These helpers yield
 * until the condition holds or the timeout passes on the hardware counter
 * (timer_get_count()), and return the elapsed time.
 *
 * On a timeout each helper keeps yielding for 1 s and logs whether the
 * condition holds 1, 10, 100 and 1000 ms after the bound and when it first
 * held, so a failure log separates "too slow" (it holds late) from "never
 * happened" (it never holds). A pass that used more than a quarter of the
 * bound is logged too, so a bound that is getting tight shows up before it
 * fails.
 *
 * Choosing a bound: at least what the old count took on the slowest kernel
 * measured, Pi 5 with the NC axis (2.3 us per bare yield, C1 run 2), rounded
 * up, and never less than 1 s unless measured, because a yield inside a
 * wait loop can run other tasks for milliseconds (C1 run 2 measured
 * 1.16 to 5.3 ms per yield in test_component_cleanup_ctx_is_per_task). A
 * pass returns as soon as the condition holds, so a generous bound costs
 * nothing on success.
 */

#ifndef TEST_WAIT_H
#define TEST_WAIT_H

#include <stdbool.h>
#include <stdint.h>

struct task;

/* Condition: true when the wait is over. Called with the ctx given to the
 * wait, from the waiting task, once per poll. */
typedef bool (*test_wait_cond_t)(void *ctx);

/* Optional value to log at the bound and at each re-poll (for example a
 * free-page count), or NULL. */
typedef long long (*test_wait_val_t)(void *ctx);

/* Bounds in microseconds, written readably. */
#define TEST_WAIT_MS(ms) ((uint64_t)(ms) * 1000u)
#define TEST_WAIT_S(s)   ((uint64_t)(s) * 1000000u)

/*
 * Yield until cond(ctx) is true or timeout_us of wall clock has passed.
 * Returns the elapsed microseconds when the condition held (0 if it held on
 * entry), or -1 on a timeout, after the 1 s of re-polls and the log line.
 * `what` names the wait in the log.
 */
long long test_wait_until(const char *what, test_wait_cond_t cond, void *ctx,
                          uint64_t timeout_us);

/* As test_wait_until, also logging val(ctx) at the bound and each re-poll. */
long long test_wait_until_v(const char *what, test_wait_cond_t cond,
                            test_wait_val_t val, void *ctx,
                            uint64_t timeout_us);

/* Wait for *flag to be true (load-acquire). */
long long test_wait_flag(const char *what, volatile bool *flag,
                         uint64_t timeout_us);

/* Wait for *v >= target (load-acquire). */
long long test_wait_int_at_least(const char *what, volatile int *v, int target,
                                 uint64_t timeout_us);

/* Wait for *v != 0 (load-acquire). */
long long test_wait_u32_nonzero(const char *what, volatile uint32_t *v,
                                uint64_t timeout_us);

/* Wait for t->state == TASK_TERMINATED. */
long long test_wait_task_terminated(const char *what, struct task *t,
                                    uint64_t timeout_us);

/* Microseconds between two timer_get_count() stamps (b >= a). */
long long test_wait_us_between(uint64_t a, uint64_t b);

#endif /* TEST_WAIT_H */
