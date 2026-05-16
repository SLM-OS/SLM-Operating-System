/*
 * ai_state.c - State vector extraction for AI scheduler
 *
 * Extracts the 108-dimensional observation vector from kernel state.
 * Layout matches the simulator's observation space (slm_sim/observation.py):
 *   - Per-core features:  36 floats (6 cores × 6 features)
 *   - Per-task features:  64 floats (8 tasks × 8 features)
 *   - Global features:     8 floats
 *
 * This file is compiled WITHOUT -mgeneral-regs-only, so float
 * operations are available.
 */

#include "ai_state.h"
#include "ai_types.h"
#include "sched.h"
#include "task.h"
#include "smp.h"
#include "slm_ffi.h"
#if !defined(PLATFORM_X86_64)
#include "pmu.h"
#endif
#include <stdint.h>

/* Integer accessors defined in sched.c — float conversion done here.
 * sched.c is compiled with -mgeneral-regs-only so it can't return float. */
extern void     sched_ai_get_utilization_raw(uint32_t cpu, uint64_t *running, uint64_t *total);
extern void     sched_ai_get_deadline_miss_raw(uint32_t *misses, uint32_t *window_size);
extern void     sched_ai_get_latency_raw(uint64_t *cumulative_ns, uint32_t *count);
extern uint32_t sched_ai_get_top_tasks(struct task **out, uint32_t max);

static float get_utilization(uint32_t cpu)
{
    uint64_t running, total;
    sched_ai_get_utilization_raw(cpu, &running, &total);
    if (total == 0) return 0.0f;
    return (float)running / (float)total;
}

static float get_deadline_miss_rate(void)
{
    uint32_t misses, window_size;
    sched_ai_get_deadline_miss_raw(&misses, &window_size);
    if (window_size == 0) return 0.0f;
    return (float)misses / (float)window_size;
}

static float get_avg_latency_ns(void)
{
    uint64_t cumulative_ns;
    uint32_t count;
    sched_ai_get_latency_raw(&cumulative_ns, &count);
    if (count == 0) return 0.0f;
    return (float)(cumulative_ns / count);
}

static float clamp01(float x)
{
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

/*
 * Extract per-core features (36 floats).
 *
 * For each core c in [0, AI_STATE_NUM_CORES):
 *   [c*6+0] utilization        running_ticks / total_ticks
 *   [c*6+1] queue_depth        ready_count / 32.0
 *   [c*6+2] cache_pressure     L1D miss rate EWMA, normalized [0,1]
 *                              (PMU-fed via pmu_sample_cache_pressure;
 *                              ZERO on PLATFORM_X86_64 — RDPMC tracked
 *                              separately as #870)
 *   [c*6+3] core_type          1.0 (homogeneous)
 *   [c*6+4] isolated           0 or 1
 *   [c*6+5] current_task_prio  effective_priority / 7.0
 *
 * Cores beyond cpu_count are zero-filled.
 */
static void extract_per_core(float *out)
{
    uint32_t isolated = sched_get_isolated_cores();

    for (int c = 0; c < AI_STATE_NUM_CORES; c++) {
        float *f = &out[c * AI_FEATURES_PER_CORE];

        if ((uint32_t)c >= cpu_count) {
            /* Zero-fill unused core slots */
            for (int j = 0; j < AI_FEATURES_PER_CORE; j++)
                f[j] = 0.0f;
            continue;
        }

        struct cpu_runqueue *rq = sched_cpu_rq(c);

        f[0] = get_utilization(c);
        f[1] = (float)rq->ready_count / 32.0f;
#if !defined(PLATFORM_X86_64)
        /* cache_pressure (#872): L1D miss-rate EWMA from each CPU's
         * own PMU sample, taken on `scheduler_tick`. The Q16.16 fixed-
         * point cache is converted to a [0.0, 1.0] float here so pmu.c
         * itself stays in -mgeneral-regs-only land. Zero for CPUs whose
         * PMU sample has never fired (pre-boot, or QEMU TCG where
         * event counters return 0). */
        f[2] = (float)pmu_get_cache_pressure_q16((uint32_t)c) / 65536.0f;
#else
        f[2] = 0.0f;  /* x86-64 RDPMC sibling tracked as #870 */
#endif
        f[3] = 1.0f;  /* core_type — homogeneous on Jetson/Pi5 */
        f[4] = (isolated & (1U << c)) ? 1.0f : 0.0f;

        /* Current task's effective priority on this core.
         * task_current() is per-CPU and only valid on the calling CPU,
         * so for other CPUs we check the run queue head as a proxy.
         *
         * Lock-free read of rq->head from another CPU. Snapshot the
         * pointer locally so a concurrent migration / pop on the
         * target CPU can't free the task between the NULL-check and
         * the field load. The torn-read window remains best-effort —
         * effective_priority may briefly read a stale value — but
         * that's acceptable for an AI feature vector that already
         * tolerates noise. The pointer-snapshot guards against the
         * NULL-deref / use-after-free, which would actually crash. */
        struct task *h = rq->head;
        if (rq->idle_task && h) {
            f[5] = (float)h->effective_priority / 7.0f;
        } else {
            f[5] = 0.0f;  /* idle or empty */
        }
    }
}

/*
 * Extract per-task features (64 floats).
 *
 * Top 8 tasks by effective_priority. For each task t in [0, 8):
 *   [36 + t*8+0] priority           effective_priority / 7.0
 *   [36 + t*8+1] deadline_urgency   1.0 - (deadline - now) / 1e9, clamped [0,1]
 *   [36 + t*8+2] working_set        0.0 (future: slm_task_info)
 *   [36 + t*8+3] model_size         0.0 (future)
 *   [36 + t*8+4] inference_dur      0.0 (future)
 *   [36 + t*8+5] can_use_gpu        0.0 (future)
 *   [36 + t*8+6] wait_time          (now - arrival_time_ns) / 1e9
 *   [36 + t*8+7] component_type     0.0 (future)
 *
 * If fewer than 8 tasks exist, remaining slots are zero-filled.
 */
static void extract_per_task(float *out, uint64_t now)
{
    struct task *tasks[AI_STATE_NUM_TASKS];
    uint32_t n = sched_ai_get_top_tasks(tasks, AI_STATE_NUM_TASKS);

    for (int t = 0; t < AI_STATE_NUM_TASKS; t++) {
        float *f = &out[t * AI_FEATURES_PER_TASK];

        if ((uint32_t)t >= n || !tasks[t]) {
            /* Zero-fill unused task slots */
            for (int j = 0; j < AI_FEATURES_PER_TASK; j++)
                f[j] = 0.0f;
            continue;
        }

        struct task *task = tasks[t];

        f[0] = (float)task->effective_priority / 7.0f;

        /* Deadline urgency: approaches 1.0 as deadline nears */
        if (task->deadline_ns > 0 && now < task->deadline_ns) {
            float remaining_s = (float)(task->deadline_ns - now) / 1e9f;
            f[1] = clamp01(1.0f - remaining_s);
        } else if (task->deadline_ns > 0) {
            f[1] = 1.0f;  /* deadline missed */
        } else {
            f[1] = 0.0f;  /* no deadline */
        }

        f[2] = 0.0f;  /* working_set — future */
        f[3] = 0.0f;  /* model_size — future */
        f[4] = 0.0f;  /* inference_dur — future */
        f[5] = 0.0f;  /* can_use_gpu — future */

        /* Wait time: time since task was added to scheduler */
#ifdef CONFIG_AI_SCHEDULER
        if (task->arrival_time_ns > 0 && now > task->arrival_time_ns) {
            f[6] = (float)(now - task->arrival_time_ns) / 1e9f;
        } else {
            f[6] = 0.0f;
        }
#else
        f[6] = 0.0f;
#endif

        f[7] = 0.0f;  /* component_type — future */
    }
}

/*
 * Extract global features (8 floats).
 *
 *   [100] ready_count              sum(ready_count) / 64.0
 *   [101] deadline_miss_rate       rolling window
 *   [102] avg_latency              cumulative / count / 1e7
 *   [103] weight_pool_pressure     0.0 (future: Rust runtime)
 *   [104] workspace_pool_pressure  0.0 (future)
 *   [105] gpu_queue_depth          0.0 (future: GPU driver)
 *   [106] load_imbalance           std_dev(utils) / mean(utils), clamped [0,1]
 *   [107] episode_time             0.0 (not applicable to real kernel)
 */
static void extract_global(float *out)
{
    /* Ready count across all CPUs.
     *
     * `ready_count` reads are unsynchronized across CPUs — each CPU
     * can mutate its own slot while we sum here, so the result is a
     * point-in-time-ish snapshot rather than a transactionally
     * consistent total. That's acceptable for an AI feature input
     * (the policy already tolerates noisy inputs) and faster than
     * acquiring every per-CPU rq_lock; the per-feature snapshot
     * pattern is consistent with f[5] in extract_per_core(). */
    uint32_t total_ready = 0;
    for (uint32_t c = 0; c < cpu_count; c++) {
        total_ready += sched_cpu_rq(c)->ready_count;
    }
    out[0] = (float)total_ready / 64.0f;

    /* Deadline miss rate */
    out[1] = get_deadline_miss_rate();

    /* Average latency (normalized to ~10ms scale) */
    out[2] = get_avg_latency_ns() / 1e7f;

    out[3] = 0.0f;  /* weight_pool_pressure — future */
    out[4] = 0.0f;  /* workspace_pool_pressure — future */
    out[5] = 0.0f;  /* gpu_queue_depth — future */

    /* Load imbalance: coefficient of variation of per-core utilization */
    if (cpu_count > 1) {
        float sum = 0.0f;
        float utils[AI_STATE_NUM_CORES];

        for (uint32_t c = 0; c < cpu_count && c < AI_STATE_NUM_CORES; c++) {
            utils[c] = get_utilization(c);
            sum += utils[c];
        }
        float mean = sum / (float)cpu_count;

        if (mean > 0.001f) {
            float var_sum = 0.0f;
            for (uint32_t c = 0; c < cpu_count && c < AI_STATE_NUM_CORES; c++) {
                float diff = utils[c] - mean;
                var_sum += diff * diff;
            }
            /* coefficient_of_variation = sqrt(variance) / mean. Use the
             * hardware sqrt intrinsic so the runtime feature matches what
             * scripts/hailo/generate_calibration_data.py emits during
             * training (numpy's std()). The previous Newton's iteration
             * used `std_dev = variance` as initial guess, which produces
             * ~50% relative error after only two iterations for typical
             * variances (e.g. variance=0.04 returns ~0.30, true sqrt 0.20).
             * That skewed every load-imbalance feature fed to the AI
             * scheduler away from the training distribution. */
            float variance = var_sum / (float)cpu_count;
            float std_dev = 0.0f;
            if (variance > 0.0f) {
                std_dev = __builtin_sqrtf(variance);
            }
            out[6] = clamp01(std_dev / mean);
        } else {
            out[6] = 0.0f;
        }
    } else {
        out[6] = 0.0f;
    }

    out[7] = 0.0f;  /* episode_time — not applicable */
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void ai_extract_state(float state[AI_STATE_DIM])
{
    uint64_t now = slm_get_time_ns();

    /* Per-core features: offsets 0-35 */
    extract_per_core(&state[0]);

    /* Per-task features: offsets 36-99 */
    extract_per_task(&state[AI_STATE_NUM_CORES * AI_FEATURES_PER_CORE], now);

    /* Global features: offsets 100-107 */
    extract_global(&state[AI_STATE_NUM_CORES * AI_FEATURES_PER_CORE +
                          AI_STATE_NUM_TASKS * AI_FEATURES_PER_TASK]);
}
