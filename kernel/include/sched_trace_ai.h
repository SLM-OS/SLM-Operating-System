/*
 * sched_trace_ai.h — AI-scheduler decision-trace ring buffer (#880).
 *
 * Captures per-decision state + action tuples (plus per-completion
 * outcome records) so the sibling-repo training pipeline can fine-
 * tune MLP/PPO policies on real SLM-OS scheduling behaviour (rather
 * than only the simulated workloads in `slm_sim/workloads/`).
 *
 * This is DIFFERENT from the existing `kernel/include/sched_trace.h`
 * cross-CPU dispatch visualizer (#195), which records 16-byte
 * context-switch events for ASCII timelines. The AI trace is wider
 * (~480 B per record — full 108-dim FP32 state vector) and is
 * triggered on policy decisions, not context switches.
 *
 * Shell verb: `sched aitrace start/stop/stats/dump` — distinct from
 * `sched trace ...` so neither tracer's UI breaks the other.
 *
 * Wire format documented in `docs/sched-trace-format.md`.
 */

#ifndef KERNEL_SCHED_TRACE_AI_H
#define KERNEL_SCHED_TRACE_AI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* File-header magic for dumped traces. ASCII 'S' 'L' 'T' 'S' →
 * "SLM-OS scheduler Trace, decisionS". Endianness: file is written
 * little-endian (the platforms SLM-OS targets are all LE) but the
 * format spec is byte-defined so cross-arch readers can interpret
 * without endian-flipping. */
#define SCHED_TRACE_AI_MAGIC    0x53544C53u
#define SCHED_TRACE_AI_VERSION  1u

/* Each record is a fixed 480 bytes regardless of kind. Wastes some
 * space on COMPLETION records but lets the ring be a flat array and
 * the file format a stream of equal-size frames the ingester can
 * mmap or seek into without parsing a length prefix per record. */
#define SCHED_TRACE_AI_RECORD_SIZE  480u

/* Ring size in records. 4096 × 480 B = ~1.92 MB. Allocated from
 * PMM at `sched_trace_ai_init()` so the BSS footprint of AI_SCHED
 * builds doesn't grow when the trace is off. */
#define SCHED_TRACE_AI_RING_ENTRIES 4096u

/* AI state-vector dimension. Must match AI_STATE_DIM from ai_types.h.
 * Asserted at init time. */
#define SCHED_TRACE_AI_STATE_DIM    108u

enum sched_trace_ai_kind {
    SCHED_TRACE_AI_KIND_DECISION   = 1,
    SCHED_TRACE_AI_KIND_COMPLETION = 2,
};

/* Fixed-size record. Two variants share a union so every slot in
 * the ring is the same size. `kind` discriminates. Field offsets
 * are pinned by docs/sched-trace-format.md — do NOT reorder without
 * bumping SCHED_TRACE_AI_VERSION. */
struct __attribute__((packed)) sched_trace_ai_record {
    uint8_t  kind;              /* offset 0  — enum sched_trace_ai_kind */
    uint8_t  cpu_recorded;      /* offset 1  — CPU that wrote the record */
    uint16_t _hdr_pad;          /* offset 2 */
    uint32_t task_id;           /* offset 4  — task slot id, matches across DECISION/COMPLETION */
    uint64_t timestamp_ns;      /* offset 8  — slm_get_time_ns() at record time */

    union {
        struct __attribute__((packed)) {
            char     policy_name[16];   /* offset 16, null-padded */
            int32_t  action_core;       /* offset 32, CPU chosen by assign_cpu */
            int32_t  action_priority;   /* offset 36, effective_priority at decision time */
            int32_t  action_preempt;    /* offset 40, reserved for future preempt-action field */
            uint32_t _decision_pad;     /* offset 44 */
            float    state[SCHED_TRACE_AI_STATE_DIM]; /* offset 48..480 — 432 B */
        } decision;

        struct __attribute__((packed)) {
            uint64_t dispatch_ns;       /* offset 16, original DECISION timestamp_ns; always 0 in v1
                                         * (kernel doesn't yet track per-task dispatch separately
                                         * — sibling-repo ingester correlates via task_id). */
            uint64_t completion_ns;     /* offset 24, when task_exit fired */
            uint64_t deadline_ns;       /* offset 32, copy of task->deadline_ns (0 = none) */
            uint32_t latency_to_complete_us; /* offset 40, completion_ns - dispatch_ns */
            uint8_t  ran_on_cpu;        /* offset 44, task->assigned_cpu at exit */
            uint8_t  deadline_met;      /* offset 45, 1 if completion_ns <= deadline */
            uint8_t  _completion_pad[2];/* offset 46 */
            uint8_t  _tail_pad[480 - 48]; /* unused payload (zeroed) */
        } completion;
    } u;
};

_Static_assert(sizeof(struct sched_trace_ai_record) == SCHED_TRACE_AI_RECORD_SIZE,
               "sched_trace_ai_record must be exactly 480 bytes");

/* File header written by `sched_trace_ai_dump_to_buf` and read by
 * the sibling-repo ingester. 32 bytes; followed immediately by
 * `record_count` × SCHED_TRACE_AI_RECORD_SIZE bytes. */
struct __attribute__((packed)) sched_trace_ai_file_header {
    uint32_t magic;             /* SCHED_TRACE_AI_MAGIC */
    uint16_t version;           /* SCHED_TRACE_AI_VERSION */
    uint16_t record_size;       /* SCHED_TRACE_AI_RECORD_SIZE (sanity check) */
    uint32_t record_count;      /* number of records following */
    uint32_t state_dim;         /* SCHED_TRACE_AI_STATE_DIM */
    uint64_t total_events_since_start;   /* unsaturated event counter at dump time */
    uint64_t dropped_events;    /* how many records were overwritten before dump */
};

_Static_assert(sizeof(struct sched_trace_ai_file_header) == 32,
               "file header must be exactly 32 bytes");

/* ---- Initialization ---- */

/* Allocate the ring buffer from PMM. Idempotent. Returns 0 on success,
 * negative on alloc failure. Must be called before any record/start. */
int sched_trace_ai_init(void);

/* ---- Control plane (shell + Lua bindings call these) ---- */

/* Start recording. Resets head/total/dropped counters. Cheap; just
 * flips the atomic enable flag. */
void sched_trace_ai_start(void);

/* Stop recording. Existing records remain in the ring for `dump`. */
void sched_trace_ai_stop(void);

/* Discard all records. Safe to call while tracing is on. */
void sched_trace_ai_clear(void);

/* Is tracing currently enabled? */
bool sched_trace_ai_is_enabled(void);

/* Stats — for `sched aitrace stats`. */
uint32_t sched_trace_ai_records_used(void);    /* min(total_events, ring_size) */
uint64_t sched_trace_ai_total_events(void);    /* monotonic event counter since start */
uint64_t sched_trace_ai_dropped(void);         /* records that wrapped before dump */

/* ---- Hot path (called from scheduler_add_task / task_exit) ---- */

/* Record a DECISION event. Called from `scheduler_add_task` right
 * after `active_policy->assign_cpu(task)` returns. The state vector
 * is extracted internally via `ai_extract_state(task, state)` so
 * the policies don't need to be modified. No-op when tracing is
 * disabled — single atomic load + branch.
 *
 * @task:          The task whose CPU was just chosen.
 * @policy_name:   Active policy's name (string from sched_policy_ops.name).
 * @action_core:   The CPU `assign_cpu` returned.
 *
 * Safe to call from any context. May not be called from inside an
 * IRQ handler (the state extraction touches per-CPU data which the
 * handler may have partially updated).
 */
struct task;
void sched_trace_ai_record_decision(struct task *task,
                                    const char *policy_name,
                                    uint32_t action_core);

/* Record a COMPLETION event. Called from `task_exit` (or wherever a
 * task transitions to TASK_TERMINATED). No-op when tracing is
 * disabled. */
void sched_trace_ai_record_completion(struct task *task);

/* ---- Dump ---- */

/* Serialize the trace into a caller-supplied buffer. Layout:
 *   [sched_trace_ai_file_header][record_0][record_1]...[record_N-1]
 *
 * Returns the number of bytes written, or 0 if buf is too small.
 * Required size: sizeof(file_header) + records_used() * RECORD_SIZE.
 *
 * The dump is taken WHILE tracing may still be active; new records
 * recorded during the dump are not included (snapshot semantics).
 */
size_t sched_trace_ai_dump_to_buf(uint8_t *buf, size_t buflen);

/* Total buffer size required for a full dump at the current fill
 * level. Useful for sizing the destination file or VFS write. */
size_t sched_trace_ai_dump_size(void);

/* Streaming dump — for sinks that can't fit the whole trace (up to
 * ~1.9 MB) in a single buffer. `cb` is invoked one or more times
 * with chunks of bytes that, concatenated in call order, form the
 * same byte sequence `sched_trace_ai_dump_to_buf` would produce.
 *
 * Returns the total bytes streamed, or 0 if `cb` returned negative
 * at any chunk. `cb` should return 0 on success and a negative
 * errno-ish code to abort. */
typedef int (*sched_trace_ai_dump_cb)(void *ctx, const void *bytes, size_t len);
size_t sched_trace_ai_dump_stream(sched_trace_ai_dump_cb cb, void *ctx);

/* Convenience: dump the trace to a VFS path. Returns number of bytes
 * written on success, 0 on failure (mount not found, open failed,
 * write failed). Used by both the shell `sched aitrace dump` verb
 * and the `slm.sched_aitrace_dump` Lua binding. */
size_t sched_trace_ai_dump_to_path(const char *path);

#endif /* KERNEL_SCHED_TRACE_AI_H */
