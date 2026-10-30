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
    /// Observed access pattern (matches the sibling `AccessPattern`
    /// IntEnum: 0=SEQUENTIAL, 1=RANDOM, 2=STRIDED, 3=BURST). Drives the
    /// `predicted_reuse_dist` feature heuristic in `features.rs`.
    pub access_pattern: u8,
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

    /// Whether this policy has a real GPU forward pass wired through
    /// the GA10B compute-dispatch path. Default `false` — atomic
    /// policies (LRU, LFU, ARC, …) and the CPU MLP / XGBoost
    /// implementations are FFMA-on-CPU only. The shell's
    /// `gpu use eviction on` toggle consults the active pool's
    /// policy via this hook (mirroring the sched path's
    /// `sched_policy_ops::has_gpu_backend` field): when no policy
    /// declares true, the toggle accepts with a "scaffold only"
    /// warning, recording operator intent without changing dispatch.
    /// Flipping a policy's return value to `true` requires the
    /// matching `slm_gpu_run_eviction_batch()` plumbing on the
    /// SLM-OS side — see `docs/design/gpu-policy-models.md` PR-6.
    fn has_gpu_backend(&self) -> bool { false }

    /// Phase 1 of a registry decision that may use the GPU (#1020).
    /// Called with `REGISTRY_LOCK` held. A policy that wants this
    /// decision scored on the GPU returns its per-candidate feature
    /// rows; the registry then drops the lock, runs one batched
    /// dispatch, re-takes the lock and calls
    /// [`Self::finish_gpu_select`] with the result. `None` (the
    /// default) means "decide on the CPU" and the registry calls
    /// `finish_gpu_select(candidates, None)` straight away, still under
    /// the same lock acquisition.
    fn gpu_batch_features(&mut self, _candidates: &[BlockMeta])
        -> Option<Vec<BlockFeatures>>
    {
        None
    }

    /// Phase 3 of a registry decision (#1020), called with
    /// `REGISTRY_LOCK` held. `gpu_scores` is `Some` with one score per
    /// candidate when the batched dispatch succeeded, `None` when it
    /// failed or was never attempted. Must not dispatch the GPU itself
    /// — that is the point of the split. The default ignores the
    /// scores and runs `select_victim`, which is correct for every
    /// policy without a GPU path.
    fn finish_gpu_select(&mut self, candidates: &[BlockMeta],
                         _gpu_scores: Option<&[f32]>) -> usize
    {
        self.select_victim(candidates)
    }

    /// Per-expert weights for ensemble policies (CACHEUS). Default
    /// impl returns `None` — atomic policies don't have experts.
    /// The shell's `eviction stats` command uses this to surface
    /// live weights without introducing `Any`-based downcasts.
    fn ensemble_weights(&self) -> Option<&[f32]> { None }

    /// Per-expert names (CACHEUS). Default `None`.
    fn ensemble_expert_names(&self) -> Option<alloc::vec::Vec<&'static str>> {
        None
    }

    /// Notify the policy that `block_id` has been evicted from the pool.
    /// Default no-op. ARC overrides to populate ghost lists proactively
    /// (#114).
    fn notify_eviction(&mut self, _block_id: u32) {}

    /// Time-series of ensemble weight snapshots (CACHEUS). One entry
    /// is pushed after every `update_feedback` that adjusted weights;
    /// the buffer is capped so the oldest entries are evicted. Default
    /// `None` — atomic policies have no weights to trace. See #111.
    fn ensemble_trajectory(&self) -> Option<&[TrajectoryEntry]> { None }

    /// Copy the trajectory into caller-owned storage in oldest-first
    /// order. Atomic policies have no trajectory and return 0.
    fn ensemble_trajectory_snapshot(&self, out: &mut [TrajectoryEntry]) -> usize {
        match self.ensemble_trajectory() {
            Some(slice) => {
                let n = slice.len().min(out.len());
                out[..n].copy_from_slice(&slice[..n]);
                n
            }
            None => 0,
        }
    }
}

/// One snapshot of ensemble weights at a point in time. Used by
/// CacheusSelector to record how the expert mixture adapts across
/// feedback events; surfaced by `eviction trajectory` in the shell.
///
/// Kept fixed-size so the snapshot structure is `Copy` and the FFI
/// can serialise it without allocating per-entry.
pub const MAX_EXPERTS: usize = 5;

#[derive(Debug, Clone, Copy)]
pub struct TrajectoryEntry {
    /// `slm_get_time_ns()` at the moment this snapshot was taken.
    pub timestamp_ns: u64,
    /// Number of expert weights populated in `weights`.
    pub n_experts: u32,
    /// Weight values, zero-padded past `n_experts`. Values are in
    /// `[0, 1]` and sum to 1 across the first `n_experts` entries.
    pub weights: [f32; MAX_EXPERTS],
}
