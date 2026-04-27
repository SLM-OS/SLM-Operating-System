//! Model registry — static storage for loaded ONNX models.
//!
//! Follows the same pattern as `component/registry.rs`: a fixed-size array
//! of model entries protected by a spinlock.

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::mm::{self, ModelHandle};
use crate::mm::model_loader::LoadError;
use super::onnx_parser;
use super::graph::{OperatorGraph, WeightTable, WeightEntry, TensorName, TensorShape, ElemType};

/// Maximum simultaneously loaded models.
pub const MAX_MODELS: usize = 8;

/// Convert an IEEE 754 half-precision (FP16) value to single-precision (FP32).
///
/// FP16: 1 sign + 5 exponent (bias 15) + 10 mantissa
/// FP32: 1 sign + 8 exponent (bias 127) + 23 mantissa
pub(crate) fn fp16_to_f32(half: u16) -> f32 {
    let sign = ((half >> 15) & 1) as u32;
    let exp = ((half >> 10) & 0x1F) as u32;
    let mant = (half & 0x3FF) as u32;

    let f32_bits = if exp == 0 {
        if mant == 0 {
            // Zero (positive or negative)
            sign << 31
        } else {
            // Subnormal: normalize to FP32
            let mut m = mant;
            let mut e: i32 = -14; // FP16 subnormal exponent
            while (m & 0x400) == 0 {
                m <<= 1;
                e -= 1;
            }
            m &= 0x3FF; // Remove implicit 1
            let fp32_exp = ((e + 127) as u32) & 0xFF;
            (sign << 31) | (fp32_exp << 23) | (m << 13)
        }
    } else if exp == 0x1F {
        // Inf or NaN
        (sign << 31) | (0xFF << 23) | (mant << 13)
    } else {
        // Normal: rebias exponent from 15 to 127
        let fp32_exp = exp - 15 + 127;
        (sign << 31) | (fp32_exp << 23) | (mant << 13)
    };

    f32::from_bits(f32_bits)
}

/// Model name length (matches C-side struct).
pub const MODEL_NAME_LEN: usize = 32;

// =============================================================================
// FFI-safe model metadata
// =============================================================================

/// Model info struct passed to C via FFI.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ModelInfoC {
    pub name: [u8; MODEL_NAME_LEN],
    pub format: u8,       // 0=GGUF, 1=ONNX, 2=Raw
    pub _pad: [u8; 3],
    pub param_count: u64,
    pub weight_size: u64,
    pub workspace_size: u64,
    pub node_count: u32,
    pub input_count: u32,
    pub output_count: u32,
    pub pinned: u8,       // 1 if pinned, 0 if evictable (#37)
    pub _pad2: [u8; 3],
    pub use_count: u32,   // Number of inference calls (#37)
    pub last_used_ms: u32, // ms since boot of last access (#37)
}

impl ModelInfoC {
    pub const EMPTY: Self = Self {
        name: [0; MODEL_NAME_LEN],
        format: 0,
        _pad: [0; 3],
        param_count: 0,
        weight_size: 0,
        workspace_size: 0,
        node_count: 0,
        input_count: 0,
        output_count: 0,
        pinned: 0,
        _pad2: [0; 3],
        use_count: 0,
        last_used_ms: 0,
    };
}

// =============================================================================
// Registry internals
// =============================================================================

/// A loaded model entry in the registry.
struct LoadedModelEntry {
    name: [u8; MODEL_NAME_LEN],
    weights: ModelHandle,
    workspace: ModelHandle,
    graph: OperatorGraph,
    weight_table: WeightTable,
    info: ModelInfoC,
    active: bool,
    last_used: u64,   // Timestamp from slm_get_time_ns for LRU eviction
    use_count: u32,   // Number of inference calls
    pinned: bool,     // If true, cannot be evicted by LRU
    /// Per-model GPU-dispatch toggle. Layered on top of the master
    /// `slm_gpu_inference_enabled()` flag — the engine fastpath
    /// requires both to be true. Default ON at load so flipping
    /// just the master switch enables every loaded model; flip
    /// per-model OFF via `model use-gpu <name|idx> off` to force
    /// a specific model back to CPU without disturbing the master.
    gpu_dispatch_enabled: bool,
}

impl LoadedModelEntry {
    const fn empty() -> Self {
        Self {
            name: [0; MODEL_NAME_LEN],
            weights: ModelHandle::null(),
            workspace: ModelHandle::null(),
            graph: OperatorGraph::EMPTY,
            weight_table: WeightTable::EMPTY,
            info: ModelInfoC::EMPTY,
            active: false,
            last_used: 0,
            use_count: 0,
            pinned: false,
            gpu_dispatch_enabled: true,
        }
    }
}

/// The model registry state.
struct ModelRegistry {
    entries: [LoadedModelEntry; MAX_MODELS],
    initialized: bool,
}

impl ModelRegistry {
    const fn new() -> Self {
        Self {
            entries: [
                LoadedModelEntry::empty(),
                LoadedModelEntry::empty(),
                LoadedModelEntry::empty(),
                LoadedModelEntry::empty(),
                LoadedModelEntry::empty(),
                LoadedModelEntry::empty(),
                LoadedModelEntry::empty(),
                LoadedModelEntry::empty(),
            ],
            initialized: false,
        }
    }
}

/// SyncWrapper for static storage (same pattern as component/registry.rs).
/// SAFETY: Access is protected by LOCK spinlock.
#[repr(transparent)]
struct SyncWrapper<T>(UnsafeCell<T>);

unsafe impl<T> Sync for SyncWrapper<T> {}

impl<T> SyncWrapper<T> {
    const fn new(value: T) -> Self {
        SyncWrapper(UnsafeCell::new(value))
    }

    fn get(&self) -> *mut T {
        self.0.get()
    }
}

static REGISTRY: SyncWrapper<ModelRegistry> = SyncWrapper::new(ModelRegistry::new());
static LOCK: AtomicBool = AtomicBool::new(false);

/// RAII guard for the registry spinlock.
///
/// Released on drop so early returns via `?` or panics (even with
/// `panic = "abort"`, this remains the safer pattern) can't leak the
/// lock.
struct SpinGuard;

impl SpinGuard {
    fn new() -> Self {
        while LOCK
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        SpinGuard
    }
}

impl Drop for SpinGuard {
    fn drop(&mut self) {
        LOCK.store(false, Ordering::Release);
    }
}

// =============================================================================
// Public API
// =============================================================================

/// Initialize the model registry.
pub fn init() {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to REGISTRY.
    unsafe {
        let reg = &mut *REGISTRY.get();
        if !reg.initialized {
            for entry in reg.entries.iter_mut() {
                *entry = LoadedModelEntry::empty();
            }
            reg.initialized = true;
        }
    }
}

/// Load an ONNX model from a buffer.
///
/// Returns the registry slot index on success.
pub fn load_model(name: &[u8], data: &[u8]) -> Result<usize, LoadError> {
    // Parse the ONNX protobuf
    let parsed = onnx_parser::parse_onnx(data).map_err(|_| LoadError::CorruptedData)?;

    // Build the operator graph
    let graph = onnx_parser::build_graph(&parsed)?;

    // Calculate total weight size (expanded for FP16→FP32 conversion)
    let total_weight_size = parsed.total_weight_size_expanded();
    if total_weight_size == 0 {
        return Err(LoadError::InvalidFormat);
    }

    // Allocate weight memory from pool
    let weights = mm::alloc_weights(total_weight_size)?;

    // Copy weight data into allocated block
    let weight_ptr = match mm::get_ptr(weights) {
        Some(ptr) => ptr,
        None => {
            let _ = mm::free(weights);
            return Err(LoadError::AllocFailed);
        }
    };

    // Copy each initializer's data into the weight block and build weight table
    let mut weight_table = WeightTable::EMPTY;
    let mut offset: usize = 0;
    for i in 0..parsed.initializer_count {
        let tensor = &parsed.initializers[i];
        let data_size = tensor.data_size();
        if data_size == 0 {
            continue;
        }

        // Determine data source and handle varint-encoded int64_data specially.
        // int64_data is varint-encoded in protobuf, but we need raw little-endian
        // bytes for the inference engine to read directly as *const i64.
        // Static buffer to avoid stack allocation (load_model already heavy on stack)
        static mut I64_DECODE_BUF: [u8; 128] = [0u8; 128];
        let src = if let Some(raw) = tensor.raw_data {
            raw
        } else if let Some(floats) = tensor.float_data {
            floats
        } else if let Some(i64_packed) = tensor.int64_data {
            // Decode varint-encoded int64 values to raw little-endian bytes.
            // SAFETY: load_model is serialized by the registry lock, so
            // I64_DECODE_BUF is not concurrently accessed.
            unsafe {
                let max_items = I64_DECODE_BUF.len() / 8;
                let mut item_count: usize = 0;
                for val in super::protobuf::packed_varint_i64(i64_packed) {
                    if item_count >= max_items {
                        break;
                    }
                    if let Ok(v) = val {
                        let bytes = (v as i64).to_le_bytes();
                        let off = item_count * 8;
                        I64_DECODE_BUF[off..off + 8].copy_from_slice(&bytes);
                        item_count += 1;
                    }
                }
                &I64_DECODE_BUF[..item_count * 8]
            }
        } else {
            continue;
        };

        // Check if FP16→FP32 conversion is needed
        let is_fp16 = tensor.data_type == super::onnx_parser::OnnxDataType::Float16;
        let stored_size = if is_fp16 {
            // FP16: each 2-byte element becomes 4 bytes
            (src.len() / 2) * 4
        } else {
            src.len()
        };

        // Record weight entry in table
        if weight_table.count < super::graph::MAX_WEIGHT_ENTRIES {
            let mut shape = TensorShape::EMPTY;
            let ndim = core::cmp::min(tensor.shape.ndim as usize, 8);
            for d in 0..ndim {
                shape.dims[d] = if tensor.shape.dims[d] > 0 {
                    tensor.shape.dims[d] as u32
                } else {
                    1
                };
            }
            shape.ndim = ndim as u8;
            // FP16 weights are stored as FP32 after conversion
            shape.elem_type = if is_fp16 {
                ElemType::Float
            } else {
                ElemType::from_onnx(tensor.data_type as u32)
            };

            weight_table.entries[weight_table.count] = WeightEntry {
                name: TensorName::from_bytes(tensor.name.as_bytes()),
                offset: offset as u32,
                size: stored_size as u32,
                shape,
            };
            weight_table.count += 1;
        }

        // SAFETY: weight_ptr is valid for total_weight_size bytes from alloc_weights.
        unsafe {
            let dest = weight_ptr.add(offset);
            if is_fp16 {
                // Convert FP16 → FP32 in-place during copy
                let n_elements = src.len() / 2;
                let dest_f32 = dest as *mut f32;
                for e in 0..n_elements {
                    let half = u16::from_le_bytes([src[e * 2], src[e * 2 + 1]]);
                    *dest_f32.add(e) = fp16_to_f32(half);
                }
            } else {
                core::ptr::copy_nonoverlapping(src.as_ptr(), dest, src.len());
            }
        }
        offset += stored_size;
    }

    // Estimate workspace: max intermediate tensor size (rough heuristic)
    // Use 25% of weight size or minimum 64KB
    let workspace_size = core::cmp::max(total_weight_size / 4, 64 * 1024);
    let workspace = match mm::alloc_workspace(workspace_size) {
        Ok(w) => w,
        Err(e) => {
            let _ = mm::free(weights);
            return Err(e.into());
        }
    };

    // Count total parameters
    let mut param_count: u64 = 0;
    for i in 0..parsed.initializer_count {
        param_count += parsed.initializers[i].num_elements() as u64;
    }

    // Build metadata
    let mut info = ModelInfoC::EMPTY;
    let name_len = core::cmp::min(name.len(), MODEL_NAME_LEN - 1);
    info.name[..name_len].copy_from_slice(&name[..name_len]);
    info.format = 1; // ONNX
    info.param_count = param_count;
    info.weight_size = total_weight_size as u64;
    info.workspace_size = workspace_size as u64;
    info.node_count = graph.node_count as u32;
    info.input_count = graph.input_count as u32;
    info.output_count = graph.output_count as u32;

    // Store in registry
    let result = {
        let _g = SpinGuard::new();
        // SAFETY: SpinGuard held — exclusive access to REGISTRY.
        unsafe {
            let reg = &mut *REGISTRY.get();

        // Find a free slot
        let mut slot_idx = None;
        for (i, entry) in reg.entries.iter().enumerate() {
            if !entry.active {
                slot_idx = Some(i);
                break;
            }
        }

        // If no free slot, try LRU eviction
        if slot_idx.is_none() {
            let mut lru_idx: Option<usize> = None;
            let mut lru_time: u64 = u64::MAX;
            for (i, entry) in reg.entries.iter().enumerate() {
                if entry.active && !entry.pinned && entry.last_used < lru_time {
                    lru_time = entry.last_used;
                    lru_idx = Some(i);
                }
            }
            if let Some(evict_idx) = lru_idx {
                // Evict: free memory
                let evicted = &mut reg.entries[evict_idx];
                let _ = mm::free(evicted.weights);
                let _ = mm::free(evicted.workspace);
                *evicted = LoadedModelEntry::empty();
                slot_idx = Some(evict_idx);
            }
        }

        match slot_idx {
            Some(idx) => {
                let mut entry_name = [0u8; MODEL_NAME_LEN];
                entry_name[..name_len].copy_from_slice(&name[..name_len]);

                // Start from `empty()` and override only the fields that
                // differ — keeps the post-load default in one place
                // (`LoadedModelEntry::empty`) so a future change to e.g.
                // the per-model GPU-dispatch default doesn't have to be
                // mirrored at both sites.
                let mut entry = LoadedModelEntry::empty();
                entry.name = entry_name;
                entry.weights = weights;
                entry.workspace = workspace;
                entry.graph = graph;
                entry.weight_table = weight_table;
                entry.info = info;
                entry.active = true;
                entry.last_used = crate::kernel_ffi::get_time_ns();
                reg.entries[idx] = entry;
                Ok(idx)
            }
                None => Err(LoadError::ModelTooLarge), // All slots pinned
            }
        }
    };

    if result.is_err() {
        // Clean up on failure
        let _ = mm::free(weights);
        let _ = mm::free(workspace);
    }

    result
}

/// Touch a model (update last_used timestamp and use_count).
/// Called on each inference to maintain LRU ordering.
pub fn touch_model(index: usize) {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to REGISTRY.
    unsafe {
        let reg = &mut *REGISTRY.get();
        if index < MAX_MODELS && reg.entries[index].active {
            reg.entries[index].last_used = crate::kernel_ffi::get_time_ns();
            reg.entries[index].use_count += 1;
        }
    }
}

/// Pin a model to prevent LRU eviction.
pub fn pin_model(index: usize) -> bool {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to REGISTRY.
    unsafe {
        let reg = &mut *REGISTRY.get();
        if index < MAX_MODELS && reg.entries[index].active {
            reg.entries[index].pinned = true;
            true
        } else {
            false
        }
    }
}

/// Set the per-model GPU-dispatch toggle. Returns true if the index
/// is a loaded model and the flag was updated; false otherwise.
pub fn set_gpu_dispatch_enabled(index: usize, enabled: bool) -> bool {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to REGISTRY.
    unsafe {
        let reg = &mut *REGISTRY.get();
        if index < MAX_MODELS && reg.entries[index].active {
            reg.entries[index].gpu_dispatch_enabled = enabled;
            true
        } else {
            false
        }
    }
}

/// Read the per-model GPU-dispatch toggle. Returns false (CPU-only)
/// for any index outside the active set, so a stale call from a
/// pre-unload code path can't accidentally enable GPU dispatch.
pub fn gpu_dispatch_enabled(index: usize) -> bool {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        index < MAX_MODELS
            && reg.entries[index].active
            && reg.entries[index].gpu_dispatch_enabled
    }
}

/// Total flat fp32 element count expected for the model's input
/// tensor(s). Used by the shell `model infer-file` command to reject
/// shape-mismatched files before handing them to the engine, so an
/// operator-readable error replaces the engine's opaque `EngineError`.
///
/// Returns `None` for an inactive index. Returns `Some(0)` only if
/// the graph carries a zero-product shape, which the loader should
/// have already rejected — callers should treat `Some(0)` the same
/// as `None`.
pub fn expected_input_floats(index: usize) -> Option<usize> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        if index >= MAX_MODELS || !reg.entries[index].active {
            return None;
        }
        let graph = &reg.entries[index].graph;
        let mut total: usize = 0;
        for i in 0..graph.input_count {
            let shape = &graph.input_shapes[i];
            let ndim = shape.ndim as usize;
            let mut elems: usize = 1;
            for d in 0..ndim {
                elems = elems.saturating_mul(shape.dims[d] as usize);
            }
            total = total.saturating_add(elems);
        }
        Some(total)
    }
}

/// Unpin a model (allow LRU eviction).
pub fn unpin_model(index: usize) -> bool {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to REGISTRY.
    unsafe {
        let reg = &mut *REGISTRY.get();
        if index < MAX_MODELS && reg.entries[index].active {
            reg.entries[index].pinned = false;
            true
        } else {
            false
        }
    }
}

/// Unload a model by registry index.
pub fn unload_model(index: usize) -> Result<(), LoadError> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to REGISTRY.
    unsafe {
        let reg = &mut *REGISTRY.get();
        if index >= MAX_MODELS || !reg.entries[index].active {
            Err(LoadError::InvalidFormat)
        } else {
            let entry = &mut reg.entries[index];
            let w = entry.weights;
            let ws = entry.workspace;
            *entry = LoadedModelEntry::empty();
            // Free memory outside lock would be better, but these are quick ops
            let _ = mm::free(w);
            let _ = mm::free(ws);
            Ok(())
        }
    }
}

/// Get model info by index.
pub fn get_info(index: usize) -> Option<ModelInfoC> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        if index >= MAX_MODELS || !reg.entries[index].active {
            None
        } else {
            let e = &reg.entries[index];
            let mut info = e.info;
            // #37: populate dynamic fields from the live entry.
            info.pinned = if e.pinned { 1 } else { 0 };
            info.use_count = e.use_count;
            // Convert ns to ms for the C side (avoids 64-bit division
            // in kernel -mgeneral-regs-only code).
            info.last_used_ms = (e.last_used / 1_000_000) as u32;
            Some(info)
        }
    }
}

/// Get the operator graph for a loaded model.
pub fn get_graph(index: usize) -> Option<OperatorGraph> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        if index >= MAX_MODELS || !reg.entries[index].active {
            None
        } else {
            Some(reg.entries[index].graph.clone())
        }
    }
}

/// Get the weight memory handle for a loaded model.
pub fn get_weights(index: usize) -> Option<ModelHandle> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        if index >= MAX_MODELS || !reg.entries[index].active {
            None
        } else {
            Some(reg.entries[index].weights)
        }
    }
}

/// Share a model's weight memory with another consumer.
///
/// Increments the refcount on the weight block so it stays alive
/// even if the original model is unloaded. The caller must call
/// `mm::free()` on the returned handle when done.
pub fn share_weights(index: usize) -> Option<ModelHandle> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        if index >= MAX_MODELS || !reg.entries[index].active {
            None
        } else {
            mm::share(reg.entries[index].weights).ok()
        }
    }
}

/// Get the workspace memory handle for a loaded model.
pub fn get_workspace(index: usize) -> Option<ModelHandle> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        if index >= MAX_MODELS || !reg.entries[index].active {
            None
        } else {
            Some(reg.entries[index].workspace)
        }
    }
}

/// Get the weight table for a loaded model.
pub fn get_weight_table(index: usize) -> Option<WeightTable> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        if index >= MAX_MODELS || !reg.entries[index].active {
            None
        } else {
            Some(reg.entries[index].weight_table.clone())
        }
    }
}

/// Find a model by name (null-terminated or exact-length byte slice).
pub fn find_by_name(name: &[u8]) -> Option<usize> {
    // Trim trailing null
    let name = if name.last() == Some(&0) {
        &name[..name.len() - 1]
    } else {
        name
    };

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        let mut found = None;
        for (i, entry) in reg.entries.iter().enumerate() {
            if entry.active {
                let entry_name_len = entry.name.iter()
                    .position(|&b| b == 0)
                    .unwrap_or(entry.name.len());
                let entry_name = &entry.name[..entry_name_len];
                if entry_name == name {
                    found = Some(i);
                    break;
                }
            }
        }
        found
    }
}

/// Copy a model's operator graph directly into a caller-provided buffer.
///
/// Avoids returning the 6KB OperatorGraph by value (stack overflow in debug).
pub fn copy_graph_into(index: usize, dest: &mut OperatorGraph) -> bool {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        if index >= MAX_MODELS || !reg.entries[index].active {
            false
        } else {
            // Direct field copy into destination (no intermediate stack variable)
            core::ptr::copy_nonoverlapping(
                &reg.entries[index].graph as *const OperatorGraph,
                dest as *mut OperatorGraph,
                1,
            );
            true
        }
    }
}

/// Copy a model's weight table directly into a caller-provided buffer.
pub fn copy_weight_table_into(index: usize, dest: &mut WeightTable) -> bool {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        if index >= MAX_MODELS || !reg.entries[index].active {
            false
        } else {
            core::ptr::copy_nonoverlapping(
                &reg.entries[index].weight_table as *const WeightTable,
                dest as *mut WeightTable,
                1,
            );
            true
        }
    }
}

/// Get the number of loaded models.
pub fn count() -> usize {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive read access to REGISTRY.
    unsafe {
        let reg = &*REGISTRY.get();
        reg.entries.iter().filter(|e| e.active).count()
    }
}
