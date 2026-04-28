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

pub use tensor::Tensor;
pub use workspace::BumpAllocator;
pub use engine::{InferenceEngine, EngineError, InferenceStats, run_inference, get_stats};
