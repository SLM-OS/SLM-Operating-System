/*
 * slm_ffi.h - FFI declarations for Rust runtime
 *
 * These functions provide a stable C ABI for the Rust runtime to call.
 * All functions use simple types that are FFI-safe.
 */

#ifndef SLM_FFI_H
#define SLM_FFI_H

#include <stdint.h>
#include <stddef.h>

#include "config.h"   /* MODEL_MEM_WEIGHT_MB / MODEL_MEM_WORKSPACE_MB defaults */

/*
 * ==========================================================================
 * Error Codes (shared between C and Rust)
 * ==========================================================================
 */

#define SLM_OK              0
#define SLM_ERR_NOMEM      -1
#define SLM_ERR_INVALID    -2
#define SLM_ERR_BUSY       -3
#define SLM_ERR_TIMEOUT    -4

/*
 * ==========================================================================
 * Memory Management
 * ==========================================================================
 */

/*
 * Allocate contiguous physical pages.
 *
 * @count: Number of 4KB pages to allocate
 * Returns: Physical address of first page, or 0 on failure
 */
void *slm_alloc_pages(size_t count);

/*
 * Free contiguous physical pages.
 *
 * @addr: Physical address of first page
 * @count: Number of pages to free
 */
void slm_free_pages(void *addr, size_t count);

/*
 * Map a region into the kernel virtual address space.
 *
 * @virt: Virtual address (must be 2MB aligned)
 * @phys: Physical address (must be 2MB aligned)
 * @size: Size in bytes (rounded up to 2MB)
 * @flags: VMM_FLAG_* values
 * Returns: SLM_OK on success, negative error code on failure
 */
int slm_map_region(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags);

/*
 * Unmap a region from the kernel virtual address space.
 *
 * @virt: Virtual address (must be 2MB aligned)
 * @size: Size in bytes (rounded up to 2MB)
 * Returns: SLM_OK on success, negative error code on failure
 */
int slm_unmap_region(uint64_t virt, uint64_t size);

/*
 * ==========================================================================
 * Debug Output
 * ==========================================================================
 */

/*
 * Print a string to the UART console.
 *
 * @s: Null-terminated string
 */
void slm_print(const char *s);

/*
 * ==========================================================================
 * Timing
 * ==========================================================================
 */

/*
 * Get current time in nanoseconds since boot.
 * Returns: Nanoseconds elapsed since boot
 */
uint64_t slm_get_time_ns(void);

/*
 * Pure helper: convert a tick count and timer frequency to nanoseconds
 * without overflowing for realistic uptimes. Split-multiply form of
 * `ticks * 1e9 / freq`; see the implementation comment for bounds.
 * Primarily exported so unit tests can drive synthetic tick values
 * past the x86-64 ~5 s overflow boundary (#171).
 */
uint64_t slm_time_ticks_to_ns(uint64_t ticks, uint64_t freq);

/*
 * Sleep the current task for the given number of milliseconds.
 *
 * @ms: Sleep duration in milliseconds (0 returns immediately)
 */
void slm_sleep_ms(uint32_t ms);

/*
 * ==========================================================================
 * GPU Cache Coherency
 * ==========================================================================
 */

/*
 * Flush CPU caches for a memory region so GPU sees latest data.
 * Called before GPU reads CPU-written data (DC CVAC).
 *
 * @addr: Virtual address of region
 * @size: Size in bytes
 */
void slm_gpu_sync_for_device(void *addr, size_t size);

/*
 * Invalidate CPU caches for a memory region so CPU sees GPU-written data.
 * Called after GPU writes, before CPU reads (DC IVAC).
 *
 * @addr: Virtual address of region
 * @size: Size in bytes
 */
void slm_gpu_sync_for_cpu(void *addr, size_t size);

/*
 * ==========================================================================
 * Task Management
 * ==========================================================================
 */

/* Task entry function type (for Rust) */
typedef void (*slm_task_entry_t)(void *arg);

/*
 * Create a new kernel task.
 *
 * @name: Null-terminated task name
 * @entry: Entry point function
 * @arg: Argument passed to entry function
 * Returns: Task ID (non-zero) on success, 0 on failure
 */
uint32_t slm_task_create(const char *name, slm_task_entry_t entry, void *arg);

/*
 * Set task priority.
 *
 * @task_id: Task ID (from slm_task_create)
 * @priority: Priority level (0-7, higher = more important)
 * Returns: SLM_OK on success, SLM_ERR_INVALID if task not found
 */
int slm_task_set_priority(uint32_t task_id, uint8_t priority);

/*
 * Set task deadline.
 *
 * @task_id: Task ID (from slm_task_create)
 * @deadline_ns: Absolute deadline in nanoseconds (0 = no deadline)
 * Returns: SLM_OK on success, SLM_ERR_INVALID if task not found
 */
int slm_task_set_deadline(uint32_t task_id, uint64_t deadline_ns);

/*
 * Get the current task's ID.
 *
 * Returns: Task ID of the currently running task, or 0 if no task is running.
 */
uint32_t slm_task_current(void);

/*
 * ==========================================================================
 * IPC - Message Queues
 * ==========================================================================
 */

/*
 * Send a message to a queue.
 *
 * @queue_id: Queue identifier
 * @msg: Pointer to message data
 * @msg_size: Size of message in bytes
 * @timeout_ms: 0 = non-blocking, -1 = wait forever, >0 = timeout in ms
 * Returns: SLM_OK on success, negative error code on failure
 */
int slm_msg_send(uint32_t queue_id, const void *msg, size_t msg_size, int timeout_ms);

/*
 * Receive a message from a queue.
 *
 * @queue_id: Queue identifier
 * @msg: Buffer to receive message
 * @msg_size: Size of buffer in bytes
 * @timeout_ms: 0 = non-blocking, -1 = wait forever, >0 = timeout in ms
 * Returns: SLM_OK on success, negative error code on failure
 */
int slm_msg_recv(uint32_t queue_id, void *msg, size_t msg_size, int timeout_ms);

/*
 * ==========================================================================
 * Rust Runtime Initialization (called from C)
 * ==========================================================================
 */

/*
 * Initialize Rust runtime heap.
 * Called by C kernel during boot.
 *
 * @heap_start: Pointer to heap memory
 * @heap_size: Size of heap in bytes
 */
extern void rust_heap_init(void *heap_start, size_t heap_size);

/*
 * Initialize Rust runtime.
 * Called by C kernel during boot.
 * Returns: 42 on success (magic number for verification)
 */
extern int rust_init(void);

/*
 * Print hello message from Rust.
 * For testing FFI integration.
 */
extern void rust_hello(void);

/*
 * Trigger a Rust panic for testing.
 * Verifies the Rust panic handler correctly calls C panic.
 * WARNING: This function does not return!
 */
extern void rust_test_panic(void);

/*
 * Validate FFI type sizes and alignments.
 * Called internally by rust_init().
 * Returns: 0 on success, non-zero error code on failure.
 */
extern int rust_ffi_validate(void);

/*
 * Run Rust FFI integration tests.
 * Tests all FFI functions from the Rust side.
 * Returns: Number of test failures (0 = all passed).
 */
extern int rust_run_tests(void);

/*
 * Initialize model memory pools.
 *
 * `weight_mb` and `workspace_mb` are megabyte sizes for the two pools.
 * Both must be multiples of 2 (each pool block is 2 MB); the Rust
 * allocator rejects misaligned values with -1. Per-platform defaults
 * live in <config.h> as MODEL_MEM_WEIGHT_MB / MODEL_MEM_WORKSPACE_MB
 * — call sites should pass those constants rather than hard-coding.
 *
 * Returns: 0 on success, -1 on failure (alignment error or PMM out
 * of memory).
 */
extern int rust_model_mem_init(uint32_t weight_mb, uint32_t workspace_mb);

/*
 * Run model memory tests.
 * Tests allocation, sharing, statistics, and GPU stubs.
 * Returns: Number of test failures (0 = all passed).
 */
extern int rust_model_mem_test(void);

/*
 * Model memory pool statistics (for shell command).
 */
typedef struct {
    size_t total_blocks;
    size_t free_blocks;
    size_t allocated_blocks;
    size_t shared_blocks;
    size_t peak_usage;
    /* Total eviction count (Phase AI-Eviction M6). When AI_EVICTION
     * is off this stays 0 for every snapshot — the allocator has no
     * policy path to increment it. */
    uint64_t evictions_total;
} RustPoolStats;

/*
 * Get weight pool statistics.
 * Returns statistics structure by value.
 */
extern RustPoolStats rust_weight_pool_stats(void);

/*
 * Get workspace pool statistics.
 * Returns statistics structure by value.
 */
extern RustPoolStats rust_workspace_pool_stats(void);

/*
 * ==========================================================================
 * Eviction-Policy Shell FFI (Phase AI-Eviction M7)
 * ==========================================================================
 */

/* Whether the ai_eviction Cargo feature was compiled in.
 * Returns 1 when enabled, 0 when disabled. */
extern int rust_eviction_enabled(void);

/* Copy the active policy's name into a caller-owned buffer.
 * Returns number of bytes written (excluding the null terminator). */
extern size_t rust_eviction_policy_name(uint8_t *out_buf, size_t buf_len);

/* Space-separated null-terminated list of registered policy names.
 * Points to static storage; do not free. */
extern const uint8_t *rust_eviction_policy_list(void);

/* Install a named policy. Returns 0 on success, -1 on unknown name,
 * -2 when the ai_eviction feature is disabled. */
extern int32_t rust_eviction_policy_set(const uint8_t *name);

/* Snapshot of evictable-block count (pool-agnostic). -1 when feature off. */
extern int32_t rust_eviction_snapshot_count(void);

/* Combined stats blob for the `eviction` shell command.
 * Expert weights are in basis points (0..10000, 1 bp = 0.01%) so this
 * header stays free of floats — the kernel compiles with
 * -mgeneral-regs-only. */
typedef struct {
    int32_t feature_enabled;
    int32_t models_available;
    uint64_t weight_evictions;
    uint64_t workspace_evictions;
    size_t weight_allocated;
    size_t weight_total;
    size_t workspace_allocated;
    size_t workspace_total;
    int32_t snapshot_candidates;
    uint32_t cacheus_expert_count;
    uint32_t expert_weights_bp[5];
    /* #115: generic per-policy counters. Reset on every policy swap. */
    uint64_t policy_decisions;
    uint64_t policy_fallbacks;
    uint64_t policy_avg_latency_ns;
} RustEvictionStats;

/* Populate `out` with the current eviction stats. Returns 0 on success. */
extern int32_t rust_eviction_get_stats(RustEvictionStats *out);

/* Runtime eviction-blob staging/activation backend (#dynamic-policy-loading).
 * kind_id: 1 = xgboost, 2 = mlp, 3 = cacheus_config.
 * state: 0 = empty, 1 = staged, 2 = active, 3 = rolled_back. */
typedef struct {
    uint16_t version;
    uint16_t kind_id;
    uint16_t feature_schema_version;
    uint16_t _reserved0;
    uint32_t payload_len;
    uint32_t checksum;
} RustEvictionBlobMeta;

typedef struct {
    uint16_t kind_id;
    uint16_t state;
    uint32_t has_staged;
    uint32_t has_active;
    uint32_t has_rollback;
    RustEvictionBlobMeta staged;
    RustEvictionBlobMeta active;
    RustEvictionBlobMeta rollback;
} RustEvictionBlobStatus;

/* Stage a validated blob from `data[0..len)`. Returns 0 on success,
 * -1 on invalid args / unknown kind, -2 when ai_eviction is disabled,
 * -3 on parse/validation failure, -4 when the blob header kind does
 * not match `kind_id`. */
extern int32_t rust_eviction_blob_validate(uint16_t kind_id, const uint8_t *data, size_t len);
extern int32_t rust_eviction_blob_stage(uint16_t kind_id, const uint8_t *data, size_t len);

/* Query, activate, roll back, or clear the runtime blob for `kind_id`.
 * Status returns 0 on success, -1 on invalid args/kind, -2 when feature
 * off. Activate returns -3 when no staged blob exists. Rollback returns
 * -3 when no rollback blob exists. Clear returns 0 on success. */
extern int32_t rust_eviction_blob_status(uint16_t kind_id, RustEvictionBlobStatus *out);
extern int32_t rust_eviction_blob_activate(uint16_t kind_id);
extern int32_t rust_eviction_blob_rollback(uint16_t kind_id);
extern int32_t rust_eviction_blob_clear(uint16_t kind_id);

/* Feature-name introspection (#112). */
extern uint32_t rust_eviction_feature_count(void);
extern size_t rust_eviction_feature_name(uint32_t index, uint8_t *buf, size_t buf_len);

/* Workload replay comparison (#117). */
typedef struct {
    uint8_t  policy_name[32];
    uint32_t faults;
    uint32_t hits;
    uint32_t total_accesses;
} RustEvictionCompareResult;
extern int32_t rust_eviction_workload_compare(
    RustEvictionCompareResult *out, uint32_t max_policies);

/* Per-pool eviction policy (#120). pool_id: 0 = weight, 1 = workspace. */
extern int32_t rust_eviction_policy_set_pool(uint8_t pool_id, const uint8_t *name);
extern size_t rust_eviction_policy_name_pool(uint8_t pool_id, uint8_t *out_buf, size_t buf_len);

/* Bump / set / read the active-inferences counter consumed by the
 * SlmHeuristicPolicy "inactive-models first" eviction tier. #113.
 * Bump clamps at zero; indices ≥ 64 are silently ignored. */
extern void rust_eviction_bump_active_inferences(uint8_t model_id, int32_t delta);
extern void rust_eviction_set_active_inferences(uint8_t model_id, uint32_t count);
extern uint32_t rust_eviction_get_active_inferences(uint8_t model_id);

/* CACHEUS weight trajectory entry (#111). Weights are in integer
 * basis points (0..10000, 1 bp = 0.01%) to keep the kernel's
 * -mgeneral-regs-only code float-free. */
typedef struct {
    uint64_t timestamp_ns;
    uint32_t n_experts;
    uint32_t _pad;
    uint32_t weights_bp[5];
} RustTrajectoryEntry;

/* Copy the CACHEUS weight trajectory into `out`, oldest-first.
 * Returns the number of entries written (>=0), 0 if no CACHEUS policy
 * is installed or the trajectory is empty, -1 on invalid arguments. */
extern int32_t rust_eviction_get_trajectory(
    RustTrajectoryEntry *out, uint32_t max_entries);

/* Per-policy average select_victim latency in nanoseconds over
 * `iterations` calls. Returns UINT64_MAX on error (unknown policy,
 * feature off, bogus clock, zero iterations). Used by the M9 bench
 * harness and the `eviction bench` shell command (future). */
extern uint64_t rust_eviction_bench_latency_ns(
    const uint8_t *name, uint32_t iterations);

/*
 * ==========================================================================
 * Model Loader FFI (Phase 5)
 * ==========================================================================
 */

/*
 * Model info structure returned by Rust model loader.
 */
typedef struct {
    uint8_t  name[32];
    uint8_t  format;        /* 0=GGUF, 1=ONNX, 2=Raw */
    uint8_t  _pad[3];
    uint64_t param_count;
    uint64_t weight_size;
    uint64_t workspace_size;
    uint32_t node_count;
    uint32_t input_count;
    uint32_t output_count;
    uint8_t  pinned;       /* 1 if pinned, 0 if evictable (#37) */
    uint8_t  _pad2[3];
    uint32_t use_count;    /* Number of inference calls (#37) */
    uint32_t last_used_ms; /* ms since boot of last access (#37) */
} RustModelInfo;

/*
 * Initialize the model loader registry.
 * Returns: 0 on success.
 */
extern int rust_model_loader_init(void);

/*
 * Load the built-in MNIST ONNX model (26 KB, embedded in the Rust binary).
 * Returns model registry index (>= 0) on success, negative on failure.
 */
extern int rust_model_load_builtin_mnist(void);

/*
 * Topic-name buffer length used by the Rust message router. The router
 * copies up to this many bytes (including NUL) into a `topic_out` buffer
 * passed to msg_router_receive(), so callers must provide at least this
 * much storage.
 */
#define MSG_ROUTER_TOPIC_LEN 16

/*
 * Publish a message to a topic via the message router.
 * Returns: Number of subscribers that received the message.
 */
extern int msg_router_publish(const uint8_t *topic_name, const uint8_t *data);

/*
 * Publish a message with explicit priority (0 = normal, higher = more urgent).
 * Higher-priority messages are delivered first by msg_router_receive.
 * Returns: Number of subscribers that received the message.
 */
extern int msg_router_publish_priority(const uint8_t *topic_name,
                                       const uint8_t *data, uint8_t priority);

/*
 * Publish a large message. Currently delegates to msg_router_publish
 * (copies data). Future: will pass by reference for true zero-copy.
 * Returns: Number of subscribers that received the message.
 */
extern int msg_router_publish_large(const char *topic_name, const char *data,
                                    uint32_t data_len);

/*
 * Load an ONNX model from a buffer.
 * Returns: Registry index (>= 0) on success, -1 on error.
 */
extern int rust_model_load(const char *name, const uint8_t *data, size_t data_len);

/*
 * Unload a model by registry index.
 * Returns: 0 on success, -1 on error.
 */
extern int rust_model_unload(uint32_t index);

/*
 * Get model info by registry index.
 * Returns: 0 on success, -1 on error. Fills info struct.
 */
extern int rust_model_get_info(uint32_t index, RustModelInfo *info);

/*
 * Get number of loaded models.
 */
extern uint32_t rust_model_count(void);

/*
 * Find a model by name (null-terminated).
 * Returns: Registry index (>= 0) if found, -1 if not found.
 */
extern int rust_model_find(const char *name);

/*
 * Pin a model to prevent LRU eviction.
 * Returns: 0 on success, -1 on error.
 */
extern int rust_model_pin(uint32_t index);

/*
 * Unpin a model (allow LRU eviction).
 * Returns: 0 on success, -1 on error.
 */
extern int rust_model_unpin(uint32_t index);

/*
 * Share a model's weight memory (increment refcount).
 * Returns: 0 on success, -1 on error.
 */
extern int rust_model_share_weights(uint32_t index);

/*
 * Set the per-model GPU-dispatch toggle.
 *
 * `enabled` is treated as a boolean (0 = off, non-zero = on). The
 * shell `model use-gpu <name|idx> on|off` command writes through this.
 * Default at load is ON, so flipping the master `gpu use inference`
 * flag enables every loaded model; this per-model override takes a
 * specific model back to CPU without disturbing the master.
 *
 * Returns: 0 on success, -1 if `index` is not a loaded model.
 */
extern int rust_model_set_gpu_dispatch(uint32_t index, uint8_t enabled);

/*
 * Total flat fp32 element count expected for the model's input
 * tensor(s).
 *
 * Used by the shell `model infer-file` command to reject shape-
 * mismatched files before handing them to the engine — replaces
 * the engine's opaque `EngineError::InvalidInput` with an operator-
 * readable "expected N floats, got M" line.
 *
 * Returns: element count (>= 1) on success, -1 on invalid index
 * or a degenerate (zero-product) input shape.
 */
extern int rust_model_expected_input_floats(uint32_t index);

/*
 * Run inference on a loaded model with caller-supplied fp32 input,
 * printing logits and argmax to UART. Returns the argmax class
 * index (>= 0) on success, or:
 *   -1 : `model_index` not loaded
 *   -2 : `input` is NULL or `input_floats` is 0
 *   -3 : engine error (shape mismatch, ops failure, etc.)
 *
 * `input` must point to `input_floats` × 4 bytes of 4-byte-aligned
 * fp32. The shell `model infer-file` command hands in a
 * `pmm_alloc_pages` buffer; the page alignment satisfies the fp32
 * load alignment the inference kernels assume.
 */
extern int rust_infer_buf_and_print(uint32_t model_index,
                                    const float *input,
                                    size_t input_floats);

/*
 * Run model loader tests.
 * Returns: Number of failures (0 = all passed).
 */
extern int rust_model_loader_test(void);

/*
 * ==========================================================================
 * Inference Engine FFI (Phase 5, M2)
 * ==========================================================================
 */

/*
 * Run inference on a loaded model.
 *
 * @model_index: Registry index (from rust_model_load)
 * @input_data: Pointer to FP32 input array
 * @input_len: Number of floats in input
 * @output_buf: Buffer for FP32 output
 * @output_len: Capacity of output buffer (in floats)
 * Returns: Number of output floats on success, negative on error.
 */
/*
 * Run inference with zero input and return the argmax class.
 * Returns: class index (>= 0) on success, -1 on error.
 * Used by kernel-mode components that cannot handle FP types.
 */
extern int rust_infer_classify(uint32_t model_index);

extern int rust_infer(uint32_t model_index, const float *input_data,
                      size_t input_len, float *output_buf, size_t output_len);

/*
 * Run inference engine tests.
 * Returns: Number of failures (0 = all passed).
 */
extern int rust_inference_test(void);

/*
 * Inference statistics structure.
 */
typedef struct {
    uint64_t total_inferences;
    uint64_t total_time_ns;
    uint64_t min_time_ns;
    uint64_t max_time_ns;
    uint64_t last_time_ns;
    uint64_t errors;
} RustInferStats;

/*
 * Get inference performance statistics.
 * Returns: 0 on success, -1 on error.
 */
extern int rust_infer_stats(RustInferStats *stats);

/*
 * Run inference benchmark (N iterations, prints results to UART).
 * Returns: 0 on success, -1 on error.
 */
extern int rust_infer_bench(uint32_t model_index, uint32_t iterations);

/*
 * Square FP32 matmul benchmark (128×128×128, N iterations). Prints
 * latency and achieved GFLOPS. Exercises the NEON-tiled FP32 kernel
 * on aarch64; scalar fallback on other archs.
 * Returns: 0 on success, -1 on error.
 */
extern int rust_matmul_bench_fp32(uint32_t iterations);

/*
 * Run an MNIST-shape Conv2D benchmark (1x4x28x28 × 8x4x3x3, pad=1,
 * stride=1, N iterations). Prints latency and achieved MFLOPS.
 * Returns: 0 on success, -1 on error.
 */
extern int rust_conv_bench_fp32(uint32_t iterations);

/*
 * Square matmul benchmark with B matrix stored as FP16 (128×128×128, N
 * iterations). Exercises the on-the-fly FP16→FP32 row conversion in
 * ops::matmul. Compare against rust_matmul_bench_fp32 to see
 * dequantization overhead. Returns 0 / -1.
 */
extern int rust_matmul_bench_fp16(uint32_t iterations);

/*
 * Square matmul benchmark with A and B stored as INT8 with scale +
 * zero_point (128×128×128, N iterations). Output is FP32 (dequantized).
 * Exercises the scalar INT32-accumulating path today; future NEON INT8
 * kernels will plug in here. Returns 0 / -1.
 */
extern int rust_matmul_bench_int8(uint32_t iterations);

/*
 * ==========================================================================
 * GPU Compute FFI (Phase 5, M3)
 * ==========================================================================
 */

/*
 * GPU info structure for Rust FFI.
 */
typedef struct {
    uint8_t  name[32];         /* Driver name (e.g., "nvidia" or "stub") */
    uint8_t  device[64];       /* Device description */
    uint32_t capabilities;     /* GPU_CAP_* flags */
    uint32_t cuda_cores;
    uint32_t tensor_cores;
    uint64_t memory_size;
    uint8_t  unified_memory;   /* 1 if CPU/GPU share memory */
    uint8_t  compute_ready;    /* 1 if submit/wait are implemented */
    uint8_t  _pad[6];
} RustGpuInfo;

/*
 * Check if GPU subsystem is available.
 * Returns: 1 if available, 0 if not.
 */
int slm_gpu_available(void);

/*
 * Operator-intent toggle for GPU inference dispatch.
 *
 * Returns 1 when `gpu use inference on` has been set (the default
 * after boot is 0 = off; M2's `gpu_consumer` flag starts disabled
 * so existing CPU behavior is preserved until an operator opts in).
 * The Rust engine consults this in `mnist_gpu_fastpath_eligible`
 * before attempting GPU dispatch.
 *
 * Mirrors operator intent on every platform — flipping the flag
 * succeeds whenever `slm_gpu_available()` is non-zero (stub or
 * real). On platforms without an MNIST GA10x fastpath (everywhere
 * except Jetson today) the engine still falls back to CPU even
 * when this returns 1; the flag records intent, the dispatch
 * decision lives in the Rust runtime.
 */
int slm_gpu_inference_enabled(void);

/*
 * Per-model GPU dispatch toggle.
 *
 * Layered on top of the master `gpu use inference` flag: the Rust
 * engine must see master=ON AND per-model=ON to fire the GPU
 * fastpath. Default after `model_load` is per-model=ON, so flipping
 * the master flag alone is enough to opt in for every loaded model;
 * `model use-gpu <name|idx> off` is the per-model override.
 *
 * Returns 1 if the per-model flag is on, 0 if off, 0 if `model_index`
 * is out of range.
 */
int slm_model_gpu_dispatch_enabled(uint32_t model_index);

/*
 * Get GPU info for Rust.
 * Returns: 0 on success, -1 on error. Fills info struct.
 */
int slm_gpu_get_info(RustGpuInfo *info);

/*
 * Run the pre-loaded MNIST GPU pipeline. Requires a v5 channel
 * handoff to be present in DRAM (set up by
 * scripts/gpu-kernel-mnist.c --preserve-for-kexec pre-kexec) and
 * for the channel to have been inherited (lazy-initialised on
 * first call: nvgpu inherit + nvgpu channel run automatically).
 *
 * `logits_bytes_out` must point at a 40-byte buffer that receives
 * the final op's output (10 fp32 values, little-endian). The
 * buffer is delivered as raw bytes because the kernel target
 * compiles with -mgeneral-regs-only and cannot manipulate
 * floating-point types directly; callers (Lua, Rust, dedicated
 * kernel modules with FP enabled) interpret as fp32. The argmax
 * helper below provides FP-free predicted-class extraction.
 *
 * Returns 0 on success; negative rc on failure (no v5 handoff,
 * channel inherit failed, dispatch timed out, etc.). On non-Jetson
 * platforms returns -1 unconditionally.
 */
int slm_gpu_run_mnist(void *logits_bytes_out);

/*
 * FP-free argmax over an array of fp32 bit patterns. Used by
 * Lua / shell callers that need the predicted class but can't do
 * fp32 comparisons directly under -mgeneral-regs-only.
 *
 * `logits_bytes` must point at `n_logits` * 4 bytes of
 * little-endian fp32 values. Returns the index of the largest
 * value, or -1 if `n_logits == 0` or `logits_bytes == NULL`. On
 * NaN inputs the comparison is undefined (no MNIST output should
 * produce NaN).
 */
int slm_fp32_argmax(const void *logits_bytes, uint32_t n_logits);

/*
 * Write user-supplied input bytes into the GPU's MNIST input buffer
 * at runtime. Pairs with slm_gpu_run_mnist() — call this first to
 * swap in a different image, then call run_mnist() to classify it.
 *
 * Requires a v6 channel handoff (`scripts/gpu-kernel-mnist.c`
 * --preserve-for-kexec writes one when v6 is enabled). The buffer is
 * cache-cleaned after the write so the GPU sees the fresh tensor on
 * the next dispatch.
 *
 * `bytes` is opaque to the kernel: for MNIST it should be
 * 1×1×28×28 = 784 fp32 values (3,136 bytes), but the kernel just
 * bounds-checks and memcpys. `cap` must be ≤ the handoff's
 * `input_buf_size`.
 *
 * Returns 0 on success, or on Jetson:
 *   -1 = no v6 handoff loaded (input_buf_phys == 0)
 *   -2 = cap exceeds input_buf_size
 *   -3 = NULL bytes pointer
 * On non-Jetson platforms returns -1 unconditionally.
 */
int slm_gpu_set_mnist_input(const void *bytes, size_t cap);

/*
 * Fill the GPU's MNIST input buffer with `n_floats` copies of the
 * fp32 bit pattern `value_bits` (e.g. 0x3F800000 for 1.0f). Built in
 * to dodge serial-link corruption on long `lua-admin -e` commands —
 * a uniform-fill is enough to prove that swapping the input changes
 * the argmax, and the entire call fits in a 30-character Lua string.
 *
 * `n_floats` must be ≤ input_buf_size / 4 (3,136 / 4 = 784 for the
 * MNIST pipeline). Cache-clean is issued after the fill.
 *
 * Returns 0 on success, negative rc on failure (no v6 handoff,
 * n_floats too large, etc.). On non-Jetson platforms returns -1.
 */
int slm_gpu_set_mnist_input_fill(uint32_t value_bits, uint32_t n_floats);

/*
 * Print GPU status to UART (called from Rust shell command).
 */
extern void rust_gpu_print_status(void);

/*
 * Run GPU compute integration tests.
 * Returns: Number of failures (0 = all passed).
 */
extern int rust_gpu_compute_test(void);

/*
 * Run component integration tests.
 * Returns: Number of failures (0 = all passed).
 */
extern int rust_component_test(void);

/*
 * ==========================================================================
 * Test Support Functions (called from Rust tests)
 * ==========================================================================
 */

/*
 * Get a test message queue ID for FFI testing.
 * Creates a queue if needed, returns the queue ID.
 * Returns: Queue ID (non-zero), or 0 if queue creation failed.
 */
uint32_t slm_ffi_get_test_queue(void);

/*
 * ==========================================================================
 * IRQ Control (for Rust-side IRQ-safe locks)
 * ==========================================================================
 */

/*
 * Disable local IRQs and return previous DAIF/EFLAGS state.
 * Paired with slm_irq_restore.
 */
uint64_t slm_irq_save(void);

/*
 * Restore local IRQ state from a prior slm_irq_save().
 */
void slm_irq_restore(uint64_t flags);

/*
 * ==========================================================================
 * SLM (Small Language Model) Loader — Phase SLM, M1
 * ==========================================================================
 *
 * Parallel API to rust_model_load (ONNX) for GGUF-format SLMs.
 * The SLM registry stores parsed metadata (architecture,
 * dimensions, vocab size) without copying weights into the
 * model_mem pool — that arrives in M5 once the decoder needs them.
 */

#define SLM_ARCH_NAME_LEN 16  /* matches Rust SLM_ARCH_LEN */
#define SLM_MODEL_NAME_LEN 32 /* matches Rust SLM_NAME_LEN */

/*
 * C-layout snapshot of an SLM registry entry. Returned by value via
 * rust_slm_get_info; matches the field order of Rust's
 * `SlmModelInfoC`.
 */
typedef struct {
    uint8_t  architecture[SLM_ARCH_NAME_LEN]; /* null-padded ASCII */
    uint8_t  name[SLM_MODEL_NAME_LEN];        /* null-padded ASCII */
    uint32_t block_count;          /* n_layer            */
    uint32_t embedding_length;     /* hidden width       */
    uint32_t head_count;           /* attention heads    */
    uint32_t head_count_kv;        /* GQA KV heads       */
    uint32_t head_dim;             /* per-head dimension */
    uint32_t feed_forward_length;  /* SwiGLU intermediate*/
    uint32_t context_length;       /* trained ctx        */
    uint32_t vocab_size;           /* tokenizer vocab    */
    uint32_t tensor_count;         /* GGUF tensor count  */
    uint32_t source_bytes;         /* on-disk size       */
    float    rope_freq_base;       /* RoPE theta         */
} SlmModelInfoC;

/*
 * Load a GGUF model into the SLM registry.
 * @name: null-terminated model name (clamped to 31 bytes).
 * @data: pointer to GGUF bytes.
 * @data_len: number of bytes at @data.
 * Returns slot index (>= 0) on success, -1 on error.
 */
extern int rust_slm_load(const uint8_t *name, const uint8_t *data, size_t data_len);

/*
 * Unload a SLM by slot index. Returns 0 on success, -1 on error.
 */
extern int rust_slm_unload(uint32_t index);

/*
 * Snapshot the registry entry at @index into @info.
 * Returns 0 on success, -1 if the slot is empty / out of range.
 */
extern int rust_slm_get_info(uint32_t index, SlmModelInfoC *info);

/*
 * Number of currently-loaded SLMs.
 */
extern uint32_t rust_slm_count(void);

#endif /* SLM_FFI_H */
