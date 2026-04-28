//! Per-prompt decoder loop for the SLM runtime.
//!
//! M5.2 of the SLM integration plan (see
//! `docs/plans/slm-integration-plan.md` §M5 and
//! `docs/specs/slm-integration.md`). Drives prefill + autoregressive
//! decode against a [`Session`], honouring the cooperative-cancel
//! flag and per-token callback.
//!
//! **M5.3.2:** the forward pass now walks the M4 transformer op chain
//! end-to-end via [`crate::slm::forward::forward_one`]. The decoder
//! still owns the prefill / sampling / callback state machine; the
//! per-token numeric work happens behind [`forward_step_via_registry`].
//! When a model lacks the expected per-layer weight tensors (e.g. the
//! M5.3.1 fixture only ships `output_norm.weight` + `token_embd.weight`)
//! [`forward_step_via_registry`] returns an empty logit vector and the
//! outer loop exits cleanly — this is the same contract M5.2 used and
//! is what the tests below rely on for the synthetic fixture path.
//!
//! `no_std` + `alloc` only.

#![allow(clippy::module_name_repetitions)]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

use crate::slm::{
    forward::{forward_one, ForwardScratch},
    registry,
    session::{Session, SessionState},
};

extern "C" {
    /// Cooperative scheduler yield. Linked from the C kernel as
    /// `yield()`; the decoder calls this between prefill chunks and
    /// after every emitted token so the rest of the system makes
    /// progress during long generations.
    #[link_name = "yield"]
    fn sched_yield();

    /// Wall-clock counter in nanoseconds (`slm_get_time_ns`). Used
    /// for the per-prompt TTFT and total-decode telemetry.
    fn slm_get_time_ns() -> u64;
}

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Token-callback signature.
///
/// The decoder calls this once per **decoded** token (prefill tokens
/// are silent — they only populate the KV cache). The callback
/// receives the token id and the UTF-8 bytes the tokenizer decoded
/// for it. Returning `false` stops generation immediately; the
/// session transitions to [`SessionState::Open`] (clean stop) and
/// the call returns the partial stats.
pub type TokenCallback = fn(token_id: u32, bytes: &[u8]) -> bool;

/// Decoder configuration.
pub struct DecodeConfig {
    /// Stop after this many generated tokens. `0` = unlimited (until
    /// EOS, the cooperative stop flag, or the cache fills).
    pub max_new_tokens: u32,
    /// Token id sampled to indicate end-of-sequence. The decoder
    /// emits the EOS token via the callback, then returns.
    pub eos_token_id: u32,
    /// Prefill chunk size. The prompt is processed in blocks of this
    /// many tokens with a cooperative yield between each block.
    /// Spec recommends 64.
    pub prefill_chunk: u32,
}

impl DecodeConfig {
    /// Sensible defaults: 256 tokens, EOS id 2 (Qwen ChatML
    /// `<|im_end|>`), 64-token prefill chunks. Callers should
    /// override per-model.
    pub fn default() -> Self {
        Self {
            max_new_tokens: 256,
            eos_token_id: 2,
            prefill_chunk: 64,
        }
    }
}

/// Per-prompt timing + token counts. Returned by
/// [`run_prompt`].
#[derive(Debug, Clone, Copy)]
pub struct DecodeStats {
    /// Number of tokens in the (tokenized) prompt — these populate
    /// the KV cache during prefill.
    pub prefill_tokens: u32,
    /// Number of tokens emitted via the callback (excludes prefill).
    pub decode_tokens: u32,
    /// Time-to-first-token: nanoseconds from `run_prompt` entry to
    /// the first sampled token.
    pub ttft_ns: u64,
    /// Total time spent in the decode loop (post-prefill), in
    /// nanoseconds.
    pub decode_ns: u64,
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Run prefill + decode for one prompt.
///
/// Sequence (high-level):
/// 1. State guard — only proceed if `session.state == Open`.
///    Transitions to [`SessionState::Decoding`].
/// 2. Tokenize the prompt via the registry-owned [`Bbpe`].
/// 3. Prefill in chunks of `cfg.prefill_chunk`, yielding between
///    chunks. Prefill does not sample; it only populates the KV
///    cache.
/// 4. Sample the first token from the last prefill step's logits
///    (TTFT measured here).
/// 5. Decode loop: forward, sample, decode bytes, emit via `cb`,
///    yield. Stops on EOS, callback returning `false`, the
///    cooperative stop flag, the per-prompt cap, or KV-cache full.
/// 6. Update session counters and return [`DecodeStats`].
///
/// **Stub note:** see the module-level doc — `forward_step` returns
/// zero logits in M5.2/M5.3.1, so the produced token ids will all be
/// `0`. M5.3.2 replaces the body with the real per-layer ops chain.
/// The contract — state transitions, telemetry, callback invocation
/// pattern, tokenizer wire-up — is what's exercised by tests and the
/// M7 shell.
pub fn run_prompt(
    session: &mut Session,
    prompt_text: &str,
    cfg: &DecodeConfig,
    cb: TokenCallback,
) -> Option<DecodeStats> {
    if session.state != SessionState::Open {
        return None;
    }
    session.state = SessionState::Decoding;
    session.stop_flag = false;

    let t_start = unsafe { slm_get_time_ns() };

    // 1) Tokenize the prompt and decode the EOS bytes via the
    //    registry-owned [`Bbpe`]. `with_loaded_slm` holds the
    //    registry lock only for the duration of the closure; we
    //    extract owned copies of everything the decode loop needs
    //    (token-id Vec, byte-decode helper closures aren't an
    //    option in `no_std` so we'll re-enter per token below).
    let prompt_ids: Vec<u32> = match registry::with_loaded_slm(
        session.model_handle as usize,
        |slm| match slm.tokenizer() {
            Some(tk) => Some(tk.encode(prompt_text)),
            None => None,
        },
    ) {
        Some(Some(ids)) => ids,
        // Empty / missing handle / no tokenizer — restore state and
        // bail. The session is still reusable.
        _ => {
            session.state = SessionState::Open;
            return None;
        }
    };
    let prefill_tokens = prompt_ids.len() as u32;

    // Allocate the forward-pass scratch once per prompt. M5.3.2 sizes
    // it from the session's cached arch + max_ctx; the buffers are
    // reused across every token in this prompt.
    let mut scratch = ForwardScratch::new(
        &session.arch,
        session.max_ctx,
        session.vocab_size as usize,
    );

    // 2) Prefill — push every prompt token through the forward pass,
    //    yielding between chunks. We deliberately walk one token at
    //    a time inside each chunk (rather than batched prefill);
    //    M5.3.2 can revisit if benchmarks demand it.
    let chunk = if cfg.prefill_chunk == 0 { 64 } else { cfg.prefill_chunk } as usize;
    // Pre-allocate the per-prompt logit buffer once at vocab capacity.
    // `forward_step_via_registry` uses `extend_from_slice` to refill
    // it, which is a memcpy (no realloc) when capacity already covers
    // vocab_size. The sampler then mutates this Vec in-place.
    let mut last_logits: Vec<f32> = Vec::with_capacity(session.vocab_size as usize);
    let mut consumed = 0usize;
    while consumed < prompt_ids.len() {
        // Stop flag honoured before each chunk so a slow prefill on
        // a long prompt can still be cancelled.
        if session.stop_flag {
            session.state = SessionState::Stopped;
            return Some(DecodeStats {
                prefill_tokens: consumed as u32,
                decode_tokens: 0,
                ttft_ns: 0,
                decode_ns: 0,
            });
        }
        let end = core::cmp::min(consumed + chunk, prompt_ids.len());
        for &tok in &prompt_ids[consumed..end] {
            forward_step_via_registry(session, tok, &mut scratch, &mut last_logits);
        }
        consumed = end;
        // Yield once per chunk, not once per token, so the runtime
        // doesn't drown in scheduler overhead for short prompts.
        unsafe { sched_yield(); }
    }

    // If the prompt was empty or KV-cache append failed somewhere,
    // we have no logits to sample from — bail out cleanly. Telemetry
    // still reflects the prefill work done so callers (M7's `slm
    // stats`) see the tokenized prompt size; matches the post-decode
    // path's update behaviour.
    if last_logits.is_empty() {
        session.state = SessionState::Open;
        session.prompts_completed = session.prompts_completed.saturating_add(1);
        session.tokens_in = session.tokens_in.saturating_add(prefill_tokens);
        return Some(DecodeStats {
            prefill_tokens,
            decode_tokens: 0,
            ttft_ns: 0,
            decode_ns: 0,
        });
    }

    // 3) Sample the first post-prompt token. TTFT spans tokenization
    //    + prefill + this sample call. The decode_ns clock starts
    //    after sampling the first token.
    let mut next_id = session.sampler_state.sample(&session.sampler_cfg, &mut last_logits);
    let t_first_token = unsafe { slm_get_time_ns() };
    let ttft_ns = t_first_token.saturating_sub(t_start);

    let mut decode_tokens: u32 = 0;
    let mut keep_going = emit_token(session, next_id, cb);
    decode_tokens += 1;

    // 4) Decode loop. The loop body re-enters even when keep_going
    //    is false so we can update counters before bailing — but we
    //    don't run another forward step in that case.
    loop {
        if !keep_going {
            // Callback asked for an early stop. Treat this as a
            // clean termination — the session is reusable.
            session.state = SessionState::Open;
            break;
        }
        if session.stop_flag {
            session.state = SessionState::Stopped;
            break;
        }
        if next_id == cfg.eos_token_id {
            session.state = SessionState::Open;
            break;
        }
        if cfg.max_new_tokens != 0 && decode_tokens >= cfg.max_new_tokens {
            session.state = SessionState::Open;
            break;
        }
        if session.kv.len >= session.max_ctx {
            // Cache full — graceful stop. The decoder ran the prompt
            // through but ran out of context budget.
            session.state = SessionState::Open;
            break;
        }

        // Forward + sample for the next token. Reuses last_logits's
        // backing allocation (sized once at vocab capacity above).
        forward_step_via_registry(session, next_id, &mut scratch, &mut last_logits);
        if last_logits.is_empty() {
            session.state = SessionState::Open;
            break;
        }
        next_id = session.sampler_state.sample(&session.sampler_cfg, &mut last_logits);
        decode_tokens += 1;
        keep_going = emit_token(session, next_id, cb);

        // Yield between every emitted token. The cooperative cancel
        // flag check at the top of the next iteration is what makes
        // `request_stop` responsive.
        unsafe { sched_yield(); }
    }

    let t_end = unsafe { slm_get_time_ns() };
    let decode_ns = t_end.saturating_sub(t_first_token);

    // 5) Update session telemetry.
    session.prompts_completed = session.prompts_completed.saturating_add(1);
    session.tokens_in = session.tokens_in.saturating_add(prefill_tokens);
    session.tokens_out = session.tokens_out.saturating_add(decode_tokens);
    session.last_ttft_ns = ttft_ns;
    session.last_decode_ns = decode_ns;

    Some(DecodeStats {
        prefill_tokens,
        decode_tokens,
        ttft_ns,
        decode_ns,
    })
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

/// Emit one token via the callback, decoding to UTF-8 bytes first
/// using the registry-owned tokenizer.
///
/// Returns `false` when the callback asks for an early stop.
fn emit_token(session: &Session, token_id: u32, cb: TokenCallback) -> bool {
    let bytes: String =
        registry::with_loaded_slm(session.model_handle as usize, |slm| match slm.tokenizer() {
            Some(tk) => tk.decode(&[token_id]),
            None => String::new(),
        })
        .unwrap_or_default();
    cb(token_id, bytes.as_bytes())
}

/// Look up the loaded model's `LoadedSlm` under the registry lock and
/// drive the M5.3.2 forward pass over its weight tensors. The lock is
/// held for the duration of the per-token op chain — typical token
/// latency is a few hundred microseconds on Pi 5 (per the M9 budget),
/// so the window is fine for the FFI shim's "one prompt at a time"
/// shape; M5.3.3 / M9 will revisit if concurrent prompts become a
/// requirement.
///
/// Returns an empty `Vec` (which the caller treats as "no logits, stop
/// gracefully") on:
/// - missing or invalid model handle,
/// - missing per-layer weight tensors (e.g. the M5.3.1 fixture, which
///   only ships `output_norm.weight` + `token_embd.weight` — the loop
///   exits cleanly without a panic),
/// - KV-cache full,
/// - shape / arch mismatch detected inside [`forward_one`].
fn forward_step_via_registry(
    session: &mut Session,
    token_id: u32,
    scratch: &mut ForwardScratch,
    out: &mut Vec<f32>,
) {
    out.clear();
    let vocab = session.vocab_size as usize;
    let succeeded = registry::with_loaded_slm(session.model_handle as usize, |slm| {
        forward_one(session, slm, token_id, scratch).is_some()
    })
    .unwrap_or(false);
    if !succeeded {
        return;
    }
    // Copy from scratch.logits into the caller's persistent Vec. This
    // is one memcpy per token and avoids the alloc churn that a
    // per-token `Vec<f32>` return would force.
    if scratch.logits.len() < vocab {
        return;
    }
    out.extend_from_slice(&scratch.logits[..vocab]);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slm::registry::{
        build_qwen_test_fixture, ensure_mm_initialized_for_tests, load_slm,
        reset_for_tests as reset_registry,
    };
    use crate::slm::sampler::Sampler;
    use crate::slm::session::reset_for_tests as reset_sessions;
    use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
    use alloc::vec;

    /// Cross-test serialization: the session table, the registry,
    /// and the test counters below are all process-global.
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

    // The decoder calls FFI shims (`yield`, `slm_get_time_ns`) that
    // the kernel provides at link time. For host-side `cargo test`
    // we stub them here so tests don't need the kernel.
    #[no_mangle]
    extern "C" fn r#yield() {}

    #[no_mangle]
    extern "C" fn slm_get_time_ns() -> u64 {
        0
    }

    static CALL_COUNT: AtomicU32 = AtomicU32::new(0);
    static STOP_AFTER: AtomicU32 = AtomicU32::new(u32::MAX);

    fn count_callback(_token_id: u32, _bytes: &[u8]) -> bool {
        let prior = CALL_COUNT.fetch_add(1, Ordering::SeqCst);
        prior + 1 < STOP_AFTER.load(Ordering::SeqCst)
    }

    fn always_callback(_token_id: u32, _bytes: &[u8]) -> bool {
        CALL_COUNT.fetch_add(1, Ordering::SeqCst);
        true
    }

    /// Open a fresh session against the M5.3.1 Qwen fixture. The
    /// fixture's tokenizer (built inside `load_slm` from the embedded
    /// `tokenizer.ggml.tokens` / `merges` arrays) is reused via the
    /// registry — there is no longer a separate `Bbpe` parameter.
    ///
    /// Vocab size = 256 so the fixture's byte-fallback prefix (id
    /// `i` = byte `i` for `i < 128`) covers the ASCII range — the
    /// test prompts like `"hi"` encode to two byte-fallback tokens.
    fn fresh_session() -> Session {
        ensure_mm_initialized_for_tests();
        let mut buf = vec![0u8; 16384];
        let n = build_qwen_test_fixture(256, &mut buf).expect("fixture");
        buf.truncate(n);
        let handle = load_slm(b"qwen-test", &buf).expect("load_slm") as u32;
        Session::open(handle, 16, Sampler::Greedy, 0xDEAD_BEEF).expect("open")
    }

    /// **M5.3.2 contract update.** The synthetic fixture from M5.3.1
    /// only ships `output_norm.weight` + `token_embd.weight`; it
    /// lacks the per-layer Q/K/V/O/MLP weights `forward_one` requires.
    /// `forward_step_via_registry` therefore returns an empty
    /// `Vec<f32>` per token, prefill produces no logits, and the
    /// outer loop exits cleanly via the
    /// `if last_logits.is_empty()` early-return branch. The session
    /// must end in `Open` (reusable) with `decode_tokens == 0`. The
    /// real numeric path is exercised by `forward.rs`'s unit tests
    /// (option (c) hand-crafted pipeline) and by M5.3.3's hardware
    /// demo against a complete Qwen2 GGUF.
    #[test]
    fn run_prompt_with_incomplete_fixture_returns_open_with_zero_decode() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_sessions();
        CALL_COUNT.store(0, Ordering::SeqCst);
        STOP_AFTER.store(u32::MAX, Ordering::SeqCst);

        let mut s = fresh_session();
        let cfg = DecodeConfig {
            max_new_tokens: 3,
            eos_token_id: 9999,
            prefill_chunk: 4,
        };
        let stats = run_prompt(&mut s, "hi", &cfg, always_callback).expect("decoded");
        assert_eq!(s.state, SessionState::Open);
        assert_eq!(stats.decode_tokens, 0);
        assert_eq!(s.tokens_out, 0);
        // Prompt was still tokenized — prefill_tokens reflects what
        // the tokenizer produced even though no logits emerged.
        assert!(stats.prefill_tokens > 0);
    }

    /// **M5.3.2 contract update.** Same shape as the previous test —
    /// the callback never gets invoked because the empty-logits early
    /// return fires first. `count_callback` and the `STOP_AFTER`
    /// machinery still need to run without panicking; the post-
    /// condition is `decode_tokens == 0` and `state == Open`. Once
    /// M5.3.3 builds a complete fixture, a follow-up test will
    /// exercise the callback-stop path against real logits.
    #[test]
    fn run_prompt_respects_callback_stop_path() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_sessions();
        CALL_COUNT.store(0, Ordering::SeqCst);
        STOP_AFTER.store(2, Ordering::SeqCst);

        let mut s = fresh_session();
        let cfg = DecodeConfig {
            max_new_tokens: 100,
            eos_token_id: 9999,
            prefill_chunk: 4,
        };
        let stats = run_prompt(&mut s, "hi", &cfg, count_callback).expect("decoded");
        assert_eq!(stats.decode_tokens, 0);
        assert_eq!(s.state, SessionState::Open);
    }

    /// **M5.3.2 contract update.** Stop-flag handling is exercised by
    /// the structural early-return path; with the M5.3.1 fixture there
    /// are no logits to sample so the post-prefill branch returns
    /// before the decode loop's stop-flag check. Either way the
    /// session must end in a reusable state — `Open` for the
    /// no-logits early return, `Stopped` if the flag was set during
    /// prefill chunks. This test asserts the cleaner of the two.
    #[test]
    fn run_prompt_handles_no_logits_path_gracefully() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_sessions();
        CALL_COUNT.store(0, Ordering::SeqCst);
        STOP_AFTER.store(u32::MAX, Ordering::SeqCst);

        let mut s = fresh_session();
        let cfg = DecodeConfig {
            max_new_tokens: 50,
            eos_token_id: 9999,
            prefill_chunk: 4,
        };

        fn noop(_tok: u32, _: &[u8]) -> bool {
            CALL_COUNT.fetch_add(1, Ordering::SeqCst);
            true
        }
        let stats = run_prompt(&mut s, "hi", &cfg, noop).expect("decoded");
        assert_eq!(stats.decode_tokens, 0);
        assert_eq!(s.state, SessionState::Open);
    }

    #[test]
    fn run_prompt_returns_none_when_session_not_open() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_sessions();
        let mut s = fresh_session();
        s.state = SessionState::Stopped;
        let cfg = DecodeConfig::default();
        assert!(run_prompt(&mut s, "hi", &cfg, always_callback).is_none());

        s.state = SessionState::Closed;
        assert!(run_prompt(&mut s, "hi", &cfg, always_callback).is_none());
    }

    /// **M5.3.2 contract update.** The "one callback per emitted
    /// token" invariant is asserted via `forward.rs`'s pipeline-
    /// composition test. With the M5.3.1 fixture the empty-logits
    /// path skips the decode loop entirely, so we can't observe
    /// emit-frequency here; the test below pins the corresponding
    /// counter contract (CALL_COUNT stays 0 when no tokens are
    /// emitted).
    #[test]
    fn run_prompt_does_not_invoke_callback_when_logits_empty() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_sessions();
        CALL_COUNT.store(0, Ordering::SeqCst);
        STOP_AFTER.store(u32::MAX, Ordering::SeqCst);

        let mut s = fresh_session();
        let cfg = DecodeConfig {
            max_new_tokens: 5,
            eos_token_id: 9999,
            prefill_chunk: 4,
        };
        let stats = run_prompt(&mut s, "hi", &cfg, always_callback).expect("decoded");
        assert_eq!(stats.decode_tokens, 0);
        assert_eq!(CALL_COUNT.load(Ordering::SeqCst), 0);
    }

    /// M5.3.1: a non-empty prompt drives the registry-owned tokenizer
    /// and increments tokens_in. Confirms the `Bbpe` plumbing is
    /// live; the actual content of `tokens_in` depends on how the
    /// fixture's tokens encode "hi", which the test doesn't pin down
    /// — only that something nonzero went through prefill.
    #[test]
    fn run_prompt_uses_registered_tokenizer() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_sessions();
        CALL_COUNT.store(0, Ordering::SeqCst);
        STOP_AFTER.store(u32::MAX, Ordering::SeqCst);

        let mut s = fresh_session();
        let cfg = DecodeConfig {
            max_new_tokens: 1,
            eos_token_id: 9999,
            prefill_chunk: 4,
        };
        let stats = run_prompt(&mut s, "hi", &cfg, always_callback).expect("decoded");
        assert!(stats.prefill_tokens > 0, "prompt should tokenize to >=1 token");
        assert_eq!(s.tokens_in, stats.prefill_tokens);
    }
}
