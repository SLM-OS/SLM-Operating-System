# Jetson GA10B — MNIST Inference on GPU: Execution Plan

**Last updated:** 2026-04-25
**Branch:** `jetson-gpu-multicta` (and successors)
**Companion to:** `docs/jetson-capstone-handoff.md` §"Phase 8+ compute scale-up"

**Goal:** run `models/test/mnist.onnx` end-to-end on the Jetson Orin Nano GA10B
GPU from SLM-OS post-kexec, returning correct 10-class logits for an input
image, with the GPU dispatch routed through the existing Rust runtime
(`runtime/src/inference/engine.rs`) and validated against the CPU NEON path
as a correctness oracle.

The CPU path runs MNIST today via `slm.model_load_mnist()` + `slm.model_infer()`.
This plan adds a parallel GPU path that the runtime can switch to per-op via
the existing `Backend::Gpu` selector in `runtime/src/inference/gpu.rs`.

---

## 1. Why MNIST first

MNIST is the smallest workload in the SLM-OS tree that exercises the four op
families an SLM inference loop hits: convolution, fully-connected matmul,
elementwise add/activation, and pooling. The model fits in 26 KB of weights,
runs in single-digit ms on CPU, and has known-correct outputs — every GPU
op can be cross-validated cell-for-cell against the existing
`runtime/src/inference/ops.rs` implementations.

If the Jetson GPU can run MNIST end-to-end, the remaining distance to a real
SLM (Llama-class) is mostly about scaling individual ops — same dispatch
path, larger tiles, same validation shape.

---

## 2. Model anatomy

`models/test/mnist.onnx` (26 KB, fp32):

```
[Input3]   1×1×28×28 fp32                                    784 elem
   │
   │ Conv 5×5 SAME, 8 out-ch, stride 1   (Parameter5: 8×1×5×5)
   ▼
[Convolution28_Output_0]  1×8×28×28                        6,272 elem
   │  Add bias (Parameter6: 8×1×1)
   ▼
[Plus30_Output_0]  1×8×28×28                               6,272 elem
   │  ReLU
   ▼
[ReLU32_Output_0]  1×8×28×28                               6,272 elem
   │  MaxPool 2×2 stride 2
   ▼
[Pooling66_Output_0]  1×8×14×14                            1,568 elem
   │
   │ Conv 5×5 SAME, 16 out-ch, stride 1  (Parameter87: 16×8×5×5)
   ▼
[Convolution110_Output_0]  1×16×14×14                      3,136 elem
   │  Add bias (Parameter88: 16×1×1)
   ▼
[Plus112_Output_0]  1×16×14×14                             3,136 elem
   │  ReLU
   ▼
[ReLU114_Output_0]  1×16×14×14                             3,136 elem
   │  MaxPool 3×3 stride 3
   ▼
[Pooling160_Output_0]  1×16×4×4                              256 elem
   │  Reshape (free)
   ▼
                          1×256
   │  MatMul (Parameter193: 256×10 after reshape)
   ▼
[Times212_Output_0]  1×10                                     10 elem
   │  Add bias (Parameter194: 1×10)
   ▼
[Plus214_Output_0]  1×10                                      10 elem  ← output
```

**Compute scale:** Conv1 dominates at 156k MACs; total inference ≈ 350k MACs.
That is ~5,000× more than `matmul4x4_mt`, well within reach of a single
Ampere SM.

**Op inventory:** 2 Conv2D, 3 Add, 2 ReLU, 2 MaxPool, 2 Reshape (no compute),
1 MatMul.

---

## 3. Current state (post-PR-#362)

| Capability | Status |
|---|---|
| Channel inherit + handoff v4 across kexec | ✅ working on jetson-nano-1 |
| Single-CTA single-thread int32 kernels | ✅ write_cafe, dot4, matmul4x4 |
| Single-CTA multi-thread int32 kernel | ✅ matmul4x4_mt (16 threads in 4×4 CTA) |
| Multi-CTA grid dispatch (CTA_RASTER > 1) | ❌ untested |
| fp32 SASS dispatched from raw nvgpu | ❌ all int32 to date |
| Per-shape runtime parameterization | ❌ shapes baked into compiled SASS |
| Multi-op pipeline (N kernels chained) | ❌ one kernel per `nvgpu launch-kernel` |
| Rust runtime → GPU FFI hook | ⚠️ `gpu_execute_matmul` exists as stub returning `Err(NotReady)` |
| ONNX op coverage on CPU NEON | ✅ all required ops in `runtime/src/inference/ops.rs` |
| `select_backend` thresholds | ⚠️ MatMul gate is `> 4096 input_elements`; MNIST's matmul is 2,560 (filtered out today) |

---

## 4. Phase plan

Each phase has a single, hardware-validatable deliverable. The CPU path stays
intact throughout — every GPU op gets unit-validated against
`runtime/src/inference/ops::*` for the same MNIST input.

```
M0 (multi-CTA grid)
 └─→ M1 (fp32 SASS port)
      └─→ M2 (parameterized GEMM)
           ├─→ M3 (elementwise: Add, ReLU)
           ├─→ M4 (MaxPool2D)
           └─→ M5 (Conv2D direct)
                └─→ M6 (multi-op pipeline + handoff extension)
                     └─→ M7 (Rust runtime wiring)
                          └─→ M8 (end-to-end MNIST inference on GPU)
```

M3 and M4 are independent siblings and can run in parallel after M2.
M5 (Conv2D) is the largest single step and can also begin as soon as M2 is
validated.

---

### M0 — Multi-CTA grid dispatch

**Goal:** prove that the QMD's `CTA_RASTER_WIDTH/HEIGHT/DEPTH` fields
(currently hardwired to 1) drive multi-CTA scheduling on GA10B and that
kernels can read `SR_CTAID` to map a CTA to a tile of work.

**Concrete kernel:** `matmul8x8_grid` — 8×8 int32 matmul dispatched as a
2×2 grid of 4×4-thread CTAs. Each CTA computes one 4×4 tile of the 8×8
output. 4 CTAs × 16 threads = 64 threads total.

**QMD deltas vs `matmul4x4_mt`:**
- `CTA_RASTER_WIDTH`: 1 → 2
- `CTA_RASTER_HEIGHT`: 1 → 2
- `CTA_THREAD_DIMENSION0/1`: stay at 4

**Test data:** A = `[1..64]` row-major 8×8, B = Aᵀ, C = A·Aᵀ Gram matrix.
Sentinel: C[0][0] = 1²+2²+...+8² = **204**.

**Deliverables:**
- `scripts/cuda/matmul8x8_grid.cu`
- `scripts/gpu-kernel-matmul8x8-grid.c` — uses shared scaffolding,
  overrides raster + thread dims via `gpu_qmd_set_bits`
- SASS extracted via the same `cuobjdump --extract-elf` recipe

**Exit criteria:**
- Linux-side: all 64 cells match expected
- SLM-OS post-kexec: `nvgpu launch-kernel` returns `poll=0xCC` (= 204)
- GP_GET advances 1 → 2 (single-submit consumed)

**Effort:** small. Mechanical extension of `matmul4x4_mt`. No new SLM-OS-side
code — same `ga10b_pick_launch_payload` path, same v4 handoff.

---

### M1 — First fp32 kernel

**Goal:** confirm fp32 SASS (FFMA, FFMA.RP, etc.) dispatches correctly on
GA10B from raw nvgpu. All four kernels shipped to date have been integer
(IMAD); MNIST is fp32 throughout.

**Concrete kernel:** `matmul4x4_mt_fp32` — port of `matmul4x4_mt` from int32
to float. Same shape (4×4 matmul, 16-thread CTA). Test data: A = first 16
positive integers as fp32, B = Aᵀ, expected C[0][0] = 30.0f.

**Risks:**
- `REGISTER_COUNT_V` may need to grow above 128 (FFMA chains often use more
  registers than IMAD due to 2-source-1-dest layout). The QMD field
  supports up to 256 registers per thread.
- Sentinel comparison: SLM-OS polls a `uint32_t`. fp32 30.0 has bit pattern
  `0x41F00000` — can be polled exactly via `expected_payload = 0x41F00000`.

**Deliverables:**
- `scripts/cuda/matmul4x4_mt_fp32.cu`
- `scripts/gpu-kernel-matmul4x4-mt-fp32.c`

**Exit criteria:**
- Linux-side: all 16 cells match (fp32 exact equality is fine for these
  small integers in fp32 representation)
- SLM-OS post-kexec: `expected_payload = 0x41F00000` polled and matched

**Effort:** small. Smaller than M0 conceptually since the launcher
infrastructure is unchanged.

---

### M2 — Parameterized GEMM

**Goal:** support arbitrary M×K×N matmul shapes via cbuf-passed parameters,
so a single SASS blob handles every MatMul shape MNIST (and future models)
need. This is the first kernel that reads its problem size from cbuf at
dispatch time rather than baking the shape into the SASS.

**Concrete kernel:** `gemm_fp32_tiled` — fp32 GEMM with shapes M, K, N
read from `cbuf[0][0x178], 0x17C, 0x180`. Tile size fixed (e.g. 16×16×8) so
the SASS is shape-agnostic but tile-shaped.

The standard tiled-GEMM pattern:
- Each CTA computes a `BLOCK_M × BLOCK_N` tile of C.
- Inner loop steps over K in chunks of `BLOCK_K`.
- Threads cooperatively load A and B tiles into shared memory, then
  multiply-accumulate.

**Deliverables:**
- `scripts/cuda/gemm_fp32_tiled.cu`
- `scripts/gpu-kernel-gemm-fp32.c` with command-line shape args
  `--m M --k K --n N`. Allocates A, B, C buffers sized M·K, K·N, M·N;
  writes shapes to cbuf.

**Test cases:**
- 4×4 (matches existing matmul4x4 — regression check)
- 8×8 (matches M0 — regression check)
- 1×256 × 256×10 (the MNIST FC layer — produces 1×10 output)
- 16×16 (tile-aligned, no edge case)
- 17×17 (tile-misaligned, exercises edge handling)

**Exit criteria:**
- All 5 shapes produce correct outputs Linux-side
- 1×256×10 case validated cell-for-cell against `runtime/inference/ops::matmul`
- MNIST-shape case dispatched from SLM-OS post-kexec

**Effort:** moderate. Tile-edge handling and shared-memory cooperation are
the new pieces. The QMD's `SHARED_MEMORY_SIZE` field needs a real value
(currently 0).

---

### M3 — Elementwise ops (Add + ReLU)

**Goal:** one-CTA-per-tile kernels for the trivial pointwise ops. ReLU and
Add can be a single fused kernel since MNIST always pairs them.

**Concrete kernel:** `relu_add_fp32` — for each output cell `i,j,k`,
`out[i,j,k] = max(0, a[i,j,k] + b[ch])` where `b` broadcasts over spatial
dims. Each thread handles one output element.

Tile shape: 32×32 = 1,024 threads/CTA (Ampere max). Grid sized to cover the
output volume.

**Test cases:**
- 1×8×28×28 + 8 bias (Conv1 output shape)
- 1×16×14×14 + 16 bias (Conv2 output shape)
- 1×10 + 10 bias (final FC output — degenerate 1×10×1×1)

**Exit criteria:** all three shapes match `runtime/inference/ops::add` +
`::relu` cell-for-cell.

**Effort:** small. Trivial arithmetic, but first kernel that reads
`SR_CTAID` for non-square grids.

---

### M4 — MaxPool2D

**Goal:** windowed-reduction kernel for the two MaxPool ops. Same
"one-CTA-per-output-tile" pattern as M3 but each thread reduces over a
`kh × kw` window of inputs.

**Concrete kernel:** `maxpool2d_fp32` — fp32 max over a `kH×kW` window with
configurable stride. Window dims and stride read from cbuf.

**Test cases:**
- 1×8×28×28 → 1×8×14×14 (2×2/2×2)
- 1×16×14×14 → 1×16×4×4 (3×3/3×3 — note: stride=3 over 14×14 wastes the
  trailing 2 elements, must match ONNX `auto_pad=NOTSET pads=[0,0,0,0]`
  behavior)

**Exit criteria:** match `runtime/inference/ops::maxpool2d` cell-for-cell.

**Effort:** small. Indexing logic is the only new piece.

---

### M5 — Conv2D

**Goal:** fp32 2D convolution kernel for the two Conv layers. By far the
largest single step in the plan.

**Approach:** **direct convolution** (not im2col + GEMM). Each output cell
sums over `kH × kW × in_channels` MACs. For MNIST this means at most 25 ×
8 = 200 MACs per output cell (Conv2). Direct conv keeps the kernel count
low and avoids the im2col memory blow-up (small at MNIST scale, but a
habit worth not forming).

**Tile shape:** one CTA computes a `BLOCK_H × BLOCK_W` tile of one output
channel. With `BLOCK_H = BLOCK_W = 4` and `BLOCK_OC = 1`, a 28×28 output
needs 7×7 = 49 CTAs per output channel × 8 channels = 392 CTAs for Conv1.
Well within Ampere's grid limit.

**SAME padding** is the wrinkle. With `kH=kW=5, stride=1, SAME_UPPER`, the
implicit pads are `[2,2,2,2]` (pads added below+right when kernel size is
odd, as ONNX 1.17 specifies for `SAME_UPPER`). The kernel needs to clamp
or zero out-of-bounds reads — handled with a per-load conditional.

**Concrete kernel:** `conv2d_fp32_direct` — params from cbuf:
- input addr, weight addr, output addr
- `H, W, C_in, C_out, kH, kW, stride_h, stride_w, pad_h, pad_w`

**Test cases:**
- Conv1 shape: 1×1×28×28 input, 8×1×5×5 weight → 1×8×28×28 (SAME pad=2)
- Conv2 shape: 1×8×14×14 input, 16×8×5×5 weight → 1×16×14×14 (SAME pad=2)

**Exit criteria:** match `runtime/inference/ops::conv2d` cell-for-cell on
both shapes.

**Effort:** large. This is the budget item — expect 2-3 hardware iterations.

---

### M6 — Multi-op pipeline + handoff extension

**Goal:** dispatch N ops in sequence from one SLM-OS-side `nvgpu
launch-kernel` invocation, with each op's output feeding the next op's
input. Today `launch-kernel` dispatches a single QMD; MNIST needs 10
dispatches in order.

**Approach (path of least resistance):** extend the handoff to **v5** with
an array of `(qmd_phys, qmd_gva, output_phys, expected_payload)` triples,
plus a count `n_ops`. The Linux helper builds N QMDs pre-kexec (one per op),
sets each op's input pointer in its cbuf to the previous op's output, and
writes the array. SLM-OS iterates: dispatch QMD i, poll its output, advance
to i+1.

**Alternative considered:** Linux helper builds a single multi-QMD
pushbuffer with N consecutive `SET_OBJECT + SEND_PCAS_A + SEND_SIGNALING_PCAS2_B`
triples and SLM-OS dispatches the whole chain in one submit. Faster (one
doorbell ring instead of N) but more complex error handling — pick this up
in a follow-up if the iterative path becomes a perf bottleneck.

**Handoff v5 fields (additions vs v4):**
- `uint32_t n_ops` — number of QMDs in the chain
- `uint64_t qmd_array_phys` — pointer to an array of `n_ops` QMDs
- `uint64_t output_array_phys` — pointer to an array of `n_ops` × `(output_phys, expected_payload)` pairs

Backward compat: v4 handoffs treated as `n_ops=1` with the existing
single-QMD fields. Static_asserts grow for v5.

**Test plan:**
- Dispatch a 2-op chain manually (e.g. matmul → relu) post-kexec, confirm
  both ops fire and the chained output is correct.
- Dispatch the full 10-op MNIST chain (after M5 lands).

**Effort:** moderate. Most of the work is the handoff extension and the
SLM-OS-side iteration loop; per-op QMD construction is already done by the
Linux helper for free.

---

### M7 — Rust runtime wiring

**Goal:** make `runtime/src/inference/gpu.rs:gpu_execute_*` actually call
into the kernel-side GPU dispatch, so `select_backend` can route MNIST ops
to GPU and the engine's existing dispatch loop in `engine.rs` works
unchanged.

**Concrete changes:**
1. New FFI in `kernel/include/slm_ffi.h`:
   ```
   int slm_gpu_run_mnist(const float *input, float *output);
   ```
   Initial implementation just hands off to `ga10b_bringup_run_mnist_chain`
   which reuses the v5 handoff.
2. Rust side in `runtime/src/inference/gpu.rs`: replace
   `gpu_execute_matmul`'s stub with a real call. Add stubs for `conv2d`,
   `relu`, `maxpool2d`, `add`. The first end-to-end version can short-circuit
   the per-op routing — call `slm_gpu_run_mnist` once when the engine sees
   the first op of the MNIST graph and skip the per-op CPU calls.
3. Lower the `select_backend` matmul threshold from 4,096 to 256 so MNIST's
   FC layer routes to GPU. Threshold tuning is a hyperparameter — leave a
   follow-up note for proper hardware-measured tuning rather than reusing
   an `M9` label (M9 is now claimed by the runtime input swap milestone).

**Test plan:**
- `slm.model_infer(mnist)` returns the same 10 logits as the CPU path on
  `models/test/mnist_input_sample.bin` (reuse whatever input the existing
  demo uses, or pin a canonical one).

**Effort:** small once M0–M6 are in. The integration is a thin shim.

---

### M8 — End-to-end validation

**Goal:** run MNIST inference on GPU from SLM-OS post-kexec via the Rust
runtime path, with bit-for-bit (or fp32-tolerance: ≤ 1e-5 relative) match
against the CPU NEON path.

**Validation harness:**
- Pin a canonical input image (one of the digit samples in `models/test/`
  or generate a deterministic synthetic input).
- CPU run: `slm.model_infer(mnist)` with `Backend::Cpu` forced — record 10
  logits.
- GPU run: same input, `Backend::Gpu` forced — record 10 logits.
- Compare element-wise. argmax should match exactly; absolute tolerance ≤
  1e-3 on the logits (Conv accumulators in fp32 are tight enough that
  larger drift indicates a bug, not a precision issue).

**Exit criteria:**
- argmax matches CPU
- max relative error < 1e-3 across all 10 logits
- post-kexec dispatch on jetson-nano-1 succeeds in 10/10 consecutive runs

**Deliverables:**
- A new `nvgpu run-mnist` shell command (or a Lua API
  `slm.gpu_run_mnist()`) that exercises the path standalone.
- A status update in `docs/jetson-capstone-handoff.md` and
  `docs/capstone-feature-status.md`.

---

## 5. Out of scope

The following items are explicitly **not** part of getting MNIST to function
on the GPU. They are the natural follow-ups but should not block M0–M8.

| Item | Why deferred |
|---|---|
| SLM-OS-native channel + QMD setup | The Linux helper still does all `nvgpu` ioctls pre-kexec. Removing this dependency requires re-implementing the nvgpu IOCTL surface in SLM-OS — a separate multi-week project. |
| SLM-OS-native shader compilation | Shaders are CUDA-compiled. Porting NAK or writing an Ampere SASS assembler is its own large effort. |
| Tensor cores | GA10B has tensor cores but they need WMMA/MMA SASS instructions and a different QMD register layout. fp32 FFMA on regular cores is enough for MNIST. |
| Multi-batch inference | Plan assumes batch=1 throughout. Multi-batch needs different tile shapes and a different QMD `CTA_RASTER_DEPTH` value. |
| Mixed precision (fp16/int8) | The CPU path already supports these; the GPU path will inherit them once basic fp32 works. |
| Real SLM (Llama-class) inference | The next milestone after MNIST. Reuses M2 (GEMM), M3 (elementwise), M5 (Conv1D for tokenization), but adds attention, RMSNorm, GELU, KV cache. |

---

## 6. Risks and open questions

| Risk | Mitigation |
|---|---|
| **Conv2D SASS register pressure.** Direct conv with 25 MACs in the inner loop may want more than the default 128 registers per thread. | M5 first iteration: launch with `REGISTER_COUNT_V = 255` (max). If CUDA reports register spills (visible in `nvcc -Xptxas -v`), restructure the loop. |
| **Multi-CTA scheduling on a single SM.** GA10B has 8 SMs but the kernel runs in one TPC's compute pipe today. M0 should reveal whether multi-CTA actually distributes across SMs or serializes onto one. | If serialized: investigate the QMD's `SM_DISABLE_MASK` and channel preempt mode settings. Should not block MNIST functioning, only perf. |
| **~~fp32 sentinel polling exact-equality~~ (RESOLVED in M10).** ~~fp32 30.0 is an exact bit pattern (0x41F00000) so M1's `expected_payload = 0x41F00000` is exact. But Conv outputs (M5) won't be exact — they're sums of many fp32 muls and the rounding mode matters.~~ | ~~M5 onwards: don't use `expected_payload` for Conv-output validation. SLM-OS just polls "any non-zero write to the sentinel" then the Rust runtime cross-checks against CPU. M6's handoff v5 must support this "watchdog" mode.~~ M10 retired value-sentinel polling entirely on the SLM-OS side: each op's pushbuffer ends with a `REPORT_SEMAPHORE_EXECUTE` (RELEASE), and SLM-OS polls the semaphore for a magic payload (0xCAFEDEAD). No fp32 / sentinel-zero issues. The launcher's pre-kexec self-check still uses value-sentinel polling because Linux's nvgpu owns the GPU there. |
| **Linux helper builds 10 QMDs pre-kexec — but the channel handoff only describes one.** | M6's handoff v5 explicitly carries an array of QMDs. |
| **SAME padding semantics.** ONNX `SAME_UPPER` with odd kernel size pads more on bottom-right than top-left. Easy to get wrong. | M5 unit test: small input + known-padding-sensitive weight, compare against CPU. |
| **Memory budget on jetson-nano-1.** Plenty of free RAM (~7 GB) and nvmap heap, but multi-CTA dispatches and Conv output buffers do grow. | Track total nvmap allocation; the largest single buffer is Conv1's 25 KB output. Far below any realistic limit. |

---

## 7. Validation strategy (cross-cutting)

Each phase ships with **two** validations:

1. **Linux-side self-check** — the launcher computes the expected output
   independently (in C, reusing the existing per-shape test vectors) and
   compares cell-for-cell. Pre-kexec sanity.
2. **Post-kexec SLM-OS verification** — `nvgpu launch-kernel` with the
   appropriate sentinel matches; the Rust runtime cross-checks the full
   output buffer against `runtime/inference/ops::*` for the same input.

The CPU NEON path stays as the **correctness oracle** through M8. Every GPU
op is a parallel implementation of an op SLM-OS already executes correctly;
divergence between the two is always a bug in the GPU path, never the CPU
path.

---

## 8. Tracking

Phases are tracked in GitHub issues via the **MNIST-on-GPU** label (creation:
TODO).

| Phase | Status | Result |
|---|---|---|
| M0 — multi-CTA grid | ✅ done | matmul8x8_grid: 64/64 cells correct, multi-CTA distributed across SMs |
| M1 — fp32 SASS | ✅ done | matmul4x4_mt_fp32: first FFMA from raw nvgpu, sentinel 0x41F00000 = 30.0f |
| M2 — parameterized GEMM | ✅ done | gemm_fp32: 4×4×4, 8×8×8, 1×256×10 (MNIST FC), 16×16×16, 17×17×17 — all shapes correct |
| M3 — elementwise ops | ✅ done | add_bias_relu_fp32: conv1, conv2, fc shapes — all match CPU |
| M4 — MaxPool2D | ✅ done | maxpool2d_fp32: pool1 (2×2/2), pool2 (3×3/3) — all match CPU |
| M5 — Conv2D | ✅ done | conv2d_fp32_direct: conv1 (6,272 cells) + conv2 (3,136 cells) — all match CPU |
| M6 — pipeline + handoff v5 | ✅ done | 2-op test pipeline + matmul4x4_mt_fp32 chain validates end-to-end |
| M7 — Lua/shell + Rust runtime wiring | ✅ done | `slm.gpu_run_mnist()` Lua binding + `nvgpu run-mnist` shell command (PR #373/#376); `engine::run_inference` short-circuits to `gpu::run_mnist_gpu_fastpath` when the active model is "mnist" and GPU compute is ready |
| M8 — end-to-end MNIST | ✅ done | 8-op chain produces logits matching CPU NEON, argmax = 3 |
| M9 — runtime input swap (handoff v6) | ✅ done | `slm.gpu_set_mnist_input{,_fill}` + `ga10b_bringup_set_input{,_fill}` overwrite the GPU's input tensor between dispatches; cache-clean + DSB sequence verified; PR #376 |
| M10 — SEMAPHORE_RELEASE completion + custom-file demo | ✅ done | `ga10b_build_launch_kernel_with_sema_pushbuffer` appends a `REPORT_SEMAPHORE_EXECUTE` (OP=RELEASE) to each op's pushbuffer. AMPERE_COMPUTE_B drains the compute pipeline and flushes L2 → DRAM before the release fires, so polling the semaphore is a real "all output is in DRAM" signal. Closes #372 (sentinel-zero deadlock) and #390 (Conv1 truncation past 4 KB). Adds `slm.model_infer_bytes(idx, bytes)` and `slm.model_infer_file(idx, path)` Lua bindings + 10 MNIST test-digit fixtures embedded at `/mnt/files/digits/digit_N.bin` for demo-grade end-to-end GPU classification of arbitrary inputs |

**Status as of 2026-04-25:** MNIST inference functioning on the
Jetson GA10B GPU from SLM-OS post-kexec for **arbitrary user-
supplied digit images** via `slm.model_infer_file('/mnt/files/digits/digit_N.bin')`.
Hardware-validated on jetson-nano-1: digit_3, digit_7, digit_0
all classified correctly through the GPU fastpath. M0..M10 all
landed; the `jetson-gpu-multicta` branch carries M0, M1–M5 (one
batched commit), M6, and M8; M7 (Lua/shell + Rust-runtime wiring)
ships across PR #373 and #376; M9 (runtime input swap, handoff v6)
ships in PR #376; M10 (SEMAPHORE_RELEASE + custom-file demo) is
the current PR. See `git log` for hardware-validation traces.

**Completion-signal architecture (M10):** Per-op completion now
uses AMPERE_COMPUTE_B's `REPORT_SEMAPHORE_EXECUTE` instead of
polling a value-sentinel cell of the kernel's output. The
launcher-pre-kexec self-check still uses the value-sentinel
mode because Linux's nvgpu has its own L2 flush sequencing; the
sentinel-cell calibration in `pipeline_sentinels.bin` is still
read by SLM-OS only for `read_pipeline_output`'s base-address
arithmetic (final op's `output_phys` = base + sentinel × 4, and
the launcher pins the final op's sentinel to cell 0 so the
read-back gets the full logits buffer). Future cleanup: add a
dedicated `final_output_phys` field to v6+ handoffs so the
sentinel-and-read-base concerns separate cleanly.

Two non-obvious bugs were uncovered along the way and are
documented in commit messages:

1. **Built-in CUDA dim variables.** Parameterized kernels read
   `blockDim.x/y` from `cbuf[0][0x0..0x8]`. The original launcher
   left that region zeroed, so every CTA computed
   `i = blockIdx.y * 0 + threadIdx.y`, collapsing all multi-CTA
   dispatches onto the (0,0) tile. Fix in
   `gpu_write_builtin_dims()`; every parameterized launcher calls
   it after QMD overrides.

2. **Multi-submit GP_PUT advance.** The launcher-side
   `gpu_submit_and_poll` originally hardcoded `GP_PUT = 1` and
   wrote slot 0 every call. Single-shot kernels never tripped
   over this; the M6 pipeline test exposed it because op 1's
   submit was a silent no-op. Fix reads current GP_PUT, writes
   the next slot, advances by 1 — same pattern the kernel-side
   helper already used.

This document is the source of truth for phase ordering and exit
criteria. Issue updates roll up to here, not the other way around.
