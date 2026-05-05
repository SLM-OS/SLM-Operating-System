//! Per-session state for the SLM decoder.
//!
//! M5.2 of the SLM integration plan (see
//! `docs/plans/slm-integration-plan.md` §M5 and
//! `docs/specs/slm-integration.md`). A `Session` ties together a loaded
//! GGUF model (looked up in [`crate::slm::registry`]), an FP16 KV cache
//! ([`crate::slm::kv_cache::KvCache`]), and a sampler
//! ([`crate::slm::sampler`]). The session is the unit of conversation
//! state — `prompt()` calls drive the decoder loop, `reset()` clears the
//! KV cache for a fresh conversation, `request_stop()` cooperatively
//! cancels an in-flight prompt, and `close()` releases the slot.
//!
//! A 4-slot fixed table mirrors the registry's pattern from M1.4: bounded
//! to keep the `no_std` runtime predictable, and protected by a
//! SpinGuard for cross-FFI access from the M7 shell.
//!
//! `no_std` + `alloc` only.

#![allow(clippy::module_name_repetitions)]

use core::sync::atomic::{AtomicBool, Ordering};

use crate::slm::{
    gguf::ArchInfo,
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
    /// `stop_flag` was honoured mid-decode. Next prompt requires
    /// [`Session::reset`] first.
    Stopped,
    /// Slot released; subsequent operations return errors.
    Closed,
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
    /// Cooperative-cancel flag. The decoder loop checks this at each
    /// `yield` boundary; M7's shell flips it via `rust_slm_stop`.
    pub stop_flag: bool,
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
            stop_flag: false,
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
        self.stop_flag = false;
        self.prompts_completed = 0;
        self.tokens_in = 0;
        self.tokens_out = 0;
        self.last_ttft_ns = 0;
        self.last_decode_ns = 0;
        if self.state != SessionState::Closed {
            self.state = SessionState::Open;
        }
    }

    /// Request cooperative cancellation. The decoder loop notices at
    /// the next yield boundary and transitions to
    /// [`SessionState::Stopped`].
    pub fn request_stop(&mut self) {
        self.stop_flag = true;
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
// The decoder operates on `&mut Session`, and the FFI layer takes a
// `session_id` (= slot index). To avoid juggling raw pointers from C,
// the FFI shim acquires the spin lock, resolves the slot, and calls
// the helper functions in this module. The table is private; only
// the public functions below cross the module boundary.
// ---------------------------------------------------------------------------

const EMPTY_SESSION: Option<Session> = None;

static SESSION_LOCK: AtomicBool = AtomicBool::new(false);
static mut SESSIONS: [Option<Session>; SLM_MAX_SESSIONS] =
    [EMPTY_SESSION; SLM_MAX_SESSIONS];

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
        if slot.is_none() {
            *slot = Some(session);
            return Some(i);
        }
    }
    None
}

/// Free the slot at `index`. Returns `None` if the slot was already
/// empty or `index` is out of range; otherwise returns the freed
/// session (so the caller can run any extra teardown).
pub fn remove(index: usize) -> Option<Session> {
    if index >= SLM_MAX_SESSIONS {
        return None;
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    let slots = unsafe { &mut *core::ptr::addr_of_mut!(SESSIONS) };
    slots[index].take()
}

/// Run a closure against the session at `index` while the table lock
/// is held.
///
/// The callback gets `&mut Session`; returning a value out of the
/// closure is the canonical way to copy state out without leaking
/// references past the lock release.
///
/// Returns `None` if the slot is empty or out of range.
pub fn with_session<R, F>(index: usize, f: F) -> Option<R>
where
    F: FnOnce(&mut Session) -> R,
{
    if index >= SLM_MAX_SESSIONS {
        return None;
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to SESSIONS for the
    // duration of `f`. The closure must not re-enter the session
    // table (would deadlock); callers are decoder.rs / lib.rs FFI
    // shims and stay self-contained.
    let slots = unsafe { &mut *core::ptr::addr_of_mut!(SESSIONS) };
    let slot = slots[index].as_mut()?;
    Some(f(slot))
}

/// Number of currently-open sessions.
pub fn count() -> usize {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    let slots = unsafe { &*core::ptr::addr_of!(SESSIONS) };
    slots.iter().filter(|s| s.is_some()).count()
}

#[cfg(test)]
pub(crate) fn reset_for_tests() {
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held; cfg(test) only.
    let slots = unsafe { &mut *core::ptr::addr_of_mut!(SESSIONS) };
    for s in slots.iter_mut() {
        *s = None;
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
        let mut s = make_session();
        assert!(!s.stop_flag);
        s.request_stop();
        assert!(s.stop_flag);
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
        assert!(taken.is_some());
        assert!(remove(idx).is_none(), "slot already empty");
    }
}
