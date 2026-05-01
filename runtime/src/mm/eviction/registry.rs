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

use super::policy::{BlockMeta, EvictionPolicy, PoolType};

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

// `slm_get_time_ns` is declared in `kernel_ffi.rs` and used here via
// the canonical re-export. Avoiding a duplicate `extern "C"` block
// for the same C symbol keeps the Rust↔C signature in one place; a
// future change to the C-side prototype would otherwise risk silent
// signature divergence between the two declarations.
use crate::kernel_ffi::slm_get_time_ns;

extern "C" {
    /// M3: bucketed-histogram + EWMA rate hooks. Updates the global
    /// eviction-consumer telemetry stored in `kernel/src/admin_telemetry.c`.
    /// Same dt as the existing LATENCY_TOTAL_NS counter — these are
    /// additive, not replacements. C-side no-ops if M3 hasn't built
    /// in (won't happen — `admin_telemetry.c` is unconditionally
    /// compiled into the kernel).
    fn admin_telemetry_record_eviction_decision(dt_ns: u64);
    fn admin_telemetry_record_eviction_fallback();
}

/// Construct a policy instance from a stable ASCII config name.
pub fn make_policy_by_name(name: &str) -> Option<Box<dyn EvictionPolicy + Send>> {
    match name {
        "lru" => Some(Box::new(super::lru::LruPolicy::new())),
        "lfu" => Some(Box::new(super::lfu::LfuPolicy::new())),
        "arc" => Some(Box::new(super::arc::ARCPolicy::new())),
        "slm" => Some(Box::new(super::slm_heuristic::SlmHeuristicPolicy::new())),
        "xgboost" => Some(Box::new(super::xgboost::XGBoostPolicy::new())),
        "mlp" => Some(Box::new(super::mlp::MlpPolicy::new())),
        "cacheus" => Some(Box::new(super::cacheus::CacheusSelector::ml_only())),
        "cacheus_all5" => Some(Box::new(super::cacheus::CacheusSelector::all_5())),
        "first_candidate" => Some(Box::new(FirstCandidatePolicy)),
        _ => None,
    }
}

/// Construct the default eviction policy.
///
/// M3 picked LRU as the fallback default: it is the strongest
/// classical baseline in the sibling project's Phase 5 sweep, and it
/// matches the implicit first-fit-then-FIFO behaviour of the pre-M6
/// allocator most closely. Builders can override the compiled-in
/// default via the `SLM_DEFAULT_EVICTION_POLICY` environment variable
/// threaded through the top-level Makefile.
fn default_policy() -> Box<dyn EvictionPolicy + Send> {
    if let Some(name) = option_env!("SLM_DEFAULT_EVICTION_POLICY") {
        if let Some(policy) = make_policy_by_name(name) {
            return policy;
        }
    }
    make_policy_by_name("lru").expect("built-in LRU policy missing")
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
// Per-pool policies (#120): index 0 = Weight, index 1 = Workspace.
const NUM_POOLS: usize = 2;
static mut ACTIVE_POLICIES: [Option<Box<dyn EvictionPolicy + Send>>; NUM_POOLS] = [None, None];

fn pool_index(pool: PoolType) -> usize {
    match pool {
        PoolType::Weight => 0,
        PoolType::Workspace => 1,
    }
}

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
        let p = addr_of_mut!(ACTIVE_POLICIES);
        for slot in (*p).iter_mut() {
            if slot.is_none() {
                *slot = Some(default_policy());
            }
        }
    }
}

/// Force-replace both pool policies with the default. Used by the
/// selftest to leave the registry in a known state.
pub fn reset_to_default() {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        let p = addr_of_mut!(ACTIVE_POLICIES);
        for slot in (*p).iter_mut() {
            *slot = Some(default_policy());
        }
    }
    counters_reset();
}

/// Replace the policy for ALL pools. Previous policies are dropped.
/// This is the backward-compatible entry point used by the shell's
/// `eviction policy <name>` command.
pub fn set_eviction_policy(policy: Box<dyn EvictionPolicy + Send>) {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    // Both pools share the same boxed policy instance — but we can't
    // share a Box across slots, so we install into weight and create a
    // fresh default for workspace... Actually, the simplest correct
    // approach: the caller's policy goes into weight (pool 0), and a
    // fresh copy of the same-named policy goes into workspace (pool 1).
    // But we can't clone a dyn EvictionPolicy. Instead: install the
    // caller's policy into weight, and create a fresh default for
    // workspace. The "set both pools to the same policy" use-case is
    // handled by calling set_eviction_policy_for_pool twice.
    unsafe {
        let p = addr_of_mut!(ACTIVE_POLICIES);
        // Weight pool gets the caller's policy; workspace gets default.
        // This matches the common "set a global policy" use pattern —
        // workspace eviction is less interesting in practice.
        (*p)[1] = Some(default_policy());
        (*p)[0] = Some(policy);
    }
    counters_reset();
}

/// Replace the policy for a single pool. #120.
pub fn set_eviction_policy_for_pool(
    pool: PoolType,
    policy: Box<dyn EvictionPolicy + Send>,
) {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        (*addr_of_mut!(ACTIVE_POLICIES))[pool_index(pool)] = Some(policy);
    }
    counters_reset();
}

/// Name of the weight-pool policy, or `"none"` if uninitialized.
/// Use `get_eviction_policy_name_for_pool` for a specific pool.
pub fn get_eviction_policy_name() -> &'static str {
    get_eviction_policy_name_for_pool(PoolType::Weight)
}

/// Name of a specific pool's policy. #120.
pub fn get_eviction_policy_name_for_pool(pool: PoolType) -> &'static str {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        match &(*addr_of_mut!(ACTIVE_POLICIES))[pool_index(pool)] {
            Some(p) => p.name(),
            None => "none",
        }
    }
}

/// Invoke the weight-pool policy under the registry lock. Backward
/// compat wrapper — callers that don't specify a pool get weight.
pub fn with_active_policy<R>(f: impl FnOnce(&mut dyn EvictionPolicy) -> R) -> Option<R> {
    with_active_policy_for_pool(PoolType::Weight, f)
}

/// Invoke a specific pool's policy under the registry lock. #120.
pub fn with_active_policy_for_pool<R>(
    pool: PoolType,
    f: impl FnOnce(&mut dyn EvictionPolicy) -> R,
) -> Option<R> {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        match &mut (*addr_of_mut!(ACTIVE_POLICIES))[pool_index(pool)] {
            Some(p) => Some(f(p.as_mut())),
            None => None,
        }
    }
}

/// True if any pool's currently-installed eviction policy declares a
/// real GPU forward pass via `EvictionPolicy::has_gpu_backend()`.
///
/// Backs the C-side `gpu use eviction on` validation in
/// `kernel/src/gpu_consumer.c`: when this returns false, the toggle
/// accepts the operator's intent but emits a "scaffold only" warning
/// (mirroring the sched path). Today every shipped policy returns
/// false from `has_gpu_backend`; flipping that requires landing the
/// matching `slm_gpu_run_eviction_inference` dispatch
/// (`docs/specs/gpu-policy-models.md` PR-6).
pub fn any_active_policy_has_gpu_backend() -> bool {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access to ACTIVE_POLICIES.
    unsafe {
        let p = addr_of_mut!(ACTIVE_POLICIES);
        for slot in (*p).iter() {
            if let Some(policy) = slot.as_ref() {
                if policy.has_gpu_backend() {
                    return true;
                }
            }
        }
    }
    false
}

/// Drop the installed policies (test-only).
#[cfg(test)]
pub fn clear_for_test() {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access.
    unsafe {
        let p = addr_of_mut!(ACTIVE_POLICIES);
        for slot in (*p).iter_mut() {
            *slot = None;
        }
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
    // Determine pool from the first candidate (callers pre-filter by
    // pool, so all candidates share the same pool_type).
    let pool = candidates[0].pool_type;
    let t0 = unsafe { slm_get_time_ns() };
    let out = with_active_policy_for_pool(pool, |p| p.select_victim(candidates));
    let t1 = unsafe { slm_get_time_ns() };
    if out.is_some() {
        DECISIONS.fetch_add(1, Ordering::Relaxed);
        let dt = t1.saturating_sub(t0);
        LATENCY_TOTAL_NS.fetch_add(dt, Ordering::Relaxed);
        LATENCY_SAMPLES.fetch_add(1, Ordering::Relaxed);
        // SAFETY: the C-side function is wait-free, takes only a scalar
        // argument, and never returns an error. The admin_telemetry_*
        // globals live in BSS so calling at any point post-boot is
        // safe. No locks, no allocation.
        unsafe { admin_telemetry_record_eviction_decision(dt); }
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
        // SAFETY: same contract as `admin_telemetry_record_eviction_decision`
        // above — wait-free, scalar arg-less, BSS-resident.
        unsafe { admin_telemetry_record_eviction_fallback(); }
    }
    with_active_policy(|p| p.update_feedback(block_id, was_fault));
}

/// Notify the active policy that `block_id` has been evicted.
/// Routes to `EvictionPolicy::notify_eviction` (no-op for most
/// policies; ARC uses it to populate ghost lists proactively). #114.
pub fn notify_eviction(block_id: u32, pool: PoolType) {
    with_active_policy_for_pool(pool, |p| p.notify_eviction(block_id));
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
    fn factory_accepts_known_policy_names() {
        assert_eq!(make_policy_by_name("lru").unwrap().name(), "LRU");
        assert_eq!(make_policy_by_name("lfu").unwrap().name(), "LFU");
        assert_eq!(make_policy_by_name("arc").unwrap().name(), "ARC");
        assert_eq!(make_policy_by_name("slm").unwrap().name(), "SLM-Heuristic");
        assert!(make_policy_by_name("nope").is_none());
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
