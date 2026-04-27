/*
 * gpu_consumer.c - Per-consumer GPU enable/disable implementation (M2).
 *
 * Three atomic_bool flags + last-change timestamps. The validation
 * logic for `set(true)` is intentionally conservative for M2 — no
 * consumer has a wired GPU backend yet, so every "enable" call
 * returns NOTSUPP with a reason. M3+ unlocks each consumer in turn:
 *   - SCHED unlocks when a sched_policy_ops with `has_gpu_backend=true`
 *     is registered AND active.
 *   - EVICTION unlocks when an eviction_policy with a GPU backend is
 *     registered (Rust side). The C-side toggle still validates here.
 *   - INFERENCE unlocks when a GPU inference engine is registered with
 *     `slm_gpu_available()` returning non-zero.
 */

#include "gpu_consumer.h"
#include "sched.h"
#include "sched_policy.h"
#include "slm_ffi.h"
#include "string.h"

#include <stdatomic.h>

static atomic_bool g_consumer_enabled[GPU_CONSUMER_COUNT];
static uint64_t g_consumer_change_ms[GPU_CONSUMER_COUNT];

static const char *const k_consumer_names[GPU_CONSUMER_COUNT] = {
    [GPU_CONSUMER_SCHED]     = "sched",
    [GPU_CONSUMER_EVICTION]  = "eviction",
    [GPU_CONSUMER_INFERENCE] = "inference",
};

const char *gpu_consumer_name(enum gpu_consumer c)
{
    if ((unsigned)c >= GPU_CONSUMER_COUNT) return NULL;
    return k_consumer_names[c];
}

enum gpu_consumer gpu_consumer_from_name(const char *name)
{
    if (!name) return GPU_CONSUMER_COUNT;
    for (unsigned i = 0; i < GPU_CONSUMER_COUNT; i++) {
        if (strcmp(name, k_consumer_names[i]) == 0)
            return (enum gpu_consumer)i;
    }
    return GPU_CONSUMER_COUNT;
}

bool gpu_consumer_enabled(enum gpu_consumer c)
{
    if ((unsigned)c >= GPU_CONSUMER_COUNT) return false;
    return atomic_load_explicit(&g_consumer_enabled[c], memory_order_acquire);
}

uint64_t gpu_consumer_last_change_ms(enum gpu_consumer c)
{
    if ((unsigned)c >= GPU_CONSUMER_COUNT) return 0;
    /* `g_consumer_change_ms` is plain memory; the only writer is
     * `gpu_consumer_set`. A racing reader may see a torn 64-bit value
     * on 32-bit ARM, but on the supported 64-bit platforms the read
     * is atomic at the hardware level. Acceptable for a stats field
     * — same race window the existing scheduler stats already accept. */
    return g_consumer_change_ms[c];
}

/* Look up the active scheduler policy and report whether it declares
 * a GPU backend. M2 returns false for every registered policy
 * (heuristic, ai_mlp, ai_ppo, ai_hailo) — they ship with
 * `has_gpu_backend = false` until M3+ adds a real GPU MLP path. */
static bool active_sched_policy_has_gpu_backend(void)
{
    const char *name = sched_get_policy();
    if (!name) return false;
    const struct sched_policy_ops *ops = sched_find_policy(name);
    if (!ops) return false;
    return ops->has_gpu_backend;
}

int gpu_consumer_set(enum gpu_consumer c, bool enabled,
                     const char **out_reason)
{
    if ((unsigned)c >= GPU_CONSUMER_COUNT) {
        if (out_reason) *out_reason = "unknown consumer";
        return GPU_CONSUMER_ERR_INVAL;
    }

    if (!enabled) {
        /* Disable always succeeds, even if it was already off. */
        atomic_store_explicit(&g_consumer_enabled[c], false,
                              memory_order_release);
        g_consumer_change_ms[c] = slm_get_time_ns() / 1000000ull;
        if (out_reason) *out_reason = NULL;
        return 0;
    }

    /* Step 1: GPU available at all? */
    if (slm_gpu_available() == 0) {
        if (out_reason) *out_reason = "GPU not available on this build";
        return GPU_CONSUMER_ERR_NODEV;
    }

    /* Step 2: per-consumer accept-or-warn declaration.
     *
     * The toggles are now operator-intent flags: enabling the flag
     * always succeeds when the GPU is available; the actual dispatch
     * behavior depends on whether each consumer has a GPU backend
     * actually wired up.
     *
     *   inference: WIRED. The MNIST whole-graph fastpath
     *              (`engine::mnist_gpu_fastpath_eligible` in the Rust
     *              runtime) consults this flag — flipping it OFF
     *              forces CPU fallback even on Jetson with the v6
     *              channel handoff present.
     *
     *   sched:     SCAFFOLDED ONLY. No `sched_policy_ops` declares
     *              has_gpu_backend=true today (see
     *              kernel/include/sched_policy.h §`has_gpu_backend`).
     *              The toggle accepts on/off so operators can pre-
     *              configure intent before a GPU-capable scheduler
     *              policy lands; until then the dispatch path is
     *              pure CPU regardless of the flag, and a warning
     *              is returned via `out_reason` to make the gap
     *              visible in `gpu use status`.
     *
     *   eviction:  SCAFFOLDED ONLY. Eviction policies live in the
     *              Rust runtime and don't expose a GPU dispatch
     *              entry point yet. Same accept-with-warning
     *              behavior as sched. */
    switch (c) {
    case GPU_CONSUMER_SCHED:
        if (!active_sched_policy_has_gpu_backend()) {
            if (out_reason)
                *out_reason = "scaffold only — no scheduler policy "
                              "declares a GPU backend yet";
            /* Fall through to the flip — operator intent is recorded
             * even though dispatch is unaffected. */
        }
        break;

    case GPU_CONSUMER_EVICTION:
        /* Symmetric to the sched case: consult the Rust runtime via
         * `eviction_active_policy_has_gpu_backend()` (which scans
         * both pools' installed policies). When no policy declares
         * a GPU backend, accept-with-warning so operators can still
         * record intent ahead of the PR-6 dispatch landing. The
         * shipped policies (LRU/LFU/ARC/CACHEUS/MLP/XGBoost) all
         * return false from `EvictionPolicy::has_gpu_backend()`
         * today; this branch will start accepting cleanly the first
         * time a policy flips that to true. */
        if (!eviction_active_policy_has_gpu_backend()) {
            if (out_reason)
                *out_reason = "scaffold only — no eviction policy "
                              "declares a GPU backend yet";
            /* Fall through to the flip — operator intent is recorded
             * even though dispatch is unaffected. */
        }
        break;

    case GPU_CONSUMER_INFERENCE:
        /* MNIST GPU fastpath consults this flag in
         * runtime/src/inference/engine.rs. */
        break;

    default:
        if (out_reason) *out_reason = "unknown consumer";
        return GPU_CONSUMER_ERR_INVAL;
    }

    /* All gates passed — flip it on.
     *
     * Do NOT clear `*out_reason` here. The accept-with-warning cases
     * above (sched / eviction with no real GPU backend wired) set
     * `*out_reason` on the success path, and the shell renders that
     * as a "note: …" line so the operator can see the gap between
     * "intent recorded" and "dispatch actually changed". The fully
     * wired inference case never writes `*out_reason`, so the
     * caller's initial NULL persists. */
    atomic_store_explicit(&g_consumer_enabled[c], true,
                          memory_order_release);
    g_consumer_change_ms[c] = slm_get_time_ns() / 1000000ull;
    return 0;
}

void gpu_consumer_status_get(struct gpu_consumer_status *out)
{
    if (!out) return;
    out->sched     = gpu_consumer_enabled(GPU_CONSUMER_SCHED);
    out->eviction  = gpu_consumer_enabled(GPU_CONSUMER_EVICTION);
    out->inference = gpu_consumer_enabled(GPU_CONSUMER_INFERENCE);
    out->gpu_ready = slm_gpu_available() != 0;
    out->sched_change_ms     = gpu_consumer_last_change_ms(GPU_CONSUMER_SCHED);
    out->eviction_change_ms  = gpu_consumer_last_change_ms(GPU_CONSUMER_EVICTION);
    out->inference_change_ms = gpu_consumer_last_change_ms(GPU_CONSUMER_INFERENCE);
}
