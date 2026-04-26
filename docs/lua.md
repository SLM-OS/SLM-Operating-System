# Lua Scripting

SLM-OS includes an embedded Lua 5.4 interpreter for scripting and automation.

## Overview

Lua integration provides:
- Interactive REPL (Read-Eval-Print Loop) for experimentation
- Direct code execution via command line
- Kernel API access through the `slm` module
- Standard Lua libraries: base, table, string, math

## Shell Commands

```
lua              # Enter interactive REPL
lua -e "code"    # Execute Lua code directly
lua <file>       # Run script from filesystem
lua-admin        # Full admin/global-control Lua REPL
```

`lua` is the default concurrent-safe surface. It exposes the
session-local / read-mostly bindings intended for multiple interactive
sessions. `lua-admin` is the serialized surface for global mutators
such as task control, shell escape, component hot-swap, and telnetd
control.

### Interactive REPL

```
slmos> lua
Lua 5.4 REPL - type 'exit' to quit
>>> print("Hello from Lua!")
Hello from Lua!
>>> 1 + 2
>>> print(1 + 2)
3
>>> exit
Exiting Lua REPL
```

REPL controls:
- `exit` - Exit the REPL
- `Ctrl+D` - Exit the REPL
- `Ctrl+C` - Cancel current line

### Direct Execution

```
slmos> lua -e "print(1+2)"
3
slmos> lua -e "for i=1,5 do print(i) end"
1
2
3
4
5
```

### Script Files

Scripts can be loaded from the mounted filesystem:

```
slmos> write /mnt/files/hello.lua print('Hello from file!')
slmos> lua /mnt/files/hello.lua
Hello from file!
```

Scripts have full access to the `slm` module:

```
slmos> write /mnt/files/status.lua slm.print('Up: ' .. slm.uptime() .. 'ms')
slmos> lua /mnt/files/status.lua
Up: 12345ms
```

Scripts are limited to ~16 KB (fits the largest embedded demo with headroom —
the buffer lives on the shell task's 64 KB stack). If the file cannot be read,
exceeds the size limit, or contains errors, `lua` prints a diagnostic and
returns to the shell.

## SLM-OS Kernel Bindings

The `slm` module provides access to kernel functionality:

### System

| Function | Description |
|----------|-------------|
| `slm.print(...)` | Print to console (replaces Lua's print) |
| `slm.uptime()` | Get system uptime in milliseconds |
| `slm.mem_stats()` | Get memory statistics table |
| `slm.tasks()` | Get list of running tasks |
| `slm.sleep(ms)` | Sleep for milliseconds |
| `slm.yield()` | Yield CPU to scheduler |
| `slm.version()` | Get SLM-OS version string |
| `slm.cpu_count()` | Get number of CPUs |
| `slm.cpu_id()` | Get current CPU ID |

The `slm` module also exposes three string constants sourced from
`build_info.h` (regenerated on every build under
`${CMAKE_BINARY_DIR}/include/` from `version.txt` + git state + UTC):

| Constant | Description |
|----------|-------------|
| `slm.VERSION` | Semantic version (e.g. `"0.4.0"`). Same value as `slm.version()` minus the `"SLM-OS "` prefix. |
| `slm.BUILD_STAMP` | UTC build timestamp, exactly 14 ASCII digits in `YYYYMMDDhhmmss` form (string-sortable). |
| `slm.BUILD_SHA` | Short git SHA of the source tree at build time, with a `-dirty` suffix when the working tree had uncommitted changes. |

The same triple is printed in the boot banner and is reflected by
`cat /sys/version`.

### Component Management

| Function | Description |
|----------|-------------|
| `slm.component_count()` | Number of registered components |
| `slm.component_list()` | Array of component tables (name, version, type, state, priority, task_id, index) |
| `slm.component_find(name)` | Find component by name, returns index or nil |
| `slm.component_run(name)` | Admin surface only. Run a built-in component, returns index or nil |
| `slm.component_hot_swap(old, new)` | Admin surface only. Replace component preserving subscriptions, returns index or nil |
| `slm.component_hot_swap_stateful(old, new)` | Admin surface only. Replace component, transfer state + subscriptions. Sensor monitor transfers alert count. |

### Message Routing

| Function | Description |
|----------|-------------|
| `slm.msg_publish(topic, data)` | Publish a message to a topic. Returns number of subscribers that received it. Wildcard subscribers matching the topic prefix also receive the message. |
| `slm.msg_publish_priority(topic, data, priority)` | Publish with explicit priority (0=normal, higher=more urgent). Higher-priority messages are delivered first by `msg_router_receive`. |
| `slm.msg_subscribe(topic, fn)` | Register a Lua callback for a topic. Pass `"/foo/*"` for wildcard prefix match. Returns a subscription handle (integer ≥ 1) on success or `nil` when the Lua subscription pool is full. Callbacks are dispatched as `fn(topic, data)` at `slm.yield` / `slm.sleep` / `slm.read_line` points — they do not run truly concurrently. Callback errors are logged and swallowed so one bad handler does not break the drain loop. |
| `slm.msg_unsubscribe(handle)` | Remove a subscription. Returns `true` on success, `false` if the handle is unknown. |
| `slm.msg_drain()` | Manually dispatch any pending callbacks. Usually not needed — drains happen automatically at yield points — but useful for scripts that compute without yielding. |
| `slm.try_getc()` | Nonblocking one-character read from the current shell session. Returns a one-byte string or `nil` when no input is pending. Useful for live dashboards that refresh without blocking on `read_line()`. |

### Scheduler

| Function | Description |
|----------|-------------|
| `slm.sched_policy()` | Get current scheduler policy name (e.g., `"heuristic"`). |
| `slm.sched_stats()` | Scheduler statistics: `{task_count, ready_count, context_switches, timer_ticks, policy}`. |
| `slm.sched_set_policy(name)` | Admin surface only. Switch active scheduler policy at runtime. Returns `true` on success, `false` on unknown name. |
| `slm.sched_policy_list()` | Array of `{name, active}` tables for every registered policy. Exactly one entry has `active=true`. |
| `slm.ai_sched_stats()` | AI scheduler statistics: `{policy, decisions, fallbacks, avg_latency_ns, histogram}`. Returns `nil` when `CONFIG_AI_SCHEDULER` is off. |
| `slm.ai_sched_decision(task_id)` | Last AI-scheduler decision recorded for this task: `{core, priority_adj, preempt, raw}`. `priority_adj` is `0`/`1`/`2` (none/boost/reduce); `preempt` is `0`/`1`; `raw` is the packed action index (`core*6 + priority_adj*2 + preempt`). Returns `nil` when `CONFIG_AI_SCHEDULER` is off, the task id is unknown, or the AI policy has never run on the task. |
| `slm.task_migrate(task_id, target_cpu)` | Admin surface only. Move a non-running task to a specific CPU. Returns `true` on success, `false` if the task is running, the affinity forbids it, or the arguments are out of range. |
| `slm.task_create(name, fn)` | Admin surface only. Spawn a kernel task that runs `fn` in a fresh `lua_State`. `fn` is serialized via `lua_dump` (bytecode only — no upvalues or global captures). Returns the task id (≥1) on success, `nil` on pool exhaustion / dump failure / task creation failure. Concurrency: each running Lua task keeps its own `lua_State`, but all share one Lua heap — `heap_reset` is deferred until the last state closes. Pool is capped at 16 concurrent Lua tasks. |
| `slm.task_kill(task_id)` | Admin surface only. Terminate a task (`scheduler_remove_task` + `task_destroy`). Refuses the idle task (id 0), the current task (use `task_exit` for self-termination), and already-terminated tasks. Returns bool. |
| `slm.task_set_priority(task_id, priority)` | Admin surface only. Change a task's priority. `priority` must be in `[0, 7]` (0=idle, 7=critical). Returns bool. |
| `slm.task_pin(task_id, cpu)` | Admin surface only. Pin a task to a specific CPU. Pass a negative `cpu` to clear affinity (task becomes CPU_AFFINITY_ANY). Returns bool. |

### CPU / Memory / IPC

| Function | Description |
|----------|-------------|
| `slm.cpu_info()` | Per-CPU state: `{online_count, total_count, current_cpu, cpus={{id, isolated, ticks, schedules}, ...}}`. |
| `slm.term_size()` | Current shell-session terminal metadata: `{cols, rows, term}`. `cols`/`rows` default to 80x24 when the client did not negotiate NAWS. |
| `slm.vmm_stats()` | Virtual memory statistics: `{l1_tables, l2_tables, blocks_mapped, bytes_mapped}`. Returns `nil` on x86-64 (no VMM yet). |
| `slm.ipc_stats()` | IPC statistics: `{queue_count, buffer_count, msgs_sent, msgs_recv}`. |

### Eviction

Available by default. When the kernel is built with
`DISABLE_EVICTION=ON`, every
binding returns `nil`/`false` — scripts can branch on the return value
without a compile-time guard.

| Function | Description |
|----------|-------------|
| `slm.eviction_policy()` | Current policy name string (`"lru"`, `"lfu"`, `"cacheus"`, `"xgboost"`, ...) or `nil`. |
| `slm.eviction_set_policy(name)` | Admin surface only. Switch active eviction policy. Returns `true` on success, `false` on unknown name / feature off. |
| `slm.eviction_stats()` | `{enabled, policy, weight_evictions, workspace_evictions, weight_allocated, weight_total, workspace_allocated, workspace_total, snapshot_candidates, expert_weights_bp}` or `nil`. CACHEUS expert weights are in basis points (0–10000). |
| `slm.eviction_model_status(kind)` | Runtime blob status for one kind (`"xgboost"`, `"mlp"`, `"cacheus_config"`): `{kind, state, has_staged, has_active, has_rollback, staged?, active?, rollback?}` or `nil`. Each `*_meta` table contains `{version, kind_id, feature_schema_version, payload_len, checksum}`. |
| `slm.eviction_model_load(kind, path)` | Admin surface only. Stage a blob from a VFS path into the RAM-backed eviction store. Returns `true` on success, `false` on invalid kind/path/blob. |
| `slm.eviction_model_activate(kind)` | Admin surface only. Promote the staged blob for `kind` to active. Returns `true` on success, `false` when no staged blob exists or the feature is off. |
| `slm.eviction_model_rollback(kind)` | Admin surface only. Restore the previous active blob for `kind`. Returns `true` on success, `false` when no rollback blob exists or the feature is off. |
| `slm.eviction_model_clear(kind)` | Admin surface only. Clear staged, active, and rollback slots for `kind`. Returns `true` on success, `false` on invalid kind / feature off. |

### Model Memory and Inference

| Function | Description |
|----------|-------------|
| `slm.model_stats()` | Pool statistics: `{weights={total_blocks, free_blocks, allocated_blocks, shared_blocks, peak_usage}, workspace={...}}` |
| `slm.model_load_mnist()` | Admin surface only. Load the built-in MNIST ONNX model (26 KB). Returns model index or -1. |
| `slm.model_list()` | Array of loaded models: `{{index, name, format, params, weight_size, nodes}, ...}`. |
| `slm.model_info(index)` | Detailed info: `{index, name, format, params, weight_size, workspace_size, nodes, inputs, outputs}` or `nil` if index invalid. |
| `slm.model_find(name)` | Find a loaded model by name. Returns index or -1. |
| `slm.model_infer(index)` | Admin surface for now. Run inference on a loaded model. Returns predicted class (0-9 for MNIST). |
| `slm.model_bench(index, iters)` | Admin surface only. Run inference benchmark for `iters` iterations. Results printed to UART. Returns 0 on success, -1 on error. |
| `slm.model_load(path[, name])` | Admin surface only. Load an ONNX model from a VFS path. Mirrors the `model load <path>` shell command: reads the file, allocates a transient PMM buffer, calls `rust_model_load`, frees the buffer. If `name` is omitted, the model name is derived from the filename (extension stripped). Returns the model index (≥0) on success, -1 on any error. |
| `slm.model_pin(index)` | Admin surface only. Pin a model to prevent LRU eviction. Returns 0 on success, -1 on error. |
| `slm.model_unpin(index)` | Admin surface only. Unpin a model (allow LRU eviction). Returns 0 on success, -1 on error. |
| `slm.infer_stats()` | Cumulative inference statistics: `{total, total_ns, min_ns, max_ns, last_ns, errors}`. |
| `slm.gpu_status()` | GPU subsystem info: `{available, name, device, compute_ready, unified_memory, memory_size}`. `.available` is always set; other fields populated when a driver is present. |

### Hailo NPU (AI HAT+)

Available on `lua-admin` for now, not the default concurrent `lua`
surface.

The `slm.hailo` sub-table exposes the Hailo-8L NPU backend when the Pi 5
AI HAT+ is present (the backend registers itself during boot after
probing the external PCIe link). On platforms without the AI HAT+ —
QEMU, x86-64, Pi 5 without the HAT, Jetson — every function below
degrades gracefully: `status()` reports `available=false`,
`load()`/`infer()` return `nil`, `unload()` returns `false`. Scripts
that use `slm.hailo.*` therefore remain portable across platforms.

| Function | Description |
|----------|-------------|
| `slm.hailo.status()` | Device probe. Returns a table: `{available}` when the backend is absent; `{available, name, slots_in_use, slots_max}` when present. `name` is `"hailo-8"`; `slots_max` is the hard cap (currently 4 concurrent models). |
| `slm.hailo.load(path)` | Load a compiled HEF from the VFS, staging it through a PMM-allocated buffer. Drives the firmware context-switch sequence (ACTIVATION → PRELIMINARY → DYNAMIC → ENABLED). Returns the model handle (integer ≥ 0) on success or `nil` on any failure (missing file, bad header, backend absent, all slots full). |
| `slm.hailo.infer(handle, input_bytes)` | Run one inference. `input_bytes` is a Lua string whose byte length must match the model's declared input tensor size (retrieved from the HEF at load time). Returns the raw output tensor as a Lua string, or `nil` on any failure (wrong input size, invalid handle, backend absent, firmware error). |
| `slm.hailo.unload(handle)` | Release the NPU slot claimed by a prior `load()`. Returns `true` on success, `false` on any failure. Required before `load()`ing a fifth model once the four-slot cap is reached. |

Typical lifecycle:

```lua
local s = slm.hailo.status()
if not s.available then
    print("No Hailo NPU on this platform")
    return
end

local h = slm.hailo.load("/mnt/files/mobilenet_v1.hef")
if h == nil then error("load failed") end

-- Input size is model-specific. For a probe-style demo, see
-- scripts/demo_hailo.lua which tries common shapes until one is
-- accepted.
local input = string.rep("\0", 150528)  -- 224 * 224 * 3
local output = slm.hailo.infer(h, input)
if output == nil then error("infer failed") end

-- output is a raw INT8 tensor (1000 classes for MobileNet-V1).
print("output length:", #output)

assert(slm.hailo.unload(h))
```

Full walkthrough with a rolling-FPS benchmark: `lua /mnt/files/demo_hailo.lua`
(see `docs/demo.md` §"Hailo NPU demo").

### Camera capture

The `slm.camera` sub-table exposes the camera capture API. Available on
both the safe and admin Lua surfaces — the bindings themselves perform no
mutation; running inference on the captured frame still requires admin
(`slm.model_infer_bytes` lives on the admin surface).

Backends recognised:

| Name | Status | Notes |
|------|--------|-------|
| `"mock"` | Always reachable when the kernel is built with `MOCK_CAMERA_FRAME=ON` (default). | Returns a baked-in 1640×1232 RAW10 RGGB frame produced from a single MNIST test digit at build time. Used for QEMU CI coverage and as a fallback on hardware where no IMX219 stack is wired up yet. |
| `"imx219-0"` / `"imx219-1"` | Not yet implemented. | Reserved names for the J17 / J20 connectors on the Jetson Orin Nano dev kit. `open()` returns `nil` until the driver lands (`docs/jetson-camera-imx219-plan.md`). |

| Function | Description |
|----------|-------------|
| `slm.camera.open(name)` | Resolve a camera by name. Returns a handle table `{name, capture, close}` on success, or `nil` if the backend is missing or the name is unknown. The returned table is method-callable (`cam:capture()`, `cam:close()`). |
| `slm.camera.capture(arg)` | Procedural form. `arg` is the camera name (string) OR a handle table whose `.name` field is read — so `cam:capture()` works through Lua's `a:b()` sugar. Returns `(frame_id, width, height, bayer)` on success, `nil` on failure. `frame_id` is a kernel-side integer that names the captured buffer for `preprocess_mnist`; today only `0` is valid (the mock's `.rodata` frame). `bayer` is `0` for RGGB. |
| `slm.camera.close(arg)` | Procedural form. Returns `true`. The mock backend owns no per-handle state; real backends will release DMA buffers here. |
| `slm.camera.preprocess_mnist(frame_id, width, height, bayer)` | Decode the named frame into 3,136 bytes of fp32 in `[0, 1]` suitable for `slm.model_infer_bytes`. Pipeline is centred 1232×1232 crop → green-channel-only Bayer extract → 44×44 box-average → integer-only IEEE 754 normalise. Returns the bytes string on success; `(nil, rc<0)` on error: `-1` bad `frame_id`, `-2` mock backend not embedded, `-3` `(w, h, bayer)` don't match the backing frame. |

Frame bytes are not exposed to Lua because the 1640×1232 RAW10 frame is
2.5 MB — well above the Lua heap cap. `preprocess_mnist` therefore reads
the bytes through the kernel-side `frame_id`. Future small-mode captures
(e.g. 640×480) could grow a string-bytes overload.

Typical lifecycle:

```lua
local cam = slm.camera.open("mock")
if cam == nil then error("camera backend missing") end

local fid, w, h, bayer = cam:capture()
local mnist_bytes = slm.camera.preprocess_mnist(fid, w, h, bayer)

local idx = slm.model_load_mnist()
local logits, argmax = slm.model_infer_bytes(idx, mnist_bytes)
slm.print("predicted digit: " .. argmax)

cam:close()
```

The QEMU integration test (`test_slm_camera_e2e_mnist_mock` in
`kernel/tests/test_lua.c`) runs this exact sequence against the mock
frame and asserts `argmax == 3` (the baked digit's class). The full
plan for the IMX219 hardware backend is in
`docs/jetson-camera-imx219-plan.md`; per-driver code-read summaries are
in `docs/jetson-camera-{imx219,nvcsi,vi}-driver-notes.md`.

### Memory Statistics

```lua
>>> stats = slm.mem_stats()
>>> for k,v in pairs(stats) do print(k, v) end
total_kb    1048576
free_kb     1020432
used_kb     28144
```

### Task Information

```lua
>>> for _,t in ipairs(slm.tasks()) do
...   print(t.id, t.name, t.state, t.cpu)
... end
0       idle    ready   0
1       idle    ready   1
2       idle    ready   2
3       idle    ready   3
4       init    blocked 0
5       test    running 1
6       shell   running 2
```

### Component Management

```lua
>>> slm.component_run('counter')
0
>>> slm.component_run('echo')
1
>>> for _,c in ipairs(slm.component_list()) do
...   print(c.name, c.state, c.type)
... end
counter   running   service
echo      running   service
>>> slm.component_hot_swap('counter', 'listener')
0
```

### Model Memory Statistics

```lua
>>> stats = slm.model_stats()
>>> w = stats.weights
>>> print('Weight pool: ' .. w.free_blocks .. '/' .. w.total_blocks .. ' free')
Weight pool: 128/128 free
>>> ws = stats.workspace
>>> print('Workspace: ' .. ws.free_blocks .. '/' .. ws.total_blocks .. ' free')
Workspace: 64/64 free
```

## Implementation Details

### Build Configuration

Lua is built as a separate static library to allow floating-point operations
while the kernel itself uses `-mgeneral-regs-only`. The Lua library includes:

- Lua 5.4 core (`kernel/lib/lua/src/`)
- Libc stubs for freestanding environment (`kernel/src/lua_stubs.c`)
- SLM-OS bindings (`kernel/src/lua_slm.c`)
- Shell command (`kernel/src/lua_shell.c`)
- setjmp/longjmp for error handling (`kernel/arch/arm64/setjmp.S`)

### Lua Configuration

Lua is configured for embedded use with:
- `LUA_32BITS=1` - 32-bit integers and floats
- `LUA_USE_C89=1` - C89 compatibility mode
- `LUAI_MAXSTACK=15000` - Reduced stack size
- `LUA_MINBUFFER=32` - Smaller buffers

### Memory

Lua uses a 1MB heap allocated in `.bss` for all allocations. The heap
uses a first-fit allocator with coalescing on free.

### Libc Stubs

The freestanding environment provides minimal implementations of:
- Memory: `malloc`, `free`, `realloc`, `calloc`
- Strings: `strrchr`, `strcat`, `strstr`, `strdup`, etc.
- Math: `sin`, `cos`, `exp`, `log`, `sqrt`, `pow`, etc.
- I/O: `printf`, `fprintf` (output to UART)
- Time: `time`, `clock` (based on timer counter)

## Limitations

- **~16 KB script size limit**: Scripts loaded via `lua <path>` must fit in the shell task's on-stack buffer (16 KB today).
- **No coroutines**: The coroutine library is not enabled
- **No debug library**: The debug library is not enabled
- **No os library**: System calls are not available
- **Limited math precision**: Math functions use Taylor series approximations
- **Inverse-trig stubs**: `math.asin`, `math.acos`, `math.atan`, `math.atan2`
  are placeholder stubs that return `0.0`. The first call to each logs a
  one-time warning to the kernel console. Lua code that depends on inverse
  trig must implement the approximation locally or run on a host build.
- **No `msg_subscribe` yet**: Scripts can publish messages via
  `slm.msg_publish()` but cannot register a Lua callback as a subscriber.
  A Lua-defined pure-subscriber component requires callback-queue plumbing
  that has not landed yet (see audit in #152).
- **No Lua-defined tasks**: Task *introspection* is available via
  `slm.tasks()`, but `slm.task_create(fn)` / `slm.task_kill(id)` etc. have
  not been implemented. Scripts drive work through existing native components
  and hot-swap.

## Future Enhancements

- `slm.msg_subscribe(topic, callback)` — requires callback queuing design.
- `slm.task_*` — create/kill/pin Lua tasks; requires per-task Lua state or
  bytecode-trampoline design.
- `slm.model_load(path, name)` — generic ONNX loader from VFS (today only
  the built-in MNIST model can be loaded from Lua).
- Script-driven test automation (subset of the existing C unit tests expressed
  as Lua scripts).
- Configuration files in Lua.
