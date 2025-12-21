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

// Re-export commonly used types
pub use kernel_ffi::{KernelError, KernelResult, MemFlags, ShmFlags};

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
    // Call C panic with a static message
    // TODO: Format panic info into a buffer and pass to C
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

    // Test 5: Task creation with Rust entry point
    {
        let handle = unsafe {
            kernel_ffi::slm_task_create(
                b"rust_test\0".as_ptr() as *const core::ffi::c_char,
                rust_test_task,
                core::ptr::null_mut(),
            )
        };
        let passed = !handle.is_null();
        print_test_result(b"slm_task_create (Rust entry)\0", passed);
        if !passed { failures += 1; }
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
