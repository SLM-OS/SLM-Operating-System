/*
 * latency_hist.c - Log2 latency histogram implementation (M1).
 *
 * No locks. Single-writer per histogram is the contract; readers may
 * race a writer and observe a slightly-stale count, identical to how
 * `ai_policy_stats` is already accessed today.
 */

#include "latency_hist.h"
#include <string.h>

void latency_hist_record(struct latency_hist *h, uint64_t ns)
{
    if (!h) return;

    uint32_t idx = latency_hist_bucket_index(ns);
    h->buckets[idx]++;
    h->count++;
    h->sum_ns += ns;

    if (h->count == 1) {
        h->min_ns = ns;
        h->max_ns = ns;
    } else {
        if (ns < h->min_ns) h->min_ns = ns;
        if (ns > h->max_ns) h->max_ns = ns;
    }
}

void latency_hist_reset(struct latency_hist *h)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
}

void latency_hist_snapshot(const struct latency_hist *src,
                           struct latency_hist *dst)
{
    if (!src || !dst) return;
    /* memcpy is the simplest stable read; a concurrent writer may
     * leave dst slightly inconsistent (count vs bucket totals), but
     * never corrupt — same race window the existing stats counters
     * already accept. */
    memcpy(dst, src, sizeof(*dst));
}

uint64_t latency_hist_percentile(const struct latency_hist *h, uint32_t pct)
{
    if (!h || h->count == 0) return 0;

    if (pct < 1u) pct = 1u;
    if (pct > 99u) pct = 99u;

    /* Round up so e.g. p99 of a 100-sample histogram lands on the
     * 99th sample, not the 100th. */
    uint64_t target = (h->count * pct + 99u) / 100u;
    if (target == 0) target = 1;

    uint64_t cumulative = 0;
    for (uint32_t i = 0; i < LATENCY_HIST_BUCKETS; i++) {
        cumulative += h->buckets[i];
        if (cumulative >= target) {
            /* Midpoint of bucket: low + half-width. The bucket spans
             * [2^(i+5), 2^(i+6)), so width = 2^(i+5) and midpoint
             * sits at 2^(i+5) + 2^(i+4) = 1.5 * 2^(i+5). */
            uint64_t low = latency_hist_bucket_low_ns(i);
            return low + (low >> 1u);
        }
    }
    /* Should be unreachable when count > 0; guard anyway. */
    return latency_hist_bucket_low_ns(LATENCY_HIST_BUCKETS - 1u);
}
