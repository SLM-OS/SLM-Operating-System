//! Per-session state for the SLM decoder.
//!
//! M5.2 of the SLM integration plan (see
//! `docs/plans/slm-integration-plan.md` §M5 and
//! `docs/design/slm-integration.md`). A `Session` ties together a loaded
//! GGUF model (looked up in [`crate::slm::registry`]), an FP16 KV cache
//! ([`crate::slm::kv_cache::KvCache`]), and a sampler
//! ([`crate::slm::sampler`]). The session is the unit of conversation
//! state — `prompt()` calls drive the decoder loop, `reset()` clears the
//! KV cache for a fresh conversation, `request_stop()` cooperatively
//! cancels an in-flight prompt, and `close()` releases the slot.
//!
//! A 4-slot fixed table mirrors the registry's pattern from M1.4: bounded
//! to keep the `no_std` runtime predictable, and protected by a
//! SpinGuard for cross-FFI access from the M7 shell. The guard is only
//! ever held for table bookkeeping: a prompt takes the session out of
//! its slot with [`checkout`], so a decode that runs for minutes doesn't
//! keep `slm stop` or `slm stats` spinning on the lock.
//!
//! `no_std` + `alloc` only.

#![allow(clippy::module_name_repetitions)]

use alloc::boxed::Box;
use alloc::sync::Arc;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicBool, Ordering};

use crate::inference::ops_transformer::{RopePairing, RopeTable};
use crate::slm::{
    gguf::ArchInfo,
    gpu_program::{BuildError, GpuProgram},
    kv_cache::KvCache,
    registry,
    sampler::{Sampler, SamplerState},
};

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Maximum number of concurrent open sessions. Same envelope as the
/// model registry — a session needs a non-trivial KV-cache allocation
/// (≈100 MiB for Qwen2.5-1.5B at 4096 context) so the cap stays small.
pub const SLM_MAX_SESSIONS: usize = 4;

/// Lifecycle state of a [`Session`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionState {
    /// Created via [`Session::open`] or freshly [`Session::reset`]'d;
    /// ready for the next prompt.
    Open,
    /// Inside a `prompt()` call. The decoder loop polls
    /// [`Session::stop_flag`] at every yield boundary.
    Decoding,
    /// The stop flag was honoured mid-decode. The stop lands between
    /// forward steps, so every committed KV position is complete and the
    /// next prompt continues the conversation; [`Session::reset`] is
    /// only needed to start a fresh one.
    Stopped,
    /// Slot released; subsequent operations return errors.
    Closed,
}

/// Where a session's forward steps run. Chosen at `slm launch`
/// ([`Session::choose_path`]) from [`GpuMode`] and what the boot offers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ForwardPath {
    /// [`GpuProgram`]: the whole step as one GPU command stream.
    Program,
    /// `forward_one`, each op going to the GPU if the tier table says so.
    /// Only without a channel the program can use (kexec boots).
    PerOp,
    /// `forward_one` on this CPU, every op.
    Cpu,
}

/// What `slm launch --gpu` asked for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum GpuMode {
    /// Use whatever the tier table says, falling back per op to the CPU.
    Auto = 0,
    /// Never dispatch to the GPU for this session.
    Cpu = 1,
    /// Like `Auto`, but `slm launch` refuses when no op has a GPU tier.
    On = 2,
}

impl GpuMode {
    pub fn from_u32(v: u32) -> Option<Self> {
        match v {
            0 => Some(GpuMode::Auto),
            1 => Some(GpuMode::Cpu),
            2 => Some(GpuMode::On),
            _ => None,
        }
    }
}

/// `--threads` when the operator didn't give one (spec default).
pub const DEFAULT_THREADS: u32 = 4;
/// Upper bound on `--threads`. Jetson Orin Nano has six cores.
pub const MAX_THREADS: u32 = 64;

/// Why the last prompt stopped generating. Values are FFI: they mirror
/// `SLM_STOP_*` in `kernel/include/slm_ffi.h`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum StopReason {
    /// No prompt has run yet.
    None = 0,
    /// The model sampled an end-of-generation token.
    Eos = 1,
    /// Hit the per-prompt `--max` cap.
    MaxTokens = 2,
    /// `slm stop` or Ctrl-C.
    Stopped = 3,
    /// The token callback asked to stop.
    Callback = 4,
    /// The KV cache has no room left: either the prompt didn't fit
    /// (nothing ran) or generation filled it.
    ContextFull = 5,
    /// The forward pass produced no logits (e.g. a model without the
    /// per-layer weights, or a shape error).
    NoLogits = 6,
    /// Nothing to do: the prompt text was empty.
    EmptyPrompt = 7,
}

/// Per-session decoder state.
///
/// All fields are `pub` to keep the decoder loop in `decoder.rs`
/// straightforward — `Session` is an implementation-detail aggregate
/// rather than an encapsulated type. The 4-slot table below is the
/// real ownership boundary; FFI callers never see the struct directly.
pub struct Session {
    /// Index into the SLM registry. Must be a valid loaded slot for
    /// the lifetime of the session.
    pub model_handle: u32,
    /// Cached architecture metadata — copied from the registry on
    /// [`Session::open`] so the decoder doesn't have to re-acquire the
    /// registry lock per token.
    pub arch: ArchInfo,
    /// Vocab size from the registry (cached for the same reason).
    pub vocab_size: u32,
    /// Caller-supplied context ceiling (capped at `arch.context_length`).
    pub max_ctx: usize,
    /// Per-session KV cache.
    pub kv: KvCache,
    /// Sampler configuration (immutable for the session's life).
    pub sampler_cfg: Sampler,
    /// Sampler PRNG state (mutated on every token).
    pub sampler_state: SamplerState,
    /// Lifecycle state — see [`SessionState`].
    pub state: SessionState,
    /// Cooperative-cancel flag. The decoder loop checks it between
    /// forward steps. It's shared with the slot table so `rust_slm_stop`
    /// can set it while this session is checked out for a prompt.
    pub stop_flag: Arc<AtomicBool>,
    /// What `--gpu` asked for. See [`GpuMode`].
    pub gpu_mode: GpuMode,
    /// Where forward steps run now.
    pub path: ForwardPath,
    /// The program behind [`ForwardPath::Program`].
    pub gpu: Option<Box<GpuProgram>>,
    /// Why the session isn't (or stopped being) on the program.
    pub gpu_note: Option<&'static str>,
    /// Tokens the program has run for this session, kept after a fallback.
    pub gpu_tokens: u64,
    /// Tokens `forward_one` has run.
    pub cpu_tokens: u64,
    /// GPU time over [`Self::gpu_tokens`], for `slm gpu`.
    pub gpu_us_per_token: u64,
    /// What `--threads` asked for. Stored only: the forward path is
    /// single-threaded until the worker pool lands.
    pub threads: u32,
    /// Why the last prompt stopped.
    pub last_stop_reason: StopReason,
    /// The last sampled token when a prompt ended without feeding it to
    /// the model and it wasn't an end-of-generation token. The next chat
    /// turn feeds it so the KV cache holds the reply as displayed.
    pub pending_token: Option<u32>,
    /// Number of prompts processed since the last [`Session::reset`]
    /// or open.
    pub prompts_completed: u32,
    /// Cumulative input tokens (prefill) across all prompts.
    pub tokens_in: u32,
    /// Cumulative output tokens (decoded) across all prompts.
    pub tokens_out: u32,
    /// Last prompt's time-to-first-token in nanoseconds.
    pub last_ttft_ns: u64,
    /// Last prompt's total decode duration in nanoseconds.
    pub last_decode_ns: u64,
}

impl Session {
    /// Open a new session over `model_handle`.
    ///
    /// Returns `None` if:
    /// - `model_handle` doesn't index a loaded model
    /// - `max_ctx` is 0 or KV-cache allocation fails (e.g. dimension
    ///   overflow per [`KvCache::new`])
    pub fn open(
        model_handle: u32,
        max_ctx: usize,
        sampler_cfg: Sampler,
        seed: u64,
    ) -> Option<Self> {
        let info = registry::get_info(model_handle as usize)?;
        // Cap context at the model's trained limit. A caller-supplied
        // ceiling above `context_length` would just waste memory and
        // produce garbage past the trained range.
        let trained = info.context_length as usize;
        if max_ctx == 0 || trained == 0 {
            return None;
        }
        let max_ctx = if max_ctx > trained { trained } else { max_ctx };

        let kv = KvCache::new(
            info.block_count as usize,
            max_ctx,
            info.head_count_kv as usize,
            info.head_dim as usize,
        )?;

        // Reconstruct an `ArchInfo` from the C-layout snapshot. The
        // registry stores the original `ArchInfo`, but `get_info`
        // returns a flattened `SlmModelInfoC` — we only need the few
        // fields the decoder reads.
        let arch = ArchInfo {
            architecture: arch_kind_from_bytes(&info.architecture),
            block_count: info.block_count,
            embedding_length: info.embedding_length,
            head_count: info.head_count,
            head_count_kv: info.head_count_kv,
            head_dim: info.head_dim,
            feed_forward_length: info.feed_forward_length,
            context_length: info.context_length,
            rope_freq_base: info.rope_freq_base,
        };

        Some(Self {
            model_handle,
            arch,
            vocab_size: info.vocab_size,
            max_ctx,
            kv,
            sampler_cfg,
            sampler_state: SamplerState::new(seed),
            state: SessionState::Open,
            stop_flag: Arc::new(AtomicBool::new(false)),
            gpu_mode: GpuMode::Auto,
            path: ForwardPath::PerOp,
            gpu: None,
            gpu_note: None,
            gpu_tokens: 0,
            cpu_tokens: 0,
            gpu_us_per_token: 0,
            threads: DEFAULT_THREADS,
            last_stop_reason: StopReason::None,
            pending_token: None,
            prompts_completed: 0,
            tokens_in: 0,
            tokens_out: 0,
            last_ttft_ns: 0,
            last_decode_ns: 0,
        })
    }

    /// Reset KV cache and counters for a fresh conversation, keeping
    /// the loaded model and sampler config. Transitions any state
    /// other than [`SessionState::Closed`] back to [`SessionState::Open`].
    pub fn reset(&mut self) {
        self.kv.clear();
        if let Some(p) = self.gpu.as_mut() {
            p.history.clear();
        }
        self.stop_flag.store(false, Ordering::Release);
        self.pending_token = None;
        self.last_stop_reason = StopReason::None;
        self.prompts_completed = 0;
        self.tokens_in = 0;
        self.tokens_out = 0;
        self.last_ttft_ns = 0;
        self.last_decode_ns = 0;
        if self.state != SessionState::Closed {
            self.state = SessionState::Open;
        }
    }

    /// Apply `--gpu`: build the program if the mode wants one. `Err` is for
    /// `--gpu on` with no program, and leaves the session as it was.
    pub fn choose_path(&mut self, mode: GpuMode) -> Result<(), &'static str> {
        if mode == GpuMode::Cpu {
            // Program's KV cache goes with it, so a session with history
            // starts over: `slm launch` configures before any prompt.
            if self.gpu.take().is_some() {
                self.kv.ensure_storage();
                self.kv.clear();
            }
            self.gpu_note = None;
            self.gpu_mode = mode;
            self.path = ForwardPath::Cpu;
            return Ok(());
        }
        // A program in place stays: the kernel is the same either way.
        if self.gpu.is_some() {
            self.gpu_mode = mode;
            return Ok(());
        }
        // The program starts with an empty cache; a session with history
        // keeps what it has.
        if self.kv.len != 0 {
            const WHY: &str = "session already holds positions";
            self.gpu_note = Some(WHY);
            if mode == GpuMode::On {
                return Err(WHY);
            }
            self.gpu_mode = mode;
            self.path = ForwardPath::Cpu;
            return Ok(());
        }
        let vocab = self.vocab_size;
        let ctx = self.max_ctx;
        let head_dim = self.arch.head_dim as usize;
        let rope = RopeTable::new(
            head_dim,
            ctx,
            self.arch.rope_freq_base,
            RopePairing::for_arch(self.arch.architecture),
        );
        let built = registry::with_loaded_slm(self.model_handle as usize, |slm| {
            GpuProgram::build(slm, ctx, vocab, &rope.cos_sin)
        });
        match built {
            Some(Ok(p)) => {
                self.kv.release_storage();
                self.gpu = Some(p);
                self.gpu_note = None;
                self.path = ForwardPath::Program;
                self.gpu_mode = mode;
                Ok(())
            }
            Some(Err(e)) => {
                let (why, path) = match e {
                    BuildError::NoBackend => {
                        ("no SLM-OS-built GPU channel (needs a UEFI-direct boot)", ForwardPath::PerOp)
                    }
                    BuildError::Failed(w) => (w, ForwardPath::Cpu),
                };
                if mode == GpuMode::On {
                    self.gpu_note = Some(why);
                    return Err(why);
                }
                self.gpu_note = Some(why);
                self.gpu_mode = mode;
                self.path = path;
                Ok(())
            }
            None => Err("model not loaded"),
        }
    }

    /// Request cooperative cancellation. The decoder loop notices before
    /// its next forward step and transitions to
    /// [`SessionState::Stopped`].
    pub fn request_stop(&self) {
        self.stop_flag.store(true, Ordering::Release);
    }

    /// Whether [`Self::request_stop`] has fired and not been honoured.
    pub fn stop_requested(&self) -> bool {
        self.stop_flag.load(Ordering::Acquire)
    }

    fn snapshot(&self) -> SessionSnapshot {
        SessionSnapshot {
            model_handle: self.model_handle,
            gpu_mode: self.gpu_mode,
            path: self.path,
            gpu_note: self.gpu_note,
            gpu_tokens: self.gpu_tokens,
            cpu_tokens: self.cpu_tokens,
            gpu_us_per_token: self.gpu_us_per_token,
            launches: self.gpu.as_ref().map_or(0, |p| p.launches),
            barriers: self.gpu.as_ref().map_or(0, |p| p.barriers),
            arena_bytes: self.gpu.as_ref().map_or(0, |p| p.data_bytes),
            max_batch: self.gpu.as_ref().map_or(0, |p| p.max_batch),
            weights_big: self.gpu.as_ref().is_some_and(|p| p.weights_big),
            threads: self.threads,
            max_ctx: self.max_ctx,
            kv_len: self.kv.len,
            state: self.state,
            busy: false,
            prompts_completed: self.prompts_completed,
            tokens_in: self.tokens_in,
            tokens_out: self.tokens_out,
            last_ttft_ns: self.last_ttft_ns,
            last_decode_ns: self.last_decode_ns,
            last_stop_reason: self.last_stop_reason,
        }
    }

    /// Mark the session closed. The slot table will free the slot
    /// shortly afterward; calling `close` directly on the struct is
    /// the FFI-safe way to record intent.
    pub fn close(&mut self) {
        self.state = SessionState::Closed;
    }
}

/// Resolve the raw architecture bytes back to an [`ArchKind`]. The
/// registry stores `b"qwen2"` / `b"llama"` null-padded; we just match
/// the prefix.
fn arch_kind_from_bytes(bytes: &[u8]) -> crate::slm::gguf::ArchKind {
    use crate::slm::gguf::ArchKind;
    if bytes.starts_with(b"qwen2") {
        ArchKind::Qwen2
    } else {
        // Default to LLaMA for any other tag — `validate_for_inference`
        // already rejected unknown architectures at load time, so the
        // only string the registry can hold besides "qwen2" is "llama".
        ArchKind::Llama
    }
}

// ---------------------------------------------------------------------------
// Session table — 4-slot fixed array protected by a spinlock.
//
// The FFI layer takes a `session_id` (= slot index). The lock covers
// table bookkeeping only and is never held across a decode: `checkout`
// moves the session out of its slot and leaves a `Leased` record behind
// so the other verbs can still answer while a prompt runs. The table is
// private; only the public functions below cross the module boundary.
// ---------------------------------------------------------------------------

/// Copy of a session's externally visible state.
#[derive(Debug, Clone, Copy)]
pub struct SessionSnapshot {
    pub model_handle: u32,
    pub gpu_mode: GpuMode,
    pub path: ForwardPath,
    pub gpu_note: Option<&'static str>,
    pub gpu_tokens: u64,
    pub cpu_tokens: u64,
    pub gpu_us_per_token: u64,
    pub launches: u32,
    pub barriers: u32,
    pub arena_bytes: u64,
    pub max_batch: u32,
    pub weights_big: bool,
    pub threads: u32,
    pub max_ctx: usize,
    pub kv_len: usize,
    pub state: SessionState,
    /// A prompt is in flight. The counters are then as of its start.
    pub busy: bool,
    pub prompts_completed: u32,
    pub tokens_in: u32,
    pub tokens_out: u32,
    pub last_ttft_ns: u64,
    pub last_decode_ns: u64,
    pub last_stop_reason: StopReason,
}

/// Why a slot operation couldn't run.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SlotError {
    /// Out of range, or nothing is open there.
    Empty,
    /// A prompt holds the session.
    Busy,
}

/// Slot record for a session that's out on a [`SessionLease`].
struct Leased {
    stop: Arc<AtomicBool>,
    snapshot: SessionSnapshot,
}

enum Slot {
    Empty,
    Ready(Session),
    Leased(Leased),
}

const EMPTY_SLOT: Slot = Slot::Empty;

static SESSION_LOCK: AtomicBool = AtomicBool::new(false);
static mut SESSIONS: [Slot; SLM_MAX_SESSIONS] = [EMPTY_SLOT; SLM_MAX_SESSIONS];

struct SpinGuard;

impl SpinGuard {
    fn new() -> Self {
        while SESSION_LOCK
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
        SESSION_LOCK.store(false, Ordering::Release);
    }
}

/// Insert a freshly-opened session into the first free slot.
///
/// Returns the slot index or `None` if every slot is occupied. The
/// caller usually calls [`Session::open`] first and hands the result
/// here.
pub fn insert(session: Session) -> Option<usize> {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to SESSIONS.
    let slots = unsafe { &mut *core::ptr::addr_of_mut!(SESSIONS) };
    for (i, slot) in slots.iter_mut().enumerate() {
        if matches!(slot, Slot::Empty) {
            *slot = Slot::Ready(session);
            return Some(i);
        }
    }
    None
}

/// Free the slot at `index` and return the session, so the caller can
/// run any extra teardown.
pub fn remove(index: usize) -> Result<Session, SlotError> {
    if index >= SLM_MAX_SESSIONS {
        return Err(SlotError::Empty);
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    let slots = unsafe { &mut *core::ptr::addr_of_mut!(SESSIONS) };
    match core::mem::replace(&mut slots[index], Slot::Empty) {
        Slot::Ready(session) => Ok(session),
        Slot::Empty => Err(SlotError::Empty),
        leased @ Slot::Leased(_) => {
            slots[index] = leased;
            Err(SlotError::Busy)
        }
    }
}

/// Run a closure against the session at `index` while the table lock
/// is held. The closure must be short and must not re-enter the table.
/// Fails with [`SlotError::Busy`] while a prompt holds the session.
pub fn try_with_session<R, F>(index: usize, f: F) -> Result<R, SlotError>
where
    F: FnOnce(&mut Session) -> R,
{
    if index >= SLM_MAX_SESSIONS {
        return Err(SlotError::Empty);
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to SESSIONS for the
    // duration of `f`.
    let slots = unsafe { &mut *core::ptr::addr_of_mut!(SESSIONS) };
    match &mut slots[index] {
        Slot::Ready(session) => Ok(f(session)),
        Slot::Empty => Err(SlotError::Empty),
        Slot::Leased(_) => Err(SlotError::Busy),
    }
}

/// [`try_with_session`] for callers that don't care why it failed.
pub fn with_session<R, F>(index: usize, f: F) -> Option<R>
where
    F: FnOnce(&mut Session) -> R,
{
    try_with_session(index, f).ok()
}

/// Exclusive hold on a session for the length of one prompt. Dropping
/// the lease puts the session back in its slot and clears the stop flag.
pub struct SessionLease {
    index: usize,
    session: Option<Session>,
}

impl Deref for SessionLease {
    type Target = Session;

    fn deref(&self) -> &Session {
        // `session` is only None inside `drop`.
        self.session.as_ref().unwrap_or_else(|| unreachable!())
    }
}

impl DerefMut for SessionLease {
    fn deref_mut(&mut self) -> &mut Session {
        self.session.as_mut().unwrap_or_else(|| unreachable!())
    }
}

impl Drop for SessionLease {
    fn drop(&mut self) {
        if let Some(session) = self.session.take() {
            session.stop_flag.store(false, Ordering::Release);
            let _g = SpinGuard::new();
            // SAFETY: SpinGuard held.
            let slots = unsafe { &mut *core::ptr::addr_of_mut!(SESSIONS) };
            slots[self.index] = Slot::Ready(session);
        }
    }
}

/// Take the session at `index` out of the table for a prompt.
pub fn checkout(index: usize) -> Result<SessionLease, SlotError> {
    if index >= SLM_MAX_SESSIONS {
        return Err(SlotError::Empty);
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    let slots = unsafe { &mut *core::ptr::addr_of_mut!(SESSIONS) };
    match core::mem::replace(&mut slots[index], Slot::Empty) {
        Slot::Ready(session) => {
            let mut snapshot = session.snapshot();
            snapshot.busy = true;
            snapshot.state = SessionState::Decoding;
            slots[index] = Slot::Leased(Leased {
                stop: session.stop_flag.clone(),
                snapshot,
            });
            Ok(SessionLease { index, session: Some(session) })
        }
        Slot::Empty => Err(SlotError::Empty),
        leased @ Slot::Leased(_) => {
            slots[index] = leased;
            Err(SlotError::Busy)
        }
    }
}

/// Ask the prompt running on `index` to stop. Returns whether one was
/// running; stopping an idle session changes nothing, so a stale stop
/// can't cancel the next prompt.
pub fn request_stop(index: usize) -> Result<bool, SlotError> {
    if index >= SLM_MAX_SESSIONS {
        return Err(SlotError::Empty);
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held; the flag itself is atomic and the decoder
    // reads it without this lock.
    let slots = unsafe { &*core::ptr::addr_of!(SESSIONS) };
    match &slots[index] {
        Slot::Leased(leased) => {
            leased.stop.store(true, Ordering::Release);
            Ok(true)
        }
        Slot::Ready(_) => Ok(false),
        Slot::Empty => Err(SlotError::Empty),
    }
}

/// Copy out a session's state. Works while a prompt runs: the counters
/// are then the ones from the prompt's start and `busy` is set.
pub fn snapshot(index: usize) -> Option<SessionSnapshot> {
    if index >= SLM_MAX_SESSIONS {
        return None;
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    let slots = unsafe { &*core::ptr::addr_of!(SESSIONS) };
    match &slots[index] {
        Slot::Ready(session) => Some(session.snapshot()),
        Slot::Leased(leased) => Some(leased.snapshot),
        Slot::Empty => None,
    }
}

/// Whether a running prompt is using `model_handle`.
pub fn model_busy(model_handle: u32) -> bool {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    let slots = unsafe { &*core::ptr::addr_of!(SESSIONS) };
    slots.iter().any(|slot| {
        matches!(slot, Slot::Leased(leased) if leased.snapshot.model_handle == model_handle)
    })
}

/// Number of currently-open sessions, including any running a prompt.
pub fn count() -> usize {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    let slots = unsafe { &*core::ptr::addr_of!(SESSIONS) };
    slots.iter().filter(|s| !matches!(s, Slot::Empty)).count()
}

#[cfg(test)]
pub(crate) fn reset_for_tests() {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held; cfg(test) only.
    let slots = unsafe { &mut *core::ptr::addr_of_mut!(SESSIONS) };
    for s in slots.iter_mut() {
        *s = Slot::Empty;
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slm::registry::{
        build_qwen_test_fixture, ensure_mm_initialized_for_tests, load_slm,
        reset_for_tests as reset_registry, SLM_MAX_SLOTS,
    };
    use alloc::vec;

    /// Same TestSerial pattern as registry — `cargo test` runs in
    /// parallel, and both the SLM registry and session table are
    /// process-global statics that share state across tests in this
    /// file. Hold this guard for the full test body.
    static TEST_SERIAL: AtomicBool = AtomicBool::new(false);

    struct TestSerialGuard;

    impl TestSerialGuard {
        fn new() -> Self {
            while TEST_SERIAL
                .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
                .is_err()
            {
                core::hint::spin_loop();
            }
            TestSerialGuard
        }
    }

    impl Drop for TestSerialGuard {
        fn drop(&mut self) {
            TEST_SERIAL.store(false, Ordering::Release);
        }
    }

    fn load_test_model() -> u32 {
        ensure_mm_initialized_for_tests();
        let mut buf = vec![0u8; 8192];
        let n = build_qwen_test_fixture(64, &mut buf).expect("fixture fits");
        buf.truncate(n);
        let idx = load_slm(b"qwen-test", &buf).expect("load_slm");
        idx as u32
    }

    fn make_session() -> Session {
        let handle = load_test_model();
        Session::open(handle, 32, Sampler::Greedy, 0xCAFE).expect("open")
    }

    #[test]
    fn open_returns_session_for_loaded_model() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        let s = make_session();
        assert_eq!(s.state, SessionState::Open);
        assert_eq!(s.max_ctx, 32);
        assert_eq!(s.arch.block_count, 28);
        assert_eq!(s.arch.head_count_kv, 2);
        assert_eq!(s.arch.head_dim, 128);
        assert_eq!(s.vocab_size, 64);
        assert_eq!(s.kv.n_layers, 28);
        assert_eq!(s.kv.max_ctx, 32);
        assert_eq!(s.tokens_in, 0);
        assert_eq!(s.tokens_out, 0);
    }

    #[test]
    fn open_returns_none_for_invalid_handle() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        // No model loaded — handle 0 is empty.
        assert!(Session::open(0, 32, Sampler::Greedy, 1).is_none());
    }

    #[test]
    fn open_caps_max_ctx_at_trained_length() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        let handle = load_test_model();
        // Qwen fixture context_length = 32_768. Asking for more
        // should clamp to that value.
        let s = Session::open(handle, 100_000, Sampler::Greedy, 1).expect("open");
        assert_eq!(s.max_ctx, 32_768);
    }

    #[test]
    fn reset_clears_kv_and_counters() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        let mut s = make_session();
        // Synthesize some progress.
        s.prompts_completed = 3;
        s.tokens_in = 17;
        s.tokens_out = 42;
        s.last_ttft_ns = 1_000_000;
        s.state = SessionState::Stopped;
        s.pending_token = Some(7);
        s.request_stop();
        let zero_kv = vec![0u16; (s.arch.head_count_kv * s.arch.head_dim) as usize];
        s.kv.append(0, &zero_kv, &zero_kv).expect("append");
        s.kv.commit_position().expect("commit");
        assert_eq!(s.kv.len, 1);

        s.reset();
        assert_eq!(s.kv.len, 0);
        assert_eq!(s.prompts_completed, 0);
        assert_eq!(s.tokens_in, 0);
        assert_eq!(s.tokens_out, 0);
        assert_eq!(s.last_ttft_ns, 0);
        assert_eq!(s.state, SessionState::Open);
        assert_eq!(s.pending_token, None);
        assert!(!s.stop_requested());
    }

    #[test]
    fn close_marks_unusable() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        let mut s = make_session();
        s.close();
        assert_eq!(s.state, SessionState::Closed);
        // Reset on a closed session should not revive it.
        s.reset();
        assert_eq!(s.state, SessionState::Closed);
    }

    #[test]
    fn request_stop_sets_flag() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        let s = make_session();
        assert!(!s.stop_requested());
        s.request_stop();
        assert!(s.stop_requested());
    }

    /// The lock must be free while a prompt holds the session, or `slm
    /// stop` spins for the whole decode (the bug this table layout
    /// fixes). A second thread stands in for a second shell session.
    #[test]
    fn stop_reaches_a_checked_out_session_from_another_thread() {
        extern crate std;
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        let idx = insert(make_session()).expect("insert");

        let lease = checkout(idx).expect("checkout");
        assert!(!lease.stop_requested());

        let other = std::thread::spawn(move || {
            let was_running = request_stop(idx).expect("stop");
            let snap = snapshot(idx).expect("snapshot");
            (was_running, snap.busy, snap.state)
        });
        // Join with the lease still out: this deadlocks if anything in
        // the stop path needs the session back.
        let (was_running, busy, state) = other.join().expect("join");
        assert!(was_running);
        assert!(busy);
        assert_eq!(state, SessionState::Decoding);
        assert!(lease.stop_requested(), "decoder sees the flag without the lock");

        drop(lease);
        let snap = snapshot(idx).expect("snapshot");
        assert!(!snap.busy);
        assert_eq!(request_stop(idx), Ok(false), "nothing is running now");
        assert!(
            !with_session(idx, |s| s.stop_requested()).expect("slot"),
            "a stale stop must not cancel the next prompt",
        );
    }

    #[test]
    fn busy_session_refuses_structural_verbs() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        let idx = insert(make_session()).expect("insert");
        let model = snapshot(idx).expect("snapshot").model_handle;

        assert!(!model_busy(model));
        let lease = checkout(idx).expect("checkout");
        assert!(model_busy(model));
        assert_eq!(count(), 1, "a running session still counts");
        assert!(matches!(remove(idx), Err(SlotError::Busy)));
        assert!(matches!(checkout(idx), Err(SlotError::Busy)));
        assert_eq!(try_with_session(idx, |s| s.reset()), Err(SlotError::Busy));
        drop(lease);
        assert!(!model_busy(model));
        assert_eq!(try_with_session(idx, |s| s.reset()), Ok(()));
        assert!(remove(idx).is_ok());
        assert!(matches!(remove(idx), Err(SlotError::Empty)));
        assert!(matches!(checkout(idx), Err(SlotError::Empty)));
        assert_eq!(request_stop(idx), Err(SlotError::Empty));
        assert_eq!(request_stop(SLM_MAX_SESSIONS), Err(SlotError::Empty));
    }

    #[test]
    fn session_table_overflow_returns_error() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        let handle = load_test_model();
        for i in 0..SLM_MAX_SESSIONS {
            let s = Session::open(handle, 32, Sampler::Greedy, i as u64 + 1)
                .expect("open");
            let idx = insert(s).expect("insert");
            assert_eq!(idx, i);
        }
        // Slot table is full — next insert must fail.
        let s = Session::open(handle, 32, Sampler::Greedy, 99).expect("open");
        assert!(insert(s).is_none());

        // Sanity: SLM_MAX_SLOTS is the registry-side cap, distinct
        // from the session cap. Session table has its own ceiling.
        assert_eq!(count(), SLM_MAX_SESSIONS);
        let _ = SLM_MAX_SLOTS; // suppress unused
    }

    #[test]
    fn with_session_returns_none_for_empty_slot() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        // No sessions yet.
        assert!(with_session(0, |_| ()).is_none());
        // Out-of-range too.
        assert!(with_session(SLM_MAX_SESSIONS + 5, |_| ()).is_none());
    }

    #[test]
    fn remove_takes_slot_back() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_for_tests();
        let s = make_session();
        let idx = insert(s).expect("insert");
        let taken = remove(idx);
        assert!(taken.is_ok());
        assert!(remove(idx).is_err(), "slot already empty");
    }
}
