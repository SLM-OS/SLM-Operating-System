/*
 * workloads.h — Real-workload comparison harness for AI scheduler policies (#882).
 *
 * Drives a representative task mix through `scheduler_add_task` so that
 * pluggable policies (heuristic, ai_mlp, ai_ppo, ai_xgb) can be compared
 * on scheduling **quality** (deadline-miss rate, completion-time
 * percentiles, CPU-balance) rather than only inference-only decision
 * latency. The shell entry point lives in `shell_sys.c` under
 * `bench sched-policy --workload <name>`.
 *
 * The harness is gated behind CONFIG_AI_SCHEDULER because three of the
 * four comparison policies require the AI runtime. The heuristic-only
 * row remains available when running the harness on AI_SCHED builds.
 *
 * Per the exploratory-OS framing (#848) the harness reports
 * characterization data; no winning policy is declared.
 */

#ifndef KERNEL_SCHED_AI_WORKLOADS_H
#define KERNEL_SCHED_AI_WORKLOADS_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/* Maximum tasks dispatched per workload run. Bounded so the records
 * array fits comfortably under the per-task slot budget in NC memory
 * and so a missed completion never strands more than this many task
 * slots in the global task table. */
#define BENCH_WORKLOAD_MAX_TASKS  32u

/* Task template — describes one entry in a workload's repeating
 * pattern. `est_runtime_us` is the target busy-loop duration the
 * task body will spin for; `deadline_slack_us` is the deadline
 * relative to dispatch time (0 = no deadline); `priority` maps to
 * the TASK_PRIORITY_* constants in `task.h`. */
struct bench_wl_template {
    uint32_t est_runtime_us;
    uint32_t deadline_slack_us;
    uint8_t  priority;
    uint8_t  _pad[3];
};

/* Workload definition. Templates are applied round-robin to
 * `n_tasks_total` dispatch slots; `arrival_spacing_us` is the
 * busy-wait gap between successive dispatches (0 = burst). */
struct bench_workload {
    const char *name;
    const struct bench_wl_template *templates;
    uint32_t n_templates;
    uint32_t n_tasks_total;
    uint32_t arrival_spacing_us;
};

/* Per-task slot: combines the worker's input (runtime_us +
 * dispatch_ns, written by the driver before scheduler_add_task) and
 * the worker's output (completion_ns, ran_on_cpu, deadline_met,
 * done). Both halves live in one struct so a single
 * cache_clean_range(&wl_slots[i], sizeof(wl_slots[i])) on the driver
 * side covers everything the worker reads from this slot — important
 * on Pi 5 / Jetson where per-core L2 caches are incoherent (no SMPEN).
 * See kernel/CLAUDE.md §"Cache Maintenance (Pi 5 / No SMPEN)". */
struct bench_wl_record {
    /* --- Driver-written input (read by worker) --- */
    uint64_t dispatch_ns;     /* slm_get_time_ns() at scheduler_add_task time */
    uint32_t runtime_us;      /* template's est_runtime_us — worker busy-waits this long */
    uint32_t _pad_input;
    uint64_t deadline_ns;     /* absolute deadline copied from task->deadline_ns; 0 = none */
    /* --- Worker-written output (read by driver after task exits) --- */
    uint64_t completion_ns;   /* end_ns; 0 = not yet done */
    uint32_t latency_us;      /* completion_ns - dispatch_ns */
    uint8_t  ran_on_cpu;
    uint8_t  deadline_met;    /* 1 if completion_ns <= deadline, else 0 */
    uint8_t  done;            /* 1 once worker has finished and written its slot */
    uint8_t  _pad_output;
};

/* Aggregate result of one (policy × workload) bench run. */
struct bench_workload_result {
    uint32_t tasks_dispatched;
    uint32_t tasks_completed;
    uint32_t deadline_tasks;     /* tasks that had a deadline */
    uint32_t deadline_misses;
    uint64_t completion_p50_us;
    uint64_t completion_p99_us;
    uint64_t completion_max_us;
    uint32_t cpu_completions[MAX_CPUS];
    /* CPU-balance metric — coefficient of variation × 1000, lower is
     * more balanced. Computed across CPUs that received >0 tasks.
     * Stored as integer milli-units to avoid printing fp from kernel. */
    uint32_t cpu_balance_milli_cov;
    uint64_t duration_ns;
    uint64_t throughput_milli; /* tasks per 1000 seconds = milli-tasks/s; printable as X.XXX */
};

/* Look up a workload by name. Returns NULL if no match. */
const struct bench_workload *bench_workload_find(const char *name);

/* Enumerate workloads (for `--all`-style iteration / help text). */
uint32_t bench_workload_count(void);
const struct bench_workload *bench_workload_get(uint32_t idx);

/*
 * Run one (policy × workload) bench cycle.
 *
 * Switches to `policy` for the duration of the run, dispatches the
 * workload's task mix through `scheduler_add_task` (so the active
 * policy's `assign_cpu` is exercised), waits for completion with a
 * generous timeout, then restores the previous policy. The aggregate
 * metrics are written to *out.
 *
 * Returns 0 on success, -1 on setup error (invalid policy / workload /
 * task table exhaustion before any dispatches).
 *
 * Must be called from the shell task (CPU 0). Worker tasks are
 * dispatched at TASK_PRIORITY_HIGH so they preempt the bench loop's
 * busy-wait on real platforms.
 */
struct sched_policy_ops;
int bench_workload_run(const struct sched_policy_ops *policy,
                       const struct bench_workload *wl,
                       struct bench_workload_result *out);

#endif /* KERNEL_SCHED_AI_WORKLOADS_H */
