//! ARC (Adaptive Replacement Cache) eviction policy.
//!
//! Ported from the Python reference `src/policies/arc.py` in the
//! sibling `slm-os-page-sim` project. The sibling Rust crate does not
//! yet ship an ARC port — this file is the canonical Rust translation.
//!
//! ARC maintains four LRU-ordered lists:
//!   - **T1**: blocks accessed once (recency)
//!   - **T2**: blocks accessed more than once (frequency)
//!   - **B1**: ghost entries for recent T1 evictions
//!   - **B2**: ghost entries for recent T2 evictions
//!
//! The adaptive parameter `p` targets the split between T1 and T2.
//! A ghost hit in B1 grows `p` (favour recency); a ghost hit in B2
//! shrinks it (favour frequency). Ghost lists are bounded at
//! `max_ghost_size` entries (default 256) and trimmed from the LRU
//! end as they overflow.
//!
//! Callers funnel every access into [`ARCPolicy::notify_access`] and
//! every eviction into [`ARCPolicy::notify_eviction`]. [`EvictionPolicy::
//! update_feedback`] forwards `(block_id, was_fault)` into
//! `notify_access` so the registry's generic feedback-plumbing drives
//! ARC without bespoke FFI.
//!
//! Reference: Megiddo & Modha, "ARC: A Self-Tuning, Low Overhead
//! Replacement Cache," FAST 2003.

use alloc::collections::VecDeque;
use alloc::vec::Vec;

use super::policy::{BlockMeta, EvictionPolicy};

/// Default upper bound for each ghost list.
pub const ARC_DEFAULT_GHOST_SIZE: usize = 256;

/// LRU-ordered list of `(block_id, model_id)` entries. Insertions go to
/// the MRU end; eviction / trimming pops from the LRU end. `move_to_end`
/// is O(n) because we scan to find the entry, but both `n` and the
/// call frequency are bounded by the cache size, so this is fine for
/// the slow path. A manual intrusive linked list would cut that to
/// O(1) at the cost of a much larger implementation.
#[derive(Default)]
struct ArcList {
    entries: VecDeque<(u32, u8)>,
}

impl ArcList {
    fn new() -> Self { Self { entries: VecDeque::new() } }

    fn len(&self) -> usize { self.entries.len() }

    fn contains(&self, block_id: u32) -> bool {
        self.entries.iter().any(|(id, _)| *id == block_id)
    }

    fn insert_mru(&mut self, block_id: u32, model_id: u8) {
        self.entries.push_back((block_id, model_id));
    }

    /// Remove a specific block. Returns its `model_id` if found.
    fn remove(&mut self, block_id: u32) -> Option<u8> {
        let pos = self.entries.iter().position(|(id, _)| *id == block_id)?;
        let (_id, model_id) = self.entries.remove(pos)?;
        Some(model_id)
    }

    /// Remove a block if present and push it to MRU.
    fn move_to_mru(&mut self, block_id: u32) -> bool {
        if let Some(m) = self.remove(block_id) {
            self.entries.push_back((block_id, m));
            true
        } else {
            false
        }
    }

    /// Pop the least-recently-used entry (front of the deque).
    fn pop_lru(&mut self) -> Option<(u32, u8)> {
        self.entries.pop_front()
    }

    fn clear(&mut self) { self.entries.clear(); }
}

pub struct ARCPolicy {
    t1: ArcList,
    t2: ArcList,
    b1: ArcList,
    b2: ArcList,
    /// Target size for T1 (as a float so integer deltas can track the
    /// adaptive updates exactly as the Python reference does).
    p: f32,
    max_ghost: usize,
}

impl Default for ARCPolicy {
    fn default() -> Self { Self::new() }
}

impl ARCPolicy {
    pub fn new() -> Self {
        Self::with_max_ghost(ARC_DEFAULT_GHOST_SIZE)
    }

    pub fn with_max_ghost(max_ghost: usize) -> Self {
        Self {
            t1: ArcList::new(),
            t2: ArcList::new(),
            b1: ArcList::new(),
            b2: ArcList::new(),
            p: 0.0,
            max_ghost,
        }
    }

    /// Current adaptive parameter — exposed for tests and introspection.
    pub fn target_p(&self) -> f32 { self.p }

    pub fn t1_len(&self) -> usize { self.t1.len() }
    pub fn t2_len(&self) -> usize { self.t2.len() }
    pub fn b1_len(&self) -> usize { self.b1.len() }
    pub fn b2_len(&self) -> usize { self.b2.len() }

    /// Called on every access (hit or miss) to maintain T1/T2/B1/B2.
    ///
    /// Cases are ordered exactly as in the Python reference. Any
    /// deviation breaks ghost-hit adaptation.
    pub fn notify_access(&mut self, block_id: u32, model_id: u8) {
        // Case 1: Hit in T1 → promote to T2 (MRU).
        if self.t1.contains(block_id) {
            self.t1.remove(block_id);
            self.t2.insert_mru(block_id, model_id);
            return;
        }

        // Case 2: Hit in T2 → move to MRU of T2.
        if self.t2.contains(block_id) {
            self.t2.move_to_mru(block_id);
            return;
        }

        // Case 3: Ghost hit in B1 → increase p (favour recency).
        if self.b1.contains(block_id) {
            let delta = core::cmp::max(
                1,
                (self.b2.len() as i64) / core::cmp::max(self.b1.len() as i64, 1),
            ) as f32;
            self.p = (self.p + delta).min(self.max_ghost as f32);
            self.b1.remove(block_id);
            self.t2.insert_mru(block_id, model_id);
            return;
        }

        // Case 4: Ghost hit in B2 → decrease p (favour frequency).
        if self.b2.contains(block_id) {
            let delta = core::cmp::max(
                1,
                (self.b1.len() as i64) / core::cmp::max(self.b2.len() as i64, 1),
            ) as f32;
            self.p = (self.p - delta).max(0.0);
            self.b2.remove(block_id);
            self.t2.insert_mru(block_id, model_id);
            return;
        }

        // Case 5: Complete miss — insert into T1 and trim ghost lists.
        self.t1.insert_mru(block_id, model_id);
        self.trim_ghost_lists();
    }

    /// Called when a block is evicted from the live cache. Moves the
    /// block's tracking entry into the matching ghost list.
    pub fn notify_eviction(&mut self, block_id: u32) {
        if let Some(model_id) = self.t1.remove(block_id) {
            self.b1.insert_mru(block_id, model_id);
        } else if let Some(model_id) = self.t2.remove(block_id) {
            self.b2.insert_mru(block_id, model_id);
        }
        self.trim_ghost_lists();
    }

    fn trim_ghost_lists(&mut self) {
        while self.b1.len() > self.max_ghost { self.b1.pop_lru(); }
        while self.b2.len() > self.max_ghost { self.b2.pop_lru(); }
    }

    /// Return the original index of the LRU block among the supplied
    /// candidate indices.
    fn pick_lru(candidates: &[BlockMeta], indices: &[usize]) -> usize {
        debug_assert!(!indices.is_empty(), "ARC pick_lru on empty indices");
        let mut victim_idx = indices[0];
        let mut min_time = candidates[victim_idx].last_access_time;
        for &i in indices.iter().skip(1) {
            if candidates[i].last_access_time < min_time {
                min_time = candidates[i].last_access_time;
                victim_idx = i;
            }
        }
        victim_idx
    }
}

impl EvictionPolicy for ARCPolicy {
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(!candidates.is_empty(), "ARC select_victim on empty list");

        let mut t1_candidates: Vec<usize> = Vec::new();
        let mut t2_candidates: Vec<usize> = Vec::new();
        for (i, b) in candidates.iter().enumerate() {
            if self.t1.contains(b.block_id) {
                t1_candidates.push(i);
            } else if self.t2.contains(b.block_id) {
                t2_candidates.push(i);
            }
        }

        if !t1_candidates.is_empty() && (self.t1.len() as f32) > self.p {
            Self::pick_lru(candidates, &t1_candidates)
        } else if !t2_candidates.is_empty() {
            Self::pick_lru(candidates, &t2_candidates)
        } else if !t1_candidates.is_empty() {
            Self::pick_lru(candidates, &t1_candidates)
        } else {
            // Fallback: LRU over all candidates.
            let all: Vec<usize> = (0..candidates.len()).collect();
            Self::pick_lru(candidates, &all)
        }
    }

    fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32> {
        candidates
            .iter()
            .map(|b| {
                if self.t1.contains(b.block_id) && (self.t1.len() as f32) > self.p {
                    0.8
                } else if self.t1.contains(b.block_id) {
                    0.5
                } else if self.t2.contains(b.block_id) {
                    0.3
                } else {
                    // Unknown blocks (never seen by notify_access) are
                    // treated as moderately evictable — matches the
                    // Python reference's default for unseen candidates.
                    0.6
                }
            })
            .collect()
    }

    /// Bridge the trait's generic feedback hook to ARC's notify_access.
    /// `was_fault = true` means the evicted block was re-accessed, so
    /// surface the ghost-hit adaptation immediately. Non-fault feedback
    /// (eviction window expired) is a no-op — the block has already
    /// been recorded in the matching ghost list by `notify_eviction`.
    ///
    /// The policy doesn't know the original `model_id` here, so ghost
    /// hits re-enter with model_id=0. That's a known approximation vs
    /// the Python reference, which tracks model_id alongside the
    /// ghost entry; the capstone CACHEUS experiments use ARC as a
    /// recency/frequency expert only, so the missing identity doesn't
    /// affect its score outputs.
    fn update_feedback(&mut self, block_id: u32, was_fault: bool) {
        if was_fault {
            self.notify_access(block_id, 0);
        }
    }

    fn notify_eviction(&mut self, block_id: u32) {
        ARCPolicy::notify_eviction(self, block_id);
    }

    fn reset(&mut self) {
        self.t1.clear();
        self.t2.clear();
        self.b1.clear();
        self.b2.clear();
        self.p = 0.0;
    }

    fn name(&self) -> &'static str { "ARC" }
}
