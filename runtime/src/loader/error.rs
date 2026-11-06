//! Errors from loading a model, shared by the ONNX and GGUF loaders.

use crate::mm::model_mem::AllocError;

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

#[cfg(test)]
mod tests {
    use super::*;

    /// No caller acts on these yet, but the mapping keeps a real out-of-memory, a
    /// stale handle and an allocator bug apart for any that need to.
    #[test]
    fn alloc_errors_keep_their_meaning() {
        let cases = [
            (AllocError::OutOfMemory, LoadError::AllocFailed),
            (AllocError::PmmFailed, LoadError::AllocFailed),
            (AllocError::StaleHandle, LoadError::AllocStaleHandle),
            (AllocError::InvalidHandle, LoadError::AllocInvalidHandle),
            (AllocError::AlignmentError, LoadError::AllocInternal),
            (AllocError::NotInitialized, LoadError::AllocInternal),
            (AllocError::Oversized, LoadError::AllocInternal),
        ];
        for (alloc_error, load_error) in cases {
            assert_eq!(LoadError::from(alloc_error), load_error);
        }
    }
}
