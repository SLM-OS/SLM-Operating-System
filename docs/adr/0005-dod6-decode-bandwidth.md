# ADR 0005: replace the 60 percent tensor-core target with decode bandwidth

## Context

Definition of Done item 6 asked for at least 60 percent tensor-core utilization during decode and HMMA on all matmul ops. Single-stream decode cannot reach that on GA10B with any implementation. Qwen2.5-1.5B needs about 3 GFLOP per token, so 49.5 tok/s is about 150 GFLOP/s, about 1 percent of the tensor cores' dense FP16 rate at 1020 MHz. Each token reads about 1 GB of weights, so decode is limited by memory: 49.5 tok/s moves about 49 GB/s of the 102.4 GB/s LPDDR5 peak. The plan's own risk table says to ship when tok/s is met but utilization is low, and its design doc assumed 30 to 50 percent, a figure that applies to batched work.

## Decision

Item 6 passes when decode is at least 25 tok/s and at least llama.cpp CUDA at the same GPU clock (ADR 0001), TTFT is under 400 ms on the 64-token prompt, all weight matmuls run on Tier-1 HMMA, and `slm stats --gpu` reports decode DRAM bandwidth as a fraction of peak (target at least 50 percent). Tensor-core figures stay in the output for information; prefill is where they are meaningful. GQA attention may stay on CUDA cores.

The bandwidth figure is derived: plan bytes per token (weights plus KV at the current position) times decode rate, over 32 bytes per EMC clock. It needs no hardware counter.

## Alternatives considered

- Keep 60 percent: rejected; no implementation meets it in single-stream decode, and llama.cpp measures 0.2 to 0.9 percent.
- Redefine utilization as the share of MACs on tensor cores: rejected; it reads about 98 percent and says nothing about speed.
- Move GQA to HMMA: rejected for now. At batch 1 it is a memory-bound vector operation over the KV cache, about 0.5 percent of the multiply work.

## Consequences

The tensor-core lines in `slm stats --gpu` are labelled information only. HMMA decode kernels show 2 to 10 percent tensor-pipe activity, much of it padding in 16 by 8 tiles holding one real row; that is not a quality signal. Batched or speculative decoding would be the way to raise it, as separate work.

## Evidence

jetson-nano-2, UEFI-direct, 1020 / 3199 MHz: decode 49.4 to 49.5 tok/s against llama.cpp's 43.0; TTFT 78 ms; all weight matmuls on HMMA. Plan bytes are about 982 MB per token (weights read by decode, plus KV; the 151936-row embedding table contributes one row), so 49.5 tok/s is about 48.6 GB/s, **47.4 percent of peak: below the 50 percent target**. The review that proposed this criterion estimated 55 GB/s from the 1.04 GiB file size, which counts the unread embedding rows. llama.cpp reads the same bytes at 43.0 tok/s, about 41 percent. The bandwidth criterion is therefore not met as written; the target needs either a parity form (at least llama.cpp's fraction) or further kernel work, and that choice is the user's. Prefill: about 197 GFLOP in 73 ms, about 2.7 TFLOPS, consistent with the 13 to 23 percent ncu tensor-pipe reading. Details: `docs/design/slm-gpu-program.md`.
