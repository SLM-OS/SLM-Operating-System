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
//! This module ships the simpler ops (RMSNorm, RoPE, FP16 embedding lookup,
//! SiLU). The matmul-heavy GQA / SwiGLU MLP / LMHead path is M4.2.
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
use crate::slm::gguf::{f16_to_f32, f32_to_f16};

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
}
