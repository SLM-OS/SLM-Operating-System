# SLM Integration on Jetson Orin Nano — Fact Sheet

End-to-end support for a generative Small Language Model (Qwen2.5-1.5B-Instruct, Q4_K_M GGUF) on the Jetson Orin Nano: model upload, CLI launch, prompt → streamed response, and GPU-accelerated inference.

## Matrix

Legend: ✅ shipped · 🟡 partial · ❌ not applicable on this platform

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson Orin Nano | x86-64 |
|---|---|---|---|---|
| GGUF model upload via VFS / `slm xload` | ✅ | ✅ (RAM-constrained) | ✅ primary target | 🟡 if VFS reachable |
| GGUF v3 parser | ✅ | ✅ | ✅ | ✅ |
| Q4_K_M dequant + vec_dot (NEON SDOT, 3.3× scalar) | ✅ | ✅ | ✅ | 🟡 SSE scalar |
| Multi-quant dispatch (Q4_0, Q4_K, Q5_0, Q6_K, Q8_0) | ✅ | ✅ | ✅ | 🟡 |
| Transformer op set (RMSNorm, RoPE, GQA, SwiGLU) | ✅ | ✅ | ✅ | 🟡 SSE port |
| KV cache | ✅ Rust heap | ✅ | ✅ | 🟡 |
| BBPE tokenizer (Qwen vocab) + GPT-2 byte-to-unicode | ✅ | ✅ | ✅ | 🟡 |
| Autoregressive decode loop | ✅ | ✅ | ✅ | 🟡 |
| Streaming output to UART (token-by-token) | ✅ | ✅ | ✅ | 🟡 |
| `slm load / launch / prompt / status / unload / xload` shell verbs | ✅ | ✅ | ✅ | 🟡 |
| GPU-accelerated transformer ops (HMMA FP16) | ❌ | ❌ | ✅ partial — RMSNORM, GQA_ATTN, EMBEDDING.Q4K, HMMA matmul, MNIST MLP | ❌ |
| GPU-resident full transformer chain | ❌ | ❌ | 🟡 RoPE, SwiGLU down-projection, LM-head still CPU | ❌ |
| Per-token latency / TTFT / throughput telemetry | ✅ | ✅ | ✅ | 🟡 |
| Sampler (greedy, temperature, top-k, top-p) | ✅ | ✅ | ✅ | 🟡 |
| Chat template renderer (Qwen ChatML) | ✅ | ✅ | ✅ | 🟡 |
| LRU model eviction (reuses Phase-5 registry) | ✅ | ✅ | ✅ | ✅ |

## Skipped / Blocked

- **Big.LITTLE-aware decode-thread placement** (Jetson 6× A78AE dual-cluster) — uniform placement today.
- **`slm-runner` EL0-isolated decode component** — EL0 task primitives shipped on Pi 5 (#697/#731/#734); component-runtime EL0 wiring is the missing piece.
- **GPU IMMA INT8 path** — deferred follow-up; needs INT8 activations.
- **x86-64** — most SLM features are NEON-tuned; the SSE port works but is not the primary target.

## See Also

- [`../design/slm-integration.md`](../design/slm-integration.md) — full design spec (target SLM rationale, runtime stack, memory plan, GPU integration paths).
- [`memory.md`](memory.md) — pool layout, OP-TEE carveout, Jetson 1 GB weight pool.
- [`gpu-inference.md`](gpu-inference.md) — GA10B compute path the GPU-resident ops dispatch through.
- [`components.md`](components.md) — component lifecycle the `slm-runner` will inherit.
