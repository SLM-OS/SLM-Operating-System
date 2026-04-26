/*
 * admin_telemetry.h - Consumer-scoped latency + rate counters (M3).
 *
 * Mirrors the per-policy `latency_hist` + `rate_ewma` pair that
 * sched_ai.c already keeps for the scheduler decision site, but
 * stored in process-global slots indexed by consumer rather than
 * embedded in each policy struct. Two consumers wired in M3:
 *
 *   EVICTION  — measured at `runtime/src/mm/eviction/registry.rs`
 *               `select_victim()`, recorded via FFI from Rust.
 *   INFERENCE — measured at the three `slm.model_infer*` Lua
 *               bindings, recorded inline.
 *
 * The scheduler is NOT routed through here — its histogram is
 * per-policy by design, exposed via `sched_ai_get_rate_stats`
 * (see kernel/sched/ai/sched_ai.c).
 *
 * Per-model inference rate breakdown (spec §6.2 promises this) is
 * deferred to M4 alongside the telemetry feed; M3 reports a single
 * global inference series.
 */

#ifndef ADMIN_TELEMETRY_H
#define ADMIN_TELEMETRY_H

#include "latency_hist.h"
#include "rate_ewma.h"

#include <stdbool.h>
#include <stdint.h>

/* ===== Eviction ====================================================== */

/* Called from Rust eviction registry after `select_victim` measures dt.
 * `dt_ns` is the wall-clock latency of the policy's victim selection.
 * Bumps the latency histogram and ticks the decision rate. */
void admin_telemetry_record_eviction_decision(uint64_t dt_ns);

/* Called when an eviction is observed to have re-faulted (fallback).
 * Mirrors `update_feedback(_, true)` on the Rust side. Ticks the
 * fallback rate; does NOT touch the latency hist (the cost was
 * already recorded at decision time). */
void admin_telemetry_record_eviction_fallback(void);

/* Snapshot accessor — returns 0 always (no failure mode). */
int admin_telemetry_get_eviction_stats(struct latency_hist *out_hist,
                                       uint64_t *out_decision_rate_q16,
                                       uint64_t *out_fallback_rate_q16,
                                       uint64_t *out_total_decisions,
                                       uint64_t *out_total_fallbacks,
                                       uint64_t *out_total_ns);

/* Reset counters to zero. Useful for the `stats eviction --reset`
 * shell control surface (M4). */
void admin_telemetry_reset_eviction(void);

/* ===== Inference ===================================================== */

/* Called from the three `l_model_infer*` Lua bindings after each
 * inference call. `ok` is true if the underlying inference returned
 * a non-error result. `dt_ns` is the wall-clock latency.
 *
 * Errors are NOT excluded from the latency histogram — callers may
 * want to know "how long does a failed inference take" — but they
 * tick the error_rate counter instead of the decision_rate. */
void admin_telemetry_record_inference(uint64_t dt_ns, bool ok);

/* Snapshot accessor. */
int admin_telemetry_get_inference_stats(struct latency_hist *out_hist,
                                        uint64_t *out_calls_per_s_q16,
                                        uint64_t *out_errors_per_s_q16,
                                        uint64_t *out_total_calls,
                                        uint64_t *out_total_errors,
                                        uint64_t *out_total_ns);

/* Reset counters to zero. */
void admin_telemetry_reset_inference(void);

#endif /* ADMIN_TELEMETRY_H */
