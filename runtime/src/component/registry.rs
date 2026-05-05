//! Component Registry
//!
//! Manages the collection of loaded components.

use super::state::{ComponentInfo, ComponentState};
use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, Ordering};

/// Maximum number of components that can be loaded simultaneously.
pub const MAX_COMPONENTS: usize = 16;

/// A wrapper that allows UnsafeCell to be used in statics.
/// SAFETY: Access is protected by REGISTRY_LOCK spinlock.
#[repr(transparent)]
struct SyncWrapper<T>(UnsafeCell<T>);

// SAFETY: We use a spinlock (REGISTRY_LOCK) to synchronize all access
unsafe impl<T> Sync for SyncWrapper<T> {}

impl<T> SyncWrapper<T> {
    const fn new(value: T) -> Self {
        SyncWrapper(UnsafeCell::new(value))
    }

    /// Get a raw pointer to the inner value.
    fn get(&self) -> *mut T {
        self.0.get()
    }
}

/// Global component registry.
///
/// Uses static storage to avoid heap allocation.
/// Protected by a simple spinlock for thread safety.
static REGISTRY: SyncWrapper<Registry> = SyncWrapper::new(Registry::new());
static REGISTRY_LOCK: AtomicBool = AtomicBool::new(false);

/// Component registry structure.
struct Registry {
    /// Component slots
    components: [ComponentInfo; MAX_COMPONENTS],
    /// Number of active components
    count: usize,
    /// Initialized flag
    initialized: bool,
}

impl Registry {
    /// Create a new, empty registry.
    const fn new() -> Self {
        Registry {
            components: [ComponentInfo::new(); MAX_COMPONENTS],
            count: 0,
            initialized: false,
        }
    }
}

/// Acquire the registry lock.
fn lock() {
    while REGISTRY_LOCK.compare_exchange_weak(
        false,
        true,
        Ordering::Acquire,
        Ordering::Relaxed,
    ).is_err() {
        core::hint::spin_loop();
    }
}

/// Release the registry lock.
fn unlock() {
    REGISTRY_LOCK.store(false, Ordering::Release);
}

/// RAII guard for REGISTRY_LOCK. Used by paths with multiple early
/// returns where hand-rolled `lock()/unlock()` is fragile (an edit
/// that introduces a new early-return path easily forgets the
/// matching unlock). Other functions in this file still use the
/// explicit pair where the control flow is straight-line.
struct SpinGuard;

impl SpinGuard {
    fn new() -> Self {
        lock();
        SpinGuard
    }
}

impl Drop for SpinGuard {
    fn drop(&mut self) {
        unlock();
    }
}

/// Initialize the component registry.
pub fn init() {
    lock();
    // SAFETY: We hold the lock, SyncUnsafeCell guarantees no other access
    unsafe {
        let reg = &mut *REGISTRY.get();
        if !reg.initialized {
            for slot in reg.components.iter_mut() {
                *slot = ComponentInfo::new();
            }
            reg.count = 0;
            reg.initialized = true;
        }
    }
    unlock();
}

/// Get the number of active components.
pub fn count() -> usize {
    lock();
    // SAFETY: We hold the lock, SyncUnsafeCell guarantees no other access
    let c = unsafe { (*REGISTRY.get()).count };
    unlock();
    c
}

/// Register a new component.
///
/// Returns the slot index on success.
pub fn register(info: ComponentInfo) -> Result<usize, &'static str> {
    lock();
    // SAFETY: We hold the lock, SyncUnsafeCell guarantees no other access
    let result = unsafe {
        let reg = &mut *REGISTRY.get();
        // Find an empty slot
        let mut slot_idx = None;
        for (i, slot) in reg.components.iter().enumerate() {
            if !slot.is_active() {
                slot_idx = Some(i);
                break;
            }
        }

        match slot_idx {
            Some(idx) => {
                reg.components[idx] = info;
                reg.count += 1;
                Ok(idx)
            }
            None => Err("No free component slots"),
        }
    };
    unlock();
    result
}

/// Unregister a component by index.
pub fn unregister(index: usize) -> Result<(), &'static str> {
    lock();
    // SAFETY: We hold the lock, SyncUnsafeCell guarantees no other access
    let result = unsafe {
        let reg = &mut *REGISTRY.get();
        if index >= MAX_COMPONENTS {
            Err("Invalid component index")
        } else if !reg.components[index].is_active() {
            Err("Component slot not active")
        } else {
            reg.components[index] = ComponentInfo::new();
            if reg.count > 0 {
                reg.count -= 1;
            }
            Ok(())
        }
    };
    unlock();
    result
}

/// Get component info by index.
pub fn get_by_index(index: usize) -> Option<ComponentInfo> {
    lock();
    // SAFETY: We hold the lock, SyncUnsafeCell guarantees no other access
    let result = unsafe {
        let reg = &*REGISTRY.get();
        if index >= MAX_COMPONENTS {
            None
        } else if !reg.components[index].is_active() {
            None
        } else {
            Some(reg.components[index].clone())
        }
    };
    unlock();
    result
}

/// Find a component by name.
///
/// Returns the slot index if found. Uses RAII SpinGuard so the
/// early-return path can't leak the registry lock.
pub fn find_by_name(name: &[u8]) -> Option<usize> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        for (i, slot) in reg.components.iter().enumerate() {
            if slot.is_active() {
                // Compare names
                let slot_name_len = slot.name.iter()
                    .position(|&b| b == 0)
                    .unwrap_or(slot.name.len());
                let slot_name = &slot.name[..slot_name_len];

                if slot_name.len() == name.len() && slot_name == name {
                    return Some(i);
                }
            }
        }
        None
    }
}

/// Set component state.
pub fn set_state(index: usize, state: ComponentState) -> Result<(), &'static str> {
    lock();
    // SAFETY: We hold the lock, SyncUnsafeCell guarantees no other access
    let result = unsafe {
        let reg = &mut *REGISTRY.get();
        if index >= MAX_COMPONENTS {
            Err("Invalid component index")
        } else if !reg.components[index].is_active() {
            Err("Component slot not active")
        } else {
            reg.components[index].state = state;
            Ok(())
        }
    };
    unlock();
    result
}

