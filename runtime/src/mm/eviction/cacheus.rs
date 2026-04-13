//! CACHEUS-style adaptive expert selector.
//!
//! Ported from `slm_os_integration/src/cacheus.rs` in the sibling
//! `slm-os-page-sim` project, after the Phase 5 fixes:
//!
//! - `EvictionRecord` stores the ensemble's **actual choice**, not the
//!   last expert's choice. The original bug made all experts appear
//!   to agree with the last one and distorted weight updates.
//! - On `was_fault=true` (evicted block was re-accessed), agreeing
//!   experts are penalised and disagreeing ones get a small reward.
//! - On `was_fault=false`, agreeing experts are rewarded.
//! - Weights are floored at `min_weight` and renormalised so no expert
//!   is permanently silenced.
//!
//! The Phase 5 sweep in the sibling project showed that the `ml_only`
//! pool (XGBoost + MLP, `lr=0.4`, `window=200`) wins at 0.212 mean
//! normalised fault rate — beating `all_5` (0.427) by ~2×. Adding the
//! classical experts dilutes the ensemble on the SLM workload.
//! [`CacheusSelector::ml_only`] is the recommended runtime default;
//! [`CacheusSelector::all_5`] is provided for ablation experiments.

use alloc::boxed::Box;
use alloc::collections::VecDeque;
use alloc::vec::Vec;

use super::lru::LruPolicy;
use super::lfu::LfuPolicy;
use super::slm_heuristic::SlmHeuristicPolicy;
use super::mlp::MlpPolicy;
use super::policy::{BlockMeta, EvictionPolicy};
use super::xgboost::XGBoostPolicy;

/// Default expert-pool tuning from the sibling Phase 5 sweep.
pub const CACHEUS_DEFAULT_LR: f32 = 0.4;
pub const CACHEUS_DEFAULT_WINDOW: usize = 200;
const DEFAULT_MIN_WEIGHT: f32 = 0.01;

/// Records one ensemble decision so feedback can later reward or
/// penalise the experts that agreed with it.
#[derive(Clone)]
struct EvictionRecord {
    victim_block_id: u32,
    expert_choices: Vec<usize>, // per-expert candidate index at decision time
    ensemble_choice: usize,
}

/// Weighted ensemble of `EvictionPolicy` experts with online weight
/// updates. Weights are non-negative, sum to 1, and are floored at
/// `min_weight` after every update so no expert is silenced forever.
pub struct CacheusSelector {
    experts: Vec<Box<dyn EvictionPolicy + Send>>,
    weights: Vec<f32>,
    learning_rate: f32,
    min_weight: f32,
    window_size: usize,
    history: VecDeque<EvictionRecord>,
    expert_faults: Vec<u32>,
    expert_decisions: Vec<u32>,
}

impl CacheusSelector {
    /// Build a selector with uniform initial weights.
    ///
    /// Panics (in debug) if the expert pool is empty.
    pub fn new(
        experts: Vec<Box<dyn EvictionPolicy + Send>>,
        learning_rate: f32,
        window_size: usize,
    ) -> Self {
        debug_assert!(!experts.is_empty(), "CACHEUS requires at least one expert");
        let n = experts.len();
        let weights = alloc::vec![1.0 / n as f32; n];
        Self {
            experts,
            weights,
            learning_rate,
            min_weight: DEFAULT_MIN_WEIGHT,
            window_size,
            history: VecDeque::with_capacity(window_size),
            expert_faults: alloc::vec![0; n],
            expert_decisions: alloc::vec![0; n],
        }
    }

    /// Recommended runtime default: XGBoost + int8 MLP, `lr=0.4`,
    /// `window=200`. Matches the sibling's winning `ml_only` pool.
    pub fn ml_only() -> Self {
        let experts: Vec<Box<dyn EvictionPolicy + Send>> = alloc::vec![
            Box::new(XGBoostPolicy::new()),
            Box::new(MlpPolicy::new()),
        ];
        Self::new(experts, CACHEUS_DEFAULT_LR, CACHEUS_DEFAULT_WINDOW)
    }

    /// Ablation-experiment constructor: LRU + LFU + SLM-Heuristic +
    /// XGBoost + int8 MLP. Underperforms `ml_only` on the sibling
    /// workload but remains useful for comparison.
    pub fn all_5() -> Self {
        let experts: Vec<Box<dyn EvictionPolicy + Send>> = alloc::vec![
            Box::new(LruPolicy::new()),
            Box::new(LfuPolicy::new()),
            Box::new(SlmHeuristicPolicy::new()),
            Box::new(XGBoostPolicy::new()),
            Box::new(MlpPolicy::new()),
        ];
        Self::new(experts, CACHEUS_DEFAULT_LR, CACHEUS_DEFAULT_WINDOW)
    }

    pub fn weights(&self) -> &[f32] { &self.weights }
    pub fn expert_names(&self) -> Vec<&'static str> {
        self.experts.iter().map(|e| e.name()).collect()
    }
    pub fn expert_faults(&self) -> &[u32] { &self.expert_faults }
    pub fn expert_decisions(&self) -> &[u32] { &self.expert_decisions }
    pub fn history_len(&self) -> usize { self.history.len() }

    /// Apply a multiplicative weight update for one feedback signal.
    fn update_weights(&mut self, record: &EvictionRecord, was_fault: bool) {
        for (i, &expert_choice) in record.expert_choices.iter().enumerate() {
            self.expert_decisions[i] += 1;
            let agreed = expert_choice == record.ensemble_choice;
            if was_fault {
                if agreed {
                    self.weights[i] *= 1.0 - self.learning_rate;
                    self.expert_faults[i] += 1;
                } else {
                    self.weights[i] *= 1.0 + 0.5 * self.learning_rate;
                }
            } else if agreed {
                self.weights[i] *= 1.0 + self.learning_rate;
            }
        }

        // Floor + renormalise.
        let mut total = 0.0_f32;
        for w in self.weights.iter_mut() {
            if *w < self.min_weight { *w = self.min_weight; }
            total += *w;
        }
        if total > 0.0 {
            for w in self.weights.iter_mut() {
                *w /= total;
            }
        }
    }
}

impl EvictionPolicy for CacheusSelector {
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(
            !candidates.is_empty(),
            "CACHEUS select_victim on empty list"
        );

        let n = candidates.len();
        let mut combined = alloc::vec![0.0_f32; n];
        let mut expert_choices = Vec::with_capacity(self.experts.len());

        for (i, expert) in self.experts.iter_mut().enumerate() {
            let scores = expert.score(candidates);
            let mut best_idx = 0;
            let mut best_score = f32::MIN;
            for (j, &s) in scores.iter().enumerate() {
                combined[j] += self.weights[i] * s;
                if s > best_score {
                    best_score = s;
                    best_idx = j;
                }
            }
            expert_choices.push(best_idx);
        }

        let mut victim = 0;
        let mut max_combined = combined[0];
        for (j, &s) in combined.iter().enumerate().skip(1) {
            if s > max_combined {
                max_combined = s;
                victim = j;
            }
        }

        // Drop the oldest record if the history is at capacity.
        if self.history.len() == self.window_size {
            self.history.pop_front();
        }
        self.history.push_back(EvictionRecord {
            victim_block_id: candidates[victim].block_id,
            expert_choices,
            ensemble_choice: victim,
        });

        victim
    }

    /// Per-candidate score is the weighted sum of expert scores —
    /// lets CACHEUS nest inside another CACHEUS as an expert, though
    /// we don't exercise that today.
    fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32> {
        let n = candidates.len();
        if n == 0 { return Vec::new(); }
        let mut combined = alloc::vec![0.0_f32; n];
        for (i, expert) in self.experts.iter_mut().enumerate() {
            let scores = expert.score(candidates);
            for (j, &s) in scores.iter().enumerate() {
                combined[j] += self.weights[i] * s;
            }
        }
        combined
    }

    fn update_feedback(&mut self, block_id: u32, was_fault: bool) {
        // Search history newest-to-oldest for the matching eviction.
        let found = self
            .history
            .iter()
            .rev()
            .find(|r| r.victim_block_id == block_id)
            .cloned();
        if let Some(record) = found {
            self.update_weights(&record, was_fault);
        }
    }

    fn reset(&mut self) {
        let n = self.experts.len();
        for w in self.weights.iter_mut() { *w = 1.0 / n as f32; }
        self.history.clear();
        for v in self.expert_faults.iter_mut() { *v = 0; }
        for v in self.expert_decisions.iter_mut() { *v = 0; }
        for e in self.experts.iter_mut() { e.reset(); }
    }

    fn name(&self) -> &'static str { "CACHEUS" }
}
