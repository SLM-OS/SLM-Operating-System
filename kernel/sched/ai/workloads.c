/*
 * workloads.c — Real-workload comparison harness (#882, sub-ticket of #61).
 *
 * Drives a representative task mix through `scheduler_add_task` so
 * different scheduler policies can be compared on real scheduling
 * quality, not only inference-only decision latency. See workloads.h
 * for the public API.
 *
 * Design notes
 * ------------
 * The driver runs in the shell task on CPU 0. Worker tasks are
 * created at TASK_PRIORITY_HIGH so they preempt the shell's
 * busy-wait on cooperative-preempt platforms. Each worker spins on
 * `timer_get_count()` for its template's `est_runtime_us` and then
 * records (completion_ns, cpu, deadline_met) into a per-task slot
 * in a BSS records array. The driver loops `slm_get_time_ns()` until
 * all slots show `done==1` or the per-run timeout expires.
 *
 * Cross-CPU coherency: each worker writes only its own slot, then
 * calls cache_clean on it; the driver invalidates the array before
 * reading. On NC-memory platforms the records still live in BSS for
 * simplicity — every record is on its own 64-byte cacheline and the
 * "writer-side clean + reader-side invalidate" pattern is exactly
 * the one called out in kernel/CLAUDE.md §"Cache Maintenance (Pi 5
 * / No SMPEN)". This is the same approach `bench stealing` uses on
 * cacheable BSS platforms; we follow it uniformly so the harness has
 * one code path.
 */

#if defined(CONFIG_AI_SCHEDULER)

#include "workloads.h"
#include "task.h"
#include "sched.h"
#include "sched_policy.h"
#include "smp.h"
#include "cache.h"
#include "timer.h"
#include "string.h"
#include "preempt_point.h"
#include "debug.h"

#include <stdint.h>
#include <stdbool.h>

/* slm_get_time_ns prototype lives in kernel/include/util.h on most
 * platforms; declare locally to keep the include surface narrow. */
extern uint64_t slm_get_time_ns(void);

#define BENCH_WL_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* ---- Workload templates ---- */

/* "mixed" — representative cross-section. Two short tight-deadline
 * tasks for every long no-deadline task. Closest to the
 * `slm_sim/workloads/system.py` shape the synthetic-baseline weights
 * were trained on, so it's the natural smoke test for differentiating
 * policies before the ai-real fine-tune lands. */
static const struct bench_wl_template wl_mixed_tpls[] = {
    { .est_runtime_us =  300, .deadline_slack_us = 2000, .priority = 5 },
    { .est_runtime_us =  500, .deadline_slack_us = 4000, .priority = 4 },
    { .est_runtime_us = 1500, .deadline_slack_us =    0, .priority = 3 },
};

/* "deadline-heavy" — every task carries a tight relative deadline.
 * Heuristic should miss more deadlines than ai_mlp/ai_ppo if the
 * trained policies' deadline-pressure heuristics generalize. */
static const struct bench_wl_template wl_deadline_tpls[] = {
    { .est_runtime_us =  400, .deadline_slack_us = 1500, .priority = 6 },
    { .est_runtime_us =  600, .deadline_slack_us = 2000, .priority = 6 },
    { .est_runtime_us =  250, .deadline_slack_us = 1000, .priority = 7 },
};

/* "latency-sensitive" — very short tasks with very tight deadlines,
 * arriving in close succession. CPU-balance matters most here. */
static const struct bench_wl_template wl_latency_tpls[] = {
    { .est_runtime_us = 150, .deadline_slack_us =  800, .priority = 6 },
    { .est_runtime_us = 200, .deadline_slack_us = 1000, .priority = 7 },
};

/* "cpu-bound" — long tasks, no deadlines. Surfaces work-balance
 * differences without deadline pressure. */
static const struct bench_wl_template wl_cpu_bound_tpls[] = {
    { .est_runtime_us = 4000, .deadline_slack_us = 0, .priority = 4 },
    { .est_runtime_us = 2000, .deadline_slack_us = 0, .priority = 5 },
};

static const struct bench_workload bench_workloads_table[] = {
    {
        .name = "mixed",
        .templates = wl_mixed_tpls,
        .n_templates = BENCH_WL_ARRAY_SIZE(wl_mixed_tpls),
        .n_tasks_total = 24,
        .arrival_spacing_us = 300,
    },
    {
        .name = "deadline-heavy",
        .templates = wl_deadline_tpls,
        .n_templates = BENCH_WL_ARRAY_SIZE(wl_deadline_tpls),
        .n_tasks_total = 24,
        .arrival_spacing_us = 200,
    },
    {
        .name = "latency-sensitive",
        .templates = wl_latency_tpls,
        .n_templates = BENCH_WL_ARRAY_SIZE(wl_latency_tpls),
        .n_tasks_total = 24,
        .arrival_spacing_us = 150,
    },
    {
        .name = "cpu-bound",
        .templates = wl_cpu_bound_tpls,
        .n_templates = BENCH_WL_ARRAY_SIZE(wl_cpu_bound_tpls),
        .n_tasks_total = 16,
        .arrival_spacing_us = 0,
    },
};

const struct bench_workload *bench_workload_find(const char *name)
{
    if (!name) return 0;
    for (uint32_t i = 0; i < BENCH_WL_ARRAY_SIZE(bench_workloads_table); i++) {
        if (strcmp(name, bench_workloads_table[i].name) == 0) {
            return &bench_workloads_table[i];
        }
    }
    return 0;
}

uint32_t bench_workload_count(void)
{
    return (uint32_t)BENCH_WL_ARRAY_SIZE(bench_workloads_table);
}

const struct bench_workload *bench_workload_get(uint32_t idx)
{
    if (idx >= BENCH_WL_ARRAY_SIZE(bench_workloads_table)) return 0;
    return &bench_workloads_table[idx];
}

/* ---- Worker bookkeeping ---- */

/* Each slot on its own 64-byte cacheline so adjacent workers writing
 * different slots can't false-share. The records array is module
 * static so the worker can find its slot from `arg`. Only one
 * `bench_workload_run` may be in flight at a time (the shell is
 * single-threaded).
 *
 * The driver's input (`dispatch_ns`, `runtime_us`, `deadline_ns`)
 * lives in the same struct as the worker's output so a single
 * cache_clean_range on the slot covers everything the worker reads
 * before scheduler_add_task hands it off — without this, on Pi 5 a
 * worker dispatched to a non-CPU-0 core would read stale zeros for
 * `runtime_us` and busy-wait for zero cycles (incoherent per-core
 * L2; see kernel/CLAUDE.md §"Cache Maintenance"). */
struct __attribute__((aligned(64))) bench_wl_slot {
    struct bench_wl_record rec;
    uint8_t  _pad[64 - sizeof(struct bench_wl_record)];
};
static struct bench_wl_slot wl_slots[BENCH_WORKLOAD_MAX_TASKS] __attribute__((aligned(64)));

/* Re-entry guard. `bench_workload_run` mutates the module-static
 * `wl_slots` array; a concurrent second caller (Lua, IPC, future
 * non-shell entry point) would silently corrupt records. Mirrors the
 * `in_split` pattern documented in kernel/CLAUDE.md §"Buddy Allocator".
 *
 * Best-effort fail-loud, NOT a cross-CPU lock — on Pi 5 / Jetson the
 * `volatile bool` write isn't guaranteed visible to a concurrent reader
 * on another CPU without cache maintenance. The documented precondition
 * (shell task on CPU 0, single-threaded) makes this a non-issue in
 * practice. A future Lua/IPC binding that legitimately needs concurrent
 * access must add an external spinlock around the whole run rather than
 * lean on this guard. */
static volatile bool bench_workload_in_flight = false;

static void workload_busy_wait_us(uint32_t us)
{
    if (us == 0) return;
    uint64_t freq = timer_get_frequency();
    uint64_t target_cycles = ((uint64_t)us * freq) / 1000000ULL;
    uint64_t start = timer_get_count();
    while ((timer_get_count() - start) < target_cycles) {
        __asm__ volatile("" ::: "memory");
        /* Cheap when the local quantum hasn't expired; falls into
         * schedule() when ≥10 ms has elapsed (COOP_PREEMPT builds).
         * Required by the kernel/CLAUDE.md §"Preemption Model"
         * policy because the longest workload template's busy-wait
         * (cpu-bound = 4000 us) can otherwise monopolize a CPU.
         * Call site is lock-free per the preempt_point.h contract. */
        slm_preempt_point();
    }
}

static void workload_task_body(void *arg)
{
    uint32_t slot = (uint32_t)(uintptr_t)arg;
    if (slot >= BENCH_WORKLOAD_MAX_TASKS) {
        /* Defensive — caller never passes this, but a bad cast
         * would corrupt our records array. */
        return;
    }

    /* Invalidate this slot so we read the driver's just-cleaned
     * dispatch_ns / runtime_us / deadline_ns rather than any stale
     * local L1 contents on Pi 5 / Jetson. */
    cache_invalidate_range(&wl_slots[slot], sizeof(wl_slots[slot]));

    uint32_t runtime_us = wl_slots[slot].rec.runtime_us;
    uint64_t dispatch_ns = wl_slots[slot].rec.dispatch_ns;
    uint64_t deadline_ns = wl_slots[slot].rec.deadline_ns;

    /* Busy-spin for the template's runtime. */
    workload_busy_wait_us(runtime_us);

    uint64_t end_ns = slm_get_time_ns();
    wl_slots[slot].rec.completion_ns = end_ns;
    wl_slots[slot].rec.latency_us =
        (uint32_t)((end_ns - dispatch_ns) / 1000ULL);
    wl_slots[slot].rec.ran_on_cpu = (uint8_t)cpu_id();
    wl_slots[slot].rec.deadline_met =
        (deadline_ns == 0 || end_ns <= deadline_ns) ? 1u : 0u;
    wl_slots[slot].rec.done = 1;
    cache_clean_range(&wl_slots[slot], sizeof(wl_slots[slot]));
}

/* ---- Quantile + stddev helpers (integer-only) ---- */

/* Simple insertion sort — N <= 32 so O(N^2) is fine. */
static void sort_u64(uint64_t *arr, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        uint64_t key = arr[i];
        uint32_t j = i;
        while (j > 0 && arr[j - 1] > key) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

/* Quantile interpolation: returns arr[ceil(q*n) - 1] for sorted arr.
 * q is in milli-units (e.g. 500 = p50). n must be >= 1. */
static uint64_t quantile_milli(const uint64_t *sorted, uint32_t n, uint32_t q_milli)
{
    if (n == 0) return 0;
    uint64_t idx = ((uint64_t)q_milli * (uint64_t)n + 999u) / 1000u;
    if (idx == 0) idx = 1;
    if (idx > n) idx = n;
    return sorted[idx - 1];
}

/* Coefficient of variation × 1000 across the non-zero entries of `counts`.
 * COV = stddev / mean. Smaller = more even distribution.
 * Integer-only — avoids floating-point in kernel context.
 *
 * To compute stddev without sqrt, we use the formula
 *   variance = E[x^2] - (E[x])^2
 *   stddev = sqrt(variance)
 *   cov_milli = (stddev * 1000) / mean
 * For integer sqrt we use a 16-iteration Newton step.
 */
static uint32_t isqrt_u64(uint64_t n)
{
    if (n == 0) return 0;
    /* Initial estimate: high bit / 2. */
    uint64_t x = n;
    uint64_t r = 1;
    while (x > 0) { r <<= 1; x >>= 2; }
    /* Newton iterations: r' = (r + n/r) / 2. 16 is plenty for u64. */
    for (int i = 0; i < 16; i++) {
        if (r == 0) break;
        r = (r + n / r) / 2;
    }
    return (uint32_t)r;
}

static uint32_t compute_cov_milli(const uint32_t *counts, uint32_t n_cpus)
{
    uint64_t sum = 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n_cpus; i++) {
        if (counts[i] > 0) { sum += counts[i]; k++; }
    }
    if (k < 2 || sum == 0) return 0;
    uint64_t mean = sum / k;
    if (mean == 0) return 0;
    uint64_t sq_sum = 0;
    for (uint32_t i = 0; i < n_cpus; i++) {
        if (counts[i] > 0) {
            int64_t d = (int64_t)counts[i] - (int64_t)mean;
            sq_sum += (uint64_t)(d * d);
        }
    }
    uint64_t variance = sq_sum / k;
    uint32_t sd = isqrt_u64(variance);
    /* COV * 1000 = sd / mean * 1000 */
    return (uint32_t)(((uint64_t)sd * 1000ULL) / mean);
}

/* ---- Main entry point ---- */

int bench_workload_run(const struct sched_policy_ops *policy,
                       const struct bench_workload *wl,
                       struct bench_workload_result *out)
{
    if (!policy || !wl || !out) return -1;
    if (wl->n_tasks_total == 0 || wl->n_tasks_total > BENCH_WORKLOAD_MAX_TASKS) return -1;
    if (wl->n_templates == 0) return -1;

    if (bench_workload_in_flight) {
        panic("bench_workload_run: reentrant invocation (wl_slots is single-owner)");
    }
    bench_workload_in_flight = true;

    /* Snapshot active policy so we can restore on the way out. */
    const struct sched_policy_ops *prev = sched_find_policy(sched_get_policy());

    if (sched_set_policy(policy) < 0) {
        bench_workload_in_flight = false;
        return -1;
    }

    /* Clear records. The dispatch loop below fills in dispatch_ns /
     * runtime_us / deadline_ns per slot and then cache_cleans the
     * slot before scheduler_add_task hands the worker off. */
    for (uint32_t i = 0; i < BENCH_WORKLOAD_MAX_TASKS; i++) {
        wl_slots[i].rec.dispatch_ns = 0;
        wl_slots[i].rec.runtime_us = 0;
        wl_slots[i].rec.deadline_ns = 0;
        wl_slots[i].rec.completion_ns = 0;
        wl_slots[i].rec.latency_us = 0;
        wl_slots[i].rec.ran_on_cpu = 0xFF;
        wl_slots[i].rec.deadline_met = 0;
        wl_slots[i].rec.done = 0;
    }
    cache_clean_range(wl_slots, sizeof(wl_slots));

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        out->cpu_completions[i] = 0;
    }

    uint64_t bench_start_ns = slm_get_time_ns();

    /* Dispatch loop. */
    uint32_t dispatched = 0;
    struct task *created[BENCH_WORKLOAD_MAX_TASKS] = { 0 };
    for (uint32_t i = 0; i < wl->n_tasks_total; i++) {
        const struct bench_wl_template *tpl = &wl->templates[i % wl->n_templates];

        /* Create the worker first — task_create_with_priority does not
         * queue it, so the worker can't run until scheduler_add_task
         * below. Allocator + zeroing can take a non-trivial slice on
         * Pi 5, so capture dispatch_ns AFTER creation to honor the
         * header's "at scheduler_add_task time" contract. */
        struct task *t = task_create_with_priority(
            "wl_bench", workload_task_body,
            (void *)(uintptr_t)i, tpl->priority);
        if (!t) break;  /* Task table exhausted — report whatever we got. */
        created[i] = t;

        uint64_t now_ns = slm_get_time_ns();
        wl_slots[i].rec.dispatch_ns = now_ns;
        wl_slots[i].rec.runtime_us = tpl->est_runtime_us;
        if (tpl->deadline_slack_us > 0) {
            wl_slots[i].rec.deadline_ns =
                now_ns + (uint64_t)tpl->deadline_slack_us * 1000ULL;
            task_set_deadline(t, wl_slots[i].rec.deadline_ns);
        }
        /* Push slot to PoC so the worker (which may run on a
         * different CPU with an incoherent L2) sees these fields. */
        cache_clean_range(&wl_slots[i], sizeof(wl_slots[i]));

        scheduler_add_task(t);
        dispatched++;

        if (wl->arrival_spacing_us > 0 && i + 1 < wl->n_tasks_total) {
            workload_busy_wait_us(wl->arrival_spacing_us);
        }
    }

    /* Wait for completion. Generous timeout = 4x expected workload
     * duration + 1s floor. Expected duration estimated from sum of
     * runtime templates, assuming perfect parallelism on cpu_count
     * cores (which the policies should approach). */
    uint64_t expected_us = 0;
    for (uint32_t i = 0; i < wl->n_tasks_total; i++) {
        expected_us += wl->templates[i % wl->n_templates].est_runtime_us;
    }
    expected_us /= (cpu_count > 0 ? cpu_count : 1u);
    uint64_t timeout_ns = (uint64_t)expected_us * 4000ULL + 1000000000ULL;

    for (;;) {
        cache_invalidate_range(wl_slots, sizeof(wl_slots));
        uint32_t done = 0;
        for (uint32_t i = 0; i < dispatched; i++) {
            if (wl_slots[i].rec.done) done++;
        }
        if (done >= dispatched) break;
        uint64_t elapsed = slm_get_time_ns() - bench_start_ns;
        if (elapsed > timeout_ns) break;
        /* Brief busy-wait then re-check. yield() would also work but
         * a busy-wait keeps the timing simpler and doesn't return
         * control to the scheduler in a way that could re-queue this
         * task. */
        workload_busy_wait_us(200);
    }

    uint64_t bench_end_ns = slm_get_time_ns();
    cache_invalidate_range(wl_slots, sizeof(wl_slots));

    /* Tally. */
    out->tasks_dispatched = dispatched;
    out->tasks_completed = 0;
    out->deadline_tasks = 0;
    out->deadline_misses = 0;
    out->completion_max_us = 0;
    uint64_t latencies[BENCH_WORKLOAD_MAX_TASKS];
    uint32_t lat_count = 0;
    for (uint32_t i = 0; i < dispatched; i++) {
        if (!wl_slots[i].rec.done) continue;
        out->tasks_completed++;
        if (wl_slots[i].rec.ran_on_cpu < MAX_CPUS) {
            out->cpu_completions[wl_slots[i].rec.ran_on_cpu]++;
        }
        latencies[lat_count++] = (uint64_t)wl_slots[i].rec.latency_us;
        if ((uint64_t)wl_slots[i].rec.latency_us > out->completion_max_us) {
            out->completion_max_us = wl_slots[i].rec.latency_us;
        }
        if (wl_slots[i].rec.deadline_ns != 0) {
            out->deadline_tasks++;
            if (!wl_slots[i].rec.deadline_met) out->deadline_misses++;
        }
    }
    if (lat_count > 0) {
        sort_u64(latencies, lat_count);
        out->completion_p50_us = quantile_milli(latencies, lat_count, 500);
        out->completion_p99_us = quantile_milli(latencies, lat_count, 990);
    } else {
        out->completion_p50_us = 0;
        out->completion_p99_us = 0;
    }
    out->cpu_balance_milli_cov = compute_cov_milli(out->cpu_completions, MAX_CPUS);
    out->duration_ns = bench_end_ns - bench_start_ns;
    if (out->duration_ns > 0) {
        /* tasks per 1000 seconds, i.e. milli-tasks/sec for two-decimal
         * display: ((completed * 1e9 * 1000) / duration_ns) → milli */
        out->throughput_milli =
            ((uint64_t)out->tasks_completed * 1000000000ULL * 1000ULL)
            / out->duration_ns;
    } else {
        out->throughput_milli = 0;
    }

    /* Reap finished task structs so the next run starts with a clean
     * table. Any task that didn't finish (timed-out) is forcibly
     * terminated then destroyed to keep the table clean — see kernel
     * CLAUDE.md "Leaky tests that block CPU 1" for why this matters. */
    for (uint32_t i = 0; i < dispatched; i++) {
        if (!created[i]) continue;
        if (!wl_slots[i].rec.done) {
            scheduler_terminate_task(created[i]);
        }
        task_destroy(created[i]);
    }

    if (prev) (void)sched_set_policy(prev);

    bench_workload_in_flight = false;
    return 0;
}

#endif /* CONFIG_AI_SCHEDULER */
