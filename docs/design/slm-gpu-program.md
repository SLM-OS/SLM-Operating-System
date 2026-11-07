# SLM GPU program: design and evidence

The decision and its reasons are in [ADR 0003](../adr/0003-slm-gpu-graph.md).
This page holds how the program is built, how it is checked, and the
measurements behind the ADR. Shell verbs are listed in
[the UEFI-direct guide](../deploy/jetson-uefi-direct.md).

## How a program is built

- `slm launch` builds the plan in Rust (`runtime/src/slm/gpu_program.rs`).
  `kernel/gpu/slm_prog.c` encodes each op for its kernel, and
  `kernel/gpu/nvidia/ga10b_prog.c` writes the pushbuffer, QMDs and cbufs.
- Weights are the GGUF tensor bytes mapped into the cold channel's VM on
  64 KB PTEs, without a copy. Weights and activations are cacheable; QMDs,
  cbufs, the pushbuffer, semaphores and the step page are volatile. One L2
  evict per step makes the logits visible to the CPU.
- The session arena holds activations, the KV cache, QMDs, cbufs and
  pushbuffers: 19 MB at `--ctx 512`.
- The program has 423 launches and 310 barriers. Barriers are
  WAIT_FOR_IDLE, with the QMD invalidating shader data. A program ends with
  WAIT_FOR_IDLE and a flush before its semaphore. The semaphore release
  alone fired about 1 us after launching a kernel that was still running,
  which returned half-computed logits.
- The embedding lookup runs on the GPU. KV append needs no op: the K and V
  projections write the position's cache slot. Kernels read positions and
  token ids from the step page, so a decode step patches 84 cbufs.
- The kernel ABI comes from `scripts/cuda/slm/MANIFEST.json`.
  `scripts/cuda/gen-slm-kernel-table.py` checks the parameter offsets and
  generates the table the encoder uses.

### Kernel selection

| Op | Decode (1 row) | Prefill, up to 8 rows | Prefill, 9 to 64 rows |
|---|---|---|---|
| Q4_K / Q6_K matmul, N above 256 | `_b1` | `_b8` | `_bs` (x in shared memory, grid ceil(N/120), block 32x8) |
| Q4_K / Q6_K matmul, N up to 256 | `_b1` | `_b8` | `_b8` split per x tile of 8 rows (grid y = ceil(B/8)) |
| RMSNorm, attention | `_w8` (block 32x8) | `_w8` | `_w8` |

The `_bs` kernels use 104 (Q4_K) and 118 (Q6_K) registers and 16 KB of
shared memory. x must be 16-byte aligned. Shaders come from
`make oplib-shaders` with host CUDA 12.0.

### Concurrency and failure

- A dispatcher that finds a program in flight releases the dispatch lock
  until it ends; sched-MLP's try-lock takes its CPU fallback. Any CPU can
  retire the program from its end semaphore, so a waiter with IRQs masked
  cannot deadlock (#1093 class).
- A step times out after 5 s. The timeout dumps GR and MMU fault state once,
  and the session rebuilds its KV cache on the CPU by replaying its tokens.
- After kexec there is no cold channel: `--gpu on` is refused and
  `--gpu auto` keeps the per-op path.

## Measurements on SLM-OS

jetson-nano-2, cold UEFI-direct boot, model from `/boot/models`, GPC
1020 MHz, EMC 3199 MHz (read by `slm gpu` after the runs). C3K kernels at
78105b51, built with CUDA 12.0. Each run: `slm reset 0`, then
`slm prompt 0 --raw --max 256`, then `slm gpu profile 0`.

| Run | Decode (tok/s) | First token (ms) | Prefill, 64 rows, GPU (ms) |
|---|---|---|---|
| p1 r1 | 49.528 | 75 | 72.67 |
| p1 r2 | 49.517 | 75 | 72.51 |
| p1 r3 | 49.513 | 75 | 72.55 |
| p2 r1 | 49.526 | 75 | 72.49 |
| p2 r2 | 49.528 | 75 | 72.60 |
| p2 r3 | 49.531 | 75 | 72.54 |
| p3 r1 | 49.518 | 75 | 72.53 |
| p3 r2 | 49.523 | 75 | 72.44 |
| p3 r3 | 49.519 | 75 | 72.52 |

The bar from ADR 0001 is 43.0 tok/s decode and 74 ms for a 63-token
prefill. Both are met. A decode step takes 19.82 ms: 19.32 ms on the GPU,
52 us of L2 evict. The previous build (C3K kernels at 97d69f43, split
`_b64` for the small-N matmuls) gave 49.42 to 49.44 tok/s, 88.5 ms of
prefill and 91 to 92 ms to the first token.

Output is coherent and identical across the three runs of each prompt. It
departs from llama.cpp's output at p1 "page tables. The" (hardware against
page), p2 "She had" (always against never) and p3 "repeatedly" (attempt
against try), the same points as with the previous kernels. On the earlier
kernels at p1, SLM-OS's CPU path printed the same 64 tokens as the program.

### Correctness checks

- `slm gpu opcheck 0`: every kernel at 1, 2, 9, 63 and 64 rows on the real
  model's tensors against an exact FP64 dot product over dequantized
  weights. 0 failed; the largest error is 7 milli (`mm_down`, 63 and 64
  rows). FP16 subnormals are honoured. The `vs q8k` lines compare against
  llama.cpp-style Q8_K activations and are informational.
- `slm gpu golden` on the tiny model: same top token on 10 of 10 steps, at
  most 1.423 from llama.cpp (limit 1.5) and 0.021 from the exact CPU
  reference (limit 0.1).
- Launch overhead, measured with the earlier kernels: 4.1 us per launch and
  barrier, against CUDA's 1 to 3 us.

### Failure and race tests

- `slm gpu inject hang` and `fault` each time out once at 5 s, print GR
  state (the fault names its VA), replay on the CPU with the same tokens and
  leave the shell working. Consumers on that channel then time out too until
  a reboot.
- `nvgpu cold race 2000` during p1 gave 4000 correct results with no errors
  and unchanged text. Decode dropped to 48.92 tok/s; the slowest consumer
  pair took 87.6 ms (it waited out the prefill).

## Kernels under JetPack (C3K)

Native CUDA 12.6 harness (`scripts/cuda/harness/`), MAXN_SUPER, 1020 MHz,
479 ops in one CUDA graph, real weights, random 63-token prompt.

| | Native harness | Bar |
|---|---|---|
| Decode at position 63 | 22.47 ms, 44.5 tok/s (44.2 with logit readback) | 43.0 tok/s |
| Prefill, 63 tokens | 71.4 ms | 74 ms |

- 147 checks pass on the real model. At B=63 the logits stay within 0.018
  of an exact CPU reference (tolerance 0.1) and the top token matches.
  Every kernel is within 1e-3 of output RMS or 2 FP16 ULP.
- Against llama.cpp on the tiny model the largest logit error is 1.42, with
  the same top token at 10 of 10 steps. CPU variants put this down to
  llama.cpp's Q8_K activations: F32 plus Q8_K gives 0.54, F32 plus an exact
  dot gives 1.41. The remaining 0.5 is not explained. Q8_K differs from an
  exact dot by up to 3.4 percent of RMS on real weights; the GPU kernels
  differ by under 3e-5.

How each step was found:

- Decode matmuls are issue-bound. The stall counters show about one
  instruction per active scheduler cycle, while reads stay under the
  72 GB/s that the tile shape sustains. Weights go into HMMA as FP16
  subnormals, which the tensor core honours, at 30 instructions per mma
  (52 at first).
- Prefill was limited by x missing the L1 and by registers. x sits in shared
  memory across 8 warps, and register use dropped from 132 to 104 so that
  more warps fit.
- Splitting K into ranges kept x in the L1 but was slower overall.
- llama.cpp's mmq uses 26 to 28 instructions per mma with int8 activations.
  These kernels stay FP16; no IMMA is used.

### Tensor-core utilization

- `ncu` counter `sm__pipe_tensor_cycles_active`, percent of peak elapsed:
  decode matmuls 2 to 10 percent, prefill 13 to 23 percent. llama.cpp under
  the same tool: decode 0.2 to 0.9 percent, prefill 1.1 to 14.6 percent.
  Neither reaches the 60 percent in the DoD.
- Mma issue cycles over matmul wall time, from known mma counts and the
  measured 3.44 G mma/s: 7.7 percent.
- MAC share on tensor cores: 99.47 percent. GQA is the only SIMT
  matmul-class op.
- On bare metal these counters are programmed through `tegra-soc-hwpm`, not
  nvgpu. Its `hwpm_dbg_regops` log lists 323 register addresses for one
  `ncu` read but no values, so SLM-OS does not read them yet.

## Greedy text against llama.cpp

Greedy text against llama.cpp CUDA (`run-teacher.sh`, p1 to p3, 256 tokens, llama.cpp's tokens fed back):

| Matmul input | Top-1 agreement | Largest llama.cpp gap where we differ | First free-run divergence |
|---|---|---|---|
| FP16 (as shipped) | 98.8, 98.8, 99.6 percent | 0.091 logits | 50, 83, 154 |
| q8_1, as llama.cpp CUDA | 99.6, 98.4, 98.4 percent | 0.126 logits | 50, 83, 20 |

Every disagreement is a near tie. The FP16 divergences are the ones seen on SLM-OS: "page tables. The" hardware/page, "She had" always/never, "repeatedly" attempt/try. Emulating q8_1 does not track llama.cpp longer, so it is not adopted. On p2, the FP16 harness output does not loop (no sentence more than twice). The q8_1 run loops on "It was a creature that was unlike any she had ever seen before." (8 times), the sentence SLM-OS repeats. So the loop follows from which near-tie branch is taken; it is not a systematic error.
