# GPU Per-Op Dispatch — Spec

Replace name-based whole-graph MNIST GPU eligibility with a graph-aware per-op
dispatch path so any model whose ops the GPU implements can run on the GPU.

**Tracking:** to be filed alongside the first implementation PR.
**Status:** spec only. No code changes proposed in this document.

---

## The problem this fixes

Today eligibility is decided by **model name**:

```rust
// runtime/src/inference/engine.rs:682
fn mnist_gpu_fastpath_eligible(model_index: usize) -> bool {
    let caps = super::gpu::GpuCapabilities::detect();
    if !caps.has_compute() { return false; }
    let info = registry::get_info(model_index)?;
    let name_len = info.name.iter().position(|&b| b == 0).unwrap_or(info.name.len());
    &info.name[..name_len] == b"mnist"
}
```

If the model is named `"mnist"`, the engine routes it through
`gpu::run_mnist_gpu_fastpath` which dispatches a **pre-built command stream**
the kernel uploaded to GPU FB at boot (the "v6 handoff", set up by
`ga10b_bringup` on Jetson and the analogous path on x86-64). Any other model
name — even an architecturally identical MNIST clone, or a model the GPU
could trivially handle — falls through to the CPU engine.

The name match is a tell: the GPU integration is at the "whole-graph fastpath"
stage, not a real op dispatcher. `gpu_execute_matmul` (the per-op entry point)
exists but stubs out:

```rust
// runtime/src/inference/gpu.rs:142
pub fn gpu_execute_matmul(...) -> Result<(), GpuError> {
    Err(GpuError::NotReady)
}
```

`select_backend` already returns `Backend::Gpu` for matmul/gemm/conv at the
right input size, but the dispatch site bottoms out at this stub.

## What "fixed" looks like

Eligibility becomes a **graph property**, not a name property:

```rust
fn graph_gpu_eligible(model_index: usize, caps: &GpuCapabilities) -> bool {
    if !caps.has_compute() { return false; }
    let graph = registry::get_graph(model_index)?;
    graph.ops().all(|op| gpu_supports(op))
}
```

`gpu_supports(op)` answers: *can the GPU run this op at the requested shapes,
dtypes, and memory layout?* When every op in the model passes, the engine
walks the graph op-by-op and dispatches each through `gpu_execute_*`. When
one op fails, the engine falls back to the CPU path for the whole graph
(no mid-graph CPU/GPU bouncing — the FB→host→FB copies would dominate any win).

The whole-graph MNIST fastpath stays as an optimisation for the specific
case where the entire pipeline is already pre-uploaded — but it's no longer
the only way to reach the GPU.

---

## Scope (in)

- **Op-level GPU FFI surface** for the four ops MNIST and similar
  small classifiers exercise: MatMul (Gemm), Conv2D, ReLU, Softmax.
  Other ops join this surface as their kernels are written.
- **Graph traversal-based eligibility check** that replaces the
  name match in `engine::run_inference`.
- **Tensor lifetime contract**: who owns the FB allocation, when
  the host→FB copy happens, when the FB→host copy happens, and
  how intermediate activations stay resident across ops.
- **Fallback-on-error policy**: any per-op dispatch error returns
  the whole inference to CPU and records a counter so the
  watchdog / telemetry feed can surface the recurrence.
- **Cross-platform**: the FFI is platform-agnostic; the kernel-side
  implementation is split between Jetson GA10B (`kernel/gpu/nvidia/ga10b_*.c`)
  and x86-64 GA10x (`kernel/gpu/nvidia/bringup.c`).

## Scope (out)

- **Quantised / int8 inference paths.** The per-op surface is fp32 only
  in v1.
- **Multi-model concurrent dispatch.** One model at a time on the GPU;
  serialised via the existing engine spinlock.
- **Mid-graph backend swap.** A graph either runs entirely on GPU (every
  op supported) or entirely on CPU. No hybrid.
- **Custom user-defined ops via Lua / WASM / ELF.** v1 only matches the
  small fixed op set above.
- **Removing the whole-graph MNIST fastpath.** It stays as a fast path
  for the pre-uploaded case; per-op is the new general path.

---

## FFI surface

New entries in `kernel/include/slm_ffi.h` (C side) and
`runtime/src/kernel_ffi.rs` (Rust side). Naming follows the existing
`slm_gpu_*` convention used by `set_mnist_input` / `run_mnist`.

| C function | Returns | Purpose |
|---|---|---|
| `slm_gpu_op_matmul(a_phys, b_phys, c_phys, m, k, n)` | `int` | C = A·B; A is m×k, B is k×n, C is m×n; all fp32 in FB |
| `slm_gpu_op_gemm(a_phys, b_phys, c_phys, m, k, n, alpha, beta)` | `int` | C = α·A·B + β·C; same layout |
| `slm_gpu_op_conv2d(in_phys, w_phys, b_phys, out_phys, params)` | `int` | NCHW input, OIHW weights, optional bias |
| `slm_gpu_op_relu(in_phys, out_phys, n)` | `int` | Element-wise ReLU; in-place when `in_phys == out_phys` |
| `slm_gpu_op_softmax(in_phys, out_phys, n)` | `int` | 1-D softmax |
| `slm_gpu_alloc_fb(size, align)` | `uint64_t` | Returns FB physical address; 0 on failure |
| `slm_gpu_free_fb(phys, size)` | `void` | Deallocates FB |
| `slm_gpu_copy_to_fb(host, fb_phys, size)` | `int` | Host → FB copy; cache-clean on host side |
| `slm_gpu_copy_from_fb(fb_phys, host, size)` | `int` | FB → host copy; cache-invalidate on host side |
| `slm_gpu_op_supported(op_type, params)` | `bool` | Capability probe used by `gpu_supports()` |

Return convention: 0 on success, negative `GpuError` discriminant on
failure (matches the existing `Result<(), GpuError>` Rust shape).

`*_phys` arguments are **GPU framebuffer physical addresses** returned
by `slm_gpu_alloc_fb`. The host never dereferences them directly; only
`copy_to_fb`/`copy_from_fb` cross the boundary.

## Eligibility check

`runtime/src/inference/engine.rs` replaces the name match:

```rust
fn graph_gpu_eligible(model_index: usize, caps: &GpuCapabilities) -> bool {
    if !caps.has_compute() { return false; }
    let info = match registry::get_info(model_index) { Some(i) => i, None => return false };
    let graph = match info.graph_ref() { Some(g) => g, None => return false };
    graph.ops().all(|op| gpu::gpu_supports(op))
}

fn gpu_supports(op: &GraphOp) -> bool {
    match op.kind {
        OpType::MatMul | OpType::Gemm => /* shapes within bounds, fp32 */,
        OpType::Conv => /* stride/pad/groups all supported */,
        OpType::ReLU | OpType::Softmax => true,
        _ => false,
    }
}
```

Eligibility is computed **once per inference call**, not per-op, so a
graph with one unsupported op falls back without partial GPU work.

## Tensor lifetimes

Inside `run_inference`, the engine maintains an FB scratch arena (a
bump allocator over a single `slm_gpu_alloc_fb`'d block, `MNIST_GPU_SCRATCH_BYTES = 1 MB`
to start). Per-call sequence:

1. **Reset** the bump arena.
2. **Upload** input tensor: `copy_to_fb(input, in_phys, n*4)`.
3. **For each op**: allocate output tensor in arena, dispatch
   `slm_gpu_op_*`, record output `*_phys` for the next op.
4. **Download** final output: `copy_from_fb(out_phys, output, n*4)`.
5. **Reset** the arena (cheap; no per-op free).

Weights and biases (constant per model) are allocated in a **separate
persistent FB region** at model load time, not in the per-call arena.
This matches what `run_mnist_gpu_fastpath` does today with the v6
handoff but generalises it.

## Failure handling

Any non-zero return from a `slm_gpu_op_*` rolls back the whole inference
to the CPU engine — same as the current MNIST fastpath does on error.
Two new counters (added to `InferenceStats`):

- `gpu_dispatch_failures` — count of per-op errors that triggered fallback
- `gpu_dispatch_attempts` — count of inferences that reached the GPU path

Surfaced via `model stats` and the M3 telemetry feed.

## Plumbing changes

| File | Change |
|---|---|
| `kernel/include/slm_ffi.h` | Add the 9 `slm_gpu_op_*` / `slm_gpu_*_fb` entries |
| `kernel/gpu/nvidia/ga10b_*.c` | Implement the ops on Jetson via PCAS2_B push-buffers |
| `kernel/gpu/nvidia/bringup.c` | Implement the ops on x86-64 GA10x once #185 unblocks |
| `runtime/src/kernel_ffi.rs` | Mirror the FFI on the Rust side |
| `runtime/src/inference/gpu.rs` | Replace `gpu_execute_matmul` stub; add `gpu_supports`, FB arena |
| `runtime/src/inference/engine.rs` | Swap name match for `graph_gpu_eligible`; per-op walk |
| `runtime/src/loader/registry.rs` | Expose graph-ref accessor (`graph_ref` above) |
| `host-tools/gsp-harness/test_*` | Per-op host-side tests using the existing harness |
| `docs/fact-sheets/gpu-inference.md` | Update matrix to reflect per-op surface |

## Test plan

1. **Host-side unit tests** (`host-tools/gsp-harness/test_gpu_per_op.c`)
   for each op: known input → expected output, fp32 epsilon = 1e-5.
2. **Engine-level test** (`runtime/src/inference/`) that builds a
   minimal 3-op graph (matmul → relu → softmax), runs it both on CPU
   and on GPU, and asserts the outputs match within epsilon.
3. **Eligibility test**: a graph containing one unsupported op routes
   to CPU; `gpu_dispatch_attempts` does not increment.
4. **Failure-fallback test**: inject a per-op error mid-graph;
   `gpu_dispatch_failures` increments and the call returns the CPU
   result.
5. **Hardware regression** on jetson-nano-2: load MNIST under the new
   path (no name match, just graph eligibility) and confirm prediction
   matches the CPU baseline. Whole-graph fastpath disabled via a
   debug flag during this test so the per-op path actually exercises.

## Prerequisites / blockers

- **Jetson #258** (PBDMA doorbell blocked at NS EL2) blocks all per-op
  dispatch on Jetson — same blocker as the whole-graph fastpath ran
  into. Per-op needs the doorbell open just like the v6 handoff did.
- **x86-64 #185** (SEC2 priv-lockdown) blocks per-op dispatch on x86
  pending the same fix.
- **Graph metadata in registry** — today `loader::registry` exposes
  `name`, `index`, etc.; it does **not** expose a structured op list.
  The first PR is "expose `graph_ref()` returning a `&[GraphOp]`",
  decoupled from the GPU work.

## Sequencing

1. **PR-1**: Registry graph-ref accessor (no behavior change). Pure
   refactor; opens the door for the eligibility check.
2. **PR-2**: `gpu_supports` + `graph_gpu_eligible` returning false for
   everything (still routes via name match). Adds the function shape
   and the test scaffold.
3. **PR-3**: First real op (`slm_gpu_op_matmul` + tests). Eligibility
   recognises matmul-only graphs.
4. **PR-4**: Conv2D, ReLU, Softmax. Eligibility recognises full MNIST
   graph topology.
5. **PR-5**: Switch the engine to prefer graph eligibility over name;
   remove the name match. Whole-graph MNIST fastpath stays as a
   detected-by-graph-shape optimisation.
6. **PR-6**: Update `docs/fact-sheets/gpu-inference.md` and close this spec.

Each PR ships independent test coverage and an end-to-end check on
jetson-nano-2 (or behind the `#258` blocker, the host-side harness).

## See also

- `runtime/src/inference/engine.rs` — current name-match path
- `runtime/src/inference/gpu.rs` — `gpu_execute_matmul` stub, `select_backend`
- `kernel/gpu/nvidia/ga10b_bringup.c` — Ampere PCAS2_B push-buffer building blocks
- `docs/fact-sheets/gpu-inference.md` — current GPU capabilities matrix
- `CLAUDE.md` §"Jetson GA10B — Ampere compute dispatch uses PCAS2_B"
- Issues: #258 (Jetson PBDMA doorbell), #185 (x86-64 SEC2 priv-lock)

*Last updated: 2026-04-26*
