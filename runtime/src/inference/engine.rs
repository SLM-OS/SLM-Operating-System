//! Inference engine — walks an OperatorGraph and executes operators.
//!
//! The engine resolves tensor names to data pointers using a flat binding table,
//! dispatches each operator, and manages intermediate tensor memory via the
//! workspace bump allocator.

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use crate::loader::graph::*;
use crate::loader::registry;
use crate::mm;
use crate::kernel_ffi;
use super::tensor::Tensor;
use super::workspace::BumpAllocator;
use super::ops;

/// Maximum named tensors tracked during inference.
const MAX_BINDINGS: usize = 32;

// =============================================================================
// Inference Statistics
// =============================================================================

/// Inference performance statistics (thread-safe via atomics).
#[repr(C)]
#[derive(Debug)]
pub struct InferenceStats {
    pub total_inferences: u64,
    pub total_time_ns: u64,
    pub min_time_ns: u64,
    pub max_time_ns: u64,
    pub last_time_ns: u64,
    pub errors: u64,
}

/// Atomic counters for inference stats.
static STATS_TOTAL: AtomicU64 = AtomicU64::new(0);
static STATS_TIME: AtomicU64 = AtomicU64::new(0);
static STATS_MIN: AtomicU64 = AtomicU64::new(u64::MAX);
static STATS_MAX: AtomicU64 = AtomicU64::new(0);
static STATS_LAST: AtomicU64 = AtomicU64::new(0);
static STATS_ERRORS: AtomicU64 = AtomicU64::new(0);

/// Get a snapshot of inference statistics.
pub fn get_stats() -> InferenceStats {
    let min = STATS_MIN.load(Ordering::Relaxed);
    InferenceStats {
        total_inferences: STATS_TOTAL.load(Ordering::Relaxed),
        total_time_ns: STATS_TIME.load(Ordering::Relaxed),
        min_time_ns: if min == u64::MAX { 0 } else { min },
        max_time_ns: STATS_MAX.load(Ordering::Relaxed),
        last_time_ns: STATS_LAST.load(Ordering::Relaxed),
        errors: STATS_ERRORS.load(Ordering::Relaxed),
    }
}

/// Record a successful inference.
fn record_inference(elapsed_ns: u64) {
    STATS_TOTAL.fetch_add(1, Ordering::Relaxed);
    STATS_TIME.fetch_add(elapsed_ns, Ordering::Relaxed);
    STATS_LAST.store(elapsed_ns, Ordering::Relaxed);

    // Update min (CAS loop)
    let mut cur = STATS_MIN.load(Ordering::Relaxed);
    while elapsed_ns < cur {
        match STATS_MIN.compare_exchange_weak(cur, elapsed_ns, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => break,
            Err(actual) => cur = actual,
        }
    }

    // Update max (CAS loop)
    cur = STATS_MAX.load(Ordering::Relaxed);
    while elapsed_ns > cur {
        match STATS_MAX.compare_exchange_weak(cur, elapsed_ns, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => break,
            Err(actual) => cur = actual,
        }
    }
}

fn record_error() {
    STATS_ERRORS.fetch_add(1, Ordering::Relaxed);
}

// =============================================================================
// Per-operator profiling (#56)
// =============================================================================

/// Number of buckets in the per-operator profile table.
///
/// One bucket per known `OpType` variant plus one for `OpType::Unknown`,
/// so a graph with an unhandled op still gets counted instead of being
/// silently lost. `op_type_to_index` maps the enum onto this index space.
pub const PROFILE_NUM_OPS: usize = 18;

/// One row of the per-operator profile snapshot.
///
/// `op_type` is the same `u8` discriminant the engine stores in
/// `GraphNode.op_type` (so the C side can label rows without copying
/// any strings across the FFI). `count == 0` means the op never ran in
/// the current measurement window.
///
/// Layout MUST match `RustOpProfileEntry` in `kernel/include/slm_ffi.h`.
/// `#[repr(C)]` on a `(u8, u64, ...)` would natively pad 7 bytes after
/// `op_type` anyway, but the explicit `_pad` field documents the
/// guarantee and lets the snapshot writer zero-init the bytes (no
/// kernel-stack contents leak across the FFI to the shell printer).
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct OpProfileEntry {
    pub op_type: u8,
    /// Explicit padding for FFI layout parity with
    /// `RustOpProfileEntry` in `kernel/include/slm_ffi.h`. Treat as
    /// private — `pub` only because `#[repr(C)]` requires all fields
    /// be visible to layout consumers; `#[doc(hidden)]` keeps it out
    /// of rustdoc so consumers aren't tempted to read or write it.
    #[doc(hidden)]
    pub _pad: [u8; 7],
    pub count: u64,
    pub total_ns: u64,
    pub min_ns: u64,
    pub max_ns: u64,
    // PMU sums (#871). Zero on x86-64 (#870) and on QEMU TCG (event
    // counters not modelled). See `kernel/include/slm_ffi.h` for the
    // bucket-to-event mapping.
    pub cache_misses: u64,
    pub l2_misses: u64,
    pub instructions_retired: u64,
    pub branch_mispredictions: u64,
    pub cache_references: u64,
    pub backend_stalls: u64,
    pub cycles: u64,
}

impl OpProfileEntry {
    pub const EMPTY: Self = Self {
        op_type: 0,
        _pad: [0; 7],
        count: 0,
        total_ns: 0,
        min_ns: 0,
        max_ns: 0,
        cache_misses: 0,
        l2_misses: 0,
        instructions_retired: 0,
        branch_mispredictions: 0,
        cache_references: 0,
        backend_stalls: 0,
        cycles: 0,
    };
}

static OP_PROFILE_ENABLED: AtomicBool = AtomicBool::new(false);

// One `AtomicU64` per (bucket, field). Split arrays (rather than an
// `[OpProfileEntry; N]` struct) so that the hot path can update a
// single field without a CAS-the-whole-struct dance.
static PROF_COUNT: [AtomicU64; PROFILE_NUM_OPS] = {
    const Z: AtomicU64 = AtomicU64::new(0);
    [Z; PROFILE_NUM_OPS]
};
static PROF_TOTAL_NS: [AtomicU64; PROFILE_NUM_OPS] = {
    const Z: AtomicU64 = AtomicU64::new(0);
    [Z; PROFILE_NUM_OPS]
};
static PROF_MIN_NS: [AtomicU64; PROFILE_NUM_OPS] = {
    const M: AtomicU64 = AtomicU64::new(u64::MAX);
    [M; PROFILE_NUM_OPS]
};
static PROF_MAX_NS: [AtomicU64; PROFILE_NUM_OPS] = {
    const Z: AtomicU64 = AtomicU64::new(0);
    [Z; PROFILE_NUM_OPS]
};
// PMU per-bucket accumulators (#871). Same shape as the timing fields;
// snapshot reads use Relaxed loads, recorder uses fetch_add.
const fn z_arr() -> [AtomicU64; PROFILE_NUM_OPS] {
    const Z: AtomicU64 = AtomicU64::new(0);
    [Z; PROFILE_NUM_OPS]
}
static PROF_CACHE_MISSES: [AtomicU64; PROFILE_NUM_OPS] = z_arr();
static PROF_L2_MISSES: [AtomicU64; PROFILE_NUM_OPS] = z_arr();
static PROF_INST_RETIRED: [AtomicU64; PROFILE_NUM_OPS] = z_arr();
static PROF_BR_MISPRED: [AtomicU64; PROFILE_NUM_OPS] = z_arr();
static PROF_CACHE_REFS: [AtomicU64; PROFILE_NUM_OPS] = z_arr();
static PROF_BACKEND_STALLS: [AtomicU64; PROFILE_NUM_OPS] = z_arr();
static PROF_CYCLES: [AtomicU64; PROFILE_NUM_OPS] = z_arr();

/// Map an `OpType` discriminant into the `[0, PROFILE_NUM_OPS)` index
/// space the profile arrays use. `OpType::Unknown` is intentionally
/// folded onto the last slot rather than being dropped on the floor.
fn op_type_to_index(op: OpType) -> usize {
    match op {
        OpType::MatMul => 0,
        OpType::Add => 1,
        OpType::Relu => 2,
        OpType::Softmax => 3,
        OpType::LayerNorm => 4,
        OpType::Reshape => 5,
        OpType::Transpose => 6,
        OpType::Gather => 7,
        OpType::Concat => 8,
        OpType::Unsqueeze => 9,
        OpType::Gemm => 10,
        OpType::Flatten => 11,
        OpType::Shape => 12,
        OpType::Constant => 13,
        OpType::Cast => 14,
        OpType::Conv => 15,
        OpType::MaxPool => 16,
        OpType::Unknown => 17,
    }
}

/// Reverse of `op_type_to_index` — used by snapshot consumers to label
/// rows with the original `OpType` discriminant.
fn index_to_op_type_u8(i: usize) -> u8 {
    match i {
        0 => OpType::MatMul as u8,
        1 => OpType::Add as u8,
        2 => OpType::Relu as u8,
        3 => OpType::Softmax as u8,
        4 => OpType::LayerNorm as u8,
        5 => OpType::Reshape as u8,
        6 => OpType::Transpose as u8,
        7 => OpType::Gather as u8,
        8 => OpType::Concat as u8,
        9 => OpType::Unsqueeze as u8,
        10 => OpType::Gemm as u8,
        11 => OpType::Flatten as u8,
        12 => OpType::Shape as u8,
        13 => OpType::Constant as u8,
        14 => OpType::Cast as u8,
        15 => OpType::Conv as u8,
        16 => OpType::MaxPool as u8,
        _ => OpType::Unknown as u8,
    }
}

/// Enable or disable per-op profiling. When disabled, `execute_node`
/// takes the same fast path it did before #56 (single match dispatch,
/// no timestamps). When enabled, every op invocation pays two CNTPCT
/// reads + one `fetch_add` triple — measured at ~250 ns/op on Pi 5,
/// negligible against op latencies in the µs range but enough that the
/// flag stays opt-in.
pub fn op_profile_set_enabled(enabled: bool) {
    OP_PROFILE_ENABLED.store(enabled, Ordering::Relaxed);
}

pub fn op_profile_is_enabled() -> bool {
    OP_PROFILE_ENABLED.load(Ordering::Relaxed)
}

/// Zero every bucket. `min_ns` resets to `u64::MAX` so the first
/// recorded sample wins the CAS unconditionally; the snapshot getter
/// reports it as 0 if `count == 0`.
///
/// **Not atomic** with respect to a concurrent `op_profile_record`:
/// a record interleaved with the reset can yield COUNT=0 while
/// TOTAL/MIN/MAX still carry pre-reset data (or vice-versa). The
/// intended usage is `reset` → run the workload → `snapshot`; the
/// engine's `EngineGuard` already serialises ops in practice so an
/// interleaving is only possible if the operator runs `model profile
/// reset` mid-bench, which is a user-error pattern.
pub fn op_profile_reset() {
    for i in 0..PROFILE_NUM_OPS {
        PROF_COUNT[i].store(0, Ordering::Relaxed);
        PROF_TOTAL_NS[i].store(0, Ordering::Relaxed);
        PROF_MIN_NS[i].store(u64::MAX, Ordering::Relaxed);
        PROF_MAX_NS[i].store(0, Ordering::Relaxed);
        PROF_CACHE_MISSES[i].store(0, Ordering::Relaxed);
        PROF_L2_MISSES[i].store(0, Ordering::Relaxed);
        PROF_INST_RETIRED[i].store(0, Ordering::Relaxed);
        PROF_BR_MISPRED[i].store(0, Ordering::Relaxed);
        PROF_CACHE_REFS[i].store(0, Ordering::Relaxed);
        PROF_BACKEND_STALLS[i].store(0, Ordering::Relaxed);
        PROF_CYCLES[i].store(0, Ordering::Relaxed);
    }
}

/// PMU counter snapshot captured at op start (#871). Built from the
/// FFI `slm_pmu_*` reads after `slm_pmu_reset`, so every field is a
/// delta-from-zero for the wrapped op.
///
/// On x86-64 and on platforms where the PMU has not been enabled on
/// the calling CPU, every field reads as zero — the harness records
/// zero per-op deltas and the snapshot consumer sees blank PMU columns
/// instead of garbage. This matches how QEMU TCG behaves (cycle counter
/// works but event counters return zero).
#[derive(Default, Clone, Copy)]
struct PmuSnapshot {
    cycles: u64,
    cache_misses: u64,
    l2_misses: u64,
    instructions_retired: u64,
    branch_mispredictions: u64,
    cache_references: u64,
    backend_stalls: u64,
}

impl PmuSnapshot {
    fn capture() -> Self {
        // SAFETY: every FFI call is a pure register read with no
        // pointer arguments. On platforms without a PMU the C wrappers
        // return zero.
        unsafe {
            Self {
                cycles: kernel_ffi::slm_pmu_read_cycles(),
                cache_misses: kernel_ffi::slm_pmu_read_event(0) as u64,
                l2_misses: kernel_ffi::slm_pmu_read_event(1) as u64,
                instructions_retired: kernel_ffi::slm_pmu_read_event(2) as u64,
                branch_mispredictions: kernel_ffi::slm_pmu_read_event(3) as u64,
                cache_references: kernel_ffi::slm_pmu_read_event(4) as u64,
                backend_stalls: kernel_ffi::slm_pmu_read_event(5) as u64,
            }
        }
    }
}

/// Record one op invocation. Called from `execute_node` only when
/// profiling is enabled — the caller does the enable check so that the
/// disabled path doesn't even read `OP_PROFILE_ENABLED`. The `pmu`
/// snapshot is the delta captured between `pmu_reset` and the
/// post-dispatch read.
fn op_profile_record(op_type: OpType, ns: u64, pmu: &PmuSnapshot) {
    let i = op_type_to_index(op_type);
    PROF_COUNT[i].fetch_add(1, Ordering::Relaxed);
    PROF_TOTAL_NS[i].fetch_add(ns, Ordering::Relaxed);
    let mut cur = PROF_MIN_NS[i].load(Ordering::Relaxed);
    while ns < cur {
        match PROF_MIN_NS[i].compare_exchange_weak(
            cur, ns, Ordering::Relaxed, Ordering::Relaxed,
        ) {
            Ok(_) => break,
            Err(actual) => cur = actual,
        }
    }
    cur = PROF_MAX_NS[i].load(Ordering::Relaxed);
    while ns > cur {
        match PROF_MAX_NS[i].compare_exchange_weak(
            cur, ns, Ordering::Relaxed, Ordering::Relaxed,
        ) {
            Ok(_) => break,
            Err(actual) => cur = actual,
        }
    }
    PROF_CACHE_MISSES[i].fetch_add(pmu.cache_misses, Ordering::Relaxed);
    PROF_L2_MISSES[i].fetch_add(pmu.l2_misses, Ordering::Relaxed);
    PROF_INST_RETIRED[i].fetch_add(pmu.instructions_retired, Ordering::Relaxed);
    PROF_BR_MISPRED[i].fetch_add(pmu.branch_mispredictions, Ordering::Relaxed);
    PROF_CACHE_REFS[i].fetch_add(pmu.cache_references, Ordering::Relaxed);
    PROF_BACKEND_STALLS[i].fetch_add(pmu.backend_stalls, Ordering::Relaxed);
    PROF_CYCLES[i].fetch_add(pmu.cycles, Ordering::Relaxed);
}

/// Copy at most `max_entries` rows into `out` (one per op bucket, in
/// fixed `op_type_to_index` order). Returns the number of rows written.
/// Caller is responsible for sizing `out` to at least `PROFILE_NUM_OPS`
/// entries when they want the full table.
///
/// **Not atomic across rows.** Each row's four fields (count, total,
/// min, max) are read with independent Relaxed loads, and the
/// row-to-row loop is not synchronised against
/// `op_profile_record`. A concurrent record can land between two
/// rows of the same snapshot. For a diagnostic profiler this is fine
/// — the inference engine serialises ops through `EngineGuard` so
/// the only "concurrent" recorder in practice is the very op the
/// shell command interleaves with, and the loop completes in tens of
/// nanoseconds. Do not use this snapshot for hard correctness checks.
///
/// # Safety
///
/// `out` must be writable for `min(max_entries, PROFILE_NUM_OPS)`
/// `OpProfileEntry` values and properly aligned.
pub unsafe fn op_profile_snapshot(
    out: *mut OpProfileEntry,
    max_entries: usize,
) -> usize {
    let n = core::cmp::min(max_entries, PROFILE_NUM_OPS);
    for i in 0..n {
        let count = PROF_COUNT[i].load(Ordering::Relaxed);
        let min = PROF_MIN_NS[i].load(Ordering::Relaxed);
        let entry = OpProfileEntry {
            op_type: index_to_op_type_u8(i),
            _pad: [0; 7],
            count,
            total_ns: PROF_TOTAL_NS[i].load(Ordering::Relaxed),
            // When no samples have been recorded, `min` is still
            // `u64::MAX` from reset. Surface 0 to the consumer so a
            // `model profile show` line for an unused op reads cleanly
            // instead of as a 64-bit poison value.
            min_ns: if count == 0 || min == u64::MAX { 0 } else { min },
            max_ns: PROF_MAX_NS[i].load(Ordering::Relaxed),
            cache_misses: PROF_CACHE_MISSES[i].load(Ordering::Relaxed),
            l2_misses: PROF_L2_MISSES[i].load(Ordering::Relaxed),
            instructions_retired: PROF_INST_RETIRED[i].load(Ordering::Relaxed),
            branch_mispredictions: PROF_BR_MISPRED[i].load(Ordering::Relaxed),
            cache_references: PROF_CACHE_REFS[i].load(Ordering::Relaxed),
            backend_stalls: PROF_BACKEND_STALLS[i].load(Ordering::Relaxed),
            cycles: PROF_CYCLES[i].load(Ordering::Relaxed),
        };
        unsafe { *out.add(i) = entry; }
    }
    n
}

/// Errors from the inference engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EngineError {
    ModelNotFound,
    WorkspaceExhausted,
    UnsupportedOp,
    ShapeMismatch,
    /// Shape dimensions multiplied past `usize::MAX` — malformed model input.
    ShapeOverflow,
    InvalidInput,
    WeightNotFound,
    InternalError,
}

/// A binding from a tensor name to its data.
#[derive(Clone, Copy)]
struct TensorBinding {
    name: TensorName,
    tensor: Tensor,
    active: bool,
}

impl TensorBinding {
    const EMPTY: Self = Self {
        name: TensorName::EMPTY,
        tensor: Tensor::EMPTY,
        active: false,
    };
}

/// CPU inference engine for a single model.
///
/// Uses a static buffer (~10KB) to avoid both stack overflow and heap
/// allocation issues. Only one inference can run at a time (spinlock).
pub struct InferenceEngine {
    graph: OperatorGraph,
    weight_table: WeightTable,
    weight_base: *const u8,
    workspace: BumpAllocator,
    bindings: [TensorBinding; MAX_BINDINGS],
    binding_count: usize,
    gpu_caps: super::gpu::GpuCapabilities,
    initialized: bool,
}

// SAFETY: InferenceEngine operates on kernel-managed memory.
// Access is serialized by ENGINE_LOCK.
unsafe impl Send for InferenceEngine {}
unsafe impl Sync for InferenceEngine {}

/// Static wrapper for the engine (same pattern as component/registry.rs).
#[repr(transparent)]
struct SyncWrapper<T>(UnsafeCell<T>);
unsafe impl<T> Sync for SyncWrapper<T> {}
impl<T> SyncWrapper<T> {
    const fn new(value: T) -> Self { SyncWrapper(UnsafeCell::new(value)) }
    fn get(&self) -> *mut T { self.0.get() }
}

static ENGINE: SyncWrapper<InferenceEngine> = SyncWrapper::new(InferenceEngine::empty());
static ENGINE_LOCK: AtomicBool = AtomicBool::new(false);

fn engine_lock() {
    while ENGINE_LOCK.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
        core::hint::spin_loop();
    }
}

fn engine_unlock() {
    ENGINE_LOCK.store(false, Ordering::Release);
}

/// RAII guard for ENGINE_LOCK. Closes the early-return windows in
/// `run_inference` (GPU fastpath success → return Ok(n)). The previous
/// pattern called `engine_unlock()` immediately before each early
/// return, which is correct today but easy to miss if a future edit
/// adds a new path.
struct EngineGuard;

impl EngineGuard {
    fn new() -> Self {
        engine_lock();
        EngineGuard
    }
}

impl Drop for EngineGuard {
    fn drop(&mut self) {
        engine_unlock();
    }
}

impl InferenceEngine {
    const fn empty() -> Self {
        Self {
            graph: OperatorGraph::EMPTY,
            weight_table: WeightTable::EMPTY,
            weight_base: core::ptr::null(),
            workspace: BumpAllocator::empty(),
            bindings: [TensorBinding::EMPTY; MAX_BINDINGS],
            binding_count: 0,
            gpu_caps: super::gpu::GpuCapabilities::NONE,
            initialized: false,
        }
    }

    /// Initialize the static engine for a model. Must be called with lock held.
    ///
    /// Uses `copy_graph_into`/`copy_weight_table_into` to avoid putting
    /// large structs (OperatorGraph ~6KB) on the stack.
    fn init(&mut self, model_index: usize) -> Result<(), EngineError> {
        if !registry::copy_graph_into(model_index, &mut self.graph) {
            return Err(EngineError::ModelNotFound);
        }
        if !registry::copy_weight_table_into(model_index, &mut self.weight_table) {
            return Err(EngineError::ModelNotFound);
        }

        let weight_handle = registry::get_weights(model_index)
            .ok_or(EngineError::ModelNotFound)?;
        let workspace_handle = registry::get_workspace(model_index)
            .ok_or(EngineError::ModelNotFound)?;

        let weight_base = mm::get_ptr(weight_handle)
            .ok_or(EngineError::InternalError)?;
        let ws_ptr = mm::get_ptr(workspace_handle)
            .ok_or(EngineError::InternalError)?;
        let ws_size = mm::get_size(workspace_handle)
            .unwrap_or(0);

        self.weight_base = weight_base as *const u8;
        self.workspace = BumpAllocator::new(ws_ptr, ws_size);
        self.binding_count = 0;
        self.gpu_caps = super::gpu::GpuCapabilities::detect();
        self.initialized = true;
        Ok(())
    }

    /// Create an engine for the given model, returning Err if model not found.
    ///
    /// This is a lightweight check — actual work happens in `run_inference`.
    pub fn new(model_index: usize) -> Result<(), EngineError> {
        // Just verify the model exists
        registry::get_graph(model_index).ok_or(EngineError::ModelNotFound)?;
        Ok(())
    }

    /// Run inference on FP32 input data.
    ///
    /// Returns the number of output floats written to `output`.
    pub fn run(
        &mut self,
        input: *const f32,
        input_len: usize,
        output: *mut f32,
        output_len: usize,
    ) -> Result<usize, EngineError> {
        self.run_batch(1, input, input_len, output, output_len)
    }

    /// Run inference treating `input` as a stack of `batch_size`
    /// samples laid out contiguously. Each declared graph input is
    /// bound with its first dimension overridden to `batch_size`; the
    /// per-sample element count is `declared_elements / declared_dim0`.
    ///
    /// `batch_size == 1` is equivalent to `run()` and is the path the
    /// existing FFI callers take.
    pub fn run_batch(
        &mut self,
        batch_size: usize,
        input: *const f32,
        input_len: usize,
        output: *mut f32,
        output_len: usize,
    ) -> Result<usize, EngineError> {
        if batch_size == 0 {
            return Err(EngineError::InvalidInput);
        }
        // Reset workspace and bindings
        self.workspace.reset();
        self.binding_count = 0;

        // Bind graph input(s)
        self.bind_graph_inputs(batch_size, input, input_len)?;

        // Bind all weights from the weight table
        self.bind_weights()?;

        // Execute nodes in topological order
        for i in 0..self.graph.node_count {
            self.execute_node(i)?;
        }

        // Copy graph output(s) to caller's buffer
        self.copy_output(output, output_len)
    }

    /// Bind graph input names to the provided input data.
    ///
    /// When `batch_size > 1`, the first dimension of every graph input
    /// is overridden from the declared value (usually 1) to
    /// `batch_size`. The remaining dimensions describe the per-sample
    /// shape, so `input_len` must equal `batch_size *
    /// per_sample_elements`. This is the only seam the batched
    /// dispatcher needs — every downstream op (Conv2D, MatMul, Gemm,
    /// Softmax, MaxPool, Reshape) already iterates over `input.dim(0)`
    /// and handles the batch dimension correctly.
    ///
    /// ASSUMPTION (load-bearing): every supported model declares the
    /// batch dimension at index 0 of its first input. The MNIST
    /// model and every other model SLM-OS loads today follow this
    /// convention; if a future model puts channels or another
    /// dimension first, this override silently corrupts the shape.
    /// Add a per-model "batch_dim_index" property and pass it
    /// through to break that assumption.
    fn bind_graph_inputs(
        &mut self,
        batch_size: usize,
        input: *const f32,
        input_len: usize,
    ) -> Result<(), EngineError> {
        if input.is_null() || input_len == 0 {
            return Err(EngineError::InvalidInput);
        }

        for i in 0..self.graph.input_count {
            let name = &self.graph.input_names[i];
            let shape = &self.graph.input_shapes[i];

            // Build shape array from TensorShape, overriding dim 0.
            let mut dims = [0u32; 8];
            let ndim = shape.ndim as usize;
            for d in 0..ndim {
                dims[d] = shape.dims[d];
            }
            if batch_size > 1 {
                if ndim == 0 {
                    return Err(EngineError::ShapeMismatch);
                }
                // Override the leading dimension with the actual batch
                // size. The declared value is typically 1 (training-
                // time batch dimension) or a dynamic placeholder.
                let bs_u32: u32 = u32::try_from(batch_size)
                    .map_err(|_| EngineError::ShapeOverflow)?;
                dims[0] = bs_u32;
            }

            let tensor = Tensor::new(input, &dims[..ndim]);

            // Verify element count matches the provided buffer. The
            // `> input_len` check catches under-sized buffers; the
            // debug_assert pins the stronger invariant that the caller
            // sized the buffer to exactly `batch_size * per_sample`
            // elements (oversized buffers technically still work but
            // are a sign of caller confusion — flag in debug builds).
            if tensor.num_elements() > input_len {
                return Err(EngineError::InvalidInput);
            }
            debug_assert_eq!(
                tensor.num_elements(),
                input_len,
                "bind_graph_inputs: input_len ({}) should match batch_size * per_sample ({})",
                input_len,
                tensor.num_elements()
            );

            self.bind(*name, tensor);
        }

        Ok(())
    }

    /// Bind all weight initializers from the weight table.
    fn bind_weights(&mut self) -> Result<(), EngineError> {
        for i in 0..self.weight_table.count {
            let entry = &self.weight_table.entries[i];

            // Build shape
            let mut dims = [0u32; 8];
            let ndim = entry.shape.ndim as usize;
            for d in 0..ndim {
                dims[d] = entry.shape.dims[d];
            }

            // Create tensor with appropriate elem_type
            let tensor = unsafe {
                let raw_ptr = self.weight_base.add(entry.offset as usize);
                if entry.shape.elem_type == crate::loader::graph::ElemType::Float16 {
                    Tensor::new_fp16(raw_ptr as *const u16, &dims[..ndim])
                } else {
                    Tensor::new(raw_ptr as *const f32, &dims[..ndim])
                }
            };
            self.bind(entry.name, tensor);
        }

        Ok(())
    }

    /// Execute a single graph node.
    ///
    /// Wraps the op dispatch in CNTPCT timestamps when per-op profiling
    /// is enabled (#56). The unwrapped path is unchanged when profiling
    /// is off: a single load of `OP_PROFILE_ENABLED` and a branch.
    fn execute_node(&mut self, node_idx: usize) -> Result<(), EngineError> {
        let node = self.graph.nodes[node_idx];

        if op_profile_is_enabled() {
            // Reset PMU counters so the post-op reads are absolute
            // per-op deltas — no subtraction-with-overflow corner case
            // even though event counters are only 32-bit. PMU pre-reset
            // is cheap (one MSR) compared to a typical op latency (µs+).
            // SAFETY: pure register write on the calling CPU; no-op on
            // platforms without PMU.
            unsafe { kernel_ffi::slm_pmu_reset(); }
            let start = kernel_ffi::get_time_ns();
            let result = self.dispatch_node(&node);
            let elapsed = kernel_ffi::get_time_ns().saturating_sub(start);
            let pmu = PmuSnapshot::capture();
            // Record the timing even on error so the profile reflects
            // wall-clock cost paid by the engine, not just successful
            // ops. Callers that want success-only numbers can filter
            // by `result.is_ok()` themselves.
            op_profile_record(node.op_type, elapsed, &pmu);
            result
        } else {
            self.dispatch_node(&node)
        }
    }

    /// Inner op dispatch, separated from `execute_node` so the
    /// profiling wrapper above can time it without the match having to
    /// be duplicated. The body is exactly what `execute_node` did
    /// before #56.
    fn dispatch_node(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        match node.op_type {
            OpType::Reshape => self.exec_reshape(node),
            OpType::MatMul => {
                // Hybrid dispatch: try GPU for large MatMul, fall back to CPU
                let backend = super::gpu::select_backend(
                    OpType::MatMul,
                    self.estimate_input_elements(node),
                    &self.gpu_caps,
                );
                if backend == super::gpu::Backend::Gpu {
                    if self.exec_matmul_gpu(node).is_ok() {
                        return Ok(());
                    }
                }
                self.exec_matmul(node)
            }
            OpType::Add => self.exec_add(node),
            OpType::Relu => self.exec_relu(node),
            OpType::Softmax => self.exec_softmax(node),
            OpType::Gemm => self.exec_gemm(node),
            OpType::Flatten => self.exec_flatten(node),
            OpType::Conv => self.exec_conv(node),
            OpType::MaxPool => self.exec_maxpool(node),
            // Shape/Constant/Cast are handled implicitly (weights already bound)
            OpType::Shape | OpType::Constant | OpType::Cast | OpType::Unsqueeze => {
                // These ops produce values that should already be in the weight table
                // or can be skipped for inference
                Ok(())
            }
            _ => Err(EngineError::UnsupportedOp),
        }
    }

    /// Reshape: reinterpret tensor with new shape.
    ///
    /// Reads target shape from the second input (int64 initializer in weight table).
    /// Falls back to 2D flatten if shape tensor not available.
    fn exec_reshape(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;
        let total = input.num_elements();

        // Try to read target shape from the second input
        if node.input_count >= 2 {
            if let Ok(shape_tensor) = self.resolve(&node.inputs[1]) {
                // The shape tensor has shape [N] where N is the number of output dims.
                // Data is stored as little-endian int64 (decoded from varint during loading).
                let ndim = shape_tensor.num_elements();
                if ndim > 0 && ndim <= 8 {
                    let mut new_shape = [0u32; 8];
                    let mut has_infer = false;
                    let mut infer_idx = 0;
                    let mut known_product: usize = 1;

                    unsafe {
                        let i64_ptr = shape_tensor.data as *const i64;
                        for i in 0..ndim {
                            let dim = *i64_ptr.add(i);
                            if dim > 0 {
                                // Reject dims that don't fit in u32. Casting an
                                // i64 > u32::MAX to u32 silently truncates the
                                // upper 32 bits, producing a tiny wrong shape.
                                if dim > i64::from(u32::MAX) {
                                    return Err(EngineError::ShapeOverflow);
                                }
                                new_shape[i] = dim as u32;
                                known_product *= dim as usize;
                            } else if dim == -1 {
                                has_infer = true;
                                infer_idx = i;
                                new_shape[i] = 1; // placeholder
                            } else if dim == 0 {
                                // Copy from input
                                let d = if i < input.ndim as usize { input.dim(i) } else { 1 };
                                new_shape[i] = d;
                                known_product *= d as usize;
                            } else {
                                // Negative dim other than -1 is malformed.
                                // Casting silently wraps to a large u32 and
                                // would corrupt the reshape silently.
                                return Err(EngineError::ShapeOverflow);
                            }
                        }
                    }

                    if has_infer && known_product > 0 {
                        new_shape[infer_idx] = (total / known_product) as u32;
                    }

                    // Validate
                    let product: usize = new_shape[..ndim].iter().map(|&d| d as usize).product();
                    if product == total {
                        let result = ops::reshape(&input, &new_shape[..ndim])?;
                        self.bind_output(node, 0, result);
                        return Ok(());
                    }
                    // If validation fails, fall through to flatten
                }
            }
        }

        // Fallback: flatten to 2D [batch, features]
        let batch = if input.ndim > 0 { input.dim(0) as usize } else { 1 };
        let features = if batch > 0 { total / batch } else { total };
        let result = ops::reshape(&input, &[batch as u32, features as u32])?;
        self.bind_output(node, 0, result);
        Ok(())
    }

    /// MatMul: C = A × B
    fn exec_matmul(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let a = self.resolve(&node.inputs[0])?;
        let b = self.resolve(&node.inputs[1])?;

        let m = a.rows() as usize;
        let n = b.cols() as usize;

        let mut out = self.workspace.alloc_tensor(&[m as u32, n as u32])?;

        ops::matmul(&a, &b, &mut out)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Add: out = a + b
    fn exec_add(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let a = self.resolve(&node.inputs[0])?;
        let b = self.resolve(&node.inputs[1])?;

        // Output shape matches the larger input
        let mut out_shape = [0u32; 8];
        let ndim = a.ndim as usize;
        for i in 0..ndim {
            out_shape[i] = a.shape[i];
        }

        let mut out = self.workspace.alloc_tensor(&out_shape[..ndim])?;

        ops::add(&a, &b, &mut out)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Relu: out = max(0, input)
    fn exec_relu(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;

        let mut out_shape = [0u32; 8];
        let ndim = input.ndim as usize;
        for i in 0..ndim {
            out_shape[i] = input.shape[i];
        }

        let mut out = self.workspace.alloc_tensor(&out_shape[..ndim])?;

        ops::relu(&input, &mut out)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Softmax: out = softmax(input) along last axis
    fn exec_softmax(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;

        let mut out_shape = [0u32; 8];
        let ndim = input.ndim as usize;
        for i in 0..ndim {
            out_shape[i] = input.shape[i];
        }

        let mut out = self.workspace.alloc_tensor(&out_shape[..ndim])?;

        ops::softmax(&input, &mut out)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Gemm: Y = A × B + C
    fn exec_gemm(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let a = self.resolve(&node.inputs[0])?;
        let b = self.resolve(&node.inputs[1])?;
        let c = if node.input_count >= 3 {
            Some(self.resolve(&node.inputs[2])?)
        } else {
            None
        };

        let m = a.rows() as usize;
        let n = b.cols() as usize;

        let mut out = self.workspace.alloc_tensor(&[m as u32, n as u32])?;

        ops::gemm(&a, &b, c.as_ref(), &mut out)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Flatten: reshape to 2D [batch, features]
    fn exec_flatten(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;
        let total = input.num_elements();
        // Default axis=1: keep first dim, flatten rest
        let batch = if input.ndim > 0 { input.shape[0] as usize } else { 1 };
        let features = total / batch;

        let result = ops::reshape(&input, &[batch as u32, features as u32])?;
        self.bind_output(node, 0, result);
        Ok(())
    }

    /// Conv2D: out = conv(input, weight) + bias
    ///
    /// Infers kernel size from the weight tensor shape.
    /// Default: stride=1, pad=0 (MNIST uses these defaults for first conv).
    fn exec_conv(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;
        let weight = self.resolve(&node.inputs[1])?;
        let bias = if node.input_count >= 3 {
            Some(self.resolve(&node.inputs[2])?)
        } else {
            None
        };

        if input.ndim < 4 || weight.ndim < 4 {
            return Err(EngineError::ShapeMismatch);
        }

        let batch = input.dim(0) as usize;
        let c_out = weight.dim(0) as usize;
        let kh = weight.dim(2);
        let kw = weight.dim(3);

        // Default stride=1, pad=0 (MNIST-12 uses kernel_shape=[5,5] stride=1 pad=0)
        let sh: u32 = 1;
        let sw: u32 = 1;
        let ph: u32 = 0;
        let pw: u32 = 0;

        // Use checked arithmetic throughout so a kernel larger than
        // the padded input doesn't underflow u32 to a giant h_out/
        // w_out (which would then alloc_tensor a huge — and probably
        // failed — workspace, or for unbounded sizes silently
        // corrupt). `2 * ph` is also checked so a future caller
        // threading a large `ph` through can't wrap before the add
        // even reaches checked_add.
        let two_ph = 2u32.checked_mul(ph)
            .ok_or(EngineError::ShapeMismatch)?;
        let two_pw = 2u32.checked_mul(pw)
            .ok_or(EngineError::ShapeMismatch)?;
        let h_padded = input.dim(2)
            .checked_add(two_ph)
            .ok_or(EngineError::ShapeMismatch)?;
        let w_padded = input.dim(3)
            .checked_add(two_pw)
            .ok_or(EngineError::ShapeMismatch)?;
        let h_out = h_padded.checked_sub(kh)
            .ok_or(EngineError::ShapeMismatch)? / sh + 1;
        let w_out = w_padded.checked_sub(kw)
            .ok_or(EngineError::ShapeMismatch)? / sw + 1;

        let mut out = self.workspace.alloc_tensor(
            &[batch as u32, c_out as u32, h_out, w_out],
        )?;

        ops::conv2d(&input, &weight, bias.as_ref(), &mut out, kh, kw, sh, sw, ph, pw)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// MaxPool2D: out = maxpool(input)
    ///
    /// Default: kernel=2×2, stride=2 (MNIST-12 uses these).
    fn exec_maxpool(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;

        if input.ndim < 4 {
            return Err(EngineError::ShapeMismatch);
        }

        let batch = input.dim(0) as usize;
        let channels = input.dim(1) as usize;

        // Default kernel=2, stride=2 (MNIST-12 uses kernel_shape=[2,2] strides=[2,2])
        let kh: u32 = 2;
        let kw: u32 = 2;
        let sh: u32 = 2;
        let sw: u32 = 2;

        // Same checked-subtraction pattern as exec_conv: a kh > input.dim(2)
        // input would otherwise underflow u32 and produce a giant h_out.
        let h_out = input.dim(2).checked_sub(kh)
            .ok_or(EngineError::ShapeMismatch)? / sh + 1;
        let w_out = input.dim(3).checked_sub(kw)
            .ok_or(EngineError::ShapeMismatch)? / sw + 1;

        let mut out = self.workspace.alloc_tensor(
            &[batch as u32, channels as u32, h_out, w_out],
        )?;

        ops::maxpool2d(&input, &mut out, kh, kw, sh, sw)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Estimate total input elements for a node (for GPU placement decisions).
    fn estimate_input_elements(&self, node: &GraphNode) -> usize {
        let mut total = 0;
        for i in 0..node.input_count as usize {
            if let Ok(t) = self.resolve(&node.inputs[i]) {
                total += t.num_elements();
            }
        }
        total
    }

    /// Attempt to execute MatMul on GPU. Falls back on any error.
    fn exec_matmul_gpu(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let a = self.resolve(&node.inputs[0])?;
        let b = self.resolve(&node.inputs[1])?;

        let m = a.rows() as usize;
        let k = a.cols() as usize;
        let n = b.cols() as usize;

        let out = self.workspace.alloc_tensor(&[m as u32, n as u32])?;

        super::gpu::gpu_execute_matmul(a.data, b.data, out.data_mut(), m, k, n)
            .map_err(|_| EngineError::UnsupportedOp)?;

        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Copy graph output to the caller's buffer.
    fn copy_output(&self, output: *mut f32, output_len: usize) -> Result<usize, EngineError> {
        if self.graph.output_count == 0 {
            return Err(EngineError::InternalError);
        }

        let out_name = &self.graph.output_names[0];
        let out_tensor = self.resolve(out_name)?;
        let n = out_tensor.num_elements();

        if n > output_len {
            return Err(EngineError::InvalidInput);
        }

        unsafe {
            core::ptr::copy_nonoverlapping(out_tensor.data, output, n);
        }

        Ok(n)
    }

    // =========================================================================
    // Binding table operations
    // =========================================================================

    /// Bind a tensor name to a tensor descriptor.
    fn bind(&mut self, name: TensorName, tensor: Tensor) {
        // Check if name already exists (update in place)
        for i in 0..self.binding_count {
            if self.bindings[i].active && self.bindings[i].name.eq_bytes(name.as_bytes()) {
                self.bindings[i].tensor = tensor;
                return;
            }
        }
        // Add new binding
        if self.binding_count < MAX_BINDINGS {
            self.bindings[self.binding_count] = TensorBinding {
                name,
                tensor,
                active: true,
            };
            self.binding_count += 1;
        }
    }

    /// Bind a node's output to the binding table.
    fn bind_output(&mut self, node: &GraphNode, output_idx: usize, tensor: Tensor) {
        if output_idx < node.output_count as usize {
            self.bind(node.outputs[output_idx], tensor);
        }
    }

    /// Resolve a tensor name to its tensor descriptor.
    fn resolve(&self, name: &TensorName) -> Result<Tensor, EngineError> {
        if name.is_empty() {
            return Err(EngineError::WeightNotFound);
        }
        for i in 0..self.binding_count {
            if self.bindings[i].active && self.bindings[i].name.eq_bytes(name.as_bytes()) {
                return Ok(self.bindings[i].tensor);
            }
        }
        Err(EngineError::WeightNotFound)
    }
}

// =============================================================================
// Public API — uses the static engine with lock
// =============================================================================

/// Detect whether the given `model_index` is a registered "mnist"
/// model and the GPU has compute ready. When true, the MNIST graph
/// can be dispatched whole via `gpu::run_mnist_gpu_fastpath` rather
/// than walked op-by-op. Per the M7 plan, this is the first end-to-
/// end Backend::Gpu integration: the per-op dispatch path lacks a
/// general GEMM/Conv FFI today.
///
/// Looks up the model's name by `model_index` (rather than asking the
/// registry to resolve "mnist" → index) so repeated `model_load_mnist`
/// calls — each producing a fresh registry slot — all qualify, not
/// just the first one.
fn mnist_gpu_fastpath_eligible(model_index: usize) -> bool {
    extern "C" {
        fn slm_gpu_inference_enabled() -> i32;
    }

    // Master operator-intent toggle: `gpu use inference on` must be
    // set. Default at boot is OFF so existing CPU behavior is the
    // baseline until an operator opts in. SAFETY: pure FFI read,
    // no aliasing/lifetime concerns.
    let master_on = unsafe { slm_gpu_inference_enabled() } != 0;
    if !master_on {
        return false;
    }

    // Per-model toggle layered on top — flipping a single model
    // back to CPU without disturbing others is `model use-gpu
    // <name|idx> off`.
    if !registry::gpu_dispatch_enabled(model_index) {
        return false;
    }

    let caps = super::gpu::GpuCapabilities::detect();
    if !caps.has_compute() {
        return false;
    }
    let info = match registry::get_info(model_index) {
        Some(i) => i,
        None => return false,
    };
    let name_len = info.name.iter().position(|&b| b == 0).unwrap_or(info.name.len());
    &info.name[..name_len] == b"mnist"
}

/// Run inference on a loaded model using the static engine.
///
/// Thread-safe: only one inference at a time via spinlock.
///
/// # Safety
///
/// Caller must ensure that:
/// - `input` is non-null and points to at least `input_len` `f32` values
///   (4-byte aligned). The buffer must remain valid and unaliased for
///   the duration of the call.
/// - `output` is non-null and points to at least `output_len` `f32`
///   slots (4-byte aligned), valid for writes and unaliased for the
///   duration of the call.
/// - Both buffers stay live across the spinlock-protected inference
///   step (the engine reads `input` and writes up to `output_len`
///   elements into `output`).
///
/// Callers that already hold typed Rust slice / array references
/// (`as_ptr()` / `as_mut_ptr()` from a stack array or `static`) trivially
/// satisfy these. The signature stays raw because the kernel C side
/// passes through plain `*const float` / `*mut float` from
/// `pmm_alloc_pages` etc. and a typed Rust wrapper would impose
/// conversions for no benefit.
pub unsafe fn run_inference(
    model_index: usize,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
) -> Result<usize, EngineError> {
    run_inference_inner(model_index, 1, input, input_len, output, output_len)
}

/// Batched variant of `run_inference`.
///
/// Treats the input buffer as `batch_size` samples laid out
/// contiguously and runs the model's full op pipeline once across the
/// whole batch. The output buffer receives `batch_size * output_dim`
/// floats.
///
/// The MNIST GPU fast path is single-sample only (its FFI asserts
/// `input_len == 784`), so batched dispatches always take the CPU
/// path. This is documented in `docs/architecture.md` and surfaces in
/// the #858 benchmark matrix.
///
/// # Safety
///
/// Same preconditions as `run_inference`: buffers are non-null,
/// 4-byte-aligned, sized to cover `input_len` / `output_len` floats,
/// and live through the call.
pub unsafe fn run_inference_batched(
    model_index: usize,
    batch_size: usize,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
) -> Result<usize, EngineError> {
    run_inference_inner(model_index, batch_size, input, input_len, output, output_len)
}

unsafe fn run_inference_inner(
    model_index: usize,
    batch_size: usize,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
) -> Result<usize, EngineError> {
    let _guard = EngineGuard::new();

    // Pin the weight block for the duration of the call. If a
    // concurrent `unload` or `swap_model` retargets the registry slot
    // mid-flight, the OLD weight block stays allocated until this
    // lease drops at function exit — no torn reads, no use-after-free.
    //
    // EngineGuard releases ENGINE_LOCK on Drop, so we just return.
    let _weight_lease = match registry::WeightLease::acquire(model_index) {
        Some(l) => l,
        None => {
            record_error();
            return Err(EngineError::ModelNotFound);
        }
    };

    let start = kernel_ffi::get_time_ns();

    // GPU fast path: when the active model is "mnist", the GPU is
    // compute-ready, and the call is a single MNIST sample, route the
    // whole graph through the v6 handoff. Batched dispatches always
    // take the CPU path because the GPU FFI's input slot is sized for
    // exactly one 1×1×28×28 sample.
    let result: Result<usize, EngineError>;
    if batch_size == 1 && mnist_gpu_fastpath_eligible(model_index) {
        // Successful dispatch is the steady-state happy path; logging
        // it every iteration drowns the console at >1 inf/s. The
        // failure path is rare and operator-actionable, so it stays.
        match super::gpu::run_mnist_gpu_fastpath(input, input_len, output, output_len) {
            Ok(n) => {
                let elapsed = kernel_ffi::get_time_ns().saturating_sub(start);
                record_inference(elapsed);
                return Ok(n);
            }
            Err(_) => {
                crate::log::log_info(b"[engine] mnist GPU fastpath failed -- falling back to CPU\0");
                // Fall through to CPU path. Caller still gets a valid
                // result. The rc isn't surfaced today; once a diagnostic
                // ring lands, log it here.
            }
        }
    }

    // SAFETY: We hold the engine lock (via EngineGuard), exclusive access guaranteed.
    result = unsafe {
        let engine = &mut *ENGINE.get();
        match engine.init(model_index) {
            Ok(()) => engine.run_batch(batch_size, input, input_len, output, output_len),
            Err(e) => Err(e),
        }
    };

    let elapsed = kernel_ffi::get_time_ns().saturating_sub(start);

    match &result {
        Ok(_) => record_inference(elapsed),
        Err(_) => record_error(),
    }

    result
}
