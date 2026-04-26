/*
 * gpu_consumer.h - Per-consumer GPU enable/disable (admin & telemetry, M2)
 *
 * Three orthogonal toggles let the operator decide which subsystem is
 * allowed to dispatch through the GPU at runtime:
 *
 *   - GPU_CONSUMER_SCHED       — AI scheduler policy inference (CPU vs GPU MLP)
 *   - GPU_CONSUMER_EVICTION    — eviction policy inference (CPU vs GPU MLP/XGB)
 *   - GPU_CONSUMER_INFERENCE   — general model_infer dispatch
 *
 * Default: all OFF. Toggling ON requires (a) a GPU is present and ready
 * AND (b) the active policy / inference engine declares a GPU backend.
 * Until the corresponding consumer in M3+ actually wires a GPU backend,
 * `gpu_consumer_set(c, true)` returns a negative error code with a
 * human-readable reason in `out_reason`. Toggling OFF always succeeds.
 *
 * Storage is `atomic_bool[3]`; reads are lock-free for the dispatch
 * fast path. Writes use a plain `atomic_store_explicit` — concurrent
 * writers race naturally, last-writer-wins. The `last_change_ms`
 * bookkeeping is a separate plain-uint64 store; on 64-bit ARM/x86
 * the hardware store is atomic, so a reader either sees the new
 * toggle + new timestamp or the old toggle + old timestamp.
 */

#ifndef GPU_CONSUMER_H
#define GPU_CONSUMER_H

#include <stdbool.h>
#include <stdint.h>

enum gpu_consumer {
    GPU_CONSUMER_SCHED     = 0,
    GPU_CONSUMER_EVICTION  = 1,
    GPU_CONSUMER_INFERENCE = 2,
    GPU_CONSUMER_COUNT     = 3,
};

/* Stable lowercase name. NULL for out-of-range input. */
const char *gpu_consumer_name(enum gpu_consumer c);

/* Lookup by name (lowercase, exact match). Returns GPU_CONSUMER_COUNT if
 * the name doesn't match any known consumer. */
enum gpu_consumer gpu_consumer_from_name(const char *name);

/* Read the current toggle state. Lock-free; safe from any context. */
bool gpu_consumer_enabled(enum gpu_consumer c);

/* Error codes returned by `gpu_consumer_set` on rejection. Negative
 * for compatibility with the kernel's existing return-int convention.
 * Local rather than pulling POSIX `errno.h` (the kernel is freestanding
 * — see kernel/include/net.h header for the same pattern). */
#define GPU_CONSUMER_ERR_INVAL    (-1)  /* unknown consumer / NULL name */
#define GPU_CONSUMER_ERR_NODEV    (-2)  /* GPU not available on this build */
#define GPU_CONSUMER_ERR_NOTSUPP  (-3)  /* active backend has no GPU path */

/*
 * Set the toggle. Returns 0 on success, one of GPU_CONSUMER_ERR_*
 * on rejection.
 *
 * On rejection, *out_reason (if non-NULL) is set to a const string
 * literal describing the reason. The string is owned by the implementation
 * and remains valid for the lifetime of the kernel — the caller does
 * not free it.
 *
 * Disable (enabled=false) always succeeds.
 *
 * Concurrent setters race naturally on the atomic; the last writer
 * wins. There is no per-consumer lock — the cost of a race is one
 * extra millisecond of `last_change_ms` on the loser.
 */
int gpu_consumer_set(enum gpu_consumer c, bool enabled,
                     const char **out_reason);

/* Wall-clock millisecond timestamp of the most recent successful
 * `gpu_consumer_set`. Returns 0 if the toggle has never been written
 * since boot. */
uint64_t gpu_consumer_last_change_ms(enum gpu_consumer c);

/* Convenience: snapshot all three toggles plus a top-level `gpu_ready`
 * flag (true iff `slm_gpu_available()` returns non-zero) into the
 * provided struct. Cheap; reads atomics + one FFI call. */
struct gpu_consumer_status {
    bool sched;
    bool eviction;
    bool inference;
    bool gpu_ready;
    uint64_t sched_change_ms;
    uint64_t eviction_change_ms;
    uint64_t inference_change_ms;
};

void gpu_consumer_status_get(struct gpu_consumer_status *out);

#endif /* GPU_CONSUMER_H */
