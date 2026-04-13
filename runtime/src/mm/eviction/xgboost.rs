//! XGBoost eviction policy.
//!
//! Builds the 27-feature vector for each candidate via
//! [`super::features::extract_features`], runs each row through the
//! generated [`super::generated::xgb_predict`] if-else chain, and
//! picks the candidate with the highest score. The predict function
//! returns `P(optimal eviction target)` after a sigmoid, so higher
//! scores are more evictable.
//!
//! When `ai_eviction_models` is OFF, `xgb_predict` is the stub that
//! returns 0.5 for every input — in that build all candidates tie and
//! this policy reduces to a constant "pick index 0". Callers should
//! consult [`super::generated::MODELS_AVAILABLE`] and install a
//! classical policy instead if they want differentiated decisions.

use alloc::vec::Vec;

use super::features::extract_features;
use super::generated;
use super::policy::{BlockMeta, EvictionPolicy};

#[derive(Default)]
pub struct XGBoostPolicy;

impl XGBoostPolicy {
    pub fn new() -> Self { Self }
}

impl EvictionPolicy for XGBoostPolicy {
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(
            !candidates.is_empty(),
            "XGBoostPolicy select_victim on empty list"
        );
        let features = extract_features(candidates);

        let mut best = 0usize;
        let mut best_score = f32::MIN;
        for (i, row) in features.iter().enumerate() {
            let s = generated::xgb_predict(row);
            if s > best_score {
                best_score = s;
                best = i;
            }
        }
        best
    }

    fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32> {
        let features = extract_features(candidates);
        features.iter().map(|row| generated::xgb_predict(row)).collect()
    }

    fn name(&self) -> &'static str { "XGBoost" }
}
