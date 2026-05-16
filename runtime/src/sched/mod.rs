//! Scheduler policy module for SLM-OS.
//!
//! This module provides Rust-side scheduling policies for AI inference tasks.
//! The actual scheduling is done by the C kernel, but this module provides:
//! - Priority and deadline management from Rust
//! - Scheduling hints for SLM inference tasks
//! - Core affinity recommendations based on task characteristics
//!
//! # Architecture
//!
//! The scheduler uses a hybrid approach:
//! - Fixed priority base (8 levels: 0-7)
//! - Deadline boost for time-critical inference tasks
//! - Core affinity hints for heterogeneous systems (big.LITTLE)

pub mod deadline;
pub mod heterogeneous;
pub mod inference;

// Re-export commonly used types from deadline module
pub use deadline::{
    SchedulingHint, TaskDeadline, SlmTaskInfo,
    schedule_slm_task, suggest_core_affinity,
};

// Re-export inference scheduler types
pub use inference::{
    InferenceScheduler, InferenceRequest, InferenceResult,
    InferenceConfig, RequestState, SchedulerError, SchedulerStats,
    submit_inference_sync, batching_enabled, set_batching_enabled,
    batch_size, set_batch_size, batch_timeout_us, set_batch_timeout_us,
    stats as scheduler_stats, reset_stats as reset_scheduler_stats,
    queue_depth as scheduler_queue_depth,
    MAX_BATCH, MAX_INPUT_DIM, MAX_OUTPUT_DIM,
};

// Re-export heterogeneous scheduling types
pub use heterogeneous::{
    CpuTopology, CoreInfo, ClusterInfo, TaskPlacement,
    LoadBalancer, CoreLoad,
};

// Priority and CoreType are defined in this module (above)

/// Priority levels matching C kernel's TASK_PRIORITY_* constants.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Priority {
    /// Background/idle tasks (lowest priority)
    Idle = 0,
    /// Low priority tasks
    Low = 2,
    /// Default priority for normal tasks
    Normal = 4,
    /// High priority tasks
    High = 6,
    /// Critical priority (highest)
    Critical = 7,
}

impl Priority {
    /// Convert from raw priority value.
    pub fn from_raw(value: u8) -> Self {
        match value {
            0 => Priority::Idle,
            1 | 2 => Priority::Low,
            3 | 4 => Priority::Normal,
            5 | 6 => Priority::High,
            7 => Priority::Critical,
            _ => Priority::Normal, // Clamp to valid range
        }
    }

    /// Get raw priority value.
    pub fn as_raw(self) -> u8 {
        self as u8
    }
}

impl Default for Priority {
    fn default() -> Self {
        Priority::Normal
    }
}

/// Core type for heterogeneous scheduling.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CoreType {
    /// Performance core (big core) - for compute-intensive tasks
    Performance,
    /// Efficiency core (LITTLE core) - for background tasks
    Efficiency,
    /// Any core - no preference
    Any,
}
