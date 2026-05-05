//! Q4_K_M and Q8_K quantization kernels.
//!
//! Numeric algorithms translated from llama.cpp's
//! `ggml/src/ggml-quants.c` and `ggml/src/ggml-cpu/quants.c`
//! (MIT-licensed). See `LICENSES.md` for the full attribution.
//!
//! # Layout reference
//!
//! ## Q4_K (super-block: 256 elements, 144 bytes)
//!
//! ```text
//! struct block_q4_K {
//!     f16   d;            //  2 B   super-block scale  (offset   0..  2)
//!     f16   dmin;         //  2 B   super-block min    (offset   2..  4)
//!     u8    scales[12];   // 12 B   8 x 6-bit scales + 8 x 6-bit mins
//!     u8    qs[128];      //128 B   256 nibbles (4-bit weights)
//! };  // total 144 B, 4.5 bpw
//! ```
//!
//! Super-block-level dequantization formula (per 32-element half-block
//! `j`, `j` in `0..8`):
//!
//! ```text
//! sc = scale_6bit(j)
//! mn = min_6bit(j)
//! out[j*32 + l]      = d * sc * (qs_lo(l)) - dmin * mn   for l in 0..32
//! ```
//!
//! where `scale_6bit` / `min_6bit` come from the `get_scale_min_k4`
//! 6-bit unpacking already implemented by
//! [`crate::slm::gguf::Q4KBlockView::scale`] / `::min`. The 4-bit
//! quants alternate between low and high nibbles of `qs`: each 64-byte
//! group (`qs[k*32..(k+1)*32]` paired with the next 32) yields 64
//! output values via two scales `is=2*k`, `is+1=2*k+1`.
//!
//! ## Q8_K (super-block: 256 elements, 292 bytes)
//!
//! ```text
//! struct block_q8_K {
//!     f32     d;          //  4 B   block scale (FP32)
//!     int8_t  qs[256];    //256 B   8-bit quants
//!     int16_t bsums[16];  // 32 B   sum of qs[16*j..16*(j+1)] per j
//! };  // total 292 B
//! ```
//!
//! `bsums[j] = sum_{l=0..16} qs[16*j + l]`. The 16 entries factor the
//! min-term cleanly out of the inner loop in
//! [`vec_dot_q4_k_q8_k`].

#![allow(clippy::needless_range_loop)]

use crate::slm::gguf::{Q4KBlockView, Q4_K_BLOCK_ELEMENTS, Q4_K_BLOCK_SIZE};

// ---------------------------------------------------------------------------
// Q8_K layout constants
// ---------------------------------------------------------------------------

/// Bytes per Q8_K block: `4 (d) + 256 (qs) + 32 (bsums) = 292`.
pub const Q8_K_BLOCK_SIZE: usize = 292;

/// Elements per Q8_K block.
pub const Q8_K_BLOCK_ELEMENTS: usize = 256;

/// Number of Q8_K blocks needed to cover `elements` weights, rounding
/// up. Mirrors the Q4_K helper.
pub const fn q8_k_block_count(elements: usize) -> usize {
    elements / Q8_K_BLOCK_ELEMENTS
        + ((elements % Q8_K_BLOCK_ELEMENTS != 0) as usize)
}

/// Checked on-disk byte size of `elements` weights in Q8_K. Returns
/// `None` if the multiplication would overflow `usize`.
pub fn q8_k_byte_size(elements: usize) -> Option<usize> {
    q8_k_block_count(elements).checked_mul(Q8_K_BLOCK_SIZE)
}

// Internal byte offsets inside one Q8_K block.
const Q8K_OFF_D: usize = 0;
const Q8K_OFF_QS: usize = 4;
const Q8K_OFF_BSUMS: usize = 4 + Q8_K_BLOCK_ELEMENTS;

// ---------------------------------------------------------------------------
// Q4_K dequantization
// ---------------------------------------------------------------------------

/// Dequantize a row of Q4_K-packed weights into FP32.
///
/// `weights` must be a contiguous slice of `N * Q4_K_BLOCK_SIZE`
/// bytes (`N` super-blocks). `out` must have at least `N *
/// Q4_K_BLOCK_ELEMENTS` floats of capacity. Returns the number of
/// floats written, or `None` if the slices don't line up (e.g.
/// `weights.len() % 144 != 0` or `out` is too small).
///
/// This is the M3 reference implementation: scalar, no SIMD. NEON
/// fast path is layered on top in a follow-up — the inner per-32
/// dequant is factored to make that drop-in.
pub fn dequantize_row_q4_k(weights: &[u8], out: &mut [f32]) -> Option<usize> {
    if weights.len() % Q4_K_BLOCK_SIZE != 0 {
        return None;
    }
    let nb = weights.len() / Q4_K_BLOCK_SIZE;
    let n_out = nb.checked_mul(Q4_K_BLOCK_ELEMENTS)?;
    if out.len() < n_out {
        return None;
    }
    for b in 0..nb {
        let block = &weights[b * Q4_K_BLOCK_SIZE..(b + 1) * Q4_K_BLOCK_SIZE];
        let view = Q4KBlockView::from_slice(block)?;
        let dst = &mut out[b * Q4_K_BLOCK_ELEMENTS..(b + 1) * Q4_K_BLOCK_ELEMENTS];
        dequant_q4k_block(&view, dst);
    }
    Some(n_out)
}

/// Dequantize a single Q4_K block into 256 FP32 outputs.
///
/// Mirrors `dequantize_row_q4_K` from llama.cpp. The qs bytes are
/// read in 32-byte chunks: bytes `[0..32]` go with scale `is=0`
/// (low nibbles) and `is=1` (high nibbles). Bytes `[32..64]` go
/// with `is=2`, `is=3`. And so on for `is in 0..8`.
#[inline]
fn dequant_q4k_block(view: &Q4KBlockView<'_>, out: &mut [f32]) {
    debug_assert!(out.len() >= Q4_K_BLOCK_ELEMENTS);
    let d = view.d();
    let dmin = view.dmin();
    // Eight scale/min pairs cover the 256-element block in two
    // 32-element halves per `j` step (low nibbles, then high).
    for j in 0..4 {
        let is = j * 2;
        let sc1 = view.scale(is) as u32 as f32;
        let mn1 = view.min(is) as u32 as f32;
        let sc2 = view.scale(is + 1) as u32 as f32;
        let mn2 = view.min(is + 1) as u32 as f32;
        let d1 = d * sc1;
        let m1 = dmin * mn1;
        let d2 = d * sc2;
        let m2 = dmin * mn2;

        // qs offset: each `j` consumes 32 bytes starting at j*32.
        let qs_base = j * 32;
        let out_base = j * 64;
        for l in 0..32 {
            let q = view.quant(qs_base + l);
            out[out_base + l] = d1 * ((q & 0x0F) as i32 as f32) - m1;
            out[out_base + 32 + l] = d2 * ((q >> 4) as i32 as f32) - m2;
        }
    }
}

// ---------------------------------------------------------------------------
// Q8_K quantization
// ---------------------------------------------------------------------------

/// Quantize a row of FP32 floats into Q8_K-packed bytes.
///
/// `floats.len()` must be a multiple of [`Q8_K_BLOCK_ELEMENTS`].
/// `out` must have at least `q8_k_byte_size(floats.len())` bytes.
/// Returns the number of bytes written, or `None` on shape mismatch.
///
/// Mirrors `quantize_row_q8_K_ref` from llama.cpp:
///
/// 1. Find element with maximum absolute value (`amax`) and its
///    signed value (`max`).
/// 2. If `amax == 0`, write `d = 0`, `qs = 0`, `bsums = 0`.
/// 3. Otherwise, compute `iscale = -127 / max`, quantize each
///    element via `nearest_int(iscale * x)` clamped to `[-128, 127]`
///    via `min(127, v)`. (The negative `iscale` is intentional —
///    cancels in the dot.)
/// 4. Fill `bsums[j]` with `sum_{l=0..16} qs[16*j + l]`.
/// 5. Write `d = 1.0 / iscale`.
pub fn quantize_row_q8_k(floats: &[f32], out: &mut [u8]) -> Option<usize> {
    if floats.len() % Q8_K_BLOCK_ELEMENTS != 0 {
        return None;
    }
    let nb = floats.len() / Q8_K_BLOCK_ELEMENTS;
    let n_bytes = nb.checked_mul(Q8_K_BLOCK_SIZE)?;
    if out.len() < n_bytes {
        return None;
    }
    for b in 0..nb {
        let src = &floats[b * Q8_K_BLOCK_ELEMENTS..(b + 1) * Q8_K_BLOCK_ELEMENTS];
        let dst = &mut out[b * Q8_K_BLOCK_SIZE..(b + 1) * Q8_K_BLOCK_SIZE];
        quantize_q8k_block(src, dst);
    }
    Some(n_bytes)
}

#[inline]
fn quantize_q8k_block(src: &[f32], dst: &mut [u8]) {
    debug_assert!(src.len() >= Q8_K_BLOCK_ELEMENTS);
    debug_assert!(dst.len() >= Q8_K_BLOCK_SIZE);

    // Pass 1: find amax and signed max.
    let mut amax = 0.0f32;
    let mut max = 0.0f32;
    for j in 0..Q8_K_BLOCK_ELEMENTS {
        let ax = src[j].abs();
        if ax > amax {
            amax = ax;
            max = src[j];
        }
    }

    if amax == 0.0 {
        // Zero block: d = 0, qs = 0, bsums = 0.
        for b in dst.iter_mut().take(Q8_K_BLOCK_SIZE) {
            *b = 0;
        }
        return;
    }

    let iscale = -127.0f32 / max;

    // Quantize values into qs[0..256] (a scoped borrow so the
    // bsums write below can re-borrow the same `dst`).
    {
        let qs = &mut dst[Q8K_OFF_QS..Q8K_OFF_QS + Q8_K_BLOCK_ELEMENTS];
        for j in 0..Q8_K_BLOCK_ELEMENTS {
            let v = nearest_int(iscale * src[j]);
            // Clamp on the upper side only, matching llama.cpp's
            // `MIN(127, v)`. The lower side is bounded by
            // construction via the amax denominator.
            let v_clamped = if v > 127 { 127 } else { v };
            qs[j] = v_clamped as i8 as u8;
        }
    }

    // bsums[j] = sum_{l=0..16} qs[16*j + l] (signed). Read back
    // from `dst` directly so we don't carry the qs slice across
    // the bsums writes.
    let bsums_off = Q8K_OFF_BSUMS;
    for j in 0..16 {
        let mut sum: i32 = 0;
        for l in 0..16 {
            sum += dst[Q8K_OFF_QS + j * 16 + l] as i8 as i32;
        }
        let s16 = sum as i16;
        let bytes = s16.to_le_bytes();
        dst[bsums_off + j * 2] = bytes[0];
        dst[bsums_off + j * 2 + 1] = bytes[1];
    }

    // d = 1 / iscale (FP32, little-endian).
    let d = 1.0f32 / iscale;
    let d_bytes = d.to_le_bytes();
    dst[Q8K_OFF_D] = d_bytes[0];
    dst[Q8K_OFF_D + 1] = d_bytes[1];
    dst[Q8K_OFF_D + 2] = d_bytes[2];
    dst[Q8K_OFF_D + 3] = d_bytes[3];
}

/// `(int)round_half_away_from_zero(f)` — round-half-AWAY-from-zero
/// rounding for the Q8_K quantizer.
///
/// **Note on bit-identity with llama.cpp:** GGML's
/// `ggml_vec_dot_q4_K_q8_K`-companion `nearest_int` uses the
/// magic-add trick `fval + 12582912.f`, which on x86-64 with the
/// default rounding mode is round-half-to-EVEN (banker's rounding).
/// SLM-OS rounds away from zero. The two agree everywhere except
/// at exact `iscale * x = ±k.5` ties — vanishingly rare with a
/// real f32 multiply, so the practical impact on Q8_K inputs is
/// nil. Bit-identity with HuggingFace / llama.cpp Q8_K outputs is
/// therefore *not* guaranteed; numeric closeness within Q8_K's
/// per-element noise floor (~1/127 of the block max) is.
///
/// If a future caller needs strict bit-equality, swap to the
/// magic-add trick and update the `nearest_int_rounds_*` test to
/// pin `nearest_int(0.5) == 0`, `nearest_int(1.5) == 2`.
#[inline]
fn nearest_int(f: f32) -> i32 {
    if f >= 0.0 {
        (f + 0.5) as i32
    } else {
        -(((-f) + 0.5) as i32)
    }
}

// ---------------------------------------------------------------------------
// Q8_K accessors (test-friendly + dot-product helpers)
// ---------------------------------------------------------------------------

/// Read `d` (FP32) from a Q8_K block at `bytes[0..292]`.
#[inline]
fn q8k_d(bytes: &[u8]) -> f32 {
    let arr = [bytes[0], bytes[1], bytes[2], bytes[3]];
    f32::from_le_bytes(arr)
}

/// Read `qs[idx]` as i8 from a Q8_K block.
#[inline]
fn q8k_qs(bytes: &[u8], idx: usize) -> i8 {
    bytes[Q8K_OFF_QS + idx] as i8
}

/// Read `bsums[idx]` as i16 from a Q8_K block.
#[inline]
fn q8k_bsum(bytes: &[u8], idx: usize) -> i16 {
    let lo = bytes[Q8K_OFF_BSUMS + idx * 2];
    let hi = bytes[Q8K_OFF_BSUMS + idx * 2 + 1];
    i16::from_le_bytes([lo, hi])
}

// ---------------------------------------------------------------------------
// vec_dot_q4_K_q8_K
// ---------------------------------------------------------------------------

/// Dot product of a Q4_K weight row and a Q8_K activation row.
///
/// Both rows must cover the same number of elements, a multiple of
/// 256. Returns `None` if shapes mismatch (`weights.len() % 144 != 0`
/// or `acts.len() % 292 != 0` or the two implied block counts differ).
///
/// This is the M5/M6 hot path: every QKV / output / gate / up / down
/// projection inside the transformer reduces to this kernel.
///
/// Algorithm (mirrors `ggml_vec_dot_q4_K_q8_K_generic`):
///
/// ```text
/// for each super-block pair b in 0..nb:
///     d_w   = q4k.d
///     dmin  = q4k.dmin
///     d_a   = q8k.d
///
///     // Min term factors over half-block bsums.
///     // mins index runs 0..8; bsums[j] for j in 0..16 share
///     // bucket `j/2`.
///     min_sum = sum_{j=0..16} bsums[j] * min_6bit(j/2)
///
///     // Inner-block accumulation: 8 sub-blocks of 32 elements
///     // each. Each sub-block has its own 6-bit scale.
///     scale_sum = 0
///     for is in 0..8:
///         partial = sum_{l=0..32} q4_value(l, is) * q8.qs[is*32 + l]
///         scale_sum += scale_6bit(is) * partial
///
///     total += d_w * d_a * scale_sum  -  dmin * d_a * min_sum
/// ```
pub fn vec_dot_q4_k_q8_k(weights: &[u8], acts: &[u8]) -> Option<f32> {
    if weights.len() % Q4_K_BLOCK_SIZE != 0 {
        return None;
    }
    if acts.len() % Q8_K_BLOCK_SIZE != 0 {
        return None;
    }
    let nb_w = weights.len() / Q4_K_BLOCK_SIZE;
    let nb_a = acts.len() / Q8_K_BLOCK_SIZE;
    if nb_w != nb_a {
        return None;
    }

    let mut sumf = 0.0f32;
    for b in 0..nb_w {
        let w_block = &weights[b * Q4_K_BLOCK_SIZE..(b + 1) * Q4_K_BLOCK_SIZE];
        let a_block = &acts[b * Q8_K_BLOCK_SIZE..(b + 1) * Q8_K_BLOCK_SIZE];
        let view = Q4KBlockView::from_slice(w_block)?;
        sumf += dot_q4k_q8k_block(&view, a_block);
    }
    Some(sumf)
}

/// Per-block dot product. Pulled out so the NEON fast path can swap
/// in without restructuring the outer loop.
#[inline]
fn dot_q4k_q8k_block(view: &Q4KBlockView<'_>, a_block: &[u8]) -> f32 {
    let d_w = view.d();
    let dmin_w = view.dmin();
    let d_a = q8k_d(a_block);

    // Min-term: bsums[j] (j=0..16) share min bucket j/2 (so each
    // mins[k], k=0..8, gets bsums[2k] + bsums[2k+1]).
    let mut min_sum: i32 = 0;
    for j in 0..16 {
        let mn = view.min(j / 2) as i32;
        min_sum += q8k_bsum(a_block, j) as i32 * mn;
    }

    // Inner: 8 sub-blocks of 32 elements; each sub-block has its
    // own 6-bit scale. The 32 q4 values of sub-block `is` come
    // from low/high nibbles of the qs array — the same layout as
    // the dequantizer.
    let mut scale_sum: i32 = 0;
    for j in 0..4 {
        let qs_base = j * 32;
        let q8_lo_base = (j * 2) * 32;
        let q8_hi_base = (j * 2 + 1) * 32;

        let sc_lo = view.scale(j * 2) as i32;
        let sc_hi = view.scale(j * 2 + 1) as i32;

        let mut acc_lo: i32 = 0;
        let mut acc_hi: i32 = 0;
        for l in 0..32 {
            let q = view.quant(qs_base + l) as i32;
            let q_lo = q & 0x0F;
            let q_hi = q >> 4;
            acc_lo += q_lo * (q8k_qs(a_block, q8_lo_base + l) as i32);
            acc_hi += q_hi * (q8k_qs(a_block, q8_hi_base + l) as i32);
        }
        scale_sum += sc_lo * acc_lo + sc_hi * acc_hi;
    }

    d_w * d_a * (scale_sum as f32) - dmin_w * d_a * (min_sum as f32)
}

// ---------------------------------------------------------------------------
// Microbench
// ---------------------------------------------------------------------------

/// Microbench helper: runs `vec_dot_q4_k_q8_k` over `elements`-wide
/// rows once and returns elapsed nanoseconds via the kernel's
/// monotonic clock. Returns `0` on shape error or unavailable clock.
///
/// Surfaced as a function (not a shell command) for M3 — the
/// `bench q4kdot` shell wrapper lands in M9 and calls this through
/// FFI. Inputs are filled deterministically so repeated calls are
/// reproducible.
///
/// `elements` must be a multiple of 256; the function returns 0 if
/// not, or if the implied alloc would overflow.
pub fn benchmark_q4k_q8k_dot(elements: usize) -> u64 {
    if elements == 0 || elements % Q4_K_BLOCK_ELEMENTS != 0 {
        return 0;
    }
    let nb = elements / Q4_K_BLOCK_ELEMENTS;
    let w_bytes = match nb.checked_mul(Q4_K_BLOCK_SIZE) {
        Some(v) => v,
        None => return 0,
    };
    let a_bytes = match nb.checked_mul(Q8_K_BLOCK_SIZE) {
        Some(v) => v,
        None => return 0,
    };

    extern crate alloc;
    let mut weights = alloc::vec![0u8; w_bytes];
    let mut acts = alloc::vec![0u8; a_bytes];

    // Fill weights with a deterministic pattern: d = 1.0 (f16
    // 0x3C00), dmin = 0.5 (f16 0x3800), scales running
    // 0,1,2,...,11, qs ascending mod 256.
    for b in 0..nb {
        let base = b * Q4_K_BLOCK_SIZE;
        weights[base] = 0x00;
        weights[base + 1] = 0x3C;
        weights[base + 2] = 0x00;
        weights[base + 3] = 0x38;
        for i in 0..12 {
            weights[base + 4 + i] = (i as u8) | ((i as u8) << 4);
        }
        for i in 0..128 {
            weights[base + 16 + i] = (i ^ b) as u8;
        }
    }
    // Fill acts with d = 1.0 (f32), qs alternating +1/-1, bsums
    // matching.
    for b in 0..nb {
        let base = b * Q8_K_BLOCK_SIZE;
        let d_bytes = 1.0f32.to_le_bytes();
        acts[base..base + 4].copy_from_slice(&d_bytes);
        for i in 0..256 {
            acts[base + 4 + i] = if i & 1 == 0 { 1 } else { 0xFFu8 };
        }
        // bsums[j] = sum of 16 elements (eight 1's and eight -1's = 0)
        for j in 0..16 {
            acts[base + Q8K_OFF_BSUMS + j * 2] = 0;
            acts[base + Q8K_OFF_BSUMS + j * 2 + 1] = 0;
        }
    }

    let start = monotonic_ns();
    // Black-box the result: store into a static-like sink.
    let s = vec_dot_q4_k_q8_k(&weights, &acts).unwrap_or(0.0);
    // Use the result to prevent the optimizer from eliding the call.
    core::hint::black_box(s);
    let end = monotonic_ns();
    end.wrapping_sub(start)
}

/// Read a monotonic nanosecond counter. Best-effort; returns 0 if
/// no clock is available so the caller can detect the no-op case.
#[inline]
fn monotonic_ns() -> u64 {
    // On aarch64 the generic counter (CNTVCT_EL0) ticks at
    // CNTFRQ_EL0 Hz — typically 19.2 MHz on Pi 5 / Jetson but
    // platform-dependent. We don't have FFI for that here, so
    // fall back to 0 (caller will see "0 ns" and know to wire
    // the kernel-side clock in M9).
    #[cfg(target_arch = "aarch64")]
    {
        let cnt: u64;
        let frq: u64;
        // SAFETY: CNTVCT_EL0 / CNTFRQ_EL0 are EL0-readable system
        // registers per ARMv8-A; reading them has no side effects.
        unsafe {
            core::arch::asm!("mrs {0}, cntvct_el0", out(reg) cnt, options(nostack, preserves_flags));
            core::arch::asm!("mrs {0}, cntfrq_el0", out(reg) frq, options(nostack, preserves_flags));
        }
        if frq == 0 {
            return 0;
        }
        // ns = cnt * 1e9 / frq. Use u128 to avoid overflow on
        // long-running benches.
        let ns = (cnt as u128).saturating_mul(1_000_000_000) / (frq as u128);
        ns as u64
    }
    #[cfg(not(target_arch = "aarch64"))]
    {
        0
    }
}

// ---------------------------------------------------------------------------
// Q8_0 dequantization
// ---------------------------------------------------------------------------
//
// Block layout: { f16 d; i8 qs[32]; }  — 34 bytes per 32 elements.
// Each weight is a signed 8-bit value scaled by `d` (per-block).

use crate::slm::gguf::{
    Q5_0_BLOCK_ELEMENTS, Q5_0_BLOCK_SIZE, Q6_K_BLOCK_ELEMENTS, Q6_K_BLOCK_SIZE,
    Q8_0_BLOCK_ELEMENTS, Q8_0_BLOCK_SIZE, GgmlType,
};

/// Dequantize a Q8_0-packed weight row to FP32.
///
/// `weights.len()` must be a multiple of [`Q8_0_BLOCK_SIZE`]. `out`
/// must have at least `nb * Q8_0_BLOCK_ELEMENTS` floats. Returns the
/// number of floats written, or `None` on shape mismatch.
pub fn dequantize_row_q8_0(weights: &[u8], out: &mut [f32]) -> Option<usize> {
    if weights.len() % Q8_0_BLOCK_SIZE != 0 {
        return None;
    }
    let nb = weights.len() / Q8_0_BLOCK_SIZE;
    let n_out = nb.checked_mul(Q8_0_BLOCK_ELEMENTS)?;
    if out.len() < n_out {
        return None;
    }
    for b in 0..nb {
        let block = &weights[b * Q8_0_BLOCK_SIZE..(b + 1) * Q8_0_BLOCK_SIZE];
        let d = crate::slm::gguf::f16_to_f32(u16::from_le_bytes([block[0], block[1]]));
        let dst = &mut out[b * Q8_0_BLOCK_ELEMENTS..(b + 1) * Q8_0_BLOCK_ELEMENTS];
        for i in 0..Q8_0_BLOCK_ELEMENTS {
            dst[i] = d * (block[2 + i] as i8 as f32);
        }
    }
    Some(n_out)
}

// ---------------------------------------------------------------------------
// Q5_0 dequantization
// ---------------------------------------------------------------------------
//
// Block layout: { f16 d; u8 qh[4]; u8 qs[16]; } — 22 bytes per 32 elements.
// `qh` is a 32-bit packed bitfield: bit i of qh holds the 5th bit of
// element i. `qs` packs two 4-bit nibbles per byte (low nibble = element
// `i` for i < 16, high nibble = element `i + 16`). Combined 5-bit value
// (0..31) is offset by -16 to give signed range -16..15.

/// Dequantize a Q5_0-packed weight row to FP32.
pub fn dequantize_row_q5_0(weights: &[u8], out: &mut [f32]) -> Option<usize> {
    if weights.len() % Q5_0_BLOCK_SIZE != 0 {
        return None;
    }
    let nb = weights.len() / Q5_0_BLOCK_SIZE;
    let n_out = nb.checked_mul(Q5_0_BLOCK_ELEMENTS)?;
    if out.len() < n_out {
        return None;
    }
    for b in 0..nb {
        let block = &weights[b * Q5_0_BLOCK_SIZE..(b + 1) * Q5_0_BLOCK_SIZE];
        let d = crate::slm::gguf::f16_to_f32(u16::from_le_bytes([block[0], block[1]]));
        let qh = u32::from_le_bytes([block[2], block[3], block[4], block[5]]);
        let qs = &block[6..22];
        let dst = &mut out[b * Q5_0_BLOCK_ELEMENTS..(b + 1) * Q5_0_BLOCK_ELEMENTS];
        for i in 0..16 {
            let q = qs[i];
            let xh_lo = ((qh >> i) & 1) << 4;
            let xh_hi = ((qh >> (i + 16)) & 1) << 4;
            let q_lo = ((q & 0x0F) | xh_lo as u8) as i32 - 16;
            let q_hi = ((q >> 4) | xh_hi as u8) as i32 - 16;
            dst[i] = d * (q_lo as f32);
            dst[i + 16] = d * (q_hi as f32);
        }
    }
    Some(n_out)
}

// ---------------------------------------------------------------------------
// Q6_K dequantization
// ---------------------------------------------------------------------------
//
// Block layout (per llama.cpp `block_q6_K`):
//   u8  ql[128]    — low 4 bits of each weight (256 weights total)
//   u8  qh[64]     — high 2 bits, packed 4-per-byte
//   i8  scales[16] — per-16-element sub-block scales
//   f16 d          — super-block scale
//
// Total: 128 + 64 + 16 + 2 = 210 bytes per 256-element super-block.
//
// The super-block is processed in two 128-element halves. Within a
// half (32 ql bytes, 32 qh bytes, 8 scales): each `l in 0..32` decodes
// 4 weights at output positions {l, l+32, l+64, l+96}, picking up 2
// high bits from the same `qh[l]` byte (shifts 0/2/4/6) and 4 low
// bits from `ql[l]` and `ql[l+32]` (low/high nibbles). Combined 6-bit
// values are offset by -32 to yield signed range -32..31.

/// Dequantize a Q6_K-packed weight row to FP32.
pub fn dequantize_row_q6_k(weights: &[u8], out: &mut [f32]) -> Option<usize> {
    if weights.len() % Q6_K_BLOCK_SIZE != 0 {
        return None;
    }
    let nb = weights.len() / Q6_K_BLOCK_SIZE;
    let n_out = nb.checked_mul(Q6_K_BLOCK_ELEMENTS)?;
    if out.len() < n_out {
        return None;
    }
    for b in 0..nb {
        let block = &weights[b * Q6_K_BLOCK_SIZE..(b + 1) * Q6_K_BLOCK_SIZE];
        let ql_all = &block[0..128];
        let qh_all = &block[128..192];
        let scales = &block[192..208];
        let d = crate::slm::gguf::f16_to_f32(u16::from_le_bytes([block[208], block[209]]));
        let dst = &mut out[b * Q6_K_BLOCK_ELEMENTS..(b + 1) * Q6_K_BLOCK_ELEMENTS];

        for half in 0..2 {
            dequant_q6k_half(
                &ql_all[half * 64..half * 64 + 64],
                &qh_all[half * 32..half * 32 + 32],
                &scales[half * 8..half * 8 + 8],
                d,
                &mut dst[half * 128..half * 128 + 128],
            );
        }
    }
    Some(n_out)
}

/// Decode one 128-element half of a Q6_K super-block.
///
/// `ql` is 64 bytes (low-4-bit nibbles for 128 weights, packed as
/// low/high nibbles), `qh` is 32 bytes (high-2-bit pairs, 4 weights
/// per byte at shifts 0/2/4/6), `sc` is 8 signed `i8` scales (2
/// sub-blocks of 16 elements each, with the second sub-block's two
/// scales picked up at `+2`, `+4`, `+6` offsets). `d` is the f32
/// super-block scale; `y` is the 128-element output.
///
/// Each `l in 0..32` decodes 4 weights at positions `{l, l+32, l+64,
/// l+96}`. Scale bytes are signed (`as i8 as i32`); reading them
/// unsigned silently scales every output by ~63× wrong magnitude
/// (and wrong sign for negative scales) — pinned by
/// `dequantize_q6k_signed_scale_positive_output` in this module's
/// tests.
#[inline]
fn dequant_q6k_half(ql: &[u8], qh: &[u8], sc: &[u8], d: f32, y: &mut [f32]) {
    debug_assert_eq!(ql.len(), 64);
    debug_assert_eq!(qh.len(), 32);
    debug_assert_eq!(sc.len(), 8);
    debug_assert_eq!(y.len(), 128);
    for l in 0..32 {
        let is = l / 16; // sub-block index within this half (0 or 1)
        let q1 = ((ql[l] & 0x0F) | (((qh[l] >> 0) & 0x03) << 4)) as i32 - 32;
        let q2 = ((ql[l + 32] & 0x0F) | (((qh[l] >> 2) & 0x03) << 4)) as i32 - 32;
        let q3 = ((ql[l] >> 4) | (((qh[l] >> 4) & 0x03) << 4)) as i32 - 32;
        let q4 = ((ql[l + 32] >> 4) | (((qh[l] >> 6) & 0x03) << 4)) as i32 - 32;
        y[l] = d * (sc[is] as i8 as i32 as f32) * (q1 as f32);
        y[l + 32] = d * (sc[is + 2] as i8 as i32 as f32) * (q2 as f32);
        y[l + 64] = d * (sc[is + 4] as i8 as i32 as f32) * (q3 as f32);
        y[l + 96] = d * (sc[is + 6] as i8 as i32 as f32) * (q4 as f32);
    }
}

// ---------------------------------------------------------------------------
// Generic dispatcher
// ---------------------------------------------------------------------------

/// Bytes-per-row for a tensor of `n_elements` quantized as `ggml_type`.
///
/// Covers the types the M5 forward pass actually loads from real
/// GGUFs: F32, F16, Q4_K (Qwen2 demo), Q6_K (LM-head in any K_M
/// quant), Q5_0 / Q8_0 (SmolLM attention + embedding). Other types
/// return `None` — caller falls back to whatever error path it has
/// for "format not supported" (typically a diagnostic log + abort
/// the forward pass cleanly).
pub fn quant_row_bytes(ggml_type: GgmlType, n_elements: usize) -> Option<usize> {
    match ggml_type {
        GgmlType::F32 => n_elements.checked_mul(4),
        GgmlType::F16 => n_elements.checked_mul(2),
        GgmlType::Q4_K => crate::slm::gguf::q4_k_byte_size(n_elements),
        GgmlType::Q6_K => crate::slm::gguf::q6_k_byte_size(n_elements),
        GgmlType::Q5_0 => crate::slm::gguf::q5_0_byte_size(n_elements),
        GgmlType::Q8_0 => crate::slm::gguf::q8_0_byte_size(n_elements),
        _ => None,
    }
}

/// Dequantize one row of a quantized tensor to FP32.
///
/// `weights` is exactly one row's worth of bytes (size matches
/// [`quant_row_bytes`] for `ggml_type` and `out.len()` element
/// count). `out` is the FP32 destination, sized at the row's
/// element count.
///
/// F32 / F16 source types pass through with width conversion only.
/// All quant types decode block-by-block per the layouts documented
/// above their respective `dequantize_row_*` functions.
pub fn dequantize_row_any(
    ggml_type: GgmlType,
    weights: &[u8],
    out: &mut [f32],
) -> Option<usize> {
    match ggml_type {
        GgmlType::F32 => {
            if weights.len() != out.len().checked_mul(4)? {
                return None;
            }
            for i in 0..out.len() {
                out[i] = f32::from_le_bytes([
                    weights[i * 4],
                    weights[i * 4 + 1],
                    weights[i * 4 + 2],
                    weights[i * 4 + 3],
                ]);
            }
            Some(out.len())
        }
        GgmlType::F16 => {
            if weights.len() != out.len().checked_mul(2)? {
                return None;
            }
            for i in 0..out.len() {
                let bits = u16::from_le_bytes([weights[i * 2], weights[i * 2 + 1]]);
                out[i] = crate::slm::gguf::f16_to_f32(bits);
            }
            Some(out.len())
        }
        GgmlType::Q4_K => dequantize_row_q4_k(weights, out),
        GgmlType::Q6_K => dequantize_row_q6_k(weights, out),
        GgmlType::Q5_0 => dequantize_row_q5_0(weights, out),
        GgmlType::Q8_0 => dequantize_row_q8_0(weights, out),
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    extern crate alloc;
    use alloc::vec;
    use alloc::vec::Vec;

    // -- Layout constants ---------------------------------------------

    #[test]
    fn q4k_block_size_constants() {
        assert_eq!(Q4_K_BLOCK_SIZE, 144);
        assert_eq!(Q4_K_BLOCK_ELEMENTS, 256);
        assert_eq!(Q8_K_BLOCK_SIZE, 292);
        assert_eq!(Q8_K_BLOCK_ELEMENTS, 256);
    }

    #[test]
    fn q8k_block_count_and_byte_size() {
        assert_eq!(q8_k_block_count(0), 0);
        assert_eq!(q8_k_block_count(256), 1);
        assert_eq!(q8_k_block_count(512), 2);
        assert_eq!(q8_k_block_count(255), 1);
        assert_eq!(q8_k_byte_size(0), Some(0));
        assert_eq!(q8_k_byte_size(256), Some(292));
        assert_eq!(q8_k_byte_size(1536), Some(6 * 292));
    }

    // -- Test helpers -------------------------------------------------

    /// Write a Q4_K block to `dst`, given d/dmin (f16 bits) and
    /// per-sub-block scale/min raw 6-bit values.
    fn build_q4k_block(
        dst: &mut [u8; Q4_K_BLOCK_SIZE],
        d_f16: u16,
        dmin_f16: u16,
        scales: [u8; 8],
        mins: [u8; 8],
        qs: &[u8; 128],
    ) {
        // d, dmin
        dst[0..2].copy_from_slice(&d_f16.to_le_bytes());
        dst[2..4].copy_from_slice(&dmin_f16.to_le_bytes());

        // Pack 8 x 6-bit scales + 8 x 6-bit mins into 12 bytes per
        // get_scale_min_k4. Inverse of the unpacking in
        // Q4KBlockView::scale / Q4KBlockView::min:
        //   For i < 4: scales[i] = (sc & 0x3F) | ((sc[i+4] high 2 bits) << 6)
        //              scales[i+4] (low nibble) = mn(i) low 4
        //              scales[i+4] (high nibble) = mn(i+4) low 4 (after combine)
        //   We reconstruct the 12 bytes with the canonical encoding
        //   used by GGML reference quantizers.
        let mut s = [0u8; 12];
        for i in 0..4 {
            // Low 6 bits of scale i
            s[i] = scales[i] & 0x3F;
            // Low 6 bits of min i
            s[i + 4] = mins[i] & 0x3F;
        }
        for i in 4..8 {
            // scales[i+4] low 4 = scale(i) low 4
            // scales[i+4] high 4 = min(i) low 4
            s[i + 4] = (scales[i] & 0x0F) | ((mins[i] & 0x0F) << 4);
            // top 2 bits of scales[i+4] (i in 4..8) sit in:
            //   scales[i-4] high 2 bits encode scale(i) bits 4..6
            //   scales[i] high 2 bits encode min(i) bits 4..6
            s[i - 4] |= ((scales[i] >> 4) & 0x3) << 6;
            s[i] |= ((mins[i] >> 4) & 0x3) << 6;
        }
        dst[4..16].copy_from_slice(&s);
        dst[16..144].copy_from_slice(qs);
    }

    fn f16_bits_for(v: f32) -> u16 {
        // Round-to-nearest-even f32→f16 — sufficient for our test
        // values (1.0, 0.5, etc. are exact).
        let bits = v.to_bits();
        let sign = ((bits >> 31) & 0x1) as u16;
        let exp = (((bits >> 23) & 0xFF) as i32) - 127 + 15;
        let mant = bits & 0x7F_FFFF;
        if exp <= 0 {
            // Subnormal/zero
            return sign << 15;
        }
        if exp >= 0x1F {
            // Inf/NaN
            return (sign << 15) | (0x1F << 10);
        }
        (sign << 15) | ((exp as u16) << 10) | ((mant >> 13) as u16)
    }

    // -- Q4_K dequant -------------------------------------------------

    #[test]
    fn dequantize_q4k_zero_block() {
        // d=0, dmin=0, all qs=0 → all output zero.
        let block = [0u8; Q4_K_BLOCK_SIZE];
        let mut out = vec![1.0f32; Q4_K_BLOCK_ELEMENTS];
        let n = dequantize_row_q4_k(&block, &mut out).expect("ok");
        assert_eq!(n, Q4_K_BLOCK_ELEMENTS);
        for &v in out.iter() {
            assert_eq!(v, 0.0);
        }
    }

    #[test]
    fn dequantize_q4k_known_block() {
        // Hand-craft a Q4_K block:
        //   d    = 0.5
        //   dmin = 0.25
        //   scales = [1, 2, 3, 4, 5, 6, 7, 8]
        //   mins   = [10, 11, 12, 13, 14, 15, 16, 17]
        //   qs[k] = k mod 16 in low nibble, (k+1) mod 16 in high nibble
        //
        // For super-block layout: j∈0..4 covers 64 outputs.
        //   is = 2j, 2j+1
        //   d1 = d * sc(is),   m1 = dmin * mn(is)
        //   d2 = d * sc(is+1), m2 = dmin * mn(is+1)
        //   qs_base = j * 32
        //   for l in 0..32:
        //     out[64j + l]      = d1 * (qs[qs_base+l] & 0xF) - m1
        //     out[64j + 32 + l] = d2 * (qs[qs_base+l] >> 4) - m2
        let scales = [1u8, 2, 3, 4, 5, 6, 7, 8];
        let mins = [10u8, 11, 12, 13, 14, 15, 16, 17];
        let mut qs = [0u8; 128];
        for k in 0..128 {
            let lo = (k as u8) & 0x0F;
            let hi = ((k as u8 + 1) & 0x0F) << 4;
            qs[k] = lo | hi;
        }

        let d = 0.5f32;
        let dmin = 0.25f32;
        let mut block = [0u8; Q4_K_BLOCK_SIZE];
        build_q4k_block(
            &mut block,
            f16_bits_for(d),
            f16_bits_for(dmin),
            scales,
            mins,
            &qs,
        );

        // Sanity: re-read scales/mins through the view to confirm
        // packing matches unpacking.
        let view = Q4KBlockView::from_slice(&block).unwrap();
        for i in 0..8 {
            assert_eq!(view.scale(i), scales[i], "scale {} round-trip", i);
            assert_eq!(view.min(i), mins[i], "min {} round-trip", i);
        }
        assert_eq!(view.d(), d);
        assert_eq!(view.dmin(), dmin);

        let mut out = vec![0.0f32; Q4_K_BLOCK_ELEMENTS];
        let n = dequantize_row_q4_k(&block, &mut out).unwrap();
        assert_eq!(n, Q4_K_BLOCK_ELEMENTS);

        // Compute expected via the formula and check.
        for j in 0..4 {
            let is = j * 2;
            let d1 = d * scales[is] as f32;
            let m1 = dmin * mins[is] as f32;
            let d2 = d * scales[is + 1] as f32;
            let m2 = dmin * mins[is + 1] as f32;
            for l in 0..32 {
                let q = qs[j * 32 + l];
                let exp_lo = d1 * ((q & 0x0F) as f32) - m1;
                let exp_hi = d2 * ((q >> 4) as f32) - m2;
                let got_lo = out[j * 64 + l];
                let got_hi = out[j * 64 + 32 + l];
                assert!(
                    (got_lo - exp_lo).abs() < 1e-3,
                    "low j={} l={}: got {} expected {}",
                    j,
                    l,
                    got_lo,
                    exp_lo
                );
                assert!(
                    (got_hi - exp_hi).abs() < 1e-3,
                    "high j={} l={}: got {} expected {}",
                    j,
                    l,
                    got_hi,
                    exp_hi
                );
            }
        }
    }

    #[test]
    fn dequantize_q4k_rejects_bad_sizes() {
        let bad = [0u8; 143];
        let mut out = vec![0.0f32; 256];
        assert!(dequantize_row_q4_k(&bad, &mut out).is_none());

        let block = [0u8; Q4_K_BLOCK_SIZE];
        let mut tiny = [0.0f32; 100];
        assert!(dequantize_row_q4_k(&block, &mut tiny).is_none());
    }

    // -- Q8_K quant ---------------------------------------------------

    /// Tiny inline dequantizer used only by Q8_K round-trip tests.
    fn dequantize_q8k(block: &[u8], out: &mut [f32]) {
        let d = q8k_d(block);
        for j in 0..Q8_K_BLOCK_ELEMENTS {
            out[j] = d * (q8k_qs(block, j) as f32);
        }
    }

    #[test]
    fn quantize_q8k_zero_row() {
        let floats = vec![0.0f32; Q8_K_BLOCK_ELEMENTS];
        let mut out = vec![0xAAu8; Q8_K_BLOCK_SIZE];
        let n = quantize_row_q8_k(&floats, &mut out).unwrap();
        assert_eq!(n, Q8_K_BLOCK_SIZE);
        // All-zero block: d, qs, bsums all zero.
        assert_eq!(q8k_d(&out), 0.0);
        for j in 0..Q8_K_BLOCK_ELEMENTS {
            assert_eq!(q8k_qs(&out, j), 0);
        }
        for j in 0..16 {
            assert_eq!(q8k_bsum(&out, j), 0);
        }
    }

    #[test]
    fn quantize_q8k_round_trip_close() {
        // Build a deterministic input row in [-1, 1].
        let mut floats = vec![0.0f32; Q8_K_BLOCK_ELEMENTS];
        for j in 0..Q8_K_BLOCK_ELEMENTS {
            // Mix of magnitudes so amax isn't trivial.
            let phase = (j as f32) * 0.05;
            floats[j] = phase.sin() * 0.7;
        }

        let mut packed = vec![0u8; Q8_K_BLOCK_SIZE];
        quantize_row_q8_k(&floats, &mut packed).unwrap();

        // Round-trip via the inline dequantizer.
        let mut got = vec![0.0f32; Q8_K_BLOCK_ELEMENTS];
        dequantize_q8k(&packed, &mut got);

        // Find amax to compute relative error tolerance.
        let mut amax = 0.0f32;
        for &v in &floats {
            if v.abs() > amax {
                amax = v.abs();
            }
        }
        // Q8_K resolution is amax / 127 per element. Allow 1.5
        // resolution units (~1.18%) of slop per element.
        let tol = 1.5 * amax / 127.0;
        for j in 0..Q8_K_BLOCK_ELEMENTS {
            let err = (got[j] - floats[j]).abs();
            assert!(
                err <= tol,
                "j={}: got {}, want {}, err {} > tol {}",
                j,
                got[j],
                floats[j],
                err,
                tol
            );
        }

        // bsums consistency: bsums[j] == sum_{l=0..16} qs[16*j + l].
        for j in 0..16 {
            let mut sum: i32 = 0;
            for l in 0..16 {
                sum += q8k_qs(&packed, j * 16 + l) as i32;
            }
            assert_eq!(q8k_bsum(&packed, j) as i32, sum, "bsums[{}]", j);
        }
    }

    #[test]
    fn quantize_q8k_rejects_bad_sizes() {
        let floats = vec![0.0f32; 100]; // not multiple of 256
        let mut out = vec![0u8; 1024];
        assert!(quantize_row_q8_k(&floats, &mut out).is_none());

        let floats = vec![0.0f32; Q8_K_BLOCK_ELEMENTS];
        let mut tiny = vec![0u8; 10];
        assert!(quantize_row_q8_k(&floats, &mut tiny).is_none());
    }

    // -- Vec dot ------------------------------------------------------

    #[test]
    fn vec_dot_q4k_q8k_known() {
        // Hand-craft matched Q4_K (weights) + Q8_K (acts) blocks.
        // Compare against an independent dequant + element-wise
        // multiply reference.

        // Weights: d=0.5, dmin=0.25, scales=[1..8], mins=[2..9],
        // qs ascending.
        let scales = [1u8, 2, 3, 4, 5, 6, 7, 8];
        let mins = [2u8, 3, 4, 5, 6, 7, 8, 9];
        let mut qs = [0u8; 128];
        for k in 0..128 {
            qs[k] = ((k & 0x0F) as u8) | (((k + 3) & 0x0F) as u8) << 4;
        }
        let mut w_block = [0u8; Q4_K_BLOCK_SIZE];
        let d_w = 0.5f32;
        let dmin_w = 0.25f32;
        build_q4k_block(
            &mut w_block,
            f16_bits_for(d_w),
            f16_bits_for(dmin_w),
            scales,
            mins,
            &qs,
        );

        // Activations: build a known FP32 row, quantize to Q8_K.
        let mut a_floats = vec![0.0f32; Q8_K_BLOCK_ELEMENTS];
        for j in 0..Q8_K_BLOCK_ELEMENTS {
            let phase = (j as f32) * 0.07 + 0.1;
            a_floats[j] = phase.cos() * 0.9;
        }
        let mut a_block = vec![0u8; Q8_K_BLOCK_SIZE];
        quantize_row_q8_k(&a_floats, &mut a_block).unwrap();

        // Reference dot: dequantize weights, then sum w * dequant_a.
        let mut w_floats = vec![0.0f32; Q4_K_BLOCK_ELEMENTS];
        dequantize_row_q4_k(&w_block, &mut w_floats).unwrap();
        let mut a_dq = vec![0.0f32; Q8_K_BLOCK_ELEMENTS];
        dequantize_q8k(&a_block, &mut a_dq);
        let mut expected = 0.0f64;
        for j in 0..Q4_K_BLOCK_ELEMENTS {
            expected += (w_floats[j] as f64) * (a_dq[j] as f64);
        }

        let got = vec_dot_q4_k_q8_k(&w_block, &a_block).unwrap();

        // Match within FP32 epsilon scaled by row norms. 1e-2
        // absolute is more than tight enough for the magnitudes
        // here (scales/mins capped at 9, amax ~ 0.9).
        assert!(
            (got as f64 - expected).abs() < 1e-2,
            "dot mismatch: got {} expected {}",
            got,
            expected
        );
    }

    #[test]
    fn vec_dot_q4k_q8k_multi_block() {
        // 2 super-blocks of each. Verify the per-block accumulator
        // wires correctly.
        let nb = 2;

        // Build weights with simple per-block patterns.
        let mut w = vec![0u8; nb * Q4_K_BLOCK_SIZE];
        let mut a_floats = vec![0.0f32; nb * Q8_K_BLOCK_ELEMENTS];

        for b in 0..nb {
            let scales = [1u8, 1, 1, 1, 1, 1, 1, 1];
            let mins = [0u8; 8];
            let mut qs = [0u8; 128];
            for k in 0..128 {
                qs[k] = ((b as u8 + 1) << 4) | (b as u8 + 1);
            }
            let mut block = [0u8; Q4_K_BLOCK_SIZE];
            build_q4k_block(
                &mut block,
                f16_bits_for(1.0),
                f16_bits_for(0.0),
                scales,
                mins,
                &qs,
            );
            w[b * Q4_K_BLOCK_SIZE..(b + 1) * Q4_K_BLOCK_SIZE].copy_from_slice(&block);

            // Set act floats so each block is non-zero and unique.
            for j in 0..Q8_K_BLOCK_ELEMENTS {
                a_floats[b * Q8_K_BLOCK_ELEMENTS + j] = ((b + 1) as f32) * 0.1
                    * (((j as i32) - 128) as f32 / 128.0);
            }
        }
        let mut a = vec![0u8; nb * Q8_K_BLOCK_SIZE];
        quantize_row_q8_k(&a_floats, &mut a).unwrap();

        // Reference: dequantize all, do the dot in f64.
        let mut w_dq = vec![0.0f32; nb * Q4_K_BLOCK_ELEMENTS];
        dequantize_row_q4_k(&w, &mut w_dq).unwrap();
        let mut a_dq = vec![0.0f32; nb * Q8_K_BLOCK_ELEMENTS];
        for b in 0..nb {
            let blk = &a[b * Q8_K_BLOCK_SIZE..(b + 1) * Q8_K_BLOCK_SIZE];
            let dst = &mut a_dq[b * Q8_K_BLOCK_ELEMENTS..(b + 1) * Q8_K_BLOCK_ELEMENTS];
            dequantize_q8k(blk, dst);
        }
        let mut expected = 0.0f64;
        for j in 0..nb * Q4_K_BLOCK_ELEMENTS {
            expected += (w_dq[j] as f64) * (a_dq[j] as f64);
        }

        let got = vec_dot_q4_k_q8_k(&w, &a).unwrap();
        assert!(
            (got as f64 - expected).abs() < 5e-2,
            "multi-block dot mismatch: got {} expected {}",
            got,
            expected
        );
    }

    #[test]
    fn vec_dot_q4k_q8k_rejects_mismatched_shapes() {
        // 1 weight block, 2 act blocks.
        let w = vec![0u8; Q4_K_BLOCK_SIZE];
        let a = vec![0u8; 2 * Q8_K_BLOCK_SIZE];
        assert!(vec_dot_q4_k_q8_k(&w, &a).is_none());

        // weights not multiple of 144.
        let bad_w = vec![0u8; 143];
        let a = vec![0u8; Q8_K_BLOCK_SIZE];
        assert!(vec_dot_q4_k_q8_k(&bad_w, &a).is_none());

        // acts not multiple of 292.
        let w = vec![0u8; Q4_K_BLOCK_SIZE];
        let bad_a = vec![0u8; 291];
        assert!(vec_dot_q4_k_q8_k(&w, &bad_a).is_none());
    }

    #[test]
    fn nearest_int_rounds_half_away_from_zero() {
        assert_eq!(nearest_int(0.0), 0);
        assert_eq!(nearest_int(0.4), 0);
        assert_eq!(nearest_int(0.5), 1);
        assert_eq!(nearest_int(-0.4), 0);
        assert_eq!(nearest_int(-0.5), -1);
        assert_eq!(nearest_int(127.0), 127);
        assert_eq!(nearest_int(-128.0), -128);
    }

    // -- Q6_K dequant -------------------------------------------------

    #[test]
    fn dequantize_q6k_signed_scale_positive_output() {
        // Regression for the signed-scale bug: sc bytes were cast
        // `as i32` (unsigned) instead of `as i8 as i32`. A scale of
        // -1 stored as 0xFF should give positive output when the
        // quant values are negative, not a huge negative.
        //
        // Block layout: ql=0, qh=0, d=1.0 →
        //   every 6-bit quant = (0 | 0) - 32 = -32.
        // With scales[0] = -1 (0xFF):
        //   y[0] = d * (-1) * (-32) = +32.0  (correct, signed)
        //   y[0] = d * 255 * (-32) = -8160   (wrong, unsigned)
        let mut block = [0u8; Q6_K_BLOCK_SIZE];
        // d = 1.0 (f16 0x3C00) at bytes 208..210.
        block[208] = 0x00;
        block[209] = 0x3C;
        // scales[0] = -1 stored as 0xFF (i8 two's complement).
        block[192] = 0xFF;
        // All other scales = 1 for sanity.
        for i in 1..16 {
            block[192 + i] = 0x01;
        }
        // ql=0, qh=0 → all 6-bit quants = 0|0 - 32 = -32.

        let mut out = vec![0.0f32; Q6_K_BLOCK_ELEMENTS];
        let n = dequantize_row_q6_k(&block, &mut out).expect("dequant ok");
        assert_eq!(n, Q6_K_BLOCK_ELEMENTS);

        // y[0] should use scales[0]=-1, q=-32 → +32.0.
        assert!(
            (out[0] - 32.0).abs() < 1e-3,
            "signed scale -1 × q=-32 should give +32, got {}",
            out[0]
        );

        // y[32] uses scales[2]=1 (since is = l/16 = 0 for l in 0..15,
        // scale index = is + 2 = 2). scales[2] = 0x01 = +1.
        // y[32] = d * 1 * (-32) = -32.
        assert!(
            (out[32] - (-32.0)).abs() < 1e-3,
            "positive scale +1 × q=-32 should give -32, got {}",
            out[32]
        );
    }

    // -- Black-hole consume to keep multi_block result alive in
    //    builds without core::hint::black_box being a no-op.
    #[allow(dead_code)]
    fn use_result(v: f32) -> Vec<f32> {
        vec![v; 1]
    }
}
