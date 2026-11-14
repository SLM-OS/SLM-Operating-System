/*
 * switch_check.c - switch-in window test instruments (#1234). See
 * switch_check.h. Compiles to an empty unit unless CONFIG_SWITCH_CHECK or
 * CONFIG_SWITCH_WINDOW_TEST is defined.
 */

#include "switch_check.h"

#if defined(CONFIG_SWITCH_CHECK) || defined(CONFIG_SWITCH_WINDOW_TEST)
#include "config.h"
#include "task.h"
#include "timer.h"
#include "uart.h"
#include "smp.h"
#include "sched_diag.h"
#include <stdbool.h>
#endif

#if defined(CONFIG_SWITCH_CHECK)

static uint32_t sc_restored[MAX_TASKS];  /* written by the switch-in CPU */
static uint32_t sc_saved[MAX_TASKS];     /* written by the switch-out CPU */
static uint32_t sc_owner[MAX_TASKS];     /* cpu + 1 while a CPU owns it */
static struct switch_check_stats sc_stats;

static int sc_index(struct task *t)
{
    struct task *base = task_slot(0);
    if (!t || !base || t < base || t >= base + MAX_TASKS) {
        return -1;                       /* not a task_table task */
    }
    return (int)(t - base);
}

/* Under the incoming CPU's rq_lock, before switch_to restores next. */
void switch_check_in(struct task *next, uint32_t cpu)
{
    int i = sc_index(next);
    if (i < 0) {
        return;
    }
    uint32_t g = __atomic_load_n(&sc_restored[i], __ATOMIC_RELAXED);
    uint32_t s = __atomic_load_n(&sc_saved[i], __ATOMIC_ACQUIRE);
    uint32_t expect = 0;
    int dual = !__atomic_compare_exchange_n(&sc_owner[i], &expect, cpu + 1, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    int bad = (s != g);
    __atomic_fetch_add(&sc_stats.switch_ins, 1, __ATOMIC_RELAXED);
    if (bad || dual) {
        if (bad) {
            __atomic_fetch_add(&sc_stats.bad_gen, 1, __ATOMIC_RELAXED);
        }
        if (dual) {
            __atomic_fetch_add(&sc_stats.bad_dual, 1, __ATOMIC_RELAXED);
        }
        uint32_t zero = 0;
        uint32_t kind = (uint32_t)(bad ? 1 : 0) | (uint32_t)(dual ? 2 : 0);
        if (__atomic_compare_exchange_n(&sc_stats.first_kind, &zero, kind, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            sc_stats.first_task_id = next->id;
            sc_stats.first_cpu = cpu;
            sc_stats.first_owner = expect;
            /* Print at once: the stale context about to run may wedge or
             * fault the system before any test can report. */
            uart_printf_unlocked("\n[switch_check] FIRST: task id=%u '%s' switched in on "
                                 "cpu %u: %s%s (restored_gen=%u saved_gen=%u owner=%u)\n",
                                 next->id, next->name, cpu,
                                 bad ? "previous save not complete" : "",
                                 dual ? (bad ? ", owned by another CPU" : "owned by another CPU") : "",
                                 g, s, expect);
        }
        if (dual) {
            /* Take ownership anyway so the next switch-out releases it. */
            __atomic_store_n(&sc_owner[i], cpu + 1, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&sc_restored[i], g + 1, __ATOMIC_RELAXED);
}

/* On the CPU that ran prev, after switch_to saved it, before on_cpu = 0. */
void switch_check_out(struct task *prev)
{
    int i = sc_index(prev);
    if (i < 0) {
        return;
    }
    __atomic_store_n(&sc_owner[i], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sc_saved[i], __atomic_load_n(&sc_restored[i], __ATOMIC_RELAXED),
                     __ATOMIC_RELEASE);
}

void switch_check_snapshot(struct switch_check_stats *out)
{
    out->switch_ins = __atomic_load_n(&sc_stats.switch_ins, __ATOMIC_RELAXED);
    out->bad_gen = __atomic_load_n(&sc_stats.bad_gen, __ATOMIC_RELAXED);
    out->bad_dual = __atomic_load_n(&sc_stats.bad_dual, __ATOMIC_RELAXED);
    out->first_kind = __atomic_load_n(&sc_stats.first_kind, __ATOMIC_ACQUIRE);
    out->first_task_id = sc_stats.first_task_id;
    out->first_cpu = sc_stats.first_cpu;
    out->first_owner = sc_stats.first_owner;
}

#endif /* CONFIG_SWITCH_CHECK */

#if defined(CONFIG_SWITCH_WINDOW_TEST)

volatile int switch_window_on;
struct task *volatile switch_window_task;
volatile int switch_window_in;
volatile int switch_window_ticks_after_move;
volatile int switch_window_moved;
volatile int switch_window_ticks_seen;

/* Run-4 critic window patch (critic-window-experiment.patch): in the
 * outgoing switch window, after rq_unlock and before switch_to, hold the
 * window open 25 ms for one chosen outgoing task that went back to READY,
 * with the caller's IRQ state, so that a timer tick lands inside it. */
void switch_window_hold(struct task *current)
{
    if (switch_window_on && current && current == switch_window_task &&
        current->state == TASK_READY) {
        __atomic_store_n(&switch_window_in, 1, __ATOMIC_RELEASE);
        uint64_t f = timer_get_frequency();
        uint64_t t0 = timer_get_count();
        while (timer_get_count() - t0 < f / 40) {
        }
        int want = switch_window_ticks_after_move;
        if (want > 0) {
            /* Hold until `want` ticks on this CPU after the move, so one
             * lands while the moved task's new CPU is at its switch-in.
             * A design that waits for this window under the new CPU's
             * rq_lock wedges this CPU in the tick (it never returns
             * here); a correct one lets the ticks through. */
            uint32_t cpu = cpu_id();
            while (!__atomic_load_n(&switch_window_moved, __ATOMIC_ACQUIRE) &&
                   timer_get_count() - t0 < f / 5) {
            }
            uint32_t k0 = sched_diag_tick[cpu];
            while (sched_diag_tick[cpu] - k0 < (uint32_t)want &&
                   timer_get_count() - t0 < f / 5) {
            }
            switch_window_ticks_seen = (int)(sched_diag_tick[cpu] - k0);
        }
        __atomic_store_n(&switch_window_in, 0, __ATOMIC_RELEASE);
    }
}

#endif /* CONFIG_SWITCH_WINDOW_TEST */
