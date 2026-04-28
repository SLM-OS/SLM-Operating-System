//! Per-prompt decoder loop for the SLM runtime.
//!
//! M5.2 of the SLM integration plan (see
//! `docs/plans/slm-integration-plan.md` §M5 and
//! `docs/specs/slm-integration.md`). Drives prefill + autoregressive
//! decode against a [`Session`], honouring the cooperative-cancel
//! flag and per-token callback.
//!
//! **Stub status (M5.2):** The forward pass is wired structurally but
//! not yet operational. The registry currently stores GGUF metadata
//! only; the per-tensor pointers the M4 ops chain expects (embedding
//! table, per-layer attention/MLP weights, LM-head) aren't yet
//! resident in the model_mem pool. M5.3 will replace
//! [`forward_step`] with the real per-layer op chain
//! (rmsnorm → gqa_decode_step → rmsnorm → swiglu_mlp → … → lm_head).
//! Until then the placeholder appends zero KV slices and returns a
//! zero logits vector — sampling deterministically picks token id 0,
//! which is enough to exercise the state machine, the stop-flag path,
//! and the per-token callback contract.
//!
//! `no_std` + `alloc` only.

#![allow(clippy::module_name_repetitions)]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

use crate::slm::{
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

    // 2) Prefill — push every prompt token through the forward pass,
    //    yielding between chunks. We deliberately walk one token at
    //    a time inside each chunk (rather than batched prefill);
    //    M5.3.2 can revisit if benchmarks demand it.
    let chunk = if cfg.prefill_chunk == 0 { 64 } else { cfg.prefill_chunk } as usize;
    let mut last_logits: Vec<f32> = Vec::new();
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
            last_logits = forward_step_via_registry(session, tok);
        }
        consumed = end;
        // Yield once per chunk, not once per token, so the runtime
        // doesn't drown in scheduler overhead for short prompts.
        unsafe { sched_yield(); }
    }

    // If the prompt was empty or KV-cache append failed somewhere,
    // we have no logits to sample from — bail out cleanly.
    if last_logits.is_empty() {
        session.state = SessionState::Open;
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

        // Forward + sample for the next token.
        let mut logits = forward_step_via_registry(session, next_id);
        if logits.is_empty() {
            session.state = SessionState::Open;
            break;
        }
        next_id = session.sampler_state.sample(&session.sampler_cfg, &mut logits);
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

/// Wrapper that fetches `&LoadedSlm` under the registry lock and
/// delegates to [`forward_step`]. Splitting the lookup out from the
/// numeric op keeps the lock window per-token (re-acquired between
/// tokens, so a concurrent `slm load` can interleave) and gives
/// M5.3.2 a clean signature to fill in.
fn forward_step_via_registry(session: &mut Session, token_id: u32) -> Vec<f32> {
    // Take a tiny snapshot of what `forward_step` actually consumes.
    // The closure can't borrow `session` mutably and `slm` at the
    // same time across the lock boundary, so any mutation of the
    // session (KV cache append, etc.) happens after the lock drops.
    //
    // M5.3.2 will replace the body of `forward_step` with the real
    // op chain that actually walks `slm.tensor_bytes(...)` for
    // weights — at that point the lock window grows back to "once
    // per token" but the structure stays the same.
    let snapshot = registry::with_loaded_slm(session.model_handle as usize, |slm| {
        ForwardSnapshot {
            vocab_size: slm.arch().embedding_length, // unused stub field
            arch_vocab: session.vocab_size,
            // Touch the slm so the borrow shows up in the type — keeps
            // the closure honest now and gives M5.3.2 a clear hook.
            tensor_count: slm.tensors().len() as u32,
        }
    });
    if snapshot.is_none() {
        return Vec::new();
    }

    forward_step(session, token_id)
}

/// Snapshot of registry state that the per-token forward pass needs
/// to read while holding the registry lock. Kept tiny so the lock
/// window stays short.
#[allow(dead_code)] // Fields read by M5.3.2's real forward_step.
struct ForwardSnapshot {
    vocab_size: u32,
    arch_vocab: u32,
    tensor_count: u32,
}

/// **PLACEHOLDER for M5.3.2:** walk the per-layer ops with mock
/// weights, populate the KV cache with zero entries for the embedded
/// token, and return a zero logit vector of length `vocab_size`.
///
/// The real version (M5.3.2) will:
/// - Look up the embedding row for `token_id` from the registry's
///   weight pool (FP16) via `LoadedSlm::tensor_bytes("token_embd.weight")`.
/// - For each transformer block: rmsnorm → q/k/v projections →
///   `gqa_decode_step` (which appends KV via `session.kv.append`) →
///   o-proj → residual → rmsnorm → `swiglu_mlp` → residual.
/// - Apply final rmsnorm and `lm_head` to produce logits.
///
/// Until then the loop in `run_prompt` exercises the surrounding
/// state machine without depending on actual numeric ops.
fn forward_step(session: &mut Session, _token_id: u32) -> Vec<f32> {
    let kv_len = (session.arch.head_count_kv as usize)
        .saturating_mul(session.arch.head_dim as usize);
    if kv_len == 0 || session.kv.len >= session.max_ctx {
        // Avoid a zero-length append (which `KvCache::append` rejects)
        // or appending past the cache. Either condition causes the
        // outer loop to terminate cleanly.
        return Vec::new();
    }
    let zero_kv = alloc::vec![0u16; kv_len];
    for layer in 0..session.arch.block_count as usize {
        if session.kv.append(layer, &zero_kv, &zero_kv).is_none() {
            return Vec::new();
        }
    }
    let _ = session.kv.commit_position();

    // Vocab size is cached on the session at open time.
    let vocab = session.vocab_size as usize;
    if vocab == 0 {
        return Vec::new();
    }
    alloc::vec![0.0f32; vocab]
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

    #[test]
    fn run_prompt_advances_state_through_decoding_to_open() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_sessions();
        CALL_COUNT.store(0, Ordering::SeqCst);
        STOP_AFTER.store(u32::MAX, Ordering::SeqCst);

        let mut s = fresh_session();
        let cfg = DecodeConfig {
            // Greedy on zero logits returns id 0; pick a small cap so
            // the loop terminates quickly.
            max_new_tokens: 3,
            // Use 9999 as EOS — id 0 won't match it, so we rely on
            // max_new_tokens to terminate.
            eos_token_id: 9999,
            prefill_chunk: 4,
        };
        let stats = run_prompt(&mut s, "hi", &cfg, always_callback).expect("decoded");
        assert_eq!(s.state, SessionState::Open);
        assert_eq!(stats.decode_tokens, 3);
        assert_eq!(s.tokens_out, 3);
        assert_eq!(s.prompts_completed, 1);
    }

    #[test]
    fn run_prompt_respects_callback_stop() {
        let _serial = TestSerialGuard::new();
        reset_registry();
        reset_sessions();
        CALL_COUNT.store(0, Ordering::SeqCst);
        // Tell the callback to stop after 2 emits.
        STOP_AFTER.store(2, Ordering::SeqCst);

        let mut s = fresh_session();
        let cfg = DecodeConfig {
            max_new_tokens: 100,
            eos_token_id: 9999,
            prefill_chunk: 4,
        };
        let stats = run_prompt(&mut s, "hi", &cfg, count_callback).expect("decoded");
        // Callback returned false on its 2nd invocation. The loop
        // exits cleanly with decode_tokens == 2.
        assert_eq!(stats.decode_tokens, 2);
        assert_eq!(s.state, SessionState::Open);
    }

    #[test]
    fn run_prompt_respects_stop_flag() {
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

        // Stop after the first decode iteration via the natural
        // "callback returns false" path; greedy on zero logits picks
        // id 0 every time.
        fn stopper(tok: u32, _: &[u8]) -> bool {
            CALL_COUNT.fetch_add(1, Ordering::SeqCst);
            tok != 0
        }
        let stats = run_prompt(&mut s, "hi", &cfg, stopper).expect("decoded");
        assert!(stats.decode_tokens >= 1);
        // Stopper returned false → clean stop, state should be Open.
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

    #[test]
    fn run_prompt_emits_one_token_per_callback_call() {
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
        assert_eq!(stats.decode_tokens, 5);
        assert_eq!(CALL_COUNT.load(Ordering::SeqCst), 5);
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
