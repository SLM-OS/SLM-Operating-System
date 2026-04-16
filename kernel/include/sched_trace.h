/*
 * sched_trace.h - Lightweight scheduler trace buffer (#195)
 *
 * A circular buffer of scheduler events (context switches, migrations,
 * wakes) used by the `sched trace` shell command to visualize cross-CPU
 * dispatch behavior. When tracing is disabled, each hook is a single
 * atomic load guarded branch — zero cost when not in use.
 *
 * Buffer is sized for 4096 events; older events are overwritten
 * silently. Tracing must be explicitly enabled via
 * `sched_trace_start()` before events are recorded.
 */

#ifndef SCHED_TRACE_H
#define SCHED_TRACE_H

#include <stdint.h>
#include <stdbool.h>

/* Max events in the circular buffer. Power of two so modulo reduces
 * to an AND. 4096 events × 16 bytes = 64 KB. */
#define SCHED_TRACE_CAPACITY 4096u

/* Event type codes. Kept as a compact set so the recorded byte stays
 * useful for filtering without bloating the buffer. */
enum sched_trace_event {
    SCHED_TRACE_SCHED   = 1,  /* Context switch: prev → next on this CPU */
    SCHED_TRACE_MIGRATE = 2,  /* Task moved from prev_cpu to this_cpu */
    SCHED_TRACE_WAKE    = 3,  /* Task transitioned into READY */
    SCHED_TRACE_PREEMPT = 4,  /* Timer/IPI preempted current task */
};

struct sched_trace_record {
    uint64_t timestamp_ns;   /* slm_get_time_ns() at record time */
    uint16_t prev_task_id;   /* Previous task id, or 0xFFFF if none */
    uint16_t next_task_id;   /* Incoming task id */
    uint8_t  cpu;            /* CPU where the event was recorded */
    uint8_t  prev_cpu;       /* MIGRATE source CPU, else same as cpu */
    uint8_t  event;          /* enum sched_trace_event */
    uint8_t  _pad;
};

/* Start recording events. Resets the buffer head. Cheap — just flips
 * the atomic flag and zeros the write cursor. */
void sched_trace_start(void);

/* Stop recording. Existing events remain in the buffer. */
void sched_trace_stop(void);

/* Discard all recorded events. Safe to call while tracing is on. */
void sched_trace_clear(void);

/* Is tracing currently enabled? Used by the shell command to print
 * the status line. */
bool sched_trace_is_enabled(void);

/* Record a context-switch event. prev_task may be NULL on first
 * schedule; next_task must be non-NULL. Safe to call from any context
 * — purely atomic; no locks. No-op when tracing is disabled. */
struct task;
void sched_trace_record_switch(uint32_t cpu, struct task *prev,
                                struct task *next);

/* Record a migration event. No-op when tracing is disabled. */
void sched_trace_record_migrate(struct task *task, uint32_t from_cpu,
                                 uint32_t to_cpu);

/* Snapshot the buffer contents.
 *
 * Copies up to `max` events into `out`, starting from the oldest
 * retained event. Returns the number of events copied. When the
 * buffer has wrapped, only the most recent SCHED_TRACE_CAPACITY
 * events are available. */
uint32_t sched_trace_snapshot(struct sched_trace_record *out, uint32_t max);

/* Total events recorded since start or clear (monotonic, saturating
 * at UINT64_MAX). Useful for the "dropped" count — if total exceeds
 * SCHED_TRACE_CAPACITY, older events were overwritten. */
uint64_t sched_trace_total_events(void);

#endif /* SCHED_TRACE_H */
