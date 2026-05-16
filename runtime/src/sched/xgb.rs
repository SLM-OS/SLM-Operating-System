//! XGBoost cascade scheduler policy (#855).
//!
//! Holds the runtime-loaded `XGBC` cascade (3 classifiers — `core /
//! priority / preempt`) emitted by `slm-os-scheduler-ai`'s
//! `write_xgboost_blob` (#850 / sibling-repo `scripts/export_models.py`)
//! and exposes the C-callable FFI used by `kernel/sched/ai/sched_xgb.c`
//! and the Lua/shell `slm.sched_model_*` wiring.
//!
//! Storage lives Rust-side because the trained cascade is ~9 MB —
//! far too large for the 528 KB-per-slot static dense pool used by
//! the MLP / PPO stores in `kernel/sched/ai/runtime_model.c`.
//!
//! # Cascade ordering
//!
//! Matches `TripleClassifier.predict` in
//! `~/projects/slm-os-scheduler-ai/training/xgboost/train.py`:
//!
//! ```text
//!   core_clf      reads 113 features (108 raw + 5 derived)
//!   priority_clf  reads 114 features (113 + predicted core)
//!   preempt_clf   reads 115 features (114 + predicted priority)
//! ```
//!
//! Each prediction is the **raw label value** (e.g. `core ∈ {0..5}`),
//! not the encoded class index. The classifier's `label_classes` map
//! does the inverse-transform that Python's `LabelEncoder` performs.

use alloc::sync::Arc;
use alloc::vec::Vec;
use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::ml::xgb_tree::{self, XgbCascade};

// ---------------------------------------------------------------------
// Wire-format constants — must mirror runtime_model.h + the SEMB header
// shape used by every `SCHED_MODEL_KIND_*` blob.
// ---------------------------------------------------------------------

const SEMB_MAGIC: [u8; 4] = *b"SEMB";
const SEMB_OUTER_HEADER_LEN: usize = 24;
const SEMB_BLOB_VERSION_V1: u16 = 1;
const SCHED_MODEL_KIND_XGBOOST: u16 = 0x1006;
const SCHED_MODEL_SCHEMA_V1: u16 = 1;

// ---------------------------------------------------------------------
// Feature-shape constants — must mirror
// `slm-os-scheduler-ai/training/xgboost/features.py` and
// `slm_sim/observation.py` exactly. A drift here silently mis-feeds
// the trees.
// ---------------------------------------------------------------------

pub const STATE_DIM: usize = 108;
pub const N_DERIVED_FEATURES: usize = 5;
pub const FEATURES_PER_CORE: usize = 6;
pub const FEATURES_PER_TASK: usize = 8;
pub const MAX_CORES: usize = 6;
pub const MAX_PENDING_TASKS: usize = 8;
pub const GLOBAL_FEATURE_COUNT: usize = 8;

const TASK_START: usize = MAX_CORES * FEATURES_PER_CORE; // 36
const GLOBAL_START: usize =
    TASK_START + MAX_PENDING_TASKS * FEATURES_PER_TASK; // 100

// Per-core sub-offsets.
const CORE_UTIL: usize = 0;
const CORE_CACHE: usize = 2;
const CORE_TYPE: usize = 3;
const CORE_ISOLATED: usize = 4;

// Per-task sub-offsets.
const TASK_DEADLINE_URG: usize = 1;
const TASK_GPU: usize = 5;

// Global sub-offsets.
const GLOB_GPU_DEPTH: usize = 5;

// Cascade input widths.
const CORE_INPUT_LEN: usize = STATE_DIM + N_DERIVED_FEATURES; // 113
const PRIORITY_INPUT_LEN: usize = CORE_INPUT_LEN + 1; // 114
const PREEMPT_INPUT_LEN: usize = PRIORITY_INPUT_LEN + 1; // 115

// Per-classifier feature-index bound for the parser. Each classifier
// only references features from its own input vector (the previous
// classifier's prediction shows up as a new feature index for the
// next stage).
const CASCADE_MAX_FEATURE_IDX: [usize; 3] = [
    CORE_INPUT_LEN,
    PRIORITY_INPUT_LEN,
    PREEMPT_INPUT_LEN,
];

// ---------------------------------------------------------------------
// FFI status struct — mirrors `struct sched_model_status` /
// `struct sched_model_meta` in `kernel/sched/ai/runtime_model.h`. Must
// stay byte-for-byte aligned with that layout (size + field offsets).
// ---------------------------------------------------------------------

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct SchedModelMetaC {
    pub version: u16,
    pub schema_version: u16,
    pub feature_version: u16,
    pub action_version: u16,
    pub action_count: u16,
    pub _pad: u16,
    pub payload_len: u32,
    pub checksum: u32,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct SchedModelStatusC {
    pub kind_id: u16,
    pub state: u16,
    pub has_staged: u32,
    pub has_active: u32,
    pub has_rollback: u32,
    pub staged: SchedModelMetaC,
    pub active: SchedModelMetaC,
    pub rollback: SchedModelMetaC,
}

const SCHED_MODEL_EMPTY: u16 = 0;
const SCHED_MODEL_STAGED: u16 = 1;
const SCHED_MODEL_ACTIVE: u16 = 2;
const SCHED_MODEL_ROLLED_BACK: u16 = 3;

// Build-time layout pin against `struct sched_model_status` /
// `struct sched_model_meta` in `kernel/sched/ai/runtime_model.h`.
// Either the field order or the implicit pad changes here, the build
// breaks instead of the C side silently mis-reading the FFI status
// payload.
const _: () = assert!(core::mem::size_of::<SchedModelMetaC>() == 20);
const _: () = assert!(core::mem::size_of::<SchedModelStatusC>() == 76);

// ---------------------------------------------------------------------
// Slot store. Three slots — staged, active, rollback — match the
// existing dense-pool semantics so the Lua/shell verbs (`stage`,
// `activate`, `rollback`, `clear`, `status`) behave identically across
// all model kinds.
// ---------------------------------------------------------------------

struct Slot {
    /// `Arc` (not `Box`) so `predict()` can clone a reference under
    /// the lock and walk the trees with the lock released — the lock
    /// would otherwise be held for hundreds of thousands of node
    /// reads per decision.
    cascade: Option<Arc<XgbCascade>>,
    meta: SchedModelMetaC,
}

impl Slot {
    const fn empty() -> Self {
        Self {
            cascade: None,
            meta: SchedModelMetaC {
                version: 0,
                schema_version: 0,
                feature_version: 0,
                action_version: 0,
                action_count: 0,
                _pad: 0,
                payload_len: 0,
                checksum: 0,
            },
        }
    }

    fn present(&self) -> bool {
        self.cascade.is_some()
    }

    fn clear(&mut self) {
        self.cascade = None;
        self.meta = SchedModelMetaC::default();
    }
}

struct Store {
    staged: Slot,
    active: Slot,
    rollback: Slot,
    state: u16,
}

impl Store {
    const fn empty() -> Self {
        Self {
            staged: Slot::empty(),
            active: Slot::empty(),
            rollback: Slot::empty(),
            state: SCHED_MODEL_EMPTY,
        }
    }
}

static STORE_LOCK: AtomicBool = AtomicBool::new(false);
static mut STORE: Store = Store::empty();

struct SpinGuard;

impl SpinGuard {
    fn new() -> Self {
        while STORE_LOCK
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
        STORE_LOCK.store(false, Ordering::Release);
    }
}

unsafe fn store_mut() -> *mut Store {
    addr_of_mut!(STORE)
}

// ---------------------------------------------------------------------
// SEMB outer-header parse. Borrowed shape from
// `runtime/src/mm/eviction/blob.rs::parse_blob` but pinned to the
// scheduler-side kind id (0x1006) and the FNV-1a checksum used by the
// existing C-side parser in `runtime_model.c`.
// ---------------------------------------------------------------------

fn read_u16_le(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([b[off], b[off + 1]])
}

fn read_u32_le(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

fn fnv1a_32(bytes: &[u8]) -> u32 {
    let mut h: u32 = 0x811C_9DC5;
    for &b in bytes {
        h ^= b as u32;
        h = h.wrapping_mul(0x0100_0193);
    }
    h
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum XgbStageError {
    TooShort,
    BadMagic,
    BadVersion,
    BadKind,
    BadSchema,
    NonZeroReserved,
    BadLength,
    Checksum,
    PayloadInvalid,
    BadClassifierCount,
    /// `activate()` called with no staged blob, or `rollback()` called
    /// with no rollback slot. C-side surfaces both as `-1` (same as
    /// any other error), but Rust callers can pattern-match for
    /// clearer logging.
    NotPresent,
}

fn parse_outer_header(bytes: &[u8]) -> Result<(SchedModelMetaC, &[u8]), XgbStageError> {
    if bytes.len() < SEMB_OUTER_HEADER_LEN {
        return Err(XgbStageError::TooShort);
    }
    if bytes[0..4] != SEMB_MAGIC {
        return Err(XgbStageError::BadMagic);
    }
    let version = read_u16_le(bytes, 4);
    if version != SEMB_BLOB_VERSION_V1 {
        return Err(XgbStageError::BadVersion);
    }
    let kind = read_u16_le(bytes, 6);
    if kind != SCHED_MODEL_KIND_XGBOOST {
        return Err(XgbStageError::BadKind);
    }
    let schema_version = read_u16_le(bytes, 8);
    if schema_version != SCHED_MODEL_SCHEMA_V1 {
        return Err(XgbStageError::BadSchema);
    }
    if read_u16_le(bytes, 10) != 0 || read_u32_le(bytes, 20) != 0 {
        return Err(XgbStageError::NonZeroReserved);
    }
    let payload_len = read_u32_le(bytes, 12);
    let checksum = read_u32_le(bytes, 16);
    let total = SEMB_OUTER_HEADER_LEN
        .checked_add(payload_len as usize)
        .ok_or(XgbStageError::BadLength)?;
    if bytes.len() != total {
        return Err(XgbStageError::BadLength);
    }
    let payload = &bytes[SEMB_OUTER_HEADER_LEN..];
    if fnv1a_32(payload) != checksum {
        return Err(XgbStageError::Checksum);
    }
    let meta = SchedModelMetaC {
        version,
        schema_version,
        feature_version: 0,
        action_version: 0,
        action_count: 0,
        _pad: 0,
        payload_len,
        checksum,
    };
    Ok((meta, payload))
}

fn parse_full(bytes: &[u8]) -> Result<(SchedModelMetaC, XgbCascade), XgbStageError> {
    let (meta, payload) = parse_outer_header(bytes)?;
    let cascade = xgb_tree::parse_cascade(payload, &CASCADE_MAX_FEATURE_IDX)
        .map_err(|_| XgbStageError::PayloadInvalid)?;
    if cascade.classifiers.len() != 3 {
        return Err(XgbStageError::BadClassifierCount);
    }
    Ok((meta, cascade))
}

// ---------------------------------------------------------------------
// Public stage / activate / rollback / clear / status / predict.
// All operations short-circuit in O(1) if the store is empty.
// ---------------------------------------------------------------------

pub fn validate(bytes: &[u8]) -> Result<(), XgbStageError> {
    parse_full(bytes).map(|_| ())
}

pub fn stage(bytes: &[u8]) -> Result<(), XgbStageError> {
    let (meta, cascade) = parse_full(bytes)?;
    let arc = Arc::new(cascade);
    let _g = SpinGuard::new();
    // SAFETY: lock held — exclusive access to STORE.
    unsafe {
        let s = &mut *store_mut();
        s.staged.cascade = Some(arc);
        s.staged.meta = meta;
        s.state = SCHED_MODEL_STAGED;
    }
    Ok(())
}

pub fn activate() -> Result<(), XgbStageError> {
    let _g = SpinGuard::new();
    unsafe {
        let s = &mut *store_mut();
        if !s.staged.present() {
            return Err(XgbStageError::NotPresent);
        }
        // Move active into rollback (drops the prior rollback), then
        // staged into active. Manual swaps because Slot isn't Copy.
        s.rollback.cascade = s.active.cascade.take();
        s.rollback.meta = s.active.meta;
        s.active.cascade = s.staged.cascade.take();
        s.active.meta = s.staged.meta;
        s.staged.clear();
        s.state = SCHED_MODEL_ACTIVE;
    }
    Ok(())
}

pub fn rollback() -> Result<(), XgbStageError> {
    let _g = SpinGuard::new();
    unsafe {
        let s = &mut *store_mut();
        if !s.rollback.present() {
            return Err(XgbStageError::NotPresent);
        }
        // Swap active and rollback in place.
        let (a_c, a_m) = (s.active.cascade.take(), s.active.meta);
        s.active.cascade = s.rollback.cascade.take();
        s.active.meta = s.rollback.meta;
        s.rollback.cascade = a_c;
        s.rollback.meta = a_m;
        s.state = SCHED_MODEL_ROLLED_BACK;
    }
    Ok(())
}

pub fn clear() {
    let _g = SpinGuard::new();
    unsafe {
        let s = &mut *store_mut();
        s.staged.clear();
        s.active.clear();
        s.rollback.clear();
        s.state = SCHED_MODEL_EMPTY;
    }
}

pub fn status() -> SchedModelStatusC {
    let _g = SpinGuard::new();
    let mut out = SchedModelStatusC::default();
    out.kind_id = SCHED_MODEL_KIND_XGBOOST;
    unsafe {
        let s = &*store_mut();
        out.state = s.state;
        out.has_staged = if s.staged.present() { 1 } else { 0 };
        out.has_active = if s.active.present() { 1 } else { 0 };
        out.has_rollback = if s.rollback.present() { 1 } else { 0 };
        out.staged = s.staged.meta;
        out.active = s.active.meta;
        out.rollback = s.rollback.meta;
    }
    out
}

/// True if there's an active cascade ready to predict against.
pub fn is_active() -> bool {
    let _g = SpinGuard::new();
    unsafe { (*store_mut()).active.present() }
}

// ---------------------------------------------------------------------
// Derived features. Mirrors
// `slm-os-scheduler-ai/training/xgboost/features.py::add_derived_features`
// term-for-term. Any drift here silently mis-feeds the cascade.
// ---------------------------------------------------------------------

fn compute_derived(state: &[f32; STATE_DIM]) -> [f32; N_DERIVED_FEATURES] {
    let mut out = [0.0_f32; N_DERIVED_FEATURES];

    // 1. max_deadline_urgency over the top-K pending tasks.
    let mut max_urg = 0.0_f32;
    for t in 0..MAX_PENDING_TASKS {
        let off = TASK_START + t * FEATURES_PER_TASK + TASK_DEADLINE_URG;
        let v = state[off];
        if v > max_urg {
            max_urg = v;
        }
    }
    out[0] = max_urg;

    // 2. core_util_std with the same "non-padding" mask Python uses.
    // Python: include core if (core_type > 0 OR util > 0 OR c < 4).
    let mut utils: [f32; MAX_CORES] = [0.0; MAX_CORES];
    let mut n = 0usize;
    for c in 0..MAX_CORES {
        let base = c * FEATURES_PER_CORE;
        let u = state[base + CORE_UTIL];
        let core_type = state[base + CORE_TYPE];
        if core_type > 0.0 || u > 0.0 || c < 4 {
            utils[n] = u;
            n += 1;
        }
    }
    debug_assert!(n <= MAX_CORES);
    if n > 1 {
        let mut sum = 0.0_f32;
        for i in 0..n {
            sum += utils[i];
        }
        let mean = sum / (n as f32);
        let mut var_sum = 0.0_f32;
        for i in 0..n {
            let d = utils[i] - mean;
            var_sum += d * d;
        }
        // Population std (numpy default). Use the no_libm `mathf::sqrtf`
        // — `libm::sqrtf` triggers the f16 soften crash on
        // x86_64-unknown-none (issue #141).
        let var = var_sum / (n as f32);
        out[1] = crate::inference::mathf::sqrtf(var);
    } else {
        out[1] = 0.0;
    }

    // 3. deadline_task_count, normalized to [0, 1].
    let mut count: u32 = 0;
    for t in 0..MAX_PENDING_TASKS {
        let off = TASK_START + t * FEATURES_PER_TASK + TASK_DEADLINE_URG;
        if state[off] > 0.0 {
            count += 1;
        }
    }
    out[2] = (count as f32) / (MAX_PENDING_TASKS as f32);

    // 4. gpu_should_use: (top task is GPU-eligible) AND (GPU queue depth low).
    let gpu_depth = state[GLOBAL_START + GLOB_GPU_DEPTH];
    let top_task_gpu = state[TASK_START + TASK_GPU];
    out[3] = if top_task_gpu > 0.5 && gpu_depth < 0.25 {
        1.0
    } else {
        0.0
    };

    // 5. best_cache_fit_core: lowest cache pressure among non-isolated
    // cores, normalized by (MAX_CORES - 1).
    let mut best_pressure = f32::INFINITY;
    let mut best_core = 0usize;
    for c in 0..MAX_CORES {
        let base = c * FEATURES_PER_CORE;
        if state[base + CORE_ISOLATED] > 0.5 {
            continue;
        }
        let pressure = state[base + CORE_CACHE];
        if pressure < best_pressure {
            best_pressure = pressure;
            best_core = c;
        }
    }
    let denom = (MAX_CORES - 1) as f32;
    out[4] = (best_core as f32) / denom;

    out
}

// ---------------------------------------------------------------------
// Predict. Three classifiers in cascade — each appends its prediction
// to the input vector before invoking the next.
// ---------------------------------------------------------------------

#[derive(Debug, Clone, Copy)]
pub struct CascadeOutput {
    pub core: i32,
    pub priority: i32,
    pub preempt: i32,
}

pub fn predict(state: &[f32; STATE_DIM]) -> Option<CascadeOutput> {
    // Snapshot the active cascade under the lock, release the lock,
    // then walk the trees. `Arc::clone` is a single refcount bump
    // so the critical section is O(1) regardless of cascade size.
    // Without this, every IRQ-context `assign_cpu` on every CPU
    // would serialise on the lock for the full tree walk
    // (hundreds of thousands of node reads).
    let cascade: Arc<XgbCascade> = {
        let _g = SpinGuard::new();
        // SAFETY: lock held — exclusive access to STORE.
        unsafe { (*store_mut()).active.cascade.clone() }
    }?;
    debug_assert_eq!(cascade.classifiers.len(), 3);

    let derived = compute_derived(state);

    // Stack-allocated 115-float feature buffer — avoids the
    // ~460-byte heap alloc that a `Vec` would do per assign_cpu.
    // `assign_cpu` runs from IRQ context on hardware-tick paths;
    // hitting the global Rust heap there pressures the allocator
    // and lengthens the IRQ-disabled window.
    let mut features = [0.0_f32; PREEMPT_INPUT_LEN];
    let mut len: usize = 0;
    for (dst, src) in features[..STATE_DIM].iter_mut().zip(state.iter()) {
        *dst = *src;
    }
    len += STATE_DIM;
    for (dst, src) in features[len..len + N_DERIVED_FEATURES]
        .iter_mut()
        .zip(derived.iter())
    {
        *dst = *src;
    }
    len += N_DERIVED_FEATURES;
    debug_assert_eq!(len, CORE_INPUT_LEN);

    let core_clf = &cascade.classifiers[0];
    let core_label =
        core_clf.predict_label(&features[..len], core_clf.label_classes().len());

    features[len] = core_label as f32;
    len += 1;
    debug_assert_eq!(len, PRIORITY_INPUT_LEN);
    let priority_clf = &cascade.classifiers[1];
    let prio_label = priority_clf
        .predict_label(&features[..len], priority_clf.label_classes().len());

    features[len] = prio_label as f32;
    len += 1;
    debug_assert_eq!(len, PREEMPT_INPUT_LEN);
    let preempt_clf = &cascade.classifiers[2];
    let preempt_label = preempt_clf
        .predict_label(&features[..len], preempt_clf.label_classes().len());

    Some(CascadeOutput {
        core: core_label,
        priority: prio_label,
        preempt: preempt_label,
    })
}

// ---------------------------------------------------------------------
// C FFI surface. All `extern "C"` entry points are no-mangle and use
// `*const`/`*mut` pointers because the caller is C; safety contracts
// are documented per function.
// ---------------------------------------------------------------------

/// Validate a candidate blob without mutating the store.
///
/// SAFETY (caller contract): `data..data+len` must be a valid
/// readable slice for the duration of the call.
#[no_mangle]
pub extern "C" fn rust_sched_xgb_validate_blob(
    data: *const u8,
    len: usize,
) -> i32 {
    if data.is_null() || len == 0 {
        return -1;
    }
    // SAFETY: caller-contract; validated above.
    let slice = unsafe { core::slice::from_raw_parts(data, len) };
    match validate(slice) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

/// Parse + stage a blob into the STAGED slot.
///
/// SAFETY (caller contract): `data..data+len` must be a valid
/// readable slice for the duration of the call. Bytes are copied
/// into Rust-owned heap; the caller may free its buffer immediately
/// on return.
#[no_mangle]
pub extern "C" fn rust_sched_xgb_stage_blob(
    data: *const u8,
    len: usize,
) -> i32 {
    if data.is_null() || len == 0 {
        return -1;
    }
    // SAFETY: caller-contract; validated above.
    let slice = unsafe { core::slice::from_raw_parts(data, len) };
    match stage(slice) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

#[no_mangle]
pub extern "C" fn rust_sched_xgb_activate() -> i32 {
    match activate() {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

#[no_mangle]
pub extern "C" fn rust_sched_xgb_rollback() -> i32 {
    match rollback() {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

#[no_mangle]
pub extern "C" fn rust_sched_xgb_clear() -> i32 {
    clear();
    0
}

/// SAFETY (caller contract): `out` must point to a writable
/// `struct sched_model_status` (`SchedModelStatusC`). Layout is pinned
/// on both sides — `const _: () = assert!` in this module
/// (`size_of::<SchedModelStatusC>() == 76`) and `_Static_assert` in
/// `kernel/sched/ai/runtime_model.h` — so any future reorder breaks
/// the build instead of silently mis-reading the status payload.
#[no_mangle]
pub extern "C" fn rust_sched_xgb_status(out: *mut SchedModelStatusC) -> i32 {
    if out.is_null() {
        return -1;
    }
    // SAFETY: caller-contract; validated above.
    unsafe {
        *out = status();
    }
    0
}

/// Returns 1 if a cascade is currently active and ready to serve
/// predictions, 0 otherwise.
#[no_mangle]
pub extern "C" fn rust_sched_xgb_is_active() -> i32 {
    if is_active() {
        1
    } else {
        0
    }
}

/// Run the cascade against the supplied 108-d state vector. On
/// success, writes the (core, priority, preempt) triple to the
/// output pointers and returns 0. Returns -1 if no cascade is active
/// or any of the pointers is NULL.
///
/// SAFETY (caller contract): `state` must point to `STATE_DIM`
/// contiguous `f32`s and be 4-byte aligned (any `float state[N]` on
/// the C side satisfies this naturally). `out_*` must be non-null and
/// writable.
#[no_mangle]
pub extern "C" fn rust_sched_xgb_predict(
    state: *const f32,
    out_core: *mut i32,
    out_priority: *mut i32,
    out_preempt: *mut i32,
) -> i32 {
    if state.is_null()
        || out_core.is_null()
        || out_priority.is_null()
        || out_preempt.is_null()
    {
        return -1;
    }
    // SAFETY: caller-contract; non-null + STATE_DIM floats validated
    // by the FFI documentation. The cast to a fixed-size array
    // borrow is safe because the caller promised at least STATE_DIM
    // contiguous floats.
    let buf: &[f32; STATE_DIM] = unsafe { &*(state as *const [f32; STATE_DIM]) };
    let Some(out) = predict(buf) else {
        return -1;
    };
    // SAFETY: caller-contract; out_* validated non-null above.
    unsafe {
        *out_core = out.core;
        *out_priority = out.priority;
        *out_preempt = out.preempt;
    }
    0
}

// ---------------------------------------------------------------------
// Smoke-test fixture. Reused by the kernel-side `rust_run_tests` path
// in `lib.rs` (synthetic 3-classifier cascade — no trained weights
// required) and by the unit tests below.
// ---------------------------------------------------------------------

/// Build a tiny SEMB+XGBC blob holding a 3-classifier cascade that
/// emits raw labels (3, 1, 1) regardless of input. Each classifier is
/// 1 tree of 1 leaf with `n_classes == 1`.
pub fn build_smoke_blob() -> Vec<u8> {
    let mut payload: Vec<u8> = Vec::new();
    payload.extend_from_slice(b"XGBC");
    payload.extend_from_slice(&1u16.to_le_bytes()); // version
    payload.extend_from_slice(&0u16.to_le_bytes()); // reserved
    payload.extend_from_slice(&3u16.to_le_bytes()); // n_classifiers
    payload.extend_from_slice(&0u16.to_le_bytes()); // reserved
    payload.extend_from_slice(&0u32.to_le_bytes()); // reserved

    for label in [3i32, 1i32, 1i32] {
        payload.extend_from_slice(&1u32.to_le_bytes()); // n_trees
        payload.extend_from_slice(&1u32.to_le_bytes()); // n_nodes
        payload.extend_from_slice(&1u16.to_le_bytes()); // n_classes
        payload.extend_from_slice(&0u16.to_le_bytes()); // reserved (u16)
        payload.extend_from_slice(&0u32.to_le_bytes()); // reserved (u32)
        payload.extend_from_slice(&0u32.to_le_bytes()); // root[0]
        // Single leaf node.
        payload.extend_from_slice(&0u16.to_le_bytes()); // feature
        payload.extend_from_slice(&1u16.to_le_bytes()); // FLAG_LEAF
        payload.extend_from_slice(&0u32.to_le_bytes()); // left
        payload.extend_from_slice(&0u32.to_le_bytes()); // right
        payload.extend_from_slice(&0.0_f32.to_le_bytes()); // threshold
        payload.extend_from_slice(&1.0_f32.to_le_bytes()); // value
        payload.extend_from_slice(&label.to_le_bytes()); // label[0]
    }

    // Synthetic blob is <200 bytes; `as u32` truncation is safe and
    // avoids a panic path reachable from the kernel test harness.
    let payload_len = payload.len() as u32;
    let checksum = fnv1a_32(&payload);

    let mut blob: Vec<u8> = Vec::with_capacity(SEMB_OUTER_HEADER_LEN + payload.len());
    blob.extend_from_slice(&SEMB_MAGIC);
    blob.extend_from_slice(&SEMB_BLOB_VERSION_V1.to_le_bytes());
    blob.extend_from_slice(&SCHED_MODEL_KIND_XGBOOST.to_le_bytes());
    blob.extend_from_slice(&SCHED_MODEL_SCHEMA_V1.to_le_bytes());
    blob.extend_from_slice(&0u16.to_le_bytes()); // reserved
    blob.extend_from_slice(&payload_len.to_le_bytes());
    blob.extend_from_slice(&checksum.to_le_bytes());
    blob.extend_from_slice(&0u32.to_le_bytes()); // trailing reserved
    blob.extend_from_slice(&payload);
    blob
}

/// Build a tiny SEMB+XGBC blob whose stage 2 and stage 3 classifiers
/// are 2-class. Used by the kernel-side `rust_run_tests` regression
/// for #920: with the multiclass-only `predict_argmax` path (pre-fix),
/// `predict_label` would split each classifier's trees alternately
/// into score buckets and pick the wrong label. With the binary
/// margin-threshold path (post-fix), the cascade returns the labels
/// pinned below.
///
/// Layout per classifier:
/// - Classifier 0: n_classes=1 (multiclass-trivial), always emits label `42`.
/// - Classifier 1: n_classes=2, two leaves `[0.5, 0.5]`. Binary margin = 1.0
///   ≥ 0 → class 1 → label `20`. Argmax-only path would tie-break to
///   class 0 → label `10`.
/// - Classifier 2: n_classes=2, two leaves `[-0.6, +0.4]`. Binary margin
///   = -0.2 < 0 → class 0 → label `30`. Argmax-only path would pick
///   class 1 (0.4 > -0.6) → label `40`.
///
/// Expected post-fix output: `(42, 20, 30)`.
pub fn build_binary_cascade_smoke_blob() -> Vec<u8> {
    let mut payload: Vec<u8> = Vec::new();
    payload.extend_from_slice(b"XGBC");
    payload.extend_from_slice(&1u16.to_le_bytes()); // version
    payload.extend_from_slice(&0u16.to_le_bytes()); // reserved
    payload.extend_from_slice(&3u16.to_le_bytes()); // n_classifiers
    payload.extend_from_slice(&0u16.to_le_bytes()); // reserved
    payload.extend_from_slice(&0u32.to_le_bytes()); // reserved

    // Helper closure shape: one leaf-only tree per root.
    let push_classifier =
        |payload: &mut Vec<u8>, leaves: &[f32], labels: &[i32]| {
            let n_trees = leaves.len() as u32;
            let n_nodes = leaves.len() as u32;
            let n_classes = labels.len() as u16;
            payload.extend_from_slice(&n_trees.to_le_bytes());
            payload.extend_from_slice(&n_nodes.to_le_bytes());
            payload.extend_from_slice(&n_classes.to_le_bytes());
            payload.extend_from_slice(&0u16.to_le_bytes()); // reserved (u16)
            payload.extend_from_slice(&0u32.to_le_bytes()); // reserved (u32)
            // Roots — one per tree, addressed by tree index (each tree
            // is one node, so root[i] = i).
            for i in 0..leaves.len() as u32 {
                payload.extend_from_slice(&i.to_le_bytes());
            }
            for &leaf in leaves {
                payload.extend_from_slice(&0u16.to_le_bytes()); // feature
                payload.extend_from_slice(&1u16.to_le_bytes()); // FLAG_LEAF
                payload.extend_from_slice(&0u32.to_le_bytes()); // left
                payload.extend_from_slice(&0u32.to_le_bytes()); // right
                payload.extend_from_slice(&0.0_f32.to_le_bytes()); // threshold
                payload.extend_from_slice(&leaf.to_le_bytes()); // value
            }
            for &lbl in labels {
                payload.extend_from_slice(&lbl.to_le_bytes());
            }
        };

    push_classifier(&mut payload, &[1.0_f32], &[42i32]);
    push_classifier(&mut payload, &[0.5_f32, 0.5_f32], &[10i32, 20i32]);
    push_classifier(&mut payload, &[-0.6_f32, 0.4_f32], &[30i32, 40i32]);

    let payload_len = payload.len() as u32;
    let checksum = fnv1a_32(&payload);

    let mut blob: Vec<u8> = Vec::with_capacity(SEMB_OUTER_HEADER_LEN + payload.len());
    blob.extend_from_slice(&SEMB_MAGIC);
    blob.extend_from_slice(&SEMB_BLOB_VERSION_V1.to_le_bytes());
    blob.extend_from_slice(&SCHED_MODEL_KIND_XGBOOST.to_le_bytes());
    blob.extend_from_slice(&SCHED_MODEL_SCHEMA_V1.to_le_bytes());
    blob.extend_from_slice(&0u16.to_le_bytes()); // reserved
    blob.extend_from_slice(&payload_len.to_le_bytes());
    blob.extend_from_slice(&checksum.to_le_bytes());
    blob.extend_from_slice(&0u32.to_le_bytes()); // trailing reserved
    blob.extend_from_slice(&payload);
    blob
}

// ---------------------------------------------------------------------
// Unit tests. Exercise every part of the path: SEMB outer-header
// round-trip, cascade parse, staged → active dance, derived-feature
// computation, full predict.
// ---------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn build_minimal_cascade_blob() -> Vec<u8> {
        super::build_smoke_blob()
    }

    /// Reset the global STORE between tests — module-static state would
    /// otherwise carry over and conflate failure attribution.
    fn reset_store() {
        clear();
    }

    #[test]
    fn outer_header_rejects_wrong_kind() {
        reset_store();
        let mut blob = build_minimal_cascade_blob();
        // Overwrite kind id at offset 6 with a foreign value.
        blob[6] = 0x99;
        blob[7] = 0x99;
        // Recompute checksum (changing the kind doesn't affect payload,
        // so checksum is still valid — the kind check fires first).
        assert!(matches!(validate(&blob), Err(XgbStageError::BadKind)));
    }

    #[test]
    fn outer_header_rejects_bad_checksum() {
        reset_store();
        let mut blob = build_minimal_cascade_blob();
        // Flip a byte in the payload (after the 24-byte header).
        let last = blob.len() - 1;
        blob[last] ^= 0xFF;
        assert!(matches!(validate(&blob), Err(XgbStageError::Checksum)));
    }

    #[test]
    fn stage_activate_predict_round_trip() {
        reset_store();
        let blob = build_minimal_cascade_blob();
        assert!(stage(&blob).is_ok());
        let s = status();
        assert_eq!(s.has_staged, 1);
        assert_eq!(s.has_active, 0);
        assert!(activate().is_ok());
        let s = status();
        assert_eq!(s.has_active, 1);
        assert_eq!(s.has_staged, 0);

        let state = [0.0_f32; STATE_DIM];
        let out = predict(&state).expect("active cascade predicts");
        // Each classifier has 1 class so argmax is 0 → label_map[0].
        // The minimal cascade's labels were [3, 1, 1].
        assert_eq!(out.core, 3);
        assert_eq!(out.priority, 1);
        assert_eq!(out.preempt, 1);
    }

    #[test]
    fn rollback_restores_prior_active() {
        reset_store();
        let blob = build_minimal_cascade_blob();
        stage(&blob).unwrap();
        activate().unwrap();
        // Stage a *different* cascade and activate to push the first
        // into rollback. Reusing the same blob bytes is fine for the
        // mechanics test — what matters is that the slot dance works.
        stage(&blob).unwrap();
        activate().unwrap();
        let s = status();
        assert_eq!(s.has_rollback, 1);
        assert!(rollback().is_ok());
        let s = status();
        assert_eq!(s.state, SCHED_MODEL_ROLLED_BACK);
    }

    #[test]
    fn predict_returns_none_when_no_active_cascade() {
        reset_store();
        let state = [0.0_f32; STATE_DIM];
        assert!(predict(&state).is_none());
    }

    #[test]
    fn activate_without_staged_returns_not_present() {
        reset_store();
        assert!(matches!(activate(), Err(XgbStageError::NotPresent)));
    }

    #[test]
    fn rollback_without_prior_returns_not_present() {
        reset_store();
        assert!(matches!(rollback(), Err(XgbStageError::NotPresent)));
    }

    #[test]
    fn derived_features_match_python_reference() {
        // Construct a state where every derived feature has a
        // hand-computable answer and assert each one.
        let mut state = [0.0_f32; STATE_DIM];
        // Task 2 has urgency 0.7 (max), task 5 has 0.3.
        state[TASK_START + 2 * FEATURES_PER_TASK + TASK_DEADLINE_URG] = 0.7;
        state[TASK_START + 5 * FEATURES_PER_TASK + TASK_DEADLINE_URG] = 0.3;
        // Cores 0..3 have utilisations [0.2, 0.4, 0.0, 0.6]; cores 4..5
        // have type=0 / util=0 → excluded by the (c<4) mask.
        state[0 * FEATURES_PER_CORE + CORE_UTIL] = 0.2;
        state[1 * FEATURES_PER_CORE + CORE_UTIL] = 0.4;
        state[3 * FEATURES_PER_CORE + CORE_UTIL] = 0.6;
        // Top task is GPU-eligible; gpu queue depth is 0.1 (< 0.25).
        state[TASK_START + TASK_GPU] = 1.0;
        state[GLOBAL_START + GLOB_GPU_DEPTH] = 0.1;
        // Core 2 has the lowest cache pressure (0.1) and is non-isolated.
        for c in 0..MAX_CORES {
            state[c * FEATURES_PER_CORE + CORE_CACHE] = 0.5;
        }
        state[2 * FEATURES_PER_CORE + CORE_CACHE] = 0.1;

        let d = compute_derived(&state);
        assert!((d[0] - 0.7).abs() < 1e-6);
        // utils = [0.2, 0.4, 0.0, 0.6]; mean = 0.3; var = 0.05; std ≈ 0.2236.
        assert!((d[1] - 0.22360680).abs() < 1e-3,
                "core_util_std = {} (expected ~0.224)", d[1]);
        // 2 tasks have urgency > 0; 2 / 8 = 0.25.
        assert!((d[2] - 0.25).abs() < 1e-6);
        assert_eq!(d[3], 1.0);
        // best_core = 2; 2 / 5 = 0.4.
        assert!((d[4] - 0.4).abs() < 1e-6);
    }
}
