/*
 * rate_ewma.c - Exponentially-smoothed event rate implementation (M1).
 *
 * Q16.16 fixed-point arithmetic throughout. The hottest path
 * (`rate_ewma_tick`) is one division (1e9 << 16 / dt_ns), one
 * multiply-add for the EWMA blend, and a couple of stores. No FP,
 * no allocation, safe to call from any scheduler context.
 */

#include "rate_ewma.h"
#include <string.h>

#define NS_PER_S_Q16  ((uint64_t)1000000000ull << RATE_EWMA_Q16_SHIFT)

void rate_ewma_init(struct rate_ewma *r, uint32_t alpha_q16)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->alpha_q16 = alpha_q16 ? alpha_q16 : RATE_EWMA_DEFAULT_ALPHA_Q16;
    if (r->alpha_q16 > RATE_EWMA_Q16_ONE) r->alpha_q16 = RATE_EWMA_Q16_ONE;
}

void rate_ewma_tick(struct rate_ewma *r, uint64_t now_ns)
{
    if (!r) return;

    /* First-ever tick: just timestamp it. We need a `dt` to compute
     * an instantaneous rate, so the rate stays at zero until the
     * second event lands. */
    if (r->last_event_ns == 0) {
        r->last_event_ns = now_ns;
        if (r->alpha_q16 == 0) {
            r->alpha_q16 = RATE_EWMA_DEFAULT_ALPHA_Q16;
        }
        return;
    }

    uint64_t dt_ns = (now_ns > r->last_event_ns)
                    ? (now_ns - r->last_event_ns)
                    : 1u;

    /* Instantaneous rate (events/sec) in Q16.16:
     *   1 event / dt_ns ns × 1e9 ns/s = 1e9 / dt_ns events/s
     *   Q16:  (1e9 << 16) / dt_ns
     * Saturate: extreme bursts (dt_ns < 1) capped to keep the EWMA
     * blend well-behaved. */
    uint64_t inst_q16 = NS_PER_S_Q16 / dt_ns;

    /* EWMA blend: rate = alpha * inst + (1 - alpha) * rate
     * All terms in Q16.16; intermediate is Q32 so we shift right
     * by alpha's Q16 width afterwards. */
    uint32_t alpha = r->alpha_q16;
    uint64_t blended =
          (inst_q16 * alpha)
        + (r->rate_per_s_q16 * (RATE_EWMA_Q16_ONE - alpha));
    r->rate_per_s_q16 = blended >> RATE_EWMA_Q16_SHIFT;

    r->last_event_ns = now_ns;
}

uint64_t rate_ewma_get_q16(const struct rate_ewma *r, uint64_t now_ns)
{
    if (!r) return 0;
    if (r->last_event_ns == 0) return 0;

    /* Stale stream → report zero. Without this, a channel that was
     * once busy and is now idle would keep reporting its last
     * smoothed value indefinitely. */
    uint64_t age = (now_ns > r->last_event_ns)
                 ? (now_ns - r->last_event_ns)
                 : 0u;
    if (age >= RATE_EWMA_STALE_NS) return 0;

    return r->rate_per_s_q16;
}
