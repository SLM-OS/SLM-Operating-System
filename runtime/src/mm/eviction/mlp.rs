//! Int8-MLP eviction policy.
//!
//! Mirrors [`XGBoostPolicy`] but routes each candidate's 27-feature
//! vector through [`super::generated::mlp_predict`] — the int8-
//! quantised 4-layer MLP imported from the sibling project
//! (27 → 64 → 32 → 16 → 1 with sigmoid on the output).
//!
//! The integer-arithmetic path is well over an order of magnitude
//! smaller than the XGBoost chain (~5 KB of weights vs ~140 KB of
//! generated code) and is the better fit for memory-constrained builds.
//!
//! When `ai_eviction_models` is OFF, `mlp_predict` is the stub that
//! returns 0.5 for any input — the policy reduces to a constant
//! "pick index 0" decision. See [`super::generated::MODELS_AVAILABLE`].

use alloc::vec::Vec;

use super::features::extract_features;
use super::generated;
use super::{active_blob, blob_status, BlobKind, RuntimeMlpModel};
use super::policy::{BlockMeta, EvictionPolicy};

#[derive(Clone)]
struct RuntimeCache {
    checksum: u32,
    model: RuntimeMlpModel,
}

#[derive(Default)]
pub struct MlpPolicy {
    runtime_cache: Option<RuntimeCache>,
}

impl MlpPolicy {
    pub fn new() -> Self { Self { runtime_cache: None } }

    fn refresh_runtime_cache(&mut self) {
        let active_checksum = blob_status(BlobKind::Mlp)
            .active
            .map(|meta| meta.checksum);
        if self.runtime_cache.as_ref().map(|c| c.checksum) == active_checksum {
            return;
        }

        if let Some(checksum) = active_checksum {
            self.runtime_cache = active_blob(BlobKind::Mlp)
                .and_then(|blob| super::runtime_mlp::parse_payload(&blob.payload).ok())
                .map(|model| RuntimeCache { checksum, model });
        } else {
            self.runtime_cache = None;
        }
    }

    fn score_row(&self, row: &super::policy::BlockFeatures) -> f32 {
        if let Some(cache) = self.runtime_cache.as_ref() {
            return cache.model.predict(row);
        }
        generated::mlp_predict(row)
    }
}

impl EvictionPolicy for MlpPolicy {
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(
            !candidates.is_empty(),
            "MlpPolicy select_victim on empty list"
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

    fn name(&self) -> &'static str { "MLP" }
}
