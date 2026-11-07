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

Host only so far. `runtime/src/slm/gpu_program.rs` runs the plan on a software GPU built from the CPU kernels: decode and batched prefill match llama.cpp's logits on `tiny_qwen2.gguf` and stay within 1e-2 of `forward_one`, a hung step falls back with the same greedy tokens. `kernel/tests/test_slm_prog.c` covers pushbuffer, QMD, cbuf and arena contents. Decode for Qwen2.5-1.5B is 422 launches with 308 barriers.

Pending: `slm gpu smoke` and `slm gpu golden` on jetson-nano-2, then decode tok/s and prefill ms against ADR 0001. This ADR is updated with them.

## Implementation notes

- Layout: `slm launch` builds a plan in Rust; `kernel/gpu/slm_prog.c` encodes it; `kernel/gpu/nvidia/ga10b_prog.c` holds the pure pushbuffer, QMD and cbuf writers.
- Barriers: WAIT_FOR_IDLE plus INVALIDATE_SHADER_CACHES (DATA, FLUSH_DATA), as NVK does for compute. `slm gpu opts` selects cheaper forms for measurement.
- Page tables: weights and data use 64 KB PTEs, about 140 KB of tables for 1.1 GB; `slm xload` memory is scattered and maps at 4 KB, which works at about 2.2 MB of tables.
- KV append needs no op: K and V projections write into the position's cache slot, so `slm_kv_append` isn't launched. Kernels read positions and token ids from the step page, so a decode step patches only K and V output pointers and RoPE on K (84 cbufs for 28 layers).
- Other consumers: a dispatcher that finds a program in flight releases the dispatch lock until it ends; sched-MLP's try-lock takes its CPU fallback. Any CPU can retire the program from its end semaphore, so a waiter with IRQs masked cannot deadlock on the owner (#1093 class).
- Failure: a timeout (5 s) dumps GR and MMU fault state once and kills the program. The session rebuilds its KV cache on the CPU by replaying its tokens, since a wedged GPU cannot be read back.
- Kexec: there is no cold channel, so `--gpu on` is refused and `--gpu auto` keeps the per-op tier path. The per-op path is otherwise unchanged.
