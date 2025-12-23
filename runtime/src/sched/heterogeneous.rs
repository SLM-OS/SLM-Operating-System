//! Heterogeneous scheduling support for big.LITTLE systems.
//!
//! This module provides awareness of heterogeneous CPU architectures like
//! ARM big.LITTLE, where different cores have different performance and
//! power characteristics.
//!
//! # Architecture
//!
//! On the Jetson Orin Nano (Cortex-A78AE), all cores are identical. However,
//! this module is designed to support future heterogeneous systems and
//! provides the abstractions needed for optimal task placement.
//!
//! # Core Types
//!
//! - **Performance cores** (big): High clock speed, larger caches, better
//!   for compute-intensive inference tasks
//! - **Efficiency cores** (LITTLE): Lower power, suitable for background
//!   tasks and small models that fit in cache
//!
//! # Usage
//!
//! ```ignore
//! use crate::sched::heterogeneous::{CpuTopology, TaskPlacement};
//!
//! let topology = CpuTopology::detect();
//! let placement = TaskPlacement::for_inference(model_size, deadline);
//! let core_id = topology.select_core(&placement);
//! ```

use super::CoreType;

// =============================================================================
// Core Information
// =============================================================================

/// Information about a single CPU core.
#[derive(Debug, Clone, Copy)]
pub struct CoreInfo {
    /// Core ID (0-based).
    pub id: u8,
    /// Type of core (Performance or Efficiency).
    pub core_type: CoreType,
    /// Cluster ID this core belongs to.
    pub cluster_id: u8,
    /// Whether this core is currently online.
    pub online: bool,
    /// Relative performance level (1-100, higher is faster).
    pub performance_level: u8,
}

impl CoreInfo {
    /// Create a new CoreInfo.
    pub const fn new(id: u8, core_type: CoreType, cluster_id: u8) -> Self {
        Self {
            id,
            core_type,
            cluster_id,
            online: true,
            performance_level: match core_type {
                CoreType::Performance => 100,
                CoreType::Efficiency => 50,
                CoreType::Any => 75,
            },
        }
    }

    /// Check if this core is suitable for a given core type preference.
    pub fn matches(&self, preference: CoreType) -> bool {
        match preference {
            CoreType::Any => true,
            CoreType::Performance => self.core_type == CoreType::Performance,
            CoreType::Efficiency => self.core_type == CoreType::Efficiency,
        }
    }
}

// =============================================================================
// Cluster Information
// =============================================================================

/// A cluster of CPU cores with similar characteristics.
#[derive(Debug, Clone, Copy)]
pub struct ClusterInfo {
    /// Cluster ID.
    pub id: u8,
    /// Type of cores in this cluster.
    pub core_type: CoreType,
    /// First core ID in this cluster.
    pub first_core: u8,
    /// Number of cores in this cluster.
    pub core_count: u8,
}

impl ClusterInfo {
    /// Create a new ClusterInfo.
    pub const fn new(id: u8, core_type: CoreType, first_core: u8, core_count: u8) -> Self {
        Self {
            id,
            core_type,
            first_core,
            core_count,
        }
    }
}

// =============================================================================
// CPU Topology
// =============================================================================

/// Maximum number of cores supported.
pub const MAX_CORES: usize = 8;

/// Maximum number of clusters supported.
pub const MAX_CLUSTERS: usize = 2;

/// CPU topology for the system.
///
/// This is a skeleton implementation for Phase 3.
/// In Phase 5, this will read actual hardware topology from device tree.
#[derive(Debug)]
pub struct CpuTopology {
    /// Total number of cores.
    num_cores: u8,
    /// Number of clusters.
    num_clusters: u8,
    /// Core information.
    cores: [CoreInfo; MAX_CORES],
    /// Cluster information.
    clusters: [ClusterInfo; MAX_CLUSTERS],
    /// Whether this is a heterogeneous system.
    is_heterogeneous: bool,
}

impl CpuTopology {
    /// Create a homogeneous topology (all cores identical).
    ///
    /// This is used for systems like Jetson Orin Nano where all
    /// Cortex-A78AE cores are identical performance cores.
    pub fn homogeneous(num_cores: u8) -> Self {
        let mut cores = [CoreInfo::new(0, CoreType::Performance, 0); MAX_CORES];
        for i in 0..num_cores.min(MAX_CORES as u8) {
            cores[i as usize] = CoreInfo::new(i, CoreType::Performance, 0);
        }

        Self {
            num_cores,
            num_clusters: 1,
            cores,
            clusters: [
                ClusterInfo::new(0, CoreType::Performance, 0, num_cores),
                ClusterInfo::new(0, CoreType::Any, 0, 0), // Unused
            ],
            is_heterogeneous: false,
        }
    }

    /// Create a big.LITTLE topology.
    ///
    /// # Arguments
    /// * `big_cores` - Number of performance (big) cores
    /// * `little_cores` - Number of efficiency (LITTLE) cores
    pub fn big_little(big_cores: u8, little_cores: u8) -> Self {
        let total = big_cores + little_cores;
        let mut cores = [CoreInfo::new(0, CoreType::Any, 0); MAX_CORES];

        // Big cores first
        for i in 0..big_cores.min(MAX_CORES as u8) {
            cores[i as usize] = CoreInfo::new(i, CoreType::Performance, 0);
        }

        // LITTLE cores
        for i in 0..little_cores.min((MAX_CORES as u8).saturating_sub(big_cores)) {
            let idx = (big_cores + i) as usize;
            cores[idx] = CoreInfo::new(big_cores + i, CoreType::Efficiency, 1);
        }

        Self {
            num_cores: total.min(MAX_CORES as u8),
            num_clusters: 2,
            cores,
            clusters: [
                ClusterInfo::new(0, CoreType::Performance, 0, big_cores),
                ClusterInfo::new(1, CoreType::Efficiency, big_cores, little_cores),
            ],
            is_heterogeneous: true,
        }
    }

    /// Detect system topology.
    ///
    /// Currently returns a default 4-core homogeneous topology.
    /// In Phase 5, this will read from device tree or ACPI.
    pub fn detect() -> Self {
        // Default: assume 4 homogeneous performance cores (Jetson Orin Nano)
        Self::homogeneous(4)
    }

    /// Get the number of cores.
    pub fn num_cores(&self) -> u8 {
        self.num_cores
    }

    /// Get the number of clusters.
    pub fn num_clusters(&self) -> u8 {
        self.num_clusters
    }

    /// Check if system is heterogeneous.
    pub fn is_heterogeneous(&self) -> bool {
        self.is_heterogeneous
    }

    /// Get core information by ID.
    pub fn get_core(&self, id: u8) -> Option<&CoreInfo> {
        if id < self.num_cores {
            Some(&self.cores[id as usize])
        } else {
            None
        }
    }

    /// Get cluster information by ID.
    pub fn get_cluster(&self, id: u8) -> Option<&ClusterInfo> {
        if id < self.num_clusters {
            Some(&self.clusters[id as usize])
        } else {
            None
        }
    }

    /// Count cores of a specific type.
    pub fn count_cores(&self, core_type: CoreType) -> u8 {
        match core_type {
            CoreType::Any => self.num_cores,
            _ => {
                let mut count = 0;
                for i in 0..self.num_cores {
                    if self.cores[i as usize].core_type == core_type {
                        count += 1;
                    }
                }
                count
            }
        }
    }

    /// Select a core matching the given preference.
    ///
    /// Returns the core ID, or None if no suitable core is available.
    pub fn select_core(&self, preference: CoreType) -> Option<u8> {
        // Try to find an exact match first
        for i in 0..self.num_cores {
            let core = &self.cores[i as usize];
            if core.online && core.matches(preference) {
                return Some(i);
            }
        }

        // If no exact match, any online core will do
        if preference != CoreType::Any {
            for i in 0..self.num_cores {
                if self.cores[i as usize].online {
                    return Some(i);
                }
            }
        }

        None
    }

    /// Get the best core for compute-intensive tasks.
    pub fn best_performance_core(&self) -> Option<u8> {
        self.select_core(CoreType::Performance)
    }

    /// Get a core suitable for background/low-power tasks.
    pub fn best_efficiency_core(&self) -> Option<u8> {
        if self.is_heterogeneous {
            self.select_core(CoreType::Efficiency)
        } else {
            // On homogeneous systems, any core works
            self.select_core(CoreType::Any)
        }
    }
}

impl Default for CpuTopology {
    fn default() -> Self {
        Self::detect()
    }
}

// =============================================================================
// Task Placement Policy
// =============================================================================

/// Task placement preference for heterogeneous scheduling.
#[derive(Debug, Clone, Copy)]
pub struct TaskPlacement {
    /// Preferred core type.
    pub core_type: CoreType,
    /// Specific core ID to pin to (if any).
    pub pinned_core: Option<u8>,
    /// Specific cluster ID to prefer (if any).
    pub preferred_cluster: Option<u8>,
    /// Whether task can migrate between cores.
    pub allow_migration: bool,
}

impl TaskPlacement {
    /// Default placement (any core, allow migration).
    pub const DEFAULT: Self = Self {
        core_type: CoreType::Any,
        pinned_core: None,
        preferred_cluster: None,
        allow_migration: true,
    };

    /// Placement for high-priority inference tasks.
    pub const INFERENCE_HIGH: Self = Self {
        core_type: CoreType::Performance,
        pinned_core: None,
        preferred_cluster: Some(0), // Assume cluster 0 is performance
        allow_migration: false,     // Pin to avoid migration overhead
    };

    /// Placement for background tasks.
    pub const BACKGROUND: Self = Self {
        core_type: CoreType::Efficiency,
        pinned_core: None,
        preferred_cluster: None,
        allow_migration: true,
    };

    /// Create placement for an inference task based on model size and urgency.
    ///
    /// # Arguments
    /// * `model_size_bytes` - Size of the model working set
    /// * `is_urgent` - Whether task has a tight deadline
    pub fn for_inference(model_size_bytes: usize, is_urgent: bool) -> Self {
        // Threshold for "small" models that can run on efficiency cores
        const SMALL_MODEL_THRESHOLD: usize = 8 * 1024 * 1024; // 8 MB

        if is_urgent {
            Self::INFERENCE_HIGH
        } else if model_size_bytes < SMALL_MODEL_THRESHOLD {
            Self {
                core_type: CoreType::Efficiency,
                pinned_core: None,
                preferred_cluster: None,
                allow_migration: true,
            }
        } else {
            Self {
                core_type: CoreType::Performance,
                pinned_core: None,
                preferred_cluster: Some(0),
                allow_migration: true,
            }
        }
    }

    /// Pin task to a specific core.
    pub fn pin_to(core_id: u8) -> Self {
        Self {
            core_type: CoreType::Any,
            pinned_core: Some(core_id),
            preferred_cluster: None,
            allow_migration: false,
        }
    }

    /// Restrict task to a specific cluster.
    pub fn in_cluster(cluster_id: u8, core_type: CoreType) -> Self {
        Self {
            core_type,
            pinned_core: None,
            preferred_cluster: Some(cluster_id),
            allow_migration: true,
        }
    }
}

impl Default for TaskPlacement {
    fn default() -> Self {
        Self::DEFAULT
    }
}

// =============================================================================
// Load Balancing
// =============================================================================

/// Per-core load tracking for placement decisions.
#[derive(Debug, Clone, Copy, Default)]
pub struct CoreLoad {
    /// Current load percentage (0-100).
    pub load_percent: u8,
    /// Number of tasks currently assigned.
    pub task_count: u16,
    /// Whether core is accepting new tasks.
    pub accepting: bool,
}

/// System-wide load balancer state.
///
/// This is a skeleton implementation. In Phase 5, this will integrate
/// with the kernel scheduler for real load tracking.
#[derive(Debug)]
pub struct LoadBalancer {
    /// CPU topology.
    topology: CpuTopology,
    /// Per-core load information.
    core_loads: [CoreLoad; MAX_CORES],
}

impl LoadBalancer {
    /// Create a new load balancer for the given topology.
    pub fn new(topology: CpuTopology) -> Self {
        let mut core_loads = [CoreLoad::default(); MAX_CORES];
        for i in 0..topology.num_cores() as usize {
            core_loads[i].accepting = true;
        }

        Self {
            topology,
            core_loads,
        }
    }

    /// Create with auto-detected topology.
    pub fn detect() -> Self {
        Self::new(CpuTopology::detect())
    }

    /// Get the CPU topology.
    pub fn topology(&self) -> &CpuTopology {
        &self.topology
    }

    /// Update load for a core.
    pub fn update_load(&mut self, core_id: u8, load_percent: u8, task_count: u16) {
        if (core_id as usize) < MAX_CORES {
            self.core_loads[core_id as usize].load_percent = load_percent;
            self.core_loads[core_id as usize].task_count = task_count;
        }
    }

    /// Select the best core for a task based on placement preferences and load.
    ///
    /// Returns the core ID, or None if no suitable core is found.
    pub fn select_core(&self, placement: &TaskPlacement) -> Option<u8> {
        // If pinned to specific core, use that
        if let Some(core_id) = placement.pinned_core {
            if core_id < self.topology.num_cores() {
                return Some(core_id);
            }
        }

        // Find least-loaded core matching preferences
        let mut best_core: Option<u8> = None;
        let mut best_load: u8 = u8::MAX;

        for i in 0..self.topology.num_cores() {
            let core = self.topology.get_core(i).unwrap();
            let load = &self.core_loads[i as usize];

            // Skip offline or non-accepting cores
            if !core.online || !load.accepting {
                continue;
            }

            // Check cluster preference
            if let Some(cluster) = placement.preferred_cluster {
                if core.cluster_id != cluster {
                    continue;
                }
            }

            // Check core type preference
            if !core.matches(placement.core_type) {
                continue;
            }

            // Track least-loaded matching core
            if load.load_percent < best_load {
                best_load = load.load_percent;
                best_core = Some(i);
            }
        }

        // If no match found, try relaxed search (any online core)
        if best_core.is_none() && placement.core_type != CoreType::Any {
            best_load = u8::MAX;
            for i in 0..self.topology.num_cores() {
                let core = self.topology.get_core(i).unwrap();
                let load = &self.core_loads[i as usize];

                if core.online && load.accepting && load.load_percent < best_load {
                    best_load = load.load_percent;
                    best_core = Some(i);
                }
            }
        }

        best_core
    }
}

impl Default for LoadBalancer {
    fn default() -> Self {
        Self::detect()
    }
}

// =============================================================================
// FFI Exports
// =============================================================================

/// Get the number of CPU cores.
#[no_mangle]
pub extern "C" fn rust_cpu_num_cores() -> u8 {
    CpuTopology::detect().num_cores()
}

/// Check if the system is heterogeneous.
#[no_mangle]
pub extern "C" fn rust_cpu_is_heterogeneous() -> bool {
    CpuTopology::detect().is_heterogeneous()
}

/// Get the recommended core for an inference task.
///
/// # Arguments
/// * `model_size` - Size of model in bytes
/// * `is_urgent` - Non-zero if task has tight deadline
///
/// # Returns
/// Core ID to use, or 0xFF if no recommendation.
#[no_mangle]
pub extern "C" fn rust_select_inference_core(model_size: usize, is_urgent: u8) -> u8 {
    let topology = CpuTopology::detect();
    let placement = TaskPlacement::for_inference(model_size, is_urgent != 0);
    topology.select_core(placement.core_type).unwrap_or(0xFF)
}
