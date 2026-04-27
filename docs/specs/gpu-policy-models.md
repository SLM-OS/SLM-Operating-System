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

- **Tensor-core (HMMA) shaders.** Today's MNIST shaders are FFMA
  fp32. Policy MLPs will use the same instruction class — the
  tensor-core variant is its own follow-up spec.
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

Each policy needs the same op kit currently used by MNIST, plus
whatever extra ops its forward pass uses. For both target policies
the working set is small:

| Op | Source | Notes |
|---|---|---|
| `gemm_fp32` | `scripts/cuda/gemm_fp32.cu` | already exists |
| `add_bias_relu_fp32` | `scripts/cuda/add_bias_relu_fp32.cu` | already exists |
| `softmax_fp32` (sched only) | new | small kernel; tile + reduction over N actions |
| `argmax_fp32` (sched, eviction) | new | pure reduction; no fp arithmetic in result |

For the spec we assume sched and eviction MLPs each fit into 3-5 ops
(matmul → bias+relu → matmul → bias → softmax/argmax). Confirmed by
reading `mlp_forward` in `runtime/src/sched/inference.rs` before
the PR-1 work item below.

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

The eviction trait in `runtime/src/eviction/` doesn't have a
`has_gpu_backend` flag today. Add one, mirroring the
`sched_policy_ops` field, plus a runtime getter the C side can
expose via `slm_eviction_active_has_gpu_backend()`. Update
`gpu_consumer.c`'s `GPU_CONSUMER_EVICTION` case to consult it
(currently the case just records intent; promote to the same
"reject when no policy declares it" check sched uses, OR keep the
operator-intent semantics and surface the gap in `gpu use status`).

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
| PR-1 | **Read-and-document.** Inspect `mlp_forward` in sched and the eviction Q-net forward pass; write the exact op DAG and shape table into this spec. No code changes. | none |
| PR-2 | **Sched shaders.** Build any new SASS variants needed (likely just a small softmax/argmax). Linux-side `gpu-kernel-sched-mlp.c` producing a v6 handoff; ship CPU/GPU agreement check. | Jetson |
| PR-3 | **Sched dispatch.** SLM-OS-side `slm_gpu_run_sched_inference` + Rust eligibility wiring + flip `has_gpu_backend = true` on the active sched policy. `gpu use sched on` now actually moves work onto the GPU. | Jetson |
| PR-4 | **Eviction trait + scaffold.** Add `has_gpu_backend` to the eviction trait, expose the getter, update `GPU_CONSUMER_EVICTION` validation. Pure plumbing; no GPU dispatch yet. | none |
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

*Last updated: 2026-04-26*
