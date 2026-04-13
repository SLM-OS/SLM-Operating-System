//! Pluggable eviction-policy layer for the `ModelAllocator`.
//!
//! This module is a port target for the sibling `slm-os-page-sim`
//! reference crate. M1 ships only the trait, metadata snapshot, and
//! registry. M3 ports the four classical policies (LRU, LFU, ARC,
//! SLM-Heuristic). M4 pulls in the trained XGBoost + int8 MLP. M5
//! lands the CACHEUS adaptive ensemble.
//!
//! Gated on the `ai_eviction` cargo feature so the baseline kernel
//! build has no additional code or memory cost.

pub mod policy;
pub mod registry;

pub use policy::{BlockFeatures, BlockMeta, EvictionPolicy, PoolType};
pub use registry::{
    get_eviction_policy_name, reset_to_default, score, select_victim,
    set_eviction_policy, update_feedback, with_active_policy,
};

/// Initialise the eviction subsystem.
///
/// Installs the default placeholder policy (replaced in M3 with LRU).
/// Safe to call multiple times; subsequent calls are no-ops unless a
/// policy has been explicitly cleared. Must be called after the Rust
/// heap is up (the default policy is boxed).
pub fn init() {
    registry::init_default();
}
