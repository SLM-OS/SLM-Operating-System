# Inference Engine API Reference

API reference for the SLM-OS inference engine. The engine walks an ONNX operator graph
in topological order, resolves tensor names through a binding table, dispatches each
operator to a CPU (or GPU) implementation, and manages intermediate tensors via a
bump-allocated workspace.

**Source files:**
- `runtime/src/inference/engine.rs` -- Core engine and statistics
- `runtime/src/inference/ops.rs` -- CPU operator implementations
- `runtime/src/inference/tensor.rs` -- Tensor descriptor
- `runtime/src/inference/workspace.rs` -- Bump allocator for intermediate tensors
- `runtime/src/inference/gpu.rs` -- GPU backend and placement logic
- `kernel/include/slm_ffi.h` -- C FFI declarations

---

## C FFI Functions

These functions are exported by the Rust runtime with `#[no_mangle] pub extern "C"` linkage
and declared in `kernel/include/slm_ffi.h`.

### rust_infer

```c
extern int rust_infer(uint32_t model_index, const float *input_data,
                      size_t input_len, float *output_buf, size_t output_len);
```

Run inference on a loaded model.

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `model_index` | `uint32_t` | Registry index (from `rust_model_load`) |
| `input_data` | `const float *` | Pointer to FP32 input array |
| `input_len` | `size_t` | Number of floats in input |
| `output_buf` | `float *` | Buffer for FP32 output |
| `output_len` | `size_t` | Capacity of output buffer (in floats) |

**Returns:** Number of output floats written on success, `-1` if a pointer is null,
`-2` on inference error (model not found, shape mismatch, workspace exhaustion, etc.).

**Thread safety:** Only one inference can execute at a time. The engine uses a spinlock
to serialize concurrent calls.

---

### rust_infer_classify

```c
extern int rust_infer_classify(uint32_t model_index);
```

Run inference with a zero-filled input buffer and return the argmax class index.
Intended for kernel-mode callers that cannot handle floating-point types directly
(kernel C code is compiled with `-mgeneral-regs-only`).

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `model_index` | `uint32_t` | Registry index |

**Returns:** Class index (>= 0) on success, `-1` on error (model not found,
engine error, or zero outputs).

**Buffers:** 784-element input buffer is a `static` immutable zero array
(safe to share across concurrent callers). The 64-element output buffer
is stack-local — two concurrent callers cannot race the same array. The
argmax loop is capped at the output buffer length so a future engine
contract drift cannot panic-abort the kernel via an OOB index.

**Thread safety:** Multiple CPUs can call this concurrently. The
inference engine itself serialises with a spinlock; per-call output
storage is per-stack so post-engine processing does not need additional
synchronisation.

---

### rust_infer_and_print

```c
extern int rust_infer_and_print(uint32_t model_index);
```

Run inference with an internal zero-filled 784-element input buffer
and print logits + predicted class to UART. Backs the `model infer
<name|idx>` shell command.

The output array is a stack-local `[f32; 64]` (256 B), so two
concurrent shell sessions calling `model infer` can't race the same
buffer. Loop bound is capped at the buffer length defensively in case
the engine's per-call output count ever drifts past it.

**Returns:**
- `0` on success
- `-1` if `model_index` is not a loaded model
- `-3` if the engine errors or returns 0 outputs (treated as engine
  failure rather than "predicted class 0", which would otherwise be
  ambiguous)

---

### rust_infer_buf_and_print

```c
extern int rust_infer_buf_and_print(uint32_t model_index,
                                    const float *input,
                                    size_t input_floats);
```

Run inference on a loaded model with a caller-supplied fp32 input
buffer and print logits + argmax to UART. Backs the `model
infer-file <name|idx> <path>` shell command — kernel C reads the
file via VFS, hands the byte buffer here, and this function calls
the engine and prints the result.

`input` must point to `input_floats × 4` bytes of 4-byte-aligned
fp32. The shell hands in a `pmm_alloc_pages` buffer; the page
alignment satisfies the fp32 load alignment the inference kernels
assume. The output array is a stack-local `[f32; 64]` for the same
reason as `rust_infer_and_print`.

**Returns:**
- argmax class index (>= 0) on success
- `-1` if `model_index` is not loaded
- `-2` if `input` is NULL or `input_floats` is 0
- `-3` if the engine errors or returns 0 outputs

The argmax loop is capped at the output buffer length so a contract
drift can't panic-abort the kernel via an OOB index.

---

### rust_infer_stats

```c
extern int rust_infer_stats(RustInferStats *stats);
```

Retrieve a snapshot of inference performance statistics.

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `stats` | `RustInferStats *` | Output pointer for statistics |

**Returns:** `0` on success, `-1` if pointer is null.

---

### rust_infer_bench

```c
extern int rust_infer_bench(uint32_t model_index, uint32_t iterations);
```

Run an inference benchmark for N iterations and print min/avg/max latency to UART.
Uses a zero-filled 784-element input buffer.

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `model_index` | `uint32_t` | Registry index |
| `iterations` | `uint32_t` | Number of inference iterations to run |

**Returns:** `0` on success, `-1` if iterations is zero or model not found.

---

### rust_inference_test

```c
extern int rust_inference_test(void);
```

Run the inference engine test suite. Tests cover the bump allocator, tensor operations,
operator correctness (matmul, add, relu, softmax, reshape, conv2d, maxpool2d, gemm),
and end-to-end inference on a loaded model.

**Returns:** Number of test failures (`0` = all passed).

---

## RustInferStats Struct

Defined in `kernel/include/slm_ffi.h` (C) and `runtime/src/inference/engine.rs` (Rust as `InferenceStats`).

```c
typedef struct {
    uint64_t total_inferences;  /* Number of successful inferences     */
    uint64_t total_time_ns;     /* Cumulative inference time            */
    uint64_t min_time_ns;       /* Fastest inference (0 if none)        */
    uint64_t max_time_ns;       /* Slowest inference                    */
    uint64_t last_time_ns;      /* Most recent inference time           */
    uint64_t errors;            /* Number of failed inferences          */
} RustInferStats;
```

Statistics are tracked via atomic counters (`AtomicU64`) and are safe to read from any
thread without locking.

---

## Rust Internal API

### run_inference

```rust
pub fn run_inference(
    model_index: usize,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
) -> Result<usize, EngineError>
```

Top-level inference entry point. Acquires the engine spinlock, initializes the static
`InferenceEngine` for the specified model, executes the forward pass, records timing
statistics, and releases the lock.

**Returns:** `Ok(n)` where `n` is the number of output floats, or `Err(EngineError)`.

---

### get_stats

```rust
pub fn get_stats() -> InferenceStats
```

Return an atomic snapshot of cumulative inference statistics.

---

### InferenceEngine

```rust
pub struct InferenceEngine {
    graph: OperatorGraph,
    weight_table: WeightTable,
    weight_base: *const u8,
    workspace: BumpAllocator,
    bindings: [TensorBinding; 32],  // MAX_BINDINGS
    binding_count: usize,
    gpu_caps: GpuCapabilities,
    initialized: bool,
}
```

The engine is stored in a `static` variable protected by a spinlock. On each inference
call, it is re-initialized for the target model via `init(model_index)`.

**Key methods:**

| Method | Description |
|--------|-------------|
| `init(model_index)` | Load graph, weight table, and memory pointers for a model |
| `run(input, input_len, output, output_len)` | Execute the full forward pass |
| `bind(name, tensor)` | Bind a tensor name to a `Tensor` descriptor |
| `resolve(name)` | Look up a `Tensor` by name in the binding table |

---

### EngineError

```rust
pub enum EngineError {
    ModelNotFound,
    WorkspaceExhausted,
    UnsupportedOp,
    ShapeMismatch,
    /// Shape dimensions multiplied past usize::MAX — malformed model input.
    ShapeOverflow,
    InvalidInput,
    WeightNotFound,
    InternalError,
}
```

`ShapeOverflow` is distinct from `WorkspaceExhausted`: overflow indicates
a malformed model (dimensions that can't fit in `usize`), whereas
`WorkspaceExhausted` means the bump allocator ran out of space for a
shape that was arithmetically valid.

---

## Tensor

Defined in `runtime/src/inference/tensor.rs`. A non-owning FP32 tensor descriptor (~44 bytes).

```rust
pub struct Tensor {
    pub data: *const f32,
    pub shape: [u32; 8],   // MAX_DIMS
    pub ndim: u8,
}
```

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `fn new(data: *const f32, shape: &[u32]) -> Self` | Create from pointer and shape |
| `num_elements` | `fn num_elements(&self) -> usize` | Product of all dimensions |
| `size_bytes` | `fn size_bytes(&self) -> usize` | `num_elements() * 4` |
| `dim` | `fn dim(&self, i: usize) -> u32` | Dimension at index `i` |
| `rows` | `fn rows(&self) -> u32` | Second-to-last dimension (or 1 if 1D) |
| `cols` | `fn cols(&self) -> u32` | Last dimension |
| `data_mut` | `fn data_mut(&self) -> *mut f32` | Cast data pointer to mutable |
| `get` | `unsafe fn get(&self, index: usize) -> f32` | Read element at flat index |
| `set` | `unsafe fn set(&self, index: usize, value: f32)` | Write element at flat index |

---

## BumpAllocator (Workspace)

Defined in `runtime/src/inference/workspace.rs`. Allocates intermediate tensor storage
from a contiguous workspace memory block. Reset between inference calls for O(1) deallocation.

```rust
pub struct BumpAllocator {
    base: *mut u8,
    capacity: usize,
    offset: usize,
}
```

**Methods:**

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `fn new(base: *mut u8, capacity: usize) -> Self` | Create over a workspace block |
| `alloc` | `fn alloc(&mut self, size: usize, align: usize) -> *mut u8` | Allocate bytes (null on exhaustion or arithmetic overflow) |
| `alloc_tensor` | `fn alloc_tensor(&mut self, shape: &[u32]) -> Result<Tensor, EngineError>` | Allocate an FP32 tensor; `Err(ShapeOverflow)` on dim overflow, `Err(WorkspaceExhausted)` on OOM |
| `reset` | `fn reset(&mut self)` | Free all allocations in O(1) |
| `remaining` | `fn remaining(&self) -> usize` | Bytes remaining |
| `used` | `fn used(&self) -> usize` | Bytes used |

Tensor data is aligned to 16 bytes (`TENSOR_ALIGN`) for NEON/SSE compatibility.

---

## Operator Implementations

Defined in `runtime/src/inference/ops.rs`. All operators are pure functions that read
from input tensors and write to pre-allocated output tensors. No dynamic allocation occurs
within operator functions.

### matmul

```rust
pub fn matmul(a: &Tensor, b: &Tensor, out: &mut Tensor) -> Result<(), EngineError>
```

Matrix multiplication: `C[M,N] = A[M,K] x B[K,N]`. Row-major layout. Loop order
is `i, k, j` for cache-friendly access on the A matrix and auto-vectorization of
the inner loop.

Supports 2D matrices and 1D vectors (treated as row or column vectors).
Returns `ShapeMismatch` if inner dimensions disagree or output size is wrong.

---

### add

```rust
pub fn add(a: &Tensor, b: &Tensor, out: &mut Tensor) -> Result<(), EngineError>
```

Element-wise addition with broadcasting:

| Case | Condition | Behavior |
|------|-----------|----------|
| Same shape | `a.len == b.len` | `out[i] = a[i] + b[i]` |
| Scalar | `b.len == 1` | `out[i] = a[i] + b[0]` |
| Bias broadcast | `b.len == a.cols` | `b` added to each row of `a` |
| General | `a.len % b.len == 0` | `b` repeated cyclically |

---

### relu

```rust
pub fn relu(input: &Tensor, out: &mut Tensor) -> Result<(), EngineError>
```

Element-wise ReLU: `out[i] = max(0, input[i])`. Can operate in-place when
`input.data == out.data`.

---

### softmax

```rust
pub fn softmax(input: &Tensor, out: &mut Tensor) -> Result<(), EngineError>
```

Numerically stable softmax along the last axis:
`out[i] = exp(x[i] - max) / sum(exp(x - max))`. Uses `libm::expf` for
`no_std` compatibility. Operates row-by-row for multi-row tensors.

---

### reshape

```rust
pub fn reshape(input: &Tensor, new_shape: &[u32]) -> Result<Tensor, EngineError>
```

Zero-copy reshape: returns a new `Tensor` descriptor sharing the input's data
pointer with a reinterpreted shape. Validates that the total element count is
preserved. Returns `ShapeMismatch` on mismatch.

---

### conv2d

```rust
pub fn conv2d(
    input: &Tensor,         // [N, C_in, H, W]
    weight: &Tensor,        // [C_out, C_in, kH, kW]
    bias: Option<&Tensor>,  // [C_out]
    out: &mut Tensor,       // [N, C_out, H_out, W_out]
    kernel_h: u32, kernel_w: u32,
    stride_h: u32, stride_w: u32,
    pad_h: u32, pad_w: u32,
) -> Result<(), EngineError>
```

2D convolution in NCHW layout. The engine infers kernel size from the weight tensor
shape and uses default parameters (stride=1, pad=0) matching MNIST-12 conventions.
Bias is added per output channel when present.

Output spatial dimensions: `H_out = (H + 2*pad - kH) / stride + 1`.

---

### maxpool2d

```rust
pub fn maxpool2d(
    input: &Tensor,    // [N, C, H, W]
    out: &mut Tensor,  // [N, C, H_out, W_out]
    kernel_h: u32, kernel_w: u32,
    stride_h: u32, stride_w: u32,
) -> Result<(), EngineError>
```

2D max pooling in NCHW layout. Default parameters (kernel=2x2, stride=2) match
MNIST-12 conventions. Output spatial dimensions: `H_out = (H - kH) / stride + 1`.

---

### gemm

```rust
pub fn gemm(
    a: &Tensor,
    b: &Tensor,
    c: Option<&Tensor>,
    out: &mut Tensor,
) -> Result<(), EngineError>
```

Generalized matrix multiply: `Y = A x B + C` (simplified ONNX Gemm with
alpha=1, beta=1, no transpose). Implemented as `matmul` followed by `add` when
bias `c` is present.

---

## GPU Backend

Defined in `runtime/src/inference/gpu.rs`. Provides GPU detection, operator placement
heuristics, and stub dispatch for GPU-accelerated operators.

### GpuStatus

```rust
pub enum GpuStatus {
    NotAvailable,       // No GPU driver or hardware
    DetectedNoCompute,  // GPU probed but compute not ready (Jetson without GSP)
    ComputeReady,       // GPU ready for compute dispatch
}
```

### GpuCapabilities

```rust
pub struct GpuCapabilities {
    pub status: GpuStatus,
    pub capabilities: u32,   // GPU_CAP_* flags
    pub cuda_cores: u32,
    pub tensor_cores: u32,
    pub unified_memory: bool,
    pub name: [u8; 32],
    pub device: [u8; 64],
}
```

**Key methods:**

| Method | Description |
|--------|-------------|
| `detect() -> Self` | Query kernel for GPU info via FFI |
| `has_compute() -> bool` | Whether GPU compute is available |
| `has_tensor_cores() -> bool` | Whether tensor cores are present |

**Capability flags:**

| Flag | Value | Description |
|------|-------|-------------|
| `GPU_CAP_COMPUTE` | `0x01` | Basic compute capability |
| `GPU_CAP_TENSOR_CORES` | `0x02` | Tensor core support |
| `GPU_CAP_UNIFIED_MEMORY` | `0x08` | CPU/GPU shared memory |

### Backend Enum

```rust
pub enum Backend {
    Cpu,
    Gpu,
}
```

### select_backend

```rust
pub fn select_backend(op: OpType, input_elements: usize, caps: &GpuCapabilities) -> Backend
```

Select the best compute backend for an operator based on type, input size, and
GPU capabilities.

**Placement heuristics:**

| Operator | Threshold | Backend |
|----------|-----------|---------|
| MatMul, Gemm | > 4096 elements | GPU |
| Conv | > 8192 elements | GPU |
| All others | -- | CPU |

Returns `Backend::Cpu` when GPU compute is unavailable regardless of thresholds.

### gpu_execute_matmul

```rust
pub fn gpu_execute_matmul(
    _a_ptr: *const f32, _b_ptr: *const f32, _out_ptr: *mut f32,
    _m: usize, _k: usize, _n: usize,
) -> Result<(), GpuError>
```

Stub GPU MatMul dispatch. Currently always returns `Err(GpuError::NotReady)`,
causing the engine to fall back to the CPU implementation. When GSP firmware loading
is implemented, this function would flush caches, submit a GPU command, wait for
completion, and invalidate caches.

### GpuError

```rust
pub enum GpuError {
    NotAvailable,
    NotReady,
    OutOfMemory,
    SyncFailed,
}
```

---

## GPU FFI Functions

### rust_gpu_print_status

```c
extern void rust_gpu_print_status(void);
```

Print GPU status information to UART: driver name, device description, compute
status, core counts, and memory configuration.

### rust_gpu_compute_test

```c
extern int rust_gpu_compute_test(void);
```

Run GPU compute integration tests. Tests capability detection, backend selection
logic, and GPU MatMul stub fallback behavior.

**Returns:** Number of test failures (`0` = all passed).

---

## Shell Commands

The kernel shell provides inference operations through the `model` command:

| Command | Description |
|---------|-------------|
| `model infer <index>` | Run inference on a loaded model and print output probabilities |
| `model bench <index> [N]` | Run N iterations (default 10) and print min/avg/max latency |
| `model stats` | Display cumulative inference statistics |
| `model gpu` | Print GPU status and capabilities |

Example session:

```
SLM-OS> model infer 0
Output (10 classes):
  [0] 0.1012  [1] 0.0987  [2] 0.1005  [3] 0.0998
  [4] 0.0991  [5] 0.0996  [6] 0.1010  [7] 0.1003
  [8] 0.0999  [9] 0.0999
Predicted class: 0

SLM-OS> model bench 0 100
Benchmark: 100 iterations on model 0
  Min:  1.23 ms
  Avg:  1.45 ms
  Max:  2.01 ms
  Pass: 100/100

SLM-OS> model stats
Inference statistics:
  Total:   100 inferences
  Errors:  0
  Min:     1.23 ms
  Avg:     1.45 ms
  Max:     2.01 ms
  Last:    1.38 ms

SLM-OS> model gpu
GPU Status:
  Driver:         stub
  Device:         No GPU detected
  Compute:        Not available
```

---

*Last updated: April 2026*
