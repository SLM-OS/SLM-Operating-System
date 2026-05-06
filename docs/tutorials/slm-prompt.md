# Tutorial: Running `slm prompt`

User-facing walkthrough for the `slm` shell verb family on Jetson
Orin Nano. Covers loading a GGUF, opening a session, sampling
configuration, prompt streaming, telemetry, and the `slm-runner`
component path.

**Status (2026-04-27):** every plumbing step in this tutorial works
end-to-end, but real text generation is gated on **M5.3** — see the
"What you'll see today" section below before running through the
demo expecting coherent output.

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
[slm] loaded handle=0  arch=qwen2  blocks=28 hidden=1536
      head=12/2 head_dim=128 vocab=152064 ctx=32768 source=1014 MB

slmos> slm list
  0  qwen2  vocab=152064  source=1014 MB

slmos> slm info 0
arch         : qwen2
block_count  : 28
embedding    : 1536
head_count   : 12
head_count_kv: 2
head_dim     : 128
ff_length    : 8960
context_len  : 32768
vocab_size   : 152064
tensor_count : 339
source_bytes : 1014 MB
rope_freq    : 1000000.0

slmos> slm launch 0 --ctx 4096 --sampler topkp --temp 0.7 --topk 40 --topp 0.9
[slm] session=0 backend=CPU(M5.2 stub) kv_cache=234 MB ctx=4096

slmos> slm prompt 0 "Explain virtual memory in two sentences."
[output streams here, character by character]
[slm] ttft=312 ms  decode=27.4 tok/s  prefill=146 tok/s  peak=1.34 GB

slmos> slm stats 0
session 0  prompts=1  tokens_in=11  tokens_out=58
  prefill  : 146 tok/s   75 ms total
  decode   :  27.4 tok/s  2117 ms total  (avg 36.5 ms / token)
  ttft     : 312 ms
  kv_used  : 69 / 4096 (1.7%)
  oov      : 0

slmos> slm close 0
slmos> slm unload 0
```

## Verb reference

| Verb | Behavior |
|------|----------|
| `slm load <path>` | Read VFS file → register in slot table. Print model handle + summary. |
| `slm list` | Iterate registered models. |
| `slm info <handle>` | Pretty-print the full `SlmModelInfoC` snapshot. |
| `slm launch <handle> [--ctx N] [--sampler …] [--temp F] [--topk N] [--topp F] [--seed N]` | Open a session over a registered model; allocate KV cache. |
| `slm prompt <session> "<text>"` | Tokenize, prefill, decode; stream tokens to UART. |
| `slm stream <session>` | Read a multi-line prompt from stdin until a blank line, then run as `slm prompt`. |
| `slm stop <session>` | Cooperative cancel — sets the session's stop flag; the decoder notices at the next yield boundary. Useful from a second UART session if a long generation is running. |
| `slm reset <session>` | Clear KV cache for a fresh conversation; keep the model loaded. |
| `slm unload <handle>` | Release the model slot. |
| `slm close <session>` | Release the session slot. |
| `slm status` | One-line summary: model count + session count + pool utilization. |
| `slm stats <session>` | Last-prompt + cumulative telemetry. |
| `slm gpu` | Currently prints "GPU dispatch not yet wired (M6.A-3 deferred)". The structural scaffolding shipped in M6.1; the SASS kernel authoring + L4T-side loader are tracked separately. |

## Sampling options

| Sampler | Flags | When to pick |
|---------|-------|--------------|
| `greedy` | (none) | Deterministic. Good for testing, regression checks, or when you need bit-stable output. |
| `temp`   | `--temp F` | Softmax with `logit / T`. T → 0 acts like greedy; T → ∞ approaches uniform. Demo default is `0.7`. |
| `topk`   | `--temp F --topk N` | Mask all but the top-N highest-logit tokens before softmax. Useful when the long tail produces incoherent words. |
| `topp`   | `--temp F --topp F` | Nucleus sampling: keep the smallest prefix whose softmax sum reaches `p`. Demo default is `0.9`. |
| `topkp`  | `--temp F --topk N --topp F` | Top-K then top-P then temperature — the demo default in the spec; balances coherence and diversity. |

## Cooperative cancel

Long generations can be aborted by sending `slm stop <session>` from
a **second UART session** to the same SLM-OS instance. The decoder
checks the cooperative-cancel flag at every per-token yield boundary,
so cancellation latency is at most one token of compute (typically
sub-second for the decode loop).

## The `slm-runner` component path

For programmatic use (Lua scripts, multi-session pipelines, or any
caller that prefers pub/sub over the shell), `component run
slm-runner` opens the same session machinery and wires it to the
message router:

| Topic           | Direction | Payload                                      |
|-----------------|-----------|----------------------------------------------|
| `/slm/prompt`   | inbound   | UTF-8 prompt string                          |
| `/slm/token`    | outbound  | UTF-8 token bytes (one per token)            |
| `/slm/done`     | outbound  | `rc=<n> tokens_out=<n> prompts=<n>`          |

A short Lua publisher example lives at `scripts/slm-chat-demo.lua`
(if shipped) or in the `slm-runner` tutorial at
`docs/tutorials/slm-component.md`.

## What you'll see today

The full plumbing is in place from `slm load` through token streaming
on UART, but the per-layer forward pass is a documented **stub**
(M5.2). With zero-logit greedy sampling the decoder reliably emits
token id 0 (typically a special token) for every step until
`max_new_tokens` is hit. The stats line still populates — `ttft_ns`,
`decode_ns`, `tokens_out` — so the telemetry path is exercised.

Real coherent text waits on **M5.3**: the registry needs to keep the
parsed GGUF buffer alive (currently metadata-only), and the
forward_step needs to walk the M4 op chain over the actual weight
tensors. That work is filed as a follow-up issue and is the next
visible deliverable on the SLM track.

## See also

- `docs/specs/slm-integration.md` — full integration spec
- `docs/plans/slm-integration-plan.md` — milestone-by-milestone plan
- `docs/tutorials/slm-models.md` — fetching and staging GGUF files
- `docs/tutorials/slm-component.md` — the `slm-runner` path
- `docs/setup.md` §"Jetson SD-Card Layout" — where models live on disk

*Last updated: 27 April 2026*
