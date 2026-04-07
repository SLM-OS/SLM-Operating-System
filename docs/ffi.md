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
```

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
// Initialize model memory pools (weight + workspace)
int rust_model_mem_init(void);  // Returns 0 on success

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

## Future Extensions

Planned FFI additions:
- Model loading functions (GGUF/ONNX parsing)
- GPU command submission (requires GSP firmware)
- Inference scheduling (request queuing, batching)
- Tensor operations

These will follow the same patterns established here.

---

*Last updated: April 2026*
