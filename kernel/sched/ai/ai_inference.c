/*
 * ai_inference.c - AI inference engine for SLM-OS scheduler
 *
 * Core math functions (matvec, relu, argmax) and 4-layer MLP/PPO
 * forward-pass implementations. All scratch memory is stack-allocated
 * for thread safety — no static globals, no dynamic allocation.
 *
 * This file is compiled WITHOUT -mgeneral-regs-only, so FP/NEON
 * instructions are available. On AArch64, the compiler auto-vectorizes
 * with NEON when -O2 and -mcpu are set. Explicit NEON intrinsics are
 * provided as a fallback if auto-vectorization is insufficient.
 *
 * Performance target: < 50µs per inference on Cortex-A78 @ 1.5 GHz
 * (~262K FLOPs, ~22µs with NEON at ~12 GFLOPS)
 */

#include "ai_inference.h"
#include "ai_weights.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define USE_NEON 1
#else
#define USE_NEON 0
#endif

/* ============================================================================
 * Core math functions
 * ============================================================================ */

/*
 * Matrix-vector multiply with bias: out[M] = W[M×N] * in[N] + bias[M]
 *
 * W is row-major: W[i*N + j] is row i, column j.
 * Inner loop accesses W with stride 1 (cache-friendly).
 */
static void ai_matvec(const float *W, const float *bias,
                      const float *in, float *out, int M, int N)
{
    for (int i = 0; i < M; i++) {
        const float *row = &W[i * N];

#if USE_NEON
        /* NEON 4-wide accumulation */
        float32x4_t acc = vdupq_n_f32(0.0f);
        int j = 0;

        /* Process 4 elements at a time */
        for (; j <= N - 4; j += 4) {
            float32x4_t w = vld1q_f32(&row[j]);
            float32x4_t x = vld1q_f32(&in[j]);
            acc = vfmaq_f32(acc, w, x);
        }

        /* Horizontal sum of accumulator */
        float sum = vaddvq_f32(acc);

        /* Handle remaining elements */
        for (; j < N; j++) {
            sum += row[j] * in[j];
        }
#else
        /* Scalar fallback */
        float sum = 0.0f;
        for (int j = 0; j < N; j++) {
            sum += row[j] * in[j];
        }
#endif

        out[i] = sum + bias[i];
    }
}

/*
 * In-place ReLU activation: x[i] = max(0, x[i])
 */
static void ai_relu(float *x, int n)
{
#if USE_NEON
    float32x4_t zero = vdupq_n_f32(0.0f);
    int i = 0;

    for (; i <= n - 4; i += 4) {
        float32x4_t v = vld1q_f32(&x[i]);
        v = vmaxq_f32(v, zero);
        vst1q_f32(&x[i], v);
    }

    for (; i < n; i++) {
        if (x[i] < 0.0f) x[i] = 0.0f;
    }
#else
    for (int i = 0; i < n; i++) {
        if (x[i] < 0.0f) x[i] = 0.0f;
    }
#endif
}

/*
 * Argmax: return index of largest element in x[n].
 * Ties broken by first occurrence.
 */
static int ai_argmax(const float *x, int n)
{
    if (n <= 0) return -1;

    int best = 0;
    float best_val = x[0];

    for (int i = 1; i < n; i++) {
        if (x[i] > best_val) {
            best_val = x[i];
            best = i;
        }
    }

    return best;
}

/* ============================================================================
 * Action validation
 * ============================================================================ */

int ai_validate_action(const struct ai_sched_action *action, uint32_t num_cores)
{
    if (!action) return 0;
    if (action->core_assignment >= num_cores) return 0;
    if (action->priority_adj > 2) return 0;
    if (action->preempt > 1) return 0;
    return 1;
}

/* ============================================================================
 * Forward pass (shared between MLP and PPO)
 * ============================================================================ */

/*
 * 4-layer forward pass: input → hidden0 → hidden1 → hidden2 → logits
 *
 * Uses two stack-allocated scratch buffers that alternate as input/output
 * across layers. Maximum dimension is AI_MLP_MAX_DIM (256).
 *
 * Layer 0: [108] → W0[256×108] → ReLU → [256]
 * Layer 1: [256] → W1[256×256] → ReLU → [256]
 * Layer 2: [256] → W2[128×256] → ReLU → [128]
 * Layer 3: [128] → W3[N×128]   →        [N]    (logits, no activation)
 */
static int forward_pass(const float state[AI_STATE_DIM],
                        const float *w0, const float *b0,
                        const float *w1, const float *b1,
                        const float *w2, const float *b2,
                        const float *w3, const float *b3,
                        struct ai_sched_action *action)
{
    /* Stack-allocated scratch buffers — thread safe, no locking needed.
     * Two buffers alternate: one is input, the other is output. */
    float buf_a[AI_MLP_MAX_DIM];  /* 256 floats = 1 KB */
    float buf_b[AI_MLP_MAX_DIM];  /* 256 floats = 1 KB */
    float logits[AI_SCHED_N_ACTIONS];

    /* Layer 0: state[108] → buf_a[256] */
    ai_matvec(w0, b0, state, buf_a, AI_MLP_LAYER0_OUT, AI_MLP_LAYER0_IN);
    ai_relu(buf_a, AI_MLP_LAYER0_OUT);

    /* Layer 1: buf_a[256] → buf_b[256] */
    ai_matvec(w1, b1, buf_a, buf_b, AI_MLP_LAYER1_OUT, AI_MLP_LAYER1_IN);
    ai_relu(buf_b, AI_MLP_LAYER1_OUT);

    /* Layer 2: buf_b[256] → buf_a[128] */
    ai_matvec(w2, b2, buf_b, buf_a, AI_MLP_LAYER2_OUT, AI_MLP_LAYER2_IN);
    ai_relu(buf_a, AI_MLP_LAYER2_OUT);

    /* Layer 3: buf_a[128] → logits[N_ACTIONS] (no activation) */
    ai_matvec(w3, b3, buf_a, logits, AI_MLP_LAYER3_OUT, AI_MLP_LAYER3_IN);

    /* Select action with highest logit */
    int best = ai_argmax(logits, AI_SCHED_N_ACTIONS);
    if (best < 0) return -1;

    ai_decode_action(best, action);
    return 0;
}

/* ============================================================================
 * Test wrappers (expose static functions for unit testing)
 * ============================================================================ */

#ifdef ENABLE_BOOT_TESTS
void ai_test_matvec(const float *W, const float *bias,
                    const float *in, float *out, int M, int N)
{
    ai_matvec(W, bias, in, out, M, N);
}

void ai_test_relu(float *x, int n)
{
    ai_relu(x, n);
}

int ai_test_argmax(const float *x, int n)
{
    return ai_argmax(x, n);
}
#endif

/* ============================================================================
 * Public API
 * ============================================================================ */

int ai_schedule_mlp(const float state[AI_STATE_DIM],
                    struct ai_sched_action *action)
{
    if (!state || !action) return -1;

    return forward_pass(state,
                        ai_mlp_w0, ai_mlp_b0,
                        ai_mlp_w1, ai_mlp_b1,
                        ai_mlp_w2, ai_mlp_b2,
                        ai_mlp_w3, ai_mlp_b3,
                        action);
}

int ai_schedule_ppo(const float state[AI_STATE_DIM],
                    struct ai_sched_action *action)
{
    if (!state || !action) return -1;

    return forward_pass(state,
                        ai_ppo_w0, ai_ppo_b0,
                        ai_ppo_w1, ai_ppo_b1,
                        ai_ppo_w2, ai_ppo_b2,
                        ai_ppo_w3, ai_ppo_b3,
                        action);
}
