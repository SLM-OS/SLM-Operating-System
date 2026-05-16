# FFI (Foreign Function Interface) Documentation

This document describes the FFI boundary between the C kernel and Rust runtime in SLM-OS.

## Overview

SLM-OS uses a hybrid C/Rust architecture:
- **C kernel**: Core OS functionality (boot, memory, scheduling, IPC)
- **Rust runtime**: Higher-level components (future: model loading, inference scheduling)

The FFI layer allows safe communication between these components.

## Files

| File | Purpose |
|------|---------|
| `kernel/include/slm_ffi.h` | C function declarations for FFI |
| `kernel/src/slm_ffi.c` | C function implementations |
| `runtime/src/kernel_ffi.rs` | Rust extern declarations and safe wrappers |
| `runtime/src/lib.rs` | Rust entry points called from C |

---

## Calling Conventions

All FFI functions use the C ABI (`extern "C"` in Rust). This ensures:
- Consistent argument passing (ARM64 AAPCS64)
- No name mangling (`#[no_mangle]` in Rust)
- Predictable struct layouts (`#[repr(C)]`)

### Rust to C

```rust
// Rust declaration
extern "C" {
    fn slm_print(s: *const c_char);
}

// Call from Rust
unsafe { slm_print(b"Hello\0".as_ptr() as *const c_char); }
```

### C to Rust

```c
// C declaration (in slm_ffi.h)
extern int rust_init(void);

// Call from C
int magic = rust_init();
```

---

## Error Handling

### Error Codes

All FFI functions returning `int` use these error codes (defined in `slm_ffi.h`):

| Code | Name | Meaning |
|------|------|---------|
| 0 | `SLM_OK` | Success |
| -1 | `SLM_ERR_NOMEM` | Out of memory |
| -2 | `SLM_ERR_INVALID` | Invalid parameter |
| -3 | `SLM_ERR_BUSY` | Resource busy |
| -4 | `SLM_ERR_TIMEOUT` | Operation timed out |

### Rust Error Type

The Rust side wraps error codes in `KernelError`:

```rust
pub enum KernelError {
    OutOfMemory,    // SLM_ERR_NOMEM
    InvalidParam,   // SLM_ERR_INVALID
    Busy,           // SLM_ERR_BUSY
    Timeout,        // SLM_ERR_TIMEOUT
    Unknown(i32),   // Other error codes
}
```

Safe wrappers return `Result<T, KernelError>`:

```rust
// Safe wrapper
pub fn map_region(virt: u64, phys: u64, size: u64, flags: MemFlags)
    -> KernelResult<()>;

// Usage
match map_region(0x1000_0000, 0x1000_0000, 0x20_0000, MemFlags::KERNEL_DATA) {
    Ok(()) => { /* success */ }
    Err(KernelError::OutOfMemory) => { /* handle OOM */ }
    Err(e) => { /* other error */ }
}
```

---

## Ownership Rules

### Memory Allocation

| Function | Allocator | Free With |
|----------|-----------|-----------|
| `slm_alloc_pages()` | C (PMM) | `slm_free_pages()` |
| Rust `Box`/`Vec` | Rust (linked_list_allocator) | Rust drop |

**Rule**: Memory allocated by C must be freed by C. Memory allocated by Rust must be freed by Rust.

### Pointer Parameters

- **Borrows**: Pointers passed to functions are borrowed for the duration of the call only
- **No ownership transfer**: The callee does not take ownership unless explicitly documented
- **Null-terminated strings**: All string pointers must point to null-terminated data

```rust
// SAFE: String is borrowed, not transferred
slm_print(b"Hello\0".as_ptr() as *const c_char);
// String goes out of scope here, but slm_print has already finished
```

### Opaque Handles

Handles (`TaskHandle`, `QueueHandle`, `BufferHandle`) are opaque pointers to C structures:

```rust
#[repr(transparent)]
pub struct TaskHandle(pub *mut c_void);
```

**Rules**:
- Do not dereference handles in Rust
- Do not store handles beyond their lifetime
- Null handles indicate failure

---

## Type Mappings

### Primitives

| C Type | Rust Type | Size |
|--------|-----------|------|
| `int` / `int32_t` | `i32` | 4 bytes |
| `uint32_t` | `u32` | 4 bytes |
| `int64_t` | `i64` | 8 bytes |
| `uint64_t` | `u64` | 8 bytes |
| `size_t` | `usize` | 8 bytes (64-bit) |
| `void *` | `*mut c_void` | 8 bytes |
| `const char *` | `*const c_char` | 8 bytes |

### Flags

Use `bitflags` for flag types:

```rust
bitflags! {
    pub struct MemFlags: u32 {
        const DEVICE    = 1 << 0;
        const NOCACHE   = 1 << 1;
        const READ      = 1 << 2;
        const WRITE     = 1 << 3;
        const EXEC      = 1 << 4;
        // ...
    }
}
```

These map directly to C `#define` constants in `vmm.h`.

---

## Available FFI Functions

### Memory Management

```c
void *slm_alloc_pages(size_t count);
void slm_free_pages(void *addr, size_t count);
int slm_map_region(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags);
int slm_unmap_region(uint64_t virt, uint64_t size);
```

### Debug Output

```c
void slm_print(const char *s);  // Null-terminated string
```

### Timing

```c
uint64_t slm_get_time_ns(void);  // Nanoseconds since boot
uint64_t slm_time_ticks_to_ns(uint64_t ticks, uint64_t freq);
```
The `slm_get_time_ns` conversion uses an overflow-safe
split-multiply internally so it stays correct on high-frequency
timers (x86-64 TSC). `slm_time_ticks_to_ns` exposes the same
helper for tests and callers that need to convert explicit
tick counts.

### Task Management

```c
// Create a new task (returns task ID, or 0 on failure)
uint32_t slm_task_create(const char *name, void (*entry)(void *), void *arg);

// Get the current task's ID (0 if no task running)
uint32_t slm_task_current(void);

// Set task priority (0-7, higher = more important)
int slm_task_set_priority(uint32_t task_id, uint8_t priority);

// Set task deadline in nanoseconds (0 = no deadline)
int slm_task_set_deadline(uint32_t task_id, uint64_t deadline_ns);
```

### IPC (Message Queues)

```c
int slm_msg_send(uint32_t queue_id, const void *msg, size_t msg_size, int timeout_ms);
int slm_msg_recv(uint32_t queue_id, void *msg, size_t msg_size, int timeout_ms);
```

### Rust Entry Points (called from C)

```c
void rust_heap_init(void *heap_start, size_t heap_size);
int rust_init(void);      // Returns 42 on success
void rust_hello(void);    // Prints "Hello from Rust!"
void rust_test_panic(void);  // Triggers Rust panic (for testing)
int rust_ffi_validate(void); // Validates FFI types (0 = success)
```

### Rust Logging (called from C)

The Rust logging module provides FFI-accessible logging functions:

```c
// Set log level: 0=Debug, 1=Info, 2=Warn, 3=Error, 4=Off
void rust_log_set_level(uint8_t level);
uint8_t rust_log_get_level(void);

// Log messages (msg must be null-terminated)
void rust_log_debug(const char *msg);
void rust_log_info(const char *msg);
void rust_log_warn(const char *msg);
void rust_log_error(const char *msg);
```

### Heterogeneous Scheduling (called from C)

```c
uint8_t rust_cpu_num_cores(void);           // Get number of CPU cores
bool rust_cpu_is_heterogeneous(void);       // Check if big.LITTLE
uint8_t rust_select_inference_core(         // Recommend core for inference
    size_t model_size,
    uint8_t is_urgent
);  // Returns core ID or 0xFF if no recommendation
```

### Model Memory (called from C)

```c
// Initialize model memory pools (weight + workspace).
// Sizes come from MODEL_MEM_WEIGHT_MB / MODEL_MEM_WORKSPACE_MB in <config.h>;
// both must be multiples of 2 (the pool block size).
int rust_model_mem_init(uint32_t weight_mb, uint32_t workspace_mb);  // Returns 0 on success

// Pool statistics structure (returned by value)
typedef struct {
    size_t total_blocks;      // Total 2MB blocks in pool
    size_t free_blocks;       // Currently free blocks
    size_t allocated_blocks;  // Currently allocated (exclusive)
    size_t shared_blocks;     // Currently shared (refcount > 1)
    size_t peak_usage;        // High-water mark
} RustPoolStats;

// Get pool statistics
RustPoolStats rust_weight_pool_stats(void);     // Weight pool (model parameters)
RustPoolStats rust_workspace_pool_stats(void);  // Workspace pool (inference scratch)
```

### Model Loader (called from C)

The model loader parses ONNX models and manages a registry of loaded models. Implemented in Rust (`runtime/src/loader/`) with C FFI wrappers.

```c
// Initialize the model loader registry
// Returns: 0 on success
int rust_model_loader_init(void);

// Load an ONNX model from a buffer
// @param name: Model name (null-terminated)
// @param data: Pointer to ONNX protobuf data
// @param data_len: Size of data in bytes
// Returns: Registry index (>= 0) on success, -1 on error
int rust_model_load(const char *name, const uint8_t *data, size_t data_len);

// Unload a model by registry index
// Returns: 0 on success, -1 on error
int rust_model_unload(uint32_t index);

// Get model info by registry index
// Fills the RustModelInfo struct on success
// Returns: 0 on success, -1 on error
int rust_model_get_info(uint32_t index, RustModelInfo *info);

// Get number of currently loaded models
uint32_t rust_model_count(void);

// Find a model by name (null-terminated)
// Returns: Registry index (>= 0) if found, -1 if not found
int rust_model_find(const char *name);

// Run model loader self-tests
// Returns: Number of failures (0 = all passed)
int rust_model_loader_test(void);
```

#### RustModelInfo Structure

```c
typedef struct {
    uint8_t  name[32];        // Model name (null-terminated)
    uint8_t  format;          // 0=GGUF, 1=ONNX, 2=Raw
    uint8_t  _pad[3];         // Alignment padding
    uint64_t param_count;     // Total parameters across all weight tensors
    uint64_t weight_size;     // Weight data size in bytes
    uint64_t workspace_size;  // Workspace allocation in bytes
    uint32_t node_count;      // Operator nodes in the graph
    uint32_t input_count;     // Graph-level inputs (excluding initializers)
    uint32_t output_count;    // Graph-level outputs
    uint32_t _reserved;       // Future use
} RustModelInfo;
```

The registry supports up to 8 simultaneously loaded models. Memory is allocated from the weight and workspace pools (see Model Memory above) and freed automatically on unload.

### Inference Engine (called from C)

The inference engine executes ONNX operator graphs on loaded models. Implemented in Rust (`runtime/src/inference/`) with C FFI wrappers.

```c
// Run inference on a loaded model
// @param model_index: Registry index of loaded model
// @param input_data: Pointer to input float array (NULL for zero input)
// @param input_len: Number of input floats
// @param output_buf: Buffer to receive output floats
// @param output_len: Capacity of output buffer in floats
// Returns: output float count on success, negative on error
// -1 = NULL pointer, -2 = inference error, -3 = engine creation failed
int rust_infer(uint32_t model_index, const float *input_data,
               size_t input_len, float *output_buf, size_t output_len);

// Run inference with zero input and return argmax class index
// @param model_index: Registry index of loaded model
// Returns: class index (>= 0) on success, -1 on error
// Used by kernel-mode components that cannot handle FP types.
// Internally allocates a static 784-float input buffer (zeros) and
// a 64-float output buffer, runs the full inference pipeline, and
// returns the index of the highest output value.
int rust_infer_classify(uint32_t model_index);

// Run inference with zero input, print results to UART
// Prints output probabilities and predicted class
// @param model_index: Registry index of loaded model
// Returns: 0 on success, negative on error
int rust_infer_and_print(uint32_t model_index);

// Run inference engine self-tests
// Returns: number of test failures (0 = all passed)
int rust_inference_test(void);

// Get inference performance statistics
// @param stats: Pointer to RustInferStats struct to fill
// Returns: 0 on success, -1 on error
int rust_infer_stats(RustInferStats *stats);

// Benchmark model inference latency
// @param model_index: Registry index of loaded model
// @param iterations: Number of inference iterations to run
// Returns: 0 on success, negative on error
// Runs N iterations of inference, prints min/avg/max latency results to UART
int rust_infer_bench(uint32_t model_index, uint32_t iterations);
```

#### RustInferStats Structure

```c
typedef struct {
    uint64_t total_inferences;  // Total inference calls completed
    uint64_t total_errors;      // Total inference errors
    uint64_t min_latency_us;    // Minimum inference latency (microseconds)
    uint64_t max_latency_us;    // Maximum inference latency (microseconds)
    uint64_t avg_latency_us;    // Average inference latency (microseconds)
    uint64_t last_latency_us;   // Most recent inference latency (microseconds)
} RustInferStats;
```

The `rust_infer_stats()` function fills the caller-provided struct with cumulative statistics from the inference engine. Latency values are tracked per-inference and reported in microseconds. The `rust_infer_bench()` function runs the specified number of iterations on the given model and prints a summary (min/avg/max latency) to the UART console.

### GPU Compute (called from C)

The GPU compute layer provides capability detection and status reporting for GPU-accelerated inference. Implemented in Rust (`runtime/src/inference/gpu.rs`) with C FFI wrappers in `kernel/src/slm_ffi.c`.

```c
// Check if GPU subsystem is available
// Returns: 1 if available, 0 if not
int slm_gpu_available(void);

// Get GPU info for Rust
// @param info: Pointer to RustGpuInfo struct to fill
// Returns: 0 on success, -1 on error
int slm_gpu_get_info(RustGpuInfo *info);

// Print GPU status to UART (called from shell `model gpu` command)
// Displays driver, device, compute status, cores, and inference backend
void rust_gpu_print_status(void);

// Run GPU compute integration tests
// Tests capability detection, backend selection, and cache coherency stubs
// Returns: Number of failures (0 = all passed)
int rust_gpu_compute_test(void);
```

#### RustGpuInfo Structure

```c
typedef struct {
    uint8_t  name[32];         // Driver name (e.g., "nvidia", "stub", "none")
    uint8_t  device[64];       // Device description
    uint32_t capabilities;     // GPU_CAP_* flags (COMPUTE, TENSOR_CORES, UNIFIED_MEMORY)
    uint32_t cuda_cores;       // Number of CUDA cores (0 if N/A)
    uint32_t tensor_cores;     // Number of tensor cores (0 if N/A)
    uint64_t memory_size;      // GPU memory in bytes (0 for unified memory)
    uint8_t  unified_memory;   // 1 if CPU/GPU share memory
    uint8_t  compute_ready;    // 1 if gpu_submit/gpu_wait are implemented
    uint8_t  _pad[6];          // Alignment padding
} RustGpuInfo;
```

**Field offsets are pinned at compile time.** The Rust mirror struct
`GpuInfoFfi` (in `runtime/src/kernel_ffi.rs`) carries
`core::mem::offset_of!` const-eval asserts on every field plus a
`size_of` check (PR #598). Note that `#[repr(C)]` honours natural
alignment, so the `u64 memory_size` lives at offset **112**, not
108 — there is a 4-byte padding gap after `tensor_cores: u32` (at
104-108) to bring the u64 to 8-aligned. Total `sizeof(RustGpuInfo)`
is **128**, not 124. Any future struct edit (re-ordering, adding a
field, changing a type) fails the const-eval instead of silently
desyncing the Rust↔C layout.

The `slm_gpu_available()` function delegates to the kernel's `gpu_available()`, which checks whether a GPU driver has been registered and initialized. The `slm_gpu_get_info()` function queries the active GPU driver via `gpu_get_info()` and copies the result into the `RustGpuInfo` layout expected by Rust.

### Component System (called from C)

The component system is implemented in Rust (`runtime/src/component/`) with C FFI wrappers.

```c
// Initialize the component registry
int component_system_init(void);  // Returns 0 on success

// Get the number of registered components
uint32_t component_count(void);

// Register a new component
// @param name: Component name (null-terminated, max 31 chars)
// @param version: Version string (null-terminated, max 15 chars)
// @param component_type: 0=service, 1=driver, 2=application
// @param priority: 0=low, 1=normal, 2=high
// Returns component index on success, -1 on error
int component_register(const char *name, const char *version,
                       uint8_t component_type, uint8_t priority);

// Unregister a component by index
// Returns 0 on success, -1 on error
int component_unregister(uint32_t index);

// Find a component by name
// Returns component index, or -1 if not found
int component_find(const char *name);

// Get component info by index
// Returns 0 on success, -1 on error
int component_get_info(uint32_t index, ComponentInfo *info);

// Set component state
// States: 0=Loaded, 1=Initializing, 2=Running, 3=Suspended,
//         4=Updating, 5=Terminating, 6=Unloaded
// Returns 0 on success, -1 on error
int component_set_state(uint32_t index, uint8_t state);

// Hot-swap a running component with a new version.
// Preserves topic subscriptions across the swap.
// Returns new component index on success, -1 on error
int component_hot_swap(const char *old_name, const char *new_name);
```

### Message Router (Rust → C FFI)

The message router is implemented in Rust (`runtime/src/msg_router.rs`) and exports `extern "C"` functions:

```c
void msg_router_init(void);
int msg_router_subscribe(const char *topic_name, int component_idx);
int msg_router_publish(const char *topic_name, const char *data);
const char *msg_router_receive(int component_idx, char *topic_out);
void msg_router_ack(int component_idx);
void msg_router_unsubscribe_all(int component_idx);
void msg_router_get_subscriptions(int component_idx,
    char topic_names[][16], int *count_out, int max_topics);
void msg_router_list(void);
```

#### ComponentInfo Structure

```c
typedef struct {
    char name[32];          // Component name (null-terminated)
    char version[16];       // Version string (null-terminated)
    uint8_t component_type; // 0=service, 1=driver, 2=application
    uint8_t priority;       // 0=low, 1=normal, 2=high
    uint8_t state;          // Lifecycle state (see above)
    uint8_t _pad;           // Padding
    uint32_t task_id;       // Associated task ID (0 if none)
} ComponentInfo;
```

### XGBoost Scheduler Cascade (Rust → C FFI)

The `ai_xgb` scheduler policy (`SCHED_MODEL_KIND_XGBOOST = 0x1006`)
stores its 3-classifier cascade Rust-side because the trained model
(~9 MB) exceeds the static dense pool used by every other
`SCHED_MODEL_KIND_*`. The C side in `kernel/sched/ai/runtime_model.c`
forwards the kind id to these Rust entry points; everything else
(MLP, PPO, config, thresholds, rebalance) stays in the existing
dense paths.

Declared in `kernel/include/slm_ffi.h`:

```c
extern int32_t rust_sched_xgb_validate_blob(const uint8_t *data,
                                            size_t len);
extern int32_t rust_sched_xgb_stage_blob(const uint8_t *data,
                                         size_t len);
extern int32_t rust_sched_xgb_activate(void);
extern int32_t rust_sched_xgb_rollback(void);
extern int32_t rust_sched_xgb_clear(void);
extern int32_t rust_sched_xgb_status(void *out);
extern int32_t rust_sched_xgb_is_active(void);
extern int32_t rust_sched_xgb_predict(const float *state,
                                      int32_t *out_core,
                                      int32_t *out_priority,
                                      int32_t *out_preempt);
```

Return-code convention: `0` on success, `-1` on any error (NULL
pointer, parse failure, no cascade staged, no rollback slot, etc.).
The C side has no need to differentiate; Rust callers can pattern-
match on the `XgbStageError` variant if richer reporting is needed.

`rust_sched_xgb_status` writes a `struct sched_model_status` (76
bytes — pinned by `_Static_assert(sizeof(struct sched_model_status)
== 76, ...)` in `kernel/sched/ai/runtime_model.h` and a matching
`const _: () = assert!(core::mem::size_of::<SchedModelStatusC>() ==
76)` in `runtime/src/sched/xgb.rs`) to the supplied buffer. Both
asserts also cover the 20-byte `sched_model_meta` / `SchedModelMetaC`
that nests inside it.

`rust_sched_xgb_predict`'s `state` pointer must reference at least
`AI_STATE_DIM = 108` contiguous `float`s and be 4-byte aligned (any
C `float` array satisfies this naturally). Output pointers must be
non-NULL; on success the cascade's raw labels — not encoded class
indices — are written.

Runs from IRQ context on hardware-tick paths. Rust-side `predict()`
does an `Arc::clone` of the active cascade under the store lock and
then walks the trees with the lock released, so concurrent
`assign_cpu` calls on different CPUs don't serialize on hundreds of
thousands of node reads. The wire format (SEMB outer + XGBC inner)
is documented in `docs/contracts/runtime-blob-formats.md`.

---

## Panic Handling

Rust panics are routed to the C `panic()` function with source location:

```rust
#[panic_handler]
fn rust_panic(info: &PanicInfo) -> ! {
    // Prints: "RUST PANIC: at file.rs:line:column"
    // Then calls C panic() to halt the kernel
}
```

The panic handler extracts `PanicInfo::location()` to print the source file, line, and column before halting. The file path is copied to a stack buffer with null termination (since Rust `&str` is not null-terminated).

To test the panic handler:
```c
rust_test_panic();  // Does not return
```

---

## Type Validation

The Rust runtime validates FFI type sizes and alignments at:
1. **Compile time**: `const _: () = { assert!(...) };` blocks
2. **Run time**: `rust_ffi_validate()` called during `rust_init()`

Validation failures cause `rust_init()` to return a negative error code instead of 42.

---

## Safety Guidelines

### When Writing Rust FFI Code

1. **Use safe wrappers** whenever possible (`kernel_ffi::alloc_pages()` not `slm_alloc_pages()`)
2. **Document `unsafe` blocks** with why they are sound
3. **Validate pointers** before dereferencing
4. **Use `NonNull`** for pointers that cannot be null
5. **Ensure null termination** for strings passed to C

### When Writing C FFI Code

1. **Check for NULL** before dereferencing Rust-provided pointers
2. **Validate sizes** match expected values
3. **Document ownership** in function comments
4. **Use `const`** for read-only pointer parameters

---

## GPU Cache Coherency FFI (April 2026)

| C Function | Rust FFI | Purpose |
|------------|----------|---------|
| `slm_gpu_sync_for_device(addr, size)` | `slm_gpu_sync_for_device(addr, size)` | Clean CPU caches (DC CVAC) so GPU sees latest data |
| `slm_gpu_sync_for_cpu(addr, size)` | `slm_gpu_sync_for_cpu(addr, size)` | Invalidate CPU caches (DC IVAC) so CPU sees GPU-written data |

Used by the Rust model memory allocator (`gpu_map`/`gpu_unmap`) to ensure cache coherency when sharing memory between CPU and GPU on Jetson Orin (unified memory architecture).

---

## Syscall Interface (Phase 5 M4)

Milestone 4 adds a kernel-internal syscall dispatch mechanism for user-mode (EL0) components. This is **not** an FFI boundary — syscalls use the ARM64 `SVC #0` instruction rather than C function calls. The syscall dispatch table (`kernel/src/syscall.c`) routes requests from EL0 to kernel handlers that call existing FFI functions (e.g., `rust_infer`, `msg_router_publish`).

The syscall ABI is documented in `docs/component-isolation.md`. Seven syscalls are defined (SYS_EXIT through SYS_LOG), with user-side stubs in `kernel/include/user_syscall.h`.

---

## Future Extensions

Planned FFI additions:
- GPU command submission (requires GSP firmware)
- Inference scheduling (request queuing, batching)

These will follow the same patterns established here.

---

*Last updated: April 2026*
