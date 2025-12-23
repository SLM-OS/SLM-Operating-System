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
