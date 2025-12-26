//! Component System Module
//!
//! Provides component loading, lifecycle management, and registration.
//!
//! Components are self-contained units of functionality that can be:
//! - Loaded and unloaded at runtime
//! - Hot-swapped without system restart
//! - Interconnected via message passing

mod state;
mod registry;
mod manifest;

pub use state::{ComponentState, ComponentInfo, ComponentType, Priority};
pub use registry::MAX_COMPONENTS;
pub use manifest::parse_manifest;

use core::ffi::c_char;

/// Maximum length of component name
pub const MAX_NAME_LEN: usize = 32;

/// Maximum length of component description
pub const MAX_DESC_LEN: usize = 64;

/// Maximum length of component version string
pub const MAX_VERSION_LEN: usize = 16;

// =============================================================================
// FFI Entry Points
// =============================================================================

/// Initialize the component system.
///
/// Called by C kernel during boot.
#[no_mangle]
pub extern "C" fn component_system_init() -> i32 {
    registry::init();
    0 // Success
}

/// Get the number of loaded components.
#[no_mangle]
pub extern "C" fn component_count() -> u32 {
    registry::count() as u32
}

/// Get component info by index.
///
/// Returns 0 on success, -1 on error.
/// Fills in the provided ComponentInfo struct.
#[no_mangle]
pub extern "C" fn component_get_info(
    index: u32,
    info: *mut ComponentInfo,
) -> i32 {
    if info.is_null() {
        return -1;
    }

    match registry::get_by_index(index as usize) {
        Some(component_info) => {
            // SAFETY: info is non-null and we're writing valid data
            unsafe {
                core::ptr::write(info, component_info.clone());
            }
            0
        }
        None => -1,
    }
}

/// Find a component by name.
///
/// Returns component index, or -1 if not found.
#[no_mangle]
pub extern "C" fn component_find(name: *const c_char) -> i32 {
    if name.is_null() {
        return -1;
    }

    // Convert C string to Rust slice
    let name_str = unsafe {
        let mut len = 0;
        let mut p = name;
        while *p != 0 {
            len += 1;
            p = p.add(1);
            if len >= MAX_NAME_LEN {
                break;
            }
        }
        core::slice::from_raw_parts(name as *const u8, len)
    };

    match registry::find_by_name(name_str) {
        Some(idx) => idx as i32,
        None => -1,
    }
}

/// Register a component.
///
/// @param name: Component name (null-terminated)
/// @param version: Version string (null-terminated)
/// @param component_type: 0=service, 1=driver, 2=application
/// @param priority: 0-7 priority level
///
/// Returns component index on success, -1 on error.
#[no_mangle]
pub extern "C" fn component_register(
    name: *const c_char,
    version: *const c_char,
    component_type: u8,
    priority: u8,
) -> i32 {
    if name.is_null() || version.is_null() {
        return -1;
    }

    // Parse component type
    let ctype = match component_type {
        0 => ComponentType::Service,
        1 => ComponentType::Driver,
        2 => ComponentType::Application,
        _ => return -1,
    };

    // Parse priority
    let prio = Priority::from_u8(priority);

    // Convert C strings
    let name_bytes = unsafe { c_str_to_bytes(name, MAX_NAME_LEN) };
    let version_bytes = unsafe { c_str_to_bytes(version, MAX_VERSION_LEN) };

    // Create component info
    let mut info = ComponentInfo::new();
    info.set_name(name_bytes);
    info.set_version(version_bytes);
    info.component_type = ctype;
    info.priority = prio;
    info.state = ComponentState::Loaded;

    // Register
    match registry::register(info) {
        Ok(idx) => idx as i32,
        Err(_) => -1,
    }
}

/// Set component state.
///
/// @param index: Component index
/// @param state: New state (0=Loaded, 1=Initializing, 2=Running, etc.)
///
/// Returns 0 on success, -1 on error.
#[no_mangle]
pub extern "C" fn component_set_state(index: u32, state: u8) -> i32 {
    let new_state = match state {
        0 => ComponentState::Loaded,
        1 => ComponentState::Initializing,
        2 => ComponentState::Running,
        3 => ComponentState::Suspended,
        4 => ComponentState::Updating,
        5 => ComponentState::Terminating,
        6 => ComponentState::Unloaded,
        _ => return -1,
    };

    match registry::set_state(index as usize, new_state) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

/// Unregister a component.
///
/// Returns 0 on success, -1 on error.
#[no_mangle]
pub extern "C" fn component_unregister(index: u32) -> i32 {
    match registry::unregister(index as usize) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

// =============================================================================
// Helper Functions
// =============================================================================

/// Convert a C string to a byte slice.
///
/// # Safety
/// - `s` must be a valid, null-terminated C string
/// - The returned slice is valid only as long as `s` is valid
unsafe fn c_str_to_bytes(s: *const c_char, max_len: usize) -> &'static [u8] {
    let mut len = 0;
    let mut p = s;
    while *p != 0 {
        len += 1;
        p = p.add(1);
        if len >= max_len {
            break;
        }
    }
    core::slice::from_raw_parts(s as *const u8, len)
}
