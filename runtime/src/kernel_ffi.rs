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

/// Opaque handle to a kernel task.
///
/// Tasks are managed by the C kernel. This is a pointer to `struct task`.
#[repr(transparent)]
#[derive(Debug, Clone, Copy)]
pub struct TaskHandle(pub *mut core::ffi::c_void);

impl TaskHandle {
    /// Null task handle.
    pub const NULL: Self = TaskHandle(core::ptr::null_mut());

    /// Check if this handle is null.
    pub fn is_null(self) -> bool {
        self.0.is_null()
    }
}

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
    /// Task handle on success, null on failure.
    pub fn slm_task_create(
        name: *const c_char,
        entry: extern "C" fn(*mut core::ffi::c_void),
        arg: *mut core::ffi::c_void,
    ) -> TaskHandle;

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
    pub fn panic(msg: *const u8) -> !;

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

    // Handle sizes (should be pointer-sized)
    assert!(core::mem::size_of::<TaskHandle>() == 8);
    assert!(core::mem::size_of::<QueueHandle>() == 8);
    assert!(core::mem::size_of::<BufferHandle>() == 8);

    // Flags sizes
    assert!(core::mem::size_of::<MemFlags>() == 4);
    assert!(core::mem::size_of::<ShmFlags>() == 4);
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
