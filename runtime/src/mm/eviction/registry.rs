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
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use super::lru::LruPolicy;
use super::policy::{BlockMeta, EvictionPolicy};

// =============================================================================
// Per-policy counters (#115)
// =============================================================================
//
// Every `select_victim` call increments DECISIONS and accumulates the
// wall-clock latency. `update_feedback(_, true)` increments FALLBACKS.
// Counters are global to the installed policy — they reset on every
// policy swap (`set_eviction_policy` / `reset_to_default`) so the
// reading always reflects the lifetime of the *currently installed*
// policy, not of the registry itself.
//
// Relaxed ordering is sufficient: counters are observational, never
// consumed for control flow. Using relaxed avoids any extra barrier
// on the hot select_victim path.

static DECISIONS: AtomicU64 = AtomicU64::new(0);
static FALLBACKS: AtomicU64 = AtomicU64::new(0);
static LATENCY_TOTAL_NS: AtomicU64 = AtomicU64::new(0);
static LATENCY_SAMPLES: AtomicU64 = AtomicU64::new(0);

fn counters_reset() {
    DECISIONS.store(0, Ordering::Relaxed);
    FALLBACKS.store(0, Ordering::Relaxed);
    LATENCY_TOTAL_NS.store(0, Ordering::Relaxed);
    LATENCY_SAMPLES.store(0, Ordering::Relaxed);
}

/// Snapshot of the per-policy counters. All values are lifetime totals
/// for the currently-installed policy.
#[derive(Clone, Copy, Default)]
pub struct PolicyCounters {
    pub decisions: u64,
    pub fallbacks: u64,
    pub avg_latency_ns: u64,
}

/// Read a consistent snapshot of the per-policy counters.
pub fn policy_counters() -> PolicyCounters {
    let decisions = DECISIONS.load(Ordering::Relaxed);
    let fallbacks = FALLBACKS.load(Ordering::Relaxed);
    let total_ns = LATENCY_TOTAL_NS.load(Ordering::Relaxed);
    let samples = LATENCY_SAMPLES.load(Ordering::Relaxed);
    let avg = if samples == 0 { 0 } else { total_ns / samples };
    PolicyCounters { decisions, fallbacks, avg_latency_ns: avg }
}

extern "C" {
    /// ARM generic timer / x86 TSC monotonic nanoseconds. Used to time
    /// `select_victim` calls for the policy-latency counter.
    fn slm_get_time_ns() -> u64;
}

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
    // Counters are per-policy — clear after the swap so new decisions
    // count against the freshly-installed default.
    counters_reset();
}

/// Replace the active policy. Previous policy is dropped.
pub fn set_eviction_policy(policy: Box<dyn EvictionPolicy + Send>) {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        *addr_of_mut!(ACTIVE_POLICY) = Some(policy);
    }
    counters_reset();
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
///
/// Per-policy counters (#115): increments DECISIONS and accumulates
/// the select_victim wall-clock latency regardless of which concrete
/// policy is installed. Fallbacks are observed separately via
/// `update_feedback(_, true)`.
pub fn select_victim(candidates: &[BlockMeta]) -> Option<usize> {
    if candidates.is_empty() {
        return None;
    }
    // SAFETY: slm_get_time_ns is a kernel FFI — reads CNTPCT_EL0 /
    // TSC; always safe to call from any context.
    let t0 = unsafe { slm_get_time_ns() };
    let out = with_active_policy(|p| p.select_victim(candidates));
    let t1 = unsafe { slm_get_time_ns() };
    if out.is_some() {
        DECISIONS.fetch_add(1, Ordering::Relaxed);
        let dt = t1.saturating_sub(t0);
        LATENCY_TOTAL_NS.fetch_add(dt, Ordering::Relaxed);
        LATENCY_SAMPLES.fetch_add(1, Ordering::Relaxed);
    }
    out
}

/// Forward feedback to the active policy. No-op if the registry is
/// unset or the policy does not learn from feedback.
///
/// A `was_fault = true` callback indicates the active policy's victim
/// choice led to a re-fault — i.e. a fallback from the policy's
/// perspective. Bump the per-policy FALLBACKS counter here so
/// reporting does not need to crack open each concrete policy's
/// internal state.
pub fn update_feedback(block_id: u32, was_fault: bool) {
    if was_fault {
        FALLBACKS.fetch_add(1, Ordering::Relaxed);
    }
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
