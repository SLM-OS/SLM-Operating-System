# GPU Policy-Model Inference — Spec

Wire the AI scheduler and page-eviction policy models to the same
GA10B compute-dispatch path the MNIST inference engine uses today,
so the `gpu use sched on` / `gpu use eviction on` toggles produce
real GPU work instead of accepting-as-scaffold.

**Tracking:** to be filed alongside the first implementation PR.
**Status:** spec only. No code changes proposed in this document.

---

## Why this matters

`gpu use inference on` already works end-to-end: the MNIST whole-graph
fastpath in `engine::run_inference` consults the master flag, the
per-model `model use-gpu` flag, and dispatches eight ops (Conv2D,
Add+ReLU, MaxPool, …, MatMul, AddBias) through the inherited
GA10B channel handoff. The latency is observable (~395 ms vs CPU
~45 ms), the predictions are correct, the v6 channel-inherit story
is documented in `docs/jetson-cbb-report.md`.

The `sched` and `eviction` toggles are accept-with-warning today.
Flipping `gpu use sched on` records operator intent but the
scheduler MLP/PPO inference still runs on CPU NEON (`runtime/src/sched/inference.rs`).
Same for the page-eviction Q-network in
`runtime/src/eviction/`. To make these toggles actually move the
workload onto the GPU, three pieces have to land per policy:

1. A SASS shader bundle that computes the policy's forward pass.
2. A v6+ pipeline-handoff producer in `scripts/` that allocates GPU
   buffers, loads weights, builds the QMD chain, and stays alive
   across kexec.
3. SLM-OS-side dispatch glue that exposes `slm_gpu_run_<policy>`
   and consults the per-consumer toggle.

This is the same shape of work `gpu-kernel-mnist.c` did for inference
— the difference is per-policy-model topology and a different op set.

## Scope (in)

- **Scheduler MLP** — the small (≈64 → 128 → action) MLP in
  `runtime/src/sched/inference.rs`'s `mlp_forward`. Two `gemm_fp32`
  calls plus ReLU between them. The output is a 1-of-N argmax over
  candidate-CPU action indices.
- **Eviction Q-net** — the per-page Q-network used by
  `runtime/src/eviction/`. Topology TBD by reading the actual code,
  but it's a comparably-small classifier.
- **Per-consumer SLM-OS dispatch entry point** under
  `slm_gpu_run_sched_inference()` / `slm_gpu_run_eviction_inference()`
  with the same shape as `slm_gpu_run_mnist`.
- **Engine eligibility** that consults `gpu_consumer_enabled(GPU_CONSUMER_SCHED)`
  / `_EVICTION` before dispatching.
- **Channel-handoff producer** for each — extending `gpu-kernel-mnist`'s
  pattern with new SASS shaders for the policy ops.

## Scope (out)

- **Tensor-core (HMMA) shaders for policy MLPs.** Policy MLPs stay
  on FFMA fp32 in this spec. MNIST has since gained an HMMA tier
  via `--gemm-tier hmma` on the launcher (#661/#676; backed by the
  `gemm_hmma_fp16` and `gemm_hmma_fp32a_fp16w` kernels in
  `scripts/cuda/`), but the policy MLP path is intentionally not
  touched here — moving policy ops onto tensor cores is its own
  follow-up spec, and the FP32 MLP path is good enough for the
  inference latency these policies need.
- **Quantised inference paths.** All policy weights stay fp32 in v1.
- **Online training / weight updates from SLM-OS.** Read-only
  forward pass; weight refresh is still the host helper's job.
- **Multi-policy concurrency.** One GPU dispatch in flight at a time;
  serialised through the existing engine spinlock.
- **Removing the CPU fallback.** Always available; the toggle is
  override-on-top.

---

## Per-policy work breakdown

### A. Shaders

PR-1 inspection result. Both policies are pure FFMA-fp32 matmul +
ReLU chains; neither uses softmax. Sched does an argmax; eviction
returns a sigmoid'd scalar score, no argmax. The MNIST shader kit
already covers everything except the eviction output's sigmoid.

| Op | Source | Notes |
|---|---|---|
| `gemm_fp32` | `scripts/cuda/gemm_fp32.cu` | already exists; covers every linear layer in both policies |
| `add_bias_relu_fp32` | `scripts/cuda/add_bias_relu_fp32.cu` | already exists; covers L0-L2 of sched and L1-L3 of eviction |
| `add_bias_fp32` (sched output, eviction output pre-sigmoid) | new (drop the ReLU branch from `add_bias_relu_fp32.cu`) | Or: pass an `apply_relu=0` flag on the existing kernel — MNIST's launcher already plumbs that bit |
| `sigmoid_fp32` (eviction only) | new | scalar reduction over a single fp32; tiny kernel |
| `argmax_fp32` (sched only — final decode) | optional | Pure reduction. Cheaper to leave the argmax CPU-side at 42 elements; only worth a kernel if the launcher needs it for sentinel verification. |

#### Sched MLP — op DAG

Source: `kernel/sched/ai/ai_inference.c::forward_logits`. Shape
constants in `kernel/sched/ai/ai_types.h:105-115`.
`AI_SCHED_N_ACTIONS = 42` on Jetson / x86-64 / QEMU (6 cores ×
3 priorities × 2 GPU bits); `24` on Pi 5 (4 cores × 3 × 2). The
spec assumes the Jetson value below — the only knob that varies
across the action-count split is `N` in the Layer 3 weight tensor.

| # | Op | In shape | Weight shape | Bias | Activation | Out shape |
|---|---|---|---|---|---|---|
| 0 | gemm | state [108] | W0 [256×108] | b0 [256] | ReLU | h0 [256] |
| 1 | gemm | h0 [256] | W1 [256×256] | b1 [256] | ReLU | h1 [256] |
| 2 | gemm | h1 [256] | W2 [128×256] | b2 [128] | ReLU | h2 [128] |
| 3 | gemm | h2 [128] | W3 [42×128] | b3 [42] | none | logits [42] |

After op 3 the kernel does `argmax(logits)` on CPU and decodes the
action index via `ai_decode_action` (CPU, scheduler-internal — no GPU
work). 4 GPU ops total; same pattern as MNIST's
`gemm + add_bias_relu` pair, just three of them in series + one
plain `gemm + add_bias` finalizer.

Weight blob layout (for `gpu-kernel-sched-mlp.c` to load): the
runtime accepts both compiled-in (`ai_mlp_w*` / `ai_mlp_b*` from
`ai_weights_mlp.c`) and runtime-uploaded (`sched_runtime_mlp_*`)
sources via the `sched_runtime_mlp_acquire/release` shim. For the
launcher, dump the compiled-in arrays once and stage them under
`scripts/sched-weights/` mirroring the MNIST layout.

Total weight floats:
`(108·256 + 256) + (256·256 + 256) + (256·128 + 128) + (128·42 + 42)
 = 27,904 + 65,792 + 32,896 + 5,418
 = 132,010 floats = 528,040 B ≈ 516 KB`.
Workspace (h0 + h1 + h2 = 256+256+128 floats = 2,560 B) is trivial.

#### Eviction MLP — op DAG

Source: `runtime/src/mm/eviction/runtime_mlp.rs::predict`. Shape
constants in the same file (lines 15-19). Note: this is the
runtime-loaded `MLP1` payload format used by `model load` of an
`.evi.bin` blob. The compiled-in default eviction policy on
`PLATFORM_JETSON_ORIN_NANO` is XGBoost (`runtime_xgboost.rs`), not
this MLP — but the toggle wiring is the same once the MLP is the
active policy.

| # | Op | In shape | Weight shape | Bias | Activation | Out shape |
|---|---|---|---|---|---|---|
| 0 | gemm | features [27] | W_L1 [64×27] | b_L1 [64] | ReLU | h1 [64] |
| 1 | gemm | h1 [64] | W_L2 [32×64] | b_L2 [32] | ReLU | h2 [32] |
| 2 | gemm | h2 [32] | W_L3 [16×32] | b_L3 [16] | ReLU | h3 [16] |
| 3 | gemm | h3 [16] | W_OUT [1×16] | b_OUT [1] | sigmoid | score [1] |

5 GPU ops if we keep sigmoid as a separate kernel; 4 if we inline
the sigmoid into the final `add_bias` shader (a 1-element scalar
op — cheap to specialize). Total weight floats:
`(27·64 + 64) + (64·32 + 32) + (32·16 + 16) + (16·1 + 1)
 = 1,792 + 2,080 + 528 + 17 = 4,417 floats = 17,668 B ≈ 17.3 KB`.
Smaller than MNIST's weights — comfortably fits the same channel-
handoff layout.

`features[27]` is a `BlockFeatures` row from
`runtime/src/mm/eviction/policy.rs`; the launcher needs to mirror
that layout. The output is a single fp32 in `[0,1]` (eviction
probability score) — the eviction loop ranks pages by this score
and evicts the highest. Output decoding is CPU-side; no GPU
argmax needed.

#### Implications for shader work

- The MNIST `gemm_fp32` kernel handles the M=1 row-vector case
  already (used for the MNIST FC layer). Both policies need only
  M=1 GEMMs against rectangular weight matrices, so no new GEMM
  variant is required.
- The MNIST `add_bias_relu_fp32` kernel's `apply_relu` bit covers
  everything except the eviction sigmoid. That's the single new
  shader needed — `sigmoid_fp32` (or an extension to the existing
  bias kernel with an `activation_kind` enum).
- Sched needs no new shaders at all if argmax stays CPU-side
  (which it should, at N=42).

Build pipeline matches MNIST:

```
nvcc -arch=sm_87 -o <kernel> <kernel>.cu
cuobjdump --extract-elf all <kernel>
readelf -SW <kernel>.2.sm_87.cubin | awk '/\.text\./ ...'
dd if=<cubin> of=<kernel>_shader.sass bs=1 skip=$off count=$size
```

(Same `scripts/build-shaders.sh` recipe added in the GPU-for-real
investigation; extend it to cover the new shaders.)

### B. Channel-handoff producer

Two new launchers, modelled on `gpu-kernel-mnist.c`:

- `scripts/gpu-kernel-sched-mlp.c` — loads sched MLP weights from
  `scripts/sched-weights/` (extracted from the SLM-OS Rust runtime's
  embedded `scheduler_mlp.bin`), builds an N-op pipeline (3-5 ops),
  publishes a v6 handoff with `pipeline_n_ops` and an
  `input_buf_phys` sized for the policy's feature vector.
- `scripts/gpu-kernel-eviction-qnet.c` — same shape for the eviction
  Q-net.

Each producer takes `--preserve-for-kexec`, runs the dispatch once
on Linux to validate against a CPU reference, then sleeps holding
the channel. Linux nvgpu must reach FECS/GPCCS PASS state first
(same prerequisite as MNIST).

The handoff carries a new `pipeline_kind` discriminator (`MNIST`,
`SCHED_MLP`, `EVICTION_QNET`) so SLM-OS knows which dispatch entry
to use; today there's only `MNIST` implicitly.

### C. SLM-OS dispatch

In `kernel/src/slm_ffi.c`:

```c
int slm_gpu_run_sched_inference(const float *features,
                                size_t feature_count,
                                float *action_logits_out,
                                size_t action_count);

int slm_gpu_run_eviction_inference(const float *page_features,
                                   size_t feature_count,
                                   float *q_values_out,
                                   size_t q_count);
```

Each delegates to `ga10b_bringup_set_input` + `ga10b_bringup_launch_kernel`
+ `ga10b_bringup_read_pipeline_output`, exactly as
`slm_gpu_run_mnist` does. The pipeline-kind discriminator selects
which channel handoff to inherit (the producer guarantees a unique
magic per kind).

In `runtime/src/sched/inference.rs`:

```rust
fn sched_gpu_eligible() -> bool {
    extern "C" {
        fn slm_gpu_inference_enabled() -> i32;     // master
        fn slm_gpu_consumer_sched_enabled() -> i32; // per-consumer
    }
    unsafe { slm_gpu_inference_enabled() != 0 && slm_gpu_consumer_sched_enabled() != 0 }
}
```

Mirror in the eviction module. Add the C-side getters in
`slm_ffi.c` wrapping `gpu_consumer_enabled(GPU_CONSUMER_SCHED)` /
`_EVICTION`.

In `kernel/src/sched_policy_*.c`: flip `has_gpu_backend = true` on
each policy that has a working GPU dispatch. The C-side
`active_sched_policy_has_gpu_backend()` gate already plumbs through
to the toggle accept logic.

### D. Eviction policy plumbing

✅ **Landed in PR-4 (2026-04-27).** The eviction trait
(`runtime/src/mm/eviction/policy.rs::EvictionPolicy`) gained a
default-`false` `has_gpu_backend(&self) -> bool` method, mirroring
the sched-side `sched_policy_ops::has_gpu_backend` field. The
runtime registry exposes `any_active_policy_has_gpu_backend()`
which scans all installed pool policies; this is bridged to C via
`rust_eviction_active_policy_has_gpu_backend()` (FFI) and
`eviction_active_policy_has_gpu_backend()` (C-callable wrapper in
`kernel/src/slm_ffi.c`, declared in `slm_ffi.h`).

`gpu_consumer.c`'s `GPU_CONSUMER_EVICTION` validation now consults
the C wrapper and emits an updated "scaffold only — no eviction
policy declares a GPU backend yet" warning when no policy has
flipped its return value. The toggle still flips so operators can
record intent ahead of PR-6's dispatch landing — symmetric to the
sched accept-with-warning path.

Test coverage in `runtime/src/lib.rs::rust_eviction_selftest`
installs a `GpuBacked` test policy, asserts the registry scan +
FFI both report true, then reverts to the default and verifies
both report false. C-side
`test_eviction_active_policy_has_gpu_backend_default_false` pins
the C wrapper.

### E. Test coverage

For each policy:

1. **Host-side unit test** comparing the SASS shader's output against
   the Rust CPU reference for a fixed input (1e-5 fp32 epsilon).
2. **Linux-side launcher self-check** — the producer's `argmax: GPU=N CPU=N`
   gate (same shape as `gpu-kernel-mnist.c`'s
   `SUCCESS: GPU and CPU agree on MNIST class N`).
3. **SLM-OS-side regression** under boot tests that asserts the
   per-consumer toggle gates dispatch (master OFF + per-consumer ON
   = CPU; both ON = GPU; per-consumer OFF = CPU).
4. **Hardware verification** on jetson-nano-2 with serial-trace
   capture of the `[engine] sched GPU fastpath dispatching` /
   `eviction GPU fastpath dispatching` log lines.

---

## Sequencing

| PR | Content | Hardware needed |
|---|---|---|
| PR-1 | ✅ **Read-and-document — landed 2026-04-27.** Op DAGs and shape tables for both policies are in §A above. Key findings: both are FFMA-fp32 matmul + ReLU chains; the only new shader needed is `sigmoid_fp32` for the eviction output (sched stays in the existing MNIST shader kit, with argmax left CPU-side). The skeleton in `runtime/src/sched/inference.rs` is a Phase-5 placeholder — the real sched MLP forward lives in `kernel/sched/ai/ai_inference.c::forward_logits` (C, gated on `AI_SCHED=ON`). | none |
| PR-2 | ✅ **Sched shaders + launcher — landed 2026-04-27.** `scripts/gpu-kernel-sched-mlp.c` runs the 8-op pipeline (gemm + addrelu × 3, then gemm + addbias) on GA10B by reusing MNIST's `gemm_fp32` and `add_bias_relu_fp32` SASS shaders unchanged. Weights extracted from `kernel/sched/ai/ai_weights_mlp.c` by `scripts/sched-extract-weights.py` (CC0 / project-internal — the C source is auto-generated from the sibling project per `scripts/import_ai_weights.sh`); the script transposes each W from the C source's `[OUT][IN]` layout into the GEMM-expected `[IN][OUT]`. Linux-side self-check passes 5/5 deterministically: argmax GPU=CPU=8, max abs err 0.000275 across the 42-element logit vector. The handoff format does NOT yet carry a `pipeline_kind` discriminator — PR-3 will add that field to the handoff struct so SLM-OS can branch between MNIST and SCHED_MLP dispatch entries. | Jetson |
| PR-3 | ✅ **Sched dispatch — landed 2026-04-27. Dispatch-cliff fix landed 2026-05-06 (#651 + #653, commit 87849d5f).** Handoff struct gained a `pipeline_kind` discriminator (repurposes `_pad4`; default 0=MNIST keeps backward compat). New `ga10b_bringup_channel_kind(b, wanted_kind)` finds the matching handoff in DRAM. `slm_gpu_run_sched_inference(state, len, logits)` C-side entry parallels `slm_gpu_run_mnist`; `g_sched_bringup` global parallels `g_mnist_bringup`. `ai_mlp_forward_logits` routes through the GPU path when `gpu use sched on` AND the sched-MLP handoff is present, falling back to CPU NEON otherwise. `sched_policy_ai_mlp.has_gpu_backend = true` so the toggle accepts cleanly. Hardware-verified on jetson-nano-2: 3/3 `slm.task_create` calls each dispatched all 8 ops on GPU (`[GA10B-P8] payload observed — GPU executed submit` × 24), `[GA10B-P6] Found handoff at phys 0x116e13000 (kind=1)`. **Cliff fix:** the original landing exposed a 5 ms IRQ-off dispatch cost that wedged the kernel under task-create bursts (#652 — six CPUs piling up on `g_gpu_dispatch_lock` from concurrent `assign_cpu` calls starved the timer tick / RX-stall watchdog). Closed by `GPU_SCHED_DISPATCH_RATE_LIMIT_NS = 50 ms` rate limiter + `spin_trylock` on the dispatch path; over-budget or contended callers fall back silently to CPU NEON via the existing `forward_via_device` rc=-1 branch. Worst-case IRQ-off load now bounded at ≈ 10 % of one CPU instead of "5 ms × N CPUs per burst". 7 cross-platform predicate tests in `kernel/tests/test_gpu_dispatch_breaker.c` pin the boundary semantics. | Jetson |
| PR-4 | ✅ **Eviction trait + scaffold — landed 2026-04-27.** Default-false `EvictionPolicy::has_gpu_backend()` method added; registry-side `any_active_policy_has_gpu_backend()` scans both pools; C-side `eviction_active_policy_has_gpu_backend()` wraps the FFI; `GPU_CONSUMER_EVICTION` validation in `gpu_consumer.c` now consults it and emits the updated "no eviction policy declares a GPU backend yet" warning. Tests cover both branches via the GpuBacked test-only policy. | none |
| PR-5 | **Eviction shaders + producer.** | Jetson |
| PR-6 | **Eviction dispatch.** Same shape as PR-3 for the eviction Q-net. | Jetson |

Each PR ships independent test coverage and an end-to-end check on
jetson-nano-2 (or, when nvgpu's ACR-bootstrap is unhappy on a board,
the host-side launcher's CPU/GPU agreement self-check).

## Prerequisites / blockers

- Linux nvgpu must reach FECS/GPCCS PASS state on the test board for
  the channel-inherit path to work. This is occasionally flaky on
  the lab Jetson Nano under `apt upgrade` drift; it's a board-side
  concern, not a SLM-OS-side blocker, but it gates verification.
- The MNIST GPU dispatch path (`scripts/gpu-kernel-mnist.c` +
  `slm_gpu_run_mnist`) is the working baseline. Any drift in the
  channel-handoff format (`struct ga10b_channel_handoff`) reaches
  every policy producer.
- Tensor-core support is **out of scope** here — these are FFMA-only
  shaders. Tensor-core variants are tracked in the per-op GPU
  dispatch spec follow-up (`docs/specs/gpu-per-op-dispatch.md`).

## See also

- `docs/specs/gpu-inference.md` — current GPU capabilities matrix
- `docs/specs/gpu-per-op-dispatch.md` — long-term per-op GPU
  dispatch architecture (separate spec)
- `docs/jetson-cbb-report.md` §"Compute dispatch" — proof that
  bare-metal SLM-OS can ring the doorbell and execute compute
  kernels on GA10B
- `kernel/include/sched_policy.h` §`has_gpu_backend` — the field
  this spec proposes flipping per-policy
- `runtime/src/sched/inference.rs` — current CPU MLP forward pass
- `kernel/gpu/nvidia/ga10b_bringup.c` §`launch_kernel` — the
  v5/v6 pipeline-mode dispatch this spec reuses

*Last updated: 2026-05-07 — PR-1 (op DAG + shape tables), PR-2 (sched launcher), PR-3 (sched dispatch + pipeline_kind discriminator + dispatch-cliff fix #651/#653), and PR-4 (eviction-trait scaffold) landed. PR-5/PR-6 (eviction shaders + dispatch) deferred — the eviction MLP weights are runtime-loadable only (no compiled-in source to extract from like ai_weights_mlp.c), so PR-5 needs either a trained `.evi.bin` from the sibling slm-os-page-sim project or a synthetic-weight test path.*
