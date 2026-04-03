//! Logging infrastructure for SLM-OS Rust runtime.
//!
//! This module provides logging functionality via UART FFI to the kernel.
//! Since we are in `no_std` environment, formatting is limited to static
//! strings and simple numeric values.
//!
//! # Usage
//!
//! ```ignore
//! use crate::log::{log_info, log_warn, log_error, log_debug};
//!
//! log_info(b"System initialized\0");
//! log_warn(b"Low memory condition\0");
//! log_error(b"Failed to allocate\0");
//! log_debug(b"Entering function\0");
//! ```
//!
//! For logging with numeric values:
//! ```ignore
//! log_info_val(b"Allocated pages: \0", 42);
//! log_hex(b"Address: \0", 0xDEADBEEF);
//! ```

use core::ffi::c_char;

// =============================================================================
// Log Levels
// =============================================================================

/// Log level for filtering messages.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum LogLevel {
    /// Debug messages (most verbose)
    Debug = 0,
    /// Informational messages
    Info = 1,
    /// Warning messages
    Warn = 2,
    /// Error messages
    Error = 3,
    /// No logging (silent)
    Off = 4,
}

/// Global log level filter. Messages below this level are suppressed.
/// Uses atomic storage for thread-safe access from any core.
static LOG_LEVEL: core::sync::atomic::AtomicU8 =
    core::sync::atomic::AtomicU8::new(LogLevel::Info as u8);

/// Set the global log level.
pub fn set_log_level(level: LogLevel) {
    LOG_LEVEL.store(level as u8, core::sync::atomic::Ordering::Release);
}

/// Get the current log level.
pub fn get_log_level() -> LogLevel {
    match LOG_LEVEL.load(core::sync::atomic::Ordering::Relaxed) {
        0 => LogLevel::Debug,
        1 => LogLevel::Info,
        2 => LogLevel::Warn,
        3 => LogLevel::Error,
        _ => LogLevel::Off,
    }
}

// =============================================================================
// FFI to Kernel UART
// =============================================================================

extern "C" {
    fn slm_print(s: *const c_char);
    fn uart_puts(s: *const u8);
}

/// Print a null-terminated string to UART.
///
/// # Safety
/// The string must be null-terminated.
#[inline]
fn uart_print(s: &[u8]) {
    if !s.is_empty() && s[s.len() - 1] == 0 {
        // SAFETY: String is null-terminated as verified above
        unsafe { uart_puts(s.as_ptr()) };
    }
}

// =============================================================================
// Logging Functions
// =============================================================================

/// Log a debug message.
///
/// The message must be null-terminated.
#[inline]
pub fn log_debug(msg: &[u8]) {
    if get_log_level() <= LogLevel::Debug {
        uart_print(b"[DEBUG] \0");
        uart_print(msg);
    }
}

/// Log an info message.
///
/// The message must be null-terminated.
#[inline]
pub fn log_info(msg: &[u8]) {
    if get_log_level() <= LogLevel::Info {
        uart_print(b"[INFO] \0");
        uart_print(msg);
    }
}

/// Log a warning message.
///
/// The message must be null-terminated.
#[inline]
pub fn log_warn(msg: &[u8]) {
    if get_log_level() <= LogLevel::Warn {
        uart_print(b"[WARN] \0");
        uart_print(msg);
    }
}

/// Log an error message.
///
/// The message must be null-terminated.
#[inline]
pub fn log_error(msg: &[u8]) {
    if get_log_level() <= LogLevel::Error {
        uart_print(b"[ERROR] \0");
        uart_print(msg);
    }
}

// =============================================================================
// Logging with Values
// =============================================================================

/// Buffer for numeric formatting.
/// Maximum needed: 20 digits for u64 + null terminator.
const NUM_BUF_SIZE: usize = 24;

/// Format an unsigned integer to a buffer.
///
/// Returns the starting index in the buffer where the number begins.
fn format_u64(value: u64, buf: &mut [u8; NUM_BUF_SIZE]) -> usize {
    // Fill buffer from the end
    let mut idx = NUM_BUF_SIZE - 1;
    buf[idx] = 0; // Null terminator
    idx -= 1;

    if value == 0 {
        buf[idx] = b'0';
        return idx;
    }

    let mut v = value;
    while v > 0 && idx > 0 {
        buf[idx] = b'0' + (v % 10) as u8;
        v /= 10;
        idx -= 1;
    }

    idx + 1
}

/// Format an unsigned integer as hexadecimal to a buffer.
///
/// Returns the starting index in the buffer where the number begins.
fn format_hex(value: u64, buf: &mut [u8; NUM_BUF_SIZE]) -> usize {
    const HEX_CHARS: &[u8; 16] = b"0123456789abcdef";

    // Fill buffer from the end
    let mut idx = NUM_BUF_SIZE - 1;
    buf[idx] = 0; // Null terminator
    idx -= 1;

    if value == 0 {
        buf[idx] = b'0';
        idx -= 1;
        buf[idx] = b'x';
        idx -= 1;
        buf[idx] = b'0';
        return idx;
    }

    let mut v = value;
    while v > 0 && idx > 2 {
        buf[idx] = HEX_CHARS[(v & 0xF) as usize];
        v >>= 4;
        idx -= 1;
    }

    // Add 0x prefix
    buf[idx] = b'x';
    idx -= 1;
    buf[idx] = b'0';

    idx
}

/// Log an info message with an unsigned integer value.
///
/// The prefix must be null-terminated.
pub fn log_info_val(prefix: &[u8], value: u64) {
    if get_log_level() <= LogLevel::Info {
        uart_print(b"[INFO] \0");
        // Print prefix without null terminator
        if !prefix.is_empty() && prefix[prefix.len() - 1] == 0 {
            unsafe { uart_puts(prefix.as_ptr()) };
        }
        // Format and print value
        let mut buf = [0u8; NUM_BUF_SIZE];
        let start = format_u64(value, &mut buf);
        unsafe { uart_puts(buf[start..].as_ptr()) };
        uart_print(b"\n\0");
    }
}

/// Log an info message with a hexadecimal value.
///
/// The prefix must be null-terminated.
pub fn log_hex(prefix: &[u8], value: u64) {
    if get_log_level() <= LogLevel::Info {
        uart_print(b"[INFO] \0");
        if !prefix.is_empty() && prefix[prefix.len() - 1] == 0 {
            unsafe { uart_puts(prefix.as_ptr()) };
        }
        let mut buf = [0u8; NUM_BUF_SIZE];
        let start = format_hex(value, &mut buf);
        unsafe { uart_puts(buf[start..].as_ptr()) };
        uart_print(b"\n\0");
    }
}

/// Log a debug message with an unsigned integer value.
pub fn log_debug_val(prefix: &[u8], value: u64) {
    if get_log_level() <= LogLevel::Debug {
        uart_print(b"[DEBUG] \0");
        if !prefix.is_empty() && prefix[prefix.len() - 1] == 0 {
            unsafe { uart_puts(prefix.as_ptr()) };
        }
        let mut buf = [0u8; NUM_BUF_SIZE];
        let start = format_u64(value, &mut buf);
        unsafe { uart_puts(buf[start..].as_ptr()) };
        uart_print(b"\n\0");
    }
}

/// Log an error message with an unsigned integer value.
pub fn log_error_val(prefix: &[u8], value: u64) {
    if get_log_level() <= LogLevel::Error {
        uart_print(b"[ERROR] \0");
        if !prefix.is_empty() && prefix[prefix.len() - 1] == 0 {
            unsafe { uart_puts(prefix.as_ptr()) };
        }
        let mut buf = [0u8; NUM_BUF_SIZE];
        let start = format_u64(value, &mut buf);
        unsafe { uart_puts(buf[start..].as_ptr()) };
        uart_print(b"\n\0");
    }
}

/// Log a warning message with an unsigned integer value.
pub fn log_warn_val(prefix: &[u8], value: u64) {
    if get_log_level() <= LogLevel::Warn {
        uart_print(b"[WARN] \0");
        if !prefix.is_empty() && prefix[prefix.len() - 1] == 0 {
            unsafe { uart_puts(prefix.as_ptr()) };
        }
        let mut buf = [0u8; NUM_BUF_SIZE];
        let start = format_u64(value, &mut buf);
        unsafe { uart_puts(buf[start..].as_ptr()) };
        uart_print(b"\n\0");
    }
}

// =============================================================================
// FFI Exports
// =============================================================================

/// Set log level from C code.
///
/// Level values: 0=Debug, 1=Info, 2=Warn, 3=Error, 4=Off
#[no_mangle]
pub extern "C" fn rust_log_set_level(level: u8) {
    let log_level = match level {
        0 => LogLevel::Debug,
        1 => LogLevel::Info,
        2 => LogLevel::Warn,
        3 => LogLevel::Error,
        _ => LogLevel::Off,
    };
    set_log_level(log_level);
}

/// Get current log level from C code.
#[no_mangle]
pub extern "C" fn rust_log_get_level() -> u8 {
    get_log_level() as u8
}

/// Log an info message from C code.
///
/// # Safety
/// The message must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn rust_log_info(msg: *const c_char) {
    if get_log_level() <= LogLevel::Info {
        uart_print(b"[INFO] \0");
        slm_print(msg);
    }
}

/// Log an error message from C code.
///
/// # Safety
/// The message must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn rust_log_error(msg: *const c_char) {
    if get_log_level() <= LogLevel::Error {
        uart_print(b"[ERROR] \0");
        slm_print(msg);
    }
}

/// Log a warning message from C code.
///
/// # Safety
/// The message must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn rust_log_warn(msg: *const c_char) {
    if get_log_level() <= LogLevel::Warn {
        uart_print(b"[WARN] \0");
        slm_print(msg);
    }
}

/// Log a debug message from C code.
///
/// # Safety
/// The message must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn rust_log_debug(msg: *const c_char) {
    if get_log_level() <= LogLevel::Debug {
        uart_print(b"[DEBUG] \0");
        slm_print(msg);
    }
}

// =============================================================================
// Tests
// =============================================================================

/// Run logging tests.
///
/// Returns number of failures.
#[no_mangle]
pub extern "C" fn rust_log_test() -> i32 {
    uart_print(b"[TEST] Running logging tests...\n\0");

    // Test 1: Basic logging at each level
    log_debug(b"Debug message test\n\0");
    log_info(b"Info message test\n\0");
    log_warn(b"Warning message test\n\0");
    log_error(b"Error message test\n\0");

    // Test 2: Logging with values
    log_info_val(b"Test value: \0", 12345);
    log_hex(b"Test hex: \0", 0xDEADBEEF);

    // Test 3: Log level filtering
    let original_level = get_log_level();

    set_log_level(LogLevel::Error);
    uart_print(b"  [INFO] Level set to Error - info should be suppressed:\n\0");
    log_info(b"This should NOT appear\n\0");
    log_error(b"This SHOULD appear\n\0");

    set_log_level(LogLevel::Off);
    uart_print(b"  [INFO] Level set to Off - nothing should appear:\n\0");
    log_error(b"This should NOT appear either\n\0");

    // Restore original level
    set_log_level(original_level);

    uart_print(b"[PASS] Logging tests complete (visual verification required)\n\0");
    0
}
