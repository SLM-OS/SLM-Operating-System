/*
 * ai_test_helpers.c - Test helper functions for AI inference engine
 *
 * These functions run inside the ai_sched library (FP enabled) so they
 * can use float directly. They return 0 on pass, -1 on fail — the test
 * harness (compiled with -mgeneral-regs-only) just checks the return code.
 *
 * Only compiled when ENABLE_BOOT_TESTS is defined.
 */

#ifdef ENABLE_BOOT_TESTS

#include "ai_inference.h"
#include "ai_weights.h"
#include <stdint.h>
#include <stddef.h>

/* Forward declarations of test wrappers */
extern void ai_test_matvec(const float *W, const float *bias,
                           const float *in, float *out, int M, int N);
extern void ai_test_relu(float *x, int n);
extern int  ai_test_argmax(const float *x, int n);

static int approx_eq(float a, float b, float tol)
{
    float diff = a - b;
    if (diff < 0) diff = -diff;
    return diff <= tol;
}

/*
 * Test: matvec with a known 2×3 matrix.
 *
 * W = [[1, 2, 3],    bias = [10, 20]    in = [1, 1, 1]
 *      [4, 5, 6]]
 *
 * out[0] = 1*1 + 2*1 + 3*1 + 10 = 16
 * out[1] = 4*1 + 5*1 + 6*1 + 20 = 35
 */
int ai_test_matvec_basic(void)
{
    float W[] = {1, 2, 3, 4, 5, 6};
    float bias[] = {10, 20};
    float in[] = {1, 1, 1};
    float out[2];

    ai_test_matvec(W, bias, in, out, 2, 3);

    if (!approx_eq(out[0], 16.0f, 0.001f)) return -1;
    if (!approx_eq(out[1], 35.0f, 0.001f)) return -1;
    return 0;
}

/*
 * Test: matvec with identity-like matrix (4×4).
 * W = identity, bias = zeros, in = [1, 2, 3, 4] → out = [1, 2, 3, 4]
 */
int ai_test_matvec_identity(void)
{
    float W[] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float bias[] = {0, 0, 0, 0};
    float in[] = {1, 2, 3, 4};
    float out[4];

    ai_test_matvec(W, bias, in, out, 4, 4);

    for (int i = 0; i < 4; i++) {
        if (!approx_eq(out[i], in[i], 0.001f)) return -1;
    }
    return 0;
}

/*
 * Test: matvec with larger dimensions matching model layer sizes.
 * Uses zero weights (from stub) — output should equal bias.
 */
int ai_test_matvec_zero_weights(void)
{
    float in[AI_MLP_LAYER0_IN];
    float out[AI_MLP_LAYER0_OUT];

    /* Input = all ones */
    for (int i = 0; i < AI_MLP_LAYER0_IN; i++)
        in[i] = 1.0f;

    /* Zero weights + zero bias → output should be all zeros */
    ai_test_matvec(ai_mlp_w0, ai_mlp_b0, in, out,
                   AI_MLP_LAYER0_OUT, AI_MLP_LAYER0_IN);

    for (int i = 0; i < AI_MLP_LAYER0_OUT; i++) {
        if (!approx_eq(out[i], 0.0f, 0.001f)) return -1;
    }
    return 0;
}

/*
 * Test: relu with positive and negative values.
 */
int ai_test_relu_mixed(void)
{
    float x[] = {-3.0f, -1.0f, 0.0f, 1.0f, 5.0f, -0.5f, 2.0f, -100.0f};
    float expected[] = {0.0f, 0.0f, 0.0f, 1.0f, 5.0f, 0.0f, 2.0f, 0.0f};

    ai_test_relu(x, 8);

    for (int i = 0; i < 8; i++) {
        if (!approx_eq(x[i], expected[i], 0.001f)) return -1;
    }
    return 0;
}

/*
 * Test: relu with all positive — no change.
 */
int ai_test_relu_all_positive(void)
{
    float x[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    float orig[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};

    ai_test_relu(x, 5);

    for (int i = 0; i < 5; i++) {
        if (!approx_eq(x[i], orig[i], 0.001f)) return -1;
    }
    return 0;
}

/*
 * Test: relu with all negative — all become zero.
 */
int ai_test_relu_all_negative(void)
{
    float x[] = {-1.0f, -2.0f, -3.0f, -4.0f};

    ai_test_relu(x, 4);

    for (int i = 0; i < 4; i++) {
        if (!approx_eq(x[i], 0.0f, 0.001f)) return -1;
    }
    return 0;
}

/*
 * Test: argmax with clear maximum.
 */
int ai_test_argmax_basic(void)
{
    float x[] = {1.0f, 5.0f, 3.0f, 2.0f};
    int idx = ai_test_argmax(x, 4);
    if (idx != 1) return -1;
    return 0;
}

/*
 * Test: argmax with maximum at the end.
 */
int ai_test_argmax_last(void)
{
    float x[] = {0.0f, 0.0f, 0.0f, 10.0f};
    int idx = ai_test_argmax(x, 4);
    if (idx != 3) return -1;
    return 0;
}

/*
 * Test: argmax with maximum at the beginning.
 */
int ai_test_argmax_first(void)
{
    float x[] = {10.0f, 0.0f, 0.0f, 0.0f};
    int idx = ai_test_argmax(x, 4);
    if (idx != 0) return -1;
    return 0;
}

/*
 * Test: argmax with ties — first occurrence wins.
 */
int ai_test_argmax_tie(void)
{
    float x[] = {5.0f, 5.0f, 5.0f};
    int idx = ai_test_argmax(x, 3);
    if (idx != 0) return -1;
    return 0;
}

/*
 * Test: argmax with negative values.
 */
int ai_test_argmax_negative(void)
{
    float x[] = {-5.0f, -1.0f, -3.0f};
    int idx = ai_test_argmax(x, 3);
    if (idx != 1) return -1;
    return 0;
}

/*
 * Test: Full MLP inference with stub (zero) weights.
 * All-zero weights → all-zero logits → argmax returns 0 → action (0,0,0).
 */
int ai_test_mlp_stub_inference(void)
{
    float state[AI_STATE_DIM];
    for (int i = 0; i < AI_STATE_DIM; i++)
        state[i] = 1.0f;

    struct ai_sched_action action;
    int ret = ai_schedule_mlp(state, &action);
    if (ret != 0) return -1;

    /* Zero weights → all logits equal (0) → argmax picks index 0 */
    if (action.core_assignment != 0) return -1;
    if (action.priority_adj != 0) return -1;
    if (action.preempt != 0) return -1;

    return 0;
}

/*
 * Test: PPO inference produces same result as MLP with stub weights.
 */
int ai_test_ppo_stub_inference(void)
{
    float state[AI_STATE_DIM];
    for (int i = 0; i < AI_STATE_DIM; i++)
        state[i] = 0.5f;

    struct ai_sched_action action;
    int ret = ai_schedule_ppo(state, &action);
    if (ret != 0) return -1;

    /* Same zero weights → same result */
    if (action.core_assignment != 0) return -1;
    if (action.priority_adj != 0) return -1;
    if (action.preempt != 0) return -1;

    return 0;
}

/*
 * Test: ai_validate_action with valid and invalid inputs.
 */
int ai_test_validate_action(void)
{
    struct ai_sched_action a;

    /* Valid action */
    a.core_assignment = 0;
    a.priority_adj = 1;
    a.preempt = 0;
    if (!ai_validate_action(&a, 4)) return -1;

    /* Core out of range */
    a.core_assignment = 4;
    if (ai_validate_action(&a, 4)) return -2;

    /* Priority adj out of range */
    a.core_assignment = 0;
    a.priority_adj = 3;
    if (ai_validate_action(&a, 4)) return -3;

    /* Preempt out of range */
    a.priority_adj = 0;
    a.preempt = 2;
    if (ai_validate_action(&a, 4)) return -4;

    /* NULL pointer */
    if (ai_validate_action(NULL, 4)) return -5;

    return 0;
}

/*
 * Test: argmax with empty array (n=0) returns -1.
 */
int ai_test_argmax_empty(void)
{
    float x[] = {1.0f};
    int idx = ai_test_argmax(x, 0);
    if (idx != -1) return -1;
    return 0;
}

/*
 * Test: argmax with single element returns 0.
 */
int ai_test_argmax_single(void)
{
    float x[] = {42.0f};
    int idx = ai_test_argmax(x, 1);
    if (idx != 0) return -1;
    return 0;
}

/*
 * Test: relu with single element.
 */
int ai_test_relu_single_neg(void)
{
    float x[] = {-5.0f};
    ai_test_relu(x, 1);
    if (!approx_eq(x[0], 0.0f, 0.001f)) return -1;
    return 0;
}

/*
 * Test: relu with n=0 — should not crash.
 */
int ai_test_relu_empty(void)
{
    float x[] = {1.0f};
    ai_test_relu(x, 0);
    /* x[0] should be unchanged since n=0 means nothing to process */
    if (!approx_eq(x[0], 1.0f, 0.001f)) return -1;
    return 0;
}

/*
 * Test: matvec with M=1 (single output row).
 * W = [2, 3], bias = [1], in = [4, 5] → out = 2*4 + 3*5 + 1 = 24
 */
int ai_test_matvec_single_row(void)
{
    float W[] = {2.0f, 3.0f};
    float bias[] = {1.0f};
    float in[] = {4.0f, 5.0f};
    float out[1];

    ai_test_matvec(W, bias, in, out, 1, 2);
    if (!approx_eq(out[0], 24.0f, 0.001f)) return -1;
    return 0;
}

/*
 * Test: matvec with larger dimensions (8×8) to exercise NEON path.
 * Uses non-trivial values to verify correctness beyond the 4-wide boundary.
 */
int ai_test_matvec_8x8(void)
{
    /* W = all 1s, bias = all 0, in = [1..8] → each out = sum(1..8) = 36 */
    float W[64];
    float bias[8];
    float in[8];
    float out[8];

    for (int i = 0; i < 64; i++) W[i] = 1.0f;
    for (int i = 0; i < 8; i++) bias[i] = 0.0f;
    for (int i = 0; i < 8; i++) in[i] = (float)(i + 1);

    ai_test_matvec(W, bias, in, out, 8, 8);

    float expected = 36.0f; /* 1+2+3+4+5+6+7+8 */
    for (int i = 0; i < 8; i++) {
        if (!approx_eq(out[i], expected, 0.01f)) return -1;
    }
    return 0;
}

/*
 * Test: ai_schedule_mlp rejects NULL state pointer.
 */
int ai_test_mlp_null_state(void)
{
    struct ai_sched_action action;
    int ret = ai_schedule_mlp(NULL, &action);
    if (ret != -1) return -1;
    return 0;
}

/*
 * Test: ai_schedule_ppo rejects NULL state pointer.
 */
int ai_test_ppo_null_state(void)
{
    struct ai_sched_action action;
    int ret = ai_schedule_ppo(NULL, &action);
    if (ret != -1) return -1;
    return 0;
}

/*
 * Test: MLP inference output action is within valid bounds.
 */
int ai_test_mlp_action_bounds(void)
{
    float state[AI_STATE_DIM];
    for (int i = 0; i < AI_STATE_DIM; i++)
        state[i] = 0.0f;

    struct ai_sched_action action;
    int ret = ai_schedule_mlp(state, &action);
    if (ret != 0) return -1;
    if (action.preempt > 1) return -2;
    if (action.priority_adj > 2) return -3;
    /* core_assignment bound depends on N_ACTIONS encoding */
    if (action.core_assignment >= (AI_SCHED_N_ACTIONS + 5) / 6) return -4;
    return 0;
}

/*
 * Test: ai_decode_action roundtrip — every valid index decodes
 * to components that re-encode to the same index.
 */
int ai_test_decode_roundtrip(void)
{
    for (int idx = 0; idx < AI_SCHED_N_ACTIONS; idx++) {
        struct ai_sched_action a;
        ai_decode_action(idx, &a);

        /* Re-encode: idx = core * 6 + priority_adj * 2 + preempt */
        int reencoded = a.core_assignment * 6 + a.priority_adj * 2 + a.preempt;
        if (reencoded != idx) return -(idx + 1);
    }
    return 0;
}

#endif /* ENABLE_BOOT_TESTS */
