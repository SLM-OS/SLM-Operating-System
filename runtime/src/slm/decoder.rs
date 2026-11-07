//! Per-prompt decoder loop for the SLM runtime.
//!
//! M5.2 of the SLM integration plan (see
//! `docs/plans/slm-integration-plan.md` §M5 and
//! `docs/design/slm-integration.md`). Drives prefill + autoregressive
//! decode against a [`Session`], honouring the cooperative-cancel
//! flag and per-token callback. Renders the ChatML template unless the
//! caller asks for [`PromptMode::Raw`], stops on the model's own
//! end-of-generation tokens, and only ever hands the callback complete
//! UTF-8.
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

use alloc::vec::Vec;
use core::sync::atomic::Ordering;

use crate::inference::gpu_slm::{select_tier, OpKind, Tier};
use crate::slm::{
    chat_template::{
        encode_chat, encode_chat_continuation, ChatTurn, Role, GENERIC_SYSTEM_PROMPT,
        QWEN_SYSTEM_PROMPT,
    },
    forward::{forward_one, ForwardScratch},
    gguf::ArchKind,
    registry,
    session::{ForwardPath, Session, SessionState, StopReason},
    tokenizer::Bbpe,
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

/// Token id passed to the callback for the final bytes of a prompt when
/// the output ended part-way through a UTF-8 sequence. It isn't a model
/// token; don't count it. Mirrors `SLM_TOKEN_ID_FLUSH`.
pub const TOKEN_ID_FLUSH: u32 = u32::MAX;

/// Token-callback signature.
///
/// The decoder calls this once per **decoded** token (prefill tokens
/// are silent — they only populate the KV cache) except the
/// end-of-generation token, which ends the prompt without being
/// emitted. `bytes` is always complete UTF-8: a token that ends inside
/// a multi-byte sequence hands over only what's complete and the next
/// call carries the rest, so `bytes` can be empty. Returning `false`
/// stops generation immediately; the session transitions to
/// [`SessionState::Open`] (clean stop) and the call returns the partial
/// stats.
pub type TokenCallback = fn(token_id: u32, bytes: &[u8]) -> bool;

/// How to turn the prompt text into tokens.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PromptMode {
    /// Treat the text as a user turn and render the ChatML template
    /// around it. Special-token names in the text stay plain text.
    Chat,
    /// Tokenize the text as given, mapping special-token names to their
    /// ids. For callers that rendered their own prompt.
    Raw,
}

/// Token ids that end generation.
#[derive(Debug, Clone, Copy)]
pub struct StopTokens {
    ids: [Option<u32>; 3],
}

impl StopTokens {
    /// One explicit stop id.
    pub fn one(id: u32) -> Self {
        Self { ids: [Some(id), None, None] }
    }

    fn from_tokenizer(tk: &Bbpe) -> Self {
        Self { ids: tk.stop_ids() }
    }

    fn contains(&self, id: u32) -> bool {
        self.ids.contains(&Some(id))
    }
}

/// Decoder configuration.
pub struct DecodeConfig {
    /// Stop after this many generated tokens. `0` = unlimited (until
    /// EOS, the cooperative stop flag, or the cache fills).
    pub max_new_tokens: u32,
    /// Stop ids. `None` reads them from the model: its
    /// `tokenizer.ggml.eos_token_id`, `<|im_end|>` and `<|endoftext|>`.
    pub stop_tokens: Option<StopTokens>,
    /// Yield to the scheduler every this many prefill tokens.
    pub prefill_chunk: u32,
    /// See [`PromptMode`].
    pub mode: PromptMode,
    /// Checked before every forward step alongside the stop flag. For
    /// signals the caller has to poll for, such as Ctrl-C.
    pub poll_stop: Option<fn() -> bool>,
}

impl DecodeConfig {
    /// 256 tokens, the model's stop ids, a chat prompt.
    pub fn default() -> Self {
        Self {
            max_new_tokens: 256,
            stop_tokens: None,
            prefill_chunk: 1,
            mode: PromptMode::Chat,
            poll_stop: None,
        }
    }
}

/// Per-prompt timing + token counts. Returned by
/// [`run_prompt`].
#[derive(Debug, Clone, Copy)]
pub struct DecodeStats {
    /// Number of tokens fed to the model for this prompt — these
    /// populate the KV cache during prefill.
    pub prefill_tokens: u32,
    /// Number of tokens emitted via the callback (excludes prefill and
    /// the end-of-generation token).
    pub decode_tokens: u32,
    /// Time-to-first-token: nanoseconds from `run_prompt` entry to
    /// the first sampled token.
    pub ttft_ns: u64,
    /// Total time spent in the decode loop (post-prefill), in
    /// nanoseconds.
    pub decode_ns: u64,
    /// Why generation ended.
    pub stop_reason: StopReason,
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Run prefill + decode for one prompt.
///
/// Sequence (high-level):
/// 1. State guard — proceeds from [`SessionState::Open`] or
///    [`SessionState::Stopped`]; transitions to
///    [`SessionState::Decoding`].
/// 2. Render + tokenize the prompt via the registry-owned [`Bbpe`]. A
///    prompt that doesn't fit in the KV cache stops here with
///    [`StopReason::ContextFull`].
/// 3. Prefill one token at a time, yielding every `cfg.prefill_chunk`.
///    Prefill does not sample; it only populates the KV cache.
/// 4. Sample the first token from the last prefill step's logits
///    (TTFT measured here).
/// 5. Decode loop: emit, forward, sample, yield. Stops on a stop token,
///    callback returning `false`, the cooperative stop flag, the
///    per-prompt cap, or KV-cache full.
/// 6. Update session counters and return [`DecodeStats`].
///
/// A stop only lands between forward steps, so the KV cache only ever
/// holds whole positions and the session can take another prompt.
///
/// Returns `None`, leaving the session as it was, if it isn't in a
/// state that accepts prompts or the model has gone away.
pub fn run_prompt(
    session: &mut Session,
    prompt_text: &str,
    cfg: &DecodeConfig,
    cb: TokenCallback,
) -> Option<DecodeStats> {
    if session.state != SessionState::Open && session.state != SessionState::Stopped {
        return None;
    }
    let prior_state = session.state;

    if prompt_text.is_empty() {
        session.state = SessionState::Open;
        session.last_stop_reason = StopReason::EmptyPrompt;
        session.prompts_completed = session.prompts_completed.saturating_add(1);
        return Some(DecodeStats {
            prefill_tokens: 0,
            decode_tokens: 0,
            ttft_ns: 0,
            decode_ns: 0,
            stop_reason: StopReason::EmptyPrompt,
        });
    }

    let t_start = unsafe { slm_get_time_ns() };

    // 1) Tokenize. `with_loaded_slm` holds the registry lock only for
    //    the closure, so everything the loop needs is copied out.
    let tokenized = registry::with_loaded_slm(session.model_handle as usize, |slm| {
        let tk = slm.tokenizer()?;
        Some((
            build_prompt_ids(session, prompt_text, cfg.mode, tk),
            cfg.stop_tokens.unwrap_or_else(|| StopTokens::from_tokenizer(tk)),
        ))
    });
    let (prompt_ids, stop_tokens) = match tokenized {
        Some(Some(t)) => t,
        // Missing handle / no tokenizer — the session is still reusable.
        _ => {
            session.state = prior_state;
            return None;
        }
    };

    session.state = SessionState::Decoding;
    let prefill_tokens = prompt_ids.len() as u32;

    if session.kv.len.saturating_add(prompt_ids.len()) > session.max_ctx {
        return Some(finish(session, 0, 0, 0, 0, StopReason::ContextFull));
    }
    // A chat prompt just fed any held-back reply token; a raw one is as
    // given, so it drops it.
    session.pending_token = None;

    // Allocate the forward-pass scratch once per prompt. It's sized from
    // the session's cached arch + max_ctx and reused for every token.
    let mut scratch = ForwardScratch::new(
        &session.arch,
        session.max_ctx,
        session.vocab_size as usize,
    );

    // 2) Prefill. Pre-allocate the logit buffer once at vocab capacity:
    //    refilling it is a memcpy, and the sampler mutates it in place.
    let chunk = if cfg.prefill_chunk == 0 { 1 } else { cfg.prefill_chunk } as usize;
    let mut last_logits: Vec<f32> = Vec::with_capacity(session.vocab_size as usize);
    let mut i = 0usize;
    while i < prompt_ids.len() {
        if stop_wanted(session, cfg) {
            return Some(finish(session, i as u32, 0, 0, 0, StopReason::Stopped));
        }
        // A GPU program takes the prompt in batches. Anything else, or a
        // program that just failed, goes one token at a time.
        let batch = session
            .gpu
            .as_ref()
            .filter(|_| !script::armed())
            .map_or(1, |p| p.max_batch as usize)
            .min(prompt_ids.len() - i);
        let tokens = &prompt_ids[i..i + batch];
        if batch > 1 {
            step_many(session, tokens, &mut scratch, &mut last_logits);
        } else {
            step(session, tokens[0], true, &mut scratch, &mut last_logits);
        }
        i += batch;
        if i % chunk == 0 || batch > 1 {
            unsafe { sched_yield(); }
        }
    }

    // No logits: the forward pass couldn't run on this model. Telemetry
    // still reflects the tokenized prompt size.
    if last_logits.is_empty() {
        return Some(finish(session, prefill_tokens, 0, 0, 0, StopReason::NoLogits));
    }

    // 3) Sample the first post-prompt token. TTFT spans tokenization +
    //    prefill + this sample call. The decode_ns clock starts after it.
    let mut next_id = session.sampler_state.sample(&session.sampler_cfg, &mut last_logits);
    let t_first_token = unsafe { slm_get_time_ns() };
    let ttft_ns = t_first_token.saturating_sub(t_start);

    let mut decode_tokens: u32 = 0;
    let mut utf8 = Utf8Stream::new();
    let mut emit_buf: Vec<u8> = Vec::with_capacity(16);

    // 4) Decode loop. `pending_token` tracks the one sampled token the
    //    model hasn't been fed: if the loop ends here, the next chat
    //    turn feeds it.
    let reason = loop {
        if stop_tokens.contains(next_id) {
            break StopReason::Eos;
        }
        decode_tokens += 1;
        session.pending_token = Some(next_id);
        if !emit_token(session, next_id, &mut utf8, &mut emit_buf, cb) {
            break StopReason::Callback;
        }
        if stop_wanted(session, cfg) {
            break StopReason::Stopped;
        }
        if cfg.max_new_tokens != 0 && decode_tokens >= cfg.max_new_tokens {
            break StopReason::MaxTokens;
        }
        if session.kv.len >= session.max_ctx {
            break StopReason::ContextFull;
        }

        // Forward + sample for the next token. Reuses last_logits's
        // backing allocation (sized once at vocab capacity above).
        step(session, next_id, false, &mut scratch, &mut last_logits);
        if last_logits.is_empty() {
            break StopReason::NoLogits;
        }
        session.pending_token = None;
        next_id = session.sampler_state.sample(&session.sampler_cfg, &mut last_logits);

        // Yield between every token so other tasks (and whoever is going
        // to send `slm stop`) get the CPU.
        unsafe { sched_yield(); }
    };

    // Output that ended inside a UTF-8 sequence gets a replacement
    // character instead of being dropped or left half-written.
    emit_buf.clear();
    utf8.finish(&mut emit_buf);
    if !emit_buf.is_empty() {
        cb(TOKEN_ID_FLUSH, &emit_buf);
    }

    let t_end = unsafe { slm_get_time_ns() };
    let decode_ns = t_end.saturating_sub(t_first_token);

    // 5) Update session telemetry.
    Some(finish(session, prefill_tokens, decode_tokens, ttft_ns, decode_ns, reason))
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

/// Record how a prompt ended and put the session back in a reusable state.
fn finish(
    session: &mut Session,
    prefill_tokens: u32,
    decode_tokens: u32,
    ttft_ns: u64,
    decode_ns: u64,
    stop_reason: StopReason,
) -> DecodeStats {
    if stop_reason == StopReason::Stopped {
        session.state = SessionState::Stopped;
        session.stop_flag.store(false, Ordering::Release);
    } else {
        session.state = SessionState::Open;
    }
    session.prompts_completed = session.prompts_completed.saturating_add(1);
    session.tokens_in = session.tokens_in.saturating_add(prefill_tokens);
    session.tokens_out = session.tokens_out.saturating_add(decode_tokens);
    session.last_ttft_ns = ttft_ns;
    session.last_decode_ns = decode_ns;
    session.last_stop_reason = stop_reason;
    DecodeStats { prefill_tokens, decode_tokens, ttft_ns, decode_ns, stop_reason }
}

/// True if someone asked this prompt to stop. Latches a polled request
/// into the flag so the two sources behave the same.
fn stop_wanted(session: &Session, cfg: &DecodeConfig) -> bool {
    if let Some(poll) = cfg.poll_stop {
        if poll() {
            session.request_stop();
        }
    }
    session.stop_requested()
}

/// Turn the prompt text into the tokens to feed the model.
fn build_prompt_ids(session: &Session, text: &str, mode: PromptMode, tk: &Bbpe) -> Vec<u32> {
    match mode {
        PromptMode::Raw => tk.encode_with_specials(text),
        PromptMode::Chat if session.kv.len == 0 => {
            let system = match session.arch.architecture {
                ArchKind::Qwen2 => QWEN_SYSTEM_PROMPT,
                _ => GENERIC_SYSTEM_PROMPT,
            };
            encode_chat(
                tk,
                &[
                    ChatTurn { role: Role::System, content: system },
                    ChatTurn { role: Role::User, content: text },
                ],
            )
        }
        PromptMode::Chat => encode_chat_continuation(tk, session.pending_token, text),
    }
}

const ALL_OPS: [OpKind; OpKind::COUNT] = [
    OpKind::RmsNorm,
    OpKind::Rope,
    OpKind::Embedding,
    OpKind::Q4kDot,
    OpKind::Q4kGemm,
    OpKind::GqaAttn,
    OpKind::SwiGlu,
    OpKind::LmHead,
];

/// How many ops the tier table currently sends to the GPU.
pub fn gpu_tier_ops() -> u32 {
    ALL_OPS
        .iter()
        .filter(|&&op| matches!(select_tier(op), Tier::Simt | Tier::Hmma))
        .count() as u32
}

/// Reassembles UTF-8 across tokens. BPE tokens split on bytes, so a CJK
/// character or an emoji can span two or more of them.
struct Utf8Stream {
    held: [u8; 4],
    held_len: usize,
}

impl Utf8Stream {
    fn new() -> Self {
        Self { held: [0; 4], held_len: 0 }
    }

    /// Append to `out` everything in `held + bytes` that can be printed
    /// now. Keeps an incomplete trailing sequence; bytes that can never
    /// become valid turn into U+FFFD.
    fn push(&mut self, bytes: &[u8], out: &mut Vec<u8>) {
        let mut work: Vec<u8> = Vec::with_capacity(self.held_len + bytes.len());
        work.extend_from_slice(&self.held[..self.held_len]);
        work.extend_from_slice(bytes);
        self.held_len = 0;

        let mut rest: &[u8] = &work;
        loop {
            match core::str::from_utf8(rest) {
                Ok(_) => {
                    out.extend_from_slice(rest);
                    return;
                }
                Err(e) => {
                    let valid = e.valid_up_to();
                    out.extend_from_slice(&rest[..valid]);
                    match e.error_len() {
                        Some(bad) => {
                            out.extend_from_slice("\u{FFFD}".as_bytes());
                            rest = &rest[valid + bad..];
                        }
                        None => {
                            // At most 3 bytes: a longer prefix would be
                            // a complete 4-byte sequence.
                            let tail = &rest[valid..];
                            self.held[..tail.len()].copy_from_slice(tail);
                            self.held_len = tail.len();
                            return;
                        }
                    }
                }
            }
        }
    }

    /// End of output: an incomplete sequence can't finish now.
    fn finish(&mut self, out: &mut Vec<u8>) {
        if self.held_len > 0 {
            out.extend_from_slice("\u{FFFD}".as_bytes());
            self.held_len = 0;
        }
    }
}

/// Emit one token via the callback, decoding it to bytes first using the
/// registry-owned tokenizer. Returns `false` when the callback asks for
/// an early stop.
fn emit_token(
    session: &Session,
    token_id: u32,
    utf8: &mut Utf8Stream,
    out: &mut Vec<u8>,
    cb: TokenCallback,
) -> bool {
    let mut raw: Vec<u8> = Vec::with_capacity(16);
    let _ = registry::with_loaded_slm(session.model_handle as usize, |slm| {
        if let Some(tk) = slm.tokenizer() {
            tk.decode_token_bytes(token_id, &mut raw);
        }
    });
    out.clear();
    utf8.push(&raw, out);
    cb(token_id, out)
}

/// One forward step: the real forward pass, or a test script if one is
/// armed.
fn step(
    session: &mut Session,
    token_id: u32,
    prefill: bool,
    scratch: &mut ForwardScratch,
    out: &mut Vec<f32>,
) {
    if script::armed() {
        script::step(session, prefill, out);
    } else if session.gpu.is_some() {
        step_many(session, core::slice::from_ref(&token_id), scratch, out);
    } else {
        session.cpu_tokens = session.cpu_tokens.saturating_add(1);
        forward_step_via_registry(session, token_id, scratch, out);
    }
}

/// Feed `tokens` through the session's GPU program. If that fails, the
/// session moves to the CPU for good and these tokens run there, so the
/// caller gets the same logits either way.
fn step_many(session: &mut Session, tokens: &[u32], scratch: &mut ForwardScratch, out: &mut Vec<f32>) {
    let mut prog = match session.gpu.take() {
        Some(p) => p,
        None => {
            run_on_cpu(session, tokens, scratch, out);
            return;
        }
    };
    let pos = session.kv.len;
    // No lock held here: the wait needs IRQs on (runtime/CLAUDE.md, #1093).
    match prog.run(tokens, pos, out) {
        Ok(()) => {
            session.kv.len = pos + tokens.len();
            session.gpu_tokens = session.gpu_tokens.saturating_add(tokens.len() as u64);
            session.gpu_us_per_token = prog.gpu_us_per_token();
            session.gpu = Some(prog);
        }
        Err(why) => {
            let history = core::mem::take(&mut prog.history);
            // Drop closes the program: unmaps the arena, frees it.
            drop(prog);
            fall_back_to_cpu(session, why, &history, scratch, out);
            run_on_cpu(session, tokens, scratch, out);
        }
    }
}

fn run_on_cpu(session: &mut Session, tokens: &[u32], scratch: &mut ForwardScratch, out: &mut Vec<f32>) {
    for &t in tokens {
        session.cpu_tokens = session.cpu_tokens.saturating_add(1);
        forward_step_via_registry(session, t, scratch, out);
    }
}

/// The program's KV cache died with it, so the CPU rebuilds its own from
/// the tokens fed so far. Slow for a long history but always correct, and
/// it doesn't depend on reading anything back from a wedged GPU.
fn fall_back_to_cpu(
    session: &mut Session,
    why: &'static str,
    history: &[u32],
    scratch: &mut ForwardScratch,
    out: &mut Vec<f32>,
) {
    let mut line = alloc::string::String::new();
    let _ = core::fmt::Write::write_fmt(
        &mut line,
        format_args!(
            "[slm] GPU program failed ({why}); session continues on the CPU, replaying {} tokens\n\0",
            history.len()
        ),
    );
    crate::kernel_ffi::print(line.as_bytes());
    session.gpu_note = Some(why);
    session.path = ForwardPath::Cpu;
    session.kv.ensure_storage();
    session.kv.clear();
    run_on_cpu(session, history, scratch, out);
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

/// Scripted logits, so tests can drive the whole prompt path with a
/// fixture that has no layer weights. Armed only by
/// [`test_script_set`] (`rust_slm_test_script`), which only tests call;
/// production pays one relaxed load per forward step.
mod script {
    use super::Session;
    use core::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};

    pub(super) const CAP: usize = 64;
    pub(super) static LEN: AtomicUsize = AtomicUsize::new(0);
    pub(super) static POS: AtomicUsize = AtomicUsize::new(0);
    pub(super) static LOOPING: AtomicBool = AtomicBool::new(false);
    pub(super) static IDS: [AtomicU32; CAP] = [const { AtomicU32::new(0) }; CAP];
    pub(super) static GPU_OPS_SEEN: AtomicU32 = AtomicU32::new(0);

    pub(super) fn armed() -> bool {
        LEN.load(Ordering::Relaxed) != 0
    }

    /// Prefill peeks at the first scripted id; each decode step moves
    /// on. A one-shot script running out looks like a forward failure.
    /// Non-looping scripts advance the KV length as a real forward does.
    pub(super) fn step(session: &mut Session, prefill: bool, out: &mut alloc::vec::Vec<f32>) {
        let per_op = session.path == super::ForwardPath::PerOp;
        GPU_OPS_SEEN.store(if per_op { super::gpu_tier_ops() } else { 0 }, Ordering::Relaxed);
        let len = LEN.load(Ordering::Relaxed);
        let looping = LOOPING.load(Ordering::Relaxed);
        let mut pos = POS.load(Ordering::Relaxed);
        if !prefill {
            pos += 1;
            if looping && pos >= len {
                pos = 0;
            }
            POS.store(pos, Ordering::Relaxed);
        }
        out.clear();
        if !looping && session.kv.len < session.max_ctx {
            session.kv.len += 1;
        }
        if pos < len {
            let id = IDS[pos].load(Ordering::Relaxed) as usize;
            out.resize(session.vocab_size as usize, 0.0);
            if let Some(slot) = out.get_mut(id) {
                *slot = 1.0;
            }
        }
    }
}

/// Arm the scripted-logits test hook: greedy sampling then produces
/// `ids` in order. `looping` repeats them until something stops the
/// prompt, without advancing the KV cache. An empty slice disarms.
pub fn test_script_set(ids: &[u32], looping: bool) -> bool {
    if ids.len() > script::CAP {
        return false;
    }
    script::LEN.store(0, Ordering::Relaxed);
    for (slot, &id) in script::IDS.iter().zip(ids) {
        slot.store(id, Ordering::Relaxed);
    }
    script::POS.store(0, Ordering::Relaxed);
    script::LOOPING.store(looping, Ordering::Relaxed);
    script::LEN.store(ids.len(), Ordering::Relaxed);
    true
}

/// [`gpu_tier_ops`] as seen by the most recent scripted forward step,
/// which is how a test observes `--gpu cpu` from inside a prompt.
pub fn test_script_gpu_ops_seen() -> u32 {
    script::GPU_OPS_SEEN.load(Ordering::Relaxed)
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use crate::inference::gpu_slm::set_tier;
    use crate::slm::registry::{
        build_cli_test_fixture, build_qwen_test_fixture, ensure_mm_initialized_for_tests,
        load_slm, reset_for_tests as reset_registry, CLI_FIXTURE_ENDOFTEXT, CLI_FIXTURE_EOS,
        CLI_FIXTURE_IM_END, CLI_FIXTURE_IM_START,
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

    fn raw_cfg(max_new_tokens: u32) -> DecodeConfig {
        DecodeConfig {
            max_new_tokens,
            stop_tokens: Some(StopTokens::one(9999)),
            prefill_chunk: 4,
            mode: PromptMode::Raw,
            poll_stop: None,
        }
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
        let cfg = raw_cfg(3);
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
        let cfg = raw_cfg(100);
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
        let cfg = raw_cfg(50);

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
        let cfg = DecodeConfig::default();
        s.state = SessionState::Decoding;
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
        let cfg = raw_cfg(5);
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
        let cfg = raw_cfg(1);
        let stats = run_prompt(&mut s, "hi", &cfg, always_callback).expect("decoded");
        assert!(stats.prefill_tokens > 0, "prompt should tokenize to >=1 token");
        assert_eq!(s.tokens_in, stats.prefill_tokens);
    }

    // -- Scripted prompt path ------------------------------------------

    static OUTPUT: std::sync::Mutex<Vec<u8>> = std::sync::Mutex::new(Vec::new());
    static IDS_SEEN: std::sync::Mutex<Vec<u32>> = std::sync::Mutex::new(Vec::new());
    static STOP_AT_TOKEN: AtomicU32 = AtomicU32::new(u32::MAX);
    static POLL_AT_CALL: AtomicU32 = AtomicU32::new(u32::MAX);

    fn record(token_id: u32, bytes: &[u8]) -> bool {
        OUTPUT.lock().unwrap().extend_from_slice(bytes);
        IDS_SEEN.lock().unwrap().push(token_id);
        true
    }

    fn output() -> Vec<u8> {
        OUTPUT.lock().unwrap().clone()
    }

    fn ids_seen() -> Vec<u32> {
        IDS_SEEN.lock().unwrap().clone()
    }

    fn cli_session(ctx: usize) -> Session {
        ensure_mm_initialized_for_tests();
        reset_registry();
        reset_sessions();
        let mut buf = vec![0u8; 16384];
        let n = build_cli_test_fixture(&mut buf).expect("fixture");
        buf.truncate(n);
        let handle = load_slm(b"cli-test", &buf).expect("load_slm") as u32;
        OUTPUT.lock().unwrap().clear();
        IDS_SEEN.lock().unwrap().clear();
        Session::open(handle, ctx, Sampler::Greedy, 1).expect("open")
    }

    fn model_cfg(mode: PromptMode, max_new_tokens: u32) -> DecodeConfig {
        DecodeConfig {
            max_new_tokens,
            stop_tokens: None,
            prefill_chunk: 1,
            mode,
            poll_stop: None,
        }
    }

    /// The EOS id used to be hard-coded to 2. Id 2 must now generate on,
    /// and the id the GGUF names must stop generation.
    #[test]
    fn eos_comes_from_gguf_metadata_not_a_constant() {
        let _serial = TestSerialGuard::new();
        let mut s = cli_session(64);
        test_script_set(&[97, 2, 98, CLI_FIXTURE_EOS, 99], false);
        let stats = run_prompt(&mut s, "hi", &model_cfg(PromptMode::Raw, 0), record).expect("run");
        test_script_set(&[], false);
        assert_eq!(stats.stop_reason, StopReason::Eos);
        assert_eq!(ids_seen(), [97, 2, 98], "stop token itself isn't emitted");
        assert_eq!(stats.decode_tokens, 3);
        assert_eq!(s.state, SessionState::Open);
        assert_eq!(s.pending_token, None, "stop tokens are not fed back");
    }

    #[test]
    fn im_end_and_endoftext_also_stop() {
        let _serial = TestSerialGuard::new();
        for stop in [CLI_FIXTURE_IM_END, CLI_FIXTURE_ENDOFTEXT] {
            let mut s = cli_session(64);
            test_script_set(&[97, stop, 98], false);
            let stats =
                run_prompt(&mut s, "hi", &model_cfg(PromptMode::Raw, 0), record).expect("run");
            test_script_set(&[], false);
            assert_eq!(stats.stop_reason, StopReason::Eos);
            assert_eq!(output(), b"a");
        }
    }

    #[test]
    fn max_new_tokens_caps_output() {
        let _serial = TestSerialGuard::new();
        let mut s = cli_session(64);
        test_script_set(&[97, 98, 99, 100], false);
        let stats = run_prompt(&mut s, "hi", &model_cfg(PromptMode::Raw, 2), record).expect("run");
        test_script_set(&[], false);
        assert_eq!(stats.stop_reason, StopReason::MaxTokens);
        assert_eq!(output(), b"ab");
        assert_eq!(s.pending_token, Some(98));
    }

    /// Chat mode renders the template: the fixture has no merges, so
    /// the expected ids are exactly what `encode_chat` makes.
    #[test]
    fn chat_mode_renders_chatml_and_raw_parses_specials() {
        let _serial = TestSerialGuard::new();
        let mut s = cli_session(256);
        test_script_set(&[CLI_FIXTURE_IM_END], false);
        let cfg = model_cfg(PromptMode::Chat, 0);
        let chat = run_prompt(&mut s, "hi", &cfg, record).expect("run");
        // <|im_start|>system\n + system + <|im_end|>\n + <|im_start|>user\n
        // + hi + <|im_end|>\n + <|im_start|>assistant\n, one byte per token.
        let bytes = "system\n".len() + QWEN_SYSTEM_PROMPT.len() + 1
            + "user\n".len() + 2 + 1 + "assistant\n".len();
        assert_eq!(chat.prefill_tokens as usize, bytes + 3 + 2);

        // --raw with the same text rendered by hand costs the same tokens
        // only if the special names became single ids.
        let mut s = cli_session(256);
        test_script_set(&[CLI_FIXTURE_IM_END], false);
        let raw_cfg = model_cfg(PromptMode::Raw, 0);
        let text = "<|im_start|>hi<|im_end|>";
        let raw = run_prompt(&mut s, text, &raw_cfg, record).expect("run");
        assert_eq!(raw.prefill_tokens, 4);
        test_script_set(&[], false);
        let _ = CLI_FIXTURE_IM_START;
    }

    /// A character split across tokens must reach the callback whole.
    #[test]
    fn utf8_split_across_tokens_is_not_broken() {
        let _serial = TestSerialGuard::new();
        let mut s = cli_session(64);
        // U+4F60 = E4 BD A0, then U+1F600 = F0 9F 98 80, one byte a token.
        test_script_set(
            &[0xE4, 0xBD, 0xA0, 0xF0, 0x9F, 0x98, 0x80, 97, CLI_FIXTURE_IM_END],
            false,
        );
        let stats = run_prompt(&mut s, "hi", &model_cfg(PromptMode::Raw, 0), record).expect("run");
        test_script_set(&[], false);
        assert_eq!(output(), "\u{4F60}\u{1F600}a".as_bytes());
        // One callback per model token still, even when it carries nothing.
        assert_eq!(ids_seen().len(), 8);
        assert_eq!(stats.decode_tokens, 8);
    }

    #[test]
    fn utf8_cut_off_at_the_end_becomes_a_replacement_char() {
        let _serial = TestSerialGuard::new();
        let mut s = cli_session(64);
        test_script_set(&[97, 0xE4, 0xBD, CLI_FIXTURE_IM_END], false);
        let stats = run_prompt(&mut s, "hi", &model_cfg(PromptMode::Raw, 0), record).expect("run");
        test_script_set(&[], false);
        assert_eq!(output(), "a\u{FFFD}".as_bytes());
        assert_eq!(ids_seen(), [97, 0xE4, 0xBD, TOKEN_ID_FLUSH]);
        assert_eq!(stats.decode_tokens, 3, "the flush isn't a model token");
    }

    #[test]
    fn utf8_stream_replaces_invalid_bytes() {
        let mut out = Vec::new();
        let mut u = Utf8Stream::new();
        u.push(&[0xFF, b'a'], &mut out);
        assert_eq!(out, "\u{FFFD}a".as_bytes());
        out.clear();
        // A lead byte followed by a non-continuation: the lead is bad, the
        // following byte survives.
        u.push(&[0xE4], &mut out);
        assert!(out.is_empty());
        u.push(&[b'b'], &mut out);
        assert_eq!(out, "\u{FFFD}b".as_bytes());
        out.clear();
        u.finish(&mut out);
        assert!(out.is_empty(), "nothing left to flush");
    }

    fn stop_self_on_token(token_id: u32, bytes: &[u8]) -> bool {
        record(token_id, bytes);
        token_id != STOP_AT_TOKEN.load(Ordering::SeqCst)
    }

    /// Set the stop flag from a callback, as `slm stop` from another
    /// shell would, on a looping script that otherwise never ends.
    #[test]
    fn stop_flag_ends_a_prompt_and_the_session_takes_another() {
        let _serial = TestSerialGuard::new();
        static STOP_HANDLE: std::sync::Mutex<Option<alloc::sync::Arc<core::sync::atomic::AtomicBool>>> =
            std::sync::Mutex::new(None);
        fn cb(token_id: u32, bytes: &[u8]) -> bool {
            record(token_id, bytes);
            if IDS_SEEN.lock().unwrap().len() == 3 {
                if let Some(flag) = STOP_HANDLE.lock().unwrap().as_ref() {
                    flag.store(true, Ordering::SeqCst);
                }
            }
            true
        }

        let mut s = cli_session(64);
        *STOP_HANDLE.lock().unwrap() = Some(s.stop_flag.clone());
        test_script_set(&[97, 98], true);
        let stats = run_prompt(&mut s, "hi", &model_cfg(PromptMode::Raw, 0), cb).expect("run");
        assert_eq!(stats.stop_reason, StopReason::Stopped);
        assert_eq!(stats.decode_tokens, 3);
        assert_eq!(s.state, SessionState::Stopped);
        assert!(!s.stop_requested(), "honoured stops are cleared");
        assert_eq!(s.pending_token, Some(97));

        // No `slm reset` needed.
        IDS_SEEN.lock().unwrap().clear();
        test_script_set(&[98, CLI_FIXTURE_IM_END], false);
        let again =
            run_prompt(&mut s, "yo", &model_cfg(PromptMode::Raw, 0), record).expect("second");
        test_script_set(&[], false);
        assert_eq!(again.stop_reason, StopReason::Eos);
        assert_eq!(ids_seen(), [98]);
        assert_eq!(s.state, SessionState::Open);
    }

    /// A stop during prefill must not wait for the prompt to finish.
    #[test]
    fn poll_stop_interrupts_prefill() {
        let _serial = TestSerialGuard::new();
        fn poll() -> bool {
            POLL_AT_CALL.fetch_sub(1, Ordering::SeqCst) == 1
        }
        let mut s = cli_session(64);
        POLL_AT_CALL.store(3, Ordering::SeqCst);
        test_script_set(&[97], true);
        let mut cfg = model_cfg(PromptMode::Raw, 0);
        cfg.poll_stop = Some(poll);
        let stats = run_prompt(&mut s, "abcdefgh", &cfg, record).expect("run");
        test_script_set(&[], false);
        assert_eq!(stats.stop_reason, StopReason::Stopped);
        assert_eq!(stats.prefill_tokens, 2, "stopped before the third token");
        assert!(ids_seen().is_empty(), "no output from a stopped prefill");
        assert_eq!(s.state, SessionState::Stopped);
    }

    /// The next chat turn feeds the held-back token and closes the turn.
    #[test]
    fn chat_continuation_feeds_the_held_back_token() {
        let _serial = TestSerialGuard::new();
        let mut s = cli_session(256);
        STOP_AT_TOKEN.store(98, Ordering::SeqCst);
        test_script_set(&[97, 98, 99], false);
        let cfg = model_cfg(PromptMode::Chat, 0);
        let first = run_prompt(&mut s, "hi", &cfg, stop_self_on_token).expect("first");
        STOP_AT_TOKEN.store(u32::MAX, Ordering::SeqCst);
        assert_eq!(first.stop_reason, StopReason::Callback);
        assert_eq!(s.pending_token, Some(98));
        let kv_after_first = s.kv.len;

        test_script_set(&[CLI_FIXTURE_IM_END], false);
        let second = run_prompt(&mut s, "yo", &cfg, record).expect("second");
        test_script_set(&[], false);
        // 98 + <|im_end|> + \n + <|im_start|> + "user\n" + "yo" + <|im_end|>
        // + \n + <|im_start|> + "assistant\n"
        assert_eq!(second.prefill_tokens as usize, 1 + 1 + 1 + 1 + 5 + 2 + 1 + 1 + 1 + 10);
        assert_eq!(s.pending_token, None);
        assert_eq!(s.kv.len, kv_after_first + second.prefill_tokens as usize);
    }

    #[test]
    fn prompt_too_long_for_context_runs_nothing() {
        let _serial = TestSerialGuard::new();
        let mut s = cli_session(4);
        test_script_set(&[97], false);
        let stats =
            run_prompt(&mut s, "abcdefgh", &model_cfg(PromptMode::Raw, 0), record).expect("run");
        test_script_set(&[], false);
        assert_eq!(stats.stop_reason, StopReason::ContextFull);
        assert_eq!(stats.prefill_tokens, 0);
        assert_eq!(s.kv.len, 0);
        assert_eq!(s.state, SessionState::Open);
    }

    /// `--gpu cpu` must hold every op off the GPU while the prompt runs,
    /// and leave the table as it found it.
    #[test]
    fn gpu_cpu_mode_forces_the_cpu_tier_during_a_prompt() {
        let _serial = TestSerialGuard::new();
        let mut s = cli_session(64);
        set_tier(OpKind::Q4kDot, Tier::Simt);
        set_tier(OpKind::SwiGlu, Tier::Hmma);
        test_script_set(&[97, CLI_FIXTURE_IM_END], false);

        let cfg = model_cfg(PromptMode::Raw, 0);
        run_prompt(&mut s, "hi", &cfg, record).expect("run");
        assert_eq!(test_script_gpu_ops_seen(), 2, "auto keeps the table");

        s.gpu_mode = crate::slm::session::GpuMode::Cpu;
        s.path = ForwardPath::Cpu;
        test_script_set(&[97, CLI_FIXTURE_IM_END], false);
        run_prompt(&mut s, "hi", &cfg, record).expect("run");
        assert_eq!(test_script_gpu_ops_seen(), 0, "cpu holds every op on the CPU");
        test_script_set(&[], false);

        assert_eq!(select_tier(OpKind::Q4kDot), Tier::Simt, "restored");
        assert_eq!(select_tier(OpKind::SwiGlu), Tier::Hmma, "restored");
        set_tier(OpKind::Q4kDot, Tier::Cpu);
        set_tier(OpKind::SwiGlu, Tier::Cpu);
    }

    #[test]
    fn empty_prompt_is_a_no_op() {
        let _serial = TestSerialGuard::new();
        let mut s = cli_session(4);
        let stats = run_prompt(&mut s, "", &model_cfg(PromptMode::Chat, 0), record).expect("run");
        assert_eq!(stats.stop_reason, StopReason::EmptyPrompt);
        assert_eq!(stats.prefill_tokens, 0);
        assert!(ids_seen().is_empty());
    }
}
