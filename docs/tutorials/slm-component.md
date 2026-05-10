# Tutorial: Using the `slm-runner` Component

The `slm-runner` is a built-in SLM-OS component that turns a loaded GGUF
language model into a long-lived chat service. It subscribes to
`/slm/prompt`, drives each prompt through the M5.2 session FFI, streams
generated tokens back on `/slm/token`, and announces per-prompt
completion on `/slm/done`.

This tutorial walks through running the component, publishing a prompt
to it, and consuming the streamed response from a Lua script. Reading
[`docs/tutorials/component.md`](component.md) first is recommended —
the component lifecycle and message-router primitives below are the
same ones the `digit_classifier` walkthrough introduces.

---

## Overview

```
+-----------+   /slm/prompt    +-------------+   /slm/token   +-----------+
|  Producer | ---------------> |  slm-runner | -------------> | Consumer  |
| (Lua/API) |                  |  component  | -------------> |  (Lua/UI) |
+-----------+                  +-------------+   /slm/done    +-----------+
```

The runner opens a single SLM session over slot 0 (the most recently
loaded model) and keeps it alive for the lifetime of the component.
Every `/slm/prompt` message triggers one prefill+decode pass; the token
callback publishes each generated UTF-8 token as its own message on
`/slm/token`, then a final `/slm/done` summary carries the per-prompt
counters.

### Topic Surface

| Topic | Direction | Payload | Notes |
|-------|-----------|---------|-------|
| `/slm/prompt` | runner subscribes | UTF-8 prompt string | One message per prompt; payload size capped at 60 bytes by the message router |
| `/slm/token` | runner publishes | UTF-8 token bytes | One message per generated token; consumer must ack quickly under the router's latest-wins semantics |
| `/slm/done` | runner publishes | `rc=<n> tokens_out=<n> prompts=<n>` | Always published once per prompt, even when the decoder rejects the request |

---

## Step 1: Load a Model

The runner requires an SLM in slot 0 of the registry. On Jetson the
recommended path is `slm xload`, which streams the GGUF straight
into a single PMM buffer (see `docs/setup.md` §"Jetson SD-Card
Layout" for why the LittleFS-staged path doesn't fit a Qwen-class
model on 8 GB). On platforms with enough contiguous PMM the
file-path `slm load` form still works.

```
# Jetson (streamed):
SLM-OS> slm xload qwen <total_bytes>
[INFO] SLM 'qwen' loaded at slot 0 (28 layers, vocab 152064)

# Or, on a platform with the alloc+copy headroom:
SLM-OS> slm load /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf
[INFO] SLM 'qwen2.5-1.5b' loaded at slot 0 (28 layers, vocab 152064)

SLM-OS> slm list
[0] qwen  arch=qwen2  ctx=32768  vocab=152064
```

If the runner is started before any model is loaded, the component
exits cleanly after logging:

```
[slm-runner] ERROR: no SLM loaded
[slm-runner] Load one with: slm xload <name> <bytes>
```

---

## Step 2: Start the Component

```
SLM-OS> component run slm-runner
Component 'slm-runner' v1.0.0 started (idx=5, task=12)
[slm-runner] Started (component 5, session 0), subscribed to /slm/prompt
```

The runner opens a session with the spec's demo defaults: `max_ctx=4096`,
sampler `top_k=40, top_p=0.9, temperature=0.7`, seed `0`. The session
stays open until the component task exits (60 seconds idle timeout, or
explicit termination).

---

## Step 3: Publish a Prompt

Any task on the system can drive the runner by publishing on
`/slm/prompt`. From the shell, the simplest path is the `msg send`
verb:

```
SLM-OS> msg send /slm/prompt "Say hello"
[slm-runner] prompt #1 rc=0 tokens=12
```

Each generated token shows up as a `/slm/token` publish; subscribe a
listener component to see the stream:

```
SLM-OS> component run listener           # toy listener; subscribes to "events"
SLM-OS> msg subscribe /slm/token listener  # bind it to /slm/token instead
```

---

## Step 4: Lua Demo

A more interesting consumer is a Lua script that subscribes to both
`/slm/token` and `/slm/done`, prints tokens as they arrive, and exits
on the done message:

```lua
-- slm-chat-demo.lua: publish a prompt and stream the response.

local function on_token(topic, payload)
    io.write(payload)
    io.flush()
end

local function on_done(topic, payload)
    print()
    print("[done] " .. payload)
    msg.unsubscribe("/slm/token")
    msg.unsubscribe("/slm/done")
end

msg.subscribe("/slm/token", on_token)
msg.subscribe("/slm/done",  on_done)

msg.publish("/slm/prompt", "Explain RAII in one sentence.")
```

Run with `lua /mnt/files/slm-chat-demo.lua` after `component run
slm-runner` is active. The expected output is the model's response
streaming token-by-token, followed by a `[done] rc=0 tokens_out=N
prompts=1` summary line.

---

## Step 5: Inspect the Session

The shell's `slm` family exposes the same session the runner is
driving:

```
SLM-OS> slm status
models=1 sessions=1
SLM-OS> slm stats 0
session 0: prompts=1 tokens_in=4 tokens_out=12 ttft=42ms decode=180ms
```

`slm stats <session_id>` prints the same `SlmStatsC` snapshot the
runner publishes on `/slm/done`.

---

## Limitations (M8.1)

- **M5.2 placeholder weights.** The decoder runs on zero-initialised
  weights, so non-empty prompts currently return `rc=-1` and emit no
  tokens. Real generation lands with M5.3 once the GGUF weights are
  mapped into the model-memory pool. The runner still publishes
  `/slm/done` on the rejected path so subscribers always observe
  completion.
- **Hot-swap caveat.** Replacing the runner via `component_hot_swap`
  rebuilds the session over slot 0. If the underlying SLM has been
  unregistered between the two runs, the new instance logs the
  `no SLM loaded` hint and exits. Spec target: full session/KV
  carryover across hot-swap (tracked as a follow-up).
- **Topic payload cap.** The message router caps payloads at 60 bytes.
  Tokens longer than 60 bytes (rare for BBPE outputs) are truncated
  before publish.

---

## Source Files

| File | Purpose |
|------|---------|
| `kernel/src/slm_runner.c` | Component entry point, token callback, prompt dispatch |
| `kernel/src/component_runtime.c` | Built-in registration in `builtin_components[]` |
| `kernel/include/slm_ffi.h` | `rust_slm_session_open` / `rust_slm_prompt` / `rust_slm_stats` declarations |
| `runtime/src/slm/decoder.rs` | Rust-side prefill + decode loop |
| `runtime/src/msg_router.rs` | Topic router driving `/slm/prompt` -> `/slm/token` -> `/slm/done` |

See [`docs/design/slm-integration.md`](../design/slm-integration.md)
"Component Integration" for the design rationale and
[`docs/plans/slm-integration-plan.md`](../plans/slm-integration-plan.md)
M8 for the delivery context.

---

*Last updated: April 2026*
