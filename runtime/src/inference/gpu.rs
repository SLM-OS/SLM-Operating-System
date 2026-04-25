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
/// Heuristic: route MatMul/Gemm/Conv to GPU when input size justifies
/// the dispatch overhead. Thresholds are deliberately low: MNIST's FC
/// layer is 1×256×10 = 2,560 input elements, and routing it to GPU
/// validates the M7 wiring even though a per-op dispatch costs more
/// than batching the whole graph. The graph-level fast path
/// (`run_mnist_gpu_fastpath`) takes precedence over per-op dispatch
/// when the engine sees the MNIST signature.
///
/// Note: this threshold is currently inert in production. Per-op
/// `gpu_execute_matmul` is still a stub returning `Err(NotReady)`,
/// so for non-MNIST workloads the engine always falls through to
/// the CPU path regardless of what `select_backend` returns. The
/// threshold becomes load-bearing only when a per-op GPU dispatch
/// path lands; it should be re-tuned on real hardware then.
pub fn select_backend(
    op: OpType,
    input_elements: usize,
    caps: &GpuCapabilities,
) -> Backend {
    if !caps.has_compute() {
        return Backend::Cpu;
    }

    match op {
        OpType::MatMul | OpType::Gemm if input_elements >= 256 => Backend::Gpu,
        OpType::Conv if input_elements >= 256 => Backend::Gpu,
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
    DispatchFailed(i32),
}

/// Per-op MatMul on GPU. Today this is a stub: SLM-OS's GA10B
/// dispatch path only exposes whole-graph MNIST inference via
/// `run_mnist_gpu_fastpath`. Once a per-op dispatch lands (a
/// dedicated GEMM kernel handoff that doesn't require pre-uploading
/// the entire MNIST pipeline pre-kexec), this can call into it.
/// Today returns `Err(NotReady)` so the engine falls back to CPU.
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

/// Whole-graph MNIST GPU dispatch: swap the input tensor in via
/// `slm_gpu_set_mnist_input`, then run the pre-uploaded 8-op pipeline
/// via `slm_gpu_run_mnist`, then copy the resulting 10 logits to
/// `output`. Returns the number of output floats written (always 10
/// on success).
///
/// Preconditions enforced by the caller (`engine::run_inference`):
/// the active model is `mnist`, GPU compute is ready, and the input
/// is exactly 1×1×28×28 = 784 fp32 values.
///
/// On failure returns `Err(GpuError::DispatchFailed(rc))` so the
/// caller can fall back to the CPU path.
pub fn run_mnist_gpu_fastpath(
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
) -> Result<usize, GpuError> {
    const MNIST_INPUT_FLOATS: usize = 1 * 1 * 28 * 28;
    const MNIST_LOGIT_COUNT: usize = 10;

    if input_len != MNIST_INPUT_FLOATS {
        return Err(GpuError::NotReady);
    }
    if output_len < MNIST_LOGIT_COUNT {
        return Err(GpuError::NotReady);
    }
    if input.is_null() || output.is_null() {
        return Err(GpuError::NotReady);
    }

    // Reinterpret the f32 input slice as raw bytes for the FFI.
    // SAFETY: input is non-null and we've checked it covers
    // MNIST_INPUT_FLOATS f32 values. The byte view aliases the same
    // memory for the duration of the FFI call.
    let input_bytes: &[u8] = unsafe {
        core::slice::from_raw_parts(
            input as *const u8,
            MNIST_INPUT_FLOATS * core::mem::size_of::<f32>(),
        )
    };
    let rc = kernel_ffi::gpu_set_mnist_input(input_bytes);
    if rc < 0 {
        return Err(GpuError::DispatchFailed(rc));
    }

    let mut logits = [0f32; MNIST_LOGIT_COUNT];
    let rc = kernel_ffi::gpu_run_mnist(&mut logits);
    if rc < 0 {
        return Err(GpuError::DispatchFailed(rc));
    }

    // SAFETY: output is non-null and output_len ≥ MNIST_LOGIT_COUNT
    // (checked above). copy_nonoverlapping is sound because logits
    // is a stack array distinct from the output buffer.
    unsafe {
        core::ptr::copy_nonoverlapping(
            logits.as_ptr(),
            output,
            MNIST_LOGIT_COUNT,
        );
    }
    Ok(MNIST_LOGIT_COUNT)
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
