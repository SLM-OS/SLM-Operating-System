//! Transformer operator kernels (M4 of the SLM integration plan).
//! Forms the per-layer building blocks the M5 decoder composes.
//!
//! All ops follow the `(input: &[u16], weight/state: &..., output: &mut [u16])`
//! shape: borrowed FP16-as-bits inputs, borrowed-or-owned mutable outputs.
//! Numeric accumulators are FP32; storage is FP16. FP16 storage shrinks the
//! activation footprint to fit Qwen2.5-1.5B's ~230 MB KV cache; FP32
//! accumulators preserve numerical fidelity through long reduction chains
//! (RMS, attention softmax, etc.).
//!
//! Includes the simpler ops (RMSNorm, RoPE, FP16 embedding lookup, SiLU)
//! from M4.1 plus the matmul-heavy `gqa_decode_step`, `swiglu_mlp`,
//! `lm_head`, and `matmul_q4k_*` kernels from M4.2 — together the full
//! per-layer building blocks the M5 decoder composes.
//!
//! # Numeric conventions
//!
//! - Storage:    FP16 bit pattern as `u16` (stable Rust has no `f16` type).
//! - Accumulators: `f32`.
//! - Conversion: `crate::slm::gguf::{f16_to_f32, f32_to_f16}`.
//!
//! # `no_std`
//!
//! All ops are scalar and `no_std`. NEON acceleration is M4.3 / M9.

#![cfg(feature = "slm")]

use alloc::vec;
use alloc::vec::Vec;

use crate::inference::mathf::sqrtf;
use crate::inference::quant::{q8_k_byte_size, quantize_row_q8_k, vec_dot_q4_k_q8_k};
use crate::slm::gguf::{Q4_K_BLOCK_ELEMENTS, f16_to_f32, f32_to_f16, q4_k_byte_size};

// ---------------------------------------------------------------------------
// RMSNorm
// ---------------------------------------------------------------------------

/// Root Mean Square layer norm with per-channel learned scale (`gamma`).
///
/// `x` is one row of FP16 storage of length `n`; `gamma` is the learned
/// per-channel scale of the same length. `out` is the pre-allocated FP16
/// output of the same length. `eps` is a small number (Qwen2.5 uses 1e-6)
/// added inside the sqrt to avoid division by zero.
///
/// Computes:
///
/// ```text
/// rms_inv = 1 / sqrt(mean(x_i^2) + eps)
/// out_i   = x_i * rms_inv * gamma_i
/// ```
///
/// Accumulator is FP32. `mathf::sqrtf` is used (see `runtime/CLAUDE.md`
/// "mathf — scalar libm replacements" for why we don't call `libm::sqrtf`).
///
/// Returns `Some(())` on success, `None` if `x`, `gamma`, and `out` don't
/// all have the same length (or are empty).
pub fn rmsnorm(x: &[u16], gamma: &[u16], eps: f32, out: &mut [u16]) -> Option<()> {
    let n = x.len();
    if n == 0 || gamma.len() != n || out.len() != n {
        return None;
    }

    // Sum of squares in FP32 to avoid the precision loss that would
    // come from accumulating ~thousands of FP16 values.
    let mut acc_sq: f32 = 0.0;
    for &bits in x.iter() {
        let v = f16_to_f32(bits);
        acc_sq += v * v;
    }

    let mean_sq = acc_sq / (n as f32);
    let rms_inv = 1.0 / sqrtf(mean_sq + eps);

    for i in 0..n {
        let xv = f16_to_f32(x[i]);
        let gv = f16_to_f32(gamma[i]);
        out[i] = f32_to_f16(xv * rms_inv * gv);
    }

    Some(())
}

// ---------------------------------------------------------------------------
// RoPE
// ---------------------------------------------------------------------------

/// Precomputed RoPE cos/sin table for a given `head_dim` and `theta_base`.
///
/// Built once per session (head_dim is per-arch, theta_base comes from the
/// GGUF metadata `*.rope.freq_base`). The table stores cos/sin pairs as
/// `f32` since head_dim is small (~128) and the LUT footprint is tiny
/// even at long context: 128 dims × 4096 positions × 4 B ≈ 2 MB.
///
/// # Convention
///
/// This implementation uses the GPT-NeoX / Llama / Qwen2 pair-interleave
/// convention: pair `i` is `(vec[2i], vec[2i+1])`, **not** the alternate
/// `(vec[i], vec[i + head_dim/2])` half-split convention some reference
/// implementations use.
///
/// # Layout
///
/// `cos_sin` is a flat `f32` vector of length `max_pos * head_dim`.
/// For position `pos` and pair index `i` (i in `0..head_dim/2`):
///
/// ```text
/// cos_sin[pos * head_dim + 2*i]     = cos(pos * theta_i)
/// cos_sin[pos * head_dim + 2*i + 1] = sin(pos * theta_i)
/// ```
///
/// where `theta_i = theta_base ^ (-2 * i / head_dim)`.
pub struct RopeTable {
    /// Per-position cos/sin pairs. Layout: `[pos][i]` interleaved cos at
    /// even idx, sin at odd idx; total `max_pos * head_dim` entries.
    pub cos_sin: Vec<f32>,
    pub head_dim: usize,
    pub max_pos: usize,
}

impl RopeTable {
    /// Precompute cos and sin for every (position, pair) combination.
    ///
    /// `head_dim` must be even (RoPE rotates pairs); odd head_dim is
    /// rejected via an empty table to keep the API infallible.
    ///
    /// `head_dim * max_pos` is checked for `usize` overflow before
    /// allocation.
    pub fn new(head_dim: usize, max_pos: usize, theta_base: f32) -> Self {
        // Reject odd head_dim or zero dims by returning an empty table
        // — `apply` will then fall through with no-op behaviour.
        if head_dim == 0 || max_pos == 0 || head_dim % 2 != 0 {
            return Self {
                cos_sin: Vec::new(),
                head_dim,
                max_pos,
            };
        }

        let total = match head_dim.checked_mul(max_pos) {
            Some(v) => v,
            None => {
                return Self {
                    cos_sin: Vec::new(),
                    head_dim,
                    max_pos,
                };
            }
        };

        let mut cos_sin = vec![0.0f32; total];
        let half = head_dim / 2;

        for pos in 0..max_pos {
            for i in 0..half {
                // theta_i = theta_base ^ (-2*i / head_dim).
                let exponent = -2.0 * (i as f32) / (head_dim as f32);
                // libm::powf is allowed (mathf only replaces sqrtf/tanhf).
                let theta_i = libm::powf(theta_base, exponent);
                let angle = (pos as f32) * theta_i;
                // libm::cosf / sinf are allowed for the same reason.
                let c = libm::cosf(angle);
                let s = libm::sinf(angle);
                let base = pos * head_dim + 2 * i;
                cos_sin[base] = c;
                cos_sin[base + 1] = s;
            }
        }

        Self {
            cos_sin,
            head_dim,
            max_pos,
        }
    }

    /// Apply rotary embedding to `vec` in-place.
    ///
    /// `vec` is a single query or key vector of length `head_dim`; pairs
    /// `(vec[2i], vec[2i+1])` get rotated by the position's angle. FP16
    /// storage, FP32 trig already baked into the LUT.
    ///
    /// No-op when `pos >= max_pos`, `vec.len() != head_dim`, or the LUT
    /// is empty (degenerate `head_dim` / overflow).
    pub fn apply(&self, pos: usize, vec: &mut [u16]) {
        if self.cos_sin.is_empty()
            || pos >= self.max_pos
            || vec.len() != self.head_dim
            || self.head_dim % 2 != 0
        {
            return;
        }
        let half = self.head_dim / 2;
        let base = pos * self.head_dim;
        for i in 0..half {
            let c = self.cos_sin[base + 2 * i];
            let s = self.cos_sin[base + 2 * i + 1];
            let v0 = f16_to_f32(vec[2 * i]);
            let v1 = f16_to_f32(vec[2 * i + 1]);
            vec[2 * i] = f32_to_f16(v0 * c - v1 * s);
            vec[2 * i + 1] = f32_to_f16(v0 * s + v1 * c);
        }
    }
}

// ---------------------------------------------------------------------------
// Embedding lookup (FP16 path)
// ---------------------------------------------------------------------------

/// Gather one row from an FP16 embedding table.
///
/// `table` is the `vocab_size * embedding_dim` FP16 weight matrix in
/// row-major order. `token_id` selects which row. `out` is the
/// `embedding_dim`-length FP16 destination.
///
/// Returns `None` on any of:
///   - `embedding_dim == 0`
///   - `out.len() != embedding_dim`
///   - `table.len()` is not a multiple of `embedding_dim`
///   - `token_id` is out of range (`>= vocab_size`)
///   - row offset arithmetic overflows `usize`
///
/// # Note on Q4_K embeddings
///
/// For Qwen2.5-1.5B the embedding table is **Q4_K-quantized**, not FP16.
/// The Q4_K variant is M4.2 / M5 territory; this PR ships the FP16
/// reference for testing and the M5 GPU path's fallback. The function
/// is named with the `_fp16` suffix to make the storage format explicit.
pub fn embedding_lookup_fp16(
    table: &[u16],
    embedding_dim: usize,
    token_id: u32,
    out: &mut [u16],
) -> Option<()> {
    if embedding_dim == 0 || out.len() != embedding_dim {
        return None;
    }
    if table.len() % embedding_dim != 0 {
        return None;
    }
    let vocab_size = table.len() / embedding_dim;
    // `usize::try_from` keeps hygiene on 32-bit hosts even though
    // bare-metal targets are 64-bit.
    let tok = match usize::try_from(token_id) {
        Ok(v) => v,
        Err(_) => return None,
    };
    if tok >= vocab_size {
        return None;
    }

    let start = tok.checked_mul(embedding_dim)?;
    let end = start.checked_add(embedding_dim)?;
    if end > table.len() {
        return None;
    }

    out.copy_from_slice(&table[start..end]);
    Some(())
}

// ---------------------------------------------------------------------------
// SiLU activation
// ---------------------------------------------------------------------------

/// SiLU activation in-place: `x = x * sigmoid(x)`.
///
/// `sigmoid(x) = 1 / (1 + exp(-x))`. FP16 storage, FP32 internally.
///
/// `libm::expf` is allowed (only `sqrtf` and `tanhf` triggered the #141
/// f16 soften crash and were moved to `mathf`).
pub fn silu(x: &mut [u16]) {
    for slot in x.iter_mut() {
        let xf = f16_to_f32(*slot);
        let s = xf / (1.0 + libm::expf(-xf));
        *slot = f32_to_f16(s);
    }
}

/// SiLU with a separate output buffer, leaving `x` untouched.
///
/// Returns `None` when `x` and `out` differ in length.
pub fn silu_out(x: &[u16], out: &mut [u16]) -> Option<()> {
    if x.len() != out.len() {
        return None;
    }
    for (i, &bits) in x.iter().enumerate() {
        let xf = f16_to_f32(bits);
        let s = xf / (1.0 + libm::expf(-xf));
        out[i] = f32_to_f16(s);
    }
    Some(())
}

// ---------------------------------------------------------------------------
// Q4_K matmul (M4.2)
// ---------------------------------------------------------------------------

/// Single-row Q4_K-weight × FP16-activation matmul.
///
/// `weights` is one Q4_K-packed row of `cols` elements (`cols` must be a
/// multiple of 256 — Qwen2.5-1.5B uses {1536, 8960}). `acts_fp16` is the
/// FP16 activation row of `cols` elements. `q8k_scratch` is caller-owned
/// scratch of `q8_k_byte_size(cols)` bytes used to quantize the
/// activations into Q8_K before the dot product.
///
/// Routes through M3's `quantize_row_q8_k` + `vec_dot_q4_k_q8_k`.
///
/// Returns `None` on shape mismatch.
pub fn matmul_q4k_row(
    weights: &[u8],
    acts_fp16: &[u16],
    q8k_scratch: &mut [u8],
    cols: usize,
) -> Option<f32> {
    if cols == 0 || cols % Q4_K_BLOCK_ELEMENTS != 0 {
        return None;
    }
    if acts_fp16.len() != cols {
        return None;
    }
    if weights.len() != q4_k_byte_size(cols)? {
        return None;
    }
    if q8k_scratch.len() < q8_k_byte_size(cols)? {
        return None;
    }

    // Widen activations FP16 → FP32 once, on the stack-friendly path:
    // an alloc::Vec<f32> here. The M5 decoder will pre-allocate a
    // session-scoped scratch buffer to avoid this allocation per call.
    let mut acts_f32: Vec<f32> = Vec::with_capacity(cols);
    for &bits in acts_fp16.iter() {
        acts_f32.push(f16_to_f32(bits));
    }

    let q8k_len = q8_k_byte_size(cols)?;
    quantize_row_q8_k(&acts_f32, &mut q8k_scratch[..q8k_len])?;
    vec_dot_q4_k_q8_k(weights, &q8k_scratch[..q8k_len])
}

/// Multi-row Q4_K-weight × FP16-activation matmul.
///
/// `weights` is `rows * q4_k_byte_size(cols)` packed bytes laid out
/// row-major. `acts_fp16` is the FP16 input vector of `cols` elements
/// (shared across all output rows). `out_fp32` receives the per-row
/// dot products. `q8k_scratch` is reused across rows so the activation
/// quantization is paid once.
///
/// Returns `None` on shape mismatch.
///
/// `#[inline]` so the per-row `vec_dot` call site sees the row stride
/// + activation pointer at compile time and the quantize-once /
/// dot-many pattern can hoist the Q8_K conversion out of the loop.
#[inline]
pub fn matmul_q4k_rows(
    weights: &[u8],
    rows: usize,
    cols: usize,
    acts_fp16: &[u16],
    q8k_scratch: &mut [u8],
    out_fp32: &mut [f32],
) -> Option<()> {
    if cols == 0 || cols % Q4_K_BLOCK_ELEMENTS != 0 {
        return None;
    }
    if acts_fp16.len() != cols || out_fp32.len() != rows {
        return None;
    }
    let row_bytes = q4_k_byte_size(cols)?;
    let total = row_bytes.checked_mul(rows)?;
    if weights.len() != total {
        return None;
    }
    let q8k_len = q8_k_byte_size(cols)?;
    if q8k_scratch.len() < q8k_len {
        return None;
    }

    // Quantize the shared activations once.
    let mut acts_f32: Vec<f32> = Vec::with_capacity(cols);
    for &bits in acts_fp16.iter() {
        acts_f32.push(f16_to_f32(bits));
    }
    quantize_row_q8_k(&acts_f32, &mut q8k_scratch[..q8k_len])?;

    // Loop over weight rows, calling the dot kernel directly so we
    // skip the per-row activation re-quantization that
    // `matmul_q4k_row` would otherwise pay.
    for r in 0..rows {
        let start = r.checked_mul(row_bytes)?;
        let end = start.checked_add(row_bytes)?;
        let row = &weights[start..end];
        out_fp32[r] = vec_dot_q4_k_q8_k(row, &q8k_scratch[..q8k_len])?;
    }
    Some(())
}

/// Batched Q4_K matmul: `[batch × cols]` activations × `[rows × cols]`
/// weights → `[batch × rows]` output (row-major in both).
///
/// Used by [`crate::slm::forward::forward_batch`] to amortize the
/// dominant prefill cost — the weight stream from DRAM. With
/// `matmul_q4k_rows`, every prompt token re-reads ~1 GB of Qwen2.5
/// weights; here, each weight row is read once per call and dot-
/// producted against all `batch` activation rows in the inner loop,
/// keeping it hot in L1 / L2.
///
/// **Layout.** `acts_fp16` is `batch * cols` u16s, contiguous per
/// activation row (row 0: `[0..cols]`, row 1: `[cols..2*cols]`, …).
/// `out_fp32` is `batch * rows` f32s, contiguous per output row
/// (row b's results: `[b*rows..(b+1)*rows]`). `q8k_scratch` is
/// `batch * q8_k_byte_size(cols)` bytes — every activation row gets
/// quantized into Q8_K once, up front, so the inner loop is pure
/// `vec_dot_q4_k_q8_k`.
///
/// **Why this layout for `out`.** A `[batch × rows]` row-major
/// output is what every downstream op wants (residual add, RMSNorm,
/// the next per-token RoPE / attention). A column-major
/// `[rows × batch]` layout would let the inner loop write
/// contiguously into one weight row's slot but would need a
/// transpose before any per-token op — strictly worse.
///
/// **Why innermost is `for b in 0..batch`.** Each weight row
/// (`row_bytes` bytes — 144 B for Q4_K with cols=256) is loaded
/// once and `vec_dot`-applied against `batch` Q8_K activation
/// rows. For Qwen2.5-1.5B (cols=1536, row_bytes=864), the weight
/// row stays hot across all `batch` SDOT inner loops; the Q8_K
/// activation rows (also ~292 B for cols=256, ~1.7 KB for
/// cols=1536) live in cache for the duration. This is the whole
/// point — bandwidth is amortized `batch×`.
///
/// Bit-equal to calling [`matmul_q4k_rows`] `batch` times on each
/// activation row. `B=1` is identical to `matmul_q4k_rows` (modulo
/// one extra outer-loop iteration).
///
/// Returns `None` on shape mismatch.
#[inline]
pub fn matmul_q4k_rows_batch(
    weights: &[u8],
    rows: usize,
    cols: usize,
    batch: usize,
    acts_fp16: &[u16],
    q8k_scratch: &mut [u8],
    out_fp32: &mut [f32],
) -> Option<()> {
    if cols == 0 || cols % Q4_K_BLOCK_ELEMENTS != 0 || batch == 0 || rows == 0 {
        return None;
    }
    if acts_fp16.len() != batch.checked_mul(cols)? {
        return None;
    }
    if out_fp32.len() != batch.checked_mul(rows)? {
        return None;
    }
    let row_bytes = q4_k_byte_size(cols)?;
    if weights.len() != row_bytes.checked_mul(rows)? {
        return None;
    }
    let q8k_len = q8_k_byte_size(cols)?;
    let q8k_total = q8k_len.checked_mul(batch)?;
    if q8k_scratch.len() < q8k_total {
        return None;
    }

    // Quantize all `batch` activation rows up front into a packed
    // [batch × q8k_len] buffer. After this loop the inner kernel is
    // pure vec_dot, no per-row F32 setup needed.
    let mut acts_f32: Vec<f32> = vec![0.0f32; cols];
    for b in 0..batch {
        let act_start = b.checked_mul(cols)?;
        for i in 0..cols {
            acts_f32[i] = f16_to_f32(acts_fp16[act_start + i]);
        }
        let q_start = b.checked_mul(q8k_len)?;
        let q_end = q_start.checked_add(q8k_len)?;
        quantize_row_q8_k(&acts_f32, &mut q8k_scratch[q_start..q_end])?;
    }

    // Inner-loop order: weight row outer, batch inner. Each weight
    // row is loaded once into cache, then dot-producted against
    // `batch` Q8_K activation rows before moving to the next weight
    // row. Output is [batch × rows] row-major, so the write to
    // `out_fp32[b * rows + r]` is the right slot.
    for r in 0..rows {
        let w_start = r.checked_mul(row_bytes)?;
        let w_end = w_start.checked_add(row_bytes)?;
        let w_row = &weights[w_start..w_end];
        for b in 0..batch {
            let q_start = b.checked_mul(q8k_len)?;
            let q_end = q_start.checked_add(q8k_len)?;
            let dot = vec_dot_q4_k_q8_k(w_row, &q8k_scratch[q_start..q_end])?;
            out_fp32[b.checked_mul(rows)?.checked_add(r)?] = dot;
        }
    }
    Some(())
}

/// Dispatching multi-row matmul over any supported GGML quant type.
///
/// Real-world Q4_K_M GGUFs mix quant types within a single file —
/// Q4_K dominates, with Q6_K sprinkled on the LM head and a handful of
/// "important" weights, plus Q5_0 / Q8_0 on legacy-quant SmolLM
/// builds. This function is the single dispatch surface the M5 forward
/// pass uses; it picks the optimized vec_dot path for Q4_K and falls
/// back to a per-row "dequantize then F32 dot product" loop for other
/// types.
///
/// `weights` is `rows × quant_row_bytes(quant_type, cols)` packed
/// bytes laid out row-major. `acts_fp16` is the FP16 input vector
/// of `cols` elements (shared across all output rows). `q8k_scratch`
/// is reused across rows by the Q4_K fast path; ignored on other
/// types but still required as a parameter so callers can keep one
/// allocation. `dequant_scratch` holds one dequantized row of FP32
/// weights for the fallback path; sized at the largest `cols` the
/// caller will ever pass. `out_fp32` receives the per-row dot
/// products.
///
/// Returns `None` on shape mismatch or unsupported quant type.
///
/// `acts_f32_scratch` is caller-owned scratch for the FP32-widened
/// activations the per-row fallback dot product reads from. It must
/// have length ≥ `cols`. Pre-allocated by [`crate::slm::forward::ForwardScratch`]
/// so the per-token decode loop is allocation-free; the Q4_K fast
/// path doesn't read it.
pub fn matmul_quant_rows(
    quant_type: crate::slm::gguf::GgmlType,
    weights: &[u8],
    rows: usize,
    cols: usize,
    acts_fp16: &[u16],
    q8k_scratch: &mut [u8],
    dequant_scratch: &mut [f32],
    acts_f32_scratch: &mut [f32],
    out_fp32: &mut [f32],
) -> Option<()> {
    use crate::inference::quant::{dequantize_row_any, quant_row_bytes};
    use crate::slm::gguf::GgmlType;

    if cols == 0 || acts_fp16.len() != cols || out_fp32.len() != rows {
        return None;
    }

    // Q4_K stays on the optimized vec_dot path when `cols` is a clean
    // multiple of 256 (Qwen2.5-1.5B's 1536-element rows qualify; the
    // M5.3.x synthetic fixture qualifies). SmolLM2's `hidden = 576`
    // does NOT — `matmul_q4k_rows` enforces alignment and would
    // reject. Fall through to the per-row dequant path in that case.
    if quant_type == GgmlType::Q4_K && cols % Q4_K_BLOCK_ELEMENTS == 0 {
        return matmul_q4k_rows(weights, rows, cols, acts_fp16, q8k_scratch, out_fp32);
    }

    // Generic fallback: per-row dequantize to F32, then dot product
    // with the FP16-widened activations. Slower than Q4_K's vec_dot
    // but correct for any supported quant type. Used for Q6_K
    // (LM head + ~29 "important" tensors in Qwen2.5 Q4_K_M), Q5_0
    // (SmolLM attention), Q8_0 (SmolLM token_embd + V projection),
    // F32, F16.
    let row_bytes = quant_row_bytes(quant_type, cols)?;
    let total = row_bytes.checked_mul(rows)?;
    if weights.len() != total {
        return None;
    }

    // For Q4_K (and other K-quants with super-block padding), the
    // stored row may be larger than `cols` floats: `q4_k_byte_size`
    // pads partial trailing super-blocks out to the next 256-element
    // boundary. SmolLM2's hidden dim 576 is NOT divisible by 256,
    // so a 576-element Q4_K row is stored as 3 super-blocks = 768
    // elements with 192 padding. The dequant kernel writes the full
    // padded count; we only dot-product the first `cols` of those.
    let n_per_row = match quant_type {
        GgmlType::Q4_K => {
            let nb = row_bytes / 144;
            nb.checked_mul(256)?
        }
        // Q6_K rows aren't padded — its byte-size helper rejects
        // non-256-aligned counts upstream, so n_per_row == cols.
        GgmlType::Q6_K | GgmlType::Q5_0 | GgmlType::Q8_0 | GgmlType::F32 | GgmlType::F16 => cols,
        _ => return None,
    };
    // n_per_row >= cols by construction in every branch above (Q4_K's
    // ceil-rounded count, identity for the rest); the assert documents
    // the invariant for future maintainers without paying a release-
    // build branch.
    debug_assert!(n_per_row >= cols, "n_per_row {} < cols {}", n_per_row, cols);
    if dequant_scratch.len() < n_per_row || acts_f32_scratch.len() < cols {
        return None;
    }

    // Widen activations once into the caller-supplied scratch. Pre-
    // this refactor, this allocated a fresh `Vec<f32>` of size `cols`
    // per call — for a 30-layer model with ~7 matmuls per layer that
    // was ~211 heap allocations per token.
    //
    // Only `acts_f32_scratch[..cols]` is overwritten; bytes past
    // `cols` keep stale data from the previous call. That's safe
    // because the dot-product loop below reads `[..cols]` only —
    // skipping the rest avoids a memset on every matmul.
    for (i, &bits) in acts_fp16.iter().enumerate() {
        acts_f32_scratch[i] = f16_to_f32(bits);
    }

    for r in 0..rows {
        let start = r.checked_mul(row_bytes)?;
        let end = start.checked_add(row_bytes)?;
        let row = &weights[start..end];
        let n = dequantize_row_any(quant_type, row, &mut dequant_scratch[..n_per_row])?;
        if n < cols {
            return None;
        }
        let mut acc: f32 = 0.0;
        for i in 0..cols {
            acc += dequant_scratch[i] * acts_f32_scratch[i];
        }
        out_fp32[r] = acc;
    }
    Some(())
}

/// Batched dispatch wrapper around [`matmul_q4k_rows_batch`].
///
/// `acts_fp16` is `[batch × cols]` row-major; `out_fp32` is
/// `[batch × rows]` row-major. For Q4_K weights with `cols` a
/// multiple of 256, the batched kernel is invoked directly. For any
/// other quant type, falls back to `batch` independent
/// [`matmul_quant_rows`] calls — losing batching gain but keeping the
/// dispatch surface uniform so callers don't have to special-case
/// quant type. The fallback is what Qwen2.5-1.5B's LM head (Q6_K)
/// goes through, but the LM head runs once per `forward_batch` call
/// (only the last batch row's logits matter for sampling), so the
/// fallback's cost is paid only once per call regardless of batch
/// size.
///
/// `q8k_scratch` must be ≥ `batch * q8_k_byte_size(cols)` for the
/// Q4_K fast path; the fallback only needs ≥ `q8_k_byte_size(cols)`
/// (it reuses the front of the buffer per call).
pub fn matmul_quant_rows_batch(
    quant_type: crate::slm::gguf::GgmlType,
    weights: &[u8],
    rows: usize,
    cols: usize,
    batch: usize,
    acts_fp16: &[u16],
    q8k_scratch: &mut [u8],
    dequant_scratch: &mut [f32],
    acts_f32_scratch: &mut [f32],
    out_fp32: &mut [f32],
) -> Option<()> {
    use crate::slm::gguf::GgmlType;

    if batch == 0 || cols == 0 || rows == 0 {
        return None;
    }
    if acts_fp16.len() != batch.checked_mul(cols)?
        || out_fp32.len() != batch.checked_mul(rows)?
    {
        return None;
    }
    if quant_type == GgmlType::Q4_K && cols % Q4_K_BLOCK_ELEMENTS == 0 {
        return matmul_q4k_rows_batch(
            weights, rows, cols, batch, acts_fp16, q8k_scratch, out_fp32,
        );
    }
    for b in 0..batch {
        let acts = &acts_fp16[b.checked_mul(cols)?..(b + 1).checked_mul(cols)?];
        let out = &mut out_fp32[b.checked_mul(rows)?..(b + 1).checked_mul(rows)?];
        matmul_quant_rows(
            quant_type,
            weights,
            rows,
            cols,
            acts,
            q8k_scratch,
            dequant_scratch,
            acts_f32_scratch,
            out,
        )?;
    }
    Some(())
}

// ---------------------------------------------------------------------------
// Grouped-Query Attention (decode step)
// ---------------------------------------------------------------------------

/// One-step grouped-query attention for the decode loop.
///
/// At decode time the model has just produced one new token. For that
/// position we have:
/// - one query row per attention head (`n_head_q × head_dim`),
/// - the cumulative key/value cache covering positions `0..seq_len`
///   (`seq_len × n_head_kv × head_dim` for each of K and V),
/// - we want the attention output (`n_head_q × head_dim`) that the
///   subsequent output projection consumes.
///
/// **GQA pairing:** every `g = n_head_q / n_head_kv` query heads share one
/// KV head. Qwen2.5-1.5B has `g = 6` (12 query heads, 2 KV heads).
///
/// **Causal mask:** implicit. `seq_len` is the count of past+current
/// positions to attend to; future positions simply aren't passed in.
///
/// `scratch_logits` is a per-head FP32 scratch buffer of length
/// `seq_len`, reused across heads.
///
/// Returns `None` on shape mismatch or when `n_head_q % n_head_kv != 0`.
pub fn gqa_decode_step(
    q: &[u16],
    k: &[u16],
    v: &[u16],
    n_head_q: usize,
    n_head_kv: usize,
    head_dim: usize,
    seq_len: usize,
    scratch_logits: &mut [f32],
    out: &mut [u16],
) -> Option<()> {
    if head_dim == 0
        || n_head_q == 0
        || n_head_kv == 0
        || seq_len == 0
        || n_head_q % n_head_kv != 0
    {
        return None;
    }
    let q_len = n_head_q.checked_mul(head_dim)?;
    let kv_per_pos = n_head_kv.checked_mul(head_dim)?;
    let kv_len = seq_len.checked_mul(kv_per_pos)?;
    if q.len() != q_len || k.len() != kv_len || v.len() != kv_len || out.len() != q_len {
        return None;
    }
    if scratch_logits.len() < seq_len {
        return None;
    }

    let group = n_head_q / n_head_kv;
    let inv_sqrt_head = 1.0 / sqrtf(head_dim as f32);

    for hq in 0..n_head_q {
        let hkv = hq / group;
        let q_off = hq.checked_mul(head_dim)?;
        let q_row = &q[q_off..q_off + head_dim];

        // 1. Logits[t] = (q · k_t) / sqrt(head_dim) for t in 0..seq_len.
        // The outer-loop multiplies above use `checked_mul`. The
        // inner indices below cannot overflow given `kv_len = seq_len
        // * kv_per_pos` already passed `checked_mul`, but pin that
        // invariant with a `debug_assert!` so a future refactor that
        // weakens the outer bound surfaces here, not in production.
        let mut max_logit = f32::NEG_INFINITY;
        for t in 0..seq_len {
            let k_off = t * kv_per_pos + hkv * head_dim;
            debug_assert!(k_off + head_dim <= k.len());
            let mut acc: f32 = 0.0;
            for d in 0..head_dim {
                acc += f16_to_f32(q_row[d]) * f16_to_f32(k[k_off + d]);
            }
            let logit = acc * inv_sqrt_head;
            scratch_logits[t] = logit;
            if logit > max_logit {
                max_logit = logit;
            }
        }

        // 2. Softmax (numerically stable: subtract max, exp, normalize).
        let mut denom: f32 = 0.0;
        for t in 0..seq_len {
            let e = libm::expf(scratch_logits[t] - max_logit);
            scratch_logits[t] = e;
            denom += e;
        }
        // `denom` is strictly positive after at least one finite
        // exp() call, but keep the guard for the degenerate case.
        let inv_denom = if denom > 0.0 { 1.0 / denom } else { 0.0 };
        for t in 0..seq_len {
            scratch_logits[t] *= inv_denom;
        }

        // 3. out[hq] = sum_t softmax[t] * v[t, hkv].
        let out_off = hq * head_dim;
        debug_assert!(out_off + head_dim <= out.len());
        for d in 0..head_dim {
            let mut acc: f32 = 0.0;
            for t in 0..seq_len {
                let v_off = t * kv_per_pos + hkv * head_dim;
                debug_assert!(v_off + head_dim <= v.len());
                acc += scratch_logits[t] * f16_to_f32(v[v_off + d]);
            }
            out[out_off + d] = f32_to_f16(acc);
        }
    }

    Some(())
}

// ---------------------------------------------------------------------------
// SwiGLU MLP block
// ---------------------------------------------------------------------------

/// SwiGLU MLP block: `down(silu(gate(x)) * up(x))`.
///
/// Three Q4_K matmuls plus an element-wise SiLU and an element-wise
/// multiply. `gate_w` and `up_w` project from `hidden_size` to
/// `intermediate_size`; `down_w` projects back. `gate_scratch` and
/// `up_scratch` hold the intermediate FP16 vectors. `q8k_scratch`
/// must be `q8_k_byte_size(max(hidden_size, intermediate_size))`
/// bytes.
///
/// Returns `None` on shape mismatch.
pub fn swiglu_mlp(
    x: &[u16],
    gate_w: &[u8],
    up_w: &[u8],
    down_w: &[u8],
    hidden_size: usize,
    intermediate_size: usize,
    gate_scratch: &mut [u16],
    up_scratch: &mut [u16],
    q8k_scratch: &mut [u8],
    out: &mut [u16],
) -> Option<()> {
    // Q4_K-only legacy signature kept for the existing test fixture.
    // Real models use the dispatching variant below. Both scratch
    // buffers are allocated locally because the legacy callers don't
    // reach this far in the hot path — fixture tests only. Pre-size
    // to `max(hidden, intermediate)` so `swiglu_mlp_q`'s resize-up
    // path doesn't reallocate during fixture runs.
    let max_cols = hidden_size.max(intermediate_size);
    let mut tmp_dequant: Vec<f32> = Vec::with_capacity(max_cols);
    let mut tmp_acts_f32: Vec<f32> = Vec::with_capacity(max_cols);
    swiglu_mlp_q(
        x,
        crate::slm::gguf::GgmlType::Q4_K,
        gate_w,
        crate::slm::gguf::GgmlType::Q4_K,
        up_w,
        crate::slm::gguf::GgmlType::Q4_K,
        down_w,
        hidden_size,
        intermediate_size,
        gate_scratch,
        up_scratch,
        q8k_scratch,
        &mut tmp_dequant,
        &mut tmp_acts_f32,
        out,
    )
}

/// SwiGLU MLP block accepting per-weight quant types.
///
/// `gate_w`/`up_w`/`down_w` may each carry a different GGML quant
/// (Qwen2.5 Q4_K_M and SmolLM both mix Q4_K and Q6_K within one
/// FFN block). `dequant_scratch` is shared scratch for the
/// dispatcher's per-row dequantization fallback; sized at the
/// largest of `hidden_size` and `intermediate_size`. `acts_f32_scratch`
/// is the FP32-widened activation buffer that `matmul_quant_rows`
/// reads (caller-owned to skip a heap alloc per call).
pub fn swiglu_mlp_q(
    x: &[u16],
    gate_quant: crate::slm::gguf::GgmlType,
    gate_w: &[u8],
    up_quant: crate::slm::gguf::GgmlType,
    up_w: &[u8],
    down_quant: crate::slm::gguf::GgmlType,
    down_w: &[u8],
    hidden_size: usize,
    intermediate_size: usize,
    gate_scratch: &mut [u16],
    up_scratch: &mut [u16],
    q8k_scratch: &mut [u8],
    dequant_scratch: &mut Vec<f32>,
    acts_f32_scratch: &mut Vec<f32>,
    out: &mut [u16],
) -> Option<()> {
    // 256-block alignment was enforced when the only supported quant
    // was Q4_K. With the multi-quant dispatcher in `matmul_quant_rows`,
    // partial-block rows are handled inline (SmolLM2's hidden=576 is
    // a multiple of Q5_0/Q8_0's 32 but not Q4_K's 256).
    if hidden_size == 0 || intermediate_size == 0 {
        return None;
    }
    if x.len() != hidden_size
        || out.len() != hidden_size
        || gate_scratch.len() != intermediate_size
        || up_scratch.len() != intermediate_size
    {
        return None;
    }

    // Make sure the dequant scratch can hold the largest inner-row
    // we'll dequantize. Both gate/up consume `hidden_size`-element
    // rows; down consumes `intermediate_size`. Same sizing for
    // `acts_f32_scratch` since the matmul widens activations of
    // length `cols` (= hidden for gate/up, = intermediate for down).
    let max_cols = hidden_size.max(intermediate_size);
    if dequant_scratch.len() < max_cols {
        dequant_scratch.resize(max_cols, 0.0);
    }
    if acts_f32_scratch.len() < max_cols {
        acts_f32_scratch.resize(max_cols, 0.0);
    }

    // gate = matmul(x, gate_w) [intermediate_size]
    // up   = matmul(x, up_w)   [intermediate_size]
    let mut tmp_f32: Vec<f32> = vec![0.0; intermediate_size];
    matmul_quant_rows(
        gate_quant,
        gate_w,
        intermediate_size,
        hidden_size,
        x,
        q8k_scratch,
        dequant_scratch,
        acts_f32_scratch,
        &mut tmp_f32,
    )?;
    for i in 0..intermediate_size {
        gate_scratch[i] = f32_to_f16(tmp_f32[i]);
    }
    matmul_quant_rows(
        up_quant,
        up_w,
        intermediate_size,
        hidden_size,
        x,
        q8k_scratch,
        dequant_scratch,
        acts_f32_scratch,
        &mut tmp_f32,
    )?;
    for i in 0..intermediate_size {
        up_scratch[i] = f32_to_f16(tmp_f32[i]);
    }

    // gate ← silu(gate); gate ← gate ⊙ up
    silu(gate_scratch);
    for i in 0..intermediate_size {
        let gv = f16_to_f32(gate_scratch[i]);
        let uv = f16_to_f32(up_scratch[i]);
        gate_scratch[i] = f32_to_f16(gv * uv);
    }

    // out = matmul(gate, down_w) [hidden_size]
    let mut out_f32: Vec<f32> = vec![0.0; hidden_size];
    matmul_quant_rows(
        down_quant,
        down_w,
        hidden_size,
        intermediate_size,
        gate_scratch,
        q8k_scratch,
        dequant_scratch,
        acts_f32_scratch,
        &mut out_f32,
    )?;
    for i in 0..hidden_size {
        out[i] = f32_to_f16(out_f32[i]);
    }
    Some(())
}

// ---------------------------------------------------------------------------
// LM head
// ---------------------------------------------------------------------------

/// Final FP16 hidden → FP32 logits projection over the vocab.
///
/// `x` is the post-final-RMSNorm hidden state of `hidden_size` floats
/// (FP16). `weight` is the Q4_K-packed `[vocab_size, hidden_size]`
/// matrix. `q8k_scratch` is `q8_k_byte_size(hidden_size)` bytes.
/// `logits` receives `vocab_size` FP32 outputs.
///
/// Returns `None` on shape mismatch.
pub fn lm_head(
    x: &[u16],
    weight: &[u8],
    hidden_size: usize,
    vocab_size: usize,
    q8k_scratch: &mut [u8],
    logits: &mut [f32],
) -> Option<()> {
    // Q4_K-only legacy signature kept for the existing test fixture.
    // Real LM heads in Qwen2.5 Q4_K_M and SmolLM are Q6_K — call the
    // dispatching variant directly from forward.rs. Pre-size both
    // scratch buffers to `hidden_size` (the matmul inner dim) so
    // `lm_head_q`'s resize-up path doesn't reallocate.
    let mut tmp_dequant: Vec<f32> = Vec::with_capacity(hidden_size);
    let mut tmp_acts_f32: Vec<f32> = Vec::with_capacity(hidden_size);
    lm_head_q(
        x,
        crate::slm::gguf::GgmlType::Q4_K,
        weight,
        hidden_size,
        vocab_size,
        q8k_scratch,
        &mut tmp_dequant,
        &mut tmp_acts_f32,
        logits,
    )
}

/// LM-head projection accepting an explicit weight quant type.
///
/// LM head ("output.weight") in any K_M-tier GGUF is typically Q6_K
/// for accuracy; some variants ship F16. Tied embeddings (no
/// dedicated `output.weight`) reuse `token_embd.weight` which can be
/// Q4_K, Q6_K, or Q8_0 depending on quant tier.
pub fn lm_head_q(
    x: &[u16],
    weight_quant: crate::slm::gguf::GgmlType,
    weight: &[u8],
    hidden_size: usize,
    vocab_size: usize,
    q8k_scratch: &mut [u8],
    dequant_scratch: &mut Vec<f32>,
    acts_f32_scratch: &mut Vec<f32>,
    logits: &mut [f32],
) -> Option<()> {
    if hidden_size == 0 || vocab_size == 0 {
        return None;
    }
    if dequant_scratch.len() < hidden_size {
        dequant_scratch.resize(hidden_size, 0.0);
    }
    if acts_f32_scratch.len() < hidden_size {
        acts_f32_scratch.resize(hidden_size, 0.0);
    }
    matmul_quant_rows(
        weight_quant,
        weight,
        vocab_size,
        hidden_size,
        x,
        q8k_scratch,
        dequant_scratch,
        acts_f32_scratch,
        logits,
    )
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    /// Helper: build an FP16 buffer from an `f32` slice.
    fn from_f32(values: &[f32]) -> Vec<u16> {
        values.iter().copied().map(f32_to_f16).collect()
    }

    /// Helper: convert an FP16 buffer back to FP32 for assertions.
    fn to_f32(values: &[u16]) -> Vec<f32> {
        values.iter().copied().map(f16_to_f32).collect()
    }

    fn approx_eq(a: f32, b: f32, eps: f32) -> bool {
        (a - b).abs() <= eps
    }

    // ---- RMSNorm ------------------------------------------------------

    #[test]
    fn rmsnorm_zero_input_returns_zero() {
        let x = from_f32(&[0.0, 0.0, 0.0, 0.0]);
        let g = from_f32(&[1.0, 2.0, 3.0, 4.0]);
        let mut out = vec![0u16; 4];
        // rms_inv = 1/sqrt(0 + eps), but each x_i is 0 so the product
        // x_i * rms_inv * g_i is 0 regardless of gamma.
        rmsnorm(&x, &g, 1e-6, &mut out).expect("shape ok");
        for v in to_f32(&out) {
            assert!(approx_eq(v, 0.0, 1e-6), "expected 0, got {}", v);
        }
    }

    #[test]
    fn rmsnorm_unit_input_with_unit_gamma_is_one() {
        // rms = sqrt(mean(1)) = 1, rms_inv = 1, so out_i = 1 * 1 * 1 = 1.
        let x = from_f32(&[1.0; 8]);
        let g = from_f32(&[1.0; 8]);
        let mut out = vec![0u16; 8];
        rmsnorm(&x, &g, 1e-6, &mut out).expect("shape ok");
        for v in to_f32(&out) {
            // FP16 epsilon is ~1e-3; eps=1e-6 inside the sqrt nudges
            // the result very slightly below 1.0.
            assert!(approx_eq(v, 1.0, 2e-3), "expected 1, got {}", v);
        }
    }

    #[test]
    fn rmsnorm_known_vector() {
        // Input [1, 2, 3, 4]. mean(x^2) = (1+4+9+16)/4 = 7.5.
        // rms = sqrt(7.5) ≈ 2.7386. rms_inv ≈ 0.3651.
        // With gamma all-ones, out ≈ [0.3651, 0.7303, 1.0954, 1.4606].
        let x = from_f32(&[1.0, 2.0, 3.0, 4.0]);
        let g = from_f32(&[1.0, 1.0, 1.0, 1.0]);
        let mut out = vec![0u16; 4];
        rmsnorm(&x, &g, 1e-6, &mut out).expect("shape ok");
        let result = to_f32(&out);
        let expected = [0.3651f32, 0.7303, 1.0954, 1.4606];
        for (i, (&got, &want)) in result.iter().zip(expected.iter()).enumerate() {
            // 1e-2 tolerance per the spec; FP16 storage is the dominant
            // error term here.
            assert!(
                approx_eq(got, want, 1e-2),
                "index {}: got {}, want {}",
                i,
                got,
                want
            );
        }
    }

    #[test]
    fn rmsnorm_rejects_shape_mismatch() {
        let x = from_f32(&[1.0, 2.0, 3.0, 4.0]);
        let g = from_f32(&[1.0, 1.0, 1.0]); // wrong length
        let mut out = vec![0u16; 4];
        assert!(rmsnorm(&x, &g, 1e-6, &mut out).is_none());
    }

    // ---- RoPE ---------------------------------------------------------

    #[test]
    fn rope_table_position_zero_is_identity() {
        // At pos=0, every angle is 0, so every cos=1 and sin=0.
        // Applying RoPE leaves the vector unchanged.
        let head_dim = 8usize;
        let table = RopeTable::new(head_dim, 16, 10000.0);
        let original = from_f32(&[0.5, -1.5, 2.0, -0.25, 0.75, 1.0, -0.5, 0.125]);
        let mut vec_buf = original.clone();
        table.apply(0, &mut vec_buf);
        // Compare in FP32 — bit-for-bit equality holds here too, but
        // the FP32 path is the one the spec gives a tolerance for.
        let before = to_f32(&original);
        let after = to_f32(&vec_buf);
        for (i, (&a, &b)) in before.iter().zip(after.iter()).enumerate() {
            assert!(
                approx_eq(a, b, 1e-3),
                "index {}: before {} after {}",
                i,
                a,
                b
            );
        }
    }

    #[test]
    fn rope_norm_preserved() {
        // RoPE is a rotation: ||apply(pos, v)|| == ||v||.
        let head_dim = 8usize;
        let table = RopeTable::new(head_dim, 32, 10000.0);
        let original = from_f32(&[0.5, -1.5, 2.0, -0.25, 0.75, 1.0, -0.5, 0.125]);
        let norm_before: f32 = to_f32(&original).iter().map(|x| x * x).sum::<f32>();

        for &pos in &[0usize, 1, 5, 17, 31] {
            let mut v = original.clone();
            table.apply(pos, &mut v);
            let norm_after: f32 = to_f32(&v).iter().map(|x| x * x).sum::<f32>();
            // FP16 storage gives ~1e-3 relative error; the squared norm
            // amplifies that by roughly 2x.
            let rel = (norm_after - norm_before).abs() / norm_before;
            assert!(rel < 5e-3, "pos {}: norm drift {} (rel)", pos, rel);
        }
    }

    #[test]
    fn rope_table_rejects_odd_head_dim() {
        let table = RopeTable::new(7, 16, 10000.0);
        assert!(table.cos_sin.is_empty(), "odd head_dim should yield empty LUT");
        // apply() with mismatched head_dim is a no-op.
        let mut v = from_f32(&[1.0; 7]);
        table.apply(0, &mut v);
        // Buffer should be untouched.
        for &b in &v {
            assert_eq!(f16_to_f32(b), 1.0);
        }
    }

    // ---- Embedding lookup --------------------------------------------

    #[test]
    fn embedding_lookup_returns_correct_row() {
        // 8x4 synthetic table: row r is [10*r, 10*r+1, 10*r+2, 10*r+3].
        let mut table_f32 = Vec::with_capacity(8 * 4);
        for r in 0..8u32 {
            for c in 0..4u32 {
                table_f32.push((10 * r + c) as f32);
            }
        }
        let table = from_f32(&table_f32);
        let mut out = vec![0u16; 4];
        embedding_lookup_fp16(&table, 4, 5, &mut out).expect("in range");
        let got = to_f32(&out);
        let want = [50.0, 51.0, 52.0, 53.0];
        for (i, (&a, &b)) in got.iter().zip(want.iter()).enumerate() {
            assert!(approx_eq(a, b, 1e-1), "idx {}: got {} want {}", i, a, b);
        }
    }

    #[test]
    fn embedding_lookup_rejects_oob_token() {
        let table = from_f32(&[0.0; 8 * 4]);
        let mut out = vec![0u16; 4];
        // vocab_size = 8, so token_id == 8 is out of range.
        assert!(embedding_lookup_fp16(&table, 4, 8, &mut out).is_none());
        // And anything beyond.
        assert!(embedding_lookup_fp16(&table, 4, 100, &mut out).is_none());
    }

    #[test]
    fn embedding_lookup_rejects_bad_shape() {
        let table = from_f32(&[0.0; 8 * 4]);
        let mut out = vec![0u16; 3]; // wrong length
        assert!(embedding_lookup_fp16(&table, 4, 0, &mut out).is_none());

        let mut out_ok = vec![0u16; 4];
        // Table not a multiple of embedding_dim.
        let bad_table = from_f32(&[0.0; 31]);
        assert!(embedding_lookup_fp16(&bad_table, 4, 0, &mut out_ok).is_none());
    }

    // ---- SiLU --------------------------------------------------------

    #[test]
    fn silu_zero_returns_zero() {
        let mut buf = from_f32(&[0.0]);
        silu(&mut buf);
        assert!(approx_eq(f16_to_f32(buf[0]), 0.0, 1e-4));
    }

    #[test]
    fn silu_known_value() {
        // silu(1.0) = 1.0 * sigmoid(1.0) = 1.0 / (1 + e^-1) ≈ 0.7311.
        let mut buf = from_f32(&[1.0]);
        silu(&mut buf);
        let got = f16_to_f32(buf[0]);
        assert!(
            approx_eq(got, 0.7311, 1e-2),
            "silu(1.0) ≈ 0.7311, got {}",
            got
        );
    }

    #[test]
    fn silu_negative_pulls_toward_zero() {
        // silu(-2.0) is negative but |silu(-x)| < |x| for x > 0.
        // silu(-2.0) ≈ -2.0 * sigmoid(-2.0) ≈ -2.0 * 0.1192 ≈ -0.2384.
        let mut buf = from_f32(&[-2.0]);
        silu(&mut buf);
        let got = f16_to_f32(buf[0]);
        assert!(got < 0.0, "expected negative, got {}", got);
        assert!(got.abs() < 2.0, "|silu(-2.0)| < 2.0, got {}", got.abs());
        assert!(
            approx_eq(got, -0.2384, 1e-2),
            "silu(-2.0) ≈ -0.2384, got {}",
            got
        );
    }

    #[test]
    fn silu_out_preserves_input() {
        let x = from_f32(&[1.0, -2.0, 0.0, 3.0]);
        let original = x.clone();
        let mut out = vec![0u16; 4];
        silu_out(&x, &mut out).expect("shape ok");
        // Input untouched.
        assert_eq!(x, original);
        // Output matches in-place SiLU.
        let mut inplace = original.clone();
        silu(&mut inplace);
        assert_eq!(out, inplace);
    }

    #[test]
    fn silu_out_rejects_shape_mismatch() {
        let x = from_f32(&[1.0; 4]);
        let mut out = vec![0u16; 3];
        assert!(silu_out(&x, &mut out).is_none());
    }

    // ---- M4.2: Q4_K matmul + GQA + SwiGLU + LMHead --------------------

    use crate::inference::quant::q8_k_byte_size;
    use crate::slm::gguf::Q4_K_BLOCK_SIZE;

    /// Build a Q4_K-encoded weight row of `cols` elements from a
    /// known FP32 vector. Reuses M3's vec_dot test fixture builder
    /// approach: dequantize-then-quantize would be lossy, so we
    /// construct a block with d=1.0, dmin=0, scales=[1; 8], mins=[0; 8]
    /// and qs encoding the integers 0..15 per nibble. That gives a
    /// known integer dot product for the activation pattern below.
    fn q4k_block_zeros() -> [u8; Q4_K_BLOCK_SIZE] {
        // d = +0.0 (f16 0x0000) → dequant always = 0.
        [0u8; Q4_K_BLOCK_SIZE]
    }

    #[test]
    fn matmul_q4k_row_zero_weights_zero_dot() {
        let cols = Q4_K_BLOCK_ELEMENTS;
        let weights = q4k_block_zeros();
        let acts = from_f32(&vec![1.0; cols]);
        let mut q8k = vec![0u8; q8_k_byte_size(cols).unwrap()];
        let dot = matmul_q4k_row(&weights, &acts, &mut q8k, cols).expect("matmul");
        assert!(dot.abs() < 1e-3, "zero weights → zero dot, got {}", dot);
    }

    #[test]
    fn matmul_q4k_row_zero_acts_zero_dot() {
        let cols = Q4_K_BLOCK_ELEMENTS;
        let weights = q4k_block_zeros();
        // Even with zero weights, the path still has to handle
        // zero activations cleanly (no div-by-zero in iscale).
        let acts = vec![0u16; cols];
        let mut q8k = vec![0u8; q8_k_byte_size(cols).unwrap()];
        let dot = matmul_q4k_row(&weights, &acts, &mut q8k, cols).expect("matmul");
        assert!(dot.abs() < 1e-3);
    }

    #[test]
    fn matmul_q4k_row_rejects_shape_mismatch() {
        let cols = Q4_K_BLOCK_ELEMENTS;
        let weights = q4k_block_zeros();
        let acts_short = from_f32(&vec![1.0; cols - 1]);
        let mut q8k = vec![0u8; q8_k_byte_size(cols).unwrap()];
        assert!(matmul_q4k_row(&weights, &acts_short, &mut q8k, cols).is_none());
    }

    #[test]
    fn matmul_q4k_row_rejects_non_block_cols() {
        // 200 isn't a multiple of 256.
        let weights = vec![0u8; 200];
        let acts = vec![0u16; 200];
        let mut q8k = vec![0u8; 200];
        assert!(matmul_q4k_row(&weights, &acts, &mut q8k, 200).is_none());
    }

    #[test]
    fn matmul_q4k_rows_consistent_with_per_row() {
        let cols = Q4_K_BLOCK_ELEMENTS;
        let rows = 3;
        // Three identical zero-weight blocks → zero dot for each row.
        let mut weights: Vec<u8> = Vec::with_capacity(rows * Q4_K_BLOCK_SIZE);
        for _ in 0..rows {
            weights.extend_from_slice(&q4k_block_zeros());
        }
        let acts = from_f32(&vec![1.0; cols]);
        let mut q8k = vec![0u8; q8_k_byte_size(cols).unwrap()];
        let mut out = vec![0.0f32; rows];
        matmul_q4k_rows(&weights, rows, cols, &acts, &mut q8k, &mut out).expect("rows");
        for v in &out {
            assert!(v.abs() < 1e-3, "zero-weight row → 0 dot");
        }

        // Compare against per-row matmul.
        for r in 0..rows {
            let row = &weights[r * Q4_K_BLOCK_SIZE..(r + 1) * Q4_K_BLOCK_SIZE];
            let mut q8k_each = vec![0u8; q8_k_byte_size(cols).unwrap()];
            let single = matmul_q4k_row(row, &acts, &mut q8k_each, cols).expect("single");
            assert!((single - out[r]).abs() < 1e-3);
        }
    }

    /// Build a non-trivial Q4_K-encoded weight row of `cols` elements
    /// with a deterministic per-row bit pattern. Used by the batched
    /// matmul tests so a regression in inner-loop ordering would
    /// actually flip a number (vs the all-zero fixture above which
    /// returns zero for every path).
    ///
    /// Each block uses d=1.0 (f16 0x3C00), dmin=0, scales-and-mins
    /// header set so per-sub-block scale=1 / min=0, and `qs` filled
    /// with `(i ^ row_seed) & 0x0F | ((i ^ row_seed) << 4) & 0xF0`
    /// so two adjacent rows produce distinguishable dot products.
    fn q4k_row_nontrivial(cols: usize, row_seed: u8) -> Vec<u8> {
        let mut row = vec![0u8; q4_k_byte_size(cols).unwrap()];
        let nb = cols / Q4_K_BLOCK_ELEMENTS;
        for b in 0..nb {
            let base = b * Q4_K_BLOCK_SIZE;
            row[base] = 0x00;
            row[base + 1] = 0x3C;
            row[base + 2] = 0x00;
            row[base + 3] = 0x00;
            for i in 0..12 {
                row[base + 4 + i] = (i as u8) | ((i as u8) << 4);
            }
            for i in 0..128 {
                let nib = ((i as u8) ^ row_seed) & 0x0F;
                row[base + 16 + i] = nib | (nib << 4);
            }
        }
        row
    }

    #[test]
    fn matmul_q4k_rows_batch_b1_matches_matmul_q4k_rows() {
        // batch=1 must be bit-equal to matmul_q4k_rows for any weights.
        let cols = Q4_K_BLOCK_ELEMENTS;
        let rows = 4usize;
        let mut weights = Vec::new();
        for r in 0..rows {
            weights.extend_from_slice(&q4k_row_nontrivial(cols, r as u8));
        }
        let acts = from_f32(&(0..cols).map(|i| (i as f32) / (cols as f32)).collect::<Vec<_>>());
        let q8k_per = q8_k_byte_size(cols).unwrap();

        let mut q8k_a = vec![0u8; q8k_per];
        let mut out_serial = vec![0.0f32; rows];
        matmul_q4k_rows(&weights, rows, cols, &acts, &mut q8k_a, &mut out_serial)
            .expect("serial");

        let mut q8k_b = vec![0u8; q8k_per];
        let mut out_batch = vec![0.0f32; rows];
        matmul_q4k_rows_batch(&weights, rows, cols, 1, &acts, &mut q8k_b, &mut out_batch)
            .expect("batch B=1");
        assert_eq!(out_batch, out_serial, "batch=1 must equal serial bit-for-bit");
    }

    #[test]
    fn matmul_q4k_rows_batch_matches_per_token_loop() {
        // batched(B>1) result row b must equal matmul_q4k_rows on
        // activation row b for every b in 0..batch.
        let cols = Q4_K_BLOCK_ELEMENTS;
        let rows = 5usize;
        let batch = 3usize;
        let mut weights = Vec::new();
        for r in 0..rows {
            weights.extend_from_slice(&q4k_row_nontrivial(cols, (r as u8).wrapping_add(7)));
        }

        // Distinct activation row per batch slot.
        let mut acts_fp16: Vec<u16> = Vec::with_capacity(batch * cols);
        for b in 0..batch {
            let row_f32: Vec<f32> = (0..cols)
                .map(|i| ((i + b * 13) as f32) / (cols as f32))
                .collect();
            acts_fp16.extend_from_slice(&from_f32(&row_f32));
        }

        let q8k_per = q8_k_byte_size(cols).unwrap();
        let mut q8k_batch = vec![0u8; q8k_per * batch];
        let mut out_batch = vec![0.0f32; batch * rows];
        matmul_q4k_rows_batch(
            &weights, rows, cols, batch, &acts_fp16, &mut q8k_batch, &mut out_batch,
        )
        .expect("batch");

        for b in 0..batch {
            let act = &acts_fp16[b * cols..(b + 1) * cols];
            let mut q8k_each = vec![0u8; q8k_per];
            let mut out_each = vec![0.0f32; rows];
            matmul_q4k_rows(&weights, rows, cols, act, &mut q8k_each, &mut out_each)
                .expect("per-token");
            for r in 0..rows {
                let bv = out_batch[b * rows + r];
                let sv = out_each[r];
                assert_eq!(
                    bv.to_bits(),
                    sv.to_bits(),
                    "batch row b={b} r={r}: batched={bv} per-token={sv}"
                );
            }
        }
    }

    #[test]
    fn matmul_q4k_rows_batch_rejects_bad_shapes() {
        let cols = Q4_K_BLOCK_ELEMENTS;
        let rows = 2usize;
        let weights = vec![0u8; rows * Q4_K_BLOCK_SIZE];
        let q8k_per = q8_k_byte_size(cols).unwrap();
        let acts = vec![0u16; cols * 2];
        let mut q8k = vec![0u8; q8k_per * 2];
        let mut out = vec![0.0f32; rows * 2];

        // batch == 0
        assert!(
            matmul_q4k_rows_batch(&weights, rows, cols, 0, &acts, &mut q8k, &mut out).is_none()
        );
        // cols not multiple of 256
        assert!(matmul_q4k_rows_batch(&weights, rows, 200, 2, &acts, &mut q8k, &mut out).is_none());
        // acts shape mismatch
        let acts_short = vec![0u16; cols];
        assert!(
            matmul_q4k_rows_batch(&weights, rows, cols, 2, &acts_short, &mut q8k, &mut out)
                .is_none()
        );
        // out shape mismatch
        let mut out_short = vec![0.0f32; rows];
        assert!(
            matmul_q4k_rows_batch(&weights, rows, cols, 2, &acts, &mut q8k, &mut out_short)
                .is_none()
        );
        // q8k_scratch too small
        let mut q8k_short = vec![0u8; q8k_per];
        assert!(
            matmul_q4k_rows_batch(&weights, rows, cols, 2, &acts, &mut q8k_short, &mut out)
                .is_none()
        );
    }

    // ---- GQA --------------------------------------------------------

    #[test]
    fn gqa_decode_single_position_returns_v() {
        // n_head_q=2, n_head_kv=1 (group=2), head_dim=4, seq_len=1.
        // With seq_len=1, softmax([logit]) = [1.0], so out == v.
        let n_head_q = 2;
        let n_head_kv = 1;
        let head_dim = 4;
        let seq_len = 1;
        let q = from_f32(&[1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]); // 2 heads × 4 dims
        let k = from_f32(&[0.5, 0.5, 0.5, 0.5]); // 1 kv-head × 4 dims × 1 pos
        let v = from_f32(&[0.1, 0.2, 0.3, 0.4]);
        let mut scratch = vec![0.0f32; seq_len];
        let mut out = vec![0u16; n_head_q * head_dim];
        gqa_decode_step(
            &q, &k, &v, n_head_q, n_head_kv, head_dim, seq_len, &mut scratch, &mut out,
        )
        .expect("gqa");
        let out_f32 = to_f32(&out);
        // Both query heads share the single KV head; output is v
        // for both.
        for h in 0..n_head_q {
            for d in 0..head_dim {
                let expected = [0.1, 0.2, 0.3, 0.4][d];
                assert!(
                    approx_eq(out_f32[h * head_dim + d], expected, 1e-2),
                    "head {h} dim {d} = {} expected {expected}",
                    out_f32[h * head_dim + d]
                );
            }
        }
    }

    #[test]
    fn gqa_decode_two_positions_softmax_blends_v() {
        // 1 head, 1 kv-head, head_dim=2, seq_len=2.
        // q=[1,0]; k0=[0,1] (logit≈0); k1=[1,0] (logit≈1/sqrt(2)).
        // Softmax favours k1, so output blends toward v1.
        let q = from_f32(&[1.0, 0.0]);
        let k = from_f32(&[0.0, 1.0, 1.0, 0.0]); // pos 0, pos 1
        let v = from_f32(&[1.0, 1.0, 10.0, 10.0]); // v0=[1,1], v1=[10,10]
        let mut scratch = vec![0.0f32; 2];
        let mut out = vec![0u16; 2];
        gqa_decode_step(&q, &k, &v, 1, 1, 2, 2, &mut scratch, &mut out).expect("gqa");
        let got = to_f32(&out);
        // Expected: softmax([0, 1/sqrt(2)]) = [α, β], β > α, output ≈ α*1 + β*10.
        let inv = 1.0 / sqrtf(2.0);
        let e0 = libm::expf(0.0 - inv);
        let e1 = libm::expf(inv - inv);
        let denom = e0 + e1;
        let alpha = e0 / denom;
        let beta = e1 / denom;
        let expected = alpha * 1.0 + beta * 10.0;
        assert!(approx_eq(got[0], expected, 5e-2));
        assert!(approx_eq(got[1], expected, 5e-2));
    }

    #[test]
    fn gqa_decode_rejects_bad_head_ratio() {
        // 5 query heads, 2 kv heads → 5 % 2 != 0.
        let q = vec![0u16; 5 * 4];
        let k = vec![0u16; 1 * 2 * 4];
        let v = vec![0u16; 1 * 2 * 4];
        let mut scratch = vec![0.0f32; 1];
        let mut out = vec![0u16; 5 * 4];
        assert!(
            gqa_decode_step(&q, &k, &v, 5, 2, 4, 1, &mut scratch, &mut out).is_none()
        );
    }

    // ---- SwiGLU MLP -------------------------------------------------

    #[test]
    fn swiglu_mlp_zero_input_zero_output() {
        // x=0 ⇒ gate=up=0 ⇒ silu(0)*0 = 0 ⇒ down(0) = 0.
        let hidden = Q4_K_BLOCK_ELEMENTS;
        let inter = Q4_K_BLOCK_ELEMENTS;
        let row = Q4_K_BLOCK_SIZE;
        let gate_w = vec![0u8; inter * row];
        let up_w = vec![0u8; inter * row];
        let down_w = vec![0u8; hidden * row];
        let x = vec![0u16; hidden];
        let mut gate_s = vec![0u16; inter];
        let mut up_s = vec![0u16; inter];
        let mut q8k =
            vec![0u8; q8_k_byte_size(hidden.max(inter)).unwrap()];
        let mut out = vec![0u16; hidden];
        swiglu_mlp(
            &x, &gate_w, &up_w, &down_w, hidden, inter, &mut gate_s, &mut up_s, &mut q8k,
            &mut out,
        )
        .expect("swiglu");
        for &b in &out {
            assert_eq!(f16_to_f32(b), 0.0);
        }
    }

    #[test]
    fn swiglu_mlp_rejects_non_block_dims() {
        let bad_hidden = 200;
        let inter = Q4_K_BLOCK_ELEMENTS;
        let mut q8k = vec![0u8; 4096];
        let mut gate_s = vec![0u16; inter];
        let mut up_s = vec![0u16; inter];
        let mut out = vec![0u16; bad_hidden];
        let res = swiglu_mlp(
            &vec![0u16; bad_hidden],
            &[],
            &[],
            &[],
            bad_hidden,
            inter,
            &mut gate_s,
            &mut up_s,
            &mut q8k,
            &mut out,
        );
        assert!(res.is_none());
    }

    // ---- LM head ----------------------------------------------------

    #[test]
    fn lm_head_returns_logit_per_vocab() {
        // 8-vocab × 256-hidden, all zero weights → all-zero logits.
        let hidden = Q4_K_BLOCK_ELEMENTS;
        let vocab = 8;
        let row = Q4_K_BLOCK_SIZE;
        let weight = vec![0u8; vocab * row];
        let x = from_f32(&vec![1.0; hidden]);
        let mut q8k = vec![0u8; q8_k_byte_size(hidden).unwrap()];
        let mut logits = vec![1.0f32; vocab]; // pre-fill non-zero
        lm_head(&x, &weight, hidden, vocab, &mut q8k, &mut logits).expect("lm_head");
        for &v in &logits {
            assert!(v.abs() < 1e-3, "zero weight → zero logit, got {v}");
        }
    }

    #[test]
    fn lm_head_rejects_zero_dims() {
        let mut q8k = vec![0u8; 256];
        let mut logits = vec![0.0f32; 4];
        assert!(lm_head(&[], &[], 0, 4, &mut q8k, &mut logits).is_none());
        assert!(
            lm_head(&[0u16; 4], &[], 4, 0, &mut q8k, &mut logits).is_none()
        );
    }
}
