# Spec: Admin & Telemetry Suite

**Status:** Draft, 2026-04-25
**Owner:** TBD
**Scope:** Make the Jetson GA10B GPU operationally useful from a live SLM-OS shell, with per-consumer enable/disable, hot-swap of scheduler and eviction policies, runtime model upload + launch, a streaming telemetry feed, and a Lua admin/monitor tool that ties it all together.

This spec is **additive**. It extends existing substrate (policy vtable, eviction trait, msg_router, xput, Lua bindings) rather than re-inventing it. Re-uses are called out explicitly so reviewers can verify nothing is being duplicated.

---

## 1. Goals

1. GPU inference can be enabled or disabled **per consumer** at runtime: scheduler, eviction, general model inference.
2. Lua exposes the inference rate (calls/s, p50/p99 latency, fallback rate) for each of the three consumers.
3. Operators can read per-decision latency for context switches, eviction victim selection, and model inference, with bucketed histograms (not just totals).
4. Scheduler and eviction policies can be hot-swapped at runtime, including switching the backend (CPU vs GPU) of any policy that supports both.
5. New models can be uploaded over telnet, registered against a name, and launched without reboot.
6. A live streaming telemetry feed publishes every dynamic statistic the system collects.
7. A Lua admin/monitor TUI wraps all of this in something a human can drive over a 80×24 telnet session.

## 2. Non-goals

- New GPU bringup work. The GA10B inherit-from-Linux path is the substrate; if `nvgpu inherit` succeeds the GPU is usable, otherwise the suite degrades to CPU-only.
- Persistent model store on eMMC/flash. Models live in `/mnt/files/` (LittleFS, RAM-backed). Persistence is tracked separately in `dynamic-kernel-replace-plan.md`.
- Multi-tenant policy isolation. There is one global scheduler and one global eviction policy at a time.
- Auth on the telnet admin surface. This is a lab tool; trust boundary is the network. Documented in §11.

## 3. Existing substrate (must re-use)

| Surface | Location | What's there today |
|---|---|---|
| Scheduler policy vtable | `kernel/include/sched_policy.h:36-74` | `struct sched_policy_ops` with `assign_cpu(task) → cpu_id`. Runtime registry, up to 8 policies. `sched_set_policy()` swaps at runtime. |
| Compiled-in policies | `kernel/sched/sched_heuristic.c`, `kernel/sched/ai/sched_ai.c:250,304,594` | `heuristic`, `ai_mlp`, `ai_ppo`, `ai_hailo` |
| Scheduler stats | `kernel/sched/ai/sched_ai.c:30-35` | per-policy `ai_policy_stats` { decisions, fallbacks, total_latency_ns, action_hist[N] } |
| Scheduler trace ring | `kernel/include/sched_trace.h:22-40`, `kernel/src/sched_trace.c` | 4096-entry circular buffer of context switch / migrate / wake / preempt events |
| Eviction policy trait | `runtime/src/mm/eviction/policy.rs:53-79` | `EvictionPolicy::select_victim(&[BlockMeta]) → usize`, `score()`, `update_feedback()`, `name()` |
| Eviction policies | `runtime/src/mm/eviction/` | LRU, LFU, MRU, FIFO, ARC, XGBoost, MLP, CACHEUS |
| Dynamic blob format | `docs/specs/runtime-blob-formats.md` | `SEMB` outer wrapper, FNV-1a checksum, schema versioned |
| Policy/model loading | `docs/dynamic-policy-model-loading-plan.md` | `sched model load/activate/rollback/clear`, `eviction model load/activate/...` (eviction phases E1-E5 ✅, scheduler S1-S3 partial). Lua side has `slm.sched_model_*`, `slm.eviction_model_*`. |
| File upload | `kernel/src/shell_fs.c:568-760` | `xput begin/chunk/finish/abort`, hex over shell, FNV-1a, writes to LittleFS at `/mnt/files/`. 512-byte chunks. |
| Lua bindings | `kernel/src/lua_slm.c` | 40+ bindings under `slm.*`. Per-session `lua_State *`. Safe vs admin tables. |
| Lua REPL | `kernel/src/lua_shell.c` | `lua`, `lua-admin`, `lua -e`, `lua <path>`. Per-session persistent state. |
| Pub/sub bus | message router (existing); shell `msg publish/subscribe`, Lua `slm.msg_publish/msg_subscribe/msg_drain` | string topics, table payloads |
| Time | `slm.uptime()`, `slm.uptime_us()` (lua_slm.c:87-118) | ms / µs from `timer_get_count()` |
| GPU shell | `kernel/src/shell_sys.c:2641,2703` | `gpu`, `nvgpu prepare/run/info/inherit/channel/submit/launch-kernel/run-mnist` |
| GPU Lua | `kernel/src/lua_slm.c:683,716,743,2638` | `slm.gpu_status`, `slm.gpu_run_mnist`, `slm.gpu_set_mnist_input*` |
| Inference stats | `slm.infer_stats()` (lua_slm.c:2616-2625) | `{ total_time_ns, min_time_ns, max_time_ns, last_time_ns, count }` per model |

The work in this spec sits **on top of** these surfaces. New code is the glue — toggles, rate counters, telemetry sink, admin TUI. Heavy mechanism (policy registries, blob staging, GPU dispatch) already exists.

## 4. Architecture

```
+----------------------------------------------------------+
|                Lua admin TUI (admin.lua)                 |
|   pages: overview / tasks / sched / eviction / models /  |
|          telemetry / repl                                |
+--------------+------------+------------+-----------------+
               |            |            |
       slm.gpu_use_*   slm.sched_*   slm.telemetry_*
       slm.eviction_*  slm.model_*   slm.latency_*
               |            |            |
+--------------v------------v------------v-----------------+
|                  C/Rust kernel surface                    |
|                                                           |
|  gpu_consumer flags  policy registries  msg_router bus    |
|        |                  |                  ^            |
|        v                  v                  |            |
|   sched assign_cpu() ----+ records latency --+            |
|   eviction select_victim() ---- records latency ---+      |
|   model_infer() ---------------- records latency --+      |
|        |                                                  |
|        +-- if use_gpu && policy.has_gpu_backend --> GPU   |
|        +-- else --> CPU policy                            |
+----------------------------------------------------------+
```

Three orthogonal facets:
- **Toggle** — three booleans live behind a small API; consumers consult them on every decision.
- **Latency** — every decision wraps a `latency_hist_record(start, end)` call; histograms live next to the existing per-policy stat structs.
- **Telemetry** — the same hook that records latency also publishes a sample to `/telemetry/<consumer>/<metric>`. The msg_router does the fanout. Subscribers (admin TUI, Lua scripts, future log forwarders) attach by topic pattern.

## 5. New shell commands

```
gpu use sched on|off
gpu use eviction on|off
gpu use inference on|off
gpu use status                            # tabular summary

sched policy <name> [--backend cpu|gpu]   # extend existing command
eviction policy <name> [--backend cpu|gpu]

model upload <name> [--kind ggml|hailo|mnist|raw]
                                          # opens an xput session bound to /mnt/models/<name>.blob,
                                          # writes a sidecar /mnt/models/<name>.meta on finish
model list                                # name, kind, size, sha256, ts, status
model launch <name> [--task-name N] [--args "<json>"]
                                          # returns task id; emits /telemetry/model/<name>/launched
model unload <name>                       # ref-checked; refuses if running

stats sched [--reset]
stats eviction [--reset]
stats inference [--reset]
stats latency <consumer> [--window-ms N]  # rendered histogram

telemetry subscribe <topic-pattern>       # blocks, streams to current shell; ^C to stop
telemetry list-topics
telemetry stats                           # publishers, subscribers, drops

admin                                     # launches the Lua admin TUI
```

`gpu use` is the new top-level. Other commands extend existing surfaces (`sched policy`, `eviction policy`, `model`, `stats`, `telemetry`).

## 6. New Lua bindings

All under the `slm.*` namespace. Safe-table additions are read-only; admin-table additions can mutate state.

### 6.1 GPU consumer toggle

```lua
slm.gpu_use_set(consumer, enabled)        -- "sched"|"eviction"|"inference", bool. returns ok, err
slm.gpu_use_get(consumer)                 -- returns bool
slm.gpu_use_status()                      -- {sched=bool, eviction=bool, inference=bool,
                                          --  gpu_ready=bool, last_change_ms=int}
```

`gpu_use_set("sched", true)` returns `(false, "policy 'heuristic' has no gpu backend")` if the active policy has `has_gpu_backend == false`. Toggle is rejected, not silently ignored.

### 6.2 Rates and latencies

```lua
slm.sched_decision_rate()       -- {decisions_per_s, fallback_rate,
                                --  p50_latency_ns, p90_latency_ns, p99_latency_ns,
                                --  total_decisions, total_fallbacks, total_ns}
slm.eviction_decision_rate()    -- same shape
slm.inference_rate()            -- {[model_name] = {calls_per_s, p50_ns, p99_ns, errors_per_s, last_ts_ms}, ...}

slm.latency_histogram(consumer) -- {buckets={[low_ns]=count, ...}, total, min_ns, max_ns,
                                --  p50_ns, p90_ns, p99_ns}
```

Rates are computed over a 1 s sliding window via per-consumer EWMA stored in the kernel; Lua side does no math.

### 6.3 Policies

```lua
slm.sched_policy_list()         -- {{name="heuristic", has_gpu_backend=false, active=true}, ...}
slm.sched_policy_set(name, opts) -- opts.backend = "cpu"|"gpu"; returns ok, err
slm.eviction_policy_list()
slm.eviction_policy_set(name, opts)
```

`*_set` is the same as the existing `slm.sched_set_policy` / `slm.eviction_set_policy` but accepts the `opts` table. The old single-arg form keeps working.

### 6.4 Models

```lua
-- Upload (mirror of xput; also captures kind metadata)
slm.model_upload_begin(name, expected_size, kind)
slm.model_upload_chunk(name, offset, hex)
slm.model_upload_finish(name, sha256_hex)
slm.model_upload_abort(name)

slm.model_list()                -- {{name, kind, size, sha256, ts_loaded_ms, status}, ...}
slm.model_launch(name, args)    -- args is a Lua table; serialized to JSON for the engine
                                -- returns {task_id=N} or (nil, err)
slm.model_unload(name)
```

### 6.5 Telemetry

```lua
slm.telemetry_subscribe(pattern, fn)   -- pattern matches /telemetry/<consumer>/<metric>;
                                       -- fn(topic, sample_table) called from the consumer-shell context.
                                       -- returns subscription id.
slm.telemetry_unsubscribe(id)
slm.telemetry_publish(topic, table)    -- script-side metrics emit
slm.telemetry_list_topics()            -- {{topic, publishers, subscribers, samples_total, drops_total}, ...}
slm.telemetry_stats()                  -- aggregate counts
```

Pattern is a glob: `/telemetry/sched/*`, `/telemetry/inference/mnist/*`. Backed by msg_router's existing pub/sub.

## 7. Telemetry feed

### 7.1 Topic schema

| Topic | When | Sample fields |
|---|---|---|
| `/telemetry/sched/decision` | every assign_cpu | `{ts_ns, policy, action, latency_ns, fallback (bool), backend ("cpu"|"gpu")}` |
| `/telemetry/sched/aggregate/1s` | 1 Hz | `{ts_ns, decisions, fallbacks, p50_ns, p99_ns}` |
| `/telemetry/sched/policy_swap` | on swap | `{ts_ns, from, to, by ("shell"|"lua"|"admin")}` |
| `/telemetry/eviction/victim` | per eviction | `{ts_ns, policy, victim_block_id, candidates, latency_ns, backend}` |
| `/telemetry/eviction/aggregate/1s` | 1 Hz | `{ts_ns, evictions, p50_ns, p99_ns, hit_rate, fault_rate}` |
| `/telemetry/eviction/policy_swap` | on swap | same shape as sched policy_swap |
| `/telemetry/inference/<model>/result` | per call | `{ts_ns, model, latency_ns, ok (bool), backend}` |
| `/telemetry/inference/<model>/aggregate/1s` | 1 Hz | `{ts_ns, calls, errors, p50_ns, p99_ns}` |
| `/telemetry/model/<name>/status` | on lifecycle | `{ts_ns, name, status ("uploaded"|"launched"|"running"|"unloaded"|"error"), detail}` |
| `/telemetry/gpu/use_changed` | on toggle | `{ts_ns, consumer, enabled, by}` |
| `/telemetry/system/heartbeat` | 1 Hz | `{ts_ns, uptime_ms, free_kb, cpu_load[]}` |

### 7.2 Back-pressure

The msg_router has a finite queue per subscriber. Spec rule: **drop oldest** sample when full, increment a per-subscription `drops_total` counter, surface that counter in `slm.telemetry_stats()` and `telemetry stats`. Slow subscribers do not block publishers.

### 7.3 Aggregation

High-rate consumers (`sched/decision` can fire >10 kHz under load) publish raw on every decision **only if any subscriber is attached to the raw topic**. The 1 Hz aggregate topic is always emitted. This avoids spending cycles on a fanout no one listens to.

## 8. Latency & rate measurement

### 8.1 New header

`kernel/include/latency_hist.h`:

```c
struct latency_hist {
    uint64_t buckets[32];   // log2 buckets: bucket i covers [2^(i+5), 2^(i+6)) ns,
                            // bucket 0 = [32 ns, 64 ns), bucket 31 = [~17 s, ~34 s)
    uint64_t count;
    uint64_t sum_ns;
    uint64_t min_ns;
    uint64_t max_ns;
};

void latency_hist_record(struct latency_hist *h, uint64_t ns);
void latency_hist_reset(struct latency_hist *h);
uint64_t latency_hist_pct(const struct latency_hist *h, uint32_t pct);  /* 50, 90, 99 */
```

Lock-free single-writer (one consumer site per histogram); reader uses snapshot copy.

### 8.2 Per-consumer EWMA rates

`struct rate_ewma { uint64_t last_ts_ns; double rate_per_s; double alpha; }`. Updated on each event by `rate_ewma_tick(r, now_ns)`. Lua reads computed value directly.

### 8.3 Wiring

| Site | What |
|---|---|
| `kernel/sched/ai/sched_ai.c:73-81` | already measures latency into `total_latency_ns`. Add `latency_hist_record(&policy->latency_hist, dt)` and `rate_ewma_tick(&policy->decision_rate, t1)`. Emit `/telemetry/sched/decision` if subscribed. |
| `runtime/src/mm/eviction/registry.rs` | add `latency_hist` and `rate_ewma` to `PolicyCounters`. Wrap `select_victim` call in registry. Emit `/telemetry/eviction/victim`. |
| `kernel/src/lua_slm.c` model_infer | wrap `slm_model_infer` with the same. Emit `/telemetry/inference/<model>/result`. |
| `kernel/src/sched_trace.c` aggregator task | new 1 Hz task publishes `/telemetry/sched/aggregate/1s` and `/telemetry/system/heartbeat`. |

## 9. GPU consumer toggle

### 9.1 State

`kernel/include/gpu_consumer.h`:

```c
enum gpu_consumer { GPU_CONSUMER_SCHED, GPU_CONSUMER_EVICTION, GPU_CONSUMER_INFERENCE, GPU_CONSUMER_COUNT };

bool gpu_consumer_enabled(enum gpu_consumer c);
int  gpu_consumer_set(enum gpu_consumer c, bool enabled);  /* returns 0 or -EOPNOTSUPP */
```

Backed by an `atomic_bool[3]`. Default: all OFF. Toggle ON requires `gpu_ready()` AND active policy declares `has_gpu_backend == true`.

### 9.2 Decision sites

```c
/* sched_ai.c::ai_assign_cpu_common */
bool use_gpu = gpu_consumer_enabled(GPU_CONSUMER_SCHED) && policy->ops->has_gpu_backend;
int action = use_gpu ? policy->ops->infer_gpu(state) : policy->ops->infer_cpu(state);
```

Same shape for eviction `select_victim` and `model_infer`.

### 9.3 Backend declaration

`struct sched_policy_ops` gains:

```c
bool has_gpu_backend;
int  (*infer_cpu)(const float *state, int dim);
int  (*infer_gpu)(const float *state, int dim);   /* may be NULL when has_gpu_backend == false */
```

The existing `assign_cpu` becomes a thin wrapper that picks the right backend per the toggle. Heuristic policy keeps `has_gpu_backend = false`.

## 10. Model upload + launch

### 10.1 Upload

```
model upload <name> --kind <kind>
  → opens xput session pointed at /mnt/models/<name>.blob
  → on xput finish: writes /mnt/models/<name>.meta with {kind, size, sha256, uploaded_ts_ms}
  → publishes /telemetry/model/<name>/status {status="uploaded", ...}
```

`xput` itself doesn't change. The `model upload` shell command initializes the xput session and arms a finish-hook that writes the sidecar.

### 10.2 Launch

```
model launch <name> [--task-name N] [--args "<json>"]
  → reads /mnt/models/<name>.meta
  → looks up engine for kind (ggml/hailo/mnist/raw)
  → engine instantiates: returns {task_id} or err
  → publishes /telemetry/model/<name>/status {status="launched", task_id}
```

Engine registry is a small table in `kernel/src/model_engine.c`; each kind has `(load, run, unload)` function pointers. `mnist` already exists; `hailo` and `ggml` are stubs that return ENOSYS until those engines land. `raw` is "load and report; do not run" — useful for shipping inference weights to a policy hot-swap target.

### 10.3 Unload

`model unload <name>` is allowed only if no task references the loaded model. Otherwise returns `EBUSY` and prints the task ids that still hold a reference.

## 11. Lua admin TUI tool

### 11.1 Location and lifecycle

- Source: `runtime/lua/admin.lua` (new directory, mirrors how demo scripts are structured).
- Embedded into the kernel via `.incbin` in `kernel/src/admin_script_embed.S`. Written to `/scripts/admin.lua` by `kernel/src/demo_init.c` at boot, alongside the existing demo scripts.
- Launched by the new `admin` shell command, which is a 4-line C wrapper that calls `lua /scripts/admin.lua`.

### 11.2 UX

80×24 baseline; uses `slm.term_size()` to grow.

```
+--+ SLM-OS Admin -- jetson-nano-2 -- 12:34:05 +-------- 1.2 KB/s -+
| 1 Overview   2 Tasks   3 Sched   4 Eviction   5 Models   6 Tele  |
+------------------------------------------------------------------+
|                                                                  |
|  (page body)                                                     |
|                                                                  |
+------------------------------------------------------------------+
| q quit  r refresh  / search  ? help              [page-specific] |
+------------------------------------------------------------------+
```

Page bodies:

- **1 Overview** — uptime, free/used mem, sched policy, eviction policy, GPU consumer table, IP, telnet sessions, last 3 telemetry samples.
- **2 Tasks** — live task list (id, name, state, cpu, prio). Keys: `k <id>` kill, `m <id> <cpu>` migrate, `p <id> <prio>` set priority.
- **3 Scheduler** — current policy + backend, available policies, decision rate (calls/s + p50/p99), latency histogram (sparkline). Keys: `s <name>` swap policy, `b cpu|gpu` swap backend, `c` clear stats.
- **4 Eviction** — same shape as Scheduler page.
- **5 Models** — registered models with size/sha256/status. Keys: `l <name> [args]` launch, `u <name>` unload, `up <name> <local-path>` upload (drives `xput` chunked from local file via shell-side helper).
- **6 Telemetry** — live tail with pattern filter; `f <pattern>` set filter; samples render compact JSON.
- **7 REPL** — drops into raw lua REPL; `:q` returns to TUI.

### 11.3 Refresh model

Per-page background timer at 1 Hz (configurable per page; telemetry tail is event-driven). Render uses double-buffer — full repaint per tick, no incremental cursor games. At 80×24 this is ~2 KB/s of telnet traffic; well within budget.

### 11.4 Local upload helper

Uploading a real model byte-for-byte over a hex-encoded `xput chunk` is slow over a TCP shell. The helper is `runtime/scripts/admin/slm-model-upload.py` (new). It opens a TCP socket to port 2323, drives `model upload` + chunked `xput chunk` from a local file, verifies sha256, calls `xput finish`. The Lua TUI's `up <name> <path>` page shows the command to run host-side; the upload itself happens out-of-band over the same telnet port.

This is the same pattern as `scripts/slm-put.py` (from the dynamic-policy plan, F2 partial). Reuse it if possible; extend if the metadata sidecar isn't supported.

## 12. Milestones

- **M1** — `latency_hist` + `rate_ewma` headers; wire scheduler decision site; expose via `slm.sched_decision_rate()`, `slm.latency_histogram("sched")`. Test: QEMU bench shows non-zero p50/p99.
- **M2** — `gpu_consumer` toggle, `slm.gpu_use_*`, `gpu use` shell command. Test: toggle is rejected for heuristic policy.
- **M3** — Same pattern for eviction (Rust + Lua glue) and inference (lua_slm.c wrapper).
- **M4** — Telemetry feed: topic emission at decision sites, `slm.telemetry_*`, `telemetry` shell command. Test: subscribing to `/telemetry/sched/*` from a second telnet session shows samples in real time.
- **M5** — Model upload + launch shell + Lua. Test: `slm-model-upload.py` pushes a 1 MB blob, sha256 verifies, `model launch` returns a task id.
- **M6** — `admin.lua` TUI, embedded, launched by `admin`. Test: 7 pages render at 80×24 over telnet, refresh stable for 10 min.
- **M7** — `docs/demo-admin.md` walkthrough; one paragraph per page, screenshot of each over telnet, end-to-end "upload + launch + watch" runbook.

Each milestone is a separate PR. M1-M3 are pure infrastructure; M4-M7 stack on top.

## 13. Test plan

- **Unit (host)** — `latency_hist` bucket math (boundary cases, overflow), `rate_ewma` smoothing under bursty input, `gpu_consumer` toggle atomicity.
- **QEMU regression** — rate counters accumulate under `bench` workloads. Policy swap during a live workload does not crash. Telemetry subscribe/unsubscribe under fanout pressure (10 subscribers, 10 kHz emission) has bounded memory and reports drops.
- **Hardware (Jetson)** — `gpu use sched on` succeeds with `ai_mlp` active; `gpu use eviction on` succeeds with `xgboost` active. `admin` over telnet renders all pages and refreshes for 10 min without leaks (track `slm.mem_stats()` delta).
- **Soak** — `admin.lua` running 1 h, telemetry feed at sustained 1 kHz, no growth in `mem_stats().used_kb`.
- **Negative** — `gpu use sched on` with `heuristic` active returns `EOPNOTSUPP`. `model launch` with corrupt sha256 in meta refuses. `model unload` of a referenced model returns `EBUSY` and prints holders.

## 14. Open questions

1. **Per-task GPU toggle?** Spec is global. If demos need a per-task knob (e.g., one task uses GPU inference, another stays CPU), follow up in a v2.
2. **Auth on `admin`.** None. Telnet is unauthenticated. Acceptable for the lab; will need TLS + a token before any deployment outside the lab.
3. **Aggregation window for inference rate.** 1 s EWMA is a choice; a sliding-window p99 would be more accurate but more expensive. Ship with EWMA; reconsider if demo numbers feel laggy.
4. **Telemetry persistence.** Spec is in-memory only. A future "tee to LittleFS" sink can be added by subscribing to `/telemetry/**` from a kernel task and writing to a rotating file. Not in scope here.
5. **Model engine for `ggml`.** Stubbed. Real implementation is its own spec; this one just promises the launch surface and a meaningful ENOSYS until then.

## 15. References

- `docs/specs/gpu-inference.md` — GA10B nvgpu bringup, what `nvgpu prepare/run/inherit` do.
- `docs/specs/ai-scheduler.md` — policy interface and state vector.
- `docs/specs/ai-eviction.md` — eviction trait and feature schema.
- `docs/specs/lua.md` — current Lua surface.
- `docs/specs/runtime-blob-formats.md` — `SEMB` blob format and checksum scheme.
- `docs/dynamic-policy-model-loading-plan.md` — runtime model upload/activate/rollback (extended here for general model launch).
- `docs/dynamic-kernel-replace-plan.md` — kernel replacement via tryboot (out of scope; referenced for context).
- `docs/specs/shell.md` — shell command conventions, xput protocol.
