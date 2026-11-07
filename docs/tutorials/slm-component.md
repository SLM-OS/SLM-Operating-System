# Tutorial: Using the `slm-runner` Component

The `slm-runner` is a built-in SLM-OS component that turns a loaded GGUF
language model into a service on the message bus. It subscribes to
`/slm/prompt` and `/slm/stop`, answers each prompt through the session FFI,
streams generated tokens on `/slm/token`, and ends every prompt with one
`/slm/done`.

This tutorial covers starting the runner, asking it something from the
shell and from Lua, and what it guarantees. Reading
[`docs/tutorials/component.md`](component.md) first is recommended: the
component lifecycle and message-router primitives are the same ones the
`digit_classifier` walkthrough introduces.

---

## What Works

| Item | State |
|------|-------|
| Client publishes `/slm/prompt`, receives every token in order, then `/slm/done` with stats | ✅ `make test`: scripted logits and the real forward path on `scripts/fixtures/slm/tiny_qwen2.gguf` |
| Prompts up to 511 bytes (a 64-token prompt is about 370) | ✅ `make test` |
| Stop by `/slm/stop`, `slm stop <session>`, or Ctrl-C in `slm ask` | ✅ `make test` |
| Runner restart, model choice, `--gpu`, bad handle | ✅ `make test` |
| Real Qwen on Jetson after a cold UEFI-direct boot | ☐ integration stage of #475 |
| Tests on x86-64 | ⏸️ `kernel/tests/test_slm_runner.c` stays ARM64-only, as `test_slm_shell.c` does; x86-64 builds and links the runner but doesn't run it |

The runner runs **in-kernel only**. The EL0 task path works, but the component
runtime still creates components at EL1, and the runner has not run at EL0 on
any platform. A fault in the decoder is a kernel fault.

Hot-swap of the runner is not tested. The model stays in the registry across a
swap, but the new instance opens its own session, so a running conversation
and its KV cache don't carry over.

---

## Overview

```
+-----------+   /slm/prompt    +-------------+   /slm/token   +-----------+
|  Client   | ---------------> |  slm-runner | -------------> |  Client   |
| (shell or |   /slm/stop      |  component  |   /slm/done    | (shell or |
|    Lua)   | ---------------> |  in-kernel  | -------------> |    Lua)   |
+-----------+                  +-------------+                +-----------+
```

### Topic Surface

| Topic | Direction | Payload | Notes |
|-------|-----------|---------|-------|
| `/slm/prompt` | runner subscribes | UTF-8 prompt, up to 511 bytes | One message per prompt. A longer publish is refused, never cut |
| `/slm/stop` | runner subscribes | ignored | Ends the running prompt before its next forward step. Does nothing when idle |
| `/slm/token` | runner publishes | UTF-8 text | One message per generated token that has printable bytes |
| `/slm/done` | runner publishes | see below | Exactly one per prompt, on every path |

A token whose bytes end inside a character carries what's complete, and the
next one brings the rest, so a message never holds half a character. A token
longer than a message is split on a character boundary. NUL bytes can't travel
on the bus and are left out.

### `/slm/done`

A prompt that ran:

```
rc=0 stop=eos tokens_in=98 tokens_out=8 msgs=8 dropped=0 ttft_ms=42 decode_ms=180 session=0 prompts=1
```

A prompt that couldn't start:

```
rc=-1 err=no-model stop=none tokens_in=0 tokens_out=0 msgs=0 dropped=0 prompts=1
```

| Field | Meaning |
|-------|---------|
| `rc` | 0 if the prompt ran, even if stopped early. -1 on a decoder error such as invalid UTF-8. -3 (`SLM_ERR_BUSY`) if a `slm prompt` was running |
| `err` | Only when it couldn't start: `no-model`, `bad-handle`, `no-gpu-program` (`--gpu on` with no GPU program; the UART line says why), `open-failed` |
| `stop` | `eos`, `max`, `stopped`, `callback`, `ctx-full`, `no-logits`, `empty`, `none` |
| `tokens_in` | Tokens fed to the model, chat template included |
| `tokens_out` | Tokens generated |
| `msgs` | `/slm/token` messages published. A client that counted fewer lost some |
| `dropped` | Times a subscriber's mailbox stayed full for the whole wait, over all subscribers |
| `ttft_ms`, `decode_ms` | Time to first token; time in the decode loop after it |
| `prompts` | Prompts handled since this runner started |

---

## Delivery

Tokens are published with `msg_router_publish_reliable`. The mailbox holds one
message per subscriber and a publish waits for it to be read, so a slow reader
slows the decoder and loses nothing. Order on `/slm/token` is publication
order, and `/slm/done` comes last because each token was acked first.

The wait has a limit. A reader that takes more than 30 s over one token loses
the next one, which shows in `dropped=`, and for the rest of that prompt the
runner waits 1 s per token. That keeps a subscriber that died without
unsubscribing from wedging the service. Other subscribers still get everything.

A client on a higher priority than the runner must sleep while it waits, not
yield: the runner runs at the shell's priority, and a yield doesn't let a
lower-priority task run. `msg_router_publish_reliable` already does this when
the runner is the one waiting.

There are no request ids. One prompt runs at a time, and a client that wants a
clean reply waits for `/slm/done` before publishing again. `slm ask` refuses
while the runner is busy. A prompt published anyway waits in the mailbox until
the current one ends; a stop leaves it there.

---

## Step 1: Load a Model

On Jetson after a cold UEFI-direct boot the model comes from the boot-time
delivery in `docs/deploy/jetson-uefi-direct.md`. Otherwise:

```
slmos> slm xload qwen <total_bytes>
slmos> slm load /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf
```

The runner doesn't need a model to start. Without one, a prompt gets
`rc=-1 err=no-model`, and a model loaded later is picked up by the next prompt
with no restart.

---

## Step 2: Start the Runner

```
slmos> slm runner start --gpu auto
Component 'slm-runner' v1.0.0 started (idx=5, task=12)
[slm-runner] Started (component 5, session 0, model 0), subscribed to /slm/prompt
slm-runner: running component=5 session=0 model=0 gpu=auto busy=0 prompts=0 dropped=0
```

`slm runner start` takes every session option of `slm launch` (`--ctx
--sampler --temp --topk --topp --seed --gpu --threads`), applied through the
same `rust_slm_session_configure` call, plus:

| Option | Meaning | Default |
|--------|---------|---------|
| `--handle H` | Always use slot H | The most recently loaded model |
| `--max N` | Tokens per prompt, 0 for no limit | 256 |
| `--raw` | Tokenize prompts as given instead of rendering the chat template | Chat template |

Other defaults: `--ctx 4096`, sampler `topkp` with temperature 0.7, top-k 40,
top-p 0.9, seed 1, `--gpu auto`.

`component run slm-runner` also starts it, with the options of the last `slm
runner start`, or the defaults if there hasn't been one.

**Which model.** Without `--handle`, each prompt uses the model loaded most
recently that's still loaded: not slot 0, and not the highest slot. If that
isn't the model the open session is on, or its slot was reloaded, the runner
closes the session and opens a new one. With `--handle H` an empty slot H gets
`rc=-1 err=bad-handle`.

**What keeps it running, and what restarts it.** The runner has no idle
timeout. It runs until `slm runner stop`, or until its task is killed from
outside. Nothing starts or restarts it on its own: not boot, not a crash. `slm
runner start` or `component run slm-runner` does.

**One conversation per prompt.** The runner resets the session before each
prompt, so one client's text can't reach another's reply and the context can't
fill up over time. A client that wants a conversation uses `slm launch` and
`slm prompt`.

---

## Step 3: Ask It

`slm ask` is a client that only uses the bus. It subscribes to `/slm/token` and
`/slm/done`, publishes the prompt, prints what arrives, and compares what it
counted with `msgs=`:

```
slmos> slm ask "Explain RAII in one sentence."
RAII ties a resource's lifetime to an object's scope...
[slm done] rc=0 stop=eos tokens_in=38 tokens_out=21 msgs=21 dropped=0 ttft_ms=3100 decode_ms=52000 session=0 prompts=1
[slm ask] received 21 of 21 token messages
```

Ctrl-C publishes `/slm/stop` and keeps reading to `/slm/done`. `--timeout S`
sets how long to go without a message before giving up (default 600 s, enough
for a CPU prefill of a long prompt). It returns an error if the runner refused
the prompt, if tokens went missing, or on a timeout.

Without a client, `msg send /slm/prompt "Say hello"` starts a prompt too, and
`[slm-runner] prompt #1 rc=0 tokens=12` shows up on the UART when it ends.

---

## Step 4: A Lua Client

```lua
-- Publish a prompt and stream the response.
local done = nil

slm.msg_subscribe("/slm/token", function(_, text) io.write(text) end)
slm.msg_subscribe("/slm/done",  function(_, stats) done = stats end)

slm.msg_publish("/slm/prompt", "Explain RAII in one sentence.")
while not done do
    slm.yield()
end
print()
print("[done] " .. done)
```

Subscriptions are delivered on the script's own task at `slm.yield()`,
`slm.sleep()` and `slm.msg_drain()`, so a script holding back from those holds
back the decoder.

---

## Step 5: Stop

```
slmos> msg send /slm/stop now      # the running prompt
slmos> slm stop 0                  # the same, by session id
slmos> slm runner status
slmos> slm runner stop             # the runner: ends any prompt, closes the session
```

---

## Limitations

- **In-kernel only**, as above.
- **No request ids.** Two clients at once see each other's tokens.
- **511-byte prompts.** The bus carries C strings up to that size.
- **Pinned to CPU 0** with the shell, like every component. The decode shares
  that CPU with the shell at the same priority.

---

## Source Files

| File | Purpose |
|------|---------|
| `kernel/src/slm_runner.c` | Component entry point, token stream, `slm ask` client |
| `kernel/include/slm_runner.h` | Topics, configuration, status, router additions |
| `kernel/src/slm_shell.c` | `slm runner` and `slm ask` verbs |
| `kernel/src/component_runtime.c` | Built-in registration in `builtin_components[]` |
| `runtime/src/msg_router.rs` | Topics, mailboxes, `msg_router_publish_reliable` |
| `runtime/src/slm/decoder.rs` | Rust-side prefill and decode loop |
| `kernel/tests/test_slm_runner.c` | Tests: bus path end to end |

See [`docs/design/slm-integration.md`](../design/slm-integration.md)
"Component Integration" for the design rationale and
[`docs/plans/slm-integration-plan.md`](../plans/slm-integration-plan.md)
M8 for the delivery context.

---

*Last updated: November 2026*
