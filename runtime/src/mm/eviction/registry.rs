//! Global eviction-policy registry.
//!
//! Holds the `ACTIVE_POLICY` that `ModelAllocator` consults when the
//! weight or workspace pool is full (wired up in M6). The registry is
//! protected by a dedicated spinlock so that policy swaps and
//! allocator-driven `select_victim` calls never race.
//!
//! Follows the `model_mem` SpinGuard RAII pattern (no `std::sync::Mutex`
//! available in `no_std`).
//!
//! ## Runtime contract
//!
//! - **Task context only.** The SpinGuard here does not disable IRQs,
//!   matching the `model_mem` allocator's long-standing contract.
//!   Calling from an interrupt handler can deadlock if the interrupted
//!   task already holds the lock.
//! - **Lock ordering.** The allocator's pool `LOCK` (in
//!   `mm::model_mem`) must be released before calling any public
//!   function in this module; `mm::model_mem::evict_and_retry` already
//!   does. The two locks are independent — CACHEUS's own heap
//!   allocations go through `LockedHeap`, a third domain — so no ABBA
//!   cycle is possible.
//! - **Memory ordering.** Policy installation and lookup use the
//!   compare-exchange's `Acquire` / `Release` semantics, which emit
//!   `DMB ISH` on ARM64. That guarantees cross-CPU visibility of the
//!   new `ACTIVE_POLICY` pointer after `set_eviction_policy` returns.

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicBool, Ordering};

use super::lru::LruPolicy;
use super::policy::{BlockMeta, EvictionPolicy};

/// Construct the default eviction policy.
///
/// M3 picked LRU: it is the strongest classical baseline in the
/// sibling project's Phase 5 sweep, and it matches the implicit
/// first-fit-then-FIFO behaviour of the pre-M6 allocator most
/// closely. Tests that need a deterministic trivial policy can
/// install [`FirstCandidatePolicy`] explicitly via
/// [`set_eviction_policy`].
fn default_policy() -> Box<dyn EvictionPolicy + Send> {
    Box::new(LruPolicy::new())
}

/// Deterministic policy that always picks index 0.
///
/// Kept public so tests can install it via [`set_eviction_policy`].
/// Not used as the default any more (see [`default_policy`]).
pub struct FirstCandidatePolicy;

impl EvictionPolicy for FirstCandidatePolicy {
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(!candidates.is_empty(), "select_victim on empty list");
        0
    }

    fn name(&self) -> &'static str {
        "FirstCandidate"
    }
}

static REGISTRY_LOCK: AtomicBool = AtomicBool::new(false);
// SAFETY: Only accessed while holding REGISTRY_LOCK.
static mut ACTIVE_POLICY: Option<Box<dyn EvictionPolicy + Send>> = None;

/// RAII spinlock guard for the registry.
struct SpinGuard;

impl SpinGuard {
    fn new() -> Self {
        while REGISTRY_LOCK
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        SpinGuard
    }
}

impl Drop for SpinGuard {
    fn drop(&mut self) {
        REGISTRY_LOCK.store(false, Ordering::Release);
    }
}

/// Install the default policy if none is set. Idempotent.
///
/// Called by `mm::eviction::init()` once the Rust heap is up. Safe to
/// call multiple times; the first call wins. Use `reset_to_default()`
/// to force a replacement.
pub fn init_default() {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        if (*addr_of_mut!(ACTIVE_POLICY)).is_none() {
            *addr_of_mut!(ACTIVE_POLICY) = Some(default_policy());
        }
    }
}

/// Force-replace the active policy with the default. Used by the
/// selftest to leave the registry in a known state.
pub fn reset_to_default() {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        *addr_of_mut!(ACTIVE_POLICY) = Some(default_policy());
    }
}

/// Replace the active policy. Previous policy is dropped.
pub fn set_eviction_policy(policy: Box<dyn EvictionPolicy + Send>) {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        *addr_of_mut!(ACTIVE_POLICY) = Some(policy);
    }
}

/// Name of the currently installed policy, or `"none"` if the registry
/// has not been initialised.
pub fn get_eviction_policy_name() -> &'static str {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        match &*addr_of_mut!(ACTIVE_POLICY) {
            Some(p) => p.name(),
            None => "none",
        }
    }
}

/// Invoke the active policy under the registry lock.
///
/// Returns `None` if no policy is installed; otherwise invokes `f` with
/// an exclusive reference to the policy. The allocator's pool lock
/// must NOT be held when calling this (the policy is free to allocate
/// via the global heap, which is reentrancy-free but still benefits
/// from the no-nested-locks discipline).
pub fn with_active_policy<R>(f: impl FnOnce(&mut dyn EvictionPolicy) -> R) -> Option<R> {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        match &mut *addr_of_mut!(ACTIVE_POLICY) {
            Some(p) => Some(f(p.as_mut())),
            None => None,
        }
    }
}

/// Drop the installed policy (test-only).
///
/// Useful when a test wants to verify the unset behaviour. Production
/// code should install a replacement via `set_eviction_policy` instead.
#[cfg(test)]
pub fn clear_for_test() {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        *addr_of_mut!(ACTIVE_POLICY) = None;
    }
}

/// Select a victim using the active policy.
///
/// Convenience helper for M6's allocator path. Returns `None` if no
/// policy is installed or the candidate list is empty.
pub fn select_victim(candidates: &[BlockMeta]) -> Option<usize> {
    if candidates.is_empty() {
        return None;
    }
    with_active_policy(|p| p.select_victim(candidates))
}

/// Forward feedback to the active policy. No-op if the registry is
/// unset or the policy does not learn from feedback.
pub fn update_feedback(block_id: u32, was_fault: bool) {
    with_active_policy(|p| p.update_feedback(block_id, was_fault));
}

/// Collect the scores the active policy assigns to a candidate list.
///
/// Returns an empty vector if no policy is installed.
pub fn score(candidates: &[BlockMeta]) -> Vec<f32> {
    with_active_policy(|p| p.score(candidates)).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;

    struct NamedPolicy(&'static str);
    impl EvictionPolicy for NamedPolicy {
        fn select_victim(&mut self, _: &[BlockMeta]) -> usize { 0 }
        fn name(&self) -> &'static str { self.0 }
    }

    #[test]
    fn default_is_lru() {
        init_default();
        assert_eq!(get_eviction_policy_name(), "LRU");
    }

    #[test]
    fn swap_replaces_policy() {
        init_default();
        set_eviction_policy(Box::new(NamedPolicy("TestPolicyA")));
        assert_eq!(get_eviction_policy_name(), "TestPolicyA");
        set_eviction_policy(Box::new(NamedPolicy("TestPolicyB")));
        assert_eq!(get_eviction_policy_name(), "TestPolicyB");
    }

    #[test]
    fn name_is_none_when_uninitialised() {
        clear_for_test();
        assert_eq!(get_eviction_policy_name().to_string(), "none");
        init_default();
    }
}
