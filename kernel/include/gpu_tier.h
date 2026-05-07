/*
 * gpu_tier.h - Runtime tier preference for GPU dispatch (#664)
 *
 * Sibling of `gpu_consumer.h`. Where `gpu_consumer` says WHO is using
 * the GPU (sched / eviction / inference), `gpu_tier` says WHICH kernel
 * variant the dispatcher prefers when multiple are available in the
 * operator library:
 *
 *   AUTO   prefer HMMA tensor-core; fall back to SIMT; fall back to CPU
 *   HMMA   require HMMA; missing kernel for an op is an error
 *   SIMT   force CUDA-core FP32 path (current behavior)
 *   CPU    skip GPU entirely; route to NEON
 *
 * Tier values are the existing SLM_GPU_TIER_* macros from
 * `gpu_handoff.h` — one source of truth shared with the per-op handoff
 * descriptor and the Rust runtime's `Tier` enum.
 *
 * Setting a tier preference while `gpu_consumer_enabled(INFERENCE)` is
 * false succeeds — it's a preference, not a dispatch. The dispatch
 * path observes the tier only when inference is enabled.
 */

#ifndef GPU_TIER_H
#define GPU_TIER_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu_handoff.h"

/* Sentinel returned by gpu_tier_from_name() on unknown name. Distinct
 * from any valid SLM_GPU_TIER_* value so callers can distinguish
 * "unknown" from "valid AUTO". */
#define SLM_GPU_TIER_INVALID  UINT32_MAX

/* Stable lowercase canonical name. NULL for invalid input.
 * Returns: "auto", "hmma", "simt", "cpu". The "fp32" alias for SIMT
 * is accepted by from_name() but not emitted by name(). */
const char *gpu_tier_name(uint32_t tier);

/* Lookup by name (lowercase, exact match). Recognised: "auto", "hmma",
 * "simt", "fp32" (alias for SIMT), "cpu". Returns SLM_GPU_TIER_INVALID
 * for unknown names. */
uint32_t gpu_tier_from_name(const char *name);

/* Read the current tier preference. Lock-free; safe from any context.
 * Default at boot is SLM_GPU_TIER_AUTO. */
uint32_t gpu_tier_get(void);

/* Error codes returned by gpu_tier_set on rejection. Local rather
 * than POSIX errno (kernel is freestanding). Negative for
 * compatibility with the kernel return-int convention. */
#define GPU_TIER_ERR_INVAL    (-1)  /* unknown tier value */
#define GPU_TIER_ERR_NODEV    (-2)  /* GPU not available on this build */
#define GPU_TIER_ERR_NOTSUPP  (-3)  /* tier not buildable on this platform */

/* Set the tier preference. Returns 0 on success, GPU_TIER_ERR_*
 * on rejection. *out_reason (if non-NULL) is set on either success
 * (informational note, e.g. "preference active when inference
 * enabled") or failure (rejection reason).
 *
 * Concurrent setters race naturally on the atomic; the last writer
 * wins. There is no per-tier lock — the cost of a race is one extra
 * millisecond of `last_change_ms` on the loser. */
int gpu_tier_set(uint32_t tier, const char **out_reason);

/* Wall-clock millisecond timestamp of the most recent successful
 * gpu_tier_set. Returns 0 if the toggle has never been written
 * since boot. */
uint64_t gpu_tier_last_change_ms(void);

/* Snapshot status for shell + telemetry. */
struct gpu_tier_status {
    uint32_t tier;
    bool gpu_ready;
    bool inference_enabled;
    uint64_t change_ms;
};

void gpu_tier_status_get(struct gpu_tier_status *out);

#endif /* GPU_TIER_H */
