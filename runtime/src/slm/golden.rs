//! Golden-vector checks against upstream llama.cpp / ggml (#475, DoD item 7).
//!
//! The vectors live in `scripts/fixtures/slm/` and come from an external
//! reference, never from this crate; `PROVENANCE.md` there records the
//! llama.cpp commit and `scripts/tools/slm-gen-goldens.py` regenerates them.
//!
//! Compiled into the kernel as well as the host tests because the ops take
//! different code paths per architecture (the NEON Q4_K kernel only exists on
//! aarch64), and `make runtime-test` only ever runs the x86-64 host paths.
//! `kernel/tests/test_slm_golden.c` hands the fixtures in from `.incbin`, so
//! production kernels carry this code but not the data.
//!
//! # Tolerances
//!
//! SLM-OS keeps activations in FP16 where the reference uses F32, so each
//! comparison allows what the storage type forces and no more:
//!
//! - FP16 outputs: half an FP16 ULP of the expected value (the rounding of a
//!   correct result into FP16), plus [`F32_NOISE`] for F32 accumulation order.
//! - Dequantized weights: [`F32_NOISE`]. Both sides compute `d * q` in F32 and
//!   differ at most in evaluation order.
//! - Q8_K bytes: exact. Integer output of the same algorithm.
//! - Q4_K dots: [`DOT_NOISE`], absolute. The same integers on both sides,
//!   summed per block in F32, so the error scales with the block values and
//!   not with the final sum, which may cancel.
//! - Q6_K dots: [`Q6K_DOT_NOISE`] against an exact dot over ggml's dequantized
//!   weights, since `matmul_quant_rows` takes an F32 dot there. Against ggml's
//!   own Q6_K result, which pushes the activation through Q8_K, the bound is
//!   `sum |w_j| * step_j / 2`, with `step` the Q8_K step of `x_j`'s block.
//! - End-to-end logits: [`LOGIT_TOLERANCE`], see there.

#![cfg(feature = "slm")]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

use crate::inference::ops_transformer::{
    gqa_decode_step, lm_head_q, matmul_quant_rows, rmsnorm, swiglu_elementwise, RopePairing,
    RopeTable,
};
use crate::inference::quant::{
    dequantize_row_q4_k, dequantize_row_q6_k, q8_k_byte_size, quantize_row_q8_k,
    vec_dot_q4_k_q8_k,
};
use crate::slm::forward::{forward_one, project_with_bias, ForwardScratch};
use crate::slm::gguf::{f16_to_f32, f32_to_f16, GgmlType};
use crate::slm::{registry, sampler::Sampler, session::Session};

/// Relative allowance for F32 evaluation-order differences: a few ULP (F32
/// ULP is 1.2e-7) across the short sums in these ops.
const F32_NOISE: f32 = 2e-6;

/// Absolute allowance on Q4_K dots: blocks here have magnitude near 2, where
/// an F32 ULP is 2.4e-7, and the two sides sum the blocks in different orders.
const DOT_NOISE: f32 = 1e-6;

/// Absolute allowance on an F32 dot of 512 terms with partial sums near 2:
/// worst case 512 additions at half an F32 ULP (1.2e-7) each is 3e-5.
const Q6K_DOT_NOISE: f32 = 3e-5;

/// Allowance on the largest per-logit error against llama.cpp over a pass.
///
/// llama.cpp keeps hidden state in F32 and K/V in F16; SLM-OS keeps all of it
/// in FP16. That is not what sets this number: FP32 activations show the same
/// error on the real model, and llama.cpp's scalar and AVX2 builds differ from
/// each other by as much. Rounding noise of about 1 F32 ULP reaches each
/// matmul's Q8_K step (`max / 127`), where a bin flip turns it into a whole
/// step, and the flips add up layer over layer. See "Numeric fidelity against
/// llama.cpp" in `docs/design/slm-integration.md`. With correct RoPE the
/// measured maxima over the 10 fixture steps run 0.25 to 0.73 on logits of
/// about +/-18 (`golden_measure_logit_error`). The wrong RoPE pairing reaches
/// 10.9 by the third step.
pub const LOGIT_TOLERANCE: f32 = 1.0;

/// Allowance on one step's RMS logit error, for errors spread too thinly for
/// [`LOGIT_TOLERANCE`] to see. Measured with correct RoPE: 0.08 to 0.25.
pub const LOGIT_RMS_TOLERANCE: f32 = 0.35;

/// The same two allowances at position 0, where only one pass of noise has
/// accumulated. Measured: max 0.25, RMS 0.085.
pub const FIRST_STEP_TOLERANCE: f32 = 0.40;
pub const FIRST_STEP_RMS_TOLERANCE: f32 = 0.13;

/// Where a golden comparison first failed.
#[derive(Debug, Clone)]
pub struct Mismatch {
    pub what: String,
    pub index: usize,
    pub got: f32,
    pub want: f32,
    pub tolerance: f32,
}

fn mismatch(what: &str, index: usize, got: f32, want: f32, tolerance: f32) -> Mismatch {
    Mismatch { what: String::from(what), index, got, want, tolerance }
}

fn malformed(what: &str) -> Mismatch {
    mismatch(what, 0, f32::NAN, f32::NAN, 0.0)
}

/// Parsed `SGR1` record container, see `scripts/fixtures/slm/PROVENANCE.md`.
pub struct Records<'a> {
    data: &'a [u8],
}

impl<'a> Records<'a> {
    pub fn new(data: &'a [u8]) -> Option<Self> {
        if data.len() < 8 || &data[..4] != b"SGR1" {
            return None;
        }
        Some(Self { data })
    }

    pub fn get(&self, name: &str) -> Option<&'a [u8]> {
        let count = read_u32(self.data, 4)? as usize;
        let mut p = 8usize;
        for _ in 0..count {
            let name_len = read_u32(self.data, p)? as usize;
            p = p.checked_add(4)?;
            let name_end = p.checked_add(name_len)?;
            let this_name = self.data.get(p..name_end)?;
            let len = read_u32(self.data, name_end)? as usize;
            let body = name_end.checked_add(4)?;
            let body_end = body.checked_add(len)?;
            let bytes = self.data.get(body..body_end)?;
            if this_name == name.as_bytes() {
                return Some(bytes);
            }
            p = body.checked_add(len.checked_add(3)? & !3)?;
        }
        None
    }

    pub(crate) fn f32s(&self, name: &str) -> Result<Vec<f32>, Mismatch> {
        let bytes = self.get(name).ok_or_else(|| malformed(name))?;
        Ok(bytes
            .chunks_exact(4)
            .map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect())
    }

    pub(crate) fn u32s(&self, name: &str) -> Result<Vec<u32>, Mismatch> {
        let bytes = self.get(name).ok_or_else(|| malformed(name))?;
        Ok(bytes
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect())
    }

    /// F32 values the generator rounded to exact FP16.
    fn fp16s(&self, name: &str) -> Result<Vec<u16>, Mismatch> {
        let v = self.f32s(name)?;
        let mut out = Vec::with_capacity(v.len());
        for (i, &x) in v.iter().enumerate() {
            let h = f32_to_f16(x);
            if f16_to_f32(h) != x {
                return Err(mismatch(name, i, f16_to_f32(h), x, 0.0));
            }
            out.push(h);
        }
        Ok(out)
    }

    fn bytes(&self, name: &str) -> Result<&'a [u8], Mismatch> {
        self.get(name).ok_or_else(|| malformed(name))
    }
}

fn read_u32(data: &[u8], at: usize) -> Option<u32> {
    let end = at.checked_add(4)?;
    let b = data.get(at..end)?;
    Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// Half of one FP16 ULP at `v`'s magnitude.
fn fp16_half_ulp(v: f32) -> f32 {
    const SUBNORMAL_HALF_ULP: f32 = 2.980_232_2e-8; // 2^-25
    let e = ((v.abs().to_bits() >> 23) & 0xff) as i32 - 127;
    if e < -14 {
        return SUBNORMAL_HALF_ULP;
    }
    f32::from_bits(((e - 11 + 127) as u32) << 23)
}

fn compare_fp16(
    what: &str,
    got: &[u16],
    want: &[f32],
    abs_extra: f32,
) -> Result<(), Mismatch> {
    if got.len() != want.len() {
        return Err(malformed(what));
    }
    for i in 0..want.len() {
        let g = f16_to_f32(got[i]);
        let tol = 1.01 * fp16_half_ulp(want[i]) + F32_NOISE * want[i].abs() + abs_extra;
        if !((g - want[i]).abs() <= tol) {
            return Err(mismatch(what, i, g, want[i], tol));
        }
    }
    Ok(())
}

fn compare_f32(what: &str, got: &[f32], want: &[f32], rel: f32, abs: f32) -> Result<(), Mismatch> {
    if got.len() != want.len() {
        return Err(malformed(what));
    }
    for i in 0..want.len() {
        let tol = rel * want[i].abs() + abs;
        if !((got[i] - want[i]).abs() <= tol) {
            return Err(mismatch(what, i, got[i], want[i], tol));
        }
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Single ops
// ---------------------------------------------------------------------------

pub fn check_rmsnorm(r: &Records<'_>) -> Result<(), Mismatch> {
    let x = r.fp16s("rmsnorm.x")?;
    let gamma = r.fp16s("rmsnorm.gamma")?;
    let want = r.f32s("rmsnorm.out")?;
    let mut out = vec![0u16; x.len()];
    rmsnorm(&x, &gamma, 1e-6, &mut out).ok_or_else(|| malformed("rmsnorm"))?;
    compare_fp16("rmsnorm", &out, &want, 0.0)
}

/// Rotates `rope.x` per head at each position. Inputs up to about 5, so the
/// trig factors' F32 error appears as an absolute term (cos/sin differ by a
/// ULP or two between libm and ggml's `cosf`).
pub fn check_rope(r: &Records<'_>) -> Result<(), Mismatch> {
    let x = r.fp16s("rope.x")?;
    let params = r.f32s("rope.params")?;
    let positions = r.f32s("rope.positions")?;
    let head_dim = params[0] as usize;
    let n_head = params[1] as usize;
    let max_pos = positions.iter().fold(0.0f32, |a, &b| a.max(b)) as usize + 1;
    let table = RopeTable::new(head_dim, max_pos, params[2], RopePairing::HalfSplit);
    for (i, &pos) in positions.iter().enumerate() {
        let want = r.f32s(&format!("rope.out.{i}"))?;
        let mut v = x.clone();
        for h in 0..n_head {
            table.apply(pos as usize, &mut v[h * head_dim..(h + 1) * head_dim]);
        }
        compare_fp16(&format!("rope pos={}", pos as usize), &v, &want, 3e-6)?;
    }
    Ok(())
}

pub fn check_gqa(r: &Records<'_>) -> Result<(), Mismatch> {
    let dims = r.f32s("gqa.dims")?;
    let (n_q, n_kv, dim, seq) =
        (dims[0] as usize, dims[1] as usize, dims[2] as usize, dims[3] as usize);
    let q = r.fp16s("gqa.q")?;
    let k = r.fp16s("gqa.k")?;
    let v = r.fp16s("gqa.v")?;
    let want = r.f32s("gqa.out")?;
    let mut scratch = vec![0.0f32; seq];
    let mut out = vec![0u16; n_q * dim];
    gqa_decode_step(&q, &k, &v, n_q, n_kv, dim, seq, &mut scratch, &mut out)
        .ok_or_else(|| malformed("gqa"))?;
    compare_fp16("gqa", &out, &want, 3e-6)
}

/// `silu(gate) * up`. The product of two rounded factors (`silu` writes FP16,
/// the product rounds again), so the error is the output's half ULP plus
/// `up` times the half ULP of `silu(gate)`; ggml's `silu` is itself accurate
/// to about 1e-6 relative.
pub fn check_swiglu(r: &Records<'_>) -> Result<(), Mismatch> {
    let gate = r.fp16s("swiglu.gate")?;
    let up = r.fp16s("swiglu.up")?;
    let want = r.f32s("swiglu.out")?;
    let mut g = gate.clone();
    let n = g.len();
    swiglu_elementwise(&mut g, &up, n);
    for i in 0..n {
        let got = f16_to_f32(g[i]);
        let up_v = f16_to_f32(up[i]);
        let silu_v = if up_v != 0.0 { want[i] / up_v } else { 0.0 };
        let tol = 1.01 * fp16_half_ulp(want[i])
            + 1.01 * up_v.abs() * fp16_half_ulp(silu_v)
            + F32_NOISE * want[i].abs();
        if !((got - want[i]).abs() <= tol) {
            return Err(mismatch("swiglu", i, got, want[i], tol));
        }
    }
    Ok(())
}

pub fn check_q4k_dequant(r: &Records<'_>) -> Result<(), Mismatch> {
    let bytes = r.bytes("q4k.bytes")?;
    let want = r.f32s("q4k.dequant")?;
    let mut got = vec![0.0f32; want.len()];
    dequantize_row_q4_k(bytes, &mut got).ok_or_else(|| malformed("q4k dequant"))?;
    compare_f32("q4k dequant", &got, &want, F32_NOISE, 0.0)
}

pub fn check_q6k_dequant(r: &Records<'_>) -> Result<(), Mismatch> {
    let bytes = r.bytes("q6k.bytes")?;
    let want = r.f32s("q6k.dequant")?;
    let mut got = vec![0.0f32; want.len()];
    dequantize_row_q6_k(bytes, &mut got).ok_or_else(|| malformed("q6k dequant"))?;
    compare_f32("q6k dequant", &got, &want, F32_NOISE, 0.0)
}

pub fn check_q8k_quantize(r: &Records<'_>) -> Result<(), Mismatch> {
    let x = r.f32s("quant.x")?;
    let want = r.bytes("q8k.bytes")?;
    let mut got = vec![0u8; q8_k_byte_size(x.len()).ok_or_else(|| malformed("q8k size"))?];
    quantize_row_q8_k(&x, &mut got).ok_or_else(|| malformed("q8k quantize"))?;
    if got.len() != want.len() {
        return Err(malformed("q8k length"));
    }
    for i in 0..want.len() {
        if got[i] != want[i] {
            return Err(mismatch("q8k byte", i, got[i] as f32, want[i] as f32, 0.0));
        }
    }
    Ok(())
}

fn quant_dims(r: &Records<'_>) -> Result<(usize, usize), Mismatch> {
    let d = r.f32s("quant.dims")?;
    Ok((d[0] as usize, d[1] as usize))
}

/// The dedicated kernel: NEON `sdot` on aarch64, scalar elsewhere.
pub fn check_vec_dot(r: &Records<'_>) -> Result<(), Mismatch> {
    let (cols, rows) = quant_dims(r)?;
    let x = r.f32s("quant.x")?;
    let q4 = r.bytes("q4k.bytes")?;
    let want = r.f32s("q4k.dot")?;
    let mut acts = vec![0u8; q8_k_byte_size(cols).ok_or_else(|| malformed("q8k size"))?];
    quantize_row_q8_k(&x, &mut acts).ok_or_else(|| malformed("q8k quantize"))?;
    let row_bytes = q4.len() / rows;
    let mut got = vec![0.0f32; rows];
    for i in 0..rows {
        got[i] = vec_dot_q4_k_q8_k(&q4[i * row_bytes..(i + 1) * row_bytes], &acts)
            .ok_or_else(|| malformed("vec_dot"))?;
    }
    compare_f32("vec_dot q4k*q8k", &got, &want, 0.0, DOT_NOISE)
}

/// `lm_head_q` on both weight types real GGUFs use. Q4_K goes through the same
/// integer path as ggml, so [`DOT_NOISE`] holds; Q6_K takes the F32 dot, see
/// the module doc for its bound.
pub fn check_lm_head(r: &Records<'_>) -> Result<(), Mismatch> {
    let (cols, rows) = quant_dims(r)?;
    let x = r.fp16s("quant.x")?;
    let x_f32 = r.f32s("quant.x")?;
    let mut q8 = vec![0u8; q8_k_byte_size(cols).ok_or_else(|| malformed("q8k size"))?];
    let mut dequant = vec![0.0f32; cols];
    let mut acts = vec![0.0f32; cols];
    let mut got = vec![0.0f32; rows];

    let q4 = r.bytes("q4k.bytes")?;
    lm_head_q(&x, GgmlType::Q4_K, q4, cols, rows, &mut q8, &mut dequant, &mut acts, &mut got)
        .ok_or_else(|| malformed("lm_head q4k"))?;
    let want = r.f32s("q4k.dot")?;
    compare_f32("lm_head q4k", &got, &want, 0.0, DOT_NOISE)?;

    let q6 = r.bytes("q6k.bytes")?;
    lm_head_q(&x, GgmlType::Q6_K, q6, cols, rows, &mut q8, &mut dequant, &mut acts, &mut got)
        .ok_or_else(|| malformed("lm_head q6k"))?;
    let exact = r.f32s("q6k.dot_exact")?;
    compare_f32("lm_head q6k exact", &got, &exact, 0.0, Q6K_DOT_NOISE)?;
    let want = r.f32s("q6k.dot")?;
    let weights = r.f32s("q6k.dequant")?;
    for row in 0..rows {
        let mut bound = 0.0f32;
        for block in 0..cols / 256 {
            let range = block * 256..(block + 1) * 256;
            let max = x_f32[range.clone()].iter().fold(0.0f32, |a, &b| a.max(b.abs()));
            let w_abs: f32 =
                weights[row * cols..][range].iter().map(|w| w.abs()).sum();
            bound += w_abs * 0.5 * max / 127.0;
        }
        let tol = bound + F32_NOISE * want[row].abs();
        if !((got[row] - want[row]).abs() <= tol) {
            return Err(mismatch("lm_head q6k", row, got[row], want[row], tol));
        }
    }

    // And the dispatcher the projections use, on the same Q4_K weights.
    matmul_quant_rows(GgmlType::Q4_K, q4, rows, cols, &x, &mut q8, &mut dequant, &mut acts, &mut got)
        .ok_or_else(|| malformed("matmul q4k"))?;
    let want = r.f32s("q4k.dot")?;
    compare_f32("matmul q4k", &got, &want, 0.0, DOT_NOISE)
}

/// The production Q/K/V projection, [`project_with_bias`], with its own
/// scratch. `name` is the projection's name in the fixture, unused here.
fn production_projection(
    _name: &str,
    ty: GgmlType,
    weights: &[u8],
    rows: usize,
    x: &[u16],
    bias: Option<&[u8]>,
    out: &mut [u16],
) -> Option<()> {
    let cols = x.len();
    let mut q8 = vec![0u8; q8_k_byte_size(cols)?];
    let (mut mm, mut dequant, mut acts) = (Vec::new(), Vec::new(), Vec::new());
    project_with_bias(ty, weights, rows, cols, x, bias, &mut q8, &mut mm, &mut dequant, &mut acts, out)
}

/// Q, K and V with their biases, `matmul(x, W) + b` rounded to FP16, as
/// `forward_one` builds them (`golden_qkv_bias.rec`, #1165). The reference is
/// ggml's `ggml_add(ggml_mul_mat(W, x), b)`, llama.cpp's Qwen2 graph. Q4_K takes
/// the same integer Q8_K path on both sides, so [`DOT_NOISE`] plus the FP16
/// rounding of the result, as `check_lm_head`'s Q4_K. Q6_K takes an F32 dot
/// here: [`Q6K_DOT_NOISE`] against an exact dot plus the bias, and the Q8_K
/// bound (module doc) against ggml's own result.
pub fn check_qkv_bias(r: &Records<'_>) -> Result<(), Mismatch> {
    check_qkv_bias_with(r, &mut production_projection)
}

/// [`check_qkv_bias`] on any projection, so a test can show that a wrong one
/// (bias dropped, misindexed, another projection's) fails it.
pub(crate) fn check_qkv_bias_with(
    r: &Records<'_>,
    project: &mut dyn FnMut(&str, GgmlType, &[u8], usize, &[u16], Option<&[u8]>, &mut [u16]) -> Option<()>,
) -> Result<(), Mismatch> {
    let x = r.fp16s("qkv_bias.x")?;
    let x_f32 = r.f32s("qkv_bias.x")?;
    let names = core::str::from_utf8(r.bytes("qkv_bias.names")?)
        .map_err(|_| malformed("qkv_bias.names"))?;
    let blocks = x.len() / 256;
    if blocks == 0 || x.len() % 256 != 0 {
        return Err(malformed("qkv_bias.x"));
    }
    let mut checked = 0usize;
    for name in names.split(',') {
        let field = |f: &str| format!("qkv_bias.{name}.{f}");
        let ty = GgmlType(*r.f32s(&field("type"))?.first().ok_or_else(|| malformed("qkv type"))? as u32);
        let weights = r.bytes(&field("w"))?;
        let bias = r.bytes(&field("b"))?;
        let want = r.f32s(&field("out"))?;
        let rows = want.len();
        if bias.len() != rows * 4 {
            return Err(malformed(&field("b")));
        }
        let mut got = vec![0u16; rows];
        project(name, ty, weights, rows, &x, Some(bias), &mut got)
            .ok_or_else(|| malformed(&field("projection")))?;
        let label = format!("qkv_bias {name}");
        if ty == GgmlType::Q4_K {
            compare_fp16(&label, &got, &want, DOT_NOISE)?;
        } else if ty == GgmlType::Q6_K {
            let exact = r.f32s(&field("out_exact"))?;
            compare_fp16(&format!("{label} exact"), &got, &exact, Q6K_DOT_NOISE)?;
            let wabs = r.f32s(&field("wabs"))?;
            if wabs.len() != rows * blocks {
                return Err(malformed(&field("wabs")));
            }
            for row in 0..rows {
                let mut bound = 0.0f32;
                for block in 0..blocks {
                    let max = x_f32[block * 256..(block + 1) * 256]
                        .iter()
                        .fold(0.0f32, |a, &b| a.max(b.abs()));
                    bound += wabs[row * blocks + block] * 0.5 * max / 127.0;
                }
                let g = f16_to_f32(got[row]);
                let tol = 1.01 * fp16_half_ulp(want[row]) + bound + F32_NOISE * want[row].abs();
                if !((g - want[row]).abs() <= tol) {
                    return Err(mismatch(&label, row, g, want[row], tol));
                }
            }
        } else {
            return Err(malformed(&field("type")));
        }
        checked += 1;
    }
    if checked == 0 {
        return Err(malformed("qkv_bias.names"));
    }
    Ok(())
}

/// Every single-op check.
pub fn check_ops(r: &Records<'_>) -> Result<(), Mismatch> {
    check_rmsnorm(r)?;
    check_rope(r)?;
    check_gqa(r)?;
    check_swiglu(r)?;
    check_q4k_dequant(r)?;
    check_q6k_dequant(r)?;
    check_q8k_quantize(r)?;
    check_vec_dot(r)?;
    check_lm_head(r)
}

// ---------------------------------------------------------------------------
// End to end
// ---------------------------------------------------------------------------

fn argmax(row: &[f32]) -> u32 {
    let mut best = 0usize;
    for i in 1..row.len() {
        if row[i] > row[best] {
            best = i;
        }
    }
    best as u32
}

/// Drive `forward_one` over `ids` then greedily, the same steps llama.cpp
/// took: `logits` has one row per decoded token, `tokens` the argmax picks.
fn check_steps(
    model: u32,
    what: &str,
    ids: &[u32],
    tokens: &[u32],
    logits: &[f32],
    n_vocab: usize,
    tolerance: f32,
    rms_tolerance: f32,
) -> Result<(), Mismatch> {
    let mut session = Session::open(model, 64, Sampler::Greedy, 0)
        .ok_or_else(|| malformed("session open"))?;
    let mut scratch =
        ForwardScratch::new(&session.arch, session.max_ctx, session.vocab_size as usize);
    let steps = logits.len() / n_vocab;
    let mut token = ids[0];
    let mut generated = 0usize;
    for step in 0..steps {
        registry::with_loaded_slm(model as usize, |slm| {
            forward_one(&mut session, slm, token, &mut scratch)
        })
        .flatten()
        .ok_or_else(|| malformed("forward_one"))?;
        let got = &scratch.logits[..n_vocab];
        let want = &logits[step * n_vocab..(step + 1) * n_vocab];
        let label = format!("{what} logits step {step}");
        compare_f32(&label, got, want, 0.0, tolerance)?;
        let mut sum = 0.0f32;
        for i in 0..n_vocab {
            sum += (got[i] - want[i]) * (got[i] - want[i]);
        }
        let rms = crate::inference::mathf::sqrtf(sum / n_vocab as f32);
        if !(rms <= rms_tolerance) {
            return Err(mismatch(&format!("{label} rms"), step, rms, 0.0, rms_tolerance));
        }
        if step + 1 < ids.len() {
            token = ids[step + 1];
            continue;
        }
        let pick = argmax(got);
        if pick != tokens[generated] {
            return Err(mismatch(
                &format!("{what} token {generated}"),
                step,
                pick as f32,
                tokens[generated] as f32,
                0.0,
            ));
        }
        generated += 1;
        token = pick;
    }
    Ok(())
}

/// One token at position 0. RoPE is the identity there and the attention
/// softmax has a single entry, so this covers the path from the embedding
/// through the V and output projections, residuals, norms, the MLP and the LM
/// head, and says nothing about Q, K, RoPE or the softmax: [`check_greedy`]
/// covers those.
pub fn check_first_step(model: u32, r: &Records<'_>) -> Result<(), Mismatch> {
    let ids = r.u32s("first.ids")?;
    let tokens = r.u32s("first.tokens")?;
    let logits = r.f32s("first.logits")?;
    let n_vocab = r.u32s("greedy.n_vocab")?[0] as usize;
    check_steps(
        model,
        "first",
        &ids,
        &tokens,
        &logits,
        n_vocab,
        FIRST_STEP_TOLERANCE,
        FIRST_STEP_RMS_TOLERANCE,
    )
}

/// The prompt and the greedy run, with every step's logits. Multi-position,
/// so this is what catches wrong RoPE pairing (#1156).
pub fn check_greedy(model: u32, r: &Records<'_>) -> Result<(), Mismatch> {
    let ids = r.u32s("prompt.ids")?;
    let tokens = r.u32s("greedy.tokens")?;
    let logits = r.f32s("greedy.logits")?;
    let n_vocab = r.u32s("greedy.n_vocab")?[0] as usize;
    check_steps(
        model,
        "greedy",
        &ids,
        &tokens,
        &logits,
        n_vocab,
        LOGIT_TOLERANCE,
        LOGIT_RMS_TOLERANCE,
    )
}

// ---------------------------------------------------------------------------
// The real model on hardware (`slm golden`, #1165)
// ---------------------------------------------------------------------------
//
// The real Qwen2.5-1.5B GGUF is 1.1 GB, too big for CI, so its goldens run on
// a board after a UEFI-direct boot, on the model `/boot/models` delivers. The
// fixture (`scripts/fixtures/slm/qwen25_real_golden.rec`, made on the host by
// `scripts/tools/slm-gen-goldens.py --only real-model`) holds cases, each a
// prompt, llama.cpp's greedy continuation of it and, for every step that
// predicts one of those tokens, the full row of logits from the exact CPU
// reference: the GPU program's own reference with exact dot products, run on
// the host (ADR 0004). The board builds the GPU program as `slm launch --gpu
// on` does and drives it with `GpuProgram::run`, the call a session's decode
// makes, over the same tokens, teacher-forced. Each step must have every logit
// within `GOLDEN_EXACT_TOLERANCE` (0.1) of that row and the same top token as
// llama.cpp. The `long` case's prompt is 64 tokens, so in batch mode it goes
// through one prefill step of 64 rows, the most the program takes.

/// Feed each prompt as one prefill batch, as `slm prompt` does, instead of one
/// token at a time. Keep in step with `slm_shell.c`.
pub const REAL_FLAG_BATCH: u32 = 1;
/// Deliberate mutation: zero the Q, K and V biases of the layer the fixture
/// names in the loaded weights the GPU program maps, for this run, then put
/// them back.
pub const REAL_FLAG_DROP_BIAS: u32 = 2;

/// Context the program is built with, as `slm launch --ctx 512`.
const REAL_CTX: usize = 512;

/// Keep in step with `SlmRealGoldenReport` in `kernel/src/slm_shell.c`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RealGoldenReport {
    /// Steps compared over all cases: one per token of llama.cpp's
    /// continuations.
    pub steps: u32,
    /// Steps whose top logit is llama.cpp's greedy token.
    pub top_matches: u32,
    /// First step (counted over all cases) with a logit more than 0.1 from
    /// the exact reference, or -1.
    pub first_bad_step: i32,
    pub first_bad_index: i32,
    /// Thousandths, since kernel C has no floating point.
    pub first_bad_got_milli: i32,
    pub first_bad_want_milli: i32,
    /// Largest error against the exact reference over all steps, in thousandths.
    pub max_err_milli: i32,
    /// First step whose top token isn't llama.cpp's, or -1.
    pub first_top_miss_step: i32,
    /// Bytes the mutation zeroed, 0 without one.
    pub mutated_bytes: u32,
    /// 1 once a mutation was undone and the bytes read back as they were.
    pub restored: u32,
    /// Cases run.
    pub cases: u32,
    /// Most rows one GPU program step took (the largest prefill batch).
    pub max_rows: u32,
    /// GPU time per decode token in the last case.
    pub gpu_us_per_token: u64,
}

impl RealGoldenReport {
    /// Every step within the tolerance with llama.cpp's top token.
    pub fn passed(&self) -> bool {
        self.steps > 0
            && self.first_bad_step < 0
            && self.first_top_miss_step < 0
            && self.top_matches == self.steps
    }
}

/// One case of the fixture.
struct RealCase<'a> {
    name: &'a str,
    ids: Vec<u32>,
    tokens: Vec<u32>,
    margins: Vec<f32>,
    offset: Vec<f32>,
    scale: Vec<f32>,
    /// `steps * n_vocab` little-endian i16, row-major.
    q: &'a [u8],
}

/// The fixture, parsed and checked for consistency.
struct RealFixture<'a> {
    cases: Vec<RealCase<'a>>,
    n_vocab: usize,
    model_bytes: usize,
    mutate_layer: u32,
}

impl<'a> RealFixture<'a> {
    fn parse(r: &Records<'a>) -> Result<Self, &'static str> {
        let bad = |_| "record file is missing fields";
        let one = |name: &str| -> Result<u32, &'static str> {
            r.u32s(name).map_err(bad)?.first().copied().ok_or("record file is missing fields")
        };
        let n_vocab = one("real.n_vocab")? as usize;
        let model_bytes = one("real.model.bytes")? as usize;
        let mutate_layer = one("real.mutate.layer")?;
        let names = core::str::from_utf8(r.bytes("real.cases").map_err(bad)?)
            .map_err(|_| "record file is missing fields")?;
        let mut cases = Vec::new();
        for name in names.split(',') {
            let field = |f: &str| format!("real.{name}.{f}");
            let ids = r.u32s(&field("prompt.ids")).map_err(bad)?;
            let tokens = r.u32s(&field("tokens")).map_err(bad)?;
            let margins = r.f32s(&field("margins")).map_err(bad)?;
            let offset = r.f32s(&field("exact.offset")).map_err(bad)?;
            let scale = r.f32s(&field("exact.scale")).map_err(bad)?;
            let q = r.bytes(&field("exact.q")).map_err(bad)?;
            let steps = tokens.len();
            if ids.is_empty()
                || steps == 0
                || margins.len() != steps
                || offset.len() != steps
                || scale.len() != steps
                || Some(q.len()) != steps.checked_mul(n_vocab).and_then(|n| n.checked_mul(2))
            {
                return Err("record file fields disagree");
            }
            // The last generated token is never fed, so the program sees this many.
            if ids.len() + steps - 1 > REAL_CTX {
                return Err("record file is longer than the program's context");
            }
            cases.push(RealCase { name, ids, tokens, margins, offset, scale, q });
        }
        if n_vocab == 0 || cases.is_empty() {
            return Err("record file fields disagree");
        }
        Ok(Self { cases, n_vocab, model_bytes, mutate_layer })
    }
}

impl RealCase<'_> {
    fn want(&self, n_vocab: usize, step: usize, i: usize) -> f32 {
        let at = (step * n_vocab + i) * 2;
        let q = i16::from_le_bytes([self.q[at], self.q[at + 1]]);
        self.offset[step] + self.scale[step] * q as f32
    }
}

/// Zeroed bias bytes and what they held.
struct BiasMutation {
    model: usize,
    saved: Vec<(usize, Vec<u8>)>,
}

impl BiasMutation {
    /// Zero `blk.<layer>.attn_{q,k,v}.bias` in the loaded weights, under the
    /// registry lock, which every CPU forward pass holds while it reads them.
    /// Refused while a GPU program maps the weights: it reads them without
    /// that lock. The programs this run builds afterwards map the zeroed
    /// bytes: each GPU step ends with an L2 flush and sysmem invalidate, and
    /// the bytes are cleaned to memory here.
    fn apply(model: usize, layer: u32) -> Result<Self, &'static str> {
        let saved = registry::with_loaded_slm(model, |slm| {
            // Under the slot lock, which every new pin takes too.
            if registry::gpu_pinned(model) {
                return Err("a GPU program maps this model; close its sessions first");
            }
            let base = slm.weight_base_for_mutation().ok_or("model has no weight bytes")?;
            let start = slm.weight_bytes().ok_or("model has no weight bytes")?.as_ptr() as usize;
            let mut saved = Vec::new();
            for which in ["q", "k", "v"] {
                let name = format!("blk.{layer}.attn_{which}.bias");
                let bytes = slm.tensor_bytes(&name).ok_or("the model has no such bias")?;
                saved.push((bytes.as_ptr() as usize - start, bytes.to_vec()));
            }
            for (off, bytes) in &saved {
                // SAFETY: `off..off + len` is a tensor inside the weight
                // buffer, `base` is that buffer with write access, and the
                // registry lock keeps every CPU reader out while it changes.
                // No GPU program maps it (checked above).
                unsafe {
                    let p = base.as_ptr().add(*off);
                    core::ptr::write_bytes(p, 0, bytes.len());
                    crate::kernel_ffi::slm_gpu_sync_for_device(p, bytes.len());
                }
            }
            Ok(saved)
        })
        .ok_or("model not loaded")??;
        let m = Self { model, saved };
        let line = format!(
            "[slm] real golden: drop-bias zeroed blk.{layer}.attn_q/k/v.bias ({} bytes) in the weights the GPU program maps\n\0",
            m.bytes()
        );
        crate::kernel_ffi::print(line.as_bytes());
        Ok(m)
    }

    /// Put the bytes back and read them back. True when they match.
    fn restore(self) -> bool {
        registry::with_loaded_slm(self.model, |slm| {
            let Some(base) = slm.weight_base_for_mutation() else {
                return false;
            };
            for (off, bytes) in &self.saved {
                // SAFETY: as in `apply`; the programs that read the zeroed
                // bytes have been dropped, so no GPU mapping is left.
                unsafe {
                    let p = base.as_ptr().add(*off);
                    core::ptr::copy_nonoverlapping(bytes.as_ptr(), p, bytes.len());
                    crate::kernel_ffi::slm_gpu_sync_for_device(p, bytes.len());
                }
            }
            let Some(all) = slm.weight_bytes() else {
                return false;
            };
            self.saved.iter().all(|(off, bytes)| &all[*off..*off + bytes.len()] == bytes.as_slice())
        })
        .unwrap_or(false)
    }

    fn bytes(&self) -> u32 {
        self.saved.iter().map(|(_, b)| b.len() as u32).sum()
    }
}

/// Run every case of the fixture on `model` through the GPU program and
/// compare (see the section comment). `Err` when a run itself failed; a
/// mismatch is `Ok` with `rep` saying where ([`RealGoldenReport::passed`]).
pub fn real_golden_run(
    model: u32,
    rec: &[u8],
    flags: u32,
    rep: &mut RealGoldenReport,
) -> Result<(), &'static str> {
    *rep = RealGoldenReport { first_bad_step: -1, first_bad_index: -1, first_top_miss_step: -1, ..Default::default() };
    let r = Records::new(rec).ok_or("bad record file")?;
    let fx = RealFixture::parse(&r)?;
    let info = registry::get_info(model as usize).ok_or("model not loaded")?;
    if info.vocab_size as usize != fx.n_vocab {
        return Err("record file is for another model");
    }
    let len = registry::with_loaded_slm(model as usize, |slm| slm.weight_bytes().map(|b| b.len()))
        .flatten()
        .ok_or("model has no weight bytes")?;
    if len != fx.model_bytes {
        return Err("the loaded model is not the file the record was made from");
    }
    let mutation = if flags & REAL_FLAG_DROP_BIAS != 0 {
        let m = BiasMutation::apply(model as usize, fx.mutate_layer)?;
        rep.mutated_bytes = m.bytes();
        Some(m)
    } else {
        None
    };
    let mut result = Ok(());
    let mut max_err = 0.0f32;
    for case in &fx.cases {
        result = real_run_case(model, &fx, case, flags & REAL_FLAG_BATCH != 0, &mut max_err, rep);
        if result.is_err() {
            break;
        }
        rep.cases += 1;
    }
    if let Some(m) = mutation {
        rep.restored = m.restore() as u32;
        if rep.restored == 0 {
            return Err("the mutated biases did not read back as they were");
        }
    }
    result
}

fn real_run_case(
    model: u32,
    fx: &RealFixture<'_>,
    case: &RealCase<'_>,
    batched: bool,
    max_err: &mut f32,
    rep: &mut RealGoldenReport,
) -> Result<(), &'static str> {
    use crate::inference::ops_transformer::{RopePairing, RopeTable};
    use crate::slm::gpu_program::{BuildError, GpuProgram, GOLDEN_EXACT_TOLERANCE};

    let (head_dim, base, arch) = registry::with_loaded_slm(model as usize, |slm| {
        (slm.arch().head_dim, slm.arch().rope_freq_base, slm.arch().architecture)
    })
    .ok_or("model not loaded")?;
    let rope = RopeTable::new(head_dim as usize, REAL_CTX, base, RopePairing::for_arch(arch));
    let vocab = fx.n_vocab as u32;
    // Read and pin under the registry lock, build without it, as
    // `Session::choose_path` does for `slm launch --gpu on`.
    let prep = registry::with_loaded_slm(model as usize, |slm| GpuProgram::prepare(slm, vocab))
        .ok_or("model not loaded")?;
    let mut prog = prep
        .and_then(|i| GpuProgram::build_prepared(i, REAL_CTX, &rope.cos_sin))
        .map_err(|e| match e {
            BuildError::NoBackend => "no SLM-OS-built GPU channel (needs a UEFI-direct boot)",
            BuildError::Failed(w) => w,
        })?;
    let p = case.ids.len();
    let line = format!(
        "[slm] real golden {}: GPU program(launches={} barriers={} batch={} arena={}MB), ctx {REAL_CTX}, prompt {p} tokens {}, {} steps\n\0",
        case.name,
        prog.launches,
        prog.barriers,
        prog.max_batch,
        prog.data_bytes >> 20,
        if batched { "in one prefill step" } else { "one decode step each" },
        case.tokens.len(),
    );
    crate::kernel_ffi::print(line.as_bytes());

    let first_step = rep.steps;
    let mut compare = |step: usize, out: &[f32], rep: &mut RealGoldenReport| -> Result<(), &'static str> {
        if out.len() != fx.n_vocab {
            return Err("the program returned a short row");
        }
        let global = (first_step as usize + step) as i32;
        let (mut step_max, mut at) = (0.0f32, 0usize);
        let mut top = 0usize;
        for i in 0..fx.n_vocab {
            let want = case.want(fx.n_vocab, step, i);
            let e = (out[i] - want).abs();
            // NaN fails both tests.
            if !(e <= step_max) {
                step_max = e;
                at = i;
            }
            if !(e <= GOLDEN_EXACT_TOLERANCE) && rep.first_bad_step < 0 {
                rep.first_bad_step = global;
                rep.first_bad_index = i as i32;
                rep.first_bad_got_milli = (out[i] * 1000.0) as i32;
                rep.first_bad_want_milli = (want * 1000.0) as i32;
            }
            if out[i] > out[top] {
                top = i;
            }
        }
        let want_top = case.tokens[step];
        if top as u32 == want_top {
            rep.top_matches += 1;
        } else if rep.first_top_miss_step < 0 {
            rep.first_top_miss_step = global;
        }
        if !(step_max <= *max_err) {
            *max_err = step_max;
        }
        rep.max_err_milli = if max_err.is_finite() { (*max_err * 1000.0) as i32 } else { i32::MAX };
        rep.steps += 1;
        let line = format!(
            "[slm] real golden {} step {step}: max err {} milli at {at}, top {top} want {want_top} (llama.cpp margin {} milli)\n\0",
            case.name,
            if step_max.is_finite() { (step_max * 1000.0) as i32 } else { i32::MAX },
            (case.margins[step] * 1000.0) as i32,
        );
        crate::kernel_ffi::print(line.as_bytes());
        Ok(())
    };

    let mut out = Vec::new();
    if batched {
        if p < 2 || (prog.max_batch as usize) < p {
            return Err("the prompt does not fit one prefill batch");
        }
        prog.run(&case.ids, 0, &mut out)?;
        rep.max_rows = rep.max_rows.max(p as u32);
    } else {
        for (i, &t) in case.ids.iter().enumerate() {
            prog.run(core::slice::from_ref(&t), i, &mut out)?;
        }
        rep.max_rows = rep.max_rows.max(1);
    }
    compare(0, &out, rep)?;
    for step in 1..case.tokens.len() {
        prog.run(core::slice::from_ref(&case.tokens[step - 1]), p + step - 1, &mut out)?;
        compare(step, &out, rep)?;
    }
    rep.gpu_us_per_token = prog.gpu_us_per_token();
    let line = format!(
        "[slm] real golden {}: {} GPU steps, {} tokens, {} us per decode token\n\0",
        case.name, prog.steps, prog.tokens, rep.gpu_us_per_token,
    );
    crate::kernel_ffi::print(line.as_bytes());
    Ok(())
}

/// Run the real-model golden from C (`slm golden`). Returns 0 when it ran
/// (pass or fail is in `out`), -1 on a bad argument, -2 when it could not
/// run, with the reason on the console.
///
/// # Safety
/// `rec` must point to `rec_len` bytes that stay readable and unchanged for
/// the life of the kernel (the embedded fixture), and `out` to a writable
/// [`RealGoldenReport`].
#[no_mangle]
pub unsafe extern "C" fn rust_slm_golden_real(
    model: u32,
    rec: *const u8,
    rec_len: usize,
    flags: u32,
    out: *mut RealGoldenReport,
) -> i32 {
    if rec.is_null() || rec_len == 0 || out.is_null() {
        return -1;
    }
    // SAFETY: the caller's contract: static, immutable data.
    let rec: &'static [u8] = unsafe { core::slice::from_raw_parts(rec, rec_len) };
    // SAFETY: the caller's contract.
    let rep = unsafe { &mut *out };
    match real_golden_run(model, rec, flags, rep) {
        Ok(()) => 0,
        Err(why) => {
            let line = format!("[slm] real golden: {why}\n\0");
            crate::kernel_ffi::print(line.as_bytes());
            -2
        }
    }
}

// ---------------------------------------------------------------------------
// FFI for kernel/tests/test_slm_golden.c
// ---------------------------------------------------------------------------

fn report(m: &Mismatch) {
    let line = format!(
        "[slm-golden] {} [{}]: got {} want {} (tolerance {})\n\0",
        m.what, m.index, m.got, m.want, m.tolerance
    );
    // SAFETY: the buffer ends in the NUL pushed just above and outlives the call.
    unsafe {
        crate::kernel_ffi::uart_puts(line.as_ptr());
    }
}

fn ffi_result(r: Result<(), Mismatch>) -> i32 {
    match r {
        Ok(()) => 0,
        Err(m) => {
            report(&m);
            1
        }
    }
}

/// # Safety
/// `rec` must point to `rec_len` readable bytes.
unsafe fn records_from_raw<'a>(rec: *const u8, rec_len: usize) -> Option<Records<'a>> {
    if rec.is_null() || rec_len == 0 {
        return None;
    }
    // SAFETY: the caller's contract.
    Records::new(unsafe { core::slice::from_raw_parts(rec, rec_len) })
}

/// Which check [`rust_slm_golden_check`] runs. Mirrored in test_slm_golden.c.
const GOLDEN_OPS: u32 = 0;
const GOLDEN_FIRST_STEP: u32 = 1;
const GOLDEN_GREEDY: u32 = 2;
const GOLDEN_ROPE: u32 = 3;
const GOLDEN_QKV_BIAS: u32 = 4;

/// Run one golden check from C. Returns 0 on a match, 1 on a mismatch (printed
/// on the UART), -1 on a bad argument. `model` is ignored by the op checks.
///
/// # Safety
/// `rec` must point to `rec_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rust_slm_golden_check(
    which: u32,
    model: u32,
    rec: *const u8,
    rec_len: usize,
) -> i32 {
    // SAFETY: the caller's contract.
    let Some(r) = (unsafe { records_from_raw(rec, rec_len) }) else {
        return -1;
    };
    match which {
        GOLDEN_OPS => ffi_result(check_ops(&r)),
        GOLDEN_FIRST_STEP => ffi_result(check_first_step(model, &r)),
        GOLDEN_GREEDY => ffi_result(check_greedy(model, &r)),
        GOLDEN_ROPE => ffi_result(check_rope(&r)),
        GOLDEN_QKV_BIAS => ffi_result(check_qkv_bias(&r)),
        _ => -1,
    }
}

/// Copy record `name` (NUL-terminated) into `out`, returning its byte length,
/// or -1 if it is absent or does not fit.
///
/// # Safety
/// `rec` must point to `rec_len` readable bytes, `name` must be NUL-terminated
/// and `out` must point to `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rust_slm_golden_record(
    rec: *const u8,
    rec_len: usize,
    name: *const core::ffi::c_char,
    out: *mut u8,
    cap: usize,
) -> i32 {
    // SAFETY: the caller's contract.
    let Some(r) = (unsafe { records_from_raw(rec, rec_len) }) else {
        return -1;
    };
    if name.is_null() || out.is_null() {
        return -1;
    }
    // SAFETY: the caller's contract.
    let Ok(name) = (unsafe { core::ffi::CStr::from_ptr(name) }).to_str() else {
        return -1;
    };
    let Some(bytes) = r.get(name) else {
        return -1;
    };
    if bytes.len() > cap || bytes.len() > i32::MAX as usize {
        return -1;
    }
    // SAFETY: `out` has `cap >= bytes.len()` writable bytes, and it is a
    // caller buffer, not the fixture.
    unsafe {
        core::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len());
    }
    bytes.len() as i32
}

/// Open a greedy session. C test files build with `-mgeneral-regs-only`, so
/// they cannot pass the two `f32` arguments of `rust_slm_session_open`.
#[no_mangle]
pub extern "C" fn rust_slm_golden_open_greedy(model: u32, max_ctx: u32) -> i32 {
    crate::rust_slm_session_open(model, max_ctx, 0, 0.0, 0, 0.0, 0)
}

// ---------------------------------------------------------------------------
// Host tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slm::chat_template::{encode_chat, ChatTurn, Role};
    use crate::slm::gguf::Gguf;
    use crate::slm::registry::{
        ensure_mm_initialized_for_tests, load_slm, reset_for_tests,
    };
    use crate::slm::tokenizer::Bbpe;

    const OPS: &[u8] = include_bytes!("../../../scripts/fixtures/slm/golden_ops.rec");
    const QKV: &[u8] = include_bytes!("../../../scripts/fixtures/slm/golden_qkv_bias.rec");
    const TINY: &[u8] = include_bytes!("../../../scripts/fixtures/slm/tiny_qwen2.gguf");
    const GREEDY: &[u8] = include_bytes!("../../../scripts/fixtures/slm/tiny_qwen2_greedy.rec");
    const TOK_CASES: &str = include_str!("../../../scripts/fixtures/slm/tokenizer_goldens.tsv");

    fn ops() -> Records<'static> {
        Records::new(OPS).expect("golden_ops.rec")
    }

    fn greedy_rec() -> Records<'static> {
        Records::new(GREEDY).expect("tiny_qwen2_greedy.rec")
    }

    fn load_tiny() -> u32 {
        ensure_mm_initialized_for_tests();
        reset_for_tests();
        load_slm(b"tiny-qwen2", TINY).expect("tiny_qwen2.gguf loads") as u32
    }

    fn pass(r: Result<(), Mismatch>) {
        if let Err(m) = r {
            panic!("{m:?}");
        }
    }

    #[test]
    fn golden_rmsnorm() {
        pass(check_rmsnorm(&ops()));
    }

    #[test]
    fn golden_rope_position_zero_is_identity() {
        // Holds under either pairing convention, so it stays on while the
        // whole check below is ignored.
        let r = ops();
        let x = r.fp16s("rope.x").unwrap();
        let params = r.f32s("rope.params").unwrap();
        let (head_dim, n_head) = (params[0] as usize, params[1] as usize);
        let table = RopeTable::new(head_dim, 1, params[2], RopePairing::HalfSplit);
        let mut v = x.clone();
        for h in 0..n_head {
            table.apply(0, &mut v[h * head_dim..(h + 1) * head_dim]);
        }
        assert_eq!(v, x);
        assert_eq!(r.f32s("rope.positions").unwrap()[0], 0.0);
        pass(compare_fp16("rope pos=0", &v, &r.f32s("rope.out.0").unwrap(), 0.0));
    }

    #[test]
    fn golden_rope() {
        pass(check_rope(&ops()));
    }

    #[test]
    fn golden_gqa() {
        pass(check_gqa(&ops()));
    }

    #[test]
    fn golden_swiglu() {
        pass(check_swiglu(&ops()));
    }

    #[test]
    fn golden_q4k_dequant() {
        pass(check_q4k_dequant(&ops()));
    }

    #[test]
    fn golden_q6k_dequant() {
        pass(check_q6k_dequant(&ops()));
    }

    #[test]
    fn golden_q8k_quantize() {
        pass(check_q8k_quantize(&ops()));
    }

    #[test]
    fn golden_vec_dot() {
        pass(check_vec_dot(&ops()));
    }

    #[test]
    fn golden_lm_head() {
        pass(check_lm_head(&ops()));
    }

    fn qkv() -> Records<'static> {
        Records::new(QKV).expect("golden_qkv_bias.rec")
    }

    #[test]
    fn golden_qkv_bias() {
        pass(check_qkv_bias(&qkv()));
    }

    /// The vector catches the bias defects it is there for, in every
    /// projection on its own: the bias dropped, read one row off, read
    /// back to front, or another projection's bias added.
    #[test]
    fn golden_qkv_bias_catches_wrong_bias() {
        let r = qkv();
        let names: Vec<String> = core::str::from_utf8(r.get("qkv_bias.names").unwrap())
            .unwrap()
            .split(',')
            .map(String::from)
            .collect();
        assert_eq!(names, ["q", "k", "v4", "v6"]);
        let other = |name: &str| -> Vec<u8> {
            // K and V share a width; Q's is wider, so it takes K's twice.
            let src = if name == "k" { "v4" } else { "k" };
            let b = r.get(&format!("qkv_bias.{src}.b")).unwrap();
            b.iter().cycle().take(r.get(&format!("qkv_bias.{name}.b")).unwrap().len()).copied().collect()
        };
        type Mutant = fn(&[u8]) -> Option<Vec<u8>>;
        let mutants: [(&str, Mutant); 3] = [
            ("dropped", |_| None),
            ("one row off", |b| {
                let mut v = b.to_vec();
                v.rotate_left(4);
                Some(v)
            }),
            ("reversed", |b| Some(b.chunks_exact(4).rev().flatten().copied().collect())),
        ];
        for target in &names {
            for (what, mutant) in mutants {
                let mut project = |name: &str, ty, w: &[u8], rows, x: &[u16], b: Option<&[u8]>, out: &mut [u16]| {
                    if name != target.as_str() {
                        return production_projection(name, ty, w, rows, x, b, out);
                    }
                    let m = mutant(b?);
                    production_projection(name, ty, w, rows, x, m.as_deref(), out)
                };
                let err = check_qkv_bias_with(&r, &mut project);
                let m = err.expect_err(&format!("{target}: bias {what} passed"));
                assert!(m.what.contains(target.as_str()), "{target} {what}: {m:?}");
            }
            let swapped = other(target);
            let mut project = |name: &str, ty, w: &[u8], rows, x: &[u16], b: Option<&[u8]>, out: &mut [u16]| {
                let b = if name == target.as_str() { Some(swapped.as_slice()) } else { b };
                production_projection(name, ty, w, rows, x, b, out)
            };
            let m = check_qkv_bias_with(&r, &mut project)
                .expect_err(&format!("{target}: another projection's bias passed"));
            assert!(m.what.contains(target.as_str()), "{target} swapped: {m:?}");
        }
    }

    // ---- the real-model golden's harness (`slm golden`, #1165) -------------

    /// `SGR1` records, sorted by name as the Python generator writes them.
    fn write_records(mut recs: Vec<(String, Vec<u8>)>) -> Vec<u8> {
        recs.sort_by(|a, b| a.0.cmp(&b.0));
        let mut out = b"SGR1".to_vec();
        out.extend_from_slice(&(recs.len() as u32).to_le_bytes());
        for (name, data) in recs {
            out.extend_from_slice(&(name.len() as u32).to_le_bytes());
            out.extend_from_slice(name.as_bytes());
            out.extend_from_slice(&(data.len() as u32).to_le_bytes());
            out.extend_from_slice(&data);
            out.resize(out.len() + (((data.len() + 3) & !3) - data.len()), 0);
        }
        out
    }

    fn u32_bytes(v: &[u32]) -> Vec<u8> {
        v.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    fn f32_bytes(v: &[f32]) -> Vec<u8> {
        v.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    /// Largest error the fixture's 16-bit rows may add to the exact
    /// reference: 1% of the 0.1 the board allows.
    const REAL_STORE_ERROR: f32 = 1e-3;

    /// One case of the fixture: a prompt and llama.cpp's greedy run on it.
    struct LlamaCase {
        name: String,
        text: Vec<u8>,
        ids: Vec<u32>,
        tokens: Vec<u32>,
        margins: Vec<f32>,
    }

    /// The fixture `slm golden` reads, made on the host. For each case the GPU
    /// program runs on the software GPU, which executes the exact CPU
    /// reference (`exact: true`), over the prompt and llama.cpp's tokens, one
    /// token at a time, teacher-forced; each step that predicts one of those
    /// tokens keeps its full row of logits, stored as i16 with a per-row
    /// offset and scale. Panics if the exact reference's top token is not
    /// llama.cpp's at some step, since the board could then never pass.
    fn make_real_fixture(model: u32, cases: &[LlamaCase], mutate_layer: u32) -> Vec<u8> {
        use crate::inference::ops_transformer::{RopePairing, RopeTable};
        use crate::slm::gpu_program::{soft, GpuProgram};
        soft::reset_knobs();
        let (head_dim, base, arch, len) = registry::with_loaded_slm(model as usize, |slm| {
            let a = slm.arch();
            (a.head_dim, a.rope_freq_base, a.architecture, slm.weight_bytes().unwrap().len())
        })
        .unwrap();
        let vocab = registry::get_info(model as usize).unwrap().vocab_size;
        let rope = RopeTable::new(head_dim as usize, REAL_CTX, base, RopePairing::for_arch(arch));
        let names: Vec<&str> = cases.iter().map(|c| c.name.as_str()).collect();
        let mut recs: Vec<(String, Vec<u8>)> = vec![
            ("real.cases".into(), names.join(",").into_bytes()),
            ("real.n_vocab".into(), u32_bytes(&[vocab])),
            ("real.model.bytes".into(), u32_bytes(&[u32::try_from(len).unwrap()])),
            ("real.mutate.layer".into(), u32_bytes(&[mutate_layer])),
        ];
        for c in cases {
            let prep = registry::with_loaded_slm(model as usize, |slm| GpuProgram::prepare(slm, vocab)).unwrap();
            let Ok(mut prog) = prep.and_then(|i| GpuProgram::build_prepared(i, REAL_CTX, &rope.cos_sin)) else {
                panic!("the software GPU did not build the program");
            };
            let mut out = Vec::new();
            let mut rows = Vec::new();
            for (i, &t) in c.ids.iter().enumerate() {
                prog.run(&[t], i, &mut out).unwrap();
            }
            rows.push(out.clone());
            for s in 1..c.tokens.len() {
                prog.run(&[c.tokens[s - 1]], c.ids.len() + s - 1, &mut out).unwrap();
                rows.push(out.clone());
            }
            drop(prog);

            let (mut offset, mut scale, mut q) = (Vec::new(), Vec::new(), Vec::new());
            let (mut top, mut top_margin) = (Vec::new(), Vec::new());
            for (s, row) in rows.iter().enumerate() {
                let lo = row.iter().fold(f32::INFINITY, |a, &b| a.min(b));
                let hi = row.iter().fold(f32::NEG_INFINITY, |a, &b| a.max(b));
                assert!(lo.is_finite() && hi.is_finite(), "{} step {s}: non-finite logits", c.name);
                let o = 0.5 * (lo + hi);
                let k = ((hi - lo) / 65534.0).max(f32::MIN_POSITIVE);
                for &v in row {
                    let n = libm::roundf((v - o) / k).clamp(-32767.0, 32767.0) as i16;
                    let back = o + k * n as f32;
                    assert!((back - v).abs() <= REAL_STORE_ERROR, "{} step {s}: stored {back} for {v}", c.name);
                    q.extend_from_slice(&n.to_le_bytes());
                }
                offset.push(o);
                scale.push(k);
                let t = argmax(row);
                let mut second = f32::NEG_INFINITY;
                for (i, &v) in row.iter().enumerate() {
                    if i as u32 != t && v > second {
                        second = v;
                    }
                }
                assert_eq!(
                    t, c.tokens[s],
                    "{} step {s}: the exact reference's top token is not llama.cpp's (margin {})",
                    c.name, c.margins[s]
                );
                top.push(t);
                top_margin.push(row[t as usize] - second);
            }
            let field = |f: &str| format!("real.{}.{f}", c.name);
            recs.extend([
                (field("prompt.text"), c.text.clone()),
                (field("prompt.ids"), u32_bytes(&c.ids)),
                (field("tokens"), u32_bytes(&c.tokens)),
                (field("margins"), f32_bytes(&c.margins)),
                (field("exact.offset"), f32_bytes(&offset)),
                (field("exact.scale"), f32_bytes(&scale)),
                (field("exact.q"), q),
                (field("exact.top"), u32_bytes(&top)),
                (field("exact.margin"), f32_bytes(&top_margin)),
            ]);
        }
        write_records(recs)
    }

    fn weights_digest(model: u32) -> u64 {
        registry::with_loaded_slm(model as usize, |slm| {
            // FNV-1a: enough to see any byte change.
            slm.weight_bytes().unwrap().iter().fold(0xcbf2_9ce4_8422_2325u64, |h, &b| {
                (h ^ b as u64).wrapping_mul(0x100_0000_01b3)
            })
        })
        .unwrap()
    }

    /// Run the harness with each of `flags` and check the verdict: without the
    /// mutation it passes; with it, it fails and puts the weights back. Ends
    /// with a run without the mutation, which must pass again.
    fn assert_real_harness(model: u32, rec: &[u8], flags: &[u32]) {
        let before = weights_digest(model);
        let mut last = None;
        for &f in flags {
            let mut rep = RealGoldenReport::default();
            real_golden_run(model, rec, f, &mut rep).unwrap();
            std::println!("real golden flags {f}: passed {} {rep:?}", rep.passed());
            if f & REAL_FLAG_DROP_BIAS == 0 {
                assert!(rep.passed(), "flags {f}: {rep:?}");
                assert_eq!(rep.mutated_bytes, 0);
            } else {
                assert!(!rep.passed(), "flags {f}: the mutation passed: {rep:?}");
                assert!(rep.first_bad_step >= 0, "{rep:?}");
                assert!(rep.mutated_bytes > 0 && rep.restored == 1, "{rep:?}");
                assert_eq!(weights_digest(model), before, "flags {f}: weights not restored");
            }
            if f & REAL_FLAG_BATCH != 0 {
                let longest = RealFixture::parse(&Records::new(rec).unwrap())
                    .unwrap()
                    .cases
                    .iter()
                    .map(|c| c.ids.len() as u32)
                    .max()
                    .unwrap();
                assert_eq!(rep.max_rows, longest, "{rep:?}");
            }
            last = Some(f);
        }
        assert_eq!(last.map(|f| f & REAL_FLAG_DROP_BIAS), Some(0), "end on a run without the mutation");
    }

    /// The harness on the tiny model, in CI: the exact reference made by
    /// [`make_real_fixture`] on the software GPU, llama.cpp's greedy tokens
    /// from the tiny fixture. Also refuses a model the fixture isn't for.
    #[test]
    fn golden_real_harness_on_tiny_passes_and_catches_dropped_bias() {
        let model = load_tiny();
        let r = greedy_rec();
        let case = |name: &str, text: &str, ids: &str, tokens: &str| LlamaCase {
            name: name.into(),
            text: r.get(text).unwrap().to_vec(),
            ids: r.u32s(ids).unwrap(),
            tokens: r.u32s(tokens).unwrap(),
            margins: r.f32s("greedy.margins").unwrap(),
        };
        let cases = [
            case("greedy", "prompt.text", "prompt.ids", "greedy.tokens"),
            // A second case, so the loop over cases is covered too.
            case("again", "prompt.text", "prompt.ids", "greedy.tokens"),
        ];
        let rec = make_real_fixture(model, &cases, 0);
        assert_real_harness(
            model,
            &rec,
            &[0, REAL_FLAG_BATCH, REAL_FLAG_DROP_BIAS, REAL_FLAG_DROP_BIAS | REAL_FLAG_BATCH, 0],
        );

        // A fixture for another file is refused, not compared.
        let mut other = rec.clone();
        let at = other.windows(16).position(|w| w == b"real.model.bytes").unwrap() + 20;
        other[at] ^= 1;
        let mut rep = RealGoldenReport::default();
        assert_eq!(
            real_golden_run(model, &other, 0, &mut rep),
            Err("the loaded model is not the file the record was made from")
        );
        crate::slm::gpu_program::soft::reset_knobs();
        reset_for_tests();
    }

    /// Writes the real model's fixture. Run by `slm-gen-goldens.py --only
    /// real-model`, which supplies `SLM_GOLDEN_MODEL` (the GGUF),
    /// `SLM_REAL_LLAMA_REC` (llama.cpp's greedy runs: `cases`, then per case
    /// `<case>.prompt.text`, `.prompt.ids`, `.tokens`, `.margins`),
    /// `SLM_REAL_LAYER` (the layer the mutation hits) and `SLM_REAL_OUT`.
    /// `--release`.
    #[test]
    #[ignore = "needs the real model; run by slm-gen-goldens.py"]
    fn golden_real_model_write_fixture() {
        let bytes = std::fs::read(std::env::var("SLM_GOLDEN_MODEL").unwrap()).unwrap();
        let llama = std::fs::read(std::env::var("SLM_REAL_LLAMA_REC").unwrap()).unwrap();
        let layer: u32 = std::env::var("SLM_REAL_LAYER").unwrap().parse().unwrap();
        let out = std::env::var("SLM_REAL_OUT").unwrap();
        ensure_mm_initialized_for_tests();
        reset_for_tests();
        let model = load_slm(b"real", &bytes).expect("real model loads") as u32;
        drop(bytes);
        let r = Records::new(&llama).unwrap();
        let names = String::from_utf8(r.get("cases").unwrap().to_vec()).unwrap();
        let cases: Vec<LlamaCase> = names
            .split(',')
            .map(|n| LlamaCase {
                name: n.into(),
                text: r.get(&format!("{n}.prompt.text")).unwrap().to_vec(),
                ids: r.u32s(&format!("{n}.prompt.ids")).unwrap(),
                tokens: r.u32s(&format!("{n}.tokens")).unwrap(),
                margins: r.f32s(&format!("{n}.margins")).unwrap(),
            })
            .collect();
        let rec = make_real_fixture(model, &cases, layer);
        std::fs::write(&out, &rec).unwrap();
        std::println!("wrote {out}: {} bytes", rec.len());
        crate::slm::gpu_program::soft::reset_knobs();
        reset_for_tests();
    }

    /// The hardware target's check on the host (`make slm-real-golden-host`):
    /// the checked-in fixture against the real model on the software GPU.
    /// Batched it passes and with the bias mutation it fails; then one token
    /// at a time it passes. The software GPU runs about 10 s a token, so the
    /// per-token run comes once. `SLM_GOLDEN_MODEL` is the GGUF. `--release`.
    #[test]
    #[ignore = "needs the real model; see the doc comment"]
    fn golden_real_model_host() {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../scripts/fixtures/slm/qwen25_real_golden.rec");
        let rec = std::fs::read(path).expect("the fixture");
        let bytes = std::fs::read(std::env::var("SLM_GOLDEN_MODEL").unwrap()).unwrap();
        ensure_mm_initialized_for_tests();
        reset_for_tests();
        crate::slm::gpu_program::soft::reset_knobs();
        let model = load_slm(b"real", &bytes).expect("real model loads") as u32;
        drop(bytes);
        assert_real_harness(model, &rec, &[REAL_FLAG_BATCH, REAL_FLAG_DROP_BIAS | REAL_FLAG_BATCH, 0]);
        crate::slm::gpu_program::soft::reset_knobs();
        reset_for_tests();
    }

    #[test]
    fn golden_e2e_first_step_logits_match_llama_cpp() {
        let model = load_tiny();
        pass(check_first_step(model, &greedy_rec()));
        reset_for_tests();
    }

    #[test]
    fn golden_e2e_greedy_matches_llama_cpp() {
        let model = load_tiny();
        pass(check_greedy(model, &greedy_rec()));
        reset_for_tests();
    }

    /// Print each step's logit error and greedy pick against a reference run.
    fn print_step_errors(model: u32, ids: &[u32], r: &Records<'_>) {
        let tokens = r.u32s("greedy.tokens").unwrap();
        let want = r.f32s("greedy.logits").unwrap();
        let n_vocab = r.u32s("greedy.n_vocab").unwrap()[0] as usize;
        let mut seq = ids.to_vec();
        seq.extend_from_slice(&tokens);
        let mut session = Session::open(model, 64, Sampler::Greedy, 0).unwrap();
        let mut scratch =
            ForwardScratch::new(&session.arch, session.max_ctx, session.vocab_size as usize);
        for step in 0..want.len() / n_vocab {
            registry::with_loaded_slm(model as usize, |slm| {
                forward_one(&mut session, slm, seq[step], &mut scratch)
            })
            .flatten()
            .unwrap();
            let row = &want[step * n_vocab..(step + 1) * n_vocab];
            let mut max = 0.0f32;
            let mut sum = 0.0f32;
            for i in 0..n_vocab {
                let e = (scratch.logits[i] - row[i]).abs();
                max = max.max(e);
                sum += e * e;
            }
            std::println!(
                "step {step}: max {max:.4} rms {:.4} argmax got {} want {}",
                libm::sqrtf(sum / n_vocab as f32),
                argmax(&scratch.logits[..n_vocab]),
                argmax(row)
            );
        }
    }

    /// Not a check: the data behind [`LOGIT_TOLERANCE`].
    /// `cargo test ... golden_measure -- --ignored --nocapture`
    #[test]
    #[ignore = "diagnostic"]
    fn golden_measure_logit_error() {
        let model = load_tiny();
        let r = greedy_rec();
        print_step_errors(model, &r.u32s("prompt.ids").unwrap(), &r);
        reset_for_tests();
    }

    /// Manual cross-check on the real model, too big for CI. Env:
    /// `SLM_GOLDEN_MODEL` (GGUF), `SLM_GOLDEN_REC` (output of
    /// `slm-golden-driver greedy`), `SLM_GOLDEN_IDS` (its comma-separated
    /// prompt). Prints each step's error and picks; run with `--release` and
    /// `--ignored --nocapture`.
    #[test]
    #[ignore = "needs the real model; see the doc comment"]
    fn golden_real_model_vs_llama_cpp() {
        let bytes = std::fs::read(std::env::var("SLM_GOLDEN_MODEL").unwrap()).unwrap();
        let rec_bytes = std::fs::read(std::env::var("SLM_GOLDEN_REC").unwrap()).unwrap();
        let ids = ids_of(&std::env::var("SLM_GOLDEN_IDS").unwrap());
        ensure_mm_initialized_for_tests();
        reset_for_tests();
        let model = load_slm(b"real", &bytes).expect("real model loads") as u32;
        print_step_errors(model, &ids, &Records::new(&rec_bytes).unwrap());
        reset_for_tests();
    }

    /// Token-for-token measurement on the real model. Teacher-forced on
    /// llama.cpp's tokens so every step stays comparable: the first argmax
    /// mismatch is also where a free run would diverge. Env as above plus
    /// `SLM_NUM`: `forward` for the production path, otherwise rounding flags
    /// for [`crate::slm::numerics_probe::Rounding::parse`].
    #[test]
    #[ignore = "needs the real model; see the doc comment"]
    fn golden_real_model_numerics() {
        use crate::slm::numerics_probe::{Probe, Rounding};
        let bytes = std::fs::read(std::env::var("SLM_GOLDEN_MODEL").unwrap()).unwrap();
        let rec_bytes = std::fs::read(std::env::var("SLM_GOLDEN_REC").unwrap()).unwrap();
        let ids = ids_of(&std::env::var("SLM_GOLDEN_IDS").unwrap());
        let spec = std::env::var("SLM_NUM").unwrap_or_else(|_| "forward".into());
        let max_steps: usize =
            std::env::var("SLM_NUM_STEPS").ok().map_or(usize::MAX, |s| s.parse().unwrap());
        ensure_mm_initialized_for_tests();
        reset_for_tests();
        let model = load_slm(b"real", &bytes).expect("real model loads") as u32;
        let r = Records::new(&rec_bytes).unwrap();
        let tokens = r.u32s("greedy.tokens").unwrap();
        let want = r.f32s("greedy.logits").unwrap();
        let margins = r.f32s("greedy.margins").unwrap();
        let n_vocab = r.u32s("greedy.n_vocab").unwrap()[0] as usize;
        let mut seq = ids.clone();
        seq.extend_from_slice(&tokens);
        let total = want.len() / n_vocab;
        let steps = total.min(max_steps);

        let mut session = Session::open(model, total + 8, Sampler::Greedy, 0).unwrap();
        let mut scratch =
            ForwardScratch::new(&session.arch, session.max_ctx, session.vocab_size as usize);
        // The pairing before #1156, for the before/after table in the design doc.
        if std::env::var("SLM_NUM_ROPE").as_deref() == Ok("interleaved") {
            scratch.rope = RopeTable::new(
                session.arch.head_dim as usize,
                session.max_ctx,
                session.arch.rope_freq_base,
                RopePairing::Interleaved,
            );
        }
        let mut probe = (spec != "forward").then(|| {
            registry::with_loaded_slm(model as usize, |slm| {
                Probe::new(slm, Rounding::parse(&spec), total + 8, n_vocab)
            })
            .unwrap()
        });
        if let (Some(p), Ok(path)) = (probe.as_mut(), std::env::var("SLM_NUM_TRACE")) {
            p.trace = Some(std::fs::File::create(path).unwrap());
        }
        let started = std::time::Instant::now();
        let (mut first, mut wrong, mut worst) = (None, 0usize, 0.0f32);
        for step in 0..steps {
            registry::with_loaded_slm(model as usize, |slm| {
                match probe.as_mut() {
                    Some(p) => p.step(slm, seq[step]),
                    None => {
                        forward_one(&mut session, slm, seq[step], &mut scratch).unwrap();
                    }
                }
            })
            .unwrap();
            let got = match probe.as_ref() {
                Some(p) => &p.logits[..n_vocab],
                None => &scratch.logits[..n_vocab],
            };
            let row = &want[step * n_vocab..(step + 1) * n_vocab];
            let (mut max, mut sum) = (0.0f32, 0.0f64);
            for i in 0..n_vocab {
                let e = (got[i] - row[i]).abs();
                max = max.max(e);
                sum += (e * e) as f64;
            }
            worst = worst.max(max);
            if step + 1 < ids.len() {
                std::println!("prompt {step}: max {max:.4} rms {:.4}", (sum / n_vocab as f64).sqrt());
                continue;
            }
            let g = step + 1 - ids.len();
            let pick = argmax(got);
            let bad = pick != tokens[g];
            if bad {
                wrong += 1;
                first.get_or_insert(g);
            }
            std::println!(
                "gen {g}: want {} got {pick}{} margin {:.3} max {max:.4} rms {:.4}",
                tokens[g],
                if bad { " DIFF" } else { "" },
                margins[g],
                (sum / n_vocab as f64).sqrt(),
            );
        }
        std::println!(
            "SUMMARY {spec}: steps {steps} first_diff {first:?} wrong {wrong} worst_max {worst:.4} ms_per_step {:.0}",
            started.elapsed().as_secs_f64() * 1e3 / steps as f64
        );
        reset_for_tests();
    }

    /// How much of the logit error against llama.cpp needs no difference in
    /// the model at all: the same F32 forward pass twice, the second with +/-1
    /// F32 ULP of noise on the hidden state after each residual sum. Env as above.
    #[test]
    #[ignore = "needs the real model; see the doc comment"]
    fn golden_real_model_one_ulp_sensitivity() {
        use crate::slm::numerics_probe::{Probe, Rounding};
        let bytes = std::fs::read(std::env::var("SLM_GOLDEN_MODEL").unwrap()).unwrap();
        let rec_bytes = std::fs::read(std::env::var("SLM_GOLDEN_REC").unwrap()).unwrap();
        let ids = ids_of(&std::env::var("SLM_GOLDEN_IDS").unwrap());
        let steps: usize = std::env::var("SLM_NUM_STEPS").unwrap().parse().unwrap();
        ensure_mm_initialized_for_tests();
        reset_for_tests();
        let model = load_slm(b"real", &bytes).expect("real model loads") as u32;
        let r = Records::new(&rec_bytes).unwrap();
        let n_vocab = r.u32s("greedy.n_vocab").unwrap()[0] as usize;
        let mut seq = ids.clone();
        seq.extend_from_slice(&r.u32s("greedy.tokens").unwrap());
        let (mut a, mut b) = registry::with_loaded_slm(model as usize, |slm| {
            let flags = Rounding::parse("none");
            (Probe::new(slm, flags, seq.len(), n_vocab), Probe::new(slm, flags, seq.len(), n_vocab))
        })
        .unwrap();
        b.dither = 0x1234_5678;
        for step in 0..steps {
            registry::with_loaded_slm(model as usize, |slm| {
                a.step(slm, seq[step]);
                b.step(slm, seq[step]);
            })
            .unwrap();
            let (mut max, mut sum) = (0.0f32, 0.0f64);
            for i in 0..n_vocab {
                let e = (a.logits[i] - b.logits[i]).abs();
                max = max.max(e);
                sum += (e * e) as f64;
            }
            std::println!(
                "ulp {step}: max {max:.4} rms {:.4} argmax {} {}",
                (sum / n_vocab as f64).sqrt(),
                argmax(&a.logits[..n_vocab]),
                argmax(&b.logits[..n_vocab]),
            );
        }
        reset_for_tests();
    }

    /// The probe with every flag set is the forward pass it claims to model.
    /// Comparing bits keeps its conclusions honest about the production path.
    #[test]
    fn numerics_probe_all_flags_matches_forward_one() {
        use crate::slm::numerics_probe::{Probe, Rounding};
        let model = load_tiny();
        let mut session = Session::open(model, 16, Sampler::Greedy, 0).unwrap();
        let n_vocab = session.vocab_size as usize;
        let mut scratch = ForwardScratch::new(&session.arch, session.max_ctx, n_vocab);
        let mut probe = registry::with_loaded_slm(model as usize, |slm| {
            Probe::new(slm, Rounding::parse("all"), 16, n_vocab)
        })
        .unwrap();
        for token in [1u32, 40, 77, 3] {
            registry::with_loaded_slm(model as usize, |slm| {
                forward_one(&mut session, slm, token, &mut scratch).unwrap();
                probe.step(slm, token);
            })
            .unwrap();
            assert_eq!(&probe.logits[..n_vocab], &scratch.logits[..n_vocab]);
        }
        reset_for_tests();
    }

    // ---- through the FFI and the text path ---------------------------------

    struct Collected {
        ids: Vec<u32>,
    }

    unsafe extern "C" fn collect(
        user: *mut core::ffi::c_void,
        token_id: u32,
        _bytes: *const u8,
        _len: usize,
    ) -> i32 {
        // SAFETY: `user` is the `Collected` passed to `rust_slm_prompt` below.
        unsafe { (*(user as *mut Collected)).ids.push(token_id) };
        1
    }

    fn prompt_tokens(model: u32, text: &[u8], n: u32) -> Vec<u32> {
        let session = crate::rust_slm_session_open(model, 64, 0, 0.0, 0, 0.0, 0);
        assert!(session >= 0);
        let mut got = Collected { ids: Vec::new() };
        let rc = unsafe {
            // Raw: the references are tokens for exactly this text, no template.
            crate::rust_slm_prompt_ex(
                session as u32,
                text.as_ptr(),
                text.len(),
                crate::SLM_PROMPT_RAW,
                n,
                collect,
                None,
                &mut got as *mut Collected as *mut core::ffi::c_void,
            )
        };
        assert_eq!(rc, 0);
        assert_eq!(crate::rust_slm_session_close(session as u32), 0);
        got.ids
    }

    #[test]
    fn golden_ffi_first_token_matches_llama_cpp() {
        let model = load_tiny();
        let r = greedy_rec();
        let want = r.u32s("first.tokens").unwrap();
        let got = prompt_tokens(model, r.get("first.text").unwrap(), 1);
        assert_eq!(got, want);
        reset_for_tests();
    }

    #[test]
    fn golden_ffi_greedy_matches_llama_cpp() {
        let model = load_tiny();
        let r = greedy_rec();
        let want = r.u32s("greedy.tokens").unwrap();
        let got = prompt_tokens(model, r.get("prompt.text").unwrap(), want.len() as u32);
        assert_eq!(got, want);
        reset_for_tests();
    }

    // ---- tokenizer against the real Qwen vocabulary -------------------------

    fn vocab_bytes() -> Vec<u8> {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../scripts/fixtures/slm/qwen_vocab.gguf");
        std::fs::read(path).expect("qwen_vocab.gguf")
    }

    fn unhex(s: &str) -> Vec<u8> {
        (0..s.len() / 2)
            .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
            .collect()
    }

    fn ids_of(s: &str) -> Vec<u32> {
        if s.is_empty() {
            return Vec::new();
        }
        s.split(',').map(|x| x.parse().unwrap()).collect()
    }

    fn run_case(tok: &Bbpe, line: &str) -> (String, bool, String) {
        let f: Vec<&str> = line.split('\t').collect();
        let want = ids_of(f[3]);
        let got = match f[0] {
            "plain" => {
                let text = String::from_utf8(unhex(f[2])).unwrap();
                tok.encode(&text)
            }
            "chat" => {
                let owned: Vec<(Role, String)> = f[2]
                    .split(';')
                    .map(|t| {
                        let (role, hex) = t.split_once('=').unwrap();
                        let role = match role {
                            "system" => Role::System,
                            "user" => Role::User,
                            "assistant" => Role::Assistant,
                            other => panic!("role {other}"),
                        };
                        (role, String::from_utf8(unhex(hex)).unwrap())
                    })
                    .collect();
                let turns: Vec<ChatTurn<'_>> = owned
                    .iter()
                    .map(|(role, content)| ChatTurn { role: *role, content })
                    .collect();
                encode_chat(tok, &turns)
            }
            other => panic!("mode {other}"),
        };
        let detail = format!("{}: got {got:?}\n  want {want:?}", f[1]);
        (String::from(f[1]), got == want, detail)
    }

    /// #1158: stop tokens come from the file. In the real vocabulary token 2
    /// is the text token `#` and must not stop anything; 151645 is `<|im_end|>`.
    /// The greedy goldens avoid token 2 by seed, so they can't show this.
    #[test]
    fn golden_stop_ids_are_the_real_models_not_token_2() {
        let bytes = vocab_bytes();
        let gguf = Gguf::parse(&bytes).expect("vocab fixture parses");
        let tok = Bbpe::from_gguf(&gguf).expect("vocab fixture builds a tokenizer");
        assert_eq!(tok.decode(&[2]), "#");
        assert_eq!(tok.eos_id(), Some(151_645));
        assert_eq!(tok.special_id("<|im_end|>"), Some(151_645));
        let stops = tok.stop_ids();
        assert!(stops.contains(&Some(151_645)));
        assert!(stops.contains(&Some(151_643)), "<|endoftext|>");
        assert!(!stops.contains(&Some(2)));
    }

    /// #1158 through the FFI `slm prompt` uses, on a GGUF llama.cpp accepted:
    /// the reply runs through token 2 and ends at the file's EOS.
    #[test]
    fn golden_ffi_runs_past_token_2_and_stops_at_gguf_eos() {
        use crate::slm::decoder::test_script_set;
        let model = load_tiny();
        let gguf_eos = registry::with_loaded_slm(model as usize, |slm| {
            slm.tokenizer().map(|t| t.eos_id())
        })
        .flatten()
        .flatten()
        .expect("tiny model has an EOS");
        assert_ne!(gguf_eos, 2);
        test_script_set(&[97, 2, 98, gguf_eos, 99], false);
        let got = prompt_tokens(model, b"hello", 0);
        test_script_set(&[], false);
        assert_eq!(got, [97, 2, 98], "2 is text, the EOS ends the reply unemitted");
        reset_for_tests();
    }

    #[test]
    fn golden_tokenizer_real_qwen_vocab() {
        let bytes = vocab_bytes();
        let gguf = Gguf::parse(&bytes).expect("vocab fixture parses");
        let tok = Bbpe::from_gguf(&gguf).expect("vocab fixture builds a tokenizer");
        assert_eq!(tok.vocab_size(), 151_936);
        let mut failures = Vec::new();
        let mut cases = 0;
        for line in TOK_CASES.lines() {
            cases += 1;
            let (_, ok, detail) = run_case(&tok, line);
            if !ok {
                failures.push(detail);
            }
        }
        assert!(cases >= 40, "only {cases} cases");
        assert!(failures.is_empty(), "{} mismatches:\n{}", failures.len(), failures.join("\n"));
    }
}
