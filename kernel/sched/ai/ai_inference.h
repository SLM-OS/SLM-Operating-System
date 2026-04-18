/*
 * ai_inference.h - AI inference engine for scheduler
 *
 * Provides forward-pass functions for MLP and PPO models.
 * Implemented in ai_inference.c (M3).
 */

#ifndef AI_INFERENCE_H
#define AI_INFERENCE_H

#include "ai_types.h"

/*
 * Run MLP inference on the given state vector.
 *
 * @state:  Input state vector (AI_STATE_DIM floats)
 * @action: Output decoded action
 *
 * Returns: 0 on success, -1 on error
 */
int ai_schedule_mlp(const float state[AI_STATE_DIM],
                    struct ai_sched_action *action);

/*
 * Run PPO inference on the given state vector.
 *
 * @state:  Input state vector (AI_STATE_DIM floats)
 * @action: Output decoded action
 *
 * Returns: 0 on success, -1 on error
 */
int ai_schedule_ppo(const float state[AI_STATE_DIM],
                    struct ai_sched_action *action);

/* ============================================================================
 * Test-accessible wrappers for internal math functions.
 * Only available when ENABLE_BOOT_TESTS is defined.
 * ============================================================================ */
#ifdef ENABLE_BOOT_TESTS
void ai_test_matvec(const float *W, const float *bias,
                    const float *in, float *out, int M, int N);
void ai_test_relu(float *x, int n);
int  ai_test_argmax(const float *x, int n);
#endif

/*
 * Validate a decoded action against runtime constraints.
 *
 * @action:    Action to validate
 * @num_cores: Number of online CPUs
 *
 * Returns: 1 if valid, 0 if out of bounds
 */
int ai_validate_action(const struct ai_sched_action *action, uint32_t num_cores);

/*
 * Run only the MLP forward pass and write raw logits.
 *
 * Unlike ai_schedule_mlp() (which also does argmax + decode), this
 * function returns the raw N_ACTIONS-dim output vector. Used by the
 * inference_device CPU backend so the generic tensor-in/tensor-out
 * surface stays decoupled from the scheduler's action encoding.
 *
 * @state: Input state vector (AI_STATE_DIM floats)
 * @out:   Output logits (AI_SCHED_N_ACTIONS floats)
 */
void ai_mlp_forward_logits(const float state[AI_STATE_DIM],
                           float out[AI_SCHED_N_ACTIONS]);

#endif /* AI_INFERENCE_H */
