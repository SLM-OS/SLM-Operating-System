/*
 * shell_top.c - Live system dashboard (#191)
 *
 * Refreshing view of CPU utilisation, active tasks, and memory pool
 * state. Consolidates the information otherwise scattered across
 * `tasks`, `mem`, `cpu`, `sched stats`, and `eviction` into a single
 * screen that updates on an interval until the user presses 'q'.
 *
 * Usage:
 *   top                — 1-second refresh, run until 'q'
 *   top <secs>         — custom refresh interval in seconds (1..60)
 *   top -n <count>     — iterate <count> times and exit (for scripting)
 *   top -n <count> <s> — both
 */

#include "shell.h"
#include "shell_internal.h"
#include "uart.h"
#include "task.h"
#include "sched.h"
#include "sched_policy.h"
#include "pmm.h"
#include "smp.h"
#include "timer.h"
#include "slm_ffi.h"
#include "platform.h"
#include "string.h"
#include "config.h"
#include <stdint.h>
#include <stdbool.h>

/* Maximum number of seconds between refreshes. */
#define TOP_MAX_REFRESH_SECS 60

/*
 * Render a single top frame. Screen is cleared by the caller so this
 * function only draws; it does not sleep or poll.
 */
static void top_render_frame(uint32_t refresh_secs, uint32_t iter_idx)
{
    /* Header */
    uint64_t count = timer_get_count();
    uint64_t freq  = timer_get_frequency();
    if (freq == 0) freq = 1;
    uint64_t total_seconds = count / freq;
    uint64_t hours   = total_seconds / 3600;
    uint64_t minutes = (total_seconds % 3600) / 60;
    uint64_t seconds = total_seconds % 60;

    const char *policy = sched_get_policy();
    uart_printf("SLM-OS top  —  uptime %lu:%02lu:%02lu  "
                "policy %s  refresh %us  frame %u  "
                "(q to quit)\r\n\r\n",
                (unsigned long)hours, (unsigned long)minutes,
                (unsigned long)seconds,
                policy ? policy : "?",
                refresh_secs, iter_idx);

    /* Per-CPU row:
     *   CPU  Util  Current Task
     * Util comes from cpu_runqueue.running_ticks / total_ticks when
     * AI_SCHEDULER is compiled in; otherwise printed as "-".
     */
    uart_puts("CPU  Util   Ready  Current Task\r\n");
    uart_puts("---  -----  -----  --------------------\r\n");
    for (uint32_t c = 0; c < cpu_count; c++) {
        struct cpu_runqueue *rq = sched_cpu_rq(c);
        const char *cur_name = "-";
        if (c == cpu_id()) {
            struct task *t = task_current();
            if (t && t->name[0]) cur_name = t->name;
        } else if (rq && rq->head) {
            /* Best-effort: show head-of-queue on other CPUs since
             * task_current() only works for the caller's CPU. */
            cur_name = rq->head->name[0] ? rq->head->name : "-";
        }
#ifdef CONFIG_AI_SCHEDULER
        uint32_t pct = 0;
        if (rq && rq->total_ticks > 0) {
            pct = (uint32_t)(rq->running_ticks * 100u / rq->total_ticks);
        }
        uart_printf("%3u  %4u%%  %5u  %s\r\n",
                    c, pct,
                    rq ? rq->ready_count : 0,
                    cur_name);
#else
        uart_printf("%3u    -    %5u  %s\r\n",
                    c,
                    rq ? rq->ready_count : 0,
                    cur_name);
#endif
    }

    /* Task roll-up */
    struct sched_stats stats;
    scheduler_get_stats(&stats);
    uint32_t running_tasks = 0;
    uint32_t blocked_tasks = 0;
    uint32_t ready_tasks = 0;
    for (uint32_t id = 0; id < MAX_TASKS; id++) {
        struct task *t = task_get(id);
        if (!t) continue;
        switch (t->state) {
        case TASK_RUNNING:    running_tasks++; break;
        case TASK_BLOCKED:    blocked_tasks++; break;
        case TASK_READY:      ready_tasks++;   break;
        default: break;
        }
    }

    uart_printf("\r\nTasks: %u total (%u running, %u ready, %u blocked)\r\n",
                stats.task_count, running_tasks, ready_tasks, blocked_tasks);

    /* Memory */
    size_t total_pages = pmm_get_total_pages();
    size_t free_pages  = pmm_get_free_pages();
    size_t used_pages  = total_pages - free_pages;
    size_t total_kb = (total_pages * 4096) / 1024;
    size_t used_kb  = (used_pages  * 4096) / 1024;
    uint32_t used_pct = total_pages ? (uint32_t)(used_pages * 100 / total_pages) : 0;
    uart_printf("Memory: %lu / %lu KB used (%u%%)\r\n",
                (unsigned long)used_kb, (unsigned long)total_kb, used_pct);

    /* AI eviction — pool utilisation + eviction counts when available. */
    RustEvictionStats ev = {0};
    rust_eviction_get_stats(&ev);
    if (ev.feature_enabled) {
        uint32_t wpct = ev.weight_total ?
            (uint32_t)(ev.weight_allocated * 100 / ev.weight_total) : 0;
        uint32_t spct = ev.workspace_total ?
            (uint32_t)(ev.workspace_allocated * 100 / ev.workspace_total) : 0;
        char policy_buf[32]; policy_buf[0] = 0;
        rust_eviction_policy_name((uint8_t *)policy_buf, sizeof(policy_buf));
        uart_printf("Eviction (%s): weight %zu/%zu blk (%u%%) ev=%lu  "
                    "workspace %zu/%zu blk (%u%%) ev=%lu\r\n",
                    policy_buf,
                    ev.weight_allocated, ev.weight_total, wpct,
                    (unsigned long)ev.weight_evictions,
                    ev.workspace_allocated, ev.workspace_total, spct,
                    (unsigned long)ev.workspace_evictions);
    }

    uart_printf("Context switches: %lu   Timer ticks: %lu\r\n",
                (unsigned long)stats.context_switches,
                (unsigned long)stats.timer_ticks);
}

/*
 * Poll the UART for the quit character. Returns true if 'q' or 'Q' or
 * ESC was received. Consumes any other available input to avoid
 * backlogging the shell line editor.
 */
static bool top_poll_quit(void)
{
    int c;
    while ((c = uart_try_getc()) >= 0) {
        if (c == 'q' || c == 'Q' || c == 0x03 /* Ctrl-C */ || c == 0x1B /* ESC */) {
            return true;
        }
    }
    return false;
}

/*
 * Sleep in ~100ms increments while checking for the quit key, so 'q'
 * registers within one poll period regardless of the refresh interval.
 */
static bool top_sleep_with_poll(uint32_t secs)
{
    uint32_t ms_remaining = secs * 1000u;
    while (ms_remaining > 0) {
        uint32_t step = ms_remaining > 100u ? 100u : ms_remaining;
        sleep_ms(step);
        if (top_poll_quit()) return true;
        ms_remaining -= step;
    }
    return false;
}

int cmd_top(int argc, char *argv[])
{
    uint32_t refresh_secs = 1;
    uint32_t max_iter = 0;  /* 0 = run until 'q' */

    /* Parse args: "-n <count>" then optional refresh seconds. */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) {
                uart_puts("top: -n requires a count\r\n");
                return 1;
            }
            if (shell_parse_uint(argv[i + 1], &max_iter) < 0) {
                uart_puts("top: invalid count\r\n");
                return 1;
            }
            i += 2;
        } else {
            uint32_t v;
            if (shell_parse_uint(argv[i], &v) < 0 || v == 0 || v > TOP_MAX_REFRESH_SECS) {
                uart_printf("top: refresh must be 1..%u seconds\r\n",
                            TOP_MAX_REFRESH_SECS);
                return 1;
            }
            refresh_secs = v;
            i++;
        }
    }

    uint32_t iter = 0;
    for (;;) {
        /* ANSI: cursor home + clear-below so the screen stays anchored. */
        uart_puts("\033[H\033[2J");
        top_render_frame(refresh_secs, iter);
        iter++;

        if (max_iter != 0 && iter >= max_iter) {
            break;
        }
        if (top_sleep_with_poll(refresh_secs)) {
            break;
        }
    }
    uart_puts("\r\n");
    return 0;
}
