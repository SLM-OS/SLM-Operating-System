# SLM-OS Rust Runtime API Reference

This document provides a concise reference for the Rust runtime's public FFI interface. All functions use `#[no_mangle] extern "C"` linkage and are callable from the C kernel. Source files are under `runtime/src/`.

---

## Runtime Initialization (`lib.rs`)

```rust
pub unsafe extern "C" fn rust_heap_init(heap_start: *mut u8, heap_size: usize)
```
Initialize the Rust heap allocator. Must be called exactly once before any Rust allocations.

```rust
pub extern "C" fn rust_init() -> i32
```
Initialize the Rust runtime. Validates FFI type compatibility. Returns 42 on success, negative on error.

```rust
pub extern "C" fn rust_hello()
```
Print a greeting via UART. Used to verify FFI integration.

```rust
pub extern "C" fn rust_test_panic()
```
Trigger a Rust panic for testing the panic handler. Does not return.

```rust
pub extern "C" fn rust_ffi_validate() -> i32
```
Validate FFI type sizes and alignments against C expectations. Returns 0 on success. Called internally by `rust_init()`.

```rust
pub extern "C" fn rust_run_tests() -> i32
```
Run all Rust FFI integration tests. Returns the number of failures (0 = all passed).

---

## Model Memory (`lib.rs`, `mm/model_mem.rs`)

Two-pool allocator for model weights and inference workspace. Pools are backed by 2 MB-aligned physical pages allocated from the C kernel's PMM.

```rust
pub extern "C" fn rust_model_mem_init() -> i32
```
Initialize model memory pools (256 MB weights, 128 MB workspace for 1 GB RAM). Returns 0 on success, -1 on failure.

```rust
pub extern "C" fn rust_model_alloc_weights(size: usize) -> ModelHandle
```
Allocate from the weight pool. Returns a `ModelHandle` (check `is_null()` for failure).

```rust
pub extern "C" fn rust_model_alloc_workspace(size: usize) -> ModelHandle
```
Allocate from the workspace pool. Returns a `ModelHandle`.

```rust
pub extern "C" fn rust_model_free(handle: ModelHandle) -> i32
```
Free a model memory allocation. Returns 0 on success, -1 on error.

```rust
pub extern "C" fn rust_model_share(handle: ModelHandle) -> ModelHandle
```
Increment refcount on a handle (for sharing between tasks). Returns a new handle or null on error.

```rust
pub extern "C" fn rust_model_get_ptr(handle: ModelHandle) -> *mut u8
```
Return the raw pointer for a handle. Returns `NULL` if invalid.

```rust
pub extern "C" fn rust_model_get_size(handle: ModelHandle) -> usize
```
Return the allocation size for a handle. Returns 0 if invalid.

```rust
pub extern "C" fn rust_weight_pool_stats() -> PoolStats
```
Return weight pool statistics (total/free/allocated/shared blocks, peak usage).

```rust
pub extern "C" fn rust_workspace_pool_stats() -> PoolStats
```
Return workspace pool statistics.

```rust
pub extern "C" fn rust_model_mem_test() -> i32
```
Run model memory tests. Returns the number of failures.

---

## Model Loader (`lib.rs`, `loader/`)

Registry-based model loader supporting ONNX graph parsing, weight extraction, and operator dispatch.

**Thread Safety:** The model registry uses internal spinlock protection for concurrent access. However, model load/unload operations are designed to be called from CPU 0 only (shell commands, boot init). Inference (`rust_infer_classify`) is safe to call from any CPU as it reads model weights without modification.

**Memory Lifecycle:** Models persist in the registry until explicitly unloaded via `rust_model_unload()` or evicted by the LRU cache. The registry holds up to 8 models simultaneously. When all slots are full, loading a new model evicts the least recently used non-pinned model. Models can be pinned to prevent eviction. Each inference call updates the model's LRU timestamp. FP16 weights are automatically converted to FP32 at load time. Weight memory supports reference-counted sharing via `rust_model_share_weights()`.

**Index Bounds:** All functions that accept a model index validate it against the registry size. Out-of-range indices return -1. The `rust_model_find()` function returns -1 for unknown model names.

```rust
pub extern "C" fn rust_model_loader_init() -> i32
```
Initialize the model loader registry. Returns 0 on success.

```rust
pub unsafe extern "C" fn rust_model_load(name: *const u8, data: *const u8, data_len: usize) -> i32
```
Load a model from an in-memory buffer. `name` is null-terminated. Returns the registry index (>= 0) on success, -1 on error.

```rust
pub extern "C" fn rust_model_unload(index: u32) -> i32
```
Unload a model by registry index. Returns 0 on success, -1 on error.

```rust
pub unsafe extern "C" fn rust_model_get_info(index: u32, info: *mut ModelInfoC) -> i32
```
Fill a `ModelInfoC` struct with model metadata (name, format, parameter count, weight/workspace sizes, node/input/output counts). Returns 0 on success.

```rust
pub extern "C" fn rust_model_count() -> u32
```
Return the number of currently loaded models.

```rust
pub unsafe extern "C" fn rust_model_find(name: *const u8) -> i32
```
Find a model by name. Returns the registry index (>= 0) if found, -1 if not found.

```rust
pub extern "C" fn rust_model_pin(index: u32) -> i32
```
Pin a model to prevent LRU eviction. Returns 0 on success, -1 if the index is invalid or the slot is empty.

```rust
pub extern "C" fn rust_model_unpin(index: u32) -> i32
```
Unpin a model, allowing LRU eviction. Returns 0 on success, -1 on error.

```rust
pub extern "C" fn rust_model_share_weights(index: u32) -> i32
```
Increment the reference count on a model's weight memory block. This allows the weight memory to survive even if the model is unloaded from the registry. The caller must eventually release the reference. Returns 0 on success, -1 on error.

```rust
pub extern "C" fn rust_model_loader_test() -> i32
```
Run model loader tests. Returns the number of failures.

---

## Inference Engine (`lib.rs`, `inference/`)

Executes ONNX computation graphs using a bump-allocated workspace. Supports MatMul, Add, Relu, Softmax, Conv, MaxPool, Reshape, and Flatten operators.

**SIMD Optimization:** All operators use NEON SIMD on AArch64 (4-wide float32x4_t). MatMul uses cache-friendly 32×32 tiling for large matrices. Conv2D uses im2col to reshape convolution into a single matmul call. Scalar fallback on x86-64 and other architectures (SSE intrinsics trigger an LLVM code generation crash on `x86_64-unknown-none` due to the soft-float target — see runtime/src/inference/ops.rs for details).

**Precision Support:**
- FP32: Default, all operators
- FP16: Weight tensors can be stored as 16-bit (`TensorElemType::Float16`). MatMul converts FP16 weight rows to FP32 on-the-fly using a scratch buffer (halves weight memory, same compute precision). Native NEON FP16 compute (`float16x8_t`) deferred until Rust stabilizes `f16` (tracking issue rust-lang/rust#116909).
- INT8: Quantized matmul with INT32 accumulation and FP32 dequantized output. `quantize_fp32_to_int8()` for post-training quantization with min/max calibration. Asymmetric quantization (scale + zero_point per tensor, stored in `QuantParams`).

```rust
pub unsafe extern "C" fn rust_infer(
    model_index: u32, input_data: *const f32, input_len: usize,
    output_buf: *mut f32, output_len: usize,
) -> i32
```
Run inference on a loaded model. Writes output floats to `output_buf`. Returns the number of output floats on success, negative on error.

```rust
pub extern "C" fn rust_infer_classify(model_index: u32) -> i32
```
Run inference with zero input and return the argmax class index. Returns class index (>= 0) on success, -1 on error. Used by kernel-mode code that cannot use floating-point types.

```rust
pub extern "C" fn rust_infer_and_print(model_index: u32) -> i32
```
Run inference and print results (outputs, timing, predicted class) to UART. Returns 0 on success.

```rust
pub extern "C" fn rust_infer_stats(stats: *mut InferenceStats) -> i32
```
Fill an `InferenceStats` struct with cumulative performance data (total inferences, min/avg/max/last latency, error count). Returns 0 on success.

```rust
pub extern "C" fn rust_infer_bench(model_index: u32, iterations: u32) -> i32
```
Run an inference benchmark for N iterations and print min/avg/max latency to UART. Returns 0 on success.

```rust
pub extern "C" fn rust_inference_test() -> i32
```
Run inference engine tests. Returns the number of failures.

---

## Component System (`component/mod.rs`)

Registry for runtime components (model loaders, inference engines, preprocessors, custom components). Each component has a name, version, type, priority, and lifecycle state.

```rust
pub extern "C" fn component_system_init() -> i32
```
Initialize the component registry. Returns 0 on success.

```rust
pub extern "C" fn component_register(
    name: *const c_char, version: *const c_char,
    component_type: u8, priority: u8,
) -> i32
```
Register a new component. `component_type`: 0=ModelLoader, 1=InferenceEngine, 2=Preprocessor, 3=Custom. Returns the component index (>= 0) on success, -1 on error.

```rust
pub extern "C" fn component_unregister(index: u32) -> i32
```
Unregister a component. Returns 0 on success, -1 on error.

```rust
pub extern "C" fn component_count() -> u32
```
Return the number of registered components.

```rust
pub extern "C" fn component_get_info(index: u32, info: *mut ComponentInfo) -> i32
```
Fill a `ComponentInfo` struct with component metadata. Returns 0 on success, -1 if not found.

```rust
pub extern "C" fn component_find(name: *const c_char) -> i32
```
Find a component by name. Returns the index (>= 0) if found, -1 if not found.

```rust
pub extern "C" fn component_set_state(index: u32, state: u8) -> i32
```
Set a component's lifecycle state. States: 0=Loaded, 1=Initializing, 2=Ready, 3=Running, 4=Error, 5=Unloading. Returns 0 on success.

```rust
pub extern "C" fn rust_component_test() -> i32
```
Run component system tests. Returns the number of failures.

---

## Message Router (`msg_router.rs`)

Topic-based publish/subscribe message router. Components subscribe to named topics and exchange 64-byte messages. Supports wildcard subscriptions (patterns ending in `*`) and prioritized message delivery. All functions use `extern "C"` linkage and are the sole implementation — the former C file `kernel/src/msg_router.c` was removed in commit `8c53ada`.

```rust
pub extern "C" fn msg_router_init()
```
Initialize the message router. Clears all topics, subscriptions, and wildcard patterns.

```rust
pub extern "C" fn msg_router_subscribe(topic_name: *const u8, component_idx: i32) -> i32
```
Subscribe a component to a topic (created on first use). If the topic name ends with `*`, creates a **wildcard subscription** that matches all topics with the given prefix (e.g., `"/sensors/*"` matches `"/sensors/data"`, `"/sensors/temp"`). Returns 0 on success, -1 on error.

```rust
pub extern "C" fn msg_router_publish(topic_name: *const u8, data: *const u8) -> i32
```
Publish a 64-byte message to a topic with normal priority. Delivers to both exact-match and wildcard subscribers. Returns the number of subscribers that received the message.

```rust
pub extern "C" fn msg_router_publish_priority(
    topic_name: *const u8, data: *const u8, priority: u8,
) -> i32
```
Publish with explicit priority (0 = normal, higher = more urgent). When a component has multiple pending messages, `msg_router_receive` returns the highest-priority one first. Returns the number of subscribers that received the message.

```rust
pub extern "C" fn msg_router_receive(
    component_idx: i32, topic_out: *mut u8, data_out: *mut u8,
) -> i32
```
Receive the next pending message for a component. Fills `topic_out` (32 bytes) and `data_out` (64 bytes). Returns 1 if a message was received, 0 if none pending. Caller must call `msg_router_ack()` after processing.

```rust
pub extern "C" fn msg_router_ack(component_idx: i32)
```
Acknowledge receipt of a message. Clears the ready flag and sets the ack flag.

```rust
pub extern "C" fn msg_router_unsubscribe_all(component_idx: i32)
```
Remove all subscriptions for a component. Called during component unload.

```rust
pub extern "C" fn msg_router_get_subscriptions(
    component_idx: i32, topic_names: *mut [u8; 32], max_topics: u32, count_out: *mut u32,
) -> i32
```
List all topics a component is subscribed to. Fills `topic_names` array. Returns 0 on success.

```rust
pub extern "C" fn msg_router_list()
```
Print all topics and their subscribers to UART.

---

## Logging (`log.rs`)

Leveled logging infrastructure that outputs via UART FFI.

```rust
pub extern "C" fn rust_log_set_level(level: u8)
```
Set the log level. Values: 0=Debug, 1=Info, 2=Warn, 3=Error, 4=Off.

```rust
pub extern "C" fn rust_log_get_level() -> u8
```
Return the current log level.

```rust
pub unsafe extern "C" fn rust_log_debug(msg: *const c_char)
pub unsafe extern "C" fn rust_log_info(msg: *const c_char)
pub unsafe extern "C" fn rust_log_warn(msg: *const c_char)
pub unsafe extern "C" fn rust_log_error(msg: *const c_char)
```
Log a null-terminated message at the specified level. Messages below the current level are suppressed. Each message is prefixed with a timestamp and level tag.

```rust
pub extern "C" fn rust_log_test() -> i32
```
Run logging subsystem tests. Returns the number of failures.

---

## CPU Topology (`sched/heterogeneous.rs`)

Detects ARM64 CPU topology (homogeneous vs. big.LITTLE) and provides core selection hints.

```rust
pub extern "C" fn rust_cpu_num_cores() -> u8
```
Return the number of CPU cores.

```rust
pub extern "C" fn rust_cpu_is_heterogeneous() -> bool
```
Return true if the system has heterogeneous (big.LITTLE) cores.

```rust
pub extern "C" fn rust_select_inference_core(model_size: usize, is_urgent: u8) -> u8
```
Select an appropriate core for an inference workload based on model size and urgency. Returns a core ID, or 0xFF if no recommendation.

---

## GPU Compute (`lib.rs`)

```rust
pub extern "C" fn rust_gpu_print_status()
```
Print GPU subsystem status to UART.

```rust
pub extern "C" fn rust_gpu_compute_test() -> i32
```
Run GPU compute integration tests. Returns the number of failures.
