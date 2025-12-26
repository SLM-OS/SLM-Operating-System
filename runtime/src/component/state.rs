//! Component State and Info Types
//!
//! Defines the lifecycle states and metadata for components.

use super::{MAX_NAME_LEN, MAX_DESC_LEN, MAX_VERSION_LEN};

/// Component lifecycle states.
///
/// See docs/components.md for state transition diagram.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ComponentState {
    /// Component binary loaded into memory
    Loaded = 0,

    /// Component's init function is running
    Initializing = 1,

    /// Component is active and processing
    Running = 2,

    /// Component temporarily paused
    Suspended = 3,

    /// Component being replaced by new version
    Updating = 4,

    /// Component shutdown in progress
    Terminating = 5,

    /// Component resources freed
    Unloaded = 6,
}

impl ComponentState {
    /// Convert from u8 value.
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(ComponentState::Loaded),
            1 => Some(ComponentState::Initializing),
            2 => Some(ComponentState::Running),
            3 => Some(ComponentState::Suspended),
            4 => Some(ComponentState::Updating),
            5 => Some(ComponentState::Terminating),
            6 => Some(ComponentState::Unloaded),
            _ => None,
        }
    }

    /// Get state name as string.
    pub fn as_str(&self) -> &'static str {
        match self {
            ComponentState::Loaded => "loaded",
            ComponentState::Initializing => "initializing",
            ComponentState::Running => "running",
            ComponentState::Suspended => "suspended",
            ComponentState::Updating => "updating",
            ComponentState::Terminating => "terminating",
            ComponentState::Unloaded => "unloaded",
        }
    }
}

/// Component type classification.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ComponentType {
    /// Background service providing functionality
    Service = 0,

    /// Hardware interface component
    Driver = 1,

    /// User-facing application or pipeline
    Application = 2,
}

impl ComponentType {
    /// Convert from u8 value.
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(ComponentType::Service),
            1 => Some(ComponentType::Driver),
            2 => Some(ComponentType::Application),
            _ => None,
        }
    }

    /// Get type name as string.
    pub fn as_str(&self) -> &'static str {
        match self {
            ComponentType::Service => "service",
            ComponentType::Driver => "driver",
            ComponentType::Application => "application",
        }
    }
}

/// Priority levels for component tasks.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Priority {
    Idle = 0,
    Low = 2,
    Normal = 4,
    High = 6,
    Critical = 7,
}

impl Priority {
    /// Convert from u8 value.
    pub fn from_u8(value: u8) -> Self {
        match value {
            0 => Priority::Idle,
            1 | 2 => Priority::Low,
            3 | 4 => Priority::Normal,
            5 | 6 => Priority::High,
            7 => Priority::Critical,
            _ => Priority::Normal,
        }
    }

    /// Convert to u8 value.
    pub fn as_u8(&self) -> u8 {
        *self as u8
    }

    /// Get priority name as string.
    pub fn as_str(&self) -> &'static str {
        match self {
            Priority::Idle => "idle",
            Priority::Low => "low",
            Priority::Normal => "normal",
            Priority::High => "high",
            Priority::Critical => "critical",
        }
    }
}

/// Component metadata and runtime state.
///
/// This structure is passed across the FFI boundary, so it uses
/// fixed-size arrays instead of heap allocation.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct ComponentInfo {
    /// Component name (null-terminated)
    pub name: [u8; MAX_NAME_LEN],

    /// Version string (null-terminated)
    pub version: [u8; MAX_VERSION_LEN],

    /// Short description (null-terminated)
    pub description: [u8; MAX_DESC_LEN],

    /// Component type
    pub component_type: ComponentType,

    /// Task priority
    pub priority: Priority,

    /// Current lifecycle state
    pub state: ComponentState,

    /// Padding for alignment
    _pad: u8,

    /// Task ID (0 = no task)
    pub task_id: u32,

    /// Memory usage in KB
    pub memory_kb: u32,

    /// Context switches count
    pub switches: u64,
}

impl ComponentInfo {
    /// Create a new, empty component info.
    pub const fn new() -> Self {
        ComponentInfo {
            name: [0; MAX_NAME_LEN],
            version: [0; MAX_VERSION_LEN],
            description: [0; MAX_DESC_LEN],
            component_type: ComponentType::Service,
            priority: Priority::Normal,
            state: ComponentState::Unloaded,
            _pad: 0,
            task_id: 0,
            memory_kb: 0,
            switches: 0,
        }
    }

    /// Set the component name from a byte slice.
    pub fn set_name(&mut self, name: &[u8]) {
        let len = core::cmp::min(name.len(), MAX_NAME_LEN - 1);
        self.name[..len].copy_from_slice(&name[..len]);
        self.name[len] = 0; // Null terminate
    }

    /// Set the version from a byte slice.
    pub fn set_version(&mut self, version: &[u8]) {
        let len = core::cmp::min(version.len(), MAX_VERSION_LEN - 1);
        self.version[..len].copy_from_slice(&version[..len]);
        self.version[len] = 0; // Null terminate
    }

    /// Set the description from a byte slice.
    pub fn set_description(&mut self, desc: &[u8]) {
        let len = core::cmp::min(desc.len(), MAX_DESC_LEN - 1);
        self.description[..len].copy_from_slice(&desc[..len]);
        self.description[len] = 0; // Null terminate
    }

    /// Get name as a string slice.
    pub fn name_str(&self) -> &str {
        let len = self.name.iter().position(|&b| b == 0).unwrap_or(MAX_NAME_LEN);
        // SAFETY: We control the name bytes and ensure they're valid ASCII
        unsafe { core::str::from_utf8_unchecked(&self.name[..len]) }
    }

    /// Get version as a string slice.
    pub fn version_str(&self) -> &str {
        let len = self.version.iter().position(|&b| b == 0).unwrap_or(MAX_VERSION_LEN);
        unsafe { core::str::from_utf8_unchecked(&self.version[..len]) }
    }

    /// Check if this slot is in use (has a valid component).
    pub fn is_active(&self) -> bool {
        self.state != ComponentState::Unloaded && self.name[0] != 0
    }
}

impl Default for ComponentInfo {
    fn default() -> Self {
        Self::new()
    }
}
