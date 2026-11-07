# Tutorial: Running `slm prompt`

User-facing walkthrough for the `slm` shell verb family on Jetson
Orin Nano. Covers loading a GGUF, opening a session, sampling
configuration, prompt streaming, telemetry, and the `slm-runner`
component path.

## Prerequisites

- A Jetson Orin Nano running SLM-OS post-kexec (see
  `docs/jetson-boot.md`).
- A GGUF v3 model. On Jetson the recommended path is `slm xload`,
  which streams the file straight into a single PMM buffer over the
  telnet shell — see `docs/setup.md` §"Jetson SD-Card Layout" for the
  rationale (the LittleFS round-trip needs two order-19 buddies that
  Jetson's 8 GB system can't produce simultaneously). On platforms
  that fit `slm load`'s alloc+copy pattern, a GGUF can also be staged
  at `/mnt/files/<name>.gguf` and loaded by path.
- Per-platform model-memory pool sizing wired in M0.2 — Jetson's
  defaults (2 GB weight, 256 MB workspace) cover Qwen2.5-1.5B-Q4_K_M.

## End-to-end flow

```
# Jetson (recommended): stream-load via the telnet shell.
slmos> slm xload qwen <total_bytes>
SLM-XLOAD ready name=qwen total=<total_bytes>
... (1 GB streamed over the wire) ...
SLM-XLOAD done received=<total_bytes>
[slm] loaded handle=0  arch=qwen2  blocks=28 hidden=1536

# Or, on platforms with enough contiguous PMM headroom:
slmos> slm load /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf
[slm] loaded handle=0  arch=qwen2  blocks=28 hidden=1536 head=12/2 head_dim=128 vocab=151936 ctx=32768 source=1065 MB

slmos> slm list
  0  qwen2  vocab=151936  source=1065 MB

slmos> slm info 0
arch         : qwen2
block_count  : 28
embedding    : 1536
head_count   : 12
head_count_kv: 2
head_dim     : 128
ff_length    : 8960
context_len  : 32768
vocab_size   : 151936
tensor_count : 339
source_bytes : 1065 MB
rope_freq    : 1000000.0

slmos> slm launch 0 --ctx 4096 --sampler topkp --temp 0.7 --topk 40 --topp 0.9 --gpu auto
[slm] session=0 backend=CPU(no GPU ops available) threads=4(stored; forward is single-threaded) sampler=topkp ctx=4096 temp=0.700 top_k=40 top_p=0.900 seed=1

slmos> slm prompt 0 "Explain virtual memory in two sentences."
[output streams here, character by character]
[slm] ttft=<ms> ms  decode=<rate> tok/s  total=<ms> ms tokens=<n> bytes=<n> stop=eos

slmos> slm stats 0
session 0  prompts=1  tokens_in=<n>  tokens_out=<n>
  ttft     : <ms> ms
  decode   : <ms> ms total
  stop     : eos
  kv_used  : <n> / 4096
  backend=CPU(no GPU ops available)
  threads  : 4 (stored; forward is single-threaded)

slmos> slm close 0
slmos> slm unload 0
```

## Verb reference

| Verb | Behavior |
|------|----------|
| `slm load <path>` | Read VFS file → register in slot table. Print model handle + summary. |
| `slm list` | Iterate registered models. |
| `slm info <handle>` | Pretty-print the full `SlmModelInfoC` snapshot. |
| `slm launch <handle> [--ctx N] [--sampler …] [--temp F] [--topk N] [--topp F] [--seed N] [--gpu auto\|cpu\|on] [--threads K]` | Open a session over a registered model; allocate KV cache. The banner states the real backend. |
| `slm prompt <session> [--raw] [--max N] "<text>"` | Render the chat template, tokenize, prefill, decode; stream the reply to UART. |
| `slm stream <session> [--raw] [--max N]` | Read a multi-line prompt from stdin until a blank line, then run as `slm prompt`. |
| `slm stop <session>` | Stop a running prompt from another shell session. Takes no lock, so it never waits for the prompt. Says so if nothing is running. |
| `slm reset <session>` | Clear KV cache for a fresh conversation; keep the model loaded. Refused while a prompt runs on the session. |
| `slm unload <handle>` | Release the model slot. Refused while a prompt runs on the model. |
| `slm close <session>` | Release the session slot. Refused while a prompt runs on it. |
| `slm status` | One-line summary: model count + session count + pool utilization. |
| `slm stats <session>` | Last-prompt + cumulative telemetry. |
| `slm gpu` | Per-op tier and dispatch counters. |

Only one prompt runs at a time, across all sessions; a second gets a clear
error. `slm stats`, `slm status`, `slm list`, `slm info` and `slm gpu` answer
while a prompt runs. `slm stats` then shows the figures from before it started.

## Launch options

`--gpu` selects where ops run:

| Mode | Behavior |
|------|----------|
| `auto` (default) | Follow the tier table, falling back to the CPU per op. |
| `cpu` | Hold every op on the CPU for the duration of each prompt. |
| `on` | As `auto`, but `slm launch` fails if no op has a GPU tier. |

`--threads K` (1 to 64, default 4) is stored on the session, and has no effect
yet: the forward path is single-threaded until the worker pool lands. The banner
and `slm stats` say so.

## Prompt text and options

The shell parser handles quotes for every command (#1136). A word starting
with `"` or `'` runs to the matching quote, spaces included. Inside `"..."`,
`\"` and `\\` are a literal quote and backslash; `'...'` is fully literal. A
quote in the middle of a word is an ordinary character, so `What's` is one word.
An unterminated quote, or more than 16 words on the line, is an error and runs
nothing. Prompts of more than a dozen words therefore need quotes.

By default the text becomes a user turn in the ChatML template, with the model's
default system prompt on the first turn of a conversation:

```
<|im_start|>system
You are Qwen, created by Alibaba Cloud. You are a helpful assistant.<|im_end|>
<|im_start|>user
{text}<|im_end|>
<|im_start|>assistant
```

Special-token names inside the text stay plain text, so a prompt can't forge a
turn boundary.

| Option | Behavior |
|--------|----------|
| `--raw` | Feed the text as given, with no template. Special-token names such as `<\|im_start\|>` become their single token ids. For pre-rendered prompts, such as the 64-token benchmark prompts. |
| `--max N` | Stop after N generated tokens. Default 256; 0 means until EOS or the context fills. |
| `--` | End of options, for text that starts with dashes. |

Options go before the text or at the very end of the line. Since a shell line
can't hold a newline, `slm prompt` turns `\n`, `\t` and `\\` in the text into a
newline, a tab and a backslash, with or without `--raw`:

```
slmos> slm prompt 0 --raw --max 32 "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n"
```

`slm stream` lines can hold real newlines, so it does no unescaping.

Generation ends on the GGUF's `tokenizer.ggml.eos_token_id`, `<|im_end|>` or
`<|endoftext|>`, whichever comes first. The end token isn't printed or counted.
The `stop=` field of the summary line gives the reason:

| `stop=` | Meaning |
|---------|---------|
| `eos` | The model sampled an end token. |
| `max` | Reached `--max`. |
| `stopped` | `slm stop` or Ctrl-C. |
| `ctx-full` | The context is full, or the prompt didn't fit and nothing ran. |
| `no-logits` | The forward pass produced nothing, e.g. a GGUF without layer weights. |

Output arrives one token at a time and is flushed after each. A character split
across tokens is held until it's complete, so a multi-byte sequence is never
printed in pieces. Output that ends in the middle of one prints U+FFFD.

Prompts on one session continue one conversation until `slm reset`.

## Sampling options

| Sampler | Flags | When to pick |
|---------|-------|--------------|
| `greedy` | (none) | Deterministic. Good for testing, regression checks, or when you need bit-stable output. |
| `temp`   | `--temp F` | Softmax with `logit / T`. T → 0 acts like greedy; T → ∞ approaches uniform. Demo default is `0.7`. |
| `topk`   | `--temp F --topk N` | Mask all but the top-N highest-logit tokens before softmax. Useful when the long tail produces incoherent words. |
| `topp`   | `--temp F --topp F` | Nucleus sampling: keep the smallest prefix whose softmax sum reaches `p`. Demo default is `0.9`. |
| `topkp`  | `--temp F --topk N --topp F` | Top-K then top-P then temperature — the demo default in the spec; balances coherence and diversity. |

## Stopping a prompt

A running prompt can be stopped two ways:

- Ctrl-C on the shell session running it. Other bytes typed during a decode stay
  queued for the next command.
- `slm stop <session>` from a second shell session, such as telnet while the
  prompt runs on the UART.

The decoder checks before every forward step, prefill included, so a stop takes
effect within one token of compute.

A stopped session takes new prompts without `slm reset`. The stop lands between
forward steps, so the KV cache holds only complete positions, and the next prompt
continues the conversation. `slm reset` starts a fresh one.

## The `slm-runner` component path

For programmatic use (Lua scripts, multi-session pipelines, or any
caller that prefers pub/sub over the shell), `component run
slm-runner` opens the same session machinery and wires it to the
message router:

| Topic           | Direction | Payload                                      |
|-----------------|-----------|----------------------------------------------|
| `/slm/prompt`   | inbound   | UTF-8 prompt string, rendered as a chat turn |
| `/slm/token`    | outbound  | UTF-8 token bytes (one per token)            |
| `/slm/done`     | outbound  | `rc=<n> tokens_out=<n> prompts=<n>`          |

A short Lua publisher example lives at `scripts/slm-chat-demo.lua`
(if shipped) or in the `slm-runner` tutorial at
`docs/tutorials/slm-component.md`.

## See also

- `docs/design/slm-integration.md` — full integration spec
- `docs/plans/slm-integration-plan.md` — milestone-by-milestone plan
- `docs/tutorials/slm-models.md` — fetching and staging GGUF files
- `docs/tutorials/slm-component.md` — the `slm-runner` path
- `docs/setup.md` §"Jetson SD-Card Layout" — where models live on disk

*Last updated: 6 November 2026*
