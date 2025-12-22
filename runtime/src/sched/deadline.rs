//! Deadline-aware scheduling for SLM inference tasks.
//!
//! Provides deadline management and scheduling hints for AI inference workloads.
//! Works with the C kernel scheduler which performs the actual scheduling.

use crate::kernel_ffi::{self, TaskId, KernelResult};
use super::{Priority, CoreType};

/// Deadline thresholds for priority boosting (in nanoseconds).
/// These match the C kernel's DEADLINE_*_NS constants.
pub mod thresholds {
    /// Boost to CRITICAL when deadline is within 10ms
    pub const CRITICAL_NS: u64 = 10 * 1_000_000;
    /// Boost to HIGH when deadline is within 50ms
    pub const HIGH_NS: u64 = 50 * 1_000_000;
    /// Boost +1 when deadline is within 100ms
    pub const BOOST_NS: u64 = 100 * 1_000_000;
}

/// Deadline specification for a task.
#[derive(Debug, Clone, Copy)]
pub struct TaskDeadline {
    /// Absolute deadline in nanoseconds since boot (0 = no deadline)
    pub deadline_ns: u64,
}

impl TaskDeadline {
    /// No deadline - task runs at base priority.
    pub const NONE: Self = TaskDeadline { deadline_ns: 0 };

    /// Create deadline from absolute time in nanoseconds.
    pub fn absolute(deadline_ns: u64) -> Self {
        TaskDeadline { deadline_ns }
    }

    /// Create deadline relative to current time.
    pub fn relative(duration_ns: u64) -> Self {
        let now = kernel_ffi::get_time_ns();
        TaskDeadline {
            deadline_ns: now.saturating_add(duration_ns),
        }
    }

    /// Create deadline for inference with target latency in milliseconds.
    pub fn inference_latency_ms(target_ms: u64) -> Self {
        Self::relative(target_ms * 1_000_000)
    }

    /// Check if deadline has passed.
    pub fn is_expired(&self) -> bool {
        if self.deadline_ns == 0 {
            return false; // No deadline means never expired
        }
        kernel_ffi::get_time_ns() >= self.deadline_ns
    }

    /// Get remaining time until deadline (0 if expired or no deadline).
    pub fn remaining_ns(&self) -> u64 {
        if self.deadline_ns == 0 {
            return u64::MAX; // No deadline
        }
        let now = kernel_ffi::get_time_ns();
        self.deadline_ns.saturating_sub(now)
    }
}

/// Scheduling hint returned by schedule_slm_task.
#[derive(Debug, Clone, Copy)]
pub struct SchedulingHint {
    /// Recommended priority for the task.
    pub priority: Priority,
    /// Recommended core type for the task.
    pub core_type: CoreType,
    /// Whether task should be pinned to a specific core.
    pub pin_to_core: bool,
}

impl Default for SchedulingHint {
    fn default() -> Self {
        SchedulingHint {
            priority: Priority::Normal,
            core_type: CoreType::Any,
            pin_to_core: false,
        }
    }
}

/// SLM task characteristics for scheduling decisions.
#[derive(Debug, Clone, Copy)]
pub struct SlmTaskInfo {
    /// Model working set size in bytes (weights + activations).
    pub working_set_bytes: usize,
    /// Deadline for completion.
    pub deadline: TaskDeadline,
    /// Base priority (before deadline boost).
    pub base_priority: Priority,
}

impl Default for SlmTaskInfo {
    fn default() -> Self {
        SlmTaskInfo {
            working_set_bytes: 0,
            deadline: TaskDeadline::NONE,
            base_priority: Priority::Normal,
        }
    }
}

/// Threshold for "small" models that fit in cache and can run on efficiency cores.
const SMALL_MODEL_THRESHOLD: usize = 8 * 1024 * 1024; // 8 MB

/// Schedule an SLM inference task.
///
/// Returns scheduling hints based on task characteristics:
/// - Urgent tasks (deadline < 50ms) get HIGH priority and performance core
/// - Small models (< 8MB working set) can run on efficiency cores
/// - Large models with deadlines get performance cores
///
/// # Arguments
/// * `task_id` - The task to schedule
/// * `info` - Task characteristics for scheduling decisions
///
/// # Returns
/// Scheduling hint with recommended priority and core type.
pub fn schedule_slm_task(task_id: TaskId, info: &SlmTaskInfo) -> KernelResult<SchedulingHint> {
    let mut hint = SchedulingHint {
        priority: info.base_priority,
        core_type: CoreType::Any,
        pin_to_core: false,
    };

    // Determine effective priority based on deadline
    if info.deadline.deadline_ns != 0 {
        let remaining = info.deadline.remaining_ns();

        if remaining < thresholds::CRITICAL_NS {
            // Very urgent - boost to critical, pin to performance core
            hint.priority = Priority::Critical;
            hint.core_type = CoreType::Performance;
            hint.pin_to_core = true;
        } else if remaining < thresholds::HIGH_NS {
            // Urgent - boost to high priority, prefer performance core
            hint.priority = Priority::High;
            hint.core_type = CoreType::Performance;
        } else if remaining < thresholds::BOOST_NS {
            // Approaching deadline - boost by 1 level
            hint.priority = Priority::from_raw(info.base_priority.as_raw().saturating_add(1).min(7));
        }
    }

    // Determine core preference based on working set size
    if hint.core_type == CoreType::Any {
        hint.core_type = suggest_core_affinity(info.working_set_bytes, &info.deadline);
    }

    // Apply priority to task via FFI
    kernel_ffi::task_set_priority(task_id, hint.priority.as_raw())?;

    // Apply deadline to task via FFI
    kernel_ffi::task_set_deadline(task_id, info.deadline.deadline_ns)?;

    Ok(hint)
}

/// Suggest core affinity based on working set and deadline.
///
/// Small models without urgent deadlines can run on efficiency cores.
/// Large models or urgent tasks should run on performance cores.
pub fn suggest_core_affinity(working_set_bytes: usize, deadline: &TaskDeadline) -> CoreType {
    // Urgent tasks always go to performance cores
    if deadline.deadline_ns != 0 && deadline.remaining_ns() < thresholds::HIGH_NS {
        return CoreType::Performance;
    }

    // Small models can run on efficiency cores
    if working_set_bytes < SMALL_MODEL_THRESHOLD {
        return CoreType::Efficiency;
    }

    // Large models benefit from performance cores
    CoreType::Performance
}

