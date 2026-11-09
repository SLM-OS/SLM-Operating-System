/*
 * gpu_chan_state.h - Whether a dispatcher may use the cold GPU channel
 * (#1171). Pure state, no locking of its own: slm_ffi.c keeps one
 * instance and calls every function below with g_gpu_dispatch_lock held.
 *
 * The channel is UP, RECOVERING (a step timed out and ga10b_cold_recover
 * owns the hardware) or DEAD (recovery failed; consumers stay on the CPU
 * until a reboot or a manual `nvgpu cold recover`). On top of UP, a GPU
 * program can be in flight: submitted under the lock, waited for without
 * it, until its owner calls finished or starts a recovery.
 *
 * A dispatcher asks gpu_chan_gate:
 *   GO    take the channel (the lock stays held);
 *   WAIT  a program is still running: drop the lock and try again;
 *   DOWN  don't touch the channel: drop the lock, take the CPU fallback.
 * A program past its deadline is DOWN, not GO. Before #1171 an expired
 * flight let the dispatcher in, which queued its work behind the hung
 * program, where it timed out too (2 s with IRQs masked each).
 */
#ifndef GPU_CHAN_STATE_H
#define GPU_CHAN_STATE_H

#include <stdbool.h>
#include <stdint.h>

enum gpu_chan_mode {
    GPU_CHAN_UP = 0,
    GPU_CHAN_RECOVERING = 1,
    GPU_CHAN_DEAD = 2,
};

enum gpu_chan_gate {
    GPU_CHAN_GO = 0,
    GPU_CHAN_WAIT = 1,
    GPU_CHAN_DOWN = 2,
};

struct gpu_chan_state {
    uint32_t mode;              /* enum gpu_chan_mode */
    bool flight;                /* a GPU program owns the channel */
    uint64_t flight_deadline_ns;
    uint32_t epoch;             /* recoveries that brought the channel back */
    uint32_t failed;            /* recoveries that left it DEAD */
};

void gpu_chan_init(struct gpu_chan_state *c);

/* `end_seen`: the in-flight program's end semaphore holds its payload
 * (the caller reads it; meaningless without a flight). A finished
 * program is retired here, by whichever CPU sees it first. */
enum gpu_chan_gate gpu_chan_gate(struct gpu_chan_state *c, bool end_seen, uint64_t now_ns);

void gpu_chan_flight_start(struct gpu_chan_state *c, uint64_t deadline_ns);
void gpu_chan_flight_end(struct gpu_chan_state *c);

/* Start a recovery: false if one is already running. Ends the flight
 * (the program is the reason) and makes every gate DOWN. */
bool gpu_chan_recover_begin(struct gpu_chan_state *c);
/* UP and a new epoch if `ok`, else DEAD. */
void gpu_chan_recover_end(struct gpu_chan_state *c, bool ok);

#endif /* GPU_CHAN_STATE_H */
