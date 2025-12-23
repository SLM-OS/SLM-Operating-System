//! Memory management module for SLM-OS runtime.
//!
//! This module provides:
//! - Model memory allocator for AI weights and inference workspace
//! - GPU memory mapping support
//! - Model loader skeleton (full implementation in Phase 5)
//!
//! See `docs/model-memory.md` for design documentation.

pub mod model_mem;
pub mod model_loader;

pub use model_mem::{
    AllocError, ModelHandle, PoolStats,
    alloc_weights, alloc_workspace, free,
    share, unshare, get_ptr, get_size,
    weight_pool_stats, workspace_pool_stats,
    model_mem_init,
};

pub use model_loader::{
    ModelLoader, LoadedModel, ModelMetadata,
    ModelFormat, LoadError,
};
