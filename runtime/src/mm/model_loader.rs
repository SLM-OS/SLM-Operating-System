//! Model Loader for SLM-OS
//!
//! This module provides the skeleton for loading AI models into memory.
//! The full implementation will be completed in Phase 5.
//!
//! # Design
//!
//! The ModelLoader is responsible for:
//! - Reading model files (GGUF, ONNX, etc.)
//! - Allocating memory from the model memory pools
//! - Mapping model weights and workspace
//! - Managing model lifecycle
//!
//! # Usage
//!
//! ```ignore
//! let loader = ModelLoader::new();
//! let model = loader.load_from_buffer(&model_data)?;
//! // Use model for inference...
//! drop(model); // Automatically unloads
//! ```

use super::model_mem::{ModelHandle, AllocError};

/// Model format types supported by the loader.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ModelFormat {
    /// GGUF format (llama.cpp)
    Gguf,
    /// ONNX format
    Onnx,
    /// Raw weight tensor format
    RawTensors,
    /// Unknown/unsupported format
    Unknown,
}

/// Model metadata extracted during loading.
#[derive(Debug, Clone)]
pub struct ModelMetadata {
    /// Model format
    pub format: ModelFormat,
    /// Model name (if available)
    pub name: Option<&'static str>,
    /// Number of parameters
    pub param_count: u64,
    /// Size of weights in bytes
    pub weight_size: usize,
    /// Size of workspace needed for inference
    pub workspace_size: usize,
    /// Whether model supports quantization
    pub quantized: bool,
}

impl Default for ModelMetadata {
    fn default() -> Self {
        Self {
            format: ModelFormat::Unknown,
            name: None,
            param_count: 0,
            weight_size: 0,
            workspace_size: 0,
            quantized: false,
        }
    }
}

/// A loaded model ready for inference.
pub struct LoadedModel {
    /// Weight memory handle
    weights: ModelHandle,
    /// Workspace memory handle
    workspace: ModelHandle,
    /// Model metadata
    metadata: ModelMetadata,
}

impl LoadedModel {
    /// Get the weight memory handle.
    pub fn weights(&self) -> ModelHandle {
        self.weights
    }

    /// Get the workspace memory handle.
    pub fn workspace(&self) -> ModelHandle {
        self.workspace
    }

    /// Get model metadata.
    pub fn metadata(&self) -> &ModelMetadata {
        &self.metadata
    }

    /// Get the number of parameters.
    pub fn param_count(&self) -> u64 {
        self.metadata.param_count
    }

    /// Check if the model is quantized.
    pub fn is_quantized(&self) -> bool {
        self.metadata.quantized
    }
}

impl Drop for LoadedModel {
    fn drop(&mut self) {
        // Free model memory when dropped
        // Note: In full implementation, this would properly release resources
        let _ = super::free(self.weights);
        let _ = super::free(self.workspace);
    }
}

/// Error type for model loading operations.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoadError {
    /// Pool out of memory or PMM exhaustion.
    AllocFailed,
    /// Concurrent free/reuse race against the loader's handle.
    AllocStaleHandle,
    /// Allocation handle no longer corresponds to a live slot.
    AllocInvalidHandle,
    /// Allocator internal precondition violated.
    AllocInternal,
    /// Invalid model format
    InvalidFormat,
    /// Model too large for available memory
    ModelTooLarge,
    /// Model data is corrupted
    CorruptedData,
    /// Feature not implemented
    NotImplemented,
    /// Registry index does not point at an active slot. Distinct from
    /// `InvalidFormat` so swap_model can report "no model in this slot"
    /// without conflating it with a malformed payload.
    InvalidIndex,
    /// The backend at this slot does not implement weight hot-swap.
    /// Currently only the CPU ONNX backend supports it; Hailo's
    /// CCW-reupload swap path is tracked separately in #532.
    SwapNotSupported,
}

impl From<AllocError> for LoadError {
    fn from(e: AllocError) -> Self {
        // Map per-variant so a caller observing LoadError can
        // distinguish "real OOM" from "stale handle race" from
        // "internal allocator precondition violated". Collapsing
        // every variant to AllocFailed loses signal that's useful
        // to higher-level retry / fallback logic.
        match e {
            AllocError::OutOfMemory | AllocError::PmmFailed => LoadError::AllocFailed,
            AllocError::StaleHandle => LoadError::AllocStaleHandle,
            AllocError::InvalidHandle => LoadError::AllocInvalidHandle,
            AllocError::AlignmentError
            | AllocError::NotInitialized
            | AllocError::Oversized => LoadError::AllocInternal,
        }
    }
}

/// Model loader for loading AI models into memory.
///
/// This is a skeleton implementation for Phase 3.
/// Full implementation will be completed in Phase 5.
pub struct ModelLoader {
    /// Maximum model size allowed (in bytes)
    max_model_size: usize,
    /// Whether to allow quantized models
    allow_quantized: bool,
}

impl ModelLoader {
    /// Default maximum model size (256 MB)
    pub const DEFAULT_MAX_SIZE: usize = 256 * 1024 * 1024;

    /// Create a new ModelLoader with default settings.
    pub fn new() -> Self {
        Self {
            max_model_size: Self::DEFAULT_MAX_SIZE,
            allow_quantized: true,
        }
    }

    /// Create a new ModelLoader with custom maximum size.
    pub fn with_max_size(max_size: usize) -> Self {
        Self {
            max_model_size: max_size,
            allow_quantized: true,
        }
    }

    /// Set whether quantized models are allowed.
    pub fn allow_quantized(mut self, allow: bool) -> Self {
        self.allow_quantized = allow;
        self
    }

    /// Detect the format of a model from its header.
    ///
    /// # Arguments
    /// * `data` - The first bytes of the model file
    ///
    /// # Returns
    /// The detected ModelFormat
    pub fn detect_format(data: &[u8]) -> ModelFormat {
        if data.len() < 4 {
            return ModelFormat::Unknown;
        }

        // GGUF magic: "GGUF" (0x46554747)
        if data.len() >= 4 && &data[0..4] == b"GGUF" {
            return ModelFormat::Gguf;
        }

        // ONNX magic: starts with protobuf header (0x08)
        // This is a simplified check; real implementation would be more thorough
        if data.len() >= 8 && data[0] == 0x08 {
            return ModelFormat::Onnx;
        }

        ModelFormat::Unknown
    }

    /// Load a model from a memory buffer.
    ///
    /// # Arguments
    /// * `data` - The complete model data
    ///
    /// # Returns
    /// A LoadedModel on success, or LoadError on failure
    ///
    /// # Note
    /// This is a skeleton implementation. Full parsing will be added in Phase 5.
    pub fn load_from_buffer(&self, data: &[u8]) -> Result<LoadedModel, LoadError> {
        // Phase 3 skeleton - just validates basic constraints
        if data.len() > self.max_model_size {
            return Err(LoadError::ModelTooLarge);
        }

        let format = Self::detect_format(data);
        if format == ModelFormat::Unknown {
            return Err(LoadError::InvalidFormat);
        }

        // For the skeleton, we'll allocate placeholder memory
        // Real implementation would parse the model and allocate exact sizes
        let weight_size = data.len();
        let workspace_size = weight_size / 4; // Estimate: 25% of model size

        // Allocate weight memory
        let weights = super::alloc_weights(weight_size)?;

        // Allocate workspace
        let workspace = match super::alloc_workspace(workspace_size) {
            Ok(w) => w,
            Err(e) => {
                let _ = super::free(weights);
                return Err(e.into());
            }
        };

        let metadata = ModelMetadata {
            format,
            name: None,
            param_count: 0, // Would be parsed from model
            weight_size,
            workspace_size,
            quantized: false, // Would be parsed from model
        };

        Ok(LoadedModel {
            weights,
            workspace,
            metadata,
        })
    }

    /// Estimate memory requirements for a model.
    ///
    /// # Arguments
    /// * `data` - Model header data (first ~1KB is usually sufficient)
    ///
    /// # Returns
    /// Tuple of (weight_size, workspace_size) estimates
    pub fn estimate_memory(&self, data: &[u8]) -> Result<(usize, usize), LoadError> {
        let format = Self::detect_format(data);
        if format == ModelFormat::Unknown {
            return Err(LoadError::InvalidFormat);
        }

        // Skeleton: estimate based on data length
        // Real implementation would parse model header
        Err(LoadError::NotImplemented)
    }
}

impl Default for ModelLoader {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_detect_format() {
        // GGUF magic
        let gguf_data = b"GGUF\x03\x00\x00\x00";
        assert_eq!(ModelLoader::detect_format(gguf_data), ModelFormat::Gguf);

        // Unknown format
        let unknown = b"UNKN";
        assert_eq!(ModelLoader::detect_format(unknown), ModelFormat::Unknown);
    }
}
