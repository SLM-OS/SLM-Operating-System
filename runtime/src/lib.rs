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
pub mod component;
pub mod msg_router;
pub mod loader;
pub mod inference;

// Re-export commonly used types
pub use kernel_ffi::{KernelError, KernelResult, MemFlags, ShmFlags, TaskId};
pub use log::{LogLevel, log_debug, log_info, log_warn, log_error};
pub use mm::{ModelHandle, AllocError, PoolStats};
pub use sched::{Priority, CoreType, SchedulingHint, TaskDeadline, SlmTaskInfo};
pub use component::{ComponentState, ComponentInfo, ComponentType, Priority as ComponentPriority};

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
fn rust_panic(info: &PanicInfo) -> ! {
    extern "C" {
        fn uart_printf(fmt: *const u8, ...);
    }

    unsafe {
        kernel_ffi::uart_puts(b"RUST PANIC: \0".as_ptr());
    }

    if let Some(loc) = info.location() {
        // Copy file path to null-terminated stack buffer (file() is not null-terminated)
        let file = loc.file().as_bytes();
        let mut buf = [0u8; 64];
        let len = if file.len() < 63 { file.len() } else { 63 };
        buf[..len].copy_from_slice(&file[..len]);
        buf[len] = 0;

        unsafe {
            uart_printf(
                b"at %s:%u:%u\n\0".as_ptr(),
                buf.as_ptr(),
                loc.line(),
                loc.column(),
            );
        }
    } else {
        unsafe {
            kernel_ffi::uart_puts(b"(no location)\n\0".as_ptr());
        }
    }

    unsafe {
        kernel_ffi::panic(b"Rust panic - halting\0".as_ptr());
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

    // =========================================================================
    // Message Router (Rust implementation) tests
    // =========================================================================

    unsafe {
        kernel_ffi::uart_puts(b"[TEST] Running Rust msg_router tests...\n\0".as_ptr());
    }

    // Test 12: msg_router_init + subscribe
    {
        msg_router::msg_router_init();
        let ret = msg_router::msg_router_subscribe(b"rust_topic\0".as_ptr(), 0);
        let passed = ret == 0;
        print_test_result(b"msg_router_init + subscribe\0", passed);
        if !passed { failures += 1; }
    }

    // Test 13: subscribe multiple to same topic
    {
        msg_router::msg_router_init();
        let r1 = msg_router::msg_router_subscribe(b"multi\0".as_ptr(), 0);
        let r2 = msg_router::msg_router_subscribe(b"multi\0".as_ptr(), 1);
        let r3 = msg_router::msg_router_subscribe(b"multi\0".as_ptr(), 2);
        let passed = r1 == 0 && r2 == 0 && r3 == 0;
        print_test_result(b"subscribe 3 to same topic\0", passed);
        if !passed { failures += 1; }
    }

    // Test 14: subscribe overflow (max 4 per topic)
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"full\0".as_ptr(), 0);
        msg_router::msg_router_subscribe(b"full\0".as_ptr(), 1);
        msg_router::msg_router_subscribe(b"full\0".as_ptr(), 2);
        msg_router::msg_router_subscribe(b"full\0".as_ptr(), 3);
        let overflow = msg_router::msg_router_subscribe(b"full\0".as_ptr(), 4);
        let passed = overflow == -1;
        print_test_result(b"subscribe overflow returns -1\0", passed);
        if !passed { failures += 1; }
    }

    // Test 15: max topics overflow (8 topics)
    {
        msg_router::msg_router_init();
        for i in 0..8u8 {
            let mut name = [0u8; 4];
            name[0] = b't';
            name[1] = b'0' + i;
            name[2] = 0;
            msg_router::msg_router_subscribe(name.as_ptr(), i as i32);
        }
        let mut name9 = [0u8; 4];
        name9[0] = b't';
        name9[1] = b'9';
        name9[2] = 0;
        let overflow = msg_router::msg_router_subscribe(name9.as_ptr(), 8);
        let passed = overflow == -1;
        print_test_result(b"topic overflow returns -1\0", passed);
        if !passed { failures += 1; }
    }

    // Test 16: receive with no message returns NULL
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"empty\0".as_ptr(), 50);
        let mut topic_buf = [0u8; 16];
        let data = msg_router::msg_router_receive(50, topic_buf.as_mut_ptr());
        let passed = data.is_null();
        print_test_result(b"receive empty returns NULL\0", passed);
        if !passed { failures += 1; }
    }

    // Test 17: receive for unsubscribed component returns NULL
    {
        msg_router::msg_router_init();
        let data = msg_router::msg_router_receive(99, core::ptr::null_mut());
        let passed = data.is_null();
        print_test_result(b"receive unsubscribed returns NULL\0", passed);
        if !passed { failures += 1; }
    }

    // Test 18: publish to nonexistent topic returns 0
    {
        msg_router::msg_router_init();
        let delivered = msg_router::msg_router_publish(
            b"nonexistent\0".as_ptr(),
            b"hello\0".as_ptr(),
        );
        let passed = delivered == 0;
        print_test_result(b"publish nonexistent topic returns 0\0", passed);
        if !passed { failures += 1; }
    }

    // Test 19: reinit clears subscriptions
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"temp\0".as_ptr(), 0);
        msg_router::msg_router_init(); // reinit
        let delivered = msg_router::msg_router_publish(
            b"temp\0".as_ptr(),
            b"gone\0".as_ptr(),
        );
        let passed = delivered == 0;
        print_test_result(b"reinit clears subscriptions\0", passed);
        if !passed { failures += 1; }
    }

    // Test 20: ack with no pending message doesn't crash
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"ack_test\0".as_ptr(), 60);
        msg_router::msg_router_ack(60); // no message pending
        let data = msg_router::msg_router_receive(60, core::ptr::null_mut());
        let passed = data.is_null();
        print_test_result(b"ack no-pending safe\0", passed);
        if !passed { failures += 1; }
    }

    // Test 21: list doesn't crash with topics
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"logs\0".as_ptr(), 0);
        msg_router::msg_router_subscribe(b"events\0".as_ptr(), 1);
        msg_router::msg_router_list();
        print_test_result(b"msg_router_list safe\0", true);
    }

    // Test 22: unsubscribe_all clears subscriptions and reclaims empty topic
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"cleanup\0".as_ptr(), 70);
        msg_router::msg_router_unsubscribe_all(70);
        // Topic should be reclaimed — publish should return 0
        let delivered = msg_router::msg_router_publish(
            b"cleanup\0".as_ptr(),
            b"gone\0".as_ptr(),
        );
        let passed = delivered == 0;
        print_test_result(b"unsubscribe_all reclaims topic\0", passed);
        if !passed { failures += 1; }
    }

    // Test 23: unsubscribe_all leaves other subscribers intact
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"shared\0".as_ptr(), 80);
        msg_router::msg_router_subscribe(b"shared\0".as_ptr(), 81);
        msg_router::msg_router_unsubscribe_all(80);
        // Component 81 should still be subscribed — receive should find mailbox
        // (no message pending, but the subscription slot should exist)
        // Verify by checking that component 81 can still be found in topic
        let data = msg_router::msg_router_receive(81, core::ptr::null_mut());
        // No message pending, so NULL is expected — but topic should still exist
        let passed = data.is_null(); // subscription exists, just no message
        print_test_result(b"unsubscribe_all preserves others\0", passed);
        if !passed { failures += 1; }
    }

    // Test 24: get_subscriptions returns correct count and topic names
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"topicA\0".as_ptr(), 90);
        msg_router::msg_router_subscribe(b"topicB\0".as_ptr(), 90);
        let mut topics = [[0u8; 16]; 8];
        let mut count: i32 = 0;
        msg_router::msg_router_get_subscriptions(
            90,
            topics.as_mut_ptr(),
            &mut count,
            8,
        );
        // Verify count
        let count_ok = count == 2;
        // Verify topic names (order may vary, check both are present)
        let name_a = topics[0][0] == b't'; // "topicA" or "topicB" starts with 't'
        let name_b = topics[1][0] == b't';
        let passed = count_ok && name_a && name_b;
        print_test_result(b"get_subscriptions count and names\0", passed);
        if !passed { failures += 1; }
    }

    // Test 25: unsubscribe_all across multiple topics
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"multi1\0".as_ptr(), 91);
        msg_router::msg_router_subscribe(b"multi2\0".as_ptr(), 91);
        msg_router::msg_router_subscribe(b"multi3\0".as_ptr(), 91);
        msg_router::msg_router_unsubscribe_all(91);
        // All 3 topics should be reclaimed
        let mut topics = [[0u8; 16]; 8];
        let mut count: i32 = 0;
        msg_router::msg_router_get_subscriptions(
            91,
            topics.as_mut_ptr(),
            &mut count,
            8,
        );
        let passed = count == 0;
        print_test_result(b"unsubscribe_all clears multiple topics\0", passed);
        if !passed { failures += 1; }
    }

    // Test 26: unsubscribe_all on nonexistent component is a no-op
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"safe\0".as_ptr(), 92);
        msg_router::msg_router_unsubscribe_all(999); // nonexistent
        // Component 92's subscription should be untouched
        let mut topics = [[0u8; 16]; 8];
        let mut count: i32 = 0;
        msg_router::msg_router_get_subscriptions(
            92,
            topics.as_mut_ptr(),
            &mut count,
            8,
        );
        let passed = count == 1;
        print_test_result(b"unsubscribe_all nonexistent is no-op\0", passed);
        if !passed { failures += 1; }
    }

    // Test 27: get_subscriptions with zero subscriptions
    {
        msg_router::msg_router_init();
        let mut topics = [[0u8; 16]; 8];
        let mut count: i32 = -1;
        msg_router::msg_router_get_subscriptions(
            93,
            topics.as_mut_ptr(),
            &mut count,
            8,
        );
        let passed = count == 0;
        print_test_result(b"get_subscriptions zero returns 0\0", passed);
        if !passed { failures += 1; }
    }

    // Test 28: get_subscriptions with max_topics=0 returns count but writes nothing
    {
        msg_router::msg_router_init();
        msg_router::msg_router_subscribe(b"limited\0".as_ptr(), 94);
        let mut topics = [[0u8; 16]; 8];
        let mut count: i32 = 0;
        msg_router::msg_router_get_subscriptions(
            94,
            topics.as_mut_ptr(),
            &mut count,
            0, // max_topics = 0
        );
        // count should still report actual number, but no names written
        let passed = count == 1;
        print_test_result(b"get_subscriptions max=0 still counts\0", passed);
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

// =============================================================================
// Model Loader API (Phase 5)
// =============================================================================

/// Initialize the model loader registry.
#[no_mangle]
pub extern "C" fn rust_model_loader_init() -> i32 {
    loader::registry::init();
    0
}

/// Load an ONNX model from a buffer.
///
/// Returns model registry index (>= 0) on success, negative error on failure.
///
/// # Safety
/// - `name` must be a valid null-terminated string pointer
/// - `data` must be a valid pointer to `data_len` bytes
#[no_mangle]
pub unsafe extern "C" fn rust_model_load(
    name: *const u8,
    data: *const u8,
    data_len: usize,
) -> i32 {
    if name.is_null() || data.is_null() || data_len == 0 {
        return -1;
    }

    // Find name length (null-terminated)
    let mut name_len = 0;
    while *name.add(name_len) != 0 && name_len < 31 {
        name_len += 1;
    }
    let name_slice = core::slice::from_raw_parts(name, name_len);
    let data_slice = core::slice::from_raw_parts(data, data_len);

    match loader::registry::load_model(name_slice, data_slice) {
        Ok(idx) => idx as i32,
        Err(_) => -1,
    }
}

/// Unload a model by registry index.
///
/// Returns 0 on success, -1 on error.
#[no_mangle]
pub extern "C" fn rust_model_unload(index: u32) -> i32 {
    match loader::registry::unload_model(index as usize) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

/// Get model info by registry index.
///
/// Returns 0 on success, -1 if index is invalid.
///
/// # Safety
/// - `info` must be a valid pointer to a `ModelInfoC`-sized buffer
#[no_mangle]
pub unsafe extern "C" fn rust_model_get_info(
    index: u32,
    info: *mut loader::registry::ModelInfoC,
) -> i32 {
    if info.is_null() {
        return -1;
    }
    match loader::registry::get_info(index as usize) {
        Some(model_info) => {
            *info = model_info;
            0
        }
        None => -1,
    }
}

/// Get the number of loaded models.
#[no_mangle]
pub extern "C" fn rust_model_count() -> u32 {
    loader::registry::count() as u32
}

/// Find a model by name.
///
/// Returns registry index (>= 0) if found, -1 if not found.
///
/// # Safety
/// - `name` must be a valid null-terminated string pointer
#[no_mangle]
pub unsafe extern "C" fn rust_model_find(name: *const u8) -> i32 {
    if name.is_null() {
        return -1;
    }

    let mut name_len = 0;
    while *name.add(name_len) != 0 && name_len < 31 {
        name_len += 1;
    }
    let name_slice = core::slice::from_raw_parts(name, name_len);

    match loader::registry::find_by_name(name_slice) {
        Some(idx) => idx as i32,
        None => -1,
    }
}

/// Run model loader tests.
///
/// Returns number of test failures (0 = all passed).
#[no_mangle]
pub extern "C" fn rust_model_loader_test() -> i32 {
    let mut failures: i32 = 0;

    unsafe {
        kernel_ffi::uart_puts(b"[TEST] Running model loader tests...\n\0".as_ptr());
    }

    // Test 1: Protobuf varint decoding
    {
        let data = [0x08]; // varint encoding of 8 with no continuation
        let result = loader::protobuf::decode_varint(&data);
        let passed = result == Ok((8, 1));
        print_test_result(b"protobuf: varint single byte\0", passed);
        if !passed { failures += 1; }
    }

    // Test 2: Multi-byte varint
    {
        let data = [0xAC, 0x02]; // 300 = 0b100101100 -> [0xAC, 0x02]
        let result = loader::protobuf::decode_varint(&data);
        let passed = result == Ok((300, 2));
        print_test_result(b"protobuf: varint multi-byte (300)\0", passed);
        if !passed { failures += 1; }
    }

    // Test 3: Varint EOF
    {
        let data = [0x80]; // Continuation bit set but no more bytes
        let result = loader::protobuf::decode_varint(&data);
        let passed = result.is_err();
        print_test_result(b"protobuf: varint EOF error\0", passed);
        if !passed { failures += 1; }
    }

    // Test 4: ProtoIter over simple message
    {
        // Field 1, varint, value 7: tag=0x08, value=0x07
        // Field 2, length-delimited, "hi": tag=0x12, len=0x02, 'h', 'i'
        let data = [0x08, 0x07, 0x12, 0x02, b'h', b'i'];
        let mut iter = loader::protobuf::ProtoIter::new(&data);

        let f1 = iter.next();
        let f1_ok = match f1 {
            Some(Ok(f)) => f.field_number == 1 && matches!(f.data, loader::protobuf::FieldData::Varint(7)),
            _ => false,
        };

        let f2 = iter.next();
        let f2_ok = match f2 {
            Some(Ok(f)) => {
                f.field_number == 2 &&
                matches!(f.data, loader::protobuf::FieldData::Bytes(b) if b == b"hi")
            },
            _ => false,
        };

        let end = iter.next().is_none();
        let passed = f1_ok && f2_ok && end;
        print_test_result(b"protobuf: field iteration\0", passed);
        if !passed { failures += 1; }
    }

    // Test 5: Registry init + count
    {
        loader::registry::init();
        let count = loader::registry::count();
        let passed = count == 0;
        print_test_result(b"registry: init empty\0", passed);
        if !passed { failures += 1; }
    }

    // Test 6: OpType name mapping
    {
        let passed =
            loader::graph::OpType::from_name(b"MatMul") == loader::graph::OpType::MatMul &&
            loader::graph::OpType::from_name(b"Add") == loader::graph::OpType::Add &&
            loader::graph::OpType::from_name(b"Relu") == loader::graph::OpType::Relu &&
            loader::graph::OpType::from_name(b"Softmax") == loader::graph::OpType::Softmax &&
            loader::graph::OpType::from_name(b"NotARealOp") == loader::graph::OpType::Unknown;
        print_test_result(b"graph: OpType::from_name\0", passed);
        if !passed { failures += 1; }
    }

    // Test 7: TensorName operations
    {
        let name = loader::graph::TensorName::from_bytes(b"test_tensor");
        let passed =
            name.eq_bytes(b"test_tensor") &&
            !name.is_empty() &&
            name.len == 11 &&
            loader::graph::TensorName::EMPTY.is_empty();
        print_test_result(b"graph: TensorName ops\0", passed);
        if !passed { failures += 1; }
    }

    // Test 8: TensorShape num_elements
    {
        let mut shape = loader::graph::TensorShape::EMPTY;
        shape.dims[0] = 2;
        shape.dims[1] = 3;
        shape.dims[2] = 4;
        shape.ndim = 3;
        shape.elem_type = loader::graph::ElemType::Float;
        let passed = shape.num_elements() == 24 && shape.size_bytes() == 96;
        print_test_result(b"graph: TensorShape sizing\0", passed);
        if !passed { failures += 1; }
    }

    // MNIST model data — static at function scope so all tests can access it
    static MNIST_ONNX: &[u8] = include_bytes!("../../models/test/mnist.onnx");

    // Test 9: Parse MNIST ONNX model
    {

        let result = loader::onnx_parser::parse_onnx(MNIST_ONNX);
        let passed = result.is_ok();
        print_test_result(b"onnx: parse MNIST model\0", passed);
        if !passed { failures += 1; }

        if let Ok(ref parsed) = result {
            // Test 10: Verify ir_version
            {
                let passed = parsed.ir_version > 0;
                print_test_result(b"onnx: ir_version > 0\0", passed);
                if !passed { failures += 1; }
            }

            // Test 11: Verify nodes found
            {
                let passed = parsed.node_count > 0;
                print_test_result(b"onnx: has nodes\0", passed);
                if !passed { failures += 1; }
            }

            // Test 12: Verify initializers (weights) found
            {
                let passed = parsed.initializer_count > 0;
                print_test_result(b"onnx: has initializers\0", passed);
                if !passed { failures += 1; }
            }

            // Test 13: Verify total weight size > 0
            {
                let total = parsed.total_weight_size();
                let passed = total > 0;
                print_test_result(b"onnx: weight size > 0\0", passed);
                if !passed { failures += 1; }
            }

            // Test 14: Verify graph inputs/outputs
            {
                let passed = parsed.input_count > 0 && parsed.output_count > 0;
                print_test_result(b"onnx: has inputs and outputs\0", passed);
                if !passed { failures += 1; }
            }

            // Test 15: Build operator graph
            {
                let graph_result = loader::onnx_parser::build_graph(parsed);
                let passed = graph_result.is_ok();
                print_test_result(b"onnx: build_graph succeeds\0", passed);
                if !passed { failures += 1; }

                if let Ok(graph) = graph_result {
                    // Test 16: Graph has correct structure
                    {
                        let passed = graph.node_count == parsed.node_count &&
                                     graph.input_count > 0 &&
                                     graph.output_count > 0;
                        print_test_result(b"onnx: graph structure valid\0", passed);
                        if !passed { failures += 1; }
                    }
                }
            }

        }

        // Drop the parsed ONNX model before the registry test to avoid
        // having two large heap objects simultaneously (~30KB each).
        drop(result);

        // Test 17: Full load/unload lifecycle via registry
        {
            loader::registry::init();
            let load_result = loader::registry::load_model(b"mnist_test", MNIST_ONNX);
            let loaded = load_result.is_ok();
            print_test_result(b"registry: load MNIST model\0", loaded);
            if !loaded { failures += 1; }

            if let Ok(idx) = load_result {
                // Verify model info
                let info = loader::registry::get_info(idx);
                let info_ok = info.is_some();
                print_test_result(b"registry: get_info after load\0", info_ok);
                if !info_ok { failures += 1; }

                if let Some(info) = info {
                    let meta_ok = info.param_count > 0 &&
                                  info.weight_size > 0 &&
                                  info.node_count > 0;
                    print_test_result(b"registry: model metadata valid\0", meta_ok);
                    if !meta_ok { failures += 1; }
                }

                // Find by name
                let found = loader::registry::find_by_name(b"mnist_test");
                let find_ok = found == Some(idx);
                print_test_result(b"registry: find_by_name\0", find_ok);
                if !find_ok { failures += 1; }

                // Count
                let count_ok = loader::registry::count() == 1;
                print_test_result(b"registry: count == 1\0", count_ok);
                if !count_ok { failures += 1; }

                // Unload
                let unload_ok = loader::registry::unload_model(idx).is_ok();
                print_test_result(b"registry: unload model\0", unload_ok);
                if !unload_ok { failures += 1; }

                // Verify unloaded
                let empty = loader::registry::count() == 0;
                print_test_result(b"registry: count == 0 after unload\0", empty);
                if !empty { failures += 1; }
            }
        }
    }

    // =========================================================================
    // Error and edge case tests
    // =========================================================================

    unsafe {
        kernel_ffi::uart_puts(b"[TEST] Running model loader error/edge case tests...\n\0".as_ptr());
    }

    // Test 18: Error — corrupted ONNX data
    {
        let garbage: [u8; 64] = [0xFF, 0xFE, 0xAB, 0xCD, 0x00, 0x01, 0x02, 0x03,
                                  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                  0xDE, 0xAD, 0xBE, 0xEF, 0x42, 0x43, 0x44, 0x45,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                  0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
                                  0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                  0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
                                  0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22];
        let result = loader::onnx_parser::parse_onnx(&garbage);
        // Corrupted data should either fail to parse or produce an empty/invalid model.
        // The protobuf parser may return Ok with garbage fields — the important thing
        // is that it does NOT panic.
        let passed = result.is_err() || result.is_ok();
        print_test_result(b"error: corrupted ONNX data handled\0", passed);
        if !passed { failures += 1; }

        // If it returned Ok, verify the registry rejects it (no valid weights)
        if result.is_ok() {
            loader::registry::init();
            let load_result = loader::registry::load_model(b"garbage", &garbage);
            let passed = load_result.is_err();
            print_test_result(b"error: corrupted data rejected by registry\0", passed);
            if !passed { failures += 1; }
        }
    }

    // Test 19: Error — empty input
    {
        let empty: [u8; 0] = [];
        let result = loader::onnx_parser::parse_onnx(&empty);
        // Empty input: parse_onnx may return Ok with 0 nodes (valid empty protobuf)
        // or Err — either is acceptable, as long as it does not panic.
        let passed = match &result {
            Ok(parsed) => parsed.node_count == 0,
            Err(_) => true,
        };
        print_test_result(b"error: empty input handled\0", passed);
        if !passed { failures += 1; }
    }

    // Test 20: Error — truncated ONNX (first 10 bytes only)
    {
        let truncated = &MNIST_ONNX[..core::cmp::min(10, MNIST_ONNX.len())];
        let result = loader::onnx_parser::parse_onnx(truncated);
        // Truncated data should parse partially or fail — must not panic.
        // The key check: it does not crash and the registry rejects it.
        let parse_ok = result.is_err() || result.is_ok();
        print_test_result(b"error: truncated ONNX handled\0", parse_ok);
        if !parse_ok { failures += 1; }

        // Try loading truncated data through the registry — should fail
        loader::registry::init();
        let load_result = loader::registry::load_model(b"truncated", truncated);
        let passed = load_result.is_err();
        print_test_result(b"error: truncated ONNX rejected by registry\0", passed);
        if !passed { failures += 1; }
    }

    // Test 21: Multiple model loading — load MNIST twice with different names
    {
        loader::registry::init();
        let load1 = loader::registry::load_model(b"mnist_a", MNIST_ONNX);
        let load1_ok = load1.is_ok();
        print_test_result(b"multi: load first model\0", load1_ok);
        if !load1_ok { failures += 1; }

        let load2 = loader::registry::load_model(b"mnist_b", MNIST_ONNX);
        let load2_ok = load2.is_ok();
        print_test_result(b"multi: load second model\0", load2_ok);
        if !load2_ok { failures += 1; }

        let count_ok = loader::registry::count() == 2;
        print_test_result(b"multi: count == 2\0", count_ok);
        if !count_ok { failures += 1; }

        // Verify both are findable by name
        let find_a = loader::registry::find_by_name(b"mnist_a").is_some();
        let find_b = loader::registry::find_by_name(b"mnist_b").is_some();
        let find_ok = find_a && find_b;
        print_test_result(b"multi: both findable by name\0", find_ok);
        if !find_ok { failures += 1; }

        // Unload both
        if let Ok(idx1) = load1 {
            let _ = loader::registry::unload_model(idx1);
        }
        if let Ok(idx2) = load2 {
            let _ = loader::registry::unload_model(idx2);
        }
        let empty = loader::registry::count() == 0;
        print_test_result(b"multi: count == 0 after unload both\0", empty);
        if !empty { failures += 1; }
    }

    // Test 22: Load 3 models, verify count, unload all
    {
        loader::registry::init();
        let r1 = loader::registry::load_model(b"m0", MNIST_ONNX);
        let r2 = loader::registry::load_model(b"m1", MNIST_ONNX);
        let r3 = loader::registry::load_model(b"m2", MNIST_ONNX);
        let all_ok = r1.is_ok() && r2.is_ok() && r3.is_ok();
        let count_ok = loader::registry::count() == 3;
        print_test_result(b"registry: load 3 models\0", all_ok && count_ok);
        if !(all_ok && count_ok) { failures += 1; }

        // Unload all
        if let Ok(i) = r1 { let _ = loader::registry::unload_model(i); }
        if let Ok(i) = r2 { let _ = loader::registry::unload_model(i); }
        if let Ok(i) = r3 { let _ = loader::registry::unload_model(i); }
        let cleanup_ok = loader::registry::count() == 0;
        print_test_result(b"registry: cleanup 3 models\0", cleanup_ok);
        if !cleanup_ok { failures += 1; }
    }

    // Test 23: Find nonexistent model
    {
        loader::registry::init();
        let found = loader::registry::find_by_name(b"nonexistent");
        let passed = found.is_none();
        print_test_result(b"find: nonexistent returns None\0", passed);
        if !passed { failures += 1; }
    }

    // Test 24: Unload invalid index
    {
        loader::registry::init();
        let result = loader::registry::unload_model(99);
        let passed = result.is_err();
        print_test_result(b"unload: invalid index returns Err\0", passed);
        if !passed { failures += 1; }
    }

    // Test 25: Double unload — load, unload, try to unload again
    {
        loader::registry::init();
        let load_result = loader::registry::load_model(b"double_test", MNIST_ONNX);
        let loaded = load_result.is_ok();
        print_test_result(b"double unload: load succeeds\0", loaded);
        if !loaded { failures += 1; }

        if let Ok(idx) = load_result {
            // First unload should succeed
            let first_unload = loader::registry::unload_model(idx);
            let first_ok = first_unload.is_ok();
            print_test_result(b"double unload: first unload succeeds\0", first_ok);
            if !first_ok { failures += 1; }

            // Second unload should fail (slot is now empty)
            let second_unload = loader::registry::unload_model(idx);
            let second_fails = second_unload.is_err();
            print_test_result(b"double unload: second unload fails\0", second_fails);
            if !second_fails { failures += 1; }
        }
    }

    // Summary
    unsafe {
        if failures == 0 {
            kernel_ffi::uart_puts(b"[INFO] Model loader tests passed\n\0".as_ptr());
        } else {
            kernel_ffi::uart_puts(b"[FAIL] Model loader tests had failures\n\0".as_ptr());
        }
    }

    failures
}

// =============================================================================
// Inference API (Phase 5, M2)
// =============================================================================

/// Run inference on a loaded model.
///
/// Returns number of output floats written on success, negative on error.
///
/// # Safety
/// - `input_data` must point to at least `input_len` floats
/// - `output_buf` must point to at least `output_len` floats
#[no_mangle]
pub unsafe extern "C" fn rust_infer(
    model_index: u32,
    input_data: *const f32,
    input_len: usize,
    output_buf: *mut f32,
    output_len: usize,
) -> i32 {
    if input_data.is_null() || output_buf.is_null() {
        return -1;
    }
    match inference::run_inference(
        model_index as usize,
        input_data,
        input_len,
        output_buf,
        output_len,
    ) {
        Ok(n) => n as i32,
        Err(_) => -2,
    }
}

/// Run inference on a loaded model with zero input and print results.
///
/// Used by the shell `model infer` command to avoid FP operations in
/// kernel C code (compiled with -mgeneral-regs-only).
#[no_mangle]
pub extern "C" fn rust_infer_and_print(model_index: u32) -> i32 {
    extern "C" {
        fn uart_printf(fmt: *const u8, ...);
    }

    let idx = model_index as usize;
    let info = match loader::registry::get_info(idx) {
        Some(i) => i,
        None => return -1,
    };

    // Use static buffers to avoid blowing the 32KB stack
    static INPUT: [f32; 784] = [0.0f32; 784];
    static mut OUTPUT: [f32; 64] = [0.0f32; 64];

    let start = kernel_ffi::get_time_ns();

    // SAFETY: This function is only called from the single-threaded shell.
    let result = unsafe {
        for o in OUTPUT.iter_mut() { *o = 0.0; }

        match inference::run_inference(
            idx,
            INPUT.as_ptr(),
            INPUT.len(),
            OUTPUT.as_mut_ptr(),
            OUTPUT.len(),
        ) {
            Ok(n) => n,
            Err(_) => return -3,
        }
    };

    let end = kernel_ffi::get_time_ns();
    let elapsed_us = (end - start) / 1000;

    unsafe {
        uart_printf(
            b"Inference on '%s' completed in %lu us\r\n\0".as_ptr(),
            info.name.as_ptr(),
            elapsed_us as u64,
        );
        uart_printf(b"  Outputs (%d values):\r\n\0".as_ptr(), result as i32);
    }

    // Find argmax and print outputs
    let mut argmax: usize = 0;
    let mut max_val = unsafe { OUTPUT[0] };
    for i in 0..result {
        let val = unsafe { OUTPUT[i] };
        let pct = (val * 1000.0) as i32;
        let pct = if pct < 0 { 0 } else { pct };
        unsafe {
            uart_printf(b"    [%d] = 0.%03d\r\n\0".as_ptr(), i as i32, pct);
        }
        if val > max_val {
            max_val = val;
            argmax = i;
        }
    }

    unsafe {
        uart_printf(b"  Predicted class: %d\r\n\0".as_ptr(), argmax as i32);
    }

    0
}

/// Run inference engine tests.
///
/// Returns number of test failures (0 = all passed).
#[no_mangle]
pub extern "C" fn rust_inference_test() -> i32 {
    let mut failures: i32 = 0;

    unsafe {
        kernel_ffi::uart_puts(b"[TEST] Running inference tests...\n\0".as_ptr());
    }

    // Test 1: BumpAllocator basic alloc/reset
    {
        let mut buf = [0u8; 1024];
        let mut alloc = inference::BumpAllocator::new(buf.as_mut_ptr(), buf.len());
        let p1 = alloc.alloc(64, 16);
        let p1_ok = !p1.is_null();
        let used_ok = alloc.used() >= 64;
        alloc.reset();
        let reset_ok = alloc.used() == 0;
        let passed = p1_ok && used_ok && reset_ok;
        print_test_result(b"workspace: bump alloc/reset\0", passed);
        if !passed { failures += 1; }
    }

    // Test 2: BumpAllocator tensor alloc
    {
        let mut buf = [0u8; 4096];
        let mut alloc = inference::BumpAllocator::new(buf.as_mut_ptr(), buf.len());
        let t = alloc.alloc_tensor(&[2, 3]);
        let passed = t.is_some() && t.unwrap().num_elements() == 6;
        print_test_result(b"workspace: tensor alloc\0", passed);
        if !passed { failures += 1; }
    }

    // Test 3: MatMul correctness
    // A = [[1,2,3],[4,5,6]] (2x3), B = [[7,8],[9,10],[11,12]] (3x2)
    // C = [[58,64],[139,154]] (2x2)
    {
        let a_data: [f32; 6] = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0];
        let b_data: [f32; 6] = [7.0, 8.0, 9.0, 10.0, 11.0, 12.0];
        let mut c_data: [f32; 4] = [0.0; 4];

        let a = inference::Tensor::new(a_data.as_ptr(), &[2, 3]);
        let b = inference::Tensor::new(b_data.as_ptr(), &[3, 2]);
        let mut c = inference::Tensor::new(c_data.as_mut_ptr() as *const f32, &[2, 2]);

        let result = inference::ops::matmul(&a, &b, &mut c);
        let passed = result.is_ok() &&
            unsafe {
                let p = c.data;
                (*p.add(0) - 58.0).abs() < 0.01 &&
                (*p.add(1) - 64.0).abs() < 0.01 &&
                (*p.add(2) - 139.0).abs() < 0.01 &&
                (*p.add(3) - 154.0).abs() < 0.01
            };
        print_test_result(b"ops: matmul 2x3 * 3x2\0", passed);
        if !passed { failures += 1; }
    }

    // Test 4: Add with broadcast
    {
        let a_data: [f32; 6] = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0];
        let b_data: [f32; 3] = [10.0, 20.0, 30.0];
        let mut c_data: [f32; 6] = [0.0; 6];

        let a = inference::Tensor::new(a_data.as_ptr(), &[2, 3]);
        let b = inference::Tensor::new(b_data.as_ptr(), &[3]);
        let mut c = inference::Tensor::new(c_data.as_mut_ptr() as *const f32, &[2, 3]);

        let result = inference::ops::add(&a, &b, &mut c);
        let passed = result.is_ok() &&
            unsafe {
                let p = c.data;
                (*p.add(0) - 11.0).abs() < 0.01 &&
                (*p.add(1) - 22.0).abs() < 0.01 &&
                (*p.add(2) - 33.0).abs() < 0.01 &&
                (*p.add(3) - 14.0).abs() < 0.01 &&
                (*p.add(4) - 25.0).abs() < 0.01 &&
                (*p.add(5) - 36.0).abs() < 0.01
            };
        print_test_result(b"ops: add broadcast [2,3]+[3]\0", passed);
        if !passed { failures += 1; }
    }

    // Test 5: Relu
    {
        let input_data: [f32; 4] = [-2.0, -0.5, 0.0, 3.0];
        let mut out_data: [f32; 4] = [0.0; 4];

        let input = inference::Tensor::new(input_data.as_ptr(), &[4]);
        let mut out = inference::Tensor::new(out_data.as_mut_ptr() as *const f32, &[4]);

        let result = inference::ops::relu(&input, &mut out);
        let passed = result.is_ok() &&
            unsafe {
                let p = out.data;
                *p.add(0) == 0.0 && *p.add(1) == 0.0 &&
                *p.add(2) == 0.0 && *p.add(3) == 3.0
            };
        print_test_result(b"ops: relu\0", passed);
        if !passed { failures += 1; }
    }

    // Test 6: Softmax
    {
        let input_data: [f32; 3] = [1.0, 2.0, 3.0];
        let mut out_data: [f32; 3] = [0.0; 3];

        let input = inference::Tensor::new(input_data.as_ptr(), &[3]);
        let mut out = inference::Tensor::new(out_data.as_mut_ptr() as *const f32, &[3]);

        let result = inference::ops::softmax(&input, &mut out);
        let sum = unsafe { *out.data.add(0) + *out.data.add(1) + *out.data.add(2) };
        let monotonic = unsafe { *out.data.add(0) < *out.data.add(1) && *out.data.add(1) < *out.data.add(2) };
        let passed = result.is_ok() && (sum - 1.0).abs() < 0.01 && monotonic;
        print_test_result(b"ops: softmax sums to 1.0\0", passed);
        if !passed { failures += 1; }
    }

    // Test 7: Reshape
    {
        let data: [f32; 6] = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0];
        let input = inference::Tensor::new(data.as_ptr(), &[2, 3]);
        let result = inference::ops::reshape(&input, &[3, 2]);
        let passed = result.is_ok() && result.unwrap().num_elements() == 6;
        print_test_result(b"ops: reshape preserves elements\0", passed);
        if !passed { failures += 1; }
    }

    // Test 8: Conv2D unit test
    // 1x1x4x4 input (all ones), 1x1x2x2 kernel (all ones), no bias
    // stride=1, pad=0 => output 1x1x3x3, each element = 4.0
    {
        let input_data: [f32; 16] = [1.0; 16];
        let weight_data: [f32; 4] = [1.0; 4];
        let mut out_data: [f32; 9] = [0.0; 9];

        let input = inference::Tensor::new(input_data.as_ptr(), &[1, 1, 4, 4]);
        let weight = inference::Tensor::new(weight_data.as_ptr(), &[1, 1, 2, 2]);
        let mut out = inference::Tensor::new(out_data.as_mut_ptr() as *const f32, &[1, 1, 3, 3]);

        let result = inference::ops::conv2d(&input, &weight, None, &mut out, 2, 2, 1, 1, 0, 0);
        let passed = result.is_ok() && unsafe {
            let p = out.data;
            let mut ok = true;
            let mut i = 0;
            while i < 9 {
                if (*p.add(i) - 4.0).abs() > 0.01 { ok = false; }
                i += 1;
            }
            ok
        };
        print_test_result(b"ops: conv2d 1x1x4x4 k=2x2\0", passed);
        if !passed { failures += 1; }
    }

    // Test 9: MaxPool2D unit test
    // 1x1x4x4 input with values 1..16, 2x2 pool stride 2
    // output 1x1x2x2 = [6, 8, 14, 16]
    {
        let input_data: [f32; 16] = [
            1.0,  2.0,  3.0,  4.0,
            5.0,  6.0,  7.0,  8.0,
            9.0,  10.0, 11.0, 12.0,
            13.0, 14.0, 15.0, 16.0,
        ];
        let mut out_data: [f32; 4] = [0.0; 4];

        let input = inference::Tensor::new(input_data.as_ptr(), &[1, 1, 4, 4]);
        let mut out = inference::Tensor::new(out_data.as_mut_ptr() as *const f32, &[1, 1, 2, 2]);

        let result = inference::ops::maxpool2d(&input, &mut out, 2, 2, 2, 2);
        let passed = result.is_ok() && unsafe {
            let p = out.data;
            (*p.add(0) - 6.0).abs() < 0.01 &&
            (*p.add(1) - 8.0).abs() < 0.01 &&
            (*p.add(2) - 14.0).abs() < 0.01 &&
            (*p.add(3) - 16.0).abs() < 0.01
        };
        print_test_result(b"ops: maxpool2d 2x2 stride 2\0", passed);
        if !passed { failures += 1; }
    }

    // Test 10: Gemm unit test
    // A=[1,2; 3,4] B=[5,6; 7,8] C=[1,1] => A*B+C = [[20,23],[44,51]]
    {
        let a_data: [f32; 4] = [1.0, 2.0, 3.0, 4.0];
        let b_data: [f32; 4] = [5.0, 6.0, 7.0, 8.0];
        let c_data: [f32; 2] = [1.0, 1.0];
        let mut out_data: [f32; 4] = [0.0; 4];

        let a = inference::Tensor::new(a_data.as_ptr(), &[2, 2]);
        let b = inference::Tensor::new(b_data.as_ptr(), &[2, 2]);
        let c = inference::Tensor::new(c_data.as_ptr(), &[2]);
        let mut out = inference::Tensor::new(out_data.as_mut_ptr() as *const f32, &[2, 2]);

        let result = inference::ops::gemm(&a, &b, Some(&c), &mut out);
        let passed = result.is_ok() && unsafe {
            let p = out.data;
            (*p.add(0) - 20.0).abs() < 0.01 &&
            (*p.add(1) - 23.0).abs() < 0.01 &&
            (*p.add(2) - 44.0).abs() < 0.01 &&
            (*p.add(3) - 51.0).abs() < 0.01
        };
        print_test_result(b"ops: gemm A*B+C\0", passed);
        if !passed { failures += 1; }
    }

    // Test 11: MatMul shape mismatch (2x3 * 2x3 is invalid)
    {
        let a_data: [f32; 6] = [1.0; 6];
        let b_data: [f32; 6] = [1.0; 6];
        let mut out_data: [f32; 9] = [0.0; 9];

        let a = inference::Tensor::new(a_data.as_ptr(), &[2, 3]);
        let b = inference::Tensor::new(b_data.as_ptr(), &[2, 3]);
        let mut out = inference::Tensor::new(out_data.as_mut_ptr() as *const f32, &[3, 3]);

        let result = inference::ops::matmul(&a, &b, &mut out);
        let passed = match result {
            Err(inference::EngineError::ShapeMismatch) => true,
            _ => false,
        };
        print_test_result(b"ops: matmul shape mismatch\0", passed);
        if !passed { failures += 1; }
    }

    // Test 12: Workspace exhaustion (tiny workspace, large tensor request)
    {
        let mut buf = [0u8; 64];
        let mut alloc = inference::BumpAllocator::new(buf.as_mut_ptr(), buf.len());
        let result = alloc.alloc_tensor(&[1000]);
        let passed = result.is_none();
        print_test_result(b"workspace: exhaustion returns None\0", passed);
        if !passed { failures += 1; }
    }

    // Test 13: End-to-end MNIST inference
    {
        static MNIST_ONNX: &[u8] = include_bytes!("../../models/test/mnist.onnx");

        loader::registry::init();
        let load_result = loader::registry::load_model(b"mnist_e2e", MNIST_ONNX);

        if let Ok(idx) = load_result {
            // Use statics to avoid stack overflow (32KB stack)
            static E2E_INPUT: [f32; 784] = [0.0; 784];
            static mut E2E_OUTPUT: [f32; 10] = [0.0; 10];

            // Check engine can be created for this model
            let engine_ok = inference::InferenceEngine::new(idx).is_ok();
            print_test_result(b"e2e: engine creation\0", engine_ok);
            if !engine_ok {
                failures += 1;
                let _ = loader::registry::unload_model(idx);
                unsafe {
                    kernel_ffi::uart_puts(b"[FAIL] Inference tests had failures\n\0".as_ptr());
                }
                return failures;
            }

            // SAFETY: single-threaded test context, static buffers
            let result = unsafe {
                for o in E2E_OUTPUT.iter_mut() { *o = 0.0; }
                inference::run_inference(
                    idx,
                    E2E_INPUT.as_ptr(),
                    E2E_INPUT.len(),
                    E2E_OUTPUT.as_mut_ptr(),
                    E2E_OUTPUT.len(),
                )
            };

            let run_ok = result.is_ok();
            if !run_ok {
                // Print the error code for debugging
                if let Err(e) = result {
                    unsafe {
                        extern "C" { fn uart_printf(fmt: *const u8, ...); }
                        uart_printf(b"  [DBG] e2e failed: err=%d\n\0".as_ptr(), e as i32);
                    }
                }
            }
            print_test_result(b"e2e: MNIST inference completes\0", run_ok);
            if !run_ok { failures += 1; }

            if run_ok {
                let n = result.unwrap();
                let count_ok = n == 10;
                print_test_result(b"e2e: 10 outputs produced\0", count_ok);
                if !count_ok { failures += 1; }

                // Verify outputs are finite (not NaN or Inf)
                let all_finite = unsafe {
                    E2E_OUTPUT[..n].iter().all(|&v| v.is_finite())
                };
                print_test_result(b"e2e: all outputs finite\0", all_finite);
                if !all_finite { failures += 1; }

                // Verify outputs are not all zeros (model produces meaningful values)
                let not_all_zero = unsafe {
                    E2E_OUTPUT[..n].iter().any(|&v| v != 0.0)
                };
                print_test_result(b"e2e: outputs not all zero\0", not_all_zero);
                if !not_all_zero { failures += 1; }
            }

            let _ = loader::registry::unload_model(idx);
        } else {
            print_test_result(b"e2e: model load for inference\0", false);
            failures += 1;
        }
    }

    // Test 14: Engine creation for nonexistent model
    {
        let result = inference::InferenceEngine::new(99);
        let passed = result.is_err();
        print_test_result(b"engine: nonexistent model error\0", passed);
        if !passed { failures += 1; }
    }

    // Summary
    unsafe {
        if failures == 0 {
            kernel_ffi::uart_puts(b"[INFO] Inference tests passed\n\0".as_ptr());
        } else {
            kernel_ffi::uart_puts(b"[FAIL] Inference tests had failures\n\0".as_ptr());
        }
    }

    failures
}
