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
use super::{active_blob, blob_status, BlobKind, RuntimeXGBoostModel};
use super::policy::{BlockMeta, EvictionPolicy};

#[derive(Clone)]
struct RuntimeCache {
    checksum: u32,
    model: RuntimeXGBoostModel,
}

#[derive(Default)]
pub struct XGBoostPolicy {
    runtime_cache: Option<RuntimeCache>,
}

impl XGBoostPolicy {
    pub fn new() -> Self { Self { runtime_cache: None } }

    fn refresh_runtime_cache(&mut self) {
        let active_checksum = blob_status(BlobKind::XGBoost)
            .active
            .map(|meta| meta.checksum);
        if self.runtime_cache.as_ref().map(|c| c.checksum) == active_checksum {
            return;
        }

        if let Some(checksum) = active_checksum {
            self.runtime_cache = active_blob(BlobKind::XGBoost)
                .and_then(|blob| super::runtime_xgboost::parse_payload(&blob.payload).ok())
                .map(|model| RuntimeCache { checksum, model });
        } else {
            self.runtime_cache = None;
        }
    }

    fn score_row(&self, row: &super::policy::BlockFeatures) -> f32 {
        if let Some(cache) = self.runtime_cache.as_ref() {
            return cache.model.predict(row);
        }
        generated::xgb_predict(row)
    }
}

impl EvictionPolicy for XGBoostPolicy {
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(
            !candidates.is_empty(),
            "XGBoostPolicy select_victim on empty list"
        );
        self.refresh_runtime_cache();
        let features = extract_features(candidates);

        let mut best = 0usize;
        let mut best_score = f32::MIN;
        for (i, row) in features.iter().enumerate() {
            let s = self.score_row(row);
            if s > best_score {
                best_score = s;
                best = i;
            }
        }
        best
    }

    fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32> {
        self.refresh_runtime_cache();
        let features = extract_features(candidates);
        features.iter().map(|row| self.score_row(row)).collect()
    }

    fn name(&self) -> &'static str { "XGBoost" }
}
