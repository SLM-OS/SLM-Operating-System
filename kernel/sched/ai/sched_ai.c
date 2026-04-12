/*
 * sched_ai.c - AI scheduling policy implementations for SLM-OS
 *
 * Provides sched_policy_ops for MLP and PPO models. Each policy:
 *   1. Saves FP register state (may be called from IRQ context)
 *   2. Extracts the 108-dim state vector from kernel data
 *   3. Runs neural network inference
 *   4. Validates the action (core in range, not isolated)
 *   5. Falls back to heuristic on invalid actions
 *   6. Applies priority adjustment and preempt flag
 *   7. Restores FP register state
 *
 * This file is compiled WITHOUT -mgeneral-regs-only, so FP/NEON
 * instructions are available.
 */

#include "sched_policy.h"
#include "fp_context.h"
#include "ai_inference.h"
#include "ai_state.h"
#include "ai_types.h"
#include "sched.h"
#include "smp.h"
#include "slm_ffi.h"
#include "debug.h"

/* Per-policy statistics */
struct ai_policy_stats {
    uint32_t decisions;
    uint32_t fallbacks;
    uint64_t total_latency_ns;
    uint32_t action_hist[AI_SCHED_N_ACTIONS];  /* Per-action index counts */
};

static struct ai_policy_stats ai_mlp_stats;
static struct ai_policy_stats ai_ppo_stats;

/* ============================================================================
 * Shared logic
 * ============================================================================ */

/*
 * Common assign_cpu implementation. Extracts state, runs inference,
 * validates and applies the action.
 *
 * @task:       Task being assigned
 * @infer_fn:   ai_schedule_mlp or ai_schedule_ppo
 * @stats:      Per-policy statistics to update
 *
 * Returns: target CPU ID
 */
static uint32_t ai_assign_cpu_common(
    struct task *task,
    int (*infer_fn)(const float *, struct ai_sched_action *),
    struct ai_policy_stats *stats)
{
    FP_CONTEXT_SAVE();

    float state[AI_STATE_DIM];
    struct ai_sched_action action;

    uint64_t t0 = slm_get_time_ns();

    ai_extract_state(state);

    int ret = infer_fn(state, &action);

    uint64_t t1 = slm_get_time_ns();
    stats->total_latency_ns += (t1 - t0);
    stats->decisions++;

    /* Record action in histogram (inverse of ai_decode_action) */
    if (ret >= 0) {
        int idx = action.core_assignment * AI_ACTIONS_PER_CORE
                + action.priority_adj * AI_SCHED_PREEMPT_OPTS
                + action.preempt;
        if (idx >= 0 && idx < AI_SCHED_N_ACTIONS)
            stats->action_hist[idx]++;
    }

    if (ret < 0) {
        stats->fallbacks++;
        FP_CONTEXT_RESTORE();
        return sched_policy_heuristic.assign_cpu(task);
    }

    /* Validate action against runtime constraints */
    if (!ai_validate_action(&action, cpu_count) ||
        (sched_is_core_isolated(action.core_assignment) &&
         task->cpu_affinity == CPU_AFFINITY_ANY)) {
        stats->fallbacks++;
        FP_CONTEXT_RESTORE();
        return sched_policy_heuristic.assign_cpu(task);
    }

    /* Apply priority adjustment */
    if (action.priority_adj == 1) {
        /* Boost */
        if (task->effective_priority < TASK_PRIORITY_CRITICAL)
            task->effective_priority++;
    } else if (action.priority_adj == 2) {
        /* Reduce */
        if (task->effective_priority > TASK_PRIORITY_IDLE)
            task->effective_priority--;
    }

    /* Preempt: boost priority so pick_next_task prefers this task */
    if (action.preempt && task->effective_priority < TASK_PRIORITY_CRITICAL) {
        task->effective_priority++;
    }

    FP_CONTEXT_RESTORE();
    return action.core_assignment;
}

/* ============================================================================
 * MLP policy
 * ============================================================================ */

static int ai_mlp_init(void)
{
    ai_mlp_stats.decisions = 0;
    ai_mlp_stats.fallbacks = 0;
    ai_mlp_stats.total_latency_ns = 0;
    for (int i = 0; i < AI_SCHED_N_ACTIONS; i++)
        ai_mlp_stats.action_hist[i] = 0;

    /* Validate weight dimensions match expected architecture */
    _Static_assert(AI_MLP_LAYER0_IN == AI_STATE_DIM,
        "MLP layer 0 input must match state dimension");
    _Static_assert(AI_MLP_LAYER3_OUT == AI_SCHED_N_ACTIONS,
        "MLP layer 3 output must match action count");

    /* Run a self-test inference to verify linkage */
    FP_CONTEXT_SAVE();

    float state[AI_STATE_DIM];
    struct ai_sched_action action;
    for (int i = 0; i < AI_STATE_DIM; i++) state[i] = 0.0f;

    int ret = ai_schedule_mlp(state, &action);

    FP_CONTEXT_RESTORE();

    if (ret < 0) {
        WARN("AI MLP: self-test inference failed");
        return -1;
    }

    INFO("AI MLP: policy initialized (self-test passed)");
    return 0;
}

static void ai_mlp_shutdown(void)
{
    if (ai_mlp_stats.decisions > 0) {
        uint64_t avg_ns = ai_mlp_stats.total_latency_ns / ai_mlp_stats.decisions;
        INFO("AI MLP: %u decisions, %u fallbacks, avg latency %lu ns",
             ai_mlp_stats.decisions, ai_mlp_stats.fallbacks,
             (unsigned long)avg_ns);
    }
}

static uint32_t ai_mlp_assign_cpu(struct task *task)
{
    return ai_assign_cpu_common(task, ai_schedule_mlp, &ai_mlp_stats);
}

const struct sched_policy_ops sched_policy_ai_mlp = {
    .name       = "ai_mlp",
    .init       = ai_mlp_init,
    .shutdown   = ai_mlp_shutdown,
    .assign_cpu = ai_mlp_assign_cpu,
    .tick       = NULL,
};

/* ============================================================================
 * PPO policy
 * ============================================================================ */

static int ai_ppo_init(void)
{
    ai_ppo_stats.decisions = 0;
    ai_ppo_stats.fallbacks = 0;
    ai_ppo_stats.total_latency_ns = 0;
    for (int i = 0; i < AI_SCHED_N_ACTIONS; i++)
        ai_ppo_stats.action_hist[i] = 0;

    FP_CONTEXT_SAVE();

    float state[AI_STATE_DIM];
    struct ai_sched_action action;
    for (int i = 0; i < AI_STATE_DIM; i++) state[i] = 0.0f;

    int ret = ai_schedule_ppo(state, &action);

    FP_CONTEXT_RESTORE();

    if (ret < 0) {
        WARN("AI PPO: self-test inference failed");
        return -1;
    }

    INFO("AI PPO: policy initialized (self-test passed)");
    return 0;
}

static void ai_ppo_shutdown(void)
{
    if (ai_ppo_stats.decisions > 0) {
        uint64_t avg_ns = ai_ppo_stats.total_latency_ns / ai_ppo_stats.decisions;
        INFO("AI PPO: %u decisions, %u fallbacks, avg latency %lu ns",
             ai_ppo_stats.decisions, ai_ppo_stats.fallbacks,
             (unsigned long)avg_ns);
    }
}

static uint32_t ai_ppo_assign_cpu(struct task *task)
{
    return ai_assign_cpu_common(task, ai_schedule_ppo, &ai_ppo_stats);
}

const struct sched_policy_ops sched_policy_ai_ppo = {
    .name       = "ai_ppo",
    .init       = ai_ppo_init,
    .shutdown   = ai_ppo_shutdown,
    .assign_cpu = ai_ppo_assign_cpu,
    .tick       = NULL,
};

/* ============================================================================
 * Statistics Access
 * ============================================================================ */

void sched_ai_get_stats(const char *policy_name,
                        uint32_t *decisions, uint32_t *fallbacks,
                        uint64_t *avg_latency_ns,
                        const uint32_t **action_hist, int *n_actions)
{
    struct ai_policy_stats *stats = NULL;
    if (policy_name[0] == 'a' && policy_name[3] == 'm')
        stats = &ai_mlp_stats;
    else if (policy_name[0] == 'a' && policy_name[3] == 'p')
        stats = &ai_ppo_stats;

    if (!stats) {
        *decisions = 0;
        *fallbacks = 0;
        *avg_latency_ns = 0;
        *action_hist = NULL;
        *n_actions = 0;
        return;
    }

    *decisions = stats->decisions;
    *fallbacks = stats->fallbacks;
    *avg_latency_ns = stats->decisions > 0
        ? stats->total_latency_ns / stats->decisions : 0;
    *action_hist = stats->action_hist;
    *n_actions = AI_SCHED_N_ACTIONS;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

void sched_ai_init(void)
{
    sched_register_policy(&sched_policy_ai_mlp);
    sched_register_policy(&sched_policy_ai_ppo);
}
