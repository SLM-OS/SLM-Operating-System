//! Eviction-side wrapper around the shared XGBoost engine.
//!
//! The actual tree-traversal + parsing primitives now live in
//! [`crate::ml::xgb_tree`] so the AI scheduler (#855) can reuse the
//! same bounds-checked, cycle-safe loader. This module preserves the
//! historical eviction-side API:
//!
//! - [`RuntimeXGBoostModel`] — opaque model handle accepted by
//!   [`super::xgboost::XGBoostPolicy`].
//! - [`RuntimeXGBoostError`] — error enum with the same variant set
//!   the eviction tests assert against (`NonZeroReserved` etc.).
//! - [`parse_payload`] — single-classifier (XGB1) entry point pinned
//!   at the eviction `BlockFeatures` width (27).
//! - [`build_test_payload_first_feature_split`] — fixture used by
//!   `lib::rust_eviction_run_tests`.

use alloc::vec::Vec;

use super::policy::BlockFeatures;
use crate::ml::xgb_tree;

pub const PAYLOAD_MAGIC: [u8; 4] = xgb_tree::PAYLOAD_MAGIC_SINGLE_V1;
pub const PAYLOAD_VERSION_V1: u16 = xgb_tree::PAYLOAD_VERSION_V1;
pub const PAYLOAD_HEADER_LEN: usize = xgb_tree::SINGLE_HEADER_LEN;
pub const NODE_LEN_V1: usize = xgb_tree::NODE_LEN_V1;
/// Total size of the canonical 1-tree, 3-node test payload — matches
/// what `build_test_payload_first_feature_split` emits.
pub const PAYLOAD_LEN_V1: usize = PAYLOAD_HEADER_LEN + 2 + 3 * NODE_LEN_V1;

const FEATURE_COUNT: usize = 27;

#[derive(Clone, Debug)]
pub struct RuntimeXGBoostModel {
    inner: xgb_tree::XgbModel,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RuntimeXGBoostError {
    TooShort,
    BadMagic,
    UnsupportedVersion,
    NonZeroReserved,
    BadLength,
    EmptyModel,
    TooManyTrees,
    TooManyNodes,
    RootOutOfRange,
    ChildOutOfRange,
    InvalidFeatureIndex,
    InvalidThreshold,
    InvalidLeafValue,
}

fn map_err(e: xgb_tree::XgbError) -> RuntimeXGBoostError {
    match e {
        xgb_tree::XgbError::TooShort => RuntimeXGBoostError::TooShort,
        xgb_tree::XgbError::BadMagic => RuntimeXGBoostError::BadMagic,
        xgb_tree::XgbError::UnsupportedVersion => RuntimeXGBoostError::UnsupportedVersion,
        xgb_tree::XgbError::NonZeroReserved => RuntimeXGBoostError::NonZeroReserved,
        xgb_tree::XgbError::BadLength => RuntimeXGBoostError::BadLength,
        xgb_tree::XgbError::EmptyModel => RuntimeXGBoostError::EmptyModel,
        xgb_tree::XgbError::TooManyTrees => RuntimeXGBoostError::TooManyTrees,
        xgb_tree::XgbError::TooManyNodes => RuntimeXGBoostError::TooManyNodes,
        // The cascade-only variants below are unreachable from
        // single-classifier parsing; map conservatively to BadLength.
        xgb_tree::XgbError::TooManyClassifiers
        | xgb_tree::XgbError::TooManyLabels => RuntimeXGBoostError::BadLength,
        xgb_tree::XgbError::RootOutOfRange => RuntimeXGBoostError::RootOutOfRange,
        xgb_tree::XgbError::ChildOutOfRange => RuntimeXGBoostError::ChildOutOfRange,
        xgb_tree::XgbError::InvalidFeatureIndex => RuntimeXGBoostError::InvalidFeatureIndex,
        xgb_tree::XgbError::InvalidThreshold => RuntimeXGBoostError::InvalidThreshold,
        xgb_tree::XgbError::InvalidLeafValue => RuntimeXGBoostError::InvalidLeafValue,
    }
}

pub fn parse_payload(bytes: &[u8]) -> Result<RuntimeXGBoostModel, RuntimeXGBoostError> {
    xgb_tree::parse_single(bytes, FEATURE_COUNT)
        .map(|inner| RuntimeXGBoostModel { inner })
        .map_err(map_err)
}

impl RuntimeXGBoostModel {
    pub fn predict(&self, features: &BlockFeatures) -> f32 {
        self.inner.predict_sigmoid(features.as_slice())
    }
}

#[cfg(feature = "ai_eviction")]
pub fn build_test_payload_first_feature_split(threshold: f32, left: f32, right: f32) -> Vec<u8> {
    xgb_tree::build_test_single_first_feature_split(threshold, left, right)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Eviction-side regression: the wrapper must surface the
    /// `NonZeroReserved` variant the lib test depends on (lib.rs
    /// `runtime_xgboost_payload_rejects_nonzero_reserved`).
    #[test]
    fn nonzero_reserved_byte_in_header_is_rejected() {
        let mut payload = build_test_payload_first_feature_split(0.5, -1.0, 1.0);
        payload[6] = 1;
        assert!(matches!(
            parse_payload(&payload),
            Err(RuntimeXGBoostError::NonZeroReserved)
        ));
    }

    #[test]
    fn predict_uses_block_features_slice() {
        let payload = build_test_payload_first_feature_split(0.5, -2.0, 2.0);
        let model = parse_payload(&payload).expect("parse");
        let mut features: BlockFeatures = [0.0; FEATURE_COUNT];
        let p_low = model.predict(&features);
        features[0] = 1.0;
        let p_high = model.predict(&features);
        assert!(p_high > p_low);
    }
}
