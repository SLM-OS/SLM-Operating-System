/*
 * admin_telemetry.c - Consumer-scoped telemetry storage (M3).
 *
 * Process-global per-consumer state for eviction and inference. See
 * `admin_telemetry.h` for the contract.
 */

#include "admin_telemetry.h"
#include "latency_hist.h"
#include "platform.h"
#include "pmm.h"
#include "rate_ewma.h"
#include "slm_ffi.h"
#include "smp.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

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

/* ===== Periodic publishers (tel.cpu / tel.stl / tel.mem) =============
 *
 * Runs from net_poll() at ~100 Hz; emits at TELEMETRY_PERIODIC_INTERVAL_MS
 * boundaries (default 1 Hz). Cheap when not yet time to publish (one
 * timestamp compare + arithmetic).
 *
 * The first call after boot establishes the snapshot baseline and does
 * not publish — deltas are meaningless on the first sample. Subsequent
 * calls publish the per-CPU and aggregate-eviction deltas.
 *
 * sched_diag_* counters live in NC memory on PLATFORM_HAS_NC_MEMORY
 * (Pi 5, Jetson) and in BSS otherwise. The pointer-vs-array distinction
 * doesn't matter at the indexing site (both index uniformly), but the
 * extern declarations have to match the underlying definition or the
 * linker quietly resolves the wrong type. Mirrors the existing pattern
 * in kernel/src/shell_sys.c.
 */
#if defined(PLATFORM_HAS_NC_MEMORY)
extern volatile uint32_t *sched_diag_picked;
extern volatile uint32_t *sched_diag_idle_loops;
extern volatile uint32_t *sched_diag_steal_attempts;
extern volatile uint32_t *sched_diag_steal_successes;
extern volatile uint32_t *sched_diag_steal_stale;
extern volatile uint32_t *sched_diag_steal_empty_victim;
extern volatile uint32_t *sched_diag_steal_push_full;
#else
extern volatile uint32_t sched_diag_picked[];
extern volatile uint32_t sched_diag_idle_loops[];
extern volatile uint32_t sched_diag_steal_attempts[];
extern volatile uint32_t sched_diag_steal_successes[];
extern volatile uint32_t sched_diag_steal_stale[];
extern volatile uint32_t sched_diag_steal_empty_victim[];
extern volatile uint32_t sched_diag_steal_push_full[];
#endif

static uint32_t g_periodic_last_pump_ms;
static bool     g_periodic_baseline;
static uint64_t g_cpu_published;
static uint64_t g_steal_published;
static uint64_t g_memory_published;

/* Per-CPU snapshots from prior tick. MAX_CPUS sized so we can run on
 * any platform without compile-time dependence on cpu_count. */
static uint32_t g_prev_picked[MAX_CPUS];
static uint32_t g_prev_idle_loops[MAX_CPUS];
static uint32_t g_prev_steal_attempts[MAX_CPUS];
static uint32_t g_prev_steal_successes[MAX_CPUS];
static uint32_t g_prev_steal_stale[MAX_CPUS];
static uint32_t g_prev_steal_empty_victim[MAX_CPUS];
static uint32_t g_prev_steal_push_full[MAX_CPUS];
static uint64_t g_prev_evi_weight;
static uint64_t g_prev_evi_workspace;

/* Last-payload buffers for the test seam. Captured every time the
 * corresponding publish_*() helper builds a payload string, regardless
 * of whether msg_router has any subscribers. Tests verify the formatted
 * shape without standing up an msg_router subscription. */
static char g_last_cpu_payload[TELEMETRY_MAX_PAYLOAD];
static char g_last_steal_payload[TELEMETRY_MAX_PAYLOAD];
static char g_last_memory_payload[TELEMETRY_MAX_PAYLOAD];

/* Wrap-safe delta helper. sched_diag_* are uint32_t and incremented
 * without saturation; if a counter wraps between samples we still want
 * a sensible delta (the unsigned subtraction does the right thing
 * mod 2^32 — at 1 Hz publish + one increment per scheduler decision,
 * a wrap takes ≥4 billion decisions ≥ months of uptime, so this
 * mostly just guards against future counter-type changes). */
static inline uint32_t delta_u32(uint32_t now, uint32_t prev)
{
    return (uint32_t)(now - prev);
}

static void publish_cpu_util(uint32_t cpus)
{
    char payload[TELEMETRY_MAX_PAYLOAD];
    size_t pos = 0;

    /* Read every counter once before computing — minimises skew from
     * concurrent updates by other CPUs. NC-allocated pointers are
     * non-NULL by the time net_poll() drives this (scheduler_init
     * runs first); BSS arrays are always valid. */
    uint32_t picked_now[MAX_CPUS];
    uint32_t idle_now[MAX_CPUS];
    for (uint32_t i = 0; i < cpus && i < MAX_CPUS; i++) {
        picked_now[i] = sched_diag_picked[i];
        idle_now[i]   = sched_diag_idle_loops[i];
    }

    for (uint32_t i = 0; i < cpus && i < MAX_CPUS; i++) {
        uint32_t dp = delta_u32(picked_now[i], g_prev_picked[i]);
        uint32_t di = delta_u32(idle_now[i],   g_prev_idle_loops[i]);
        /* Load% over the interval. dp counts task-picks (active work);
         * di counts idle-loop iterations (CPU was idle). The ratio
         * approximates "fraction of scheduler decisions that found
         * work to run" — a coarse but cheap proxy for utilization
         * that doesn't need wall-clock timing per CPU. Returns 0 when
         * neither counter advanced (CPU offline / dormant — see #216). */
        uint64_t denom = (uint64_t)dp + (uint64_t)di;
        uint32_t load = denom > 0 ? (uint32_t)(((uint64_t)dp * 100ull) / denom) : 0u;

        char key[4] = { 'c', '0', '\0', '\0' };
        if (i < 10) {
            key[1] = (char)('0' + i);
        } else {
            key[1] = (char)('0' + (i / 10));
            key[2] = (char)('0' + (i % 10));
        }
        if (append_kv_uint(payload, sizeof(payload), &pos, key, load) != 0)
            break; /* payload full — drop remaining CPUs from this sample */

        g_prev_picked[i]     = picked_now[i];
        g_prev_idle_loops[i] = idle_now[i];
    }
    payload[pos] = '\0';
    memcpy(g_last_cpu_payload, payload, sizeof(g_last_cpu_payload));
    if (msg_router_publish((const uint8_t *)TELEMETRY_TOPIC_CPU_UTIL,
                           (const uint8_t *)payload) == 0) {
        g_cpu_published += 1u;
    }
}

static void publish_steal(uint32_t cpus)
{
    char payload[TELEMETRY_MAX_PAYLOAD];
    size_t pos = 0;
    uint64_t att = 0, ok = 0, stl = 0, emp = 0, fll = 0;

    for (uint32_t i = 0; i < cpus && i < MAX_CPUS; i++) {
        uint32_t a_now = sched_diag_steal_attempts[i];
        uint32_t s_now = sched_diag_steal_successes[i];
        uint32_t t_now = sched_diag_steal_stale[i];
        uint32_t e_now = sched_diag_steal_empty_victim[i];
        uint32_t f_now = sched_diag_steal_push_full[i];

        att += delta_u32(a_now, g_prev_steal_attempts[i]);
        ok  += delta_u32(s_now, g_prev_steal_successes[i]);
        stl += delta_u32(t_now, g_prev_steal_stale[i]);
        emp += delta_u32(e_now, g_prev_steal_empty_victim[i]);
        fll += delta_u32(f_now, g_prev_steal_push_full[i]);

        g_prev_steal_attempts[i]     = a_now;
        g_prev_steal_successes[i]    = s_now;
        g_prev_steal_stale[i]        = t_now;
        g_prev_steal_empty_victim[i] = e_now;
        g_prev_steal_push_full[i]    = f_now;
    }

    if (append_kv_uint(payload, sizeof(payload), &pos, "att", att) != 0) goto done;
    if (append_kv_uint(payload, sizeof(payload), &pos, "ok",  ok)  != 0) goto done;
    if (append_kv_uint(payload, sizeof(payload), &pos, "stl", stl) != 0) goto done;
    if (append_kv_uint(payload, sizeof(payload), &pos, "emp", emp) != 0) goto done;
    if (append_kv_uint(payload, sizeof(payload), &pos, "fll", fll) != 0) goto done;
done:
    payload[pos] = '\0';
    memcpy(g_last_steal_payload, payload, sizeof(g_last_steal_payload));
    if (msg_router_publish((const uint8_t *)TELEMETRY_TOPIC_STEAL,
                           (const uint8_t *)payload) == 0) {
        g_steal_published += 1u;
    }
}

static void publish_memory(void)
{
    char payload[TELEMETRY_MAX_PAYLOAD];
    size_t pos = 0;
    struct pmm_stats pst;
    pmm_get_stats(&pst);

    RustEvictionStats est;
    memset(&est, 0, sizeof(est));
    int32_t evi_rc = rust_eviction_get_stats(&est);

    uint64_t wev = 0, xev = 0;
    if (evi_rc == 0) {
        /* u64 unsigned subtraction is wrap-safe the same way u32 is
         * (mod 2^64) — same rationale as delta_u32, just at the
         * native counter width so we don't lose info if eviction
         * counts ever exceed UINT32_MAX. */
        wev = est.weight_evictions    - g_prev_evi_weight;
        xev = est.workspace_evictions - g_prev_evi_workspace;
        g_prev_evi_weight    = est.weight_evictions;
        g_prev_evi_workspace = est.workspace_evictions;
    }

    if (append_kv_uint(payload, sizeof(payload), &pos, "fp",  (uint64_t)pst.free_pages)  != 0) goto done;
    if (append_kv_uint(payload, sizeof(payload), &pos, "tp",  (uint64_t)pst.total_pages) != 0) goto done;
    if (append_kv_uint(payload, sizeof(payload), &pos, "wev", wev) != 0) goto done;
    if (append_kv_uint(payload, sizeof(payload), &pos, "xev", xev) != 0) goto done;
done:
    payload[pos] = '\0';
    memcpy(g_last_memory_payload, payload, sizeof(g_last_memory_payload));
    if (msg_router_publish((const uint8_t *)TELEMETRY_TOPIC_MEMORY,
                           (const uint8_t *)payload) == 0) {
        g_memory_published += 1u;
    }
}

void admin_telemetry_periodic_pump(uint32_t now_ms)
{
    if (!g_periodic_baseline) {
        /* First call: capture the baseline snapshot but don't publish.
         * A delta needs two samples; the first one establishes "prev". */
        uint32_t cpus = cpu_count;
        if (cpus > MAX_CPUS) cpus = MAX_CPUS;
        for (uint32_t i = 0; i < cpus; i++) {
            g_prev_picked[i]             = sched_diag_picked[i];
            g_prev_idle_loops[i]         = sched_diag_idle_loops[i];
            g_prev_steal_attempts[i]     = sched_diag_steal_attempts[i];
            g_prev_steal_successes[i]    = sched_diag_steal_successes[i];
            g_prev_steal_stale[i]        = sched_diag_steal_stale[i];
            g_prev_steal_empty_victim[i] = sched_diag_steal_empty_victim[i];
            g_prev_steal_push_full[i]    = sched_diag_steal_push_full[i];
        }
        RustEvictionStats est;
        memset(&est, 0, sizeof(est));
        if (rust_eviction_get_stats(&est) == 0) {
            g_prev_evi_weight    = est.weight_evictions;
            g_prev_evi_workspace = est.workspace_evictions;
        }
        g_periodic_last_pump_ms = now_ms;
        g_periodic_baseline = true;
        return;
    }

    /* Wrap-safe interval check. sys_now() rolls over every ~49 days
     * (uint32_t ms); the unsigned subtraction handles that case. */
    uint32_t elapsed = now_ms - g_periodic_last_pump_ms;
    if (elapsed < TELEMETRY_PERIODIC_INTERVAL_MS) return;
    g_periodic_last_pump_ms = now_ms;

    uint32_t cpus = cpu_count;
    if (cpus > MAX_CPUS) cpus = MAX_CPUS;

    publish_cpu_util(cpus);
    publish_steal(cpus);
    publish_memory();
}

void admin_telemetry_get_periodic_stats(struct admin_telemetry_periodic_stats *out)
{
    if (!out) return;
    out->cpu_published    = g_cpu_published;
    out->steal_published  = g_steal_published;
    out->memory_published = g_memory_published;
    out->last_pump_ms     = g_periodic_last_pump_ms;
    out->baseline_set     = g_periodic_baseline;
}

void admin_telemetry_periodic_reset_for_tests(void)
{
    g_periodic_last_pump_ms = 0;
    g_periodic_baseline = false;
    g_cpu_published = 0;
    g_steal_published = 0;
    g_memory_published = 0;
    memset(g_prev_picked, 0, sizeof(g_prev_picked));
    memset(g_prev_idle_loops, 0, sizeof(g_prev_idle_loops));
    memset(g_prev_steal_attempts, 0, sizeof(g_prev_steal_attempts));
    memset(g_prev_steal_successes, 0, sizeof(g_prev_steal_successes));
    memset(g_prev_steal_stale, 0, sizeof(g_prev_steal_stale));
    memset(g_prev_steal_empty_victim, 0, sizeof(g_prev_steal_empty_victim));
    memset(g_prev_steal_push_full, 0, sizeof(g_prev_steal_push_full));
    g_prev_evi_weight = 0;
    g_prev_evi_workspace = 0;
    g_last_cpu_payload[0]    = '\0';
    g_last_steal_payload[0]  = '\0';
    g_last_memory_payload[0] = '\0';
}

/* Test seam: copy the most recent payload string published on each
 * periodic topic into `out`. Empty string when the topic has not been
 * published since reset. Mirrors how the publish helpers truncate at
 * TELEMETRY_MAX_PAYLOAD-1, so callers can safely use any cap >= 1. */
static void copy_payload_truncating(char *out, size_t cap, const char *src)
{
    if (!out || cap == 0) return;
    size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; i++) {
        out[i] = src[i];
    }
    out[i] = '\0';
}

void admin_telemetry_get_last_cpu_payload_for_tests(char *out, size_t cap)
{
    copy_payload_truncating(out, cap, g_last_cpu_payload);
}

void admin_telemetry_get_last_steal_payload_for_tests(char *out, size_t cap)
{
    copy_payload_truncating(out, cap, g_last_steal_payload);
}

void admin_telemetry_get_last_memory_payload_for_tests(char *out, size_t cap)
{
    copy_payload_truncating(out, cap, g_last_memory_payload);
}

/* ===== AI scheduler decision audit (tel.aix) ========================= */

static uint64_t g_ai_decision_published;
static char     g_last_ai_decision_payload[TELEMETRY_MAX_PAYLOAD];

static int append_kv_char(char *buf, size_t cap, size_t *pos,
                          const char *key, char value)
{
    size_t klen = 0;
    for (const char *p = key; *p; p++) klen++;
    size_t need = (*pos > 0 ? 1u : 0u) + klen + 1u + 1u; /* sp+key+'='+ch */
    if (*pos + need + 1u > cap) return -1;

    if (*pos > 0) buf[(*pos)++] = ' ';
    for (const char *p = key; *p; p++) buf[(*pos)++] = *p;
    buf[(*pos)++] = '=';
    buf[(*pos)++] = value;
    return 0;
}

void admin_telemetry_record_ai_decision(char policy_id,
                                        uint8_t core_assignment,
                                        uint8_t priority_adj,
                                        uint8_t preempt,
                                        uint64_t dt_ns,
                                        bool fallback)
{
    char payload[TELEMETRY_MAX_PAYLOAD];
    size_t pos = 0;

    /* Wire-format guard. policy_id lands directly in the payload string
     * which travels through msg_router → tcp_telemetry_server's
     * newline-framed protocol; an out-of-set char (especially '\n' or
     * '\0') would corrupt the per-line TCP framing seen by host
     * consumers. Production callers all pass documented constants;
     * fail-open with '?' so a future buggy caller is detectable in
     * the wire stream rather than silently breaking it. */
    if (policy_id != ADMIN_TEL_AI_POLICY_MLP   &&
        policy_id != ADMIN_TEL_AI_POLICY_PPO   &&
        policy_id != ADMIN_TEL_AI_POLICY_HAILO) {
        policy_id = ADMIN_TEL_AI_POLICY_UNKNOWN;
    }

    /* p=<one char>. Always present — the policy id is meaningful in
     * both success and fallback cases (which AI policy was active
     * when the decision was attempted). */
    if (append_kv_char(payload, sizeof(payload), &pos, "p", policy_id) != 0)
        goto done;

    if (!fallback) {
        /* Action fields. core/priority_adj/preempt are u8 with small
         * domains; append_kv_uint formats them as decimal. */
        if (append_kv_uint(payload, sizeof(payload), &pos, "c",  (uint64_t)core_assignment) != 0) goto done;
        if (append_kv_uint(payload, sizeof(payload), &pos, "pa", (uint64_t)priority_adj)    != 0) goto done;
        if (append_kv_uint(payload, sizeof(payload), &pos, "pe", (uint64_t)preempt)         != 0) goto done;
    }

    if (append_kv_uint(payload, sizeof(payload), &pos, "dt", dt_ns) != 0) goto done;
    if (append_kv_uint(payload, sizeof(payload), &pos, "fb", fallback ? 1ull : 0ull) != 0) goto done;
done:
    payload[pos] = '\0';
    memcpy(g_last_ai_decision_payload, payload, sizeof(g_last_ai_decision_payload));
    if (msg_router_publish((const uint8_t *)TELEMETRY_TOPIC_AI_DECIDE,
                           (const uint8_t *)payload) == 0) {
        g_ai_decision_published += 1u;
    }
}

uint64_t admin_telemetry_get_ai_decision_published(void)
{
    return g_ai_decision_published;
}

void admin_telemetry_get_last_ai_decision_payload_for_tests(char *out, size_t cap)
{
    copy_payload_truncating(out, cap, g_last_ai_decision_payload);
}

void admin_telemetry_ai_decision_reset_for_tests(void)
{
    g_ai_decision_published = 0;
    g_last_ai_decision_payload[0] = '\0';
}
