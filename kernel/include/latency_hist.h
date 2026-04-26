/*
 * latency_hist.h - Log2 latency histogram (admin & telemetry suite, M1)
 *
 * 32 power-of-two buckets covering [32 ns, 2^37 ns ≈ 137 s). Bucket i
 * holds samples in [2^(i+5), 2^(i+6)) ns. Values below 32 ns clamp to
 * bucket 0; values at or above 2^37 ns clamp to bucket 31.
 *
 * Thread-safety: single-writer per histogram is the design contract
 * (one consumer site, e.g. the scheduler decision path). Concurrent
 * writers race the same way `ai_policy_stats` already does — counts
 * may be slightly off under contention but never corrupt. Readers
 * take a snapshot copy for safe percentile readout.
 *
 * Deliberately no allocation: all storage is by-value in the struct.
 */

#ifndef LATENCY_HIST_H
#define LATENCY_HIST_H

#include <stdint.h>

#define LATENCY_HIST_BUCKETS 32u

struct latency_hist {
    uint64_t buckets[LATENCY_HIST_BUCKETS];
    uint64_t count;
    uint64_t sum_ns;
    uint64_t min_ns;
    uint64_t max_ns;
};

/* Index `ns` into the bucket array. Clamps to [0, LATENCY_HIST_BUCKETS-1]. */
static inline uint32_t latency_hist_bucket_index(uint64_t ns)
{
    if (ns < 32u) return 0u;
    /* log2_floor(ns) via 63 - clz; clz of 0 is undefined, so the
     * ns < 32 fast path above keeps the input safely positive. */
    uint32_t lg = 63u - (uint32_t)__builtin_clzll(ns);
    if (lg < 5u) return 0u;
    uint32_t idx = lg - 5u;
    if (idx >= LATENCY_HIST_BUCKETS) idx = LATENCY_HIST_BUCKETS - 1u;
    return idx;
}

/* Lower bound of bucket `i` in nanoseconds (`2^(i+5)`). */
static inline uint64_t latency_hist_bucket_low_ns(uint32_t i)
{
    return (uint64_t)1u << (i + 5u);
}

/* Record one observation. Safe to call from any context — no locks,
 * no allocation, no float. Single-writer assumption (see header). */
void latency_hist_record(struct latency_hist *h, uint64_t ns);

/* Reset all buckets and counters. */
void latency_hist_reset(struct latency_hist *h);

/* Percentile readout. `pct` is in [1, 99]; values outside that range
 * are clamped. Returns the lower bound of the bucket that contains
 * the percentile, plus a half-bucket offset (so p50 sits at the
 * midpoint of its bucket). Returns 0 when count == 0. */
uint64_t latency_hist_percentile(const struct latency_hist *h, uint32_t pct);

/* Snapshot copy. Useful when a reader needs a stable view while a
 * writer may be active on another CPU. */
void latency_hist_snapshot(const struct latency_hist *src,
                           struct latency_hist *dst);

#endif /* LATENCY_HIST_H */
