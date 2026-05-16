//! Generic ML primitives shared across subsystems.
//!
//! Currently hosts the XGBoost tree-traversal engine (`xgb_tree`) used
//! by both the eviction `XGBoostPolicy` and (per #855) the AI scheduler
//! cascade policy. New shared inference helpers (e.g. small MLPs,
//! adaptive predictors) belong here.

pub mod xgb_tree;
