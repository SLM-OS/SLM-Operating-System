# ADR 0001: llama.cpp CUDA at the matching clock is the #475 reference bar

## Context

Definition of Done item 6 sets absolute numbers (at least 25 tok/s, under 400 ms TTFT). Issue #475 also has to meet the reference bar of llama.cpp (CUDA) under JetPack on the same board, with the same GGUF and the same 64-token prompts at temperature 0. The power mode changes that reference by about 10 percent for decode and by nearly a factor of two for prefill.

## Decision

Reference: llama.cpp ca89968a, CUDA 12.6, `-ngl 99 -t 4 -c 4096 -b 64 --temp 0 -no-cnv`, warm kernel cache, jetson-nano-2, prompts in `bench/475/prompts/`, runner `bench/475/run-llama.sh`.

| Mode | GPU clock | EMC | Decode tok/s (median) | Prefill of 63 tokens (median) |
|---|---|---|---|---|
| 25W (board default) | 918 MHz, about 907 observed | 3199 MHz under load | 39.1 | 135 ms |
| MAXN_SUPER + jetson_clocks | 1020 MHz | 3199 MHz | 43.0 | 74 ms |

SLM-OS is compared with the row matching the GPU clock it runs at. If it runs at 1020 MHz, the bar is 43.0 tok/s and 74 ms. The DoD numbers apply as well.

Token-for-token equality is not a criterion. Greedy output is required to be coherent and is compared with the reference, and the first divergence is reported.

## Alternatives considered

- DoD numbers alone: rejected, they are weaker than the bar the issue sets.
- 25W row whatever the SLM-OS clock: rejected, it would flatter SLM-OS at 1020 MHz.
- Token-exact match: rejected. llama.cpp's own scalar and AVX2 builds diverge from each other at steps 15, 45 and 6 on these kinds of prompts, so the criterion would measure rounding noise.
- First-run numbers: rejected. The build has no sm_87 kernels and JIT compiles once (1.16 tok/s, 57 s), which is a build artifact rather than steady-state speed.

## Consequences

The bar is well above the DoD. Prefill needs batched execution: 64 tokens one at a time at 43 tok/s would take 1.5 s. Memory clock on the UEFI path has to be measured and set, because the reference runs at 3199 MHz EMC.

## Evidence

`bench/475/llama-results-ca89968a.tgz`. Output for p1 is byte-identical between runs and between power modes. Board power during the MAXN run was about 17.4 W (VDD_IN).
