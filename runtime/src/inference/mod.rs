//! CPU Inference Engine for SLM-OS
//!
//! Executes ONNX operator graphs on loaded models using FP32 CPU arithmetic.
//!
//! # Architecture
//!
//! ```text
//! Input data ──> InferenceEngine.run()
//!                  │
//!                  ├─ bind graph inputs
//!                  ├─ bind weight tensors (from WeightTable)
//!                  ├─ for each node in topological order:
//!                  │    resolve inputs → dispatch op → bind output
//!                  └─ copy graph output to caller
//! ```

pub mod tensor;
pub mod workspace;
pub mod mathf;
pub mod ops;
pub mod engine;
pub mod gpu;

#[cfg(feature = "slm")]
pub mod quant;

#[cfg(feature = "slm")]
pub mod ops_transformer;

#[cfg(feature = "slm")]
pub mod gpu_slm;

pub use tensor::Tensor;
pub use workspace::BumpAllocator;
pub use engine::{InferenceEngine, EngineError, InferenceStats, run_inference, get_stats};
pub use engine::{
    OpProfileEntry, PROFILE_NUM_OPS,
    op_profile_set_enabled, op_profile_is_enabled,
    op_profile_reset, op_profile_snapshot,
};
