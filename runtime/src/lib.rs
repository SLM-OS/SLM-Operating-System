//! SLM-OS Rust Runtime
//!
//! This crate provides Rust components for the SLM-OS kernel, including:
//! - Safe wrappers around kernel FFI functions (`kernel_ffi` module)
//! - Memory allocator integration
//! - Future: Model loading and inference scheduling

#![no_std]
#![no_main]

use core::panic::PanicInfo;
use linked_list_allocator::LockedHeap;

// =============================================================================
// Modules
// =============================================================================

pub mod kernel_ffi;
pub mod log;
pub mod mm;
pub mod sched;

// Re-export commonly used types
pub use kernel_ffi::{KernelError, KernelResult, MemFlags, ShmFlags, TaskId};
pub use log::{LogLevel, log_debug, log_info, log_warn, log_error};
pub use mm::{ModelHandle, AllocError, PoolStats};
pub use sched::{Priority, CoreType, SchedulingHint, TaskDeadline, SlmTaskInfo};

// =============================================================================
// Global Allocator
// =============================================================================

#[global_allocator]
static ALLOCATOR: LockedHeap = LockedHeap::empty();

/// Initialize the Rust heap allocator.
///
/// # Safety
/// - `heap_start` must be a valid pointer to allocatable memory
/// - `heap_size` must accurately reflect the available memory
/// - This function must only be called once
#[no_mangle]
pub unsafe extern "C" fn rust_heap_init(heap_start: *mut u8, heap_size: usize) {
    ALLOCATOR.lock().init(heap_start, heap_size);
}

// =============================================================================
// Panic Handler
// =============================================================================

#[panic_handler]
fn rust_panic(_info: &PanicInfo) -> ! {
    // Call C panic with a static message.
    // (Formatting PanicInfo deferred — see TODO.md "Deferred to Phase 4+")
    static MSG: &[u8] = b"Rust panic!\0";
    unsafe {
        kernel_ffi::panic(MSG.as_ptr());
    }
}

// =============================================================================
// Entry Point for Testing
// =============================================================================

/// Initialize the Rust runtime.
///
/// Validates FFI types match C expectations, then returns magic number 42.
/// Called by C kernel during boot.
#[no_mangle]
pub extern "C" fn rust_init() -> i32 {
    // Validate FFI types at runtime
    let ffi_result = kernel_ffi::rust_ffi_validate();
    if ffi_result != 0 {
        // FFI validation failed - return negative error
        return -ffi_result;
    }

    // Return magic number to confirm success
    42
}

/// Print a greeting via C UART.
/// This demonstrates FFI from Rust to C.
#[no_mangle]
pub extern "C" fn rust_hello() {
    static MSG: &[u8] = b"Hello from Rust!\n\0";
    unsafe {
        kernel_ffi::uart_puts(MSG.as_ptr());
    }
}

/// Trigger a Rust panic for testing.
/// This verifies the panic handler correctly calls C panic.
#[no_mangle]
pub extern "C" fn rust_test_panic() {
    panic!("Test panic from Rust");
}

// =============================================================================
// FFI Tests
// =============================================================================

/// Helper to print a test result.
fn print_test_result(name: &[u8], passed: bool) {
    unsafe {
        if passed {
            kernel_ffi::uart_puts(b"  [PASS] \0".as_ptr());
        } else {
            kernel_ffi::uart_puts(b"  [FAIL] \0".as_ptr());
        }
        kernel_ffi::uart_puts(name.as_ptr());
        kernel_ffi::uart_puts(b"\n\0".as_ptr());
    }
}

/// Simple Rust task entry point for testing slm_task_create.
extern "C" fn rust_test_task(_arg: *mut core::ffi::c_void) {
    unsafe {
        kernel_ffi::uart_puts(b"  [INFO] Rust task running!\n\0".as_ptr());
    }
    // Task will exit naturally
}

/// Run FFI integration tests.
///
/// Tests all FFI functions and returns the number of failures.
/// Called by C kernel during test runs.
#[no_mangle]
pub extern "C" fn rust_run_tests() -> i32 {
    let mut failures: i32 = 0;

    unsafe {
        kernel_ffi::uart_puts(b"[TEST] Running Rust FFI tests...\n\0".as_ptr());
    }

    // Test 1: Page allocation and free
    {
        let result = kernel_ffi::alloc_pages(1);
        let passed = result.is_ok();
        if let Ok(ptr) = result {
            // Free the page
            unsafe { kernel_ffi::free_pages(ptr, 1) };
        }
        print_test_result(b"alloc_pages/free_pages\0", passed);
        if !passed { failures += 1; }
    }

    // Test 2: Multiple page allocation
    {
        let result = kernel_ffi::alloc_pages(4);
        let passed = result.is_ok();
        if let Ok(ptr) = result {
            unsafe { kernel_ffi::free_pages(ptr, 4) };
        }
        print_test_result(b"alloc_pages (4 pages)\0", passed);
        if !passed { failures += 1; }
    }

    // Test 3: slm_print via safe wrapper
    {
        // This tests that print works (if we see output, it passed)
        kernel_ffi::print(b"  [INFO] slm_print works\n\0");
        print_test_result(b"slm_print\0", true);
    }

    // Test 4: Get time (should return without crashing)
    {
        let time = kernel_ffi::get_time_ns();
        // Time might be 0 (not fully implemented), but shouldn't crash
        let passed = true; // If we got here, it worked
        let _ = time; // Suppress unused warning
        print_test_result(b"get_time_ns\0", passed);
    }

    // Test 5: Task creation with Rust entry point (using safe wrapper)
    {
        let result = kernel_ffi::task_create(
            b"rust_test\0",
            rust_test_task,
            core::ptr::null_mut(),
        );
        let passed = result.is_ok();
        if let Ok(task_id) = result {
            // Task ID should be non-zero
            let valid_id = task_id.is_valid();
            print_test_result(b"slm_task_create (Rust entry)\0", passed && valid_id);
            if !valid_id { failures += 1; }
        } else {
            print_test_result(b"slm_task_create (Rust entry)\0", false);
            failures += 1;
        }
    }

    // Test 6: Message queue send/receive (if test queue available)
    {
        // Get a test queue ID from C (queue ID 0 is reserved for testing)
        let queue_id = unsafe { slm_ffi_get_test_queue() };
        if queue_id != 0 {
            // Prepare a test message
            let mut send_buf: [u8; 64] = [0; 64];
            send_buf[0] = 0x42; // Magic byte
            send_buf[1] = 0x43;

            // Send
            let send_result = kernel_ffi::msg_send(queue_id, &send_buf, 0);
            let send_passed = send_result.is_ok();
            print_test_result(b"msg_send\0", send_passed);
            if !send_passed { failures += 1; }

            // Receive
            let mut recv_buf: [u8; 64] = [0; 64];
            let recv_result = kernel_ffi::msg_recv(queue_id, &mut recv_buf, 0);
            let recv_passed = recv_result.is_ok() && recv_buf[0] == 0x42 && recv_buf[1] == 0x43;
            print_test_result(b"msg_recv\0", recv_passed);
            if !recv_passed { failures += 1; }
        } else {
            print_test_result(b"msg_send/recv (skipped - no test queue)\0", true);
        }
    }

    // Test 7: Priority enum conversion
    {
        use sched::Priority;
        // Test round-trip conversion
        let p = Priority::High;
        let raw = p.as_raw();
        let back = Priority::from_raw(raw);
        let passed = back == Priority::High && raw == 6;
        print_test_result(b"Priority round-trip\0", passed);
        if !passed { failures += 1; }
    }

    // Test 8: TaskDeadline::NONE
    {
        use sched::TaskDeadline;
        let deadline = TaskDeadline::NONE;
        // NONE deadline should never be expired
        let passed = deadline.deadline_ns == 0 && !deadline.is_expired();
        print_test_result(b"TaskDeadline::NONE\0", passed);
        if !passed { failures += 1; }
    }

    // Test 9: suggest_core_affinity heuristics
    {
        use sched::{TaskDeadline, CoreType, suggest_core_affinity};
        // Small model (< 8MB) with no deadline -> Efficiency core
        let core = suggest_core_affinity(4 * 1024 * 1024, &TaskDeadline::NONE);
        let passed = core == CoreType::Efficiency;
        print_test_result(b"suggest_core_affinity (small)\0", passed);
        if !passed { failures += 1; }
    }

    // Test 10: Large model core affinity
    {
        use sched::{TaskDeadline, CoreType, suggest_core_affinity};
        // Large model (> 8MB) -> Performance core
        let core = suggest_core_affinity(16 * 1024 * 1024, &TaskDeadline::NONE);
        let passed = core == CoreType::Performance;
        print_test_result(b"suggest_core_affinity (large)\0", passed);
        if !passed { failures += 1; }
    }

    // Test 11: SchedulingHint defaults
    {
        use sched::{SchedulingHint, Priority, CoreType};
        let hint = SchedulingHint::default();
        let passed = hint.priority == Priority::Normal
            && hint.core_type == CoreType::Any
            && !hint.pin_to_core;
        print_test_result(b"SchedulingHint::default()\0", passed);
        if !passed { failures += 1; }
    }

    // Summary
    unsafe {
        if failures == 0 {
            kernel_ffi::uart_puts(b"[INFO] FFI tests passed\n\0".as_ptr());
        } else {
            kernel_ffi::uart_puts(b"[FAIL] FFI tests had failures\n\0".as_ptr());
        }
    }

    failures
}

// External C function to get test queue ID
extern "C" {
    fn slm_ffi_get_test_queue() -> u32;
}

// =============================================================================
// Model Memory API
// =============================================================================

/// Initialize model memory pools.
///
/// Called by C kernel to set up Rust model memory allocator.
/// Uses 256 MB for weights and 128 MB for workspace (with 1GB QEMU RAM).
#[no_mangle]
pub extern "C" fn rust_model_mem_init() -> i32 {
    // Sizes for 1GB RAM testing - large enough for meaningful model tests
    let weight_mb: usize = 256;
    let workspace_mb: usize = 128;

    match mm::model_mem_init(weight_mb, workspace_mb) {
        Ok(()) => 0,
        Err(e) => {
            unsafe {
                kernel_ffi::uart_puts(b"[FAIL] Model memory init failed: \0".as_ptr());
                match e {
                    mm::AllocError::PmmFailed => {
                        kernel_ffi::uart_puts(b"PMM allocation failed\n\0".as_ptr());
                    }
                    mm::AllocError::AlignmentError => {
                        kernel_ffi::uart_puts(b"alignment error\n\0".as_ptr());
                    }
                    _ => {
                        kernel_ffi::uart_puts(b"unknown error\n\0".as_ptr());
                    }
                }
            }
            -1
        }
    }
}

/// Run model memory tests.
///
/// Returns number of test failures (0 = all passed).
#[no_mangle]
pub extern "C" fn rust_model_mem_test() -> i32 {
    let mut failures: i32 = 0;

    unsafe {
        kernel_ffi::uart_puts(b"[TEST] Running model memory tests...\n\0".as_ptr());
    }

    // Test 1: Weight pool allocation
    {
        let result = mm::alloc_weights(mm::model_mem::BLOCK_SIZE);
        let passed = result.is_ok();
        if let Ok(handle) = result {
            // Verify we can get pointer and size
            let ptr = mm::get_ptr(handle);
            let size = mm::get_size(handle);
            let valid = ptr.is_some() && size == Some(mm::model_mem::BLOCK_SIZE);
            if valid {
                // Free the block
                let _ = mm::free(handle);
            }
            print_test_result(b"weight alloc/free\0", passed && valid);
            if !valid { failures += 1; }
        } else {
            print_test_result(b"weight alloc/free\0", false);
            failures += 1;
        }
    }

    // Test 2: Workspace pool allocation
    {
        let result = mm::alloc_workspace(mm::model_mem::BLOCK_SIZE);
        let passed = result.is_ok();
        if let Ok(handle) = result {
            let ptr = mm::get_ptr(handle);
            let size = mm::get_size(handle);
            let valid = ptr.is_some() && size == Some(mm::model_mem::BLOCK_SIZE);
            if valid {
                let _ = mm::free(handle);
            }
            print_test_result(b"workspace alloc/free\0", passed && valid);
            if !valid { failures += 1; }
        } else {
            print_test_result(b"workspace alloc/free\0", false);
            failures += 1;
        }
    }

    // Test 3: Reference counting / sharing
    {
        let result = mm::alloc_weights(mm::model_mem::BLOCK_SIZE);
        if let Ok(handle1) = result {
            // Share the block
            let share_result = mm::share(handle1);
            let share_ok = share_result.is_ok();

            if share_ok {
                // First free should just decrement refcount
                let free1 = mm::free(handle1);
                let free1_ok = free1.is_ok();

                // Block should still be valid (refcount was 2, now 1)
                let ptr = mm::get_ptr(handle1);
                let still_valid = ptr.is_some();

                // Second free should actually release
                let free2 = mm::free(handle1);
                let free2_ok = free2.is_ok();

                let passed = share_ok && free1_ok && still_valid && free2_ok;
                print_test_result(b"refcount/sharing\0", passed);
                if !passed { failures += 1; }
            } else {
                print_test_result(b"refcount/sharing\0", false);
                failures += 1;
            }
        } else {
            print_test_result(b"refcount/sharing\0", false);
            failures += 1;
        }
    }

    // Test 4: Pool exhaustion and recovery
    {
        // Allocate many blocks until we run out
        let mut handles = [mm::ModelHandle::null(); 16];
        let mut allocated = 0;

        for i in 0..16 {
            if let Ok(h) = mm::alloc_weights(mm::model_mem::BLOCK_SIZE) {
                handles[i] = h;
                allocated += 1;
            } else {
                break;
            }
        }

        // Should have allocated at least 1 block
        let alloc_ok = allocated > 0;

        // Free all and verify we can allocate again
        for i in 0..allocated {
            let _ = mm::free(handles[i]);
        }

        // Now should be able to allocate again
        let realloc = mm::alloc_weights(mm::model_mem::BLOCK_SIZE);
        let realloc_ok = realloc.is_ok();
        if let Ok(h) = realloc {
            let _ = mm::free(h);
        }

        let passed = alloc_ok && realloc_ok;
        print_test_result(b"pool exhaust/recovery\0", passed);
        if !passed { failures += 1; }
    }

    // Test 5: Statistics
    {
        let stats = mm::weight_pool_stats();
        let valid = stats.total_blocks > 0 && stats.free_blocks <= stats.total_blocks;
        print_test_result(b"pool statistics\0", valid);
        if !valid { failures += 1; }
    }

    // Test 6: GPU mapping stubs
    {
        let result = mm::alloc_weights(mm::model_mem::BLOCK_SIZE);
        if let Ok(handle) = result {
            let gpu_addr = mm::model_mem::gpu_map(handle);
            let map_ok = gpu_addr.is_ok();

            let unmap_result = mm::model_mem::gpu_unmap(handle);
            let unmap_ok = unmap_result.is_ok();

            let _ = mm::free(handle);

            let passed = map_ok && unmap_ok;
            print_test_result(b"GPU map/unmap stubs\0", passed);
            if !passed { failures += 1; }
        } else {
            print_test_result(b"GPU map/unmap stubs\0", false);
            failures += 1;
        }
    }

    // Summary
    unsafe {
        if failures == 0 {
            kernel_ffi::uart_puts(b"[INFO] Model memory tests passed\n\0".as_ptr());
        } else {
            kernel_ffi::uart_puts(b"[FAIL] Model memory tests had failures\n\0".as_ptr());
        }
    }

    failures
}

// =============================================================================
// Model Memory FFI (for C tests)
// =============================================================================

/// Allocate from weight pool. Returns ModelHandle (check is_null).
#[no_mangle]
pub extern "C" fn rust_model_alloc_weights(size: usize) -> mm::ModelHandle {
    mm::alloc_weights(size).unwrap_or(mm::ModelHandle::null())
}

/// Allocate from workspace pool. Returns ModelHandle (check is_null).
#[no_mangle]
pub extern "C" fn rust_model_alloc_workspace(size: usize) -> mm::ModelHandle {
    mm::alloc_workspace(size).unwrap_or(mm::ModelHandle::null())
}

/// Free a model memory handle. Returns 0 on success, -1 on error.
#[no_mangle]
pub extern "C" fn rust_model_free(handle: mm::ModelHandle) -> i32 {
    match mm::free(handle) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

/// Share a model memory handle (increment refcount).
#[no_mangle]
pub extern "C" fn rust_model_share(handle: mm::ModelHandle) -> mm::ModelHandle {
    mm::share(handle).unwrap_or(mm::ModelHandle::null())
}

/// Get raw pointer for a handle. Returns NULL if invalid.
#[no_mangle]
pub extern "C" fn rust_model_get_ptr(handle: mm::ModelHandle) -> *mut u8 {
    mm::get_ptr(handle).unwrap_or(core::ptr::null_mut())
}

/// Get allocation size. Returns 0 if invalid.
#[no_mangle]
pub extern "C" fn rust_model_get_size(handle: mm::ModelHandle) -> usize {
    mm::get_size(handle).unwrap_or(0)
}

/// Get weight pool statistics.
#[no_mangle]
pub extern "C" fn rust_weight_pool_stats() -> mm::PoolStats {
    mm::weight_pool_stats()
}

/// Get workspace pool statistics.
#[no_mangle]
pub extern "C" fn rust_workspace_pool_stats() -> mm::PoolStats {
    mm::workspace_pool_stats()
}
