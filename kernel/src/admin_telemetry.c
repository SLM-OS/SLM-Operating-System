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

#include <stdint.h>
#include <stddef.h>

/* Forward decl — implemented in runtime/src/msg_router.rs. We cannot
 * pull `kernel/include/msg_router.h` here without dragging in the
 * Rust-shaped C wrappers; this function-only extern keeps the include
 * graph small. */
extern int msg_router_publish(const uint8_t *topic_name, const uint8_t *data);

/* Append a `key=value` pair to `buf[*pos..cap]`, with `value` formatted
 * as decimal uint64. Inserts a leading space when `*pos > 0`. Returns
 * 0 on success, -1 if the formatted text would exceed cap-1 (leaves
 * space for the trailing nul). The caller is expected to nul-terminate
 * after the final append. */
static int append_kv_uint(char *buf, size_t cap, size_t *pos,
                          const char *key, uint64_t value)
{
    /* Decimal length of `value` (max 20 for UINT64_MAX). */
    char digits[24];
    int dlen = 0;
    if (value == 0) {
        digits[dlen++] = '0';
    } else {
        while (value > 0) {
            digits[dlen++] = (char)('0' + (value % 10ull));
            value /= 10ull;
        }
    }

    size_t need = (*pos > 0 ? 1u : 0u);   /* leading space */
    for (const char *p = key; *p; p++) need++;
    need += 1u;                            /* '=' */
    need += (size_t)dlen;
    if (*pos + need + 1u > cap) return -1;

    if (*pos > 0) buf[(*pos)++] = ' ';
    for (const char *p = key; *p; p++) buf[(*pos)++] = *p;
    buf[(*pos)++] = '=';
    while (dlen > 0) buf[(*pos)++] = digits[--dlen];
    return 0;
}

/* msg_router caps MAX_MSG_LEN at 60 bytes (incl. nul); 64 here is the
 * scratch budget on the publish path. Sized so the fixed key strings
 * `dt=<20-digit ns>` + `fb=<bit>` + separating space + nul fit with
 * room to spare. */
#define TELEMETRY_MAX_PAYLOAD 64u

/* Cumulative event counters — incremented by every successful publish
 * call. Surfaced via `admin_telemetry_get_feed_stats` and the
 * `telemetry stats` shell command. Same plain-uint64 race window as
 * the rest of admin_telemetry.c. */
static uint64_t g_eviction_published;
static uint64_t g_inference_published;

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

    /* Publish — fb=0 here; the fallback (re-fault) callback below
     * publishes its own follow-up event with fb=1. Eviction is low
     * rate (allocator-driven, not bench-stress'd), so an unconditional
     * publish is fine even when no subscribers are attached: msg_router
     * short-circuits via topic-not-found. */
    char payload[TELEMETRY_MAX_PAYLOAD];
    size_t pos = 0;
    if (append_kv_uint(payload, sizeof(payload), &pos, "dt", dt_ns) == 0 &&
        append_kv_uint(payload, sizeof(payload), &pos, "fb", 0u) == 0) {
        payload[pos] = '\0';
        (void)msg_router_publish((const uint8_t *)TELEMETRY_TOPIC_EVICTION,
                                 (const uint8_t *)payload);
        g_eviction_published += 1u;
    }
}

void admin_telemetry_record_eviction_fallback(void)
{
    rate_ewma_tick(&g_eviction_fallback_rate, slm_get_time_ns());
    g_eviction_fallbacks += 1u;

    char payload[TELEMETRY_MAX_PAYLOAD];
    size_t pos = 0;
    /* Fallback is observed AFTER the original decision, so dt is no
     * longer meaningful here. Publish just the fb=1 marker so a
     * subscriber can correlate against the most recent decision. */
    if (append_kv_uint(payload, sizeof(payload), &pos, "fb", 1u) == 0) {
        payload[pos] = '\0';
        (void)msg_router_publish((const uint8_t *)TELEMETRY_TOPIC_EVICTION,
                                 (const uint8_t *)payload);
        g_eviction_published += 1u;
    }
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

    char payload[TELEMETRY_MAX_PAYLOAD];
    size_t pos = 0;
    if (append_kv_uint(payload, sizeof(payload), &pos, "dt", dt_ns) == 0 &&
        append_kv_uint(payload, sizeof(payload), &pos, "ok",
                       ok ? 1ull : 0ull) == 0) {
        payload[pos] = '\0';
        (void)msg_router_publish((const uint8_t *)TELEMETRY_TOPIC_INFERENCE,
                                 (const uint8_t *)payload);
        g_inference_published += 1u;
    }
}

void admin_telemetry_get_feed_stats(struct admin_telemetry_feed_stats *out)
{
    if (!out) return;
    out->eviction_published  = g_eviction_published;
    out->inference_published = g_inference_published;
    out->eviction_topic      = TELEMETRY_TOPIC_EVICTION;
    out->inference_topic     = TELEMETRY_TOPIC_INFERENCE;
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
