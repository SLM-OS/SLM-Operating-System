//! FFI bindings to the SLM-OS C kernel.
//!
//! This module provides:
//! - Raw `extern "C"` declarations matching `kernel/include/slm_ffi.h`
//! - Type-safe wrappers returning `Result` types
//! - `MemFlags` bitflags for memory mapping
//! - `KernelError` enum for error handling
//!
//! # FFI Calling Conventions
//!
//! All functions use the C ABI (`extern "C"`). Ownership rules:
//! - Pointers passed to C are borrowed; C does not take ownership
//! - Memory allocated by C (e.g., `slm_alloc_pages`) must be freed by C
//! - Strings passed to C must be null-terminated
//!
//! # Safety
//!
//! The raw FFI functions are unsafe. Use the safe wrappers in this module
//! whenever possible. The safe wrappers handle:
//! - Null pointer checks
//! - Error code translation to `Result`
//! - Memory alignment requirements

use core::ffi::c_char;
use core::ptr::NonNull;

use bitflags::bitflags;

// =============================================================================
// Error Codes (must match slm_ffi.h)
// =============================================================================

/// Success.
pub const SLM_OK: i32 = 0;
/// Out of memory.
pub const SLM_ERR_NOMEM: i32 = -1;
/// Invalid parameter.
pub const SLM_ERR_INVALID: i32 = -2;
/// Resource busy.
pub const SLM_ERR_BUSY: i32 = -3;
/// Operation timed out.
pub const SLM_ERR_TIMEOUT: i32 = -4;

/// Kernel error type returned by FFI functions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KernelError {
    /// Out of memory (SLM_ERR_NOMEM).
    OutOfMemory,
    /// Invalid parameter (SLM_ERR_INVALID).
    InvalidParam,
    /// Resource is busy (SLM_ERR_BUSY).
    Busy,
    /// Operation timed out (SLM_ERR_TIMEOUT).
    Timeout,
    /// Unknown error code from C.
    Unknown(i32),
}

impl KernelError {
    /// Convert from C error code to KernelError.
    pub fn from_code(code: i32) -> Option<Self> {
        if code >= 0 {
            None // Not an error
        } else {
            Some(match code {
                SLM_ERR_NOMEM => KernelError::OutOfMemory,
                SLM_ERR_INVALID => KernelError::InvalidParam,
                SLM_ERR_BUSY => KernelError::Busy,
                SLM_ERR_TIMEOUT => KernelError::Timeout,
                _ => KernelError::Unknown(code),
            })
        }
    }

    /// Convert KernelError back to C error code.
    pub fn to_code(self) -> i32 {
        match self {
            KernelError::OutOfMemory => SLM_ERR_NOMEM,
            KernelError::InvalidParam => SLM_ERR_INVALID,
            KernelError::Busy => SLM_ERR_BUSY,
            KernelError::Timeout => SLM_ERR_TIMEOUT,
            KernelError::Unknown(code) => code,
        }
    }
}

// =============================================================================
// Memory Flags (must match vmm.h and ipc.h)
// =============================================================================

bitflags! {
    /// Memory mapping flags for VMM operations.
    ///
    /// These must match the `VMM_FLAG_*` constants in `kernel/include/vmm.h`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct MemFlags: u32 {
        // Memory type flags
        /// Device memory (non-cacheable, non-gathering).
        const DEVICE         = 1 << 0;
        /// Non-cacheable normal memory.
        const NOCACHE        = 1 << 1;

        // Permission flags
        /// Memory is readable.
        const READ           = 1 << 2;
        /// Memory is writable.
        const WRITE          = 1 << 3;
        /// Memory is executable.
        const EXEC           = 1 << 4;

        // SLM-specific flags
        /// Memory is mapped for GPU access.
        const GPU_MAPPED     = 1 << 8;
        /// Memory contains model weights.
        const MODEL_PAGE     = 1 << 9;
        /// Memory is hot inference data.
        const INFERENCE_HOT  = 1 << 10;

        // Common combinations
        /// Kernel code: readable + executable.
        const KERNEL_CODE    = Self::READ.bits() | Self::EXEC.bits();
        /// Kernel data: readable + writable.
        const KERNEL_DATA    = Self::READ.bits() | Self::WRITE.bits();
        /// Device I/O: device + readable + writable.
        const DEVICE_IO      = Self::DEVICE.bits() | Self::READ.bits() | Self::WRITE.bits();
        /// DMA buffer: non-cacheable + readable + writable.
        const DMA_BUFFER     = Self::NOCACHE.bits() | Self::READ.bits() | Self::WRITE.bits();
    }
}

bitflags! {
    /// Shared buffer flags for IPC operations.
    ///
    /// These must match the `SHM_*` constants in `kernel/include/ipc.h`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct ShmFlags: u32 {
        /// Buffer is readable.
        const READ           = 1 << 0;
        /// Buffer is writable.
        const WRITE          = 1 << 1;
        /// Buffer is GPU-accessible.
        const GPU_ACCESSIBLE = 1 << 2;
        /// Buffer holds model weights.
        const MODEL_PAGE     = 1 << 3;
        /// Buffer holds hot inference data.
        const INFERENCE_HOT  = 1 << 4;

        /// Read + write access.
        const RDWR           = Self::READ.bits() | Self::WRITE.bits();
    }
}

// =============================================================================
// Opaque Handle Types
// =============================================================================

/// GPU info structure from the kernel (matches C RustGpuInfo).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GpuInfoFfi {
    pub name: [u8; 32],
    pub device: [u8; 64],
    pub capabilities: u32,
    pub cuda_cores: u32,
    pub tensor_cores: u32,
    pub memory_size: u64,
    pub unified_memory: u8,
    pub compute_ready: u8,
    pub _pad: [u8; 6],
}

impl GpuInfoFfi {
    pub const EMPTY: Self = Self {
        name: [0; 32],
        device: [0; 64],
        capabilities: 0,
        cuda_cores: 0,
        tensor_cores: 0,
        memory_size: 0,
        unified_memory: 0,
        compute_ready: 0,
        _pad: [0; 6],
    };
}

/// Task identifier returned by slm_task_create.
///
/// Tasks are identified by their ID (uint32_t in C). ID 0 is reserved
/// and indicates an invalid/failed task creation.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TaskId(pub u32);

impl TaskId {
    /// Invalid task ID (indicates failure).
    pub const INVALID: Self = TaskId(0);

    /// Check if this task ID is valid.
    pub fn is_valid(self) -> bool {
        self.0 != 0
    }
}

/// Legacy alias for TaskId (for compatibility during transition).
pub type TaskHandle = TaskId;

/// Opaque handle to a message queue.
///
/// Queues are managed by the C kernel. This is a pointer to `struct msg_queue`.
#[repr(transparent)]
#[derive(Debug, Clone, Copy)]
pub struct QueueHandle(pub *mut core::ffi::c_void);

impl QueueHandle {
    /// Null queue handle.
    pub const NULL: Self = QueueHandle(core::ptr::null_mut());

    /// Check if this handle is null.
    pub fn is_null(self) -> bool {
        self.0.is_null()
    }
}

/// Opaque handle to a shared buffer.
///
/// Buffers are managed by the C kernel. This is a pointer to `struct shared_buffer`.
#[repr(transparent)]
#[derive(Debug, Clone, Copy)]
pub struct BufferHandle(pub *mut core::ffi::c_void);

impl BufferHandle {
    /// Null buffer handle.
    pub const NULL: Self = BufferHandle(core::ptr::null_mut());

    /// Check if this handle is null.
    pub fn is_null(self) -> bool {
        self.0.is_null()
    }
}

// =============================================================================
// Raw FFI Declarations (unsafe)
// =============================================================================

extern "C" {
    // -------------------------------------------------------------------------
    // Memory Management
    // -------------------------------------------------------------------------

    /// Allocate contiguous physical pages.
    ///
    /// # Arguments
    /// * `count` - Number of 4KB pages to allocate
    ///
    /// # Returns
    /// Physical address of first page, or null on failure.
    pub fn slm_alloc_pages(count: usize) -> *mut u8;

    /// Free contiguous physical pages.
    ///
    /// # Arguments
    /// * `addr` - Physical address of first page
    /// * `count` - Number of pages to free
    pub fn slm_free_pages(addr: *mut u8, count: usize);

    /// Map a region into kernel virtual address space.
    ///
    /// # Arguments
    /// * `virt` - Virtual address (must be 2MB aligned)
    /// * `phys` - Physical address (must be 2MB aligned)
    /// * `size` - Size in bytes (rounded up to 2MB)
    /// * `flags` - MemFlags bits
    ///
    /// # Returns
    /// SLM_OK on success, negative error code on failure.
    pub fn slm_map_region(virt: u64, phys: u64, size: u64, flags: u32) -> i32;

    /// Unmap a region from kernel virtual address space.
    ///
    /// # Arguments
    /// * `virt` - Virtual address (must be 2MB aligned)
    /// * `size` - Size in bytes (rounded up to 2MB)
    ///
    /// # Returns
    /// SLM_OK on success, negative error code on failure.
    pub fn slm_unmap_region(virt: u64, size: u64) -> i32;

    // -------------------------------------------------------------------------
    // Debug Output
    // -------------------------------------------------------------------------

    /// Print a null-terminated string to UART console.
    pub fn slm_print(s: *const c_char);

    // -------------------------------------------------------------------------
    // Timing
    // -------------------------------------------------------------------------

    /// Get current time in nanoseconds since boot.
    pub fn slm_get_time_ns() -> u64;

    // -------------------------------------------------------------------------
    // GPU Cache Coherency
    // -------------------------------------------------------------------------

    /// Flush CPU caches so GPU sees latest data (DC CVAC).
    pub fn slm_gpu_sync_for_device(addr: *mut u8, size: usize);

    /// Invalidate CPU caches so CPU sees GPU-written data (DC IVAC).
    pub fn slm_gpu_sync_for_cpu(addr: *mut u8, size: usize);

    /// Check if GPU subsystem is available. Returns 1 if available, 0 if not.
    pub fn slm_gpu_available() -> i32;

    /// Get GPU info. Returns 0 on success, -1 on error.
    pub fn slm_gpu_get_info(info: *mut GpuInfoFfi) -> i32;

    /// Dispatch the MNIST inference pipeline on the Jetson GA10B GPU.
    ///
    /// `logits_bytes_out` must point at 40 bytes; on success the buffer
    /// is filled with 10 little-endian fp32 logits. Returns 0 on success,
    /// negative rc on failure (no v5 handoff, dispatch timed out, etc.).
    /// On non-Jetson platforms returns -1.
    pub fn slm_gpu_run_mnist(logits_bytes_out: *mut u8) -> i32;

    /// Write user-supplied input bytes into the GPU's MNIST input
    /// buffer. Pairs with `slm_gpu_run_mnist`. `cap` must be ≤ the
    /// v6 handoff's `input_buf_size`. Returns 0 on success, negative
    /// rc on failure (-1 = no v6 handoff, -2 = cap too large, -3 =
    /// NULL bytes pointer). On non-Jetson platforms returns -1.
    pub fn slm_gpu_set_mnist_input(bytes: *const u8, cap: usize) -> i32;

    /// FP-free argmax over fp32 bit patterns. `logits_bytes` points
    /// at `n_logits * 4` bytes of little-endian fp32. Returns the
    /// argmax index, or -1 on bad args. Used by callers that compile
    /// without floating-point support.
    pub fn slm_fp32_argmax(logits_bytes: *const u8, n_logits: u32) -> i32;

    // -------------------------------------------------------------------------
    // Task Management
    // -------------------------------------------------------------------------

    /// Create a new kernel task.
    ///
    /// # Arguments
    /// * `name` - Null-terminated task name
    /// * `entry` - Entry point function
    /// * `arg` - Argument passed to entry function
    ///
    /// # Returns
    /// Task ID (non-zero) on success, 0 on failure.
    pub fn slm_task_create(
        name: *const c_char,
        entry: extern "C" fn(*mut core::ffi::c_void),
        arg: *mut core::ffi::c_void,
    ) -> TaskId;

    /// Set task priority.
    ///
    /// # Arguments
    /// * `task_id` - Task ID (from slm_task_create)
    /// * `priority` - Priority level (0-7, higher = more important)
    ///
    /// # Returns
    /// SLM_OK on success, SLM_ERR_INVALID if task not found.
    pub fn slm_task_set_priority(task_id: TaskId, priority: u8) -> i32;

    /// Set task deadline.
    ///
    /// # Arguments
    /// * `task_id` - Task ID (from slm_task_create)
    /// * `deadline_ns` - Absolute deadline in nanoseconds (0 = no deadline)
    ///
    /// # Returns
    /// SLM_OK on success, SLM_ERR_INVALID if task not found.
    pub fn slm_task_set_deadline(task_id: TaskId, deadline_ns: u64) -> i32;

    /// Get the current task's ID.
    ///
    /// # Returns
    /// Task ID of the currently running task, or 0 if no task is running.
    pub fn slm_task_current() -> TaskId;

    /// Create a kernel task pinned to a specific CPU. Used by the
    /// parallel matmul worker pool to spawn one persistent worker per
    /// remote CPU at boot. Returns task ID (non-zero) on success.
    pub fn slm_task_create_pinned(
        name: *const c_char,
        entry: extern "C" fn(*mut core::ffi::c_void),
        arg: *mut core::ffi::c_void,
        target_cpu: u32,
    ) -> TaskId;

    /// Number of online CPUs. Used by the parallel matmul dispatcher
    /// to size its worker pool.
    pub fn slm_cpu_count() -> u32;

    /// Cross-CPU cache maintenance for the parallel matmul output
    /// buffer. Pre-SMPEN on Jetson the per-CPU L2s are incoherent;
    /// the worker writing its output slice from a remote CPU must
    /// `clean` (DC CVAC) and the reader must `invalidate` (DC IVAC)
    /// before reading. Both reduce to no-ops once SMPEN provides
    /// hardware coherency (GH issue #655).
    pub fn slm_cache_clean_range(addr: *const u8, size: usize);
    pub fn slm_cache_invalidate_range(addr: *mut u8, size: usize);

    /// Allocate from the kernel's non-cacheable region. Returns NULL
    /// on platforms without an NC region (QEMU virt, x86-64). Used
    /// by the parallel matmul mailboxes so dispatcher writes are
    /// instantly visible to polling workers.
    pub fn slm_ncmem_alloc(size: usize, align: usize) -> *mut u8;

    /// Issue an SEV broadcast to wake any CPU currently in WFE.
    /// Used by the parallel matmul dispatcher to wake remote
    /// workers after filling their mailbox.
    pub fn slm_sev();

    // -------------------------------------------------------------------------
    // IPC - Message Queues
    // -------------------------------------------------------------------------

    /// Send a message to a queue.
    ///
    /// # Arguments
    /// * `queue_id` - Queue identifier
    /// * `msg` - Pointer to message data
    /// * `msg_size` - Size of message in bytes
    /// * `timeout_ms` - Timeout: 0 = non-blocking, -1 = wait forever
    ///
    /// # Returns
    /// SLM_OK on success, negative error code on failure.
    pub fn slm_msg_send(queue_id: u32, msg: *const u8, msg_size: usize, timeout_ms: i32) -> i32;

    /// Receive a message from a queue.
    ///
    /// # Arguments
    /// * `queue_id` - Queue identifier
    /// * `msg` - Buffer to receive message
    /// * `msg_size` - Size of buffer in bytes
    /// * `timeout_ms` - Timeout: 0 = non-blocking, -1 = wait forever
    ///
    /// # Returns
    /// SLM_OK on success, negative error code on failure.
    pub fn slm_msg_recv(queue_id: u32, msg: *mut u8, msg_size: usize, timeout_ms: i32) -> i32;

    // -------------------------------------------------------------------------
    // Panic (defined in C kernel)
    // -------------------------------------------------------------------------

    /// Kernel panic - does not return.
    ///
    /// Renamed Rust binding (was `panic`) so that a downstream module
    /// doing `use kernel_ffi::*` does not lexically shadow Rust's
    /// `panic!` macro. `link_name = "panic"` keeps the C-side symbol
    /// unchanged (defined as `void panic(const char *fmt, ...)` in
    /// `kernel/include/debug.h`); only the Rust-visible name moves.
    #[link_name = "panic"]
    pub fn slm_panic(msg: *const u8) -> !;

    /// Print to UART (for rust_hello).
    pub fn uart_puts(s: *const u8);
}

// =============================================================================
// Safe Wrappers
// =============================================================================

/// Result type for kernel operations.
pub type KernelResult<T> = Result<T, KernelError>;

/// Allocate contiguous physical pages.
///
/// Returns a non-null pointer to the allocated memory, or an error.
///
/// # Arguments
/// * `count` - Number of 4KB pages to allocate
///
/// # Errors
/// Returns `KernelError::OutOfMemory` if allocation fails.
pub fn alloc_pages(count: usize) -> KernelResult<NonNull<u8>> {
    let ptr = unsafe { slm_alloc_pages(count) };
    NonNull::new(ptr).ok_or(KernelError::OutOfMemory)
}

/// Free contiguous physical pages.
///
/// # Safety
/// The pointer must have been returned by `alloc_pages` with the same count.
pub unsafe fn free_pages(ptr: NonNull<u8>, count: usize) {
    slm_free_pages(ptr.as_ptr(), count);
}

/// Map a region into kernel virtual address space.
///
/// # Arguments
/// * `virt` - Virtual address (must be 2MB aligned)
/// * `phys` - Physical address (must be 2MB aligned)
/// * `size` - Size in bytes (rounded up to 2MB)
/// * `flags` - Memory mapping flags
///
/// # Errors
/// Returns `KernelError` on failure.
pub fn map_region(virt: u64, phys: u64, size: u64, flags: MemFlags) -> KernelResult<()> {
    let ret = unsafe { slm_map_region(virt, phys, size, flags.bits()) };
    if ret == SLM_OK {
        Ok(())
    } else {
        Err(KernelError::from_code(ret).unwrap_or(KernelError::Unknown(ret)))
    }
}

/// Unmap a region from kernel virtual address space.
///
/// # Arguments
/// * `virt` - Virtual address (must be 2MB aligned)
/// * `size` - Size in bytes (rounded up to 2MB)
///
/// # Errors
/// Returns `KernelError` on failure.
pub fn unmap_region(virt: u64, size: u64) -> KernelResult<()> {
    let ret = unsafe { slm_unmap_region(virt, size) };
    if ret == SLM_OK {
        Ok(())
    } else {
        Err(KernelError::from_code(ret).unwrap_or(KernelError::Unknown(ret)))
    }
}

/// Print a string to the kernel console.
///
/// The string must be null-terminated.
pub fn print(s: &[u8]) {
    // Ensure null termination
    if s.last() == Some(&0) {
        unsafe { slm_print(s.as_ptr() as *const c_char) };
    }
    // If not null-terminated, we can't safely print
    // (in the future, could copy to a buffer and add null)
}

/// Get current time in nanoseconds since boot.
pub fn get_time_ns() -> u64 {
    unsafe { slm_get_time_ns() }
}

/// Check if GPU is available.
pub fn gpu_available() -> bool {
    unsafe { slm_gpu_available() != 0 }
}

/// Get GPU info from the kernel.
pub fn gpu_get_info() -> GpuInfoFfi {
    let mut info = GpuInfoFfi::EMPTY;
    unsafe { slm_gpu_get_info(&mut info); }
    info
}

/// Dispatch the MNIST GPU inference pipeline. Fills `logits_out` with
/// 10 fp32 values on success. Returns the kernel rc — 0 on success,
/// negative on failure (no GPU handoff, dispatch timed out, non-Jetson
/// platform, etc.).
pub fn gpu_run_mnist(logits_out: &mut [f32; 10]) -> i32 {
    // SAFETY: 10 f32 = 40 bytes, matches the FFI's required buffer size.
    unsafe {
        slm_gpu_run_mnist(logits_out.as_mut_ptr() as *mut u8)
    }
}

/// Write user-supplied input bytes into the GPU's MNIST input buffer
/// before the next `gpu_run_mnist`. Returns the kernel rc.
pub fn gpu_set_mnist_input(bytes: &[u8]) -> i32 {
    // SAFETY: passing a slice's pointer + length is the documented
    // contract for the FFI's `bytes` / `cap` parameters.
    unsafe {
        slm_gpu_set_mnist_input(bytes.as_ptr(), bytes.len())
    }
}

/// FP-free argmax over `n_logits` fp32 bit patterns.
pub fn fp32_argmax(logits: &[f32]) -> i32 {
    // SAFETY: f32 slice cast to u8 ptr; n_logits matches the
    // f32-element count.
    unsafe {
        slm_fp32_argmax(logits.as_ptr() as *const u8, logits.len() as u32)
    }
}

/// Send a message to a queue.
///
/// # Arguments
/// * `queue_id` - Queue identifier
/// * `msg` - Message data
/// * `timeout_ms` - Timeout in milliseconds (0 = non-blocking, -1 = wait forever)
///
/// # Errors
/// Returns `KernelError` on failure.
pub fn msg_send(queue_id: u32, msg: &[u8], timeout_ms: i32) -> KernelResult<()> {
    let ret = unsafe { slm_msg_send(queue_id, msg.as_ptr(), msg.len(), timeout_ms) };
    if ret == SLM_OK {
        Ok(())
    } else {
        Err(KernelError::from_code(ret).unwrap_or(KernelError::Unknown(ret)))
    }
}

/// Receive a message from a queue.
///
/// # Arguments
/// * `queue_id` - Queue identifier
/// * `buf` - Buffer to receive message
/// * `timeout_ms` - Timeout in milliseconds (0 = non-blocking, -1 = wait forever)
///
/// # Errors
/// Returns `KernelError` on failure.
pub fn msg_recv(queue_id: u32, buf: &mut [u8], timeout_ms: i32) -> KernelResult<()> {
    let ret = unsafe { slm_msg_recv(queue_id, buf.as_mut_ptr(), buf.len(), timeout_ms) };
    if ret == SLM_OK {
        Ok(())
    } else {
        Err(KernelError::from_code(ret).unwrap_or(KernelError::Unknown(ret)))
    }
}

/// Create a new kernel task (safe wrapper).
///
/// # Arguments
/// * `name` - Task name (must be null-terminated)
/// * `entry` - Entry point function
/// * `arg` - Argument passed to entry
///
/// # Returns
/// Task ID on success, or error if creation failed.
pub fn task_create(
    name: &[u8],
    entry: extern "C" fn(*mut core::ffi::c_void),
    arg: *mut core::ffi::c_void,
) -> KernelResult<TaskId> {
    // Ensure null termination
    if name.last() != Some(&0) {
        return Err(KernelError::InvalidParam);
    }
    let task_id = unsafe { slm_task_create(name.as_ptr() as *const c_char, entry, arg) };
    if task_id.is_valid() {
        Ok(task_id)
    } else {
        Err(KernelError::OutOfMemory)
    }
}

/// Set task priority.
///
/// # Arguments
/// * `task_id` - Task ID (from task_create)
/// * `priority` - Priority level (0-7, higher = more important)
///
/// # Errors
/// Returns `KernelError::InvalidParam` if task not found.
pub fn task_set_priority(task_id: TaskId, priority: u8) -> KernelResult<()> {
    let ret = unsafe { slm_task_set_priority(task_id, priority) };
    if ret == SLM_OK {
        Ok(())
    } else {
        Err(KernelError::from_code(ret).unwrap_or(KernelError::Unknown(ret)))
    }
}

/// Set task deadline.
///
/// # Arguments
/// * `task_id` - Task ID (from task_create)
/// * `deadline_ns` - Absolute deadline in nanoseconds (0 = no deadline)
///
/// # Errors
/// Returns `KernelError::InvalidParam` if task not found.
pub fn task_set_deadline(task_id: TaskId, deadline_ns: u64) -> KernelResult<()> {
    let ret = unsafe { slm_task_set_deadline(task_id, deadline_ns) };
    if ret == SLM_OK {
        Ok(())
    } else {
        Err(KernelError::from_code(ret).unwrap_or(KernelError::Unknown(ret)))
    }
}

/// Get the current task's ID.
///
/// # Returns
/// The task ID of the currently running task. Returns `TaskId(0)` if called
/// before the scheduler is initialized or from interrupt context.
pub fn task_current() -> TaskId {
    // SAFETY: slm_task_current is a simple accessor with no side effects.
    unsafe { slm_task_current() }
}

// =============================================================================
// FFI Type Size/Alignment Tests
// =============================================================================

/// Verify FFI type sizes and alignments at compile time.
///
/// These assertions ensure Rust types match C expectations.
#[allow(dead_code)]
const _: () = {
    // Pointer sizes
    assert!(core::mem::size_of::<*mut u8>() == 8); // 64-bit pointers
    assert!(core::mem::size_of::<usize>() == 8);   // size_t is 64-bit

    // Integer sizes
    assert!(core::mem::size_of::<u32>() == 4);     // uint32_t
    assert!(core::mem::size_of::<u64>() == 8);     // uint64_t
    assert!(core::mem::size_of::<i32>() == 4);     // int32_t / int

    // TaskId is 32-bit (u32)
    assert!(core::mem::size_of::<TaskId>() == 4);

    // Handle sizes (should be pointer-sized)
    assert!(core::mem::size_of::<QueueHandle>() == 8);
    assert!(core::mem::size_of::<BufferHandle>() == 8);

    // Flags sizes
    assert!(core::mem::size_of::<MemFlags>() == 4);
    assert!(core::mem::size_of::<ShmFlags>() == 4);

    // GpuInfoFfi field offsets — pin every field so a future struct
    // edit (re-ordering, adding a field, changing a type) fails the
    // build instead of silently desyncing from the C-side
    // RustGpuInfo layout. The C-side definition lives in
    // `kernel/src/slm_ffi.c`'s `slm_gpu_get_info` accessor.
    //
    // Note: `#[repr(C)]` honours natural alignment, so the u64
    // `memory_size` sits at offset 112 (not 108 — there is a 4-byte
    // padding gap after `tensor_cores: u32` at 104..108 to bring the
    // u64 to 8-aligned). Total size is 128 (struct alignment = 8).
    assert!(core::mem::offset_of!(GpuInfoFfi, name) == 0);
    assert!(core::mem::offset_of!(GpuInfoFfi, device) == 32);
    assert!(core::mem::offset_of!(GpuInfoFfi, capabilities) == 96);
    assert!(core::mem::offset_of!(GpuInfoFfi, cuda_cores) == 100);
    assert!(core::mem::offset_of!(GpuInfoFfi, tensor_cores) == 104);
    assert!(core::mem::offset_of!(GpuInfoFfi, memory_size) == 112);
    assert!(core::mem::offset_of!(GpuInfoFfi, unified_memory) == 120);
    assert!(core::mem::offset_of!(GpuInfoFfi, compute_ready) == 121);
    assert!(core::mem::size_of::<GpuInfoFfi>() == 128);
};

/// Run-time FFI validation tests.
///
/// Called from Rust init to verify FFI is working correctly.
#[no_mangle]
pub extern "C" fn rust_ffi_validate() -> i32 {
    // Test 1: Verify error code values match C
    if SLM_OK != 0 { return 1; }
    if SLM_ERR_NOMEM != -1 { return 2; }
    if SLM_ERR_INVALID != -2 { return 3; }
    if SLM_ERR_BUSY != -3 { return 4; }
    if SLM_ERR_TIMEOUT != -4 { return 5; }

    // Test 2: Verify MemFlags bits match C VMM_FLAG_* values
    if MemFlags::DEVICE.bits() != 1 { return 10; }
    if MemFlags::NOCACHE.bits() != 2 { return 11; }
    if MemFlags::READ.bits() != 4 { return 12; }
    if MemFlags::WRITE.bits() != 8 { return 13; }
    if MemFlags::EXEC.bits() != 16 { return 14; }
    if MemFlags::GPU_MAPPED.bits() != 256 { return 15; }
    if MemFlags::MODEL_PAGE.bits() != 512 { return 16; }
    if MemFlags::INFERENCE_HOT.bits() != 1024 { return 17; }

    // Test 3: Verify ShmFlags bits match C SHM_* values
    if ShmFlags::READ.bits() != 1 { return 20; }
    if ShmFlags::WRITE.bits() != 2 { return 21; }
    if ShmFlags::GPU_ACCESSIBLE.bits() != 4 { return 22; }

    // All tests passed
    0
}
