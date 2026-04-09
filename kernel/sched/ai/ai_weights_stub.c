/*
 * ai_weights_stub.c - Zero-initialized weight arrays for development
 *
 * Provides all-zero weight and bias arrays so the AI scheduler can
 * compile and link without Plan A deliverables. The inference engine
 * will produce uniform logits (all actions equally likely), effectively
 * making random CPU assignments.
 *
 * Replace with real weights from Plan A export pipeline:
 *   python scripts/export_models.py --model mlp --platform jetson_orin_nano
 *   cp deploy/generated/ai_weights_mlp.c kernel/sched/ai/
 */

#include "ai_types.h"

/* ============================================================================
 * MLP stub weights (all zeros)
 * ============================================================================ */

const float ai_mlp_w0[AI_MLP_LAYER0_OUT * AI_MLP_LAYER0_IN]
    __attribute__((aligned(64))) = {0};
const float ai_mlp_b0[AI_MLP_LAYER0_OUT]
    __attribute__((aligned(64))) = {0};

const float ai_mlp_w1[AI_MLP_LAYER1_OUT * AI_MLP_LAYER1_IN]
    __attribute__((aligned(64))) = {0};
const float ai_mlp_b1[AI_MLP_LAYER1_OUT]
    __attribute__((aligned(64))) = {0};

const float ai_mlp_w2[AI_MLP_LAYER2_OUT * AI_MLP_LAYER2_IN]
    __attribute__((aligned(64))) = {0};
const float ai_mlp_b2[AI_MLP_LAYER2_OUT]
    __attribute__((aligned(64))) = {0};

const float ai_mlp_w3[AI_MLP_LAYER3_OUT * AI_MLP_LAYER3_IN]
    __attribute__((aligned(64))) = {0};
const float ai_mlp_b3[AI_MLP_LAYER3_OUT]
    __attribute__((aligned(64))) = {0};

/* ============================================================================
 * PPO stub weights (all zeros)
 * ============================================================================ */

const float ai_ppo_w0[AI_MLP_LAYER0_OUT * AI_MLP_LAYER0_IN]
    __attribute__((aligned(64))) = {0};
const float ai_ppo_b0[AI_MLP_LAYER0_OUT]
    __attribute__((aligned(64))) = {0};

const float ai_ppo_w1[AI_MLP_LAYER1_OUT * AI_MLP_LAYER1_IN]
    __attribute__((aligned(64))) = {0};
const float ai_ppo_b1[AI_MLP_LAYER1_OUT]
    __attribute__((aligned(64))) = {0};

const float ai_ppo_w2[AI_MLP_LAYER2_OUT * AI_MLP_LAYER2_IN]
    __attribute__((aligned(64))) = {0};
const float ai_ppo_b2[AI_MLP_LAYER2_OUT]
    __attribute__((aligned(64))) = {0};

const float ai_ppo_w3[AI_MLP_LAYER3_OUT * AI_MLP_LAYER3_IN]
    __attribute__((aligned(64))) = {0};
const float ai_ppo_b3[AI_MLP_LAYER3_OUT]
    __attribute__((aligned(64))) = {0};
