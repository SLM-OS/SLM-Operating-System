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
#include <stddef.h>
#include <stdint.h>

/*
 * Telemetry topic naming (M4).
 *
 * Spec §7.1 calls for `/telemetry/<consumer>/<metric>` paths, but the
 * underlying msg_router caps `TOPIC_NAME_LEN` at 16 bytes (including
 * null terminator) — `/telemetry/sched/decision` doesn't fit.
 *
 * M4 ships a compact `tel.<consumer>` schema instead. Long-form topic
 * names are deferred to a follow-up that bumps msg_router's buffer
 * sizes (cross-cutting change). Documented in spec §14.
 *
 * Wildcard pattern: `tel.*` matches all telemetry topics. Subscribe
 * via existing `slm.msg_subscribe` or `slm.telemetry_subscribe`.
 *
 * Sample payload format: short ASCII key=value pairs separated by
 * single spaces, capped at 59 chars + nul to fit MAX_MSG_LEN=60.
 *   tel.evi:  "dt=<ns> fb=<0|1>"           e.g. "dt=12345 fb=0"
 *   tel.inf:  "dt=<ns> ok=<0|1>"           e.g. "dt=87654 ok=1"
 *   tel.cpu:  "c0=<ld> c1=<ld> ... c<n>=<ld>"  e.g. "c0=42 c1=99 c2=18 c3=7"
 *             — per-CPU load percent (0..100) over the publish interval.
 *   tel.stl:  "att=<n> ok=<n> stl=<n> emp=<n> fll=<n>"
 *             — work-stealing deltas across all CPUs since last publish.
 *   tel.mem:  "fp=<n> tp=<n> wev=<n> xev=<n>"
 *             — free pages, total pages, weight/workspace eviction deltas.
 *   tel.aix:  "p=<m|p|h> c=<core> pa=<0..2> pe=<0|1> dt=<ns> fb=<0|1>"
 *             — per-AI-scheduler-decision audit (policy id, chosen
 *             core, priority adj, preempt, latency, fallback). Action
 *             fields are omitted when fb=1 (inference failed → action
 *             undefined, heuristic fallback path took over).
 */
#define TELEMETRY_TOPIC_EVICTION   "tel.evi"
#define TELEMETRY_TOPIC_INFERENCE  "tel.inf"
#define TELEMETRY_TOPIC_CPU_UTIL   "tel.cpu"
#define TELEMETRY_TOPIC_STEAL      "tel.stl"
#define TELEMETRY_TOPIC_MEMORY     "tel.mem"
#define TELEMETRY_TOPIC_AI_DECIDE  "tel.aix"
#define TELEMETRY_TOPIC_PREFIX     "tel."

/* Default publish interval for the periodic pump (1 Hz). */
#define TELEMETRY_PERIODIC_INTERVAL_MS  1000u

/* ===== Eviction ====================================================== */

/* Called from Rust eviction registry after `select_victim` measures dt.
 * `dt_ns` is the wall-clock latency of the policy's victim selection.
 * Bumps the latency histogram and ticks the decision rate.
 *
 * BLOCKING NOTE: this call publishes to the `tel.evi` msg_router topic.
 * If a subscriber is registered but has stopped draining its mailbox
 * (e.g. a wedged Lua script, a disconnected telnet shell that left a
 * `slm.telemetry_subscribe` callback alive), `msg_router_publish` will
 * stall up to ACK_TIMEOUT_SECS (5s) per call waiting for the ACK
 * timeout. Eviction is allocator-driven, so a stalled record path
 * stalls every allocation attempt that triggers victim selection.
 *
 * Mitigation: keep telemetry subscriptions short-lived. The
 * `slm.telemetry_unsubscribe(handle)` call from Lua, or shell
 * disconnect (which runs the per-state teardown helper at lua_slm.c),
 * removes the subscription. If you observe eviction-path latency
 * regressions, check `slm.msg_router` is not over-subscribed first. */
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

/* ===== Telemetry feed introspection (M4) ============================ */

struct admin_telemetry_feed_stats {
    /* Cumulative count of `tel.evi` payloads pushed to msg_router
     * (regardless of whether anyone was subscribed). */
    uint64_t eviction_published;
    /* Cumulative count of `tel.inf` payloads pushed to msg_router. */
    uint64_t inference_published;
    /* Topic name strings the operator can subscribe to. */
    const char *eviction_topic;
    const char *inference_topic;
};

void admin_telemetry_get_feed_stats(struct admin_telemetry_feed_stats *out);

/* ===== Periodic publishers (tel.cpu / tel.stl / tel.mem) ============= */

/*
 * Drive the 1-Hz periodic publisher. Cheap when the publish window
 * hasn't elapsed (one timestamp compare). The caller passes the
 * current monotonic millisecond timestamp; production wires this to
 * `sys_now()` from `net_poll`. Tests pass a synthetic value to drive
 * the rate-limit deterministically.
 *
 * On each elapsed window:
 *   - Snapshot per-CPU sched_diag_picked / sched_diag_idle_loops and
 *     publish `tel.cpu` (per-CPU load percent).
 *   - Snapshot sched_diag_steal_* and publish `tel.stl` (work-stealing
 *     deltas across all CPUs).
 *   - Snapshot pmm + Rust eviction stats and publish `tel.mem` (free
 *     pages, total pages, weight/workspace eviction count deltas).
 *
 * The first call after boot establishes the snapshot baseline and
 * does not publish. Subsequent calls publish when ≥ interval_ms have
 * elapsed since the last publish.
 *
 * BLOCKING NOTE: each elapsed window fires THREE `msg_router_publish`
 * calls back-to-back (one per topic). If any subscriber to `tel.cpu`,
 * `tel.stl`, or `tel.mem` has stopped draining its mailbox (wedged
 * Lua script, leaked telnet subscription, etc.), `msg_router_publish`
 * will stall up to ACK_TIMEOUT_SECS (5 s). With three publishes per
 * tick that's up to 15 s of stall in the net_poll task per window —
 * larger amplification than the existing event-driven publishers.
 * Stalls in net_poll block lwIP timers, DHCP renewals, RX-stall
 * watchdog, CDC-ECM polling, and any other consumer of net_poll
 * cadence.
 *
 * Mitigation is the same as for `tel.evi` / `tel.inf`: keep telemetry
 * subscriptions short-lived. `slm.telemetry_unsubscribe(handle)` and
 * the per-state shell-disconnect teardown helper in `lua_slm.c`
 * remove subscriptions cleanly. If net-pump latency regresses after a
 * session leak, check `slm.msg_router` for orphan subscribers first.
 */
void admin_telemetry_periodic_pump(uint32_t now_ms);

/* Counters surfaced for observability + the test seam. */
struct admin_telemetry_periodic_stats {
    uint64_t cpu_published;       /* tel.cpu samples sent */
    uint64_t steal_published;     /* tel.stl samples sent */
    uint64_t memory_published;    /* tel.mem samples sent */
    uint32_t last_pump_ms;        /* most recent tick that emitted */
    bool     baseline_set;        /* true after first call */
};

void admin_telemetry_get_periodic_stats(struct admin_telemetry_periodic_stats *out);

/* Test seam: reset the periodic-pump baseline so tests can drive a
 * fresh sequence without rebuilding the kernel. */
void admin_telemetry_periodic_reset_for_tests(void);

/* Test seam: read back the most recent payload published on each
 * periodic topic. Capacity must be at least 64 (matches the publisher's
 * scratch buffer); shorter is silently truncated. Empty string when
 * the topic has not been published since reset. Lets unit tests pin
 * the formatted payload string without subscribing through msg_router. */
void admin_telemetry_get_last_cpu_payload_for_tests(char *out, size_t cap);
void admin_telemetry_get_last_steal_payload_for_tests(char *out, size_t cap);
void admin_telemetry_get_last_memory_payload_for_tests(char *out, size_t cap);

/* ===== AI scheduler decision audit (tel.aix) ======================== */

/* Policy identifiers carried in the `p=` field of tel.aix payloads.
 * One char so the whole payload fits MAX_MSG_LEN=60 with room to
 * spare. Match the dispatch in `sched_ai.c`. The publisher rejects
 * any policy_id outside this set with a fail-open `?` marker — see
 * admin_telemetry_record_ai_decision below for the rationale. */
#define ADMIN_TEL_AI_POLICY_MLP    'm'
#define ADMIN_TEL_AI_POLICY_PPO    'p'
#define ADMIN_TEL_AI_POLICY_HAILO  'h'
#define ADMIN_TEL_AI_POLICY_UNKNOWN '?'

/*
 * Called from sched_ai.c's ai_assign_cpu_common() after each AI
 * scheduler decision (success or fallback). Publishes a `tel.aix`
 * sample carrying:
 *
 *   p=<policy_id>           one char from ADMIN_TEL_AI_POLICY_*
 *   c=<core>                core_assignment (0..cpu_count-1) — success only
 *   pa=<priority_adj>       0=none, 1=boost, 2=reduce — success only
 *   pe=<preempt>            0=no, 1=yes — success only
 *   dt=<ns>                 wall-clock decision latency
 *   fb=<0|1>                fallback flag — 1 when AI inference failed
 *                           or returned an invalid action and the
 *                           dispatcher fell back to the heuristic
 *
 * On fallback (fb=1) the c/pa/pe fields are omitted (action is
 * undefined; the heuristic policy returned the actual CPU choice
 * and we don't track its action shape here). Pass `fallback=true`
 * with the core/priority_adj/preempt args ignored — the publisher
 * skips them.
 *
 * Implicit opt-in: this function is only called from AI-policy code
 * paths. When the active scheduler policy is `heuristic`, the
 * publish path never runs — there's no global runtime flag to
 * toggle. Same trade-off as tel.evi (allocator-driven only) and
 * tel.inf (Lua-binding-driven only): you opt in by activating the
 * relevant subsystem.
 *
 * BLOCKING NOTE: msg_router_publish can stall up to ACK_TIMEOUT_SECS
 * (5 s) if a subscriber's mailbox is wedged. AI scheduler decisions
 * are dispatcher-rate (every yield point under COOP_PREEMPT), so a
 * stalled subscriber stalls every yield in the kernel. Mitigation:
 * keep telemetry subscriptions short-lived, same as for tel.evi/inf.
 *
 * WIRE-FORMAT GUARD: any `policy_id` outside the documented set
 * ({MLP, PPO, HAILO}) is replaced with `'?'` (ADMIN_TEL_AI_POLICY_UNKNOWN)
 * before publishing. Production callers always pass the documented
 * constants; this defends against a future caller accidentally
 * passing an invalid char (typo, new-policy collision) — control
 * chars like `'\n'` would corrupt the per-line TCP framing in
 * tcp_telemetry_server.
 */
void admin_telemetry_record_ai_decision(char policy_id,
                                        uint8_t core_assignment,
                                        uint8_t priority_adj,
                                        uint8_t preempt,
                                        uint64_t dt_ns,
                                        bool fallback);

/* Cumulative count of tel.aix samples published since boot. Surfaced
 * via admin_telemetry_get_periodic_stats's sibling-style accessor. */
uint64_t admin_telemetry_get_ai_decision_published(void);

/* Test seam — same shape as the periodic-topic seams above. */
void admin_telemetry_get_last_ai_decision_payload_for_tests(char *out, size_t cap);
void admin_telemetry_ai_decision_reset_for_tests(void);

#endif /* ADMIN_TELEMETRY_H */
