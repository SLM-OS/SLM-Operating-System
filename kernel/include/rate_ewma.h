/*
 * rate_ewma.h - Exponentially-smoothed event rate (admin & telemetry suite, M1)
 *
 * Tracks events-per-second using an EWMA over the instantaneous rate
 * computed from inter-event time. Internal state is integer-only —
 * no FP context save/restore required at the call site, which keeps
 * the cost of `rate_ewma_tick` low enough to invoke from every
 * scheduler decision (10 kHz+ on a busy host).
 *
 * Internal representation is Q16.16 fixed-point so a kHz-scale rate
 * (≈ 1e3) and a 1µs-scale inter-event time (≈ 1e9 ns event/event)
 * both fit comfortably in 64 bits without losing useful resolution.
 *
 * Stale handling: if the caller hasn't ticked for several seconds
 * the smoothed rate is considered stale and `rate_ewma_get_q16`
 * decays it to zero. Without this, the smoothed value would otherwise
 * pin at the last observed rate forever after the stream goes idle.
 */

#ifndef RATE_EWMA_H
#define RATE_EWMA_H

#include <stdint.h>

/* Q16.16 representation: integer events/sec is value >> 16. */
#define RATE_EWMA_Q16_SHIFT  16u
#define RATE_EWMA_Q16_ONE    (1u << RATE_EWMA_Q16_SHIFT)

/* Default smoothing factor (≈ 0.25). Each new sample contributes
 * ~25% to the smoothed value; ~75% from history. */
#define RATE_EWMA_DEFAULT_ALPHA_Q16  16384u

/* Stale window: if no events have ticked in this many nanoseconds
 * the reader treats the rate as zero. 5 seconds keeps an idle
 * channel from reporting a phantom "still 1000/s" forever. */
#define RATE_EWMA_STALE_NS  (5ull * 1000ull * 1000ull * 1000ull)

struct rate_ewma {
    uint64_t last_event_ns;     /* timestamp of most recent tick */
    uint64_t rate_per_s_q16;    /* smoothed rate, Q16.16 */
    uint32_t alpha_q16;         /* smoothing factor in [0, 65536] */
    uint32_t _pad;
};

void rate_ewma_init(struct rate_ewma *r, uint32_t alpha_q16);

/* Record one event observed at `now_ns`. Updates the smoothed rate.
 * Cheap: a couple of multiplies and shifts; no allocation, no FP. */
void rate_ewma_tick(struct rate_ewma *r, uint64_t now_ns);

/* Read the smoothed rate (events/s) as Q16.16. `now_ns` is used
 * to apply staleness decay so an idle channel reports zero. */
uint64_t rate_ewma_get_q16(const struct rate_ewma *r, uint64_t now_ns);

/* Convenience: same as `rate_ewma_get_q16` truncated to integer
 * events-per-second. Handy when the consumer doesn't need
 * sub-event-per-second resolution. */
static inline uint64_t rate_ewma_get(const struct rate_ewma *r,
                                     uint64_t now_ns)
{
    return rate_ewma_get_q16(r, now_ns) >> RATE_EWMA_Q16_SHIFT;
}

#endif /* RATE_EWMA_H */
