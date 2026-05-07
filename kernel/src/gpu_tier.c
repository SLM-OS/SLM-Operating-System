/*
 * gpu_tier.c - Runtime tier preference for GPU dispatch (#664)
 *
 * One atomic uint32_t holding an SLM_GPU_TIER_* value plus a last-
 * change timestamp. The tier is observed by the dispatch path
 * (slm_gpu_run, eventually) when picking which library kernel to
 * launch; the toggle here is a pure preference store.
 *
 * Mirrors gpu_consumer.c's structure. Setting any tier when
 * inference is OFF succeeds — operators can pre-configure intent
 * before flipping `gpu use inference on`.
 */

#include "gpu_tier.h"

#include "gpu_consumer.h"
#include "slm_ffi.h"
#include "string.h"

#include <stdatomic.h>

static atomic_uint g_gpu_tier = SLM_GPU_TIER_AUTO;
static uint64_t g_gpu_tier_change_ms = 0;

const char *gpu_tier_name(uint32_t tier)
{
    switch (tier) {
    case SLM_GPU_TIER_AUTO: return "auto";
    case SLM_GPU_TIER_HMMA: return "hmma";
    case SLM_GPU_TIER_SIMT: return "simt";
    case SLM_GPU_TIER_CPU:  return "cpu";
    default: return NULL;
    }
}

uint32_t gpu_tier_from_name(const char *name)
{
    if (!name) return SLM_GPU_TIER_INVALID;
    if (strcmp(name, "auto") == 0) return SLM_GPU_TIER_AUTO;
    if (strcmp(name, "hmma") == 0) return SLM_GPU_TIER_HMMA;
    if (strcmp(name, "simt") == 0) return SLM_GPU_TIER_SIMT;
    if (strcmp(name, "fp32") == 0) return SLM_GPU_TIER_SIMT; /* alias */
    if (strcmp(name, "cpu")  == 0) return SLM_GPU_TIER_CPU;
    return SLM_GPU_TIER_INVALID;
}

uint32_t gpu_tier_get(void)
{
    /* `relaxed` because the atomic publishes only itself — there is
     * no other state the dispatcher needs to acquire-synchronise
     * with via this load. Same pattern as
     * `gpu_dispatch_record_result` (slm_ffi.c) and the rate-limit
     * timestamp from PR #653. */
    return atomic_load_explicit(&g_gpu_tier, memory_order_relaxed);
}

uint64_t gpu_tier_last_change_ms(void)
{
    /* Plain memory; only writer is gpu_tier_set. Same race window
     * the existing scheduler stats accept on 64-bit platforms. */
    return g_gpu_tier_change_ms;
}

int gpu_tier_set(uint32_t tier, const char **out_reason)
{
    if (gpu_tier_name(tier) == NULL) {
        if (out_reason) *out_reason = "unknown tier";
        return GPU_TIER_ERR_INVAL;
    }

    /* Tier preference is allowed on platforms without a GPU — it
     * just has no effect. We surface that as a note rather than
     * rejecting, so configuration can persist across reboots into
     * builds that may or may not have GPU support compiled in. */
    if (slm_gpu_available() == 0 && tier != SLM_GPU_TIER_CPU) {
        if (out_reason) {
            *out_reason = "no GPU on this build — preference recorded but inert";
        }
        /* Fall through to the store; preserve the operator's intent. */
    } else if (out_reason && tier != SLM_GPU_TIER_CPU
               && !gpu_consumer_enabled(GPU_CONSUMER_INFERENCE)) {
        /* GPU is available but inference is off, so the tier
         * preference won't drive any dispatch until the operator
         * flips `gpu use inference on`. Surface that gap. */
        *out_reason = "preference active when `gpu use inference on`";
    }

    atomic_store_explicit(&g_gpu_tier, tier, memory_order_relaxed);
    g_gpu_tier_change_ms = slm_get_time_ns() / 1000000ull;
    return 0;
}

/* Snapshot is intentionally non-atomic: the four fields are read
 * one at a time and a concurrent writer on another CPU could update
 * `tier` between this load and the `change_ms` read. That's
 * acceptable here — the snapshot is only consumed by debug/UI code
 * paths (`gpu tier status` shell command, telemetry feed). The
 * dispatcher reads `gpu_tier_get()` directly and never sees a torn
 * tier. */
void gpu_tier_status_get(struct gpu_tier_status *out)
{
    if (!out) return;
    out->tier              = gpu_tier_get();
    out->gpu_ready         = slm_gpu_available() != 0;
    out->inference_enabled = gpu_consumer_enabled(GPU_CONSUMER_INFERENCE);
    out->change_ms         = gpu_tier_last_change_ms();
}
