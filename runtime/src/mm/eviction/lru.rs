//! Least-Recently-Used eviction.
//!
//! Ported verbatim from `slm_os_integration/src/lru.rs` in the sibling
//! `slm-os-page-sim` project. Decisions match the Python reference in
//! `src/policies/lru.py` — pick the candidate with the smallest
//! `last_access_time`; on ties the first index wins (matches Python's
//! `<` comparison loop).

use alloc::vec::Vec;

use super::policy::{BlockMeta, EvictionPolicy};

#[derive(Default)]
pub struct LruPolicy;

impl LruPolicy {
    pub fn new() -> Self {
        Self
    }
}

impl EvictionPolicy for LruPolicy {
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(!candidates.is_empty(), "LRU select_victim on empty list");
        let mut victim = 0;
        let mut min_time = candidates[0].last_access_time;
        for (i, c) in candidates.iter().enumerate().skip(1) {
            if c.last_access_time < min_time {
                min_time = c.last_access_time;
                victim = i;
            }
        }
        victim
    }

    fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32> {
        // Inverse recency: oldest gets the highest score, scaled to [0, 1].
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
            .map(|c| (max_time - c.last_access_time) as f32 / span as f32)
            .collect()
    }

    fn name(&self) -> &'static str {
        "LRU"
    }
}
