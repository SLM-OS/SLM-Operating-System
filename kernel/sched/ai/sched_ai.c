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
#include "inference_device.h"
#include "sched.h"
#include "smp.h"
#include "slm_ffi.h"
#include "debug.h"
#include <string.h>

/* Per-policy statistics */
struct ai_policy_stats {
    uint32_t decisions;
    uint32_t fallbacks;
    uint64_t total_latency_ns;
    uint32_t action_hist[AI_SCHED_N_ACTIONS];  /* Per-action index counts */
};

/* Audit F-08 (2026-04-24): static stack budget for the Hailo policy
 * transport buffers. Policy state quantizes to AI_STATE_DIM int8s but
 * the HEF transport may pad up. 256 bytes each comfortably covers the
 * known scheduler HEFs while keeping the per-call stack frame under 1
 * KB combined with FP_CONTEXT_SAVE. Promoted to file scope per code
 * review on PR #355 so the budget is discoverable from a single
 * location instead of buried in a function. */
#define HAILO_AI_TRANSPORT_MAX  256u

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

    /* Record action in histogram (inverse of ai_decode_action) and
     * stash on the task for slm.ai_sched_decision introspection (#211). */
    if (ret >= 0) {
        int idx = action.core_assignment * AI_ACTIONS_PER_CORE
                + action.priority_adj * AI_SCHED_PREEMPT_OPTS
                + action.preempt;
        if (idx >= 0 && idx < AI_SCHED_N_ACTIONS) {
            stats->action_hist[idx]++;
            task->last_ai_action = idx;
        }
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

/*
 * MLP inference routed through the inference_device abstraction.
 *
 * Same signature as ai_schedule_mlp so it drops into the existing
 * ai_assign_cpu_common dispatcher unchanged. The underlying device
 * is whichever backend registered as "cpu-mlp" at boot — by default
 * the NEON/SSE wrapper in inference_cpu.c. When the Hailo backend
 * lands (Phase 3/5) it will register as a separate device and the
 * MLP policy switches to it via inference_device_set_default()
 * rather than any edit here.
 *
 * The FP context save/restore still happens in the enclosing
 * ai_assign_cpu_common call — this wrapper does not add its own.
 *
 * The cpu-mlp device pointer is cached after the first lookup to
 * keep assign_cpu off the registry's linear-scan + string-compare
 * path. Safe because the backend is registered once at boot and
 * never removed; concurrent first-touch stores write the same
 * pointer, so the non-atomic access is benign.
 */
static struct inference_device *cached_cpu_mlp_dev;

static int ai_schedule_mlp_via_device(const float *state,
                                      struct ai_sched_action *action)
{
    struct inference_device *dev = cached_cpu_mlp_dev;
    if (!dev) {
        dev = inference_device_find("cpu-mlp");
        if (!dev) {
            /* Backend not registered (AI_SCHED=OFF leaves the
             * registry empty). Fall back to the direct call. */
            return ai_schedule_mlp(state, action);
        }
        cached_cpu_mlp_dev = dev;
    }

    float logits[AI_SCHED_N_ACTIONS];
    inference_tensor_t in = {
        .data    = (void *)state,
        .n_elems = AI_STATE_DIM,
        .dtype   = INF_DTYPE_FP32,
        .rank    = 1,
        .shape   = { AI_STATE_DIM, 0, 0, 0 },
    };
    inference_tensor_t out = {
        .data    = logits,
        .n_elems = AI_SCHED_N_ACTIONS,
        .dtype   = INF_DTYPE_FP32,
        .rank    = 1,
        .shape   = { AI_SCHED_N_ACTIONS, 0, 0, 0 },
    };

    int rc = inference_run(dev, INF_BUILTIN_HANDLE, &in, &out);
    if (rc != INF_OK) return -1;

    /* argmax inline — ai_argmax is static in ai_inference.c. Three
     * lines of code; not worth exposing the helper. */
    int best = 0;
    float best_val = logits[0];
    for (int i = 1; i < AI_SCHED_N_ACTIONS; i++) {
        if (logits[i] > best_val) { best_val = logits[i]; best = i; }
    }
    ai_decode_action(best, action);
    return 0;
}

static uint32_t ai_mlp_assign_cpu(struct task *task)
{
    return ai_assign_cpu_common(task, ai_schedule_mlp_via_device,
                                &ai_mlp_stats);
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
 * Hailo MLP policy (Phase 6.2)
 *
 * Mirrors the CPU MLP policy but routes inference through the
 * "hailo-8" inference_device backend. Three additional mechanics:
 *
 *   1. Model loading is caller-driven. The shell's `hailo load` path
 *      calls ai_policy_hailo_set_model() with the handle returned by
 *      inference_load_model() plus the HEF-derived quantization
 *      parameters (scale + zero-point for both input and output
 *      tensors). Before a model is set, assign_cpu falls back to the
 *      heuristic policy — the device is registered but has nothing
 *      to run.
 *
 *   2. State vector (fp32) → INT8 input tensor. Uses per-element
 *      scale/zero-point quantization, the same math DFC emitted at
 *      compile time. Quantization formula:
 *          int8 = clamp(round(fp32 / scale + zero_point), -128, 127)
 *
 *   3. INT8 logits → action index. argmax on signed INT8 is invariant
 *      under monotonic dequantization (y = scale*(x - zp)), so we
 *      argmax the raw INT8 values and skip the dequant step entirely.
 *
 * Phase 6.2a (this commit) registers the policy with placeholder
 * quantization defaults (scale=1/128, zp=0) applied when the shell
 * hasn't supplied real params yet. Phase 6.2b extracts real scale/zp
 * from the HEF's quant tensor fields; once that lands, the defaults
 * are only used in tests.
 * ============================================================================ */

static struct ai_policy_stats ai_hailo_stats;

struct ai_hailo_model {
    bool     loaded;
    inference_model_handle_t handle;
    float    input_scale;    /* dequant: fp32 = (int8 - zp) * scale */
    int8_t   input_zp;
    float    output_scale;
    int8_t   output_zp;
    uint32_t input_n;        /* AI_STATE_DIM bytes (one byte per element post-quant) */
    uint32_t output_n;       /* AI_SCHED_N_ACTIONS */
};

static struct ai_hailo_model ai_hailo_model;
static struct inference_device *cached_hailo_dev;

/*
 * Public setter called from the shell's `hailo load` path (and from
 * tests) once a HEF has been parsed + loaded into the device. Passing
 * handle=INF_INVALID_HANDLE clears the current model; the policy then
 * falls back to heuristic on subsequent assign_cpu calls.
 */
void ai_policy_hailo_set_model(inference_model_handle_t handle,
                               float input_scale,  int8_t input_zp,
                               float output_scale, int8_t output_zp,
                               uint32_t input_n, uint32_t output_n)
{
    if (handle == INF_INVALID_HANDLE) {
        /* Release before clearing so a concurrent reader that still
         * observes loaded=true sees coherent (stale) fields, not a
         * half-cleared state. Readers ack with acquire before deref. */
        __atomic_store_n(&ai_hailo_model.loaded, false, __ATOMIC_RELEASE);
        return;
    }
    /* Non-atomic field writes THEN an atomic release-store of `loaded`:
     * any reader that acquires loaded==true is guaranteed to observe
     * the handle/scale/zp/n writes that happened-before. */
    ai_hailo_model.handle       = handle;
    ai_hailo_model.input_scale  = (input_scale  > 0.0f) ? input_scale  : (1.0f / 128.0f);
    ai_hailo_model.input_zp     = input_zp;
    ai_hailo_model.output_scale = (output_scale > 0.0f) ? output_scale : (1.0f / 128.0f);
    ai_hailo_model.output_zp    = output_zp;
    ai_hailo_model.input_n      = input_n  ? input_n  : AI_STATE_DIM;
    ai_hailo_model.output_n     = output_n ? output_n : (uint32_t)AI_SCHED_N_ACTIONS;
    __atomic_store_n(&ai_hailo_model.loaded, true, __ATOMIC_RELEASE);
}

inference_model_handle_t ai_policy_hailo_get_model_handle(void)
{
    /* Acquire matches the release in ai_policy_hailo_set_model so if
     * we see loaded==true, we observe the paired handle write. */
    if (!__atomic_load_n(&ai_hailo_model.loaded, __ATOMIC_ACQUIRE))
        return INF_INVALID_HANDLE;
    return ai_hailo_model.handle;
}

/* Integer-only entry point — see header for rationale. */
void ai_policy_hailo_set_model_placeholder(inference_model_handle_t handle,
                                           uint32_t input_n,
                                           uint32_t output_n)
{
    ai_policy_hailo_set_model(handle, 1.0f/128.0f, 0, 1.0f/128.0f, 0,
                              input_n, output_n);
}

/* Raw-bit-pattern entry point — see header for rationale. */
int ai_policy_hailo_set_model_from_raw(inference_model_handle_t handle,
                                       uint32_t input_scale_raw,
                                       uint32_t input_zp_raw,
                                       uint32_t output_scale_raw,
                                       uint32_t output_zp_raw,
                                       uint32_t input_n, uint32_t output_n)
{
    float in_scale, in_zp, out_scale, out_zp;
    /* Reinterpret the 32-bit IEEE-754 bit patterns as floats without
     * triggering the undefined-behavior aliasing that pointer-casts
     * would; memcpy is the standard C way to punt on this. */
    memcpy(&in_scale,  &input_scale_raw,  4);
    memcpy(&in_zp,     &input_zp_raw,     4);
    memcpy(&out_scale, &output_scale_raw, 4);
    memcpy(&out_zp,    &output_zp_raw,    4);

    /* Reject degenerate values that would crash the quantize loop
     * (x / 0 → inf) or produce garbage. Caller falls back to the
     * placeholder setter on -1 without mutating state. */
    if (!(in_scale > 0.0f) || !(out_scale > 0.0f)) return -1;
    if (in_scale != in_scale || out_scale != out_scale) return -1;  /* NaN */

    /* Clamp zero-point to the int8 range. Hailo emits integer values
     * as floats; the scheduler MLP's int8 output has zp in [-128,127]. */
    int32_t in_zpi  = (int32_t)in_zp;
    int32_t out_zpi = (int32_t)out_zp;
    if (in_zpi  < -128) in_zpi  = -128;
    if (in_zpi  >  127) in_zpi  =  127;
    if (out_zpi < -128) out_zpi = -128;
    if (out_zpi >  127) out_zpi =  127;

    ai_policy_hailo_set_model(handle, in_scale, (int8_t)in_zpi,
                              out_scale, (int8_t)out_zpi,
                              input_n, output_n);
    return 0;
}

static int ai_hailo_init(void)
{
    ai_hailo_stats.decisions = 0;
    ai_hailo_stats.fallbacks = 0;
    ai_hailo_stats.total_latency_ns = 0;
    for (int i = 0; i < AI_SCHED_N_ACTIONS; i++)
        ai_hailo_stats.action_hist[i] = 0;

    cached_hailo_dev = inference_device_find("hailo-8");

    /* Two degraded paths, both non-fatal so the policy switch still
     * succeeds and the user can load a model or move to a different
     * build target without a policy churn: every assign_cpu call
     * gracefully falls back to the heuristic (round-robin) scheduler
     * until Hailo is ready. */
    if (!cached_hailo_dev) {
        WARN("AI Hailo: 'hailo-8' device not present on this build "
             "(QEMU / x86 have no NPU). Scheduling decisions will "
             "fall back to the heuristic policy. Use `sched policy "
             "ai_mlp` for CPU-based AI inference instead.");
    } else if (!ai_hailo_model.loaded) {
        WARN("AI Hailo: policy activated but no model is loaded. "
             "Scheduling decisions will fall back to the heuristic "
             "until `hailo load <path> sched` supplies a .hef.");
    } else {
        INFO("AI Hailo: policy initialized (model handle=%d already armed)",
             (int)ai_hailo_model.handle);
    }
    return 0;
}

static void ai_hailo_shutdown(void)
{
    if (ai_hailo_stats.decisions > 0) {
        uint64_t avg_ns = ai_hailo_stats.total_latency_ns / ai_hailo_stats.decisions;
        INFO("AI Hailo: %u decisions, %u fallbacks, avg latency %lu ns",
             ai_hailo_stats.decisions, ai_hailo_stats.fallbacks,
             (unsigned long)avg_ns);
    }
    /* Don't free the model here — the shell owns the handle and may
     * switch back to Hailo later without reloading. */
}

/* Quantize an fp32 state vector to INT8 using per-tensor scale+zp.
 * clamp(round(x/scale + zp), -128, 127). */
static void quantize_fp32_to_int8(const float *src, int8_t *dst, uint32_t n,
                                   float scale, int8_t zp)
{
    for (uint32_t i = 0; i < n; i++) {
        float q = src[i] / scale + (float)zp;
        /* Round to nearest, ties away from zero. */
        int32_t qi = (int32_t)(q + (q >= 0.0f ? 0.5f : -0.5f));
        if (qi < -128) qi = -128;
        if (qi >  127) qi =  127;
        dst[i] = (int8_t)qi;
    }
}

static int ai_schedule_mlp_via_hailo(const float *state,
                                     struct ai_sched_action *action)
{
    /* Acquire-load loaded: on true, we see all set_model field writes. */
    if (!__atomic_load_n(&ai_hailo_model.loaded, __ATOMIC_ACQUIRE)
     || !cached_hailo_dev) {
        /* No model yet — return failure; ai_assign_cpu_common will
         * fall back to heuristic and bump the fallbacks counter. */
        return -1;
    }

    /* Audit F-08 (2026-04-24): backend transport buffers can exceed
     * the policy's logical AI_STATE_DIM / AI_SCHED_N_ACTIONS due to
     * HEF padding. Size the stack buffers to a generous fixed cap so
     * the HEF transport size determines the in/out byte count, not
     * the policy semantic dim. quantize_fp32_to_int8 still only
     * writes AI_STATE_DIM int8s; the trailing in_int8[AI_STATE_DIM..]
     * stays zero (buffer is stack-zeroed below) which is the conservative
     * pad value for HEFs whose extra input bytes are alignment slack. */
    uint32_t in_n  = ai_hailo_model.input_n  ? ai_hailo_model.input_n
                                             : (uint32_t)AI_STATE_DIM;
    uint32_t out_n = ai_hailo_model.output_n ? ai_hailo_model.output_n
                                             : (uint32_t)AI_SCHED_N_ACTIONS;
    if (in_n > HAILO_AI_TRANSPORT_MAX || out_n > HAILO_AI_TRANSPORT_MAX) {
        /* HEF demands more transport than we statically reserve. Fall
         * back to the heuristic; warn ONCE so the operator sees the
         * policy demotion (PR #355 review). Subsequent loads/calls of
         * a too-big HEF stay silent — the WARN is to surface the
         * downgrade, not flood the log on every assign_cpu. */
        static bool transport_too_big_warned = false;
        if (!transport_too_big_warned) {
            transport_too_big_warned = true;
            WARN("AI Hailo: HEF transport (in=%u out=%u) exceeds "
                 "HAILO_AI_TRANSPORT_MAX=%u; ai_hailo policy will "
                 "fall back to heuristic on every assign_cpu. Raise "
                 "HAILO_AI_TRANSPORT_MAX in sched_ai.c if this HEF "
                 "is intended for AI scheduling.",
                 in_n, out_n, HAILO_AI_TRANSPORT_MAX);
        }
        return -1;
    }

    int8_t in_int8[HAILO_AI_TRANSPORT_MAX]  = {0};
    int8_t out_int8[HAILO_AI_TRANSPORT_MAX] = {0};

    quantize_fp32_to_int8(state, in_int8, AI_STATE_DIM,
                          ai_hailo_model.input_scale,
                          ai_hailo_model.input_zp);

    inference_tensor_t in = {
        .data    = in_int8,
        .n_elems = in_n,
        .dtype   = INF_DTYPE_INT8,
        .rank    = 1,
        .shape   = { (uint16_t)in_n, 0, 0, 0 },
    };
    inference_tensor_t out = {
        .data    = out_int8,
        .n_elems = out_n,
        .dtype   = INF_DTYPE_INT8,
        .rank    = 1,
        .shape   = { (uint16_t)out_n, 0, 0, 0 },
    };

    int rc = inference_run(cached_hailo_dev, ai_hailo_model.handle, &in, &out);
    if (rc != INF_OK) return -1;

    /* argmax INT8 — monotonic under dequant, so no need to float-ify. */
    uint32_t n = ai_hailo_model.output_n;
    if (n > (uint32_t)AI_SCHED_N_ACTIONS) n = AI_SCHED_N_ACTIONS;
    int best = 0;
    int8_t best_val = out_int8[0];
    for (uint32_t i = 1; i < n; i++) {
        if (out_int8[i] > best_val) { best_val = out_int8[i]; best = (int)i; }
    }
    ai_decode_action(best, action);
    return 0;
}

static uint32_t ai_hailo_assign_cpu(struct task *task)
{
    return ai_assign_cpu_common(task, ai_schedule_mlp_via_hailo,
                                &ai_hailo_stats);
}

const struct sched_policy_ops sched_policy_ai_hailo = {
    .name       = "ai_hailo",
    .init       = ai_hailo_init,
    .shutdown   = ai_hailo_shutdown,
    .assign_cpu = ai_hailo_assign_cpu,
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
    /* policy_name[3] disambiguates among "ai_mlp", "ai_ppo", "ai_hailo" */
    if (policy_name[0] == 'a' && policy_name[3] == 'm')
        stats = &ai_mlp_stats;
    else if (policy_name[0] == 'a' && policy_name[3] == 'p')
        stats = &ai_ppo_stats;
    else if (policy_name[0] == 'a' && policy_name[3] == 'h')
        stats = &ai_hailo_stats;

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
#ifndef PLATFORM_X86_64
    sched_register_policy(&sched_policy_ai_hailo);
#endif
}
