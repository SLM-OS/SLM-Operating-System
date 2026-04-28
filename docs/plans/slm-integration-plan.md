# SLM Integration — Implementation Plan

Phased delivery plan for the spec in
[`docs/specs/slm-integration.md`](../specs/slm-integration.md). Brings
**Qwen2.5-1.5B-Instruct** end-to-end on the Jetson Orin Nano Super:
upload → CLI launch → prompt → streaming response → live perf monitoring →
GPU-accelerated inference.

**Status (2026-04-27):** M0 through M9 structurally complete on the
SLM feature branch (`worktree-slm-for-slmos`). End-to-end real text
generation gated on **M5.3** (real `forward_step` — registry storing
GGUF tensor data + per-session tokenizer + weight-pool wiring through
the M4 op chain) and the Jetson hardware deploy. Tracked via the
umbrella issue and the M5.3 / M6.A-2+ / M6.B/C/D / NEON follow-up
issues filed at M9 close-out.

| Milestone | Status                                                 |
|-----------|--------------------------------------------------------|
| M0        | ✅ Shipped (PR #492)                                   |
| M1        | ✅ Shipped (PR #497)                                   |
| M2        | ✅ Shipped (PR #502)                                   |
| M3        | ✅ Shipped (PR #505) — NEON deferred to M9 follow-up   |
| M4        | ✅ Shipped (PR #520) — NEON deferred to M9 follow-up   |
| M5        | ✅ Shipped (PR #524) — `forward_step` is a stub; M5.3 lands the real per-layer op chain |
| M6        | ✅ Scaffolding shipped (PR #526) — SASS authoring (M6.B/C/D) and the L4T loader (M6.A-2+) deferred |
| M7        | ✅ Shipped (PR #528)                                   |
| M8        | ✅ Shipped (PR #530) — hot-swap-safe handle preservation deferred |
| M9        | ✅ Shipped (this PR) — capstone polish + cargo-test enablement |

**Estimated calendar time:** 9–11 weeks (one engineer at the project's
historical Phase-3/4/5 cadence). Milestones M1, M2, M3, and M5 are largely
parallel-able after M0; M6 (GPU) is the longest-pole item.

**Demo target:** A live `slm prompt` exchange over UART with
Qwen2.5-1.5B-Instruct, GPU-accelerated, decoding ≥ 25 tok/s, with `slm
stats --profile` showing per-op breakdown.

---

## Icon Key

| Icon | Meaning |
|------|---------|
| ☐ | Not started |
| 🚧 | In progress |
| ✅ | Complete |
| ⏸️ | Deferred |
| 🔗 | Has dependency |

---

## Prerequisites (already in the tree)

- ✅ Phase 3: model memory pools (weight + workspace, 2 MB-aligned)
- ✅ Phase 4: component system, message router, hot-swap, EL0 syscalls
- ✅ Phase 5: ONNX loader, CPU inference engine, `model` shell family,
  `rust_infer_*` FFI, AI scheduler, CACHEUS eviction
- ✅ Phase AI-Sched: pluggable scheduler with MLP/PPO inference policies
- ✅ Jetson EL2+VHE boot via kexec, 6-core SMP, BPMP IPC
- ✅ Jetson GPU phases 1–5 (FECS method gateway proven on jetson-nano-2)
- ✅ kexec-handoff GPU bridge for MNIST (PR #376)

---

## Critical Path

```
M0 (host tooling)
   │
   ├──► M1 (GGUF parser) ──► M2 (tokenizer) ──┐
   │                                          │
   ├──► M3 (Q4 quant kernels) ──┐             │
   │                            │             │
   ├──► M4 (transformer ops) ◄──┴─────────────┤
   │            │                             │
   │            ▼                             │
   │       M5 (decoder + KV cache + sampler) ◄┘
   │            │
   │            ▼
   │       M7 (slm shell + FFI) ──► M8 (slm-runner component)
   │            │                              │
   │            ▼                              ▼
   │       (CPU demo: end-to-end on QEMU + Pi 5 + Jetson)
   │
   └──► M6 (GPU: kexec-handoff bridge for transformers)
                │
                ▼
            (GPU demo on Jetson; M9 telemetry overlays both)
```

Minimum viable demo (CPU-only): **M0 → M1 → M2 → M3 → M4 → M5 → M7**.
GPU acceleration: **M6**. Production polish: **M8 → M9**.

---

## M0 — Host Tooling & Model Provisioning

**Goal:** A reproducible recipe for fetching, validating, and shipping the
Qwen2.5-1.5B-Instruct Q4_K_M GGUF onto the target.

- ☐ **M0-1.** `host-tools/gguf-inspect/` — Rust CLI that prints GGUF
  header, all KV metadata, tensor list, and offsets. Two integration tests
  (Qwen and Llama).
- ☐ **M0-2. Per-platform model-pool sizing.** Today, the kernel calls
  `rust_model_mem_init()` with no arguments and Rust hard-codes 256 MB /
  128 MB (the Phase-5 defaults sized for QEMU). Pick one of:
  (a) a per-platform `config.h` override consumed inside the no-arg shim
  (cleanest, recommended), or
  (b) extend `rust_model_mem_init` to take `(weight_mb, workspace_mb)` and
  pass the right values from each platform's boot path.
  Required values for Jetson per the spec: weight 2 GB, workspace 256 MB,
  with a 512 MB KV-cache sub-pool carved from workspace by M5.
- ☐ **M0-3.** `scripts/fetch-slm.sh` — pinned-by-SHA256 download of
  `Qwen/Qwen2.5-1.5B-Instruct-GGUF/qwen2.5-1.5b-instruct-q4_k_m.gguf`.
- ☐ **M0-4.** `host-tools/gguf-inspect dump-vocab <file>` subcommand:
  emits the tokenizer vocab + merges as a self-contained binary blob
  (used by tests).
- ☐ **M0-5.** Add `/mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf` to the
  standard Jetson SD-card layout in `docs/setup.md`.
- ☐ **M0-6.** Doc: `docs/tutorials/slm-models.md` — how to fetch, verify,
  and stage a GGUF for SLM-OS (mirrors `docs/tutorials/models.md`).

**Acceptance:** `gguf-inspect` round-trips a GGUF and prints arch=`qwen2`,
339 tensors, vocab=152 064. On Jetson boot, `model pools` reports
2 048 MB weight / 256 MB workspace.

---

## M1 — GGUF v3 Parser

**Goal:** Zero-copy GGUF v3 parser in `runtime/src/slm/gguf.rs`. Mirrors
the architecture of the existing `loader/protobuf.rs` (no allocation,
returns offsets and slices).

- ☐ Header struct (`GGUF\0` magic, version=3, tensor_count, metadata_kv_count).
- ☐ KV-metadata reader: typed values (u32, i32, f32, string, array of …).
- ☐ Tensor-info reader: name, n_dims, dims, ggml_type, offset.
- ☐ Required-key validator: reject files lacking `general.architecture`,
  `qwen2.block_count`, `qwen2.embedding_length`, etc.
- ☐ Q4_K block layout decoded (256-element super-block, 6-bit scales/mins).
- ☐ Public API: `Gguf::parse(&[u8]) -> Result<Gguf, GgufError>`,
  `Gguf::tensor(&self, name: &str) -> Option<TensorView>`.
- ☐ Integration: extend `loader/registry.rs` to dispatch by file magic
  (`ONNX_*` vs `GGUF\0`). Existing `model load` continues to work for ONNX.
- ☐ FFI: `rust_slm_load(name, data, len) -> handle`.
- ☐ Tests: parse Qwen and Llama GGUFs offline, verify all expected
  tensors are present and their shapes match published model cards.

**Acceptance:** `model load /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf`
returns a handle, `model info <h>` prints arch=qwen2, n_layer=28, n_head=12,
n_kv_head=2, hidden=1536, vocab=152064.

---

## M2 — BBPE Tokenizer (Qwen)

**Goal:** Deterministic byte-level BPE that reproduces Hugging Face
`AutoTokenizer.encode/decode` on a held-out test corpus.

- ☐ `runtime/src/slm/tokenizer.rs` — vocab + merges loaded from GGUF
  metadata.
- ☐ Pre-tokenization: GPT-2-style regex split (Qwen uses the GPT-2 split
  pattern). Implement a small DFA-based splitter rather than pulling in a
  regex crate.
- ☐ BPE merge loop: priority-queue style; bench on a 4 KB English+code
  prompt.
- ☐ Special tokens: `<|im_start|>`, `<|im_end|>`, `<|endoftext|>`.
- ☐ `encode(&str) -> Vec<u32>` and `decode(&[u32]) -> String` with
  byte-fallback for unknown ranges.
- ☐ Chat template renderer (`runtime/src/slm/chat_template.rs`):
  ChatML wrapping for `system / user / assistant` turns.
- ☐ Tests: golden-vector test against 200 prompts pre-tokenized by the HF
  tokenizer in CI (committed as a `.bin` file alongside the test).

**Acceptance:** Round-trip `decode(encode(s)) == s` for the test corpus
with zero divergences from HF reference.

---

## M3 — Q4 Quantization Kernels

**Goal:** Hand-ported, audited Q4_K_M and Q8_K kernels in
`runtime/src/inference/quant.rs`.

- ☐ Vendor (re-license-compatible) the GGML kernels:
  `dequantize_row_q4_K`, `quantize_row_q8_K`, `vec_dot_q4_K_q8_K` (scalar
  reference + ARM NEON fast path).
- ☐ Translate to `no_std` Rust: no `assert!`, no panics in the hot loop,
  fully `unsafe` block with documented invariants.
- ☐ License header preserved at top of file; add MIT-attribution entry to
  `LICENSES.md`.
- ☐ Standalone correctness harness: dequantize a known Q4_K block and
  verify against reference float values from llama.cpp's
  `tests/test-quantize.cpp` outputs (vectors checked into the test crate).
- ☐ Microbench: `bench q4kdot` shell command — vec-dot a 1536-element
  Q4_K row against an FP16-quantized-to-Q8_K column. Target ≥ 4 GB/s
  effective on the Jetson A78AE single core.

**Acceptance:** Numerical match to reference within FP16 epsilon; ≥ 4 GB/s
single-core effective throughput on Orin Nano CPU.

---

## M4 — Transformer Operators (CPU)

**Goal:** A complete, tested set of decoder-only ops in
`runtime/src/inference/ops_transformer.rs`. NEON-optimized; correctness
first.

- ☐ `RMSNorm` (FP16 in/out, FP32 accumulator).
- ☐ `RoPE` (precomputed cos/sin LUT, applied to Q and K).
- ☐ `Embedding` lookup (gather over Q4_K table; output FP16 row).
- ☐ `GroupedQueryAttention`:
  - QKV projections (Q4_K weights × FP16 acts via `vec_dot_q4_K_q8_K`)
  - RoPE on Q, K
  - K, V append into KV cache (FP16)
  - Causal-masked scaled-dot-product attention with FP32 accumulator
  - Output projection
- ☐ `SwiGLU` MLP block (gate + up projections, SiLU on gate, multiply,
  down projection).
- ☐ `LMHead`: final projection to vocab (Q4_K × FP16 acts → FP32 logits).
- ☐ Per-op golden-vector tests against PyTorch (committed as fixed-input
  tensor pairs in `runtime/src/inference/tests/transformer_golden/`).
- ☐ Bench harness: `bench layer <op>` measures per-op cost on a fake
  layer of Qwen2.5-1.5B dimensions.

**Acceptance:** All per-op golden tests pass within 1e-2 max-abs vs
PyTorch FP16 reference; full layer (Embed → Norm → GQA → Norm → MLP)
matches reference under same tolerance.

---

## M5 — Decoder, KV Cache, Sampler, Session

**Goal:** Wire the ops into a full forward + autoregressive decode loop
with KV cache and sampling, gated by a session state machine.

- ☐ `runtime/src/slm/kv_cache.rs` — per-layer K and V FP16 buffers,
  ring-allocated from a session-owned KV-cache slot in the workspace pool.
  Helpers: `append(layer, k, v)`, `slice(layer, len) -> (&K, &V)`.
- ☐ `runtime/src/slm/sampler.rs` — greedy / temperature / top-k / top-p,
  deterministic seed, per-session state.
- ☐ `runtime/src/slm/decoder.rs`:
  - **Prefill** in chunks of 64 tokens; each chunk runs all 28 layers,
    populates KV cache, discards activations.
  - **Decode** loop: one token at a time, all 28 layers, append to KV,
    sample, emit via `slm_token_callback_t`, `yield()` cooperatively.
  - Cooperative cancel: check `session.stop_flag` at each token.
- ☐ `runtime/src/slm/session.rs` — owns model handle, tokenizer ref,
  KV-cache allocation, sampler state, stop flag, telemetry counters.
  Lifecycle: `open(cfg) → prompt(text, cb)*  → close()`.
- ☐ Pin/unpin protection: opening a session pins the model in the registry
  (existing `loader/registry.rs` API); closing unpins.
- ☐ FFI: `rust_slm_session_*`, `rust_slm_prompt`, `rust_slm_stop`,
  `rust_slm_stats`.

**Acceptance:** `rust_slm_prompt` returns a deterministic completion for a
fixed prompt + greedy sampling that matches `llama.cpp -p ... -temp 0`
output token-for-token.

---

## M6 — GPU Acceleration with Tensor Cores (Jetson, kexec-handoff bridge)

**Goal:** Extend the proven MNIST kexec-handoff bridge (PR #376) to run
the Qwen2.5-1.5B forward pass on the GA10B GPU, **driving the Ampere
tensor cores via HMMA as the main-line implementation**. Sustained
decode ≥ 25 tok/s with ≥ 60 % tensor-core utilization on matmul ops.

The Orin Nano has no NVDLA / no separate TPU — the GA10B's 32 third-gen
tensor cores are the only AI silicon on the board, and the 67-TOPS
headline figure comes from them. So tensor-core utilization is not a
nice-to-have; it's the difference between a usable demo and a slideshow.

### M6.A — Handoff & dispatch infrastructure

- ☐ **M6.A-1.** Define `slm_gpu_handoff_v1` schema in
  `kernel/include/gpu_handoff.h`: weight DMA addresses, layer plan, per-op
  **tier descriptor** (which SASS kernel variant to dispatch),
  KV-cache descriptors, semaphore page, doorbell page.
- ☐ **M6.A-2.** Pre-kexec L4T loader (`scripts/slm-gpu-bringup.c`):
  allocates GPU channel, uploads SASS kernels (both tiers per op),
  maps weight pool into GPU VA, publishes handoff struct. Mirrors
  `scripts/gpu-kernel-launch.c` from MNIST.
- ☐ **M6.A-3.** Bare-metal SLM-OS side: `runtime/src/inference/gpu_slm.rs`
  — backend implementation that submits each layer's pushbuffer using the
  inherited channel and polls the completion semaphore. Reuses Phase-5
  GPU framework hooks.
- ☐ **M6.A-4.** `select_backend()` extension: per-op tier preference
  `Auto → Tier1 (HMMA) if available, else Tier2 (CUDA-core), else CPU`.
  Smoke-test gate at session launch dispatches a tiny HMMA kernel; failure
  demotes that op to Tier 2 and logs to `slm gpu`.
- ☐ **M6.A-5.** Cache-coherency: re-use existing `slm_gpu_sync_for_device` /
  `slm_gpu_sync_for_cpu` patterns from Phase-5 GPU framework.
- ☐ **M6.A-6.** Per-pushbuffer GPU PTIMER sampling (BAR0+0x9420) and
  per-op tensor-core busy fraction (via SASS prologue/epilogue counter
  reads) wired to `slm stats --gpu`.

### M6.B — SASS kernel library, Tier-1 (HMMA tensor-core, main-line)

Authored in CUDA C++ targeting `sm_87` using `nvcuda::wmma` /
`mma.sync.aligned…` PTX, compiled with `nvcc -arch=sm_87`, SASS extracted
via `cuobjdump --dump-sass`. Reference: llama.cpp's `ggml-cuda` backend.
Each kernel is paired with a Tier-2 sibling (M6.C) so a slip on any one
HMMA kernel falls back gracefully without losing the GPU path.

- ☐ **M6.B-1.** `q4K_dot_f16.tier1.sass` — Q4_K dequant into shared
  memory → HMMA `m16n8k16` matvec. The decode workhorse (used for QKV,
  output, gate, up, down projections during 1-token decode steps).
- ☐ **M6.B-2.** `q4K_gemm_f16.tier1.sass` — Q4_K × FP16 matmul for
  prefill batches (multi-token); same dequant-then-HMMA pattern but
  tiled for higher M dim.
- ☐ **M6.B-3.** `gqa_attn_f16.tier1.sass` — fused
  Q·Kᵀ → softmax (FP32 accumulator) → P·V, with both matmuls as
  HMMA tiles. Causal mask folded in. KV-cache append happens inline.
- ☐ **M6.B-4.** `swiglu_f16.tier1.sass` — fused gate · up matmul block
  with SiLU on gate; matmul portions HMMA. `down` projection chained
  as a separate HMMA kernel to keep tile occupancy reasonable.
- ☐ **M6.B-5.** `lm_head_f16.tier1.sass` — final 1536→152 064 projection
  via HMMA, with sampled-token argmax (greedy) or top-k partial sort
  fused into the epilogue.
- ☐ **M6.B-6.** Tensor-core utilization probe: per-kernel prologue
  reads `%clock64` and the kernel's MMA-issue counter; epilogue
  computes the fraction. Counter values returned via the completion
  semaphore page so the host can log them without an extra round-trip.

### M6.C — SASS kernel library, Tier-2 (CUDA-core FP16, fallback)

Same kernels as M6.B but in plain SIMT FP16 form (no MMA instructions).
These exist to guarantee that **if any single Tier-1 kernel hard-blocks
during bring-up, the rest of the GPU path still runs**. Roughly 4–6×
slower than Tier-1 on matmul, still ~3× faster than CPU.

- ☐ **M6.C-1.** `q4K_dot_f16.tier2.sass` — `fma.f16x2`-based matvec.
- ☐ **M6.C-2.** `q4K_gemm_f16.tier2.sass`.
- ☐ **M6.C-3.** `gqa_attn_f16.tier2.sass`.
- ☐ **M6.C-4.** `swiglu_f16.tier2.sass`.
- ☐ **M6.C-5.** `lm_head_f16.tier2.sass`.

### M6.D — Element-wise / reduction kernels (single tier)

These don't hit tensor cores in either tier — pure SIMT — so they ship as
one variant.

- ☐ **M6.D-1.** `rmsnorm_f16.sass` — fused RMSNorm with FP32 reduction.
- ☐ **M6.D-2.** `rope_f16.sass` — RoPE applied to Q and K in-place.

### M6 — Acceptance

- ☐ Bit-exact (within FP16 epsilon, max-abs ≤ 1e-2) match between CPU
  and GPU forward passes on a held-out prompt — **measured separately
  for Tier-1-only and Tier-2-only kernel selections** to confirm both
  paths land independently.
- ☐ `slm bench 0 --gen-len 256` on Jetson Orin Nano Super:
  - ☐ With Tier-1 HMMA on all matmul kernels: **≥ 25 tok/s** sustained.
  - ☐ With Tier-2 CUDA-core fallback on all matmul kernels: **≥ 10 tok/s**
    sustained (degraded-mode floor).
- ☐ TTFT for a 64-token prompt ≤ 400 ms (Tier-1).
- ☐ `slm stats --gpu` reports tensor-core utilization ≥ 60 % during
  matmul ops in Tier-1 mode.
- ☐ Per-op tier selection visible via `slm gpu` (e.g. one op stuck on
  Tier 2 while others run Tier 1 is a supported, non-failure state).

### M6 — Risk note

SASS authoring (especially HMMA tile orchestration with shared-memory
dequant prefetch) is the highest-risk single item in the plan. Mitigations
are baked into the structure:

1. **Per-kernel tiering.** A failing Tier-1 kernel falls back to its
   Tier-2 sibling at op granularity, not session granularity. The demo
   still GPU-accelerates the rest of the pipeline.
2. **Tier-2 first, Tier-1 second per kernel.** Authoring order inside
   each M6.B item: write the Tier-2 SIMT kernel first (fast, simple,
   gives a known-good reference output), then the Tier-1 HMMA kernel
   against the same test vectors. This means Tier-1 work being cut never
   results in *no* GPU path for that op.
3. **CPU as ultimate floor.** Even if all of M6.B and M6.C slip,
   `select_backend()` falls all the way back to the M4 NEON
   implementation, and the demo still works at 8–12 tok/s.
4. **Hard-block treatment.** Per spec, IMMA / INT8 / Tier-3 is *already*
   scoped as a follow-up — so the "after-the-fact upgrade" pattern is
   already exercised in the design. If Tier-1 itself proves intractable
   on any op, that op moves to the same after-the-fact bucket without
   restructuring the codebase: Tier-2 holds the line, Tier-1 lands later
   as a drop-in SASS swap.

---

## M7 — Shell Integration & Streaming I/O

**Goal:** The full `slm` shell verb family from the spec, with
token-by-token UART streaming.

- ☐ `kernel/src/slm_shell.c` registers: `slm load / list / info / launch /
  prompt / stream / stop / reset / unload / status / stats / bench / gpu`.
- ☐ Token-callback bridge: C-side `slm_token_callback_t` writes UTF-8
  bytes to UART as decoded, no buffering beyond a 64-byte line wrap
  helper. Honors UART back-pressure.
- ☐ `slm stream` reads from UART line-buffered until blank input; useful
  for interactive testing without copy-paste.
- ☐ `slm stop` from a separate UART input session signals the stop flag —
  requires a small change to `shell.c` to allow command interleaving from
  the same UART (or document that `stop` works via a second session).
  **Decision needed (M7-D1):** interleaved input vs. require second UART
  session.
- ☐ Lua bindings: `slm.load`, `slm.launch`, `slm.prompt`, `slm.stats` —
  matches the existing `slm.component_*` pattern.
- ☐ Help text + categorization in `builtin_commands[]`.

**Acceptance:** Live demo over UART:

```
slmos> slm load /mnt/files/qwen2.5-1.5b-instruct-q4_k_m.gguf
slmos> slm launch 0 --ctx 4096 --gpu auto
slmos> slm prompt 0 "What is virtual memory?"
[streaming output appears character by character]
[slm] ttft=...  decode=... tok/s
```

---

## M8 — `slm-runner` Component

**Goal:** A first-class component that runs an SLM as a long-lived
service, reachable via the message router.

- ☐ `components/slm-runner/` with manifest from the spec.
- ☐ Component subscribes `/slm/prompt`; for each message:
  tokenize → render ChatML → run prompt → publish each token to
  `/slm/token` → publish final stats to `/slm/done`.
- ☐ Hot-swap-safe: model handle and KV cache live across replacements
  (similar to how `sensor_monitor` preserves subscriptions).
- ☐ EL0 isolation: works on QEMU (Phase-5 M4 path); falls back to
  in-kernel on Jetson (matches Phase-5 status).
- ☐ Lua demo (`scripts/slm-chat-demo.lua`): publishes a few prompts,
  prints streamed responses, exits.

**Acceptance:** `component run slm-runner` followed by a Lua publish to
`/slm/prompt` produces a streamed response on `/slm/token` and final
stats on `/slm/done`. Hot-swap of `slm-runner` preserves the loaded model
(verified by checking model handle remains valid post-swap).

---

## M9 — Telemetry, Benchmarks, Polish

**Goal:** All telemetry the spec promises, plus per-platform benchmark
reports.

- ☐ Per-op profiling under `slm stats --profile`: rmsnorm, rope, gqa
  (broken into qkv-proj, attn, oproj), mlp (gate, up, silu*mul, down),
  lm_head. Implementation: `Timer::elapsed()` ring buffer per op,
  reported as ms/token average.
- ☐ GPU PTIMER integration for `slm stats --gpu` (Jetson only).
- ☐ Best-effort power proxy via BPMP MRQ for `slm stats --power`.
- ☐ `slm bench` runs the spec's standard battery: 64/256-token prompt
  prefill + 256-token decode, three runs, reports min/avg/max.
- ☐ Benchmark report committed to `docs/benchmarks.md` covering CPU and
  GPU paths on QEMU, Pi 5, and Jetson.
- ☐ Update `docs/specs/slm-integration.md` matrix with measured ✅ vs goal
  ✅.
- ☐ Demo script `scripts/slm-demo.lua` for the capstone presentation.
- ☐ User guide `docs/tutorials/slm-prompt.md`.
- ☐ `slmos-review` pass on the whole branch.

**Acceptance:** Targets from the spec met on Jetson:
- Model load < 1.5 s
- TTFT < 400 ms
- Decode ≥ 25 tok/s
- Peak resident < 1.6 GB

---

## Outstanding Decisions

| ID | Decision | Options | Recommendation |
|---|---|---|---|
| M0-D1 | Where to keep the GGUF for Jetson | SD-card `/mnt/files`, eMMC, or NFS mount | SD-card initially (matches existing model-load story); revisit only if load time hurts |
| M3-D1 | Q4 kernel sourcing | hand-write from spec vs. vendor GGML | **Vendor GGML** (audited subset, MIT) — much lower risk than re-deriving the bit-packing |
| M3-D2 | Quant scheme to ship | Q4_K_M only vs. Q4_0+Q4_K_M | **Q4_K_M only** — best quality at this size; add Q4_0 only if a bring-up rung is needed |
| M4-D1 | RoPE LUT precision | FP16 vs FP32 | **FP32 LUT, FP16 apply** — LUT is tiny, accuracy gain at zero hot-path cost |
| M5-D1 | KV-cache dtype | FP16 vs Q8 | **FP16** for v1 (simpler, ample memory at 1.5B); revisit Q8 if we add a 3B model |
| M5-D2 | Sampler default | greedy vs temperature 0.7 + top_p 0.9 | Demo default = **temperature 0.7 + top_p 0.9** for natural feel; deterministic test default = **greedy** |
| M6-D1 | SASS-kernel build pipeline | hand-author PTX vs derive from CUDA via cuobjdump | **CUDA → cuobjdump SASS** — leverages well-tested llama.cpp CUDA kernels as the source of truth |
| M6-D2 | GPU fallback granularity | per-op or per-layer or per-session | **Per-op** via `select_backend()` — matches existing Phase-5 framework |
| M6-D3 | Tensor-core API to target | `nvcuda::wmma` C++ vs inline `mma.sync.aligned…` PTX | **Start with `wmma`** for readability; drop to inline PTX only on the 1–2 hottest kernels (`q4K_dot`, `gqa_attn`) if profiling shows a meaningful gap |
| M6-D4 | Tier-1 / Tier-2 authoring order per M6.B kernel | Tier-1 first vs Tier-2 first | **Tier-2 first** per kernel — gives a working golden reference before HMMA tile work begins, so any Tier-1 slip leaves the GPU path intact |
| M6-D5 | HMMA accumulator precision | FP16 vs FP32 | **FP32 accumulator** for attention (`Q·Kᵀ`, `P·V`) and `lm_head`; FP16 acceptable elsewhere. Matches llama.cpp's choices and stays inside the 1e-2 max-abs tolerance |
| M7-D1 | `slm stop` signaling | interleaved UART input vs second UART | **Interleaved** if shell.c supports it cleanly; otherwise document the second-UART workaround for the demo |
| M8-D1 | KV cache lifetime across hot-swap | preserve vs reset | **Preserve** — chat continuity matters; component generation counter handles correctness |

---

## Risk Register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **Tier-1 (HMMA tensor-core) kernel authoring slips on one or more ops** | Medium | Medium | Per-op fallback to Tier-2 (CUDA-core FP16) — the GPU path stays alive at degraded throughput (~10 tok/s floor) while affected Tier-1 kernels are added after-the-fact. Tier-2 is authored first per kernel for exactly this reason. |
| **All of M6.B + M6.C slips (no GPU path at all)** | Low | High | CPU NEON path from M4 still works (8–12 tok/s); demo ships CPU-only with GPU listed as in-flight. |
| **HMMA accuracy drift exceeds 1e-2 vs CPU reference** | Low | Medium | FP32 accumulators in attention and lm_head (M6-D5). If still divergent, raise tolerance for that op or run that op on Tier 2 / CPU. |
| **Tensor-core utilization stays below 60 % target** | Medium | Low | Acceptance criterion is utilization *and* tok/s; if tok/s target met but utilization low, ship and file an optimization issue. Likely root cause is shared-memory dequant prefetch — fix is mechanical, post-demo. |
| **GGML Q4 kernel licensing review delays M3** | Low | Medium | MIT licensed; pre-clear with project advisor; if blocked, hand-author against the documented Q4_K block format. |
| **BBPE tokenizer divergence from HF reference** | Medium | Medium | Golden-vector tests in CI. If Qwen tokenizer proves brittle, fall back to Llama-3.2-1B (smaller vocab, simpler split). |
| **KV cache OOM at long contexts** | Medium | Low | Hard-cap at 4 K context in v1; document upgrade path to sliding-window or YaRN. |
| **kexec-handoff bridge breaks under SLM-sized weight pool (2 GB)** | Medium | High | Bringup test on M6 entry. If GPU VA mapping doesn't scale, fall back to mapping per-layer rather than whole-pool. |
| **Cooperative-only preemption causes shell unresponsiveness during long decode** | High | Low | Mandatory `yield()` between every token (already in M5). `slm stop` works at token boundary granularity — sub-second. |
| **Numerical drift between CPU and GPU paths > acceptable** | Medium | Low | FP32 accumulators in attention and reductions. Acceptance test allows 1e-2 max-abs delta. |
| **Tokenizer vocab in GGUF metadata is large (152 K entries)** | Low | Low | Stream-parse rather than loading into a single allocation; or chunk into the workspace pool. |
| **Jetson EL3 timer-IRQ block prevents tight per-token timing** | Low | Low | Use `CNTPCT_EL0` reads (cooperative-preempt pattern from Phase 6) for timing — already proven. |
| **Native GA10B path (Option B) blocked progresses, then makes Option C obsolete mid-plan** | Low | None | Both paths land via the same `select_backend()` indirection. Net win, no rework. |

---

## Schedule (working estimate)

| Week | Focus |
|---|---|
| 1 | M0 (host tooling, fetch + verify GGUF) + M1 (GGUF parser) start |
| 2 | M1 wrap, M2 (tokenizer) start, M3 (quant) start in parallel |
| 3 | M2 wrap, M3 wrap, M4 (transformer ops) start |
| 4 | M4 — RMSNorm, RoPE, Embedding, MLP |
| 5 | M4 — GQA + KV cache; first per-op golden tests passing |
| 6 | M5 (decoder, sampler, session) — first end-to-end CPU completion |
| 7 | M7 (shell + streaming) — interactive demo on QEMU and Pi 5 |
| 8 | M6.A (handoff infra) + M6.D (rmsnorm, rope) + M6.C-1/2 (Tier-2 q4K_dot/gemm — earliest-possible end-to-end GPU path) |
| 9 | M6.B-1/2 (Tier-1 HMMA q4K_dot/gemm) + M6.C-3/4/5 (remaining Tier-2 kernels) — full GPU path lit at degraded-mode floor |
| 10 | M6.B-3/4/5 (Tier-1 HMMA gqa/swiglu/lm_head) + M6.B-6 (utilization probe) + M8 (slm-runner) |
| 11 | M9 (telemetry, benchmarks, polish, capstone demo) — Tier-1 hits its 25 tok/s + 60 % utilization targets here |

CPU-only demo lands by **end of week 7**. Degraded-mode GPU demo
(Tier-2 CUDA-core only, ~10 tok/s) lands by **end of week 9**. Full
tensor-core-accelerated GPU demo (Tier-1 HMMA, ≥ 25 tok/s) lands by
**end of week 11**. If Tier-1 work on any individual kernel slips past
week 11, that kernel ships on its Tier-2 sibling and the Tier-1 upgrade
lands as an after-the-fact PR — the demo is not blocked.

---

## Definition of Done

The plan is complete when, on the Jetson Orin Nano Super Dev Kit, a fresh
boot can:

1. Load Qwen2.5-1.5B-Instruct from VFS via `slm load`.
2. Launch a session on the GPU via `slm launch 0 --gpu auto`.
3. Receive a free-form prompt via `slm prompt 0 "..."` and stream a
   coherent response over UART character-by-character.
4. Show real-time per-token telemetry on `slm stats --profile`.
5. Demonstrate `slm-runner` answering prompts published to `/slm/prompt`
   on the message bus.
6. Hold ≥ 25 tok/s decode and < 400 ms TTFT on a 64-token prompt with
   Tier-1 HMMA tensor-core kernels engaged on all matmul ops, and report
   ≥ 60 % tensor-core utilization during decode via `slm stats --gpu`.
   Degraded-mode (Tier-2 fallback only) acceptance floor is ≥ 10 tok/s.
7. Pass the full `/ci` sweep (QEMU ARM64 + x86-64) with the new test
   suites, including golden-vector tokenizer, golden-vector ops, and an
   end-to-end deterministic-greedy completion test.
8. Pass `/slmos-review` against the merge diff to `main`.

---

## See also

- [`docs/specs/slm-integration.md`](../specs/slm-integration.md) — the
  spec this plan implements
- [`docs/onnx-support.md`](../onnx-support.md) — the parallel ONNX path
  (kept, not replaced)
- [`docs/model-memory.md`](../model-memory.md) — pool layout
- [`docs/scheduler.md`](../scheduler.md) — AI scheduler integration
- [`docs/eviction-extended-features.md`](../eviction-extended-features.md)
  — CACHEUS eviction
- [`docs/archive/todos/TODO - PHASE 5.md`](../archive/todos/TODO%20-%20PHASE%205.md)
  — the format this plan follows
- [`docs/archive/handoff/jetson-capstone-handoff.md`](../archive/handoff/jetson-capstone-handoff.md)
  — current Jetson GPU bringup state

*Created: 27 April 2026*
