/*
 * sched_xgb.c — XGBoost cascade scheduler policy (#855).
 *
 * Mirrors `sched_policy_ai_mlp` but invokes a Rust-side 3-classifier
 * cascade (core / priority / preempt) loaded via the existing
 * `slm.sched_model_*` workflow. Cascade storage lives Rust-side in
 * `runtime/src/sched/xgb.rs` because the trained model (~9 MB) is too
 * large for the static MLP/PPO dense pool used by other sched-model
 * kinds in `runtime_model.c`.
 *
 * Compiled WITHOUT `-mgeneral-regs-only`, like `sched_ai.c`, so the
 * Rust cascade walker can use FP/NEON freely. The C wrapper saves /
 * restores the FP context around every assign_cpu so the policy is
 * safe to invoke from IRQ context.
 */

#include "sched_policy.h"
#include "fp_context.h"
#include "ai_inference.h"
#include "ai_state.h"
#include "ai_types.h"
#include "runtime_model.h"
#include "sched.h"
#include "smp.h"
#include "slm_ffi.h"
#include "debug.h"

struct ai_xgb_stats {
    uint32_t decisions;
    uint32_t fallbacks;
    uint32_t cpu_clamped;       /* core >= cpu_count, clamped via modulo */
    uint64_t total_latency_ns;
};

static struct ai_xgb_stats ai_xgb_stats;

/* The Rust predictor (`runtime/src/sched/xgb.rs::STATE_DIM`) hard-
 * codes 108. Pin the C-side `AI_STATE_DIM` against it so a future
 * drift surfaces at build time, not as silent garbage when the
 * Rust side reads past the C-allocated buffer. */
_Static_assert(AI_STATE_DIM == 108,
    "sched_xgb.c assumes the Rust predictor's STATE_DIM == 108");

static int ai_xgb_init(void)
{
    ai_xgb_stats.decisions = 0;
    ai_xgb_stats.fallbacks = 0;
    ai_xgb_stats.cpu_clamped = 0;
    ai_xgb_stats.total_latency_ns = 0;
    /* No self-test inference: predict() returns failure cleanly when
     * no cascade is active (assign_cpu falls back to heuristic) so
     * registering with no blob loaded is the expected idle state.
     * The operator stages + activates a blob via
     *   slm.sched_model_stage("xgboost", path)
     *   slm.sched_model_activate("xgboost")
     * before switching to this policy. */
    if (rust_sched_xgb_is_active()) {
        INFO("AI XGB: policy initialized (cascade active)");
    } else {
        INFO("AI XGB: policy initialized (no cascade staged — assign_cpu "
             "will fall back to heuristic until `slm.sched_model_*` "
             "loads xgb_sched.smb)");
    }
    return 0;
}

static void ai_xgb_shutdown(void)
{
    if (ai_xgb_stats.decisions > 0) {
        uint64_t avg_ns =
            ai_xgb_stats.total_latency_ns / ai_xgb_stats.decisions;
        INFO("AI XGB: %u decisions, %u fallbacks, %u CPU-clamped, "
             "avg latency %lu ns",
             ai_xgb_stats.decisions, ai_xgb_stats.fallbacks,
             ai_xgb_stats.cpu_clamped, (unsigned long)avg_ns);
    }
}

static uint32_t ai_xgb_assign_cpu(struct task *task)
{
    FP_CONTEXT_SAVE();

    float state[AI_STATE_DIM];
    int32_t core_label = 0;
    int32_t prio_label = 0;
    int32_t preempt_label = 0;

    uint64_t t0 = slm_get_time_ns();
    ai_extract_state(state);
    int rc = rust_sched_xgb_predict(state, &core_label,
                                    &prio_label, &preempt_label);
    uint64_t t1 = slm_get_time_ns();
    ai_xgb_stats.total_latency_ns += (t1 > t0) ? (t1 - t0) : 0u;
    ai_xgb_stats.decisions++;

    if (rc != 0) {
        ai_xgb_stats.fallbacks++;
        FP_CONTEXT_RESTORE();
        return sched_policy_heuristic.assign_cpu(task);
    }

    /* CPU-id clamping. The cascade was trained on a 6-core space; on
     * Pi 5 (cpu_count == 4) the core classifier can emit 4 or 5.
     * Clamp via modulo and continue — refusing the prediction would
     * thrash to heuristic on every Pi 5 dispatch. WARN once per
     * session so the operator notices the deployment mismatch.
     *
     * Negative core labels would indicate a corrupt label_classes
     * map or a Rust-side bug; treat as a hard fallback. */
    if (core_label < 0) {
        ai_xgb_stats.fallbacks++;
        FP_CONTEXT_RESTORE();
        return sched_policy_heuristic.assign_cpu(task);
    }
    uint32_t core = (uint32_t)core_label;
    if (core >= cpu_count) {
        static bool clamp_warned = false;
        if (!clamp_warned) {
            WARN("AI XGB: cascade emitted core=%u but cpu_count=%u; "
                 "clamping via modulo. Trained for a wider topology — "
                 "predictions still applied but distribution may skew.",
                 core, cpu_count);
            clamp_warned = true;
        }
        core = core % cpu_count;
        ai_xgb_stats.cpu_clamped++;
    }

    /* Skip isolated cores: heuristic fallback respects affinity flags. */
    if (sched_is_core_isolated(core)
     && task->cpu_affinity == CPU_AFFINITY_ANY) {
        ai_xgb_stats.fallbacks++;
        FP_CONTEXT_RESTORE();
        return sched_policy_heuristic.assign_cpu(task);
    }

    /* Apply priority adjustment using the same encoding as sched_ai.c:
     *   1 = boost, 2 = reduce. Other values are no-ops. */
    if (prio_label == 1) {
        if (task->effective_priority < TASK_PRIORITY_CRITICAL)
            task->effective_priority++;
    } else if (prio_label == 2) {
        if (task->effective_priority > TASK_PRIORITY_IDLE)
            task->effective_priority--;
    }

    /* Preempt: boost priority so pick_next_task prefers this task. */
    if (preempt_label != 0
     && task->effective_priority < TASK_PRIORITY_CRITICAL) {
        task->effective_priority++;
    }

    FP_CONTEXT_RESTORE();
    return core;
}

const struct sched_policy_ops sched_policy_ai_xgb = {
    .name       = "ai_xgb",
    .init       = ai_xgb_init,
    .shutdown   = ai_xgb_shutdown,
    .assign_cpu = ai_xgb_assign_cpu,
    .tick       = NULL,
    .has_gpu_backend = false,
};
