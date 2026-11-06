//! ONNX Model Loader
//!
//! Provides ONNX model parsing, operator graph construction,
//! and a registry for loaded models.
//!
//! # Architecture
//!
//! ```text
//! ONNX bytes ──> protobuf parser ──> ONNX parser ──> ParsedOnnx (borrowed)
//!                                                         │
//!                                              build_graph + copy weights
//!                                                         │
//!                                              OperatorGraph + ModelHandle (owned)
//!                                                         │
//!                                                    registry entry
//! ```

pub mod error;
pub mod protobuf;
pub mod onnx_parser;
pub mod graph;
pub mod registry;

pub use error::LoadError;
pub use graph::{AutoPad, NodeAttrs, resolve_pads};
pub use graph::{OpType, OperatorGraph, GraphNode, TensorName, TensorShape, ElemType,
                WeightTable, WeightEntry, MAX_WEIGHT_ENTRIES};
pub use registry::{ModelInfoC, MAX_MODELS};
pub use onnx_parser::ParsedOnnx;
