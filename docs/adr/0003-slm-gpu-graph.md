# ADR 0003: run the whole SLM forward pass as one GPU command stream

## Context

The bar in ADR 0001 is 43.0 tok/s decode and 74 ms for a 63-token prefill. The hybrid path dispatches one op at a time, copies activations through 64 KB scratch slots, evicts L2 per op and holds an IRQs-off lock over each round trip. A token needs about 500 ops, so the 23 ms budget allows about 46 us per op, compute included; one RMSNorm call alone costs 50 to 60 us of overhead. Op-at-a-time prefill cannot meet 74 ms.

## Decision

At `slm launch --gpu auto|on`, build one GPU program per session on the cold channel:

1. Map the GGUF weights into the channel's VM without copying.
2. A session arena holds activations, KV cache, QMDs, cbufs and pushbuffers.
3. One prebuilt pushbuffer holds every launch, separated by compute barriers, ending in one semaphore. A step patches only token ids and positions. Only last-position logits reach the CPU, after one L2 evict.
4. Prefill is the same program at up to 64 rows.
5. The CPU waits with IRQs enabled and without the dispatch lock held.
6. Matmuls use HMMA kernels that decode Q4_K/Q6_K on the fly, with FP32 accumulators and FP16 activations.
7. A failure at build time leaves the session on the CPU path; `--gpu on` fails instead.

## Alternatives considered

- Improve the per-op path: rejected; 46 us per op leaves no room for per-op copies and evicts.
- Dequantize weights to FP16 at load: rejected; about 3 GB, read at about 129 GB/s at 43 tok/s, above the LPDDR5 limit.
- Port ggml-cuda: rejected; it needs the CUDA runtime and ships PTX.
- SIMT-only kernels: kept only as the first reference (M6-D4).

## Consequences

A UEFI-direct session runs every token in the program. The CPU forward path stays as the reference for the goldens and as the fallback. After a step timeout the channel is recovered and the session replays its steps on a rebuilt program on the GPU; the CPU replay is used only if that fails ([ADR 0006](0006-gpu-channel-recovery.md), #1171). The per-op hybrid path remains only for kexec boots, which have no cold channel. New kernels need nvcc (`make oplib-shaders`).

## Evidence

jetson-nano-2, UEFI-direct, GPC 1020 MHz, EMC 3199 MHz, p1 to p3 three times each at 256 tokens: decode 49.51 to 49.53 tok/s, prefill 72.4 to 72.7 ms for 64 rows, first token 75 ms. Both bars are met. Opcheck matches an exact reference at 1 to 64 rows; the tiny-model golden is within 1.423 of llama.cpp and 0.021 of the exact reference. Tensor cores carry 99.47 percent of the MACs; `ncu` shows them active 2 to 10 percent of decode and 13 to 23 percent of prefill, against llama.cpp's 0.2 to 0.9 percent in decode. Greedy text agrees with llama.cpp on 98.8 to 99.6 percent of teacher-forced steps, and every disagreement is a near tie (at most 0.13 logits). Per-run numbers, divergence points, failure tests and the native C3K results are in [the design doc](../design/slm-gpu-program.md).
