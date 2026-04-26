/*
 * admin_telemetry.c - Consumer-scoped telemetry storage (M3).
 *
 * Process-global per-consumer state for eviction and inference. See
 * `admin_telemetry.h` for the contract.
 */

#include "admin_telemetry.h"
#include "latency_hist.h"
#include "rate_ewma.h"
#include "slm_ffi.h"

/* ===== Eviction ====================================================== */

static struct latency_hist g_eviction_hist;
static struct rate_ewma    g_eviction_decision_rate;
static struct rate_ewma    g_eviction_fallback_rate;
static uint64_t            g_eviction_decisions;
static uint64_t            g_eviction_fallbacks;
static uint64_t            g_eviction_total_ns;

void admin_telemetry_record_eviction_decision(uint64_t dt_ns)
{
    latency_hist_record(&g_eviction_hist, dt_ns);
    /* Read time once so the rate tick and the totals see the same
     * timestamp — avoids a tiny skew if the scheduler preempts us
     * between the two reads. */
    uint64_t now = slm_get_time_ns();
    rate_ewma_tick(&g_eviction_decision_rate, now);
    g_eviction_decisions += 1u;
    g_eviction_total_ns  += dt_ns;
}

void admin_telemetry_record_eviction_fallback(void)
{
    rate_ewma_tick(&g_eviction_fallback_rate, slm_get_time_ns());
    g_eviction_fallbacks += 1u;
}

int admin_telemetry_get_eviction_stats(struct latency_hist *out_hist,
                                       uint64_t *out_decision_rate_q16,
                                       uint64_t *out_fallback_rate_q16,
                                       uint64_t *out_total_decisions,
                                       uint64_t *out_total_fallbacks,
                                       uint64_t *out_total_ns)
{
    uint64_t now = slm_get_time_ns();

    if (out_hist) latency_hist_snapshot(&g_eviction_hist, out_hist);
    if (out_decision_rate_q16)
        *out_decision_rate_q16 =
            rate_ewma_get_q16(&g_eviction_decision_rate, now);
    if (out_fallback_rate_q16)
        *out_fallback_rate_q16 =
            rate_ewma_get_q16(&g_eviction_fallback_rate, now);
    if (out_total_decisions) *out_total_decisions = g_eviction_decisions;
    if (out_total_fallbacks) *out_total_fallbacks = g_eviction_fallbacks;
    if (out_total_ns)        *out_total_ns        = g_eviction_total_ns;
    return 0;
}

void admin_telemetry_reset_eviction(void)
{
    latency_hist_reset(&g_eviction_hist);
    rate_ewma_init(&g_eviction_decision_rate, 0);
    rate_ewma_init(&g_eviction_fallback_rate, 0);
    g_eviction_decisions = 0;
    g_eviction_fallbacks = 0;
    g_eviction_total_ns  = 0;
}

/* ===== Inference ===================================================== */

static struct latency_hist g_inference_hist;
static struct rate_ewma    g_inference_call_rate;
static struct rate_ewma    g_inference_error_rate;
static uint64_t            g_inference_calls;
static uint64_t            g_inference_errors;
static uint64_t            g_inference_total_ns;

void admin_telemetry_record_inference(uint64_t dt_ns, bool ok)
{
    latency_hist_record(&g_inference_hist, dt_ns);
    uint64_t now = slm_get_time_ns();
    rate_ewma_tick(&g_inference_call_rate, now);
    g_inference_calls += 1u;
    g_inference_total_ns += dt_ns;
    if (!ok) {
        rate_ewma_tick(&g_inference_error_rate, now);
        g_inference_errors += 1u;
    }
}

int admin_telemetry_get_inference_stats(struct latency_hist *out_hist,
                                        uint64_t *out_calls_per_s_q16,
                                        uint64_t *out_errors_per_s_q16,
                                        uint64_t *out_total_calls,
                                        uint64_t *out_total_errors,
                                        uint64_t *out_total_ns)
{
    uint64_t now = slm_get_time_ns();

    if (out_hist) latency_hist_snapshot(&g_inference_hist, out_hist);
    if (out_calls_per_s_q16)
        *out_calls_per_s_q16 = rate_ewma_get_q16(&g_inference_call_rate, now);
    if (out_errors_per_s_q16)
        *out_errors_per_s_q16 = rate_ewma_get_q16(&g_inference_error_rate, now);
    if (out_total_calls)  *out_total_calls  = g_inference_calls;
    if (out_total_errors) *out_total_errors = g_inference_errors;
    if (out_total_ns)     *out_total_ns     = g_inference_total_ns;
    return 0;
}

void admin_telemetry_reset_inference(void)
{
    latency_hist_reset(&g_inference_hist);
    rate_ewma_init(&g_inference_call_rate, 0);
    rate_ewma_init(&g_inference_error_rate, 0);
    g_inference_calls = 0;
    g_inference_errors = 0;
    g_inference_total_ns = 0;
}
