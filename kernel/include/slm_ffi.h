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
 * Allocates memory from PMM for weight and workspace pools.
 * Returns: 0 on success, -1 on failure.
 */
extern int rust_model_mem_init(void);

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
    uint32_t _reserved;
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
 * Get GPU info for Rust.
 * Returns: 0 on success, -1 on error. Fills info struct.
 */
int slm_gpu_get_info(RustGpuInfo *info);

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

#endif /* SLM_FFI_H */
