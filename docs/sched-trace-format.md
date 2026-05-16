# AI Scheduler Decision-Trace Format (#880)

The AI scheduler decision-trace ring buffer captures `(state, action)`
tuples (plus per-task completion outcomes) so the sibling-repo training
pipeline can fine-tune MLP/PPO policies on real SLM-OS scheduling
behaviour rather than only the simulated workloads in
`~/projects/slm-os-scheduler-ai/slm_sim/workloads/`.

This document pins the on-disk byte layout. **Any change to field
offsets requires bumping `version` in the file header.** The sibling-
repo ingester (`data/slmos_traces.py`, added in #879 / 61c) parses
the bytes per this spec.

Distinct from the cross-CPU dispatch tracer documented in
`docs/scheduler.md` §"Trace" (`sched_trace.h`) — that one records
16-byte context-switch events for ASCII timelines; this one records
480-byte AI-decision frames.

## Endianness

All multi-byte fields are little-endian on disk. SLM-OS targets are
all LE platforms (ARM64 LE, x86-64). The ingester reads as LE
unconditionally.

## File header (32 bytes, fixed)

| Offset | Size | Type    | Field                       | Description                                                  |
|-------:|-----:|---------|-----------------------------|--------------------------------------------------------------|
|  0     | 4    | u32     | `magic`                     | `0x53544C53` (ASCII `'S' 'L' 'T' 'S'`)                       |
|  4     | 2    | u16     | `version`                   | Currently `1`. Bumped on any layout change.                  |
|  6     | 2    | u16     | `record_size`               | Always `480`. Sanity-check; ingester aborts on mismatch.     |
|  8     | 4    | u32     | `record_count`              | Number of records that follow.                               |
| 12     | 4    | u32     | `state_dim`                 | AI state-vector dimension. Currently `108`.                  |
| 16     | 8    | u64     | `total_events_since_start`  | Monotonic counter at dump time (≥ `record_count`).           |
| 24     | 8    | u64     | `dropped_events`            | Records that wrapped before dump. `total - record_count`.    |

## Record (480 bytes, fixed)

Every record is `480` bytes regardless of `kind`. The two variants
share a union; the unused part is zero-padded. Fixed-size records
let the ingester `mmap` and seek by index.

### Common header (16 bytes)

| Offset | Size | Type   | Field          | Description                                           |
|-------:|-----:|--------|----------------|-------------------------------------------------------|
|  0     | 1    | u8     | `kind`         | `1` = DECISION, `2` = COMPLETION.                     |
|  1     | 1    | u8     | `cpu_recorded` | CPU id (`cpu_id()`) that wrote this record.           |
|  2     | 2    | u16    | `_hdr_pad`     | Zero. Reserved for future flags.                      |
|  4     | 4    | u32    | `task_id`      | `struct task::id`. Joins DECISION and COMPLETION pairs. |
|  8     | 8    | u64    | `timestamp_ns` | `slm_get_time_ns()` at record time.                   |

### DECISION variant (kind = 1)

Written from `scheduler_add_task` right after `active_policy->assign_cpu`
returns. The state vector is the same one the policy's inference
function operates on (extracted via `ai_extract_state`).

| Offset | Size | Type    | Field             | Description                                          |
|-------:|-----:|---------|-------------------|------------------------------------------------------|
| 16     | 16   | char[16]| `policy_name`     | Null-padded ASCII; policy that made the decision.    |
| 32     | 4    | i32     | `action_core`     | CPU id `assign_cpu` returned. Pre-S5-override.       |
| 36     | 4    | i32     | `action_priority` | `task->effective_priority` at decision time.         |
| 40     | 4    | i32     | `action_preempt`  | Reserved (currently always `0`).                     |
| 44     | 4    | u32     | `_decision_pad`   | Zero.                                                |
| 48     | 432  | f32[108]| `state`           | 108-dim FP32 state vector (`ai_extract_state` output)|

Total: 16 + 16 + 4×4 + 432 = 480 bytes. ✓

`action_core` is the **pre-S5-override** value — the CPU the active
policy's `assign_cpu` returned, BEFORE `scheduler_add_task`'s proactive
load-balance override (see `kernel/sched/sched.c` near
`scheduler_add_task`) potentially redirects the task elsewhere. The
trace is meant for training the policy that made the choice, so
recording the policy's actual output (not the post-override target)
is what the ingester wants. The override outcome is recoverable
indirectly from the next decision's state vector and from the
matching COMPLETION's `ran_on_cpu`. Do not try to use `action_core`
as ground truth for "where the task ultimately ran".

The state vector layout matches the sibling-repo simulator's
observation space:

- `state[0..35]`   — per-core features (6 cores × 6 features)
- `state[36..99]`  — per-task features (8 tasks × 8 features)
- `state[100..107]`— global features

See `kernel/sched/ai/ai_state.c::ai_extract_state` for the canonical
field ordering. The sibling-repo `slm_sim/observation.py` describes
the same layout for its synthetic data.

### COMPLETION variant (kind = 2)

Written from `task_exit`. Links back to its matching DECISION via
`task_id` (each task has at most one DECISION + one COMPLETION
within a single trace window).

| Offset | Size | Type    | Field                    | Description                                           |
|-------:|-----:|---------|--------------------------|-------------------------------------------------------|
| 16     | 8    | u64     | `dispatch_ns`            | `timestamp_ns` of the matching DECISION (0 if unknown)|
| 24     | 8    | u64     | `completion_ns`          | `slm_get_time_ns()` at `task_exit`.                   |
| 32     | 8    | u64     | `deadline_ns`            | `task->deadline_ns` at exit (0 = no deadline).        |
| 40     | 4    | u32     | `latency_to_complete_us` | `(completion_ns - dispatch_ns) / 1000`, or 0.         |
| 44     | 1    | u8      | `ran_on_cpu`             | `task->assigned_cpu` at exit.                         |
| 45     | 1    | u8      | `deadline_met`           | 1 iff `completion_ns <= deadline_ns` (or no deadline).|
| 46     | 2    | u8[2]   | `_completion_pad`        | Zero.                                                 |
| 48     | 432  | u8[432] | `_tail_pad`              | Zero.                                                 |

Total: 16 + 32 + 432 = 480 bytes. ✓

The `dispatch_ns` field is `0` in v1 — the kernel doesn't currently
track per-task dispatch timestamps separately from the existing
trace ring. The ingester can correlate DECISION ↔ COMPLETION by
`task_id` if precise latency is needed. A future version may
populate this field directly.

## Ordering and merging

Records are written into per-CPU ring buffers. The dump streams
each per-CPU ring in oldest-to-newest order, concatenated by CPU id
(`cpu_recorded` ascending). Within a CPU, records are in monotonic
`timestamp_ns` order; across CPUs, the ingester merge-sorts on
`timestamp_ns` if a strict global order is needed.

`dropped_events > 0` indicates the ring wrapped during capture. The
oldest events are the ones that were overwritten, so the surviving
records skew toward the end of the capture window.

## Capture overhead

Targets ≤5% overhead per `scheduler_add_task` (per #880 acceptance).
At AI MLP decision-latency of ~42 µs on Pi 5, the budget is ~2 µs
per decision — comfortably above the trace-write cost (one
`ai_extract_state` call + memcpy + `cache_clean_range`).

Measure with `bench sched-policy --workload mixed --all` (#882) with
`sched aitrace stop` vs `sched aitrace start` — the table's `Tasks/s`
and `p50` columns are the relevant comparison points.

## Shell control

```
sched aitrace start              # clear ring, start capturing
sched aitrace stop               # stop capturing, print stats
sched aitrace stats              # show fill / dropped counters
sched aitrace clear              # drop all records (safe while ON)
sched aitrace dump <path>        # stream the trace into a file
```

For example:

```
slmos> sched aitrace start
slmos> bench sched-policy --workload mixed --all
slmos> sched aitrace stop
slmos> sched aitrace dump /mnt/files/sched_trace.bin
```

## Lua control

```lua
slm.sched_aitrace_start()
slm.sched_aitrace_stop()
slm.sched_aitrace_clear()
local stats = slm.sched_aitrace_stats()  -- table: enabled, used, total, dropped, dump_size
local ok = slm.sched_aitrace_dump("/mnt/files/sched_trace.bin")
```

## Sibling-repo ingester (#879)

Located at `~/projects/slm-os-scheduler-ai/data/slmos_traces.py`
(added in sub-ticket #879 / 61c). The ingester:

1. Reads the file header, validates `magic`, `version`,
   `record_size`, `state_dim`.
2. Walks `record_count` records as a NumPy structured array.
3. Filters by `kind`; DECISION records become `(state, action)`
   training pairs; COMPLETION records contribute to the reward
   signal via the simulator's existing `reward.py`.
4. Emits a Parquet file in the same shape as the simulator output
   so the existing `_train_mlp.py` / `_train_ppo.py` pipelines can
   consume it via `--source slmos-traces`.

## Version history

| Version | Date       | Change                                                |
|--------:|------------|-------------------------------------------------------|
|       1 | 2026-05-16 | Initial format (DECISION + COMPLETION, 480 B records) |
