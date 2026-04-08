/*
 * sched_ai.c - AI scheduling policy implementations
 *
 * Provides sched_policy_ops for MLP and PPO models. These policies
 * extract the scheduler state, run inference, and return a CPU
 * assignment. Full implementation in M7.
 *
 * This file is compiled WITHOUT -mgeneral-regs-only.
 */

#include "sched_policy.h"
#include "ai_inference.h"
#include "ai_state.h"
#include "ai_types.h"

/* Stub implementations — will be replaced in M7 */

static uint32_t ai_mlp_assign_cpu(struct task *task)
{
    (void)task;
    return 0;  /* Stub: always CPU 0 */
}

static uint32_t ai_ppo_assign_cpu(struct task *task)
{
    (void)task;
    return 0;  /* Stub: always CPU 0 */
}

const struct sched_policy_ops sched_policy_ai_mlp = {
    .name       = "ai_mlp",
    .init       = NULL,
    .shutdown   = NULL,
    .assign_cpu = ai_mlp_assign_cpu,
    .tick       = NULL,
};

const struct sched_policy_ops sched_policy_ai_ppo = {
    .name       = "ai_ppo",
    .init       = NULL,
    .shutdown   = NULL,
    .assign_cpu = ai_ppo_assign_cpu,
    .tick       = NULL,
};

/*
 * Register AI policies with the scheduler.
 * Called from kernel_main() when CONFIG_AI_SCHEDULER is defined.
 */
void sched_ai_init(void)
{
    sched_register_policy(&sched_policy_ai_mlp);
    sched_register_policy(&sched_policy_ai_ppo);
}
