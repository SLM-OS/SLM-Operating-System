//! Real numeric forward pass — one decode-step through the M4
//! transformer op chain.
//!
//! M5.3.2 of the SLM integration plan (see
//! `docs/plans/slm-integration-plan.md` §M5). Replaces the M5.2
//! zero-logit stub in [`crate::slm::decoder::forward_step`] with a
//! Qwen2-shaped per-layer pipeline:
//!
//! ```text
//! x   = embedding_q4k_lookup(token_embd, token_id)
//! for layer in 0..block_count:
//!     attn_in = x.clone()
//!     x_norm  = rmsnorm(x, attn_norm.weight)
//!     q       = matmul(x_norm, attn_q.weight) + attn_q.bias
//!     k       = matmul(x_norm, attn_k.weight) + attn_k.bias
//!     v       = matmul(x_norm, attn_v.weight) + attn_v.bias
//!     rope.apply(pos, q_per_head); rope.apply(pos, k_per_head)
//!     kv.append(layer, k, v)
//!     attn    = gqa_decode_step(q,
//!                               kv.k_view_with_pending(layer),
//!                               kv.v_view_with_pending(layer), …)
//!     proj    = matmul(attn, attn_output.weight)
//!     x       = attn_in + proj                  (residual)
//!
//!     mlp_in  = x.clone()
//!     x_norm  = rmsnorm(x, ffn_norm.weight)
//!     mlp_out = swiglu_mlp(x_norm, ffn_gate.weight, ffn_up.weight, ffn_down.weight)
//!     x       = mlp_in + mlp_out                (residual)
//! kv.commit_position()
//! x_norm  = rmsnorm(x, output_norm.weight)
//! logits  = lm_head(x_norm, output.weight | token_embd.weight)
//! ```
//!
//! **Qwen2 specifics.**
//! - Q/K/V projections carry **F32 biases** (`attn_q.bias`,
//!   `attn_k.bias`, `attn_v.bias`). LLaMA-1/2/3 don't; Qwen2 does.
//! - Norm weights (`*_norm.weight`, `output_norm.weight`) are stored
//!   as **F32** in the GGUF. The M4.1 `rmsnorm` takes FP16 gamma, so
//!   each call here converts on the fly via [`f32_bytes_to_fp16_vec`].
//!   The cost is ~hidden_size = 1.5k f32→f16 conversions per layer per
//!   token (43k per Qwen2.5-1.5B token); negligible vs the matmuls.
//! - `output.weight` is tied to `token_embd.weight` for some Qwen2
//!   builds — if `output.weight` isn't present we fall back to the
//!   embedding table for the LM head.
//!
//! **Q4_K embedding lookup.** The token embedding table is Q4_K-packed
//! row-major, so per-token lookup means reading
//! `q4_k_byte_size(hidden_size)` bytes for `token_id`'s row and
//! dequantizing them into FP32, then converting to FP16 storage. The
//! cost per token is one row of dequant — `embedding_length / 256`
//! Q4_K blocks (6 blocks for Qwen2.5-1.5B) — also negligible.
//!
//! `no_std` + `alloc` only.

#![cfg(feature = "slm")]
#![allow(clippy::module_name_repetitions)]
#![allow(clippy::too_many_arguments)]

extern crate alloc;

use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;
use core::fmt::Write as _;

use crate::inference::gpu_slm::{select_tier, OpKind, OperatorLibraryBackend, Tier};
use crate::inference::ops_transformer::{
    gqa_decode_step, lm_head_q, matmul_quant_rows, rmsnorm, swiglu_mlp_q, RopeTable,
};
use crate::inference::quant::{dequantize_row_any, q8_k_byte_size};
use crate::slm::gguf::{f32_to_f16, ArchInfo, GgmlType};
use crate::slm::registry::LoadedSlm;
use crate::slm::session::Session;

/// Hybrid RMSNorm: try the GPU operator library if the boot probe
/// (#714 §B.2) flipped this op's tier to `Tier::Simt`, fall through
/// to the existing CPU NEON kernel on any `BackendError` (staging
/// didn't happen, FFI failed, shape rejected, etc.). The CPU path
/// is the source of truth; the GPU path is opportunistic.
///
/// The signature mirrors the CPU `rmsnorm()` so the call sites in
/// `forward_one` / `forward_step` / final norm don't need to change
/// shape — they just swap `rmsnorm` for `rmsnorm_hybrid`. forward.rs
/// always passes single-row slices (`x.len() == gamma.len() ==
/// out.len() == n`), so the GPU dispatch is `n_rows=1, n=x.len()`.
///
/// Per-call cost on the GPU path is ~50-60 µs (cache flush +
/// dispatch + readback) — a net loss vs CPU NEON's ~5 µs, but the
/// path is here as the framework that pays off once the matmul-
/// shaped ops (Q4K_DOT, GQA_ATTN) are wired in follow-on PRs.
#[inline]
fn rmsnorm_hybrid(
    x: &[u16],
    gamma: &[u16],
    eps: f32,
    out: &mut [u16],
) -> Option<()> {
    /* CPU rmsnorm requires x.len() == gamma.len() == out.len().
     * Validate up front so the GPU FFI's shape check matches and
     * we can dispatch with n_rows=1. */
    let n = x.len();
    if n == 0 || gamma.len() != n || out.len() != n {
        return None;
    }
    if let Ok(()) = OperatorLibraryBackend::dispatch_rmsnorm(
        x, gamma, out, 1u32, n as u32, eps,
    ) {
        return Some(());
    }
    rmsnorm(x, gamma, eps, out)
}

/// W4: EMBEDDING hybrid wrapper. Looks up the token embedding table
/// in `gpu_tensor_map`; if present and the tier table allows GPU
/// dispatch, calls `OperatorLibraryBackend::dispatch_embedding`. On
/// any miss/error, falls through to the existing CPU
/// `embedding_lookup_any` path (which does the Q4_K dequant +
/// FP32→FP16 conversion in one pass).
///
/// `out` is the FP16 hidden-state row written for this token; size
/// `embedding_length` u16 elements. `cpu_scratch` is the FP32
/// dequant scratch the CPU path needs — unused on the GPU success
/// path but always passed because the function contract has to be
/// the same as `embedding_lookup_any` for the call sites.
#[inline]
fn embedding_lookup_hybrid(
    slm: &LoadedSlm,
    token_id: u32,
    out: &mut [u16],
    cpu_scratch: &mut Vec<f32>,
) -> Option<()> {
    let hidden = out.len();
    if hidden == 0 {
        return None;
    }
    /* GPU path. The `if let` chain bails to CPU on any miss without
     * rebinding ergonomics. */
    if let Some(table) = slm.gpu_tensor_map().get("token_embd.weight") {
        if select_tier(OpKind::Embedding) == Tier::Simt {
            /* Need table_row_bytes for the kernel's Q4_K block math
             * — derive from the on-disk tensor descriptor (the
             * staging didn't change byte layout, just location). */
            if let Some(info) = slm.tensor_info("token_embd.weight") {
                let table_row_bytes = match table_row_bytes_q4k(info, hidden) {
                    Some(b) => b,
                    None => 0,
                };
                if table_row_bytes != 0 {
                    if let Ok(()) = OperatorLibraryBackend::dispatch_embedding(
                        table.gpu_va,
                        table.size_bytes,
                        token_id,
                        out,
                        hidden as u32,
                        table_row_bytes,
                    ) {
                        return Some(());
                    }
                }
            }
        }
    }
    /* CPU fall-through — same as the pre-W4 path. */
    let (embd_bytes, embd_quant) = tensor_q(slm, "token_embd.weight")?;
    embedding_lookup_any(
        embd_quant,
        embd_bytes,
        hidden,
        token_id,
        out,
        cpu_scratch,
    )
}

/// W5: Q4K_DOT hybrid wrapper — try the GPU operator-library path
/// for a Q4_K matmul, fall back to the existing CPU implementation
/// on any miss/error.
///
/// Conditions for GPU dispatch:
/// 1. `slm.gpu_tensor_map().get(weight_name)` returns Some — the
///    weight tensor was successfully staged at `slm load` time.
/// 2. `select_tier(OpKind::Q4kDot) == Tier::Simt` — the boot probe
///    flipped this op to GPU.
/// 3. `cols % 256 == 0` — Q4K_DOT requires whole-superblock rows.
/// 4. `quant_type == Q4_K` — the kernel only supports Q4_K weights.
///
/// On any miss, falls through to `matmul_quant_rows` (which itself
/// dispatches Q4_K → `matmul_q4k_rows` and other types to the
/// per-row dequant path).
///
/// This is a primitive: bias addition stays the caller's
/// responsibility (the existing `project_with_bias` path is
/// unchanged and still used for Q/K/V projections; only direct
/// `matmul_quant_rows` callers should swap to this helper).
#[inline]
#[allow(clippy::too_many_arguments)]
fn matmul_q4k_rows_hybrid(
    slm: &LoadedSlm,
    weight_name: &str,
    quant_type: GgmlType,
    weights: &[u8],
    rows: usize,
    cols: usize,
    x: &[u16],
    q8k_scratch: &mut [u8],
    dequant_scratch: &mut [f32],
    acts_f32_scratch: &mut [f32],
    out_fp32: &mut [f32],
) -> Option<()> {
    if quant_type == GgmlType::Q4_K
        && cols % 256 == 0
        && cols < (1u32 << 31) as usize
        && rows < (1u32 << 31) as usize
        && select_tier(OpKind::Q4kDot) == Tier::Simt
    {
        if let Some(gref) = slm.gpu_tensor_map().get(weight_name) {
            if OperatorLibraryBackend::dispatch_q4k_dot(
                x,
                gref.gpu_va,
                gref.size_bytes,
                out_fp32,
                cols as u32,
                rows as u32,
            )
            .is_ok()
            {
                return Some(());
            }
        }
    }
    matmul_quant_rows(
        quant_type,
        weights,
        rows,
        cols,
        x,
        q8k_scratch,
        dequant_scratch,
        acts_f32_scratch,
        out_fp32,
    )
}

/// Compute the Q4_K table_row_bytes for a 1-D embedding row of
/// `hidden` elements. The kernel uses this to walk the row's
/// super-blocks; it must equal `ceil(hidden / 256) * 144`. Returns
/// `None` if the tensor isn't Q4_K (the GPU EMBEDDING kernel only
/// supports Q4_K today; SmolLM2's Q8_0 / Qwen2.5's Q6_K stay on
/// the CPU path until those kernels exist).
#[inline]
fn table_row_bytes_q4k(info: &crate::slm::registry::OwnedTensorInfo,
                       hidden: usize) -> Option<u32> {
    use crate::slm::gguf::GgmlType;
    if info.ggml_type != GgmlType::Q4_K.0 {
        return None;
    }
    /* Round hidden up to a 256-element super-block, multiply by the
     * 144-byte block stride. */
    let blocks = hidden.checked_add(255)?.checked_div(256)?;
    let bytes  = blocks.checked_mul(144)?;
    u32::try_from(bytes).ok()
}

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Per-prompt scratch for [`forward_one`]. Allocated once per prompt
/// (cheap relative to the matmuls; M5.3.3 / M9 can revisit if the
/// allocation cost shows up in profiles).
pub struct ForwardScratch {
    /// Hidden state (FP16). `embedding_length` u16s.
    pub x_fp16: Vec<u16>,
    /// Pre-allocated copy of the residual input. Same shape as `x_fp16`.
    pub residual: Vec<u16>,
    /// Post-RMSNorm hidden state. Same shape as `x_fp16`.
    pub x_norm: Vec<u16>,
    /// FP16 norm-weight cache, refilled from F32 bytes per layer.
    pub norm_fp16: Vec<u16>,

    /// Q projection output (`n_head_q × head_dim` u16s).
    pub q_fp16: Vec<u16>,
    /// K projection output (`n_head_kv × head_dim` u16s).
    pub k_fp16: Vec<u16>,
    /// V projection output (`n_head_kv × head_dim` u16s).
    pub v_fp16: Vec<u16>,

    /// FP32 staging buffer for matmul outputs. Sized to the largest
    /// any single matmul produces (`max(hidden, intermediate, vocab)`).
    pub matmul_f32: Vec<f32>,

    /// Attention output, `n_head_q × head_dim` u16s.
    pub attn_out: Vec<u16>,
    /// Attention softmax scratch (`max_ctx` f32s).
    pub attn_logits: Vec<f32>,

    /// SwiGLU `gate` intermediate (`intermediate_size` u16s).
    pub mlp_gate: Vec<u16>,
    /// SwiGLU `up` intermediate (`intermediate_size` u16s).
    pub mlp_up: Vec<u16>,
    /// SwiGLU output, projected back to `hidden_size`.
    pub mlp_out: Vec<u16>,

    /// Q8_K quantization scratch for matmul kernels. Sized to fit
    /// the largest activation row (`max(hidden_size, intermediate_size)`).
    pub q8k_scratch: Vec<u8>,

    /// Output logits buffer (`vocab_size` f32s). Owned here so the
    /// per-token decode loop doesn't allocate ~vocab_size × 4 bytes
    /// (608 KB for Qwen2.5-1.5B) on every step.
    pub logits: Vec<f32>,

    /// FP32 dequant scratch for the embedding lookup. Sized to the
    /// padded row count so a Q4_K embedding table whose `hidden_size`
    /// isn't a multiple of 256 (e.g. SmolLM2's 576 → 3 super-blocks =
    /// 768 floats) doesn't trigger a heap reallocation in
    /// `embedding_lookup_any`.
    pub embed_f32: Vec<f32>,

    /// FP32 dequant scratch for the per-row matmul fallback path
    /// (Q5_0 / Q6_K / Q8_0 weights). Sized to the largest matmul
    /// inner dimension across the model: max(hidden, intermediate)
    /// is ≥ all attention/FFN cols; LM head also fits because its
    /// inner dim is `hidden_size`. The Q4_K fast path doesn't use
    /// this — its vec_dot kernel runs directly on packed bytes.
    pub dequant_f32: Vec<f32>,

    /// FP32-widened activations scratch for the per-row matmul
    /// fallback path. `matmul_quant_rows` widens `acts_fp16` (length
    /// `cols`) into this buffer once per call, then dot-products it
    /// row-by-row with dequantized weights. Pre-allocating here
    /// removes ~211 heap allocations per token (30 layers ×
    /// ~7 matmul_quant_rows calls per layer + 1 lm_head).
    pub acts_f32: Vec<f32>,

    /// Cached RoPE table (head_dim × max_pos pairs).
    pub rope: RopeTable,

    /// Reusable buffer for formatting per-layer tensor names so the
    /// hot loop avoids one `String::new()` per access.
    pub name_buf: String,
}

impl ForwardScratch {
    /// Allocate scratch for the given architecture and context window.
    /// `max_ctx` is the session's effective context length — used to
    /// size the RoPE LUT and the attention softmax scratch.
    pub fn new(arch: &ArchInfo, max_ctx: usize, vocab_size: usize) -> Self {
        let hidden = arch.embedding_length as usize;
        let intermediate = arch.feed_forward_length as usize;
        let head_dim = arch.head_dim as usize;
        let n_head_q = arch.head_count as usize;
        let n_head_kv = arch.head_count_kv as usize;
        let vocab = vocab_size;
        let q_total = n_head_q.saturating_mul(head_dim);
        let kv_total = n_head_kv.saturating_mul(head_dim);
        let max_row = hidden.max(intermediate);
        // The arch dims come from `ArchInfo`'s u32 fields, so they
        // fit `< 2^32` and the `div_ceil + saturating_mul` math
        // below cannot overflow `usize` on 64-bit targets (where
        // `usize::MAX = 2^64 - 1`). The asserts catch the actual
        // overflow risk: `hidden.div_ceil(256)` evaluates as
        // `(hidden + 255) / 256` and would wrap if `hidden` is
        // within 255 of `usize::MAX`. On 64-bit this is unreachable
        // from the `u32` source; the guard makes the dependency
        // explicit so a future arch with `u64` dims (or a hostile
        // 32-bit target where `usize::MAX = u32::MAX`) trips it.
        debug_assert!(
            hidden <= usize::MAX - 255,
            "hidden too close to usize::MAX; div_ceil(256) would overflow",
        );
        debug_assert!(
            intermediate <= usize::MAX - 255,
            "intermediate too close to usize::MAX; div_ceil(256) would overflow",
        );
        // Padded row count for Q4_K rows whose element count isn't a
        // multiple of 256 — e.g. SmolLM2's hidden=576 stored as 3
        // super-blocks = 768 floats. The dequant kernel writes the
        // full padded count; sizing the scratch buffers to it avoids
        // a runtime resize on the first call.
        let padded_hidden = hidden.div_ceil(256).saturating_mul(256).max(hidden);
        let padded_max_row = max_row.div_ceil(256).saturating_mul(256).max(max_row);
        // q8k_scratch sized for the largest activation row that ever
        // feeds `matmul_q4k_rows`. The extra capacity costs ~ a few
        // kilobytes (one Q8_K block = 292 bytes per 256 elements).
        let q8k_bytes = q8_k_byte_size(max_row).unwrap_or(0);

        Self {
            x_fp16: vec![0u16; hidden],
            residual: vec![0u16; hidden],
            x_norm: vec![0u16; hidden],
            norm_fp16: vec![0u16; hidden],
            q_fp16: vec![0u16; q_total],
            k_fp16: vec![0u16; kv_total],
            v_fp16: vec![0u16; kv_total],
            // Reused for {hidden, intermediate, vocab}-sized matmul
            // outputs. We pre-size to `max_row`; the lm_head call
            // resizes up to `vocab_size` lazily, since vocab can be
            // much larger than hidden/intermediate.
            matmul_f32: vec![0.0f32; max_row],
            attn_out: vec![0u16; q_total],
            attn_logits: vec![0.0f32; max_ctx.max(1)],
            mlp_gate: vec![0u16; intermediate],
            mlp_up: vec![0u16; intermediate],
            mlp_out: vec![0u16; hidden],
            q8k_scratch: vec![0u8; q8k_bytes],
            logits: vec![0.0f32; vocab.max(1)],
            embed_f32: vec![0.0f32; padded_hidden],
            dequant_f32: vec![0.0f32; padded_max_row],
            acts_f32: vec![0.0f32; max_row],
            rope: RopeTable::new(head_dim, max_ctx, arch.rope_freq_base),
            name_buf: String::with_capacity(32),
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Run one decode-step forward pass.
///
/// Looks up `token_id`'s embedding, walks every transformer block,
/// applies the final norm + LM head, and writes the FP32 logits into
/// `scratch.logits[..vocab_size]`. The KV cache is extended by one
/// position via [`crate::slm::kv_cache::KvCache::commit_position`];
/// the caller reads logits from `scratch` and drives sampling.
///
/// **Why logits live in `scratch` instead of being returned by value:**
/// keeping the Vec allocation in `ForwardScratch` saves ~vocab × 4
/// bytes of malloc churn per token (608 KB for Qwen2.5-1.5B). The
/// decoder copies into its own per-prompt buffer once via
/// `extend_from_slice` for the sampler to mutate.
///
/// Returns `None` on:
/// - a missing-or-malformed weight tensor (any required name absent),
/// - a shape mismatch between a tensor and what the arch metadata
///   advertises,
/// - a KV-cache append failure (cache full),
/// - an out-of-range `token_id` for the embedding table.
///
/// All multiplications across dimensions go through `checked_mul` /
/// bounds-validated indexing per `runtime/CLAUDE.md`.
pub fn forward_one(
    session: &mut Session,
    slm: &LoadedSlm,
    token_id: u32,
    scratch: &mut ForwardScratch,
) -> Option<()> {
    let arch = slm.arch();
    let hidden = arch.embedding_length as usize;
    let intermediate = arch.feed_forward_length as usize;
    let head_dim = arch.head_dim as usize;
    let n_head_q = arch.head_count as usize;
    let n_head_kv = arch.head_count_kv as usize;
    let block_count = arch.block_count as usize;
    let vocab_size = session.vocab_size as usize;

    // Sanity: scratch was sized for this arch.
    if scratch.x_fp16.len() != hidden
        || scratch.residual.len() != hidden
        || scratch.x_norm.len() != hidden
        || scratch.norm_fp16.len() != hidden
        || scratch.q_fp16.len() != n_head_q.checked_mul(head_dim)?
        || scratch.k_fp16.len() != n_head_kv.checked_mul(head_dim)?
        || scratch.v_fp16.len() != n_head_kv.checked_mul(head_dim)?
        || scratch.attn_out.len() != n_head_q.checked_mul(head_dim)?
        || scratch.mlp_gate.len() != intermediate
        || scratch.mlp_up.len() != intermediate
        || scratch.mlp_out.len() != hidden
    {
        return None;
    }
    // hidden / intermediate were originally constrained to be multiples
    // of Q4_K_BLOCK_ELEMENTS = 256 because the M5.3.x forward pass
    // assumed every weight was Q4_K-aligned. SmolLM2's hidden=576
    // breaks that — it's a multiple of 32 (Q5_0 / Q8_0 block size)
    // but not 256. With the multi-quant dispatcher in place, the
    // matmul kernel handles partial-block Q4_K rows internally, so
    // the only structural requirement is dimensions > 0 and the
    // GQA head ratio.
    if head_dim == 0
        || n_head_q == 0
        || n_head_kv == 0
        || vocab_size == 0
        || block_count == 0
        || hidden == 0
        || intermediate == 0
        || n_head_q % n_head_kv != 0
    {
        return None;
    }

    // Position the KV cache will occupy after this token.
    let pos = session.kv.len;
    if pos >= session.max_ctx {
        return None;
    }

    // ---------------------------------------------------------------
    // Token embedding lookup. The table's quant type varies by build:
    // Qwen2.5 Q4_K_M ships token_embd as Q6_K, SmolLM2 as Q8_0, the
    // M5.3.x synthetic fixture as Q4_K. Dispatch on whatever the
    // GGUF descriptor says.
    // ---------------------------------------------------------------
    embedding_lookup_hybrid(
        slm,
        token_id,
        &mut scratch.x_fp16,
        &mut scratch.embed_f32,
    )?;

    // ---------------------------------------------------------------
    // Per-layer transformer block.
    // ---------------------------------------------------------------
    for layer in 0..block_count {
        // Save residual for the attention sub-block.
        scratch.residual.copy_from_slice(&scratch.x_fp16);

        // -- attention RMSNorm ---------------------------------------
        let attn_norm_bytes = layer_tensor_bytes(slm, &mut scratch.name_buf, layer, "attn_norm.weight")?;
        f32_bytes_into_fp16_slice(attn_norm_bytes, &mut scratch.norm_fp16)?;
        rmsnorm_hybrid(&scratch.x_fp16, &scratch.norm_fp16, 1e-6, &mut scratch.x_norm)?;

        // -- Q / K / V projections (matmul + optional F32 bias add) --
        //
        // Biases are present in Qwen2 GGUFs but absent from LLaMA-1/2/3
        // and SmolLM. Quant type varies per tensor: Qwen2.5-1.5B uses
        // Q4_K, SmolLM uses Q5_0 for Q/K/output and Q8_0 for V. Each
        // weight's `ggml_type` comes from the GGUF tensor descriptor;
        // `layer_tensor_q` returns both bytes + type in one lookup.
        let (q_w, q_quant) = layer_tensor_q(slm, &mut scratch.name_buf, layer, "attn_q.weight")?;
        let q_b = layer_tensor_bytes_opt(slm, &mut scratch.name_buf, layer, "attn_q.bias");
        project_with_bias(
            q_quant,
            q_w,
            n_head_q.checked_mul(head_dim)?,
            hidden,
            &scratch.x_norm,
            q_b,
            &mut scratch.q8k_scratch,
            &mut scratch.matmul_f32,
            &mut scratch.dequant_f32,
            &mut scratch.acts_f32,
            &mut scratch.q_fp16,
        )?;

        let (k_w, k_quant) = layer_tensor_q(slm, &mut scratch.name_buf, layer, "attn_k.weight")?;
        let k_b = layer_tensor_bytes_opt(slm, &mut scratch.name_buf, layer, "attn_k.bias");
        project_with_bias(
            k_quant,
            k_w,
            n_head_kv.checked_mul(head_dim)?,
            hidden,
            &scratch.x_norm,
            k_b,
            &mut scratch.q8k_scratch,
            &mut scratch.matmul_f32,
            &mut scratch.dequant_f32,
            &mut scratch.acts_f32,
            &mut scratch.k_fp16,
        )?;

        let (v_w, v_quant) = layer_tensor_q(slm, &mut scratch.name_buf, layer, "attn_v.weight")?;
        let v_b = layer_tensor_bytes_opt(slm, &mut scratch.name_buf, layer, "attn_v.bias");
        project_with_bias(
            v_quant,
            v_w,
            n_head_kv.checked_mul(head_dim)?,
            hidden,
            &scratch.x_norm,
            v_b,
            &mut scratch.q8k_scratch,
            &mut scratch.matmul_f32,
            &mut scratch.dequant_f32,
            &mut scratch.acts_f32,
            &mut scratch.v_fp16,
        )?;

        // -- RoPE (per-head) -----------------------------------------
        for h in 0..n_head_q {
            let off = h.checked_mul(head_dim)?;
            scratch.rope.apply(pos, &mut scratch.q_fp16[off..off + head_dim]);
        }
        for h in 0..n_head_kv {
            let off = h.checked_mul(head_dim)?;
            scratch.rope.apply(pos, &mut scratch.k_fp16[off..off + head_dim]);
        }

        // -- KV-cache append -----------------------------------------
        session.kv.append(layer, &scratch.k_fp16, &scratch.v_fp16)?;

        // -- GQA attention -------------------------------------------
        // `seq_len` covers positions `0..=pos` (inclusive — the
        // just-appended slot is part of the attention input). Use
        // the *_with_pending views so the slice covers `0..=len`
        // (length `(len+1) * per_pos`); plain `k_view` / `v_view`
        // return `0..len`, which would short the most recent slot
        // and trip `gqa_decode_step`'s shape check on every call.
        let seq_len = pos.checked_add(1)?;
        if scratch.attn_logits.len() < seq_len {
            scratch.attn_logits.resize(seq_len, 0.0);
        }
        let k_view = session.kv.k_view_with_pending(layer)?;
        let v_view = session.kv.v_view_with_pending(layer)?;
        gqa_decode_step(
            &scratch.q_fp16,
            k_view,
            v_view,
            n_head_q,
            n_head_kv,
            head_dim,
            seq_len,
            &mut scratch.attn_logits[..seq_len],
            &mut scratch.attn_out,
        )?;

        // -- Output projection ---------------------------------------
        let (o_w, o_quant) =
            layer_tensor_q(slm, &mut scratch.name_buf, layer, "attn_output.weight")?;
        let proj_out_len = hidden;
        if scratch.matmul_f32.len() < proj_out_len {
            scratch.matmul_f32.resize(proj_out_len, 0.0);
        }
        let proj_in_len = n_head_q.checked_mul(head_dim)?;
        if scratch.dequant_f32.len() < proj_in_len {
            scratch.dequant_f32.resize(proj_in_len, 0.0);
        }
        if scratch.acts_f32.len() < proj_in_len {
            scratch.acts_f32.resize(proj_in_len, 0.0);
        }
        /* W5: hybrid path — try GPU Q4K_DOT for the output
         * projection if the tensor was staged. Falls through to
         * `matmul_quant_rows` on miss (no staging, wrong quant,
         * tier table says CPU). */
        let attn_output_name = layer_tensor_name(
            &mut scratch.name_buf, layer, "attn_output.weight")?;
        matmul_q4k_rows_hybrid(
            slm,
            attn_output_name,
            o_quant,
            o_w,
            proj_out_len,
            proj_in_len,
            &scratch.attn_out,
            &mut scratch.q8k_scratch,
            &mut scratch.dequant_f32,
            &mut scratch.acts_f32,
            &mut scratch.matmul_f32[..proj_out_len],
        )?;

        // -- Attention residual --------------------------------------
        for i in 0..hidden {
            let r = crate::slm::gguf::f16_to_f32(scratch.residual[i]);
            scratch.x_fp16[i] = f32_to_f16(r + scratch.matmul_f32[i]);
        }

        // -- MLP block: residual + RMSNorm + SwiGLU + residual -------
        scratch.residual.copy_from_slice(&scratch.x_fp16);

        let ffn_norm_bytes = layer_tensor_bytes(slm, &mut scratch.name_buf, layer, "ffn_norm.weight")?;
        f32_bytes_into_fp16_slice(ffn_norm_bytes, &mut scratch.norm_fp16)?;
        rmsnorm_hybrid(&scratch.x_fp16, &scratch.norm_fp16, 1e-6, &mut scratch.x_norm)?;

        // FFN weights mix quant types in K_M tiers — Qwen2.5-1.5B has
        // ffn_gate/ffn_up = Q4_K but ffn_down = Q6_K; SmolLM mixes
        // similarly. Look up each tensor's declared type.
        let (gate_w, gate_q) = layer_tensor_q(slm, &mut scratch.name_buf, layer, "ffn_gate.weight")?;
        let (up_w, up_q) = layer_tensor_q(slm, &mut scratch.name_buf, layer, "ffn_up.weight")?;
        let (down_w, down_q) = layer_tensor_q(slm, &mut scratch.name_buf, layer, "ffn_down.weight")?;
        swiglu_mlp_q(
            &scratch.x_norm,
            gate_q,
            gate_w,
            up_q,
            up_w,
            down_q,
            down_w,
            hidden,
            intermediate,
            &mut scratch.mlp_gate,
            &mut scratch.mlp_up,
            &mut scratch.q8k_scratch,
            &mut scratch.dequant_f32,
            &mut scratch.acts_f32,
            &mut scratch.mlp_out,
        )?;

        for i in 0..hidden {
            let r = crate::slm::gguf::f16_to_f32(scratch.residual[i]);
            let m = crate::slm::gguf::f16_to_f32(scratch.mlp_out[i]);
            scratch.x_fp16[i] = f32_to_f16(r + m);
        }
    }

    // ---------------------------------------------------------------
    // Final norm + LM head.
    // ---------------------------------------------------------------
    session.kv.commit_position()?;

    let out_norm_bytes = slm.tensor_bytes("output_norm.weight")?;
    f32_bytes_into_fp16_slice(out_norm_bytes, &mut scratch.norm_fp16)?;
    rmsnorm_hybrid(&scratch.x_fp16, &scratch.norm_fp16, 1e-6, &mut scratch.x_norm)?;

    // Some Qwen2 builds tie `output.weight` to `token_embd.weight`. If
    // a dedicated LM-head tensor is present, use it; otherwise fall
    // back to the embedding table. The two tensors usually differ in
    // quant type (Qwen2.5 ships output.weight as Q6_K and reuses
    // token_embd.weight as the embedding table) — pick up whichever
    // is present and dispatch on its declared type.
    let (lm_w, lm_quant) = tensor_q(slm, "output.weight")
        .or_else(|| tensor_q(slm, "token_embd.weight"))?;

    if scratch.logits.len() < vocab_size {
        scratch.logits.resize(vocab_size, 0.0);
    }
    lm_head_q(
        &scratch.x_norm,
        lm_quant,
        lm_w,
        hidden,
        vocab_size,
        &mut scratch.q8k_scratch,
        &mut scratch.dequant_f32,
        &mut scratch.acts_f32,
        &mut scratch.logits[..vocab_size],
    )?;
    Some(())
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Quantization-aware embedding-table row lookup.
///
/// `table_bytes` is the entire embedding table laid out row-major,
/// `vocab_size` rows of `embedding_length` quantized elements (size
/// per row depends on `quant_type`). Reads `token_id`'s row,
/// dequantizes to FP32, converts to FP16, and writes
/// `embedding_length` u16s into `out`.
///
/// Token embeddings vary by quant tier: Qwen2.5-1.5B Q4_K_M ships
/// `token_embd.weight` as **Q6_K** (the heavier "important tensor"
/// tier in K_M); SmolLM2-135M Q4_K_M ships it as **Q8_0**. This
/// function dispatches via [`crate::inference::quant::dequantize_row_any`]
/// so all supported types work.
///
/// Returns `None` on:
/// - unsupported `quant_type`,
/// - the row's byte range falling outside `table_bytes`,
/// - `out.len() != embedding_length`.
pub fn embedding_q4k_lookup(
    table_bytes: &[u8],
    embedding_length: usize,
    token_id: u32,
    out: &mut [u16],
    scratch_f32: &mut Vec<f32>,
) -> Option<()> {
    // Compatibility shim for tests / fixtures that still expect a
    // Q4_K-only call path.
    embedding_lookup_any(
        GgmlType::Q4_K,
        table_bytes,
        embedding_length,
        token_id,
        out,
        scratch_f32,
    )
}

/// Quantization-aware variant of [`embedding_q4k_lookup`]. Real
/// models call this from `forward_one` with the embedding table's
/// declared quant type (Q4_K / Q6_K / Q8_0 are the common ones).
pub fn embedding_lookup_any(
    quant_type: GgmlType,
    table_bytes: &[u8],
    embedding_length: usize,
    token_id: u32,
    out: &mut [u16],
    scratch_f32: &mut Vec<f32>,
) -> Option<()> {
    if embedding_length == 0 || out.len() != embedding_length {
        return None;
    }
    let row_bytes = crate::inference::quant::quant_row_bytes(quant_type, embedding_length)?;
    let tok = usize::try_from(token_id).ok()?;
    let start = tok.checked_mul(row_bytes)?;
    let end = start.checked_add(row_bytes)?;
    if end > table_bytes.len() {
        return None;
    }
    let row = &table_bytes[start..end];

    // Q4_K rows may be padded out to the next 256-element super-block
    // boundary (`embedding_length = 576` → 3 super-blocks → 768
    // floats stored). Allocate enough scratch for the padded count;
    // we'll only convert the first `embedding_length` floats below.
    let n_per_row = match quant_type {
        GgmlType::Q4_K => {
            let nb = row_bytes / 144;
            nb.checked_mul(256)?
        }
        _ => embedding_length,
    };
    if scratch_f32.len() < n_per_row {
        scratch_f32.resize(n_per_row, 0.0);
    }
    let n = dequantize_row_any(quant_type, row, &mut scratch_f32[..n_per_row])?;
    if n < embedding_length {
        return None;
    }
    for i in 0..embedding_length {
        out[i] = f32_to_f16(scratch_f32[i]);
    }
    Some(())
}

/// Convert an F32 byte slice (little-endian) into an FP16 vector of
/// the same element count, written into `out`.
///
/// Used by [`forward_one`] to materialize Qwen2's F32 norm weights
/// into the FP16 form `rmsnorm` expects. Returns `None` if `bytes`
/// isn't a multiple of 4 or doesn't match `out.len() * 4`.
fn f32_bytes_into_fp16_slice(bytes: &[u8], out: &mut [u16]) -> Option<()> {
    if bytes.len() != out.len().checked_mul(4)? {
        return None;
    }
    for (i, chunk) in bytes.chunks_exact(4).enumerate() {
        let arr: [u8; 4] = chunk.try_into().ok()?;
        let f = f32::from_le_bytes(arr);
        out[i] = f32_to_f16(f);
    }
    Some(())
}

/// Multi-row matmul + optional per-output-row F32 bias add, FP16 output.
///
/// Used by the Q/K/V/output projections. With bias (`Some(bytes)`),
/// computes `out = matmul(x_norm, weight) + bias`. Without bias
/// (`None`), computes `out = matmul(x_norm, weight)`.
///
/// **Architecture variants:** Qwen2 stores QKV biases (`attn_q.bias`,
/// `attn_k.bias`, `attn_v.bias`) in the GGUF; LLaMA-1/2/3 and SmolLM
/// (which uses the LLaMA arch) don't. The caller passes `None` for
/// the bias-less case so a missing tensor doesn't kill the forward
/// pass via `?` on a hard `tensor_bytes` lookup.
///
/// **Quantization variants:** the weight tensor's quant type is
/// passed explicitly so this function works for Q4_K (Qwen2.5
/// attention), Q5_0 (SmolLM attention), Q8_0 (SmolLM V), and
/// anything else `matmul_quant_rows` supports.
fn project_with_bias(
    weight_quant: GgmlType,
    weight: &[u8],
    rows: usize,
    cols: usize,
    acts_fp16: &[u16],
    bias_f32_bytes: Option<&[u8]>,
    q8k_scratch: &mut [u8],
    matmul_f32: &mut Vec<f32>,
    dequant_scratch: &mut Vec<f32>,
    acts_f32_scratch: &mut Vec<f32>,
    out_fp16: &mut [u16],
) -> Option<()> {
    if out_fp16.len() != rows {
        return None;
    }
    if let Some(bytes) = bias_f32_bytes {
        if bytes.len() != rows.checked_mul(4)? {
            return None;
        }
    }
    if matmul_f32.len() < rows {
        matmul_f32.resize(rows, 0.0);
    }
    if dequant_scratch.len() < cols {
        dequant_scratch.resize(cols, 0.0);
    }
    if acts_f32_scratch.len() < cols {
        acts_f32_scratch.resize(cols, 0.0);
    }
    matmul_quant_rows(
        weight_quant,
        weight,
        rows,
        cols,
        acts_fp16,
        q8k_scratch,
        dequant_scratch,
        acts_f32_scratch,
        &mut matmul_f32[..rows],
    )?;
    match bias_f32_bytes {
        Some(bytes) => {
            for i in 0..rows {
                let chunk = &bytes[i * 4..(i + 1) * 4];
                let arr: [u8; 4] = chunk.try_into().ok()?;
                let b = f32::from_le_bytes(arr);
                out_fp16[i] = f32_to_f16(matmul_f32[i] + b);
            }
        }
        None => {
            for i in 0..rows {
                out_fp16[i] = f32_to_f16(matmul_f32[i]);
            }
        }
    }
    Some(())
}

/// W5 helper: format `blk.{layer}.{suffix}` into `name_buf` (cleared
/// first) and return the formatted string slice. Used by hybrid
/// wrappers that need the tensor name for `gpu_tensor_map` lookup
/// without doing the bytes read.
fn layer_tensor_name<'a>(
    name_buf: &'a mut String,
    layer: usize,
    suffix: &str,
) -> Option<&'a str> {
    name_buf.clear();
    write!(name_buf, "blk.{}.{}", layer, suffix).ok()?;
    Some(name_buf.as_str())
}

/// Format `blk.{layer}.{suffix}` into `name_buf` (cleared first) and
/// look up the resulting tensor's bytes.
fn layer_tensor_bytes<'a>(
    slm: &'a LoadedSlm,
    name_buf: &mut String,
    layer: usize,
    suffix: &str,
) -> Option<&'a [u8]> {
    name_buf.clear();
    // `write!` returns `Err` only on allocation failure for `String`,
    // which is already a panic path (`String::push_str` would also
    // OOM); using `.ok()?` keeps the API infallible at the call site.
    write!(name_buf, "blk.{}.{}", layer, suffix).ok()?;
    slm.tensor_bytes(name_buf.as_str())
}

/// Same shape as [`layer_tensor_bytes`] but distinguishes "tensor
/// absent from this GGUF" from "lookup itself failed". Returns
/// `Some(bytes)` when the tensor exists, `None` when it doesn't.
///
/// Used for tensors that some architectures ship and others don't
/// — e.g. Qwen2 has `attn_q.bias` / `attn_k.bias` / `attn_v.bias`
/// but LLaMA / SmolLM omit them. Callers pass the returned `Option`
/// through to [`project_with_bias`], which adds the bias when
/// present and skips it when not.
///
/// The two functions look identical at the source level — both
/// boil down to `slm.tensor_bytes(name)` — but the distinct names
/// make it obvious at the call site whether a missing tensor is a
/// hard error (use `layer_tensor_bytes(...)?`) or expected
/// (use `layer_tensor_bytes_opt(...)`).
fn layer_tensor_bytes_opt<'a>(
    slm: &'a LoadedSlm,
    name_buf: &mut String,
    layer: usize,
    suffix: &str,
) -> Option<&'a [u8]> {
    name_buf.clear();
    write!(name_buf, "blk.{}.{}", layer, suffix).ok()?;
    slm.tensor_bytes(name_buf.as_str())
}

/// Look up a layer's tensor and return its bytes plus quant type.
///
/// Real-world Q4_K_M GGUFs mix quant types within a single layer
/// (Qwen2.5-1.5B's `ffn_down` is Q6_K while `ffn_gate`/`ffn_up` are
/// Q4_K; SmolLM2 mixes Q4_K + Q5_0 + Q8_0 in attention). The forward
/// pass needs the quant type to pick the right dequant kernel; this
/// helper grabs both in one call via
/// [`LoadedSlm::tensor_info_and_bytes`] so the linear tensor-table
/// scan happens once per name rather than twice.
fn layer_tensor_q<'a>(
    slm: &'a LoadedSlm,
    name_buf: &mut String,
    layer: usize,
    suffix: &str,
) -> Option<(&'a [u8], GgmlType)> {
    name_buf.clear();
    write!(name_buf, "blk.{}.{}", layer, suffix).ok()?;
    let (info, bytes) = slm.tensor_info_and_bytes(name_buf.as_str())?;
    Some((bytes, GgmlType(info.ggml_type)))
}

/// Same as [`layer_tensor_q`] but for top-level (non-layer) tensors.
fn tensor_q<'a>(slm: &'a LoadedSlm, name: &str) -> Option<(&'a [u8], GgmlType)> {
    let (info, bytes) = slm.tensor_info_and_bytes(name)?;
    Some((bytes, GgmlType(info.ggml_type)))
}

/// Look up a `GgmlType` for the named tensor (sanity-check helper, not
/// used in the hot path). Kept as a small utility for debugging.
#[allow(dead_code)]
pub fn tensor_type(slm: &LoadedSlm, name: &str) -> Option<GgmlType> {
    Some(GgmlType(slm.tensor_info(name)?.ggml_type))
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slm::gguf::{f16_to_f32, ArchKind, Q4_K_BLOCK_SIZE};

    fn test_arch() -> ArchInfo {
        ArchInfo {
            architecture: ArchKind::Qwen2,
            block_count: 1,
            embedding_length: 256,
            head_count: 1,
            head_count_kv: 1,
            head_dim: 256,
            feed_forward_length: 256,
            context_length: 16,
            rope_freq_base: 1_000_000.0,
        }
    }

    #[test]
    fn forward_scratch_shapes_match_arch() {
        let arch = test_arch();
        let s = ForwardScratch::new(&arch, 16, 32);
        assert_eq!(s.x_fp16.len(), 256);
        assert_eq!(s.residual.len(), 256);
        assert_eq!(s.x_norm.len(), 256);
        assert_eq!(s.norm_fp16.len(), 256);
        assert_eq!(s.q_fp16.len(), 256);
        assert_eq!(s.k_fp16.len(), 256);
        assert_eq!(s.v_fp16.len(), 256);
        assert_eq!(s.mlp_gate.len(), 256);
        assert_eq!(s.mlp_up.len(), 256);
        assert_eq!(s.mlp_out.len(), 256);
        assert!(s.attn_logits.len() >= 16);
        assert!(s.q8k_scratch.len() >= 292);
    }

    #[test]
    fn embedding_q4k_lookup_zero_block_yields_zero_row() {
        // One row, one super-block, all zeros. Every dequantized
        // float is `d * (qs.lo) - dmin * mn = 0 * 0 - 0 * 0 = 0`.
        let row = [0u8; Q4_K_BLOCK_SIZE];
        let mut out = vec![0u16; Q4_K_BLOCK_ELEMENTS];
        let mut scratch = Vec::new();
        embedding_q4k_lookup(&row, Q4_K_BLOCK_ELEMENTS, 0, &mut out, &mut scratch)
            .expect("lookup");
        for &b in &out {
            assert_eq!(f16_to_f32(b), 0.0);
        }
    }

    #[test]
    fn embedding_q4k_lookup_picks_correct_row() {
        let mut bytes = vec![0u8; 2 * Q4_K_BLOCK_SIZE];
        bytes[Q4_K_BLOCK_SIZE..Q4_K_BLOCK_SIZE + 2].copy_from_slice(&0x3C00u16.to_le_bytes());
        bytes[Q4_K_BLOCK_SIZE + 4] = 0x01;
        bytes[Q4_K_BLOCK_SIZE + 16] = 0x01;
        let mut out = vec![0u16; Q4_K_BLOCK_ELEMENTS];
        let mut scratch = Vec::new();
        embedding_q4k_lookup(&bytes, Q4_K_BLOCK_ELEMENTS, 1, &mut out, &mut scratch)
            .expect("row 1");
        assert!(f16_to_f32(out[0]).abs() > 0.0);

        let mut out0 = vec![0u16; Q4_K_BLOCK_ELEMENTS];
        embedding_q4k_lookup(&bytes, Q4_K_BLOCK_ELEMENTS, 0, &mut out0, &mut scratch)
            .expect("row 0");
        assert_eq!(f16_to_f32(out0[0]), 0.0);
    }

    #[test]
    fn embedding_q4k_lookup_rejects_oob_token() {
        let row = vec![0u8; Q4_K_BLOCK_SIZE];
        let mut out = vec![0u16; Q4_K_BLOCK_ELEMENTS];
        let mut scratch = Vec::new();
        assert!(
            embedding_q4k_lookup(&row, Q4_K_BLOCK_ELEMENTS, 1, &mut out, &mut scratch).is_none()
        );
    }

    #[test]
    fn embedding_q4k_lookup_rejects_bad_dim() {
        let row = vec![0u8; Q4_K_BLOCK_SIZE];
        let mut out = vec![0u16; 200];
        let mut scratch = Vec::new();
        assert!(embedding_q4k_lookup(&row, 200, 0, &mut out, &mut scratch).is_none());
    }

    #[test]
    fn f32_bytes_into_fp16_slice_round_trips_known_values() {
        let mut buf = Vec::with_capacity(16);
        for v in &[1.0f32, -2.0, 3.5, -0.5] {
            buf.extend_from_slice(&v.to_le_bytes());
        }
        let mut out = vec![0u16; 4];
        f32_bytes_into_fp16_slice(&buf, &mut out).expect("ok");
        assert!((f16_to_f32(out[0]) - 1.0).abs() < 1e-3);
        assert!((f16_to_f32(out[1]) - (-2.0)).abs() < 1e-3);
        assert!((f16_to_f32(out[2]) - 3.5).abs() < 1e-3);
        assert!((f16_to_f32(out[3]) - (-0.5)).abs() < 1e-3);
    }

    #[test]
    fn f32_bytes_into_fp16_slice_rejects_size_mismatch() {
        let buf = vec![0u8; 9]; // not a multiple of 4
        let mut out = vec![0u16; 2];
        assert!(f32_bytes_into_fp16_slice(&buf, &mut out).is_none());
    }

    /// Pipeline composition test (option (c) from the M5.3.2 plan):
    /// hand-build a 1-layer micro-model at minimal Q4_K dimensions
    /// and exercise the same op chain `forward_one` walks (rmsnorm
    /// → matmul → bias → rope → gqa → projection → residual →
    /// rmsnorm → swiglu → residual → rmsnorm → lm_head). All weights
    /// are zero-blocks, so the expected logit is zero — the test
    /// validates that the ops compose without shape errors and that
    /// the final-norm + LM-head path returns a vocab-shaped output.
    #[test]
    fn pipeline_composition_zero_weights_zero_logits() {
        use crate::inference::ops_transformer::{
            gqa_decode_step, lm_head, matmul_q4k_rows, rmsnorm, swiglu_mlp,
        };

        let hidden = Q4_K_BLOCK_ELEMENTS;
        let inter = Q4_K_BLOCK_ELEMENTS;
        let vocab = 8usize;
        let head_dim = hidden;
        let row_bytes = Q4_K_BLOCK_SIZE;

        // Hidden state (zero, since the embedding table is zeroed).
        let x = vec![0u16; hidden];
        let mut x_norm = vec![0u16; hidden];
        let gamma = vec![0x3C00u16; hidden]; // FP16 1.0
        rmsnorm(&x, &gamma, 1e-6, &mut x_norm).expect("rmsnorm");

        // Q/K/V projections from a zero-weight Q4_K matrix.
        let qkv_weight = vec![0u8; hidden * row_bytes];
        let mut q = vec![0.0f32; hidden];
        let mut q8k = vec![0u8; q8_k_byte_size(hidden).unwrap()];
        matmul_q4k_rows(&qkv_weight, hidden, hidden, &x_norm, &mut q8k, &mut q)
            .expect("matmul");
        // Bias add (no-op; bias is zero).
        let q_fp16: Vec<u16> = q.iter().map(|&f| f32_to_f16(f)).collect();
        let k_fp16 = q_fp16.clone();
        let v_fp16 = q_fp16.clone();

        // GQA over a single position — effectively returns v.
        let mut scratch_logits = vec![0.0f32; 1];
        let mut attn_out = vec![0u16; hidden];
        gqa_decode_step(
            &q_fp16,
            &k_fp16,
            &v_fp16,
            1,
            1,
            head_dim,
            1,
            &mut scratch_logits,
            &mut attn_out,
        )
        .expect("gqa");

        // Output projection back to `hidden` (zero weights).
        let o_w = vec![0u8; hidden * row_bytes];
        let mut o_f32 = vec![0.0f32; hidden];
        matmul_q4k_rows(&o_w, hidden, hidden, &attn_out, &mut q8k, &mut o_f32)
            .expect("matmul");

        // Residual + RMSNorm.
        let mut x2 = x.clone();
        for i in 0..hidden {
            x2[i] = f32_to_f16(f16_to_f32(x[i]) + o_f32[i]);
        }
        let mut x2_norm = vec![0u16; hidden];
        rmsnorm(&x2, &gamma, 1e-6, &mut x2_norm).expect("rmsnorm");

        // SwiGLU MLP (zero weights → zero output).
        let mut mlp_out = vec![0u16; hidden];
        let mut gate_s = vec![0u16; inter];
        let mut up_s = vec![0u16; inter];
        let gw = vec![0u8; inter * row_bytes];
        let uw = vec![0u8; inter * row_bytes];
        let dw = vec![0u8; hidden * row_bytes];
        swiglu_mlp(
            &x2_norm,
            &gw,
            &uw,
            &dw,
            hidden,
            inter,
            &mut gate_s,
            &mut up_s,
            &mut q8k,
            &mut mlp_out,
        )
        .expect("swiglu");

        // Residual + final RMSNorm + LM head.
        let mut x3 = vec![0u16; hidden];
        for i in 0..hidden {
            x3[i] =
                f32_to_f16(f16_to_f32(x2[i]) + f16_to_f32(mlp_out[i]));
        }
        let mut x3_norm = vec![0u16; hidden];
        rmsnorm(&x3, &gamma, 1e-6, &mut x3_norm).expect("rmsnorm");

        let lm_w = vec![0u8; vocab * row_bytes];
        let mut logits = vec![1.0f32; vocab];
        lm_head(&x3_norm, &lm_w, hidden, vocab, &mut q8k, &mut logits)
            .expect("lm_head");
        assert_eq!(logits.len(), vocab);
        for &v in &logits {
            assert!(v.abs() < 1e-3, "zero pipeline → zero logit, got {v}");
        }
    }

    #[test]
    fn rmsnorm_via_f32_norm_matches_fp16_path() {
        // Build an FP32 gamma byte buffer; convert to FP16 via the
        // helper; compare the rmsnorm result against feeding the same
        // gamma in directly as FP16 bits.
        let n = 8usize;
        let gamma_f32 = [1.0f32, 0.5, 2.0, -1.0, 0.25, 1.5, -0.75, 1.0];
        let mut gamma_bytes = Vec::with_capacity(n * 4);
        for v in &gamma_f32 {
            gamma_bytes.extend_from_slice(&v.to_le_bytes());
        }
        let mut gamma_fp16_via_helper = vec![0u16; n];
        f32_bytes_into_fp16_slice(&gamma_bytes, &mut gamma_fp16_via_helper).expect("ok");
        let gamma_fp16_direct: Vec<u16> = gamma_f32.iter().map(|&f| f32_to_f16(f)).collect();
        assert_eq!(gamma_fp16_via_helper, gamma_fp16_direct);

        // Both paths should produce identical rmsnorm output.
        let x: Vec<u16> = (1..=n as u32).map(|v| f32_to_f16(v as f32)).collect();
        let mut out_a = vec![0u16; n];
        let mut out_b = vec![0u16; n];
        rmsnorm(&x, &gamma_fp16_via_helper, 1e-6, &mut out_a).expect("a");
        rmsnorm(&x, &gamma_fp16_direct, 1e-6, &mut out_b).expect("b");
        for i in 0..n {
            let a = f16_to_f32(out_a[i]);
            let b = f16_to_f32(out_b[i]);
            assert!(
                (a - b).abs() < 1e-3,
                "idx {i}: helper={a}, direct={b}"
            );
        }
    }
}
