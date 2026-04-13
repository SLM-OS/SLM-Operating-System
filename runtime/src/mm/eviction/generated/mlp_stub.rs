//! Stub int8-MLP predictor used when `ai_eviction_models` is OFF.
//!
//! Returns a constant 0.5 so the MLP policy (M4) compiles and links,
//! but produces uninformative scores. The real ~5 KB quantized MLP is
//! imported from `slm-os-page-sim` via
//! `scripts/import_eviction_weights.sh` and replaces this file at
//! `mlp_policy_generated.rs`.

use crate::mm::eviction::policy::BlockFeatures;

/// Constant score; callers should check `generated::MODELS_AVAILABLE`
/// and fall back to a classical policy if the stub is active.
pub fn mlp_predict(_features: &BlockFeatures) -> f32 {
    0.5
}
