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

jetson-nano-2, cold UEFI-direct boot, model from `/boot/models`, GPC 1020 MHz. EMC reads 204 MHz after boot; a BPMP floor request at launch raises it to 3199 MHz. Program: 423 launches, 310 barriers, 19 MB arena at `--ctx 512`, weights on 64 KB PTEs.

| p1, `--gpu on`, 256 tokens | SLM-OS | Bar (ADR 0001, 1020 MHz) |
|---|---|---|
| Decode | 21.9 tok/s | 43.0 tok/s |
| First token, 64-row prefill included | 218 ms | 74 ms prefill |

Output is coherent. It departs from llama.cpp after "...called page tables. The". SLM-OS's CPU path (0.41 tok/s) printed the same 64 tokens as the program, so that departure is not caused by the GPU path.

Per decode step at 64 tokens: GPU 40.9 ms of a 41.4 ms step, evict 52 us; matmuls 31.9 ms, attention 7.7 ms. The gap to the bar is in the kernels. A chain of 32-element RMSNorm launches costs 4.1 us per launch and barrier, against CUDA's 1-3 us: about 1.7 ms per token.

`slm gpu opcheck`: embed, RMSNorm, add, SwiGLU, RoPE, GQA and Q6_K matmuls agree with the CPU reference at 1, 2 and 9 rows. Q4_K matmuls differ by up to 0.25 absolute (rms under 0.02), not yet explained; the CPU reference rounds through Q8_K and the kernel does not. `slm gpu golden` on the tiny model picks the same top token as llama.cpp on 10 of 10 steps (8 of 8 batched), but exceeds the 1.0 logit tolerance (max 1.43), so it reports FAIL.

`nvgpu cold check` passes between prompts on the same channel. Not tested on hardware: a real GPU hang, and a consumer racing a step in flight.

## Implementation notes

- `slm launch` builds the plan in Rust (`gpu_program.rs`); `slm_prog.c` encodes it; `ga10b_prog.c` writes the pushbuffer, QMDs and cbufs.
- Barriers are WAIT_FOR_IDLE, with the QMD invalidating shader data. A program ends with WAIT_FOR_IDLE and a flush before its semaphore: the release alone fired about 1 us after launching a kernel still running, which returned half-computed logits.
- KV append needs no op: K and V projections write the position's cache slot. Kernels read positions and token ids from the step page, so a decode step patches 84 cbufs.
- A dispatcher that finds a program in flight releases the dispatch lock until it ends; sched-MLP's try-lock takes its CPU fallback. Any CPU can retire the program from its end semaphore, so a waiter with IRQs masked cannot deadlock (#1093 class).
- A timeout (5 s) dumps GR and MMU fault state once. The session rebuilds its KV cache on the CPU by replaying its tokens.
- `slm stats --profile` runs decode steps on a second pushbuffer in the same region: same QMDs and cbufs, plus one FLUSH_DISABLE timestamp after each barrier that crosses an op class. `slm gpu opts ... profile` remains the per-launch tool; its drains change timings.
- After kexec there is no cold channel: `--gpu on` is refused and `--gpu auto` keeps the per-op path.
