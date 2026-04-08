//! GPU compute backend for inference.
//!
//! Provides GPU capability detection, operator placement decisions, and
//! stub GPU operator dispatch. All GPU operations gracefully fall back
//! to CPU when GPU compute is unavailable (requires GSP firmware).
//!
//! # Current Status
//!
//! - QEMU: `GpuStatus::NotAvailable` — no GPU hardware
//! - Jetson: `GpuStatus::DetectedNoCompute` — GPU probed but GSP not loaded
//! - Future: `GpuStatus::ComputeReady` — when GSP firmware loading is implemented

use crate::kernel_ffi;
use crate::loader::graph::OpType;

// GPU capability flags (mirrors kernel GPU_CAP_* constants)
pub const GPU_CAP_COMPUTE: u32 = 0x01;
pub const GPU_CAP_TENSOR_CORES: u32 = 0x02;
pub const GPU_CAP_UNIFIED_MEMORY: u32 = 0x08;

/// GPU availability status for inference decisions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuStatus {
    /// No GPU driver or hardware detected.
    NotAvailable,
    /// GPU detected but compute not ready (Jetson without GSP).
    DetectedNoCompute,
    /// GPU ready for compute (future: GSP loaded).
    ComputeReady,
}

/// GPU capabilities for operator placement decisions.
#[derive(Debug, Clone, Copy)]
pub struct GpuCapabilities {
    pub status: GpuStatus,
    pub capabilities: u32,
    pub cuda_cores: u32,
    pub tensor_cores: u32,
    pub unified_memory: bool,
    pub name: [u8; 32],
    pub device: [u8; 64],
}

impl GpuCapabilities {
    pub const NONE: Self = Self {
        status: GpuStatus::NotAvailable,
        capabilities: 0,
        cuda_cores: 0,
        tensor_cores: 0,
        unified_memory: false,
        name: [0; 32],
        device: [0; 64],
    };

    /// Detect GPU capabilities via kernel FFI.
    pub fn detect() -> Self {
        if !kernel_ffi::gpu_available() {
            return Self::NONE;
        }

        let info = kernel_ffi::gpu_get_info();

        let status = if info.compute_ready != 0 {
            GpuStatus::ComputeReady
        } else if info.capabilities & GPU_CAP_COMPUTE != 0 {
            GpuStatus::DetectedNoCompute
        } else {
            GpuStatus::NotAvailable
        };

        Self {
            status,
            capabilities: info.capabilities,
            cuda_cores: info.cuda_cores,
            tensor_cores: info.tensor_cores,
            unified_memory: info.unified_memory != 0,
            name: info.name,
            device: info.device,
        }
    }

    /// Whether GPU compute is available for inference.
    pub fn has_compute(&self) -> bool {
        self.status == GpuStatus::ComputeReady
    }

    /// Whether the GPU has tensor cores.
    pub fn has_tensor_cores(&self) -> bool {
        self.capabilities & GPU_CAP_TENSOR_CORES != 0
    }
}

/// Compute backend selection.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Backend {
    Cpu,
    Gpu,
}

/// Select the best backend for an operator.
///
/// Heuristic: route large MatMul/Gemm/Conv to GPU (if available),
/// keep small or element-wise ops on CPU to avoid transfer overhead.
pub fn select_backend(
    op: OpType,
    input_elements: usize,
    caps: &GpuCapabilities,
) -> Backend {
    if !caps.has_compute() {
        return Backend::Cpu;
    }

    // Thresholds for GPU offload (preliminary, to be tuned with real hardware)
    match op {
        OpType::MatMul | OpType::Gemm if input_elements > 4096 => Backend::Gpu,
        OpType::Conv if input_elements > 8192 => Backend::Gpu,
        _ => Backend::Cpu,
    }
}

/// Errors from GPU compute operations.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuError {
    NotAvailable,
    NotReady,
    OutOfMemory,
    SyncFailed,
}

/// Stub: Execute MatMul on GPU.
///
/// When GSP firmware is loaded, this would:
/// 1. Ensure inputs are synced for GPU (cache clean)
/// 2. Submit matmul command via gpu_submit()
/// 3. Wait for completion via gpu_wait()
/// 4. Sync output for CPU (cache invalidate)
///
/// Currently always returns `Err(NotReady)`, forcing CPU fallback.
pub fn gpu_execute_matmul(
    _a_ptr: *const f32,
    _b_ptr: *const f32,
    _out_ptr: *mut f32,
    _m: usize,
    _k: usize,
    _n: usize,
) -> Result<(), GpuError> {
    Err(GpuError::NotReady)
}

/// Make a model's weights GPU-accessible by flushing CPU caches.
pub fn gpu_map_weights(model_index: usize) -> Result<(), GpuError> {
    use crate::loader::registry;
    use crate::mm;

    let weight_handle = registry::get_weights(model_index)
        .ok_or(GpuError::NotAvailable)?;
    let ptr = mm::get_ptr(weight_handle)
        .ok_or(GpuError::NotAvailable)?;
    let size = mm::get_size(weight_handle)
        .ok_or(GpuError::NotAvailable)?;

    // Flush CPU caches so GPU sees current weight data
    unsafe {
        kernel_ffi::slm_gpu_sync_for_device(ptr, size);
    }
    Ok(())
}

/// Release GPU access to model weights by invalidating CPU caches.
pub fn gpu_unmap_weights(model_index: usize) -> Result<(), GpuError> {
    use crate::loader::registry;
    use crate::mm;

    let weight_handle = registry::get_weights(model_index)
        .ok_or(GpuError::NotAvailable)?;
    let ptr = mm::get_ptr(weight_handle)
        .ok_or(GpuError::NotAvailable)?;
    let size = mm::get_size(weight_handle)
        .ok_or(GpuError::NotAvailable)?;

    // Invalidate CPU caches in case GPU modified data
    unsafe {
        kernel_ffi::slm_gpu_sync_for_cpu(ptr, size);
    }
    Ok(())
}
