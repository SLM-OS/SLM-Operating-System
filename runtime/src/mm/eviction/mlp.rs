//! Int8-MLP eviction policy.
//!
//! Mirrors [`XGBoostPolicy`] but routes each candidate's 27-feature
//! vector through [`super::generated::mlp_predict`] — the int8-
//! quantised 4-layer MLP imported from the sibling project
//! (27 → 64 → 32 → 16 → 1 with sigmoid on the output).
//!
//! The integer-arithmetic path is roughly 4× smaller than the
//! XGBoost chain (~5 KB of weights vs ~1.3 MB of generated code) and
//! is the better fit for memory-constrained builds.
//!
//! When `ai_eviction_models` is OFF, `mlp_predict` is the stub that
//! returns 0.5 for any input — the policy reduces to a constant
//! "pick index 0" decision. See [`super::generated::MODELS_AVAILABLE`].

use alloc::vec::Vec;

use super::features::extract_features;
use super::generated;
use super::policy::{BlockMeta, EvictionPolicy};

#[derive(Default)]
pub struct MlpPolicy;

impl MlpPolicy {
    pub fn new() -> Self { Self }
}

impl EvictionPolicy for MlpPolicy {
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(
            !candidates.is_empty(),
            "MlpPolicy select_victim on empty list"
        );
        let features = extract_features(candidates);

        let mut best = 0usize;
        let mut best_score = f32::MIN;
        for (i, row) in features.iter().enumerate() {
            let s = generated::mlp_predict(row);
            if s > best_score {
                best_score = s;
                best = i;
            }
        }
        best
    }

    fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32> {
        let features = extract_features(candidates);
        features.iter().map(|row| generated::mlp_predict(row)).collect()
    }

    fn name(&self) -> &'static str { "MLP" }
}
