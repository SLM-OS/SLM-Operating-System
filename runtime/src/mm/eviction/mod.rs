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
pub mod generated;
pub mod blob;

// Classical policies (M3). Ports of the sibling `slm-os-page-sim`
// reference crate + Python ARC. Decisions match the Python simulator
// candidate-for-candidate (enforced by the parity tests in
// `lib::rust_eviction_run_tests`).
pub mod lru;
pub mod lfu;
pub mod slm_heuristic;
pub mod arc;

// ML policies (M4). Feature extraction + thin wrappers around the
// generated XGBoost / int8-MLP predict functions.
pub mod features;
pub mod xgboost;
pub mod mlp;

// Adaptive ensemble (M5). Weighted expert selection with online
// updates. `ml_only` is the recommended runtime configuration.
pub mod cacheus;
pub mod tracker;

pub use policy::{BlockFeatures, BlockMeta, EvictionPolicy, PoolType,
    TrajectoryEntry, MAX_EXPERTS};
pub use blob::{
    BlobError, BlobHeader, BlobKind, ParsedBlob, BLOB_MAGIC,
    BLOB_VERSION_V1, EVICTION_FEATURE_SCHEMA_V1, HEADER_LEN,
    checksum32, parse_blob,
};
pub use registry::{
    get_eviction_policy_name, get_eviction_policy_name_for_pool,
    policy_counters, reset_to_default, score,
    notify_eviction, select_victim, set_eviction_policy,
    set_eviction_policy_for_pool,
    update_feedback, with_active_policy, with_active_policy_for_pool,
    FirstCandidatePolicy, PolicyCounters,
};
pub use lru::LruPolicy;
pub use lfu::LfuPolicy;
pub use slm_heuristic::SlmHeuristicPolicy;
pub use arc::{ARCPolicy, ARC_DEFAULT_GHOST_SIZE};
pub use features::{extract_features, AI_HORIZON_NS, FEATURE_NAMES};
pub use xgboost::XGBoostPolicy;
pub use mlp::MlpPolicy;
pub use cacheus::{CacheusSelector, CACHEUS_DEFAULT_LR, CACHEUS_DEFAULT_WINDOW};
pub use tracker::{
    ContentKey, EvictedContentTracker,
    EVICTION_FEEDBACK_WINDOW_NS, EVICTION_TRACKER_CAPACITY,
    probe_and_report_fault, drain_and_report_good,
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
