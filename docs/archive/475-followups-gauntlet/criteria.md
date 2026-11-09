# Acceptance criteria, #475 follow-ups (2026-11-08)

Builders copy their issue's section into the PR body before writing code. They may make criteria more
specific (numbers, file names), never weaker. References are like-for-like: the exact CPU reference for
numerics, current main (c69438a2) measured by the same procedure for performance, fault injection for
recovery.

## G: common gate for any change touching the GPU path or the decode path

- G1 Performance on jetson-nano-2, cold UEFI-direct boot, procedure `hw-gate.md`: on every run of p1 to p3
  x 3, decode >= 49.0 tok/s, TTFT <= 80 ms, prefill (64 rows) <= 75 ms; GPC 1020 MHz and EMC 3199 MHz.
  Median decode no worse than current main's median beyond the run-to-run spread measured on main.
- G2 `/ci` green, every step.
- G3 10/10 cold boots to `slmos>` with the boot model verified.
- G4 Greedy text on p1 to p3 byte-identical to current main's, unless the issue intends a numeric change.

## #1173 GPU VM writers without a shared lock

1. Every runtime writer of the cold channel's GPU VM (reservation and mapping: `slm_prog` build and close,
   operator library `cold_reserve` / `cold_map_at` incl. `nvgpu oplib wpool`, and any other writer an audit
   finds, listed with file:line) is serialized by one lock or provably cannot run concurrently. Boot-time
   writers that run before any concurrency are listed and justified.
2. Lock order is documented and has no ABBA with the program lock or the GPU dispatch lock; a VM writer
   that holds the dispatch lock never waits for the program lock; callers with IRQs masked never spin
   (try once, fail with BUSY).
3. Fake-backend test in `kernel/tests/test_slm_prog.c`: concurrent reservations and mappings from both sides
   get disjoint VAs and lose no page-table write. Shown to fail without the fix.
4. Hardware: at least 20 overlaps of `nvgpu oplib wpool` with `slm launch 0 --gpu on` (or an equivalent
   in-kernel race driver), then a prompt: no MMU fault, no GPU timeout, p1 text identical to main.
5. G1 to G4.

## #1159 unload while an idle session uses the model (scoped by the user)

1. Establish, with code citations (file:line) and a demonstration on hardware, whether an idle session's
   GPU program can still map weight pages that `slm unload` freed. Cover: launch pinning, unload refusal,
   the close ordering (PTEs removed before the pin is dropped), the step-timeout close path, and borrowed
   `/boot/models` pages versus pages that unload really frees.
2. If yes: fix it, with a test that fails without the fix, plus G1 to G4.
3. If no: post the evidence on #1159 and move on (the CPU-only part of #1159 is out of scope tonight).

## #1171 step timeout must not wedge the channel; golden half

1. After an injected GPU step hang (`slm gpu inject hang`) the cold channel recovers with no reboot: every
   other GPU consumer on it (sched MLP, eviction Q-net, MNIST pipeline, operator library per-op path)
   returns correct results within 5 s of the timeout line.
2. At least 20 injected hangs in one boot (plus injected faults), each followed by criterion 1, measured
   per injection; the SLM session continues (GPU after recovery, or CPU replay) with the same tokens as an
   uninjected run; `nvgpu cold check` passes at the end; a fresh `slm launch 0 --gpu on` in the same boot
   runs at G1 speed.
3. Fake-backend test of the timeout and recovery sequence that fails on main.
4. Golden half: the golden test in `runtime/src/slm/gpu_program.rs` asserts `Ok` from `golden_run`, shown to
   fail when `golden_run` returns `Err`.
5. The remaining #1171 items are fixed or explicitly accepted on the issue; `Fixes #1171` only if all are.
6. `nvgpu cold race` during p1 still gives all-correct results; G1 to G4.

## #1165 real-model goldens and Q/K/V bias op vector

1. A Q/K/V bias-add golden op vector generated from the pinned llama.cpp/ggml reference
   (`scripts/fixtures/slm/`) runs in `/ci` on the host and both QEMU kernels, and fails under a deliberate
   mutation (bias dropped or misindexed).
2. The real-model goldens run as a hardware target (make target or script) on jetson-nano-2 after a
   UEFI-direct boot, with the model from `/boot/models`: real-model logits or top tokens for a fixed set of
   steps against the exact CPU reference (0.1 logits, ADR 0004) and the llama.cpp top token. Passes on the
   branch, fails on a deliberate mutation, both shown on hardware.
3. How to run it is documented. `Refs #1165` unless the issue's "Done when" (a schedule) is fully met.

## #522 sampler: one softmax pass, no per-token allocation

1. One softmax (exp) pass per token in every sampler mode, temperature 1 and not 1, shown by an
   instrumented host test.
2. Zero heap allocations per generated token in the sampler after warm-up, every mode, vocab 151936, shown
   by a counting-allocator host test.
3. Same token sequences as main's sampler for the same seeds and logits, every mode (or a bounded,
   explained difference if bit-identity is impossible).
4. Per-token sampler time no worse than main's in any mode (blind A/B of host benchmark runs), and decode
   with the default sampler on hardware no worse than main's.
5. G1 to G4.

## #1167 GQA on HMMA

1. A tensor-core GQA (Q.K^T and P.V on HMMA) matches the exact reference within the existing tolerances
   (opcheck at 1, 2, 9, 63, 64 rows and at long context).
2. Measured against `slm_gqa_w8` on the same build and clocks at kv 64, 512 and 2048, decode and prefill.
3. Either it beats `slm_gqa_w8` at long context in decode or prefill, is selected where it wins, and G1 to
   G4 hold with the long-context gain shown on SLM-OS; or the measurements show it cannot, and this is
   recorded (design doc and ADR 0005 consequences or a new ADR).

## #1161 EMC drops to 204 MHz (investigation, at most 2 h)

Either:
- a root-cause fix that holds EMC at 3199 MHz on 10/10 cold boots without the post-xhci workaround, with
  the bisection evidence; or
- an ADR with the evidence (what was isolated, what was ruled out) and a hardened workaround: the rate is
  re-asserted after every step that can reset it, a drop is detected at runtime, and `clocks` reads
  3199 MHz on 10/10 cold boots.
Plus `slm load` at the 3199 MHz speed and G1.

## #1168 default sampler holds DoD item 3

1. No change to greedy decoding (no code change on the decode path).
2. 10 prompts x 3 seeds, default sampler, GPU path, cold UEFI-direct boot, free-form `slm prompt 0 "..."`:
   every response judged coherent by the critic's rubric (on topic, grammatical, no degenerate repetition,
   no sentence repeated more than twice, no collapse before the end or the cap).
3. Recorded in ADR 0004. `Refs #1168`.

## #1169 kexec retest (no fixes)

A kexec boot of current main on jetson-nano-2 reaches the shell; `slm load` and `slm prompt` run on the CPU
and the per-op path; `--gpu on` either runs on the inherited channel or gives a clear message. Results
posted on #1169. No code fixes; defects found are filed.
