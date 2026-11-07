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

jetson-nano-2, cold UEFI-direct boot, model from `/boot/models`, GPC 1020 MHz, EMC 3199 MHz. Program: 423 launches, 310 barriers, 19 MB arena at `--ctx 512`, weights on 64 KB PTEs, C3K kernels at 97d69f43 built with CUDA 12.0.

| p1, p2, p3 x 3, `--raw`, 256 tokens | SLM-OS | Bar (ADR 0001, 1020 MHz) | Native, same kernels |
|---|---|---|---|
| Decode | 49.42 to 49.44 tok/s | 43.0 tok/s | 44.2 tok/s |
| Prefill | 88.5 ms GPU, 64 rows | 74 ms, 63 tokens | 97.3 ms |
| First token | 91 to 92 ms | | |

Decode meets the bar. Prefill does not.

Output is coherent and identical across runs. It departs from llama.cpp's output at p1 "page tables. The", p2 "She had" and p3 "repeatedly". On the earlier kernels at p1, SLM-OS's CPU path printed the same 64 tokens as the program.

A decode step is 19.9 ms, GPU 19.4 ms, evict 52 us. `slm gpu opcheck`: every kernel at 1, 2, 9 and 64 rows matches an exact dot product within 7 milli on real tensors, so FP16 subnormals are honoured. `slm gpu golden` on the tiny model: same top token on 10 of 10 steps, at most 1.423 from llama.cpp (limit 1.5) and 0.021 from the exact CPU reference (limit 0.1). Launch overhead, measured with the earlier kernels: 4.1 us per launch and barrier, against CUDA's 1 to 3 us.

Failure on hardware: `slm gpu inject hang` and `fault` each time out once at 5 s, print GR state (the fault names its VA), replay on the CPU with the same tokens, and leave the shell working. Consumers on that channel then time out too until a reboot. Race: `nvgpu cold race 2000` during p1 gave 4000 correct results, no errors, unchanged text, decode 48.92 tok/s, slowest consumer pair 87.6 ms (it waited out the prefill).

## Implementation notes

- `slm launch` builds the plan in Rust (`gpu_program.rs`); `slm_prog.c` encodes it; `ga10b_prog.c` writes the pushbuffer, QMDs and cbufs.
- Decode uses the `_b1` matmuls and the `_w8` attention and norm; prefill uses `_bs` for N above 256 and split `_b64` otherwise. Barriers are WAIT_FOR_IDLE, with the QMD invalidating shader data. A program ends with WAIT_FOR_IDLE and a flush before its semaphore: the release alone fired about 1 us after launching a kernel still running, which returned half-computed logits.
- KV append needs no op: K and V projections write the position's cache slot. Kernels read positions and token ids from the step page, so a decode step patches 84 cbufs.
- A dispatcher that finds a program in flight releases the dispatch lock until it ends; sched-MLP's try-lock takes its CPU fallback. Any CPU can retire the program from its end semaphore, so a waiter with IRQs masked cannot deadlock (#1093 class).
- A timeout (5 s) dumps GR and MMU fault state once. The session rebuilds its KV cache on the CPU by replaying its tokens.
- After kexec there is no cold channel: `--gpu on` is refused and `--gpu auto` keeps the per-op path.
