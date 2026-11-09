/*
 * gpu_chan_state.c - Cold GPU channel state for dispatchers (#1171).
 * See gpu_chan_state.h. Pure: built on every platform for the tests.
 */
#include "gpu_chan_state.h"

#include "string.h"

void gpu_chan_init(struct gpu_chan_state *c)
{
    if (c) memset(c, 0, sizeof(*c));
}

enum gpu_chan_gate gpu_chan_gate(struct gpu_chan_state *c, bool end_seen, uint64_t now_ns)
{
    if (!c || c->mode != GPU_CHAN_UP) return GPU_CHAN_DOWN;
    if (!c->flight) return GPU_CHAN_GO;
    if (end_seen) {
        c->flight = false;
        return GPU_CHAN_GO;
    }
    /* Past its deadline the program's owner is about to recover the
     * channel: nothing queued behind it now would ever run. */
    return now_ns >= c->flight_deadline_ns ? GPU_CHAN_DOWN : GPU_CHAN_WAIT;
}

void gpu_chan_flight_start(struct gpu_chan_state *c, uint64_t deadline_ns)
{
    c->flight = true;
    c->flight_deadline_ns = deadline_ns;
}

void gpu_chan_flight_end(struct gpu_chan_state *c)
{
    c->flight = false;
}

bool gpu_chan_recover_begin(struct gpu_chan_state *c)
{
    if (c->mode == GPU_CHAN_RECOVERING) return false;
    c->flight = false;
    c->mode = GPU_CHAN_RECOVERING;
    return true;
}

void gpu_chan_recover_end(struct gpu_chan_state *c, bool ok)
{
    c->mode = ok ? GPU_CHAN_UP : GPU_CHAN_DEAD;
    if (ok) {
        c->epoch++;
    } else {
        c->failed++;
    }
}
