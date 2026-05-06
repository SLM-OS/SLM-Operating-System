# SLM Integration on Jetson Orin Nano — Fact Sheet

End-to-end support for a real generative Small Language Model on the Jetson
Orin Nano Super Dev Kit: model upload, CLI launch, prompt → response, live
performance monitoring, and GPU-accelerated inference.

This builds on Phase 5 (which delivered the ONNX loader, CPU inference engine,
model registry, AI scheduler, and CACHEUS eviction for *MNIST-class* models)
and extends the runtime to handle decoder-only transformers with autoregressive
decode, KV cache, INT4 quantization, and a tokenizer.

---

## Target SLM

**Primary:** **Qwen2.5-1.5B-Instruct** (Alibaba, Apache-2.0), quantized to
**Q4_K_M GGUF** (~1.0 GB on disk, ~1.0 GB resident weights).

**Backup:** **Llama-3.2-1B-Instruct** (Meta, Llama-3.2 community license),
Q4_K_M GGUF (~0.8 GB). Selected if the Qwen tokenizer (BBPE with byte
fallback) becomes a schedule risk; Llama-3.2's tokenizer is also BBPE but
with a smaller vocab (128 K vs Qwen's 152 K).

### Why Qwen2.5-1.5B-Instruct

| Constraint | Qwen2.5-1.5B fit |
|---|---|
| 8 GB unified memory (≈6.7 GB usable post OP-TEE) | Weights ~1.0 GB Q4_K_M; KV cache @ 4K ctx ≈ 230 MB FP16; workspace ~150 MB; total <1.4 GB resident — leaves ≥4 GB for OS, second model slot, and headroom |
| 102 GB/s LPDDR5 bandwidth (the actual bottleneck during decode) | At Q4_K_M, decode reads ~1.0 GB/token-step → theoretical ceiling ~100 tok/s; realistic ~25–35 tok/s on Orin Nano |
| Architecturally simple — every op must be hand-implemented | Standard decoder-only LLaMA-family architecture: 28 layers, hidden 1536, GQA (12 query / 2 KV heads), head_dim 128, SwiGLU MLP (intermediate 8960), RMSNorm, RoPE (theta 1 000 000), tied embeddings off |
| Strong instruction-following at small size | Qwen2.5 1.5B-Instruct beats Llama-3.2 1B and TinyLlama on MMLU, GSM8K, IFEval; close to Phi-3-mini at half the parameter count |
| GGUF availability | First-party GGUFs at `Qwen/Qwen2.5-1.5B-Instruct-GGUF`; widely mirrored |
| License | Apache-2.0 — clean for academic / capstone publication |
| Tokenizer | BBPE (152 064 merges); deterministic, no external dependencies once vocab is shipped |

### Architectural capabilities the runtime must add

The Phase 5 op set (MatMul, Add, Relu, Softmax, Reshape, Conv2D, MaxPool,
Gemm, Flatten) covers vision models. Qwen2.5 (and any LLaMA-family SLM)
additionally requires:

- **RMSNorm** (replaces LayerNorm; cheaper, no bias, no mean subtraction)
- **RoPE** (rotary position embedding applied to Q, K)
- **GroupedQueryAttention** with **causal mask** and **KV-cache append**
- **SwiGLU MLP** (`down(silu(gate(x)) * up(x))`)
- **SiLU** activation (a.k.a. Swish)
- **Embedding lookup** (gather over a 152 064 × 1536 table)
- **LM head** (final linear projecting to vocab; reuses MatMul)
- **INT4 dequantization kernels** for Q4_K_M weight blocks (Q4_0 as an
  optional simpler fallback during bring-up)

---

## Capability Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson Orin Nano | x86-64 |
|---|---|---|---|---|
| GGUF model upload via VFS | ✅ goal (via existing `model load`) | ✅ goal (constrained by RAM) | ✅ **primary target** | 🟡 if VFS reachable |
| INT4 / Q4_K_M weight format | ✅ goal | ✅ goal | ✅ goal | 🟡 |
| Transformer op set (RMSNorm, RoPE, GQA, SwiGLU) | ✅ goal | ✅ goal | ✅ goal | 🟡 SSE port |
| KV-cache backed by `model_mem` workspace pool | ✅ goal | ✅ goal | ✅ goal | 🟡 |
| BBPE tokenizer (Qwen vocab) | ✅ goal | ✅ goal | ✅ goal | 🟡 |
| Autoregressive decode loop | ✅ goal | ✅ goal | ✅ goal | 🟡 |
| Streaming output to UART (token-by-token) | ✅ goal | ✅ goal | ✅ goal | 🟡 |
| `slm load / launch / prompt / status / unload` shell verbs | ✅ goal | ✅ goal | ✅ goal | 🟡 |
| CPU NEON kernels (Q4 dot, RMSNorm, RoPE, SwiGLU) | ✅ goal | ✅ goal | ✅ goal | n/a (use SSE) |
| GPU acceleration via kexec-handoff bridge (Option C) | ❌ N/A | ❌ N/A | ✅ **goal — extends MNIST bridge** | ❌ N/A |
| GPU acceleration native (in-house GA10B driver, Option A) | ❌ N/A | ❌ N/A | 🟡 stretch, follows nvgpu phases 6–8 | ❌ N/A |
| **GPU tensor-core (HMMA FP16) utilization** for matmul-heavy ops | ❌ N/A | ❌ N/A | ✅ **goal — main-line** | ❌ N/A |
| GPU tensor-core (IMMA INT8) end-to-end | ❌ N/A | ❌ N/A | ⏸️ deferred follow-up (needs INT8 activations) | ❌ N/A |
| Per-token latency / TTFT / throughput telemetry | ✅ goal | ✅ goal | ✅ goal | 🟡 |
| Big.LITTLE-aware decode-thread placement | ❌ uniform | ❌ uniform | ✅ goal (6× A78AE, dual-cluster) | n/a |
| LRU model eviction (reuses Phase-5 registry) | ✅ inherited | ✅ inherited | ✅ inherited | ✅ inherited |
| `slm-runner` component (EL0-isolated decode service) | ✅ goal | 🟡 inherited isolation status | 🟡 inherited isolation status | ❌ EL0 unwired |
| Sampler (temperature, top-k, top-p) | ✅ goal | ✅ goal | ✅ goal | 🟡 |
| Chat template renderer (Qwen ChatML) | ✅ goal | ✅ goal | ✅ goal | 🟡 |

Legend: ✅ in-scope deliverable for this spec · 🟡 partial / inherited /
deferred · ❌ explicitly out-of-scope on this platform

---

## High-Level Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│  Shell (kernel/src/shell_*.c)                                      │
│  slm load / launch / prompt / stream / stop / status / unload      │
└──────────────┬─────────────────────────────────────────────────────┘
               │ FFI:  rust_slm_*  (extends rust_model_* / rust_infer_*)
┌──────────────▼─────────────────────────────────────────────────────┐
│  runtime/src/slm/                                  (new subsystem) │
│  ┌──────────────┐ ┌─────────────────┐ ┌─────────────────────────┐  │
│  │ gguf_parser  │ │ tokenizer (BBPE)│ │ chat_template (ChatML)  │  │
│  └──────┬───────┘ └────────┬────────┘ └────────────┬────────────┘  │
│         │                  │                       │               │
│  ┌──────▼──────────────────▼───────────────────────▼────────────┐  │
│  │ session   – holds: model handle, KV cache, sampler,          │  │
│  │             token stream, decode telemetry                   │  │
│  └──────────────────────────┬───────────────────────────────────┘  │
│                             │                                      │
│  ┌──────────────────────────▼───────────────────────────────────┐  │
│  │ decoder  – prefill + autoregressive decode loop              │  │
│  │           dispatches each layer through transformer_ops      │  │
│  └──────────────────────────┬───────────────────────────────────┘  │
│                             │                                      │
│  ┌──────────────────────────▼───────────────────────────────────┐  │
│  │ transformer_ops (extends inference/ops.rs)                   │  │
│  │ rmsnorm · rope · gqa · swiglu · q4_dot · embed · lm_head     │  │
│  │ backend dispatch:  CPU NEON  ─or─  GPU (via gpu.rs hooks)    │  │
│  └─────────────┬─────────────────────────────┬───────────────────┘ │
└────────────────┼─────────────────────────────┼─────────────────────┘
                 │                             │
        ┌────────▼─────────┐         ┌─────────▼──────────────────┐
        │ kernel CPU + NC  │         │ kernel/gpu/nvidia/ga10b    │
        │ memory + sched   │         │ + kexec-handoff bridge     │
        └──────────────────┘         │ (PR #376; extended for SLM)│
                                     └────────────────────────────┘
```

The shaded surfaces preserve every Phase-5 invariant: weights live in the
2 MB-aligned weight pool, KV cache and per-layer scratch live in the
workspace pool, the registry's pin/unpin semantics protect a launched session
from eviction, and the session is an opaque handle accessed through FFI just
like a `model_idx` today.

---

## Inference Runtime Stack

| Layer | Component | Sourcing |
|---|---|---|
| GGUF v3 parser | `runtime/src/slm/gguf.rs` | New, in-house. ~600 LOC. Header + KV metadata + tensor descriptors + mmap-style zero-copy weight slice. |
| Tokenizer (BBPE) | `runtime/src/slm/tokenizer.rs` | New, in-house. Vocab + merges loaded from GGUF metadata; byte-fallback path for unknown bytes. ~400 LOC. |
| Chat template | `runtime/src/slm/chat_template.rs` | Hard-coded Qwen ChatML format (`<|im_start|>` / `<|im_end|>`). ~80 LOC. |
| Transformer ops (CPU) | `runtime/src/inference/ops_transformer.rs` | New. RMSNorm, RoPE, GQA, SwiGLU, Q4 dot. NEON-accelerated where it matters. ~1200 LOC. |
| Q4 quant kernels | `runtime/src/inference/quant.rs` | **Vendored, audited subset of GGML's `ggml-quants.c`** — specifically `dequantize_row_q4_K`, `vec_dot_q4_K_q8_K`, and `quantize_row_q8_K`. Re-license-compatible (MIT). ~800 LOC, no libc, no malloc. |
| KV cache | `runtime/src/slm/kv_cache.rs` | New. FP16 K and V tensors per layer in a single workspace allocation; ring-buffer indexing for sliding-window prefills. ~250 LOC. |
| Sampler | `runtime/src/slm/sampler.rs` | New. Greedy + temperature + top-k + top-p. Deterministic seed for reproducibility. ~150 LOC. |
| Session state machine | `runtime/src/slm/session.rs` | New. Owns model handle, tokenizer, KV cache, sampler, telemetry. ~400 LOC. |
| Decode loop | `runtime/src/slm/decoder.rs` | New. Prefill (chunked) → autoregressive decode → token callback. Cooperative `yield()` between tokens for scheduler fairness. ~300 LOC. |
| GPU backend hooks | extends `runtime/src/inference/gpu.rs` | Reuses Phase-5 framework. `select_backend()` extended with model-size + layer-type heuristics. |

**Total new Rust:** ~4 200 LOC, all `#![no_std]`, all using existing kernel
FFI for memory and time.

### Why not pull in llama.cpp wholesale

Direct reasons:

- llama.cpp depends on libc, malloc, threads, and POSIX file I/O — all
  forbidden by `kernel/CLAUDE.md` and `runtime/CLAUDE.md`.
- It assumes a hosted runtime (stdio, errno, mmap). Porting the whole
  surface would mean shimming half a libc.
- The Phase-5 inference engine, registry, model memory pools, and op
  dispatch already work and have tests. Bolting llama.cpp alongside would
  create two parallel inference stacks.

**What we *do* take from llama.cpp / GGML:**

- The **GGUF v3 file format** (well-documented, language-agnostic). We write
  our own parser.
- The **Q4_K block layout** and **vec_dot_q4_K_q8_K** scalar/NEON kernels —
  these are tightly-tuned, well-tested, and directly droppable into a
  `no_std` Rust file once translated. License (MIT) is compatible.
- The **prompt format** for Qwen ChatML.

This keeps the OS bare-metal pure while standing on the shoulders of the
quantization work.

---

## Memory Plan (Jetson Orin Nano, 8 GB)

| Region | Size | Pool | Notes |
|---|---|---|---|
| Kernel + runtime + stacks + non-cacheable | ~80 MB | (existing) | Unchanged from Phase 5. |
| OP-TEE carveout (skipped by PMM) | 64 MB | n/a | `0xBE000000–0xC2000000`, see `docs/specs/memory.md`. |
| **Weight pool** | **1 GB** | Phase 3 weight pool, raised from 256 MB | Holds 1× Q4_K_M Qwen2.5-1.5B (~1.0 GB) — fills the pool. PMM buddy max-order is 1 GiB (#578); a multi-block allocator (#550) is the path to two-model headroom. |
| **Rust heap** | **128 MB** | `linked_list_allocator` over PMM | Hosts KV cache (~56 MB at ctx=2048 for Qwen2.5-1.5B) + `ForwardScratch` (~1 MB). Sized via `RUST_HEAP_MB` in `config.h`. |
| **KV-cache pool** *(future)* | **512 MB** | Planned sub-pool inside workspace allocator | Sized for two concurrent sessions × 4 K context × Qwen2.5-1.5B GQA dims (≈230 MB each). Today the KV cache lives on the Rust heap; this row tracks the eventual move into a dedicated pool with eviction integration. |
| Workspace (per-session scratch) | 256 MB | Phase 3 workspace pool | Per-layer activations, attention logits, RoPE LUTs. |
| Free / future | ~4.5 GB | buddy | Headroom for additional models, components, page cache. |

Weights are **memory-mapped from GGUF directly into the weight pool** at
load time (no decode pass; Q4_K blocks stay packed). Dequantization happens
on-the-fly during MatMul into a transient FP16 column tile in workspace,
which is the GGML pattern and keeps weight-side bandwidth at ~½ the FP16
cost.

> **Plumbing landed in M0.2:** `rust_model_mem_init` now takes
> `(uint32_t weight_mb, uint32_t workspace_mb)` and the kernel boot
> path passes `MODEL_MEM_WEIGHT_MB` / `MODEL_MEM_WORKSPACE_MB` from
> `<kernel/include/config.h>`. Per-platform defaults select 1 GB /
> 256 MB on Jetson, 512 MB / 256 MB on Pi 5, and the original 256 MB /
> 128 MB on QEMU and x86-64. The 1 GB Jetson cap is the PMM buddy
> max-order ceiling; bigger pools wait on the multi-block allocator
> in #550. The 512 MB KV-cache sub-pool is still M5 work; today the
> KV cache lives on the Rust heap.

---

## CLI Surface (Shell)

A new `slm` command family, registered via `shell_register_command()` from
`kernel/src/slm_shell.c`. Mirrors the Phase-5 `model` family but is
session-oriented and prompt-aware.

| Command | Behavior |
|---|---|
| `slm load <vfs-path>` | Parse GGUF, allocate weight blocks in weight pool, register model. Prints model handle, parameter count, file format, quant scheme, vocab size. Wraps `rust_slm_load(name, data, len)`. |
| `slm list` | Lists all loaded SLMs (handle, name, params, quant, weight bytes resident). |
| `slm info <handle>` | Detailed metadata: arch (`qwen2`), n_layer, n_head, n_kv_head, head_dim, hidden, vocab, RoPE theta, max ctx. |
| `slm launch <handle> [--ctx N] [--threads K] [--gpu auto\|cpu\|on]` | Creates a **session**: allocates KV-cache slot, pins the model in registry, returns a `session_id`. Default `--ctx 2048`, `--threads 4` (4 of 6 A78AE cores), `--gpu auto`. |
| `slm prompt <session_id> "<text>"` | Renders the chat template, tokenizes, runs prefill, runs decode to EOS or `--max 256`. Prints generated text **streaming** to UART, one token at a time. |
| `slm stream <session_id>` | Reads from stdin (UART line) until blank line, then runs prompt. Convenience for interactive use over serial console. |
| `slm stop <session_id>` | Cancels an in-flight decode (sets cooperative cancel flag; decoder checks at each `yield()` boundary). |
| `slm reset <session_id>` | Clears KV cache for a fresh conversation; keeps model loaded. |
| `slm unload <session_id\|--model <handle>>` | Releases session (frees KV slot) or model (unpins, allows LRU eviction). |
| `slm status` | One-line summary: loaded models, active sessions, pool utilization. |
| `slm stats [<session_id>]` | Last-prompt and cumulative stats (see "Telemetry" below). |
| `slm bench <handle> [--prompt-len N] [--gen-len M] [--runs R]` | Synthetic throughput benchmark: warm-up, R runs, reports prefill tok/s, decode tok/s, TTFT, peak-resident MB. |
| `slm gpu` | Inherits Phase-5 `model gpu` semantics; reports backend chosen for the active session and any kexec-handoff state. |

### Example session

```
# Jetson — stream the GGUF in over the telnet shell (avoids the
# alloc+copy doubling that the LittleFS path needs; see
# docs/setup.md §"Jetson SD-Card Layout"):
slmos> slm xload qwen <total_bytes>
SLM-XLOAD ready name=qwen total=<total_bytes>
... (1 GB streamed) ...
SLM-XLOAD done received=<total_bytes>
[slm] loaded handle=0  arch=qwen2  blocks=28 hidden=1536 ...

# Or, on a platform with the contiguous PMM headroom:
slmos> slm load /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf
[slm] parsed gguf v3: 339 tensors, q4_K_M, vocab=152064
[slm] loaded handle=0  qwen2  1.54 B params  weights=1014 MB  load=412 ms

slmos> slm launch 0 --ctx 4096 --gpu auto
[slm] session=0 backend=GPU(kexec-handoff) kv_cache=234 MB ctx=4096 threads=4

slmos> slm prompt 0 "Explain virtual memory in two sentences."
Virtual memory is an abstraction the OS provides so each process sees a
private, contiguous address space, while the actual physical pages may live
anywhere in RAM (or on disk). The MMU translates virtual to physical
addresses on every memory access using per-process page tables.
[slm] ttft=312 ms  decode=27.4 tok/s  prefill=146 tok/s  peak=1.34 GB

slmos> slm stats 0
session 0  prompts=1  tokens_in=11  tokens_out=58
  prefill  : 146 tok/s   75 ms total
  decode   :  27.4 tok/s  2117 ms total  (avg 36.5 ms / token)
  ttft     : 312 ms
  kv_used  : 69 / 4096 (1.7%)
  oov      : 0
```

---

## GPU Integration on Orin Nano

The Orin Nano's GA10B Ampere GPU has **1024 CUDA cores plus 32 third-gen
tensor cores**. There is **no NVDLA on the Nano variant** (the DLA is
fused off — present only on Orin NX and AGX Orin) and no separate TPU. So
the only AI silicon on the board is inside the GPU, and the headline
"67 TOPS sparse INT8" figure is dominated by tensor-core throughput. Any
serious SLM inference path must drive the tensor cores.

The integration uses two dispatch paths and a third fallback, with
tensor-core kernels as the **main-line** implementation across both GPU
paths.

### Path A — kexec-handoff bridge (primary, near-term)

The MNIST kexec-handoff bridge (`scripts/gpu-kernel-launch.c` + post-kexec
`nvgpu launch-kernel`, PR #376) is **already proven** end-to-end. We extend
it from "single-op MNIST kernel chain" to "transformer layer dispatch":

- A new handoff descriptor format `slm_gpu_handoff_v1` carries: model
  metadata, weight DMA addresses (already in unified memory), layer plan,
  KV-cache descriptors, and a doorbell page.
- Pre-kexec on L4T: build a SASS kernel library (see *Tensor-core
  utilization* below) covering `q4_K_dot_f16`, `q4_K_gemm_f16`,
  `rmsnorm_f16`, `rope_f16`, `gqa_attn_f16`, `swiglu_f16`, and
  `lm_head_f16` (LM-head MatMul + argmax/sample fused).
- Post-kexec in SLM-OS: the `slm` runtime submits each layer's pushbuffer
  using the inherited channel; the bare-metal driver only needs to drive the
  doorbell and poll the completion semaphore (already implemented for MNIST).

**Estimated effort:** 3–4 weeks of SASS-kernel authoring + handoff schema
extension. Risk: medium — the kernels are non-trivial but have llama.cpp's
CUDA implementations as reference.

### Path B — native bare-metal GA10B (stretch)

The native GPU bringup (`kernel/gpu/nvidia/ga10b_bringup.{c,h}`, phases 1–5
done, 6–8 in progress per `docs/archive/handoff/jetson-capstone-handoff.md`)
is the long-term home. When phases 6–8 (GMMU, channel/runlist, pushbuffer
construction) land, the **same SASS kernels** from Path A become directly
dispatchable without kexec dependence. The `slm` runtime treats this as a
backend swap behind `select_backend()`; user-visible behavior is unchanged.

### Tensor-core utilization (main-line across Paths A and B)

Every GPU kernel in this spec is authored in two tiers — tensor-core first,
CUDA-core fallback second — and the SASS library ships **both** so a tier-1
slip on any single kernel degrades gracefully to its tier-2 sibling without
losing the GPU path entirely.

**Tier 1 (tensor-core, FP16 — main-line):**

- All matmul-shaped kernels (`q4_K_dot_f16`, `q4_K_gemm_f16`, the QKV /
  output / gate / up / down projections inside `gqa_attn_f16` and
  `swiglu_f16`, and `lm_head_f16`) target Ampere's `HMMA.16816.F16.F16`
  instruction (the `m16n8k16` FP16-input/FP16-accumulate or
  FP16-input/FP32-accumulate tile op).
- Q4_K weights are dequantized into shared-memory FP16 tiles immediately
  before the HMMA issue, so the weight-side bandwidth advantage of Q4_K is
  preserved end-to-end.
- The `Q·Kᵀ` and `softmax(P)·V` matmuls inside fused attention are also
  HMMA tiles; only softmax/exp/normalize stays on CUDA cores.
- Authoring approach: write each kernel as CUDA C++ targeting `sm_87`
  using the `nvcuda::wmma` API (or inline PTX `mma.sync.aligned…` for the
  hottest paths), compile with `nvcc -arch=sm_87`, extract SASS via
  `cuobjdump --dump-sass`, and ship the SASS payload. Reference
  implementations cribbed from llama.cpp's `ggml-cuda` backend.

**Tier 2 (CUDA-core, FP16 — guaranteed fallback):**

- The same kernels in plain FP16 SIMT (`fma.f16x2`) form, no tensor-core
  instructions. Roughly 4–6× slower on matmul but architecturally simpler
  to author and debug.
- `select_backend()` is extended with a per-op **tier preference**:
  `Auto → Tier1 if available, else Tier2, else CPU`. Tier-1 availability
  is established at session-launch time by attempting a one-shot HMMA
  smoke kernel; failure (compile error, dispatch failure, silent
  miscompare against the reference vector) demotes that op to Tier 2 for
  the lifetime of the session and logs `slm gpu` accordingly.
- Per-op granularity means a single problematic Tier-1 kernel doesn't
  drag down the rest of the pipeline.

**Tier 3 (deferred follow-up): IMMA INT8 end-to-end.** The Ampere tensor
cores also support `IMMA.16832.S8.S8` for INT8 inputs with INT32
accumulate, which would roughly double Tier-1 throughput. Reaching it
requires INT8 *activations* (Q8_0 or AWQ-style symmetric per-channel),
calibration, and an attention path that survives the lower numerics. This
is explicitly **out of scope for v1** and listed under "Skipped / Blocked"
— the tier model leaves a clean insertion point (`Tier0_IMMA`) for it
later.

**Performance budget on Orin Nano (Qwen2.5-1.5B-Q4_K_M, dense FP16, 100 %
of one GPU):**

| Tier | Realistic sustained matmul throughput | Realistic decode rate |
|---|---|---|
| Tier 1 (HMMA FP16) — main-line | 10–15 dense FP16 TFLOPS | **25–35 tok/s** |
| Tier 2 (CUDA-core FP16) — fallback | 2–3 dense FP16 TFLOPS | 10–14 tok/s |
| Tier 3 (IMMA INT8) — future | 25–30 dense INT8 TOPS | 45–60 tok/s |

(The Orin Nano Super's 67 TOPS sparse-INT8 marketing number is the
theoretical Tier-3 ceiling under structured 2:4 sparsity. Realistic
sustained dense-FP16 utilization on tensor cores for transformer decode
is in the 30–50 % range, which gives the 10–15 TFLOPS Tier-1 figure.)

### Path C — CPU-only fallback (always available)

Pure NEON path on the 6-core A78AE. Realistic decode rate on
Qwen2.5-1.5B-Q4_K_M: **8–12 tok/s** based on published numbers for similar
SLMs on Orin Nano CPU. Acceptable for correctness-first bring-up and as a
permanent fallback if the GPU path is unavailable.

The runtime always launches with CPU as the safety net; `--gpu auto`
upgrades when a GPU backend is reachable.

---

## Telemetry & Performance Monitoring

Hooks into existing infrastructure (`task_cycles_on_cpu`, preempt log,
`InferenceStats`) and adds SLM-specific metrics. All measured per session,
aggregated across sessions for `slm status`.

| Metric | Source | Surface |
|---|---|---|
| **TTFT** (time-to-first-token, ms) | timer around tokenize → first sampled token | `slm prompt` final line, `slm stats` |
| **Prefill throughput** (tokens/s) | tokens prefilled / prefill wall-clock | `slm stats` |
| **Decode throughput** (tokens/s) | generated tokens / decode wall-clock | `slm stats`, `slm bench` |
| **Inter-token latency** (ms, min/avg/max) | per-token timer ring buffer (size 256) | `slm stats --verbose` |
| **KV utilization** (used / max ctx) | session counter | `slm stats`, `slm status` |
| **Peak resident MB** | `model_mem` watermark | `slm stats` |
| **Cache miss / eviction events** | inherits from registry counters | `slm status` |
| **Per-op latency breakdown** (rmsnorm / rope / gqa / mlp / lm_head) | optional `--profile` flag enables per-op `Timer::elapsed()` | `slm stats --profile` |
| **CPU cycle accounting** | inherited per-task `task_cycles_on_cpu` | `top`, `bench smp` |
| **GPU busy time** (Path A/B) | sample GA10B PTIMER (BAR0+0x9420) before/after each pushbuffer | `slm stats --gpu` |
| **Tensor-core utilization** (per-op tier in use; TC-busy fraction during matmul ops) | per-op tier flag set at session launch; tier-1 ops sample `sm__inst_executed_pipe_tensor` equivalent counter via the SASS prologue/epilogue | `slm stats --gpu`, `slm stats --profile` |
| **Power proxy** | sample `tegra-soctherm` via BPMP MRQ (post-kexec only) | `slm stats --power` (best-effort) |
| **OOV / sampler retries** | counters in sampler | `slm stats` |
| **Deadline misses** | inherits from Phase-5 deadline scheduler | `sched stats` |

Performance targets (Qwen2.5-1.5B-Q4_K_M, 4 K context, GPU Path A,
Tier-1 HMMA tensor cores):

- Model load: **< 1.5 s** (mmap-style register, no decode)
- TTFT (≤ 64-token prompt): **< 400 ms**
- Decode throughput: **≥ 25 tok/s sustained** over 256 tokens
- Peak resident memory: **< 1.6 GB** (weights + KV @ 1 K used)
- Sustained tensor-core matmul throughput: **≥ 10 dense FP16 TFLOPS**
  during decode (≈ 30 % of the GA10B tensor-core FP16 ceiling)
- Tensor-core utilization fraction (TC-busy / GPU-busy on matmul ops):
  **≥ 60 %** as reported by `slm stats --gpu`

If only Tier-2 (CUDA-core FP16) is reachable on a given op, the
corresponding decode-throughput target relaxes to **≥ 10 tok/s** and the
tensor-core utilization metric is reported as `n/a (tier-2 fallback)` for
that op.

---

## Component Integration

A new component type, `slm-runner`, registered via the Phase-4 component
system. Subscribes to a prompt topic, publishes a token stream topic. Runs
in EL0 on QEMU (proven Phase-5 M4 path); on real hardware it falls back to
in-kernel execution while the EL0 hardware path is being stabilized
(matches Phase-5 component status).

```yaml
# components/slm-runner/manifest.yaml
name: slm-runner
version: 1.0.0
type: slm-runner

models:
  - name: qwen2.5-1.5b
    path: /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf
    # NOTE: On Jetson the runtime resolves the model via the in-memory
    # registry (populated by an out-of-band `slm xload` over the
    # telnet shell) rather than the LittleFS path — see
    # `docs/setup.md` §"Jetson SD-Card Layout". Manifests should
    # still declare the canonical `path:` so non-Jetson platforms
    # and tooling that introspect the manifest see the source.
    preload: true
    pin: true             # use registry pin/unpin to lock against eviction

session:
  context: 4096
  threads: 4
  gpu: auto
  sampler:
    temperature: 0.7
    top_p: 0.9
    top_k: 40

resources:
  memory_mb: 1600
  workspace_mb: 256

interfaces:
  subscribes:
    - topic: /slm/prompt
  publishes:
    - topic: /slm/token       # one message per generated token
    - topic: /slm/done        # final stats
```

This puts SLM inference on the same on-ramp as `digit_classifier` from
Phase 5: `component run slm-runner` and the system becomes a chat service.

---

## Build & Dependency Plan

| Item | Where | Notes |
|---|---|---|
| `runtime/src/slm/` | new module under runtime crate | Wired into `runtime/src/lib.rs` behind `cfg(feature = "slm")` (default on). |
| `runtime/src/inference/ops_transformer.rs` | extends existing inference module | NEON paths gated `#[cfg(target_arch = "aarch64")]`. |
| `runtime/src/inference/quant.rs` | new file, vendored from GGML | License header preserved. No deps beyond `core`. |
| GGUF tooling | `host-tools/gguf-inspect/` | Host-only Rust binary for inspecting/repacking GGUF files (developer convenience). |
| Tokenizer vocab | shipped inside the GGUF | No separate file. |
| SASS kernel library (Path A) | `gpu/sass/qwen2/{tier1,tier2}/*.sass.bin` | Built pre-kexec on L4T, archived into the deployment image. Authored in CUDA C++ targeting `sm_87`: tier-1 kernels use `nvcuda::wmma` / `mma.sync` PTX for HMMA tensor-core tiles; tier-2 kernels use plain SIMT FP16 as graceful fallback. SASS extracted via `cuobjdump --dump-sass`. |
| kexec-handoff schema bump | `kernel/include/gpu_handoff.h`, `scripts/gpu-kernel-launch.c` | `slm_gpu_handoff_v1` extends MNIST handoff. |
| Shell wiring | `kernel/src/slm_shell.c` | Calls `shell_register_command("slm load", ...)` etc. |
| FFI surface additions | `kernel/include/slm_ffi.h`, `runtime/src/kernel_ffi.rs` | See "FFI Surface" below. |
| CMake | `CMakeLists.txt` minor: register new C source | No new external libs. |
| Tests | `runtime/src/slm/tests/`, `kernel/tests/` | Per-op golden-vector tests vs PyTorch reference; one MNIST-style end-to-end test against a tiny stand-in model in CI. |

**No new third-party libraries are linked.** Everything ships as source in
the repo, audited under the existing `slmos-review` rules.

### FFI Surface (additions)

```c
/* kernel/include/slm_ffi.h — additions to the rust_* family */
int32_t  rust_slm_load(const char *name, const uint8_t *gguf, size_t len);
int32_t  rust_slm_unload(int32_t handle);
int32_t  rust_slm_session_open(int32_t handle, const slm_session_cfg_t *cfg);
int32_t  rust_slm_session_close(int32_t session_id);
int32_t  rust_slm_session_reset(int32_t session_id);
int32_t  rust_slm_prompt(int32_t session_id,
                         const char *prompt, size_t prompt_len,
                         slm_token_callback_t cb, void *user);
int32_t  rust_slm_stop(int32_t session_id);
int32_t  rust_slm_stats(int32_t session_id, slm_stats_t *out);
int32_t  rust_slm_info(int32_t handle, slm_model_info_t *out);
```

`slm_token_callback_t` lets the C shell stream characters straight to UART
without bouncing through Rust println — this matches the kernel-friendly
callback pattern already used for inference probability output.

---

## Skipped / Blocked / Out of Scope

- **Multi-modal (vision-language) models.** Out of scope; revisit after
  Qwen2.5-VL once text-only path is solid.
- **Speculative decoding / draft models.** Deferred — interesting
  follow-up for the second SLM slot (e.g., Qwen2.5-0.5B as draft for
  Qwen2.5-1.5B).
- **Continuous batching / multiple concurrent decode streams.** Deferred.
  Spec assumes one active decode at a time per GPU; CPU path can be made
  re-entrant later via per-session workspace allocators.
- **LoRA / runtime fine-tuning.** Out of scope.
- **External chat protocol (HTTP / OpenAI API shim).** Out of scope; the
  `slm prompt` shell verb and the `/slm/*` topic surface are sufficient for
  the demo. Networking layer (Phase ?) can wrap them later.
- **Persistent KV cache across reboot.** Out of scope. KV is workspace
  memory.
- **Native bare-metal GA10B GPU dispatch (Option A).** Stretch goal —
  blocked on `nvgpu` phases 6–8 (channel / GMMU / pushbuffer). Tracked
  separately under the existing Jetson nvgpu workstream.
- **IMMA (INT8 tensor-core) end-to-end path** (Tier 3 in *Tensor-core
  utilization*). Out of scope for v1. Requires INT8 activations (Q8_0 or
  AWQ-style symmetric per-channel), a calibration pass on Qwen2.5-1.5B,
  and an attention path validated under the lower numerics. Worth ~2× over
  Tier-1 HMMA. Insertion point (`Tier0_IMMA`) preserved in
  `select_backend()`.
- **NVDLA / Deep Learning Accelerator.** Out of scope and not present:
  the Jetson Orin Nano has its DLA fused off (DLA exists only on Orin NX
  and AGX Orin). The Nano's only AI silicon is the GA10B GPU's CUDA cores
  and tensor cores.
- **EL0 isolation of `slm-runner` on Jetson hardware.** Inherits Phase-5
  M4 status: works on QEMU, not yet exercised on Jetson. The component runs
  in-kernel on Jetson until the hardware EL0 path is verified.
- **GGUF v1/v2 / non-Q4_K quants.** Out of scope. Ship only Q4_K_M; add
  Q4_0 support only if needed as an initial bring-up rung. Other quants
  trivial to add later (the parser handles GGUF v3 generically).
- **Dynamic batch / prompt** > 4 K context tokens. The first cut hard-caps
  context at 4096; Qwen supports more (32 K with YaRN) but KV memory and
  attention compute scale unfavorably.
- **Tokenizer pre/post normalization beyond what BBPE prescribes.** Qwen
  tokenizer needs Unicode NFC + a small split-on-spaces pass. Anything
  fancier (character-level normalization, language-specific rules) is out.

---

## See Also

- `docs/api/inference.md`, `docs/api/model-loader.md`, `docs/api/syscalls.md`
  — Phase-5 surfaces this spec extends
- `docs/onnx-support.md` — the ONNX format story (parallel format, not
  replaced)
- `docs/model-memory.md` — pool layout, 2 MB alignment, refcounting rules
- `docs/specs/components.md` — component lifecycle the `slm-runner`
  inherits
- `docs/specs/memory.md` — Jetson memory map and OP-TEE carveout
- `docs/scheduler.md` — AI scheduler and deadline policies the decode loop
  cooperates with
- `docs/eviction-extended-features.md` — CACHEUS, used unchanged for
  multi-model SLM workloads
- `docs/archive/investigations/jetson-nvgpu-bringup-research.md` — the
  authoritative GA10B bring-up plan (Path B's destination)
- `docs/archive/handoff/jetson-capstone-handoff.md` — current GPU bringup
  state on jetson-nano-2
- `docs/plans/slm-integration-plan.md` — phased delivery plan (M1–M8) for
  this spec
- Issues to file at start: GGUF parser, BBPE tokenizer, Q4 NEON kernels,
  KV-cache allocator, decode loop, `slm` shell family, kexec-handoff schema
  bump, SASS kernel library, golden-vector test harness, `slm-runner`
  component manifest

*Last updated: 27 April 2026*
