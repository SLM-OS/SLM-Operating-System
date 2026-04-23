# Concurrent Interactive Lua Plan

## Goal

Allow multiple telnet shell sessions to run interactive Lua at the same
time without interfering with each other.

The key point is that this is no longer mainly a `lua_State`
problem. Per-session states already exist for non-console REPLs. The
remaining blocker is that the `slm.*` API surface still exposes a mix
of:

- session-local / read-only operations that are naturally safe
- subsystem-backed operations that may already be safe, but need an
  explicit audit
- global mutators that still rely on shell-level serialization

So the work should be treated as **Lua binding concurrency hardening**,
not as an interpreter rewrite.

## Current State

What already works:

- Non-console interactive sessions use separate `lua_State`s.
- REPL line buffers are session-local.
- Shell I/O is per-session.
- Lua message subscriptions are per-state, not multiplexed through one
  global mailbox.
- The Lua allocator is serialized across states.
- The REPL drops the shell mutation lock while waiting for input, so
  multiple sessions can sit at `>>>` concurrently.

What does not work yet:

- Actual Lua line execution is still serialized because the `lua`
  shell command is marked mutating.
- That serialization is still necessary because many `slm.*` bindings
  mutate global system state and have not been audited for concurrent
  use.

## Staged Plan

### Stage 0: Binding Classification

Deliverable:

- A complete classification of every exported `slm.*` binding into one
  of:
  - `safe_now`
  - `needs_audit`
  - `serialized_admin`

Exit criteria:

- Every binding in `slm_lib[]` is classified.
- The classification reflects implementation reality, not just the
  function name.

### Stage 1: Allow Concurrent REPL Execution for `safe_now`

Approach:

- Split the current `lua` command execution model into:
  - input path: already concurrent
  - line execution path: concurrent only for safe bindings
- Keep a lightweight per-state execution guard if needed, but remove
  the global shell mutex dependency for safe-only evaluation.

Deliverable:

- Two telnet sessions can evaluate read-only/session-local Lua code in
  parallel.
- Interactive demos that use only `safe_now` bindings can run
  simultaneously.

Exit criteria:

- Hardware test: two REPLs run `print`, `sleep`, `yield`,
  `msg_subscribe`, `msg_publish`, etc. concurrently with no cross-talk
  or prompt stalls.

### Stage 2: Audit and Promote `needs_audit` Bindings

Approach:

- Audit each subsystem invoked by the `needs_audit` bindings.
- Add subsystem-local locking or explicit non-reentrancy guards where
  necessary.
- Promote bindings one by one from `needs_audit` to `safe_now` or
  `serialized_admin`.

Deliverable:

- A documented concurrency contract for each promoted binding.

Exit criteria:

- Every `needs_audit` binding has been either promoted or explicitly
  left serialized with justification.

### Stage 3: Introduce an Admin/Privileged Lua Surface

Approach:

- Move global mutators behind an explicit privileged surface, e.g.
  `slm_admin.*` or an "admin Lua" mode.
- Keep ordinary interactive Lua concurrent by default.

Deliverable:

- Default interactive Lua can run concurrently.
- Global control operations remain available, but are clearly
  identified as serialized/admin operations.

Exit criteria:

- `lua` (default REPL) no longer needs global shell serialization.
- The remaining serialized operations are explicit and documented.

### Stage 4: Automated Concurrency Coverage

Approach:

- Add regression tests for:
  - two simultaneous REPL sessions
  - concurrent msg subscription/delivery
  - concurrent safe bindings
  - admin operation exclusion/serialization

Deliverable:

- In-tree coverage for the binding classes and their intended
  concurrency model.

Exit criteria:

- Regressions in concurrent REPL behavior are caught without manual Pi
  testing for every change.

## Initial Binding Classification

### `safe_now`

These are already session-local, read-only, or backed by their own
subsystem logic strongly enough that they are good first candidates for
 concurrent interactive execution:

| Binding | Reason |
|---|---|
| `print` | Session-local output only |
| `uptime`, `uptime_us`, `version` | Read-only |
| `mem_stats`, `tasks`, `cpu_count`, `cpu_id`, `cpu_info`, `vmm_stats`, `ipc_stats`, `term_size` | Read-only snapshots |
| `sleep`, `yield` | Task-local scheduler interactions |
| `component_count`, `component_list`, `component_find` | Read-only registry queries |
| `msg_publish`, `msg_publish_priority` | Router-backed publish path |
| `msg_subscribe`, `msg_unsubscribe`, `msg_drain` | Already state-scoped and internally locked |
| `sched_policy`, `sched_stats`, `sched_policy_list`, `ai_sched_stats`, `ai_sched_decision` | Read-only scheduler introspection |
| `eviction_policy`, `eviction_stats` | Read-only policy/stats introspection |
| `infer_stats`, `gpu_status` | Read-only status/stat queries |
| `read_line`, `try_getc` | Session-local input path |
| `telnetd_status`, `telnetd_sessions` | Read-only daemon/session introspection |

### `needs_audit`

These might be safe to make concurrent, but they interact with shared
kernel subsystems and need an explicit audit first:

| Binding | Audit focus |
|---|---|
| `model_stats`, `model_find`, `model_list`, `model_info` | Registry snapshot consistency under concurrent loads/evictions |
| `model_infer`, `model_bench` | Inference-device and model-registry concurrency contract |
| `hailo.load`, `hailo.infer`, `hailo.unload`, `hailo.status` | Hailo backend serialization / device ownership |

### `serialized_admin`

These mutate global system state and should stay serialized unless they
are moved behind a dedicated privileged surface with their own locking:

| Binding | Why |
|---|---|
| `component_run` | Starts global components/tasks |
| `component_hot_swap`, `component_hot_swap_stateful` | Mutates global component registry/runtime |
| `model_load_mnist`, `model_load`, `model_pin`, `model_unpin`, `model_preload`, `model_preload_wait` | Mutate global model registry / residency / preload state |
| `sched_set_policy` | Global scheduler policy mutation |
| `task_create`, `task_migrate`, `task_kill`, `task_set_priority`, `task_pin` | Global task-table / scheduler mutation |
| `eviction_set_policy` | Global eviction-policy mutation |
| `shell_exec` | Escapes to full shell command surface |
| `telnetd_start`, `telnetd_stop`, `telnetd_kick` | Global daemon/session mutation |

## Recommended Execution Model

### Default interactive Lua

- Concurrent
- Exposes `safe_now`
- Gains promoted `needs_audit` bindings over time
- Does not expose global control operations directly

### Admin Lua

- Serialized
- Exposes global mutators
- Intended for bring-up, demo control, and recovery actions

This split is the cleanest long-term model because it reflects the
actual boundary that matters: **session-local scripting vs global system
control**.

## Immediate Next Steps

1. Introduce an internal binding classification table instead of
   treating the whole `lua` command as one mutating blob.
2. Gate default REPL exposure to the `safe_now` set.
3. Keep script / admin execution paths serialized until the privileged
   split lands.
4. Add a hardware smoke test that proves two telnet REPLs can evaluate
   safe bindings concurrently.
