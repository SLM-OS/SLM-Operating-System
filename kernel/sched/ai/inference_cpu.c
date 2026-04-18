/*
 * inference_cpu.c — CPU-NEON/SSE backend for inference_device.h.
 *
 * Wraps the compiled-in scheduler MLP (ai_mlp_forward_logits) as a
 * registered inference_device. Exists so the generic
 * inference_device vtable is exercised end-to-end before the Hailo
 * backend (Phase 3/5) comes online — and so scheduler code can
 * swap between CPU-MLP and Hailo-MLP at runtime without branching.
 *
 * Compiled as part of the `ai_sched` static library (see
 * CMakeLists.txt ENABLE_AI_SCHEDULER block), which omits
 * `-mgeneral-regs-only` so NEON/SSE inside the forward pass is
 * legal.
 *
 * Model loading is trivial: the weights live in ai_weights_mlp.c
 * and are compiled into the kernel. load_model returns
 * INF_BUILTIN_HANDLE regardless of input — the backend advertises
 * only one model. A future extension could allow replaceable
 * weights via the same handle space Hailo uses for multi-model
 * support, but that's not needed today.
 */

#include "inference_device.h"
#include "ai_inference.h"
#include "ai_types.h"
#include "fp_context.h"
#include "debug.h"
#include <string.h>

/* Only compiled when ENABLE_AI_SCHEDULER is on (this file lives in
 * the `ai_sched` static library — see CMakeLists.txt). The main
 * kernel calls inference_cpu_register() under the same #ifdef. */

/* -------------------------------------------------------------------------- */
/* ops                                                                         */
/* -------------------------------------------------------------------------- */

static int cpu_mlp_init(struct inference_device *dev)
{
    (void)dev;
    return INF_OK;
}

static int cpu_mlp_load_model(struct inference_device *dev,
                              const void *model, size_t size,
                              inference_model_handle_t *out)
{
    (void)dev; (void)model; (void)size;
    /* The MLP weights are compiled in; "loading" is a no-op. A
     * caller that actually has `.hef`-style bytes should route
     * that request to a backend that advertises INF_CAP_LOAD_MODEL.
     * For symmetry with those backends, we still hand back a
     * handle the caller can use in run(). */
    *out = INF_BUILTIN_HANDLE;
    return INF_OK;
}

static int cpu_mlp_run(struct inference_device *dev,
                       inference_model_handle_t h,
                       const inference_tensor_t *in,
                       inference_tensor_t *out)
{
    (void)dev;

    if (h != INF_BUILTIN_HANDLE) return INF_ERR_INVAL;
    if (!in || !out || !in->data || !out->data) return INF_ERR_INVAL;

    /* Shape check: input = 1D [AI_STATE_DIM] fp32, output =
     * 1D [AI_SCHED_N_ACTIONS] fp32. The scheduler is the only
     * consumer today and always uses exactly these shapes. */
    if (in->dtype != INF_DTYPE_FP32 || out->dtype != INF_DTYPE_FP32) {
        return INF_ERR_BAD_TENSOR;
    }
    if (in->n_elems != AI_STATE_DIM
     || out->n_elems != (uint32_t)AI_SCHED_N_ACTIONS) {
        return INF_ERR_BAD_TENSOR;
    }

    /* FP_CONTEXT_SAVE / _RESTORE is mandatory because ai_schedule_mlp
     * is currently called from IRQ context via the scheduler policy
     * path. The inference_device layer doesn't know which context
     * its callers run in, so save/restore unconditionally — the
     * same defense-in-depth approach sched_ai.c takes today. */
    FP_CONTEXT_SAVE();
    ai_mlp_forward_logits((const float *)in->data, (float *)out->data);
    FP_CONTEXT_RESTORE();

    return INF_OK;
}

static int cpu_mlp_free_model(struct inference_device *dev,
                              inference_model_handle_t h)
{
    (void)dev;
    if (h != INF_BUILTIN_HANDLE) return INF_ERR_INVAL;
    return INF_OK;
}

static void cpu_mlp_shutdown(struct inference_device *dev)
{
    (void)dev;
}

/* -------------------------------------------------------------------------- */
/* Device registration                                                         */
/* -------------------------------------------------------------------------- */

static const struct inference_device_ops cpu_mlp_ops = {
    .name        = "cpu-mlp",
    .caps        = INF_CAP_FP32,
    .init        = cpu_mlp_init,
    .shutdown    = cpu_mlp_shutdown,
    .load_model  = cpu_mlp_load_model,
    .run         = cpu_mlp_run,
    .free_model  = cpu_mlp_free_model,
};

static struct inference_device cpu_mlp_device = {
    .ops = &cpu_mlp_ops,
};

/*
 * Called once from platform init after inference_device registry
 * is ready. Safe to call when AI_SCHED=OFF — the compile-time gate
 * leaves this whole file out of the link in that case.
 */
int inference_cpu_register(void)
{
    return inference_device_register(&cpu_mlp_device);
}
