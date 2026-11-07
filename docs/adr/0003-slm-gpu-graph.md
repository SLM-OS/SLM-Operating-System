# ADR 0003: run the whole SLM forward pass as one GPU command stream

## Context

The bar in ADR 0001 is 43.0 tok/s decode and 74 ms for a 64-token prefill. The existing hybrid path dispatches one op at a time, copies activations through 64 KB scratch slots, runs an L2 evict per op, and holds an IRQs-off lock over each GPU round trip. A token needs about 500 ops, so the 23 ms budget allows about 46 us per op, compute included. The hybrid path spends more than that on overhead (documented 50-60 us for one RMSNorm call). Op-at-a-time prefill also cannot meet 74 ms: it needs batched execution.

## Decision

At `slm launch --gpu auto|on`, build a per-session GPU program:

1. Weights: map the GGUF tensor bytes into the channel's VM without copying (C1 provides physically contiguous pages). No `nvgpu oplib wpool` step.
2. A session arena in the same VM holds activations, KV cache, QMDs, cbufs and pushbuffers.
3. One prebuilt pushbuffer per program shape with all launches, separated by compute barriers, one trailing semaphore. Each step patches only the step parameters (token ids, positions, sequence length). Activations stay on the GPU across ops; only last-position logits cross to the CPU, with one L2 evict per step.
4. Prefill is the same program with batch size up to 64 and a causal mask.
5. The CPU waits with IRQs enabled and without the dispatch spinlock held for the wait (#1093 class).
6. Matmuls use Tier-1 HMMA kernels with on-the-fly Q4_K/Q6_K decode and FP32 accumulators; activations stay FP16 (C10 measured no benefit from FP32).
7. Fallback: any failure at build time leaves the session on the CPU path, and `slm launch` says so; `--gpu on` fails instead.

## Alternatives considered

- Improve the per-op hybrid path: rejected; the overhead budget of 46 us per op leaves no room for per-op copies and evicts.
- Dequantize weights to FP16 at load: rejected as the default. It needs about 3 GB, and reading 3 GB per token at 43 tok/s is about 129 GB/s, above the LPDDR5 limit. Worth a measurement only for the small FFN tensors.
- Port llama.cpp's ggml-cuda runtime: rejected; it needs the CUDA runtime, which SLM-OS does not have, and the kernels ship as PTX.
- Keep SIMT-only kernels: rejected as the final state because the DoD requires HMMA on all matmul ops, but SIMT is the first working reference per plan decision M6-D4.

## Consequences

Execution moves out of `forward_one` for GPU sessions, so two forward paths exist; the CPU path stays the reference, and the goldens compare them. New kernels need rebuilding with nvcc; shader builds become a make target. Kexec is not supported by this path.

## Evidence

Pending: per-kernel measurements under JetPack, then end-to-end numbers. This ADR is updated with them.
