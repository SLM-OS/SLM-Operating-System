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
use crate::slm::forward::{forward_one, ForwardScratch};
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

    fn f32s(&self, name: &str) -> Result<Vec<f32>, Mismatch> {
        let bytes = self.get(name).ok_or_else(|| malformed(name))?;
        Ok(bytes
            .chunks_exact(4)
            .map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect())
    }

    fn u32s(&self, name: &str) -> Result<Vec<u32>, Mismatch> {
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
