//! Inference Scheduler — runtime-toggleable batched dispatch.
//!
//! Provides a request queue, completion mailboxes, and a synchronous
//! batched-dispatch path on top of the single-request inference engine.
//!
//! Design (see issue #55 / #857):
//! - One static singleton, file-scope spinlock.
//! - Fixed-size slot table of [`MAX_BATCH`] slots; each slot owns the
//!   caller's input/output pointer pair plus a state atomic that
//!   doubles as the completion waker.
//! - The submitter that fills the last slot of a forming batch becomes
//!   the synchronous dispatcher: under the scheduler lock it claims
//!   all pending slots, drops the lock, copies the per-request inputs
//!   into a static scratch slab, calls `inference::run_inference` once
//!   with the concatenated `[N, input_dim]` input, then slices the
//!   `[N, output_dim]` result back into each caller's output buffer
//!   and signals their waker.
//! - When the configured batch size is 1, or when only one request is
//!   in flight, or when batching mode is OFF, the scheduler degrades
//!   to the existing single-request `run_inference` path so isolated
//!   callers never block waiting for peers that may never arrive.
//!   Timer-driven partial-batch flush lands in #859.

use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use core::cell::UnsafeCell;

use super::{Priority, CoreType, TaskDeadline};
use crate::inference::{self, EngineError};
use crate::mm::ModelHandle;

extern "C" {
    #[link_name = "yield"]
    fn sched_yield();
}

// =============================================================================
// Capacity constants
// =============================================================================

/// Maximum batch size — also the size of the slot table. 32 keeps the
/// static scratch slab manageable: 32 × 784 × 4 + 32 × 64 × 4 ≈ 108 KB.
pub const MAX_BATCH: usize = 32;

// PENDING stores slot indices as u8 to halve the cacheline footprint
// versus usize; bump to u16 (and revisit cacheline layout) if a future
// commit grows MAX_BATCH past 255.
const _: () = assert!(MAX_BATCH <= 255, "PENDING stores slot indices as u8");

/// Maximum per-request input length supported by the batched path.
/// Sized for MNIST (1 × 1 × 28 × 28 = 784).
///
/// LIMITATION: requests with `input_len > MAX_INPUT_DIM` silently
/// degrade to the singleton path (no error surfaced — the caller's
/// inference still runs, just without the chance to be batched). Bump
/// this constant (and re-evaluate the `SCRATCH_IN` slab size) when
/// adding a model with a larger input shape.
pub const MAX_INPUT_DIM: usize = 784;

/// Maximum per-request output length supported by the batched path.
/// 64 matches the existing `rust_infer_*` FFI output buffer convention.
/// Same silent-singleton-fallback semantics as `MAX_INPUT_DIM` apply.
pub const MAX_OUTPUT_DIM: usize = 64;

const DEFAULT_BATCH_SIZE: usize = 8;
const DEFAULT_BATCH_TIMEOUT_US: u32 = 5_000;

// =============================================================================
// Public types (kept for API compatibility with the prior skeleton)
// =============================================================================

/// State of an inference request.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RequestState {
    /// Request is queued, waiting to start
    Queued,
    /// Request is currently being processed
    Running,
    /// Request completed successfully
    Completed,
    /// Request failed with an error
    Failed,
    /// Request was cancelled
    Cancelled,
}

/// Configuration for an inference request.
#[derive(Debug, Clone)]
pub struct InferenceConfig {
    /// Maximum tokens to generate (for generative models)
    pub max_tokens: usize,
    /// Temperature for sampling (0.0 = deterministic)
    pub temperature: f32,
    /// Top-p sampling parameter
    pub top_p: f32,
    /// Deadline for completion (if any)
    pub deadline: TaskDeadline,
    /// Preferred core type
    pub core_type: CoreType,
}

impl Default for InferenceConfig {
    fn default() -> Self {
        Self {
            max_tokens: 128,
            temperature: 0.7,
            top_p: 0.9,
            deadline: TaskDeadline::NONE,
            core_type: CoreType::Any,
        }
    }
}

/// An inference request to be scheduled.
///
/// The buffer pointers are deliberately not stored here — the
/// synchronous helper `submit_inference_sync` takes them directly. The
/// public `InferenceScheduler::submit` / `get_result` API keeps the
/// metadata-only `InferenceRequest` for callers that need a token
/// handle.
pub struct InferenceRequest {
    /// Unique request ID
    id: u64,
    /// Model to use for inference
    model_handle: ModelHandle,
    /// Request configuration
    config: InferenceConfig,
    /// Priority of this request
    priority: Priority,
    /// Current state
    state: RequestState,
}

impl InferenceRequest {
    /// Create a new inference request.
    pub fn new(model_handle: ModelHandle, config: InferenceConfig) -> Self {
        static NEXT_ID: AtomicU64 = AtomicU64::new(1);
        let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);

        Self {
            id,
            model_handle,
            config,
            priority: Priority::Normal,
            state: RequestState::Queued,
        }
    }

    pub fn id(&self) -> u64 { self.id }
    pub fn model(&self) -> ModelHandle { self.model_handle }
    pub fn state(&self) -> RequestState { self.state }
    pub fn config(&self) -> &InferenceConfig { &self.config }

    pub fn with_priority(mut self, priority: Priority) -> Self {
        self.priority = priority;
        self
    }
}

/// Result of an inference request.
pub struct InferenceResult {
    request_id: u64,
    success: bool,
    tokens_generated: usize,
    inference_time_ns: u64,
}

impl InferenceResult {
    pub fn is_success(&self) -> bool { self.success }
    pub fn request_id(&self) -> u64 { self.request_id }
    pub fn tokens_generated(&self) -> usize { self.tokens_generated }
    pub fn inference_time_ns(&self) -> u64 { self.inference_time_ns }
}

/// Error type for inference scheduling operations.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SchedulerError {
    /// Queue is full
    QueueFull,
    /// Request not found
    NotFound,
    /// Invalid request
    InvalidRequest,
    /// Scheduler is not running
    NotRunning,
    /// Feature not implemented
    NotImplemented,
}

/// Statistics about the inference scheduler.
///
/// The dispatch counters (`batches_dispatched`, `batches_full`,
/// `batches_timeout`, `bypassed_deadline`) are populated by #857/#859.
/// They stay at zero whenever batching mode is OFF or every request
/// degraded to the singleton path.
#[derive(Debug, Clone, Copy, Default)]
pub struct SchedulerStats {
    /// Total requests submitted to the scheduler.
    pub total_submitted: u64,
    /// Requests that completed successfully.
    pub completed: u64,
    /// Requests that failed (engine error).
    pub failed: u64,
    /// Requests currently queued.
    pub queued: usize,
    /// Requests currently running.
    pub running: usize,
    /// Average inference time across batched + singleton paths.
    pub avg_inference_time_ns: u64,
    /// Total batched dispatches (full + timer + bypass paths combined).
    pub batches_dispatched: u64,
    /// Batched dispatches that hit the size threshold.
    pub batches_full: u64,
    /// Batched dispatches that flushed on timer (populated by #859).
    pub batches_timeout: u64,
    /// Requests that bypassed the queue due to tight deadline (#859).
    pub bypassed_deadline: u64,
    /// Requests that fell through to the singleton path (queue full,
    /// oversized input, or no peer to batch with).
    pub singleton_dispatches: u64,
}

// =============================================================================
// Slot table — the actual queue
// =============================================================================

const SLOT_IDLE: u32 = 0;
const SLOT_PENDING: u32 = 1;
const SLOT_DISPATCHING: u32 = 2;
const SLOT_DONE_OK: u32 = 3;
const SLOT_DONE_ERR: u32 = 4;

/// A single request slot.
///
/// Lifecycle (cas-driven transitions):
///   IDLE → PENDING       — submitter reserves the slot
///   PENDING → DISPATCHING — dispatcher claims the slot under lock
///   DISPATCHING → DONE_OK / DONE_ERR — dispatcher publishes result
///   DONE_* → IDLE        — waiter consumes the result and frees
///
/// The state atomic is the cross-CPU completion waker. Submitters spin
/// on it (with `sched_yield`) until the dispatcher publishes a result.
struct Slot {
    state: AtomicU32,
    /// Model index to run.
    model_index: AtomicUsize,
    /// Caller-owned input buffer.
    input_ptr: AtomicUsize,
    input_len: AtomicUsize,
    /// Caller-owned output buffer.
    output_ptr: AtomicUsize,
    output_len: AtomicUsize,
    /// Number of output floats actually written (on DONE_OK) or the
    /// numeric `EngineError` discriminant (on DONE_ERR).
    result_value: AtomicU32,
}

impl Slot {
    const fn new() -> Self {
        Self {
            state: AtomicU32::new(SLOT_IDLE),
            model_index: AtomicUsize::new(0),
            input_ptr: AtomicUsize::new(0),
            input_len: AtomicUsize::new(0),
            output_ptr: AtomicUsize::new(0),
            output_len: AtomicUsize::new(0),
            result_value: AtomicU32::new(0),
        }
    }
}

// =============================================================================
// Globals
// =============================================================================

/// Scratch slabs sized for `MAX_BATCH` rows. Static, lock-protected.
/// MNIST: 32 × 784 × 4 = ~98 KB input, 32 × 64 × 4 = 8 KB output.
struct ScratchInput(UnsafeCell<[f32; MAX_BATCH * MAX_INPUT_DIM]>);
struct ScratchOutput(UnsafeCell<[f32; MAX_BATCH * MAX_OUTPUT_DIM]>);
unsafe impl Sync for ScratchInput {}
unsafe impl Sync for ScratchOutput {}

static SCRATCH_IN: ScratchInput = ScratchInput(UnsafeCell::new([0.0; MAX_BATCH * MAX_INPUT_DIM]));
static SCRATCH_OUT: ScratchOutput = ScratchOutput(UnsafeCell::new([0.0; MAX_BATCH * MAX_OUTPUT_DIM]));

const SLOT_INIT: Slot = Slot::new();
static SLOTS: [Slot; MAX_BATCH] = [SLOT_INIT; MAX_BATCH];

/// The pending list: indices of slots in PENDING state, in insertion
/// order. `PENDING_COUNT` is the active prefix length. Both are
/// protected by `SCHED_LOCK`.
static mut PENDING: [u8; MAX_BATCH] = [0; MAX_BATCH];
static PENDING_COUNT: AtomicUsize = AtomicUsize::new(0);

static SCHED_LOCK: AtomicBool = AtomicBool::new(false);

/// Runtime-toggleable mode. Default OFF; flipped by the admin toggle
/// landing in #860.
static BATCHING_ENABLED: AtomicBool = AtomicBool::new(false);

/// Configurable batch size threshold. Clamped to `[1, MAX_BATCH]`.
static BATCH_SIZE: AtomicUsize = AtomicUsize::new(DEFAULT_BATCH_SIZE);

/// Configurable batch timeout (microseconds). Used by #859 timer flush.
static BATCH_TIMEOUT_US: AtomicU32 = AtomicU32::new(DEFAULT_BATCH_TIMEOUT_US);

// Stats counters — atomics so callers can sample without the lock.
static STAT_TOTAL_SUBMITTED: AtomicU64 = AtomicU64::new(0);
static STAT_COMPLETED: AtomicU64 = AtomicU64::new(0);
static STAT_FAILED: AtomicU64 = AtomicU64::new(0);
static STAT_BATCHES_DISPATCHED: AtomicU64 = AtomicU64::new(0);
static STAT_BATCHES_FULL: AtomicU64 = AtomicU64::new(0);
static STAT_BATCHES_TIMEOUT: AtomicU64 = AtomicU64::new(0);
static STAT_BYPASSED_DEADLINE: AtomicU64 = AtomicU64::new(0);
static STAT_SINGLETON: AtomicU64 = AtomicU64::new(0);
static STAT_RUNNING: AtomicUsize = AtomicUsize::new(0);
static STAT_TIME_TOTAL_NS: AtomicU64 = AtomicU64::new(0);
static STAT_TIME_SAMPLES: AtomicU64 = AtomicU64::new(0);

// =============================================================================
// Lock helpers (RAII)
// =============================================================================

struct SchedGuard;

impl SchedGuard {
    fn new() -> Self {
        while SCHED_LOCK
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        SchedGuard
    }
}

impl Drop for SchedGuard {
    fn drop(&mut self) {
        SCHED_LOCK.store(false, Ordering::Release);
    }
}

// =============================================================================
// Mode / configuration API
// =============================================================================

/// Whether batched dispatch is currently enabled.
pub fn batching_enabled() -> bool {
    BATCHING_ENABLED.load(Ordering::Acquire)
}

/// Enable or disable batched dispatch. The next caller picks up the
/// change. In-flight batches finish under whatever rule they entered.
pub fn set_batching_enabled(on: bool) {
    BATCHING_ENABLED.store(on, Ordering::Release);
}

/// Configured batch-size threshold (clamped to `[1, MAX_BATCH]`).
pub fn batch_size() -> usize {
    BATCH_SIZE.load(Ordering::Relaxed).clamp(1, MAX_BATCH)
}

/// Set the batch-size threshold. Out-of-range values clamp to
/// `[1, MAX_BATCH]`.
pub fn set_batch_size(n: usize) {
    BATCH_SIZE.store(n.clamp(1, MAX_BATCH), Ordering::Relaxed);
}

/// Configured per-batch flush timeout (microseconds).
pub fn batch_timeout_us() -> u32 {
    BATCH_TIMEOUT_US.load(Ordering::Relaxed)
}

/// Set the per-batch flush timeout (microseconds). Bounded at the C
/// shell binding (#860) — this raw setter clamps to a safe lower
/// bound so a misuse doesn't pin the dispatcher in a tight spin.
pub fn set_batch_timeout_us(us: u32) {
    BATCH_TIMEOUT_US.store(us.max(100), Ordering::Relaxed);
}

/// Snapshot the scheduler stats.
pub fn stats() -> SchedulerStats {
    let total = STAT_TOTAL_SUBMITTED.load(Ordering::Relaxed);
    let samples = STAT_TIME_SAMPLES.load(Ordering::Relaxed);
    let time = STAT_TIME_TOTAL_NS.load(Ordering::Relaxed);
    let avg = if samples > 0 { time / samples } else { 0 };
    SchedulerStats {
        total_submitted: total,
        completed: STAT_COMPLETED.load(Ordering::Relaxed),
        failed: STAT_FAILED.load(Ordering::Relaxed),
        queued: PENDING_COUNT.load(Ordering::Relaxed),
        running: STAT_RUNNING.load(Ordering::Relaxed),
        avg_inference_time_ns: avg,
        batches_dispatched: STAT_BATCHES_DISPATCHED.load(Ordering::Relaxed),
        batches_full: STAT_BATCHES_FULL.load(Ordering::Relaxed),
        batches_timeout: STAT_BATCHES_TIMEOUT.load(Ordering::Relaxed),
        bypassed_deadline: STAT_BYPASSED_DEADLINE.load(Ordering::Relaxed),
        singleton_dispatches: STAT_SINGLETON.load(Ordering::Relaxed),
    }
}

/// Reset all stats counters. Used by tests and the stress workload to
/// observe deltas without restart.
pub fn reset_stats() {
    STAT_TOTAL_SUBMITTED.store(0, Ordering::Relaxed);
    STAT_COMPLETED.store(0, Ordering::Relaxed);
    STAT_FAILED.store(0, Ordering::Relaxed);
    STAT_BATCHES_DISPATCHED.store(0, Ordering::Relaxed);
    STAT_BATCHES_FULL.store(0, Ordering::Relaxed);
    STAT_BATCHES_TIMEOUT.store(0, Ordering::Relaxed);
    STAT_BYPASSED_DEADLINE.store(0, Ordering::Relaxed);
    STAT_SINGLETON.store(0, Ordering::Relaxed);
    STAT_TIME_TOTAL_NS.store(0, Ordering::Relaxed);
    STAT_TIME_SAMPLES.store(0, Ordering::Relaxed);
}

/// Current pending queue depth.
pub fn queue_depth() -> usize {
    PENDING_COUNT.load(Ordering::Relaxed)
}

// =============================================================================
// Synchronous batched-dispatch helper
// =============================================================================

/// Submit a single inference request and block until it completes.
///
/// Routes through the batched dispatcher when:
///   - batching mode is ON,
///   - the per-request input fits in [`MAX_INPUT_DIM`] floats,
///   - the per-request output fits in [`MAX_OUTPUT_DIM`] floats,
///   - the configured batch size is ≥ 2,
///   - at least one other request is already queued (so a batch can
///     actually form without waiting on a peer that may never arrive
///     — partial-batch flush lands in #859).
///
/// Otherwise dispatches directly via `inference::run_inference`.
///
/// # Limitation (until #859 lands)
///
/// When `2 ≤ N < batch_size` submitters happen to arrive together
/// and no further submitter shows up, all `N` waiters sit in
/// `wait_for_slot` indefinitely — `decide_role` only treats `N == 1`
/// as the singleton-fallback case. The timer-driven partial-batch
/// flush in #859 (5 ms after the first request lands) closes this
/// window. Callers that need a deterministic time-bound today must
/// either disable batching or run with `batch_size == 1`. The
/// stress workload added in #860 exercises the threshold-driven
/// path directly (N == batch_size), so it doesn't hit this case.
///
/// Returns the number of output floats written, matching the
/// underlying engine entrypoint.
///
/// # Safety
///
/// Caller obligations on `input` / `output` are identical to
/// `inference::run_inference`'s (non-null, 4-byte-aligned, live for
/// the duration of the call, no aliasing). The scheduler does not
/// take ownership — it only reads from `input` and writes to `output`
/// while the calling task is blocked here.
pub unsafe fn submit_inference_sync(
    model_index: usize,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
    _deadline: TaskDeadline,
) -> Result<usize, EngineError> {
    STAT_TOTAL_SUBMITTED.fetch_add(1, Ordering::Relaxed);

    if input.is_null() || output.is_null() || input_len == 0 || output_len == 0 {
        STAT_FAILED.fetch_add(1, Ordering::Relaxed);
        return Err(EngineError::InvalidInput);
    }

    let cfg_size = batch_size();
    let batchable = batching_enabled()
        && cfg_size >= 2
        && input_len <= MAX_INPUT_DIM
        && output_len <= MAX_OUTPUT_DIM;

    if !batchable {
        return dispatch_singleton(model_index, input, input_len, output, output_len);
    }

    // Try to push into the queue. We reserve a slot, then either become
    // the dispatcher (if we tipped the count to `cfg_size`) or fall
    // through to the singleton path (if we'd be the only request in
    // flight — without 55b's timer flush, a lone request would block
    // forever waiting for peers).
    let slot_idx = match reserve_slot(model_index, input, input_len, output, output_len) {
        Some(idx) => idx,
        None => {
            // Queue full — degrade to singleton.
            return dispatch_singleton(model_index, input, input_len, output, output_len);
        }
    };

    // After reservation, decide our role.
    let (role, batch_indices, batch_count) = decide_role(slot_idx, cfg_size);

    match role {
        Role::Solo => {
            // We're the only entry — release the slot and dispatch
            // singleton.  This keeps isolated callers from blocking on
            // batches that never form.
            release_slot_as_solo(slot_idx);
            dispatch_singleton(model_index, input, input_len, output, output_len)
        }
        Role::Dispatcher => {
            run_dispatcher(slot_idx, &batch_indices, batch_count, DispatchKind::SizeThreshold)
        }
        Role::Waiter => wait_for_slot(slot_idx),
    }
}

/// Singleton-path helper. Updates stats and forwards to the engine.
unsafe fn dispatch_singleton(
    model_index: usize,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
) -> Result<usize, EngineError> {
    STAT_SINGLETON.fetch_add(1, Ordering::Relaxed);
    STAT_RUNNING.fetch_add(1, Ordering::Relaxed);
    let start = crate::kernel_ffi::get_time_ns();
    let result = inference::run_inference(model_index, input, input_len, output, output_len);
    let elapsed = crate::kernel_ffi::get_time_ns().saturating_sub(start);
    STAT_TIME_TOTAL_NS.fetch_add(elapsed, Ordering::Relaxed);
    STAT_TIME_SAMPLES.fetch_add(1, Ordering::Relaxed);
    STAT_RUNNING.fetch_sub(1, Ordering::Relaxed);
    match &result {
        Ok(_) => STAT_COMPLETED.fetch_add(1, Ordering::Relaxed),
        Err(_) => STAT_FAILED.fetch_add(1, Ordering::Relaxed),
    };
    result
}

/// Reserve a free slot, fill it, and append its index to the pending
/// list. Returns the slot index on success or `None` when the queue
/// is full.
unsafe fn reserve_slot(
    model_index: usize,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
) -> Option<usize> {
    let _g = SchedGuard::new();

    // Find a slot in IDLE state.
    let mut chosen: Option<usize> = None;
    for (i, slot) in SLOTS.iter().enumerate() {
        if slot
            .state
            .compare_exchange(SLOT_IDLE, SLOT_PENDING, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
        {
            chosen = Some(i);
            break;
        }
    }
    let idx = chosen?;

    let slot = &SLOTS[idx];
    slot.model_index.store(model_index, Ordering::Relaxed);
    slot.input_ptr.store(input as usize, Ordering::Relaxed);
    slot.input_len.store(input_len, Ordering::Relaxed);
    slot.output_ptr.store(output as usize, Ordering::Relaxed);
    slot.output_len.store(output_len, Ordering::Relaxed);
    slot.result_value.store(0, Ordering::Relaxed);

    // Append to pending list. SAFETY: SCHED_LOCK held.
    let n = PENDING_COUNT.load(Ordering::Relaxed);
    if n >= MAX_BATCH {
        // Shouldn't happen — slot table is also size MAX_BATCH — but
        // recover gracefully by returning the slot to IDLE.
        slot.state.store(SLOT_IDLE, Ordering::Release);
        return None;
    }
    let pending = core::ptr::addr_of_mut!(PENDING) as *mut u8;
    *pending.add(n) = idx as u8;
    PENDING_COUNT.store(n + 1, Ordering::Release);

    Some(idx)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Role {
    Solo,
    Dispatcher,
    Waiter,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum DispatchKind {
    SizeThreshold,
    Timeout,
}

/// Decide which role the submitter that just reserved `slot_idx` plays.
///
/// Returns `(role, batch_indices, batch_count)`. `batch_indices` is the
/// claimed slot list (only meaningful for `Role::Dispatcher`).
fn decide_role(
    slot_idx: usize,
    cfg_size: usize,
) -> (Role, [u8; MAX_BATCH], usize) {
    let _g = SchedGuard::new();
    let n = PENDING_COUNT.load(Ordering::Relaxed);

    if n == 1 {
        // We're the only entry. Solo path — release our slot and
        // singleton-dispatch.  (We do the actual release in
        // `release_slot_as_solo` to keep the responsibility clear.)
        let _ = slot_idx;
        let _ = cfg_size;
        return (Role::Solo, [0; MAX_BATCH], 0);
    }

    if n >= cfg_size {
        // Threshold hit. Claim the whole pending list.
        let mut indices = [0u8; MAX_BATCH];
        // SAFETY: SCHED_LOCK held.
        let pending = core::ptr::addr_of!(PENDING) as *const u8;
        for i in 0..n {
            indices[i] = unsafe { *pending.add(i) };
        }
        PENDING_COUNT.store(0, Ordering::Release);
        // Mark each claimed slot as DISPATCHING.
        for i in 0..n {
            let idx = indices[i] as usize;
            SLOTS[idx].state.store(SLOT_DISPATCHING, Ordering::Release);
        }
        return (Role::Dispatcher, indices, n);
    }

    // We're somewhere in the middle of a forming batch. Wait.
    (Role::Waiter, [0; MAX_BATCH], 0)
}

/// Release a slot whose owner decided to go singleton.
fn release_slot_as_solo(slot_idx: usize) {
    let _g = SchedGuard::new();
    // We're holding the only PENDING slot — remove the last entry of
    // the pending list and return the slot to IDLE.
    let n = PENDING_COUNT.load(Ordering::Relaxed);
    // Find and remove our index from the pending list.
    let pending = core::ptr::addr_of_mut!(PENDING) as *mut u8;
    let mut found = false;
    for i in 0..n {
        // SAFETY: SCHED_LOCK held.
        if unsafe { *pending.add(i) } as usize == slot_idx {
            // Compact: shift later entries down.
            for j in i..n.saturating_sub(1) {
                unsafe { *pending.add(j) = *pending.add(j + 1) };
            }
            PENDING_COUNT.store(n - 1, Ordering::Release);
            found = true;
            break;
        }
    }
    debug_assert!(found, "solo slot must be in pending list");
    if !found {
        // Release-build defence: if invariant violated, leave the slot
        // alone rather than transitioning a slot we don't actually own
        // back to IDLE. The slot will still get cleaned up if its true
        // owner drops a `DONE_*` result through `wait_for_slot`. The
        // worst case is a leaked PENDING slot until reboot — better
        // than corrupting another submitter's view of the table.
        return;
    }
    SLOTS[slot_idx].state.store(SLOT_IDLE, Ordering::Release);
}

/// Run the batched dispatch. Called by the slot owner that triggered
/// the dispatch threshold (or, in #859, the timer flush).
///
/// `self_idx` is the dispatcher's own slot — its output is materialised
/// in place and returned to the caller. All other slots in
/// `batch_indices[0..batch_count]` are written + signalled.
///
/// # Scratch-slab serialisation
///
/// Today only the size-threshold path enters this function, and the
/// `PENDING_COUNT → 0` transition under `SCHED_LOCK` in `decide_role`
/// serialises any two threshold dispatchers (the second would see an
/// empty queue and the count couldn't tip until the first finishes).
/// Once #859 wires a timer-flush dispatcher, the timer caller and a
/// fresh size-threshold submitter can both exit `SCHED_LOCK` holding
/// disjoint slot-index lists and race on `SCRATCH_IN` / `SCRATCH_OUT`.
/// `EngineGuard` serialises the engine call itself but not the
/// surrounding scratch fill + fan-out. #859 must either hold
/// `SCHED_LOCK` across the engine call or introduce a separate
/// `DISPATCH_LOCK` before adding the second entry point.
unsafe fn run_dispatcher(
    self_idx: usize,
    batch_indices: &[u8; MAX_BATCH],
    batch_count: usize,
    kind: DispatchKind,
) -> Result<usize, EngineError> {
    // All claimed slots must agree on model_index — different-model
    // batches aren't supported. The reserve protocol doesn't currently
    // gate on this, so we verify and split if necessary. In practice
    // batching is per-model at the API layer (#860 toggle is global
    // but the OS only has one model loaded at a time today).
    let model_index = SLOTS[self_idx].model_index.load(Ordering::Relaxed);
    let per_input_dim: usize = SLOTS[self_idx].input_len.load(Ordering::Relaxed);
    for i in 0..batch_count {
        let idx = batch_indices[i] as usize;
        let mi = SLOTS[idx].model_index.load(Ordering::Relaxed);
        let in_len = SLOTS[idx].input_len.load(Ordering::Relaxed);
        if mi != model_index || in_len != per_input_dim {
            // Heterogeneous batch — dispatch every slot as singleton.
            return dispatch_heterogeneous(batch_indices, batch_count, self_idx);
        }
    }

    // Copy each slot's input into the scratch slab.
    let scratch_in = SCRATCH_IN.0.get() as *mut f32;
    let scratch_out = SCRATCH_OUT.0.get() as *mut f32;
    let in_dim = SLOTS[self_idx].input_len.load(Ordering::Relaxed);
    for i in 0..batch_count {
        let idx = batch_indices[i] as usize;
        let src = SLOTS[idx].input_ptr.load(Ordering::Relaxed) as *const f32;
        core::ptr::copy_nonoverlapping(src, scratch_in.add(i * in_dim), in_dim);
    }

    STAT_RUNNING.fetch_add(batch_count, Ordering::Relaxed);
    STAT_BATCHES_DISPATCHED.fetch_add(1, Ordering::Relaxed);
    match kind {
        DispatchKind::SizeThreshold => {
            STAT_BATCHES_FULL.fetch_add(1, Ordering::Relaxed);
        }
        DispatchKind::Timeout => {
            STAT_BATCHES_TIMEOUT.fetch_add(1, Ordering::Relaxed);
        }
    }

    let start = crate::kernel_ffi::get_time_ns();
    // Output capacity for the batched call: each slot reserved at
    // least `per_output_cap` floats; the engine writes `N * output_dim`
    // floats total. We size the receiver buffer at
    // `batch_count * MAX_OUTPUT_DIM` so undersized output buffers in
    // individual slots don't truncate the batched call.
    let batched_out_cap = batch_count * MAX_OUTPUT_DIM;
    let result = crate::inference::engine::run_inference_batched(
        model_index,
        batch_count,
        scratch_in,
        in_dim * batch_count,
        scratch_out,
        batched_out_cap,
    );
    let elapsed = crate::kernel_ffi::get_time_ns().saturating_sub(start);
    STAT_TIME_TOTAL_NS.fetch_add(elapsed, Ordering::Relaxed);
    STAT_TIME_SAMPLES.fetch_add(1, Ordering::Relaxed);
    STAT_RUNNING.fetch_sub(batch_count, Ordering::Relaxed);

    match result {
        Ok(total_out) => {
            // Per-request output is total_out / batch_count. The engine
            // must return N * per_sample_output; a non-multiple total
            // would silently misalign every caller's output buffer, so
            // refuse it with InternalError and fail every claimed slot
            // rather than corrupt downstream readers.
            if total_out % batch_count != 0 {
                for i in 0..batch_count {
                    let idx = batch_indices[i] as usize;
                    let slot = &SLOTS[idx];
                    slot.result_value
                        .store(encode_engine_error(EngineError::InternalError), Ordering::Relaxed);
                    STAT_FAILED.fetch_add(1, Ordering::Relaxed);
                    if idx != self_idx {
                        slot.state.store(SLOT_DONE_ERR, Ordering::Release);
                    }
                }
                SLOTS[self_idx].state.store(SLOT_IDLE, Ordering::Release);
                return Err(EngineError::InternalError);
            }
            let per_out = total_out / batch_count;
            // Fan-out: copy slice into each caller's output buffer and
            // signal completion. The dispatcher itself includes its
            // own slot, but skips the waker (we return synchronously).
            for i in 0..batch_count {
                let idx = batch_indices[i] as usize;
                let slot = &SLOTS[idx];
                let cap = slot.output_len.load(Ordering::Relaxed);
                let n = per_out.min(cap);
                let dst = slot.output_ptr.load(Ordering::Relaxed) as *mut f32;
                core::ptr::copy_nonoverlapping(scratch_out.add(i * per_out), dst, n);
                slot.result_value.store(n as u32, Ordering::Relaxed);
                STAT_COMPLETED.fetch_add(1, Ordering::Relaxed);
                if idx != self_idx {
                    slot.state.store(SLOT_DONE_OK, Ordering::Release);
                }
            }
            // Self-slot transitions back to IDLE; we already have the result.
            let self_n = SLOTS[self_idx].result_value.load(Ordering::Relaxed) as usize;
            SLOTS[self_idx].state.store(SLOT_IDLE, Ordering::Release);
            Ok(self_n)
        }
        Err(e) => {
            let err_code = encode_engine_error(e);
            for i in 0..batch_count {
                let idx = batch_indices[i] as usize;
                let slot = &SLOTS[idx];
                slot.result_value.store(err_code, Ordering::Relaxed);
                STAT_FAILED.fetch_add(1, Ordering::Relaxed);
                if idx != self_idx {
                    slot.state.store(SLOT_DONE_ERR, Ordering::Release);
                }
            }
            SLOTS[self_idx].state.store(SLOT_IDLE, Ordering::Release);
            Err(e)
        }
    }
}

/// Heterogeneous fallback: each slot in `batch_indices` is dispatched
/// individually via the singleton path. Returns the dispatcher's own
/// result.
unsafe fn dispatch_heterogeneous(
    batch_indices: &[u8; MAX_BATCH],
    batch_count: usize,
    self_idx: usize,
) -> Result<usize, EngineError> {
    let mut self_result: Result<usize, EngineError> = Err(EngineError::InternalError);
    for i in 0..batch_count {
        let idx = batch_indices[i] as usize;
        let slot = &SLOTS[idx];
        let mi = slot.model_index.load(Ordering::Relaxed);
        let in_ptr = slot.input_ptr.load(Ordering::Relaxed) as *const f32;
        let in_len = slot.input_len.load(Ordering::Relaxed);
        let out_ptr = slot.output_ptr.load(Ordering::Relaxed) as *mut f32;
        let out_len = slot.output_len.load(Ordering::Relaxed);

        let r = dispatch_singleton(mi, in_ptr, in_len, out_ptr, out_len);
        match r {
            Ok(n) => {
                slot.result_value.store(n as u32, Ordering::Relaxed);
                if idx == self_idx {
                    self_result = Ok(n);
                    slot.state.store(SLOT_IDLE, Ordering::Release);
                } else {
                    slot.state.store(SLOT_DONE_OK, Ordering::Release);
                }
            }
            Err(e) => {
                slot.result_value
                    .store(encode_engine_error(e), Ordering::Relaxed);
                if idx == self_idx {
                    self_result = Err(e);
                    slot.state.store(SLOT_IDLE, Ordering::Release);
                } else {
                    slot.state.store(SLOT_DONE_ERR, Ordering::Release);
                }
            }
        }
    }
    self_result
}

/// Wait on a slot's completion atomic. Used by non-dispatcher
/// submitters. Returns the result the dispatcher published.
unsafe fn wait_for_slot(slot_idx: usize) -> Result<usize, EngineError> {
    let slot = &SLOTS[slot_idx];
    loop {
        let s = slot.state.load(Ordering::Acquire);
        match s {
            SLOT_DONE_OK => {
                let n = slot.result_value.load(Ordering::Relaxed) as usize;
                slot.state.store(SLOT_IDLE, Ordering::Release);
                return Ok(n);
            }
            SLOT_DONE_ERR => {
                let code = slot.result_value.load(Ordering::Relaxed);
                slot.state.store(SLOT_IDLE, Ordering::Release);
                return Err(decode_engine_error(code));
            }
            _ => {
                // Yield so the dispatcher (which may share this CPU
                // under cooperative scheduling) can run.
                sched_yield();
            }
        }
    }
}

// =============================================================================
// Internal: timer flush hook (used by #859)
//
// The three functions below — `flush_pending_for_test`,
// `run_dispatcher_external`, and `dispatch_heterogeneous_external` —
// are dead code in #857 (no caller). They exist so #859 (timer flush
// + deadline-aware bypass) can wire them up without churning #857's
// API surface. The unified design that lands in #859 replaces them
// with a single `run_dispatcher` callable from both the size-threshold
// path and the waiter-driven timer-flush path; expect this entire
// block to be removed when #859 merges.
// =============================================================================

/// Force-flush the currently pending batch. Reserved for #859 — not
/// reachable from #857 production paths; only used by the test helper
/// in `rust_batch_inference_test`. Gated behind `SCHED_LOCK` so a
/// forming batch isn't simultaneously claimed by both the timer and a
/// fresh submitter. Returns the number of dispatched slots, or 0 if
/// the queue was empty.
#[doc(hidden)]
pub unsafe fn flush_pending_for_test() -> usize {
    let (indices, count, run) = {
        let _g = SchedGuard::new();
        let n = PENDING_COUNT.load(Ordering::Relaxed);
        if n == 0 {
            return 0;
        }
        let mut indices = [0u8; MAX_BATCH];
        let pending = core::ptr::addr_of!(PENDING) as *const u8;
        for i in 0..n {
            indices[i] = *pending.add(i);
        }
        PENDING_COUNT.store(0, Ordering::Release);
        for i in 0..n {
            let idx = indices[i] as usize;
            SLOTS[idx].state.store(SLOT_DISPATCHING, Ordering::Release);
        }
        (indices, n, true)
    };
    if !run {
        return 0;
    }
    // The "self_idx" for a timer-driven flush isn't a real submitter,
    // so we synthesise a sentinel that no real slot will match. The
    // dispatcher signals every claimed slot (including what would have
    // been self) via DONE_*, and we ignore the returned result.
    let self_idx = usize::MAX;
    let _ = run_dispatcher_external(self_idx, &indices, count, DispatchKind::Timeout);
    count
}

/// Variant of `run_dispatcher` where the caller is not itself one of
/// the claimed slots (timer-driven flush). All slots in
/// `batch_indices[0..batch_count]` are signalled via the mailbox.
///
/// Scaffolding for #859 — see the section comment above
/// `flush_pending_for_test`. Removed when #859 lands and the unified
/// `run_dispatcher` absorbs this path.
unsafe fn run_dispatcher_external(
    _self_idx: usize,
    batch_indices: &[u8; MAX_BATCH],
    batch_count: usize,
    kind: DispatchKind,
) -> Result<(), EngineError> {
    if batch_count == 0 {
        return Ok(());
    }
    let model_index = SLOTS[batch_indices[0] as usize]
        .model_index
        .load(Ordering::Relaxed);
    let in_dim = SLOTS[batch_indices[0] as usize]
        .input_len
        .load(Ordering::Relaxed);
    for i in 0..batch_count {
        let idx = batch_indices[i] as usize;
        if SLOTS[idx].model_index.load(Ordering::Relaxed) != model_index
            || SLOTS[idx].input_len.load(Ordering::Relaxed) != in_dim
        {
            // Heterogeneous: dispatch each individually.
            return dispatch_heterogeneous_external(batch_indices, batch_count);
        }
    }

    let scratch_in = SCRATCH_IN.0.get() as *mut f32;
    let scratch_out = SCRATCH_OUT.0.get() as *mut f32;
    for i in 0..batch_count {
        let idx = batch_indices[i] as usize;
        let src = SLOTS[idx].input_ptr.load(Ordering::Relaxed) as *const f32;
        core::ptr::copy_nonoverlapping(src, scratch_in.add(i * in_dim), in_dim);
    }

    STAT_RUNNING.fetch_add(batch_count, Ordering::Relaxed);
    STAT_BATCHES_DISPATCHED.fetch_add(1, Ordering::Relaxed);
    match kind {
        DispatchKind::SizeThreshold => STAT_BATCHES_FULL.fetch_add(1, Ordering::Relaxed),
        DispatchKind::Timeout => STAT_BATCHES_TIMEOUT.fetch_add(1, Ordering::Relaxed),
    };

    let start = crate::kernel_ffi::get_time_ns();
    let batched_out_cap = batch_count * MAX_OUTPUT_DIM;
    let result = crate::inference::engine::run_inference_batched(
        model_index,
        batch_count,
        scratch_in,
        in_dim * batch_count,
        scratch_out,
        batched_out_cap,
    );
    let elapsed = crate::kernel_ffi::get_time_ns().saturating_sub(start);
    STAT_TIME_TOTAL_NS.fetch_add(elapsed, Ordering::Relaxed);
    STAT_TIME_SAMPLES.fetch_add(1, Ordering::Relaxed);
    STAT_RUNNING.fetch_sub(batch_count, Ordering::Relaxed);

    match result {
        Ok(total_out) => {
            // Same per-slot-divisibility guard as `run_dispatcher`.
            if total_out % batch_count != 0 {
                for i in 0..batch_count {
                    let idx = batch_indices[i] as usize;
                    let slot = &SLOTS[idx];
                    slot.result_value
                        .store(encode_engine_error(EngineError::InternalError), Ordering::Relaxed);
                    STAT_FAILED.fetch_add(1, Ordering::Relaxed);
                    slot.state.store(SLOT_DONE_ERR, Ordering::Release);
                }
                return Err(EngineError::InternalError);
            }
            let per_out = total_out / batch_count;
            for i in 0..batch_count {
                let idx = batch_indices[i] as usize;
                let slot = &SLOTS[idx];
                let cap = slot.output_len.load(Ordering::Relaxed);
                let n = per_out.min(cap);
                let dst = slot.output_ptr.load(Ordering::Relaxed) as *mut f32;
                core::ptr::copy_nonoverlapping(scratch_out.add(i * per_out), dst, n);
                slot.result_value.store(n as u32, Ordering::Relaxed);
                STAT_COMPLETED.fetch_add(1, Ordering::Relaxed);
                slot.state.store(SLOT_DONE_OK, Ordering::Release);
            }
            Ok(())
        }
        Err(e) => {
            let err_code = encode_engine_error(e);
            for i in 0..batch_count {
                let idx = batch_indices[i] as usize;
                let slot = &SLOTS[idx];
                slot.result_value.store(err_code, Ordering::Relaxed);
                STAT_FAILED.fetch_add(1, Ordering::Relaxed);
                slot.state.store(SLOT_DONE_ERR, Ordering::Release);
            }
            Err(e)
        }
    }
}

/// Heterogeneous-batch fallback for the timer-flush path (no
/// self-slot — the caller isn't one of `batch_indices`).
///
/// Scaffolding for #859 — see the section comment above
/// `flush_pending_for_test`. Removed when #859 lands.
unsafe fn dispatch_heterogeneous_external(
    batch_indices: &[u8; MAX_BATCH],
    batch_count: usize,
) -> Result<(), EngineError> {
    for i in 0..batch_count {
        let idx = batch_indices[i] as usize;
        let slot = &SLOTS[idx];
        let mi = slot.model_index.load(Ordering::Relaxed);
        let in_ptr = slot.input_ptr.load(Ordering::Relaxed) as *const f32;
        let in_len = slot.input_len.load(Ordering::Relaxed);
        let out_ptr = slot.output_ptr.load(Ordering::Relaxed) as *mut f32;
        let out_len = slot.output_len.load(Ordering::Relaxed);

        let r = dispatch_singleton(mi, in_ptr, in_len, out_ptr, out_len);
        match r {
            Ok(n) => {
                slot.result_value.store(n as u32, Ordering::Relaxed);
                slot.state.store(SLOT_DONE_OK, Ordering::Release);
            }
            Err(e) => {
                slot.result_value
                    .store(encode_engine_error(e), Ordering::Relaxed);
                slot.state.store(SLOT_DONE_ERR, Ordering::Release);
            }
        }
    }
    Ok(())
}

fn encode_engine_error(e: EngineError) -> u32 {
    match e {
        EngineError::ModelNotFound => 1,
        EngineError::WorkspaceExhausted => 2,
        EngineError::UnsupportedOp => 3,
        EngineError::ShapeMismatch => 4,
        EngineError::ShapeOverflow => 5,
        EngineError::InvalidInput => 6,
        EngineError::WeightNotFound => 7,
        EngineError::InternalError => 8,
    }
}

fn decode_engine_error(code: u32) -> EngineError {
    match code {
        1 => EngineError::ModelNotFound,
        2 => EngineError::WorkspaceExhausted,
        3 => EngineError::UnsupportedOp,
        4 => EngineError::ShapeMismatch,
        5 => EngineError::ShapeOverflow,
        6 => EngineError::InvalidInput,
        7 => EngineError::WeightNotFound,
        _ => EngineError::InternalError,
    }
}

// =============================================================================
// Legacy InferenceScheduler API surface (kept for the public re-exports
// in `sched::mod`; thin wrapper over the static singleton above)
// =============================================================================

/// Inference scheduler handle — legacy token-based API surface.
///
/// **Prefer [`submit_inference_sync`] for new code.** This struct
/// exists only to preserve the shape of the prior Phase-3 skeleton;
/// the metadata-only [`InferenceRequest`] it consumes doesn't carry
/// buffer pointers, so `submit()` / `cancel()` / `get_result()` all
/// return `NotImplemented`. All real state lives in module-static
/// atomics; the only methods that do real work are `start()`,
/// `stop()`, `is_running()`, `stats()`, and `queue_depth()`, which
/// are thin wrappers over the static singleton's free functions
/// (`set_batching_enabled`, `batching_enabled`, `stats`,
/// `queue_depth`).
pub struct InferenceScheduler {
    _phantom: (),
}

impl InferenceScheduler {
    /// Default maximum queue depth.
    pub const DEFAULT_QUEUE_DEPTH: usize = MAX_BATCH;

    /// Create an InferenceScheduler handle.
    pub fn new() -> Self {
        Self { _phantom: () }
    }

    /// Create an InferenceScheduler handle with a custom queue depth.
    /// The configured depth is clamped to `[1, MAX_BATCH]` and applied
    /// to the global batch-size threshold.
    pub fn with_queue_depth(max_depth: usize) -> Self {
        set_batch_size(max_depth);
        Self { _phantom: () }
    }

    /// Enable the scheduler (turns batching mode on).
    pub fn start(&mut self) -> Result<(), SchedulerError> {
        set_batching_enabled(true);
        Ok(())
    }

    /// Disable the scheduler (turns batching mode off).
    pub fn stop(&mut self) {
        set_batching_enabled(false);
    }

    /// Whether batching mode is currently on.
    pub fn is_running(&self) -> bool {
        batching_enabled()
    }

    /// Token-based submit — NOT implemented end-to-end.
    ///
    /// The metadata-only `InferenceRequest` doesn't carry buffer
    /// pointers, so this entry point has no way to deposit a request
    /// into the queue. Real submission goes through
    /// `submit_inference_sync` (which takes the buffers directly).
    /// The previous shipping shape — silently incrementing
    /// `total_submitted` and returning `Ok(request.id)` while
    /// dropping the request on the floor — has been removed because
    /// it would lose work for any caller who actually used it.
    pub fn submit(&mut self, _request: InferenceRequest) -> Result<u64, SchedulerError> {
        Err(SchedulerError::NotImplemented)
    }

    pub fn cancel(&mut self, _request_id: u64) -> Result<(), SchedulerError> {
        // Synchronous dispatch model — no in-flight cancellation.
        Err(SchedulerError::NotImplemented)
    }

    pub fn get_result(
        &mut self,
        _request_id: u64,
        _timeout_ms: u32,
    ) -> Result<InferenceResult, SchedulerError> {
        // The synchronous `submit_inference_sync` is the supported
        // path; this token-based API is preserved for future async
        // callers but not implemented end-to-end.
        Err(SchedulerError::NotImplemented)
    }

    pub fn stats(&self) -> SchedulerStats {
        stats()
    }

    pub fn queue_depth(&self) -> usize {
        queue_depth()
    }
}

impl Default for InferenceScheduler {
    fn default() -> Self {
        Self::new()
    }
}
