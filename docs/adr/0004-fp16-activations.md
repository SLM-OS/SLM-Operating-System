# ADR 0004: FP16 activations and exact dot products in the SLM forward pass

## Context

SLM-OS computes Qwen2.5 with FP16 activations between ops and FP32 accumulators. llama.cpp, the reference (ADR 0001), quantizes activations to 8-bit blocks before its matmuls (Q8_K on the CPU, q8_1 in CUDA). Greedy output of the two diverges after 50 to 154 tokens on the benchmark prompts, and on p2 SLM-OS fell into a repeated sentence that llama.cpp avoided. The question was whether to change the arithmetic to follow the reference.

## Decision

Keep FP16 activations and exact dot products (no activation quantization) on both the CPU and GPU paths. Golden tests compare against two references: an exact CPU computation (tight tolerance, 0.1 logits on the real model) and llama.cpp (loose tolerance with the top token required at every step, because of its activation quantization).

## Alternatives considered

- FP32 activations on the CPU path: rejected. Measured on host, it did not move the first divergence later on average (steps 15, 80, 19 against 90, 25, 6), and costs memory bandwidth.
- Emulating llama.cpp's q8_1 activations on the GPU: rejected. Top-1 agreement under teacher forcing did not improve (99.6, 98.4, 98.4 percent against 98.8, 98.8, 99.6), p3 diverged at step 20 instead of 154, and it produced the p2 repetition.
- A repetition penalty or n-gram guard: rejected for the benchmark, because the benchmark is temperature 0 greedy; it would change the thing measured.

## Consequences

Greedy text will keep departing from llama.cpp at near ties. This is expected and is not a defect. The CUDA toolkit that builds the shaders can flip a near tie (p2 differed between a 12.0 and a 12.6 build), so `scripts/cuda/build-oplib-shaders.sh` pins CUDA 12.0 (the toolkit every benchmark here used) and refuses another unless `ALLOW_CUDA_MISMATCH=1`.

The default sampler (top-k 40, top-p 0.9, temperature 0.7), which an interactive `slm prompt` uses, meets Definition of Done item 3 of `docs/plans/slm-integration-plan.md` (a coherent reply to a free-form prompt) on the GPU path. Interactive use keeps it, and the benchmark stays greedy with no repetition guard (#1168).

## Evidence

Teacher forcing on jetson-nano-2 (MAXN, llama.cpp CUDA tokens fed to the SLM-OS kernels): every disagreement sits where llama.cpp's own top-1 to top-2 gap is at most 0.13 logits; mean RMS logit difference 0.07 to 0.09. llama.cpp's own scalar and SIMD CPU builds diverge from each other at steps 15, 45 and 6. Q8_K differs from an exact dot product by up to 3.4 percent of output RMS on real weights; the GPU kernels differ by under 3e-5. Details: `docs/design/slm-gpu-program.md`, `docs/archive/475-gauntlet/475.md`.

Default sampler (2026-11-08, PR #1182): an image built as 7745382b, whose tree equals main c69438a2's, on jetson-nano-2 after one cold UEFI-direct boot, GPU program path. 10 prompts of different kinds went through the chat template with seeds 1, 2654435769 and 3735928559, a fresh session per run and the 256-token cap. 15 runs ended at EOS and 15 at the cap, decode 40.4 to 42.8 tok/s, and a repeated run reproduced its text byte for byte. An independent critic judged 30 of 30 replies coherent against rubric (a) to (f) in PR #1182 (on topic, grammatical, no looping, no sentence more than twice, no collapse). Raw captures: `~/slmos-ref/derivatives/475-followups/1168/`.
