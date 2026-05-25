//! Stub XGBoost predictor used when `ai_eviction_models` is OFF.
//!
//! Returns a constant 0.5 so the XGBoost policy (M4) compiles and
//! links, but produces uninformative scores. The real ~140 KB if-else
//! chain (16-tree, pruned per #961) is imported from `slm-os-page-sim` via
//! `scripts/import_eviction_weights.sh` and replaces this file at
//! `xgb_policy_generated.rs`.

use crate::mm::eviction::policy::BlockFeatures;

/// Constant score; callers should check `generated::MODELS_AVAILABLE`
/// and fall back to a classical policy if the stub is active.
pub fn xgb_predict(_features: &BlockFeatures) -> f32 {
    0.5
}
