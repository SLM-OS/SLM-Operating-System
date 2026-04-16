//! Hand-tuned SLM-OS eviction policy.
//!
//! Ported verbatim from `slm_os_integration/src/slm_heuristic.rs` in
//! the sibling `slm-os-page-sim` project. Decisions match the Python
//! reference in `src/policies/slm_heuristic.py` — priority cascade:
//!
//!   1. Evict workspace blocks before weights (cheaper to recreate).
//!   2. Within workspace, evict LRU.
//!   3. Evict weights from models with no active inferences.
//!   4. Within inactive model weights, evict LRU.
//!   5. Fallback: evict LRU among all remaining candidates.

use alloc::collections::BTreeMap;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicU32, Ordering};

use super::policy::{BlockMeta, EvictionPolicy, PoolType};

// =============================================================================
// Global active-inferences table (#113)
//
// The scheduler / inference path keeps this table up-to-date via the
// `rust_eviction_bump_active_inferences` FFI. The SlmHeuristicPolicy's
// "inactive-models first" tier consults this table instead of a per-
// instance BTreeMap so the live counts survive policy swaps (install
// CACHEUS → install SLM-Heuristic again without losing the feed).
//
// Storage: `[AtomicU32; MAX_MODELS]` indexed by model_id (u8). Zero-
// lock reads/writes; no heap allocation. Atomics use relaxed ordering
// because the counters are observational — a stale value only biases
// eviction decisions, never breaks invariants.
// =============================================================================

/// Maximum distinct model ids tracked. BlockMeta::model_id is u8, but
/// in practice the allocator hands out small ids from the model
/// registry. 64 slots cover every plausible demo workload.
pub const MAX_MODELS: usize = 64;

static ACTIVE_INFERENCES: [AtomicU32; MAX_MODELS] = {
    const Z: AtomicU32 = AtomicU32::new(0);
    [Z; MAX_MODELS]
};

/// Set the active-inference count for `model_id` to `count`. Used by
/// the FFI so external callers can reset / initialise the table.
pub fn set_active(model_id: u8, count: u32) {
    if (model_id as usize) < MAX_MODELS {
        ACTIVE_INFERENCES[model_id as usize].store(count, Ordering::Relaxed);
    }
}

/// Adjust the active-inference count for `model_id` by `delta`.
/// Clamps at zero — caller doesn't need to balance decrements.
pub fn bump_active_global(model_id: u8, delta: i32) {
    if (model_id as usize) >= MAX_MODELS {
        return;
    }
    let slot = &ACTIVE_INFERENCES[model_id as usize];
    if delta >= 0 {
        slot.fetch_add(delta as u32, Ordering::Relaxed);
    } else {
        // Subtract with underflow clamp; a racy pair of decrements can
        // briefly show a lower value, but never below zero.
        let mag = (-delta) as u32;
        loop {
            let cur = slot.load(Ordering::Relaxed);
            let next = cur.saturating_sub(mag);
            if slot.compare_exchange_weak(cur, next,
                                          Ordering::Relaxed,
                                          Ordering::Relaxed).is_ok() {
                break;
            }
        }
    }
}

/// Read the current active-inference count for `model_id`.
pub fn get_active(model_id: u8) -> u32 {
    if (model_id as usize) < MAX_MODELS {
        ACTIVE_INFERENCES[model_id as usize].load(Ordering::Relaxed)
    } else {
        0
    }
}

/// Zero every entry. Useful for test isolation.
pub fn clear_active() {
    for slot in ACTIVE_INFERENCES.iter() {
        slot.store(0, Ordering::Relaxed);
    }
}

#[derive(Default)]
pub struct SlmHeuristicPolicy {
    /// Per-instance override table. Populated only when a caller uses
    /// `set_active_inferences` directly (test path). Production code
    /// reads the global `ACTIVE_INFERENCES` array instead; see
    /// `is_active` for the preference order.
    active_inferences: BTreeMap<u8, u32>,
}

impl SlmHeuristicPolicy {
    pub fn new() -> Self {
        Self { active_inferences: BTreeMap::new() }
    }

    /// Update the active-inference table (called by the runtime when an
    /// inference starts or finishes). Phase AI-Sched feeds this from
    /// the kernel scheduler via a future FFI; M6 wires up the caller.
    pub fn set_active_inferences(&mut self, active: BTreeMap<u8, u32>) {
        self.active_inferences = active;
    }

    /// Mark `model_id` as having one more / one fewer active inference.
    /// Negative deltas below zero clamp to zero.
    pub fn bump_active(&mut self, model_id: u8, delta: i32) {
        let cur = self.active_inferences.get(&model_id).copied().unwrap_or(0) as i32;
        let next = (cur + delta).max(0) as u32;
        if next == 0 {
            self.active_inferences.remove(&model_id);
        } else {
            self.active_inferences.insert(model_id, next);
        }
    }

    fn is_active(&self, model_id: u8) -> bool {
        // Per-instance override wins — used by direct-call tests that
        // seed the policy's own BTreeMap. If that's empty, fall back
        // to the global table fed by the scheduler / FFI.
        if let Some(&n) = self.active_inferences.get(&model_id) {
            return n > 0;
        }
        get_active(model_id) > 0
    }

    /// Return the original index of the LRU block among the supplied
    /// `(original_index, block)` pairs.
    fn pick_lru(indexed: &[(usize, &BlockMeta)]) -> usize {
        debug_assert!(!indexed.is_empty());
        let mut victim_idx = indexed[0].0;
        let mut min_time = indexed[0].1.last_access_time;
        for (idx, block) in indexed.iter().skip(1) {
            if block.last_access_time < min_time {
                min_time = block.last_access_time;
                victim_idx = *idx;
            }
        }
        victim_idx
    }
}

impl EvictionPolicy for SlmHeuristicPolicy {
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(!candidates.is_empty(), "SLM select_victim on empty list");

        // Priority 1: workspace blocks (cheapest to recreate)
        let workspace: Vec<(usize, &BlockMeta)> = candidates
            .iter()
            .enumerate()
            .filter(|(_, b)| b.pool_type == PoolType::Workspace)
            .collect();
        if !workspace.is_empty() {
            return Self::pick_lru(&workspace);
        }

        // Priority 2: weights from models with no active inferences
        let inactive: Vec<(usize, &BlockMeta)> = candidates
            .iter()
            .enumerate()
            .filter(|(_, b)| !self.is_active(b.model_id))
            .collect();
        if !inactive.is_empty() {
            return Self::pick_lru(&inactive);
        }

        // Priority 3: LRU over all candidates
        let all: Vec<(usize, &BlockMeta)> = candidates.iter().enumerate().collect();
        Self::pick_lru(&all)
    }

    fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32> {
        if candidates.is_empty() {
            return Vec::new();
        }
        let mut min_time = candidates[0].last_access_time;
        let mut max_time = candidates[0].last_access_time;
        for c in &candidates[1..] {
            if c.last_access_time < min_time { min_time = c.last_access_time; }
            if c.last_access_time > max_time { max_time = c.last_access_time; }
        }
        let span = if max_time == min_time { 1 } else { max_time - min_time };

        candidates
            .iter()
            .map(|b| {
                let mut s = 0.0_f32;
                if b.pool_type == PoolType::Workspace { s += 0.6; }
                if !self.is_active(b.model_id) { s += 0.3; }
                s += 0.1 * (max_time - b.last_access_time) as f32 / span as f32;
                s
            })
            .collect()
    }

    fn reset(&mut self) {
        self.active_inferences.clear();
    }

    fn name(&self) -> &'static str {
        "SLM-Heuristic"
    }
}
