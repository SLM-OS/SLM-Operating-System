//! `EvictionPolicy` trait and the metadata types it consumes.
//!
//! Mirrors `slm_os_integration::eviction_policy` and `::block` in the
//! sibling `slm-os-page-sim` reference crate so the classical policies
//! (M3) and CACHEUS (M5) can be ported with minimal renaming. Field
//! layouts and method signatures MUST match the sibling — parity tests
//! rely on identical decisions for identical inputs.

use alloc::vec;
use alloc::vec::Vec;

/// Which memory pool a block belongs to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum PoolType {
    /// Read-only model weights (shared across inferences).
    Weight = 0,
    /// Read-write inference workspace (per-task).
    Workspace = 1,
}

/// Per-block metadata snapshot passed to eviction policies.
///
/// A runtime-side `BlockSlot` is summarised into this struct at decision
/// time. Fields are copied by value so the policy does not hold
/// references into the allocator's locked state.
#[derive(Debug, Clone, Copy)]
pub struct BlockMeta {
    pub block_id: u32,
    pub pool_type: PoolType,
    pub model_id: u8,
    pub layer_idx: i16,
    pub last_access_time: u64,
    pub load_time: u64,
    pub access_count: u32,
    pub ref_count: u8,
    pub gpu_mapped: bool,
    pub is_dirty: bool,
    pub model_priority: u8,
}

/// Flat 27-feature vector consumed by the trained ML policies (M4).
///
/// Layout matches `FeatureConfig.feature_names` in the sibling simulator.
pub type BlockFeatures = [f32; 27];

/// Contract every eviction policy implements.
///
/// Callers pass a non-empty slice of candidates; the policy returns the
/// index of the block to evict. `score()` is used by the CACHEUS
/// ensemble: each expert scores every candidate, scores are combined by
/// weight, and the argmax wins.
pub trait EvictionPolicy {
    /// Pick which candidate to evict. Returns the index into `candidates`.
    /// Panics (in debug) if `candidates` is empty; callers must filter.
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize;

    /// Per-candidate scores in `[0, 1]` where higher means more evictable.
    /// Default impl sets the `select_victim` choice to 1.0 and others to 0.0.
    fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32> {
        let n = candidates.len();
        if n == 0 {
            return Vec::new();
        }
        let mut scores = vec![0.0_f32; n];
        let victim = self.select_victim(candidates);
        scores[victim] = 1.0;
        scores
    }

    /// Feedback about a previous eviction. `was_fault = true` means the
    /// evicted block was re-accessed (i.e. the eviction was bad).
    fn update_feedback(&mut self, _block_id: u32, _was_fault: bool) {}

    /// Reset any learned state.
    fn reset(&mut self) {}

    /// Stable human-readable name used by the registry and shell.
    fn name(&self) -> &'static str;

    /// Per-expert weights for ensemble policies (CACHEUS). Default
    /// impl returns `None` — atomic policies don't have experts.
    /// The shell's `eviction stats` command uses this to surface
    /// live weights without introducing `Any`-based downcasts.
    fn ensemble_weights(&self) -> Option<&[f32]> { None }

    /// Per-expert names (CACHEUS). Default `None`.
    fn ensemble_expert_names(&self) -> Option<alloc::vec::Vec<&'static str>> {
        None
    }
}
